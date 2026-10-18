#pragma once

#ifdef _WIN32

#include "okuflow/common/video_playback.hpp"
#include "okuflow/cuda/cuda_interop.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>

namespace okuflow {

// Dedicated GPU scene for selected-media playback frames, fully separate from
// the camera CUDA surface and the presenter's global fence timeline.
//
// Ownership and threading:
// - Every public method is called on the Qt UI thread only. No method waits
//   for GPU work, joins a thread, or destroys a CUDA surface inline.
// - Configuration (shared texture + private shared fence + CudaInteropSurface
//   importing that fence) is built by one detached worker closure. Poll
//   Prepare() each tick; it reports Pending until the worker's bundle is
//   adopted, Ready once usable, Failed once an attempt has failed.
// - Replacing a bundle retires it through one AsyncCleanup
//   worker that owns the exact texture/fence/surface. Retirement first proves
//   CUDA-stream and private-fence completion with bounded waits on that
//   worker; unprovable completion retains (quarantines) the bundle until
//   process exit and latches this helper Failed. A failed attempt stays
//   Failed until Invalidate() explicitly re-arms Prepare(); quarantined
//   completion is sticky for the helper's lifetime. Neither poisons the
//   presenter's camera fence, so the caller can fall back to CPU playback.
//
// Private fence timeline (one monotonically increasing value space):
// - Process() waits on the last *recorded* graphics signal (0 before the
//   first recorded presentation) and signals the next value after its
//   kernels. A reserved value that never reaches the GPU is burned; fence
//   waits are >=, so gaps are harmless.
// - PresentationSync() reserves the next graphics value (reissuing the same
//   reservation until recorded) and returns FenceSyncParams{enable,
//   waitValue = latest CUDA producer signal, signalValue = reserved graphics
//   signal, presentationFence = private fence}. PresentSceneTexture waits on
//   waitValue and signals signalValue after the draw and any viewport
//   readback copy queued in the same submission.
// - RecordPresented() must be called only when that signal was actually
//   queued (successful PresentSceneTexture, or its independent-read out
//   parameter reporting the reserved value). Busy/canceled presents record
//   nothing, so no queue dependency is created and the same reservation is
//   reissued by the next PresentationSync().
// - Process(), RefreshSuperRes() and Invalidate() revoke an unrecorded presentation
//   reservation; a FenceSyncParams copy issued before them must not be used.
//
// UCT analysis:
// - Process(..., enableUct) toggles the surface's independent pre-zoom
//   analysis capture, stamped cameraEpoch=epoch, frameSequence=frame.sampleId,
//   settingsGeneration=appearance. TakeUctAnalysis() returns the owned device
//   image only when it matches the current processed scene's exact
//   epoch/frame/appearance; otherwise it returns null (capture is throttled
//   inside the surface, so a displayed frame may legitimately have none).
//   An untaken analysis lease is released before the next differing frame is
//   processed, so the two-slot analysis pool is never pinned by metadata for
//   an obsolete frame while inference holds the other slot.
class MediaGpuScene final {
public:
    enum class Status { Pending, Ready, Failed };

    // Two outputs is an explicit throughput/latency experiment; ordinary app
    // construction retains one until benchmark acceptance. History stays shared.
    explicit MediaGpuScene(unsigned outputBuffers = 1);
    // Never waits: any live, retiring, or still-initializing bundle is handed
    // to a bounded AsyncCleanup closure; healthy invalidation instead retains it.
    ~MediaGpuScene();

    MediaGpuScene(const MediaGpuScene&) = delete;
    MediaGpuScene& operator=(const MediaGpuScene&) = delete;

    // Adopts/builds the bundle for `device` at post-rotation dimensions.
    // Pending: initialization or prior-bundle retirement is in flight.
    // Ready: Process() may be called. Failed: see Error(); configuration
    // failure is latched until Invalidate(), quarantine forever.
    // Zero cache extents select the scene extent, bounded to 2560x1440 (or
    // portrait). Explicit extents use the same bound. Allocate/reconfigure
    // only on the worker; ordinary source/settings invalidation retains it.
    Status Prepare(ID3D12Device* device, UINT postRotationWidth, UINT postRotationHeight,
                   UINT cacheWidth = 0, UINT cacheHeight = 0);

