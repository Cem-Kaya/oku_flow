#pragma once

#ifdef _WIN32

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11_4.h>

#include <wrl/client.h>

#include "openzoom/common/recording_contract.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace openzoom {

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

    double DurationSeconds() const;
    const std::string& LastError() const { return lastError_; }
    StopReason LastStopReason() const { return stopReason_; }
    Codec ActiveCodec() const { return activeCodec_; }
    bool UsesGpuInput() const { return gpuInputEnabled_; }
    bool UsesWorkerGpuReadback() const {
        return gpuCompatibilityReadback_;
    }
    static const char* CodecName(Codec codec);

private:
    bool InitializeSink(const std::wstring& filePath,
                        UINT width,
                        UINT height,
                        UINT frameRateNumerator,
                        UINT frameRateDenominator,
                        Codec codec,
                        const AudioFormat* audioFormat,
                        IUnknown* d3dManager,
                        const GUID& inputSubtype);
    bool InitializeGpuDevice(HANDLE probeTextureHandle);
    bool InitializeGpuVideoProcessor(UINT width,
                                     UINT height,
                                     UINT frameRateNumerator,
                                     UINT frameRateDenominator);
    void ResetGpuVideoProcessor();
    bool QueueVideoSample(
        Microsoft::WRL::ComPtr<IMFSample> sample,
        const RecordingFrameIdentity& identity);
    bool CheckDiskSpace();
    FinalizeResult FinalizeAndStop(StopReason reason);
    bool WritePendingSample(std::int64_t duration100ns);
    void LeakComForProcessExit();
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
    std::atomic<WriterStage> stage_{WriterStage::kIdle};
    std::atomic<bool> abandoned_{false};
    std::string lastError_;
};

} // namespace openzoom

#endif // _WIN32
