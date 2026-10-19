#ifdef _WIN32

#include "okuflow/app/media_gpu_scene.hpp"

#include "okuflow/common/async_cleanup.hpp"
#include "okuflow/common/uct_device_image.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <QDebug>

namespace okuflow {
namespace {

struct OutputSlot {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture, cache;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t nextValue{1}, producer{}, graphics{}, reservation{};
};

struct Bundle {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> superResTexture;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::unique_ptr<CudaInteropSurface> surface;
    UINT width{};
    UINT height{};
    UINT cacheWidth{}, cacheHeight{};
    // One private monotonic timeline shared by CUDA producer signals and
    // graphics read-completion signals. Reserved-but-never-queued values are
    // burned; every consumer waits with >= semantics.
    std::uint64_t nextValue{1};
    std::uint64_t lastCudaSignal{};
    std::uint64_t lastGraphicsSignal{};
    std::uint64_t pendingGraphicsSignal{};
    OutputSlot parked;
    unsigned outputBuffers{1}, outputIndex{};
    bool sceneReady{};

    bool SelectOutput(unsigned index) {
        if (index == outputIndex) return true;
        if (!surface->SelectMediaOutputBuffer(index)) return false;
        texture.Swap(parked.texture); superResTexture.Swap(parked.cache); fence.Swap(parked.fence);
        std::swap(nextValue, parked.nextValue); std::swap(lastCudaSignal, parked.producer);
        std::swap(lastGraphicsSignal, parked.graphics); std::swap(pendingGraphicsSignal, parked.reservation);
        outputIndex = index;
        return true;
    }
    std::uint64_t sceneEpoch{};
    std::uint64_t sceneFrameSequence{};
    std::uint64_t sceneAppearance{};
    std::uint64_t sceneGeneration{}, historyResetCount{};
    std::uint64_t requestGeneration{}, archiveGeneration{};
    std::int64_t pts{};
    int turns{};
    // Weak ownership identity cannot confuse a freed CPU vector/decoder slot
    // with a later allocation at the same address. Never retain decoded bytes
    // after the synchronous copy into the surface's owned pinned pool.
    std::weak_ptr<const void> sourceStorage;
    ID3D11Texture2D* sourceTexture{}; // compared only while sourceStorage is alive
    UINT sourceWidth{}, sourceHeight{}, sourceStride{};
    bool sourceIsGpu{};
    bool resetPending{true}, forceReprocess{};
    ProcessingSettings settings;
};

struct InitSlot {
    std::mutex mutex;
    bool done{};
    std::unique_ptr<Bundle> bundle;
    std::string error;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    UINT width{};
    UINT height{};
    UINT cacheWidth{}, cacheHeight{};
    std::uint64_t generation{};
    unsigned outputBuffers{1};
};

std::string HrError(const char* what, HRESULT hr)
{
    return std::string(what) + " (hr=0x" +
           std::to_string(static_cast<unsigned long>(hr)) + ")";
}

bool SameView(const ProcessingSettings& a, const ProcessingSettings& b)
{
    const auto& x = a.spatialViewTransform;
    const auto& y = b.spatialViewTransform;
    return a.zoomAmount == b.zoomAmount && a.zoomCenterX == b.zoomCenterX &&
        a.zoomCenterY == b.zoomCenterY && a.spatialViewportWidth == b.spatialViewportWidth &&
        a.spatialViewportHeight == b.spatialViewportHeight &&
        a.deferMlSuperResForInteraction == b.deferMlSuperResForInteraction &&
        x.valid == y.valid && x.sourceX == y.sourceX && x.sourceY == y.sourceY &&
        x.sourceWidth == y.sourceWidth && x.sourceHeight == y.sourceHeight &&
        x.destinationX == y.destinationX && x.destinationY == y.destinationY &&
        x.destinationWidth == y.destinationWidth && x.destinationHeight == y.destinationHeight;
}

// Runs only inside a worker closure. Proves both queues are done with the
// bundle before anything is released: the CUDA stream (producer writes and
// the retained source copy) and the graphics queue (reads complete at the
// last recorded graphics signal; the last CUDA signal proves the final
// write itself landed). Any doubt returns false, and AsyncCleanup retains
// the whole bundle until process exit instead of freeing live GPU work.
bool RetireBundleNow(Bundle& bundle)
{
    if (bundle.surface) {
        if (!bundle.surface->WaitForIdle()) return false;
        if (!bundle.surface->PollCaptureCopy()) return false;
    }
    const auto waitOutput = [](ID3D12Fence* fence, std::uint64_t lastSignal) {
        if (fence && lastSignal != 0) {
            const UINT64 completed = fence->GetCompletedValue();
            if (completed == UINT64_MAX) return false; // device removed
            if (completed < lastSignal) {
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event) return false;
                const HRESULT hr =
                    fence->SetEventOnCompletion(lastSignal, event);
                bool done = false;
                if (SUCCEEDED(hr)) {
                    done = WaitForSingleObject(event, 1000) == WAIT_OBJECT_0;
                }
                CloseHandle(event);
                if (!done) return false;
            }
        }
        return true;
    };
    if (!waitOutput(bundle.fence.Get(), std::max(bundle.lastCudaSignal, bundle.lastGraphicsSignal)) ||
        !waitOutput(bundle.parked.fence.Get(), std::max(bundle.parked.producer, bundle.parked.graphics))) return false;
    // The surface imported the texture and fence into CUDA; destroy it while
    // both D3D12 objects are still alive, then release them.
    bundle.surface.reset();
    bundle.texture.Reset();
    bundle.superResTexture.Reset();
    bundle.fence.Reset();
    bundle.device.Reset();
    return true;
}

std::shared_ptr<AsyncCleanup> RetireBundleAsync(std::unique_ptr<Bundle> bundle)
{
    auto owned = std::shared_ptr<Bundle>(std::move(bundle));
    return AsyncCleanup::Start([owned] { return RetireBundleNow(*owned); });
}

// Worker-thread bundle construction: shared BGRA target and private shared
// fence, plus a separate bounded spatial/neural cache, and no Original
// recording surface. The CudaInteropSurface imports this private
// fence itself, independently of the presenter's global fence.
void BuildBundle(const std::shared_ptr<InitSlot>& slot)
{
    std::unique_ptr<Bundle> bundle;
    std::string error;
    try {
        bundle = std::make_unique<Bundle>();
        bundle->device = slot->device;
        bundle->width = slot->width;
        bundle->height = slot->height;
        bundle->cacheWidth = slot->cacheWidth;
        bundle->cacheHeight = slot->cacheHeight;

        HRESULT hr = slot->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                               IID_PPV_ARGS(&bundle->fence));
        if (FAILED(hr)) {
            throw std::runtime_error(
                HrError("Failed to create media scene fence", hr));
        }

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = slot->width;
        desc.Height = slot->height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                     D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        hr = slot->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&bundle->texture));
        if (FAILED(hr)) {
            throw std::runtime_error(
                HrError("Failed to create media scene texture", hr));
        }

        desc.Width = slot->cacheWidth;
        desc.Height = slot->cacheHeight;
        hr = slot->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bundle->superResTexture));
        if (FAILED(hr)) throw std::runtime_error(HrError("Failed to create media spatial cache", hr));

        bundle->surface = std::make_unique<CudaInteropSurface>(
            bundle->texture.Get(), bundle->superResTexture.Get(), bundle->fence.Get(), nullptr);
        if (!bundle->surface->IsValid()) {
            error = bundle->surface->LastError();
            if (error.empty()) error = "Media CUDA surface is unavailable.";
            // No frame was ever submitted, so destroying here is safe; the
            // destructor itself quarantines if its bounded drain fails.
            bundle.reset();
        } else if (!bundle->surface->HasExternalSemaphore()) {
            error = "Media CUDA surface could not import its private fence.";
            bundle.reset();
        } else {
            // Decoder textures rotate through bounded slots; their CUDA
            // imports are built by the surface's preparation worker and
            // cached, never imported or destroyed on the UI thread.
            bundle->surface->EnableAsyncD3D11SourcePreparation(true);
            bundle->surface->EnableFrameReprocessing();
            bundle->outputBuffers = slot->outputBuffers;
            if (slot->outputBuffers == 2) {
                hr = slot->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&bundle->parked.fence));
                if (FAILED(hr)) throw std::runtime_error(HrError("Alternate media fence", hr));
                desc.Width = slot->width; desc.Height = slot->height;
                hr = slot->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bundle->parked.texture));
                if (FAILED(hr)) throw std::runtime_error(HrError("Alternate media scene", hr));
                desc.Width = slot->cacheWidth; desc.Height = slot->cacheHeight;
                hr = slot->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bundle->parked.cache));
                if (FAILED(hr)) throw std::runtime_error(HrError("Alternate media cache", hr));
                if (!bundle->surface->ConfigureMediaOutputBuffer(bundle->parked.texture.Get(),
                    bundle->parked.cache.Get(), bundle->parked.fence.Get()))
                    throw std::runtime_error("Alternate media output import failed: " + bundle->surface->LastError());
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
        bundle.reset();
    } catch (...) {
        error = "Unknown media GPU scene initialization failure.";
        bundle.reset();
    }
    std::lock_guard lock(slot->mutex);
    slot->bundle = std::move(bundle);
    slot->error = std::move(error);
    slot->done = true;
}

} // namespace

