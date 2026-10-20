#pragma once

#ifdef _WIN32

#include "okuflow/common/yuv_color.hpp"
#include "okuflow/common/async_cleanup.hpp"
#include "okuflow/common/control_diagnostics.hpp"

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
namespace uct {class DeviceImage;class AnalysisPool;using SharedDeviceImage=std::shared_ptr<const DeviceImage>;}

class MaxineAsyncProducer;

struct FenceSyncParams {
    bool enable{false};
    uint64_t waitValue{0};
    uint64_t signalValue{0};
    // PresentSceneTexture may use an independent producer timeline. CUDA uses
    // the fence imported by its own surface constructor.
    ID3D12Fence* presentationFence{nullptr};
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

enum class SuperResRefreshResult { NoWork, Submitted, Published, Failed };

// Async D3D11 source preparation (media playback). Pending means the caller
// retries later; nothing was imported or destroyed on the calling thread.
enum class D3D11SourceStatus { Ready, Pending, Failed };

// Diagnostics for the bounded D3D11 source-import cache. Counters are
// cumulative for the surface's lifetime; asyncImportThread is the hashed
// std::thread::id that ran the most recent completed async import (0 when
// none) so tests can prove imports never ran on the calling thread.
struct D3D11SourceStatistics {
    std::uint64_t asyncPreparations{};
    std::uint64_t importsCompleted{};
    std::uint64_t importsFailed{};
    std::uint64_t cacheHits{};
    std::uint64_t asyncImportThread{};
    std::uint64_t cachedImports{};
};

struct SuperResAsyncStatistics {
    std::uint64_t submitted{}, completed{}, staleDiscarded{}, desiredOverwritten{};
    float publishedAgeMs{-1.0f};
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
    struct TextStageActivity {
        bool background{}, adaptive{}, smartSharpen{}, clahe{}, twoColor{};
        bool hysteresis{}, selectiveSharpen{}, focus{}, glare{};
        bool pipeline{};
    };
    ControlFrameIdentity controlFrame;
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
    bool enableAutoTextClarity{false}; // Entire text-effects family master; child preferences may remain dormant.
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
    // Transient scheduling intent, never a persisted user preference. Keep
    // current-frame spatial/base output while navigation has priority.
    bool deferMlSuperResForInteraction{false};
    float mlSuperResStrength{0.65f};
    bool mlSuperResUltra1440p{false};

    // Apply only to an effective per-frame copy, never persisted preferences.
    // Independent Black & White, display colors, and Maxine remain unchanged.
    void ApplyTextClarityMaster() {
        // Background and glare share a kernel. Its flatten arithmetic must be
        // neutral when only glare is selected, without erasing the saved knob.
        if (!enableAutoTextClarity || !enableBackgroundFlatten)
            backgroundFlattenStrength = 0.0f;
        if (enableAutoTextClarity) return;
        enableBackgroundFlatten = false;
        enableAdaptiveBinarization = false;
        enableSmartSharpen = false;
        enableClahe = false;
        enableTwoColorText = false;
        enableTextHysteresis = false;
        enableSelectiveSharpen = false;
        enableFocusDetection = false;
        enableGlareSuppression = false;
    }

    // A master switch permits its selected refinements; it never forces an
    // unchecked refinement on. Use this same selection at CUDA dispatch.
    TextStageActivity TextClarityStages() const {
        TextStageActivity stages{
            enableAutoTextClarity && enableBackgroundFlatten,
            enableAutoTextClarity && enableAdaptiveBinarization,
            enableAutoTextClarity && enableSmartSharpen,
            enableAutoTextClarity && enableClahe,
            enableAutoTextClarity && enableTwoColorText,
            enableAutoTextClarity && enableTextHysteresis,
            enableAutoTextClarity && enableSelectiveSharpen,
            enableAutoTextClarity && enableFocusDetection,
            enableAutoTextClarity && enableGlareSuppression};
        stages.pipeline = stages.background || stages.adaptive || stages.smartSharpen ||
                          stages.clahe || stages.focus || stages.glare;
        return stages;
    }

