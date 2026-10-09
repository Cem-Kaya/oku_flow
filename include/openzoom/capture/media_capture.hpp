#pragma once

#ifdef _WIN32

#include "openzoom/common/yuv_color.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace openzoom {

struct MediaFrame {
    // System-memory capture is normalized to tightly packed top-down rows;
    // NV12 stores Y then UV with the same positive stride. GPU readback also
    // normalizes its native pitch to this layout.
    std::vector<uint8_t> data;
    // Present only on the accelerated Media Foundation path. Holding the
    // texture alone does NOT protect the pixels: the texture belongs to the
    // source reader's sample pool, and once the IMFSample is released the
    // reader recycles the same texture for a future frame — silently
    // overwriting any queued frame that still references it. gpuSample
    // pins the sample (and therefore the texture contents) until the
    // consumer drops this MediaFrame; both are released together.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuTexture;
    Microsoft::WRL::ComPtr<IMFSample> gpuSample;
    UINT gpuSubresource{0};
    DXGI_FORMAT gpuFormat{DXGI_FORMAT_UNKNOWN};
    GUID subtype{GUID_NULL};
    YuvColorInfo yuvColor{};
    UINT width{0};
    UINT height{0};
    LONG stride{0};
    size_t dataSize{0};
    LONGLONG captureTimestamp100ns{-1};
    LONGLONG captureClock100ns{-1};
    std::uint64_t sequenceNumber{0};
    UINT frameRateNumerator{0};
    UINT frameRateDenominator{0};

    bool IsGpuResident() const { return gpuTexture != nullptr; }
};

// Ownership moves from the capture thread into the consumer. This avoids two
// full-frame vector copies before the CUDA upload path stages the frame into
// page-locked memory.
using FrameCallback = std::function<void(MediaFrame&& frame)>;
using CaptureErrorCallback = std::function<void(const std::string& message)>;

struct CameraDescriptor {
    std::wstring name;
    std::wstring symbolicLink;
    Microsoft::WRL::ComPtr<IMFActivate> activation;
};

struct VideoFormat {
    GUID subtype{GUID_NULL};
    UINT width{0};
    UINT height{0};
    UINT numerator{0};
    UINT denominator{0};
    std::wstring stableId;
};

// Plain-language classification of the most recent capture failure, derived
// from the final HRESULT of StartCapture or from a mid-stream capture error.
enum class CameraFailureKind { None, DeviceBusy, DeviceMissing, AccessDenied, Other };

enum class CaptureAccelerationMode {
    Accelerated,
    Compatibility,
};

enum class GpuFramePreparationResult {
    Ready,
    Retry,
    Unsupported,
};

class MediaCaptureSession;

// UI-owned facade. Every capture/stop worker owns a separate shared session,
// so a timed-out driver cannot access this facade or a subsequent camera.
class MediaCapture {
public:
    MediaCapture();
    ~MediaCapture();
    // Transfer facade/session ownership after the startup worker has finished.
    // Neither facade may be accessed concurrently with this operation.
    void Swap(MediaCapture& other) noexcept;
    bool Initialize();
    void Shutdown();
    std::vector<CameraDescriptor> EnumerateCameras();
    std::vector<VideoFormat> EnumerateFormats(const CameraDescriptor& descriptor);
    bool StartCapture(const CameraDescriptor& descriptor,
                      const VideoFormat* requestedFormat,
                      FrameCallback callback,
                      GUID preferredSubtype = MFVideoFormat_NV12,
                      CaptureErrorCallback errorCallback = {},
                      CaptureAccelerationMode accelerationMode = CaptureAccelerationMode::Compatibility,
                      const std::wstring& requestedStableId = {});
    // Revokes new callback entries, then allows at most 1500 ms total for
    // driver stop/quiescence and resource teardown. Callback runs on the caller
    // while backing resources remain alive: true means producer quiesced,
    // false requires abandoning imports without waiting on a wedged driver.
    // Already-entered frame/error callbacks must independently own their target.
    bool StopCapture(const std::function<void(bool quiesced)>& beforeAccelerationRelease = {});
    bool WasAbandoned() const { return abandonedForExit_; }
    bool LastStopCompleted() const { return lastStopCompleted_; }
    const std::string& LastError() const { return lastError_; }
    const std::string& FormatNotice() const { return formatNotice_; }
    const VideoFormat& NegotiatedFormat() const { return negotiatedFormat_; }
    // Native modes from the reader opened by StartCapture. Available after
    // startup without a separate camera activation; immutable until next start.
    const std::vector<VideoFormat>& NativeFormats() const { return nativeFormats_; }
    CameraFailureKind LastFailureKind() const;
    CaptureAccelerationMode AccelerationMode() const;
    bool ConsumeAccelerationValidated();
    bool ConsumeAccelerationRejected();
    bool ConsumeDeviceLost();
    const std::wstring& LastSymbolicLink() const { return lastSymbolicLink_; }
    double CurrentFrameRate() const;
    // A Ready result supplies a lease. The producer cannot overwrite its BGRA
    // texture until every consumer releases this lease after GPU copy completion.
    GpuFramePreparationResult PrepareGpuFrameForCuda(
        const MediaFrame& frame,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTexture,
        std::shared_ptr<void>& outLease);
    bool ReadbackGpuFrame(MediaFrame& frame);
private:
    std::shared_ptr<MediaCaptureSession> session_;
    bool abandonedForExit_{false};
    bool lastStopCompleted_{true};
    std::wstring lastSymbolicLink_;
    std::string lastError_;
    std::string formatNotice_;
    VideoFormat negotiatedFormat_{};
    std::vector<VideoFormat> nativeFormats_;
};
} // namespace openzoom

#endif // _WIN32