struct MediaGpuScene::State {
    std::unique_ptr<Bundle> current;
    std::shared_ptr<InitSlot> init;
    std::shared_ptr<AsyncCleanup> retirement;
    Microsoft::WRL::ComPtr<ID3D12Device> desiredDevice;
    UINT desiredWidth{};
    UINT desiredHeight{};
    UINT desiredCacheWidth{}, desiredCacheHeight{};
    bool performanceOverride{};
    unsigned outputBuffers{1};
    std::uint64_t generation{};
    // Latched attempt failure (init/process), re-armed only by Invalidate().
    bool attemptFailed{};
    // Unprovable GPU completion: sticky for this helper's lifetime.
    bool quarantined{};
    std::string error;
    uct::SharedDeviceImage analysis;

    void PollRetirement()
    {
        if (!retirement) return;
        const AsyncCleanup::Status status = retirement->Poll();
        if (status == AsyncCleanup::Status::Completed) {
            retirement.reset();
        } else if (status == AsyncCleanup::Status::Failed) {
            retirement.reset();
            quarantined = true;
            error = "Media GPU scene resources could not be proven idle and "
                    "are retained until restart.";
            qWarning() << error.c_str();
        }
    }

    // Adopts a finished init bundle when it still matches the desired
    // configuration; a superseded bundle goes straight to retirement.
    // Returns without consuming the slot while the one retirement slot is
    // busy. Invariant: a bundle is current only while no retirement runs.
    void PollInit()
    {
        if (!init) return;
        const auto slot = init; // Keep its mutex alive through unlock.
        std::unique_ptr<Bundle> bundle;
        std::string slotError;
        bool wanted = false;
        {
            std::lock_guard lock(slot->mutex);
            if (!init->done) return;
            wanted = init->generation == generation &&
                     init->device.Get() == desiredDevice.Get() &&
                     init->width == desiredWidth &&
                     init->height == desiredHeight;
            wanted = wanted && init->cacheWidth == desiredCacheWidth &&
                     init->cacheHeight == desiredCacheHeight;
            if (init->bundle && !wanted && retirement) return;
            bundle = std::move(init->bundle);
            slotError = init->error;
            init.reset();
        }
        if (bundle && !wanted) {
            retirement = RetireBundleAsync(std::move(bundle));
            return;
        }
        if (!bundle) {
            // A superseded attempt's failure is not the caller's failure.
            if (!wanted) return;
            attemptFailed = true;
            error = slotError.empty()
                        ? std::string("Media GPU scene initialization failed.")
                        : slotError;
            qWarning() << "Media GPU scene unavailable:" << error.c_str();
            return;
        }
        current = std::move(bundle);
        current->surface->SetSuperResPerformanceOverride(performanceOverride);
    }

