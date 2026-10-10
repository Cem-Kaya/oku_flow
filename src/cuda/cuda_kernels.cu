#ifdef _WIN32

#include "okuflow/cuda/cuda_kernels.hpp"

#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <math_constants.h>
#if __has_include(<cuda_surface_types.h>)
#include <cuda_surface_types.h>
#elif __has_include(<surface_types.h>)
#include <surface_types.h>
#else
#error "cuda surface type header not found"
#endif
#if __has_include(<surface_functions.h>)
#include <surface_functions.h>
#endif
#include <math.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>

namespace okuflow {

namespace {

__constant__ std::uint32_t gDisplayColorLut[256];

__device__ unsigned char FloatToByte(float value) {
    const float clamped = fminf(fmaxf(value, 0.0f), 1.0f);
    return static_cast<unsigned char>(clamped * 255.0f);
}

__device__ float3 ReadPixelLinear(const uchar4* src, size_t pitchBytes, int x, int y, int width, int height) {
    const int clampedX = max(0, min(width - 1, x));
    const int clampedY = max(0, min(height - 1, y));
    const auto* row = reinterpret_cast<const uchar4*>(reinterpret_cast<const uint8_t*>(src) + pitchBytes * clampedY);
    const uchar4 value = row[clampedX];
    return make_float3(value.x, value.y, value.z);
}

__device__ float3 BilinearSample(const uchar4* src, size_t pitchBytes,
                                 float sampleX, float sampleY,
                                 int width, int height) {
    const float fx = floorf(sampleX);
    const float fy = floorf(sampleY);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const float tx = sampleX - fx;
    const float ty = sampleY - fy;
    const float3 c00 = ReadPixelLinear(src, pitchBytes, x0, y0, width, height);
    const float3 c10 = ReadPixelLinear(src, pitchBytes, x1, y0, width, height);
    const float3 c01 = ReadPixelLinear(src, pitchBytes, x0, y1, width, height);
    const float3 c11 = ReadPixelLinear(src, pitchBytes, x1, y1, width, height);

    const float3 c0 = make_float3(c00.x + tx * (c10.x - c00.x),
                                  c00.y + tx * (c10.y - c00.y),
                                  c00.z + tx * (c10.z - c00.z));
    const float3 c1 = make_float3(c01.x + tx * (c11.x - c01.x),
                                  c01.y + tx * (c11.y - c01.y),
                                  c01.z + tx * (c11.z - c01.z));

    return make_float3(c0.x + ty * (c1.x - c0.x),
                       c0.y + ty * (c1.y - c0.y),
                       c0.z + ty * (c1.z - c0.z));
}

__device__ float3 BoxBlur3x3(const uchar4* src, size_t pitchBytes,
                             int x, int y, int width, int height) {
    float3 accum = make_float3(0.0f, 0.0f, 0.0f);
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const float3 c = ReadPixelLinear(src, pitchBytes, x + i, y + j, width, height);
            accum.x += c.x;
            accum.y += c.y;
            accum.z += c.z;
        }
    }
    const float inv = 1.0f / 9.0f;
    accum.x *= inv;
    accum.y *= inv;
    accum.z *= inv;
    return accum;
}

__global__ void GradientFillKernel(cudaSurfaceObject_t surface, int width, int height, float timeSeconds) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return;
    }

    const float fx = static_cast<float>(x) / static_cast<float>(width);
    const float fy = static_cast<float>(y) / static_cast<float>(height);
    const float phase = timeSeconds * 0.25f;

    const float r = 0.5f + 0.5f * sinf((fx + phase) * 6.28318f);
    const float g = 0.5f + 0.5f * sinf((fy + phase) * 6.28318f + 2.09439f);
    const float b = 0.5f + 0.5f * sinf(((fx + fy) * 0.5f + phase) * 6.28318f + 4.18878f);

    uchar4 color = make_uchar4(FloatToByte(b), FloatToByte(g), FloatToByte(r), 255);
    surf2Dwrite(color, surface, x * sizeof(uchar4), y);
}

} // namespace

void LaunchGradientKernel(cudaSurfaceObject_t surface, int width, int height, float timeSeconds) {
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);

    GradientFillKernel<<<gridSize, blockSize>>>(surface, width, height, timeSeconds);
}

namespace {

__device__ unsigned char ToGrayscaleChannel(const uchar4& pixel) {
    const float r = static_cast<float>(pixel.z) / 255.0f;
    const float g = static_cast<float>(pixel.y) / 255.0f;
    const float b = static_cast<float>(pixel.x) / 255.0f;
    const float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
    return FloatToByte(luminance);
}

__global__ void BlackWhiteKernel(cudaSurfaceObject_t surface, int width, int height, float threshold) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return;
    }

    const uchar4 pixel = surf2Dread<uchar4>(surface, x * static_cast<int>(sizeof(uchar4)), y);
    const float thresholdClamped = fminf(fmaxf(threshold, 0.0f), 1.0f);
    const unsigned char luminance = ToGrayscaleChannel(pixel);
    const unsigned char value = (static_cast<float>(luminance) / 255.0f) >= thresholdClamped ? 255 : 0;
    const uchar4 bwPixel = make_uchar4(value, value, value, pixel.w);
    surf2Dwrite(bwPixel, surface, x * sizeof(uchar4), y);
}

__global__ void ZoomKernel(cudaSurfaceObject_t surface, int width, int height, float zoomAmount) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return;
    }

    const float zoom = fmaxf(zoomAmount, 1.0f);

    const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
    const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);

    const float cx = 0.5f;
    const float cy = 0.5f;

    const float srcX = (nx - cx) / zoom + cx;
    const float srcY = (ny - cy) / zoom + cy;

    if (srcX < 0.0f || srcX > 1.0f || srcY < 0.0f || srcY > 1.0f) {
        surf2Dwrite(make_uchar4(0, 0, 0, 255), surface, x * sizeof(uchar4), y);
        return;
    }

    const int sampleX = static_cast<int>(srcX * static_cast<float>(width));
    const int sampleY = static_cast<int>(srcY * static_cast<float>(height));

    const int clampedX = max(0, min(width - 1, sampleX));
    const int clampedY = max(0, min(height - 1, sampleY));

    const uchar4 sampled = surf2Dread<uchar4>(surface, clampedX * static_cast<int>(sizeof(uchar4)), clampedY);
    surf2Dwrite(sampled, surface, x * sizeof(uchar4), y);
}

} // namespace

void LaunchBlackWhiteKernel(cudaSurfaceObject_t surface, int width, int height, float threshold) {
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);

    BlackWhiteKernel<<<gridSize, blockSize>>>(surface, width, height, threshold);
}

void LaunchZoomKernel(cudaSurfaceObject_t surface, int width, int height, float zoomAmount) {
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);

    ZoomKernel<<<gridSize, blockSize>>>(surface, width, height, zoomAmount);
}

namespace {

__device__ inline const uchar4* RowAt(const uchar4* base, size_t pitchBytes, int y) {
    return reinterpret_cast<const uchar4*>(reinterpret_cast<const char*>(base) + static_cast<size_t>(y) * pitchBytes);
}

__device__ inline uchar4* RowAt(uchar4* base, size_t pitchBytes, int y) {
    return reinterpret_cast<uchar4*>(reinterpret_cast<char*>(base) + static_cast<size_t>(y) * pitchBytes);
}

__device__ inline float4* RowAt(float4* base, size_t pitchBytes, int y) {
    return reinterpret_cast<float4*>(reinterpret_cast<char*>(base) + static_cast<size_t>(y) * pitchBytes);
}

__global__ void BlackWhiteLinearKernel(uchar4* dst, size_t dstPitch,
                                       const uchar4* src, size_t srcPitch,
                                       int width, int height, float threshold) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const uchar4* srcRow = RowAt(src, srcPitch, y);
    uchar4 pixel = srcRow[x];
    const float thresholdClamped = fminf(fmaxf(threshold, 0.0f), 1.0f);
    const unsigned char luminance = ToGrayscaleChannel(pixel);
    const unsigned char value = (static_cast<float>(luminance) / 255.0f) >= thresholdClamped ? 255 : 0;
    pixel.x = value;
    pixel.y = value;
    pixel.z = value;

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x] = pixel;
}

__global__ void ZoomLinearKernel(uchar4* dst, size_t dstPitch,
                                 const uchar4* src, size_t srcPitch,
                                 int width, int height,
                                 float zoomAmount, float centerXNorm, float centerYNorm) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const float zoom = fmaxf(zoomAmount, 1.0f);
    const float maxIndexX = static_cast<float>(max(width - 1, 0));
    const float maxIndexY = static_cast<float>(max(height - 1, 0));

    float centerX = fminf(fmaxf(centerXNorm, 0.0f), 1.0f) * maxIndexX;
    float centerY = fminf(fmaxf(centerYNorm, 0.0f), 1.0f) * maxIndexY;

    const float halfVisibleWidth = (static_cast<float>(width)) / (zoom * 2.0f);
    const float halfVisibleHeight = (static_cast<float>(height)) / (zoom * 2.0f);

    if (width > 1) {
        const float minCenterX = fmaxf(0.0f, halfVisibleWidth - 0.5f);
        const float maxCenterX = fminf(maxIndexX, static_cast<float>(width) - 1.0f - (halfVisibleWidth - 0.5f));
        if (minCenterX <= maxCenterX) {
            centerX = fminf(fmaxf(centerX, minCenterX), maxCenterX);
        }
    }

    if (height > 1) {
        const float minCenterY = fmaxf(0.0f, halfVisibleHeight - 0.5f);
        const float maxCenterY = fminf(maxIndexY, static_cast<float>(height) - 1.0f - (halfVisibleHeight - 0.5f));
        if (minCenterY <= maxCenterY) {
            centerY = fminf(fmaxf(centerY, minCenterY), maxCenterY);
        }
    }

    const float outputCenterX = maxIndexX * 0.5f;
    const float outputCenterY = maxIndexY * 0.5f;

    const float sx = (static_cast<float>(x) - outputCenterX) / zoom + centerX;
    const float sy = (static_cast<float>(y) - outputCenterY) / zoom + centerY;

    int sampleX = static_cast<int>(roundf(sx));
    int sampleY = static_cast<int>(roundf(sy));
    sampleX = max(0, min(width - 1, sampleX));
    sampleY = max(0, min(height - 1, sampleY));

    const uchar4* srcRow = RowAt(src, srcPitch, sampleY);
    const uchar4 sampled = srcRow[sampleX];

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x] = sampled;
}

__global__ void BlendLinearKernel(uchar4* dst, size_t dstPitch,
                                  const uchar4* base, size_t basePitch,
                                  const uchar4* enhanced, size_t enhancedPitch,
                                  int width, int height, float strength) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    const float amount = fminf(fmaxf(strength, 0.0f), 1.0f);
    const uchar4 a = RowAt(base, basePitch, y)[x];
    const uchar4 b = RowAt(enhanced, enhancedPitch, y)[x];
    uchar4 output;
    output.x = static_cast<unsigned char>(roundf(a.x + amount * (b.x - a.x)));
    output.y = static_cast<unsigned char>(roundf(a.y + amount * (b.y - a.y)));
    output.z = static_cast<unsigned char>(roundf(a.z + amount * (b.z - a.z)));
    output.w = static_cast<unsigned char>(roundf(a.w + amount * (b.w - a.w)));
    RowAt(dst, dstPitch, y)[x] = output;
}

constexpr int kMaxBlurRadius = 50;
__constant__ float gGaussianKernel[(kMaxBlurRadius * 2) + 1];
__constant__ int gGaussianRadius;

__global__ void GaussianBlurHorizontalKernel(uchar4* dst, size_t dstPitch,
                                             const uchar4* src, size_t srcPitch,
                                             int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const int radius = gGaussianRadius;

    float accumB = 0.0f;
    float accumG = 0.0f;
    float accumR = 0.0f;
    float accumA = 0.0f;

    const uchar4* srcRow = RowAt(src, srcPitch, y);
    for (int k = -radius; k <= radius; ++k) {
        int sampleX = x + k;
        sampleX = max(0, min(width - 1, sampleX));
        const uchar4 sample = srcRow[sampleX];
        const float weight = gGaussianKernel[k + radius];
        accumB += weight * sample.x;
        accumG += weight * sample.y;
        accumR += weight * sample.z;
        accumA += weight * sample.w;
    }

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x] = make_uchar4(FloatToByte(accumB / 255.0f),
                             FloatToByte(accumG / 255.0f),
                             FloatToByte(accumR / 255.0f),
                             FloatToByte(accumA / 255.0f));
}

__global__ void GaussianBlurVerticalKernel(uchar4* dst, size_t dstPitch,
                                           const uchar4* src, size_t srcPitch,
                                           int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const int radius = gGaussianRadius;

    float accumB = 0.0f;
    float accumG = 0.0f;
    float accumR = 0.0f;
    float accumA = 0.0f;

    for (int k = -radius; k <= radius; ++k) {
        int sampleY = y + k;
        sampleY = max(0, min(height - 1, sampleY));
        const uchar4* srcRow = RowAt(src, srcPitch, sampleY);
        const uchar4 sample = srcRow[x];
        const float weight = gGaussianKernel[k + radius];
        accumB += weight * sample.x;
        accumG += weight * sample.y;
        accumR += weight * sample.z;
        accumA += weight * sample.w;
    }

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x] = make_uchar4(FloatToByte(accumB / 255.0f),
                             FloatToByte(accumG / 255.0f),
                             FloatToByte(accumR / 255.0f),
                             FloatToByte(accumA / 255.0f));
}

__global__ void FocusMarkerKernel(uchar4* buffer, size_t pitchBytes,
                                  int width, int height,
                                  float centerXNorm, float centerYNorm,
                                  float outerRadius, float innerRadius) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const float centerX = centerXNorm * (width - 1.0f);
    const float centerY = centerYNorm * (height - 1.0f);
    const float dx = static_cast<float>(x) - centerX;
    const float dy = static_cast<float>(y) - centerY;
    const float distSq = dx * dx + dy * dy;

    if (distSq > outerRadius * outerRadius) {
        return;
    }

    uchar4* row = RowAt(buffer, pitchBytes, y);

    if (distSq <= innerRadius * innerRadius) {
        row[x] = make_uchar4(255, 255, 255, 255);
    } else {
        row[x] = make_uchar4(0, 0, 255, 255);
    }
}

__global__ void TemporalSmoothKernel(uchar4* dst, size_t dstPitch,
                                     const uchar4* src, size_t srcPitch,
                                     float4* history, size_t historyPitch,
                                     int width, int height,
                                     float alpha, int historySeed) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const float clampedAlpha = fminf(fmaxf(alpha, 0.0f), 1.0f);
    const float oneMinusAlpha = 1.0f - clampedAlpha;
    const bool hasHistory = historySeed != 0;

    const uchar4* srcRow = RowAt(src, srcPitch, y);
    uchar4 current = srcRow[x];

    float4* historyRow = RowAt(history, historyPitch, y);
    float4 previous = hasHistory ? historyRow[x]
                                 : make_float4(static_cast<float>(current.x),
                                              static_cast<float>(current.y),
                                              static_cast<float>(current.z),
                                              255.0f);

    const float currB = static_cast<float>(current.x);
    const float currG = static_cast<float>(current.y);
    const float currR = static_cast<float>(current.z);

    float4 blended;
    blended.x = clampedAlpha * currB + oneMinusAlpha * previous.x;
    blended.y = clampedAlpha * currG + oneMinusAlpha * previous.y;
    blended.z = clampedAlpha * currR + oneMinusAlpha * previous.z;
    blended.w = 255.0f;

    blended.x = fminf(fmaxf(blended.x, 0.0f), 255.0f);
    blended.y = fminf(fmaxf(blended.y, 0.0f), 255.0f);
    blended.z = fminf(fmaxf(blended.z, 0.0f), 255.0f);

    historyRow[x] = blended;

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x] = make_uchar4(static_cast<unsigned char>(blended.x + 0.5f),
                            static_cast<unsigned char>(blended.y + 0.5f),
                            static_cast<unsigned char>(blended.z + 0.5f),
                            255u);
}

inline void CheckCuda(const char* message) {
    cudaError_t status = cudaGetLastError();
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(message) + ": " + cudaGetErrorString(status));
    }
}

} // namespace

void LaunchBlackWhiteLinear(uchar4* dst, size_t dstPitchBytes,
                            const uchar4* src, size_t srcPitchBytes,
                            int width, int height, float threshold, cudaStream_t stream) {
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    BlackWhiteLinearKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes, src, srcPitchBytes,
                                                              width, height, threshold);
    CheckCuda("BlackWhiteLinearKernel launch failed");
}

void LaunchZoomLinear(uchar4* dst, size_t dstPitchBytes,
                      const uchar4* src, size_t srcPitchBytes,
                      int width, int height,
                      float zoomAmount, float centerXNorm, float centerYNorm,
                      cudaStream_t stream) {
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    ZoomLinearKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes, src, srcPitchBytes,
                                                        width, height, zoomAmount, centerXNorm, centerYNorm);
    CheckCuda("ZoomLinearKernel launch failed");
}

void LaunchBlendLinear(uchar4* dst, size_t dstPitchBytes,
                       const uchar4* base, size_t basePitchBytes,
                       const uchar4* enhanced, size_t enhancedPitchBytes,
                       int width, int height, float strength,
                       cudaStream_t stream) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    BlendLinearKernel<<<gridSize, blockSize, 0, stream>>>(
        dst, dstPitchBytes, base, basePitchBytes, enhanced, enhancedPitchBytes,
        width, height, strength);
    CheckCuda("BlendLinearKernel launch failed");
}

void LaunchGaussianBlurLinear(uchar4* dst, size_t dstPitchBytes,
                              uchar4* scratch, size_t scratchPitchBytes,
                              const uchar4* src, size_t srcPitchBytes,
                              int width, int height,
                              cudaStream_t stream) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);

    GaussianBlurHorizontalKernel<<<gridSize, blockSize, 0, stream>>>(scratch, scratchPitchBytes, src, srcPitchBytes, width, height);
    CheckCuda("GaussianBlurHorizontalKernel launch failed");

    GaussianBlurVerticalKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes, scratch, scratchPitchBytes, width, height);
    CheckCuda("GaussianBlurVerticalKernel launch failed");
}

void LaunchFocusMarkerLinear(uchar4* buffer, size_t pitchBytes,
                             int width, int height,
                             float centerXNorm, float centerYNorm,
                             cudaStream_t stream) {
    const float dimension = static_cast<float>(std::min(width, height));
    const float radiusOuter = fmaxf(6.0f, dimension * 0.03f);
    const float radiusInner = radiusOuter * 0.35f;

    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);

    FocusMarkerKernel<<<gridSize, blockSize, 0, stream>>>(buffer, pitchBytes, width, height,
                                                          centerXNorm, centerYNorm,
                                                          radiusOuter, radiusInner);
    CheckCuda("FocusMarkerKernel launch failed");
}

