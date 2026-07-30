#include "openzoom/cuda/cuda_kernels.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr unsigned int kMaximumPairs = 4096;
constexpr int kFocusSelectionFrames = 5;
constexpr int kDefaultReferenceAccumulationFrames = 0;
constexpr unsigned int kKeyframeCount = 4;
constexpr unsigned int kRecoveryKeyframeCount = kKeyframeCount - 1;
constexpr unsigned int kKeyframeAdmissionFrames = 300;
constexpr unsigned int kKeyframeMinimumValidFrames = 30;
constexpr int kSyntheticWidth = 160;
constexpr int kSyntheticHeight = 90;
constexpr int kSyntheticFps = 30;
constexpr int kSyntheticFrameCount = 120;

struct Options {
    std::string inputPath;
    std::string outputPath;
    std::string tracePath;
    int width{};
    int height{};
    float zoom{1.25f};
    float strength{1.0f};
    int referenceAccumulationFrames{kDefaultReferenceAccumulationFrames};
    bool syntheticClampBump{};
};

bool CheckCuda(cudaError_t status, const char* operation)
{
    if (status == cudaSuccess) {
        return true;
    }
    std::cerr << operation << ": " << cudaGetErrorString(status) << '\n';
    return false;
}

bool ParsePositiveInt(const std::string& text, int& value)
{
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(text, &consumed);
        if (consumed != text.size() || parsed <= 0) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseFloat(const std::string& text, float& value)
{
    try {
        size_t consumed = 0;
        const float parsed = std::stof(text, &consumed);
        if (consumed != text.size() || !std::isfinite(parsed)) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto consume = [&](std::string& destination) {
            if (index + 1 >= argc) {
                return false;
            }
            destination = argv[++index];
            return true;
        };
        if (argument == "--input") {
            if (!consume(options.inputPath)) {
                return false;
            }
        } else if (argument == "--output") {
            if (!consume(options.outputPath)) {
                return false;
            }
        } else if (argument == "--trace") {
            if (!consume(options.tracePath)) {
                return false;
            }
        } else if (argument == "--width") {
            std::string value;
            if (!consume(value) || !ParsePositiveInt(value, options.width)) {
                return false;
            }
        } else if (argument == "--height") {
            std::string value;
            if (!consume(value) || !ParsePositiveInt(value, options.height)) {
                return false;
            }
        } else if (argument == "--zoom") {
            std::string value;
            if (!consume(value) || !ParseFloat(value, options.zoom)) {
                return false;
            }
        } else if (argument == "--strength") {
            std::string value;
            if (!consume(value) || !ParseFloat(value, options.strength)) {
                return false;
            }
        } else if (argument == "--no-reference-accumulation") {
            options.referenceAccumulationFrames = 0;
        } else if (argument == "--synthetic-clamp-bump") {
            options.syntheticClampBump = true;
        } else {
            return false;
        }
    }
    options.zoom = std::max(options.zoom, 1.0f);
    options.strength = std::clamp(options.strength, 0.0f, 1.0f);
    return (options.syntheticClampBump || !options.inputPath.empty()) &&
           !options.outputPath.empty() &&
           !options.tracePath.empty() && options.width > 0 &&
           options.height > 0;
}

bool PrepareInput(std::ifstream& input, const Options& options, bool& isY4m,
                  std::vector<unsigned char>& yPlane,
                  std::vector<unsigned char>& chroma)
{
    std::array<char, 9> signature{};
    input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    const bool hasY4mSignature =
        input.gcount() == static_cast<std::streamsize>(signature.size()) &&
        std::string(signature.data(), signature.size()) == "YUV4MPEG2";
    input.clear();
    input.seekg(0, std::ios::beg);
    if (!hasY4mSignature) {
        isY4m = false;
        return true;
    }

    std::string header;
    if (!std::getline(input, header)) {
        std::cerr << "Could not read the Y4M stream header.\n";
        return false;
    }

    int streamWidth = 0;
    int streamHeight = 0;
    std::istringstream fields(header);
    std::string field;
    fields >> field;
    try {
        while (fields >> field) {
            if (field.size() > 1 && field.front() == 'W') {
                streamWidth = std::stoi(field.substr(1));
            } else if (field.size() > 1 && field.front() == 'H') {
                streamHeight = std::stoi(field.substr(1));
            }
        }
    } catch (...) {
        std::cerr << "The Y4M stream has invalid dimensions.\n";
        return false;
    }

    if (streamWidth != options.width || streamHeight != options.height) {
        std::cerr << "Y4M dimensions " << streamWidth << 'x' << streamHeight
                  << " do not match --width/--height " << options.width << 'x'
                  << options.height << ".\n";
        return false;
    }

    const size_t lumaBytes =
        static_cast<size_t>(options.width) * options.height;
    const size_t chromaPlaneBytes =
        static_cast<size_t>((options.width + 1) / 2) *
        static_cast<size_t>((options.height + 1) / 2);
    yPlane.resize(lumaBytes);
    chroma.resize(chromaPlaneBytes * 2);
    isY4m = true;
    return true;
}

bool ReadInputFrame(std::ifstream& input, bool isY4m,
                    std::vector<unsigned char>& yPlane,
                    std::vector<unsigned char>& chroma,
                    std::vector<uchar4>& frame, bool& reachedEnd)
{
    reachedEnd = false;
    if (!isY4m) {
        const auto frameBytes =
            static_cast<std::streamsize>(frame.size() * sizeof(uchar4));
        input.read(reinterpret_cast<char*>(frame.data()), frameBytes);
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead == 0) {
            reachedEnd = true;
            return true;
        }
        if (bytesRead != frameBytes) {
            std::cerr << "The raw BGRA input ends with a partial frame.\n";
            return false;
        }
        return true;
    }

    std::string frameHeader;
    if (!std::getline(input, frameHeader)) {
        reachedEnd = input.eof();
        if (!reachedEnd) {
            std::cerr << "Could not read the next Y4M frame header.\n";
        }
        return reachedEnd;
    }
    if (frameHeader.rfind("FRAME", 0) != 0) {
        std::cerr << "Invalid Y4M frame marker: " << frameHeader << '\n';
        return false;
    }

    input.read(reinterpret_cast<char*>(yPlane.data()),
               static_cast<std::streamsize>(yPlane.size()));
    if (input.gcount() != static_cast<std::streamsize>(yPlane.size())) {
        std::cerr << "The Y4M luma plane is truncated.\n";
        return false;
    }
    input.read(reinterpret_cast<char*>(chroma.data()),
               static_cast<std::streamsize>(chroma.size()));
    if (input.gcount() != static_cast<std::streamsize>(chroma.size())) {
        std::cerr << "The Y4M chroma planes are truncated.\n";
        return false;
    }

    for (size_t index = 0; index < frame.size(); ++index) {
        const unsigned char luma = yPlane[index];
        frame[index] = make_uchar4(luma, luma, luma, 255);
    }
    return true;
}

