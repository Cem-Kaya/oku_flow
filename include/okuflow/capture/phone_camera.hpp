#pragma once
#ifdef _WIN32
#include "okuflow/capture/media_capture.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <span>
#include <optional>
#include <cstdint>
#include <functional>
#include <string_view>

namespace okuflow {
// Public adb devices.proto fields only; unknown/non-USB/unauthorized transports are excluded.
std::vector<CameraDescriptor> ParseAdbUsbDevices(std::span<const std::uint8_t> protobuf);

// adb client endpoint from ADB_SERVER_SOCKET, ANDROID_ADB_SERVER_PORT and
// ANDROID_ADB_SERVER_ADDRESS. Only loopback TCP is supported; anything else
// (remote host, Unix socket, invalid port) disables USB phones without
// connecting to or starting a server.
struct AdbServerEndpoint {
    bool supported{true};
    bool overridden{false};
    std::uint16_t port{5037};
};
AdbServerEndpoint ParseAdbServerEndpoint(std::string_view serverSocket, std::string_view serverPort,
                                         std::string_view serverAddress);

// Test seam only; production never sets it. While set, adb traffic uses
// 127.0.0.1:port (nonzero) and launchServer replaces the bundled adb bootstrap
// (empty disables bootstrap). Setting or clearing it also resets the server
// start throttle and the per-transport app-launch budget.
struct PhoneAdbTestHooks {
    std::uint16_t port{0};
    std::function<bool(std::uint16_t port)> launchServer;
};
void SetPhoneAdbTestHooks(std::optional<PhoneAdbTestHooks> hooks);
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
    // connectedSerial is never probed (it is live or about to be). listingSucceeded
    // becomes true only after adb returned a parsed USB listing; false means
    // unknown, not absence. Probe replies set readiness/busy/problem.
    static std::vector<CameraDescriptor> Enumerate(
        const std::string& connectedSerial = {},
        const std::atomic<bool>* canceled = nullptr,
        bool* listingSucceeded = nullptr);
    // Automatic starts launch the phone app at most once per serial and USB
    // transport. A deliberate user selection may restore that one launch.
    static void AllowAppLaunch(const std::string& serial);
    bool Start(const CameraDescriptor& descriptor, IMFDXGIDeviceManager* manager,
               FrameCallback callback, CaptureErrorCallback errorCallback,
               const std::atomic<bool>* startupCanceled = nullptr);
    void Cancel();
    void Stop();
    void SetZoom(float ratio);
    void SetCamera(const std::string& id);
    void SetTorch(bool enabled);
    bool TorchAvailable() const noexcept { return torchAvailable_.load(); }
    bool TorchEnabled() const noexcept { return torchEnabled_.load(); }
    VideoFormat Format() const;
    std::string Error() const;
    // Last phone ERROR/probe code (busy, blocked, noMode, failed, hot, sessionBusy) or empty.
    std::string ProblemCode() const;
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
    std::string problemCode_;
    float pendingZoom_{-1.0f};
    std::string pendingCamera_;
    std::optional<bool> pendingTorch_;
    std::atomic<bool> torchAvailable_{false};
    std::atomic<bool> torchEnabled_{false};
};
} // namespace okuflow
#endif
