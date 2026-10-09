#ifdef _WIN32

#include "okuflow/cuda/cuda_kernels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../../third_party/nvidia_nis/NIS_Config.h"

#include <cuda_runtime.h>

// CUDA adaptations of the vendor reference shader math:
// - AMD FidelityFX Super Resolution 1.0.2, ffx_fsr1.h, EASU + RCAS
//   commit a21ffb8f6c13233ba336352bdff293894c706575 (MIT).
// - NVIDIA Image Scaling SDK 1.0.3, NIS_Scaler.h + NIS_Config.h
//   commit 35e13ba316c98eeecf16f37eae70ce88019911f6 (MIT).
//
// The original headers and complete notices live under third_party/. This
// translation unit preserves the reference filter equations and coefficient
// banks while replacing HLSL/GLSL texture and group primitives with CUDA
// pitched-memory loads.

namespace okuflow {

namespace {

constexpr int kNisPhaseCount = 64;
constexpr int kNisCoefficientCount = 8;
constexpr float kFsrRcasLimit = 0.25f - (1.0f / 16.0f);

__constant__ float gNisCoefScale[kNisPhaseCount][kNisCoefficientCount];
__constant__ float gNisCoefUsm[kNisPhaseCount][kNisCoefficientCount];

struct NisDeviceConfig {
    float detectRatio;
    float detectThreshold;
    float minContrastRatio;
    float ratioNorm;
    float contrastBoost;
    float epsilon;
    float sharpStartY;
    float sharpScaleY;
    float sharpStrengthMin;
    float sharpStrengthScale;
    float sharpLimitMin;
    float sharpLimitScale;
    float scaleX;
    float scaleY;
};

template <typename T>
__device__ const T* RowAt(const T* base, size_t pitchBytes, int y) {
    return reinterpret_cast<const T*>(
        reinterpret_cast<const unsigned char*>(base) +
        pitchBytes * static_cast<size_t>(y));
}

template <typename T>
__device__ T* RowAt(T* base, size_t pitchBytes, int y) {
    return reinterpret_cast<T*>(
        reinterpret_cast<unsigned char*>(base) +
        pitchBytes * static_cast<size_t>(y));
}

__device__ float Saturate(float value) {
    return fminf(fmaxf(value, 0.0f), 1.0f);
}

__device__ float SafeReciprocal(float value) {
    if (fabsf(value) < 1.0e-8f) {
        return value < 0.0f ? -1.0e8f : 1.0e8f;
    }
    return 1.0f / value;
}

__device__ float3 Add(float3 a, float3 b) {
    return make_float3(a.x + b.x, a.y + b.y, a.z + b.z);
}

__device__ float3 Mul(float3 value, float scale) {
    return make_float3(value.x * scale, value.y * scale, value.z * scale);
}

__device__ float3 Min(float3 a, float3 b) {
    return make_float3(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z));
}

__device__ float3 Max(float3 a, float3 b) {
    return make_float3(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z));
}

__device__ float3 Clamp01(float3 value) {
    return make_float3(Saturate(value.x), Saturate(value.y), Saturate(value.z));
}

__device__ float3 ReadRgb(const uchar4* src, size_t pitchBytes,
                          int x, int y, int width, int height) {
    const int clampedX = max(0, min(width - 1, x));
    const int clampedY = max(0, min(height - 1, y));
    const uchar4 bgra = RowAt(src, pitchBytes, clampedY)[clampedX];
    constexpr float kToUnit = 1.0f / 255.0f;
    return make_float3(static_cast<float>(bgra.z) * kToUnit,
                       static_cast<float>(bgra.y) * kToUnit,
                       static_cast<float>(bgra.x) * kToUnit);
}

__device__ float3 BilinearRgb(const uchar4* src, size_t pitchBytes,
                              float x, float y, int width, int height) {
    const int x0 = static_cast<int>(floorf(x));
    const int y0 = static_cast<int>(floorf(y));
    const float fx = x - floorf(x);
    const float fy = y - floorf(y);
    const float3 c00 = ReadRgb(src, pitchBytes, x0, y0, width, height);
    const float3 c10 = ReadRgb(src, pitchBytes, x0 + 1, y0, width, height);
    const float3 c01 = ReadRgb(src, pitchBytes, x0, y0 + 1, width, height);
    const float3 c11 = ReadRgb(src, pitchBytes, x0 + 1, y0 + 1, width, height);
    const float3 top = Add(c00, Mul(Add(c10, Mul(c00, -1.0f)), fx));
    const float3 bottom = Add(c01, Mul(Add(c11, Mul(c01, -1.0f)), fx));
    return Add(top, Mul(Add(bottom, Mul(top, -1.0f)), fy));
}

