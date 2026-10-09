#pragma once

#ifdef _WIN32

#include "okuflow/common/yuv_color.hpp"

#include <wrl/client.h>
#include <array>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "okuflow/common/spatial_cache_geometry.hpp"

#ifndef OKUFLOW_HAS_CUDA_EXT_MEMORY
#  if defined(__has_include)
#    if __has_include(<cuda_runtime.h>)
#      define OKUFLOW_HAS_CUDA_EXT_MEMORY 1
#    else
#      define OKUFLOW_HAS_CUDA_EXT_MEMORY 0
#    endif
#  else
#    define OKUFLOW_HAS_CUDA_EXT_MEMORY 1
#  endif
#endif

#if OKUFLOW_HAS_CUDA_EXT_MEMORY
#  include <cuda.h>
#  include <cuda_runtime_api.h>
#  if defined(__has_include)
#    if __has_include(<cuda_runtime.h>)
#      include <cuda_runtime.h>
#    endif
#  endif
#endif

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12Fence;
struct ID3D11Texture2D;

namespace okuflow {

class MaxineSuperRes;

struct FenceSyncParams {
    bool enable{false};
    uint64_t waitValue{0};
    uint64_t signalValue{0};
};

struct KeystoneTrackingState {
    bool paused{false};
    bool canStepBack{false};
    bool canStepForward{false};
    bool stepPending{false};
    int position{0};
    int count{0};
};

struct SuperResRoiMetadata {
    bool valid{false};
    std::uint64_t generation{};
    float sourceX{};
    float sourceY{};
    float sourceWidth{};
    float sourceHeight{};
    unsigned int outputWidth{};
    unsigned int outputHeight{};
    float scaleFactor{};
};

struct GpuStageTimings {
    float inputMs{-1.0f};
    float geometryMs{-1.0f};
    float effectsMs{-1.0f};
    float outputMs{-1.0f};
    std::uint64_t generation{0};

    bool IsValid() const {
        return generation > 0 && inputMs >= 0.0f &&
               geometryMs >= 0.0f && effectsMs >= 0.0f &&
               outputMs >= 0.0f;
    }
};

enum class SpatialUpscaler : int {
    kFsrEasuRcas = 0,
    kNis = 1,
};

enum class CudaBufferFormat : int {
    kRgba8 = 0,
    kRgba16F = 1,
};

enum class DisplayColorTransform : int {
    kNone = 0,
    kInvert = 1,
    kLumaLut = 2,
};

struct ProcessingSettings {
    bool enableBlackWhite{false};
    float blackWhiteThreshold{0.5f};
    bool enableZoom{false};             // Apply legacy image zoom in CUDA.
    // Viewing magnification also drives stabilization and the SuperRes ROI
    // when presentation owns zoom and enableZoom is false.
    float zoomAmount{1.0f};
    float zoomCenterX{0.5f};
    float zoomCenterY{0.5f};
    bool enableBlur{false};
    int blurRadius{3};
    float blurSigma{1.0f};
    bool drawFocusMarker{false};
    bool enableSpatialSharpen{false};
    SpatialUpscaler spatialUpscaler{SpatialUpscaler::kNis};
    float spatialSharpness{0.2f};
    ViewTransform spatialViewTransform{};
    unsigned int spatialViewportWidth{};
    unsigned int spatialViewportHeight{};
    CudaBufferFormat stagingFormat{CudaBufferFormat::kRgba8};
    bool enableTemporalSmoothing{false};
    float temporalSmoothingAlpha{0.25f};
    bool enableStabilization{false};
    bool enableBumpHold{false};           // Extra Stable hold-last-sharp-frame policy
    DisplayColorTransform displayColorTransform{DisplayColorTransform::kNone};
    const std::uint32_t* displayColorLut{}; // host-owned 256-entry BGRA LUT
    std::uint64_t displayColorLutGeneration{};
    std::uint32_t textForegroundBgra{0xffffffffu};
    std::uint32_t textBackgroundBgra{0xff000000u};
    float contrast{1.0f};                // 0.25..4.0, 1 = neutral
    float brightness{0.0f};              // -1..1, 0 = neutral
    bool enableKeystone{false};          // auto-detect projected slide quad, warp fronto-parallel
    bool enableAutoContrast{false};      // percentile-based level stretch before contrast/brightness
    float autoContrastStrength{0.7f};    // 0..1, blend toward full stretch
    bool enableAutoTextClarity{false};
    bool enableBackgroundFlatten{false};
    float backgroundFlattenStrength{0.8f};
    bool enableAdaptiveBinarization{false};
    float sauvolaStrength{0.28f};
    float binarizationSoftness{0.06f};
    int textPolarityMode{0};             // 0=auto, 1=dark-on-light, 2=light-on-dark
    int strokeWeight{0};                 // -3=thin through +3=bold
    bool enableSmartSharpen{false};
    float smartSharpenStrength{0.45f};
    bool enableClahe{false};
    float claheClipLimit{2.0f};
    bool enableTwoColorText{false};
    bool enableTextHysteresis{false};
    float textHysteresisStrength{0.08f};
    bool enableSelectiveSharpen{false};
    bool enableFocusDetection{false};
    float focusThreshold{0.012f};
    bool enableGlareSuppression{false};
    float glareSuppressionStrength{0.5f};
    bool enableMlSuperRes{false};
    float mlSuperResStrength{0.65f};
    bool mlSuperResUltra1440p{false};

