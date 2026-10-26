#include "okuflow/cuda/uct_analysis_pool.hpp"
#include "okuflow/common/async_cleanup.hpp"

#if OKUFLOW_ENABLE_CUDA
#include "okuflow/cuda/uct_kernels.hpp"
#include "uct_device_analysis.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <cuda_runtime.h>
#include <thread>

namespace okuflow::uct
{
namespace
{
struct Storage
{
    void *pixels{};
    void *holdHost{}; // pinned Extra Stable record, written by this slot's copy
    cudaEvent_t ready{};
    int device{};
    std::size_t capacity{};
    bool completionKnown{true};
    bool memoryPools{};
    ~Storage()
    {
        if (ready)
            cudaEventDestroy(ready);
        if (pixels)
            cudaFree(pixels);
        if (holdHost)
            cudaFreeHost(holdHost);
    }
};
// Bounded, cancellable worker wait for one storage's own copy event.
bool WaitForCopy(const Storage &source, int device, const std::atomic<bool> &cancelled,
                 QString &error)
{
    if (cudaSetDevice(device) != cudaSuccess)
    {
        error = "Analysis device is unavailable.";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;)
    {
        const auto status = cudaEventQuery(source.ready);
        if (status == cudaSuccess)
            return !cancelled.load();
        if (status != cudaErrorNotReady || cancelled.load() ||
            std::chrono::steady_clock::now() > deadline)
        {
            error = "Analysis copy did not complete.";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Download
{
    void *pixels{};
    void *reduced{};
    cudaStream_t stream{};
    int device{};
    std::size_t capacity{};
    std::shared_ptr<Storage> source;
    bool memoryPools{};
    ~Download()
    {
        cudaSetDevice(device);
        if (reduced)
        {
            if (memoryPools)
                cudaFreeAsync(reduced, stream);
            else
                cudaFree(reduced);
        }
        if (stream)
            cudaStreamDestroy(stream);
        if (pixels)
            cudaFreeHost(pixels);
    }
};
class Image final : public DeviceImage
{
  public:
    std::shared_ptr<Storage> storage;             // OCR pixels (Data)
    std::shared_ptr<Storage> validationStorage;   // single exact current frame
    bool holdRecorded{};                          // storage->holdHost belongs to this copy
    bool HasHoldProvenance() const override
    {
        return holdRecorded;
    }
    HoldProvenance ReadHoldProvenance(const std::atomic<bool> &cancelled,
                                      QString &error) const override
    {
        // The pinned record was written by the same stream-ordered copy as the
        // pixels. Read it only after that copy's event, on the calling worker.
        if (!holdRecorded || !storage->holdHost || !WaitForCopy(*storage, device, cancelled, error))
            return {HoldDisplay::Unknown, 0, 0.0f};
        HoldProvenanceRecord record{};
        std::memcpy(&record, storage->holdHost, sizeof(record));
        return DecodeHoldProvenance(record);
    }
    const void *Data() const override
    {
        return storage->pixels;
    }
    const void *ValidationData() const override
    {
        return validationStorage->pixels;
    }
    std::size_t ValidationPitch() const override
    {
        return pitch;
    }
    bool WaitReady(void *stream, QString &error) const override
    {
        return WaitOn(storage, stream, error);
    }
    bool WaitValidationReady(void *stream, QString &error) const override
    {
        return WaitOn(validationStorage, stream, error);
    }
    bool HasResidentAnalysis() const override { return true; }
    bool PrepareResidentAnalysis(RecognitionResult& result, int decimation, bool probeOnly,
                                  const std::atomic<bool>& cancelled, QString& error) const override
    {
        return resident::Prepare(*this, result, decimation, probeOnly, cancelled, error);
    }
    bool PrepareResidentInkOutlines(RecognitionResult& result,const std::atomic<bool>& cancelled,QString& error) const override
    {
        return resident::PrepareInkOutlines(*this,result,cancelled,error);
    }
    QImage Readback(const std::atomic<bool> &cancelled, QString &error) const override
    {
        return DownloadImage(storage, cancelled, error, 1);
    }
    QImage ReadValidation(const std::atomic<bool> &cancelled, QString &error) const override
    {
        return DownloadImage(validationStorage, cancelled, error, 1);
    }
    QImage ReadProbe(const std::atomic<bool> &cancelled, QString &error) const override
    {
        return DownloadImage(validationStorage, cancelled, error,
                             std::max(1, (std::max(width, height) + 959) / 960));
    }

  private:
    static bool WaitOn(const std::shared_ptr<Storage> &source, void *stream, QString &error)
    {
        const auto status =
            cudaStreamWaitEvent(static_cast<cudaStream_t>(stream), source->ready, 0);
        if (status != cudaSuccess)
            error = QString::fromUtf8(cudaGetErrorString(status));
        return status == cudaSuccess;
    }
    QImage DownloadImage(const std::shared_ptr<Storage> &sourceStorage,
                         const std::atomic<bool> &cancelled, QString &error, int decimation) const
    {
        if (!WaitForCopy(*sourceStorage, device, cancelled, error))
            return {};
        // No legacy/default-stream copy: a private nonblocking stream and
        // pinned host allocation keep this validation/fallback branch separate.
        // On unknown completion, retain both DMA endpoints until process exit.
        const int ow = width / decimation, oh = height / decimation;
        const auto bytes = static_cast<std::size_t>(ow) * oh * 4;
        // Each reader thread reuses its own stream and pinned/device buffers.
        // Steady-state probes do not allocate/free driver resources per frame.
        thread_local std::array<std::shared_ptr<Download>, 2> downloads;
        auto &cached = downloads[decimation > 1 ? 0 : 1];
        if (cached && (cached->device != device || cached->capacity < bytes ||
                       cached->memoryPools != sourceStorage->memoryPools))
        {
            auto retired = std::move(cached);
            AsyncCleanup::Start([retired] {
                return cudaSetDevice(retired->device) == cudaSuccess &&
                       cudaStreamSynchronize(retired->stream) == cudaSuccess;
            });
        }
        auto status = cudaSuccess;
        if (!cached)
        {
            cached = std::make_shared<Download>();
            cached->device = device;
            cached->memoryPools = sourceStorage->memoryPools;
            cached->capacity = bytes;
            status = cudaStreamCreateWithFlags(&cached->stream, cudaStreamNonBlocking);
            if (status == cudaSuccess)
                status = cudaHostAlloc(&cached->pixels, bytes, cudaHostAllocDefault);
            if (status == cudaSuccess && decimation > 1)
                status = cached->memoryPools
                             ? cudaMallocAsync(&cached->reduced, bytes, cached->stream)
                             : cudaMalloc(&cached->reduced, bytes);
        }
        auto transfer = cached;
        transfer->source = sourceStorage;
        const void *source = sourceStorage->pixels;
        std::size_t stride = pitch;
        if (status == cudaSuccess && decimation > 1)
        {
            gpu::Probe(sourceStorage->pixels, pitch, width, height, transfer->reduced,
                       decimation, transfer->stream);
            status = cudaGetLastError();
            source = transfer->reduced;
            stride = ow * 4;
        }
        if (status == cudaSuccess)
            status = cudaMemcpy2DAsync(transfer->pixels, ow * 4, source, stride, ow * 4, oh,
                                       cudaMemcpyDeviceToHost, transfer->stream);
        if (status != cudaSuccess)
        {
            cached.reset();
            error = QString::fromUtf8(cudaGetErrorString(status));
            AsyncCleanup::Start([transfer, device = device] {
                return cudaSetDevice(device) == cudaSuccess &&
                       (!transfer->stream ||
                        cudaStreamSynchronize(transfer->stream) == cudaSuccess);
            });
            return {};
        }
        status = cudaStreamSynchronize(transfer->stream); // recognition worker only
        if (status != cudaSuccess)
        {
            cached.reset();
            error = QString::fromUtf8(cudaGetErrorString(status));
            AsyncCleanup::Start([transfer] { return false; });
            return {};
        }
        transfer->source.reset();
        if (cancelled.load())
            return {};
        QImage result(ow, oh, QImage::Format_ARGB32);
        if (result.isNull())
        {
            error = "Analysis host allocation failed.";
            return {};
        }
        std::memcpy(result.bits(), transfer->pixels, bytes);
        return result;
    }
};
} // namespace
struct AnalysisPool::State
{
    struct Slot
    {
        std::shared_ptr<Storage> ocr;
        std::shared_ptr<Storage> validation;
    };
    std::array<Slot, 2> entries;
    AnalysisCaptureHealth health;
    AnalysisAllocation allocation{};
    InjectedFault injected{InjectedFault::None};
    bool Consume(InjectedFault fault)
    {
        if (injected != fault)
            return false;
        injected = InjectedFault::None;
        return true;
    }
    int allocatorDevice{-1};
    bool memoryPools{};
};
AnalysisPool::AnalysisPool(AnalysisAllocation allocation) : state_(std::make_shared<State>())
{
    state_->allocation = allocation;
}
AnalysisPool::~AnalysisPool() = default;
bool AnalysisPool::HasFailure() const
{
    return state_->health.Failed();
}
void AnalysisPool::InjectFaultForTesting(InjectedFault fault)
{
    state_->injected = fault;
}
namespace
{
std::shared_ptr<Storage> MakeStorage()
{
    return std::shared_ptr<Storage>(new Storage, [](Storage *storage) {
        AsyncCleanup::Start([storage] {
            cudaSetDevice(storage->device);
            if (!storage->completionKnown ||
                (storage->ready && cudaEventSynchronize(storage->ready) != cudaSuccess))
                return false;
            delete storage;
            return true;
        });
    });
}
void RetireStorage(std::shared_ptr<Storage> &slot)
{
    auto retired = std::move(slot);
    AsyncCleanup::Start([retired] {
        cudaSetDevice(retired->device);
        return true;
    });
}
} // namespace
SharedDeviceImage AnalysisPool::CaptureInternal(const void *ocr, std::size_t ocrPitch, const void *validation, std::size_t validationPitch, int width, int height, void *streamHandle, std::uint64_t epoch, std::uint64_t frame, std::uint64_t generation, std::uint64_t historyEpoch, std::uint64_t seedFrame, std::uint64_t blendFrameCount, float temporalAlpha, const void *holdProvenance)
{
    auto &state = *state_;
    const bool paired = validation != nullptr;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (!state.health.CanAttempt(now))
        return {};
    if (!ocr || width <= 0 || height <= 0 || width > 8192 ||
        height > 8192 ||
        ocrPitch < static_cast<std::size_t>(width) * 4 ||
        (paired && validationPitch < static_cast<std::size_t>(width) * 4))
    {
        state.health.Failure(now, false);
        return {};
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    // The 128 MiB bound covers the whole published image: a pair counts both.
    if (bytes * (paired ? 2 : 1) > 128ull * 1024 * 1024)
    {
        state.health.Failure(now, false);
        return {};
    }
    int device{};
    if (cudaGetDevice(&device) != cudaSuccess)
    {
        state.health.Failure(now, false);
        return {};
    }
    if (state.allocatorDevice != device)
    {
        int supported{};
        state.memoryPools = state.allocation != AnalysisAllocation::Legacy &&
                            cudaDeviceGetAttribute(&supported, cudaDevAttrMemoryPoolsSupported,
                                                   device) == cudaSuccess &&
                            supported;
        state.allocatorDevice = device;
    }
    for (auto &slot : state.entries)
    {
        std::shared_ptr<Storage> *required[2] = {&slot.ocr, &slot.validation};
        const int requiredCount = paired ? 2 : 1;
        bool busy = false;
        for (int i = 0; i < requiredCount && !busy; ++i)
        {
            auto &storage = *required[i];
            if (storage && storage->device != device)
                storage.reset();
            if (storage && !storage->completionKnown)
            {
                storage.reset();
                state.health.Failure(now, true);
                return {};
            }
            if (storage && storage.use_count() != 1)
                busy = true;
            else if (storage)
            {
                const auto status = cudaEventQuery(storage->ready);
                if (status == cudaErrorNotReady)
                    busy = true;
                else if (status != cudaSuccess)
                {
                    storage->completionKnown = false;
                    state.health.Failure(now, true);
                    return {};
                }
            }
        }
        if (busy)
            continue;
        for (int i = 0; i < requiredCount; ++i)
        {
            auto &storage = *required[i];
            if (storage && storage->capacity < bytes)
                RetireStorage(storage);
            if (!storage)
            {
                storage = MakeStorage();
                storage->device = device;
                storage->memoryPools = state.memoryPools;
                const auto allocationStream = static_cast<cudaStream_t>(streamHandle);
                // Nothing is queued yet: a failed event creation or
                // allocation leaves completion known, and the deleter frees
                // whatever exists. Handled errors are cleared so display
                // launch checks never inherit this UCT-only failure.
                if (state.Consume(InjectedFault::CleanAllocation) ||
                    cudaEventCreateWithFlags(&storage->ready, cudaEventDisableTiming) != cudaSuccess ||
                    (storage->memoryPools
                         ? cudaMallocAsync(&storage->pixels, bytes, allocationStream)
                         : cudaMalloc(&storage->pixels, bytes)) != cudaSuccess)
                {
                    storage->pixels = nullptr;
                    (void)cudaGetLastError();
                    state.health.Failure(now, false);
                    RetireStorage(storage);
                    return {};
                }
                // A stream-ordered allocation is queued work: unknown until
                // an event on the allocating stream proves it, and never
                // freed or reused against an unrecorded event. A legacy
                // allocation queued nothing, so it may retire if the event
                // cannot be recorded before any copy.
                if (storage->memoryPools)
                    storage->completionKnown = false;
                if (cudaEventRecord(storage->ready, allocationStream) != cudaSuccess)
                {
                    (void)cudaGetLastError();
                    state.health.Failure(now, !storage->completionKnown);
                    RetireStorage(storage);
                    return {};
                }
                storage->completionKnown = true;
                storage->capacity = bytes;
            }
        }
        // The Extra Stable record needs pinned host bytes owned by this slot.
        // Allocation happens once per slot; nothing is queued before it.
        if (holdProvenance && !slot.ocr->holdHost &&
            cudaHostAlloc(&slot.ocr->holdHost, sizeof(HoldProvenanceRecord), cudaHostAllocDefault) !=
                cudaSuccess)
        {
            slot.ocr->holdHost = nullptr;
            (void)cudaGetLastError();
            state.health.Failure(now, false);
            return {};
        }
        // Each copy is queued on the caller's stream and proven by its own
        // event before the next half is touched. A half becomes unknown only
        // once its copy is actually queued: a rejected enqueue queued nothing
        // and keeps the earlier proof, while a queued copy without its event
        // stays unknown and quarantined. Either way nothing is published.
        const auto stream = static_cast<cudaStream_t>(streamHandle);
        const auto enqueue = [&](Storage &target, const void *source, std::size_t sourcePitch,
                                 const void *hold) {
            if (cudaMemcpy2DAsync(target.pixels, width * 4, source, sourcePitch, width * 4, height,
                                  cudaMemcpyDeviceToDevice, stream) != cudaSuccess)
                return false;
            target.completionKnown = false;
            if (hold && cudaMemcpyAsync(target.holdHost, hold, sizeof(HoldProvenanceRecord),
                                        cudaMemcpyDeviceToHost, stream) != cudaSuccess)
                return false;
            if (state.Consume(InjectedFault::UnknownCompletion) ||
                cudaEventRecord(target.ready, stream) != cudaSuccess)
                return false;
            target.completionKnown = true;
            return true;
        };
        const bool queued = enqueue(*slot.ocr, ocr, ocrPitch, holdProvenance) &&
                            (!paired || enqueue(*slot.validation, validation, validationPitch, nullptr));
        if (!queued)
        {
            (void)cudaGetLastError();
            state.health.Failure(now, !slot.ocr->completionKnown ||
                                          (paired && !slot.validation->completionKnown));
            return {};
        }
        state.health.Success();
        auto image = std::make_shared<Image>();
        image->storage = slot.ocr;
        image->validationStorage = paired ? slot.validation : slot.ocr;
        image->holdRecorded = holdProvenance != nullptr;
        image->width = width;
        image->height = height;
        image->pitch = static_cast<std::size_t>(width) * 4;
        image->device = slot.ocr->device;
        image->sourceEpoch = epoch;
        image->frameSequence = frame;
        image->processingGeneration = generation;
        image->kind = paired ? AnalysisKind::TemporallySmoothed : AnalysisKind::SingleFrame;
        image->historyEpoch = paired ? historyEpoch : 0;
        image->seedFrameSequence = paired ? seedFrame : frame;
        image->blendFrameCount = paired ? blendFrameCount : 1;
        image->temporalAlpha = paired ? temporalAlpha : 0.0f;
        image->timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
        return image;
    }
    return {};
}
SharedDeviceImage AnalysisPool::Capture(const void *bgra, std::size_t sourcePitch, int width,
                                        int height, void *stream, std::uint64_t epoch,
                                        std::uint64_t frame, std::uint64_t generation,
                                        const void *holdProvenance)
{
    return CaptureInternal(bgra, sourcePitch, nullptr, 0, width, height, stream, epoch, frame, generation, 0, frame, 1, 0.0f, holdProvenance);
}
SharedDeviceImage AnalysisPool::CapturePair(const void *ocr, std::size_t ocrPitch,
                                            const void *validation, std::size_t validationPitch,
                                            int width, int height, void *stream,
                                            std::uint64_t epoch, std::uint64_t frame,
                                            std::uint64_t generation, std::uint64_t historyEpoch,
                                            std::uint64_t seedFrame, std::uint64_t blendFrameCount,
                                            float temporalAlpha)
{
    if (!validation)
    {
        state_->health.Failure(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count(),
            false);
        return {};
    }
    return CaptureInternal(ocr, ocrPitch, validation, validationPitch, width, height, stream, epoch, frame, generation, historyEpoch, seedFrame, blendFrameCount, temporalAlpha, nullptr);
}
} // namespace okuflow::uct
#endif