__device__ void WriteRgb(uchar4* dst, size_t pitchBytes,
                         int x, int y, float3 rgb) {
    rgb = Clamp01(rgb);
    RowAt(dst, pitchBytes, y)[x] =
        make_uchar4(static_cast<unsigned char>(rgb.z * 255.0f + 0.5f),
                    static_cast<unsigned char>(rgb.y * 255.0f + 0.5f),
                    static_cast<unsigned char>(rgb.x * 255.0f + 0.5f),
                    255u);
}

__device__ float FsrLuma(float3 rgb) {
    return rgb.z * 0.5f + rgb.x * 0.5f + rgb.y;
}

__device__ void FsrEasuSet(float2& direction, float& length, float2 phase,
                           int corner, float lA, float lB, float lC,
                           float lD, float lE) {
    float weight = 0.0f;
    if (corner == 0) {
        weight = (1.0f - phase.x) * (1.0f - phase.y);
    } else if (corner == 1) {
        weight = phase.x * (1.0f - phase.y);
    } else if (corner == 2) {
        weight = (1.0f - phase.x) * phase.y;
    } else {
        weight = phase.x * phase.y;
    }

    const float dc = lD - lC;
    const float cb = lC - lB;
    const float maxX = fmaxf(fabsf(dc), fabsf(cb));
    const float dirX = lD - lB;
    direction.x += dirX * weight;
    float lenX = maxX > 1.0e-8f ? Saturate(fabsf(dirX) / maxX) : 0.0f;
    length += lenX * lenX * weight;

    const float ec = lE - lC;
    const float ca = lC - lA;
    const float maxY = fmaxf(fabsf(ec), fabsf(ca));
    const float dirY = lE - lA;
    direction.y += dirY * weight;
    float lenY = maxY > 1.0e-8f ? Saturate(fabsf(dirY) / maxY) : 0.0f;
    length += lenY * lenY * weight;
}

__device__ void FsrEasuTap(float3& accumulatedColor,
                           float& accumulatedWeight,
                           float2 offset, float2 direction, float2 length,
                           float lobe, float clipPoint, float3 color) {
    float2 rotated{};
    rotated.x = offset.x * direction.x + offset.y * direction.y;
    rotated.y = offset.x * -direction.y + offset.y * direction.x;
    rotated.x *= length.x;
    rotated.y *= length.y;

    float distanceSquared =
        fminf(rotated.x * rotated.x + rotated.y * rotated.y, clipPoint);
    float windowBase = (2.0f / 5.0f) * distanceSquared - 1.0f;
    float windowLobe = lobe * distanceSquared - 1.0f;
    windowBase *= windowBase;
    windowLobe *= windowLobe;
    windowBase = (25.0f / 16.0f) * windowBase -
                 ((25.0f / 16.0f) - 1.0f);
    const float weight = windowBase * windowLobe;
    accumulatedColor = Add(accumulatedColor, Mul(color, weight));
    accumulatedWeight += weight;
}

