#include "okuflow/cuda/cuda_kernels.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {

constexpr int kWidth = 37;
constexpr int kHeight = 23;
constexpr int kFloatStride = kWidth + 11;
constexpr int kMaskStride = kWidth + 17;
constexpr unsigned char kGuard = 0xa5;

void CheckCuda(cudaError_t status, const char* operation)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
    }
}

struct DeviceBytes {
    unsigned char* data{};
    explicit DeviceBytes(std::size_t size)
    {
        CheckCuda(cudaMalloc(reinterpret_cast<void**>(&data), size), "cudaMalloc");
    }
    ~DeviceBytes() { cudaFree(data); }
    DeviceBytes(const DeviceBytes&) = delete;
    DeviceBytes& operator=(const DeviceBytes&) = delete;
};

std::vector<float> Complement(const std::vector<float>& image)
{
    std::vector<float> inverted(image.size());
    std::transform(image.begin(), image.end(), inverted.begin(),
                   [](float value) { return 1.0f - value; });
    return inverted;
}

class MaskRunner {
public:
    std::vector<unsigned char> Run(const std::vector<float>& image,
                                   float strength, float softness,
                                   int polarityMode, int lightFlag,
                                   bool useAnalysis = true)
    {
        if (image.size() != kWidth * kHeight) {
            throw std::runtime_error("Incorrect fixture image size");
        }
        const std::size_t floatBytes = kFloatStride * kHeight * sizeof(float);
        std::vector<float> luma(kFloatStride * kHeight, std::numeric_limits<float>::quiet_NaN());
        std::vector<float> mean(luma.size(), std::numeric_limits<float>::quiet_NaN());
        std::vector<float> squareMean(luma.size(), std::numeric_limits<float>::quiet_NaN());
        // Independent fixture moments from a real clamped 9x9 neighborhood;
        // no copy of the production threshold or smooth-step formula lives here.
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                double sum = 0.0;
                double squareSum = 0.0;
                for (int dy = -4; dy <= 4; ++dy) {
                    for (int dx = -4; dx <= 4; ++dx) {
                        const int sx = std::clamp(x + dx, 0, kWidth - 1);
                        const int sy = std::clamp(y + dy, 0, kHeight - 1);
                        const double value = image[sy * kWidth + sx];
                        sum += value;
                        squareSum += value * value;
                    }
                }
                const int index = y * kFloatStride + x;
                luma[index] = image[y * kWidth + x];
                mean[index] = static_cast<float>(sum / 81.0);
                squareMean[index] = static_cast<float>(squareSum / 81.0);
            }
        }
        CheckCuda(cudaMemcpy(luma_.data, luma.data(), floatBytes, cudaMemcpyHostToDevice), "upload luma");
        CheckCuda(cudaMemcpy(mean_.data, mean.data(), floatBytes, cudaMemcpyHostToDevice), "upload mean");
        CheckCuda(cudaMemcpy(squareMean_.data, squareMean.data(), floatBytes, cudaMemcpyHostToDevice),
                  "upload square mean");
        const int4 analysis = make_int4(lightFlag, 0, 0, 0);
        CheckCuda(cudaMemcpy(analysis_.data, &analysis, sizeof(analysis), cudaMemcpyHostToDevice),
                  "upload polarity analysis");
        const std::size_t maskBytes = kMaskStride * (kHeight + 1);
        CheckCuda(cudaMemset(mask_.data, kGuard, maskBytes), "initialize mask guards");
        okuflow::LaunchSauvolaMask(
            mask_.data, kMaskStride,
            reinterpret_cast<const float*>(luma_.data),
            reinterpret_cast<const float*>(mean_.data),
            reinterpret_cast<const float*>(squareMean_.data),
            kFloatStride * sizeof(float), kWidth, kHeight, strength, softness,
            polarityMode, useAnalysis ? reinterpret_cast<const int4*>(analysis_.data) : nullptr,
            nullptr);
        CheckCuda(cudaDeviceSynchronize(), "Sauvola synchronize");
        std::vector<unsigned char> padded(maskBytes);
        CheckCuda(cudaMemcpy(padded.data(), mask_.data, maskBytes, cudaMemcpyDeviceToHost), "read mask");
        std::vector<unsigned char> result(kWidth * kHeight);
        for (int y = 0; y <= kHeight; ++y) {
            for (int x = 0; x < kMaskStride; ++x) {
                const unsigned char value = padded[y * kMaskStride + x];
                if (y < kHeight && x < kWidth) {
                    result[y * kWidth + x] = value;
                } else if (value != kGuard) {
                    throw std::runtime_error("Sauvola overwrote pitched-row or bottom guard bytes");
                }
            }
        }
        return result;
    }

private:
    DeviceBytes luma_{kFloatStride * kHeight * sizeof(float)};
    DeviceBytes mean_{kFloatStride * kHeight * sizeof(float)};
    DeviceBytes squareMean_{kFloatStride * kHeight * sizeof(float)};
    DeviceBytes mask_{kMaskStride * (kHeight + 1)};
    DeviceBytes analysis_{sizeof(int4)};
};

