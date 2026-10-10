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

} // namespace

int main()
{
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::cout << "No CUDA device; Text Clarity CUDA test skipped.\n";
        return 77;
    }
    try {
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