__global__ void FsrEasuKernel(uchar4* dst, size_t dstPitchBytes,
                              const uchar4* src, size_t srcPitchBytes,
                              int srcWidth, int srcHeight,
                              int dstWidth, int dstHeight) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstWidth || y >= dstHeight) {
        return;
    }

    const float2 scale =
        make_float2(static_cast<float>(srcWidth) / dstWidth,
                    static_cast<float>(srcHeight) / dstHeight);
    const float2 position =
        make_float2((static_cast<float>(x) + 0.5f) * scale.x - 0.5f,
                    (static_cast<float>(y) + 0.5f) * scale.y - 0.5f);
    const int baseX = static_cast<int>(floorf(position.x));
    const int baseY = static_cast<int>(floorf(position.y));
    const float2 phase =
        make_float2(position.x - floorf(position.x),
                    position.y - floorf(position.y));

    // AMD's 12-tap EASU neighborhood:
    //     b c
    //   e f g h
    //   i j k l
    //     n o
    const float3 b = ReadRgb(src, srcPitchBytes, baseX,     baseY - 1, srcWidth, srcHeight);
    const float3 c = ReadRgb(src, srcPitchBytes, baseX + 1, baseY - 1, srcWidth, srcHeight);
    const float3 e = ReadRgb(src, srcPitchBytes, baseX - 1, baseY,     srcWidth, srcHeight);
    const float3 f = ReadRgb(src, srcPitchBytes, baseX,     baseY,     srcWidth, srcHeight);
    const float3 g = ReadRgb(src, srcPitchBytes, baseX + 1, baseY,     srcWidth, srcHeight);
    const float3 h = ReadRgb(src, srcPitchBytes, baseX + 2, baseY,     srcWidth, srcHeight);
    const float3 i = ReadRgb(src, srcPitchBytes, baseX - 1, baseY + 1, srcWidth, srcHeight);
    const float3 j = ReadRgb(src, srcPitchBytes, baseX,     baseY + 1, srcWidth, srcHeight);
    const float3 k = ReadRgb(src, srcPitchBytes, baseX + 1, baseY + 1, srcWidth, srcHeight);
    const float3 l = ReadRgb(src, srcPitchBytes, baseX + 2, baseY + 1, srcWidth, srcHeight);
    const float3 n = ReadRgb(src, srcPitchBytes, baseX,     baseY + 2, srcWidth, srcHeight);
    const float3 o = ReadRgb(src, srcPitchBytes, baseX + 1, baseY + 2, srcWidth, srcHeight);

    const float bL = FsrLuma(b);
    const float cL = FsrLuma(c);
    const float eL = FsrLuma(e);
    const float fL = FsrLuma(f);
    const float gL = FsrLuma(g);
    const float hL = FsrLuma(h);
    const float iL = FsrLuma(i);
    const float jL = FsrLuma(j);
    const float kL = FsrLuma(k);
    const float lL = FsrLuma(l);
    const float nL = FsrLuma(n);
    const float oL = FsrLuma(o);

    float2 direction = make_float2(0.0f, 0.0f);
    float length = 0.0f;
    FsrEasuSet(direction, length, phase, 0, bL, eL, fL, gL, jL);
    FsrEasuSet(direction, length, phase, 1, cL, fL, gL, hL, kL);
    FsrEasuSet(direction, length, phase, 2, fL, iL, jL, kL, nL);
    FsrEasuSet(direction, length, phase, 3, gL, jL, kL, lL, oL);

    const float directionSquared =
        direction.x * direction.x + direction.y * direction.y;
    if (directionSquared < 1.0f / 32768.0f) {
        direction = make_float2(1.0f, 0.0f);
    } else {
        const float inverseLength = rsqrtf(directionSquared);
        direction.x *= inverseLength;
        direction.y *= inverseLength;
    }

    length = length * 0.5f;
    length *= length;
    const float stretch =
        1.0f / fmaxf(fabsf(direction.x), fabsf(direction.y));
    const float2 anisotropicLength =
        make_float2(1.0f + (stretch - 1.0f) * length,
                    1.0f - 0.5f * length);
    const float lobe =
        0.5f + ((0.25f - 0.04f) - 0.5f) * length;
    const float clipPoint = 1.0f / lobe;

    const float3 minimum = Min(Min(f, g), Min(j, k));
    const float3 maximum = Max(Max(f, g), Max(j, k));
    float3 accumulatedColor = make_float3(0.0f, 0.0f, 0.0f);
    float accumulatedWeight = 0.0f;

    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(-phase.x, -1.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, b);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(1.0f - phase.x, -1.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, c);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(-1.0f - phase.x, 1.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, i);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(-phase.x, 1.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, j);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(-phase.x, -phase.y), direction, anisotropicLength, lobe, clipPoint, f);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(-1.0f - phase.x, -phase.y), direction, anisotropicLength, lobe, clipPoint, e);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(1.0f - phase.x, 1.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, k);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(2.0f - phase.x, 1.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, l);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(2.0f - phase.x, -phase.y), direction, anisotropicLength, lobe, clipPoint, h);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(1.0f - phase.x, -phase.y), direction, anisotropicLength, lobe, clipPoint, g);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(1.0f - phase.x, 2.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, o);
    FsrEasuTap(accumulatedColor, accumulatedWeight, make_float2(-phase.x, 2.0f - phase.y), direction, anisotropicLength, lobe, clipPoint, n);

    const float inverseWeight = SafeReciprocal(accumulatedWeight);
    const float3 resolved =
        Min(maximum, Max(minimum, Mul(accumulatedColor, inverseWeight)));
    WriteRgb(dst, dstPitchBytes, x, y, resolved);
}