void LaunchTemporalSmoothLinear(uchar4* dst, size_t dstPitchBytes,
                                const uchar4* src, size_t srcPitchBytes,
                                float4* history, size_t historyPitchBytes,
                                int width, int height,
                                float alpha,
                                bool historyValid,
                                cudaStream_t stream) {
    if (width <= 0 || height <= 0) {
        return;
    }

    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);

    TemporalSmoothKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes,
                                                            src, srcPitchBytes,
                                                            history, historyPitchBytes,
                                                            width, height,
                                                            alpha,
                                                            historyValid ? 1 : 0);
    CheckCuda("TemporalSmoothKernel launch failed");
}

namespace {

inline void CheckCudaStatus(cudaError_t status, const char* message) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(message) + ": " + cudaGetErrorString(status));
    }
}

constexpr int kTripodReacquireRadius = 96;
constexpr int kTripodReacquireCandidateCount =
    kTripodReacquireRadius * 2 + 1;
constexpr int kStabMinOverlap = 8;
constexpr int kStabEstimateBlockSize = 128;
constexpr float kStabInvalidSad = 3.402823466e+38f;
constexpr int kStabFeatureCellSize = 16;
constexpr int kStabFeatureBlockSize = 128;
constexpr int kStabRansacHypotheses = 50;
constexpr int kStabMinSimilarityInliers = 12;
// Box-average a factorX x factorY block of the BGRA source into one luma value.
// Buffers are BGRA (uchar4: x=B, y=G, z=R, w=A), so luma = 0.299*z + 0.587*y + 0.114*x.
__global__ void StabilizationLumaDownsampleKernel(float* dstLuma,
                                                  int smallWidth, int smallHeight,
                                                  const uchar4* src, size_t srcPitch,
                                                  int width, int height,
                                                  int factorX, int factorY) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= smallWidth || y >= smallHeight) {
        return;
    }

    const int x0 = x * factorX;
    const int y0 = y * factorY;
    const int x1 = min(x0 + factorX, width);
    const int y1 = min(y0 + factorY, height);

    float sum = 0.0f;
    int count = 0;
    for (int sy = y0; sy < y1; ++sy) {
        const uchar4* row = RowAt(src, srcPitch, sy);
        for (int sx = x0; sx < x1; ++sx) {
            const uchar4 pixel = row[sx];
            sum += 0.299f * static_cast<float>(pixel.z) +
                   0.587f * static_cast<float>(pixel.y) +
                   0.114f * static_cast<float>(pixel.x);
            ++count;
        }
    }

    dstLuma[y * smallWidth + x] = (count > 0) ? sum / static_cast<float>(count) : 0.0f;
}

__global__ void StabilizationProjectionKernel(const float* luma,
                                              int smallWidth, int smallHeight,
                                              float* colProj, float* rowProj) {
    __shared__ float colPart[16];
    __shared__ float rowPart[16];
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (threadIdx.y == 0) {
        colPart[threadIdx.x] = 0.0f;
    }
    if (threadIdx.x == 0) {
        rowPart[threadIdx.y] = 0.0f;
    }
    __syncthreads();

    if (x < smallWidth && y < smallHeight) {
        const float value = luma[y * smallWidth + x];
        atomicAdd(&colPart[threadIdx.x], value);
        atomicAdd(&rowPart[threadIdx.y], value);
    }
    __syncthreads();

    // Shared-memory contention is local to the SM. Only one partial per row
    // and column reaches global memory from each 16x16 block.
    if (threadIdx.y == 0 && x < smallWidth) {
        atomicAdd(&colProj[x], colPart[threadIdx.x]);
    }
    if (threadIdx.x == 0 && y < smallHeight) {
        atomicAdd(&rowProj[y], rowPart[threadIdx.y]);
    }
}

__device__ void ResetStabilizationState(StabilizationState* state) {
    state->actualPath = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->filteredPath = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->correction = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->previousCorrection = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->lastFrameMotion = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->diagnostics = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->tripodAnchorCorrection = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->tripodAbsoluteCorrection = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->tripodRelativeMotion = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->tripodDiagnostics = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
}

__device__ float ProfileZeroMeanSad(const float* current,
                                    const float* reference,
                                    int length, int shift) {
    float currentMean = 0.0f;
    float referenceMean = 0.0f;
    int count = 0;
    for (int index = 0; index < length; ++index) {
        const int referenceIndex = index - shift;
        if (referenceIndex >= 0 && referenceIndex < length) {
            currentMean += current[index];
            referenceMean += reference[referenceIndex];
            ++count;
        }
    }
    if (count < kStabMinOverlap) {
        return kStabInvalidSad;
    }
    const float inverseCount = 1.0f / static_cast<float>(count);
    currentMean *= inverseCount;
    referenceMean *= inverseCount;
    float sum = 0.0f;
    for (int index = 0; index < length; ++index) {
        const int referenceIndex = index - shift;
        if (referenceIndex >= 0 && referenceIndex < length) {
            sum += fabsf((current[index] - currentMean) -
                         (reference[referenceIndex] - referenceMean));
        }
    }
    return sum * inverseCount;
}

__device__ float4 ClampStabilizationCorrection(float4 correction,
                                               float maxCorrectionFraction,
                                               int fullWidth,
                                               int fullHeight,
                                               bool* translationLimited) {
    const float translationLimit =
        fminf(fmaxf(maxCorrectionFraction, 0.06f), 0.45f);
    const float unclampedX = correction.x;
    const float unclampedY = correction.y;
    correction.x = fminf(fmaxf(correction.x, -translationLimit * fullWidth),
                         translationLimit * fullWidth);
    correction.y = fminf(fmaxf(correction.y, -translationLimit * fullHeight),
                         translationLimit * fullHeight);
    correction.z = fminf(fmaxf(correction.z, -0.0523599f), 0.0523599f);
    correction.w = fminf(fmaxf(correction.w, -0.04f), 0.04f);
    if (translationLimited) {
        *translationLimited =
            fabsf(correction.x - unclampedX) > 0.01f ||
            fabsf(correction.y - unclampedY) > 0.01f;
    }
    return correction;
}

__device__ void UpdateVirtualTripodPath(StabilizationState* state,
                                        float4 referenceMotion,
                                        bool motionValid,
                                        float strength,
                                        float zoomAmount,
                                        float maxCorrectionFraction,
                                        int fullWidth,
                                        int fullHeight) {
    state->previousCorrection = state->correction;
    if (!motionValid) {
        state->diagnostics.y = 0.0f;
        state->diagnostics.w = 0.0f;
        state->tripodDiagnostics.x = 0.0f;
        state->tripodDiagnostics.y += 1.0f;
        state->tripodDiagnostics.z = 2.0f;
        // A rejected absolute model cannot inject drift. Keep both the last
        // correction and the last trustworthy motion unchanged until the
        // fixed reference matches again or the host deliberately re-locks.
        return;
    }

    // Keep the raw accepted measurement as the next prepared-LK seed. The
    // displayed correction is clamped below, but feeding that clamp back into
    // tracking creates a hard capture-range cliff once the rig moves farther
    // than the crop reserve.
    state->lastFrameMotion = referenceMotion;
    state->actualPath = referenceMotion;
    state->filteredPath = state->tripodAnchorCorrection;

    strength = fminf(fmaxf(strength, 0.0f), 1.0f);
    zoomAmount = fmaxf(zoomAmount, 1.0f);
    // Let very slow mount settling restore crop authority at lower strengths.
    // At maximum strength this is exactly zero, preserving a literal fixed
    // coordinate system. The rate is capped to roughly one display pixel per
    // second at 30 camera FPS and never integrates pairwise motion.
    const float settleBlend = 1.0f - strength;
    const float settleStep =
        settleBlend * settleBlend * settleBlend /
        (30.0f * zoomAmount);
    const float anchorDeltaX =
        referenceMotion.x - state->tripodAnchorCorrection.x;
    const float anchorDeltaY =
        referenceMotion.y - state->tripodAnchorCorrection.y;
    state->tripodAnchorCorrection.x +=
        fminf(fmaxf(anchorDeltaX, -settleStep), settleStep);
    state->tripodAnchorCorrection.y +=
        fminf(fmaxf(anchorDeltaY, -settleStep), settleStep);

    bool translationLimited = false;
    const float4 targetCorrection = ClampStabilizationCorrection(
        make_float4(state->tripodAnchorCorrection.x - referenceMotion.x,
                    state->tripodAnchorCorrection.y - referenceMotion.y,
                    state->tripodAnchorCorrection.z - referenceMotion.z,
                    state->tripodAnchorCorrection.w - referenceMotion.w),
        maxCorrectionFraction, fullWidth, fullHeight, &translationLimited);
    const float deadbandDisplayPixels =
        0.65f + strength * (0.10f - 0.65f);
    const float deadbandSourcePixels =
        deadbandDisplayPixels / zoomAmount;
    const float maximumStepSourcePixels =
        (12.0f + strength * 84.0f) / zoomAmount;
    float deltaX = targetCorrection.x - state->correction.x;
    float deltaY = targetCorrection.y - state->correction.y;
    const float deltaLength = hypotf(deltaX, deltaY);
    if (deltaLength > deadbandSourcePixels) {
        const float appliedLength =
            fminf(deltaLength - deadbandSourcePixels,
                  maximumStepSourcePixels);
        const float gain = appliedLength / deltaLength;
        state->correction.x += deltaX * gain;
        state->correction.y += deltaY * gain;
    }

    const float displayRadius =
        0.5f * hypotf(static_cast<float>(fullWidth),
                      static_cast<float>(fullHeight)) *
        zoomAmount;
    const float angularDeadband =
        deadbandDisplayPixels / fmaxf(displayRadius, 1.0f);
    const float scaleDeadband = angularDeadband;
    const float angularDelta = targetCorrection.z - state->correction.z;
    if (fabsf(angularDelta) > angularDeadband) {
        const float magnitude =
            fminf(fabsf(angularDelta) - angularDeadband,
                  0.004f + strength * 0.016f);
        state->correction.z += copysignf(magnitude, angularDelta);
    }
    const float scaleDelta = targetCorrection.w - state->correction.w;
    if (fabsf(scaleDelta) > scaleDeadband) {
        const float magnitude =
            fminf(fabsf(scaleDelta) - scaleDeadband,
                  0.002f + strength * 0.008f);
        state->correction.w += copysignf(magnitude, scaleDelta);
    }
    state->tripodAbsoluteCorrection = state->correction;
    state->tripodRelativeMotion =
        make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->tripodDiagnostics.x += 1.0f;
    state->tripodDiagnostics.y = 0.0f;
    state->tripodDiagnostics.z = translationLimited ? 3.0f : 1.0f;
}

__global__ void VirtualTripodProjectionSeedKernel(
    const float* currentColProj, const float* currentRowProj,
    const float* referenceColProj, const float* referenceRowProj,
    int smallWidth, int smallHeight, float factorX, float factorY,
    const float4* keyframeOrigins, const unsigned int* keyframeValid,
    unsigned int keyframeIndex, StabilizationState* state) {
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }
    if (state->tripodDiagnostics.y < 0.5f ||
        keyframeValid[keyframeIndex] == 0u) {
        return;
    }

    int bestX = kTripodReacquireRadius;
    int bestY = kTripodReacquireRadius;
    float bestHorizontalError = kStabInvalidSad;
    float bestVerticalError = kStabInvalidSad;
    for (int candidate = 0;
         candidate < kTripodReacquireCandidateCount; ++candidate) {
        const int shift = candidate - kTripodReacquireRadius;
        const float horizontalError =
            abs(shift) < smallWidth - kStabMinOverlap
                ? ProfileZeroMeanSad(
                      currentColProj, referenceColProj, smallWidth, shift)
                : kStabInvalidSad;
        const float verticalError =
            abs(shift) < smallHeight - kStabMinOverlap
                ? ProfileZeroMeanSad(
                      currentRowProj, referenceRowProj, smallHeight, shift)
                : kStabInvalidSad;
        if (horizontalError < bestHorizontalError) {
            bestX = candidate;
            bestHorizontalError = horizontalError;
        }
        if (verticalError < bestVerticalError) {
            bestY = candidate;
            bestVerticalError = verticalError;
        }
    }
    const float4 origin = keyframeOrigins[keyframeIndex];
    state->lastFrameMotion.x =
        origin.x + static_cast<float>(bestX - kTripodReacquireRadius) *
                       factorX;
    state->lastFrameMotion.y =
        origin.y + static_cast<float>(bestY - kTripodReacquireRadius) *
                       factorY;
    state->lastFrameMotion.z = origin.z;
    state->lastFrameMotion.w = origin.w;
}

__device__ float SampleLuma(const float* image, int width, int height,
                            float x, float y) {
    x = fminf(fmaxf(x, 0.0f), static_cast<float>(width - 1));
    y = fminf(fmaxf(y, 0.0f), static_cast<float>(height - 1));
    const int x0 = static_cast<int>(floorf(x));
    const int y0 = static_cast<int>(floorf(y));
    const int x1 = min(x0 + 1, width - 1);
    const int y1 = min(y0 + 1, height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float top = image[y0 * width + x0] +
                      fx * (image[y0 * width + x1] - image[y0 * width + x0]);
    const float bottom = image[y1 * width + x0] +
                         fx * (image[y1 * width + x1] - image[y1 * width + x0]);
    return top + fy * (bottom - top);
}

__device__ bool TrackFeaturePyramidal(const float* previous,
                                      const float* current,
                                      int width, int height,
                                      float x, float y,
                                      float maxDisplacementPixels,
                                      float initialDisplacementX,
                                      float initialDisplacementY,
                                      float& displacementX,
                                      float& displacementY) {
    displacementX = initialDisplacementX;
    displacementY = initialDisplacementY;
    bool wellConditioned = false;

    // A three-level sparse LK solve without materializing image pyramids:
    // coarse levels subsample the same small-luma image at 4/2/1 pixel steps.
    for (int level = 2; level >= 0; --level) {
        const int step = 1 << level;
        const int radius = 10;
        for (int iteration = 0; iteration < 4; ++iteration) {
            float gxx = 0.0f;
            float gxy = 0.0f;
            float gyy = 0.0f;
            float bx = 0.0f;
            float by = 0.0f;
            int samples = 0;
            for (int oy = -radius; oy <= radius; oy += step) {
                for (int ox = -radius; ox <= radius; ox += step) {
                    const float px = x + static_cast<float>(ox);
                    const float py = y + static_cast<float>(oy);
                    const float qx = px + displacementX;
                    const float qy = py + displacementY;
                    if (px < 2.0f || py < 2.0f ||
                        px >= width - 2.0f || py >= height - 2.0f ||
                        qx < 2.0f || qy < 2.0f ||
                        qx >= width - 2.0f || qy >= height - 2.0f) {
                        continue;
                    }
                    const float gradientX =
                        0.25f * ((SampleLuma(previous, width, height, px + 1.0f, py) -
                                  SampleLuma(previous, width, height, px - 1.0f, py)) +
                                 (SampleLuma(current, width, height, qx + 1.0f, qy) -
                                  SampleLuma(current, width, height, qx - 1.0f, qy)));
                    const float gradientY =
                        0.25f * ((SampleLuma(previous, width, height, px, py + 1.0f) -
                                  SampleLuma(previous, width, height, px, py - 1.0f)) +
                                 (SampleLuma(current, width, height, qx, qy + 1.0f) -
                                  SampleLuma(current, width, height, qx, qy - 1.0f)));
                    const float temporal =
                        SampleLuma(current, width, height, qx, qy) -
                        SampleLuma(previous, width, height, px, py);
                    gxx += gradientX * gradientX;
                    gxy += gradientX * gradientY;
                    gyy += gradientY * gradientY;
                    bx += gradientX * temporal;
                    by += gradientY * temporal;
                    ++samples;
                }
            }

            const float determinant = gxx * gyy - gxy * gxy;
            if (samples < 20 || determinant < 1.0e4f) {
                return false;
            }
            wellConditioned = true;
            float deltaX = (-gyy * bx + gxy * by) / determinant;
            float deltaY = (gxy * bx - gxx * by) / determinant;
            const float maxStep = static_cast<float>(step) * 1.5f;
            deltaX = fminf(fmaxf(deltaX, -maxStep), maxStep);
            deltaY = fminf(fmaxf(deltaY, -maxStep), maxStep);
            displacementX += deltaX;
            displacementY += deltaY;
            if (deltaX * deltaX + deltaY * deltaY < 0.0025f) {
                break;
            }
        }
    }

    return wellConditioned &&
           displacementX * displacementX + displacementY * displacementY <
               maxDisplacementPixels * maxDisplacementPixels;
}

__global__ void GaussianPyramidDownsampleKernel(
    const float* source, int sourceWidth, int sourceHeight,
    float* destination, int destinationWidth, int destinationHeight) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= destinationWidth || y >= destinationHeight) {
        return;
    }
    constexpr float weights[5] = {1.0f, 4.0f, 6.0f, 4.0f, 1.0f};
    float sum = 0.0f;
    float weightSum = 0.0f;
    const int sourceX = x * 2;
    const int sourceY = y * 2;
    for (int oy = -2; oy <= 2; ++oy) {
        const int sampleY = max(0, min(sourceHeight - 1, sourceY + oy));
        for (int ox = -2; ox <= 2; ++ox) {
            const int sampleX = max(0, min(sourceWidth - 1, sourceX + ox));
            const float weight = weights[ox + 2] * weights[oy + 2];
            sum += source[sampleY * sourceWidth + sampleX] * weight;
            weightSum += weight;
        }
    }
    destination[y * destinationWidth + x] = sum / weightSum;
}

__device__ float HarrisScoreAt(const float* image, int width, int height,
                               int x, int y) {
    if (x < 2 || y < 2 || x >= width - 2 || y >= height - 2) {
        return -1.0f;
    }
    float gxx = 0.0f;
    float gxy = 0.0f;
    float gyy = 0.0f;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            const int px = x + ox;
            const int py = y + oy;
            const float gx =
                0.5f * (image[py * width + px + 1] -
                        image[py * width + px - 1]);
            const float gy =
                0.5f * (image[(py + 1) * width + px] -
                        image[(py - 1) * width + px]);
            gxx += gx * gx;
            gxy += gx * gy;
            gyy += gy * gy;
        }
    }
    const float determinant = gxx * gyy - gxy * gxy;
    const float trace = gxx + gyy;
    return determinant - 0.04f * trace * trace;
}