void RequireEqual(const std::vector<unsigned char>& actual,
                   const std::vector<unsigned char>& expected,
                   int tolerance, const char* label)
{
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (std::abs(int(actual[index]) - int(expected[index])) > tolerance) {
            throw std::runtime_error(std::string(label) + " differs at (" +
                std::to_string(index % kWidth) + ", " + std::to_string(index / kWidth) +
                "): " + std::to_string(actual[index]) + " versus " + std::to_string(expected[index]));
        }
    }
}

void UniformBackgroundIsNotInk(MaskRunner& runner)
{
    const std::vector<unsigned char> background(kWidth * kHeight, 0);
    for (float level : {0.0625f, 0.125f, 0.25f, 0.375f}) {
        const std::vector<float> dark(kWidth * kHeight, level);
        for (float strength : {0.1f, 0.3f, 0.5f}) {
            for (float softness : {0.002f, 0.04f}) {
                RequireEqual(runner.Run(dark, strength, softness, 2, 0), background, 0,
                             "Uniform dark background with explicit light text");
                RequireEqual(runner.Run(dark, strength, softness, 0, 1), background, 0,
                             "Uniform dark background with auto light text");
                RequireEqual(runner.Run(Complement(dark), strength, softness, 1, 1), background, 0,
                             "Uniform light background with explicit dark text");
            }
        }
    }
    // Representative board shades with small sensor noise under the normal
    // Sauvola/soft-edge settings must also remain background throughout.
    for (float level : {0.08f, 0.16f, 0.28f}) {
        std::vector<float> noisy(kWidth * kHeight);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                noisy[y * kWidth + x] = level + static_cast<float>((x + 3 * y) % 5 - 2) / 255.0f;
            }
        }
        RequireEqual(runner.Run(noisy, 0.28f, 0.06f, 2, 0), background, 0,
                     "Noisy dark background with explicit light text");
        RequireEqual(runner.Run(noisy, 0.28f, 0.06f, 0, 1), background, 0,
                     "Noisy dark background with auto light text");
    }
}

std::vector<float> MakeLightStrokes(float strokeLevel = 0.875f)
{
    std::vector<float> image(kWidth * kHeight, 0.125f);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const bool stroke = ((x == 6 || x == 7) && y >= 3 && y <= 19) ||
                                ((y == 9 || y == 10) && x >= 15 && x <= 30) ||
                                (x == 23 && y >= 5 && y <= 20);
            if (stroke) {
                image[y * kWidth + x] = strokeLevel;
            }
        }
    }
    return image;
}

void StrokesAndAutoPolarity(MaskRunner& runner)
{
    const std::vector<float> light = MakeLightStrokes();
    const std::vector<float> dark = Complement(light);
    std::vector<unsigned char> expected(light.size());
    std::transform(light.begin(), light.end(), expected.begin(),
                   [](float value) { return value > 0.5f ? 255 : 0; });
    const auto lightMask = runner.Run(light, 0.3f, 0.04f, 2, 0);
    const auto darkMask = runner.Run(dark, 0.3f, 0.04f, 1, 1);
    RequireEqual(lightMask, expected, 0, "Bright strokes and dark background classification");
    RequireEqual(darkMask, expected, 0, "Dark strokes and bright background classification");
    RequireEqual(runner.Run(light, 0.3f, 0.04f, 0, 1), lightMask, 0, "Auto light-text flag");
    RequireEqual(runner.Run(dark, 0.3f, 0.04f, 0, 0), darkMask, 0, "Auto dark-text flag");
    RequireEqual(runner.Run(dark, 0.3f, 0.04f, 0, 1, false), darkMask, 0,
                 "Auto polarity without analysis defaults to dark text");
    // Both flag values must actually select their branch, rather than auto
    // silently behaving as one fixed polarity for every input.
    if (runner.Run(light, 0.3f, 0.04f, 0, 0) == lightMask ||
        runner.Run(dark, 0.3f, 0.04f, 0, 1) == darkMask) {
        throw std::runtime_error("Auto polarity ignored a changed analysis flag");
    }
    for (float strokeLevel : {0.75f, 0.9f, 1.0f}) {
        const auto chalk = MakeLightStrokes(strokeLevel);
        RequireEqual(runner.Run(chalk, 0.28f, 0.06f, 2, 0), expected, 0,
                     "Chalk strokes at normal settings");
        RequireEqual(runner.Run(Complement(chalk), 0.28f, 0.06f, 1, 1), expected, 0,
                     "Complementary dark strokes at normal settings");
    }
}

void ComplementaryInputsHaveTheSameInkMask(MaskRunner& runner)
{
    std::vector<float> light(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            light[y * kWidth + x] = static_cast<float>(1 + (x * 5 + y * 3) % 15) / 16.0f;
        }
    }
    const auto dark = Complement(light);
    for (float strength : {0.1f, 0.3f, 0.5f}) {
        for (float softness : {0.002f, 0.04f, 0.15f}) {
            const auto lightMask = runner.Run(light, strength, softness, 2, 0);
            const auto darkMask = runner.Run(dark, strength, softness, 1, 1);
            // Independent fixture moment rounding can differ by one mask byte.
            RequireEqual(lightMask, darkMask, 1, "Complementary-input mask symmetry");
            if (softness == 0.15f && std::none_of(lightMask.begin(), lightMask.end(),
                    [](unsigned char value) { return value > 0 && value < 255; })) {
                throw std::runtime_error("Symmetry fixture did not exercise soft mask edges");
            }
        }
    }
}