__device__ float FsrRcasChannelLobe(float minimumRing, float maximumRing,
                                    float center) {
    const float hitMinimum =
        fminf(minimumRing, center) * SafeReciprocal(4.0f * maximumRing);
    const float hitMaximum =
        (1.0f - fmaxf(maximumRing, center)) *
        SafeReciprocal(4.0f * minimumRing - 4.0f);
    return fmaxf(-hitMinimum, hitMaximum);
}

__global__ void FsrRcasKernel(uchar4* dst, size_t dstPitchBytes,
                              const uchar4* src, size_t srcPitchBytes,
                              int width, int height, float sharpnessScale) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const float3 b = ReadRgb(src, srcPitchBytes, x, y - 1, width, height);
    const float3 d = ReadRgb(src, srcPitchBytes, x - 1, y, width, height);
    const float3 e = ReadRgb(src, srcPitchBytes, x, y, width, height);
    const float3 f = ReadRgb(src, srcPitchBytes, x + 1, y, width, height);
    const float3 h = ReadRgb(src, srcPitchBytes, x, y + 1, width, height);

    const float3 minimumRing = Min(Min(b, d), Min(f, h));
    const float3 maximumRing = Max(Max(b, d), Max(f, h));
    const float lobeR =
        FsrRcasChannelLobe(minimumRing.x, maximumRing.x, e.x);
    const float lobeG =
        FsrRcasChannelLobe(minimumRing.y, maximumRing.y, e.y);
    const float lobeB =
        FsrRcasChannelLobe(minimumRing.z, maximumRing.z, e.z);
    float lobe = fmaxf(lobeR, fmaxf(lobeG, lobeB));
    lobe = fmaxf(-kFsrRcasLimit, fminf(lobe, 0.0f)) * sharpnessScale;

    const float reciprocal = SafeReciprocal(4.0f * lobe + 1.0f);
    float3 result = Add(e, Mul(Add(Add(b, d), Add(f, h)), lobe));
    result = Mul(result, reciprocal);
    WriteRgb(dst, dstPitchBytes, x, y, result);
}

