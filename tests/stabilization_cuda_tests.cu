#include "openzoom/cuda/cuda_kernels.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct StabilizationTraceRow {
    int frame{};
    float inputDx{};
    float inputDy{};
    float correctionDx{};
    float correctionDy{};
    bool accepted{};
};

bool CheckCuda(cudaError_t status, const char* operation)
{
    if (status == cudaSuccess) {
        return true;
    }
    std::cerr << operation << ": " << cudaGetErrorString(status) << '\n';
    return false;
}

bool Near(float actual, float expected, float tolerance, const char* label)
{
    if (std::abs(actual - expected) <= tolerance) {
        return true;
    }
    std::cerr << label << ": expected " << expected << ", got " << actual
              << '\n';
    return false;
}

float SampleImage(const std::vector<float>& image, int width, int height,
                  float x, float y)
{
    x = std::clamp(x, 0.0f, static_cast<float>(width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float fx = x - x0;
    const float fy = y - y0;
    const float top = image[static_cast<size_t>(y0) * width + x0] * (1.0f - fx) +
                      image[static_cast<size_t>(y0) * width + x1] * fx;
    const float bottom =
        image[static_cast<size_t>(y1) * width + x0] * (1.0f - fx) +
        image[static_cast<size_t>(y1) * width + x1] * fx;
    return top * (1.0f - fy) + bottom * fy;
}

std::vector<float> MakeVirtualClampFrame(const std::vector<float>& reference,
                                         int width, int height, float dx, float dy,
                                         float brightness, bool occluded)
{
    std::vector<float> frame(reference.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float value = SampleImage(reference, width, height,
                                      static_cast<float>(x) - dx,
                                      static_cast<float>(y) - dy) +
                          brightness;
            if (occluded && x >= width / 3 && x < width * 2 / 3 &&
                y >= height / 5 && y < height * 4 / 5) {
                value = 65.0f + static_cast<float>((x + y) & 7);
            }
            frame[static_cast<size_t>(y) * width + x] =
                std::clamp(value, 0.0f, 255.0f);
        }
    }
    return frame;
}

std::vector<float> BlurImage(const std::vector<float>& source,
                             int width, int height)
{
    std::vector<float> blurred(source.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum = 0.0f;
            int samples = 0;
            for (int oy = -2; oy <= 2; ++oy) {
                for (int ox = -2; ox <= 2; ++ox) {
                    const int px = std::clamp(x + ox, 0, width - 1);
                    const int py = std::clamp(y + oy, 0, height - 1);
                    sum += source[static_cast<size_t>(py) * width + px];
                    ++samples;
                }
            }
            blurred[static_cast<size_t>(y) * width + x] =
                sum / static_cast<float>(samples);
        }
    }
    return blurred;
}

bool WriteStabilizationTrace(
    const std::string& path,
    const std::vector<StabilizationTraceRow>& rows)
{
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "Could not open stabilization trace output: " << path
                  << '\n';
        return false;
    }
    output << "frame,input_dx_pixels,input_dy_pixels,"
              "openzoom_correction_dx_pixels,openzoom_correction_dy_pixels,"
              "accepted\n";
    for (const StabilizationTraceRow& row : rows) {
        output << row.frame << ',' << row.inputDx << ',' << row.inputDy << ','
               << row.correctionDx << ',' << row.correctionDy << ','
               << (row.accepted ? 1 : 0) << '\n';
    }
    return output.good();
}

} // namespace