__device__ bool ComputeTranslationInverseHessian(
    const float* image, int width, int height, float x, float y,
    float4& inverseHessian) {
    constexpr int radius = 4;
    if (x < radius + 2.0f || y < radius + 2.0f ||
        x >= width - radius - 2.0f || y >= height - radius - 2.0f) {
        return false;
    }
    float gxx = 0.0f;
    float gxy = 0.0f;
    float gyy = 0.0f;
    for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
            const float px = x + static_cast<float>(ox);
            const float py = y + static_cast<float>(oy);
            const float gx =
                0.5f * (SampleLuma(image, width, height, px + 1.0f, py) -
                        SampleLuma(image, width, height, px - 1.0f, py));
            const float gy =
                0.5f * (SampleLuma(image, width, height, px, py + 1.0f) -
                        SampleLuma(image, width, height, px, py - 1.0f));
            gxx += gx * gx;
            gxy += gx * gy;
            gyy += gy * gy;
        }
    }
    const float determinant = gxx * gyy - gxy * gxy;
    const float trace = gxx + gyy;
    // Scale the conditioning gate with local gradient energy instead of using
    // one absolute corner threshold that starves dim lecture halls.
    if (trace < 1.0f || determinant < 1.0e-4f * trace * trace) {
        return false;
    }
    const float inverseDeterminant = 1.0f / determinant;
    inverseHessian =
        make_float4(gyy * inverseDeterminant,
                    -gxy * inverseDeterminant,
                    gxx * inverseDeterminant, trace);
    return true;
}

__global__ void PrepareVirtualTripodReferenceKernel(
    const float* level0, int level0Width, int level0Height,
    const float* level1, int level1Width, int level1Height,
    const float* level2, int level2Width, int level2Height,
    TripodReferenceFeature* features, unsigned int* featureCount,
    unsigned int maxFeatures) {
    __shared__ float scores[kStabFeatureBlockSize];
    __shared__ int indices[kStabFeatureBlockSize];
    const int tid = static_cast<int>(threadIdx.x);
    const int cellX = static_cast<int>(blockIdx.x) * kStabFeatureCellSize;
    const int cellY = static_cast<int>(blockIdx.y) * kStabFeatureCellSize;
    float bestScore = 0.0f;
    int bestIndex = -1;
    for (int local = tid;
         local < kStabFeatureCellSize * kStabFeatureCellSize;
         local += blockDim.x) {
        const int x = cellX + local % kStabFeatureCellSize;
        const int y = cellY + local / kStabFeatureCellSize;
        // Coarsest-level inverse compositional patches need this margin.
        if (x < 24 || y < 24 || x >= level0Width - 24 ||
            y >= level0Height - 24) {
            continue;
        }
        const float score = HarrisScoreAt(level0, level0Width, level0Height, x, y);
        if (score > bestScore) {
            bestScore = score;
            bestIndex = y * level0Width + x;
        }
    }
    scores[tid] = bestScore;
    indices[tid] = bestIndex;
    __syncthreads();
    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride && scores[tid + stride] > scores[tid]) {
            scores[tid] = scores[tid + stride];
            indices[tid] = indices[tid + stride];
        }
        __syncthreads();
    }
    if (tid != 0 || indices[0] < 0 || scores[0] <= 0.0f) {
        return;
    }

    const int integerX = indices[0] % level0Width;
    const int integerY = indices[0] / level0Width;
    const float left =
        HarrisScoreAt(level0, level0Width, level0Height, integerX - 1, integerY);
    const float right =
        HarrisScoreAt(level0, level0Width, level0Height, integerX + 1, integerY);
    const float up =
        HarrisScoreAt(level0, level0Width, level0Height, integerX, integerY - 1);
    const float down =
        HarrisScoreAt(level0, level0Width, level0Height, integerX, integerY + 1);
    const float denominatorX = left - 2.0f * scores[0] + right;
    const float denominatorY = up - 2.0f * scores[0] + down;
    const float offsetX =
        fabsf(denominatorX) > 1.0e-6f
            ? fminf(fmaxf(0.5f * (left - right) / denominatorX, -0.5f), 0.5f)
            : 0.0f;
    const float offsetY =
        fabsf(denominatorY) > 1.0e-6f
            ? fminf(fmaxf(0.5f * (up - down) / denominatorY, -0.5f), 0.5f)
            : 0.0f;

    TripodReferenceFeature feature{};
    feature.position =
        make_float2(static_cast<float>(integerX) + offsetX,
                    static_cast<float>(integerY) + offsetY);
    if (!ComputeTranslationInverseHessian(
            level0, level0Width, level0Height,
            feature.position.x, feature.position.y, feature.inverseHessian[0]) ||
        !ComputeTranslationInverseHessian(
            level1, level1Width, level1Height,
            feature.position.x * 0.5f, feature.position.y * 0.5f,
            feature.inverseHessian[1]) ||
        !ComputeTranslationInverseHessian(
            level2, level2Width, level2Height,
            feature.position.x * 0.25f, feature.position.y * 0.25f,
            feature.inverseHessian[2])) {
        return;
    }
    const unsigned int output = atomicAdd(featureCount, 1u);
    if (output < maxFeatures) {
        features[output] = feature;
    }
}

__device__ bool TrackPreparedTripodFeature(
    const TripodReferenceFeature& feature,
    const float* currentLevels[3], const float* referenceLevels[3],
    const int widths[3], const int heights[3],
    float initialDx, float initialDy, float& outputDx, float& outputDy) {
    constexpr int radius = 4;
    float dx = initialDx * 0.25f;
    float dy = initialDy * 0.25f;
    for (int level = 2; level >= 0; --level) {
        if (level < 2) {
            dx *= 2.0f;
            dy *= 2.0f;
        }
        const float scale = static_cast<float>(1 << level);
        const float x = feature.position.x / scale;
        const float y = feature.position.y / scale;
        const float4 inverse = feature.inverseHessian[level];
        for (int iteration = 0; iteration < 8; ++iteration) {
            if (x + dx < radius + 2.0f || y + dy < radius + 2.0f ||
                x + dx >= widths[level] - radius - 2.0f ||
                y + dy >= heights[level] - radius - 2.0f) {
                return false;
            }
            float bx = 0.0f;
            float by = 0.0f;
            for (int oy = -radius; oy <= radius; ++oy) {
                for (int ox = -radius; ox <= radius; ++ox) {
                    const float px = x + static_cast<float>(ox);
                    const float py = y + static_cast<float>(oy);
                    const float gx =
                        0.5f * (SampleLuma(referenceLevels[level], widths[level],
                                           heights[level], px + 1.0f, py) -
                                SampleLuma(referenceLevels[level], widths[level],
                                           heights[level], px - 1.0f, py));
                    const float gy =
                        0.5f * (SampleLuma(referenceLevels[level], widths[level],
                                           heights[level], px, py + 1.0f) -
                                SampleLuma(referenceLevels[level], widths[level],
                                           heights[level], px, py - 1.0f));
                    const float error =
                        SampleLuma(currentLevels[level], widths[level],
                                   heights[level], px + dx, py + dy) -
                        SampleLuma(referenceLevels[level], widths[level],
                                   heights[level], px, py);
                    bx += gx * error;
                    by += gy * error;
                }
            }
            float deltaX = -(inverse.x * bx + inverse.y * by);
            float deltaY = -(inverse.y * bx + inverse.z * by);
            deltaX = fminf(fmaxf(deltaX, -2.5f), 2.5f);
            deltaY = fminf(fmaxf(deltaY, -2.5f), 2.5f);
            dx += deltaX;
            dy += deltaY;
            if (deltaX * deltaX + deltaY * deltaY < 0.0004f) {
                break;
            }
        }
    }

    float absoluteError = 0.0f;
    int samples = 0;
    for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
            const float px = feature.position.x + static_cast<float>(ox);
            const float py = feature.position.y + static_cast<float>(oy);
            absoluteError +=
                fabsf(SampleLuma(currentLevels[0], widths[0], heights[0],
                                 px + dx, py + dy) -
                      SampleLuma(referenceLevels[0], widths[0], heights[0], px, py));
            ++samples;
        }
    }
    if (absoluteError / static_cast<float>(samples) > 32.0f) {
        return false;
    }
    outputDx = dx;
    outputDy = dy;
    return isfinite(dx) && isfinite(dy);
}

__global__ void PreparedVirtualTripodFeaturePairsKernel(
    const float* currentLevel0, int level0Width, int level0Height,
    const float* currentLevel1, int level1Width, int level1Height,
    const float* currentLevel2, int level2Width, int level2Height,
    const float* referenceLevel0, const float* referenceLevel1,
    const float* referenceLevel2,
    const TripodReferenceFeature* features, const unsigned int* featureCount,
    float factorX, float factorY,
    float4* pairs, unsigned int* pairCount, unsigned int maxPairs,
    const StabilizationState* motionSeedState,
    const float4* keyframeOrigins,
    const unsigned int* keyframeValid,
    unsigned int keyframeIndex,
    const TripodMatchCandidate* earlierCandidates) {
    const unsigned int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (keyframeValid != nullptr && keyframeValid[keyframeIndex] == 0u) {
        return;
    }
    if (earlierCandidates != nullptr) {
        for (unsigned int candidate = 0; candidate < keyframeIndex;
             ++candidate) {
            if (earlierCandidates[candidate].diagnostics.y >= 0.5f) {
                return;
            }
        }
    }
    const unsigned int count = min(*featureCount, maxPairs);
    if (index >= count) {
        return;
    }
    const float* currentLevels[3] = {
        currentLevel0, currentLevel1, currentLevel2};
    const float* referenceLevels[3] = {
        referenceLevel0, referenceLevel1, referenceLevel2};
    const int widths[3] = {level0Width, level1Width, level2Width};
    const int heights[3] = {level0Height, level1Height, level2Height};
    float4 seed = motionSeedState != nullptr
                      ? motionSeedState->lastFrameMotion
                      : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (keyframeOrigins != nullptr) {
        const float4 origin = keyframeOrigins[keyframeIndex];
        seed.x -= origin.x;
        seed.y -= origin.y;
        seed.z -= origin.z;
        seed.w -= origin.w;
    }
    float dx = 0.0f;
    float dy = 0.0f;
    if (!TrackPreparedTripodFeature(
            features[index], currentLevels, referenceLevels, widths, heights,
            seed.x / factorX, seed.y / factorY, dx, dy)) {
        return;
    }
    const float maxDisplacement =
        0.48f * static_cast<float>(max(level0Width, level0Height));
    if (dx * dx + dy * dy > maxDisplacement * maxDisplacement) {
        return;
    }
    const unsigned int output = atomicAdd(pairCount, 1u);
    if (output < maxPairs) {
        const float2 point = features[index].position;
        pairs[output] =
            make_float4(point.x * factorX, point.y * factorY,
                        (point.x + dx) * factorX, (point.y + dy) * factorY);
    }
}

__global__ void VirtualTripodFocusKernel(const float* luma, int width, int height,
                                         float* focusScore) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x <= 0 || y <= 0 || x >= width - 1 || y >= height - 1) {
        return;
    }
    const float center = luma[y * width + x];
    const float laplacian =
        luma[y * width + x - 1] + luma[y * width + x + 1] +
        luma[(y - 1) * width + x] + luma[(y + 1) * width + x] -
        4.0f * center;
    atomicAdd(focusScore, laplacian * laplacian);
}

__global__ void ResetBumpHoldStateKernel(BumpHoldState* state) {
    if (threadIdx.x != 0 || blockIdx.x != 0 || state == nullptr) {
        return;
    }
    state->diagnostics = make_float4(0.0f, 1.0f, 0.0f, 0.0f);
    state->previousMotion = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    state->heldFrameValid = 0u;
    state->stableFrames = 0u;
    state->recoveryFrame = 0u;
    state->captureCurrent = 0u;
    state->blendAlpha = 1.0f;
    state->reserved[0] = 0.0f;
    state->reserved[1] = 0.0f;
    state->reserved[2] = 0.0f;
}

__global__ void UpdateBumpHoldStateKernel(
    BumpHoldState* bumpState, const StabilizationState* stabilizationState,
    const float* currentFocusScore, const float* referenceFocusScore,
    int referenceReady, int fullWidth, int fullHeight,
    float motionEnterPixels, float motionExitPixels) {
    if (threadIdx.x != 0 || blockIdx.x != 0 || bumpState == nullptr ||
        stabilizationState == nullptr || currentFocusScore == nullptr ||
        referenceFocusScore == nullptr) {
        return;
    }

    constexpr float kBlurEnterRatio = 0.72f;
    constexpr float kBlurExitRatio = 0.82f;
    constexpr unsigned int kRecoveryStableFrames = 5u;
    constexpr unsigned int kCrossfadeFrames = 4u;
    constexpr float kPi = 3.14159265358979323846f;

    bumpState->captureCurrent = 0u;
    bumpState->blendAlpha = 1.0f;

    if (referenceReady == 0) {
        bumpState->diagnostics =
            make_float4(0.0f, 1.0f, 0.0f, 0.0f);
        bumpState->heldFrameValid = 0u;
        bumpState->stableFrames = 0u;
        bumpState->recoveryFrame = 0u;
        return;
    }

    const float referenceFocus = fmaxf(*referenceFocusScore, 1.0e-6f);
    const float sharpnessRatio =
        fmaxf(*currentFocusScore, 0.0f) / referenceFocus;
    const bool modelValid = stabilizationState->diagnostics.y >= 0.5f;
    const float4 motion = stabilizationState->lastFrameMotion;
    const float4 previous = bumpState->previousMotion;
    const float radius =
        0.5f * hypotf(static_cast<float>(max(fullWidth, 1)),
                      static_cast<float>(max(fullHeight, 1)));
    const float translationStep =
        hypotf(motion.x - previous.x, motion.y - previous.y);
    // Rotation and log-scale are expressed in radians/log units. Convert them
    // to a conservative source-pixel-equivalent using the translation gate.
    // Virtual Tripod currently emits translation only, but keeping these terms
    // makes the gate correct when its regularized similarity model lands.
    const float angularStep =
        fabsf(motion.z - previous.z) *
        fmaxf(motionEnterPixels / (2.0f * kPi), radius);
    const float scaleStep =
        fabsf(motion.w - previous.w) * fmaxf(radius, 1.0f);
    const float transformStep =
        translationStep + angularStep + scaleStep;
    if (modelValid) {
        bumpState->previousMotion = motion;
    }

    const bool badNow =
        !modelValid || sharpnessRatio < kBlurEnterRatio ||
        transformStep > fmaxf(motionEnterPixels, 0.1f);
    const bool stableNow =
        modelValid && sharpnessRatio >= kBlurExitRatio &&
        transformStep <= fmaxf(motionExitPixels, 0.05f);
    unsigned int mode =
        static_cast<unsigned int>(fminf(fmaxf(bumpState->diagnostics.x,
                                              0.0f),
                                        2.0f));

    if (bumpState->heldFrameValid == 0u) {
        // Only a sharp, settled, model-valid frame can become the safety
        // texture. Until one exists, remain live; ApplyBumpHold never reads
        // the uninitialized hold surface while heldFrameValid is zero.
        if (stableNow) {
            bumpState->heldFrameValid = 1u;
            bumpState->captureCurrent = 1u;
        }
        bumpState->stableFrames = 0u;
        bumpState->recoveryFrame = 0u;
        mode = 0u;
    } else if (mode == 0u) {
        if (badNow) {
            mode = 1u;
            bumpState->stableFrames = 0u;
            bumpState->recoveryFrame = 0u;
            bumpState->blendAlpha = 0.0f;
        } else {
            bumpState->captureCurrent = 1u;
        }
    } else if (mode == 1u) {
        bumpState->blendAlpha = 0.0f;
        bumpState->stableFrames =
            stableNow ? bumpState->stableFrames + 1u : 0u;
        if (bumpState->stableFrames >= kRecoveryStableFrames) {
            mode = 2u;
            bumpState->recoveryFrame = 0u;
        }
    } else {
        if (badNow) {
            mode = 1u;
            bumpState->stableFrames = 0u;
            bumpState->recoveryFrame = 0u;
            bumpState->blendAlpha = 0.0f;
        } else {
            const unsigned int nextRecoveryFrame =
                bumpState->recoveryFrame + 1u;
            bumpState->recoveryFrame =
                nextRecoveryFrame < kCrossfadeFrames
                    ? nextRecoveryFrame
                    : kCrossfadeFrames;
            bumpState->blendAlpha =
                static_cast<float>(bumpState->recoveryFrame) /
                static_cast<float>(kCrossfadeFrames);
            if (bumpState->recoveryFrame >= kCrossfadeFrames) {
                mode = 0u;
                bumpState->stableFrames = 0u;
                bumpState->captureCurrent = 1u;
                bumpState->blendAlpha = 1.0f;
            }
        }
    }

    bumpState->diagnostics =
        make_float4(static_cast<float>(mode), sharpnessRatio,
                    transformStep,
                    bumpState->heldFrameValid != 0u
                        ? 2.0f
                        : (modelValid ? 1.0f : 0.0f));
}

__global__ void ApplyBumpHoldKernel(
    uchar4* current, size_t currentPitch, uchar4* held, size_t heldPitch,
    int width, int height, const BumpHoldState* state) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height || current == nullptr || held == nullptr ||
        state == nullptr) {
        return;
    }

    auto* currentRow = reinterpret_cast<uchar4*>(
        reinterpret_cast<unsigned char*>(current) +
        static_cast<size_t>(y) * currentPitch);
    auto* heldRow = reinterpret_cast<uchar4*>(
        reinterpret_cast<unsigned char*>(held) +
        static_cast<size_t>(y) * heldPitch);
    const uchar4 live = currentRow[x];
    if (state->captureCurrent != 0u) {
        heldRow[x] = live;
        return;
    }

    const unsigned int mode =
        static_cast<unsigned int>(state->diagnostics.x);
    if (mode == 1u) {
        currentRow[x] = heldRow[x];
        return;
    }
    if (mode != 2u) {
        return;
    }

    const uchar4 frozen = heldRow[x];
    const float alpha = fminf(fmaxf(state->blendAlpha, 0.0f), 1.0f);
    const float inverse = 1.0f - alpha;
    currentRow[x] = make_uchar4(
        static_cast<unsigned char>(
            fminf(fmaxf(frozen.x * inverse + live.x * alpha, 0.0f), 255.0f)),
        static_cast<unsigned char>(
            fminf(fmaxf(frozen.y * inverse + live.y * alpha, 0.0f), 255.0f)),
        static_cast<unsigned char>(
            fminf(fmaxf(frozen.z * inverse + live.z * alpha, 0.0f), 255.0f)),
        static_cast<unsigned char>(
            fminf(fmaxf(frozen.w * inverse + live.w * alpha, 0.0f), 255.0f)));
}

__global__ void SelectSharperTripodReferenceStateKernel(
    const float* focusScore, float* bestFocusScore,
    unsigned int* selectionFlag) {
    if (threadIdx.x != 0 || blockIdx.x != 0) {
        return;
    }
    const bool selected = *focusScore >= *bestFocusScore;
    *selectionFlag = selected ? 1u : 0u;
    if (selected) {
        *bestFocusScore = *focusScore;
    }
}

__global__ void SelectSharperTripodReferenceCopyKernel(
    const float* candidate, float* reference, int pixelCount,
    const unsigned int* selectionFlag) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < pixelCount && *selectionFlag != 0u) {
        reference[index] = candidate[index];
    }
}