__device__ float NisLuma(float3 rgb) {
    return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

__device__ float4 Float4Lerp(float4 a, float4 b, float t) {
    return make_float4(a.x + (b.x - a.x) * t,
                       a.y + (b.y - a.y) * t,
                       a.z + (b.z - a.z) * t,
                       a.w + (b.w - a.w) * t);
}

__device__ float4 NisEdgeMap(const uchar4* src, size_t pitchBytes,
                             int centerX, int centerY, int width, int height,
                             const NisDeviceConfig& config) {
    float p[3][3];
#pragma unroll
    for (int row = 0; row < 3; ++row) {
#pragma unroll
        for (int column = 0; column < 3; ++column) {
            p[row][column] =
                NisLuma(ReadRgb(src, pitchBytes,
                                centerX + column - 1,
                                centerY + row - 1,
                                width, height));
        }
    }

    const float g0 = fabsf(p[0][0] + p[0][1] + p[0][2] -
                           p[2][0] - p[2][1] - p[2][2]);
    const float g45 = fabsf(p[1][0] + p[0][0] + p[0][1] -
                            p[2][1] - p[2][2] - p[1][2]);
    const float g90 = fabsf(p[0][0] + p[1][0] + p[2][0] -
                            p[0][2] - p[1][2] - p[2][2]);
    const float g135 = fabsf(p[1][0] + p[2][0] + p[2][1] -
                             p[0][1] - p[0][2] - p[1][2]);
    const float max090 = fmaxf(g0, g90);
    const float min090 = fminf(g0, g90);
    const float max45135 = fmaxf(g45, g135);
    const float min45135 = fminf(g45, g135);
    const float total = max090 + max45135;
    if (total == 0.0f) {
        return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float edge090 = fminf(max090 / total, 1.0f);
    const float edge45135 = 1.0f - edge090;
    const bool choose090 =
        max090 > min090 * config.detectRatio &&
        max090 > config.detectThreshold &&
        max090 > min45135;
    const bool choose45135 =
        max45135 > min45135 * config.detectRatio &&
        max45135 > config.detectThreshold &&
        max45135 > min090;
    const float selected090 =
        choose090 && choose45135 ? edge090 : 1.0f;
    const float selected45135 =
        choose090 && choose45135 ? edge45135 : 1.0f;
    return make_float4(
        choose090 && max090 == g0 ? selected090 : 0.0f,
        choose090 && max090 != g0 ? selected090 : 0.0f,
        choose45135 && max45135 == g45 ? selected45135 : 0.0f,
        choose45135 && max45135 != g45 ? selected45135 : 0.0f);
}

__device__ float NisCalcLti(const float pixels[6], int phase,
                            const NisDeviceConfig& config) {
    const bool firstHalf = phase <= kNisPhaseCount / 2;
    float selected = firstHalf ? pixels[0] : pixels[3];
    const float aMinimum = fminf(fminf(pixels[1], pixels[2]), selected);
    const float aMaximum = fmaxf(fmaxf(pixels[1], pixels[2]), selected);
    selected = firstHalf ? pixels[2] : pixels[5];
    const float bMinimum = fminf(fminf(pixels[3], pixels[4]), selected);
    const float bMaximum = fmaxf(fmaxf(pixels[3], pixels[4]), selected);
    const float aContrast = aMaximum - aMinimum;
    const float bContrast = bMaximum - bMinimum;
    const float contrastRatio =
        fmaxf(aContrast, bContrast) /
        (fminf(aContrast, bContrast) + config.epsilon);
    return (1.0f -
            Saturate((contrastRatio - config.minContrastRatio) *
                     config.ratioNorm)) *
           config.contrastBoost;
}

__device__ float NisEvalPoly6(const float pixels[6], int phase,
                              const NisDeviceConfig& config) {
    phase = max(0, min(kNisPhaseCount - 1, phase));
    float value = 0.0f;
    float unsharp = 0.0f;
#pragma unroll
    for (int index = 0; index < 6; ++index) {
        value += gNisCoefScale[phase][index] * pixels[index];
        unsharp += gNisCoefUsm[phase][index] * pixels[index];
    }
    const float yScale =
        1.0f - Saturate((value - config.sharpStartY) * config.sharpScaleY);
    unsharp *=
        yScale * config.sharpStrengthScale + config.sharpStrengthMin;
    const float limit =
        (yScale * config.sharpLimitScale + config.sharpLimitMin) * value;
    unsharp = fminf(limit, fmaxf(-limit, unsharp));
    unsharp *= NisCalcLti(pixels, phase, config);
    return value + unsharp;
}

__device__ float NisFilterNormal(const float pixels[6][6],
                                 int phaseX, int phaseY) {
    phaseX = max(0, min(kNisPhaseCount - 1, phaseX));
    phaseY = max(0, min(kNisPhaseCount - 1, phaseY));
    float horizontal = 0.0f;
#pragma unroll
    for (int column = 0; column < 6; ++column) {
        float vertical = 0.0f;
#pragma unroll
        for (int row = 0; row < 6; ++row) {
            vertical +=
                pixels[row][column] * gNisCoefScale[phaseY][row];
        }
        horizontal += vertical * gNisCoefScale[phaseX][column];
    }
    return horizontal;
}

__device__ float NisAddDirectionalFilters(
    const float p[6][6], float phaseX, float phaseY,
    int phaseXInt, int phaseYInt, float4 weights,
    const NisDeviceConfig& config) {
    float filtered = 0.0f;
    if (weights.x > 0.0f) {
        float interpolated[6];
#pragma unroll
        for (int index = 0; index < 6; ++index) {
            interpolated[index] =
                p[index][2] + (p[index][3] - p[index][2]) * phaseX;
        }
        filtered +=
            NisEvalPoly6(interpolated, phaseYInt, config) * weights.x;
    }
    if (weights.y > 0.0f) {
        float interpolated[6];
#pragma unroll
        for (int index = 0; index < 6; ++index) {
            interpolated[index] =
                p[2][index] + (p[3][index] - p[2][index]) * phaseY;
        }
        filtered +=
            NisEvalPoly6(interpolated, phaseXInt, config) * weights.y;
    }
    if (weights.z > 0.0f) {
        float blend = 0.5f + 0.5f * (phaseX - phaseY);
        float temporary[7];
        temporary[1] = p[2][1] + (p[1][2] - p[2][1]) * blend;
        temporary[3] = p[3][2] + (p[2][3] - p[3][2]) * blend;
        temporary[5] = p[4][3] + (p[3][4] - p[4][3]) * blend;
        blend -= 0.5f;
        const float a = blend >= 0.0f ? p[0][2] : p[2][0];
        const float b = blend >= 0.0f ? p[1][3] : p[3][1];
        const float c = blend >= 0.0f ? p[2][4] : p[4][2];
        const float d = blend >= 0.0f ? p[3][5] : p[5][3];
        temporary[0] = p[1][1] + (a - p[1][1]) * fabsf(blend);
        temporary[2] = p[2][2] + (b - p[2][2]) * fabsf(blend);
        temporary[4] = p[3][3] + (c - p[3][3]) * fabsf(blend);
        temporary[6] = p[4][4] + (d - p[4][4]) * fabsf(blend);

        float diagonal[6];
        float diagonalPhase = phaseX + phaseY;
        const int offset = diagonalPhase >= 1.0f ? 1 : 0;
#pragma unroll
        for (int index = 0; index < 6; ++index) {
            diagonal[index] = temporary[index + offset];
        }
        if (offset != 0) {
            diagonalPhase -= 1.0f;
        }
        filtered +=
            NisEvalPoly6(diagonal,
                         min(63, static_cast<int>(diagonalPhase * 64.0f)),
                         config) *
            weights.z;
    }
    if (weights.w > 0.0f) {
        float blend = 0.5f * (phaseX + phaseY);
        float temporary[7];
        temporary[1] = p[3][1] + (p[4][2] - p[3][1]) * blend;
        temporary[3] = p[2][2] + (p[3][3] - p[2][2]) * blend;
        temporary[5] = p[1][3] + (p[2][4] - p[1][3]) * blend;
        blend -= 0.5f;
        const float a = blend >= 0.0f ? p[5][2] : p[3][0];
        const float b = blend >= 0.0f ? p[4][3] : p[2][1];
        const float c = blend >= 0.0f ? p[3][4] : p[1][2];
        const float d = blend >= 0.0f ? p[2][5] : p[0][3];
        temporary[0] = p[4][1] + (a - p[4][1]) * fabsf(blend);
        temporary[2] = p[3][2] + (b - p[3][2]) * fabsf(blend);
        temporary[4] = p[2][3] + (c - p[2][3]) * fabsf(blend);
        temporary[6] = p[1][4] + (d - p[1][4]) * fabsf(blend);

        float diagonal[6];
        float diagonalPhase = 1.0f + phaseX - phaseY;
        const int offset = diagonalPhase >= 1.0f ? 1 : 0;
#pragma unroll
        for (int index = 0; index < 6; ++index) {
            diagonal[index] = temporary[index + offset];
        }
        if (offset != 0) {
            diagonalPhase -= 1.0f;
        }
        filtered +=
            NisEvalPoly6(diagonal,
                         min(63, static_cast<int>(diagonalPhase * 64.0f)),
                         config) *
            weights.w;
    }
    return filtered;
}

__global__ void NisScalerKernel(uchar4* dst, size_t dstPitchBytes,
                                const uchar4* src, size_t srcPitchBytes,
                                int srcWidth, int srcHeight,
                                int dstWidth, int dstHeight,
                                NisDeviceConfig config) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstWidth || y >= dstHeight) {
        return;
    }

    const float sourceX =
        (static_cast<float>(x) + 0.5f) * config.scaleX - 0.5f;
    const float sourceY =
        (static_cast<float>(y) + 0.5f) * config.scaleY - 0.5f;
    const int sourceFloorX = static_cast<int>(floorf(sourceX));
    const int sourceFloorY = static_cast<int>(floorf(sourceY));
    const float phaseX = sourceX - floorf(sourceX);
    const float phaseY = sourceY - floorf(sourceY);
    const int phaseXInt =
        min(63, static_cast<int>(phaseX * kNisPhaseCount));
    const int phaseYInt =
        min(63, static_cast<int>(phaseY * kNisPhaseCount));

    float p[6][6];
#pragma unroll
    for (int row = 0; row < 6; ++row) {
#pragma unroll
        for (int column = 0; column < 6; ++column) {
            p[row][column] =
                NisLuma(ReadRgb(src, srcPitchBytes,
                                sourceFloorX + column - 2,
                                sourceFloorY + row - 2,
                                srcWidth, srcHeight));
        }
    }

    float4 edges[2][2];
    edges[0][0] = NisEdgeMap(src, srcPitchBytes, sourceFloorX,
                             sourceFloorY, srcWidth, srcHeight, config);
    edges[0][1] = NisEdgeMap(src, srcPitchBytes, sourceFloorX + 1,
                             sourceFloorY, srcWidth, srcHeight, config);
    edges[1][0] = NisEdgeMap(src, srcPitchBytes, sourceFloorX,
                             sourceFloorY + 1, srcWidth, srcHeight, config);
    edges[1][1] = NisEdgeMap(src, srcPitchBytes, sourceFloorX + 1,
                             sourceFloorY + 1, srcWidth, srcHeight, config);
    const float4 top = Float4Lerp(edges[0][0], edges[0][1], phaseX);
    const float4 bottom = Float4Lerp(edges[1][0], edges[1][1], phaseX);
    const float4 weights = Float4Lerp(top, bottom, phaseY);
    const float baseWeight =
        1.0f - weights.x - weights.y - weights.z - weights.w;

    float outputLuma =
        NisFilterNormal(p, phaseXInt, phaseYInt) * baseWeight;
    outputLuma +=
        NisAddDirectionalFilters(p, phaseX, phaseY,
                                 phaseXInt, phaseYInt,
                                 weights, config);

    float3 output =
        BilinearRgb(src, srcPitchBytes, sourceX, sourceY,
                    srcWidth, srcHeight);
    const float correction = outputLuma - NisLuma(output);
    output.x += correction;
    output.y += correction;
    output.z += correction;
    WriteRgb(dst, dstPitchBytes, x, y, output);
}