template <typename T>
bool Allocate(T*& pointer, size_t count, const char* operation)
{
    return CheckCuda(
        cudaMalloc(reinterpret_cast<void**>(&pointer), count * sizeof(T)),
        operation);
}

void ComputeAnalysisDimensions(int width, int height, int& factorX,
                               int& factorY, int& smallWidth, int& smallHeight)
{
    factorX = (width + 639) / 640;
    factorY = (height + 359) / 360;
    smallWidth = (width + factorX - 1) / factorX;
    smallHeight = (height + factorY - 1) / factorY;
}

std::array<double, 2> SyntheticMotion(int frame)
{
    constexpr double kPi = 3.14159265358979323846;
    const double time = static_cast<double>(frame) / kSyntheticFps;
    double dx = 2.8 * std::sin(2.0 * kPi * 1.15 * time);
    dx += 1.1 * std::sin(2.0 * kPi * 2.75 * time + 0.3);
    double dy = 2.1 * std::sin(2.0 * kPi * 0.85 * time + 0.8);
    dy += 0.9 * std::sin(2.0 * kPi * 2.2 * time);
    const double impactTime = time - 1.55;
    if (impactTime >= 0.0) {
        const double ring = std::exp(-4.2 * impactTime);
        dx += 8.0 * ring * std::sin(2.0 * kPi * 5.2 * impactTime);
        dy -= 5.5 * ring * std::sin(2.0 * kPi * 4.6 * impactTime);
    }
    return {dx, dy};
}