__global__ void InitializeTripodAccumulatorKernel(
    const float* reference, float* accumulator,
    unsigned int* sampleCounts, int pixelCount) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < pixelCount) {
        accumulator[index] = reference[index];
        sampleCounts[index] = 1u;
    }
}

__global__ void AccumulateTripodReferenceKernel(
    const float* current, const float* reference, int width, int height,
    float factorX, float factorY,
    const float* focusScore, const float* bestFocusScore,
    const StabilizationState* state,
    float* accumulator, unsigned int* sampleCounts) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height || state->diagnostics.y < 0.5f ||
        *focusScore < 0.45f * *bestFocusScore) {
        return;
    }
    const float sampleX = static_cast<float>(x) + state->lastFrameMotion.x / factorX;
    const float sampleY = static_cast<float>(y) + state->lastFrameMotion.y / factorY;
    if (sampleX < 0.0f || sampleY < 0.0f ||
        sampleX >= width - 1.0f || sampleY >= height - 1.0f) {
        return;
    }
    const float sample = SampleLuma(current, width, height, sampleX, sampleY);
    const int index = y * width + x;
    // Reject moving foreground and lighting outliers instead of smearing them
    // into the fixed template.
    if (fabsf(sample - reference[index]) > 22.0f) {
        return;
    }
    accumulator[index] += sample;
    sampleCounts[index] += 1u;
}

__global__ void FinalizeTripodReferenceKernel(
    float* reference, const float* accumulator,
    const unsigned int* sampleCounts, int pixelCount) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < pixelCount && sampleCounts[index] > 0u) {
        reference[index] =
            accumulator[index] / static_cast<float>(sampleCounts[index]);
    }
}

// One block owns one 16x16 cell, selects the strongest Harris corner from the
// previous frame, and tracks it with the three-level LK solve above.
__global__ void StabilizationFeaturePairsKernel(
    const float* currentLuma, const float* previousLuma,
    int width, int height, float factorX, float factorY,
    int fullWidth, int fullHeight,
    float4* pairs, unsigned int* pairCount, unsigned int maxPairs,
    const StabilizationState* gateState,
    const StabilizationState* motionSeedState,
    float maxDisplacementPixels,
    int forwardBackwardCheck) {
    if (gateState != nullptr && gateState->diagnostics.y >= 0.5f) {
        return;
    }
    __shared__ float scores[kStabFeatureBlockSize];
    __shared__ int indices[kStabFeatureBlockSize];
    const int tid = static_cast<int>(threadIdx.x);
    const int cellX = static_cast<int>(blockIdx.x) * kStabFeatureCellSize;
    const int cellY = static_cast<int>(blockIdx.y) * kStabFeatureCellSize;

    float bestScore = 0.0f;
    int bestIndex = -1;
    for (int local = tid;
         local < kStabFeatureCellSize * kStabFeatureCellSize;
         local += blockDim.x) {
        const int x = cellX + local % kStabFeatureCellSize;
        const int y = cellY + local / kStabFeatureCellSize;
        if (x < 11 || y < 11 || x >= width - 11 || y >= height - 11) {
            continue;
        }
        float gxx = 0.0f;
        float gxy = 0.0f;
        float gyy = 0.0f;
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int px = x + ox;
                const int py = y + oy;
                const float gx = 0.5f *
                    (previousLuma[py * width + px + 1] -
                     previousLuma[py * width + px - 1]);
                const float gy = 0.5f *
                    (previousLuma[(py + 1) * width + px] -
                     previousLuma[(py - 1) * width + px]);
                gxx += gx * gx;
                gxy += gx * gy;
                gyy += gy * gy;
            }
        }
        const float determinant = gxx * gyy - gxy * gxy;
        const float trace = gxx + gyy;
        const float score = determinant - 0.04f * trace * trace;
        if (score > bestScore) {
            bestScore = score;
            bestIndex = y * width + x;
        }
    }
    scores[tid] = bestScore;
    indices[tid] = bestIndex;
    __syncthreads();

    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride && scores[tid + stride] > scores[tid]) {
            scores[tid] = scores[tid + stride];
            indices[tid] = indices[tid + stride];
        }
        __syncthreads();
    }

    if (tid == 0 && indices[0] >= 0 && scores[0] > 2.0e5f) {
        const float x = static_cast<float>(indices[0] % width);
        const float y = static_cast<float>(indices[0] / width);
        float initialDx = 0.0f;
        float initialDy = 0.0f;
        if (motionSeedState != nullptr) {
            const float4 motion = motionSeedState->lastFrameMotion;
            const float scale = expf(motion.w);
            const float cosine = cosf(motion.z);
            const float sine = sinf(motion.z);
            const float sourceX = x * factorX;
            const float sourceY = y * factorY;
            const float centerX = 0.5f * static_cast<float>(fullWidth - 1);
            const float centerY = 0.5f * static_cast<float>(fullHeight - 1);
            const float localX = sourceX - centerX;
            const float localY = sourceY - centerY;
            const float predictedX =
                centerX + scale * (cosine * localX - sine * localY) +
                motion.x;
            const float predictedY =
                centerY + scale * (sine * localX + cosine * localY) +
                motion.y;
            initialDx = (predictedX - sourceX) / factorX;
            initialDy = (predictedY - sourceY) / factorY;
        }
        float dx = 0.0f;
        float dy = 0.0f;
        if (TrackFeaturePyramidal(previousLuma, currentLuma, width, height,
                                  x, y, maxDisplacementPixels,
                                  initialDx, initialDy, dx, dy)) {
            bool consistent = true;
            if (forwardBackwardCheck != 0) {
                float backwardDx = 0.0f;
                float backwardDy = 0.0f;
                consistent =
                    TrackFeaturePyramidal(
                        currentLuma, previousLuma, width, height,
                        x + dx, y + dy, maxDisplacementPixels,
                        -dx, -dy, backwardDx, backwardDy) &&
                    (dx + backwardDx) * (dx + backwardDx) +
                            (dy + backwardDy) * (dy + backwardDy) <=
                        0.75f * 0.75f;
            }
            if (!consistent) {
                return;
            }
            const unsigned int output = atomicAdd(pairCount, 1u);
            if (output < maxPairs) {
                pairs[output] = make_float4(
                    x * factorX, y * factorY,
                    (x + dx) * factorX, (y + dy) * factorY);
            }
        }
    }
}

__device__ float SimilarityResidualSquared(const float4& pair,
                                           float a, float b,
                                           float tx, float ty) {
    const float predictedX = a * pair.x - b * pair.y + tx;
    const float predictedY = b * pair.x + a * pair.y + ty;
    const float errorX = predictedX - pair.z;
    const float errorY = predictedY - pair.w;
    return errorX * errorX + errorY * errorY;
}

__global__ void ResetVirtualTripodStateKernel(StabilizationState* state,
                                              int preserveCorrection) {
    if (threadIdx.x != 0 || blockIdx.x != 0) {
        return;
    }
    float4 previousCorrection = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    float previousCaptures = 0.0f;
    if (preserveCorrection != 0) {
        previousCorrection = state->correction;
        previousCaptures = state->tripodDiagnostics.w;
    }
    ResetStabilizationState(state);
    if (preserveCorrection != 0) {
        state->correction = previousCorrection;
        state->previousCorrection = previousCorrection;
        state->filteredPath = previousCorrection;
        state->tripodAnchorCorrection = previousCorrection;
        state->tripodAbsoluteCorrection = previousCorrection;
    }
    state->tripodDiagnostics.w = previousCaptures + 1.0f;
}

__device__ bool VirtualTripodPairInsideRoi(
    const float4& pair, int fullWidth, int fullHeight,
    float focusCenterX, float focusCenterY, float zoomAmount) {
    const float halfSpan =
        fminf(0.5f, fmaxf(0.15f, 1.0f / fmaxf(zoomAmount, 1.0f)));
    const float normalizedX =
        pair.x / fmaxf(static_cast<float>(fullWidth - 1), 1.0f);
    const float normalizedY =
        pair.y / fmaxf(static_cast<float>(fullHeight - 1), 1.0f);
    return fabsf(normalizedX - focusCenterX) <= halfSpan &&
           fabsf(normalizedY - focusCenterY) <= halfSpan;
}

__device__ unsigned int SelectVirtualTripodPair(
    const float4* pairs, unsigned int count, unsigned int seed,
    bool useRoi, int fullWidth, int fullHeight,
    float focusCenterX, float focusCenterY, float zoomAmount) {
    if (!useRoi) {
        return seed % count;
    }
    for (unsigned int offset = 0; offset < count; ++offset) {
        const unsigned int index = (seed + offset) % count;
        if (VirtualTripodPairInsideRoi(
                pairs[index], fullWidth, fullHeight,
                focusCenterX, focusCenterY, zoomAmount)) {
            return index;
        }
    }
    return count;
}

// Absolute-reference counterpart to the pairwise estimator. RANSAC/refit stays
// identical, but the resulting transform is sent directly to the tripod path
// rather than accumulated into actualPath.
__global__ void VirtualTripodSimilarityEstimateKernel(
    const float4* pairs, const unsigned int* pairCount, unsigned int maxPairs,
    int fullWidth, int fullHeight, float thresholdPixels,
    float strength, float maxCorrectionFraction,
    float focusCenterX, float focusCenterY, float zoomAmount,
    StabilizationState* state,
    const float4* keyframeOrigins,
    const unsigned int* keyframeValid,
    unsigned int keyframeIndex,
    TripodMatchCandidate* candidates,
    int deferPathUpdate) {
    __shared__ unsigned int hypothesisInliers[kStabRansacHypotheses];
    __shared__ float hypothesisError[kStabRansacHypotheses];
    __shared__ float hypothesisA[kStabRansacHypotheses];
    __shared__ float hypothesisB[kStabRansacHypotheses];
    __shared__ float hypothesisTx[kStabRansacHypotheses];
    __shared__ float hypothesisTy[kStabRansacHypotheses];
    __shared__ unsigned int roiPairCount;
    __shared__ int useRoi;
    const int tid = static_cast<int>(threadIdx.x);
    const unsigned int count = min(*pairCount, maxPairs);
    if (tid == 0) {
        if (candidates != nullptr) {
            candidates[keyframeIndex].motion =
                make_float4(0.0f, 0.0f, 0.0f, 0.0f);
            candidates[keyframeIndex].diagnostics =
                make_float4(0.0f, 0.0f, 0.0f, 4.0f);
            candidates[keyframeIndex].statistics =
                make_float4(static_cast<float>(count), 0.0f, 0.0f,
                            static_cast<float>(keyframeIndex));
        }
        roiPairCount = 0;
        for (unsigned int index = 0; index < count; ++index) {
            if (VirtualTripodPairInsideRoi(
                    pairs[index], fullWidth, fullHeight,
                    focusCenterX, focusCenterY, zoomAmount)) {
                ++roiPairCount;
            }
        }
        useRoi =
            roiPairCount >= static_cast<unsigned int>(
                                kStabMinSimilarityInliers)
                ? 1
                : 0;
    }
    __syncthreads();

    if (tid < kStabRansacHypotheses) {
        unsigned int inliers = 0;
        float error = 0.0f;
        float a = 1.0f;
        float b = 0.0f;
        float tx = 0.0f;
        float ty = 0.0f;
        const bool preferRoi = useRoi != 0;
        bool valid = count >= kStabMinSimilarityInliers;
        if (valid) {
            // At high magnification, stabilize what the user is reading.
            // Fall back to the full frame only when the visible crop does not
            // contain enough tracked structure to fit a robust model.
            const unsigned int firstIndex = SelectVirtualTripodPair(
                pairs, count,
                static_cast<unsigned int>(tid) * 2654435761u + 17u,
                preferRoi, fullWidth, fullHeight,
                focusCenterX, focusCenterY, zoomAmount);
            valid = firstIndex < count;
            if (valid) {
                const float4 pair = pairs[firstIndex];
                // A clamped camera is a translation problem. Estimating
                // rotation/scale from feature noise makes static text swim,
                // so Virtual Tripod deliberately uses the minimum one-pair
                // translation hypothesis while ordinary stabilization keeps
                // its full similarity model.
                tx = pair.z - pair.x;
                ty = pair.w - pair.y;
            }
        }
        if (valid) {
            const float thresholdSquared = thresholdPixels * thresholdPixels;
            for (unsigned int index = 0; index < count; ++index) {
                if (preferRoi &&
                    !VirtualTripodPairInsideRoi(
                        pairs[index], fullWidth, fullHeight,
                        focusCenterX, focusCenterY, zoomAmount)) {
                    continue;
                }
                const float residual = SimilarityResidualSquared(
                    pairs[index], a, b, tx, ty);
                if (residual <= thresholdSquared) {
                    ++inliers;
                    error += residual;
                }
            }
        }
        hypothesisInliers[tid] = inliers;
        hypothesisError[tid] = error;
        hypothesisA[tid] = a;
        hypothesisB[tid] = b;
        hypothesisTx[tid] = tx;
        hypothesisTy[tid] = ty;
    }
    __syncthreads();
    if (tid != 0) {
        return;
    }

    int best = -1;
    for (int hypothesis = 0; hypothesis < kStabRansacHypotheses; ++hypothesis) {
        if (hypothesisInliers[hypothesis] < kStabMinSimilarityInliers) {
            continue;
        }
        if (best < 0 ||
            hypothesisInliers[hypothesis] > hypothesisInliers[best] ||
            (hypothesisInliers[hypothesis] == hypothesisInliers[best] &&
             hypothesisError[hypothesis] < hypothesisError[best])) {
            best = hypothesis;
        }
    }
    if (best < 0) {
        if (deferPathUpdate == 0 && state != nullptr) {
            state->diagnostics = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
            UpdateVirtualTripodPath(
                state, make_float4(0.0f, 0.0f, 0.0f, 0.0f), false,
                strength, zoomAmount, maxCorrectionFraction,
                fullWidth, fullHeight);
        }
        return;
    }

    const float thresholdSquared = thresholdPixels * thresholdPixels;
    float meanPx = 0.0f;
    float meanPy = 0.0f;
    float meanQx = 0.0f;
    float meanQy = 0.0f;
    float minPx = static_cast<float>(fullWidth);
    float minPy = static_cast<float>(fullHeight);
    float maxPx = 0.0f;
    float maxPy = 0.0f;
    unsigned int inliers = 0;
    for (unsigned int index = 0; index < count; ++index) {
        const float4 pair = pairs[index];
        if (useRoi != 0 &&
            !VirtualTripodPairInsideRoi(
                pair, fullWidth, fullHeight,
                focusCenterX, focusCenterY, zoomAmount)) {
            continue;
        }
        if (SimilarityResidualSquared(pair,
                                      hypothesisA[best], hypothesisB[best],
                                      hypothesisTx[best], hypothesisTy[best]) <=
            thresholdSquared) {
            meanPx += pair.x;
            meanPy += pair.y;
            meanQx += pair.z;
            meanQy += pair.w;
            minPx = fminf(minPx, pair.x);
            minPy = fminf(minPy, pair.y);
            maxPx = fmaxf(maxPx, pair.x);
            maxPy = fmaxf(maxPy, pair.y);
            ++inliers;
        }
    }
    if (inliers < kStabMinSimilarityInliers) {
        if (deferPathUpdate == 0 && state != nullptr) {
            state->diagnostics =
                make_float4(static_cast<float>(inliers), 0.0f, 0.0f, 0.0f);
            UpdateVirtualTripodPath(
                state, make_float4(0.0f, 0.0f, 0.0f, 0.0f), false,
                strength, zoomAmount, maxCorrectionFraction,
                fullWidth, fullHeight);
        }
        return;
    }

    const float inverseCount = 1.0f / static_cast<float>(inliers);
    meanPx *= inverseCount;
    meanPy *= inverseCount;
    meanQx *= inverseCount;
    meanQy *= inverseCount;
    float tx = meanQx - meanPx;
    float ty = meanQy - meanPy;
    if (keyframeOrigins != nullptr) {
        tx += keyframeOrigins[keyframeIndex].x;
        ty += keyframeOrigins[keyframeIndex].y;
    }
    const unsigned int consideredPairs =
        useRoi != 0 ? roiPairCount : count;
    const float inlierRatio =
        static_cast<float>(inliers) /
        fmaxf(static_cast<float>(consideredPairs), 1.0f);
    const float coverage =
        fmaxf(maxPx - minPx, 0.0f) *
        fmaxf(maxPy - minPy, 0.0f) /
        fmaxf(static_cast<float>(fullWidth) *
                  static_cast<float>(fullHeight),
              1.0f);
    const float meanResidual =
        hypothesisError[best] / static_cast<float>(inliers);
    bool modelValid = isfinite(tx) && isfinite(ty);
    if (candidates != nullptr) {
        const bool keyframeIsValid =
            keyframeValid == nullptr ||
            keyframeValid[keyframeIndex] != 0u;
        modelValid =
            modelValid && keyframeIsValid && inlierRatio >= 0.08f &&
            coverage >= 0.0015f &&
            meanResidual <= thresholdSquared;
        candidates[keyframeIndex].motion =
            make_float4(tx, ty, 0.0f, 0.0f);
        candidates[keyframeIndex].diagnostics = make_float4(
            static_cast<float>(inliers), modelValid ? 1.0f : 0.0f,
            meanResidual, 4.0f);
        candidates[keyframeIndex].statistics = make_float4(
            static_cast<float>(consideredPairs), inlierRatio, coverage,
            static_cast<float>(keyframeIndex));
    }
    if (deferPathUpdate != 0 || state == nullptr) {
        return;
    }
    state->diagnostics = make_float4(
        static_cast<float>(inliers), modelValid ? 1.0f : 0.0f,
        meanResidual,
        modelValid ? 4.0f : 0.0f);
    UpdateVirtualTripodPath(
        state,
        make_float4(tx, ty, 0.0f, 0.0f),
        modelValid, strength, zoomAmount, maxCorrectionFraction,
        fullWidth, fullHeight);
}

__global__ void SelectVirtualTripodMatchKernel(
    const TripodMatchCandidate* candidates, unsigned int candidateCount,
    float strength, float maxCorrectionFraction, float zoomAmount,
    int fullWidth, int fullHeight, StabilizationState* state) {
    if (threadIdx.x != 0 || blockIdx.x != 0) {
        return;
    }
    int best = -1;
    float bestScore = -1.0f;
    for (unsigned int index = 0; index < candidateCount; ++index) {
        const TripodMatchCandidate candidate = candidates[index];
        if (candidate.diagnostics.y < 0.5f) {
            continue;
        }
        const float score =
            candidate.statistics.y *
            sqrtf(fmaxf(candidate.statistics.z, 0.0f)) /
            (1.0f + fmaxf(candidate.diagnostics.z, 0.0f));
        if (score > bestScore) {
            best = static_cast<int>(index);
            bestScore = score;
        }
    }
    if (best < 0) {
        state->diagnostics =
            make_float4(0.0f, 0.0f, 0.0f, 4.0f);
        UpdateVirtualTripodPath(
            state, make_float4(0.0f, 0.0f, 0.0f, 0.0f), false,
            strength, zoomAmount, maxCorrectionFraction,
            fullWidth, fullHeight);
        return;
    }

    const TripodMatchCandidate selected = candidates[best];
    state->diagnostics = selected.diagnostics;
    UpdateVirtualTripodPath(
        state, selected.motion, true, strength, zoomAmount,
        maxCorrectionFraction, fullWidth, fullHeight);
    if (best > 0 && state->tripodDiagnostics.z < 2.5f) {
        state->tripodDiagnostics.z = 4.0f;
    }
}