void OpaqueAlphaPreservesRgbAndGuards()
{
    constexpr size_t guardBytes = 64;
    const size_t pitch = (kWidth + 9) * sizeof(uchar4);
    const size_t bytes = guardBytes * 2 + pitch * (kHeight + 1);
    DeviceBytes image(bytes);
    for (int frame = 0; frame < 2; ++frame) {
        std::vector<unsigned char> expected(bytes, kGuard);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const size_t offset = guardBytes + y * pitch + x * sizeof(uchar4);
                expected[offset] = static_cast<unsigned char>((x * 17 + y + frame * 31) % 256);
                expected[offset + 1] = static_cast<unsigned char>((x + y * 29 + frame * 47) % 256);
                expected[offset + 2] = static_cast<unsigned char>((x * 11 + y * 7 + frame * 59) % 256);
                // Includes zero and stale alpha across repeated transfer-like writes.
                expected[offset + 3] = static_cast<unsigned char>((x * 63 + y * 17 + frame) % 255);
            }
        }
        CheckCuda(cudaMemcpy(image.data, expected.data(), bytes, cudaMemcpyHostToDevice), "upload alpha fixture");
        okuflow::LaunchOpaqueAlphaLinear(reinterpret_cast<uchar4*>(image.data + guardBytes),
                                         pitch, kWidth, kHeight, nullptr);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                expected[guardBytes + y * pitch + x * sizeof(uchar4) + 3] = 255;
        std::vector<unsigned char> actual(bytes);
        CheckCuda(cudaMemcpy(actual.data(), image.data, bytes, cudaMemcpyDeviceToHost), "read opaque alpha fixture");
        if (actual != expected) throw std::runtime_error("Opaque-alpha pass changed RGB/guards or left stale alpha");
    }
}

