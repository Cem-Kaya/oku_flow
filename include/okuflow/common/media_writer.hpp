#pragma once

#ifdef _WIN32

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11_4.h>

#include <wrl/client.h>

#include "okuflow/common/recording_contract.hpp"
#include "okuflow/common/gpu_read_retirement.hpp"
#include "okuflow/common/recording_preservation.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace okuflow {

// A fresh sibling filename for another encoder attempt. The original attempt
// remains untouched, including metadata written before initialization failed.
std::wstring NewRecordingAttemptPath(const std::wstring& requestedPath);

// Normalizes a CPU BGRA fallback to the GPU writer's tightly packed,
// BT.601 limited-range NV12 input contract. Never relabel BGRA as NV12.
bool ConvertRecordingBgraToNv12(const std::uint8_t* bgra,
                                std::size_t strideBytes,
                                UINT width, UINT height,
                                std::vector<std::uint8_t>& nv12);

// A completed, shareable BGRA GPU texture. The opaque lifetime token owns the
// D3D allocation and its NT handles until Media Foundation has opened them.
// readyFenceValue identifies the graphics-queue signal that completed all
// writes to the texture.
struct GpuVideoFrame {
    HANDLE textureSharedHandle{nullptr};
    HANDLE fenceSharedHandle{nullptr};
    UINT64 readyFenceValue{0};
    UINT width{0};
    UINT height{0};
    std::shared_ptr<void> lifetime;

    bool IsValid() const {
        return textureSharedHandle != nullptr &&
               fenceSharedHandle != nullptr &&
               readyFenceValue > 0 && width > 0 && height > 0 &&
               lifetime != nullptr;
    }
};

// Bounds samples accepted by an unthrottled sink writer using locally counted
// successful submissions and the last confirmed encoder output count. A failed
// or inconsistent statistics query never grants new completion credit. This
// policy is independent of COM so tests can supply delayed/failing statistics.
class RecordingEncoderBacklog {
public:
    static constexpr std::uint64_t kVideoSampleLimit = 12;
    static constexpr std::uint64_t kVideoByteLimit = 128ull * 1024 * 1024;

    RecordingEncoderBacklog(std::uint64_t sampleLimit = kVideoSampleLimit,
                            std::uint64_t byteLimit = kVideoByteLimit)
        : sampleLimit_(sampleLimit), byteLimit_(byteLimit) {}

    // One sample stays pending until the next arrives. Both must fit before
    // starting, or no sample could ever reach WriteSample during recording.
    static bool CanStart(std::uint64_t sampleBytes,
                         std::uint64_t sampleLimit = kVideoSampleLimit,
                         std::uint64_t byteLimit = kVideoByteLimit);

    bool CanAccept(const MF_SINK_WRITER_STATISTICS* statistics,
                   std::uint64_t submittedSamples,
                   std::uint64_t pendingSamples,
                   std::uint64_t sampleBytes);
    void Reset();
    bool StatisticsConfirmed() const { return statisticsConfirmed_; }

private:
    std::uint64_t sampleLimit_;
    std::uint64_t byteLimit_;
    std::uint64_t encodedSamples_{};
    std::uint64_t queuedBytes_{};
    std::uint64_t largestSampleBytes_{};
    bool statisticsConfirmed_{};
};

// Microsoft's configured raw LC AAC emits one packet per 1024 PCM frames,
// independently of input block sizes/timestamps. Retire cumulative PCM frames,
// retaining two packets of credit as a conservative encoder-delay allowance.
// Worker-confined; this pure accounting seam opens no encoder or camera.
class RecordingAudioBacklog {
public:
    static constexpr std::uint64_t kBlockLimit = 128;
    static constexpr std::uint64_t kByteLimit = 4ull * 1024 * 1024;
    static constexpr std::int64_t kDurationLimit100ns = 20'000'000;