int main(int argc, char** argv)
{
    std::string traceOutputPath;
    for (int argument = 1; argument < argc; ++argument) {
        const std::string value = argv[argument];
        if (value == "--export-corrections" && argument + 1 < argc) {
            traceOutputPath = argv[++argument];
        } else {
            std::cerr << "Usage: stabilization_cuda_tests "
                         "[--export-corrections OUTPUT.csv]\n";
            return 2;
        }
    }

    int deviceCount = 0;
    if (cudaGetDeviceCount(&deviceCount) != cudaSuccess || deviceCount == 0) {
        std::cout << "No CUDA device; stabilization CUDA test skipped.\n";
        return 77;
    }

    constexpr int width = 1280;
    constexpr int height = 720;
    constexpr unsigned int devicePairCapacity = 4096;

    float4* devicePairs = nullptr;
    unsigned int* devicePairCount = nullptr;
    openzoom::StabilizationState* deviceState = nullptr;
    bool ok =
        CheckCuda(cudaMalloc(reinterpret_cast<void**>(&devicePairs),
                             devicePairCapacity * sizeof(float4)),
                  "cudaMalloc pairs") &&
        CheckCuda(cudaMalloc(reinterpret_cast<void**>(&devicePairCount),
                             sizeof(unsigned int)),
                  "cudaMalloc pair count") &&
        CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceState),
                             sizeof(openzoom::StabilizationState)),
                  "cudaMalloc state");
    if (!ok) {
        cudaFree(devicePairs);
        cudaFree(devicePairCount);
        cudaFree(deviceState);
        return 1;
    }

    // A frozen reference can become far from the current frame even though
    // the residual motion between consecutive frames remains tiny. Seed LK
    // with the previous accepted absolute transform so a 50-analysis-pixel
    // offset remains trackable; the old zero-start solver topped out below it.
    constexpr int trackerWidth = 640;
    constexpr int trackerHeight = 360;
    constexpr int trackerShift = 50;
    std::vector<float> referenceLuma(
        static_cast<size_t>(trackerWidth) * trackerHeight);
    std::vector<float> currentLuma(referenceLuma.size(), 0.0f);
    for (int y = 0; y < trackerHeight; ++y) {
        for (int x = 0; x < trackerWidth; ++x) {
            referenceLuma[static_cast<size_t>(y) * trackerWidth + x] =
                static_cast<float>(
                    (x * 37 + y * 53 + (x / 9) * 71 + (y / 11) * 29) &
                    255);
        }
    }
    for (int y = 0; y < trackerHeight; ++y) {
        for (int x = trackerShift; x < trackerWidth; ++x) {
            currentLuma[static_cast<size_t>(y) * trackerWidth + x] =
                referenceLuma[
                    static_cast<size_t>(y) * trackerWidth +
                    (x - trackerShift)];
        }
    }
    float* deviceReferenceLuma = nullptr;
    float* deviceCurrentLuma = nullptr;
    ok = ok &&
         CheckCuda(cudaMalloc(
                       reinterpret_cast<void**>(&deviceReferenceLuma),
                       referenceLuma.size() * sizeof(float)),
                   "cudaMalloc virtual tripod reference luma") &&
         CheckCuda(cudaMalloc(
                       reinterpret_cast<void**>(&deviceCurrentLuma),
                       currentLuma.size() * sizeof(float)),
                   "cudaMalloc virtual tripod current luma") &&
         CheckCuda(cudaMemcpy(
                       deviceReferenceLuma, referenceLuma.data(),
                       referenceLuma.size() * sizeof(float),
                       cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod reference luma") &&
         CheckCuda(cudaMemcpy(
                       deviceCurrentLuma, currentLuma.data(),
                       currentLuma.size() * sizeof(float),
                       cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod current luma");
    openzoom::StabilizationState seededTrackerState{};
    seededTrackerState.lastFrameMotion =
        make_float4(96.0f, 0.0f, 0.0f, 0.0f);
    ok = ok &&
         CheckCuda(cudaMemcpy(deviceState, &seededTrackerState,
                              sizeof(seededTrackerState),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod tracker seed");
    if (ok) {
        openzoom::LaunchVirtualTripodFeaturePairs(
            deviceCurrentLuma, deviceReferenceLuma,
            trackerWidth, trackerHeight, 2.0f, 2.0f,
            width, height, devicePairs, devicePairCount,
            devicePairCapacity, deviceState, nullptr);
        unsigned int trackedPairCount = 0;
        ok = CheckCuda(cudaMemcpy(&trackedPairCount, devicePairCount,
                                  sizeof(trackedPairCount),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy virtual tripod tracked pair count");
        if (ok && trackedPairCount < 12u) {
            std::cerr << "virtual tripod seeded tracker: expected at least 12 "
                         "pairs, got "
                      << trackedPairCount << '\n';
            ok = false;
        }
        if (ok) {
            std::vector<float4> trackedPairs(trackedPairCount);
            ok = CheckCuda(cudaMemcpy(
                               trackedPairs.data(), devicePairs,
                               trackedPairs.size() * sizeof(float4),
                               cudaMemcpyDeviceToHost),
                           "cudaMemcpy virtual tripod tracked pairs");
            double averageDx = 0.0;
            for (const float4& pair : trackedPairs) {
                averageDx += static_cast<double>(pair.z - pair.x);
            }
            averageDx /= static_cast<double>(trackedPairs.size());
            if (std::abs(averageDx - 2.0 * trackerShift) > 0.75) {
                std::cerr << "virtual tripod seeded tracker: expected 100 px, "
                             "got "
                          << averageDx << " px\n";
                ok = false;
            }
        }
    }

    // Phase-B prepared reference: a real Gaussian pyramid, precomputed
    // inverse-compositional Hessians, and translation-only RANSAC must suppress
    // the known clamped-laptop vibration sequence even through a brightness
    // change and a large moving foreground occluder.
    constexpr int trackerLevel1Width = (trackerWidth + 1) / 2;
    constexpr int trackerLevel1Height = (trackerHeight + 1) / 2;
    constexpr int trackerLevel2Width = (trackerLevel1Width + 1) / 2;
    constexpr int trackerLevel2Height = (trackerLevel1Height + 1) / 2;
    float* deviceReferenceLevel1 = nullptr;
    float* deviceReferenceLevel2 = nullptr;
    float* deviceCurrentLevel1 = nullptr;
    float* deviceCurrentLevel2 = nullptr;
    openzoom::TripodReferenceFeature* devicePreparedFeatures = nullptr;
    unsigned int* devicePreparedFeatureCount = nullptr;
    float* deviceReferenceAccumulator = nullptr;
    unsigned int* deviceReferenceSampleCounts = nullptr;
    float* deviceFocusScore = nullptr;
    float* deviceBestFocusScore = nullptr;
    unsigned int* deviceSelectionFlag = nullptr;
    std::vector<StabilizationTraceRow> stabilizationTrace;
    ok = ok &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceReferenceLevel1),
                              static_cast<size_t>(trackerLevel1Width) *
                                  trackerLevel1Height * sizeof(float)),
                   "cudaMalloc prepared reference level 1") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceReferenceLevel2),
                              static_cast<size_t>(trackerLevel2Width) *
                                  trackerLevel2Height * sizeof(float)),
                   "cudaMalloc prepared reference level 2") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceCurrentLevel1),
                              static_cast<size_t>(trackerLevel1Width) *
                                  trackerLevel1Height * sizeof(float)),
                   "cudaMalloc prepared current level 1") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceCurrentLevel2),
                              static_cast<size_t>(trackerLevel2Width) *
                                  trackerLevel2Height * sizeof(float)),
                   "cudaMalloc prepared current level 2") &&
         CheckCuda(cudaMalloc(
                       reinterpret_cast<void**>(&devicePreparedFeatures),
                       devicePairCapacity *
                           sizeof(openzoom::TripodReferenceFeature)),
                   "cudaMalloc prepared tripod features") &&
         CheckCuda(cudaMalloc(
                       reinterpret_cast<void**>(&devicePreparedFeatureCount),
                       sizeof(unsigned int)),
                   "cudaMalloc prepared tripod feature count") &&
         CheckCuda(cudaMalloc(
                       reinterpret_cast<void**>(&deviceReferenceAccumulator),
                       referenceLuma.size() * sizeof(float)),
                   "cudaMalloc tripod reference accumulator") &&
         CheckCuda(cudaMalloc(
                       reinterpret_cast<void**>(&deviceReferenceSampleCounts),
                       referenceLuma.size() * sizeof(unsigned int)),
                   "cudaMalloc tripod reference sample counts") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceFocusScore),
                              sizeof(float)),
                   "cudaMalloc tripod focus score") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceBestFocusScore),
                              sizeof(float)),
                   "cudaMalloc tripod best focus score") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceSelectionFlag),
                              sizeof(unsigned int)),
                   "cudaMalloc tripod selection flag");
    if (ok) {
        openzoom::LaunchStabilizationLumaPyramid(
            deviceReferenceLuma, trackerWidth, trackerHeight,
            deviceReferenceLevel1, trackerLevel1Width, trackerLevel1Height,
            deviceReferenceLevel2, trackerLevel2Width, trackerLevel2Height,
            nullptr);
        openzoom::LaunchPrepareVirtualTripodReference(
            deviceReferenceLuma, trackerWidth, trackerHeight,
            deviceReferenceLevel1, trackerLevel1Width, trackerLevel1Height,
            deviceReferenceLevel2, trackerLevel2Width, trackerLevel2Height,
            devicePreparedFeatures, devicePreparedFeatureCount,
            devicePairCapacity, nullptr);
        unsigned int preparedFeatureCount = 0;
        ok = CheckCuda(
            cudaMemcpy(&preparedFeatureCount, devicePreparedFeatureCount,
                       sizeof(preparedFeatureCount), cudaMemcpyDeviceToHost),
            "cudaMemcpy prepared tripod feature count");
        if (ok && preparedFeatureCount < 12u) {
            std::cerr << "prepared tripod reference: expected at least 12 "
                         "features, got "
                      << preparedFeatureCount << '\n';
            ok = false;
        }
    }
    if (ok) {
        openzoom::LaunchResetVirtualTripodState(deviceState, false, nullptr);
        std::vector<float> displayErrors;
        double inputSquared = 0.0;
        double outputSquared = 0.0;
        constexpr int virtualFrames = 90;
        constexpr float analysisToSource = 2.0f;
        constexpr float testZoom = 4.0f;
        stabilizationTrace.reserve(virtualFrames);
        for (int frame = 0; ok && frame < virtualFrames; ++frame) {
            const float t = static_cast<float>(frame) / 30.0f;
            float dx = 2.8f * std::sin(2.0f * 3.14159265f * 1.15f * t) +
                       1.1f * std::sin(2.0f * 3.14159265f * 2.75f * t + 0.3f);
            float dy = 2.1f * std::sin(2.0f * 3.14159265f * 0.85f * t + 0.8f) +
                       0.9f * std::sin(2.0f * 3.14159265f * 2.2f * t);
            const float impactTime = t - 1.55f;
            if (impactTime >= 0.0f) {
                const float ring = std::exp(-4.2f * impactTime);
                dx += 8.0f * ring *
                      std::sin(2.0f * 3.14159265f * 5.2f * impactTime);
                dy -= 5.5f * ring *
                      std::sin(2.0f * 3.14159265f * 4.6f * impactTime);
            }
            const bool occluded = frame >= 54 && frame < 74;
            const float brightness =
                (frame >= 76 && frame < 86) ? -18.0f : 0.0f;
            const std::vector<float> virtualFrame =
                MakeVirtualClampFrame(referenceLuma, trackerWidth, trackerHeight,
                                      dx, dy, brightness, occluded);
            ok = CheckCuda(
                cudaMemcpy(deviceCurrentLuma, virtualFrame.data(),
                           virtualFrame.size() * sizeof(float),
                           cudaMemcpyHostToDevice),
                "cudaMemcpy virtual clamp frame");
            if (!ok) {
                break;
            }
            openzoom::LaunchStabilizationLumaPyramid(
                deviceCurrentLuma, trackerWidth, trackerHeight,
                deviceCurrentLevel1, trackerLevel1Width, trackerLevel1Height,
                deviceCurrentLevel2, trackerLevel2Width, trackerLevel2Height,
                nullptr);
            openzoom::LaunchPreparedVirtualTripodFeaturePairs(
                deviceCurrentLuma, trackerWidth, trackerHeight,
                deviceCurrentLevel1, trackerLevel1Width, trackerLevel1Height,
                deviceCurrentLevel2, trackerLevel2Width, trackerLevel2Height,
                deviceReferenceLuma, deviceReferenceLevel1,
                deviceReferenceLevel2, devicePreparedFeatures,
                devicePreparedFeatureCount, analysisToSource, analysisToSource,
                width, height, devicePairs, devicePairCount,
                devicePairCapacity, deviceState, nullptr);
            openzoom::LaunchVirtualTripodSimilarityEstimate(
                devicePairs, devicePairCount, devicePairCapacity, width, height,
                2.0f, 1.0f, 0.45f, 0.5f, 0.5f, testZoom,
                deviceState, nullptr);
            openzoom::StabilizationState measuredState{};
            ok = CheckCuda(
                cudaMemcpy(&measuredState, deviceState, sizeof(measuredState),
                           cudaMemcpyDeviceToHost),
                "cudaMemcpy prepared tripod state");
            if (!ok) {
                break;
            }
            const float sourceDx = dx * analysisToSource;
            const float sourceDy = dy * analysisToSource;
            const bool accepted = measuredState.diagnostics.y >= 0.5f;
            stabilizationTrace.push_back(
                {frame, dx, dy,
                 measuredState.correction.x / analysisToSource,
                 measuredState.correction.y / analysisToSource, accepted});
            const float residualX = sourceDx + measuredState.correction.x;
            const float residualY = sourceDy + measuredState.correction.y;
            const float input = std::hypot(sourceDx, sourceDy) * testZoom;
            const float output = std::hypot(residualX, residualY) * testZoom;
            if (frame >= 6 && !(occluded && !accepted)) {
                inputSquared += static_cast<double>(input) * input;
                outputSquared += static_cast<double>(output) * output;
                displayErrors.push_back(output);
            }
        }
        if (ok && displayErrors.size() < 45u) {
            std::cerr << "prepared tripod virtual camera: too few accepted "
                         "frames ("
                      << displayErrors.size() << ")\n";
            ok = false;
        }
        if (ok) {
            std::sort(displayErrors.begin(), displayErrors.end());
            const size_t p95Index =
                std::min(displayErrors.size() - 1,
                         static_cast<size_t>(displayErrors.size() * 0.95));
            const float p95 = displayErrors[p95Index];
            const double attenuation =
                inputSquared > 0.0
                    ? 1.0 - std::sqrt(outputSquared / inputSquared)
                    : 0.0;
            if (attenuation < 0.90 || p95 > 1.0f) {
                std::cerr
                    << "prepared tripod virtual camera: expected >=90% RMS "
                       "attenuation and <=1 display px P95, got "
                    << attenuation * 100.0 << "% and " << p95 << " px\n";
                ok = false;
            } else {
                std::cout
                    << "Prepared tripod virtual camera: "
                    << attenuation * 100.0 << "% RMS attenuation, "
                    << p95 << " display px P95.\n";
            }
        }
    }

    // The lock-time builder must choose the sharpest candidate and average
    // only registered, photometrically compatible pixels. A foreground block
    // must not be burned into the persistent reference.
    if (ok) {
        const std::vector<float> blurredReference =
            BlurImage(referenceLuma, trackerWidth, trackerHeight);
        const float zero = 0.0f;
        ok = CheckCuda(cudaMemcpy(deviceBestFocusScore, &zero, sizeof(zero),
                                  cudaMemcpyHostToDevice),
                       "cudaMemcpy reset best focus score") &&
             CheckCuda(cudaMemcpy(deviceCurrentLuma, blurredReference.data(),
                                  blurredReference.size() * sizeof(float),
                                  cudaMemcpyHostToDevice),
                       "cudaMemcpy blurred tripod candidate");
        if (ok) {
            openzoom::LaunchMeasureVirtualTripodFocus(
                deviceCurrentLuma, trackerWidth, trackerHeight,
                deviceFocusScore, nullptr);
            openzoom::LaunchSelectSharperVirtualTripodReference(
                deviceCurrentLuma, deviceReferenceLuma,
                static_cast<int>(referenceLuma.size()), deviceFocusScore,
                deviceBestFocusScore, deviceSelectionFlag, nullptr);
            ok = CheckCuda(cudaMemcpy(deviceCurrentLuma, referenceLuma.data(),
                                      referenceLuma.size() * sizeof(float),
                                      cudaMemcpyHostToDevice),
                           "cudaMemcpy sharp tripod candidate");
        }
        if (ok) {
            openzoom::LaunchMeasureVirtualTripodFocus(
                deviceCurrentLuma, trackerWidth, trackerHeight,
                deviceFocusScore, nullptr);
            openzoom::LaunchSelectSharperVirtualTripodReference(
                deviceCurrentLuma, deviceReferenceLuma,
                static_cast<int>(referenceLuma.size()), deviceFocusScore,
                deviceBestFocusScore, deviceSelectionFlag, nullptr);
            std::vector<float> selectedReference(referenceLuma.size());
            ok = CheckCuda(cudaMemcpy(
                               selectedReference.data(), deviceReferenceLuma,
                               selectedReference.size() * sizeof(float),
                               cudaMemcpyDeviceToHost),
                           "cudaMemcpy selected tripod reference");
            double selectionError = 0.0;
            for (size_t index = 0; index < selectedReference.size(); ++index) {
                selectionError +=
                    std::abs(selectedReference[index] - referenceLuma[index]);
            }
            selectionError /= static_cast<double>(selectedReference.size());
            if (ok && selectionError > 0.01) {
                std::cerr << "tripod sharp-reference selection: expected "
                             "<=0.01 MAE, got "
                          << selectionError << '\n';
                ok = false;
            }
        }
        if (ok) {
            openzoom::LaunchInitializeVirtualTripodAccumulator(
                deviceReferenceLuma, deviceReferenceAccumulator,
                deviceReferenceSampleCounts,
                static_cast<int>(referenceLuma.size()), nullptr);
            openzoom::StabilizationState accumulationState{};
            accumulationState.diagnostics.y = 1.0f;
            accumulationState.lastFrameMotion =
                make_float4(4.0f, -2.0f, 0.0f, 0.0f);
            ok = CheckCuda(cudaMemcpy(
                               deviceState, &accumulationState,
                               sizeof(accumulationState),
                               cudaMemcpyHostToDevice),
                           "cudaMemcpy tripod accumulation state");
            const std::vector<float> cleanShifted =
                MakeVirtualClampFrame(referenceLuma, trackerWidth,
                                      trackerHeight, 2.0f, -1.0f, 0.0f, false);
            const std::vector<float> occludedShifted =
                MakeVirtualClampFrame(referenceLuma, trackerWidth,
                                      trackerHeight, 2.0f, -1.0f, 0.0f, true);
            for (const std::vector<float>* candidate :
                 {&cleanShifted, &occludedShifted}) {
                ok = ok &&
                     CheckCuda(cudaMemcpy(
                                   deviceCurrentLuma, candidate->data(),
                                   candidate->size() * sizeof(float),
                                   cudaMemcpyHostToDevice),
                               "cudaMemcpy tripod accumulation candidate");
                if (!ok) {
                    break;
                }
                openzoom::LaunchMeasureVirtualTripodFocus(
                    deviceCurrentLuma, trackerWidth, trackerHeight,
                    deviceFocusScore, nullptr);
                openzoom::LaunchAccumulateVirtualTripodReference(
                    deviceCurrentLuma, deviceReferenceLuma,
                    trackerWidth, trackerHeight, 2.0f, 2.0f,
                    deviceFocusScore, deviceBestFocusScore, deviceState,
                    deviceReferenceAccumulator, deviceReferenceSampleCounts,
                    nullptr);
            }
            if (ok) {
                openzoom::LaunchFinalizeVirtualTripodReference(
                    deviceReferenceLuma, deviceReferenceAccumulator,
                    deviceReferenceSampleCounts,
                    static_cast<int>(referenceLuma.size()), nullptr);
                std::vector<float> accumulatedReference(referenceLuma.size());
                ok = CheckCuda(cudaMemcpy(
                                   accumulatedReference.data(),
                                   deviceReferenceLuma,
                                   accumulatedReference.size() * sizeof(float),
                                   cudaMemcpyDeviceToHost),
                               "cudaMemcpy accumulated tripod reference");
                double backgroundError = 0.0;
                double occluderError = 0.0;
                size_t backgroundPixels = 0;
                size_t occluderPixels = 0;
                for (int y = 4; y < trackerHeight - 4; ++y) {
                    for (int x = 4; x < trackerWidth - 4; ++x) {
                        const size_t index =
                            static_cast<size_t>(y) * trackerWidth + x;
                        const double error = std::abs(
                            accumulatedReference[index] - referenceLuma[index]);
                        const bool inOccluder =
                            x >= trackerWidth / 3 &&
                            x < trackerWidth * 2 / 3 &&
                            y >= trackerHeight / 5 &&
                            y < trackerHeight * 4 / 5;
                        if (inOccluder) {
                            occluderError += error;
                            ++occluderPixels;
                        } else {
                            backgroundError += error;
                            ++backgroundPixels;
                        }
                    }
                }
                backgroundError /= static_cast<double>(backgroundPixels);
                occluderError /= static_cast<double>(occluderPixels);
                if (ok && (backgroundError > 0.5 || occluderError > 1.5)) {
                    std::cerr
                        << "tripod registered accumulation: background MAE "
                        << backgroundError << ", occluder MAE "
                        << occluderError << '\n';
                    ok = false;
                }
            }
        }
    }
    cudaFree(deviceSelectionFlag);
    cudaFree(deviceBestFocusScore);
    cudaFree(deviceFocusScore);
    cudaFree(deviceReferenceSampleCounts);
    cudaFree(deviceReferenceAccumulator);
    cudaFree(deviceReferenceLevel1);
    cudaFree(deviceReferenceLevel2);
    cudaFree(deviceCurrentLevel1);
    cudaFree(deviceCurrentLevel2);
    cudaFree(devicePreparedFeatures);
    cudaFree(devicePreparedFeatureCount);
    cudaFree(deviceReferenceLuma);
    cudaFree(deviceCurrentLuma);
    openzoom::StabilizationState state{};
    const unsigned int noPairs = 0;
    constexpr float testFps = 30.0f;
    std::vector<float4> oscillationPairs;
    oscillationPairs.reserve(72);
    for (int y = 80; y <= 640; y += 80) {
        for (int x = 100; x <= 1180; x += 135) {
            oscillationPairs.push_back(
                make_float4(static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(x), static_cast<float>(y)));
        }
    }
    const unsigned int oscillationPairCount =
        static_cast<unsigned int>(oscillationPairs.size());
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    constexpr int recordedFrames = 103;
    constexpr float recordedFrequencyHz = 1.165f;
    constexpr float recordedAmplitude = 18.9f;

    // Virtual Tripod measures every frame against one fixed reference. Unlike
    // the pairwise filter, absolute motion must not accumulate: the displayed
    // path (reference motion + correction) stays at the captured origin even
    // across slow drift and the recorded clamp-arm oscillation.
    openzoom::LaunchResetVirtualTripodState(deviceState, false, nullptr);
    inputEnergy = 0.0;
    outputEnergy = 0.0;
    for (int frame = 0; ok && frame < recordedFrames; ++frame) {
        const float phase =
            2.0f * 3.14159265358979323846f * recordedFrequencyHz *
            static_cast<float>(frame) / testFps;
        const float position =
            45.0f * static_cast<float>(frame) /
                static_cast<float>(recordedFrames - 1) +
            recordedAmplitude * std::sin(phase);
        for (float4& pair : oscillationPairs) {
            pair.z = pair.x;
            pair.w = pair.y + position;
        }
        ok = CheckCuda(cudaMemcpy(devicePairs, oscillationPairs.data(),
                                  oscillationPairs.size() * sizeof(float4),
                                  cudaMemcpyHostToDevice),
                       "cudaMemcpy virtual tripod pairs");
        openzoom::LaunchVirtualTripodSimilarityEstimate(
            devicePairs, devicePairCount, oscillationPairCount, width, height,
            6.0f, 0.98f, 0.25f, 0.5f, 0.5f, 1.0f,
            deviceState, nullptr);
        ok = ok &&
             CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy virtual tripod state");
        if (ok) {
            const double displayedPosition =
                static_cast<double>(state.actualPath.y + state.correction.y);
            inputEnergy += static_cast<double>(position) * position;
            outputEnergy += displayedPosition * displayedPosition;
        }
    }
    if (ok) {
        const double residualRatio =
            std::sqrt(outputEnergy / std::max(inputEnergy, 1.0e-12));
        if (residualRatio > 0.01) {
            std::cerr << "virtual tripod absolute lock: expected <= 0.01 RMS "
                         "residual, got "
                      << residualRatio << '\n';
            ok = false;
        }
        ok = ok && Near(state.diagnostics.w, 4.0f, 0.01f,
                        "virtual tripod estimator tag") &&
             Near(state.tripodDiagnostics.z, 1.0f, 0.01f,
                  "virtual tripod lock state");
    }

    // Losing the reference freezes the last trustworthy correction. Re-lock
    // preserves that correction as the new anchor, preventing a visible jump.
    const float4 correctionBeforeLoss = state.correction;
    ok = ok &&
         CheckCuda(cudaMemcpy(devicePairCount, &noPairs, sizeof(noPairs),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod empty count");
    openzoom::LaunchVirtualTripodSimilarityEstimate(
        devicePairs, devicePairCount, oscillationPairCount, width, height,
        6.0f, 0.98f, 0.25f, 0.5f, 0.5f, 1.0f,
        deviceState, nullptr);
    ok = ok &&
         CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpy virtual tripod held state");
    if (ok) {
        ok = Near(state.correction.x, correctionBeforeLoss.x, 0.001f,
                  "virtual tripod held correction x") &&
             Near(state.correction.y, correctionBeforeLoss.y, 0.001f,
                  "virtual tripod held correction y") &&
             Near(state.tripodDiagnostics.z, 2.0f, 0.01f,
                  "virtual tripod weakened state");
    }
    openzoom::LaunchResetVirtualTripodState(deviceState, true, nullptr);
    for (float4& pair : oscillationPairs) {
        pair.z = pair.x;
        pair.w = pair.y;
    }
    ok = ok &&
         CheckCuda(cudaMemcpy(devicePairs, oscillationPairs.data(),
                              oscillationPairs.size() * sizeof(float4),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod relock pairs") &&
         CheckCuda(cudaMemcpy(devicePairCount, &oscillationPairCount,
                              sizeof(oscillationPairCount),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod relock count");
    openzoom::LaunchVirtualTripodSimilarityEstimate(
        devicePairs, devicePairCount, oscillationPairCount, width, height,
        6.0f, 0.98f, 0.25f, 0.5f, 0.5f, 1.0f,
        deviceState, nullptr);
    ok = ok &&
         CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpy virtual tripod relocked state");
    if (ok) {
        ok = Near(state.correction.x, correctionBeforeLoss.x, 0.001f,
                  "virtual tripod relock continuity x") &&
             Near(state.correction.y, correctionBeforeLoss.y, 0.001f,
                  "virtual tripod relock continuity y") &&
             Near(state.tripodDiagnostics.w, 2.0f, 0.01f,
                  "virtual tripod reference captures");
    }

    // At high magnification the transform must be optimized for the visible
    // reading region rather than for a larger, independently moving area
    // elsewhere in the camera frame. The ROI has fewer pairs than the
    // background on purpose; a full-frame majority vote would choose +18 px.
    std::vector<float4> roiPairs;
    for (int y = 80; y <= 640; y += 70) {
        for (int x = 80; x <= 500; x += 70) {
            const float px = static_cast<float>(x);
            const float py = static_cast<float>(y);
            roiPairs.push_back(
                make_float4(px, py, px + 18.0f, py + 7.0f));
        }
    }
    for (int y = 220; y <= 500; y += 55) {
        for (int x = 900; x <= 1180; x += 55) {
            const float px = static_cast<float>(x);
            const float py = static_cast<float>(y);
            roiPairs.push_back(
                make_float4(px, py, px + 2.5f, py - 1.25f));
        }
    }
    const unsigned int roiPairCount =
        static_cast<unsigned int>(roiPairs.size());
    openzoom::LaunchResetVirtualTripodState(deviceState, false, nullptr);
    ok = ok &&
         CheckCuda(cudaMemcpy(devicePairs, roiPairs.data(),
                              roiPairs.size() * sizeof(float4),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod ROI pairs") &&
         CheckCuda(cudaMemcpy(devicePairCount, &roiPairCount,
                              sizeof(roiPairCount), cudaMemcpyHostToDevice),
                   "cudaMemcpy virtual tripod ROI count");
    openzoom::LaunchVirtualTripodSimilarityEstimate(
        devicePairs, devicePairCount, roiPairCount, width, height,
        1.0f, 1.0f, 0.45f, 0.82f, 0.5f, 4.0f,
        deviceState, nullptr);
    ok = ok &&
         CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpy virtual tripod ROI state");
    if (ok) {
        const float displayedResidualX =
            (state.actualPath.x + state.correction.x) * 4.0f;
        const float displayedResidualY =
            (state.actualPath.y + state.correction.y) * 4.0f;
        ok = Near(state.actualPath.x, 2.5f, 0.05f,
                  "virtual tripod visible ROI motion x") &&
             Near(state.actualPath.y, -1.25f, 0.05f,
                  "virtual tripod visible ROI motion y") &&
             std::hypot(displayedResidualX, displayedResidualY) <= 0.15f;
        if (!ok) {
            std::cerr << "virtual tripod visible ROI residual: "
                      << displayedResidualX << ", "
                      << displayedResidualY << " display px\n";
        }
    }

    // Recovery keyframes live in the original lock's coordinate system. A
    // +3/-2 px match against a keyframe captured at +40/-15 px must therefore
    // produce +43/-17 px, not restart the tripod origin at +3/-2.
    openzoom::TripodMatchCandidate* deviceCandidates = nullptr;
    float4* deviceKeyframeOrigins = nullptr;
    unsigned int* deviceKeyframeValid = nullptr;
    constexpr unsigned int keyframeCount = 4u;
    const std::array<float4, keyframeCount> keyframeOrigins = {
        make_float4(0.0f, 0.0f, 0.0f, 0.0f),
        make_float4(40.0f, -15.0f, 0.0f, 0.0f),
        make_float4(0.0f, 0.0f, 0.0f, 0.0f),
        make_float4(0.0f, 0.0f, 0.0f, 0.0f)};
    const std::array<unsigned int, keyframeCount> keyframeValid = {
        1u, 1u, 0u, 0u};
    ok = ok &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceCandidates),
                              keyframeCount *
                                  sizeof(openzoom::TripodMatchCandidate)),
                   "cudaMalloc tripod candidates") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceKeyframeOrigins),
                              keyframeCount * sizeof(float4)),
                   "cudaMalloc tripod keyframe origins") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceKeyframeValid),
                              keyframeCount * sizeof(unsigned int)),
                   "cudaMalloc tripod keyframe validity") &&
         CheckCuda(cudaMemcpy(deviceKeyframeOrigins, keyframeOrigins.data(),
                              keyframeCount * sizeof(float4),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod keyframe origins") &&
         CheckCuda(cudaMemcpy(deviceKeyframeValid, keyframeValid.data(),
                              keyframeCount * sizeof(unsigned int),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod keyframe validity") &&
         CheckCuda(cudaMemset(deviceCandidates, 0,
                              keyframeCount *
                                  sizeof(openzoom::TripodMatchCandidate)),
                   "cudaMemset tripod candidates");
    std::vector<float4> recoveryPairs;
    for (int y = 80; y <= 640; y += 70) {
        for (int x = 80; x <= 1200; x += 70) {
            recoveryPairs.push_back(make_float4(
                static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(x) + 3.0f,
                static_cast<float>(y) - 2.0f));
        }
    }
    const unsigned int recoveryPairCount =
        static_cast<unsigned int>(recoveryPairs.size());
    ok = ok &&
         CheckCuda(cudaMemcpy(devicePairs, recoveryPairs.data(),
                              recoveryPairs.size() * sizeof(float4),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy recovery keyframe pairs") &&
         CheckCuda(cudaMemcpy(devicePairCount, &recoveryPairCount,
                              sizeof(recoveryPairCount),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy recovery keyframe pair count");
    if (ok) {
        openzoom::LaunchResetVirtualTripodState(deviceState, false, nullptr);
        openzoom::LaunchVirtualTripodMatchCandidate(
            devicePairs, devicePairCount, recoveryPairCount, width, height,
            1.0f, 0.5f, 0.5f, 1.0f, deviceKeyframeOrigins,
            deviceKeyframeValid, 1u, deviceCandidates, nullptr);
        openzoom::LaunchSelectVirtualTripodMatch(
            deviceCandidates, keyframeCount, 1.0f, 0.45f, 1.0f,
            width, height, deviceState, nullptr);
        ok = CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy recovery keyframe state");
    }
    if (ok) {
        ok = Near(state.actualPath.x, 43.0f, 0.05f,
                  "tripod common-coordinate recovery x") &&
             Near(state.actualPath.y, -17.0f, 0.05f,
                  "tripod common-coordinate recovery y") &&
             Near(state.tripodDiagnostics.z, 4.0f, 0.01f,
                  "tripod recovery-map state");
    }

    // When every absolute reference is rejected, pairwise tracking may bridge
    // a short dropout but may not redefine the absolute anchor.
    const float4 absoluteCorrection = state.correction;
    std::vector<float4> fallbackPairs;
    for (int y = 80; y <= 640; y += 80) {
        for (int x = 80; x <= 1200; x += 80) {
            fallbackPairs.push_back(make_float4(
                static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(x) + 2.0f,
                static_cast<float>(y) - 1.0f));
        }
    }
    const unsigned int fallbackPairCount =
        static_cast<unsigned int>(fallbackPairs.size());
    ok = ok &&
         CheckCuda(cudaMemset(deviceCandidates, 0,
                              keyframeCount *
                                  sizeof(openzoom::TripodMatchCandidate)),
                   "cudaMemset rejected tripod candidates") &&
         CheckCuda(cudaMemcpy(devicePairs, fallbackPairs.data(),
                              fallbackPairs.size() * sizeof(float4),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod fallback pairs") &&
         CheckCuda(cudaMemcpy(devicePairCount, &fallbackPairCount,
                              sizeof(fallbackPairCount),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod fallback count");
    if (ok) {
        openzoom::LaunchSelectVirtualTripodMatch(
            deviceCandidates, keyframeCount, 1.0f, 0.45f, 1.0f,
            width, height, deviceState, nullptr);
        openzoom::LaunchVirtualTripodRelativeFallback(
            devicePairs, devicePairCount, fallbackPairCount,
            width, height, 1.0f, 0.45f, deviceState, nullptr);
        ok = CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy tripod fallback state");
    }
    if (ok) {
        ok = Near(state.correction.x, absoluteCorrection.x - 2.0f, 0.05f,
                  "tripod relative fallback x") &&
             Near(state.correction.y, absoluteCorrection.y + 1.0f, 0.05f,
                  "tripod relative fallback y") &&
             Near(state.tripodDiagnostics.z, 2.0f, 0.01f,
                  "tripod relative fallback state");
    }

    // The correction budget limits what can be displayed without exposing an
    // edge; it must not limit the absolute motion fed back into the next LK
    // prediction. Otherwise a gradual move past the crop reserve permanently
    // pushes the fixed reference outside the tracker's capture range.
    for (float4& pair : recoveryPairs) {
        pair.z = pair.x + 500.0f;
        pair.w = pair.y;
    }
    ok = ok &&
         CheckCuda(cudaMemcpy(devicePairs, recoveryPairs.data(),
                              recoveryPairs.size() * sizeof(float4),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod seed clamp pairs") &&
         CheckCuda(cudaMemcpy(devicePairCount, &recoveryPairCount,
                              sizeof(recoveryPairCount),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod seed clamp count");
    if (ok) {
        openzoom::LaunchResetVirtualTripodState(deviceState, false, nullptr);
        // Let the display correction reach its crop budget. The first accepted
        // frame is deliberately rate-limited, while every frame must preserve
        // the full 500 px tracker seed.
        for (int frame = 0; frame < 16; ++frame) {
            openzoom::LaunchVirtualTripodSimilarityEstimate(
                devicePairs, devicePairCount, recoveryPairCount, width, height,
                2.0f, 1.0f, 0.25f, 0.5f, 0.5f, 4.0f,
                deviceState, nullptr);
        }
        ok = CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy tripod seed clamp state");
    }
    if (ok) {
        ok = Near(state.lastFrameMotion.x, 500.0f, 0.05f,
                  "tripod unclamped tracker seed") &&
             Near(state.correction.x, -320.0f, 0.05f,
                  "tripod clamped display correction");
    }

    // A stale fixed-reference seed after a desk bump is re-acquired from
    // wide-range, illumination-invariant row/column profiles. This estimate
    // changes only the LK prediction; the feature tracker and RANSAC still
    // decide whether the model may reach the display.
    constexpr int projectionWidth = 640;
    constexpr int projectionHeight = 360;
    constexpr int projectionShiftX = 60;
    constexpr int projectionShiftY = -35;
    std::vector<float> referenceColumns(projectionWidth);
    std::vector<float> currentColumns(projectionWidth, 0.0f);
    std::vector<float> referenceRows(projectionHeight);
    std::vector<float> currentRows(projectionHeight, 0.0f);
    for (int index = 0; index < projectionWidth; ++index) {
        referenceColumns[index] =
            1000.0f + 91.0f * std::sin(index * 0.173f) +
            static_cast<float>((index * 37 + index * index * 11) % 173);
        const int referenceIndex = index - projectionShiftX;
        if (referenceIndex >= 0 && referenceIndex < projectionWidth) {
            currentColumns[index] = referenceColumns[referenceIndex] + 48.0f;
        }
    }
    for (int index = 0; index < projectionHeight; ++index) {
        referenceRows[index] =
            700.0f + 67.0f * std::cos(index * 0.137f) +
            static_cast<float>((index * 53 + index * index * 7) % 149);
    }
    for (int index = 0; index < projectionHeight; ++index) {
        const int referenceIndex = index - projectionShiftY;
        if (referenceIndex >= 0 && referenceIndex < projectionHeight) {
            currentRows[index] = referenceRows[referenceIndex] - 31.0f;
        }
    }
    float* deviceReferenceColumns = nullptr;
    float* deviceCurrentColumns = nullptr;
    float* deviceReferenceRows = nullptr;
    float* deviceCurrentRows = nullptr;
    ok = ok &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceReferenceColumns),
                              referenceColumns.size() * sizeof(float)),
                   "cudaMalloc tripod reference columns") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceCurrentColumns),
                              currentColumns.size() * sizeof(float)),
                   "cudaMalloc tripod current columns") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceReferenceRows),
                              referenceRows.size() * sizeof(float)),
                   "cudaMalloc tripod reference rows") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceCurrentRows),
                              currentRows.size() * sizeof(float)),
                   "cudaMalloc tripod current rows") &&
         CheckCuda(cudaMemcpy(deviceReferenceColumns, referenceColumns.data(),
                              referenceColumns.size() * sizeof(float),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod reference columns") &&
         CheckCuda(cudaMemcpy(deviceCurrentColumns, currentColumns.data(),
                              currentColumns.size() * sizeof(float),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod current columns") &&
         CheckCuda(cudaMemcpy(deviceReferenceRows, referenceRows.data(),
                              referenceRows.size() * sizeof(float),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod reference rows") &&
         CheckCuda(cudaMemcpy(deviceCurrentRows, currentRows.data(),
                              currentRows.size() * sizeof(float),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy tripod current rows");
    if (ok) {
        openzoom::StabilizationState rejectedState{};
        rejectedState.tripodDiagnostics.y = 1.0f;
        ok = CheckCuda(cudaMemcpy(deviceState, &rejectedState,
                                  sizeof(rejectedState),
                                  cudaMemcpyHostToDevice),
                       "cudaMemcpy rejected tripod state");
        openzoom::LaunchVirtualTripodProjectionSeed(
            deviceCurrentColumns, deviceCurrentRows,
            deviceReferenceColumns, deviceReferenceRows,
            projectionWidth, projectionHeight, 2.0f, 2.0f,
            deviceKeyframeOrigins, deviceKeyframeValid, 0u,
            deviceState, nullptr);
        ok = ok &&
             CheckCuda(cudaMemcpy(&state, deviceState, sizeof(state),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy tripod projection seed state");
    }
    if (ok) {
        ok = Near(state.lastFrameMotion.x,
                  2.0f * projectionShiftX, 0.05f,
                  "tripod projection seed x") &&
             Near(state.lastFrameMotion.y,
                  2.0f * projectionShiftY, 0.05f,
                  "tripod projection seed y");
    }
    cudaFree(deviceCurrentRows);
    cudaFree(deviceReferenceRows);
    cudaFree(deviceCurrentColumns);
    cudaFree(deviceReferenceColumns);
    cudaFree(deviceKeyframeValid);
    cudaFree(deviceKeyframeOrigins);
    cudaFree(deviceCandidates);

    // The fixed-reference warp applies the current global correction uniformly.
    constexpr int warpWidth = 64;
    constexpr int warpHeight = 33;
    constexpr int probeX = 32;
    constexpr int probeY = warpHeight / 2;
    std::vector<uchar4> source(
        static_cast<size_t>(warpWidth) * warpHeight);
    for (int y = 0; y < warpHeight; ++y) {
        for (int x = 0; x < warpWidth; ++x) {
            source[static_cast<size_t>(y) * warpWidth + x] =
                make_uchar4(static_cast<unsigned char>(x),
                            static_cast<unsigned char>(x),
                            static_cast<unsigned char>(x), 255u);
        }
    }
    std::vector<uchar4> stabilized(source.size());
    uchar4* deviceSource = nullptr;
    uchar4* deviceStabilized = nullptr;
    const size_t warpBytes = source.size() * sizeof(uchar4);
    openzoom::StabilizationState warpState{};
    warpState.correction = make_float4(8.0f, 0.0f, 0.0f, 0.0f);
    ok = ok &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceSource),
                              warpBytes),
                   "cudaMalloc stabilization warp source") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceStabilized),
                              warpBytes),
                   "cudaMalloc stabilization warp output") &&
         CheckCuda(cudaMemcpy(deviceSource, source.data(), warpBytes,
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy stabilization warp source") &&
         CheckCuda(cudaMemcpy(deviceState, &warpState, sizeof(warpState),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy stabilization warp state");
    if (ok) {
        openzoom::LaunchStabilizationWarp(
            deviceStabilized, static_cast<size_t>(warpWidth) * sizeof(uchar4),
            deviceSource, static_cast<size_t>(warpWidth) * sizeof(uchar4),
            warpWidth, warpHeight, deviceState, nullptr);
        ok = CheckCuda(cudaMemcpy(stabilized.data(), deviceStabilized,
                                  warpBytes, cudaMemcpyDeviceToHost),
                       "cudaMemcpy stabilization warp output");
    }
    if (ok) {
        const uchar4 center =
            stabilized[static_cast<size_t>(probeY) * warpWidth + probeX];
        ok = Near(static_cast<float>(center.x),
                  static_cast<float>(probeX) - 8.0f, 1.0f,
                  "fixed-reference warp correction");
    }
    cudaFree(deviceStabilized);
    cudaFree(deviceSource);

    // Bump Hold is a presentation policy layered on a still-running Virtual
    // Tripod tracker. A large accepted transform step must retain the last
    // sharp stabilized frame, then five stable frames arm a four-frame
    // crossfade back to the live pixels.
    constexpr int bumpWidth = 4;
    constexpr int bumpHeight = 2;
    const size_t bumpBytes =
        static_cast<size_t>(bumpWidth) * bumpHeight * sizeof(uchar4);
    uchar4* deviceBumpCurrent = nullptr;
    uchar4* deviceBumpHeld = nullptr;
    openzoom::BumpHoldState* deviceBumpState = nullptr;
    float* deviceBumpFocus = nullptr;
    float* deviceBumpReferenceFocus = nullptr;
    std::vector<uchar4> bumpFrame(
        static_cast<size_t>(bumpWidth) * bumpHeight);
    auto fillBumpFrame = [&](unsigned char value) {
        std::fill(bumpFrame.begin(), bumpFrame.end(),
                  make_uchar4(value, value, value, 255u));
        return CheckCuda(
            cudaMemcpy(deviceBumpCurrent, bumpFrame.data(), bumpBytes,
                       cudaMemcpyHostToDevice),
            "cudaMemcpy bump hold live frame");
    };
    ok = ok &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceBumpCurrent),
                              bumpBytes),
                   "cudaMalloc bump hold current") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceBumpHeld),
                              bumpBytes),
                   "cudaMalloc bump hold held") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceBumpState),
                              sizeof(openzoom::BumpHoldState)),
                   "cudaMalloc bump hold state") &&
         CheckCuda(cudaMalloc(reinterpret_cast<void**>(&deviceBumpFocus),
                              sizeof(float)),
                   "cudaMalloc bump hold focus") &&
         CheckCuda(cudaMalloc(
                       reinterpret_cast<void**>(&deviceBumpReferenceFocus),
                       sizeof(float)),
                   "cudaMalloc bump hold reference focus");
    const float sharpFocus = 100.0f;
    ok = ok &&
         CheckCuda(cudaMemcpy(deviceBumpFocus, &sharpFocus,
                              sizeof(sharpFocus), cudaMemcpyHostToDevice),
                   "cudaMemcpy bump hold focus") &&
         CheckCuda(cudaMemcpy(deviceBumpReferenceFocus, &sharpFocus,
                              sizeof(sharpFocus), cudaMemcpyHostToDevice),
                   "cudaMemcpy bump hold reference focus");
    openzoom::StabilizationState bumpTrackingState{};
    bumpTrackingState.diagnostics.y = 1.0f;
    bumpTrackingState.lastFrameMotion =
        make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    ok = ok &&
         CheckCuda(cudaMemcpy(deviceState, &bumpTrackingState,
                              sizeof(bumpTrackingState),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy bump hold tracking state");
    if (ok) {
        openzoom::LaunchResetBumpHoldState(deviceBumpState, nullptr);
        ok = fillBumpFrame(10u);
        openzoom::LaunchUpdateBumpHoldState(
            deviceBumpState, deviceState, deviceBumpFocus,
            deviceBumpReferenceFocus, true, 1280, 720, 5.0f, 2.0f, nullptr);
        openzoom::LaunchApplyBumpHold(
            deviceBumpCurrent,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            deviceBumpHeld,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            bumpWidth, bumpHeight, deviceBumpState, nullptr);
    }
    if (ok) {
        bumpTrackingState.lastFrameMotion =
            make_float4(12.0f, 0.0f, 0.0f, 0.0f);
        ok = CheckCuda(cudaMemcpy(deviceState, &bumpTrackingState,
                                  sizeof(bumpTrackingState),
                                  cudaMemcpyHostToDevice),
                       "cudaMemcpy bump hold impact state") &&
             fillBumpFrame(100u);
        openzoom::LaunchUpdateBumpHoldState(
            deviceBumpState, deviceState, deviceBumpFocus,
            deviceBumpReferenceFocus, true, 1280, 720, 5.0f, 2.0f, nullptr);
        openzoom::LaunchApplyBumpHold(
            deviceBumpCurrent,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            deviceBumpHeld,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            bumpWidth, bumpHeight, deviceBumpState, nullptr);
        ok = ok &&
             CheckCuda(cudaMemcpy(bumpFrame.data(), deviceBumpCurrent,
                                  bumpBytes, cudaMemcpyDeviceToHost),
                       "cudaMemcpy bump hold impact output");
        openzoom::BumpHoldState bumpState{};
        ok = ok &&
             CheckCuda(cudaMemcpy(&bumpState, deviceBumpState,
                                  sizeof(bumpState), cudaMemcpyDeviceToHost),
                       "cudaMemcpy bump hold impact mode");
        if (ok) {
            ok = Near(bumpState.diagnostics.x, 1.0f, 0.01f,
                      "bump hold impact mode") &&
                 Near(static_cast<float>(bumpFrame.front().x), 10.0f, 0.5f,
                      "bump hold retained pixel");
        }
    }
    for (int stable = 0; ok && stable < 5; ++stable) {
        ok = fillBumpFrame(100u);
        openzoom::LaunchUpdateBumpHoldState(
            deviceBumpState, deviceState, deviceBumpFocus,
            deviceBumpReferenceFocus, true, 1280, 720, 5.0f, 2.0f, nullptr);
        openzoom::LaunchApplyBumpHold(
            deviceBumpCurrent,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            deviceBumpHeld,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            bumpWidth, bumpHeight, deviceBumpState, nullptr);
    }
    for (int recovery = 0; ok && recovery < 4; ++recovery) {
        ok = fillBumpFrame(100u);
        openzoom::LaunchUpdateBumpHoldState(
            deviceBumpState, deviceState, deviceBumpFocus,
            deviceBumpReferenceFocus, true, 1280, 720, 5.0f, 2.0f, nullptr);
        openzoom::LaunchApplyBumpHold(
            deviceBumpCurrent,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            deviceBumpHeld,
            static_cast<size_t>(bumpWidth) * sizeof(uchar4),
            bumpWidth, bumpHeight, deviceBumpState, nullptr);
        ok = ok &&
             CheckCuda(cudaMemcpy(bumpFrame.data(), deviceBumpCurrent,
                                  bumpBytes, cudaMemcpyDeviceToHost),
                       "cudaMemcpy bump hold recovery output");
        if (ok && recovery == 0) {
            ok = Near(static_cast<float>(bumpFrame.front().x), 32.0f, 1.0f,
                      "bump hold first crossfade pixel");
        }
    }
    if (ok) {
        openzoom::BumpHoldState bumpState{};
        ok = CheckCuda(cudaMemcpy(&bumpState, deviceBumpState,
                                  sizeof(bumpState), cudaMemcpyDeviceToHost),
                       "cudaMemcpy bump hold recovered mode");
        if (ok) {
            ok = Near(bumpState.diagnostics.x, 0.0f, 0.01f,
                      "bump hold recovered mode") &&
                 Near(static_cast<float>(bumpFrame.front().x), 100.0f, 0.5f,
                      "bump hold recovered pixel");
        }
    }
    cudaFree(deviceBumpReferenceFocus);
    cudaFree(deviceBumpFocus);
    cudaFree(deviceBumpState);
    cudaFree(deviceBumpHeld);
    cudaFree(deviceBumpCurrent);

    cudaFree(devicePairs);
    cudaFree(devicePairCount);
    cudaFree(deviceState);
    if (!ok) {
        return 1;
    }
    if (!traceOutputPath.empty() &&
        !WriteStabilizationTrace(traceOutputPath, stabilizationTrace)) {
        return 1;
    }
    std::cout << "Stabilization similarity/RANSAC CUDA checks passed.\n";
    return 0;
}