void SoftEdgesSurviveHysteresis()
{
    constexpr int width = 257, height = 3, floatStride = width + 7, maskStride = width + 13;
    constexpr size_t guardBytes = 64;
    constexpr float hysteresis = 0.12f;
    const size_t floatBytes = floatStride * height * sizeof(float);
    const size_t maskBytes = guardBytes * 2 + maskStride * (height + 1);
    DeviceBytes luma(floatBytes), mean(floatBytes), squares(floatBytes), mask(maskBytes), history(maskBytes);
    auto* maskPixels = mask.data + guardBytes;
    auto* historyPixels = history.data + guardBytes;
    std::vector<float> input(floatStride * height, 0.0f);
    std::vector<float> means(input.size(), 0.6f), squareMeans(input.size(), 0.4f);
    CheckCuda(cudaMemcpy(mean.data, means.data(), floatBytes, cudaMemcpyHostToDevice), "upload ramp mean");
    CheckCuda(cudaMemcpy(squares.data, squareMeans.data(), floatBytes, cudaMemcpyHostToDevice), "upload ramp moments");
    auto read = [&](DeviceBytes& buffer) {
        std::vector<unsigned char> result(maskBytes);
        CheckCuda(cudaMemcpy(result.data(), buffer.data, maskBytes, cudaMemcpyDeviceToHost), "read hysteresis mask");
        return result;
    };
    auto rawRamp = [&](float softness, float jitter) {
        // Fixed valid local moments isolate edge coverage from spatial filtering.
        // This ramp straddles the threshold and includes confident ink/background.
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                input[y * floatStride + x] = std::clamp(0.492f + (x - width / 2) * 0.004f + jitter, 0.0f, 1.0f);
        CheckCuda(cudaMemcpy(luma.data, input.data(), floatBytes, cudaMemcpyHostToDevice), "upload luma ramp");
        okuflow::LaunchSauvolaMask(maskPixels, maskStride, reinterpret_cast<float*>(luma.data),
            reinterpret_cast<float*>(mean.data), reinterpret_cast<float*>(squares.data),
            floatStride * sizeof(float), width, height, 0.3f, softness, 1, nullptr, nullptr);
        return read(mask);
    };
    auto stabilize = [&](bool valid) {
        okuflow::LaunchTextMaskHysteresis(maskPixels, maskStride, historyPixels, width, height,
                                          hysteresis, valid, nullptr);
        return read(mask);
    };
    int transitionWidth[2]{};
    int softnessIndex = 0;
    for (float softness : {0.02f, 0.25f}) {
        CheckCuda(cudaMemset(mask.data, kGuard, maskBytes), "initialize soft-mask guards");
        CheckCuda(cudaMemset(history.data, kGuard, maskBytes), "initialize soft-history guards");
        const auto initial = rawRamp(softness, -0.001f);
        auto previous = stabilize(false);
        if (previous != initial || read(history) != initial)
            throw std::runtime_error("First hysteresis frame quantized soft coverage or changed guards");
        for (int x = 0; x < width; ++x) {
            const auto value = previous[guardBytes + x];
            transitionWidth[softnessIndex] += value > 0 && value < 255;
        }
        const size_t center = guardBytes + width / 2;
        bool sawRawJitterToggle = false;
        for (float jitter : {0.001f, -0.001f, 0.001f}) {
            const auto raw = rawRamp(softness, jitter);
            sawRawJitterToggle |= (raw[center] >= 128) != (initial[center] >= 128);
            const auto stable = stabilize(true);
            if (stable[center] != initial[center])
                throw std::runtime_error("Uncertain edge lost its previous soft value under jitter");
            for (size_t index = 0; index < stable.size(); ++index) {
                const size_t relative = index >= guardBytes ? index - guardBytes : maskBytes;
                const bool visible = relative < static_cast<size_t>(maskStride * height) &&
                                     relative % maskStride < width;
                if (!visible) {
                    if (stable[index] != kGuard) throw std::runtime_error("Hysteresis wrote outside visible mask");
                    continue;
                }
                const bool uncertain = std::abs(raw[index] / 255.0f - 0.5f) < hysteresis;
                const unsigned char expected = uncertain ? previous[index] : raw[index];
                if (stable[index] != expected)
                    throw std::runtime_error("Hysteresis failed soft hold/accept contract");
                if ((stable[index] >= 128) != (previous[index] >= 128))
                    throw std::runtime_error("Small luma jitter toggled a stabilized classification");
            }
            if (read(history) != stable) throw std::runtime_error("History did not retain stable soft coverage/guards");
            previous = stable;
        }
        if (!sawRawJitterToggle) throw std::runtime_error("Ramp did not exercise threshold-crossing jitter");
        ++softnessIndex;
    }
    if (transitionWidth[0] <= 0 || transitionWidth[1] <= transitionWidth[0] * 3)
        throw std::runtime_error("Edge softness did not widen the stabilized intermediate-value transition");

    // Existing hard masks still seed binary history, hold uncertain samples,
    // and accept confident classification changes without rounding drift.
    std::vector<unsigned char> binary(maskBytes, kGuard);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            binary[guardBytes + y * maskStride + x] = x % 2 ? 255 : 0;
    CheckCuda(cudaMemset(history.data, kGuard, maskBytes), "initialize binary history");
    CheckCuda(cudaMemcpy(mask.data, binary.data(), maskBytes, cudaMemcpyHostToDevice), "upload hard mask");
    if (stabilize(false) != binary) throw std::runtime_error("Binary initial mask changed");
    auto noisy = binary;
    noisy[guardBytes] = 120;
    noisy[guardBytes + 1] = 135;
    noisy[guardBytes + 2] = 255;
    noisy[guardBytes + 3] = 0;
    auto expected = binary;
    expected[guardBytes + 2] = 255;
    expected[guardBytes + 3] = 0;
    CheckCuda(cudaMemcpy(mask.data, noisy.data(), maskBytes, cudaMemcpyHostToDevice), "upload binary-mask jitter");
    if (stabilize(true) != expected || read(history) != expected)
        throw std::runtime_error("Hard-mask hysteresis compatibility changed");
}