__global__ void InitializeVirtualTripodKeyframeKernel(
    float4* keyframeOrigins, unsigned int* keyframeValid,
    unsigned int keyframeIndex) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        keyframeOrigins[keyframeIndex] =
            make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        keyframeValid[keyframeIndex] = 1u;
    }
}

__global__ void CaptureVirtualTripodKeyframeKernel(
    const float* current, float* destination, int pixelCount,
    const StabilizationState* state, float4* keyframeOrigins,
    unsigned int* keyframeValid, unsigned int keyframeIndex,
    unsigned int minimumValidFrames) {
    const bool admit =
        state->diagnostics.y >= 0.5f &&
        fabsf(state->diagnostics.w - 4.0f) < 0.5f &&
        state->tripodDiagnostics.z != 2.0f &&
        state->tripodDiagnostics.x >=
            static_cast<float>(minimumValidFrames);
    if (!admit) {
        return;
    }
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < pixelCount) {
        destination[index] = current[index];
    }
    if (index == 0) {
        keyframeOrigins[keyframeIndex] = state->lastFrameMotion;
        keyframeValid[keyframeIndex] = 1u;
    }
}

__global__ void VirtualTripodRelativeFallbackKernel(
    const float4* pairs, const unsigned int* pairCount,
    unsigned int maxPairs, int fullWidth, int fullHeight,
    float thresholdPixels, float maxCorrectionFraction,
    StabilizationState* state) {
    if (threadIdx.x != 0 || blockIdx.x != 0 ||
        state->diagnostics.y >= 0.5f) {
        return;
    }
    const unsigned int count = min(*pairCount, maxPairs);
    if (count < static_cast<unsigned int>(kStabMinSimilarityInliers)) {
        state->correction = state->tripodAbsoluteCorrection;
        return;
    }
    const float thresholdSquared = thresholdPixels * thresholdPixels;
    unsigned int bestInliers = 0u;
    float bestError = CUDART_INF_F;
    float bestDx = 0.0f;
    float bestDy = 0.0f;
    constexpr unsigned int kFallbackHypotheses = 64u;
    for (unsigned int hypothesis = 0; hypothesis < kFallbackHypotheses;
         ++hypothesis) {
        const float4 seed =
            pairs[(hypothesis * 2654435761u + 17u) % count];
        const float dx = seed.z - seed.x;
        const float dy = seed.w - seed.y;
        unsigned int inliers = 0u;
        float error = 0.0f;
        for (unsigned int index = 0; index < count; ++index) {
            const float residualX =
                (pairs[index].z - pairs[index].x) - dx;
            const float residualY =
                (pairs[index].w - pairs[index].y) - dy;
            const float residual =
                residualX * residualX + residualY * residualY;
            if (residual <= thresholdSquared) {
                ++inliers;
                error += residual;
            }
        }
        if (inliers > bestInliers ||
            (inliers == bestInliers && error < bestError)) {
            bestInliers = inliers;
            bestError = error;
            bestDx = dx;
            bestDy = dy;
        }
    }
    if (bestInliers < static_cast<unsigned int>(kStabMinSimilarityInliers) ||
        static_cast<float>(bestInliers) /
                static_cast<float>(count) <
            0.12f) {
        state->correction = state->tripodAbsoluteCorrection;
        return;
    }

    float sumDx = 0.0f;
    float sumDy = 0.0f;
    unsigned int inliers = 0u;
    for (unsigned int index = 0; index < count; ++index) {
        const float dx = pairs[index].z - pairs[index].x;
        const float dy = pairs[index].w - pairs[index].y;
        const float residualX = dx - bestDx;
        const float residualY = dy - bestDy;
        if (residualX * residualX + residualY * residualY <=
            thresholdSquared) {
            sumDx += dx;
            sumDy += dy;
            ++inliers;
        }
    }
    const float inverseCount =
        1.0f / fmaxf(static_cast<float>(inliers), 1.0f);
    const float maxRelativeX = 0.035f * static_cast<float>(fullWidth);
    const float maxRelativeY = 0.035f * static_cast<float>(fullHeight);
    state->tripodRelativeMotion.x = fminf(
        fmaxf(state->tripodRelativeMotion.x + sumDx * inverseCount,
              -maxRelativeX),
        maxRelativeX);
    state->tripodRelativeMotion.y = fminf(
        fmaxf(state->tripodRelativeMotion.y + sumDy * inverseCount,
              -maxRelativeY),
        maxRelativeY);
    bool translationLimited = false;
    state->correction = ClampStabilizationCorrection(
        make_float4(
            state->tripodAbsoluteCorrection.x -
                state->tripodRelativeMotion.x,
            state->tripodAbsoluteCorrection.y -
                state->tripodRelativeMotion.y,
            state->tripodAbsoluteCorrection.z,
            state->tripodAbsoluteCorrection.w),
        maxCorrectionFraction, fullWidth, fullHeight,
        &translationLimited);
    state->diagnostics = make_float4(
        static_cast<float>(inliers), 1.0f,
        bestError / fmaxf(static_cast<float>(bestInliers), 1.0f), 5.0f);
    state->tripodDiagnostics.z = 2.0f;
}

// Apply the inverse of the fixed-reference similarity correction about the
// frame center.
__global__ void StabilizationWarpKernel(uchar4* dst, size_t dstPitch,
                                        const uchar4* src, size_t srcPitch,
                                        int width, int height,
                                        const StabilizationState* state) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const float4 correction = state->correction;
    const float centerX = 0.5f * static_cast<float>(width - 1);
    const float centerY = 0.5f * static_cast<float>(height - 1);
    const float translatedX = static_cast<float>(x) - centerX - correction.x;
    const float translatedY = static_cast<float>(y) - centerY - correction.y;
    const float inverseScale = expf(-correction.w);
    const float cosine = cosf(correction.z);
    const float sine = sinf(correction.z);
    const float sampleX = centerX +
        inverseScale * (cosine * translatedX + sine * translatedY);
    const float sampleY = centerY +
        inverseScale * (-sine * translatedX + cosine * translatedY);
    const float3 color = BilinearSample(src, srcPitch, sampleX, sampleY, width, height);

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x] = make_uchar4(static_cast<unsigned char>(fminf(color.x + 0.5f, 255.0f)),
                            static_cast<unsigned char>(fminf(color.y + 0.5f, 255.0f)),
                            static_cast<unsigned char>(fminf(color.z + 0.5f, 255.0f)),
                            255u);
}

__device__ float ApplyContrastBrightness(float normalized, float contrast, float brightness) {
    return fminf(fmaxf((normalized - 0.5f) * contrast + 0.5f + brightness, 0.0f), 1.0f);
}

// Auto-contrast remap: stretch [lo, hi] to [0, 1], blended with the original
// value by `strength` (0 = untouched, 1 = full stretch). The range floor
// (16/255) prevents blowups on nearly flat frames.
__device__ float ApplyAutoContrast(float value, float lo, float hi, float strength) {
    const float range = fmaxf(hi - lo, 16.0f / 255.0f);
    const float stretched = fminf(fmaxf((value - lo) / range, 0.0f), 1.0f);
    return value + strength * (stretched - value);
}

__device__ uchar4 UnpackBgra(std::uint32_t packed) {
    return make_uchar4(static_cast<unsigned char>(packed & 0xffu),
                       static_cast<unsigned char>((packed >> 8) & 0xffu),
                       static_cast<unsigned char>((packed >> 16) & 0xffu),
                       static_cast<unsigned char>((packed >> 24) & 0xffu));
}

// Contrast/brightness plus display color mode. Operates in place: each thread
// reads and writes only its own pixel, so no ping-pong buffer swap is needed.
// When enabled, the auto-contrast level stretch runs first (levels are read
// straight from device memory — no host readback), then contrast/brightness.
// Transform 0 preserves full color, transform 1 inverts each channel, and
// transform 2 maps luma through the app-provided constant-memory LUT.
__global__ void DisplayColorGradeKernel(uchar4* buffer, size_t pitchBytes,
                                        int width, int height,
                                        int colorTransform, float contrast, float brightness,
                                        const float2* autoContrastLevels,
                                        float autoContrastStrength) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    uchar4* row = RowAt(buffer, pitchBytes, y);
    const uchar4 pixel = row[x];

    float b = static_cast<float>(pixel.x) / 255.0f;
    float g = static_cast<float>(pixel.y) / 255.0f;
    float r = static_cast<float>(pixel.z) / 255.0f;

    if (autoContrastLevels != nullptr) {
        // One coalesced broadcast read per thread; the L2 cache absorbs it.
        const float2 levels = *autoContrastLevels;
        const float strength = fminf(fmaxf(autoContrastStrength, 0.0f), 1.0f);
        b = ApplyAutoContrast(b, levels.x, levels.y, strength);
        g = ApplyAutoContrast(g, levels.x, levels.y, strength);
        r = ApplyAutoContrast(r, levels.x, levels.y, strength);
    }

    b = ApplyContrastBrightness(b, contrast, brightness);
    g = ApplyContrastBrightness(g, contrast, brightness);
    r = ApplyContrastBrightness(r, contrast, brightness);

    switch (colorTransform) {
    case 1: {
        b = 1.0f - b;
        g = 1.0f - g;
        r = 1.0f - r;
        break;
    }
    case 2: {
        const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
        const uchar4 mapped = UnpackBgra(gDisplayColorLut[FloatToByte(luma)]);
        b = static_cast<float>(mapped.x) / 255.0f;
        g = static_cast<float>(mapped.y) / 255.0f;
        r = static_cast<float>(mapped.z) / 255.0f;
        break;
    }
    default: break;
    }

    row[x] = make_uchar4(FloatToByte(b), FloatToByte(g), FloatToByte(r), pixel.w);
}

__device__ inline uchar4 YuvToBgra(int yValue, int u, int v,
                                 const YuvCoefficients& coefficients) {
    const auto pixel = ConvertYuvPixel(yValue, u, v, coefficients);
    return make_uchar4(pixel.b, pixel.g, pixel.r, 255u);
}

__global__ void Nv12ToBgraKernel(uchar4* dst, size_t dstPitch,
                                 const unsigned char* yPlane, size_t yPitch,
                                 const unsigned char* uvPlane, size_t uvPitch,
                                 int width, int height, YuvCoefficients coefficients) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const unsigned char* yRow = yPlane + static_cast<size_t>(y) * yPitch;
    const unsigned char* uvRow = uvPlane + static_cast<size_t>(y >> 1) * uvPitch;
    const int uvIndex = x & ~1;
    const int u = uvRow[uvIndex];
    const int v = uvRow[uvIndex + 1];

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x] = YuvToBgra(static_cast<int>(yRow[x]), u, v, coefficients);
}

// One thread per horizontal pixel pair (YUY2 stores Y0 U Y1 V per 2 pixels).
__global__ void Yuy2ToBgraKernel(uchar4* dst, size_t dstPitch,
                                 const unsigned char* src, size_t srcPitch,
                                 int width, int height, YuvCoefficients coefficients) {
    const int pairX = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int x0 = pairX * 2;
    if (x0 >= width || y >= height) {
        return;
    }

    const unsigned char* srcPtr = src + static_cast<size_t>(y) * srcPitch +
                                  static_cast<size_t>(pairX) * 4u;
    const int y0 = srcPtr[0];
    const int u = srcPtr[1];
    const int y1 = srcPtr[2];
    const int v = srcPtr[3];

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    dstRow[x0] = YuvToBgra(y0, u, v, coefficients);
    if (x0 + 1 < width) {
        dstRow[x0 + 1] = YuvToBgra(y1, u, v, coefficients);
    }
}

// Clockwise quarter-turn rotation, gathered from the destination side so writes
// stay coalesced. dstWidth/dstHeight are the post-rotation extents.
__global__ void RotateQuarterKernel(uchar4* dst, size_t dstPitch,
                                    const uchar4* src, size_t srcPitch,
                                    int srcWidth, int srcHeight,
                                    int dstWidth, int dstHeight,
                                    int turns) {
    const int ox = blockIdx.x * blockDim.x + threadIdx.x;
    const int oy = blockIdx.y * blockDim.y + threadIdx.y;
    if (ox >= dstWidth || oy >= dstHeight) {
        return;
    }

    int ix = ox;
    int iy = oy;
    switch (turns) {
    case 1:  // 90 CW: dst(ox,oy) = src(oy, srcHeight-1-ox)
        ix = oy;
        iy = srcHeight - 1 - ox;
        break;
    case 2:  // 180
        ix = srcWidth - 1 - ox;
        iy = srcHeight - 1 - oy;
        break;
    case 3:  // 270 CW: dst(ox,oy) = src(srcWidth-1-oy, ox)
        ix = srcWidth - 1 - oy;
        iy = ox;
        break;
    default:
        break;
    }

    uchar4* dstRow = RowAt(dst, dstPitch, oy);
    dstRow[ox] = RowAt(src, srcPitch, iy)[ix];
}

// Perspective warp: each output pixel is mapped through the 3x3 homography
// (output px -> source px, row major, passed by value as kernel arguments so no
// constant-memory synchronization is needed) and sampled bilinearly. Pixels
// mapping outside the source rect are painted black.
struct KeystoneHomography {
    float m[9];
};

__global__ void KeystoneWarpKernel(uchar4* dst, size_t dstPitch,
                                   const uchar4* src, size_t srcPitch,
                                   int width, int height,
                                   KeystoneHomography h) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float w = h.m[6] * fx + h.m[7] * fy + h.m[8];

    uchar4* dstRow = RowAt(dst, dstPitch, y);
    if (fabsf(w) < 1e-8f) {
        dstRow[x] = make_uchar4(0, 0, 0, 255u);
        return;
    }

    const float invW = 1.0f / w;
    const float sx = (h.m[0] * fx + h.m[1] * fy + h.m[2]) * invW;
    const float sy = (h.m[3] * fx + h.m[4] * fy + h.m[5]) * invW;

    if (sx < 0.0f || sy < 0.0f ||
        sx > static_cast<float>(width - 1) || sy > static_cast<float>(height - 1)) {
        dstRow[x] = make_uchar4(0, 0, 0, 255u);
        return;
    }

    const float3 color = BilinearSample(src, srcPitch, sx, sy, width, height);
    dstRow[x] = make_uchar4(static_cast<unsigned char>(fminf(color.x + 0.5f, 255.0f)),
                            static_cast<unsigned char>(fminf(color.y + 0.5f, 255.0f)),
                            static_cast<unsigned char>(fminf(color.z + 0.5f, 255.0f)),
                            255u);
}

// 256-bin luma histogram. Full-frame rather than the small stabilization luma:
// the small image is produced by box-averaging, which squeezes the extremes and
// would bias the percentile levels inward; a full-frame pass is a single read
// per pixel (about the cost of the downsample itself) and measures the true
// distribution. Shared-memory block histograms keep global atomics to
// 256 per block. Requires blockDim.x * blockDim.y == 256.
__global__ void AutoContrastHistogramKernel(unsigned int* histogram,
                                            const uchar4* src, size_t srcPitch,
                                            int width, int height) {
    __shared__ unsigned int blockHist[256];
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    blockHist[tid] = 0;
    __syncthreads();

    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < width && y < height) {
        const uchar4 pixel = RowAt(src, srcPitch, y)[x];
        const float luma = 0.299f * static_cast<float>(pixel.z) +
                           0.587f * static_cast<float>(pixel.y) +
                           0.114f * static_cast<float>(pixel.x);
        const int bin = min(255, max(0, __float2int_rn(luma)));
        atomicAdd(&blockHist[bin], 1u);
    }
    __syncthreads();

    if (blockHist[tid] != 0) {
        atomicAdd(&histogram[tid], blockHist[tid]);
    }
}

// Single-thread scan over 256 bins (nanoseconds) that finds the 2nd/98th
// percentile levels and low-passes them in device memory; the grade kernel
// reads the result directly, so auto contrast never touches the host.
__global__ void AutoContrastAnalysisKernel(const unsigned int* histogram,
                                           int pixelCount,
                                           float2* levels,
                                           int levelsValid) {
    if (threadIdx.x != 0 || blockIdx.x != 0) {
        return;
    }

    const unsigned int total = static_cast<unsigned int>(max(pixelCount, 1));
    const unsigned int lowTarget = static_cast<unsigned int>(
        (static_cast<unsigned long long>(total) * 2ull) / 100ull);
    const unsigned int highTarget = static_cast<unsigned int>(
        (static_cast<unsigned long long>(total) * 98ull) / 100ull);

    int lo = 0;
    int hi = 255;
    unsigned int cumulative = 0;
    bool loFound = false;
    for (int bin = 0; bin < 256; ++bin) {
        cumulative += histogram[bin];
        if (!loFound && cumulative > lowTarget) {
            lo = bin;
            loFound = true;
        }
        if (cumulative >= highTarget) {
            hi = bin;
            break;
        }
    }
    if (hi <= lo) {
        hi = min(lo + 1, 255);
    }

    const float2 measured = make_float2(static_cast<float>(lo) / 255.0f,
                                        static_cast<float>(hi) / 255.0f);
    if (levelsValid != 0) {
        // Temporal smoothing (lerp 0.1) so slide transitions do not flash.
        const float2 previous = *levels;
        *levels = make_float2(previous.x + 0.1f * (measured.x - previous.x),
                              previous.y + 0.1f * (measured.y - previous.y));
    } else {
        *levels = measured;
    }
}

} // namespace