    float EffectiveViewingMagnification() const {
        return std::isfinite(zoomAmount) && zoomAmount > 1.0f
                   ? zoomAmount : 1.0f;
    }
};

// Input frame description.
//
// width/height are ALWAYS the dimensions of the host pixel data as laid out in
// memory (i.e. pre-rotation). Raw YUV, device BGRA and opt-in host BGRA
// rotate on the GPU. With an odd rotation (90/270), the interop surface (and
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
                                          // Host BGRA ignores this unless rotateBgraOnGpu is set.
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
    // Unconditional presentation identity, independent of optional agent diagnostics.
    std::uint64_t cameraEpoch{}, frameSequence{}, settingsGeneration{};
    // Media-only replay of the exact immutable frame. Reuse its post-geometry
    // snapshot and restore the histories from BEFORE that frame; retries and
    // viewport edits must not register/blend the same sample a second time.
    // Requires EnableFrameReprocessing() on the initialization worker.
    bool reprocessFrame{false};
    // Selected-media CPU decoding supplies unrotated BGRA. Opt in to the
    // same GPU rotation as device inputs; camera BGRA stays pre-rotated.
    // Host rows are copied into owned event-guarded pinned storage before
    // ProcessFrame returns; no borrowed host pointer survives submission.
    bool rotateBgraOnGpu{false};
};

#if OKUFLOW_HAS_CUDA_EXT_MEMORY
struct StabilizationState;
struct TripodReferenceFeature;
struct TripodMatchCandidate;
struct BumpHoldState;
struct BumpHoldProvenance;

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
    // Experimental media-only second output. Configure on the construction
    // worker before any frame; selection is nonblocking and requires an idle
    // producer stream. Both outputs share one effect history/model/stream.
    // The caller owns per-output graphics completion and safe retirement.
    bool ConfigureMediaOutputBuffer(ID3D12Resource*, ID3D12Resource*, ID3D12Fence*);
    bool SelectMediaOutputBuffer(unsigned index);


    bool ProcessFrame(const ProcessingInput& input,
                      const ProcessingSettings& settings,
                      const FenceSyncParams& fenceSync);
    // Media-only nonblocking input admission, before changing frame history.
    // Host BGRA uses the pinned ring; both types prepare GPU rotation storage.
    // Busy queues no work and sets LastFailureWasBusy(); retry the same frame.
    bool PrepareMediaBgraInput(unsigned int width, unsigned int height,
                               int rotationQuarterTurns, bool deviceResident);
    // Extra Stable captures also copy the frame's compact device hold record;
    // UCT workers read it after the copy, so held/crossfade pixels are never
    // mistaken for the newer acquisition that triggered their presentation.
    void EnableUctAnalysis(bool enabled);
    bool UctAnalysisFailed() const;
    // Explicit desktop-free fault seam (fixtures only): the next UCT-only
    // allocation of this kind fails as clean pressure, exercising its latch.
    enum class UctAllocationFault { TemporalHistory, ReplaySnapshot };
    static void InjectUctAllocationFailureForTesting(UctAllocationFault which);
    uct::SharedDeviceImage TakeUctAnalysis() { return std::move(uctAnalysisFrame_); }
    const std::string& LastError() const { return lastError_; }
    bool LastFailureWasBusy() const { return uploadAdmissionBusy_; }
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
    void ResetFrameHistory();
    // Allocate bounded replay storage on the surface's construction worker.
    // Camera surfaces leave this disabled and retain their existing path.
    void EnableFrameReprocessing();
    // Capture the saved current post-geometry image without running effects
    // or advancing history. True means the supplied fence signal was queued.
    bool RefreshUctAnalysis(const FenceSyncParams& fenceSync);
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
    void SetSuperResPerformanceOverride(bool enabled);
    void ResetSuperRes();
    void InvalidateSuperResRequest();
    bool HasSuperResWork() const;
    bool NeedsSuperResPolling() const;
    SuperResRefreshResult RefreshSuperRes(const FenceSyncParams& fenceSync);
    SuperResAsyncStatistics SuperResStatistics() const;
    // Opt-in async source preparation for rotating decoder textures (media
    // playback). When enabled, UploadD3D11Frame never imports or destroys a
    // source import on the calling thread: PrepareD3D11Source must reach
    // Ready first. The camera path never enables this and keeps its inline
    // single-texture import behavior.
    void EnableAsyncD3D11SourcePreparation(bool enabled);
    // Bounded preparation of one decoder source texture. Ready: the import
    // is cached (at most 8, LRU) and its previous copy has retired, so
    // ProcessFrame may upload from it. Pending: the one preparation worker
    // is building an import, a prior copy from this slot is still retiring,
    // or no cache slot can be evicted yet; retry the same frame. Failed:
    // see LastError(). The worker retains the exact storage lease and the
    // texture while importing; adoption on this thread only moves the
    // finished import into the cache (no native import/destroy calls).
    // Evicted imports are destroyed by an AsyncCleanup worker after their
    // copy is proven complete.
    D3D11SourceStatus PrepareD3D11Source(ID3D11Texture2D* texture,
                                         unsigned int subresource,
                                         unsigned int width, unsigned int height,
                                         std::shared_ptr<void> storageLease);
    D3D11SourceStatistics SourceStatistics() const;
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
    bool PollIdle() noexcept;
    std::shared_ptr<AsyncCleanup> BeginCaptureInteropCleanup(std::shared_ptr<void> producerKeepAlive = {}, bool atProcessExit = false);
    const std::string& StabilizerStatus() const { return stabilizerStatus_; }
    float LastStabilizerMs() const { return lastStabilizerMs_; }

    // P8 GPU timing (plan 11 Wave 1): duration of the last sampled ProcessFrame
    // kernel chain in milliseconds, or a negative value while no sample exists.
    // Sampled every 30th frame with cudaEvents; queries never block.
    float LastGpuFrameMs() const { return lastGpuFrameMs_; }
    GpuStageTimings LastGpuStageTimings() const {
        return lastGpuStageTimings_;
    }
    // Nonblocking event query; returns the last completed sample and its exact
    // provenance. A caller must reject old/mismatched samples for frame decisions.
    ControlDiagnosticSnapshot ControlDiagnostics();

    CudaInteropSurface(const CudaInteropSurface&) = delete;
    CudaInteropSurface& operator=(const CudaInteropSurface&) = delete;