    RecordingAudioBacklog(std::uint64_t blockLimit = kBlockLimit,
                          std::uint64_t byteLimit = kByteLimit,
                          std::int64_t durationLimit100ns = kDurationLimit100ns)
        : blockLimit_(blockLimit), byteLimit_(byteLimit),
          durationLimit100ns_(durationLimit100ns) {}
    bool CanAccept(const MF_SINK_WRITER_STATISTICS* statistics,
                   std::int64_t sampleTime100ns, std::int64_t duration100ns,
                   std::uint64_t bytes);
    // Call only after successful WriteSample. A failed write earns no credit.
    bool RecordAccepted(std::int64_t sampleTime100ns,
                        std::int64_t duration100ns, std::uint64_t bytes);
    void Reset(std::uint64_t bytesPerPcmFrame = 2);
    bool StatisticsConfirmed() const { return statisticsConfirmed_; }
    std::uint64_t OutstandingBytes() const { return outstandingBytes_; }
    std::size_t OutstandingBlocks() const { return blocks_.size(); }

private:
    struct Block {
        std::int64_t duration;
        std::uint64_t bytes, cumulativePcmEnd;
    };
    bool Fits(std::int64_t time, std::int64_t duration,
              std::uint64_t bytes) const;
    std::deque<Block> blocks_;
    std::uint64_t blockLimit_, byteLimit_, submittedBlocks_{}, encodedPackets_{};
    std::int64_t durationLimit100ns_, outstandingDuration100ns_{};
    std::uint64_t outstandingBytes_{}, queuedBytes_{};
    std::uint64_t bytesPerPcmFrame_{2}, submittedPcmFrames_{};
    bool statisticsConfirmed_{};
};

// Media Foundation sink-writer wrapper for live AV1 or H.264 recording.
// The output container is fragmented MP4 (fMP4): fragments are flushed to disk
// while recording, so the file stays playable up to the last completed fragment
// even if the process dies before Finalize(). The file keeps the .mp4 extension.
class VideoRecorder {
public:
    enum class Codec { Av1, H264 };

    struct AudioFormat {
        UINT sampleRate{48000};
        UINT channels{1};
        UINT bitsPerSample{16};

        bool IsValid() const {
            return sampleRate > 0 && channels > 0 &&
                   bitsPerSample == 16;
        }
    };

    // Why the recorder last transitioned from recording to stopped.
    enum class StopReason { None, Manual, DiskFull, WriteFailed };
    enum class FinalizeDisposition {
        NothingToFinalize,
        Completed,
        CompletedTruncated,
    };

    struct FinalizeResult {
        FinalizeDisposition disposition{
            FinalizeDisposition::NothingToFinalize};
        long hresult{0};
        double playableSeconds{0.0};
        std::uint64_t videoSamplesWritten{0};

        bool FullyCompleted() const {
            return disposition == FinalizeDisposition::Completed;
        }

        bool HasPlayableVideo() const {
            return FullyCompleted() && videoSamplesWritten > 0 &&
                   playableSeconds > 0.0;
        }
    };

    // The operation this recorder is currently executing. Several of these
    // calls (WriteSample, Map, driver/COM entries) are synchronous and can
    // block without a deadline; the stage stays observable from other threads
    // while the recorder is wedged, so a watchdog can name the exact blocked
    // call instead of reporting a generic hang.
    enum class WriterStage : std::uint8_t {
        kIdle = 0,
        kOpenSharedTexture,
        kOpenSharedFence,
        kGpuSync,
        kReadbackMap,
        kCreateEncoderTexture,
        kConvertNv12,
        kSubmitGpuWork,
        kCreateSampleBuffer,
        kWriteVideoSample,
        kWriteAudioSample,
        kQueryEncoderStatistics,
        kRetireGpuReaders,
        kFinalize,
    };
    static const char* StageName(WriterStage stage);
    WriterStage ActiveStage() const {
        return stage_.load(std::memory_order_relaxed);
    }
    // Called when the recording worker was declared permanently blocked
    // inside one of this recorder's synchronous calls. After this, Stop()
    // and the destructor never touch the sink writer again — calling
    // Finalize concurrently with a wedged WriteSample on the same COM
    // object is forbidden — and teardown deliberately leaks the COM
    // references for process exit to reclaim.
    void MarkAbandoned() { abandoned_.store(true); }
    bool IsAbandoned() const { return abandoned_.load(); }

    VideoRecorder();
    ~VideoRecorder();