void LaunchStabilizationLumaDownsample(float* dstLuma,
                                       int smallWidth, int smallHeight,
                                       const uchar4* src, size_t srcPitchBytes,
                                       int width, int height,
                                       int factorX, int factorY,
                                       cudaStream_t stream) {
    if (smallWidth <= 0 || smallHeight <= 0 || width <= 0 || height <= 0 ||
        factorX <= 0 || factorY <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((smallWidth + blockSize.x - 1) / blockSize.x,
                        (smallHeight + blockSize.y - 1) / blockSize.y);
    StabilizationLumaDownsampleKernel<<<gridSize, blockSize, 0, stream>>>(dstLuma,
                                                                          smallWidth, smallHeight,
                                                                          src, srcPitchBytes,
                                                                          width, height,
                                                                          factorX, factorY);
    CheckCuda("StabilizationLumaDownsampleKernel launch failed");
}

void LaunchStabilizationProjections(const float* luma,
                                    int smallWidth, int smallHeight,
                                    float* colProj, float* rowProj,
                                    cudaStream_t stream) {
    if (smallWidth <= 0 || smallHeight <= 0) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(colProj, 0, sizeof(float) * static_cast<size_t>(smallWidth), stream),
                    "cudaMemsetAsync column projection failed");
    CheckCudaStatus(cudaMemsetAsync(rowProj, 0, sizeof(float) * static_cast<size_t>(smallHeight), stream),
                    "cudaMemsetAsync row projection failed");
    const dim3 blockSize(16, 16);
    const dim3 gridSize((smallWidth + blockSize.x - 1) / blockSize.x,
                        (smallHeight + blockSize.y - 1) / blockSize.y);
    StabilizationProjectionKernel<<<gridSize, blockSize, 0, stream>>>(luma,
                                                                      smallWidth, smallHeight,
                                                                      colProj, rowProj);
    CheckCuda("StabilizationProjectionKernel launch failed");
}

void LaunchVirtualTripodProjectionSeed(
    const float* currentColProj, const float* currentRowProj,
    const float* referenceColProj, const float* referenceRowProj,
    int smallWidth, int smallHeight, float factorX, float factorY,
    const float4* keyframeOrigins, const unsigned int* keyframeValid,
    unsigned int keyframeIndex, StabilizationState* state,
    cudaStream_t stream) {
    if (!currentColProj || !currentRowProj || !referenceColProj ||
        !referenceRowProj || !keyframeOrigins || !keyframeValid || !state ||
        smallWidth <= 0 || smallHeight <= 0) {
        return;
    }
    VirtualTripodProjectionSeedKernel<<<1, 256, 0, stream>>>(
        currentColProj, currentRowProj, referenceColProj, referenceRowProj,
        smallWidth, smallHeight, factorX, factorY, keyframeOrigins,
        keyframeValid, keyframeIndex, state);
    CheckCuda("VirtualTripodProjectionSeedKernel launch failed");
}

void LaunchStabilizationFeaturePairs(const float* currentLuma,
                                     const float* previousLuma,
                                     int smallWidth, int smallHeight,
                                     float factorX, float factorY,
                                     float4* pairs, unsigned int* pairCount,
                                     unsigned int maxPairs,
                                     const StabilizationState* gateState,
                                     cudaStream_t stream) {
    if (!currentLuma || !previousLuma || !pairs || !pairCount ||
        smallWidth <= 0 || smallHeight <= 0 || maxPairs == 0) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(pairCount, 0, sizeof(unsigned int), stream),
                    "cudaMemsetAsync stabilization feature count failed");
    const dim3 gridSize(
        (smallWidth + kStabFeatureCellSize - 1) / kStabFeatureCellSize,
        (smallHeight + kStabFeatureCellSize - 1) / kStabFeatureCellSize);
    StabilizationFeaturePairsKernel<<<gridSize, kStabFeatureBlockSize, 0, stream>>>(
        currentLuma, previousLuma, smallWidth, smallHeight,
        factorX, factorY,
        static_cast<int>(smallWidth * factorX),
        static_cast<int>(smallHeight * factorY),
        pairs, pairCount, maxPairs, gateState, nullptr, 24.0f, 0);
    CheckCuda("StabilizationFeaturePairsKernel launch failed");
}

void LaunchVirtualTripodFeaturePairs(const float* currentLuma,
                                     const float* referenceLuma,
                                     int smallWidth, int smallHeight,
                                     float factorX, float factorY,
                                     int fullWidth, int fullHeight,
                                     float4* pairs, unsigned int* pairCount,
                                     unsigned int maxPairs,
                                     const StabilizationState* motionSeedState,
                                     cudaStream_t stream) {
    if (!currentLuma || !referenceLuma || !pairs || !pairCount ||
        smallWidth <= 0 || smallHeight <= 0 || maxPairs == 0) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(pairCount, 0, sizeof(unsigned int), stream),
                    "cudaMemsetAsync virtual tripod feature count failed");
    const dim3 gridSize(
        (smallWidth + kStabFeatureCellSize - 1) / kStabFeatureCellSize,
        (smallHeight + kStabFeatureCellSize - 1) / kStabFeatureCellSize);
    const float maximumDisplacement =
        0.48f * static_cast<float>(max(smallWidth, smallHeight));
    StabilizationFeaturePairsKernel<<<gridSize, kStabFeatureBlockSize, 0, stream>>>(
        currentLuma, referenceLuma, smallWidth, smallHeight,
        factorX, factorY, fullWidth, fullHeight,
        pairs, pairCount, maxPairs, nullptr, motionSeedState,
        maximumDisplacement, 1);
    CheckCuda("VirtualTripodFeaturePairsKernel launch failed");
}

void LaunchStabilizationLumaPyramid(const float* level0,
                                    int level0Width, int level0Height,
                                    float* level1,
                                    int level1Width, int level1Height,
                                    float* level2,
                                    int level2Width, int level2Height,
                                    cudaStream_t stream) {
    if (!level0 || !level1 || !level2 || level0Width <= 0 || level0Height <= 0 ||
        level1Width <= 0 || level1Height <= 0 ||
        level2Width <= 0 || level2Height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 level1Grid(
        (level1Width + blockSize.x - 1) / blockSize.x,
        (level1Height + blockSize.y - 1) / blockSize.y);
    GaussianPyramidDownsampleKernel<<<level1Grid, blockSize, 0, stream>>>(
        level0, level0Width, level0Height,
        level1, level1Width, level1Height);
    CheckCuda("GaussianPyramidDownsampleKernel level 1 launch failed");
    const dim3 level2Grid(
        (level2Width + blockSize.x - 1) / blockSize.x,
        (level2Height + blockSize.y - 1) / blockSize.y);
    GaussianPyramidDownsampleKernel<<<level2Grid, blockSize, 0, stream>>>(
        level1, level1Width, level1Height,
        level2, level2Width, level2Height);
    CheckCuda("GaussianPyramidDownsampleKernel level 2 launch failed");
}

void LaunchPrepareVirtualTripodReference(
    const float* referenceLevel0, int level0Width, int level0Height,
    const float* referenceLevel1, int level1Width, int level1Height,
    const float* referenceLevel2, int level2Width, int level2Height,
    TripodReferenceFeature* features, unsigned int* featureCount,
    unsigned int maxFeatures, cudaStream_t stream) {
    if (!referenceLevel0 || !referenceLevel1 || !referenceLevel2 ||
        !features || !featureCount || maxFeatures == 0u ||
        level0Width <= 0 || level0Height <= 0 ||
        level1Width <= 0 || level1Height <= 0 ||
        level2Width <= 0 || level2Height <= 0) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(featureCount, 0, sizeof(unsigned int), stream),
                    "cudaMemsetAsync virtual tripod prepared feature count failed");
    const dim3 gridSize(
        (level0Width + kStabFeatureCellSize - 1) / kStabFeatureCellSize,
        (level0Height + kStabFeatureCellSize - 1) / kStabFeatureCellSize);
    PrepareVirtualTripodReferenceKernel<<<
        gridSize, kStabFeatureBlockSize, 0, stream>>>(
        referenceLevel0, level0Width, level0Height,
        referenceLevel1, level1Width, level1Height,
        referenceLevel2, level2Width, level2Height,
        features, featureCount, maxFeatures);
    CheckCuda("PrepareVirtualTripodReferenceKernel launch failed");
}

void LaunchPreparedVirtualTripodFeaturePairs(
    const float* currentLevel0, int level0Width, int level0Height,
    const float* currentLevel1, int level1Width, int level1Height,
    const float* currentLevel2, int level2Width, int level2Height,
    const float* referenceLevel0, const float* referenceLevel1,
    const float* referenceLevel2,
    const TripodReferenceFeature* features,
    const unsigned int* featureCount,
    float factorX, float factorY,
    int fullWidth, int fullHeight,
    float4* pairs, unsigned int* pairCount, unsigned int maxPairs,
    const StabilizationState* motionSeedState, cudaStream_t stream) {
    if (!currentLevel0 || !currentLevel1 || !currentLevel2 ||
        !referenceLevel0 || !referenceLevel1 || !referenceLevel2 ||
        !features || !featureCount || !pairs || !pairCount ||
        maxPairs == 0u || level0Width <= 0 || level0Height <= 0 ||
        fullWidth <= 0 || fullHeight <= 0) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(pairCount, 0, sizeof(unsigned int), stream),
                    "cudaMemsetAsync prepared tripod pair count failed");
    constexpr unsigned int blockSize = 128u;
    const unsigned int blocks = (maxPairs + blockSize - 1u) / blockSize;
    PreparedVirtualTripodFeaturePairsKernel<<<blocks, blockSize, 0, stream>>>(
        currentLevel0, level0Width, level0Height,
        currentLevel1, level1Width, level1Height,
        currentLevel2, level2Width, level2Height,
        referenceLevel0, referenceLevel1, referenceLevel2,
        features, featureCount, factorX, factorY,
        pairs, pairCount, maxPairs, motionSeedState,
        nullptr, nullptr, 0u, nullptr);
    CheckCuda("PreparedVirtualTripodFeaturePairsKernel launch failed");
}

void LaunchPreparedVirtualTripodKeyframePairs(
    const float* currentLevel0, int level0Width, int level0Height,
    const float* currentLevel1, int level1Width, int level1Height,
    const float* currentLevel2, int level2Width, int level2Height,
    const float* referenceLevel0, const float* referenceLevel1,
    const float* referenceLevel2,
    const TripodReferenceFeature* features,
    const unsigned int* featureCount,
    float factorX, float factorY,
    int fullWidth, int fullHeight,
    float4* pairs, unsigned int* pairCount, unsigned int maxPairs,
    const StabilizationState* motionSeedState,
    const float4* keyframeOrigins,
    const unsigned int* keyframeValid,
    unsigned int keyframeIndex,
    const TripodMatchCandidate* earlierCandidates,
    cudaStream_t stream) {
    if (!currentLevel0 || !currentLevel1 || !currentLevel2 ||
        !referenceLevel0 || !referenceLevel1 || !referenceLevel2 ||
        !features || !featureCount || !pairs || !pairCount ||
        !keyframeOrigins || !keyframeValid || !earlierCandidates ||
        maxPairs == 0u || level0Width <= 0 || level0Height <= 0 ||
        fullWidth <= 0 || fullHeight <= 0) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(pairCount, 0, sizeof(unsigned int), stream),
                    "cudaMemsetAsync tripod keyframe pair count failed");
    constexpr unsigned int blockSize = 128u;
    const unsigned int blocks = (maxPairs + blockSize - 1u) / blockSize;
    PreparedVirtualTripodFeaturePairsKernel<<<blocks, blockSize, 0, stream>>>(
        currentLevel0, level0Width, level0Height,
        currentLevel1, level1Width, level1Height,
        currentLevel2, level2Width, level2Height,
        referenceLevel0, referenceLevel1, referenceLevel2,
        features, featureCount, factorX, factorY,
        pairs, pairCount, maxPairs, motionSeedState,
        keyframeOrigins, keyframeValid, keyframeIndex, earlierCandidates);
    CheckCuda("PreparedVirtualTripodFeaturePairsKernel keyframe launch failed");
}

void LaunchMeasureVirtualTripodFocus(const float* luma, int width, int height,
                                     float* focusScore, cudaStream_t stream) {
    if (!luma || !focusScore || width <= 2 || height <= 2) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(focusScore, 0, sizeof(float), stream),
                    "cudaMemsetAsync virtual tripod focus score failed");
    const dim3 blockSize(16, 16);
    const dim3 gridSize(
        (width + blockSize.x - 1) / blockSize.x,
        (height + blockSize.y - 1) / blockSize.y);
    VirtualTripodFocusKernel<<<gridSize, blockSize, 0, stream>>>(
        luma, width, height, focusScore);
    CheckCuda("VirtualTripodFocusKernel launch failed");
}

void LaunchResetBumpHoldState(BumpHoldState* state, cudaStream_t stream) {
    if (!state) {
        return;
    }
    ResetBumpHoldStateKernel<<<1, 1, 0, stream>>>(state);
    CheckCuda("ResetBumpHoldStateKernel launch failed");
}

void LaunchUpdateBumpHoldState(
    BumpHoldState* bumpState, const StabilizationState* stabilizationState,
    const float* currentFocusScore, const float* referenceFocusScore,
    bool referenceReady, int fullWidth, int fullHeight,
    float motionEnterPixels, float motionExitPixels, cudaStream_t stream) {
    if (!bumpState || !stabilizationState || !currentFocusScore ||
        !referenceFocusScore) {
        return;
    }
    UpdateBumpHoldStateKernel<<<1, 1, 0, stream>>>(
        bumpState, stabilizationState, currentFocusScore, referenceFocusScore,
        referenceReady ? 1 : 0, fullWidth, fullHeight,
        motionEnterPixels, motionExitPixels);
    CheckCuda("UpdateBumpHoldStateKernel launch failed");
}

void LaunchApplyBumpHold(
    uchar4* current, size_t currentPitchBytes,
    uchar4* held, size_t heldPitchBytes,
    int width, int height, const BumpHoldState* state,
    cudaStream_t stream) {
    if (!current || !held || !state || width <= 0 || height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    ApplyBumpHoldKernel<<<gridSize, blockSize, 0, stream>>>(
        current, currentPitchBytes, held, heldPitchBytes,
        width, height, state);
    CheckCuda("ApplyBumpHoldKernel launch failed");
}

void LaunchSelectSharperVirtualTripodReference(
    const float* candidate, float* reference, int pixelCount,
    const float* focusScore, float* bestFocusScore,
    unsigned int* selectionFlag, cudaStream_t stream) {
    if (!candidate || !reference || !focusScore || !bestFocusScore ||
        !selectionFlag || pixelCount <= 0) {
        return;
    }
    SelectSharperTripodReferenceStateKernel<<<1, 1, 0, stream>>>(
        focusScore, bestFocusScore, selectionFlag);
    CheckCuda("SelectSharperTripodReferenceStateKernel launch failed");
    constexpr int blockSize = 256;
    const int blocks = (pixelCount + blockSize - 1) / blockSize;
    SelectSharperTripodReferenceCopyKernel<<<blocks, blockSize, 0, stream>>>(
        candidate, reference, pixelCount, selectionFlag);
    CheckCuda("SelectSharperTripodReferenceCopyKernel launch failed");
}

void LaunchInitializeVirtualTripodAccumulator(
    const float* reference, float* accumulator, unsigned int* sampleCounts,
    int pixelCount, cudaStream_t stream) {
    if (!reference || !accumulator || !sampleCounts || pixelCount <= 0) {
        return;
    }
    constexpr int blockSize = 256;
    const int blocks = (pixelCount + blockSize - 1) / blockSize;
    InitializeTripodAccumulatorKernel<<<blocks, blockSize, 0, stream>>>(
        reference, accumulator, sampleCounts, pixelCount);
    CheckCuda("InitializeTripodAccumulatorKernel launch failed");
}

void LaunchAccumulateVirtualTripodReference(
    const float* current, const float* reference, int width, int height,
    float factorX, float factorY,
    const float* focusScore, const float* bestFocusScore,
    const StabilizationState* state,
    float* accumulator, unsigned int* sampleCounts, cudaStream_t stream) {
    if (!current || !reference || !focusScore || !bestFocusScore || !state ||
        !accumulator || !sampleCounts || width <= 0 || height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize(
        (width + blockSize.x - 1) / blockSize.x,
        (height + blockSize.y - 1) / blockSize.y);
    AccumulateTripodReferenceKernel<<<gridSize, blockSize, 0, stream>>>(
        current, reference, width, height, factorX, factorY,
        focusScore, bestFocusScore, state, accumulator, sampleCounts);
    CheckCuda("AccumulateTripodReferenceKernel launch failed");
}

void LaunchFinalizeVirtualTripodReference(
    float* reference, const float* accumulator,
    const unsigned int* sampleCounts, int pixelCount, cudaStream_t stream) {
    if (!reference || !accumulator || !sampleCounts || pixelCount <= 0) {
        return;
    }
    constexpr int blockSize = 256;
    const int blocks = (pixelCount + blockSize - 1) / blockSize;
    FinalizeTripodReferenceKernel<<<blocks, blockSize, 0, stream>>>(
        reference, accumulator, sampleCounts, pixelCount);
    CheckCuda("FinalizeTripodReferenceKernel launch failed");
}

void LaunchResetVirtualTripodState(StabilizationState* state,
                                  bool preserveCorrection,
                                  cudaStream_t stream) {
    if (!state) {
        return;
    }
    ResetVirtualTripodStateKernel<<<1, 1, 0, stream>>>(
        state, preserveCorrection ? 1 : 0);
    CheckCuda("ResetVirtualTripodStateKernel launch failed");
}

void LaunchVirtualTripodSimilarityEstimate(const float4* pairs,
                                           const unsigned int* pairCount,
                                           unsigned int maxPairs,
                                           int fullWidth, int fullHeight,
                                           float inlierThresholdPixels,
                                           float strength,
                                           float maxCorrectionFraction,
                                           float focusCenterX,
                                           float focusCenterY,
                                           float zoomAmount,
                                           StabilizationState* state,
                                           cudaStream_t stream) {
    if (!pairs || !pairCount || !state || maxPairs == 0 ||
        fullWidth <= 0 || fullHeight <= 0) {
        return;
    }
    VirtualTripodSimilarityEstimateKernel<<<1, kStabEstimateBlockSize, 0, stream>>>(
        pairs, pairCount, maxPairs, fullWidth, fullHeight,
        fmaxf(inlierThresholdPixels, 0.35f),
        fminf(fmaxf(strength, 0.0f), 1.0f),
        maxCorrectionFraction,
        fminf(fmaxf(focusCenterX, 0.0f), 1.0f),
        fminf(fmaxf(focusCenterY, 0.0f), 1.0f),
        fmaxf(zoomAmount, 1.0f), state,
        nullptr, nullptr, 0u, nullptr, 0);
    CheckCuda("VirtualTripodSimilarityEstimateKernel launch failed");
}

void LaunchVirtualTripodMatchCandidate(
    const float4* pairs, const unsigned int* pairCount,
    unsigned int maxPairs, int fullWidth, int fullHeight,
    float inlierThresholdPixels, float focusCenterX, float focusCenterY,
    float zoomAmount, const float4* keyframeOrigins,
    const unsigned int* keyframeValid, unsigned int keyframeIndex,
    TripodMatchCandidate* candidates, cudaStream_t stream) {
    if (!pairs || !pairCount || !keyframeOrigins || !keyframeValid ||
        !candidates || maxPairs == 0u || fullWidth <= 0 ||
        fullHeight <= 0) {
        return;
    }
    VirtualTripodSimilarityEstimateKernel<<<
        1, kStabEstimateBlockSize, 0, stream>>>(
        pairs, pairCount, maxPairs, fullWidth, fullHeight,
        fmaxf(inlierThresholdPixels, 0.35f), 1.0f, 0.45f,
        fminf(fmaxf(focusCenterX, 0.0f), 1.0f),
        fminf(fmaxf(focusCenterY, 0.0f), 1.0f),
        fmaxf(zoomAmount, 1.0f), nullptr,
        keyframeOrigins, keyframeValid, keyframeIndex,
        candidates, 1);
    CheckCuda("VirtualTripodSimilarityEstimateKernel candidate launch failed");
}

void LaunchSelectVirtualTripodMatch(
    const TripodMatchCandidate* candidates, unsigned int candidateCount,
    float strength, float maxCorrectionFraction, float zoomAmount,
    int fullWidth, int fullHeight, StabilizationState* state,
    cudaStream_t stream) {
    if (!candidates || candidateCount == 0u || !state ||
        fullWidth <= 0 || fullHeight <= 0) {
        return;
    }
    SelectVirtualTripodMatchKernel<<<1, 1, 0, stream>>>(
        candidates, candidateCount,
        fminf(fmaxf(strength, 0.0f), 1.0f),
        maxCorrectionFraction, fmaxf(zoomAmount, 1.0f),
        fullWidth, fullHeight, state);
    CheckCuda("SelectVirtualTripodMatchKernel launch failed");
}

void LaunchCaptureVirtualTripodKeyframe(
    const float* current, float* destination, int pixelCount,
    const StabilizationState* state, float4* keyframeOrigins,
    unsigned int* keyframeValid, unsigned int keyframeIndex,
    unsigned int minimumValidFrames, cudaStream_t stream) {
    if (!current || !destination || pixelCount <= 0 || !state ||
        !keyframeOrigins || !keyframeValid) {
        return;
    }
    constexpr int blockSize = 256;
    const int blocks = (pixelCount + blockSize - 1) / blockSize;
    CaptureVirtualTripodKeyframeKernel<<<blocks, blockSize, 0, stream>>>(
        current, destination, pixelCount, state,
        keyframeOrigins, keyframeValid, keyframeIndex,
        minimumValidFrames);
    CheckCuda("CaptureVirtualTripodKeyframeKernel launch failed");
}

void LaunchInitializeVirtualTripodKeyframe(
    float4* keyframeOrigins, unsigned int* keyframeValid,
    unsigned int keyframeIndex, cudaStream_t stream) {
    if (!keyframeOrigins || !keyframeValid) {
        return;
    }
    InitializeVirtualTripodKeyframeKernel<<<1, 1, 0, stream>>>(
        keyframeOrigins, keyframeValid, keyframeIndex);
    CheckCuda("InitializeVirtualTripodKeyframeKernel launch failed");
}

void LaunchVirtualTripodRelativeFallback(
    const float4* pairs, const unsigned int* pairCount,
    unsigned int maxPairs, int fullWidth, int fullHeight,
    float inlierThresholdPixels, float maxCorrectionFraction,
    StabilizationState* state, cudaStream_t stream) {
    if (!pairs || !pairCount || maxPairs == 0u || !state ||
        fullWidth <= 0 || fullHeight <= 0) {
        return;
    }
    VirtualTripodRelativeFallbackKernel<<<1, 1, 0, stream>>>(
        pairs, pairCount, maxPairs, fullWidth, fullHeight,
        fmaxf(inlierThresholdPixels, 0.35f),
        maxCorrectionFraction, state);
    CheckCuda("VirtualTripodRelativeFallbackKernel launch failed");
}

void LaunchStabilizationWarp(uchar4* dst, size_t dstPitchBytes,
                             const uchar4* src, size_t srcPitchBytes,
                             int width, int height,
                             const StabilizationState* state,
                             cudaStream_t stream) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    StabilizationWarpKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes,
                                                                src, srcPitchBytes,
                                                                width, height,
                                                                state);
    CheckCuda("StabilizationWarpKernel launch failed");
}

