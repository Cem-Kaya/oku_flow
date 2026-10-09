#pragma once

#ifdef _WIN32

#include "okuflow/common/yuv_color.hpp"

#include <cuda_runtime_api.h>

#include <cstdint>

namespace okuflow {

void LaunchGradientKernel(cudaSurfaceObject_t surface, int width, int height, float timeSeconds);
void LaunchBlackWhiteKernel(cudaSurfaceObject_t surface, int width, int height, float threshold);
void LaunchZoomKernel(cudaSurfaceObject_t surface, int width, int height, float zoomAmount);

void LaunchBlackWhiteLinear(uchar4* dst, size_t dstPitchBytes,
                            const uchar4* src, size_t srcPitchBytes,
                            int width, int height, float threshold, cudaStream_t stream);

void LaunchZoomLinear(uchar4* dst, size_t dstPitchBytes,
                      const uchar4* src, size_t srcPitchBytes,
                      int width, int height,
                      float zoomAmount, float centerXNorm, float centerYNorm,
                      cudaStream_t stream);

void LaunchBlendLinear(uchar4* dst, size_t dstPitchBytes,
                       const uchar4* base, size_t basePitchBytes,
                       const uchar4* enhanced, size_t enhancedPitchBytes,
                       int width, int height, float strength,
                       cudaStream_t stream);

void LaunchGaussianBlurLinear(uchar4* dst, size_t dstPitchBytes,
                              uchar4* scratch, size_t scratchPitchBytes,
                              const uchar4* src, size_t srcPitchBytes,
                              int width, int height,
                              cudaStream_t stream);

void LaunchFocusMarkerLinear(uchar4* buffer, size_t pitchBytes,
                             int width, int height,
                             float centerXNorm, float centerYNorm,
                             cudaStream_t stream);

void LaunchFsrEasuRcasLinear(uchar4* dst, size_t dstPitchBytes,
                             uchar4* scratch, size_t scratchPitchBytes,
                             const uchar4* src, size_t srcPitchBytes,
                             int srcWidth, int srcHeight,
                             int dstWidth, int dstHeight,
                             float sharpness,
                             cudaStream_t stream);

void LaunchNisLinear(uchar4* dst, size_t dstPitchBytes,
                     const uchar4* src, size_t srcPitchBytes,
                     int srcWidth, int srcHeight,
                     int dstWidth, int dstHeight,
                     float sharpness,
                     cudaStream_t stream);

// Replicate the last valid row/column into a one-texel guard when a cache
// occupies only the allocation's top-left region (linear sampler footprint).
void PadSpatialCacheBorder(cudaArray_t cache,
                           const uchar4* output, size_t outputPitchBytes,
                           unsigned int outputWidth, unsigned int outputHeight,
                           unsigned int cacheWidth, unsigned int cacheHeight,
                           cudaStream_t stream);

void LaunchTemporalSmoothLinear(uchar4* dst, size_t dstPitchBytes,
                                const uchar4* src, size_t srcPitchBytes,
                                float4* history, size_t historyPitchBytes,
                                int width, int height,
                                float alpha,
                                bool historyValid,
                                cudaStream_t stream);

// Device-resident state for fixed-reference stabilization. Components are x/y
// translation in full-resolution pixels, rotation in radians, and logarithmic
// scale.
struct StabilizationState {
    float4 actualPath;
    float4 filteredPath;
    float4 correction;
    float4 previousCorrection;
    float4 lastFrameMotion;
    // x = inlier count, y = similarity model valid, z = mean squared
    // residual, w = estimator tag (1=RTX flow, 2=features, 3=projection,
    // 4=Virtual Tripod).
    float4 diagnostics;
    // Correction carried across a continuity-preserving Virtual Tripod
    // re-lock. A new reference starts at zero motion but must retain the last
    // displayed coordinate system.
    float4 tripodAnchorCorrection;
    // Last accepted absolute correction plus a bounded relative correction
    // used only during short fixed-reference dropouts. The relative term is
    // never folded into a keyframe origin and is cleared on re-acquisition.
    float4 tripodAbsoluteCorrection;
    float4 tripodRelativeMotion;
    // x = consecutive valid frames, y = consecutive invalid frames,
    // z = state (0=off/seeding, 1=locked, 2=relative fallback,
    // 3=crop-limited, 4=recovered through keyframe map),
    // w = number of anchor captures.
    float4 tripodDiagnostics;
};

// Fixed-reference data prepared once when Virtual Tripod locks. Each entry
// stores a sub-pixel point in level-0 analysis coordinates plus the inverse
// translation Hessian for the 1x, 1/2x, and 1/4x Gaussian pyramid levels.
struct TripodReferenceFeature {
    float2 position;
    float4 inverseHessian[3];
};

// One fixed-reference registration result. Motion is expressed in the common
// coordinate system of the original tripod lock. Statistics are:
// x = pair count, y = inlier ratio, z = inlier spatial coverage,
// w = keyframe index.
struct TripodMatchCandidate {
    float4 motion;
    float4 diagnostics;
    float4 statistics;
};

// Device-resident presentation policy for mounted-camera bump suppression.
// `diagnostics` is x=mode (0=tracking, 1=holding, 2=recovering),
// y=current/reference sharpness ratio, z=absolute-transform step in source
// pixels, and w=readiness (0=unavailable, 1=preparing, 2=safe frame ready).
struct BumpHoldState {
    float4 diagnostics;
    float4 previousMotion;
    unsigned int heldFrameValid;
    unsigned int stableFrames;
    unsigned int recoveryFrame;
    unsigned int captureCurrent;
    float blendAlpha;
    float reserved[3];
};

void LaunchStabilizationLumaDownsample(float* dstLuma,
                                       int smallWidth, int smallHeight,
                                       const uchar4* src, size_t srcPitchBytes,
                                       int width, int height,
                                       int factorX, int factorY,
                                       cudaStream_t stream);

void LaunchStabilizationProjections(const float* luma,
                                    int smallWidth, int smallHeight,
                                    float* colProj, float* rowProj,
                                    cudaStream_t stream);

// Re-seeds a rejected fixed-reference tracker from a wide-range, zero-mean
// projection correlation. This changes only the next LK prediction; it never
// updates the displayed correction or admits a new anchor by itself.
void LaunchVirtualTripodProjectionSeed(
    const float* currentColProj, const float* currentRowProj,
    const float* referenceColProj, const float* referenceRowProj,
    int smallWidth, int smallHeight, float factorX, float factorY,
    const float4* keyframeOrigins, const unsigned int* keyframeValid,
    unsigned int keyframeIndex, StabilizationState* state,
    cudaStream_t stream);

void LaunchStabilizationFeaturePairs(const float* currentLuma,
                                     const float* previousLuma,
                                     int smallWidth, int smallHeight,
                                     float factorX, float factorY,
                                     float4* pairs, unsigned int* pairCount,
                                     unsigned int maxPairs,
                                     const StabilizationState* gateState,
                                     cudaStream_t stream);

// Track a fixed scene reference with a wider absolute-motion gate than the
// pairwise stabilizer. The returned pairs remain reference -> current.
void LaunchVirtualTripodFeaturePairs(const float* currentLuma,
                                     const float* referenceLuma,
                                     int smallWidth, int smallHeight,
                                     float factorX, float factorY,
                                     int fullWidth, int fullHeight,
                                     float4* pairs, unsigned int* pairCount,
                                     unsigned int maxPairs,
                                     const StabilizationState* motionSeedState,
                                     cudaStream_t stream);

void LaunchStabilizationLumaPyramid(const float* level0,
                                    int level0Width, int level0Height,
                                    float* level1,
                                    int level1Width, int level1Height,
                                    float* level2,
                                    int level2Width, int level2Height,
                                    cudaStream_t stream);

void LaunchPrepareVirtualTripodReference(
    const float* referenceLevel0, int level0Width, int level0Height,
    const float* referenceLevel1, int level1Width, int level1Height,
    const float* referenceLevel2, int level2Width, int level2Height,
    TripodReferenceFeature* features, unsigned int* featureCount,
    unsigned int maxFeatures, cudaStream_t stream);

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
    const StabilizationState* motionSeedState, cudaStream_t stream);