    // Uploads frame.gpu (exact immutable lease retained through the CUDA
    // copy) or bounded frame.bgra (actual stride; synchronously copied into
    // owned event-guarded pinned storage before asynchronous upload), rotates
    // by `rotationQuarterTurns` on the GPU, and runs the
    // complete caller-supplied production pipeline. Only legacy CUDA zoom is
    // cleared because the presenter owns zoom. Source/seek epochs, changed
    // request/archive generations, CPU/GPU type/layout, rotation and nonmonotonic PTS reset history;
    // ordinary forward frames retain it. Same-frame appearance/view edits
    // replay the saved geometry with the pre-frame histories, so paused
    // retries never accumulate temporal blending or registration.
    // Pending: not prepared, size mismatch (call Prepare), the frame's
    // decoder texture is still being imported by the surface's one bounded
    // preparation worker (imports are cached across the decoder's rotating
    // slots, at most 8, and never built or destroyed on this thread), or
    // the previous copy from that slot is still retiring (retry the same
    // frame; nothing was enqueued and no fence value was consumed). Host
    // upload slots or orientation-resize resources can also be busy; retry
    // without waiting. No CPU full-frame rotation or D3D11 import is used for BGRA.
    // Ready: scene submitted, Texture()/PresentationSync() are current.
    // Failed: GPU submission failed before its fence signal; the bundle is
    // retired asynchronously and the failure is latched until Invalidate().
    Status Process(const VideoPlaybackFrame& frame, int rotationQuarterTurns,
                   ProcessingSettings settings, std::uint64_t epoch,
                   std::uint64_t appearance, bool enableUct);

    // Current processed scene, valid only after Process() returned Ready and
    // until the next Process()/Invalidate(). Null otherwise.
    ID3D12Resource* Texture() const noexcept;
    ID3D12Resource* BaseTexture() const noexcept { return Texture(); }
    ID3D12Resource* SuperResTexture() const noexcept;
    UINT SuperResWidth() const noexcept;
    UINT SuperResHeight() const noexcept;
    SuperResRoiMetadata SuperResRoi() const;
    bool IsSuperResActive() const;
    bool IsSuperResPerformanceLimited() const;
    std::string SuperResStatus() const;
    void SetSuperResPerformanceOverride(bool enabled);
    // Explicit native edits revoke pending output without joining its worker.
    void ResetSuperRes();
    void ResetTextClarityHistory();
    bool NeedsSuperResPolling() const;
    // Owns private fence reservation, revocation and publication bookkeeping.
    // Submitted also advances the producer signal; Published additionally
    // increments SceneGeneration. Poll during pause as well as playback.
    SuperResRefreshResult RefreshSuperRes();
    SuperResAsyncStatistics SuperResStatistics() const;
    KeystoneTrackingState GetKeystoneTrackingState() const;
    void SetKeystoneTrackingPaused(bool paused);
    bool StepKeystoneCorrection(int direction);
    ControlDiagnosticSnapshot ControlDiagnostics();
    UINT Width() const noexcept;  // post-rotation scene extent, 0 when unprepared
    UINT Height() const noexcept;
    bool SceneReady() const noexcept { return Texture() != nullptr; }
    // Identity of the scene currently in Texture() (0 when none).
    std::uint64_t SceneEpoch() const noexcept;
    std::uint64_t SceneFrameSequence() const noexcept;
    std::uint64_t SceneAppearance() const noexcept;
    // Changes on base reprocessing and same-frame neural publication; use it
    // in annotation/UCT presentation identity as well as epoch/frame/settings.
    std::uint64_t SceneGeneration() const noexcept;
    std::uint64_t HistoryResetCount() const noexcept;
    // Analysis pixels are post-stabilization/keystone. Hold/temporal composites
    // and neural presentation cannot prove single-frame text correspondence;
    // clear existing OCR overlays while this is false. Spatial ROI scaling is
    // still a known mapping and remains compatible.
    bool UctCoordinatesReliable() const noexcept;

    // Sync for the next presentation of Texture(); enable=false when no
    // scene is ready. Each distinct successful presentation must signal a
    // unique value, so call RecordPresented() after each one before asking
    // for the next sync.
    FenceSyncParams PresentationSync();
    // Commits the reserved graphics signal as the value the next Process()
    // waits on. The overload taking a value commits only when it equals the
    // outstanding reservation (use the presenter's independent-read out
    // parameter, which is set even if a later present step fails).
    void RecordPresented();
    void RecordPresented(std::uint64_t independentReadSignal);

    // Owned pre-zoom device image for the current exact scene, or null when
    // unavailable (UCT off, capture throttled/busy, or already taken).
    uct::SharedDeviceImage TakeUctAnalysis();
    uct::SharedDeviceImage Analysis() { return TakeUctAnalysis(); }
    // Mirrors the surface's analysis-pool failure latch for status reporting.
    bool UctAnalysisFailed() const;

    const std::string& Error() const noexcept;

    // Bounded source-import diagnostics of the current bundle's surface
    // (zeroes when unprepared). Lets tests prove decoder-slot imports ran
    // on the preparation worker and are reused, not rebuilt per frame.
    D3D11SourceStatistics SourceStatistics() const;

    // Drops scene identity/history and pending neural identity immediately.
    // Healthy same-size bundles retain their model/import/performance caches.
    // Failed bundles and superseded initialization retire asynchronously.
    // Also re-arms a latched configuration/process failure.
    void Invalidate() noexcept;
    // Explicit nonblocking resource release. Ready proves this helper's own
    // construction/retirement closures completed; Failed retains quarantine.
    // Unlike Invalidate, this gives up healthy cached allocations/models.
    Status Release();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace okuflow

#endif // _WIN32