void TextCompositeSupportsExactAliasing()
{
    constexpr size_t guardBytes = 64;
    for (const auto extent : {std::pair<int, int>{37, 23}, {3, 5}, {1, 1}}) {
        const int width = extent.first, height = extent.second;
        const size_t sourcePitch = (width + 11) * sizeof(uchar4);
        const size_t destinationPitch = (width + 7) * sizeof(uchar4);
        const size_t maskPitch = width + 17;
        const size_t sourceBytes = guardBytes * 2 + sourcePitch * (height + 1);
        const size_t destinationBytes = guardBytes * 2 + destinationPitch * (height + 1);
        const size_t maskBytes = guardBytes * 2 + maskPitch * (height + 1);
        std::vector<unsigned char> input(sourceBytes, kGuard), coverage(maskBytes, kGuard);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t offset = guardBytes + y * sourcePitch + x * sizeof(uchar4);
                input[offset] = static_cast<unsigned char>((x * 13 + y * 17) % 256);
                input[offset + 1] = static_cast<unsigned char>((x * 29 + y * 5) % 256);
                input[offset + 2] = static_cast<unsigned char>((x * 7 + y * 31) % 256);
                input[offset + 3] = static_cast<unsigned char>((x + y) % 2 ? 217 : 0);
                constexpr unsigned char samples[]{0, 1, 64, 127, 128, 254, 255};
                coverage[guardBytes + y * maskPitch + x] = samples[(x + y) % 7];
            }
        }
        DeviceBytes source(sourceBytes), inplace(sourceBytes), output(destinationBytes), mask(maskBytes), analysis(sizeof(int4));
        CheckCuda(cudaMemcpy(source.data, input.data(), sourceBytes, cudaMemcpyHostToDevice), "upload composite source");
        CheckCuda(cudaMemcpy(mask.data, coverage.data(), maskBytes, cudaMemcpyHostToDevice), "upload composite mask");
        for (int mode : {0, 1, 2}) {
            for (int scene : {-1, 0, 1, 2}) {
                const int4 state = make_int4(0, std::max(scene, 0), 0, 0);
                CheckCuda(cudaMemcpy(analysis.data, &state, sizeof(state), cudaMemcpyHostToDevice), "upload composite analysis");
                CheckCuda(cudaMemcpy(inplace.data, input.data(), sourceBytes, cudaMemcpyHostToDevice), "reset in-place source");
                CheckCuda(cudaMemset(output.data, kGuard, destinationBytes), "initialize composite output guards");
                const auto* statePtr = scene < 0 ? nullptr : reinterpret_cast<const int4*>(analysis.data);
                const bool replacement = mode == 1 || (mode == 2 && scene > 0);
                const bool mixed = mode == 2 && !replacement;
                // Null mask proves both mixed branches return before mask access;
                // null source proves replacement reads only the coverage plane.
                const auto* maskPtr = mixed ? nullptr : mask.data + guardBytes;
                const auto* sourcePtr = replacement ? nullptr : reinterpret_cast<const uchar4*>(source.data + guardBytes);
                okuflow::LaunchTextMaskComposite(reinterpret_cast<uchar4*>(output.data + guardBytes), destinationPitch,
                    sourcePtr, sourcePitch, maskPtr, maskPitch, width, height,
                    0x11224466u, 0x7788aaccu, mode, statePtr, nullptr);
                auto* inplacePtr = reinterpret_cast<uchar4*>(inplace.data + guardBytes);
                okuflow::LaunchTextMaskComposite(inplacePtr, sourcePitch, inplacePtr, sourcePitch,
                    maskPtr, maskPitch, width, height, 0x11224466u, 0x7788aaccu, mode, statePtr, nullptr);
                std::vector<unsigned char> actual(destinationBytes), aliased(sourceBytes), untouched(sourceBytes);
                CheckCuda(cudaMemcpy(actual.data(), output.data, destinationBytes, cudaMemcpyDeviceToHost), "read composite output");
                CheckCuda(cudaMemcpy(aliased.data(), inplace.data, sourceBytes, cudaMemcpyDeviceToHost), "read aliased composite");
                CheckCuda(cudaMemcpy(untouched.data(), source.data, sourceBytes, cudaMemcpyDeviceToHost), "read untouched composite source");
                if (untouched != input) throw std::runtime_error("Out-of-place composite modified its source");
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const size_t srcOffset = guardBytes + y * sourcePitch + x * sizeof(uchar4);
                        const size_t dstOffset = guardBytes + y * destinationPitch + x * sizeof(uchar4);
                        for (int channel = 0; channel < 4; ++channel) {
                            if (actual[dstOffset + channel] != aliased[srcOffset + channel])
                                throw std::runtime_error("In-place/out-of-place composite differed bytewise");
                            if (mixed && actual[dstOffset + channel] != input[srcOffset + channel])
                                throw std::runtime_error("Mixed-content composite changed scene bytes");
                        }
                        if (replacement && actual[dstOffset + 3] != 255)
                            throw std::runtime_error("Document replacement kept source/color alpha");
                        if (mode == 0 && actual[dstOffset + 3] != input[srcOffset + 3])
                            throw std::runtime_error("Legacy blend changed source alpha");
                    }
                }
                auto requireGuards = [&](const std::vector<unsigned char>& pixels, size_t pitch) {
                    for (size_t index = 0; index < pixels.size(); ++index) {
                        const size_t relative = index >= guardBytes ? index - guardBytes : pixels.size();
                        const bool visible = relative < pitch * height && relative % pitch < width * sizeof(uchar4);
                        if (!visible && pixels[index] != kGuard)
                            throw std::runtime_error("Composite overwrote prefix, row padding, or tail guards");
                    }
                };
                requireGuards(actual, destinationPitch);
                requireGuards(aliased, sourcePitch);
            }
        }
        std::vector<unsigned char> maskAfter(maskBytes);
        CheckCuda(cudaMemcpy(maskAfter.data(), mask.data, maskBytes, cudaMemcpyDeviceToHost), "read untouched coverage");
        if (maskAfter != coverage) throw std::runtime_error("Composite modified mask/history workspace");
    }
}

