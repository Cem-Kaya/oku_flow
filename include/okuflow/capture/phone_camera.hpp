#pragma once
#ifdef _WIN32
#include "okuflow/capture/media_capture.hpp"
#include "okuflow/capture/phone_startup_budget.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <span>
#include <optional>
#include <cstdint>
#include <functional>
#include <string_view>

class QTcpSocket;
class QJsonObject;

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
    // True only for the private loopback server this OkuFlow process
    // launched itself (exact owned child in a kill-on-job-close job). An
    // overridden endpoint or a pre-existing default-port listener is borrowed.
    // Only separately proven reviewed legacy OkuFlow bundles qualify for
    // deliberate reclamation; unknown/shared SDK servers remain untouched.
    bool ownedByThisProcess{false};
};
AdbServerEndpoint ParseAdbServerEndpoint(std::string_view serverSocket, std::string_view serverPort,
                                         std::string_view serverAddress);

// Test seam only; production never sets it. While set, adb traffic uses
// 127.0.0.1:port (nonzero) and launchServer replaces the bundled adb bootstrap
// (empty disables bootstrap). The explicit owned-server fields opt into the
// private production-wiring fixture only. Setting or clearing it resets the server
// start/recovery throttles and the per-transport app-launch budget, and stops
// any owned private server so fixtures can never interact with it.
struct PhoneAdbTestHooks {
    std::uint16_t port{0};
    std::function<bool(std::uint16_t port)> launchServer;
    // Private production-wiring fixture: port stands in for the default
    // endpoint; only this explicit executable may be started on a fresh port.
    std::wstring ownedServerExecutable;
    std::vector<std::wstring> ownedServerArguments;
    std::uint32_t borrowedHangConfirmationMs{5000};
};
void SetPhoneAdbTestHooks(std::optional<PhoneAdbTestHooks> hooks);
// Stops only the private adb server this process launched and owns (exact
// job-owned child). A borrowed pre-existing/overridden server is untouched.
// Called on normal app teardown after capture stops; state is per-process
// and never persisted. Safe to call repeatedly.
void ShutdownOwnedAdbServer();
// Private owned-server port while its exact child is running, else nullopt.
std::optional<std::uint16_t> OwnedAdbServerPort();
enum class PhoneVideoCodec { Avc, Hevc };
// BorrowOnly has no lifecycle effects. PhoneUse enables guarded recovery and
// bootstrap. ReleaseForWebcam retires an idle owned child without relaunching.
enum class PhoneServerIntent { BorrowOnly, PhoneUse, ReleaseForWebcam };
// Discovery failures remain distinct from a confirmed empty USB listing.
enum class PhoneDiscoveryStatus {
    Unknown, Listed, NoListener, ServerUnresponsive, EndpointUnsupported, OwnedServerRecovering
};
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
    // unknown, not absence. Probe replies set readiness/busy/problem. An
    // optional recoverySerial permits guarded USB recovery only for the user's
    // preferred idle phone; other discovered devices remain non-claiming.
    // Only a caller with phone-use intent may request owned bootstrap; ordinary
    // camera enumeration borrows an existing server without starting one.
    static std::vector<CameraDescriptor> Enumerate(
        const std::string& connectedSerial = {},
        const std::atomic<bool>* canceled = nullptr,
        bool* listingSucceeded = nullptr,
        const std::string& recoverySerial = {},
        PhoneDiscoveryStatus* status = nullptr,
        PhoneServerIntent serverIntent = PhoneServerIntent::BorrowOnly);
    // Automatic starts launch the phone app at most once per serial and USB
    // transport. A deliberate user selection may restore that one launch.
    static void AllowAppLaunch(const std::string& serial);
    // Worker-only recovery: reset only this currently authorized USB transport
    // when adb host services respond but two full-token device shell health
    // probes time out and a fresh non-claiming companion probe also times out.
    // At most one request per phone/server per minute within this process
    // (the throttles are not persisted across restarts), including across new
    // transport IDs. Never restart/kill the server or reset busy/paused
    // camera sessions. Returns true only when adb acknowledges a targeted
    // reconnect request and the stale transport then disappears or
    // re-registers under a new ID; discovery observes the phone afterward.
    static bool RecoverStalledUsbTransport(const CameraDescriptor& descriptor,
                                          const std::atomic<bool>* canceled = nullptr);
    bool Start(const CameraDescriptor& descriptor, IMFDXGIDeviceManager* manager,
               FrameCallback callback, CaptureErrorCallback errorCallback,
               const std::atomic<bool>* startupCanceled = nullptr);
    void Cancel();
    void Stop();
    void SetZoom(float ratio);
    void SetCamera(const std::string& id);
    void SetTorch(bool enabled);
    // A positive steady-clock deadline and nonnull token bound actor requests.
    // Newer native/actor requests retire the previous token, including a dequeued
    // request that has not yet reached its guarded control-socket write.
    bool SetTorch(bool enabled, std::shared_ptr<std::atomic<bool>> canceled,
                  std::int64_t deadlineSteadyMs);
    bool TorchAvailable() const noexcept { return torchAvailable_.load(); }
    bool TorchEnabled() const noexcept { return torchEnabled_.load(); }
    VideoFormat Format() const;
    std::string Error() const;
    // Last phone ERROR/probe code (busy, blocked, noMode, failed, hot, sessionBusy) or empty.
    std::string ProblemCode() const;
private:
    friend struct PhoneTorchTestAccess;
    struct PendingTorchCommand {
        bool enabled{};
        std::shared_ptr<std::atomic<bool>> canceled;
        std::int64_t deadlineSteadyMs{};
        std::uint64_t generation{};
    };
    bool QueueTorch(bool enabled, std::shared_ptr<std::atomic<bool>> canceled,
                    std::int64_t deadlineSteadyMs);
    bool SendPendingTorch(QTcpSocket& socket);
    bool SendTorchCommand(QTcpSocket& socket, const PendingTorchCommand& command);
    void ApplyTorchAcknowledgment(const QJsonObject& status);
    void RetirePendingTorch();
    void Run(CameraDescriptor descriptor, Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager,
             FrameCallback callback, CaptureErrorCallback errorCallback, PhoneStartupBudget startupBudget);
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
    std::optional<PendingTorchCommand> pendingTorch_;
    std::shared_ptr<std::atomic<bool>> torchCancellation_;
    std::atomic<std::uint64_t> torchGeneration_{0};
    std::atomic<bool> torchAvailable_{false};
    std::atomic<bool> torchEnabled_{false};
};
} // namespace okuflow
#endif