    float EffectiveViewingMagnification() const {
        return std::isfinite(zoomAmount) && zoomAmount > 1.0f
                   ? zoomAmount : 1.0f;
    }
};

// Input frame description.
//
// width/height are ALWAYS the dimensions of the host pixel data as laid out in
// memory (i.e. pre-rotation). When inputFormat != 0 and rotationQuarterTurns is
// odd (90/270), the GPU rotates after conversion, so the interop surface (and
// every processing stage) runs at the POST-rotation dimensions
// (height x width). The caller must create the surface at the post-rotation
// size; ProcessFrame validates that.
struct ProcessingInput {
    const void* hostPixels{nullptr};
    unsigned int hostStrideBytes{0};
    unsigned int pixelSizeBytes{0};
    unsigned int width{0};
    unsigned int height{0};
    int inputFormat{0};                   // 0=BGRA8 (existing), 1=NV12, 2=YUY2
    YuvColorInfo yuvColor{};              // Used only for raw YUV conversion.
    const void* hostPlane2{nullptr};      // NV12 UV plane (nullptr otherwise)
    unsigned int hostPlane2StrideBytes{0};
    int rotationQuarterTurns{0};          // 0..3 clockwise, applied on GPU after conversion.
                                          // Ignored for inputFormat 0 (CPU already rotated BGRA).
    // Optional device-resident BGRA source produced by the accelerated camera
    // path. When present, hostPixels/strides/planes are ignored and CUDA maps
    // this D3D11 resource directly before running the existing stages.
    ID3D11Texture2D* d3d11Texture{nullptr};
    // Prevents capture from overwriting the converted texture until CUDA
    // observes copy completion; also retains its capture session backing.
    std::shared_ptr<void> d3d11TextureLease;
    unsigned int d3d11Subresource{0};
    // Publish the converted, post-rotation frame to the optional original
    // recording surface before stabilization or any visual effects.
    bool publishOriginalFrame{false};
};

#if OKUFLOW_HAS_CUDA_EXT_MEMORY
struct StabilizationState;
struct TripodReferenceFeature;
struct TripodMatchCandidate;
struct BumpHoldState;

class CudaInteropSurface {
public:
    explicit CudaInteropSurface(ID3D12Resource* texture,
                                ID3D12Resource* superResTexture = nullptr,
                                ID3D12Fence* sharedFence = nullptr,
                                ID3D12Resource* originalTexture = nullptr);
    ~CudaInteropSurface();

    bool IsValid() const { return valid_; }
    bool IsFaulted() const { return streamFaulted_; }
    bool HasExternalSemaphore() const { return externalSemaphore_ != nullptr; }

    void RunGradientDemoKernel(unsigned int width, unsigned int height, float timeSeconds);