void AutomaticDocumentSharpenSkipsOnlyReplacedScenes()
{
    constexpr size_t guardBytes = 64;
    for (const auto extent : {std::pair<int, int>{37, 23}, {3, 5}}) {
        const int width = extent.first, height = extent.second;
        const size_t pitch = (width + 9) * sizeof(uchar4);
        const size_t maskPitch = width + 13;
        const size_t bytes = guardBytes * 2 + pitch * (height + 1);
        const size_t maskBytes = guardBytes * 2 + maskPitch * (height + 1);
        std::vector<unsigned char> input(bytes, kGuard), coverage(maskBytes, kGuard);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t offset = guardBytes + y * pitch + x * sizeof(uchar4);
                input[offset] = static_cast<unsigned char>((x * 37 + y * 11) % 256);
                input[offset + 1] = static_cast<unsigned char>((x * 5 + y * 41) % 256);
                input[offset + 2] = static_cast<unsigned char>((x / 3 + y / 2) % 2 ? 230 : 20);
                input[offset + 3] = static_cast<unsigned char>((x + y) % 2 ? 217 : 0);
                coverage[guardBytes + y * maskPitch + x] =
                    static_cast<unsigned char>((x / 4 + y / 3) % 2 ? 255 : (x * 19) % 256);
            }
        }
        DeviceBytes source(bytes), mask(maskBytes), analysis(sizeof(int4));
        DeviceBytes fullDst(bytes), fullScratch(bytes), skipDst(bytes), skipScratch(bytes);
        CheckCuda(cudaMemcpy(source.data, input.data(), bytes, cudaMemcpyHostToDevice), "upload sharpen source");
        CheckCuda(cudaMemcpy(mask.data, coverage.data(), maskBytes, cudaMemcpyHostToDevice), "upload sharpen mask");
        const auto* sourcePtr = reinterpret_cast<const uchar4*>(source.data + guardBytes);
        const auto* maskPtr = mask.data + guardBytes;
        const auto* statePtr = reinterpret_cast<const int4*>(analysis.data);
        auto read = [&](DeviceBytes& buffer) {
            std::vector<unsigned char> result(bytes);
            CheckCuda(cudaMemcpy(result.data(), buffer.data, bytes, cudaMemcpyDeviceToHost), "read sharpen fixture");
            return result;
        };
        auto requireGuards = [&](const std::vector<unsigned char>& pixels) {
            for (size_t index = 0; index < pixels.size(); ++index) {
                const size_t relative = index >= guardBytes ? index - guardBytes : pixels.size();
                const bool visible = relative < pitch * height &&
                                     relative % pitch < width * sizeof(uchar4);
                if (!visible && pixels[index] != kGuard)
                    throw std::runtime_error("Smart sharpening overwrote prefix, row padding, or tail guards");
            }
        };
        const std::vector<unsigned char> untouched(bytes, kGuard);
        for (int scene : {0, 1, 2}) {
            for (bool selective : {false, true}) {
                const int4 state = make_int4(0, scene, 0, 0);
                CheckCuda(cudaMemcpy(analysis.data, &state, sizeof(state), cudaMemcpyHostToDevice), "upload sharpen scene");
                for (DeviceBytes* buffer : {&fullDst, &fullScratch, &skipDst, &skipScratch})
                    CheckCuda(cudaMemset(buffer->data, kGuard, bytes), "initialize sharpen guards");
                auto* fullPtr = reinterpret_cast<uchar4*>(fullDst.data + guardBytes);
                auto* skipPtr = reinterpret_cast<uchar4*>(skipDst.data + guardBytes);
                okuflow::LaunchSmartSharpenLinear(fullPtr, pitch,
                    reinterpret_cast<uchar4*>(fullScratch.data + guardBytes), pitch,
                    sourcePtr, pitch, maskPtr, maskPitch, width, height, 0.8f, selective, nullptr);
                okuflow::LaunchSmartSharpenLinear(skipPtr, pitch,
                    reinterpret_cast<uchar4*>(skipScratch.data + guardBytes), pitch,
                    sourcePtr, pitch, maskPtr, maskPitch, width, height, 0.8f, selective, nullptr, statePtr);
                CheckCuda(cudaDeviceSynchronize(), "smart sharpen synchronize");
                const auto fullOutput = read(fullDst), skipOutput = read(skipDst);
                requireGuards(fullOutput);
                requireGuards(skipOutput);
                requireGuards(read(fullScratch));
                requireGuards(read(skipScratch));
                if (scene == 0) {
                    // Mixed content keeps its sharpened RGB, so output and scratch are unchanged.
                    if (skipOutput != fullOutput || read(skipScratch) != read(fullScratch))
                        throw std::runtime_error("Mixed-scene sharpening changed with replacement analysis");
                    bool rgbChanged = false;
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            const size_t offset = guardBytes + y * pitch + x * sizeof(uchar4);
                            for (int channel = 0; channel < 3; ++channel)
                                rgbChanged |= fullOutput[offset + channel] != input[offset + channel];
                        }
                    }
                    if (!rgbChanged) throw std::runtime_error("Mixed-scene sharpening became an RGB no-op");
                } else if (skipOutput != untouched || read(skipScratch) != untouched) {
                    throw std::runtime_error("Document sharpening wrote pixels that the composite replaces");
                }
                // Production follows automatic sharpening with an in-place mode-2 composite.
                okuflow::LaunchTextMaskComposite(fullPtr, pitch, fullPtr, pitch, maskPtr, maskPitch,
                    width, height, 0xff000000u, 0xffffffffu, 2, statePtr, nullptr);
                okuflow::LaunchTextMaskComposite(skipPtr, pitch, skipPtr, pitch, maskPtr, maskPitch,
                    width, height, 0xff000000u, 0xffffffffu, 2, statePtr, nullptr);
                CheckCuda(cudaDeviceSynchronize(), "sharpen-composite synchronize");
                if (read(skipDst) != read(fullDst))
                    throw std::runtime_error("Skipped document sharpening changed the composited frame or guards");
            }
        }
        if (read(source) != input) throw std::runtime_error("Smart sharpening modified its source");
        std::vector<unsigned char> maskAfter(maskBytes);
        CheckCuda(cudaMemcpy(maskAfter.data(), mask.data, maskBytes, cudaMemcpyDeviceToHost), "read untouched sharpen mask");
        if (maskAfter != coverage) throw std::runtime_error("Smart sharpening modified its mask");
    }
}