    // Refuses to start when the target volume has less than 500 MB free
    // (LastError() explains why). While recording, free space is re-checked
    // every ~5 seconds; below 200 MB the recording is finalized cleanly and
    // AddFrame() returns false with StopReason::DiskFull.
    bool Start(const std::wstring& filePath,
               UINT width,
               UINT height,
               UINT frameRateNumerator,
               UINT frameRateDenominator,
               Codec codec,
               const AudioFormat* audioFormat = nullptr);
    // Starts the same fMP4 writer with a D3D device manager matched to the
    // probe frame's shared allocation. AddGpuFrame can then feed DXGI-backed
    // samples directly to a hardware encoder without a system-memory upload.
    // Returns false when the adapter/driver cannot open the shared allocation;
    // callers keep the regular Start/AddFrame path as the compatibility rung.
    bool StartGpu(const std::wstring& filePath,
                  UINT width,
                  UINT height,
                  UINT frameRateNumerator,
                  UINT frameRateDenominator,
                  Codec codec,
                  const GpuVideoFrame& probeFrame,
                  const AudioFormat* audioFormat = nullptr);
    FinalizeResult Stop();
    bool IsRecording() const { return recording_; }

    bool AddFrame(const uint8_t* bgraData,
                  size_t strideBytes,
                  const RecordingFrameIdentity& identity);
    bool AddGpuFrame(const GpuVideoFrame& frame,
                     const RecordingFrameIdentity& identity);
    bool AddAudioFrame(const std::uint8_t* pcmData,
                       std::size_t byteCount,
                       std::int64_t sampleTime100ns,
                       std::int64_t duration100ns);

    // Worker-thread-only check for the negotiated NV12 or packed BGRA input.
    // Call for BOTH recorders before admitting a
    // matched video pair. False means drop this pair, not writer failure;
    // it leaves timelines and pending samples untouched. Do not call from UI
    // watchdogs, or concurrently with WriteSample/Finalize on this writer.
    bool CanAcceptVideoFrame();
    // Check BOTH writers before submitting either PCM block. False means the
    // finite audio budget is exhausted or input is invalid; the manager ends
    // the session explicitly rather than silently dropping recorded speech.
    bool CanAcceptAudioFrame(std::size_t byteCount, std::int64_t sampleTime100ns,
                             std::int64_t duration100ns);
    // Recording-worker-only retirement, including while no new frame can be
    // admitted. PollGpuInputReads performs one nonblocking query per lease;
    // it never writes/finalizes a sample or waits. An idle worker may allow
    // query-command flushing for progress; ordinary admission does not.
    // False reports a reader fault/abandonment; Pending retains the source.
    bool HasPendingGpuReads() const { return !pendingGpuReads_.empty(); }
    bool PollGpuInputReads(bool allowFlush = false);
    long LastEncoderStatisticsError() const {
        return lastEncoderStatisticsError_;
    }

