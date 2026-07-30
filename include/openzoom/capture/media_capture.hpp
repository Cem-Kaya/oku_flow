#pragma once

#ifdef _WIN32

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openzoom {

struct MediaFrame {
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

class MediaCapture {
public:
    MediaCapture();
    ~MediaCapture();

    bool Initialize();
    void Shutdown();

    std::vector<CameraDescriptor> EnumerateCameras();
    std::vector<VideoFormat> EnumerateFormats(const CameraDescriptor& descriptor);

    bool StartCapture(const CameraDescriptor& descriptor,
                      const VideoFormat* requestedFormat,
                      FrameCallback callback,
                      GUID preferredSubtype = MFVideoFormat_NV12,
                      CaptureErrorCallback errorCallback = {},
                      CaptureAccelerationMode accelerationMode =
                          CaptureAccelerationMode::Compatibility);
    // Stops the capture thread, invokes beforeAccelerationRelease while the
    // D3D11 device and reusable interop texture are still alive, then releases
    // Media Foundation and acceleration resources. CUDA consumers use the
    // callback to release any imported external-memory view before D3D11
    // teardown.
    void StopCapture(
        const std::function<void()>& beforeAccelerationRelease = {});

    const std::string& LastError() const { return lastError_; }
    const std::string& FormatNotice() const { return formatNotice_; }
    const VideoFormat& NegotiatedFormat() const { return negotiatedFormat_; }
    CameraFailureKind LastFailureKind() const { return lastFailureKind_.load(); }
    CaptureAccelerationMode AccelerationMode() const { return accelerationMode_; }
    bool ConsumeAccelerationValidated();
    bool ConsumeAccelerationRejected();
    // Converts the retained MF texture to a reusable, NT-shareable BGRA GPU
    // texture without crossing system memory. CUDA opens the same allocation
    // through D3D12 external memory. Retry requires the caller to retain and
    // resubmit this exact MediaFrame until its D3D11 completion query resolves.
    // The texture remains owned by MediaCapture until StopCapture.
    GpuFramePreparationResult PrepareGpuFrameForCuda(
        const MediaFrame& frame,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTexture);
    // Lower rung used only for startup validation, raw recording/photo
    // capture, CPU fallback, or a failed CUDA/D3D11 interop attempt.
    bool ReadbackGpuFrame(MediaFrame& frame);

    // Atomically returns whether the capture thread detected mid-stream device
    // loss, clearing the flag. Poll from the app's frame tick to drive reconnection.
    bool ConsumeDeviceLost();

    // Symbolic link of the device from the most recent StartCapture. Kept across
    // device loss and StopCapture so the app can re-enumerate and find the same
    // physical camera again when reconnecting.
    const std::wstring& LastSymbolicLink() const { return lastSymbolicLink_; }
    double CurrentFrameRate() const;

private:
    struct FrameFormat {
        GUID subtype{GUID_NULL};
        UINT width{0};
        UINT height{0};
        LONG stride{0};
        UINT frameRateNumerator{0};
        UINT frameRateDenominator{0};
    };

    bool ConfigureReader(IMFSourceReader* reader,
                         GUID preferredSubtype,
                         const VideoFormat* requestedFormat,
                         FrameFormat& outFormat);
    bool ReadCurrentFormat(IMFSourceReader* reader, FrameFormat& outFormat);
    HRESULT TryOpenDevice(const CameraDescriptor& descriptor,
                          Microsoft::WRL::ComPtr<IMFMediaSource>& outSource,
                          Microsoft::WRL::ComPtr<IMFSourceReader>& outReader,
                          const char*& failedStage,
                          CaptureAccelerationMode accelerationMode);
    bool CreateAccelerationDeviceManager();
    bool AttachDxgiFrame(IMFMediaBuffer* buffer, MediaFrame& frame);
    bool CopyGpuFrame(const FrameFormat& format, MediaFrame& frame);
    bool EnsureVideoProcessor(const MediaFrame& frame);
    void PrepareAccelerationInteropRelease();
    void ReleaseAccelerationResources();
    bool ValidateStartupFrame(const MediaFrame& frame);
    void CaptureLoop(FrameCallback callback, CaptureErrorCallback errorCallback);
    std::vector<VideoFormat> ExtractFormats(IMFSourceReader* reader);
    static std::string HrToString(HRESULT hr);

    Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource_;
    Microsoft::WRL::ComPtr<IMFSourceReader> sourceReader_;
    Microsoft::WRL::ComPtr<IMFActivate> activeActivation_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Context_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dxgiReadbackTexture_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> d3d11VideoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> d3d11VideoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator>
        d3d11VideoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> d3d11VideoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>
        d3d11VideoProcessorOutputTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11CudaTexture_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>
        d3d11CudaOutputView_;
    Microsoft::WRL::ComPtr<ID3D11Query> d3d11CudaReadyQuery_;
    bool d3d11CudaNeedsCopy_{false};
    bool d3d11CudaQueryPending_{false};
    std::uint64_t d3d11CudaPendingSequence_{0};
    bool d3d11CudaCompletionDelayed_{false};
    DXGI_FORMAT videoProcessorInputFormat_{DXGI_FORMAT_UNKNOWN};
    UINT videoProcessorWidth_{0};
    UINT videoProcessorHeight_{0};
    std::mutex d3d11Mutex_;
    UINT dxgiResetToken_{0};
    std::thread captureThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> deviceLost_{false};
    std::atomic<CameraFailureKind> lastFailureKind_{CameraFailureKind::None};
    std::atomic<bool> accelerationValidated_{false};
    std::atomic<bool> accelerationRejected_{false};
    std::atomic<UINT> frameRateNumerator_{0};
    std::atomic<UINT> frameRateDenominator_{0};
    FrameFormat currentFormat_{};
    std::wstring lastSymbolicLink_;
    std::string lastError_;
    std::string formatNotice_;
    VideoFormat negotiatedFormat_{};
    CaptureAccelerationMode accelerationMode_{CaptureAccelerationMode::Compatibility};
    UINT startupValidationFrames_{0};
    double startupMaximumSpatialRange_{0.0};
    LONGLONG startupPreviousTimestamp_{-1};
    UINT startupAdvancingTimestamps_{0};
    bool startupValidationComplete_{false};
};

} // namespace openzoom

#endif // _WIN32