// Keyframe-map variant. The common-coordinate origin is subtracted from the
// last accepted absolute motion when seeding LK. Later keyframes may be
// skipped on-device after an earlier candidate succeeds.
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
    cudaStream_t stream);

// Lock-time reference builder. Focus is measured on-device, the sharpest
// candidate wins without a host synchronization, then accepted registered
// frames are averaged with per-pixel motion/outlier rejection.
void LaunchMeasureVirtualTripodFocus(const float* luma, int width, int height,
                                     float* focusScore, cudaStream_t stream);
void LaunchResetBumpHoldState(BumpHoldState* state, cudaStream_t stream);
void LaunchUpdateBumpHoldState(
    BumpHoldState* bumpState, const StabilizationState* stabilizationState,
    const float* currentFocusScore, const float* referenceFocusScore,
    bool referenceReady, int fullWidth, int fullHeight,
    float motionEnterPixels, float motionExitPixels, cudaStream_t stream);
void LaunchApplyBumpHold(
    uchar4* current, size_t currentPitchBytes,
    uchar4* held, size_t heldPitchBytes,
    int width, int height, const BumpHoldState* state,
    cudaStream_t stream);
void LaunchSelectSharperVirtualTripodReference(
    const float* candidate, float* reference, int pixelCount,
    const float* focusScore, float* bestFocusScore,
    unsigned int* selectionFlag, cudaStream_t stream);