    double DurationSeconds() const;
    const std::string& LastError() const { return lastError_; }
    // The selected attempt may differ from Start's requested path after a
    // GPU-input fallback. Retained through Stop for finalization/publication.
    const std::wstring& OutputPath() const { return targetPath_; }
    bool UsesGpuInput() const { return gpuInputEnabled_; }
    bool UsesWorkerGpuReadback() const {
        return gpuCompatibilityReadback_;
    }
    static const char* CodecName(Codec codec);
    // Pure startup-configuration seam, also used before any output sink is
    // created. nativeNv12 selects the actual negotiated input payload.
    static bool InputFitsEncoderBudget(UINT width, UINT height,
                                       bool nativeNv12);

private:
    friend struct VideoRecorderTestAccess;
    // Read-only fixture seam, except that a fixture may throw after writing
    // diagnostic bytes to the exclusively owned stream. Empty in production.
    std::function<void(bool, IMFByteStream*)> sinkInitializationObserver_;
    bool InitializeSink(const std::wstring& filePath,
                        UINT width,
                        UINT height,
                        UINT frameRateNumerator,
                        UINT frameRateDenominator,
                        Codec codec,
                        const AudioFormat* audioFormat,
                        IUnknown* d3dManager,
                        const GUID& inputSubtype);
    static std::uint64_t InputPayloadBytes(UINT width, UINT height,
                                           bool nativeNv12);
    bool InitializeGpuDevice(HANDLE probeTextureHandle);
    bool InitializeGpuVideoProcessor(UINT width,
                                     UINT height,
                                     UINT frameRateNumerator,
                                     UINT frameRateDenominator);
    void ResetGpuVideoProcessor();
    bool QueueVideoSample(
        Microsoft::WRL::ComPtr<IMFSample> sample,
        const RecordingFrameIdentity& identity);
    bool ValidAudioInput(std::size_t byteCount, std::int64_t sampleTime100ns,
                         std::int64_t duration100ns) const;
    bool CanAcceptAudioBlock(std::size_t byteCount, std::int64_t sampleTime100ns,
                             std::int64_t duration100ns);
    bool WriteAudioBlock(const std::uint8_t* pcmData, std::size_t byteCount,
                         std::int64_t sampleTime100ns, std::int64_t duration100ns);
    bool CheckDiskSpace();
    FinalizeResult FinalizeAndStop(StopReason reason);
    bool WritePendingSample(std::int64_t duration100ns);
    bool QueryEncoderAdmission(DWORD streamIndex,
                               RecordingEncoderBacklog& backlog,
                               std::uint64_t submittedSamples,
                               std::uint64_t pendingSamples,
                               std::uint64_t sampleBytes);
    void LeakComForProcessExit();
    void PollGpuReadLeases(bool allowFlush = false);
    struct GpuReadLease {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        Microsoft::WRL::ComPtr<ID3D11Fence> producerFence;
        UINT64 producerValue{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> input, output;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
        std::shared_ptr<void> source;
        bool queryRecorded{};
    };
    struct PendingGpuRead {
        std::shared_ptr<GpuReadLease> backing;
        std::shared_ptr<GpuReadRetirement> retirement;
    };
    std::vector<PendingGpuRead> pendingGpuReads_;
    bool gpuReadFaulted_{};
    HRESULT lastReaderQueryHr_{S_OK};
    HRESULT lastReaderDeviceHr_{S_OK};
    BOOL lastReaderQueryComplete_{};
    UINT64 lastReaderProducerCompleted_{}, lastReaderProducerRequired_{};
    void SetError(const std::string& err);

    IMFTransform* colorConverter_{nullptr}; // unused for now; reserved.
    Microsoft::WRL::ComPtr<IMFSinkWriter> sinkWriter_;
    Microsoft::WRL::ComPtr<ID3D11Device> gpuDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> gpuContext_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> gpuDeviceManager_;
    Microsoft::WRL::ComPtr<ID3D11Fence> gpuFence_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuReadbackTexture_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> gpuVideoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> gpuVideoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator>
        gpuVideoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> gpuVideoProcessor_;
    UINT gpuResetToken_{0};
    DWORD streamIndex_{0};
    DWORD audioStreamIndex_{0};
    UINT frameWidth_{0};
    UINT frameHeight_{0};
    UINT frameRateNumerator_{30};
    UINT frameRateDenominator_{1};
    RecordingTimeline timeline_;
    Microsoft::WRL::ComPtr<IMFSample> pendingSample_;
    std::int64_t pendingSampleTime100ns_{-1};
    std::int64_t timelineEnd100ns_{0};
    bool recording_{false};
    std::wstring targetPath_;
    ULONGLONG lastSpaceCheckTicks_{0};
    StopReason stopReason_{StopReason::None};
    Codec activeCodec_{Codec::H264};
    AudioFormat audioFormat_{};
    bool audioEnabled_{false};
    bool gpuInputEnabled_{false};
    bool gpuCompatibilityReadback_{false};
    std::uint64_t videoSamplesWritten_{0};
    RecordingEncoderBacklog videoBacklog_;
    RecordingAudioBacklog audioBacklog_;
    RecordingPcmTimeline audioTimeline_;
    long lastEncoderStatisticsError_{0};
    // Stop is idempotent until the next Start, including automatic stops.
    RecordingTerminalResult<FinalizeResult> terminalResult_;
    long writeFailureHr_{0};
    std::atomic<WriterStage> stage_{WriterStage::kIdle};
    std::atomic<bool> abandoned_{false};
    std::string lastError_;
};

} // namespace okuflow

#endif // _WIN32
