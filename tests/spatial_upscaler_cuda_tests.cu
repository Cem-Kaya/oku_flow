#include "okuflow/cuda/cuda_kernels.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

bool CheckCuda(cudaError_t status, const char* operation)
{
    if (status == cudaSuccess) {
        return true;
    }
    std::cerr << operation << ": " << cudaGetErrorString(status) << '\n';
    return false;
}

std::vector<uchar4> MakePattern(int width, int height)
{
    std::vector<uchar4> pixels(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int checker = ((x / 3) ^ (y / 3)) & 1;
            const int diagonal = x * 2 > y * 3 ? 73 : 0;
            pixels[static_cast<std::size_t>(y) * width + x] = make_uchar4(
                static_cast<unsigned char>((x * 29 + y * 11 + checker * 97) & 255),
                static_cast<unsigned char>((x * 7 + y * 37 + diagonal) & 255),
                static_cast<unsigned char>((x * 19 + y * 13 + checker * 41 + diagonal) & 255),
                255);
        }
    }
    return pixels;
}

bool CopyToDevice(uchar4* destination, std::size_t destinationPitch,
                  const std::vector<uchar4>& source, int width, int height)
{
    return CheckCuda(
        cudaMemcpy2D(destination, destinationPitch, source.data(),
                     static_cast<std::size_t>(width) * sizeof(uchar4),
                     static_cast<std::size_t>(width) * sizeof(uchar4), height,
                     cudaMemcpyHostToDevice),
        "cudaMemcpy2D host to device");
}

bool CopyToHost(std::vector<uchar4>& destination,
                const uchar4* source, std::size_t sourcePitch,
                int width, int height)
{
    return CheckCuda(
        cudaMemcpy2D(destination.data(),
                     static_cast<std::size_t>(width) * sizeof(uchar4),
                     source, sourcePitch,
                     static_cast<std::size_t>(width) * sizeof(uchar4), height,
                     cudaMemcpyDeviceToHost),
        "cudaMemcpy2D device to host");
}

std::size_t DifferentPixels(const std::vector<uchar4>& first,
                            const std::vector<uchar4>& second)
{
    std::size_t different = 0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        const uchar4 a = first[index];
        const uchar4 b = second[index];
        different += a.x != b.x || a.y != b.y || a.z != b.z;
    }
    return different;
}

bool CheckOpaque(const std::vector<uchar4>& pixels, const char* label)
{
    const auto transparent = std::find_if(
        pixels.begin(), pixels.end(), [](const uchar4& value) {
            return value.w != 255;
        });
    if (transparent == pixels.end()) {
        return true;
    }
    std::cerr << label << " produced a non-opaque BGRA pixel\n";
    return false;
}

