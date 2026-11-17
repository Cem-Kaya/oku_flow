#pragma once
#ifdef _WIN32
#include "okuflow/capture/media_capture.hpp"
#include "okuflow/capture/phone_startup_budget.hpp"
#include "okuflow/capture/phone_link_doctor.hpp"
#include <atomic>
#include <chrono>
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
// Every listed transport with its raw state, for the link doctor. The camera
// list above stays restricted to USB + DEVICE; this keeps unauthorized,
// authorizing, connecting and offline entries visible.
struct AdbDeviceEntry {
    std::string serial;
    std::string model;
    PhoneAdbState state{PhoneAdbState::Other};
    bool usb{false};
    std::uint64_t transportId{0};
};
std::vector<AdbDeviceEntry> ParseAdbDeviceEntries(std::span<const std::uint8_t> protobuf);
// host:devices-l text fallback for older servers.
std::vector<AdbDeviceEntry> ParseAdbDevicesLongText(std::string_view text);
// Pure parser for the bounded read-only phone shell query (wakefulness,
// keyguard, package presence, process presence). Unknown stays Unknown.
PhoneShellFacts ParsePhoneShellFacts(std::string_view text);
// SetupAPI: is an Android ADB USB interface (standard interface GUID)
// present in Windows right now? Absent also for a phone that does not
// publish that GUID; Unknown only when the SetupAPI query itself failed.
PhoneAdbInterface QueryAndroidAdbInterface();
// Short stable tag for logs and the always-on trace; never the serial itself.
std::string PhoneSerialTag(std::string_view serial);
// Always-on rolling phone link trace (two files of at most 256 KiB). An empty
// path disables the file; lines still reach the debug log.
void SetPhoneLinkTraceFile(const std::wstring& path);
// Never writes the file on the calling thread (events arrive on the UI
// thread): lines are queued for a private writer thread.
void TracePhoneLink(std::string_view line);
// Waits until every queued trace line is written (tests, shutdown).
bool FlushPhoneLinkTrace(std::chrono::milliseconds timeout);
// Events reset every recovery budget (doctor, launch, reconnect, server
// start/restart cooldowns). Resume and removal also void the server identity
// so pre-sleep transport ids and health are not trusted. Thread-safe.
void NotePhoneLinkEvent(PhoneLinkEvent event);
// Caller facts for one discovery cycle with the doctor engaged.
struct PhoneLinkRequest {
    bool phoneWanted{false};
    bool captureAbandoned{false};
    bool sessionStreaming{false};
    // Automatic first start (no saved phone, no explicit webcam): the phone
    // counts as wanted while Windows shows an ADB interface.
    bool phoneWantedIfAttached{false};
    // The phone of the selected or live session (also before its first
    // frame); preferred over the recovery serial as the ladder's target so
    // shell commands never reach a phone the user did not choose.
    std::string targetSerial;
};
struct PhoneLinkReport {
    PhoneLinkObservation observation;
    PhoneLinkDecision decision;
    // The phone the ladder is about (preferred serial, or the only USB entry).
    std::string targetSerial;
    // Short trace text of the executed action's outcome, empty when none ran.
    std::string actionResult;
};
// Missing/invalid capabilities expose no hardware zoom, never a guessed range.
std::pair<float, float> ResolvePhoneZoomRange(const QJsonObject& capabilities, const std::string& selectedCamera);

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
    // An overridden endpoint is never stopped. A default-port adb.exe that is
    // hung, or blind to a phone Windows shows and serving no other device, is
    // stopped through its exact listener handle (owner decision 2026-11-17).
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
    // Windows ADB interface presence; unset reports Unknown (no blind rules).
    std::function<PhoneAdbInterface()> adbInterface;
    // Another OkuFlow instance; unset reports none (no real process scan)
    // unless siblingImage names a fixture image for the real scan.
    std::function<bool()> otherInstanceRunning;
    // The production sibling scan, classification and leftover stop run
    // against this private fixture image and registration tag (never this
    // installation); the adb-child fallback uses siblingAdbImage.
    std::wstring siblingImage;
    std::wstring siblingTag;
    std::wstring siblingAdbImage;
    std::int64_t siblingExitingGraceMs{5000};
    std::int64_t siblingWindowlessMs{20000};
    // The leftover's termination is refused as Windows would for an
    // elevated process, to reach the adb-child step.
    bool refuseSiblingTermination{false};
    // Reports a visible window of the leftover at the last check before
    // termination (its stop then ends IdentityChanged).
    std::function<bool(DWORD processId)> siblingVisibleWindowForTest;
    // Reports the default-port listener's process as one OpenProcess
    // refuses (another user, elevated). The adb.exe image rule itself has
    // no hook: fixtures run a copy named adb.exe.
    std::function<bool()> defaultListenerUnopenable;
    // Runs when a host:kill or termination of the default listener starts,
    // before its fresh listing (lets a fixture change between decision and act).
    std::function<void()> beforeDefaultStopForTest;
    // Doctor budgets for fast fixtures; production uses PhoneLinkPolicy{}.
    std::optional<PhoneLinkPolicy> linkPolicy;
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
        PhoneServerIntent serverIntent = PhoneServerIntent::BorrowOnly,
        // Opt-in: collect a full link observation, let the doctor choose
        // the next recovery rung and run it (bounded, cancellable).
        const PhoneLinkRequest* link = nullptr,
        PhoneLinkReport* report = nullptr);
    // Starts wake the phone and launch the companion when its listener is
    // absent, at most once per start attempt, from the link doctor's one
    // per-event launch budget that the discovery ladder shares (at once
    // after an event, then 3 s, 6 s, every 20 s; once per event while the
    // app process runs with its listener closed). A deliberate user
    // selection renews that budget.
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
    std::pair<float, float> ZoomRange() const;
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
    float desiredZoom_{1.0f};
    std::pair<float, float> zoomRange_{1.0f, 1.0f};
    std::string pendingCamera_;
    std::optional<PendingTorchCommand> pendingTorch_;
    std::shared_ptr<std::atomic<bool>> torchCancellation_;
    std::atomic<std::uint64_t> torchGeneration_{0};
    std::atomic<bool> torchAvailable_{false};
    std::atomic<bool> torchEnabled_{false};
};
} // namespace okuflow
#endif