void LaunchInitializeVirtualTripodAccumulator(
    const float* reference, float* accumulator, unsigned int* sampleCounts,
    int pixelCount, cudaStream_t stream);
void LaunchAccumulateVirtualTripodReference(
    const float* current, const float* reference, int width, int height,
    float factorX, float factorY,
    const float* focusScore, const float* bestFocusScore,
    const StabilizationState* state,
    float* accumulator, unsigned int* sampleCounts, cudaStream_t stream);
void LaunchFinalizeVirtualTripodReference(
    float* reference, const float* accumulator,
    const unsigned int* sampleCounts, int pixelCount, cudaStream_t stream);

// Reset path/filter history for a newly captured fixed reference. When
// preserveCorrection is true, the current visual correction becomes the new
// anchor offset so re-locking never jumps.
void LaunchResetVirtualTripodState(StabilizationState* state,
                                  bool preserveCorrection,
                                  cudaStream_t stream);

// Solve reference-to-current motion without integrating pairwise deltas.
// Rejected models freeze the last good correction instead of drifting or
// snapping to identity.
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
                                           cudaStream_t stream);

// Estimate one fixed-reference candidate without changing the displayed path.
// SelectVirtualTripodMatch later chooses the best accepted candidate and
// updates the path exactly once.
void LaunchVirtualTripodMatchCandidate(
    const float4* pairs, const unsigned int* pairCount,
    unsigned int maxPairs, int fullWidth, int fullHeight,
    float inlierThresholdPixels, float focusCenterX, float focusCenterY,
    float zoomAmount, const float4* keyframeOrigins,
    const unsigned int* keyframeValid, unsigned int keyframeIndex,
    TripodMatchCandidate* candidates, cudaStream_t stream);

void LaunchSelectVirtualTripodMatch(
    const TripodMatchCandidate* candidates, unsigned int candidateCount,
    float strength, float maxCorrectionFraction, float zoomAmount,
    int fullWidth, int fullHeight, StabilizationState* state,
    cudaStream_t stream);

// Capture a recovery keyframe only after the current frame has a sustained,
// accepted absolute registration. The keyframe inherits that accepted
// common-coordinate motion, preventing chained-delta drift.
void LaunchCaptureVirtualTripodKeyframe(
    const float* current, float* destination, int pixelCount,
    const StabilizationState* state, float4* keyframeOrigins,
    unsigned int* keyframeValid, unsigned int keyframeIndex,
    unsigned int minimumValidFrames, cudaStream_t stream);

void LaunchInitializeVirtualTripodKeyframe(
    float4* keyframeOrigins, unsigned int* keyframeValid,
    unsigned int keyframeIndex, cudaStream_t stream);

// Short-lived pairwise correction used only while every absolute keyframe is
// rejected. It is bounded and reset immediately on absolute re-acquisition.
void LaunchVirtualTripodRelativeFallback(
    const float4* pairs, const unsigned int* pairCount,
    unsigned int maxPairs, int fullWidth, int fullHeight,
    float inlierThresholdPixels, float maxCorrectionFraction,
    StabilizationState* state, cudaStream_t stream);

void LaunchStabilizationWarp(uchar4* dst, size_t dstPitchBytes,
                             const uchar4* src, size_t srcPitchBytes,
                             int width, int height,
                             const StabilizationState* state,
                             cudaStream_t stream);

// autoContrastLevels: device float2 (x=lo, y=hi, normalized 0..1) written by
// LaunchAutoContrastAnalysis; pass nullptr to disable the auto-contrast remap.
void LaunchDisplayColorGradeLinear(uchar4* buffer, size_t pitchBytes,
                                   int width, int height,
                                   int colorTransform, float contrast, float brightness,
                                   const float2* autoContrastLevels,
                                   float autoContrastStrength,
                                   cudaStream_t stream);

// Metadata-selected BT.601/709 limited/full YUV -> BGRA; shared fixed-point
// equations match the CPU converters, including footroom and clipping.
void LaunchNv12ToBgraLinear(uchar4* dst, size_t dstPitchBytes,
                            const unsigned char* yPlane, size_t yPitchBytes,
                            const unsigned char* uvPlane, size_t uvPitchBytes,
                            int width, int height,
                            cudaStream_t stream, YuvColorInfo color = {});

void LaunchYuy2ToBgraLinear(uchar4* dst, size_t dstPitchBytes,
                            const unsigned char* src, size_t srcPitchBytes,
                            int width, int height,
                            cudaStream_t stream, YuvColorInfo color = {});