    bool ProcessFrame(const ProcessingInput& input,
                      const ProcessingSettings& settings,
                      const FenceSyncParams& fenceSync);
    const std::string& LastError() const { return lastError_; }
    bool LastFailureWasCaptureInterop() const {
        return captureInteropFailed_;
    }
    void ResetTemporalHistory();
    void ResetStabilization();
    void ResetKeystone();
    void SetKeystoneTrackingPaused(bool paused);
    bool StepKeystoneCorrection(int direction);
    KeystoneTrackingState GetKeystoneTrackingState() const;
    void ResetTextClarityHistory();
    bool HasFocusScore() const { return focusScoreValid_; }
    float LatestFocusScore() const { return latestFocusScore_; }
    bool IsFocusAcceptable(float threshold) const {
        return !focusScoreValid_ || latestFocusScore_ >= threshold;
    }
    const std::string& SuperResStatus() const { return superResStatus_; }
    bool IsSuperResActive() const { return superResActive_; }
    // Valid while IsSuperResActive(): the crop fed to the AI stage and the
    // fixed scale factor it runs at (residual zoom is applied by the sampler).
    unsigned int SuperResSourceWidth() const { return superResSourceWidth_; }
    unsigned int SuperResSourceHeight() const { return superResSourceHeight_; }
    float SuperResFactor() const { return superResFactorValue_; }
    // Shared presentation cache: valid for either Maxine or spatial upscale.
    // outputWidth/Height can occupy only the allocation's top-left rectangle.
    SuperResRoiMetadata SuperResRoi() const { return superResRoi_; }
    bool IsSuperResPerformanceLimited() const { return superResAutoDisabled_; }
    float SuperResAverageMs() const { return superResLastAverageMs_; }
    void SetSuperResPerformanceOverride(bool enabled);
    void ResetSuperRes();
    // Releases per-camera imports only after a nonblocking copy-completion
    // probe. Pending/failed copies retain imports and their producer lease.
    // atProcessExit retains the isolated legacy-driver registration fallback.
    void ResetCaptureInterop(bool atProcessExit = false);
    // Producer shutdown failed: retain the entire import and producer lease
    // until process exit without calling either driver or allocating storage.
    void AbandonCaptureInterop() noexcept;
    // Nonblocking source-copy poll. False means keep the producer lease and
    // retry later; !IsValid() distinguishes terminal failure from ordinary busy.
    bool PollCaptureCopy();
    // Bounded full-stream drain. False forbids destroying/reusing this surface.
    bool WaitForIdle() noexcept;
    const std::string& StabilizerStatus() const { return stabilizerStatus_; }
    float LastStabilizerMs() const { return lastStabilizerMs_; }

    // P8 GPU timing (plan 11 Wave 1): duration of the last sampled ProcessFrame
    // kernel chain in milliseconds, or a negative value while no sample exists.
    // Sampled every 30th frame with cudaEvents; queries never block.
    float LastGpuFrameMs() const { return lastGpuFrameMs_; }
    GpuStageTimings LastGpuStageTimings() const {
        return lastGpuStageTimings_;
    }

    CudaInteropSurface(const CudaInteropSurface&) = delete;
    CudaInteropSurface& operator=(const CudaInteropSurface&) = delete;

private:
    void Initialize(ID3D12Resource* texture,
                    ID3D12Resource* superResTexture,
                    ID3D12Fence* sharedFence,
                    ID3D12Resource* originalTexture);