void ThrowIfCudaFailed(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 cudaGetErrorString(status));
    }
}

void CheckLaunch(const char* operation) {
    ThrowIfCudaFailed(cudaGetLastError(), operation);
}

}  // namespace

void LaunchFsrEasuRcasLinear(uchar4* dst, size_t dstPitchBytes,
                             uchar4* scratch, size_t scratchPitchBytes,
                             const uchar4* src, size_t srcPitchBytes,
                             int srcWidth, int srcHeight,
                             int dstWidth, int dstHeight,
                             float sharpness,
                             cudaStream_t stream) {
    if (!dst || !scratch || !src ||
        srcWidth <= 0 || srcHeight <= 0 ||
        dstWidth <= 0 || dstHeight <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((dstWidth + blockSize.x - 1) / blockSize.x,
                        (dstHeight + blockSize.y - 1) / blockSize.y);
    FsrEasuKernel<<<gridSize, blockSize, 0, stream>>>(
        scratch, scratchPitchBytes, src, srcPitchBytes,
        srcWidth, srcHeight, dstWidth, dstHeight);
    CheckLaunch("FidelityFX FSR 1.0 EASU launch failed");

    // FsrRcasCon transforms sharpness stops with exp2(-stops). Mapping the
    // OkuFlow 0..1 slider through stops=-log2(slider) preserves that exact
    // transform while retaining 0 as a true no-sharpen endpoint.
    const float sharpnessScale =
        std::clamp(sharpness, 0.0f, 1.0f);
    FsrRcasKernel<<<gridSize, blockSize, 0, stream>>>(
        dst, dstPitchBytes, scratch, scratchPitchBytes,
        dstWidth, dstHeight, sharpnessScale);
    CheckLaunch("FidelityFX FSR 1.0 RCAS launch failed");
}

void LaunchNisLinear(uchar4* dst, size_t dstPitchBytes,
                     const uchar4* src, size_t srcPitchBytes,
                     int srcWidth, int srcHeight,
                     int dstWidth, int dstHeight,
                     float sharpness,
                     cudaStream_t stream) {
    if (!dst || !src ||
        srcWidth <= 0 || srcHeight <= 0 ||
        dstWidth <= 0 || dstHeight <= 0) {
        return;
    }

    NISConfig referenceConfig{};
    if (!NVScalerUpdateConfig(
            referenceConfig, std::clamp(sharpness, 0.0f, 1.0f),
            0, 0, static_cast<std::uint32_t>(srcWidth),
            static_cast<std::uint32_t>(srcHeight),
            static_cast<std::uint32_t>(srcWidth),
            static_cast<std::uint32_t>(srcHeight),
            0, 0, static_cast<std::uint32_t>(dstWidth),
            static_cast<std::uint32_t>(dstHeight),
            static_cast<std::uint32_t>(dstWidth),
            static_cast<std::uint32_t>(dstHeight),
            NISHDRMode::None)) {
        throw std::invalid_argument(
            "NVIDIA Image Scaling supports 1x to 2x enlargement per pass");
    }

    ThrowIfCudaFailed(
        cudaMemcpyToSymbolAsync(
            gNisCoefScale, coef_scale, sizeof(coef_scale), 0,
            cudaMemcpyHostToDevice, stream),
        "uploading NVIDIA Image Scaling scaler coefficients failed");
    ThrowIfCudaFailed(
        cudaMemcpyToSymbolAsync(
            gNisCoefUsm, coef_usm, sizeof(coef_usm), 0,
            cudaMemcpyHostToDevice, stream),
        "uploading NVIDIA Image Scaling USM coefficients failed");

    const NisDeviceConfig config{
        referenceConfig.kDetectRatio,
        referenceConfig.kDetectThres,
        referenceConfig.kMinContrastRatio,
        referenceConfig.kRatioNorm,
        referenceConfig.kContrastBoost,
        referenceConfig.kEps,
        referenceConfig.kSharpStartY,
        referenceConfig.kSharpScaleY,
        referenceConfig.kSharpStrengthMin,
        referenceConfig.kSharpStrengthScale,
        referenceConfig.kSharpLimitMin,
        referenceConfig.kSharpLimitScale,
        referenceConfig.kScaleX,
        referenceConfig.kScaleY,
    };

    const dim3 blockSize(16, 16);
    const dim3 gridSize((dstWidth + blockSize.x - 1) / blockSize.x,
                        (dstHeight + blockSize.y - 1) / blockSize.y);
    NisScalerKernel<<<gridSize, blockSize, 0, stream>>>(
        dst, dstPitchBytes, src, srcPitchBytes,
        srcWidth, srcHeight, dstWidth, dstHeight, config);
    CheckLaunch("NVIDIA Image Scaling NVScaler launch failed");
}

void PadSpatialCacheBorder(cudaArray_t cache,
                           const uchar4* output, size_t outputPitchBytes,
                           unsigned int outputWidth, unsigned int outputHeight,
                           unsigned int cacheWidth, unsigned int cacheHeight,
                           cudaStream_t stream) {
    if (!cache || !output || !outputWidth || !outputHeight ||
        outputWidth > cacheWidth || outputHeight > cacheHeight) {
        throw std::invalid_argument("Invalid spatial cache border geometry");
    }
    const auto* lastRow = reinterpret_cast<const unsigned char*>(output) +
        static_cast<size_t>(outputHeight - 1) * outputPitchBytes;
    const auto* lastColumn = reinterpret_cast<const unsigned char*>(output) +
        static_cast<size_t>(outputWidth - 1) * sizeof(uchar4);
    if (outputWidth < cacheWidth) {
        ThrowIfCudaFailed(cudaMemcpy2DToArrayAsync(
            cache, static_cast<size_t>(outputWidth) * sizeof(uchar4), 0,
            lastColumn, outputPitchBytes, sizeof(uchar4), outputHeight,
            cudaMemcpyDeviceToDevice, stream), "spatial cache guard column failed");
    }
    if (outputHeight < cacheHeight) {
        ThrowIfCudaFailed(cudaMemcpy2DToArrayAsync(
            cache, 0, outputHeight, lastRow, outputPitchBytes,
            static_cast<size_t>(outputWidth) * sizeof(uchar4), 1,
            cudaMemcpyDeviceToDevice, stream), "spatial cache guard row failed");
    }
    if (outputWidth < cacheWidth && outputHeight < cacheHeight) {
        ThrowIfCudaFailed(cudaMemcpy2DToArrayAsync(
            cache, static_cast<size_t>(outputWidth) * sizeof(uchar4), outputHeight,
            lastRow + static_cast<size_t>(outputWidth - 1) * sizeof(uchar4),
            outputPitchBytes, sizeof(uchar4), 1, cudaMemcpyDeviceToDevice, stream),
            "spatial cache guard corner failed");
    }
}

}  // namespace okuflow

#endif