// Rotate by quarterTurnsClockwise in {1,2,3}. For 1 and 3 the destination is
// srcHeight x srcWidth; for 2 it matches the source extent.
void LaunchRotateQuarterLinear(uchar4* dst, size_t dstPitchBytes,
                               const uchar4* src, size_t srcPitchBytes,
                               int srcWidth, int srcHeight,
                               int quarterTurnsClockwise,
                               cudaStream_t stream);

// homography maps OUTPUT pixel coordinates to SOURCE pixel coordinates
// (row-major 3x3, uploaded per launch as kernel arguments). Samples bilinearly,
// black outside the source rect.
void LaunchKeystoneWarp(uchar4* dst, size_t dstPitchBytes,
                        const uchar4* src, size_t srcPitchBytes,
                        int width, int height,
                        const float homography[9],
                        cudaStream_t stream);

// Zeroes histogram256 (async) then accumulates a 256-bin luma histogram using
// shared-memory per-block histograms merged with global atomics.
void LaunchAutoContrastHistogram(unsigned int* histogram256,
                                 const uchar4* src, size_t srcPitchBytes,
                                 int width, int height,
                                 cudaStream_t stream);

// Single tiny block: derives the 2nd/98th percentile levels from the histogram
// and low-passes them into `levels` (device float2, normalized 0..1). With
// levelsValid false the new measurement seeds the state directly.
void LaunchAutoContrastAnalysis(const unsigned int* histogram256,
                                int pixelCount,
                                float2* levels,
                                bool levelsValid,
                                cudaStream_t stream);

// Text-clarity kernels share a full-resolution float workspace and byte masks.
// `analysis` is device-resident: x=auto light-text flag, y=scene class
// (0=mixed, 1=paper, 2=board), z=mean luma x10000, w=contrast x10000.
void LaunchTextLocalStatistics(float* luma, size_t floatPitchBytes,
                               float* horizontal, float* mean,
                               float* sqHorizontal, float* sqMean,
                               const uchar4* src, size_t srcPitchBytes,
                               int width, int height, int radius,
                               cudaStream_t stream);
void LaunchTextSceneAnalysis(const unsigned int* histogram256, int pixelCount,
                             int4* analysis, cudaStream_t stream);
void LaunchBackgroundFlattenLinear(uchar4* dst, size_t dstPitchBytes,
                                   const uchar4* src, size_t srcPitchBytes,
                                   const float* luma, const float* mean,
                                   size_t floatPitchBytes,
                                   int width, int height, float strength,
                                   bool suppressGlare, float glareStrength,
                                   const int4* analysis,
                                   cudaStream_t stream);
void LaunchClaheLinear(uchar4* dst, size_t dstPitchBytes,
                       const uchar4* src, size_t srcPitchBytes,
                       unsigned int* tileHistograms, float* tileMaps,
                       int width, int height, float clipLimit,
                       cudaStream_t stream);
void LaunchSauvolaMask(unsigned char* mask, size_t maskPitchBytes,
                       const float* luma, const float* mean, const float* sqMean,
                       size_t floatPitchBytes, int width, int height,
                       float strength, float softness, int polarityMode,
                       const int4* analysis, cudaStream_t stream);
void LaunchStrokeWeight(unsigned char* dst, size_t dstPitchBytes,
                        const unsigned char* src, size_t srcPitchBytes,
                        int width, int height, int strokeWeight,
                        cudaStream_t stream);
void LaunchTextMaskHysteresis(unsigned char* mask, size_t maskPitchBytes,
                              unsigned char* history, int width, int height,
                              float strength, bool historyValid,
                              cudaStream_t stream);
void LaunchTextMaskComposite(uchar4* dst, size_t dstPitchBytes,
                             const uchar4* src, size_t srcPitchBytes,
                             const unsigned char* mask, size_t maskPitchBytes,
                             int width, int height,
                             std::uint32_t foregroundBgra,
                             std::uint32_t backgroundBgra,
                             int compositeMode, const int4* analysis,
                             cudaStream_t stream);
void LaunchSmartSharpenLinear(uchar4* dst, size_t dstPitchBytes,
                              uchar4* scratch, size_t scratchPitchBytes,
                              const uchar4* src, size_t srcPitchBytes,
                              const unsigned char* mask, size_t maskPitchBytes,
                              int width, int height, float strength,
                              bool selective, cudaStream_t stream);
void LaunchFocusMetric(const float* luma, size_t floatPitchBytes,
                       int width, int height, float2* stats,
                       cudaStream_t stream);

bool UploadGaussianKernel(int radius, float sigma, cudaStream_t stream);
bool UploadDisplayColorLut(const std::uint32_t* lut256, cudaStream_t stream);

} // namespace okuflow

#endif // _WIN32