double SyntheticBaseLuma(double x, double y)
{
    const int tileX = static_cast<int>(std::floor(x / 10.0));
    const int tileY = static_cast<int>(std::floor(y / 10.0));
    double value = 128.0 + (((tileX + tileY) & 1) != 0 ? 22.0 : -18.0);
    if (y >= 18.0 && y <= 66.0 && x >= 15.0 && x <= 145.0) {
        value += 25.0;
    }
    for (const double row : {25.0, 36.0, 48.0, 60.0}) {
        if (std::abs(y - row) < 1.3 && x >= 24.0 && x <= 133.0) {
            value = 30.0;
        }
    }
    for (const double column : {31.0, 58.0, 91.0, 125.0}) {
        if (std::abs(x - column) < 1.2 && y >= 20.0 && y <= 68.0) {
            value = 225.0;
        }
    }
    const double radius = std::hypot(x - 128.0, y - 22.0);
    if (radius > 8.0 && radius < 10.5) {
        value = 18.0;
    }
    return std::clamp(value, 0.0, 255.0);
}

double SyntheticBilinearLuma(double x, double y)
{
    const double x0 = std::floor(x);
    const double y0 = std::floor(y);
    const double fractionX = x - x0;
    const double fractionY = y - y0;
    const double top =
        SyntheticBaseLuma(x0, y0) * (1.0 - fractionX) +
        SyntheticBaseLuma(x0 + 1.0, y0) * fractionX;
    const double bottom =
        SyntheticBaseLuma(x0, y0 + 1.0) * (1.0 - fractionX) +
        SyntheticBaseLuma(x0 + 1.0, y0 + 1.0) * fractionX;
    return top * (1.0 - fractionY) + bottom * fractionY;
}