private:
    struct FrameReprocessingState;
    std::unique_ptr<FrameReprocessingState> frameReprocessing_;
    bool uctAnalysisEnabled_{};
    std::unique_ptr<uct::AnalysisPool> uctAnalysisPool_;
    uct::SharedDeviceImage uctAnalysisFrame_;
    std::int64_t uctLastCaptureMs_{};
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
    void ImportFenceSemaphore(ID3D12Device* device, ID3D12Fence* fence);
    bool EnsureDeviceBuffers(unsigned int width, unsigned int height, CudaBufferFormat format);
    bool EnsureTemporalHistory(unsigned int width, unsigned int height);
    // UCT-only and nonthrowing. A failed size stays latched (no allocation,
    // free or stream drain per frame) until an explicit reset or a new size.
    bool EnsureUctTemporalBuffers(unsigned int width, unsigned int height);
    // Lazily allocates the media replay snapshots for the UCT temporal
    // history/smoothed image. Only called when UCT temporal smoothing is
    // actually wanted; failure is UCT-only, never throws, and is latched
    // separately from the live EMA until an explicit reset.
    bool EnsureUctSnapshotBuffers();
    void ReleaseUctTemporalBuffers();
    // True reset: new history epoch; live history and replay snapshot revoked.
    void ResetUctTemporalHistory();
    // Explicit resets (UCT toggle, public history reset) may retry once.
    void ClearUctAllocationFailures();
    // Paused media refresh after a true reset: seeds a new one-frame EMA
    // from the saved current geometry. Attempted once per reset.
    bool RegenerateUctSmoothedSnapshot();
    bool EnsureStabilizationBuffers(unsigned int width, unsigned int height);
    bool EnsureRawInputBuffers(unsigned int width, unsigned int height, int format);
    // streamAlreadyIdle is only for pre-fence media admission after PollIdle.
    bool EnsurePinnedUploadRing(size_t requiredBytes, bool streamAlreadyIdle = false);
    bool EnsurePreRotateBuffer(unsigned int width, unsigned int height,
                               bool streamAlreadyIdle = false);
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
    void AccumulateSuperResTiming(float elapsedMs);
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
    // Keystone host state only; the public ResetKeystone also restarts the
    // UCT history. The per-frame keystone-off path must not reseed UCT.
    void ResetKeystoneState();
    void RememberKeystoneCorrection();
    void RestoreKeystoneCorrection();
    void ResetKeystoneCornersToIdentity();
    bool SynchronizeStream() noexcept;
    void QuarantineGpuResources() noexcept;
    void SwapMediaOutputBindings() noexcept;
    struct AlternateOutput;
    std::unique_ptr<AlternateOutput> alternateOutput_;
    unsigned mediaOutputIndex_{};
    bool UploadD3D11Frame(const ProcessingInput& input,
                          uchar4* destination,
                          size_t destinationPitch);

    struct D3D11InteropState;
    struct D3D11PrepareSlot;
    // Builds one complete source import (shared handles, CUDA external
    // memory, copy event) without touching any surface member, so it may run
    // on the preparation worker. Partial imports are destroyed on the same
    // thread; unprovable destruction leaks the state and reports failure.
    static std::unique_ptr<D3D11InteropState> BuildD3D11Import(
        ID3D12Device* device, int cudaDeviceId, ID3D11Texture2D* texture,
        unsigned int subresource, unsigned int width, unsigned int height,
        std::string& error);
    // Destroys one import's native handles. False means completion could
    // not be proven and the caller must retain the state (an AsyncCleanup
    // failure retains its closure until process exit).
    static bool DestroyD3D11Import(D3D11InteropState& state, int cudaDeviceId,
                                   bool atProcessExit) noexcept;
    bool PollImportCopy(D3D11InteropState& state);
    bool PollAllImportCopies();
    D3D11InteropState* FindImport(ID3D11Texture2D* texture,
                                  unsigned int subresource);
    void ActivateImport(D3D11InteropState* state);
    bool AdoptPreparedImport(ID3D11Texture2D*& finishedIdentity,
                             std::string& finishedError);
    bool EvictOldestImport();
    void DestroyImportAsync(std::unique_ptr<D3D11InteropState> state);
    void RetirePrepareSlotAtDestruction();
    void LeakPrepareSlot() noexcept;

    // Active source import plus a bounded LRU cache of parked imports
    // (front = least recently activated). Active + parked never exceeds the
    // decoder's 8-slot ceiling; the scene/analysis pools stay separate.
    std::unique_ptr<D3D11InteropState> d3d11Interop_;
    std::vector<std::unique_ptr<D3D11InteropState>> d3d11ImportCache_;
    std::shared_ptr<D3D11PrepareSlot> d3d11PrepareSlot_;
    std::shared_ptr<AsyncCleanup> d3d11EvictionCleanup_;
    bool asyncSourcePreparation_{};
    D3D11SourceStatistics sourceStatistics_{};
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
    size_t devicePitchA_{};
    size_t devicePitchB_{};
    size_t devicePitchScratch_{};
    unsigned int deviceWidth_{};
    unsigned int deviceHeight_{};
    CudaBufferFormat bufferFormat_{CudaBufferFormat::kRgba8};
    float4* deviceTemporalHistory_{};
    size_t devicePitchHistory_{};
    unsigned int historyWidth_{};
    unsigned int historyHeight_{};
    bool temporalHistoryValid_{};
    // UCT-only neutral temporal EMA (post-geometry, pre-effects). Its own
    // history/scratch; never the display temporal buffer. Allocated once at
    // surface size, released only with the other device buffers. Cost while
    // retained: 20 bytes/pixel per surface (float4 history + BGRA scratch);
    // media replay snapshots add another 20. Kept across UCT toggles: legacy
    // free/realloc would synchronize, and cleanup never drains on the UI.
    float4* deviceUctTemporalHistory_{};
    size_t deviceUctHistoryPitch_{};
    uchar4* deviceUctSmoothed_{};
    size_t deviceUctSmoothedPitch_{};
    unsigned int uctTemporalWidth_{};
    unsigned int uctTemporalHeight_{};
    bool uctTemporalValid_{};
    bool uctTemporalFailed_{};
    // Allocation failure latches: the EMA by size, media snapshots per surface.
    unsigned int uctAllocationFailedWidth_{};
    unsigned int uctAllocationFailedHeight_{};
    bool uctSnapshotFailed_{};
    std::uint64_t uctHistoryEpoch_{1};
    std::uint64_t uctSeedFrame_{};
    std::uint64_t uctBlendCount_{};
    float uctTemporalAlpha_{};
    std::uint64_t uctIdentityEpoch_{};
    int uctIdentityRotation_{};
    bool uctIdentityStabilization_{};
    bool uctIdentityKeystone_{};
    bool uctIdentityValid_{};
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
    // Written every Extra Stable frame (even while UCT is off) so a later UCT
    // enable during a hold still knows which acquisition is being shown.
    BumpHoldProvenance* deviceBumpHoldProvenance_{};
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
    ControlDiagnosticSnapshot controlDiagnostics_, pendingControlDiagnostics_;
    bool pendingDiagnosticBumpHold_{};

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
    bool uploadAdmissionBusy_{};
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
    bool keystoneReferenceValid_{};
    float keystoneLastCornerDelta_{};

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
    bool discardFocusCopy_{};
    bool lastTextClarityMasterEnabled_{};
    bool focusScoreValid_{};
    float latestFocusScore_{};

    // SDK calls and private GPU storage belong to the independent producer.
    // The presentation thread only snapshots input and publishes exact results.
    struct AsyncSuperResState;
    std::unique_ptr<AsyncSuperResState> asyncSuperRes_;
    std::unique_ptr<MaxineAsyncProducer> maxineProducer_;
    std::uint64_t processedSceneSerial_{};
    std::uint64_t superResRequestGeneration_{};
    std::uint64_t currentCameraEpoch_{}, currentFrameSequence_{}, currentSettingsGeneration_{};
    unsigned int superResTimingSourceWidth_{};
    unsigned int superResTimingSourceHeight_{};
    unsigned int superResTimingOutputWidth_{};
    unsigned int superResTimingOutputHeight_{};
    float superResTimingStrength_{-1.0f};
    float superResRequestedStrength_{-1.0f};
    std::uint64_t superResStrengthChangedAt_{};
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

    bool ProcessFrame(const ProcessingInput& /*input*/,
                      const ProcessingSettings& /*settings*/,
                      const FenceSyncParams& /*fenceSync*/) { return false; }

    const std::string& LastError() const { static std::string dummy; return dummy; }
    bool LastFailureWasCaptureInterop() const { return false; }
    bool ConfigureMediaOutputBuffer(ID3D12Resource*, ID3D12Resource*, ID3D12Fence*) { return false; }
    bool SelectMediaOutputBuffer(unsigned index) { return index == 0; }
    bool LastFailureWasBusy() const { return false; }
    bool PrepareMediaBgraInput(unsigned int, unsigned int, int, bool) { return false; }
    void ResetTemporalHistory() {}
    void ResetStabilization() {}
    void ResetKeystone() {}
    void SetKeystoneTrackingPaused(bool /*paused*/) {}
    bool StepKeystoneCorrection(int /*direction*/) { return false; }
    KeystoneTrackingState GetKeystoneTrackingState() const { return {}; }
    void ResetTextClarityHistory() {}
    void ResetFrameHistory() {}
    void EnableFrameReprocessing() {}
    bool RefreshUctAnalysis(const FenceSyncParams&) { return false; }
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
    void SetSuperResPerformanceOverride(bool /*enabled*/) {}
    void ResetSuperRes() {}
    void InvalidateSuperResRequest() {}
    bool HasSuperResWork() const { return false; }
    bool NeedsSuperResPolling() const { return false; }
    SuperResRefreshResult RefreshSuperRes(const FenceSyncParams&) { return SuperResRefreshResult::NoWork; }
    SuperResAsyncStatistics SuperResStatistics() const { return {}; }
    void EnableAsyncD3D11SourcePreparation(bool /*enabled*/) {}
    D3D11SourceStatus PrepareD3D11Source(ID3D11Texture2D*, unsigned int,
                                         unsigned int, unsigned int,
                                         std::shared_ptr<void>) {
        return D3D11SourceStatus::Failed;
    }
    D3D11SourceStatistics SourceStatistics() const { return {}; }
    void ResetCaptureInterop(bool /*atProcessExit*/ = false) {}
    void AbandonCaptureInterop() noexcept {}
    bool PollCaptureCopy() { return true; }
    bool WaitForIdle() noexcept { return true; }
    bool PollIdle() noexcept { return true; }
    std::shared_ptr<AsyncCleanup> BeginCaptureInteropCleanup(std::shared_ptr<void> producerKeepAlive = {}, bool = false) {
        return AsyncCleanup::Start([producerKeepAlive = std::move(producerKeepAlive)] { return true; });
    }
    const std::string& StabilizerStatus() const { static std::string dummy; return dummy; }
    float LastStabilizerMs() const { return -1.0f; }
    float LastGpuFrameMs() const { return -1.0f; }
    GpuStageTimings LastGpuStageTimings() const { return {}; }
    ControlDiagnosticSnapshot ControlDiagnostics() { return {}; }

    CudaInteropSurface(const CudaInteropSurface&) = delete;
    CudaInteropSurface& operator=(const CudaInteropSurface&) = delete;
};
#endif

} // namespace okuflow

#endif // _WIN32