    bool SelectCudaDeviceMatching(LUID adapterLuid);
    bool CreateSurfaceFromResource(ID3D12Device* device, ID3D12Resource* texture);
    bool CreateSuperResSurfaceFromResource(ID3D12Device* device,
                                           ID3D12Resource* texture);
    bool CreateOriginalSurfaceFromResource(ID3D12Device* device,
                                           ID3D12Resource* texture);
    bool EnsureSuperResOutputBuffer(unsigned int width, unsigned int height);
    void ImportFenceSemaphore(ID3D12Device* device, ID3D12Fence* fence);
    bool EnsureDeviceBuffers(unsigned int width, unsigned int height, CudaBufferFormat format);
    bool EnsureTemporalHistory(unsigned int width, unsigned int height);
    bool EnsureStabilizationBuffers(unsigned int width, unsigned int height);
    bool EnsureRawInputBuffers(unsigned int width, unsigned int height, int format);
    bool EnsurePinnedUploadRing(size_t requiredBytes);
    bool EnsurePreRotateBuffer(unsigned int width, unsigned int height);
    bool EnsureKeystoneResources(unsigned int width, unsigned int height);
    bool EnsureAutoContrastBuffers();
    bool EnsureTextClarityBuffers(unsigned int width, unsigned int height);
    void ReleaseDeviceBuffers();
    void ReleaseTemporalHistory();
    void ReleaseStabilization();
    void ReleaseRawInput();
    void ReleasePinnedUploadRing();
    void ReleaseKeystone();
    void ReleaseAutoContrast();
    void ReleaseTextClarity();
    void ReleaseSuperRes();
    void ConsumeStabilizerTiming();
    void ConsumeSuperResTiming();
    void UpdateSuperResCache(const uchar4* source,
                             size_t sourcePitch,
                             uchar4* destination,
                             size_t destinationPitch,
                             unsigned int width,
                             unsigned int height,
                             const ProcessingSettings& settings);
    void ConsumeProcessTiming();
    void UpdateSpatialCache(const uchar4* source, size_t sourcePitch,
                            uchar4* destination, size_t destinationPitch,
                            unsigned int width, unsigned int height,
                            const SpatialCacheGeometry& geometry,
                            const ProcessingSettings& settings);
    bool EnsureGaussianKernel(int radius, float sigma);
    bool EnsureDisplayColorLut(const std::uint32_t* lut, std::uint64_t generation);
    void RunKeystoneStage(uchar4*& current, uchar4*& alternate,
                          size_t& currentPitch, size_t& alternatePitch);
    void ConsumeKeystoneDetection();
    void RememberKeystoneCorrection();
    void RestoreKeystoneCorrection();
    void ResetKeystoneCornersToIdentity();
    bool SynchronizeStream() noexcept;
    void QuarantineGpuResources() noexcept;
    bool UploadD3D11Frame(const ProcessingInput& input,
                          uchar4* destination,
                          size_t destinationPitch);

    struct D3D11InteropState;
    std::unique_ptr<D3D11InteropState> d3d11Interop_;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device_;
    Microsoft::WRL::ComPtr<ID3D12Resource> retainedMainTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> retainedSuperResTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> retainedOriginalTexture_;
    Microsoft::WRL::ComPtr<ID3D12Fence> retainedSharedFence_;
    bool streamFaulted_{};

    cudaExternalMemory_t externalMemory_{};
    cudaMipmappedArray_t mipArray_{};
    cudaArray_t level0Array_{};
    cudaSurfaceObject_t surfaceObject_{0};
    cudaExternalMemory_t superResExternalMemory_{};
    cudaMipmappedArray_t superResMipArray_{};
    cudaArray_t superResLevel0Array_{};
    cudaSurfaceObject_t superResSurfaceObject_{0};
    cudaExternalMemory_t originalExternalMemory_{};
    cudaMipmappedArray_t originalMipArray_{};
    cudaArray_t originalLevel0Array_{};
    cudaStream_t stream_{};
    cudaExternalSemaphore_t externalSemaphore_{};

    UINT width_{};
    UINT height_{};
    UINT superResWidth_{};
    UINT superResHeight_{};
    UINT originalWidth_{};
    UINT originalHeight_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    int cudaDeviceId_{-1};
    bool valid_{false};