bool GenerateSyntheticClampBumpFrame(int frame, int width, int height,
                                     std::vector<uchar4>& destination)
{
    if (width != kSyntheticWidth || height != kSyntheticHeight ||
        frame < 0 || frame >= kSyntheticFrameCount) {
        return false;
    }

    const auto motion = SyntheticMotion(frame);
    const double brightness = frame >= 78 && frame < 94 ? -28.0 : 0.0;
    const bool blurred = frame >= 33 && frame < 39;
    const bool occluded = frame >= 54 && frame < 74;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double sourceX = x - motion[0];
            const double sourceY = y - motion[1];
            double value = 0.0;
            if (blurred) {
                for (const double offsetY : {-1.0, 0.0, 1.0}) {
                    for (const double offsetX : {-1.0, 0.0, 1.0}) {
                        value += SyntheticBilinearLuma(
                            sourceX + offsetX, sourceY + offsetY);
                    }
                }
                value /= 9.0;
            } else {
                value = SyntheticBilinearLuma(sourceX, sourceY);
            }
            value += brightness;
            if (occluded) {
                const int left = 30 + (frame - 54) * 2;
                if (x >= left && x < left + 42 && y >= 18 && y < 76) {
                    value = 72.0 + ((x + y) & 7);
                }
            }
            const auto luma = static_cast<unsigned char>(
                std::clamp(std::lround(value), 0L, 255L));
            destination[static_cast<size_t>(y) * width + x] =
                make_uchar4(luma, luma, luma, 255);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr
            << "Usage: stabilization_replay_cuda --input INPUT.bgra|INPUT.y4m "
               "--output OUTPUT.bgra --trace TRACE.csv --width WIDTH "
               "--height HEIGHT [--zoom 1.25] [--strength 1.0]\n"
               "       stabilization_replay_cuda --synthetic-clamp-bump "
               "--output OUTPUT.bgra --trace TRACE.csv --width 160 "
               "--height 90\n";
        return 2;
    }

    int deviceCount = 0;
    if (!CheckCuda(cudaGetDeviceCount(&deviceCount), "cudaGetDeviceCount") ||
        deviceCount == 0) {
        std::cerr << "No CUDA device is available.\n";
        return 77;
    }
    if (!CheckCuda(cudaSetDevice(0), "cudaSetDevice")) {
        return 1;
    }

    std::ifstream input;
    if (!options.syntheticClampBump) {
        input.open(options.inputPath, std::ios::binary);
    }
    std::ofstream output(options.outputPath,
                         std::ios::binary | std::ios::trunc);
    std::ofstream trace(options.tracePath, std::ios::trunc);
    if ((!options.syntheticClampBump && !input) || !output || !trace) {
        std::cerr << "Could not open replay input or output files.\n";
        return 1;
    }

    const size_t framePixels =
        static_cast<size_t>(options.width) * options.height;
    const size_t frameBytes = framePixels * sizeof(uchar4);
    std::vector<uchar4> hostInput(framePixels);
    std::vector<uchar4> hostOutput(framePixels);
    bool inputIsY4m = false;
    std::vector<unsigned char> yPlane;
    std::vector<unsigned char> chroma;
    if (!options.syntheticClampBump &&
        !PrepareInput(input, options, inputIsY4m, yPlane, chroma)) {
        return 1;
    }

    int factorX = 0;
    int factorY = 0;
    int analysisWidth = 0;
    int analysisHeight = 0;
    ComputeAnalysisDimensions(options.width, options.height, factorX, factorY,
                              analysisWidth, analysisHeight);
    const int level1Width = (analysisWidth + 1) / 2;
    const int level1Height = (analysisHeight + 1) / 2;
    const int level2Width = (level1Width + 1) / 2;
    const int level2Height = (level1Height + 1) / 2;
    const size_t analysisPixels =
        static_cast<size_t>(analysisWidth) * analysisHeight;

    uchar4* deviceInput = nullptr;
    uchar4* deviceOutput = nullptr;
    size_t inputPitch = 0;
    size_t outputPitch = 0;
    float* currentLuma = nullptr;
    float* previousLuma = nullptr;
    float* referenceLuma = nullptr;
    float* currentLevel1 = nullptr;
    float* currentLevel2 = nullptr;
    float* referenceLevel1 = nullptr;
    float* referenceLevel2 = nullptr;
    openzoom::TripodReferenceFeature* referenceFeatures = nullptr;
    unsigned int* referenceFeatureCount = nullptr;
    float* recoveryLuma = nullptr;
    float* recoveryLevel1 = nullptr;
    float* recoveryLevel2 = nullptr;
    openzoom::TripodReferenceFeature* recoveryFeatures = nullptr;
    unsigned int* recoveryFeatureCounts = nullptr;
    float4* keyframeOrigins = nullptr;
    unsigned int* keyframeValid = nullptr;
    openzoom::TripodMatchCandidate* matchCandidates = nullptr;
    float4* pairs = nullptr;
    unsigned int* pairCount = nullptr;
    openzoom::StabilizationState* state = nullptr;
    float* focusScore = nullptr;
    float* bestFocusScore = nullptr;
    unsigned int* selectionFlag = nullptr;
    float* referenceAccumulator = nullptr;
    unsigned int* referenceSampleCounts = nullptr;
    float* currentColProjection = nullptr;
    float* currentRowProjection = nullptr;
    float* referenceColProjection = nullptr;
    float* referenceRowProjection = nullptr;
    cudaStream_t stream = nullptr;

    bool ok =
        CheckCuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
                  "cudaStreamCreateWithFlags") &&
        CheckCuda(cudaMallocPitch(reinterpret_cast<void**>(&deviceInput),
                                  &inputPitch,
                                  static_cast<size_t>(options.width) *
                                      sizeof(uchar4),
                                  options.height),
                  "cudaMallocPitch replay input") &&
        CheckCuda(cudaMallocPitch(reinterpret_cast<void**>(&deviceOutput),
                                  &outputPitch,
                                  static_cast<size_t>(options.width) *
                                      sizeof(uchar4),
                                  options.height),
                  "cudaMallocPitch replay output") &&
        Allocate(currentLuma, analysisPixels, "cudaMalloc current luma") &&
        Allocate(previousLuma, analysisPixels, "cudaMalloc previous luma") &&
        Allocate(referenceLuma, analysisPixels, "cudaMalloc reference luma") &&
        Allocate(currentLevel1,
                 static_cast<size_t>(level1Width) * level1Height,
                 "cudaMalloc current pyramid 1") &&
        Allocate(currentLevel2,
                 static_cast<size_t>(level2Width) * level2Height,
                 "cudaMalloc current pyramid 2") &&
        Allocate(referenceLevel1,
                 static_cast<size_t>(level1Width) * level1Height,
                 "cudaMalloc reference pyramid 1") &&
        Allocate(referenceLevel2,
                 static_cast<size_t>(level2Width) * level2Height,
                 "cudaMalloc reference pyramid 2") &&
        Allocate(referenceFeatures, kMaximumPairs,
                 "cudaMalloc reference features") &&
        Allocate(referenceFeatureCount, 1,
                 "cudaMalloc reference feature count") &&
        Allocate(recoveryLuma, kRecoveryKeyframeCount * analysisPixels,
                 "cudaMalloc recovery luma") &&
        Allocate(recoveryLevel1,
                 kRecoveryKeyframeCount *
                     static_cast<size_t>(level1Width) * level1Height,
                 "cudaMalloc recovery pyramid 1") &&
        Allocate(recoveryLevel2,
                 kRecoveryKeyframeCount *
                     static_cast<size_t>(level2Width) * level2Height,
                 "cudaMalloc recovery pyramid 2") &&
        Allocate(recoveryFeatures,
                 kRecoveryKeyframeCount * kMaximumPairs,
                 "cudaMalloc recovery features") &&
        Allocate(recoveryFeatureCounts, kRecoveryKeyframeCount,
                 "cudaMalloc recovery feature counts") &&
        Allocate(keyframeOrigins, kKeyframeCount,
                 "cudaMalloc keyframe origins") &&
        Allocate(keyframeValid, kKeyframeCount,
                 "cudaMalloc keyframe validity") &&
        Allocate(matchCandidates, kKeyframeCount,
                 "cudaMalloc match candidates") &&
        Allocate(pairs, kMaximumPairs, "cudaMalloc feature pairs") &&
        Allocate(pairCount, 1, "cudaMalloc feature pair count") &&
        Allocate(state, 1, "cudaMalloc stabilization state") &&
        Allocate(focusScore, 1, "cudaMalloc focus score") &&
        Allocate(bestFocusScore, 1, "cudaMalloc best focus score") &&
        Allocate(selectionFlag, 1, "cudaMalloc selection flag") &&
        Allocate(referenceAccumulator, analysisPixels,
                 "cudaMalloc reference accumulator") &&
        Allocate(referenceSampleCounts, analysisPixels,
                 "cudaMalloc reference sample counts") &&
        Allocate(currentColProjection, analysisWidth,
                 "cudaMalloc current column projection") &&
        Allocate(currentRowProjection, analysisHeight,
                 "cudaMalloc current row projection") &&
        Allocate(referenceColProjection, analysisWidth,
                 "cudaMalloc reference column projection") &&
        Allocate(referenceRowProjection, analysisHeight,
                 "cudaMalloc reference row projection");

    auto release = [&]() {
        cudaFree(referenceRowProjection);
        cudaFree(referenceColProjection);
        cudaFree(currentRowProjection);
        cudaFree(currentColProjection);
        cudaFree(referenceSampleCounts);
        cudaFree(referenceAccumulator);
        cudaFree(selectionFlag);
        cudaFree(bestFocusScore);
        cudaFree(focusScore);
        cudaFree(state);
        cudaFree(pairCount);
        cudaFree(pairs);
        cudaFree(matchCandidates);
        cudaFree(keyframeValid);
        cudaFree(keyframeOrigins);
        cudaFree(recoveryFeatureCounts);
        cudaFree(recoveryFeatures);
        cudaFree(recoveryLevel2);
        cudaFree(recoveryLevel1);
        cudaFree(recoveryLuma);
        cudaFree(referenceFeatureCount);
        cudaFree(referenceFeatures);
        cudaFree(referenceLevel2);
        cudaFree(referenceLevel1);
        cudaFree(currentLevel2);
        cudaFree(currentLevel1);
        cudaFree(referenceLuma);
        cudaFree(previousLuma);
        cudaFree(currentLuma);
        cudaFree(deviceOutput);
        cudaFree(deviceInput);
        if (stream) {
            cudaStreamDestroy(stream);
        }
    };

    if (!ok) {
        release();
        return 1;
    }

    openzoom::LaunchResetVirtualTripodState(state, false, stream);
    ok = CheckCuda(cudaMemsetAsync(bestFocusScore, 0, sizeof(float), stream),
                   "cudaMemsetAsync best focus score") &&
         CheckCuda(cudaMemsetAsync(
                       keyframeValid, 0,
                       kKeyframeCount * sizeof(unsigned int), stream),
                   "cudaMemsetAsync keyframe validity") &&
         CheckCuda(cudaMemsetAsync(
                       matchCandidates, 0,
                       kKeyframeCount *
                           sizeof(openzoom::TripodMatchCandidate),
                       stream),
                   "cudaMemsetAsync match candidates");

    const float zoomCropReserve =
        0.5f * (1.0f - 1.0f / options.zoom);
    const float ordinaryCorrectionFraction =
        std::clamp(std::max(0.06f, zoomCropReserve), 0.06f, 0.45f);
    const float tripodCorrectionFraction =
        ordinaryCorrectionFraction +
        options.strength * (0.45f - ordinaryCorrectionFraction);
    const float inlierThreshold =
        std::max(2.0f,
                 static_cast<float>(std::max(factorX, factorY)) /
                     options.zoom);

    trace << "frame,phase,accepted,pairs,reference_features,"
             "motion_dx,motion_dy,motion_angle,motion_log_scale,"
             "correction_dx,correction_dy,correction_angle,"
             "correction_log_scale,residual_mean_square,tripod_state,"
             "absolute_pairs,absolute_inliers,absolute_inlier_ratio,"
             "absolute_coverage,absolute_residual,absolute_accepted\n";
    trace << std::fixed << std::setprecision(6);

    const size_t level1Pixels =
        static_cast<size_t>(level1Width) * level1Height;
    const size_t level2Pixels =
        static_cast<size_t>(level2Width) * level2Height;
    auto keyframeLevel0 = [&](unsigned int slot) -> float* {
        return slot == 0
                   ? referenceLuma
                   : recoveryLuma + (slot - 1) * analysisPixels;
    };
    auto keyframeLevel1 = [&](unsigned int slot) -> float* {
        return slot == 0
                   ? referenceLevel1
                   : recoveryLevel1 + (slot - 1) * level1Pixels;
    };
    auto keyframeLevel2 = [&](unsigned int slot) -> float* {
        return slot == 0
                   ? referenceLevel2
                   : recoveryLevel2 + (slot - 1) * level2Pixels;
    };
    auto keyframeFeatures =
        [&](unsigned int slot) -> openzoom::TripodReferenceFeature* {
        return slot == 0
                   ? referenceFeatures
                   : recoveryFeatures +
                         (slot - 1) * kMaximumPairs;
    };
    auto keyframeFeatureCount = [&](unsigned int slot) -> unsigned int* {
        return slot == 0
                   ? referenceFeatureCount
                   : recoveryFeatureCounts + (slot - 1);
    };
    auto buildReference = [&](unsigned int slot) {
        openzoom::LaunchStabilizationLumaPyramid(
            keyframeLevel0(slot), analysisWidth, analysisHeight,
            keyframeLevel1(slot), level1Width, level1Height,
            keyframeLevel2(slot), level2Width, level2Height, stream);
        openzoom::LaunchPrepareVirtualTripodReference(
            keyframeLevel0(slot), analysisWidth, analysisHeight,
            keyframeLevel1(slot), level1Width, level1Height,
            keyframeLevel2(slot), level2Width, level2Height,
            keyframeFeatures(slot), keyframeFeatureCount(slot),
            kMaximumPairs, stream);
        if (slot == 0) {
            openzoom::LaunchStabilizationProjections(
                keyframeLevel0(slot), analysisWidth, analysisHeight,
                referenceColProjection, referenceRowProjection, stream);
        }
    };

    bool previousValid = false;
    unsigned int keyframeAdmissionCounter = 0;
    unsigned int nextRecoverySlot = 1;
    auto estimateMotion = [&]() {
        openzoom::LaunchStabilizationProjections(
            currentLuma, analysisWidth, analysisHeight,
            currentColProjection, currentRowProjection, stream);
        openzoom::LaunchVirtualTripodProjectionSeed(
            currentColProjection, currentRowProjection,
            referenceColProjection, referenceRowProjection,
            analysisWidth, analysisHeight,
            static_cast<float>(factorX), static_cast<float>(factorY),
            keyframeOrigins, keyframeValid, 0, state, stream);
        openzoom::LaunchStabilizationLumaPyramid(
            currentLuma, analysisWidth, analysisHeight,
            currentLevel1, level1Width, level1Height,
            currentLevel2, level2Width, level2Height, stream);
        CheckCuda(cudaMemsetAsync(
                      matchCandidates, 0,
                      kKeyframeCount *
                          sizeof(openzoom::TripodMatchCandidate),
                      stream),
                  "cudaMemsetAsync replay candidates");
        for (unsigned int slot = 0; slot < kKeyframeCount; ++slot) {
            openzoom::LaunchPreparedVirtualTripodKeyframePairs(
                currentLuma, analysisWidth, analysisHeight,
                currentLevel1, level1Width, level1Height,
                currentLevel2, level2Width, level2Height,
                keyframeLevel0(slot), keyframeLevel1(slot),
                keyframeLevel2(slot), keyframeFeatures(slot),
                keyframeFeatureCount(slot),
                static_cast<float>(factorX),
                static_cast<float>(factorY),
                options.width, options.height,
                pairs, pairCount, kMaximumPairs, state,
                keyframeOrigins, keyframeValid, slot,
                matchCandidates, stream);
            openzoom::LaunchVirtualTripodMatchCandidate(
                pairs, pairCount, kMaximumPairs,
                options.width, options.height, inlierThreshold,
                0.5f, 0.5f, options.zoom, keyframeOrigins,
                keyframeValid, slot, matchCandidates, stream);
        }
        openzoom::LaunchSelectVirtualTripodMatch(
            matchCandidates, kKeyframeCount, options.strength,
            tripodCorrectionFraction, options.zoom,
            options.width, options.height, state, stream);
        if (previousValid) {
            openzoom::LaunchStabilizationFeaturePairs(
                currentLuma, previousLuma, analysisWidth, analysisHeight,
                static_cast<float>(factorX),
                static_cast<float>(factorY), pairs, pairCount,
                kMaximumPairs, state, stream);
        } else {
            CheckCuda(cudaMemsetAsync(pairCount, 0, sizeof(unsigned int),
                                      stream),
                      "cudaMemsetAsync replay fallback count");
        }
        openzoom::LaunchVirtualTripodRelativeFallback(
            pairs, pairCount, kMaximumPairs,
            options.width, options.height,
            1.5f * static_cast<float>(std::max(factorX, factorY)),
            tripodCorrectionFraction, state, stream);

        ++keyframeAdmissionCounter;
        if (keyframeAdmissionCounter >= kKeyframeAdmissionFrames) {
            keyframeAdmissionCounter = 0;
            openzoom::LaunchCaptureVirtualTripodKeyframe(
                currentLuma, keyframeLevel0(nextRecoverySlot),
                static_cast<int>(analysisPixels), state,
                keyframeOrigins, keyframeValid, nextRecoverySlot,
                kKeyframeMinimumValidFrames, stream);
            buildReference(nextRecoverySlot);
            nextRecoverySlot =
                nextRecoverySlot >= kKeyframeCount - 1
                    ? 1
                    : nextRecoverySlot + 1;
        }
    };

    int frame = 0;
    int accumulationFrame = 0;
    while (ok) {
        bool reachedEnd = false;
        if (options.syntheticClampBump) {
            if (frame >= kSyntheticFrameCount) {
                break;
            }
            if (!GenerateSyntheticClampBumpFrame(
                    frame, options.width, options.height, hostInput)) {
                std::cerr
                    << "The synthetic clamp-bump fixture requires 160x90.\n";
                ok = false;
                break;
            }
        } else {
            if (!ReadInputFrame(input, inputIsY4m, yPlane, chroma, hostInput,
                                reachedEnd)) {
                ok = false;
                break;
            }
            if (reachedEnd) {
                break;
            }
        }

        ok = CheckCuda(
            cudaMemcpy2DAsync(
                deviceInput, inputPitch, hostInput.data(),
                static_cast<size_t>(options.width) * sizeof(uchar4),
                static_cast<size_t>(options.width) * sizeof(uchar4),
                options.height, cudaMemcpyHostToDevice, stream),
            "cudaMemcpy2DAsync replay input");
        if (!ok) {
            break;
        }

        openzoom::LaunchStabilizationLumaDownsample(
            currentLuma, analysisWidth, analysisHeight, deviceInput,
            inputPitch, options.width, options.height, factorX, factorY,
            stream);

        const char* phase = "locked";
        if (frame < kFocusSelectionFrames) {
            phase = "selecting";
            openzoom::LaunchMeasureVirtualTripodFocus(
                currentLuma, analysisWidth, analysisHeight, focusScore, stream);
            openzoom::LaunchSelectSharperVirtualTripodReference(
                currentLuma, referenceLuma, static_cast<int>(analysisPixels),
                focusScore, bestFocusScore, selectionFlag, stream);
            if (frame + 1 == kFocusSelectionFrames) {
                buildReference(0);
                openzoom::LaunchInitializeVirtualTripodKeyframe(
                    keyframeOrigins, keyframeValid, 0, stream);
                openzoom::LaunchInitializeVirtualTripodAccumulator(
                    referenceLuma, referenceAccumulator,
                    referenceSampleCounts, static_cast<int>(analysisPixels),
                    stream);
            }
        } else if (accumulationFrame <
                   options.referenceAccumulationFrames) {
            phase = "accumulating";
            estimateMotion();
            openzoom::LaunchMeasureVirtualTripodFocus(
                currentLuma, analysisWidth, analysisHeight, focusScore, stream);
            openzoom::LaunchAccumulateVirtualTripodReference(
                currentLuma, referenceLuma, analysisWidth, analysisHeight,
                static_cast<float>(factorX), static_cast<float>(factorY),
                focusScore, bestFocusScore, state, referenceAccumulator,
                referenceSampleCounts, stream);
            ++accumulationFrame;
            if (accumulationFrame ==
                options.referenceAccumulationFrames) {
                openzoom::LaunchFinalizeVirtualTripodReference(
                    referenceLuma, referenceAccumulator,
                    referenceSampleCounts, static_cast<int>(analysisPixels),
                    stream);
                buildReference(0);
                openzoom::LaunchInitializeVirtualTripodKeyframe(
                    keyframeOrigins, keyframeValid, 0, stream);
            }
        } else {
            estimateMotion();
        }

        openzoom::LaunchStabilizationWarp(
            deviceOutput, outputPitch, deviceInput, inputPitch,
            options.width, options.height, state, stream);
        ok = ok && CheckCuda(
            cudaMemcpyAsync(previousLuma, currentLuma,
                            analysisPixels * sizeof(float),
                            cudaMemcpyDeviceToDevice, stream),
            "cudaMemcpyAsync replay previous luma");
        previousValid = true;

        openzoom::StabilizationState measuredState{};
        unsigned int measuredPairCount = 0;
        unsigned int measuredReferenceFeatureCount = 0;
        std::array<openzoom::TripodMatchCandidate, kKeyframeCount>
            measuredCandidates{};
        ok = CheckCuda(
                 cudaMemcpy2DAsync(
                     hostOutput.data(),
                     static_cast<size_t>(options.width) * sizeof(uchar4),
                     deviceOutput, outputPitch,
                     static_cast<size_t>(options.width) * sizeof(uchar4),
                     options.height, cudaMemcpyDeviceToHost, stream),
                 "cudaMemcpy2DAsync replay output") &&
             CheckCuda(cudaMemcpyAsync(
                           &measuredState, state, sizeof(measuredState),
                           cudaMemcpyDeviceToHost, stream),
                       "cudaMemcpyAsync replay state") &&
             CheckCuda(cudaMemcpyAsync(
                           &measuredPairCount, pairCount,
                           sizeof(measuredPairCount), cudaMemcpyDeviceToHost,
                           stream),
                       "cudaMemcpyAsync replay pair count") &&
             CheckCuda(cudaMemcpyAsync(
                           &measuredReferenceFeatureCount,
                           referenceFeatureCount,
                           sizeof(measuredReferenceFeatureCount),
                           cudaMemcpyDeviceToHost, stream),
                       "cudaMemcpyAsync replay reference feature count") &&
             CheckCuda(cudaMemcpyAsync(
                           measuredCandidates.data(), matchCandidates,
                           sizeof(measuredCandidates), cudaMemcpyDeviceToHost,
                           stream),
                       "cudaMemcpyAsync replay candidates") &&
             CheckCuda(cudaStreamSynchronize(stream),
                       "cudaStreamSynchronize replay frame");
        if (!ok) {
            break;
        }

        output.write(reinterpret_cast<const char*>(hostOutput.data()),
                     static_cast<std::streamsize>(frameBytes));
        if (!output) {
            std::cerr << "Could not write replay output frame.\n";
            ok = false;
            break;
        }

        trace << frame << ',' << phase << ','
              << (measuredState.diagnostics.y >= 0.5f ? 1 : 0) << ','
              << measuredPairCount << ',' << measuredReferenceFeatureCount
              << ',' << measuredState.lastFrameMotion.x << ','
              << measuredState.lastFrameMotion.y << ','
              << measuredState.lastFrameMotion.z << ','
              << measuredState.lastFrameMotion.w << ','
              << measuredState.correction.x << ','
              << measuredState.correction.y << ','
              << measuredState.correction.z << ','
              << measuredState.correction.w << ','
              << measuredState.diagnostics.z << ','
              << measuredState.tripodDiagnostics.z << ','
              << measuredCandidates[0].statistics.x << ','
              << measuredCandidates[0].diagnostics.x << ','
              << measuredCandidates[0].statistics.y << ','
              << measuredCandidates[0].statistics.z << ','
              << measuredCandidates[0].diagnostics.z << ','
              << (measuredCandidates[0].diagnostics.y >= 0.5f ? 1 : 0)
              << '\n';
        ++frame;
    }

    release();
    if (!ok || frame == 0) {
        return 1;
    }
    std::cout << "Replayed " << frame << " frames at " << options.width << 'x'
              << options.height << "; analysis " << analysisWidth << 'x'
              << analysisHeight << ", zoom " << options.zoom << "x, strength "
              << options.strength << ", correction reserve "
              << tripodCorrectionFraction * 100.0f << "%.\n";
    return 0;
}