void ClaheBordersAndLocalMeanParity()
{
    for (bool chroma : {false, true}) {
    for (const auto extent : {std::pair<int, int>{1280, 720}, {127, 93}, {3, 5}}) {
        const int width = extent.first, height = extent.second;
        const int stride = width + 7;
        const size_t colorPitch = stride * sizeof(uchar4);
        const size_t floatPitch = stride * sizeof(float);
        const size_t bytes = colorPitch * (height + 1);
        std::vector<uchar4> input(stride * (height + 1), make_uchar4(kGuard, kGuard, kGuard, kGuard));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const unsigned char v = static_cast<unsigned char>(20 +
                    ((x * 3 + y * 5 + (x * 8 / width) * 23) % 180));
                input[y * stride + x] = make_uchar4(v, v, static_cast<unsigned char>(v + (chroma ? 1 : 0)), 217);
            }
        }
        DeviceBytes src(bytes), dst(bytes), hist(64 * 256 * sizeof(unsigned int)), maps(64 * 256 * sizeof(float));
        CheckCuda(cudaMemcpy(src.data, input.data(), bytes, cudaMemcpyHostToDevice), "upload CLAHE fixture");
        CheckCuda(cudaMemset(dst.data, kGuard, bytes), "initialize CLAHE guards");
        okuflow::LaunchClaheLinear(reinterpret_cast<uchar4*>(dst.data), colorPitch,
            reinterpret_cast<const uchar4*>(src.data), colorPitch,
            reinterpret_cast<unsigned int*>(hist.data), reinterpret_cast<float*>(maps.data),
            width, height, 2.0f, nullptr);
        std::vector<float> tileMaps(64 * 256);
        std::vector<uchar4> output(input.size());
        CheckCuda(cudaMemcpy(tileMaps.data(), maps.data, tileMaps.size() * sizeof(float), cudaMemcpyDeviceToHost),
                  "read actual CLAHE tile maps");
        CheckCuda(cudaMemcpy(output.data(), dst.data, bytes, cudaMemcpyDeviceToHost), "read CLAHE result");
        DeviceBytes luma(bytes), horizontal(bytes), mean(bytes), sqHorizontal(bytes), sqMean(bytes);
        DeviceBytes meanLuma(bytes), meanHorizontal(bytes), meanOnly(bytes);
        // Grayscale sits on exact histogram-bin boundaries. Use the device
        // luma plane to retain CUDA rounding/FMA for bin selection, while the
        // interpolation below remains an independent double-precision oracle.
        // The chroma fixture sits 0.299 away from every bin boundary and also
        // crosschecks luma and bin selection against host arithmetic.
        okuflow::LaunchTextLocalMean(reinterpret_cast<float*>(meanLuma.data), floatPitch,
            reinterpret_cast<float*>(meanHorizontal.data), reinterpret_cast<float*>(meanOnly.data),
            reinterpret_cast<const uchar4*>(src.data), colorPitch, width, height, 1, nullptr);
        std::vector<float> deviceLuma(stride * (height + 1));
        CheckCuda(cudaMemcpy(deviceLuma.data(), meanLuma.data, bytes, cudaMemcpyDeviceToHost),
                  "read CUDA luma for interpolation oracle");
        // Reference interpolates continuous coordinates clamped to the tile
        // centers, including odd extents and images smaller than the grid.
        for (int y = 0; y <= height; ++y) {
            for (int x = 0; x < stride; ++x) {
                const auto actual = output[y * stride + x];
                if (y == height || x >= width) {
                    if (actual.x != kGuard || actual.y != kGuard || actual.z != kGuard || actual.w != kGuard)
                        throw std::runtime_error("CLAHE wrote padding");
                    continue;
                }
                const double gx = std::clamp((x + 0.5) * 8.0 / width - 0.5, 0.0, 7.0);
                const double gy = std::clamp((y + 0.5) * 8.0 / height - 0.5, 0.0, 7.0);
                const int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
                const int x1 = std::min(x0 + 1, 7), y1 = std::min(y0 + 1, 7);
                const auto pixel = input[y * stride + x];
                const float hostLuma = (0.114f * pixel.x + 0.587f * pixel.y + 0.299f * pixel.z) / 255.0f;
                const float gpuLuma = deviceLuma[y * stride + x];
                if (chroma && (std::abs(hostLuma - gpuLuma) > 1.0e-6f ||
                    static_cast<int>(hostLuma * 255.0f) != static_cast<int>(gpuLuma * 255.0f)))
                    throw std::runtime_error("Stable-chroma device/host luma mismatch");
                const float lum = chroma ? hostLuma : gpuLuma;
                const int bin = std::min(static_cast<int>(lum * 255.0f), 255);
                const double tx = gx - x0, ty = gy - y0;
                const double mapped = (1 - ty) * ((1 - tx) * tileMaps[(y0 * 8 + x0) * 256 + bin] +
                    tx * tileMaps[(y0 * 8 + x1) * 256 + bin]) +
                    ty * ((1 - tx) * tileMaps[(y1 * 8 + x0) * 256 + bin] +
                    tx * tileMaps[(y1 * 8 + x1) * 256 + bin]);
                const double delta = (mapped - lum) * 255.0;
                const int expectedB = static_cast<int>(std::clamp(pixel.x + delta, 0.0, 255.0));
                const int expectedG = static_cast<int>(std::clamp(pixel.y + delta, 0.0, 255.0));
                const int expectedR = static_cast<int>(std::clamp(pixel.z + delta, 0.0, 255.0));
                if (std::abs(static_cast<int>(actual.x) - expectedB) > 1 ||
                    std::abs(static_cast<int>(actual.y) - expectedG) > 1 ||
                    std::abs(static_cast<int>(actual.z) - expectedR) > 1 || actual.w != pixel.w) {
                    std::cerr << "CLAHE mismatch extent=" << width << 'x' << height
                              << " chroma=" << chroma << " pixel=" << x << ',' << y << " bin=" << bin
                              << " luma=" << lum << " mapped=" << mapped
                              << " expected BGR=" << expectedB << ',' << expectedG << ',' << expectedR
                              << " actual BGR=" << static_cast<int>(actual.x) << ','
                              << static_cast<int>(actual.y) << ',' << static_cast<int>(actual.z) << '\n';
                    for (int adjacent : {std::max(0, bin - 1), std::min(255, bin + 1)}) {
                        const double adjacentMapped = (1 - ty) * ((1 - tx) * tileMaps[(y0 * 8 + x0) * 256 + adjacent] +
                            tx * tileMaps[(y0 * 8 + x1) * 256 + adjacent]) +
                            ty * ((1 - tx) * tileMaps[(y1 * 8 + x0) * 256 + adjacent] +
                            tx * tileMaps[(y1 * 8 + x1) * 256 + adjacent]);
                        const int adjacentExpected = static_cast<int>(std::clamp(pixel.x +
                            (adjacentMapped - lum) * 255.0, 0.0, 255.0));
                        std::cerr << " adjacent bin=" << adjacent << " expected=" << adjacentExpected << '\n';
                    }
                    throw std::runtime_error("CLAHE tile-center interpolation mismatch");
                }
            }
        }
        for (int radius : {1, 8, 128}) {
            CheckCuda(cudaMemset(mean.data, kGuard, bytes), "initialize full mean guards");
            CheckCuda(cudaMemset(meanOnly.data, kGuard, bytes), "initialize mean-only guards");
            okuflow::LaunchTextLocalStatistics(reinterpret_cast<float*>(luma.data), floatPitch,
                reinterpret_cast<float*>(horizontal.data), reinterpret_cast<float*>(mean.data),
                reinterpret_cast<float*>(sqHorizontal.data), reinterpret_cast<float*>(sqMean.data),
                reinterpret_cast<const uchar4*>(src.data), colorPitch, width, height, radius, nullptr);
            okuflow::LaunchTextLocalMean(reinterpret_cast<float*>(meanLuma.data), floatPitch,
                reinterpret_cast<float*>(meanHorizontal.data), reinterpret_cast<float*>(meanOnly.data),
                reinterpret_cast<const uchar4*>(src.data), colorPitch, width, height, radius, nullptr);
            std::vector<unsigned char> full(bytes), only(bytes);
            CheckCuda(cudaMemcpy(full.data(), mean.data, bytes, cudaMemcpyDeviceToHost), "read full mean");
            CheckCuda(cudaMemcpy(only.data(), meanOnly.data, bytes, cudaMemcpyDeviceToHost), "read mean-only");
            if (full != only) throw std::runtime_error("Mean-only pass changed mean or padding");
        }
    }
    }
}

} // namespace

int main()
{
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::cout << "No CUDA device; Text Clarity CUDA test skipped.\n";
        return 77;
    }
    try {
        OpaqueAlphaPreservesRgbAndGuards();
        SoftEdgesSurviveHysteresis();
        TextCompositeSupportsExactAliasing();
        AutomaticDocumentSharpenSkipsOnlyReplacedScenes();
        ClaheBordersAndLocalMeanParity();
        MaskRunner runner;
        UniformBackgroundIsNotInk(runner);
        StrokesAndAutoPolarity(runner);
        ComplementaryInputsHaveTheSameInkMask(runner);
        std::cout << "Text Clarity Sauvola background, stroke, polarity, and symmetry checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