    uchar4* deviceBufferA_{};
    uchar4* deviceBufferB_{};
    uchar4* deviceScratch_{};
    uchar4* deviceSuperResOutput_{};
    size_t devicePitchA_{};
    size_t devicePitchB_{};
    size_t devicePitchScratch_{};
    size_t devicePitchSuperResOutput_{};
    unsigned int deviceSuperResOutputWidth_{};
    unsigned int deviceSuperResOutputHeight_{};
    unsigned int deviceWidth_{};
    unsigned int deviceHeight_{};
    CudaBufferFormat bufferFormat_{CudaBufferFormat::kRgba8};
    float4* deviceTemporalHistory_{};
    size_t devicePitchHistory_{};
    unsigned int historyWidth_{};
    unsigned int historyHeight_{};
    bool temporalHistoryValid_{};
    float* deviceStabLuma_{};
    float* deviceStabLumaPrevious_{};
    float* deviceStabLumaReference_{};
    float* deviceTripodCurrentPyramid1_{};
    float* deviceTripodCurrentPyramid2_{};
    float* deviceTripodReferencePyramid1_{};
    float* deviceTripodReferencePyramid2_{};
    TripodReferenceFeature* deviceTripodReferenceFeatures_{};
    unsigned int* deviceTripodReferenceFeatureCount_{};
    // Slots 1..3 of the recovery map. Slot 0 uses the primary reference
    // buffers above and is never evicted.
    float* deviceTripodRecoveryLuma_{};
    float* deviceTripodRecoveryPyramid1_{};
    float* deviceTripodRecoveryPyramid2_{};
    TripodReferenceFeature* deviceTripodRecoveryFeatures_{};
    unsigned int* deviceTripodRecoveryFeatureCounts_{};
    float4* deviceTripodKeyframeOrigins_{};
    unsigned int* deviceTripodKeyframeValid_{};
    TripodMatchCandidate* deviceTripodMatchCandidates_{};
    float* deviceTripodReferenceAccumulator_{};
    unsigned int* deviceTripodReferenceSampleCounts_{};
    float* deviceTripodFocusScore_{};
    float* deviceTripodBestFocusScore_{};
    unsigned int* deviceTripodSelectionFlag_{};
    uchar4* deviceBumpHoldFrame_{};
    size_t deviceBumpHoldPitch_{};
    BumpHoldState* deviceBumpHoldState_{};
    float* deviceStabColProjCurr_{};
    float* deviceStabRowProjCurr_{};
    float* deviceStabColProjPrev_{};
    float* deviceStabRowProjPrev_{};
    StabilizationState* deviceStabState_{};
    float4* hostStabDiagnostics_{};
    float4* deviceStabPairs_{};
    unsigned int* deviceStabPairCount_{};
    unsigned int stabSmallWidth_{};
    unsigned int stabSmallHeight_{};
    unsigned int stabFactorX_{};
    unsigned int stabFactorY_{};
    unsigned int stabFullWidth_{};
    unsigned int stabFullHeight_{};
    bool stabPrevValid_{};
    bool tripodReferenceValid_{};
    bool tripodRelockPending_{};
    bool virtualTripodActive_{};
    bool bumpHoldActive_{};
    bool tripodReferencePrepared_{};
    unsigned int tripodReferenceBuildFrame_{};
    unsigned int tripodReferenceAccumulationFrame_{};
    unsigned int tripodKeyframeAdmissionFrames_{};
    unsigned int tripodNextRecoverySlot_{1};
    cudaEvent_t stabilizerTimingStartEvent_{};
    cudaEvent_t stabilizerTimingStopEvent_{};
    bool stabilizerTimingPending_{};
    unsigned int stabilizerTimingFrameCounter_{};
    float lastStabilizerMs_{-1.0f};
    std::string stabilizerStatus_{"Stabilizer off"};
    int activeStabilizerEngine_{-1};

    // Raw camera input (NV12/YUY2) + GPU rotation staging.
    unsigned char* deviceRawPlane1_{};
    unsigned char* deviceRawPlane2_{};
    size_t rawPlane1Pitch_{};
    size_t rawPlane2Pitch_{};
    unsigned int rawWidth_{};
    unsigned int rawHeight_{};
    int rawFormat_{-1};
    uchar4* devicePreRotate_{};
    size_t devicePitchPreRotate_{};
    unsigned int preRotateWidth_{};
    unsigned int preRotateHeight_{};

