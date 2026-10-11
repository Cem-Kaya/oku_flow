#pragma once
#ifdef _WIN32
#include "okuflow/capture/media_capture.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <span>
#include <optional>

namespace okuflow {
// Public adb devices.proto fields only; unknown/non-USB/unauthorized transports are excluded.
std::vector<CameraDescriptor> ParseAdbUsbDevices(std::span<const std::uint8_t> protobuf);
enum class PhoneVideoCodec { Avc, Hevc };
// System Annex B decoder, optionally sharing the capture session's D3D11 device manager.
class PhoneVideoDecoder {
public:
    PhoneVideoDecoder(UINT width, UINT height, UINT fps, PhoneVideoCodec codec = PhoneVideoCodec::Avc,
                      IMFDXGIDeviceManager* manager = nullptr);
    ~PhoneVideoDecoder();
    // Caller owns COM/MF startup. HEVC is optional; probe activation, input type and NV12 output.
    static bool IsSupported(PhoneVideoCodec codec, IMFDXGIDeviceManager* manager = nullptr);
    void Decode(std::span<const std::uint8_t> data, LONGLONG timestamp100ns, const FrameCallback& callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Optional adb source. Blocking network/decoder work belongs to its private worker.
class PhoneCameraTransport {
public:
    ~PhoneCameraTransport();
    static std::vector<CameraDescriptor> Enumerate();
    bool Start(const CameraDescriptor& descriptor, IMFDXGIDeviceManager* manager,
               FrameCallback callback, CaptureErrorCallback errorCallback);
    void Cancel();
    void Stop();
    void SetZoom(float ratio);
    void SetCamera(const std::string& id);
    void SetTorch(bool enabled);
    bool TorchAvailable() const noexcept { return torchAvailable_.load(); }
    bool TorchEnabled() const noexcept { return torchEnabled_.load(); }
    VideoFormat Format() const;
    std::string Error() const;
private:
    void Run(CameraDescriptor descriptor, Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager,
             FrameCallback callback, CaptureErrorCallback errorCallback);
    std::atomic<bool> canceled_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    bool opened_{false};
    bool failed_{false};
    VideoFormat format_{};
    std::string error_;
    float pendingZoom_{-1.0f};
    std::string pendingCamera_;
    std::optional<bool> pendingTorch_;
    std::atomic<bool> torchAvailable_{false};
    std::atomic<bool> torchEnabled_{false};
};
} // namespace okuflow
#endif