bool CheckConstant(const std::vector<uchar4>& pixels, uchar4 expected,
                   int tolerance, const char* label)
{
    for (const uchar4 value : pixels) {
        if (std::abs(static_cast<int>(value.x) - expected.x) > tolerance ||
            std::abs(static_cast<int>(value.y) - expected.y) > tolerance ||
            std::abs(static_cast<int>(value.z) - expected.z) > tolerance ||
            value.w != 255) {
            std::cerr << label << " did not preserve a constant image: got ("
                      << static_cast<int>(value.x) << ", "
                      << static_cast<int>(value.y) << ", "
                      << static_cast<int>(value.z) << ")\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    int deviceCount = 0;
    if (cudaGetDeviceCount(&deviceCount) != cudaSuccess || deviceCount == 0) {
        std::cout << "No CUDA device; spatial upscaler CUDA test skipped.\n";
        return 77;
    }

    constexpr int sourceWidth = 32;
    constexpr int sourceHeight = 24;
    constexpr int outputWidth = sourceWidth * 2;
    constexpr int outputHeight = sourceHeight * 2;

    uchar4* deviceSource = nullptr;
    uchar4* deviceOutput = nullptr;
    uchar4* deviceScratch = nullptr;
    std::size_t sourcePitch = 0;
    std::size_t outputPitch = 0;
    std::size_t scratchPitch = 0;
    bool ok =
        CheckCuda(cudaMallocPitch(reinterpret_cast<void**>(&deviceSource),
                                  &sourcePitch,
                                  sourceWidth * sizeof(uchar4), sourceHeight),
                  "cudaMallocPitch source") &&
        CheckCuda(cudaMallocPitch(reinterpret_cast<void**>(&deviceOutput),
                                  &outputPitch,
                                  outputWidth * sizeof(uchar4), outputHeight),
                  "cudaMallocPitch output") &&
        CheckCuda(cudaMallocPitch(reinterpret_cast<void**>(&deviceScratch),
                                  &scratchPitch,
                                  outputWidth * sizeof(uchar4), outputHeight),
                  "cudaMallocPitch scratch");
    if (!ok) {
        cudaFree(deviceSource);
        cudaFree(deviceOutput);
        cudaFree(deviceScratch);
        return 1;
    }

    const std::vector<uchar4> pattern = MakePattern(sourceWidth, sourceHeight);
    std::vector<uchar4> fsrSoft(static_cast<std::size_t>(outputWidth) * outputHeight);
    std::vector<uchar4> fsrSharp(fsrSoft.size());
    std::vector<uchar4> nisSoft(fsrSoft.size());
    std::vector<uchar4> nisSharp(fsrSoft.size());

    ok = CopyToDevice(deviceSource, sourcePitch, pattern,
                      sourceWidth, sourceHeight);
    if (ok) {
        try {
            okuflow::LaunchFsrEasuRcasLinear(
                deviceOutput, outputPitch, deviceScratch, scratchPitch,
                deviceSource, sourcePitch, sourceWidth, sourceHeight,
                outputWidth, outputHeight, 0.0f, nullptr);
            ok = CheckCuda(cudaDeviceSynchronize(), "FSR EASU + RCAS synchronize") &&
                 CopyToHost(fsrSoft, deviceOutput, outputPitch,
                            outputWidth, outputHeight);

            okuflow::LaunchFsrEasuRcasLinear(
                deviceOutput, outputPitch, deviceScratch, scratchPitch,
                deviceSource, sourcePitch, sourceWidth, sourceHeight,
                outputWidth, outputHeight, 1.0f, nullptr);
            ok = ok &&
                 CheckCuda(cudaDeviceSynchronize(), "FSR sharpen synchronize") &&
                 CopyToHost(fsrSharp, deviceOutput, outputPitch,
                            outputWidth, outputHeight);

            okuflow::LaunchNisLinear(
                deviceOutput, outputPitch, deviceSource, sourcePitch,
                sourceWidth, sourceHeight, outputWidth, outputHeight,
                0.0f, nullptr);
            ok = ok &&
                 CheckCuda(cudaDeviceSynchronize(), "NIS scaler synchronize") &&
                 CopyToHost(nisSoft, deviceOutput, outputPitch,
                            outputWidth, outputHeight);

            okuflow::LaunchNisLinear(
                deviceOutput, outputPitch, deviceSource, sourcePitch,
                sourceWidth, sourceHeight, outputWidth, outputHeight,
                1.0f, nullptr);
            ok = ok &&
                 CheckCuda(cudaDeviceSynchronize(), "NIS sharpen synchronize") &&
                 CopyToHost(nisSharp, deviceOutput, outputPitch,
                            outputWidth, outputHeight);
        } catch (const std::exception& error) {
            std::cerr << "Spatial upscaler launch failed: " << error.what() << '\n';
            ok = false;
        }
    }

    if (ok) {
        constexpr std::size_t minimumChangedPixels =
            static_cast<std::size_t>(outputWidth) * outputHeight / 20;
        const std::size_t fsrChanged = DifferentPixels(fsrSoft, fsrSharp);
        const std::size_t nisChanged = DifferentPixels(nisSoft, nisSharp);
        if (fsrChanged < minimumChangedPixels) {
            std::cerr << "FSR RCAS sharpness changed only " << fsrChanged
                      << " pixels\n";
            ok = false;
        }
        if (nisChanged < minimumChangedPixels) {
            std::cerr << "NIS adaptive sharpening changed only " << nisChanged
                      << " pixels\n";
            ok = false;
        }
        if (DifferentPixels(fsrSoft, nisSoft) < minimumChangedPixels) {
            std::cerr << "FSR and NIS unexpectedly produced equivalent output\n";
            ok = false;
        }
        ok = CheckOpaque(fsrSoft, "FSR") &&
             CheckOpaque(nisSoft, "NIS") && ok;
    }

    const uchar4 constant = make_uchar4(61, 127, 203, 255);
    std::vector<uchar4> constantSource(
        static_cast<std::size_t>(sourceWidth) * sourceHeight, constant);
    std::vector<uchar4> constantOutput(fsrSoft.size());
    if (ok) {
        ok = CopyToDevice(deviceSource, sourcePitch, constantSource,
                          sourceWidth, sourceHeight);
    }
    if (ok) {
        try {
            okuflow::LaunchFsrEasuRcasLinear(
                deviceOutput, outputPitch, deviceScratch, scratchPitch,
                deviceSource, sourcePitch, sourceWidth, sourceHeight,
                outputWidth, outputHeight, 1.0f, nullptr);
            ok = CheckCuda(cudaDeviceSynchronize(), "constant FSR synchronize") &&
                 CopyToHost(constantOutput, deviceOutput, outputPitch,
                            outputWidth, outputHeight) &&
                 CheckConstant(constantOutput, constant, 1, "FSR");

            okuflow::LaunchNisLinear(
                deviceOutput, outputPitch, deviceSource, sourcePitch,
                sourceWidth, sourceHeight, outputWidth, outputHeight,
                1.0f, nullptr);
            ok = ok &&
                 CheckCuda(cudaDeviceSynchronize(), "constant NIS synchronize") &&
                 CopyToHost(constantOutput, deviceOutput, outputPitch,
                            outputWidth, outputHeight) &&
                 CheckConstant(constantOutput, constant, 1, "NIS");
        } catch (const std::exception& error) {
            std::cerr << "Constant-image test failed: " << error.what() << '\n';
            ok = false;
        }
    }

    // The viewport cache passes an offset ROI while preserving the full-scene
    // pitch. Surround it with a contrasting color to catch whole-frame reads
    // and filters sampling outside the requested crop.
    if (ok) {
        constexpr int roiX = 7, roiY = 5, roiWidth = 16, roiHeight = 12;
        auto roiScene = std::vector<uchar4>(sourceWidth * sourceHeight,
                                           make_uchar4(255, 0, 255, 255));
        for (int y = roiY; y < roiY + roiHeight; ++y) {
            for (int x = roiX; x < roiX + roiWidth; ++x) {
                roiScene[static_cast<std::size_t>(y) * sourceWidth + x] = constant;
            }
        }
        ok = CopyToDevice(deviceSource, sourcePitch, roiScene,
                          sourceWidth, sourceHeight);
        const auto* roi = reinterpret_cast<const uchar4*>(
            reinterpret_cast<const unsigned char*>(deviceSource) + roiY * sourcePitch) + roiX;
        std::vector<uchar4> roiOutput(roiWidth * 2 * roiHeight * 2);
        try {
            okuflow::LaunchNisLinear(
                deviceOutput, outputPitch, roi, sourcePitch,
                roiWidth, roiHeight, roiWidth * 2, roiHeight * 2, 0.5f, nullptr);
            ok = ok && CheckCuda(cudaDeviceSynchronize(), "ROI NIS synchronize") &&
                 CopyToHost(roiOutput, deviceOutput, outputPitch,
                            roiWidth * 2, roiHeight * 2) &&
                 CheckConstant(roiOutput, constant, 1, "ROI NIS");
            okuflow::LaunchFsrEasuRcasLinear(
                deviceOutput, outputPitch, deviceScratch, scratchPitch,
                roi, sourcePitch, roiWidth, roiHeight,
                roiWidth * 2, roiHeight * 2, 0.5f, nullptr);
            ok = ok && CheckCuda(cudaDeviceSynchronize(), "ROI FSR synchronize") &&
                 CopyToHost(roiOutput, deviceOutput, outputPitch,
                            roiWidth * 2, roiHeight * 2) &&
                 CheckConstant(roiOutput, constant, 1, "ROI FSR");
            // Residual display magnification samples up to half a texel past
            // the valid output edge. The array guard must replicate that edge,
            // including its bottom-right corner, rather than expose old pixels.
            constexpr int cacheWidth = roiWidth * 2 + 2;
            constexpr int cacheHeight = roiHeight * 2 + 2;
            cudaArray_t cache = nullptr;
            const auto descriptor = cudaCreateChannelDesc<uchar4>();
            if (CheckCuda(cudaMallocArray(&cache, &descriptor, cacheWidth, cacheHeight),
                          "guard cache allocation")) {
                const uchar4 stale = make_uchar4(255, 0, 255, 255);
                std::vector<uchar4> cachePixels(cacheWidth * cacheHeight, stale);
                ok = CheckCuda(cudaMemcpy2DToArray(cache, 0, 0,
                    cachePixels.data(), cacheWidth * sizeof(uchar4),
                    cacheWidth * sizeof(uchar4), cacheHeight,
                    cudaMemcpyHostToDevice), "initialize stale cache") && ok;
                ok = CheckCuda(cudaMemcpy2DToArray(cache, 0, 0,
                    deviceOutput, outputPitch, roiWidth * 2 * sizeof(uchar4),
                    roiHeight * 2, cudaMemcpyDeviceToDevice), "copy valid cache") && ok;
                okuflow::PadSpatialCacheBorder(cache, deviceOutput, outputPitch,
                    roiWidth * 2, roiHeight * 2, cacheWidth, cacheHeight, nullptr);
                ok = CheckCuda(cudaDeviceSynchronize(), "guard synchronize") && ok;
                ok = CheckCuda(cudaMemcpy2DFromArray(cachePixels.data(),
                    cacheWidth * sizeof(uchar4), cache, 0, 0,
                    cacheWidth * sizeof(uchar4), cacheHeight,
                    cudaMemcpyDeviceToHost), "read guarded cache") && ok;
                std::vector<uchar4> guarded;
                for (int y = 0; y <= roiHeight * 2; ++y) {
                    for (int x = 0; x <= roiWidth * 2; ++x) {
                        guarded.push_back(cachePixels[y * cacheWidth + x]);
                    }
                }
                ok = CheckConstant(guarded, constant, 1, "linear sampler guard") && ok;
                // The second guard-width row/column remains untouched: the
                // helper writes exactly the bilinear footprint, not the array.
                ok = CheckConstant({cachePixels.back()}, stale, 0,
                                   "outside sampler footprint") && ok;
                cudaFreeArray(cache);
            } else {
                ok = false;
            }
        } catch (const std::exception& error) {
            std::cerr << "ROI spatial upscale failed: " << error.what() << '\n';
            ok = false;
        }
    }

    bool rejectedUnsupportedScale = false;
    try {
        okuflow::LaunchNisLinear(
            deviceOutput, outputPitch, deviceSource, sourcePitch,
            sourceWidth, sourceHeight, sourceWidth * 3, sourceHeight * 3,
            0.5f, nullptr);
    } catch (const std::invalid_argument&) {
        rejectedUnsupportedScale = true;
    }
    if (!rejectedUnsupportedScale) {
        std::cerr << "NIS did not reject enlargement above its 2x limit\n";
        ok = false;
    }

    cudaFree(deviceSource);
    cudaFree(deviceOutput);
    cudaFree(deviceScratch);

    if (ok) {
        std::cout << "FSR 1.0 EASU/RCAS and NIS CUDA regression checks passed.\n";
        return 0;
    }
    return 1;
}