void LaunchDisplayColorGradeLinear(uchar4* buffer, size_t pitchBytes,
                                   int width, int height,
                                   int colorTransform, float contrast, float brightness,
                                   const float2* autoContrastLevels,
                                   float autoContrastStrength,
                                   cudaStream_t stream) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    DisplayColorGradeKernel<<<gridSize, blockSize, 0, stream>>>(buffer, pitchBytes,
                                                                width, height,
                                                                colorTransform, contrast, brightness,
                                                                autoContrastLevels,
                                                                autoContrastStrength);
    CheckCuda("DisplayColorGradeKernel launch failed");
}

void LaunchNv12ToBgraLinear(uchar4* dst, size_t dstPitchBytes,
                            const unsigned char* yPlane, size_t yPitchBytes,
                            const unsigned char* uvPlane, size_t uvPitchBytes,
                            int width, int height,
                            cudaStream_t stream, YuvColorInfo color) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    Nv12ToBgraKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes,
                                                         yPlane, yPitchBytes,
                                                         uvPlane, uvPitchBytes,
                                                         width, height, GetYuvCoefficients(color));
    CheckCuda("Nv12ToBgraKernel launch failed");
}

void LaunchYuy2ToBgraLinear(uchar4* dst, size_t dstPitchBytes,
                            const unsigned char* src, size_t srcPitchBytes,
                            int width, int height,
                            cudaStream_t stream, YuvColorInfo color) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const int pairCount = (width + 1) / 2;
    const dim3 blockSize(16, 16);
    const dim3 gridSize((pairCount + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    Yuy2ToBgraKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes,
                                                         src, srcPitchBytes,
                                                         width, height, GetYuvCoefficients(color));
    CheckCuda("Yuy2ToBgraKernel launch failed");
}

void LaunchRotateQuarterLinear(uchar4* dst, size_t dstPitchBytes,
                               const uchar4* src, size_t srcPitchBytes,
                               int srcWidth, int srcHeight,
                               int quarterTurnsClockwise,
                               cudaStream_t stream) {
    if (srcWidth <= 0 || srcHeight <= 0 ||
        quarterTurnsClockwise < 1 || quarterTurnsClockwise > 3) {
        return;
    }
    const bool swapExtent = (quarterTurnsClockwise & 1) != 0;
    const int dstWidth = swapExtent ? srcHeight : srcWidth;
    const int dstHeight = swapExtent ? srcWidth : srcHeight;
    const dim3 blockSize(16, 16);
    const dim3 gridSize((dstWidth + blockSize.x - 1) / blockSize.x,
                        (dstHeight + blockSize.y - 1) / blockSize.y);
    RotateQuarterKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes,
                                                            src, srcPitchBytes,
                                                            srcWidth, srcHeight,
                                                            dstWidth, dstHeight,
                                                            quarterTurnsClockwise);
    CheckCuda("RotateQuarterKernel launch failed");
}

void LaunchKeystoneWarp(uchar4* dst, size_t dstPitchBytes,
                        const uchar4* src, size_t srcPitchBytes,
                        int width, int height,
                        const float homography[9],
                        cudaStream_t stream) {
    if (width <= 0 || height <= 0 || homography == nullptr) {
        return;
    }
    KeystoneHomography h{};
    for (int i = 0; i < 9; ++i) {
        h.m[i] = homography[i];
    }
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    KeystoneWarpKernel<<<gridSize, blockSize, 0, stream>>>(dst, dstPitchBytes,
                                                           src, srcPitchBytes,
                                                           width, height,
                                                           h);
    CheckCuda("KeystoneWarpKernel launch failed");
}

void LaunchAutoContrastHistogram(unsigned int* histogram256,
                                 const uchar4* src, size_t srcPitchBytes,
                                 int width, int height,
                                 cudaStream_t stream) {
    if (width <= 0 || height <= 0) {
        return;
    }
    CheckCudaStatus(cudaMemsetAsync(histogram256, 0, 256 * sizeof(unsigned int), stream),
                    "cudaMemsetAsync histogram failed");
    // 16x16 = 256 threads per block: one shared bin per thread.
    const dim3 blockSize(16, 16);
    const dim3 gridSize((width + blockSize.x - 1) / blockSize.x,
                        (height + blockSize.y - 1) / blockSize.y);
    AutoContrastHistogramKernel<<<gridSize, blockSize, 0, stream>>>(histogram256,
                                                                    src, srcPitchBytes,
                                                                    width, height);
    CheckCuda("AutoContrastHistogramKernel launch failed");
}

void LaunchAutoContrastAnalysis(const unsigned int* histogram256,
                                int pixelCount,
                                float2* levels,
                                bool levelsValid,
                                cudaStream_t stream) {
    if (pixelCount <= 0) {
        return;
    }
    AutoContrastAnalysisKernel<<<1, 1, 0, stream>>>(histogram256,
                                                    pixelCount,
                                                    levels,
                                                    levelsValid ? 1 : 0);
    CheckCuda("AutoContrastAnalysisKernel launch failed");
}

namespace {

__device__ inline const float* FloatRow(const float* base, size_t pitch, int y) {
    return reinterpret_cast<const float*>(reinterpret_cast<const char*>(base) + static_cast<size_t>(y) * pitch);
}

__device__ inline float* FloatRow(float* base, size_t pitch, int y) {
    return reinterpret_cast<float*>(reinterpret_cast<char*>(base) + static_cast<size_t>(y) * pitch);
}

__device__ inline const unsigned char* ByteRow(const unsigned char* base, size_t pitch, int y) {
    return reinterpret_cast<const unsigned char*>(reinterpret_cast<const char*>(base) + static_cast<size_t>(y) * pitch);
}

__device__ inline unsigned char* ByteRow(unsigned char* base, size_t pitch, int y) {
    return reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(base) + static_cast<size_t>(y) * pitch);
}

__device__ inline float PixelLuma(const uchar4& p) {
    return (0.114f * static_cast<float>(p.x) +
            0.587f * static_cast<float>(p.y) +
            0.299f * static_cast<float>(p.z)) / 255.0f;
}

__global__ void TextLumaKernel(float* luma, size_t floatPitch,
                               const uchar4* src, size_t srcPitch,
                               int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    FloatRow(luma, floatPitch, y)[x] = PixelLuma(RowAt(src, srcPitch, y)[x]);
}

// One thread per row/column gives an O(N) sliding window without integral-image
// overflow or a wide per-pixel loop. Camera rows/columns run independently.
__global__ void TextBoxHorizontalKernel(const float* luma, size_t pitch,
                                        float* horizontal, float* sqHorizontal,
                                        int width, int height, int radius) {
    const int y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= height) return;
    const float* in = FloatRow(luma, pitch, y);
    float* out = FloatRow(horizontal, pitch, y);
    float* sqOut = FloatRow(sqHorizontal, pitch, y);
    float sum = 0.0f;
    float sq = 0.0f;
    int count = 0;
    for (int i = 0; i <= min(radius, width - 1); ++i) {
        sum += in[i]; sq += in[i] * in[i]; ++count;
    }
    for (int x = 0; x < width; ++x) {
        if (x > 0) {
            const int add = x + radius;
            const int remove = x - radius - 1;
            if (add < width) { sum += in[add]; sq += in[add] * in[add]; ++count; }
            if (remove >= 0) { sum -= in[remove]; sq -= in[remove] * in[remove]; --count; }
        }
        out[x] = sum / static_cast<float>(max(count, 1));
        sqOut[x] = sq / static_cast<float>(max(count, 1));
    }
}

__global__ void TextBoxVerticalKernel(const float* horizontal,
                                      const float* sqHorizontal,
                                      size_t pitch, float* mean, float* sqMean,
                                      int width, int height, int radius) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= width) return;
    float sum = 0.0f;
    float sq = 0.0f;
    int count = 0;
    for (int i = 0; i <= min(radius, height - 1); ++i) {
        sum += FloatRow(horizontal, pitch, i)[x];
        sq += FloatRow(sqHorizontal, pitch, i)[x];
        ++count;
    }
    for (int y = 0; y < height; ++y) {
        if (y > 0) {
            const int add = y + radius;
            const int remove = y - radius - 1;
            if (add < height) {
                sum += FloatRow(horizontal, pitch, add)[x];
                sq += FloatRow(sqHorizontal, pitch, add)[x];
                ++count;
            }
            if (remove >= 0) {
                sum -= FloatRow(horizontal, pitch, remove)[x];
                sq -= FloatRow(sqHorizontal, pitch, remove)[x];
                --count;
            }
        }
        FloatRow(mean, pitch, y)[x] = sum / static_cast<float>(max(count, 1));
        FloatRow(sqMean, pitch, y)[x] = sq / static_cast<float>(max(count, 1));
    }
}

__global__ void TextSceneAnalysisKernel(const unsigned int* histogram,
                                        int pixelCount, int4* analysis) {
    if (blockIdx.x != 0 || threadIdx.x != 0 || pixelCount <= 0) return;
    double sum = 0.0;
    double sumSq = 0.0;
    for (int i = 0; i < 256; ++i) {
        const double n = static_cast<double>(histogram[i]);
        const double v = static_cast<double>(i) / 255.0;
        sum += n * v;
        sumSq += n * v * v;
    }
    const float mean = static_cast<float>(sum / static_cast<double>(pixelCount));
    const float variance = fmaxf(static_cast<float>(sumSq / static_cast<double>(pixelCount)) - mean * mean, 0.0f);
    const float contrast = sqrtf(variance);
    double thirdMoment = 0.0;
    for (int i = 0; i < 256; ++i) {
        const double centered = static_cast<double>(i) / 255.0 - static_cast<double>(mean);
        thirdMoment += static_cast<double>(histogram[i]) * centered * centered * centered;
    }
    thirdMoment /= static_cast<double>(pixelCount);
    // Bright strokes on a dark field create a positive luma tail; dark ink on
    // pale paper creates a negative one. Near-symmetric scenes use mean as a
    // stable fallback instead of letting tiny histogram changes flip polarity.
    const int lightText = fabs(thirdMoment) > 1.0e-5 ? (thirdMoment > 0.0 ? 1 : 0)
                                                     : (mean < 0.46f ? 1 : 0);
    const int scene = mean > 0.62f ? 1 : (mean < 0.34f ? 2 : 0);
    *analysis = make_int4(lightText, scene,
                          static_cast<int>(mean * 10000.0f),
                          static_cast<int>(contrast * 10000.0f));
}

__global__ void BackgroundFlattenKernel(uchar4* dst, size_t dstPitch,
                                        const uchar4* src, size_t srcPitch,
                                        const float* luma, const float* mean,
                                        size_t floatPitch, int width, int height,
                                        float strength, int suppressGlare,
                                        float glareStrength, const int4* analysis) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const uchar4 p = RowAt(src, srcPitch, y)[x];
    const float lum = FloatRow(luma, floatPitch, y)[x];
    const float background = FloatRow(mean, floatPitch, y)[x];
    const int scene = analysis ? analysis->y : 0;
    const float backgroundTarget = scene == 1 ? 0.78f : (scene == 2 ? 0.28f : 0.55f);
    float corrected = fminf(fmaxf(lum + fminf(fmaxf(strength, 0.0f), 1.0f) *
                                  (backgroundTarget - background), 0.0f), 1.0f);
    if (suppressGlare && lum > 0.94f && background < 0.91f) {
        const float replacement = fminf(background + 0.08f, 0.94f);
        const float glare = fminf(fmaxf((lum - 0.94f) / 0.06f, 0.0f), 1.0f) *
                            fminf(fmaxf(glareStrength, 0.0f), 1.0f);
        corrected += glare * (replacement - corrected);
    }
    const float delta = (corrected - lum) * 255.0f;
    uchar4* row = RowAt(dst, dstPitch, y);
    row[x] = make_uchar4(static_cast<unsigned char>(fminf(fmaxf(static_cast<float>(p.x) + delta, 0.0f), 255.0f)),
                         static_cast<unsigned char>(fminf(fmaxf(static_cast<float>(p.y) + delta, 0.0f), 255.0f)),
                         static_cast<unsigned char>(fminf(fmaxf(static_cast<float>(p.z) + delta, 0.0f), 255.0f)), p.w);
}

constexpr int kClaheTilesX = 8;
constexpr int kClaheTilesY = 8;

__global__ void ClaheHistogramKernel(unsigned int* histograms,
                                     const uchar4* src, size_t srcPitch,
                                     int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int tx = min((x * kClaheTilesX) / max(width, 1), kClaheTilesX - 1);
    const int ty = min((y * kClaheTilesY) / max(height, 1), kClaheTilesY - 1);
    const int bin = min(static_cast<int>(PixelLuma(RowAt(src, srcPitch, y)[x]) * 255.0f), 255);
    atomicAdd(&histograms[(ty * kClaheTilesX + tx) * 256 + bin], 1u);
}

__global__ void ClaheMapKernel(const unsigned int* histograms, float* maps,
                               int width, int height, float clipLimit) {
    const int tile = blockIdx.x;
    const int bin = threadIdx.x;
    __shared__ unsigned int scan[256];
    __shared__ unsigned int clippedTotal;
    if (bin == 0) clippedTotal = 0;
    __syncthreads();
    const int tilePixels = max(1, ((width + 7) / 8) * ((height + 7) / 8));
    const unsigned int clip = max(1u, static_cast<unsigned int>(fmaxf(clipLimit, 1.0f) * tilePixels / 256.0f));
    unsigned int value = histograms[tile * 256 + bin];
    if (value > clip) atomicAdd(&clippedTotal, value - clip);
    value = min(value, clip);
    scan[bin] = value;
    __syncthreads();
    value += clippedTotal / 256u + (static_cast<unsigned int>(bin) < (clippedTotal % 256u) ? 1u : 0u);
    scan[bin] = value;
    __syncthreads();
    for (int offset = 1; offset < 256; offset <<= 1) {
        const unsigned int add = bin >= offset ? scan[bin - offset] : 0u;
        __syncthreads();
        scan[bin] += add;
        __syncthreads();
    }
    const unsigned int total = max(scan[255], 1u);
    maps[tile * 256 + bin] = static_cast<float>(scan[bin]) / static_cast<float>(total);
}