    void Poll()
    {
        PollRetirement();
        PollInit();
    }

    void RetireCurrent()
    {
        analysis.reset();
        if (!current) return;
        if (!retirement) {
            retirement = RetireBundleAsync(std::move(current));
        } else {
            // Unreachable by construction (see PollInit); still never destroy
            // a bundle inline on the UI thread.
            (void)RetireBundleAsync(std::move(current));
        }
    }
};

MediaGpuScene::MediaGpuScene(unsigned outputBuffers) : state_(std::make_unique<State>()) {
    if (outputBuffers != 1 && outputBuffers != 2) throw std::invalid_argument("Media output buffer count must be one or two");
    state_->outputBuffers = outputBuffers;
}

MediaGpuScene::~MediaGpuScene()
{
    // Hand every remaining bundle to a worker closure; dropped AsyncCleanup
    // handles stay alive inside their own threads, and a failed closure
    // retains its bundle until process exit. A still-initializing bundle is
    // awaited (bounded) by its closure, never by this thread.
    if (state_->current) (void)RetireBundleAsync(std::move(state_->current));
    if (state_->init) {
        auto slot = state_->init;
        (void)AsyncCleanup::Start([slot] {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            std::unique_ptr<Bundle> bundle;
            for (;;) {
                {
                    std::lock_guard lock(slot->mutex);
                    if (slot->done) {
                        bundle = std::move(slot->bundle);
                        break;
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return !bundle || RetireBundleNow(*bundle);
        });
    }
}

MediaGpuScene::Status MediaGpuScene::Prepare(ID3D12Device* device,
                                              UINT postRotationWidth,
                                              UINT postRotationHeight,
                                              UINT cacheWidth, UINT cacheHeight)
{
    State& s = *state_;
    s.Poll();
    if (s.quarantined || s.attemptFailed) return Status::Failed;
    if (!device || postRotationWidth == 0 || postRotationHeight == 0) {
        s.error = "Media GPU scene needs a device and nonzero dimensions.";
        return Status::Failed;
    }
    const UINT maxWidth = postRotationWidth >= postRotationHeight ? 2560u : 1440u;
    const UINT maxHeight = postRotationWidth >= postRotationHeight ? 1440u : 2560u;
    cacheWidth = std::min(cacheWidth ? cacheWidth : postRotationWidth, maxWidth);
    cacheHeight = std::min(cacheHeight ? cacheHeight : postRotationHeight, maxHeight);
    if (s.current && s.current->device.Get() == device &&
        s.current->width == postRotationWidth &&
        s.current->height == postRotationHeight && s.current->cacheWidth == cacheWidth &&
        s.current->cacheHeight == cacheHeight) {
        return Status::Ready;
    }
    s.desiredDevice = device;
    s.desiredWidth = postRotationWidth;
    s.desiredHeight = postRotationHeight;
    s.desiredCacheWidth = cacheWidth;
    s.desiredCacheHeight = cacheHeight;
    s.PollInit(); // adopt now that the desired configuration is recorded
    if (s.attemptFailed) return Status::Failed;
    if (s.current && s.current->device.Get() == device &&
        s.current->width == postRotationWidth &&
        s.current->height == postRotationHeight && s.current->cacheWidth == cacheWidth &&
        s.current->cacheHeight == cacheHeight) {
        return Status::Ready;
    }
    if (s.current) { // wrong device/size: retire before replacing
        s.RetireCurrent();
        return Status::Pending;
    }
    if (s.retirement || s.init) return Status::Pending;

    auto slot = std::make_shared<InitSlot>();
    slot->device = device;
    slot->width = postRotationWidth;
    slot->height = postRotationHeight;
    slot->cacheWidth = cacheWidth;
    slot->cacheHeight = cacheHeight;
    slot->generation = s.generation;
    slot->outputBuffers = s.outputBuffers;
    try {
        std::thread([slot] { BuildBundle(slot); }).detach();
    } catch (...) {
        s.attemptFailed = true;
        s.error = "Could not start the media GPU scene initialization worker.";
        return Status::Failed;
    }
    s.init = std::move(slot);
    return Status::Pending;
}

MediaGpuScene::Status MediaGpuScene::Process(const VideoPlaybackFrame& frame,
                                              int rotationQuarterTurns,
                                              ProcessingSettings settings,
                                              std::uint64_t epoch,
                                              std::uint64_t appearance,
                                              bool enableUct)
{
    State& s = *state_;
    s.Poll();
    if (s.quarantined || s.attemptFailed) return Status::Failed;
    if (!s.current) return Status::Pending;
    Bundle& b = *s.current;

    const std::shared_ptr<const VideoPlaybackGpuImage> gpuImage = frame.gpu;
    if (frame.width == 0 || frame.height == 0 || frame.width > 16384 || frame.height > 16384) {
        s.error = "Media GPU scene requires bounded, nonzero frame dimensions.";
        return Status::Failed;
    }
    std::shared_ptr<const void> sourceStorage;
    if (gpuImage) {
        if (!gpuImage->texture || !gpuImage->storage) {
            s.error = "Media GPU scene requires the exact decoded texture storage lease.";
            return Status::Failed;
        }
        D3D11_TEXTURE2D_DESC desc{};
        gpuImage->texture->GetDesc(&desc);
        if (desc.Width != frame.width || desc.Height != frame.height ||
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.ArraySize != 1 || desc.MipLevels != 1) {
            s.error = "Media decoded texture does not match its BGRA frame layout.";
            return Status::Failed;
        }
        sourceStorage = gpuImage->storage;
    } else {
        const std::uint64_t rowBytes = std::uint64_t(frame.width) * sizeof(std::uint32_t);
        const std::uint64_t layoutBytes = std::uint64_t(frame.strideBytes) * frame.height;
        const std::uint64_t minimumBytes =
            std::uint64_t(frame.strideBytes) * (frame.height - 1) + rowBytes;
        if (!frame.bgra || frame.strideBytes < rowBytes ||
            layoutBytes > VideoPlayback::kDecodedByteLimit || frame.bgra->size() < minimumBytes) {
            s.error = "Media CPU BGRA frame has an invalid stride, truncated pixels or an oversized layout.";
            return Status::Failed;
        }
        sourceStorage = frame.bgra;
    }
    const int turns = ((rotationQuarterTurns % 4) + 4) % 4;
    const UINT postWidth = (turns & 1) != 0 ? frame.height : frame.width;
    const UINT postHeight = (turns & 1) != 0 ? frame.width : frame.height;
    if (postWidth != b.width || postHeight != b.height) {
        s.error = "Media GPU scene size changed; Prepare is required.";
        return Status::Pending;
    }

    // The surface was built on a worker thread; PollIdle also selects its
    // CUDA device on this thread before the first stream submission. Admit at
    // most one processing pass so rapid ROI updates cannot queue stale work.
    const bool idle = b.surface->PollIdle();
    if (b.surface->IsFaulted() || !b.surface->IsValid()) {
        s.error = b.surface->LastError().empty()
                      ? std::string("Media CUDA surface is no longer valid.")
                      : b.surface->LastError();
        s.attemptFailed = true;
        s.RetireCurrent();
        return Status::Failed;
    }
    if (!idle) return Status::Pending;
    settings.enableZoom = false; // Presentation owns magnification.
    settings.ApplyTextClarityMaster();
    const bool sameLayout = b.sourceIsGpu == bool(gpuImage) &&
        b.sourceWidth == frame.width && b.sourceHeight == frame.height &&
        b.sourceStride == (gpuImage ? 0u : frame.strideBytes);
    const auto previousStorage = b.sourceStorage.lock();
    const bool sameStorage = previousStorage && previousStorage.get() == sourceStorage.get() &&
        !previousStorage.owner_before(sourceStorage) && !sourceStorage.owner_before(previousStorage) &&
        (!gpuImage || b.sourceTexture == gpuImage->texture);
    const bool sameFrame = b.sceneReady && sameLayout && sameStorage && b.sceneEpoch == epoch &&
        b.sceneFrameSequence == frame.sampleId && b.pts == frame.pts100ns &&
        b.requestGeneration == frame.requestGeneration && b.archiveGeneration == frame.archiveGeneration &&
        b.turns == turns;
    const bool geometryChanged = b.sceneReady &&
        (b.settings.enableStabilization != settings.enableStabilization ||
         b.settings.enableBumpHold != settings.enableBumpHold ||
         b.settings.enableKeystone != settings.enableKeystone);
    const bool discontinuity = b.resetPending || (!sameFrame && b.sceneReady &&
        (!sameLayout || b.sceneEpoch != epoch || b.requestGeneration != frame.requestGeneration ||
         b.archiveGeneration != frame.archiveGeneration || b.turns != turns ||
         frame.pts100ns <= b.pts || frame.sampleId <= b.sceneFrameSequence));

    // No repeated upload, geometry registration, temporal blend or neural
    // request replacement for a paused frame. Analysis-pool retry and private
    // inference publication are independent of advancing the media clock.
    if (sameFrame && !geometryChanged && !b.forceReprocess &&
        !b.surface->GetKeystoneTrackingState().stepPending &&
        b.sceneAppearance == appearance && SameView(b.settings, settings)) {
        const auto refreshed = RefreshSuperRes();
        if (refreshed == SuperResRefreshResult::Failed) return Status::Failed;
#if OKUFLOW_HAS_CUDA_EXT_MEMORY
        const bool analysisEnabled = enableUct && UctCoordinatesReliable();
        b.surface->EnableUctAnalysis(analysisEnabled);
        if (!analysisEnabled) s.analysis.reset();
        else if (!s.analysis) {
            b.pendingGraphicsSignal = 0;
            FenceSyncParams sync{true, b.lastGraphicsSignal, b.nextValue++};
            if (b.surface->RefreshUctAnalysis(sync)) b.lastCudaSignal = sync.signalValue;
            if (b.surface->IsFaulted()) {
                s.error = b.surface->LastError(); s.attemptFailed = true; s.RetireCurrent();
                return Status::Failed;
            }
            s.analysis = b.surface->TakeUctAnalysis();
        }
#endif
        return Status::Ready;
    }
    if (discontinuity || geometryChanged) {
        // Revoke obsolete inference immediately, including while the new
        // decoder slot is still being imported. The old base may remain
        // visible under its old stamp, but can never acquire a late neural
        // result after a source/seek transition has already been requested.
        b.surface->InvalidateSuperResRequest();
        b.pendingGraphicsSignal = 0;
        s.analysis.reset();
    }
    // The frame's decoder slot must be imported (bounded worker + cache,
    // never on this thread) and its previous copy retired before reuse.
    // Nothing has been enqueued yet, so retrying the same frame is free:
    // the prior scene keeps its exact identity and no fence value burns.
    if (gpuImage) switch (b.surface->PrepareD3D11Source(gpuImage->texture, 0, frame.width,
                                          frame.height, gpuImage->storage)) {
    case D3D11SourceStatus::Pending:
        return Status::Pending;
    case D3D11SourceStatus::Failed:
        s.error = b.surface->LastError().empty()
                      ? std::string("Media source preparation failed.")
                      : b.surface->LastError();
        s.attemptFailed = true;
        s.RetireCurrent();
        return Status::Failed;
    case D3D11SourceStatus::Ready:
        break;
    }
    // Host ring/orientation admission must precede resets: Pending keeps the
    // old frame's saved geometry/history usable for a paused appearance edit.
    if (!b.surface->PrepareMediaBgraInput(frame.width, frame.height, turns, bool(gpuImage))) {
        if (b.surface->LastFailureWasBusy() && !b.surface->IsFaulted()) return Status::Pending;
        s.error = b.surface->LastError().empty()
                      ? std::string("Media BGRA input preparation failed.") : b.surface->LastError();
        s.attemptFailed = true;
        s.RetireCurrent();
        return Status::Failed;
    }
    // Each output has its own fence. A higher producer value on output one
    // must never falsely complete a still-reading graphics value on output zero.
    unsigned targetOutput = b.outputIndex;
    if (b.outputBuffers == 2) {
        const auto freeOutput = [](ID3D12Fence* fence, std::uint64_t value) {
            const auto completed = fence->GetCompletedValue();
            return completed != UINT64_MAX && completed >= value;
        };
        const bool parkedFree = freeOutput(b.parked.fence.Get(), std::max(b.parked.producer, b.parked.graphics));
        const bool currentFree = freeOutput(b.fence.Get(), std::max(b.lastCudaSignal, b.lastGraphicsSignal));
        if (parkedFree && (b.sceneReady || !currentFree)) {
            targetOutput = 1 - b.outputIndex;
        } else if (!currentFree) return Status::Pending;
    }
    if (discontinuity || geometryChanged) {
        b.surface->ResetFrameHistory();
        ++b.historyResetCount;
        s.analysis.reset();
    }
    // Temporal Smooth and Extra Stable are both reliable for UCT: the CUDA
    // surface captures a neutral paired smoothed/current image for the
    // former, and copies each frame's device hold record for the latter, so
    // held or crossfaded pixels are named on the UCT worker. Replay and
    // refresh reuse the saved pre-frame history and hold record.
    if (b.forceReprocess || b.surface->GetKeystoneTrackingState().stepPending) s.analysis.reset();

#if OKUFLOW_HAS_CUDA_EXT_MEMORY
    // Release any untaken analysis of a different frame before this frame's
    // capture asks the two-slot pool for storage; a stale lease must never
    // be stamped onto the new frame either.
    if (s.analysis && (s.analysis->sourceEpoch != epoch ||
                       s.analysis->frameSequence != frame.sampleId ||
                       s.analysis->processingGeneration != appearance)) {
        s.analysis.reset();
    }
    if (auto orphaned = b.surface->TakeUctAnalysis()) {
        if (!s.analysis && orphaned->sourceEpoch == epoch &&
            orphaned->frameSequence == frame.sampleId &&
            orphaned->processingGeneration == appearance) {
            s.analysis = std::move(orphaned);
        }
    }
    b.surface->EnableUctAnalysis(enableUct);
    if (!enableUct) s.analysis.reset();
#else
    (void)enableUct;
#endif

    ProcessingInput input{};
    input.width = frame.width;   // pre-rotation texture or host layout
    input.height = frame.height;
    input.inputFormat = 0;       // BGRA; both media source types rotate on GPU
    input.pixelSizeBytes = sizeof(std::uint32_t);
    input.rotationQuarterTurns = turns;
    if (gpuImage) {
        input.d3d11Texture = gpuImage->texture;
        input.d3d11TextureLease = gpuImage->storage;
    } else {
        input.hostPixels = frame.bgra->data();
        input.hostStrideBytes = frame.strideBytes;
        input.rotateBgraOnGpu = true;
    }
    input.d3d11Subresource = 0;
    input.publishOriginalFrame = false;
    input.cameraEpoch = epoch;
    input.frameSequence = frame.sampleId;
    input.settingsGeneration = appearance;
    input.reprocessFrame = sameFrame && !discontinuity && !geometryChanged;

    const unsigned previousOutput = b.outputIndex;
    if (!b.SelectOutput(targetOutput)) return Status::Pending;
    // This new submission supersedes any unrecorded presentation of the old
    // scene; its reservation was never queued, so dropping it is safe.
    b.pendingGraphicsSignal = 0;
    FenceSyncParams sync{};
    sync.enable = true;
    sync.waitValue = b.lastGraphicsSignal;
    sync.signalValue = b.nextValue++;
    // presentationFence stays null: CUDA waits/signals the private fence its
    // own surface imported; the field only instructs the presenter.
    if (!b.surface->ProcessFrame(input, settings, sync)) {
        if (b.surface->LastFailureWasBusy() && !b.surface->IsFaulted()) {
            // Admission enqueued nothing (the reserved value is burned).
            if (b.SelectOutput(previousOutput)) return Status::Pending;
            // A failed rollback cannot leave old metadata on another texture.
            s.error = "Media output rollback failed"; s.attemptFailed = true;
            s.RetireCurrent(); return Status::Failed;
        }
        // Work may be queued without its fence signal. The scene must not be
        // presented or overwritten; the retirement worker proves completion
        // or quarantines the bundle. The private fence keeps this failure
        // out of the presenter's camera timeline entirely.
        s.error = b.surface->LastError().empty()
                      ? std::string("Media GPU processing failed.")
                      : b.surface->LastError();
        qWarning() << "Media GPU scene processing failed:" << s.error.c_str();
        s.attemptFailed = true;
        s.RetireCurrent();
        return Status::Failed;
    }

    b.lastCudaSignal = sync.signalValue;
    b.sceneReady = true;
    b.sceneEpoch = epoch;
    b.sceneFrameSequence = frame.sampleId;
    b.sceneAppearance = appearance;
    ++b.sceneGeneration;
    b.resetPending = false;
    b.forceReprocess = false;
    b.pts = frame.pts100ns;
    b.requestGeneration = frame.requestGeneration;
    b.archiveGeneration = frame.archiveGeneration;
    b.turns = turns;
    b.sourceStorage = sourceStorage;
    b.sourceTexture = gpuImage ? gpuImage->texture : nullptr;
    b.sourceWidth = frame.width;
    b.sourceHeight = frame.height;
    b.sourceStride = gpuImage ? 0u : frame.strideBytes;
    b.sourceIsGpu = bool(gpuImage);
    b.settings = settings;
#if OKUFLOW_HAS_CUDA_EXT_MEMORY
    if (enableUct) {
        if (auto fresh = b.surface->TakeUctAnalysis()) {
            if (fresh->sourceEpoch == epoch &&
                fresh->frameSequence == frame.sampleId &&
                fresh->processingGeneration == appearance) {
                s.analysis = std::move(fresh);
            }
            // A mismatched capture belongs to an older identity: drop it so
            // its pool slot frees instead of shadowing the new frame.
        }
    }
#endif
    return Status::Ready;
}

ID3D12Resource* MediaGpuScene::Texture() const noexcept
{
    const State& s = *state_;
    return s.current && s.current->sceneReady ? s.current->texture.Get()
                                              : nullptr;
}

UINT MediaGpuScene::Width() const noexcept
{
    return state_->current ? state_->current->width : 0;
}

UINT MediaGpuScene::Height() const noexcept
{
    return state_->current ? state_->current->height : 0;
}

ID3D12Resource* MediaGpuScene::SuperResTexture() const noexcept
{
    const auto& b = state_->current;
    return b && b->sceneReady && b->surface->SuperResRoi().valid ? b->superResTexture.Get() : nullptr;
}

UINT MediaGpuScene::SuperResWidth() const noexcept { return state_->current ? state_->current->cacheWidth : 0; }
UINT MediaGpuScene::SuperResHeight() const noexcept { return state_->current ? state_->current->cacheHeight : 0; }
SuperResRoiMetadata MediaGpuScene::SuperResRoi() const
{ return state_->current && state_->current->sceneReady ? state_->current->surface->SuperResRoi() : SuperResRoiMetadata{}; }
bool MediaGpuScene::IsSuperResActive() const
{ return state_->current && state_->current->sceneReady && state_->current->surface->IsSuperResActive(); }
bool MediaGpuScene::IsSuperResPerformanceLimited() const
{ return state_->current && state_->current->surface->IsSuperResPerformanceLimited(); }
std::string MediaGpuScene::SuperResStatus() const
{ return state_->current ? state_->current->surface->SuperResStatus() : std::string{}; }
void MediaGpuScene::SetSuperResPerformanceOverride(bool enabled)
{
    state_->performanceOverride = enabled;
    if (state_->current) state_->current->surface->SetSuperResPerformanceOverride(enabled);
}
void MediaGpuScene::ResetSuperRes() {
    if (!state_->current) return;
    state_->current->surface->ResetSuperRes();
    state_->current->forceReprocess = true;
    state_->analysis.reset();
}
void MediaGpuScene::ResetTextClarityHistory() {
    if (!state_->current) return;
    state_->current->surface->ResetTextClarityHistory();
    state_->current->surface->InvalidateSuperResRequest();
    state_->current->forceReprocess = true;
    state_->analysis.reset();
}
bool MediaGpuScene::NeedsSuperResPolling() const
{ return state_->current && state_->current->sceneReady && state_->current->surface->NeedsSuperResPolling(); }
SuperResAsyncStatistics MediaGpuScene::SuperResStatistics() const
{ return state_->current ? state_->current->surface->SuperResStatistics() : SuperResAsyncStatistics{}; }

SuperResRefreshResult MediaGpuScene::RefreshSuperRes()
{
    auto& s = *state_;
    if (!s.current || !s.current->sceneReady) return SuperResRefreshResult::NoWork;
    auto& b = *s.current;
    if (!b.surface->NeedsSuperResPolling()) return SuperResRefreshResult::NoWork;
    // A caller must request fresh presentation sync after polling. Burn any
    // unrecorded graphics reservation; CUDA only waits on recorded reads.
    b.pendingGraphicsSignal = 0;
    FenceSyncParams sync{true, b.lastGraphicsSignal, b.nextValue++};
    const auto result = b.surface->RefreshSuperRes(sync);
    if (result == SuperResRefreshResult::Submitted || result == SuperResRefreshResult::Published)
        b.lastCudaSignal = sync.signalValue;
    if (result == SuperResRefreshResult::Published) {
        ++b.sceneGeneration;
        s.analysis.reset(); // Neural output has a new presentation identity.
    }
    if (result == SuperResRefreshResult::Failed || b.surface->IsFaulted()) {
        s.error = b.surface->LastError(); s.attemptFailed = true; s.RetireCurrent();
        return SuperResRefreshResult::Failed;
    }
    return result;
}

KeystoneTrackingState MediaGpuScene::GetKeystoneTrackingState() const
{ return state_->current ? state_->current->surface->GetKeystoneTrackingState() : KeystoneTrackingState{}; }
void MediaGpuScene::SetKeystoneTrackingPaused(bool paused)
{ if (state_->current) state_->current->surface->SetKeystoneTrackingPaused(paused); }
bool MediaGpuScene::StepKeystoneCorrection(int direction)
{
    if (!state_->current || !state_->current->surface->StepKeystoneCorrection(direction)) return false;
    state_->current->forceReprocess = true;
    return true;
}
ControlDiagnosticSnapshot MediaGpuScene::ControlDiagnostics()
{ return state_->current ? state_->current->surface->ControlDiagnostics() : ControlDiagnosticSnapshot{}; }

std::uint64_t MediaGpuScene::SceneGeneration() const noexcept
{ return state_->current && state_->current->sceneReady ? state_->current->sceneGeneration : 0; }
std::uint64_t MediaGpuScene::HistoryResetCount() const noexcept
{ return state_->current ? state_->current->historyResetCount : 0; }
bool MediaGpuScene::UctCoordinatesReliable() const noexcept
{
    const auto& b = state_->current;
    return b && b->sceneReady && !b->surface->IsSuperResActive();
}

std::uint64_t MediaGpuScene::SceneEpoch() const noexcept
{
    const State& s = *state_;
    return s.current && s.current->sceneReady ? s.current->sceneEpoch : 0;
}

std::uint64_t MediaGpuScene::SceneFrameSequence() const noexcept
{
    const State& s = *state_;
    return s.current && s.current->sceneReady ? s.current->sceneFrameSequence : 0;
}

std::uint64_t MediaGpuScene::SceneAppearance() const noexcept
{
    const State& s = *state_;
    return s.current && s.current->sceneReady ? s.current->sceneAppearance : 0;
}

FenceSyncParams MediaGpuScene::PresentationSync()
{
    State& s = *state_;
    if (!s.current || !s.current->sceneReady) return {};
    Bundle& b = *s.current;
    if (b.pendingGraphicsSignal == 0) b.pendingGraphicsSignal = b.nextValue++;
    FenceSyncParams sync{};
    sync.enable = true;
    sync.waitValue = b.lastCudaSignal;
    sync.signalValue = b.pendingGraphicsSignal;
    sync.presentationFence = b.fence.Get();
    return sync;
}

void MediaGpuScene::RecordPresented()
{
    State& s = *state_;
    if (!s.current) return;
    RecordPresented(s.current->pendingGraphicsSignal);
}

void MediaGpuScene::RecordPresented(std::uint64_t independentReadSignal)
{
    State& s = *state_;
    if (!s.current || independentReadSignal == 0 ||
        independentReadSignal != s.current->pendingGraphicsSignal) {
        return;
    }
    s.current->lastGraphicsSignal = independentReadSignal;
    s.current->pendingGraphicsSignal = 0;
}

uct::SharedDeviceImage MediaGpuScene::TakeUctAnalysis()
{
    State& s = *state_;
    if (!UctCoordinatesReliable()) { s.analysis.reset(); return {}; }
    if (!s.current || !s.current->sceneReady || !s.analysis) return {};
    if (s.analysis->kind == uct::AnalysisKind::TemporallySmoothed &&
        (s.analysis->blendFrameCount < 1 ||
         s.analysis->seedFrameSequence > s.analysis->frameSequence ||
         s.analysis->historyEpoch == 0 || !(s.analysis->temporalAlpha >= 0.0f) ||
         s.analysis->temporalAlpha > 1.0f || !s.analysis->ValidationData())) {
        s.analysis.reset();
        return {};
    }
    if (s.analysis->sourceEpoch != s.current->sceneEpoch ||
        s.analysis->frameSequence != s.current->sceneFrameSequence ||
        s.analysis->processingGeneration != s.current->sceneAppearance) {
        s.analysis.reset();
        return {};
    }
    return std::move(s.analysis);
}

bool MediaGpuScene::UctAnalysisFailed() const
{
#if OKUFLOW_HAS_CUDA_EXT_MEMORY
    const State& s = *state_;
    return s.current && s.current->surface &&
           s.current->surface->UctAnalysisFailed();
#else
    return false;
#endif
}

const std::string& MediaGpuScene::Error() const noexcept
{
    return state_->error;
}

D3D11SourceStatistics MediaGpuScene::SourceStatistics() const
{
#if OKUFLOW_HAS_CUDA_EXT_MEMORY
    const State& s = *state_;
    return s.current && s.current->surface ? s.current->surface->SourceStatistics()
                                           : D3D11SourceStatistics{};
#else
    return {};
#endif
}

MediaGpuScene::Status MediaGpuScene::Release() {
    auto& s = *state_;
    if (s.current || s.init) {
        ++s.generation;
        s.desiredDevice.Reset();
        s.analysis.reset();
        s.RetireCurrent();
    }
    s.Poll();
    if (s.quarantined) return Status::Failed;
    return s.current || s.init || s.retirement ? Status::Pending : Status::Ready;
}

void MediaGpuScene::Invalidate() noexcept
{
    State& s = *state_;
    ++s.generation; // a finished-but-unadopted init bundle retires instead
    s.attemptFailed = false;
    s.desiredDevice.Reset();
    s.desiredWidth = 0;
    s.desiredHeight = 0;
    s.desiredCacheWidth = 0;
    s.desiredCacheHeight = 0;
    s.analysis.reset();
    try {
        s.PollRetirement();
        if (s.current && s.current->surface->IsValid() && !s.current->surface->IsFaulted()) {
            s.current->pendingGraphicsSignal = 0;
            s.current->sceneReady = false;
            s.current->resetPending = true;
            s.current->surface->InvalidateSuperResRequest();
        } else s.RetireCurrent();
    } catch (...) {
        // Invalidate must never destroy GPU resources inline; if even the
        // cleanup handoff failed, leak the bundle rather than release it.
        (void)s.current.release();
        s.quarantined = true;
    }
}

} // namespace okuflow

#endif // _WIN32
