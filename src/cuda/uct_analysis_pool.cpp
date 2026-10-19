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
    }
};
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
    std::shared_ptr<Storage> storage;
    const void *Data() const override
    {
        return storage->pixels;
    }
    bool WaitReady(void *stream, QString &error) const override
    {
        const auto status =
            cudaStreamWaitEvent(static_cast<cudaStream_t>(stream), storage->ready, 0);
        if (status != cudaSuccess)
            error = QString::fromUtf8(cudaGetErrorString(status));
        return status == cudaSuccess;
    }
    bool HasResidentAnalysis() const override { return true; }
    bool PrepareResidentAnalysis(RecognitionResult& result, int decimation, bool probeOnly,
                                  const std::atomic<bool>& cancelled, QString& error) const override
    {
        return resident::Prepare(*this, result, decimation, probeOnly, cancelled, error);
    }
    QImage Readback(const std::atomic<bool> &cancelled, QString &error) const override
    {
        return DownloadImage(cancelled, error, 1);
    }
    QImage ReadProbe(const std::atomic<bool> &cancelled, QString &error) const override
    {
        return DownloadImage(cancelled, error, std::max(1, (std::max(width, height) + 959) / 960));
    }

  private:
    QImage DownloadImage(const std::atomic<bool> &cancelled, QString &error, int decimation) const
    {
        if (cudaSetDevice(device) != cudaSuccess)
        {
            error = "Analysis device is unavailable.";
            return {};
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;)
        {
            const auto status = cudaEventQuery(storage->ready);
            if (status == cudaSuccess)
                break;
            if (status != cudaErrorNotReady || cancelled.load() ||
                std::chrono::steady_clock::now() > deadline)
            {
                error = "Analysis copy did not complete.";
                return {};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (cancelled.load())
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
                       cached->memoryPools != storage->memoryPools))
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
            cached->memoryPools = storage->memoryPools;
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
        transfer->source = storage;
        const void *source = storage->pixels;
        std::size_t stride = pitch;
        if (status == cudaSuccess && decimation > 1)
        {
            if (status == cudaSuccess)
            {
                gpu::Probe(storage->pixels, pitch, width, height, transfer->reduced, decimation,
                           transfer->stream);
                status = cudaGetLastError();
                source = transfer->reduced;
                stride = ow * 4;
            }
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
    std::array<std::shared_ptr<Storage>, 2> entries;
    AnalysisCaptureHealth health;
    AnalysisAllocation allocation{};
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
SharedDeviceImage AnalysisPool::Capture(const void *bgra, std::size_t sourcePitch, int width,
                                        int height, void *stream, std::uint64_t epoch,
                                        std::uint64_t frame, std::uint64_t generation)
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (!state_->health.CanAttempt(now))
        return {};
    if (!bgra || width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        sourcePitch < static_cast<std::size_t>(width) * 4)
    {
        state_->health.Failure(now, false);
        return {};
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    if (bytes > 128ull * 1024 * 1024)
    {
        state_->health.Failure(now, false);
        return {};
    }
    int device{};
    if (cudaGetDevice(&device) != cudaSuccess)
    {
        state_->health.Failure(now, false);
        return {};
    }
    if (state_->allocatorDevice != device)
    {
        int supported{};
        state_->memoryPools = state_->allocation != AnalysisAllocation::Legacy &&
                              cudaDeviceGetAttribute(&supported, cudaDevAttrMemoryPoolsSupported,
                                                     device) == cudaSuccess &&
                              supported;
        state_->allocatorDevice = device;
    }
    for (auto &slot : state_->entries)
    {
        if (slot && slot->device != device)
            slot.reset();
        if (slot && !slot->completionKnown)
        {
            slot.reset();
            state_->health.Failure(now, true);
            return {};
        }
        if (slot && slot.use_count() != 1)
            continue;
        if (slot)
        {
            const auto status = cudaEventQuery(slot->ready);
            if (status == cudaErrorNotReady)
                continue;
            if (status != cudaSuccess)
            {
                slot->completionKnown = false;
                state_->health.Failure(now, true);
                return {};
            }
        }
        if (slot && slot->capacity < bytes)
        {
            auto retired = std::move(slot);
            AsyncCleanup::Start([retired] {
                cudaSetDevice(retired->device);
                return true;
            });
        }
        if (!slot)
        {
            slot = std::shared_ptr<Storage>(new Storage, [](Storage *storage) {
                AsyncCleanup::Start([storage] {
                    cudaSetDevice(storage->device);
                    if (!storage->completionKnown ||
                        (storage->ready && cudaEventSynchronize(storage->ready) != cudaSuccess))
                        return false;
                    delete storage;
                    return true;
                });
            });
            slot->device = device;
            slot->memoryPools = state_->memoryPools;
            if (cudaEventCreateWithFlags(&slot->ready, cudaEventDisableTiming) != cudaSuccess ||
                (slot->memoryPools
                     ? cudaMallocAsync(&slot->pixels, bytes, static_cast<cudaStream_t>(stream))
                     : cudaMalloc(&slot->pixels, bytes)) != cudaSuccess)
            {
                state_->health.Failure(now, false);
                auto retired = std::move(slot);
                AsyncCleanup::Start([retired] {
                    cudaSetDevice(retired->device);
                    return true;
                });
                return {};
            }
            slot->capacity = bytes;
        }
        slot->completionKnown = false;
        if (cudaMemcpy2DAsync(slot->pixels, width * 4, bgra, sourcePitch, width * 4, height,
                              cudaMemcpyDeviceToDevice,
                              static_cast<cudaStream_t>(stream)) != cudaSuccess ||
            cudaEventRecord(slot->ready, static_cast<cudaStream_t>(stream)) != cudaSuccess)
        {
            state_->health.Failure(now, true);
            return {};
        }
        slot->completionKnown = true;
        state_->health.Success();
        auto image = std::make_shared<Image>();
        image->storage = slot;
        image->width = width;
        image->height = height;
        image->pitch = static_cast<std::size_t>(width) * 4;
        image->device = slot->device;
        image->sourceEpoch = epoch;
        image->frameSequence = frame;
        image->processingGeneration = generation;
        image->timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
        return image;
    }
    return {};
}
} // namespace okuflow::uct
#endif