__global__ void ClaheApplyKernel(uchar4* dst, size_t dstPitch,
                                 const uchar4* src, size_t srcPitch,
                                 const float* maps, int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const uchar4 p = RowAt(src, srcPitch, y)[x];
    const float lum = PixelLuma(p);
    const int bin = min(static_cast<int>(lum * 255.0f), 255);
    const float gx = (static_cast<float>(x) + 0.5f) * kClaheTilesX / max(width, 1) - 0.5f;
    const float gy = (static_cast<float>(y) + 0.5f) * kClaheTilesY / max(height, 1) - 0.5f;
    const int x0 = max(0, min(kClaheTilesX - 1, static_cast<int>(floorf(gx))));
    const int y0 = max(0, min(kClaheTilesY - 1, static_cast<int>(floorf(gy))));
    const int x1 = min(x0 + 1, kClaheTilesX - 1);
    const int y1 = min(y0 + 1, kClaheTilesY - 1);
    const float tx = fminf(fmaxf(gx - floorf(gx), 0.0f), 1.0f);
    const float ty = fminf(fmaxf(gy - floorf(gy), 0.0f), 1.0f);
    const float a = maps[(y0 * 8 + x0) * 256 + bin];
    const float b = maps[(y0 * 8 + x1) * 256 + bin];
    const float c = maps[(y1 * 8 + x0) * 256 + bin];
    const float d = maps[(y1 * 8 + x1) * 256 + bin];
    const float mapped = (a + tx * (b - a)) + ty * ((c + tx * (d - c)) - (a + tx * (b - a)));
    const float delta = (mapped - lum) * 255.0f;
    RowAt(dst, dstPitch, y)[x] = make_uchar4(
        static_cast<unsigned char>(fminf(fmaxf(p.x + delta, 0.0f), 255.0f)),
        static_cast<unsigned char>(fminf(fmaxf(p.y + delta, 0.0f), 255.0f)),
        static_cast<unsigned char>(fminf(fmaxf(p.z + delta, 0.0f), 255.0f)), p.w);
}

__device__ inline float SmoothStep(float edge0, float edge1, float x) {
    const float t = fminf(fmaxf((x - edge0) / fmaxf(edge1 - edge0, 1.0e-5f), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

__global__ void SauvolaMaskKernel(unsigned char* mask, size_t maskPitch,
                                  const float* luma, const float* mean,
                                  const float* sqMean, size_t floatPitch,
                                  int width, int height, float k, float softness,
                                  int polarityMode, const int4* analysis) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const float lum = FloatRow(luma, floatPitch, y)[x];
    const float m = FloatRow(mean, floatPitch, y)[x];
    const float variance = fmaxf(FloatRow(sqMean, floatPitch, y)[x] - m * m, 0.0f);
    const float sauvolaFactor = 1.0f + fminf(fmaxf(k, 0.1f), 0.5f) * (sqrtf(variance) / 0.5f - 1.0f);
    const float threshold = m * sauvolaFactor;
    const float s = fminf(fmaxf(softness, 0.002f), 0.25f);
    const bool lightText = polarityMode == 2 || (polarityMode == 0 && analysis && analysis->x != 0);
    // Sauvola assumes dark ink on a light background. Apply it to inverted
    // intensity and mean for light text; variance is unchanged by inversion.
    // Reversing only the comparison would classify a uniform dark field as ink.
    const float invertedThreshold = (1.0f - m) * sauvolaFactor;
    const float ink = lightText ? 1.0f - SmoothStep(invertedThreshold - s, invertedThreshold + s, 1.0f - lum)
                                : 1.0f - SmoothStep(threshold - s, threshold + s, lum);
    ByteRow(mask, maskPitch, y)[x] = FloatToByte(ink);
}

__global__ void StrokeWeightKernel(unsigned char* dst, size_t dstPitch,
                                   const unsigned char* src, size_t srcPitch,
                                   int width, int height, int radius, int dilate) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    int result = dilate ? 0 : 255;
    for (int j = -radius; j <= radius; ++j) {
        const unsigned char* row = ByteRow(src, srcPitch, max(0, min(height - 1, y + j)));
        for (int i = -radius; i <= radius; ++i) {
            const int v = row[max(0, min(width - 1, x + i))];
            result = dilate ? max(result, v) : min(result, v);
        }
    }
    ByteRow(dst, dstPitch, y)[x] = static_cast<unsigned char>(result);
}

__global__ void TextHysteresisKernel(unsigned char* mask, size_t pitch,
                                     unsigned char* history, int width, int height,
                                     float strength, int historyValid) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    unsigned char* current = ByteRow(mask, pitch, y);
    unsigned char* previous = ByteRow(history, pitch, y);
    float value = current[x] / 255.0f;
    if (historyValid && fabsf(value - 0.5f) < strength) value = previous[x] / 255.0f;
    const unsigned char stable = value >= 0.5f ? 255u : 0u;
    current[x] = stable;
    previous[x] = stable;
}

__device__ inline uchar4 TextColors(float ink,
                                    std::uint32_t foregroundBgra,
                                    std::uint32_t backgroundBgra) {
    const uchar4 foreground = UnpackBgra(foregroundBgra);
    const uchar4 background = UnpackBgra(backgroundBgra);
    return make_uchar4(
        static_cast<unsigned char>(background.x + ink * (foreground.x - background.x)),
        static_cast<unsigned char>(background.y + ink * (foreground.y - background.y)),
        static_cast<unsigned char>(background.z + ink * (foreground.z - background.z)), 255u);
}

__global__ void TextMaskCompositeKernel(uchar4* dst, size_t dstPitch,
                                        const uchar4* src, size_t srcPitch,
                                        const unsigned char* mask, size_t maskPitch,
                                        int width, int height,
                                        std::uint32_t foregroundBgra,
                                        std::uint32_t backgroundBgra,
                                        int compositeMode, const int4* analysis) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const float ink = ByteRow(mask, maskPitch, y)[x] / 255.0f;
    const uchar4 mapped = TextColors(ink, foregroundBgra, backgroundBgra);
    const bool replaceImage = compositeMode == 1 ||
                              (compositeMode == 2 && analysis && analysis->y != 0);
    if (replaceImage) {
        RowAt(dst, dstPitch, y)[x] = mapped;
    } else if (compositeMode == 2) {
        // Mixed content: background flattening plus selective smart sharpen
        // already improved text; leave photos and diagrams in natural color.
        RowAt(dst, dstPitch, y)[x] = RowAt(src, srcPitch, y)[x];
    } else {
        const uchar4 p = RowAt(src, srcPitch, y)[x];
        const float blend = 0.7f * ink;
        RowAt(dst, dstPitch, y)[x] = make_uchar4(
            static_cast<unsigned char>(p.x + blend * (mapped.x - p.x)),
            static_cast<unsigned char>(p.y + blend * (mapped.y - p.y)),
            static_cast<unsigned char>(p.z + blend * (mapped.z - p.z)), p.w);
    }
}

__global__ void Bilateral3x3Kernel(uchar4* dst, size_t dstPitch,
                                   const uchar4* src, size_t srcPitch,
                                   int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const float3 center = ReadPixelLinear(src, srcPitch, x, y, width, height);
    float3 sum = make_float3(0.0f, 0.0f, 0.0f);
    float weights = 0.0f;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const float3 p = ReadPixelLinear(src, srcPitch, x + i, y + j, width, height);
            const float db = p.x - center.x, dg = p.y - center.y, dr = p.z - center.z;
            const float spatial = (i == 0 && j == 0) ? 1.0f : ((i == 0 || j == 0) ? 0.78f : 0.61f);
            const float range = expf(-(db * db + dg * dg + dr * dr) / (3.0f * 28.0f * 28.0f));
            const float w = spatial * range;
            sum.x += p.x * w; sum.y += p.y * w; sum.z += p.z * w; weights += w;
        }
    }
    RowAt(dst, dstPitch, y)[x] = make_uchar4(
        static_cast<unsigned char>(sum.x / weights + 0.5f),
        static_cast<unsigned char>(sum.y / weights + 0.5f),
        static_cast<unsigned char>(sum.z / weights + 0.5f), 255u);
}

__global__ void SmartSharpenKernel(uchar4* dst, size_t dstPitch,
                                   const uchar4* src, size_t srcPitch,
                                   const unsigned char* mask, size_t maskPitch,
                                   int width, int height, float strength, int selective) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const float3 center = ReadPixelLinear(src, srcPitch, x, y, width, height);
    const float3 blurred = BoxBlur3x3(src, srcPitch, x, y, width, height);
    float factor = fminf(fmaxf(strength, 0.0f), 1.0f) * 1.6f;
    if (selective && mask) {
        int lo = 255, hi = 0;
        for (int j = -1; j <= 1; ++j) {
            const unsigned char* row = ByteRow(mask, maskPitch, max(0, min(height - 1, y + j)));
            for (int i = -1; i <= 1; ++i) {
                const int v = row[max(0, min(width - 1, x + i))]; lo = min(lo, v); hi = max(hi, v);
            }
        }
        factor *= static_cast<float>(hi - lo) / 255.0f;
    }
    const float clampAmount = 28.0f;
    const float db = fminf(fmaxf((center.x - blurred.x) * factor, -clampAmount), clampAmount);
    const float dg = fminf(fmaxf((center.y - blurred.y) * factor, -clampAmount), clampAmount);
    const float dr = fminf(fmaxf((center.z - blurred.z) * factor, -clampAmount), clampAmount);
    RowAt(dst, dstPitch, y)[x] = make_uchar4(
        static_cast<unsigned char>(fminf(fmaxf(center.x + db, 0.0f), 255.0f)),
        static_cast<unsigned char>(fminf(fmaxf(center.y + dg, 0.0f), 255.0f)),
        static_cast<unsigned char>(fminf(fmaxf(center.z + dr, 0.0f), 255.0f)), 255u);
}

__global__ void FocusMetricKernel(const float* luma, size_t pitch,
                                  int width, int height, float2* stats) {
    const int sx = blockIdx.x * blockDim.x + threadIdx.x;
    const int sy = blockIdx.y * blockDim.y + threadIdx.y;
    const int x = sx * 4 + 2;
    const int y = sy * 4 + 2;
    if (x >= width - 1 || y >= height - 1) return;
    const float center = FloatRow(luma, pitch, y)[x];
    const float lap = FloatRow(luma, pitch, y)[x - 1] + FloatRow(luma, pitch, y)[x + 1] +
                      FloatRow(luma, pitch, y - 1)[x] + FloatRow(luma, pitch, y + 1)[x] - 4.0f * center;
    atomicAdd(&stats->x, lap);
    atomicAdd(&stats->y, lap * lap);
}

} // namespace

void LaunchTextLocalStatistics(float* luma, size_t floatPitchBytes,
                               float* horizontal, float* mean,
                               float* sqHorizontal, float* sqMean,
                               const uchar4* src, size_t srcPitchBytes,
                               int width, int height, int radius,
                               cudaStream_t stream) {
    const dim3 block(16, 16);
    const dim3 grid((width + 15) / 16, (height + 15) / 16);
    TextLumaKernel<<<grid, block, 0, stream>>>(luma, floatPitchBytes, src, srcPitchBytes, width, height);
    TextBoxHorizontalKernel<<<(height + 127) / 128, 128, 0, stream>>>(luma, floatPitchBytes,
        horizontal, sqHorizontal, width, height, max(1, radius));
    TextBoxVerticalKernel<<<(width + 127) / 128, 128, 0, stream>>>(horizontal, sqHorizontal,
        floatPitchBytes, mean, sqMean, width, height, max(1, radius));
    CheckCuda("text local-statistics launch failed");
}

void LaunchTextSceneAnalysis(const unsigned int* histogram256, int pixelCount,
                             int4* analysis, cudaStream_t stream) {
    TextSceneAnalysisKernel<<<1, 1, 0, stream>>>(histogram256, pixelCount, analysis);
    CheckCuda("text scene analysis launch failed");
}

void LaunchBackgroundFlattenLinear(uchar4* dst, size_t dstPitchBytes,
                                   const uchar4* src, size_t srcPitchBytes,
                                   const float* luma, const float* mean,
                                   size_t floatPitchBytes, int width, int height,
                                   float strength, bool suppressGlare,
                                   float glareStrength, const int4* analysis,
                                   cudaStream_t stream) {
    const dim3 block(16, 16), grid((width + 15) / 16, (height + 15) / 16);
    BackgroundFlattenKernel<<<grid, block, 0, stream>>>(dst, dstPitchBytes, src, srcPitchBytes,
        luma, mean, floatPitchBytes, width, height, strength,
        suppressGlare ? 1 : 0, glareStrength, analysis);
    CheckCuda("background flatten launch failed");
}

void LaunchClaheLinear(uchar4* dst, size_t dstPitchBytes,
                       const uchar4* src, size_t srcPitchBytes,
                       unsigned int* tileHistograms, float* tileMaps,
                       int width, int height, float clipLimit,
                       cudaStream_t stream) {
    CheckCudaStatus(cudaMemsetAsync(tileHistograms, 0, 64 * 256 * sizeof(unsigned int), stream),
                    "CLAHE histogram clear failed");
    const dim3 block(16, 16), grid((width + 15) / 16, (height + 15) / 16);
    ClaheHistogramKernel<<<grid, block, 0, stream>>>(tileHistograms, src, srcPitchBytes, width, height);
    ClaheMapKernel<<<64, 256, 0, stream>>>(tileHistograms, tileMaps, width, height, clipLimit);
    ClaheApplyKernel<<<grid, block, 0, stream>>>(dst, dstPitchBytes, src, srcPitchBytes, tileMaps, width, height);
    CheckCuda("CLAHE launch failed");
}

void LaunchSauvolaMask(unsigned char* mask, size_t maskPitchBytes,
                       const float* luma, const float* mean, const float* sqMean,
                       size_t floatPitchBytes, int width, int height,
                       float strength, float softness, int polarityMode,
                       const int4* analysis, cudaStream_t stream) {
    const dim3 block(16, 16), grid((width + 15) / 16, (height + 15) / 16);
    SauvolaMaskKernel<<<grid, block, 0, stream>>>(mask, maskPitchBytes, luma, mean, sqMean,
        floatPitchBytes, width, height, strength, softness, polarityMode, analysis);
    CheckCuda("Sauvola mask launch failed");
}

void LaunchStrokeWeight(unsigned char* dst, size_t dstPitchBytes,
                        const unsigned char* src, size_t srcPitchBytes,
                        int width, int height, int strokeWeight,
                        cudaStream_t stream) {
    if (strokeWeight == 0) return;
    const dim3 block(16, 16), grid((width + 15) / 16, (height + 15) / 16);
    StrokeWeightKernel<<<grid, block, 0, stream>>>(dst, dstPitchBytes, src, srcPitchBytes,
        width, height, min(abs(strokeWeight), 3), strokeWeight > 0 ? 1 : 0);
    CheckCuda("stroke-weight launch failed");
}

void LaunchTextMaskHysteresis(unsigned char* mask, size_t maskPitchBytes,
                              unsigned char* history, int width, int height,
                              float strength, bool historyValid,
                              cudaStream_t stream) {
    const dim3 block(16, 16), grid((width + 15) / 16, (height + 15) / 16);
    TextHysteresisKernel<<<grid, block, 0, stream>>>(mask, maskPitchBytes, history,
        width, height, strength, historyValid ? 1 : 0);
    CheckCuda("text hysteresis launch failed");
}

void LaunchTextMaskComposite(uchar4* dst, size_t dstPitchBytes,
                             const uchar4* src, size_t srcPitchBytes,
                             const unsigned char* mask, size_t maskPitchBytes,
                             int width, int height,
                             std::uint32_t foregroundBgra,
                             std::uint32_t backgroundBgra,
                             int compositeMode, const int4* analysis,
                             cudaStream_t stream) {
    const dim3 block(16, 16), grid((width + 15) / 16, (height + 15) / 16);
    TextMaskCompositeKernel<<<grid, block, 0, stream>>>(dst, dstPitchBytes, src, srcPitchBytes,
        mask, maskPitchBytes, width, height, foregroundBgra, backgroundBgra,
        compositeMode, analysis);
    CheckCuda("text-mask composite launch failed");
}

void LaunchSmartSharpenLinear(uchar4* dst, size_t dstPitchBytes,
                              uchar4* scratch, size_t scratchPitchBytes,
                              const uchar4* src, size_t srcPitchBytes,
                              const unsigned char* mask, size_t maskPitchBytes,
                              int width, int height, float strength,
                              bool selective, cudaStream_t stream) {
    const dim3 block(16, 16), grid((width + 15) / 16, (height + 15) / 16);
    Bilateral3x3Kernel<<<grid, block, 0, stream>>>(scratch, scratchPitchBytes, src, srcPitchBytes, width, height);
    SmartSharpenKernel<<<grid, block, 0, stream>>>(dst, dstPitchBytes, scratch, scratchPitchBytes,
        mask, maskPitchBytes, width, height, strength, selective ? 1 : 0);
    CheckCuda("smart-sharpen launch failed");
}

void LaunchFocusMetric(const float* luma, size_t floatPitchBytes,
                       int width, int height, float2* stats,
                       cudaStream_t stream) {
    CheckCudaStatus(cudaMemsetAsync(stats, 0, sizeof(float2), stream), "focus metric clear failed");
    const int sampleWidth = (width + 3) / 4;
    const int sampleHeight = (height + 3) / 4;
    const dim3 block(16, 16), grid((sampleWidth + 15) / 16, (sampleHeight + 15) / 16);
    FocusMetricKernel<<<grid, block, 0, stream>>>(luma, floatPitchBytes, width, height, stats);
    CheckCuda("focus metric launch failed");
}

bool UploadGaussianKernel(int radius, float sigma, cudaStream_t stream) {
    if (radius <= 0 || sigma <= 0.0f) {
        return false;
    }

    const int clampedRadius = std::min(std::max(radius, 1), kMaxBlurRadius);
    const float sigmaClamped = std::max(sigma, 0.001f);
    const int kernelSize = clampedRadius * 2 + 1;

    std::vector<float> kernel(static_cast<size_t>(kernelSize));
    const float sigma2 = sigmaClamped * sigmaClamped;
    const float denom = 2.0f * sigma2;
    float weightSum = 0.0f;
    for (int i = -clampedRadius; i <= clampedRadius; ++i) {
        const float x = static_cast<float>(i);
        const float weight = std::exp(-(x * x) / denom);
        kernel[static_cast<size_t>(i + clampedRadius)] = weight;
        weightSum += weight;
    }
    if (weightSum <= 0.0f) {
        return false;
    }
    for (float& weight : kernel) {
        weight /= weightSum;
    }

    // CUDA stages async H2D copies from pageable host memory before returning
    // to the caller. The local vector/scalar therefore remain valid through
    // both calls. Do not make these sources page-locked without giving them
    // persistent storage plus their own completion event.
    cudaError_t status = cudaMemcpyToSymbolAsync(gGaussianKernel, kernel.data(), kernel.size() * sizeof(float),
                                                 0, cudaMemcpyHostToDevice, stream);
    if (status != cudaSuccess) {
        return false;
    }
    status = cudaMemcpyToSymbolAsync(gGaussianRadius, &clampedRadius, sizeof(int),
                                     0, cudaMemcpyHostToDevice, stream);
    if (status != cudaSuccess) {
        return false;
    }
    return true;
}

bool UploadDisplayColorLut(const std::uint32_t* lut256, cudaStream_t stream) {
    if (!lut256) {
        return false;
    }
    return cudaMemcpyToSymbolAsync(gDisplayColorLut, lut256,
                                   sizeof(gDisplayColorLut), 0,
                                   cudaMemcpyHostToDevice, stream) == cudaSuccess;
}

} // namespace okuflow

#endif // _WIN32