    // P11 host-upload ring. The MF callback thread moves each pageable frame
    // into latestFrame_ under cameraMutex_; the Qt tick moves it out, and ONLY
    // the Qt thread writes these page-locked slots or advances the slot index.
    // ProcessFrame's CUDA stream reads the active slot. Shared D3D/CUDA fence
    // values order GPU work only and cannot guard a host memcpy, so each slot
    // has a CUDA event recorded immediately after its final H2D copy. The Qt
    // thread queries the slots without blocking and skips the frame if every
    // slot is still in use. The event is ordered before the same frame's
    // FenceSyncParams::signalValue but permits reuse before the rest of that
    // frame's kernels finish.
    struct PinnedUploadSlot {
        unsigned char* data{};
        cudaEvent_t uploadComplete{};
        bool uploadPending{};
    };
    std::array<PinnedUploadSlot, 3> pinnedUploadSlots_{};
    size_t pinnedUploadCapacity_{};
    size_t pinnedUploadNextSlot_{};

    // Keystone: async small-luma snapshot (device -> pinned host) + CPU quad
    // detection. Corners are the temporally smoothed source-quad corners in
    // full-resolution pixels, order TL, TR, BR, BL.
    float* deviceKeystoneLuma_{};
    float* hostKeystoneLuma_{};        // pinned (cudaMallocHost)
    cudaEvent_t keystoneCopyEvent_{};
    bool keystoneCopyPending_{};
    unsigned int keystoneSmallWidth_{};
    unsigned int keystoneSmallHeight_{};
    unsigned int keystoneFactorX_{};
    unsigned int keystoneFactorY_{};
    unsigned int keystoneFullWidth_{};
    unsigned int keystoneFullHeight_{};
    float2 keystoneCorners_[4]{};
    unsigned int keystoneFrameCounter_{};
    unsigned int keystoneFramesSinceDetection_{};
    std::vector<std::array<float2, 4>> keystoneHistory_;
    int keystoneHistoryIndex_{-1};
    bool keystoneTrackingPaused_{};
    bool keystoneSingleStepRequested_{};
    bool keystoneSingleStepInFlight_{};

    // Auto contrast: 256-bin histogram + smoothed lo/hi levels, all device-resident.
    unsigned int* deviceHistogram_{};
    float2* deviceAutoLevels_{};
    bool autoLevelsValid_{};

    // Text clarity: shared luma/statistics workspace, masks, CLAHE maps, and
    // an infrequent asynchronous focus-score readback. No image is read back.
    float* deviceTextLuma_{};
    float* deviceTextHorizontal_{};
    float* deviceTextMean_{};
    float* deviceTextSqHorizontal_{};
    float* deviceTextSqMean_{};
    size_t deviceTextFloatPitch_{};
    unsigned char* deviceTextMaskA_{};
    unsigned char* deviceTextMaskB_{};
    unsigned char* deviceTextMaskHistory_{};
    size_t deviceTextMaskPitch_{};
    unsigned int* deviceClaheHistogram_{};
    float* deviceClaheMap_{};
    int4* deviceTextAnalysis_{};
    float2* deviceFocusStats_{};
    float2* hostFocusStats_{};
    cudaEvent_t focusCopyEvent_{};
    unsigned int textWidth_{};
    unsigned int textHeight_{};
    unsigned int focusFrameCounter_{};
    bool textMaskHistoryValid_{};
    bool focusCopyPending_{};
    bool focusScoreValid_{};
    float latestFocusScore_{};

    // NVIDIA Video Effects is loaded only when requested. Timing uses CUDA
    // events on the existing interop stream, so the latency guard never stalls
    // the render loop with a host-side synchronization.
    std::unique_ptr<MaxineSuperRes> maxineSuperRes_;
    cudaEvent_t superResStartEvent_{};
    cudaEvent_t superResStopEvent_{};
    bool superResTimingPending_{};
    bool superResAutoDisabled_{};
    bool superResPerformanceOverride_{};
    // Setup/Load failures latch on the (source extent, factor) key so a broken
    // configuration is attempted once, not once per frame. The latch clears
    // when the key changes (zoom crossed a factor boundary, viewport resized)
    // or when the feature toggle is re-enabled.
    bool superResFailureLatched_{};
    unsigned int superResFailSrcWidth_{};
    unsigned int superResFailSrcHeight_{};
    unsigned int superResFailFactorNum_{};
    unsigned int superResFailFactorDen_{};
    bool superResRequestedLastFrame_{};
    bool superResActive_{};
    unsigned int superResSourceWidth_{};
    unsigned int superResSourceHeight_{};
    float superResFactorValue_{};
    SuperResRoiMetadata superResRoi_{};
    std::uint64_t superResGeneration_{};
    unsigned int superResWarmupSamples_{};
    unsigned int superResTimingSamples_{};
    float superResTimingTotalMs_{};
    float superResLastAverageMs_{-1.0f};
    std::string superResStatus_;

    // P8 GPU timing: cudaEvent pair around the ProcessFrame kernel chain,
    // recorded every 30th frame and polled (never waited on) the next frames.
    cudaEvent_t processTimingStartEvent_{};
    cudaEvent_t processTimingInputEvent_{};
    cudaEvent_t processTimingGeometryEvent_{};
    cudaEvent_t processTimingEffectsEvent_{};
    cudaEvent_t processTimingStopEvent_{};
    bool processTimingPending_{};
    unsigned int processTimingFrameCounter_{};
    float lastGpuFrameMs_{-1.0f};
    GpuStageTimings lastGpuStageTimings_{};

    int cachedKernelRadius_{-1};
    float cachedKernelSigma_{0.0f};
    bool kernelUploaded_{};
    std::uint64_t cachedDisplayColorLutGeneration_{};
    std::string lastError_;
    bool captureInteropFailed_{false};
};
#else
class CudaInteropSurface {
public:
    explicit CudaInteropSurface(ID3D12Resource* /*texture*/,
                                ID3D12Resource* /*superResTexture*/ = nullptr,
                                ID3D12Fence* /*sharedFence*/ = nullptr,
                                ID3D12Resource* /*originalTexture*/ = nullptr) {}
    ~CudaInteropSurface() = default;

    bool IsValid() const { return false; }
    bool IsFaulted() const { return false; }
    bool HasExternalSemaphore() const { return false; }

    void RunGradientDemoKernel(unsigned int /*width*/, unsigned int /*height*/, float /*timeSeconds*/) {}

    bool ProcessFrame(const ProcessingInput& /*input*/,
                      const ProcessingSettings& /*settings*/,
                      const FenceSyncParams& /*fenceSync*/) { return false; }

    const std::string& LastError() const { static std::string dummy; return dummy; }
    bool LastFailureWasCaptureInterop() const { return false; }
    void ResetTemporalHistory() {}
    void ResetStabilization() {}
    void ResetKeystone() {}
    void SetKeystoneTrackingPaused(bool /*paused*/) {}
    bool StepKeystoneCorrection(int /*direction*/) { return false; }
    KeystoneTrackingState GetKeystoneTrackingState() const { return {}; }
    void ResetTextClarityHistory() {}
    bool HasFocusScore() const { return false; }
    float LatestFocusScore() const { return 0.0f; }
    bool IsFocusAcceptable(float /*threshold*/) const { return true; }
    const std::string& SuperResStatus() const { static std::string dummy; return dummy; }
    bool IsSuperResActive() const { return false; }
    unsigned int SuperResSourceWidth() const { return 0; }
    unsigned int SuperResSourceHeight() const { return 0; }
    float SuperResFactor() const { return 0.0f; }
    SuperResRoiMetadata SuperResRoi() const { return {}; }
    bool IsSuperResPerformanceLimited() const { return false; }
    float SuperResAverageMs() const { return -1.0f; }
    void SetSuperResPerformanceOverride(bool /*enabled*/) {}
    void ResetSuperRes() {}
    void ResetCaptureInterop(bool /*atProcessExit*/ = false) {}
    void AbandonCaptureInterop() noexcept {}
    bool PollCaptureCopy() { return true; }
    bool WaitForIdle() noexcept { return true; }
    const std::string& StabilizerStatus() const { static std::string dummy; return dummy; }
    float LastStabilizerMs() const { return -1.0f; }
    float LastGpuFrameMs() const { return -1.0f; }
    GpuStageTimings LastGpuStageTimings() const { return {}; }

    CudaInteropSurface(const CudaInteropSurface&) = delete;
    CudaInteropSurface& operator=(const CudaInteropSurface&) = delete;
};
#endif

} // namespace okuflow

#endif // _WIN32
