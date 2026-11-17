#ifdef _WIN32
#include "okuflow/capture/phone_camera.hpp"
#include "okuflow/capture/adb_owned_server.hpp"
#include "okuflow/capture/adb_legacy_recovery.hpp"
#include "okuflow/capture/capture_buffer.hpp"
#include "okuflow/capture/phone_frame_progress.hpp"
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QtEndian>
#include <QCryptographicHash>
#include <QDateTime>
#include <codecapi.h>
#include <setupapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <iphlpapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace okuflow {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr int kControlLimit = 64 * 1024;
constexpr int kVideoLimit = 16 * 1024 * 1024;
struct Packet { quint16 type{}, flags{}; QByteArray data; };
struct PhoneProtocolError : std::runtime_error {
    PhoneProtocolError(std::string code, const std::string& message)
        : std::runtime_error(message), code(std::move(code)) {}
    std::string code;
};
struct UsbResponseTimeout : std::runtime_error { using std::runtime_error::runtime_error; };
// Connection refused: nothing accepts on the port (see ProbeAdbHostVersion
// for a frozen listener whose accept queue is full, which Windows refuses).
struct AdbServerUnavailable : std::runtime_error { using std::runtime_error::runtime_error; };
// The connect itself did not complete in time: a listener exists but does
// not accept. Never "no listener"; never device-stall evidence either.
struct AdbConnectTimedOut : std::runtime_error { using std::runtime_error::runtime_error; };
// The server answered a request with FAIL: it is alive (an answer, never "no
// answer"), whatever it refused.
struct AdbFailReply : std::runtime_error { using std::runtime_error::runtime_error; };
struct AdbListenerIdentity {
    std::uint16_t port{};
    DWORD processId{};
    std::uint64_t created{};
    bool operator==(const AdbListenerIdentity&) const = default;
};
struct AdbRuntime {
    std::mutex mutex;
    std::timed_mutex lifecycleMutex;
    std::atomic<bool> shutdownRequested{false};
    std::optional<AdbListenerIdentity> lastIdentity;
    std::uint64_t serverEpoch{};
    std::optional<AdbListenerIdentity> borrowedFailureIdentity;
    Clock::time_point borrowedFailureSince{};
    unsigned borrowedFailures{};
    bool shadowServer{false};
    unsigned shadowHandbackProbes{};
    std::optional<PhoneAdbTestHooks> hooks;
    Clock::time_point nextServerStart{};
    std::map<std::pair<std::uint16_t, std::string>, Clock::time_point> nextUsbRecovery;
    std::map<std::pair<std::uint16_t, std::string>, unsigned> usbRecoveryAttempts;
    // Link doctor and its event clock (fast window for hang confirmation).
    PhoneLinkDoctor doctor;
    Clock::time_point lastEvent{};
    // Rung 4: a private server beside a responsive but blind default.
    bool shadowRequested{false};
    bool shadowForBlindness{false};
    bool borrowedConfirmed{false};
    std::optional<AdbListenerIdentity> defaultIdentity;
    // The default listener whose stop was refused (cleared when it is gone
    // or a different process listens).
    std::optional<AdbListenerIdentity> refusedListener;
    // Consecutive LaunchFailed results of OkuFlow's own adb (also a private
    // server that could not start beside the default). Reset only when
    // OkuFlow's own server answers, never by another program's default.
    int launchFailures{0};
    // The default-port listener answers host:version while its device
    // listing fails (times out, closes, invalid, FAIL): an adb stuck in a
    // USB transport. Failed listings of the same listener spanning the
    // confirmation time confirm a hang that version probes never would; a
    // successful listing, an event, another listener or its stop clear it.
    std::optional<AdbListenerIdentity> listingFailureIdentity;
    Clock::time_point listingFailureSince{};
    unsigned listingFailures{};
    bool listingConfirmed{false};
    // The last EnsureAdbServer result, for the trace (one line per change).
    std::optional<OwnedAdbServer::EnsureResult> lastEnsureResult;
    // The default listener beside OkuFlow's server, and the one host:kill
    // was last sent to: a different one is a new takeover, asked first.
    std::optional<AdbListenerIdentity> besideIdentity;
    std::optional<AdbListenerIdentity> askedIdentity;
    // A phone the default-port listener lists in state device while adbd
    // timed out for it there (cleared by events, when adbd answers there,
    // and when that listener is gone or replaced). Such a listing does not
    // serve the phone: no hand-back, and no reason to keep a takeover from
    // going on.
    struct DefaultStall { std::string serial; AdbListenerIdentity listener; };
    std::optional<DefaultStall> defaultStall;
    // A default whose version probes hang but whose listing answered and
    // showed another device in state device: it serves someone (cleared by
    // events, by a listing without one, or when that listener is gone).
    std::optional<AdbListenerIdentity> hungServesOther;
    // When that listener is listed again while the verdict stands.
    Clock::time_point hungServesOtherRelistAt{};
    // Sibling OkuFlow instances (PID, creation) being proven leftovers, and
    // whether a stop was tried (once per process) and refused.
    struct SiblingState { OkuFlowSiblingTrack track; bool stopTried{false}; bool stopRefused{false}; bool named{false}; };
    std::map<std::pair<DWORD, std::uint64_t>, SiblingState> siblings;
    // Suppresses repeated shell health probes after a definitive healthy or
    // other-host/phone-state answer. Separate from the reconnect-send slot so
    // a healthy answer does not consume the reconnect budget.
    std::map<std::pair<std::uint16_t, std::string>, Clock::time_point> nextUsbHealthProbe;
    std::map<std::string, unsigned> occupied;
    // When a phone session last delivered a decoded frame (NowMs; -1:
    // never). A session that delivers is live; one that is registered but
    // silent is starting or reconnecting (see PhoneSessionBlocksStop).
    std::atomic<std::int64_t> lastSessionFrameMs{-1};
    // The private server launched by this process, if owned mode engaged.
    // Shared so an in-flight EnsureRunning keeps it alive while teardown or
    // a test-hook reset stops it; never persisted across restarts.
    std::shared_ptr<OwnedAdbServer> ownedServer;
};
AdbRuntime& Runtime() { static AdbRuntime runtime; return runtime; }
AdbServerEndpoint EnvEndpoint() {
    return ParseAdbServerEndpoint(qEnvironmentVariable("ADB_SERVER_SOCKET").toStdString(),
        qEnvironmentVariable("ANDROID_ADB_SERVER_PORT").toStdString(),
        qEnvironmentVariable("ANDROID_ADB_SERVER_ADDRESS").toStdString());
}
AdbServerEndpoint Endpoint() {
    std::shared_ptr<OwnedAdbServer> owned;
    std::optional<PhoneAdbTestHooks> hooks;
    {
        auto& runtime = Runtime();
        std::lock_guard lock(runtime.mutex);
        hooks = runtime.hooks;
        owned = runtime.ownedServer;
    }
    const auto env = hooks ? AdbServerEndpoint{hooks->port != 0,
        hooks->ownedServerExecutable.empty(), hooks->port, false} : EnvEndpoint();
    if (!env.supported || env.overridden) return env; // Borrowed/refused; never owned.
    if (owned) {
        // A dead/failed owned child must not silently redirect an in-flight
        // session to somebody else's default server during the retry cooldown.
        // Port zero is unavailable; EnsureAdbServer still gets to retry it.
        return {true, false, owned->Port().value_or(0), true};
    }
    return env;
}
std::optional<PhoneAdbTestHooks> Hooks()
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    return runtime.hooks;
}
// The default-port listener (or the fixture standing in for it).
std::uint16_t DefaultPort()
{
    const auto hooks = Hooks();
    return hooks ? hooks->port : EnvEndpoint().port;
}
// An endpoint the user set through the adb environment variables (or a
// borrowed fixture) stays theirs: never shadowed, asked to exit or stopped.
// The real environment is always consulted, also while test hooks are set.
bool DefaultIsUserEndpoint()
{
    if (EnvEndpoint().overridden) return true;
    const auto hooks = Hooks();
    return hooks && hooks->ownedServerExecutable.empty();
}
PhoneAdbInterface RuntimeAdbInterface()
{
    const auto hooks = Hooks();
    if (hooks) return hooks->adbInterface ? hooks->adbInterface() : PhoneAdbInterface::Unknown;
    return QueryAndroidAdbInterface();
}
std::int64_t NowMs(Clock::time_point at = Clock::now())
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}
void TraceLine(std::string_view line);
// Other instances of this installation (same image, user, session). A
// visible window is the user's to close (sentence, never terminated). A
// registered instance proven to be a leftover (exit signalled for 5 s after
// its critical teardown, or no window on two observations 20 s apart and no
// answer to a window message) is terminated through a held handle once, and
// only when it plausibly holds the phone (`mayStop`: the phone is not
// listed, the Windows ADB interface is present, the blind grace passed); if
// Windows refuses, its proven adb child is stopped, else the user is told.
// An unregistered windowless one (an older version, a test run) is named in
// the trace once and never terminated; it does not stop the blind ladder.
void RuntimeSiblings(bool* windowed, bool* leftover, bool mayStop, const std::atomic<bool>* canceled,
                     const std::function<bool()>& stillWanted = {})
{
    *windowed = *leftover = false;
    const auto hooks = Hooks();
    if (hooks && hooks->otherInstanceRunning) { *windowed = hooks->otherInstanceRunning(); return; }
    // Fixtures scan only a private fixture image and tag, never this
    // installation; without one there is no scan at all.
    if (hooks && hooks->siblingImage.empty()) return;
    OkuFlowSiblingPolicy policy;
    if (hooks) { policy.exitingGraceMs = hooks->siblingExitingGraceMs; policy.windowlessConfirmMs = hooks->siblingWindowlessMs; }
    const auto now = NowMs();
    auto& runtime = Runtime();
    std::map<std::pair<DWORD, std::uint64_t>, AdbRuntime::SiblingState> seen;
    for (const auto& sibling : ObserveOkuFlowSiblings(hooks ? hooks->siblingImage : std::wstring{},
                                                      hooks ? hooks->siblingTag : std::wstring{}, canceled)) {
        const auto key = std::make_pair(sibling.processId, sibling.creation);
        AdbRuntime::SiblingState state;
        { std::lock_guard lock(runtime.mutex); if (const auto it = runtime.siblings.find(key); it != runtime.siblings.end()) state = it->second; }
        switch (ClassifyOkuFlowSibling(sibling, state.track, now, policy)) {
        case OkuFlowSiblingVerdict::Windowed: *windowed = true; break;
        case OkuFlowSiblingVerdict::UnregisteredWindowless:
            if (!state.named) {
                state.named = true;
                TraceLine("an unregistered OkuFlow instance without a window runs (older version or test run); "
                          "never stopped, the ladder goes on");
            }
            break;
        case OkuFlowSiblingVerdict::Pending: break;
        case OkuFlowSiblingVerdict::Leftover:
            // The request's authority is checked right before the stop
            // (the user may have chosen another source meanwhile).
            if (!state.stopTried && mayStop && (!stillWanted || stillWanted())) {
                state.stopTried = true;
                LeftoverInstanceStopRequest request;
                request.processId = sibling.processId;
                request.creation = sibling.creation;
                request.canceled = canceled;
                if (hooks) {
                    request.expectedImage = hooks->siblingImage;
                    request.bundledAdbImage = hooks->siblingAdbImage;
                    request.refuseTerminationForTest = hooks->refuseSiblingTermination;
                    if (hooks->siblingVisibleWindowForTest) request.visibleWindowForTest = hooks->siblingVisibleWindowForTest;
                }
                const auto status = StopLeftoverOkuFlowInstance(request);
                state.stopRefused = status == LeftoverInstanceStopStatus::Refused;
                // The instance changed between proof and stop (a window came
                // up, the image check failed): tried again on a later
                // observation that still proves it a leftover, not never.
                if (status == LeftoverInstanceStopStatus::IdentityChanged) state.stopTried = false;
                TraceLine(status == LeftoverInstanceStopStatus::Stopped ? "leftover OkuFlow instance stopped"
                    : status == LeftoverInstanceStopStatus::AdbChildStopped ? "leftover OkuFlow instance refused; its adb child stopped"
                    : status == LeftoverInstanceStopStatus::Refused ? "leftover OkuFlow instance could not be stopped"
                    : "leftover OkuFlow instance changed; nothing stopped");
            }
            if (state.stopRefused) *leftover = true;
            break;
        }
        seen[key] = state;
    }
    std::lock_guard lock(runtime.mutex);
    runtime.siblings = std::move(seen);
}
// The listener OkuFlow reaches on 127.0.0.1:port: the IPv4 loopback row,
// else an any-address row of that port (0.0.0.0 or a dual-stack ::, as an
// adb started with -a binds; AdbListenerRows, shared with the stop). A
// listener whose process cannot be opened (another user, elevated) is
// Unopenable, never "gone": it still holds the port and cannot be stopped
// by OkuFlow.
enum class ListenerState { Unknown, None, Found, Unopenable };
struct ListenerLookup {
    ListenerState state{ListenerState::Unknown};
    AdbListenerIdentity identity{}; // processId only when Unopenable.
};
// `refusedOnIpv4`: the caller's IPv4 connect to 127.0.0.1 was refused. Only
// an IPv4 row (loopback or 0.0.0.0) can stand for a listener that refused
// it (a frozen one whose accept queue is full); an IPv6-only row (::) is
// another program's socket that never saw the connect, so with no IPv4 row
// nothing listens there for OkuFlow. Without the flag a :: row identifies a
// listener that answered on 127.0.0.1 (a dual-stack socket).
ListenerLookup LookupListener(std::uint16_t port, bool refusedOnIpv4 = false)
{
    ListenerLookup lookup;
    if (!port) { lookup.state = ListenerState::None; return lookup; }
    std::vector<TcpListenerRow> rows;
    if (!ReadTcpListenerRows(&rows)) return lookup;
    const auto hooks = Hooks();
    lookup.state = ListenerState::None;
    for (const auto& row : AdbListenerRows(rows, port)) {
        if (refusedOnIpv4 && row.ipv6) continue;
        const bool forcedUnopenable = hooks && hooks->defaultListenerUnopenable && port == hooks->port &&
                                      hooks->defaultListenerUnopenable();
        HANDLE process = forcedUnopenable ? nullptr
            : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, row.processId);
        if (!process) {
            lookup.state = ListenerState::Unopenable;
            lookup.identity = {port, row.processId, 0};
            return lookup;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool valid = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                           WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        if (valid) {
            lookup.state = ListenerState::Found;
            lookup.identity = {port, row.processId,
                (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
            return lookup;
        }
    }
    return lookup;
}
std::optional<AdbListenerIdentity> ListenerIdentity(std::uint16_t port)
{
    const auto lookup = LookupListener(port);
    if (lookup.state != ListenerState::Found) return std::nullopt;
    return lookup.identity;
}
void ObserveServerIdentity()
{
    const auto identity = ListenerIdentity(Endpoint().port);
    if (!identity) return;
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    if (runtime.lastIdentity == identity) return;
    runtime.lastIdentity = identity;
    ++runtime.serverEpoch;
    runtime.nextUsbRecovery.clear();
    runtime.nextUsbHealthProbe.clear();
}
struct OccupiedPhone {
    explicit OccupiedPhone(std::string serial) : serial(std::move(serial)) {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); ++runtime.occupied[this->serial];
    }
    ~OccupiedPhone() {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        const auto entry = runtime.occupied.find(serial);
        if (entry != runtime.occupied.end() && --entry->second == 0) runtime.occupied.erase(entry);
    }
    std::string serial;
};
// Companion launch budget of the session start path: the link doctor's one
// per-event budget, shared with the discovery ladder (at once after an event,
// then 3 s, 6 s, then every 20 s; once per event while the app process runs
// with its listener closed). Replaces a separate 20 s timer per serial that
// woke a paused, locked phone for ever.
bool MayLaunch()
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    return runtime.doctor.ReserveLaunch(NowMs());
}

void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(hr) + ")");
}
void CheckCanceled(const std::atomic<bool>* canceled)
{
    if (canceled && canceled->load()) throw std::runtime_error("Phone camera canceled");
}
QByteArray Read(QTcpSocket& socket, int size, int timeout, const std::atomic<bool>* canceled = nullptr)
{
    QByteArray result;
    result.reserve(size);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    while (result.size() < size) {
        CheckCanceled(canceled);
        const auto bytes = socket.read(size - result.size());
        result.append(bytes);
        if (result.size() == size) break;
        if (socket.state() != QAbstractSocket::ConnectedState) throw std::runtime_error("Phone USB connection closed");
        if (Clock::now() >= deadline) throw UsbResponseTimeout("Phone USB response timed out");
        socket.waitForReadyRead(20);
    }
    return result;
}
bool Write(QTcpSocket& socket, const QByteArray& bytes, const std::atomic<bool>* canceled = nullptr,
           const std::function<bool()>& allowed = {})
{
    CheckCanceled(canceled);
    // Admission is checked at the actual emission boundary, after serialization.
    // Once queued in QTcpSocket, a command may be acknowledged even if its owner
    // cancels later; cancellation does not invent a compensating device state.
    if (allowed && !allowed()) return false;
    if (socket.write(bytes) != bytes.size()) throw std::runtime_error("Phone USB write failed");
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (socket.bytesToWrite()) {
        CheckCanceled(canceled);
        if (socket.state() != QAbstractSocket::ConnectedState || Clock::now() >= deadline)
            throw std::runtime_error("Phone USB write timed out");
        socket.waitForBytesWritten(20);
    }
    return true;
}
// Budgets a loaded or just-resumed laptop can meet. Waits are sliced so
// cancellation stays responsive (20 ms).
constexpr int kHostConnectMs = 500;
constexpr int kHostReplyFieldMs = 1000;
constexpr int kCompanionProbeTransportMs = 1000;
void ConnectTo(QTcpSocket& socket, std::uint16_t port, int timeout, const std::atomic<bool>* canceled = nullptr)
{
    socket.connectToHost(QHostAddress::LocalHost, port);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    while (socket.state() != QAbstractSocket::ConnectedState) {
        if (canceled && canceled->load()) { socket.abort(); CheckCanceled(canceled); }
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            socket.abort();
            throw AdbServerUnavailable("The local adb server is unavailable");
        }
        if (Clock::now() >= deadline) {
            socket.abort();
            throw AdbConnectTimedOut("The local adb server did not accept the connection in time");
        }
        socket.waitForConnected(20);
    }
}
void Connect(QTcpSocket& socket, int timeout, const std::atomic<bool>* canceled = nullptr)
{
    socket.setReadBufferSize(32 * 1024 * 1024);
    const auto endpoint = Endpoint();
    if (!endpoint.supported) throw std::runtime_error("USB phone support requires a local TCP adb server");
    if (endpoint.port == 0) throw AdbServerUnavailable("The owned adb server is unavailable");
    ConnectTo(socket, endpoint.port, timeout, canceled);
}

QString FindAdbExecutable()
{
    QStringList candidates;
    candidates << qEnvironmentVariable("OKUFLOW_ADB_PATH")
        << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("platform-tools/adb.exe"));
    for (const auto& variable : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        const QString sdk = qEnvironmentVariable(variable);
        if (!sdk.isEmpty()) candidates << QDir(sdk).filePath(QStringLiteral("platform-tools/adb.exe"));
    }
    candidates << QStandardPaths::findExecutable(QStringLiteral("adb.exe"));
    const QString localData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localData.isEmpty())
        candidates << QDir(localData).filePath(QStringLiteral("Android/Sdk/platform-tools/adb.exe"));
    for (const QString& candidate : candidates) {
        const QFileInfo file(candidate);
        if (!candidate.isEmpty() && file.isFile()) return file.absoluteFilePath();
    }
    return {};
}

// Bounded host:version probe against an explicit port (never Endpoint(),
// which the owned-server manager itself feeds). A connectable listener that
// does not answer a valid version in time is Unresponsive, distinct from no
// listener at all. So is a listener that does not accept: a connect that
// times out, or one Windows refuses because a frozen listener's accept
// queue is full while the listener table still shows it.
OwnedServerProbe ProbeAdbHostVersion(std::uint16_t port, const std::atomic<bool>* canceled)
{
    QTcpSocket socket;
    try { ConnectTo(socket, port, kHostConnectMs, canceled); }
    catch (const AdbServerUnavailable&) {
        const auto listener = LookupListener(port, true).state;
        return listener == ListenerState::Found || listener == ListenerState::Unopenable
            ? OwnedServerProbe::Unresponsive : OwnedServerProbe::NoListener;
    }
    catch (const std::exception&) { return OwnedServerProbe::Unresponsive; }
    try {
        Write(socket, QByteArrayLiteral("000chost:version"), canceled);
        if (Read(socket, 4, 1000, canceled) != "OKAY") return OwnedServerProbe::Unresponsive;
        bool valid = false;
        const int size = Read(socket, 4, 1000, canceled).toInt(&valid, 16);
        if (!valid || size != 4) return OwnedServerProbe::Unresponsive;
        Read(socket, 4, 1000, canceled).toUInt(&valid, 16);
        return valid ? OwnedServerProbe::Responsive : OwnedServerProbe::Unresponsive;
    } catch (const std::exception&) {
        return OwnedServerProbe::Unresponsive;
    }
}

std::shared_ptr<OwnedAdbServer> CreateOwnedAdbServer(
    const QString& executable, std::vector<std::wstring> fixtureArguments = {},
    std::optional<OwnedAdbServerPolicy> fixturePolicy = std::nullopt)
{
    OwnedAdbServerPolicy policy = fixturePolicy.value_or(OwnedAdbServerPolicy{});
    policy.verifyListenerOwnership = true;
    return std::make_shared<OwnedAdbServer>(
        [](std::uint16_t port, const std::atomic<bool>* canceled) {
            return ProbeAdbHostVersion(port, canceled);
        },
        [executable, fixtureArguments = std::move(fixtureArguments)](std::uint16_t port) -> std::unique_ptr<OwnedChildProcess> {
            // Direct exclusive loopback bind; no client version check and no
            // start-server/kill-server protocol commands. An empty hostname
            // in -L binds loopback; numeric hostnames are unsupported by the
            // upstream socket listener.
            std::string error;
            auto arguments = fixtureArguments;
            arguments.insert(arguments.end(), {L"-L", L"tcp:" + std::to_wstring(port), L"server", L"nodaemon"});
            qInfo() << "Starting owned Android USB support with" << executable;
            auto child = OwnedChildProcess::Launch({
                executable.toStdWString(),
                arguments, QFileInfo(executable).absolutePath().toStdWString(), {},
                {{L"ADB_EMU", L"0"}, {L"ADB_REJECT_KILL_SERVER", L"1"}}}, &error);
            if (!child)
                qWarning() << "Could not start owned Android USB support:" << QString::fromStdString(error);
            return child;
        },
        [] {
            QTcpServer picker;
            return picker.listen(QHostAddress::LocalHost, 0) ? picker.serverPort()
                                                             : std::uint16_t{0};
        },
        [] {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            return !runtime.occupied.empty();
        }, policy);
}

QByteArray AdbHostReply(const QByteArray& service, const std::atomic<bool>* canceled);
QByteArray AdbHostReplyAt(std::uint16_t port, const QByteArray& service, const std::atomic<bool>* canceled);
// One device listing of a server, shared by every caller: the protobuf form,
// and the text form when that fails (an older adb answers FAIL to it). No
// port: the endpoint in use; a port: that listener (the default beside
// OkuFlow's server). `text` reports which form answered.
std::vector<AdbDeviceEntry> ListAdbEntries(std::optional<std::uint16_t> port, const std::atomic<bool>* canceled,
                                           bool* text = nullptr)
{
    const auto reply = [&](const char* service) {
        return port ? AdbHostReplyAt(*port, service, canceled) : AdbHostReply(service, canceled);
    };
    if (text) *text = false;
    try {
        const auto data = reply("host:track-devices-proto-binary");
        return ParseAdbDeviceEntries(std::span(reinterpret_cast<const std::uint8_t*>(data.constData()),
                                               static_cast<size_t>(data.size())));
    } catch (...) {
        CheckCanceled(canceled);
    }
    const auto reply2 = reply("host:devices-l");
    if (text) *text = true;
    return ParseAdbDevicesLongText(std::string_view(reply2.constData(), static_cast<size_t>(reply2.size())));
}
bool ListsTargetUsable(const std::vector<AdbDeviceEntry>& entries, const std::string& target);
bool DefaultServesTarget(const std::vector<AdbDeviceEntry>& entries, const std::string& target);
// One probe of the default-port listener. A hang is confirmed by at least
// two failed probes of the same listener spanning the confirmation time
// (5 s), also after a resume or start: events clear the tracker, so a slow
// but healthy server that just woke up gets the full time again. Called
// both where OkuFlow borrows the default and from the doctor's cycle while
// OkuFlow's own server runs beside it.
void ClearDefaultListingHang(AdbRuntime& runtime);
bool NoteDefaultProbe(OwnedServerProbe probe, const std::optional<AdbListenerIdentity>& identity)
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    // Listing failures belong to the listener they were seen on: a different
    // listener observed now (another process, or the same port after an
    // exit and a new start) is not hung by them; it must confirm its own.
    if (runtime.listingFailures && probe != OwnedServerProbe::NoListener && runtime.listingFailureIdentity != identity)
        ClearDefaultListingHang(runtime);
    if (probe != OwnedServerProbe::Unresponsive) {
        runtime.borrowedFailures = 0;
        runtime.borrowedFailureSince = {};
        runtime.borrowedFailureIdentity.reset();
        runtime.borrowedConfirmed = false;
        if (probe == OwnedServerProbe::Responsive && identity) runtime.defaultIdentity = identity;
        // A version answer says nothing about the listing: a listing hang
        // stays confirmed for that listener. No listener: nothing hangs.
        if (probe == OwnedServerProbe::NoListener) {
            runtime.listingFailures = 0;
            runtime.listingFailureIdentity.reset();
            runtime.listingConfirmed = false;
        }
        return false;
    }
    if (!runtime.borrowedFailures || runtime.borrowedFailureIdentity != identity) {
        runtime.borrowedFailureIdentity = identity;
        runtime.borrowedFailureSince = Clock::now();
        runtime.borrowedFailures = 0;
    }
    ++runtime.borrowedFailures;
    const auto confirmation = std::chrono::milliseconds(runtime.hooks ? runtime.hooks->borrowedHangConfirmationMs : 5000);
    const bool confirmed = runtime.borrowedFailures >= 2 && Clock::now() - runtime.borrowedFailureSince >= confirmation;
    runtime.borrowedConfirmed = confirmed;
    if (identity) runtime.defaultIdentity = identity;
    return confirmed;
}
// One device listing of the default-port listener (borrowed, or beside
// OkuFlow's server): a failure of any kind (time-out, closed socket, invalid
// reply, FAIL to both listing forms) counts toward the listing hang, which is
// confirmed like a version hang: two failures of the same listener spanning
// the confirmation time. A successful listing clears it. Returns whether
// the listing hang of that listener is confirmed now.
bool NoteDefaultListing(bool failed, const std::optional<AdbListenerIdentity>& identity)
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    if (!failed) {
        runtime.listingFailures = 0;
        runtime.listingFailureSince = {};
        runtime.listingFailureIdentity.reset();
        runtime.listingConfirmed = false;
        return false;
    }
    if (!runtime.listingFailures || runtime.listingFailureIdentity != identity) {
        runtime.listingFailureIdentity = identity;
        runtime.listingFailureSince = Clock::now();
        runtime.listingFailures = 0;
    }
    ++runtime.listingFailures;
    const auto confirmation = std::chrono::milliseconds(runtime.hooks ? runtime.hooks->borrowedHangConfirmationMs : 5000);
    runtime.listingConfirmed = runtime.listingFailures >= 2 &&
        Clock::now() - runtime.listingFailureSince >= confirmation;
    return runtime.listingConfirmed;
}
// The listing hang of this listener is confirmed (a different listener is a
// new process: its own failures must confirm it).
bool DefaultListingHangConfirmed(const std::optional<AdbListenerIdentity>& identity)
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    return runtime.listingConfirmed && runtime.listingFailureIdentity == identity;
}
void ClearDefaultListingHang(AdbRuntime& runtime)
{
    runtime.listingFailures = 0;
    runtime.listingFailureSince = {};
    runtime.listingFailureIdentity.reset();
    runtime.listingConfirmed = false;
}
const char* EnsureResultName(OwnedAdbServer::EnsureResult result)
{
    using R = OwnedAdbServer::EnsureResult;
    switch (result) {
    case R::Running: return "running";
    case R::Launched: return "launched";
    case R::Restarted: return "restarted";
    case R::NotReady: return "not-ready";
    case R::LaunchFailed: return "launch-failed";
    case R::RestartDeferredLiveSession: return "restart-deferred-live-session";
    case R::Throttled: return "throttled";
    case R::Canceled: return "canceled";
    case R::Stopped: return "stopped";
    case R::BorrowedUnresponsive: return "borrowed-unresponsive";
    }
    return "unknown";
}
OwnedAdbServer::EnsureResult EnsureAdbServerOnce(const std::atomic<bool>* canceled, PhoneServerIntent intent,
                                                 const std::string& wanted, const std::function<bool()>& stillWanted,
                                                 bool session, bool* dropped);
// `wanted`: the phone the caller wants (empty: any phone). A hand-back of
// the private server needs the default to serve that phone. Every change
// of the result is one trace line (canceled calls are not a change).
// `stillWanted`: the authority of a discovery cycle or a ladder action (the
// user may choose another source while it runs); a session's own request
// has none. It is asked once the lifecycle lock is taken, after each
// blocking probe or listing, and right before every lifecycle effect: the
// legacy reclamation (and inside it, right before host:kill is written),
// a start beside the default, a launch, restart or replacement of OkuFlow's
// own child, and a retirement of it. A request that is no longer wanted
// does nothing further and returns Canceled (traced as dropped).
// `session`: a session's own request. It uses a responsive default even
// while a start beside it is requested and cannot happen (adb.exe missing):
// that start is the doctor's rung, which asks again; a session start
// through a default that serves the phone must not fail for it.
OwnedAdbServer::EnsureResult EnsureAdbServer(const std::atomic<bool>* canceled,
                                             PhoneServerIntent intent = PhoneServerIntent::PhoneUse,
                                             const std::string& wanted = {},
                                             const std::function<bool()>& stillWanted = {},
                                             bool session = false)
{
    bool dropped = false;
    const auto result = EnsureAdbServerOnce(canceled, intent, wanted, stillWanted, session, &dropped);
    if (dropped) {
        TraceLine("adb server request dropped: the source changed");
        return OwnedAdbServer::EnsureResult::Canceled;
    }
    if (result == OwnedAdbServer::EnsureResult::Canceled) return result;
    bool changed = false;
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        changed = runtime.lastEnsureResult != result;
        runtime.lastEnsureResult = result;
    }
    if (changed) TraceLine(std::string("adb server ") + EnsureResultName(result));
    return result;
}
OwnedAdbServer::EnsureResult EnsureAdbServerOnce(const std::atomic<bool>* canceled, PhoneServerIntent intent,
                                                 const std::string& wanted, const std::function<bool()>& stillWanted,
                                                 bool session, bool* dropped)
{
    using Result = OwnedAdbServer::EnsureResult;
    const bool allowOwnedBootstrap = intent == PhoneServerIntent::PhoneUse;
    // The request lost its authority: nothing further (see EnsureAdbServer).
    const auto stale = [&] {
        if (!stillWanted || stillWanted()) return false;
        *dropped = true;
        return true;
    };
    auto& runtime = Runtime();
    std::unique_lock lifecycleLock(runtime.lifecycleMutex, std::defer_lock);
    while (!lifecycleLock.try_lock_for(std::chrono::milliseconds(20))) {
        CheckCanceled(canceled);
        if (runtime.shutdownRequested.load()) return Result::Stopped;
    }
    CheckCanceled(canceled);
    std::optional<PhoneAdbTestHooks> hooks;
    std::shared_ptr<OwnedAdbServer> owned;
    // The hand-overs below are counted for the episode this call started
    // in: the mutex is released around Stop() and FindAdbExecutable(), and a
    // count noted after a newer event is dropped (as stop outcomes are).
    std::uint64_t episode = 0;
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        hooks = runtime.hooks;
        owned = runtime.ownedServer;
        episode = runtime.doctor.Episode();
        // USB is released for a webcam: a private-server start the doctor
        // requested for the phone is void (it would otherwise wait for the
        // next phone-use call and start beside whatever default runs then).
        if (intent == PhoneServerIntent::ReleaseForWebcam) runtime.shadowRequested = false;
    }
    // Revoked while the lock was awaited (another call held it).
    if (hooks && hooks->linkStageForTest) hooks->linkStageForTest("locked");
    if (stale()) return Result::Canceled;
    const auto env = hooks ? AdbServerEndpoint{hooks->port != 0,
                               hooks->ownedServerExecutable.empty(), hooks->port, false}
                           : EnvEndpoint();
    if (!env.supported) return Result::NotReady;
    if ((hooks && hooks->ownedServerExecutable.empty()) || env.overridden) {
        // Borrowed server: the test fixture or the endpoint owner manages
        // it. Probe only; never own, replace, restart or stop it.
        QTcpSocket existing;
        existing.connectToHost(QHostAddress::LocalHost, env.port);
        if (existing.waitForConnected(100)) return Result::Running;
        {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            if (Clock::now() < runtime.nextServerStart) return Result::Throttled;
            if (!hooks) return Result::NotReady; // The owner manages a custom server.
            runtime.nextServerStart = Clock::now() + std::chrono::seconds(30);
        }
        if (hooks->launchServer) hooks->launchServer(env.port);
        return Result::NotReady;
    }
    if (runtime.shutdownRequested.load()) return Result::Stopped;
    // Owned mode already engaged in this process: the manager health-checks
    // (and, when guarded conditions allow, restarts) only its exact child.
    if (owned) {
        bool occupied = false;
        { std::lock_guard lock(runtime.mutex); occupied = !runtime.occupied.empty(); }
        if (intent == PhoneServerIntent::ReleaseForWebcam && !occupied) {
            // An explicit webcam choice relinquishes idle USB ownership.
            // Active/starting phone sessions must finish first.
            if (stale()) return Result::Canceled;
            // What this manager counted is kept (see the yield below).
            const int unanswered = owned->UnansweredStartsAtRetirement();
            owned->Stop();
            { std::lock_guard lock(runtime.mutex);
              if (runtime.ownedServer == owned) runtime.ownedServer.reset();
              runtime.launchFailures += unanswered;
              runtime.nextServerStart = {}; }
            owned.reset();
        }
    }
    if (owned) {
        if (!allowOwnedBootstrap) {
            return owned->EnsureRunning(canceled, false);
        }
        bool shadow = false, occupied = false;
        { std::lock_guard lock(runtime.mutex);
          // A private server already exists, so a request to start one is
          // met or moot: it must not wait for a later call and start an
          // unrequested server beside a default nobody examined again.
          runtime.shadowRequested = false;
          shadow = runtime.shadowServer; occupied = !runtime.occupied.empty(); }
        bool blindShadow = false;
        { std::lock_guard lock(runtime.mutex); blindShadow = runtime.shadowForBlindness; }
        // A shadow started because the default is blind to a phone Windows
        // shows stays until the takeover settles or the phone is unplugged.
        if (blindShadow && RuntimeAdbInterface() == PhoneAdbInterface::Absent) blindShadow = false;
        if (shadow && !occupied && !blindShadow && owned->OwnsRunningServer()) {
            bool empty = false;
            try {
                // An empty raw listing proves there are no transports at all,
                // including unauthorized USB entries filtered from the picker.
                empty = ListAdbEntries(std::nullopt, canceled).empty();
            } catch (...) { CheckCanceled(canceled); }
            // The default answering again is not enough: it must list a
            // phone in a usable state (ready, unauthorized, authorizing).
            // Both listing forms: an older adb answers only the text one.
            bool readyDefault = false;
            if (empty) {
                try {
                    readyDefault = DefaultServesTarget(ListAdbEntries(env.port, canceled), wanted);
                } catch (...) { CheckCanceled(canceled); }
            }
            bool handback = false;
            { std::lock_guard lock(runtime.mutex);
              runtime.shadowHandbackProbes = readyDefault ? runtime.shadowHandbackProbes + 1 : 0;
              // Also at the hand-over cap: the cap prefers the side where
              // the phone is listed.
              handback = runtime.shadowHandbackProbes >= 2; }
            if (handback) {
                CheckCanceled(canceled);
                if (stale()) return Result::Canceled;
                const int unanswered = owned->UnansweredStartsAtRetirement();
                owned->Stop();
                if (hooks && hooks->afterPrivateServerRetiredForTest) hooks->afterPrivateServerRetiredForTest();
                std::lock_guard lock(runtime.mutex);
                if (runtime.ownedServer == owned) runtime.ownedServer.reset();
                runtime.launchFailures += unanswered;
                runtime.shadowServer = false; runtime.shadowHandbackProbes = 0;
                runtime.shadowForBlindness = false;
                runtime.shadowRequested = false; // A request does not outlive the server it was for.
                // Same as the ladder's hand-back: no start throttle, so a
                // default that hangs again right after is met by a private
                // server once its hang is confirmed (the hand-over cap, not
                // a 30 s throttle, bounds the swaps). Counted once retired.
                runtime.nextServerStart = {};
                runtime.doctor.NoteHandOver(episode);
                return Result::Running;
            }
        }
        // Relinquish a dead/failed child before borrowing a newly available
        // default server. Never migrate a running child's transport IDs.
        if (!owned->OwnsRunningServer()) {
            if (occupied) return Result::RestartDeferredLiveSession;
            if (ProbeAdbHostVersion(env.port, canceled) == OwnedServerProbe::Responsive) {
                if (stale()) return Result::Canceled;
                // A child that never answered (it outlived its readiness
                // deadline silent, then died: security software that lets
                // adb.exe run for a few seconds) is a start that failed. The
                // manager's count is kept with the transport's launch
                // failures before the manager is dropped, so starts beside
                // an answering default reach the "cannot start" sentence
                // instead of spending hand-overs; only OkuFlow's own server
                // answering resets it.
                const int unanswered = owned->UnansweredStartsAtRetirement();
                owned->Stop();
                std::lock_guard lock(runtime.mutex);
                if (runtime.ownedServer == owned) runtime.ownedServer.reset();
                runtime.launchFailures += unanswered;
                // No private server shadows the default any more: clear what
                // both hand-back paths clear, so an empty default listing is
                // not reported as OkuFlow's own server recovering and a
                // default that hangs again meets a private server at once.
                runtime.shadowServer = runtime.shadowForBlindness = runtime.shadowRequested = false;
                runtime.shadowHandbackProbes = 0;
                runtime.nextServerStart = {};
                return Result::Running;
            }
        }
        if (stale()) return Result::Canceled;
        const auto result = owned->EnsureRunning(canceled, true, stillWanted);
        if (result == Result::Canceled && stale()) return Result::Canceled;
        if (result == Result::Launched || result == Result::Restarted) {
            qInfo() << "OkuFlow owned adb is ready on private port" << owned->Port().value_or(0);
        }
        return result;
    }
    // First engagement borrows a responsive default listener. Repeated host
    // hangs first try the reviewed legacy OkuFlow bundle reclamation; any
    // other hung default gets a private child at once and is then stopped by
    // the link doctor through its exact listener handle (owner decision of
    // 2026-11-17). Overridden endpoints never reach this branch.
    const auto defaultProbe = ProbeAdbHostVersion(env.port, canceled);
    CheckCanceled(canceled);
    if (hooks && hooks->linkStageForTest) hooks->linkStageForTest("default-probed");
    if (stale()) return Result::Canceled;
    // A start beside the default: its shadow flags are set, and the
    // transport's own start beside a hung default is charged to the
    // hand-over cap, only once the start actually proceeds (not when the
    // executable is missing or the start is throttled).
    enum class Beside { No, Blind, Hung };
    Beside beside = Beside::No;
    bool chargeHandOver = false;
    // A default that answers its version probes while its device listing
    // stayed silent for the confirmation time is hung all the same: the
    // hung-default rules apply (a private server beside it, then the
    // doctor's pre-stop checks).
    bool listingHung = false;
    if (defaultProbe == OwnedServerProbe::Responsive) {
        const auto identity = ListenerIdentity(env.port);
        NoteDefaultProbe(defaultProbe, identity);
        listingHung = DefaultListingHangConfirmed(identity);
    }
    if (defaultProbe == OwnedServerProbe::Responsive && !listingHung) {
        std::lock_guard lock(runtime.mutex);
        // Rung 4a: a responsive default blind to the phone gets a private
        // server beside it; whichever lists the phone is used. A session's
        // own request uses the responsive default whatever start is
        // requested (see EnsureAdbServer).
        if (!runtime.shadowRequested || !allowOwnedBootstrap || session) return Result::Running;
        runtime.nextServerStart = {};
        beside = Beside::Blind;
    } else if (defaultProbe == OwnedServerProbe::Unresponsive || listingHung) {
        bool confirmed = listingHung || NoteDefaultProbe(defaultProbe, ListenerIdentity(env.port));
        {
            std::lock_guard lock(runtime.mutex);
            if (!runtime.occupied.empty()) confirmed = false;
        }
        // A listing hang without the right to start a private server (no
        // phone use, or a live session on it) keeps borrowing: its listing
        // is tried again, and one that succeeds ends the hang.
        if (!allowOwnedBootstrap || !confirmed) return listingHung ? Result::Running : Result::BorrowedUnresponsive;
        bool recoveredLegacy = false;
        // The reclamation runs in production only; a fixture reaches this
        // branch through its seam, which stands in for the reclamation (it
        // never runs against the machine's real default port).
        const bool reclaim = hooks ? static_cast<bool>(hooks->legacyReclaimForTest)
                                   : qEnvironmentVariableIsEmpty("OKUFLOW_ADB_PATH");
        bool reclaimAllowed = reclaim;
        if (reclaim && listingHung) {
            // Entered on a listing hang while the version answers: the hang
            // was confirmed by earlier listings, which the legacy
            // reclamation never looks at again. One fresh listing right
            // before it: an answer ends the hang (the default recovered: it
            // is borrowed and listed again) and an adb FAIL reply keeps it
            // by the existing rules; either way nothing is reclaimed in this
            // call. Only a listing that stays silent lets the reclamation go
            // on. (An answer that shows another device in state device is an
            // answer: never reclaimed.)
            bool answered = false, answeredFail = false;
            try {
                ListAdbEntries(env.port, canceled);
                answered = true;
            } catch (const AdbFailReply&) {
                answeredFail = true;
            } catch (const std::exception&) {}
            CheckCanceled(canceled);
            if (stale()) return Result::Canceled;
            NoteDefaultListing(!answered, ListenerIdentity(env.port));
            if (answered) {
                TraceLine("listing-hung default answers a listing again; not reclaimed");
                return Result::Running;
            }
            if (answeredFail) reclaimAllowed = false;
        }
        if (reclaimAllowed) {
            if (stale()) return Result::Canceled;
            LegacyAdbRecoveryRequest request;
            request.port = env.port;
            request.canceled = canceled;
            request.shutdownRequested = &runtime.shutdownRequested;
            request.externallyConfirmedUnresponsive = true;
            request.phoneUseIntent = true;
            request.noActiveOrStartingPhoneSessions = true;
            request.hasActiveOrStartingPhoneSessions = [&runtime] {
                std::lock_guard lock(runtime.mutex); return !runtime.occupied.empty();
            };
            request.stillWanted = stillWanted;
            LegacyAdbRecoveryResult result;
            if (hooks) {
                hooks->legacyReclaimForTest();
                result.status = LegacyAdbRecoveryStatus::NotManaged;
                result.diagnostic = "fixture seam";
            } else {
                result = TryReclaimLegacyBundledAdbServer(request);
            }
            if (result.status == LegacyAdbRecoveryStatus::Canceled) { stale(); return Result::Canceled; }
            if (result.status == LegacyAdbRecoveryStatus::Busy) return Result::BorrowedUnresponsive;
            recoveredLegacy = result.status == LegacyAdbRecoveryStatus::Recovered;
            qInfo() << "Legacy OkuFlow USB recovery:" << QString::fromStdString(result.diagnostic);
        }
        // The private server takes over USB at once. A hung default that
        // keeps the phone is then stopped by the link doctor (owner decision
        // of 2026-11-17), never here and never by name. This start beside
        // the default counts toward the per-episode hand-over cap (unless
        // the doctor's own start-shadow rung already counted it); at the cap
        // no private server starts beside it and the doctor terminates the
        // hung default or names it.
        {
            std::lock_guard lock(runtime.mutex);
            if (!recoveredLegacy) {
                if (!runtime.shadowRequested) {
                    // At the hand-over cap no private server starts beside
                    // it. A listing hang is borrowed instead (its version
                    // answers): its listing is tried again each cycle and
                    // one that succeeds ends the hang, so the sentence the
                    // cap leads to does not outlive a default that recovered.
                    if (!runtime.doctor.HandOverAllowed())
                        return listingHung ? Result::Running : Result::BorrowedUnresponsive;
                    chargeHandOver = true;
                }
                beside = Beside::Hung;
            } else {
                runtime.shadowServer = false;
                runtime.shadowForBlindness = false;
            }
        }
    } else {
        NoteDefaultProbe(defaultProbe, std::nullopt);
    }
    if (!allowOwnedBootstrap) return Result::NotReady;
    // Right before a start (beside the default, or the first engagement).
    if (stale()) return Result::Canceled;
    // Nothing to launch (missing or quarantined adb.exe) is a launch
    // failure at once, without arming the start throttle: the sentence must
    // not wait for it.
    const QString executable = hooks ? QString::fromStdWString(hooks->ownedServerExecutable)
                                     : FindAdbExecutable();
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        // A start the doctor requested beside a default that is gone now (or
        // was reclaimed as a legacy bundle) is void: this start is not beside
        // anything. Kept, it would start a private server beside a default
        // that comes back later, responsive and unexamined.
        if (beside == Beside::No) runtime.shadowRequested = false;
        if (runtime.ownedServer) { owned = runtime.ownedServer; }
        else {
            if (executable.isEmpty() || !QFileInfo(executable).isFile()) return Result::LaunchFailed;
            if (Clock::now() < runtime.nextServerStart) return Result::Throttled;
            runtime.nextServerStart = Clock::now() + std::chrono::seconds(30);
        }
        if (beside != Beside::No) {
            // The doctor's request is consumed only now: a requested start
            // that cannot launch yet stays requested (and charged once, by
            // the doctor's rung) instead of being charged again next cycle.
            runtime.shadowRequested = false;
            runtime.shadowServer = true;
            runtime.shadowForBlindness = beside == Beside::Blind;
            if (beside == Beside::Blind) runtime.shadowHandbackProbes = 0;
            if (chargeHandOver) runtime.doctor.NoteHandOver(episode);
        }
    }
    if (owned) {
        const auto result = owned->EnsureRunning(canceled, true, stillWanted);
        if (result == Result::Canceled) stale();
        return result;
    }
    owned = CreateOwnedAdbServer(executable, hooks ? hooks->ownedServerArguments : std::vector<std::wstring>{},
                                 hooks ? hooks->ownedPolicy : std::nullopt);
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        if (runtime.shutdownRequested.load()) return Result::Stopped;
        if (!runtime.ownedServer) runtime.ownedServer = owned;
        // A concurrent first engagement wins; the unstored manager has no
        // child yet, so dropping it here starts nothing.
        owned = runtime.ownedServer;
    }
    CheckCanceled(canceled);
    const auto result = owned->EnsureRunning(canceled, true, stillWanted);
    if (result == Result::Canceled) stale();
    if (result == Result::Launched || result == Result::Restarted)
        qInfo() << "OkuFlow owned adb is ready on private port" << owned->Port().value_or(0);
    return result;
}
void RequireAdbServer(const std::atomic<bool>* canceled, const std::string& wanted)
{
    using Result = OwnedAdbServer::EnsureResult;
    const auto result = EnsureAdbServer(canceled, PhoneServerIntent::PhoneUse, wanted, {}, true);
    CheckCanceled(canceled);
    if (Runtime().shutdownRequested.load()) throw std::runtime_error("Phone camera canceled");
    if (result != Result::Running && result != Result::Launched && result != Result::Restarted)
        throw std::runtime_error("The camera connection was lost. OkuFlow is reconnecting automatically.");
    ObserveServerIdentity();
}
void AdbRequest(QTcpSocket& socket, const QByteArray& service, int timeout = 3000,
                const std::atomic<bool>* canceled = nullptr)
{
    if (service.size() > 65535) throw std::runtime_error("Invalid adb request");
    Write(socket, QByteArray::number(service.size(), 16).rightJustified(4, '0') + service, canceled);
    const auto status = Read(socket, 4, timeout, canceled);
    if (status == "FAIL") {
        bool valid = false;
        const int size = Read(socket, 4, timeout, canceled).toInt(&valid, 16);
        if (!valid || size < 0 || size > kControlLimit) throw std::runtime_error("Invalid adb error");
        throw AdbFailReply(Read(socket, size, timeout, canceled).toStdString());
    }
    if (status != "OKAY") throw std::runtime_error("Invalid adb response");
}
void Transport(QTcpSocket& socket, const std::string& serial, const QByteArray& service,
               const std::atomic<bool>* canceled = nullptr, int timeout = 3000, std::uint64_t transportId = 0)
{
    Connect(socket, kHostConnectMs, canceled);
    AdbRequest(socket, transportId ? "host:transport-id:" + QByteArray::number(transportId) :
        "host:transport:" + QByteArray::fromStdString(serial), timeout, canceled);
    AdbRequest(socket, service, timeout, canceled);
}
QByteArray ReadHostReply(QTcpSocket& socket, const QByteArray& service, const std::atomic<bool>* canceled)
{
    AdbRequest(socket, service, kHostReplyFieldMs, canceled);
    bool valid = false;
    const int size = Read(socket, 4, kHostReplyFieldMs, canceled).toInt(&valid, 16);
    if (!valid || size < 0 || size > 65535) throw std::runtime_error("Invalid adb reply length");
    return Read(socket, size, kHostReplyFieldMs, canceled);
}
QByteArray AdbHostReply(const QByteArray& service, const std::atomic<bool>* canceled)
{
    CheckCanceled(canceled);
    QTcpSocket socket;
    Connect(socket, kHostConnectMs, canceled);
    return ReadHostReply(socket, service, canceled);
}
// Explicit port: the default listener while a private server shadows it.
QByteArray AdbHostReplyAt(std::uint16_t port, const QByteArray& service, const std::atomic<bool>* canceled)
{
    CheckCanceled(canceled);
    QTcpSocket socket;
    ConnectTo(socket, port, kHostConnectMs, canceled);
    return ReadHostReply(socket, service, canceled);
}
Packet ReadPacket(QTcpSocket& socket, int limit, const std::atomic<bool>* canceled = nullptr)
{
    const auto header = Read(socket, 8, 3000, canceled);
    const auto size = qFromLittleEndian<quint32>(header.constData() + 4);
    if (size > static_cast<quint32>(limit)) throw std::runtime_error("Oversized phone packet");
    return {qFromLittleEndian<quint16>(header.constData()), qFromLittleEndian<quint16>(header.constData() + 2),
            Read(socket, static_cast<int>(size), 3000, canceled)};
}
QJsonObject Json(const Packet& packet)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(packet.data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Invalid phone JSON");
    return document.object();
}
void Hello(QTcpSocket& socket, quint16 role, const QJsonObject& json,
           const std::atomic<bool>* canceled = nullptr)
{
    const auto body = QJsonDocument(json).toJson(QJsonDocument::Compact);
    QByteArray header(12, 0);
    std::memcpy(header.data(), "OKUF", 4);
    qToLittleEndian<quint16>(1, header.data() + 4);
    qToLittleEndian(role, header.data() + 6);
    qToLittleEndian<quint32>(static_cast<quint32>(body.size()), header.data() + 8);
    Write(socket, header + body, canceled);
}
bool Command(QTcpSocket& socket, QJsonObject json, const std::atomic<bool>* canceled = nullptr,
             const std::function<bool()>& allowed = {})
{
    const auto body = QJsonDocument(json).toJson(QJsonDocument::Compact);
    if (body.size() > kControlLimit) throw std::runtime_error("Oversized phone command");
    QByteArray header(8, 0);
    qToLittleEndian<quint16>(2, header.data());
    qToLittleEndian<quint32>(static_cast<quint32>(body.size()), header.data() + 4);
    return Write(socket, header + body, canceled, allowed);
}
QJsonObject ControlHello(QTcpSocket& socket, int timeout = 3000, bool hevcSupported = false,
                         const std::atomic<bool>* canceled = nullptr, bool probe = false)
{
    QJsonArray codecs{"avc"};
    if (hevcSupported) codecs.append("hevc");
    QJsonObject request{{"codecs", codecs}, {"maxWidth", 4096}, {"maxHeight", 2304},
                        {"maxPixelsPerSecond", 251658240}};
    if (probe) request.insert("probe", true);
    Hello(socket, 1, request, canceled);
    const auto header = Read(socket, 8, timeout, canceled);
    const auto size = qFromLittleEndian<quint32>(header.constData() + 4);
    if (size > kControlLimit) throw std::runtime_error("Oversized phone acknowledgement");
    const Packet packet{qFromLittleEndian<quint16>(header.constData()), qFromLittleEndian<quint16>(header.constData() + 2),
        Read(socket, static_cast<int>(size), timeout, canceled)};
    const auto json = Json(packet);
    if (packet.type == 5 && !packet.flags)
        throw PhoneProtocolError(json["code"].toString().toStdString(), json["error"].toString().toStdString());
    if (packet.type != 1 || packet.flags) throw std::runtime_error("Missing phone acknowledgement");
    if (json["protocol"].toInt() != 1 ||
        (!(probe && json["probe"].toBool()) && json["token"].toString().isEmpty()))
        throw std::runtime_error("Unsupported phone protocol");
    return json;
}

struct HevcDecoderError : std::runtime_error { using std::runtime_error::runtime_error; };

ComPtr<IMFTransform> CreatePhoneDecoder(PhoneVideoCodec codec)
{
    ComPtr<IMFTransform> transform;
    if (codec == PhoneVideoCodec::Avc) {
        Check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(transform.GetAddressOf())), "Create phone H.264 decoder");
        return transform;
    }
    if (codec != PhoneVideoCodec::Hevc) throw std::runtime_error("Unsupported phone codec");
    struct Activations {
        IMFActivate** items = nullptr;
        UINT32 count = 0;
        ~Activations() { for (UINT32 i = 0; i < count; ++i) items[i]->Release(); CoTaskMemFree(items); }
    } activations;
    const MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_HEVC};
    const MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_NV12};
    // The synchronous system MFT can internally use DXVA with our DXGI manager.
    // Asynchronous hardware-proxy MFTs need a different event-driven pump.
    Check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &activations.items, &activations.count), "Enumerate phone HEVC decoders");
    for (UINT32 i = 0; i < activations.count; ++i) {
        if (SUCCEEDED(activations.items[i]->ActivateObject(IID_PPV_ARGS(transform.ReleaseAndGetAddressOf())))) {
            activations.items[i]->DetachObject();
            return transform;
        }
    }
    throw std::runtime_error("No available Windows HEVC decoder");
}

// System H.264/HEVC MFTs accept Annex B and can share the capture session's DXGI manager.
class Decoder {
public:
    Decoder(UINT width, UINT height, UINT fps, PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
        : expectedWidth_(width), expectedHeight_(height)
    {
        transform_ = CreatePhoneDecoder(codec);
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform_->GetAttributes(&attributes))) {
            attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            UINT32 aware = 0;
            if (manager && SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware)
                transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager));
        }
        ComPtr<IMFMediaType> input;
        Check(MFCreateMediaType(&input), "Create phone input type");
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, codec == PhoneVideoCodec::Hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        if (codec == PhoneVideoCodec::Hevc) input->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        Check(transform_->SetInputType(0, input.Get(), 0), "Set phone input type");
        SelectOutput();
        Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin phone decoder");
        Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start phone decoder");
    }
    void Input(const QByteArray& data, LONGLONG timestamp, UINT fps, const FrameCallback& callback)
    {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateSample(&sample), "Create compressed sample");
        Check(MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), &buffer), "Allocate compressed sample");
        BYTE* memory = nullptr;
        Check(buffer->Lock(&memory, nullptr, nullptr), "Lock compressed sample");
        std::memcpy(memory, data.constData(), static_cast<size_t>(data.size()));
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(data.size()));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(timestamp);
        sample->SetSampleDuration(10'000'000 / std::max(1u, fps));
        HRESULT result = transform_->ProcessInput(0, sample.Get(), 0);
        if (result == MF_E_NOTACCEPTING) { Drain(fps, callback); result = transform_->ProcessInput(0, sample.Get(), 0); }
        Check(result, "Decode phone input");
        Drain(fps, callback);
    }
private:
    void SelectOutput()
    {
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            const HRESULT result = transform_->GetOutputAvailableType(0, index, &type);
            if (result == MF_E_NO_MORE_TYPES) break;
            Check(result, "Query phone decoder output");
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12) continue;
            Check(transform_->SetOutputType(0, type.Get(), 0), "Set phone NV12 output");
            Check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width_, &height_), "Read phone dimensions");
            if (!width_ || !height_ || width_ > 4096 || height_ > 2304) throw std::runtime_error("Unsupported phone dimensions");
            if (!ResolveNv12DisplayAperture(type.Get(), width_, height_, expectedWidth_, expectedHeight_, aperture_))
                throw std::runtime_error("Invalid phone decoder display aperture");
            UINT32 pitch = width_;
            type->GetUINT32(MF_MT_DEFAULT_STRIDE, &pitch);
            stride_ = static_cast<LONG>(pitch);
            return;
        }
        throw std::runtime_error("Phone decoder has no NV12 output");
    }
    void Drain(UINT fps, const FrameCallback& callback)
    {
        for (int attempt = 0; attempt < 32; ++attempt) {
            MFT_OUTPUT_STREAM_INFO info{};
            Check(transform_->GetOutputStreamInfo(0, &info), "Query decoder buffer");
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> supplied;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> buffer;
                Check(MFCreateSample(&supplied), "Create decoded sample");
                const DWORD size = std::max<DWORD>(info.cbSize, width_ * height_ * 3 / 2);
                if (size > 64 * 1024 * 1024) throw std::runtime_error("Oversized decoded phone buffer");
                Check(MFCreateAlignedMemoryBuffer(size, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Allocate decoded buffer");
                supplied->AddBuffer(buffer.Get());
                output.pSample = supplied.Get();
            }
            DWORD status = 0;
            const HRESULT result = transform_->ProcessOutput(0, 1, &output, &status);
            ComPtr<IMFCollection> events;
            events.Attach(output.pEvents);
            ComPtr<IMFSample> decoded = supplied;
            if (output.pSample && output.pSample != supplied.Get()) decoded.Attach(output.pSample);
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) { SelectOutput(); continue; }
            Check(result, "Read decoded phone frame");
            if (!decoded) continue;
            MediaFrame frame;
            frame.width = aperture_.width; frame.height = aperture_.height; frame.stride = stride_;
            frame.subtype = MFVideoFormat_NV12;
            frame.yuvColor = {YuvMatrix::Bt709, YuvRange::Limited};
            frame.frameRateNumerator = fps; frame.frameRateDenominator = 1;
            decoded->GetSampleTime(&frame.captureTimestamp100ns);
            frame.captureClock100ns = MFGetSystemTime(); // Host arrival clock; never claimed as phone sensor latency.
            frame.sequenceNumber = sequence_++;
            ComPtr<IMFMediaBuffer> buffer;
            Check(decoded->GetBufferByIndex(0, &buffer), "Read decoded buffer");
            ComPtr<IMFDXGIBuffer> dxgi;
            if (SUCCEEDED(buffer.As(&dxgi)) && SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(frame.gpuTexture.GetAddressOf())))) {
                dxgi->GetSubresourceIndex(&frame.gpuSubresource);
                D3D11_TEXTURE2D_DESC description{};
                frame.gpuTexture->GetDesc(&description);
                frame.gpuFormat = description.Format;
                frame.gpuVisibleX = aperture_.x;
                frame.gpuVisibleY = aperture_.y;
                frame.gpuSample = decoded; // Pin the decoder's reusable surface until all consumers release it.
            } else {
                std::vector<std::uint8_t> allocation;
                LONG pitch{};
                if (!CopyCaptureBuffer(buffer.Get(), frame.subtype, width_, height_, stride_, allocation, pitch))
                    throw std::runtime_error("Invalid decoded phone buffer layout");
                D3D11_MAPPED_SUBRESOURCE packed{allocation.data(), static_cast<UINT>(pitch), 0};
                if (!CopyMappedCaptureTexture(packed, DXGI_FORMAT_NV12, width_, height_,
                        frame.width, frame.height, frame.data, frame.stride, frame.subtype,
                        aperture_.x, aperture_.y))
                    throw std::runtime_error("Invalid decoded phone display pixels");
            }
            frame.dataSize = frame.data.empty() ? static_cast<size_t>(frame.width) * frame.height * 3 / 2 : frame.data.size();
            if (callback) callback(std::move(frame));
        }
        throw std::runtime_error("Phone decoder output burst exceeded its bound");
    }
    ComPtr<IMFTransform> transform_;
    UINT width_{}, height_{};
    UINT expectedWidth_{}, expectedHeight_{};
    CaptureAperture aperture_{};
    LONG stride_{};
    std::uint64_t sequence_{};
};
} // namespace

AdbServerEndpoint ParseAdbServerEndpoint(std::string_view serverSocket, std::string_view serverPort,
                                         std::string_view serverAddress)
{
    AdbServerEndpoint endpoint;
    endpoint.overridden = !serverSocket.empty() || !serverPort.empty() || !serverAddress.empty();
    const auto local = [](std::string_view host) { return host.empty() || host == "localhost" || host == "127.0.0.1"; };
    std::string_view port = serverPort;
    if (!serverSocket.empty()) {
        if (!serverSocket.starts_with("tcp:")) return {false, true, 5037};
        const auto address = serverSocket.substr(4);
        const auto separator = address.rfind(':');
        if (separator == std::string_view::npos) port = address;
        else {
            if (!local(address.substr(0, separator))) return {false, true, 5037};
            port = address.substr(separator + 1);
        }
        if (port.empty()) return {false, true, 5037};
    } else if (!local(serverAddress)) return {false, true, 5037};
    if (!port.empty()) {
        unsigned value = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        if (parsed.ec != std::errc() || parsed.ptr != port.data() + port.size() || value == 0 || value > 65535)
            return {false, true, 5037};
        endpoint.port = static_cast<std::uint16_t>(value);
    }
    return endpoint;
}
void ShutdownOwnedAdbServer()
{
    auto& runtime = Runtime();
    runtime.shutdownRequested.store(true);
    std::shared_ptr<OwnedAdbServer> owned;
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        owned = runtime.ownedServer;
    }
    // Stop outside the runtime lock: an in-flight EnsureRunning finishes its
    // bounded work first (Stop serializes on the manager), and no borrowed
    // server is ever reachable through this pointer.
    if (owned) owned->Stop();
    std::lock_guard lifecycleLock(runtime.lifecycleMutex);
    {
        std::lock_guard lock(runtime.mutex);
        owned = std::move(runtime.ownedServer);
    }
    if (owned) owned->Stop();
}
std::optional<std::uint16_t> OwnedAdbServerPort()
{
    std::shared_ptr<OwnedAdbServer> owned;
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        owned = runtime.ownedServer;
    }
    return owned ? owned->Port() : std::nullopt;
}
void SetPhoneAdbTestHooks(std::optional<PhoneAdbTestHooks> hooks)
{
    ShutdownOwnedAdbServer();
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    runtime.hooks = std::move(hooks); runtime.nextServerStart = {};
    runtime.shutdownRequested.store(false);
    runtime.lastIdentity.reset(); runtime.serverEpoch = 0;
    runtime.borrowedFailureIdentity.reset(); runtime.borrowedFailureSince = {};
    runtime.borrowedFailures = 0; runtime.shadowServer = false; runtime.shadowHandbackProbes = 0;
    runtime.nextUsbRecovery.clear(); runtime.nextUsbHealthProbe.clear();
    runtime.usbRecoveryAttempts.clear();
    runtime.doctor = PhoneLinkDoctor(runtime.hooks && runtime.hooks->linkPolicy ? *runtime.hooks->linkPolicy
                                                                                : PhoneLinkPolicy{});
    runtime.lastEvent = {};
    runtime.shadowRequested = runtime.shadowForBlindness = runtime.borrowedConfirmed = false;
    runtime.defaultIdentity.reset();
    runtime.refusedListener.reset();
    runtime.launchFailures = 0;
    ClearDefaultListingHang(runtime);
    runtime.lastEnsureResult.reset();
    runtime.besideIdentity.reset();
    runtime.askedIdentity.reset();
    runtime.defaultStall.reset();
    runtime.hungServesOther.reset();
    runtime.siblings.clear();
}
bool PhoneAdbShadowRequestedForTest()
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    return runtime.shadowRequested;
}
void PhoneCameraTransport::AllowAppLaunch(const std::string& serial)
{
    (void)serial; // One budget for the phone link, renewed by the deliberate pick.
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); runtime.doctor.RenewLaunches();
}

struct PhoneVideoDecoder::Impl {
    Decoder decoder;
    UINT fps;
    Impl(UINT width, UINT height, UINT rate, PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
        : decoder(width, height, rate, codec, manager), fps(rate) {}
};
PhoneVideoDecoder::PhoneVideoDecoder(UINT width, UINT height, UINT fps, PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
    : impl_(std::make_unique<Impl>(width, height, fps, codec, manager)) {}
PhoneVideoDecoder::~PhoneVideoDecoder() = default;
bool PhoneVideoDecoder::IsSupported(PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
{
    try { PhoneVideoDecoder probe(1920, 1080, 60, codec, manager); return true; }
    catch (const std::exception&) { return false; }
}
void PhoneVideoDecoder::Decode(std::span<const std::uint8_t> data, LONGLONG timestamp, const FrameCallback& callback)
{
    if (data.empty() || data.size() > kVideoLimit) throw std::runtime_error("Invalid compressed phone frame size");
    impl_->decoder.Input(QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())), timestamp, impl_->fps, callback);
}

namespace {
PhoneAdbState StateFromProto(std::uint64_t state)
{
    // AOSP devices.proto ConnectionState (ANY=0 is the proto3 default).
    switch (state) {
    case 1: return PhoneAdbState::Connecting;
    case 2: return PhoneAdbState::Authorizing;
    case 3: return PhoneAdbState::Unauthorized;
    case 4: return PhoneAdbState::NoPermissions;
    case 6: return PhoneAdbState::Offline;
    case 8: return PhoneAdbState::Device;
    default: return PhoneAdbState::Other;
    }
}
const char* StateName(PhoneAdbState state)
{
    switch (state) {
    case PhoneAdbState::NotListed: return "not-listed";
    case PhoneAdbState::Device: return "device";
    case PhoneAdbState::Unauthorized: return "unauthorized";
    case PhoneAdbState::Authorizing: return "authorizing";
    case PhoneAdbState::Connecting: return "connecting";
    case PhoneAdbState::Offline: return "offline";
    case PhoneAdbState::NoPermissions: return "no-permissions";
    case PhoneAdbState::Other: return "other";
    }
    return "other";
}
CameraDescriptor DescriptorFor(const AdbDeviceEntry& entry)
{
    CameraDescriptor descriptor;
    descriptor.phoneSerial = entry.serial;
    descriptor.phoneTransportId = entry.transportId;
    descriptor.symbolicLink = QString("okuflow-phone:%1").arg(QString::fromStdString(entry.serial)).toStdWString();
    descriptor.name = QString("OkuFlow Camera · %1").arg(QString::fromStdString(entry.model.empty() ? entry.serial : entry.model)
        .replace('_', ' ')).toStdWString();
    return descriptor;
}
} // namespace

std::vector<AdbDeviceEntry> ParseAdbDeviceEntries(std::span<const std::uint8_t> protobuf)
{
    struct Reader {
        std::span<const std::uint8_t> bytes;
        size_t position{};
        std::uint64_t Varint() {
            std::uint64_t result = 0;
            for (int shift = 0; shift <= 63; shift += 7) {
                if (position >= bytes.size()) throw std::runtime_error("Truncated adb protobuf varint");
                const auto byte = bytes[position++];
                if (shift == 63 && byte > 1) throw std::runtime_error("Overflowing adb protobuf varint");
                result |= static_cast<std::uint64_t>(byte & 127) << shift;
                if (byte < 128) return result;
            }
            throw std::runtime_error("Invalid adb protobuf varint");
        }
        std::span<const std::uint8_t> Bytes(std::uint64_t size) {
            if (size > bytes.size() - position) throw std::runtime_error("Truncated adb protobuf field");
            auto result = bytes.subspan(position, static_cast<size_t>(size));
            position += static_cast<size_t>(size);
            return result;
        }
        void Skip(unsigned wire) {
            if (wire == 0) Varint();
            else if (wire == 1) Bytes(8);
            else if (wire == 2) { const auto size = Varint(); Bytes(size); }
            else if (wire == 5) Bytes(4);
            else throw std::runtime_error("Unsupported adb protobuf field");
        }
    } root{protobuf};
    if (protobuf.size() > 65535) throw std::runtime_error("Oversized adb device listing");
    std::vector<AdbDeviceEntry> entries;
    while (root.position < root.bytes.size()) {
        const auto tag = root.Varint();
        if (!(tag >> 3)) throw std::runtime_error("Invalid adb protobuf field");
        if (tag != 10) { root.Skip(static_cast<unsigned>(tag & 7)); continue; }
        const auto size = root.Varint();
        Reader device{root.Bytes(size)};
        AdbDeviceEntry entry;
        std::uint64_t state = 0, type = 0;
        while (device.position < device.bytes.size()) {
            const auto field = device.Varint();
            if (!(field >> 3)) throw std::runtime_error("Invalid adb device field");
            if (field == 10 || field == 42) {
                const auto length = device.Varint();
                const auto text = device.Bytes(length);
                if (length > 512) throw std::runtime_error("Oversized adb device name");
                (field == 10 ? entry.serial : entry.model).assign(reinterpret_cast<const char*>(text.data()), text.size());
            } else if (field == 16) state = device.Varint();
            else if (field == 56) type = device.Varint();
            else if (field == 80) entry.transportId = device.Varint();
            else device.Skip(static_cast<unsigned>(field & 7));
        }
        if (entry.serial.empty()) continue;
        entry.state = StateFromProto(state);
        entry.usb = type == 1 && !entry.serial.starts_with("emulator-");
        entries.push_back(std::move(entry));
        if (entries.size() > 512) throw std::runtime_error("Too many adb devices");
    }
    return entries;
}

std::vector<CameraDescriptor> ParseAdbUsbDevices(std::span<const std::uint8_t> protobuf)
{
    std::vector<CameraDescriptor> devices;
    for (const auto& entry : ParseAdbDeviceEntries(protobuf)) {
        if (!entry.usb || entry.state != PhoneAdbState::Device) continue;
        devices.push_back(DescriptorFor(entry));
        if (devices.size() > 128) throw std::runtime_error("Too many USB devices");
    }
    return devices;
}

std::vector<AdbDeviceEntry> ParseAdbDevicesLongText(std::string_view text)
{
    std::vector<AdbDeviceEntry> entries;
    for (const auto& line : QByteArray(text.data(), static_cast<qsizetype>(text.size())).split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 2 || fields[0].isEmpty()) continue;
        AdbDeviceEntry entry;
        entry.serial = fields[0].toStdString();
        const auto& word = fields[1];
        entry.state = word == "device" ? PhoneAdbState::Device : word == "unauthorized" ? PhoneAdbState::Unauthorized :
            word == "offline" ? PhoneAdbState::Offline : word == "authorizing" ? PhoneAdbState::Authorizing :
            word == "connecting" ? PhoneAdbState::Connecting : word == "no" ? PhoneAdbState::NoPermissions :
            PhoneAdbState::Other;
        for (const auto& field : fields) {
            if (field.startsWith("usb:")) entry.usb = true;
            if (field.startsWith("model:")) entry.model = field.mid(6).toStdString();
            if (field.startsWith("transport_id:")) {
                bool valid = false;
                const auto id = field.mid(13).toULongLong(&valid);
                if (valid) entry.transportId = id;
            }
        }
        if (entry.serial.starts_with("emulator-")) entry.usb = false;
        entries.push_back(std::move(entry));
        if (entries.size() >= 64) break;
    }
    return entries;
}

PhoneShellFacts ParsePhoneShellFacts(std::string_view text)
{
    PhoneShellFacts facts;
    char section = 0;
    bool sawEnd = false, sawPackages = false, sawProcesses = false, packageLine = false, processLine = false;
    bool keyguardTrue = false, keyguardFalse = false;
    for (const auto& raw : QByteArray(text.data(), static_cast<qsizetype>(text.size())).split('\n')) {
        const auto line = raw.trimmed();
        if (line == "W:" || line == "K:" || line == "P:" || line == "R:") {
            section = line[0];
            if (section == 'P') sawPackages = true;
            if (section == 'R') sawProcesses = true;
            continue;
        }
        if (line == kPhoneShellFactsEnd) { sawEnd = true; break; }
        if (section == 'W') {
            const auto at = line.indexOf("mWakefulness=");
            if (at >= 0) {
                const auto value = line.mid(at + 13).split(' ').front();
                if (value == "Awake") facts.screenAwake = PhoneFact::Yes;
                else if (value == "Asleep" || value == "Dozing" || value == "Dreaming") facts.screenAwake = PhoneFact::No;
            }
        } else if (section == 'K') {
            // Android 9 and older print the policy's mShowingLockscreen;
            // Android 10 to 14 print the KeyguardController's
            // mKeyguardShowing / KeyguardShowing (substring of both);
            // DeviceIdleController's mScreenLocked (Android 10 and later) is
            // the fallback when the window dump changes again.
            for (const char* key : {"mShowingLockscreen=", "mDreamingLockscreen=", "KeyguardShowing=",
                                    "mScreenLocked="}) {
                const auto at = line.indexOf(key);
                if (at < 0) continue;
                const auto value = line.mid(at + static_cast<qsizetype>(std::strlen(key)));
                if (value.startsWith("true")) keyguardTrue = true;
                else if (value.startsWith("false")) keyguardFalse = true;
            }
        } else if (section == 'P') {
            if (line.startsWith("package:")) packageLine = true;
        } else if (section == 'R') {
            if (!line.isEmpty() && std::all_of(line.begin(), line.end(), [](char c) { return c == ' ' || (c >= '0' && c <= '9'); }))
                processLine = true;
        }
    }
    if (keyguardTrue) facts.keyguardShowing = PhoneFact::Yes;
    else if (keyguardFalse) facts.keyguardShowing = PhoneFact::No;
    if (packageLine) facts.appInstalled = PhoneFact::Yes;
    else if (sawPackages && sawEnd) facts.appInstalled = PhoneFact::No;
    if (processLine) facts.appRunning = PhoneFact::Yes;
    else if (sawProcesses && sawEnd) facts.appRunning = PhoneFact::No;
    return facts;
}

PhoneAdbInterface QueryAndroidAdbInterface()
{
    // Android USB debugging interface class (WinUSB/ADB), also registered for
    // arrival notifications in main_window.cpp.
    static const GUID kAdbInterface{0xf72fe0d4, 0xcbcb, 0x407d, {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b}};
    HDEVINFO set = SetupDiGetClassDevsW(&kAdbInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return PhoneAdbInterface::Unknown;
    SP_DEVICE_INTERFACE_DATA data{};
    data.cbSize = sizeof(data);
    const bool any = SetupDiEnumDeviceInterfaces(set, nullptr, &kAdbInterface, 0, &data) != FALSE;
    const DWORD error = any ? ERROR_SUCCESS : GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    if (any) return PhoneAdbInterface::Present;
    return error == ERROR_NO_MORE_ITEMS ? PhoneAdbInterface::Absent : PhoneAdbInterface::Unknown;
}

std::string PhoneSerialTag(std::string_view serial)
{
    if (serial.empty()) return "phone-none";
    const auto digest = QCryptographicHash::hash(QByteArray(serial.data(), static_cast<qsizetype>(serial.size())),
                                                 QCryptographicHash::Sha256).toHex().left(8);
    return "phone-" + digest.toStdString();
}

namespace {
// The file is written by one private writer thread, never by the caller
// (events arrive on the UI thread). The state is intentionally leaked so the
// detached writer never outlives it at process exit.
struct TraceFile {
    std::mutex mutex;
    std::condition_variable wake, drained;
    std::wstring path;
    std::deque<std::pair<std::wstring, std::string>> pending;
    bool writing{false};
    bool started{false};
};
TraceFile& Trace() { static auto* trace = new TraceFile; return *trace; }
constexpr long long kTraceFileLimit = 256 * 1024;
constexpr size_t kTraceQueueLimit = 1024;
void AppendTraceLine(const std::wstring& path, const std::string& text)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
        ((static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) >= kTraceFileLimit)
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file) return;
    std::fwrite(text.data(), 1, text.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
}
void TraceWriter()
{
    auto& trace = Trace();
    std::unique_lock lock(trace.mutex);
    for (;;) {
        trace.wake.wait(lock, [&] { return !trace.pending.empty(); });
        auto [path, text] = std::move(trace.pending.front());
        trace.pending.pop_front();
        trace.writing = true;
        lock.unlock();
        AppendTraceLine(path, text);
        lock.lock();
        trace.writing = false;
        if (trace.pending.empty()) trace.drained.notify_all();
    }
}
} // namespace

void SetPhoneLinkTraceFile(const std::wstring& path)
{
    auto& trace = Trace();
    std::lock_guard lock(trace.mutex);
    trace.path = path;
}

void TracePhoneLink(std::string_view line)
{
    const auto text = std::to_string(NowMs()) + " " +
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString() + " " + std::string(line);
    qInfo().noquote() << "Phone link:" << QString::fromStdString(text);
    auto& trace = Trace();
    std::lock_guard lock(trace.mutex);
    if (trace.path.empty() || trace.pending.size() >= kTraceQueueLimit) return;
    trace.pending.emplace_back(trace.path, text);
    if (!trace.started) {
        trace.started = true;
        std::thread(TraceWriter).detach();
    }
    trace.wake.notify_one();
}

namespace { void TraceLine(std::string_view line) { TracePhoneLink(line); } }

bool FlushPhoneLinkTrace(std::chrono::milliseconds timeout)
{
    auto& trace = Trace();
    std::unique_lock lock(trace.mutex);
    return trace.drained.wait_for(lock, timeout, [&] { return trace.pending.empty() && !trace.writing; });
}

void NotePhoneLinkEvent(PhoneLinkEvent event)
{
    // Runs on the UI thread: only short critical sections. The owned-server
    // reset is lock-free (a worker may hold that manager through a launch)
    // and the trace line is written by the trace thread.
    auto& runtime = Runtime();
    std::shared_ptr<OwnedAdbServer> owned;
    bool coalesced = false;
    {
        std::lock_guard lock(runtime.mutex);
        const auto now = Clock::now();
        // Several resume signals of one wake are one event: nothing is
        // reset again (the doctor decides; see resumeCoalesceMs).
        if (!runtime.doctor.OnEvent(event, NowMs(now))) {
            coalesced = true;
        }
    }
    if (coalesced) {
        TracePhoneLink("event " + std::string(PhoneLinkEventName(event)) + " coalesced with the previous one");
        return;
    }
    {
        std::lock_guard lock(runtime.mutex);
        const auto now = Clock::now();
        runtime.lastEvent = now;
        runtime.nextUsbRecovery.clear();
        runtime.nextUsbHealthProbe.clear();
        runtime.usbRecoveryAttempts.clear();
        runtime.nextServerStart = {};
        runtime.borrowedFailures = 0;
        runtime.borrowedFailureSince = {};
        runtime.borrowedFailureIdentity.reset();
        runtime.borrowedConfirmed = false;
        ClearDefaultListingHang(runtime); // Pre-event listings are stale too.
        // A private-server start the doctor requested belongs to the old
        // episode (its hand-over was charged there).
        runtime.shadowRequested = false;
        runtime.defaultStall.reset(); // A re-plug or resume may have healed it.
        runtime.hungServesOther.reset();
        // Pre-sleep / pre-removal transport ids and server health are stale:
        // the next identity observation starts a new server epoch.
        if (event == PhoneLinkEvent::Resume || event == PhoneLinkEvent::DeviceRemoval)
            runtime.lastIdentity.reset();
        owned = runtime.ownedServer;
    }
    if (owned) owned->ResetBudgets();
    TracePhoneLink("event " + std::string(PhoneLinkEventName(event)));
}

namespace {
// Listing without companion probes: recovery must not touch other phones.
std::vector<CameraDescriptor> ListUsbPhones(const std::atomic<bool>* canceled,
                                            std::vector<AdbDeviceEntry>* rawEntries = nullptr)
{
    bool text = false;
    auto entries = ListAdbEntries(std::nullopt, canceled, &text);
    std::vector<CameraDescriptor> attached;
    for (const auto& entry : entries) {
        if (!entry.usb || entry.state != PhoneAdbState::Device) continue;
        if (text && attached.size() >= 8) continue;
        attached.push_back(DescriptorFor(entry));
        if (attached.size() > 128) throw std::runtime_error("Too many USB devices");
    }
    std::uint64_t epoch = 0;
    { auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); epoch = runtime.serverEpoch; }
    for (auto& descriptor : attached) descriptor.phoneServerEpoch = epoch;
    if (rawEntries) *rawEntries = std::move(entries);
    return attached;
}

// True when `line` is one whole line of `text` (LF or CRLF line ends).
bool HasWholeLine(const QByteArray& text, const QByteArray& line)
{
    for (auto at = text.indexOf(line); at >= 0; at = text.indexOf(line, at + 1)) {
        const auto end = at + line.size();
        const bool starts = at == 0 || text[at - 1] == '\n';
        const bool ends = end < text.size() &&
            (text[end] == '\n' || (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n'));
        if (starts && ends) return true;
    }
    return false;
}
// Reads a short shell reply until the stream closes, the line `until`
// arrives (a whole line, never a substring: dumpsys lines and package paths
// can contain any word), the deadline passes or 64 KiB arrived.
QByteArray DrainShell(QTcpSocket& socket, const std::atomic<bool>* canceled, int timeoutMs,
                      const QByteArray& until = {})
{
    QByteArray response;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (socket.state() == QAbstractSocket::ConnectedState && Clock::now() < deadline) {
        CheckCanceled(canceled);
        socket.waitForReadyRead(20);
        response += socket.readAll();
        if (response.size() > kControlLimit) throw std::runtime_error("Oversized phone shell response");
        if (!until.isEmpty() && HasWholeLine(response, until)) break;
    }
    response += socket.readAll();
    if (response.size() > kControlLimit) throw std::runtime_error("Oversized phone shell response");
    return response;
}

enum class CompanionLaunch { Started, NotInstalled, Dropped };
// Rung 5: wake the screen (harmless input) and start the activity with the
// host-launch extra, which lets a new companion show over the keyguard. The
// Play package is tried first; the legacy debug package only when the new
// activity class is absent. Never force-stops anything.
CompanionLaunch LaunchCompanion(const std::string& serial, std::uint64_t transportId,
                                const std::atomic<bool>* canceled, bool wake,
                                const std::function<bool()>& stillWanted = {})
{
    // Each shell command is checked right before it is sent: a source
    // change meanwhile sends nothing more (Dropped).
    if (stillWanted && !stillWanted()) return CompanionLaunch::Dropped;
    if (wake) {
        try {
            QTcpSocket waker;
            Transport(waker, serial, "shell:input keyevent KEYCODE_WAKEUP", canceled, 1000, transportId);
            DrainShell(waker, canceled, 1000);
        } catch (const std::exception&) {
            CheckCanceled(canceled);
        }
    }
    for (const char* command : {
        "shell:am start -n com.okuflow/com.okuflow.camera.MainActivity --ez com.okuflow.camera.extra.HOST_LAUNCH true",
        "shell:am start -n com.okuflow.camera/.MainActivity --ez com.okuflow.camera.extra.HOST_LAUNCH true"}) {
        if (stillWanted && !stillWanted()) return CompanionLaunch::Dropped;
        QTcpSocket launcher;
        Transport(launcher, serial, command, canceled, 1000, transportId);
        const auto response = DrainShell(launcher, canceled, 2000);
        if (!response.contains("Error") && !response.contains("Exception")) return CompanionLaunch::Started;
        const bool legacy = std::string_view(command).find("com.okuflow.camera/.MainActivity") != std::string_view::npos;
        if (legacy || !response.contains("Error type 3")) return CompanionLaunch::NotInstalled;
    }
    return CompanionLaunch::NotInstalled;
}

// Read-only phone facts in one bounded shell request. A timeout here (no
// byte at all) is adbd silence; any reply proves adbd answers.
constexpr char kShellFactsCommand[] =
    "shell:echo W:; dumpsys power | grep -m1 mWakefulness=; echo K:; "
    "dumpsys window | grep -E 'mShowingLockscreen|mDreamingLockscreen|KeyguardShowing' | head -n 6; "
    "dumpsys deviceidle | grep -m1 mScreenLocked=; "
    "echo P:; pm path com.okuflow; pm path com.okuflow.camera; echo R:; pidof com.okuflow com.okuflow.camera; "
    "echo OKUFLOW_FACTS_END";
PhoneShellFacts QueryShellFacts(const AdbDeviceEntry& entry, const std::atomic<bool>* canceled, bool* timedOut)
{
    *timedOut = false;
    try {
        QTcpSocket shell;
        Transport(shell, entry.serial, kShellFactsCommand, canceled, 2000, entry.transportId);
        const auto response = DrainShell(shell, canceled, 2500, kPhoneShellFactsEnd);
        if (response.isEmpty() && shell.state() == QAbstractSocket::ConnectedState) *timedOut = true;
        return ParsePhoneShellFacts(std::string_view(response.constData(), static_cast<size_t>(response.size())));
    } catch (const UsbResponseTimeout&) {
        CheckCanceled(canceled);
        *timedOut = true;
    } catch (const std::exception&) {
        CheckCanceled(canceled);
    }
    return {};
}

using RecoveryKey = std::pair<std::uint16_t, std::string>;
constexpr std::string_view kUsbHealthToken = "OKUFLOW_USB_HEALTH\n";
// Long enough for a slow first shell on a loaded phone; a reset now needs
// two consecutive timeouts at this budget, not one.
constexpr int kUsbHealthProbeTimeoutMs = 2000;
// Healthy/definitive answers suppress shell probes briefly; events clear it.
constexpr auto kUsbHealthProbeCooldown = std::chrono::seconds(10);
// Reconnect cadence per phone/server: at once after an event, then 3 s, then
// 30 s until the next event (was a flat minute that no event renewed).
constexpr auto kUsbReconnectRetry = std::chrono::seconds(3);
constexpr auto kUsbReconnectCooldown = std::chrono::seconds(30);
constexpr auto kUsbReconnectVerifyWindow = std::chrono::seconds(2);

bool SameUsbTransportAttached(const std::vector<CameraDescriptor>& phones,
                              const CameraDescriptor& descriptor)
{
    return std::any_of(phones.begin(), phones.end(), [&](const auto& phone) {
        return phone.phoneSerial == descriptor.phoneSerial &&
            phone.phoneTransportId == descriptor.phoneTransportId &&
            (!phone.phoneServerEpoch || !descriptor.phoneServerEpoch ||
             phone.phoneServerEpoch == descriptor.phoneServerEpoch);
    });
}

void SuppressUsbHealthProbes(const RecoveryKey& key)
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    runtime.nextUsbHealthProbe[key] = Clock::now() + kUsbHealthProbeCooldown;
}

// One device-shell health probe. Returns true only on an actual response
// timeout; producing the full echoed token proves the transport progresses
// in both directions, so a single early byte followed by a stall is not
// mistaken for health. Other failures propagate and fail closed.
bool UsbShellHealthProbeTimedOut(const CameraDescriptor& descriptor,
                                  const std::atomic<bool>* canceled)
{
    try {
        QTcpSocket health;
        Transport(health, descriptor.phoneSerial, "shell:echo OKUFLOW_USB_HEALTH", canceled,
                  kUsbHealthProbeTimeoutMs, descriptor.phoneTransportId);
        const auto response = Read(health, static_cast<int>(kUsbHealthToken.size()), kUsbHealthProbeTimeoutMs, canceled);
        if (response != QByteArray(kUsbHealthToken.data(), static_cast<int>(kUsbHealthToken.size())))
            qWarning() << "Unexpected phone USB health echo; preserving this transport";
        return false;
    } catch (const UsbResponseTimeout&) {
        CheckCanceled(canceled);
        return true;
    }
}

enum class CompanionRecoveryProbe { DefinitiveResponse, TimedOut };

// Fresh non-claiming companion probe, taken immediately before a reset may
// be sent. Any definitive answer -- a probe acknowledgement (including
// busy=true for another host), a coded phone problem, or an adbd refusal --
// proves this transport can still progress and must not be reset, however
// stale the caller's descriptor was. Only a timeout is consistent with the
// stalled-shell evidence.
CompanionRecoveryProbe ProbeCompanionBeforeRecovery(const CameraDescriptor& descriptor,
                                                    const std::atomic<bool>* canceled)
{
    try {
        QTcpSocket probe;
        Transport(probe, descriptor.phoneSerial, "localabstract:okuflow_camera", canceled,
                  1000, descriptor.phoneTransportId);
        ControlHello(probe, 1000, false, canceled, true);
        return CompanionRecoveryProbe::DefinitiveResponse;
    } catch (const UsbResponseTimeout&) {
        CheckCanceled(canceled);
        return CompanionRecoveryProbe::TimedOut;
    } catch (const std::exception&) {
        CheckCanceled(canceled);
        return CompanionRecoveryProbe::DefinitiveResponse;
    }
}
} // namespace

bool PhoneCameraTransport::RecoverStalledUsbTransport(const CameraDescriptor& descriptor,
                                                      const std::atomic<bool>* canceled,
                                                      const std::function<bool()>& stillWanted)
{
    const auto endpoint = Endpoint();
    if (!endpoint.supported || descriptor.phoneSerial.empty() || !descriptor.phoneTransportId ||
        descriptor.phoneBusy || !descriptor.phoneProblem.empty()) return false;
    const auto key = std::make_pair(endpoint.port, descriptor.phoneSerial);
    try {
        CheckCanceled(canceled);
        {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            const auto now = Clock::now();
            const auto previous = runtime.nextUsbRecovery.find(key);
            if (previous != runtime.nextUsbRecovery.end() && now < previous->second) return false;
            const auto probed = runtime.nextUsbHealthProbe.find(key);
            if (probed != runtime.nextUsbHealthProbe.end() && now < probed->second) return false;
        }
        // A healthy TCP listener alone is not sufficient. Never try to replace
        // a non-adb listener or a globally hung/shared adb server.
        bool valid = false;
        const auto version = AdbHostReply("host:version", canceled);
        version.toUInt(&valid, 16);
        if (!valid || version.size() != 4) return false;
        if (!SameUsbTransportAttached(ListUsbPhones(canceled), descriptor)) return false;
        // Distinguish a stuck adb device transport from a stuck/paused camera
        // app or decoder. Two independent shell probes must both time out;
        // one slow or transiently delayed shell never resets a transport
        // other adb clients may be using. These probes launch no activity.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!UsbShellHealthProbeTimedOut(descriptor, canceled)) {
                SuppressUsbHealthProbes(key);
                return false;
            }
        }
        // Recheck after the timeouts; a replug/new transport must not receive
        // a reset intended for the stale attachment.
        if (!SameUsbTransportAttached(ListUsbPhones(canceled), descriptor)) return false;
        // The handed-in busy/problem gate can be stale (the Run failure path
        // never populated it). Re-probe without claiming: another host may
        // have taken the session while the shell probes were timing out.
        if (ProbeCompanionBeforeRecovery(descriptor, canceled) != CompanionRecoveryProbe::TimedOut) {
            SuppressUsbHealthProbes(key);
            return false;
        }
        if (!SameUsbTransportAttached(ListUsbPhones(canceled), descriptor)) return false;
        if (stillWanted && !stillWanted()) return false;
        {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            auto& next = runtime.nextUsbRecovery[key];
            if (Clock::now() < next) return false;
            // Reserve before sending, including unsupported/failed replies.
            auto& attempts = runtime.usbRecoveryAttempts[key];
            next = Clock::now() + (attempts == 0 ? std::chrono::duration_cast<Clock::duration>(kUsbReconnectRetry)
                                                 : std::chrono::duration_cast<Clock::duration>(kUsbReconnectCooldown));
            ++attempts;
        }
        const auto reply = AdbHostReply("host-transport-id:" +
            QByteArray::number(descriptor.phoneTransportId) + ":reconnect", canceled);
        if (!reply.startsWith("reconnecting ")) return false;
        // An acknowledgement is a request accepted, not an effect observed.
        // Report success only once the stale attachment actually disappears
        // or re-registers under a different transport ID.
        const auto verifyDeadline = Clock::now() + kUsbReconnectVerifyWindow;
        for (;;) {
            CheckCanceled(canceled);
            try {
                if (!SameUsbTransportAttached(ListUsbPhones(canceled), descriptor)) {
                    qWarning() << "Stalled phone USB transport reset; waiting for rediscovery";
                    return true;
                }
            } catch (const std::exception&) {
                CheckCanceled(canceled); // The server may be mid-reconnect; keep verifying.
            }
            if (Clock::now() >= verifyDeadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        qWarning() << "Phone USB reconnect was acknowledged but the same transport is still listed";
        return false;
    } catch (const std::exception&) {
        // Cancellation, unsupported service, offline device or unhealthy server:
        // ordinary discovery remains responsible for convergence.
        return false;
    }
}

namespace {
struct DiscoveryFacts {
    bool endpointSupported{true};
    PhoneServerProbe probe{PhoneServerProbe::Unknown};
    std::vector<AdbDeviceEntry> entries;
    std::map<std::string, std::pair<PhoneCompanionProbe, std::string>> companions;
};

const char* ServerKindName(PhoneServerKind kind)
{
    switch (kind) {
    case PhoneServerKind::None: return "none";
    case PhoneServerKind::Owned: return "owned";
    case PhoneServerKind::BorrowedDefault: return "borrowed-default";
    case PhoneServerKind::Override: return "override";
    }
    return "none";
}
const char* ProbeName(PhoneServerProbe probe)
{
    switch (probe) {
    case PhoneServerProbe::Unknown: return "unknown";
    case PhoneServerProbe::Responsive: return "responsive";
    case PhoneServerProbe::Unresponsive: return "unresponsive";
    case PhoneServerProbe::NoListener: return "no-listener";
    }
    return "unknown";
}
const char* CompanionName(PhoneCompanionProbe probe)
{
    switch (probe) {
    case PhoneCompanionProbe::Unknown: return "unknown";
    case PhoneCompanionProbe::Ready: return "ready";
    case PhoneCompanionProbe::SessionBusy: return "session-busy";
    case PhoneCompanionProbe::Problem: return "problem";
    case PhoneCompanionProbe::Refused: return "refused";
    case PhoneCompanionProbe::TimedOut: return "timed-out";
    }
    return "unknown";
}
const char* InterfaceName(PhoneAdbInterface value)
{
    return value == PhoneAdbInterface::Present ? "present" : value == PhoneAdbInterface::Absent ? "absent" : "unknown";
}
const char* FactName(PhoneFact fact)
{
    return fact == PhoneFact::Yes ? "yes" : fact == PhoneFact::No ? "no" : "?";
}

// Lists the default listener directly (while a private server shadows it).
// `answeredFail`: the listing failed, but with an adb FAIL reply (alive).
std::optional<std::vector<AdbDeviceEntry>> ListDefaultEntries(const std::atomic<bool>* canceled,
                                                              bool* answeredFail = nullptr)
{
    if (answeredFail) *answeredFail = false;
    try {
        // Both forms, as every listing: an older adb on the default port
        // answers only the text one (FAIL to the protobuf form).
        return ListAdbEntries(DefaultPort(), canceled);
    } catch (const AdbFailReply&) {
        CheckCanceled(canceled);
        if (answeredFail) *answeredFail = true;
        return std::nullopt;
    } catch (const std::exception&) {
        CheckCanceled(canceled);
        return std::nullopt;
    }
}
bool OtherDeviceServed(const std::vector<AdbDeviceEntry>& entries, const std::string& target)
{
    return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.state == PhoneAdbState::Device && entry.serial != target;
    });
}

void AfterDefaultStopped()
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    runtime.shadowServer = false;
    runtime.shadowForBlindness = false;
    runtime.shadowHandbackProbes = 0;
    runtime.borrowedFailures = 0;
    runtime.borrowedFailureSince = {};
    runtime.borrowedFailureIdentity.reset();
    runtime.borrowedConfirmed = false;
    ClearDefaultListingHang(runtime);
    runtime.defaultIdentity.reset();
    runtime.defaultStall.reset();
    runtime.hungServesOther.reset();
    runtime.nextServerStart = {};
}
// The default lists the phone usable: over USB as ready, unauthorized or
// authorizing. Offline, connecting, no permissions or an unknown state there
// is holding the interface without serving it, never a reason to hand back
// or to keep a takeover from going on.
bool ListsTargetUsable(const std::vector<AdbDeviceEntry>& entries, const std::string& target)
{
    return std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
        return e.usb && (target.empty() || e.serial == target) &&
            (e.state == PhoneAdbState::Device || e.state == PhoneAdbState::Unauthorized ||
             e.state == PhoneAdbState::Authorizing);
    });
}
// The serial whose "device" listing on the current default-port listener
// is known stalled (adbd timed out for it there), if any.
std::string StalledOnDefault()
{
    std::optional<AdbRuntime::DefaultStall> stall;
    { auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); stall = runtime.defaultStall; }
    if (!stall) return {};
    const auto listener = ListenerIdentity(DefaultPort());
    return listener && *listener == stall->listener ? stall->serial : std::string{};
}
// The default serves the phone: a usable listing, except "device" for a
// phone whose adbd timed out on that very listener in this episode.
bool DefaultServesTarget(const std::vector<AdbDeviceEntry>& entries, const std::string& target)
{
    const auto stalled = StalledOnDefault();
    return std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
        return e.usb && (target.empty() || e.serial == target) &&
            ((e.state == PhoneAdbState::Device && (stalled.empty() || e.serial != stalled)) ||
             e.state == PhoneAdbState::Unauthorized || e.state == PhoneAdbState::Authorizing);
    });
}
// Only the stall memory keeps the default's "device" row for the phone from
// counting (that memory is set and cleared only while OkuFlow borrows the
// default). Before another program's adb is asked to exit or terminated,
// adbd is asked once more through that very listener (one bounded shell
// echo): if it answers, the stall is over, the memory is cleared and
// nothing is stopped; the next cycle hands back. Any reply within the budget
// is an answer, whatever its bytes (a shell may end the line with CRLF), as
// for the ordinary health probe; only a timeout or a failed socket is not.
// A "device" row whose shell stays silent still does not serve the phone.
bool StallEndedOnDefault(const std::vector<AdbDeviceEntry>& entries, const std::string& target,
                         const std::atomic<bool>* canceled)
{
    if (!ListsTargetUsable(entries, target) || DefaultServesTarget(entries, target)) return false;
    const auto stalled = StalledOnDefault();
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& e) {
        return e.usb && e.state == PhoneAdbState::Device && !stalled.empty() && e.serial == stalled;
    });
    if (entry == entries.end()) return false;
    bool answers = false;
    try {
        QTcpSocket shell;
        ConnectTo(shell, DefaultPort(), kHostConnectMs, canceled);
        AdbRequest(shell, entry->transportId ? "host:transport-id:" + QByteArray::number(entry->transportId)
                                             : "host:transport:" + QByteArray::fromStdString(entry->serial),
                   kUsbHealthProbeTimeoutMs, canceled);
        AdbRequest(shell, "shell:echo OKUFLOW_USB_HEALTH", kUsbHealthProbeTimeoutMs, canceled);
        Read(shell, 1, kUsbHealthProbeTimeoutMs, canceled); // Throws on a timeout or a closed socket.
        answers = true;
    } catch (const std::exception&) {
        CheckCanceled(canceled);
    }
    if (!answers) return false;
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    if (runtime.defaultStall && runtime.defaultStall->serial == stalled) runtime.defaultStall.reset();
    return true;
}
// Every stop outcome ends somewhere: gone, retried once, or named. A
// refusal or failure belongs to the episode of the decision that caused it:
// noted after a newer event, it is dropped (the new episode starts clean).
void NoteStopOutcome(DefaultAdbStopStatus status, const ListenerLookup& listener, std::uint64_t episode)
{
    using S = DefaultAdbStopStatus;
    if (status == S::Stopped || status == S::NoListener) { AfterDefaultStopped(); return; }
    if (status == S::HostKillIgnored || status == S::Canceled) return;
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    const bool recorded = status == S::AccessDenied || status == S::NotAdb || status == S::SelfListener
        ? runtime.doctor.NoteDefaultStopRefused(episode)
        : runtime.doctor.NoteDefaultStopFailed(episode); // IdentityChanged, Failed: one retry.
    if (recorded && runtime.doctor.DefaultStopRefused()) runtime.refusedListener = listener.identity;
    if (!recorded) TraceLine("stop outcome of an earlier episode dropped");
}
void NoteListenerUnopenable(const ListenerLookup& listener, std::uint64_t episode)
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    if (runtime.doctor.NoteDefaultStopRefused(episode)) runtime.refusedListener = listener.identity;
}

// A LIVE phone session uses the connection (registered and delivering
// frames), or a fixture reports one. A session that is only starting or
// reconnecting does not count: see PhoneSessionBlocksStop.
bool PhoneSessionLive()
{
    auto& runtime = Runtime();
    std::optional<PhoneAdbTestHooks> hooks;
    {
        std::lock_guard lock(runtime.mutex);
        if (PhoneSessionBlocksStop(!runtime.occupied.empty(), runtime.lastSessionFrameMs.load(), NowMs())) return true;
        hooks = runtime.hooks;
    }
    return hooks && hooks->phoneSessionLiveForTest && hooks->phoneSessionLiveForTest();
}

std::string ExecuteLinkAction(const PhoneLinkDecision& decision, const PhoneLinkObservation& o,
                              const AdbDeviceEntry* entry, const std::string& target,
                              const PhoneLinkRequest& link, const std::atomic<bool>* canceled)
{
    using A = PhoneLinkAction;
    auto& runtime = Runtime();
    PhoneLinkPolicy policy;
    { std::lock_guard lock(runtime.mutex); policy = runtime.doctor.Policy(); }
    // The user chose another source or target since this cycle began (a
    // webcam, a file, another phone, no phone): the request lost its
    // authority. Checked right before every side effect and again after
    // each blocking pre-check; a stale request does nothing.
    const auto stale = [&] { return link.IntentStale(); };
    const std::string dropped = "dropped: the source changed";
    const std::function<bool()> stillWanted = [&] { return !stale(); };
    // Trace texts are fixed reasons: adb error texts can carry the serial.
    switch (decision.action) {
    case A::None:
    case A::Relist:
    case A::WaitForUser:
        return {};
    case A::ReconnectTransport: {
        if (!entry) return "no listed phone";
        if (stale()) return dropped;
        if (entry->state == PhoneAdbState::Device) {
            auto descriptor = DescriptorFor(*entry);
            { std::lock_guard lock(runtime.mutex); descriptor.phoneServerEpoch = runtime.serverEpoch; }
            if (PhoneCameraTransport::RecoverStalledUsbTransport(descriptor, canceled, stillWanted))
                return "stalled transport reset";
            return stale() ? dropped : "stalled transport kept (proof steps not met)";
        }
        // Without an id the request reaches every offline device of that
        // server: only on OkuFlow's own private server, never another's.
        if (!entry->transportId && o.server != PhoneServerKind::Owned)
            return "kept: no transport id, and the server is not OkuFlow's own";
        const auto service = entry->transportId
            ? "host-transport-id:" + QByteArray::number(entry->transportId) + ":reconnect"
            : QByteArray("host:reconnect-offline");
        try {
            AdbHostReply(service, canceled);
            return std::string("reconnect requested for a ") + StateName(entry->state) + " transport";
        } catch (const std::exception&) {
            CheckCanceled(canceled);
            return "reconnect request failed";
        }
    }
    case A::HandBackToDefault: {
        // Under the lifecycle lock, as the transport's own hand-back: a
        // session start that asks for a server meanwhile waits for the
        // hand-back and then uses the default, instead of registering on a
        // private server that is stopped under it.
        std::unique_lock lifecycleLock(runtime.lifecycleMutex, std::defer_lock);
        while (!lifecycleLock.try_lock_for(std::chrono::milliseconds(20))) {
            CheckCanceled(canceled);
            if (runtime.shutdownRequested.load()) return "hand-back canceled: shutting down";
        }
        std::shared_ptr<OwnedAdbServer> owned;
        bool occupied = false;
        { std::lock_guard lock(runtime.mutex); owned = runtime.ownedServer; occupied = !runtime.occupied.empty(); }
        if (!owned) return "no private server to retire";
        if (occupied) return "hand-back deferred: a live session uses the private server";
        if (const auto hooks = Hooks(); hooks && hooks->afterHandBackCheckForTest) hooks->afterHandBackCheckForTest();
        CheckCanceled(canceled);
        if (stale()) return dropped;
        // A private server retired without ever answering (the hand-back of
        // a silent one) is a start that did not work: kept with the launch
        // failures (see the transport's yield).
        const int unanswered = owned->UnansweredStartsAtRetirement();
        owned->Stop();
        if (const auto hooks = Hooks(); hooks && hooks->afterPrivateServerRetiredForTest)
            hooks->afterPrivateServerRetiredForTest();
        {
            std::lock_guard lock(runtime.mutex);
            if (runtime.ownedServer == owned) runtime.ownedServer.reset();
            runtime.launchFailures += unanswered;
            runtime.shadowServer = runtime.shadowForBlindness = runtime.shadowRequested = false;
            runtime.shadowHandbackProbes = 0;
            // Like the transport's own hand-back: a default that hangs
            // again is met by a private server at once (the cap bounds it).
            runtime.nextServerStart = {};
            // Counted now that the private server is retired, not when the
            // doctor decided it (a live session defers it), and only in the
            // decision's episode.
            runtime.doctor.NoteHandOver(decision.episode);
        }
        return "the default server lists the phone; private server retired";
    }
    case A::RestartOwnedServer: {
        std::shared_ptr<OwnedAdbServer> owned;
        { std::lock_guard lock(runtime.mutex); owned = runtime.ownedServer; }
        // Beside a default the private server is restarted only when its own
        // listing fails (it answers its version, not its listings); a blind
        // private server beside the default is the takeover's business.
        if (!owned || (o.shadowRunning && o.serverProbe != PhoneServerProbe::Unresponsive))
            return "no owned server to restart";
        if (stale()) return dropped;
        const auto result = owned->Restart(canceled, stillWanted);
        using R = OwnedAdbServer::EnsureResult;
        if (result == R::Restarted || result == R::Launched) {
            return "owned server restarted on private port " + std::to_string(owned->Port().value_or(0));
        }
        if (result == R::Canceled && stale()) return dropped;
        if (result == R::RestartDeferredLiveSession) {
            // Nothing was restarted: the restart is given back (the spacing
            // stays as reserved), so a live session does not use up the
            // restarts whose end is the re-plug sentence.
            std::lock_guard lock(runtime.mutex);
            runtime.doctor.NoteRestartDeferred(decision.episode);
            return "owned restart deferred: a live session uses it";
        }
        if (result == R::Throttled) return "owned restart throttled";
        return "owned restart did not complete";
    }
    case A::StartShadowServer: {
        if (DefaultIsUserEndpoint()) return "refused: user endpoint";
        if (const auto hooks = Hooks(); hooks && hooks->beforeShadowStartForTest) hooks->beforeShadowStartForTest();
        if (stale()) return dropped;
        const auto identity = ListenerIdentity(DefaultPort());
        if (stale()) return dropped;
        {
            std::lock_guard lock(runtime.mutex);
            runtime.shadowRequested = true;
            runtime.nextServerStart = {};
            if (identity) runtime.defaultIdentity = identity;
        }
        const auto result = EnsureAdbServer(canceled, PhoneServerIntent::PhoneUse, target, stillWanted);
        using R = OwnedAdbServer::EnsureResult;
        if (result == R::Canceled && stale()) {
            // Dropped inside the server request: the start this decision
            // asked for does not outlive it.
            std::lock_guard lock(runtime.mutex);
            runtime.shadowRequested = false;
            return dropped;
        }
        if (result == R::LaunchFailed) {
            // OkuFlow's own adb.exe could not be started beside the default
            // (missing, quarantined, dying at once): counted toward the
            // AdbCannotStart sentence; the default answering does not reset
            // it (only OkuFlow's own server answering does).
            std::lock_guard lock(runtime.mutex);
            ++runtime.launchFailures;
            return "private server beside the default could not be started";
        }
        return result == R::Launched || result == R::Running || result == R::Restarted
            ? "private server started beside the default" : "private server beside the default is not ready yet";
    }
    case A::AskDefaultServerToExit: {
        // A live phone session (delivering frames): the server it uses is
        // never stopped. Which listener a session's sockets reach is not
        // recorded (OkuFlow's endpoint can change while a session runs, and
        // a session opened on the default before a private server started
        // keeps its sockets there), so any live session refuses the stop, as
        // an aborted one (bounded by abortedStopCap; a live session reads
        // Connected on the next cycle, which renews the budgets). A session
        // that is only starting or reconnecting does not refuse it: it is
        // registered for most of the time a manual pick or an automatic
        // reconnect retries against a hung default, and refusing for it
        // would name "another program" instead of stopping the hung server
        // the session is waiting for.
        const auto sessionRegistered = [] { return PhoneSessionLive(); };
        // An ask that sends no host:kill is not remembered as sent: no
        // termination follows it as if the request had been ignored, and
        // it does not use up one of the episode's asks.
        const auto notSent = [&](std::string reason) {
            std::lock_guard lock(runtime.mutex);
            runtime.doctor.NoteExitRequestNotSent(decision.episode);
            return reason;
        };
        // Dropped for a changed source: given back, not counted.
        const auto drop = [&] {
            std::lock_guard lock(runtime.mutex);
            runtime.doctor.NoteExitRequestDropped(decision.episode);
            return dropped;
        };
        if (DefaultIsUserEndpoint() || !o.phoneWanted) return notSent("refused: user endpoint or no phone intent");
        const auto port = DefaultPort();
        if (const auto hooks = Hooks(); hooks && hooks->beforeDefaultStopForTest) hooks->beforeDefaultStopForTest();
        if (stale()) return drop();
        if (sessionRegistered()) return notSent("kept: a live phone session uses the connection");
        // Like termination, asking to exit needs the Windows ADB interface
        // at execution time: a phone unplugged since the decision is no
        // reason to stop anything.
        if (RuntimeAdbInterface() != PhoneAdbInterface::Present) return notSent("kept: the Windows ADB interface is gone");
        // Before any byte: a fresh listing that succeeded, serves no other
        // device and still does not list the phone.
        const auto listed = ListDefaultEntries(canceled);
        if (stale()) return drop();
        // Every listing of the default feeds its listing hang.
        NoteDefaultListing(!listed, ListenerIdentity(port));
        if (!listed) return notSent("kept: the default server listing failed");
        if (OtherDeviceServed(*listed, target)) return notSent("kept: the default server serves another device");
        if (DefaultServesTarget(*listed, target)) return notSent("kept: the default server lists the phone now");
        if (StallEndedOnDefault(*listed, target, canceled))
            return notSent("kept: the phone answers on the default server now");
        if (stale()) return drop();
        const auto listener = LookupListener(port);
        if (listener.state == ListenerState::None) {
            AfterDefaultStopped();
            // Nothing was asked: the ask is given back, but nothing was kept
            // either, so it is not an aborted stop.
            std::lock_guard lock(runtime.mutex);
            runtime.doctor.NoteExitRequestNotNeeded(decision.episode);
            return "default server already gone";
        }
        if (listener.state == ListenerState::Unopenable) {
            NoteListenerUnopenable(listener, decision.episode);
            return notSent("refused: the listener process cannot be opened");
        }
        if (listener.state != ListenerState::Found) return notSent("kept: the listener table cannot be read");
        // Same PID and creation time, image adb.exe, not this process; the
        // raw host:kill re-checks the listener after connecting.
        DefaultAdbStopRequest request;
        request.port = port;
        request.expectedProcessId = listener.identity.processId;
        request.expectedCreation = listener.identity.created;
        request.terminateIfAlive = false;
        request.canceled = canceled;
        request.stillWanted = [&] { return !stale() && !sessionRegistered(); };
        if (const auto hooks = Hooks(); hooks && hooks->afterHostKillConnectForTest)
            request.afterHostKillConnectForTest = hooks->afterHostKillConnectForTest;
        request.hostKillWait = std::chrono::milliseconds(policy.hostKillWaitMs);
        if (stale()) return drop();
        if (sessionRegistered()) return notSent("kept: a live phone session uses the connection");
        { std::lock_guard lock(runtime.mutex); runtime.askedIdentity = listener.identity; }
        const auto result = TryStopDefaultAdbServer(request);
        if (!result.hostKillDelivered && stale()) return drop();
        if (!result.hostKillDelivered && result.status == DefaultAdbStopStatus::Canceled && sessionRegistered())
            return notSent("kept: a live phone session uses the connection");
        NoteStopOutcome(result.status, listener, decision.episode);
        if (result.status == DefaultAdbStopStatus::Stopped) return "default server exited after host:kill";
        if (!result.hostKillDelivered) return notSent("default server ask: " + result.diagnostic);
        return "default server ask: " + result.diagnostic;
    }
    case A::TerminateDefaultServer: {
        // As for the ask: a live phone session refuses the stop; a starting
        // or reconnecting one does not.
        const auto sessionRegistered = [] { return PhoneSessionLive(); };
        if (DefaultIsUserEndpoint() || !o.phoneWanted) return "refused: user endpoint or no phone intent";
        const auto port = DefaultPort();
        // Dropped for a changed source: the attempt is given back uncounted.
        const auto drop = [&] {
            std::lock_guard lock(runtime.mutex);
            runtime.doctor.NoteTerminationDropped(decision.episode);
            return dropped;
        };
        if (const auto hooks = Hooks(); hooks && hooks->beforeDefaultStopForTest) hooks->beforeDefaultStopForTest();
        if (stale()) return drop();
        // Termination needs the Windows ADB interface at execution time too:
        // a phone unplugged since the decision is no reason to stop anything.
        if (RuntimeAdbInterface() != PhoneAdbInterface::Present) return "kept: the Windows ADB interface is gone";
        // A stop aborted because the default answered its pre-stop listing
        // stopped nothing: it does not spend the termination cap (whose end
        // is the other-program sentence). It counts for the rung that decided
        // it: the takeover's termination follows an ask (hostKillAlreadySent),
        // the hung branch's does not.
        const auto aborted = [&](const char* reason) {
            std::lock_guard lock(runtime.mutex);
            runtime.doctor.NoteTerminationAborted(decision.episode, decision.hostKillAlreadySent);
            return std::string(reason);
        };
        if (sessionRegistered()) return aborted("kept: a live phone session uses the connection");
        if (o.serverProbe == PhoneServerProbe::Responsive || o.shadowRunning) {
            bool answeredFail = false;
            const auto listed = ListDefaultEntries(canceled, &answeredFail);
            if (stale()) return drop();
            const bool listingHung = NoteDefaultListing(!listed, ListenerIdentity(port));
            if (!listed) {
                // A FAIL is an answer: kept. A listing that stays silent for
                // the confirmation time is a hung default (as in discovery):
                // the stop goes on. Silent but not confirmed yet: kept, and
                // an aborted stop of this rung, so the three terminations
                // are not spent without one stop and the rung still ends.
                if (answeredFail) return aborted("kept: the default server answers a listing with FAIL");
                if (!listingHung) return aborted("kept: the default server listing failed");
            } else {
                if (OtherDeviceServed(*listed, target)) return aborted("kept: the default server serves another device");
                if (DefaultServesTarget(*listed, target)) return aborted("kept: the default server lists the phone now");
                if (StallEndedOnDefault(*listed, target, canceled))
                    return aborted("kept: the phone answers on the default server now");
            }
        } else {
            // A hang confirmed by version probes (or by listings that stayed
            // silent) alone: one short listing before anything else. A
            // server that answers it at all (a slow but healthy one, or one
            // that recovered since the second probe, also with an adb FAIL
            // reply) is never stopped; the hang must be confirmed again from
            // scratch.
            bool answeredFail = false;
            const auto listed = ListDefaultEntries(canceled, &answeredFail);
            if (stale()) return drop();
            if (listed || answeredFail) {
                const auto identity = ListenerIdentity(port);
                NoteDefaultProbe(OwnedServerProbe::Responsive, identity);
                // A listing that succeeded ends the listing hang; a FAIL is
                // still a failed listing (it stays confirmed, so an adb
                // whose listings keep failing reaches the aborted-stop end).
                if (listed) NoteDefaultListing(false, identity);
                const bool servesOther = listed && OtherDeviceServed(*listed, target);
                {
                    // Carried into later observations of this hung listener:
                    // it serves another device, so it ends in the sentence.
                    std::lock_guard lock(runtime.mutex);
                    if (servesOther && identity) runtime.hungServesOther = identity;
                    else runtime.hungServesOther.reset();
                }
                if (servesOther) return aborted("kept: the default server serves another device");
                // Kept whatever it lists; the reason uses the same test as
                // every other pre-check (a "device" row whose adbd timed out
                // there in this episode does not serve the phone).
                if (listed && DefaultServesTarget(*listed, target))
                    return aborted("kept: the default server lists the phone");
                return aborted(answeredFail ? "kept: the default server answers a listing with FAIL"
                                            : "kept: the default server answers a listing");
            }
        }
        if (stale()) return drop();
        const auto listener = LookupListener(port);
        if (listener.state == ListenerState::None) { AfterDefaultStopped(); return "default server already gone"; }
        if (listener.state == ListenerState::Unopenable) {
            NoteListenerUnopenable(listener, decision.episode);
            return "refused: the listener process cannot be opened";
        }
        if (listener.state != ListenerState::Found) return "kept: the listener table cannot be read";
        bool askedThisListener = false;
        {
            std::lock_guard lock(runtime.mutex);
            askedThisListener = runtime.askedIdentity && *runtime.askedIdentity == listener.identity;
            const auto expected = runtime.defaultIdentity;
            if (expected && !(*expected == listener.identity)) {
                runtime.defaultIdentity = listener.identity;
                if (runtime.doctor.NoteDefaultStopFailed(decision.episode) && runtime.doctor.DefaultStopRefused())
                    runtime.refusedListener = listener.identity;
                return "refused: the listener changed since the decision";
            }
        }
        DefaultAdbStopRequest request;
        request.port = port;
        request.expectedProcessId = listener.identity.processId;
        request.expectedCreation = listener.identity.created;
        // AskDefaultServerToExit already sent host:kill to this very
        // listener in this episode; any other listener is asked first.
        request.sendHostKillFirst = !(decision.hostKillAlreadySent && askedThisListener);
        request.canceled = canceled;
        request.stillWanted = [&] { return !stale() && !sessionRegistered(); };
        if (const auto hooks = Hooks(); hooks && hooks->afterHostKillConnectForTest)
            request.afterHostKillConnectForTest = hooks->afterHostKillConnectForTest;
        request.hostKillWait = std::chrono::milliseconds(policy.hostKillWaitMs);
        if (stale()) return drop();
        if (sessionRegistered()) return aborted("kept: a live phone session uses the connection");
        const auto result = TryStopDefaultAdbServer(request);
        if (!result.hostKillDelivered && !result.terminated && stale()) return drop();
        if (!result.terminated && result.status == DefaultAdbStopStatus::Canceled && sessionRegistered())
            return aborted("kept: a live phone session uses the connection");
        NoteStopOutcome(result.status, listener, decision.episode);
        return "default server stop: " + result.diagnostic;
    }
    case A::WakeAndLaunchApp: {
        if (!entry) return "no listed phone";
        if (const auto hooks = Hooks(); hooks && hooks->beforeCompanionLaunchForTest) hooks->beforeCompanionLaunchForTest();
        if (stale()) return dropped;
        try {
            const auto launched = LaunchCompanion(entry->serial, entry->transportId, canceled, true, stillWanted);
            if (launched == CompanionLaunch::Dropped) return dropped;
            return launched == CompanionLaunch::Started
                ? "woke the phone and requested the companion" : "companion is not installed";
        } catch (const std::exception&) {
            CheckCanceled(canceled);
            return "companion launch failed";
        }
    }
    }
    return {};
}

// A cycle whose request lost its authority (the user chose another source
// while it ran) observes a source the user left: it does not reach the
// doctor, so it neither decides nor reserves anything of the episode (a
// pick of the phone that was also an event must find its "at once" budgets
// untouched). The report keeps the last diagnosis and asks for the next
// cycle at once.
void DropStaleCycle(PhoneLinkReport& report)
{
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        report.decision.diagnosis = runtime.doctor.LastDiagnosis();
        report.decision.episode = runtime.doctor.Episode();
    }
    report.decision.action = PhoneLinkAction::None;
    report.decision.nextPollMs = NowMs();
    report.decision.diagnosisChanged = false;
    report.actionResult = "dropped before its decision: the source changed";
    TracePhoneLink("cycle dropped before its decision: the source changed");
}

void RunPhoneLinkLadder(const PhoneLinkRequest& link, const std::string& preferredSerial, const DiscoveryFacts& facts,
                        const std::atomic<bool>* canceled, PhoneLinkReport& report)
{
    if (link.IntentStale()) { DropStaleCycle(report); return; }
    PhoneLinkObservation o;
    o.phoneWanted = link.phoneWanted;
    o.captureAbandoned = link.captureAbandoned;
    o.sessionStreaming = link.sessionStreaming;
    o.endpointSupported = facts.endpointSupported;
    o.serverProbe = facts.probe;
    o.adbInterface = RuntimeAdbInterface();
    // A first automatic start with no saved phone wants an attached phone:
    // Windows shows the ADB interface, or cannot tell (Unknown: SetupAPI
    // failed) and the server lists a USB phone.
    if (link.phoneWantedIfAttached &&
        (o.adbInterface == PhoneAdbInterface::Present ||
         (o.adbInterface == PhoneAdbInterface::Unknown &&
          std::any_of(facts.entries.begin(), facts.entries.end(), [](const auto& e) { return e.usb; }))))
        o.phoneWanted = true;
    const auto endpoint = Endpoint();
    bool shadow = false, confirmed = false, refused = false;
    std::optional<AdbListenerIdentity> refusedListener;
    std::shared_ptr<OwnedAdbServer> ownedManager;
    // The listener on the default port now: a confirmation recorded for
    // another listener is not this one's.
    const auto currentDefault = ListenerIdentity(DefaultPort());
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        shadow = runtime.shadowServer;
        // Hung by its version probes, or by its listings while its version
        // answers, each confirmed for the listener that is there now (the
        // stop re-verifies the listener's identity again).
        confirmed = (runtime.borrowedConfirmed && runtime.borrowedFailureIdentity == currentDefault) ||
                    (runtime.listingConfirmed && runtime.listingFailureIdentity == currentDefault);
        refused = runtime.doctor.DefaultStopRefused();
        refusedListener = runtime.refusedListener;
        o.serverLaunchFailures = runtime.launchFailures;
        ownedManager = runtime.ownedServer;
    }
    // Starts of OkuFlow's own adb that stayed alive without ever answering
    // past their start grace, or died before they ever answered (security
    // software that lets adb.exe run but blocks its socket): named like
    // launches that failed outright; one that answers ends the count.
    if (ownedManager) o.serverLaunchFailures += ownedManager->UnansweredStarts();
    // The adb environment variables make the endpoint the user's, whatever
    // test hooks say: it is reported as an override, never taken over.
    o.server = !endpoint.supported ? PhoneServerKind::None
        : endpoint.ownedByThisProcess ? PhoneServerKind::Owned
        : endpoint.overridden || DefaultIsUserEndpoint() ? PhoneServerKind::Override
                                                         : PhoneServerKind::BorrowedDefault;
    o.shadowRunning = endpoint.ownedByThisProcess && shadow;
    // A refused stop outlives nothing: the listener is gone or another
    // process listens now.
    if (refused) {
        const auto listener = LookupListener(DefaultPort());
        const bool gone = listener.state == ListenerState::None;
        const bool changed = listener.state != ListenerState::Unknown && !gone && refusedListener &&
            (listener.identity.processId != refusedListener->processId ||
             (listener.identity.created && refusedListener->created &&
              listener.identity.created != refusedListener->created));
        if (gone || changed) {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            runtime.doctor.ClearDefaultStopRefused();
            runtime.refusedListener.reset();
        }
    }

    // Target: the phone of the live or chosen session, else the preferred
    // phone, else the only (or first ready) USB entry. Shell commands never
    // reach a phone the user did not choose when a choice exists.
    std::string target = !link.targetSerial.empty() ? link.targetSerial : preferredSerial;
    const AdbDeviceEntry* entry = nullptr;
    std::vector<const AdbDeviceEntry*> usb;
    for (const auto& candidate : facts.entries) if (candidate.usb) usb.push_back(&candidate);
    if (target.empty() && !usb.empty()) {
        const auto ready = std::find_if(usb.begin(), usb.end(), [](const auto* e) { return e->state == PhoneAdbState::Device; });
        target = (ready != usb.end() ? *ready : usb.front())->serial;
    }
    for (const auto* candidate : usb) if (candidate->serial == target) entry = candidate;
    o.phoneState = entry ? entry->state : PhoneAdbState::NotListed;
    // Blind means "lists no USB entry at all": another listed phone is not.
    if (!entry && !target.empty())
        o.otherUsbListed = std::any_of(usb.begin(), usb.end(), [&](const auto* e) { return e->serial != target; });
    if (o.server == PhoneServerKind::BorrowedDefault) o.defaultHasOtherDevices = OtherDeviceServed(facts.entries, target);

    // OkuFlow's own server cannot see the phone: look at the default port
    // too (a shadowed default, or another adb that appeared later, also one
    // restarted after host:kill). It may list the phone (hand back), serve
    // other devices (named), be blind (takeover rules) or hang (confirmed
    // here, from this cycle's probe, also after an event).
    const auto ownedPort = OwnedAdbServerPort();
    if (o.server == PhoneServerKind::Owned && o.phoneState == PhoneAdbState::NotListed && !DefaultIsUserEndpoint() &&
        (!ownedPort || *ownedPort != DefaultPort())) {
        const auto port = DefaultPort();
        const auto defaultProbe = ProbeAdbHostVersion(port, canceled);
        if (defaultProbe == OwnedServerProbe::Responsive) {
            const auto identity = ListenerIdentity(port);
            NoteDefaultProbe(defaultProbe, identity);
            {
                auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
                if (identity && runtime.besideIdentity && !(*runtime.besideIdentity == *identity))
                    runtime.doctor.NoteDefaultListenerChanged();
                if (identity) runtime.besideIdentity = identity;
            }
            o.shadowRunning = true;
            const auto listed = ListDefaultEntries(canceled);
            // Its version answers; a listing that stays silent for the
            // confirmation time (or keeps failing) is a hung default all the
            // same: the hung-default rules, as for failed version probes.
            const bool listingHung = NoteDefaultListing(!listed, identity);
            if (listed) {
                o.defaultListsPhone = ListsTargetUsable(*listed, target);
                o.defaultPhoneStalled = o.defaultListsPhone && !DefaultServesTarget(*listed, target);
                o.defaultHasOtherDevices = OtherDeviceServed(*listed, target);
            } else if (listingHung) {
                o.server = PhoneServerKind::BorrowedDefault;
                o.serverProbe = PhoneServerProbe::Unresponsive;
                o.shadowRunning = false;
                confirmed = true;
            }
        } else if (defaultProbe == OwnedServerProbe::Unresponsive) {
            // The private server runs but still cannot reach the phone: the
            // hung default keeps the USB interface.
            const auto listener = LookupListener(port);
            confirmed = NoteDefaultProbe(defaultProbe, listener.state == ListenerState::Found
                ? std::optional<AdbListenerIdentity>(listener.identity) : std::nullopt);
            o.server = PhoneServerKind::BorrowedDefault;
            o.serverProbe = PhoneServerProbe::Unresponsive;
            o.shadowRunning = false;
        } else {
            NoteDefaultProbe(defaultProbe, std::nullopt);
            if (shadow) AfterDefaultStopped();
            o.shadowRunning = false;
        }
    }
    if (o.server == PhoneServerKind::BorrowedDefault && o.serverProbe == PhoneServerProbe::Unresponsive) {
        o.defaultUnresponsiveConfirmed = confirmed;
        // Its listing answered earlier and showed another device: it serves
        // someone, also while its version probes stay slow.
        std::optional<AdbListenerIdentity> servesOther;
        bool relistDue = false;
        {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            servesOther = runtime.hungServesOther;
            relistDue = Clock::now() >= runtime.hungServesOtherRelistAt;
        }
        if (servesOther) {
            const auto listener = ListenerIdentity(DefaultPort());
            if (listener && *listener == *servesOther) {
                o.defaultHasOtherDevices = true;
                // That verdict rests on an earlier listing, and an emulator
                // or TCP device that goes away raises no Windows event: while
                // the doctor waits for the user, that listener is listed
                // again at the user-wait cadence. A listing without another
                // device ends the verdict; no answer keeps it.
                if (relistDue) {
                    {
                        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
                        runtime.hungServesOtherRelistAt =
                            Clock::now() + std::chrono::milliseconds(runtime.doctor.Policy().userWaitPollMs);
                    }
                    const auto listed = ListDefaultEntries(canceled);
                    if (listed && !OtherDeviceServed(*listed, target)) {
                        o.defaultHasOtherDevices = false;
                        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
                        if (runtime.hungServesOther && *runtime.hungServesOther == *servesOther)
                            runtime.hungServesOther.reset();
                    }
                }
            }
        }
    }

    if (entry && entry->state == PhoneAdbState::Device) {
        const auto companion = facts.companions.find(entry->serial);
        if (companion != facts.companions.end()) {
            o.companion = companion->second.first;
            o.problem = companion->second.second;
        }
        if (o.companion == PhoneCompanionProbe::Refused || o.companion == PhoneCompanionProbe::TimedOut) {
            bool timedOut = false;
            o.shell = QueryShellFacts(*entry, canceled, &timedOut);
            o.adbd = timedOut ? PhoneAdbdHealth::TimedOut : PhoneAdbdHealth::Answers;
        }
        // Another program's default lists the phone as ready while adbd is
        // silent for it: remembered for this listener, so that listing does
        // not count as serving the phone once OkuFlow's own server runs
        // beside it (no hand-back; host:kill and termination go on).
        if (o.server == PhoneServerKind::BorrowedDefault) {
            const bool stalled = o.adbd == PhoneAdbdHealth::TimedOut;
            const bool answered = o.adbd == PhoneAdbdHealth::Answers ||
                (o.companion != PhoneCompanionProbe::Unknown && o.companion != PhoneCompanionProbe::TimedOut);
            const auto listener = stalled ? ListenerIdentity(DefaultPort()) : std::nullopt;
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            if (stalled && listener) runtime.defaultStall = AdbRuntime::DefaultStall{entry->serial, *listener};
            else if (answered && runtime.defaultStall && runtime.defaultStall->serial == entry->serial)
                runtime.defaultStall.reset();
        }
    }
    if (o.phoneWanted && o.companion != PhoneCompanionProbe::Ready && !o.sessionStreaming) {
        // A leftover is stopped only when it plausibly holds the phone.
        bool graceOver = false;
        { auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); graceOver = runtime.doctor.BlindGracePassed(NowMs()); }
        const bool mayStop = o.phoneState == PhoneAdbState::NotListed &&
            o.adbInterface == PhoneAdbInterface::Present && graceOver;
        RuntimeSiblings(&o.otherInstanceRunning, &o.leftoverInstanceRunning, mayStop && !link.IntentStale(), canceled,
                        [&link] { return !link.IntentStale(); });
    }
    CheckCanceled(canceled);

    if (const auto hooks = Hooks(); hooks && hooks->linkStageForTest) hooks->linkStageForTest("decide");
    PhoneLinkDecision decision;
    bool staleNow = false;
    {
        // Checked under the doctor's lock: a source change the app makes
        // before it raises an event (a pick) is seen here, or the event is
        // applied after this decision (it then renews what was reserved).
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        staleNow = link.IntentStale();
        if (!staleNow) decision = runtime.doctor.Decide(o, NowMs());
    }
    if (staleNow) { report.observation = o; DropStaleCycle(report); return; }
    report.observation = o;
    report.decision = decision;
    report.targetSerial = target;
    if (decision.diagnosisChanged) {
        TracePhoneLink(std::string("diagnosis ") + std::string(PhoneLinkDiagnosisName(decision.diagnosis)) +
            " " + PhoneSerialTag(target) + " interface=" + InterfaceName(o.adbInterface) +
            " server=" + ServerKindName(o.server) + (o.shadowRunning ? "+shadow" : "") +
            " probe=" + ProbeName(o.serverProbe) + " state=" + StateName(o.phoneState) +
            " companion=" + CompanionName(o.companion) + (o.problem.empty() ? "" : ":" + o.problem) +
            " awake=" + FactName(o.shell.screenAwake) + " keyguard=" + FactName(o.shell.keyguardShowing) +
            " installed=" + FactName(o.shell.appInstalled) + " running=" + FactName(o.shell.appRunning) +
            (o.otherInstanceRunning ? " other-instance" : "") + (o.leftoverInstanceRunning ? " leftover-instance" : "") +
            (o.defaultHasOtherDevices ? " default-serves-others" : "") +
            (o.defaultPhoneStalled ? " default-lists-phone-stalled" : ""));
    }
    if (decision.action == PhoneLinkAction::None || decision.action == PhoneLinkAction::Relist ||
        decision.action == PhoneLinkAction::WaitForUser)
        return;
    report.actionResult = ExecuteLinkAction(decision, o, entry, target, link, canceled);
    TracePhoneLink(std::string("action ") + std::string(PhoneLinkActionName(decision.action)) + " " +
                   PhoneSerialTag(target) + " -> " + report.actionResult);
}
} // namespace

std::vector<CameraDescriptor> PhoneCameraTransport::Enumerate(
    const std::string& connectedSerial, const std::atomic<bool>* canceled, bool* listingSucceeded,
    const std::string& recoverySerial, PhoneDiscoveryStatus* status, PhoneServerIntent serverIntent,
    const PhoneLinkRequest* link, PhoneLinkReport* report)
{
    if (listingSucceeded) *listingSucceeded = false;
    if (status) *status = PhoneDiscoveryStatus::Unknown;
    const bool heal = link && report;
    std::vector<CameraDescriptor> devices;
    DiscoveryFacts facts;
    try {
        [&] {
            if (!Endpoint().supported) {
                facts.endpointSupported = false;
                if (status) *status = PhoneDiscoveryStatus::EndpointUnsupported;
                return;
            }
            std::function<bool()> stillWanted;
            if (link) stillWanted = [link] { return !link->IntentStale(); };
            const auto readiness = EnsureAdbServer(canceled, serverIntent,
                link && !link->targetSerial.empty() ? link->targetSerial : recoverySerial, stillWanted);
            using Result = OwnedAdbServer::EnsureResult;
            // Dropped for a source the user left: no listing this cycle.
            if (readiness == Result::Canceled && link && link->IntentStale()) return;
            ObserveServerIdentity();
            const bool ownServer = Endpoint().ownedByThisProcess;
            {
                // Repeated launch failures of OkuFlow's own adb are named.
                // Only OkuFlow's own server answering ends them: another
                // program's default answering (Running while borrowed) says
                // nothing about OkuFlow's adb.exe.
                auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
                if (readiness == Result::LaunchFailed) ++runtime.launchFailures;
                else if (readiness == Result::Launched || readiness == Result::Restarted ||
                         (readiness == Result::Running && ownServer))
                    runtime.launchFailures = 0;
            }
            if (readiness == Result::BorrowedUnresponsive) {
                facts.probe = PhoneServerProbe::Unresponsive;
                if (status) *status = PhoneDiscoveryStatus::ServerUnresponsive;
                return;
            }
            if (Endpoint().ownedByThisProcess && readiness != Result::Running &&
                readiness != Result::Launched && readiness != Result::Restarted) {
                facts.probe = PhoneServerProbe::NoListener;
                if (status && !(canceled && canceled->load())) *status = PhoneDiscoveryStatus::OwnedServerRecovering;
                return;
            }
            // The server answered its version probe; its device listing may
            // still fail (time-out, closed socket, invalid reply, FAIL to both
            // forms): an adb stuck in a USB transport answers host:version
            // without the transport lock its listings need. That is a server
            // that does not answer, never "still starting". The default's
            // listings feed its listing hang (a successful one ends it).
            const bool borrowedDefault = !ownServer && !DefaultIsUserEndpoint();
            std::vector<CameraDescriptor> attached;
            try {
                attached = ListUsbPhones(canceled, &facts.entries);
            } catch (const AdbServerUnavailable&) {
                throw; // Nothing accepts any more: classified below.
            } catch (const std::exception&) {
                CheckCanceled(canceled);
                facts.entries.clear();
                facts.probe = PhoneServerProbe::Unresponsive;
                if (borrowedDefault) NoteDefaultListing(true, ListenerIdentity(DefaultPort()));
                if (status) *status = ownServer ? PhoneDiscoveryStatus::OwnedServerRecovering
                                                : PhoneDiscoveryStatus::ServerUnresponsive;
                return;
            }
            if (borrowedDefault) NoteDefaultListing(false, std::nullopt);
            facts.probe = PhoneServerProbe::Responsive;
            if (listingSucceeded) *listingSucceeded = true;
            if (status) *status = PhoneDiscoveryStatus::Listed;
            {
                auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
                if (!attached.empty()) { runtime.shadowServer = false; runtime.shadowForBlindness = false; }
                else if (runtime.shadowServer && status) *status = PhoneDiscoveryStatus::OwnedServerRecovering;
            }
            for (const auto& descriptor : attached) {
                CheckCanceled(canceled);
                if (devices.size() >= 32) break;
                // Keep this identity even after lens discovery so a saved phone choice never falls back to a webcam.
                devices.push_back(descriptor);
                bool occupied = false;
                { auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); occupied = runtime.occupied.contains(descriptor.phoneSerial); }
                if (descriptor.phoneSerial == connectedSerial || occupied) {
                    devices.back().phoneAppReady = true;
                    continue; // Discovery must never bind another control host to the live phone.
                }
                auto& companion = facts.companions[descriptor.phoneSerial];
                try {
                    QTcpSocket capabilitiesSocket;
                    Transport(capabilitiesSocket, descriptor.phoneSerial, "localabstract:okuflow_camera", canceled,
                              kCompanionProbeTransportMs, descriptor.phoneTransportId);
                    const auto capabilities = ControlHello(capabilitiesSocket, 1500, false, canceled, true);
                    devices.back().phoneBusy = capabilities["busy"].toBool();
                    devices.back().phoneProblem = capabilities["problem"].toString().toStdString();
                    devices.back().phoneAppReady = !devices.back().phoneBusy && devices.back().phoneProblem.empty();
                    companion = {devices.back().phoneBusy ? PhoneCompanionProbe::SessionBusy
                                 : devices.back().phoneProblem.empty() ? PhoneCompanionProbe::Ready
                                                                       : PhoneCompanionProbe::Problem,
                                 devices.back().phoneProblem};
                    const auto discoveredPhone = devices.back();
                    const auto cameras = capabilities["cameras"].toArray();
                    if (!cameras.empty()) {
                        for (const auto& camera : cameras) {
                            if (devices.size() >= 32) break;
                            const auto id = camera.toObject()["id"].toString();
                            if (id.isEmpty() || id.size() > 128) continue;
                            auto lens = discoveredPhone;
                            lens.phoneCameraId = id.toStdString();
                            const auto role = camera.toObject()["role"].toString();
                            if (role == "auto" || role == "ultrawide" || role == "main" || role == "telephoto" || role == "front")
                                lens.phoneLensRole = role.toStdString();
                            lens.symbolicLink += L"/" + id.toStdWString();
                            lens.name += L" · " + id.toStdWString();
                            devices.push_back(std::move(lens));
                        }
                        if (!devices.empty() && devices.back().phoneSerial == descriptor.phoneSerial) continue;
                    }
                } catch (const PhoneProtocolError& error) {
                    auto& phone = devices.back(); phone.phoneProblem = error.code;
                    phone.phoneBusy = error.code == "sessionBusy" || error.code == "busy";
                    companion = {error.code == "sessionBusy" ? PhoneCompanionProbe::SessionBusy : PhoneCompanionProbe::Problem,
                                 error.code};
                } catch (const UsbResponseTimeout&) {
                    companion = {PhoneCompanionProbe::TimedOut, {}};
                    // A timed-out probe is the only discovery evidence consistent
                    // with a stalled transport, and only the preferred idle phone
                    // may act on it. Ordinary device browsing is read-only. With
                    // the doctor engaged, its ladder decides instead.
                    if (!heal && descriptor.phoneSerial == recoverySerial)
                        RecoverStalledUsbTransport(descriptor, canceled);
                } catch (...) {
                    // A prompt refusal (app closed/paused, listener absent) proves
                    // adbd answered: no stall evidence and no recovery shell work.
                    // App absent: retain the saved-camera entry for later startup.
                    companion = {PhoneCompanionProbe::Refused, {}};
                }
            }
        }();
    } catch (const UsbResponseTimeout&) {
        facts.probe = PhoneServerProbe::Unresponsive;
        if (status && !(canceled && canceled->load()))
            *status = Endpoint().ownedByThisProcess ? PhoneDiscoveryStatus::OwnedServerRecovering
                                                   : PhoneDiscoveryStatus::ServerUnresponsive;
    } catch (const AdbConnectTimedOut&) {
        // A listener that does not accept in time hangs; it is not absent.
        facts.probe = PhoneServerProbe::Unresponsive;
        if (status && !(canceled && canceled->load()))
            *status = Endpoint().ownedByThisProcess ? PhoneDiscoveryStatus::OwnedServerRecovering
                                                   : PhoneDiscoveryStatus::ServerUnresponsive;
    } catch (const AdbServerUnavailable&) {
        // Refused, but a listener in the table that does not accept (full
        // accept queue of a frozen adb) hangs too.
        const auto listener = LookupListener(Endpoint().port, true).state;
        const bool present = listener == ListenerState::Found || listener == ListenerState::Unopenable;
        facts.probe = present ? PhoneServerProbe::Unresponsive : PhoneServerProbe::NoListener;
        if (status && !(canceled && canceled->load()))
            *status = Endpoint().ownedByThisProcess ? PhoneDiscoveryStatus::OwnedServerRecovering
                    : present ? PhoneDiscoveryStatus::ServerUnresponsive : PhoneDiscoveryStatus::NoListener;
    } catch (...) { /* Optional companion discovery never makes webcam enumeration fail. */ }
    if (heal && !(canceled && canceled->load())) {
        try { RunPhoneLinkLadder(*link, recoverySerial, facts, canceled, *report); }
        catch (const std::exception&) {
            if (!(canceled && canceled->load())) TracePhoneLink("ladder interrupted");
        }
    }
    return devices;
}

std::pair<float, float> ResolvePhoneZoomRange(const QJsonObject& capabilities, const std::string& selectedCamera)
{
    const auto effectiveId = selectedCamera.empty() ? capabilities["automaticCamera"].toString() : QString::fromStdString(selectedCamera);
    for (const auto& value : capabilities["cameras"].toArray()) {
        const auto camera = value.toObject();
        if (effectiveId.isEmpty() || camera["id"].toString() != effectiveId) continue;
        const double low = camera["zoomMin"].toDouble(1), high = camera["zoomMax"].toDouble(1);
        if (std::isfinite(low) && std::isfinite(high) && low >= 0.01 && high >= low && high <= 1000)
            return {float(low), float(high)};
    }
    return {1.0f, 1.0f};
}

PhoneCameraTransport::~PhoneCameraTransport() { Stop(); }
bool PhoneCameraTransport::Start(const CameraDescriptor& descriptor, IMFDXGIDeviceManager* manager,
                                 FrameCallback callback, CaptureErrorCallback errorCallback,
                                 const std::atomic<bool>* startupCanceled)
{
    ComPtr<IMFDXGIDeviceManager> retained = manager;
    const PhoneStartupBudget startupBudget(Clock::now());
    worker_ = std::thread([this, descriptor, retained, callback = std::move(callback), errorCallback = std::move(errorCallback), startupBudget] {
        Run(descriptor, retained, callback, errorCallback, startupBudget);
    });
    std::unique_lock lock(mutex_);
    const auto deadline = startupBudget.Deadline();
    while (!opened_ && !failed_ && !canceled_.load()) {
        if ((startupCanceled && startupCanceled->load()) || Clock::now() >= deadline) {
            error_ = startupCanceled && startupCanceled->load() ? "Phone camera canceled" :
                "Phone camera startup timed out. Open OkuFlow Camera and authorize USB debugging.";
            canceled_.store(true);
            return false;
        }
        ready_.wait_for(lock, std::chrono::milliseconds(20));
    }
    return opened_ && !failed_ && !canceled_.load();
}
void PhoneCameraTransport::Cancel() { canceled_.store(true); RetirePendingTorch(); ready_.notify_all(); }
void PhoneCameraTransport::Stop() { Cancel(); if (worker_.joinable()) worker_.join(); }
void PhoneCameraTransport::SetZoom(float ratio)
{
    if (!std::isfinite(ratio) || ratio <= 0) return;
    std::lock_guard lock(mutex_); desiredZoom_ = pendingZoom_ = ratio;
}
std::pair<float, float> PhoneCameraTransport::ZoomRange() const
{ std::lock_guard lock(mutex_); return zoomRange_; }
void PhoneCameraTransport::SetCamera(const std::string& id)
{ if (id.size() <= 128) { std::lock_guard lock(mutex_); pendingCamera_ = id; } }
void PhoneCameraTransport::SetTorch(bool enabled)
{ (void)QueueTorch(enabled, {}, 0); }
bool PhoneCameraTransport::SetTorch(bool enabled, std::shared_ptr<std::atomic<bool>> canceled,
                                   std::int64_t deadlineSteadyMs)
{
    if (!canceled || deadlineSteadyMs <= 0) return false;
    return QueueTorch(enabled, std::move(canceled), deadlineSteadyMs);
}
bool PhoneCameraTransport::QueueTorch(bool enabled, std::shared_ptr<std::atomic<bool>> canceled,
                                     std::int64_t deadlineSteadyMs)
{
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (canceled_.load() || failed_ || (canceled && canceled->load()) || (deadlineSteadyMs && now >= deadlineSteadyMs)) return false;
    if (torchCancellation_ && torchCancellation_ != canceled) torchCancellation_->store(true);
    torchCancellation_ = canceled;
    const auto generation = ++torchGeneration_;
    pendingTorch_ = PendingTorchCommand{enabled, std::move(canceled), deadlineSteadyMs, generation};
    return true;
}
void PhoneCameraTransport::RetirePendingTorch()
{
    std::lock_guard lock(mutex_);
    if (torchCancellation_) torchCancellation_->store(true);
    torchCancellation_.reset(); pendingTorch_.reset(); ++torchGeneration_;
}
bool PhoneCameraTransport::SendTorchCommand(QTcpSocket& socket, const PendingTorchCommand& command)
{
    return Command(socket, {{"op", "torch"}, {"enabled", command.enabled}}, &canceled_, [&] {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        return !canceled_.load() && torchGeneration_.load() == command.generation &&
            (!command.canceled || !command.canceled->load()) &&
            (!command.deadlineSteadyMs || now < command.deadlineSteadyMs);
    });
}
bool PhoneCameraTransport::SendPendingTorch(QTcpSocket& socket)
{
    std::optional<PendingTorchCommand> command;
    { std::lock_guard lock(mutex_); command.swap(pendingTorch_); }
    return command && SendTorchCommand(socket, *command);
}
void PhoneCameraTransport::ApplyTorchAcknowledgment(const QJsonObject& status)
{
    torchAvailable_.store(status["torchAvailable"].toBool(false));
    torchEnabled_.store(status["torchEnabled"].toBool(false));
}
VideoFormat PhoneCameraTransport::Format() const { std::lock_guard lock(mutex_); return format_; }
std::string PhoneCameraTransport::Error() const { std::lock_guard lock(mutex_); return error_; }
std::string PhoneCameraTransport::ProblemCode() const { std::lock_guard lock(mutex_); return problemCode_; }

namespace {
// The fixed reason a phone session ended, for the trace: exception texts can
// carry a serial (adb FAIL replies), so only known messages of this file and
// the companion's own problem codes (letters only) are named.
std::string SessionEndReason(const std::exception& exception, bool canceled)
{
    if (canceled) return "stopped by OkuFlow";
    if (const auto* protocol = dynamic_cast<const PhoneProtocolError*>(&exception)) {
        const auto& code = protocol->code;
        const bool plain = !code.empty() && code.size() <= 24 &&
            std::all_of(code.begin(), code.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
        return std::string("the phone reported ") + (plain ? code : "a problem");
    }
    if (dynamic_cast<const HevcDecoderError*>(&exception)) return "the HEVC decoder failed";
    if (dynamic_cast<const UsbResponseTimeout*>(&exception)) return "the phone stopped answering";
    if (dynamic_cast<const AdbServerUnavailable*>(&exception) || dynamic_cast<const AdbConnectTimedOut*>(&exception))
        return "the adb server is unavailable";
    if (dynamic_cast<const AdbFailReply*>(&exception)) return "the adb server refused a request";
    const std::string_view text = exception.what();
    for (const char* known : {"Phone camera stopped delivering frames", "Phone USB connection lost",
                              "Phone USB connection closed", "The selected phone is listed by adb but is not ready",
                              "The selected phone is not connected and authorized over USB",
                              "Open OkuFlow Camera, allow camera access and authorize USB debugging",
                              "Install OkuFlow Camera on the selected USB phone first",
                              "The camera connection was lost. OkuFlow is reconnecting automatically.",
                              "Phone negotiated an unsupported stream format", "Missing or reordered phone packet"})
        if (text == known) return known;
    return "another failure";
}
} // namespace

void PhoneCameraTransport::Run(CameraDescriptor descriptor, ComPtr<IMFDXGIDeviceManager> manager,
                               FrameCallback callback, CaptureErrorCallback errorCallback, PhoneStartupBudget startupBudget)
{
    std::unique_ptr<OccupiedPhone> occupied;
    bool started = false;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Check(apartment, "Initialize phone decoder COM");
        // Repair an idle owned server before this new session counts itself
        // as occupied. Other live/starting sessions remain protected.
        RequireAdbServer(&canceled_, descriptor.phoneSerial);
        occupied = std::make_unique<OccupiedPhone>(descriptor.phoneSerial);
        const LONGLONG sessionClock = MFGetSystemTime();
        LONGLONG lastDecodedClock = -1;
        std::uint64_t outputSequence = 0;
        const auto stream = [&](bool hevcSupported) {
            // New/retried connections never replay an earlier light-on request.
            RetirePendingTorch();
            torchAvailable_.store(false);
            torchEnabled_.store(false);
            // Revalidate positive USB evidence immediately before selecting a
            // transport. The bare listing supplies the current transport ID
            // without probing unrelated phones on every start/HEVC retry.
            RequireAdbServer(&canceled_, descriptor.phoneSerial);
            // A phone that is listed but not ready yet (offline after resume,
            // authorizing, connecting) gets a short bounded wait inside the
            // startup budget instead of failing a manual pick at once.
            std::vector<CameraDescriptor> attached;
            std::vector<AdbDeviceEntry> entries;
            const auto stateDeadline = std::min(Clock::now() + std::chrono::seconds(4),
                                                startupBudget.Deadline() - std::chrono::seconds(2));
            auto selected = attached.end();
            for (;;) {
                attached = ListUsbPhones(&canceled_, &entries);
                selected = std::find_if(attached.begin(), attached.end(), [&](const auto& item) { return item.phoneSerial == descriptor.phoneSerial; });
                if (selected != attached.end()) break;
                const bool listed = std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                    return entry.usb && entry.serial == descriptor.phoneSerial;
                });
                if (!listed || Clock::now() >= stateDeadline)
                    throw std::runtime_error(listed ? "The selected phone is listed by adb but is not ready"
                                                    : "The selected phone is not connected and authorized over USB");
                for (int i = 0; i < 12; ++i) { CheckCanceled(&canceled_); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            }
            descriptor.phoneTransportId = selected->phoneTransportId;
            descriptor.phoneServerEpoch = selected->phoneServerEpoch;
            QTcpSocket control, video;
            QJsonObject capabilities;
            const auto connectionDeadline = Clock::now() + std::chrono::seconds(5);
            bool launchedThisAttempt = false;
            for (;;) {
                CheckCanceled(&canceled_);
                bool endpointOpened = false;
                try {
                    Transport(control, descriptor.phoneSerial, "localabstract:okuflow_camera", &canceled_, 1000, descriptor.phoneTransportId);
                    endpointOpened = true;
                    capabilities = ControlHello(control, 1500, hevcSupported, &canceled_);
                    Transport(video, descriptor.phoneSerial, "localabstract:okuflow_camera", &canceled_, 1000, descriptor.phoneTransportId);
                    Hello(video, 2, {{"token", capabilities["token"]}}, &canceled_);
                    break;
                } catch (const PhoneProtocolError& error) {
                    control.abort(); video.abort();
                    if (error.code != "sessionBusy" || Clock::now() >= connectionDeadline) throw;
                } catch (const std::exception&) {
                    control.abort(); video.abort(); CheckCanceled(&canceled_);
                    // An absent listener gets at most one launch per start
                    // attempt, taken from the shared per-event budget.
                    if (!endpointOpened && !launchedThisAttempt && MayLaunch()) {
                        launchedThisAttempt = true;
                        TracePhoneLink("session start: woke the phone and requested the companion " +
                                       PhoneSerialTag(descriptor.phoneSerial));
                        // Wake first: a launch on a sleeping phone would otherwise be spent
                        // behind the keyguard. A paused/blocked installed app keeps its
                        // own state; the legacy package is only tried when the Play
                        // activity is absent.
                        if (LaunchCompanion(descriptor.phoneSerial, descriptor.phoneTransportId, &canceled_, true) ==
                            CompanionLaunch::NotInstalled)
                            throw std::runtime_error("Install OkuFlow Camera on the selected USB phone first");
                    }
                    if (Clock::now() >= connectionDeadline)
                        throw std::runtime_error("Open OkuFlow Camera, allow camera access and authorize USB debugging");
                }
                for (int i = 0; i < 5; ++i) { CheckCanceled(&canceled_); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            }
            const auto bound = ReadPacket(video, kControlLimit, &canceled_);
            if (bound.type != 1 || bound.flags || Json(bound)["channel"].toString() != "video") throw std::runtime_error("Phone video binding failed");
            const auto automaticId = capabilities["automaticCameraId"].toString();
            const auto selection = descriptor.phoneCameraId.empty() ? automaticId : QString::fromStdString(descriptor.phoneCameraId);
            if (!selection.isEmpty()) Command(control, {{"op", "camera"}, {"cameraId", selection}}, &canceled_);
            const auto range = ResolvePhoneZoomRange(capabilities, descriptor.phoneCameraId);
            { std::lock_guard lock(mutex_); zoomRange_ = range; }
            Command(control, {{"op", "torch"}, {"enabled", false}}, &canceled_);
            const auto now = Clock::now();
            // Initial HEVC->AVC retry shares Start's original deadline. A
            // running stream's later renegotiation gets its normal fresh budget.
            const int cameraBudgetMs = (lastDecodedClock < 0 ? startupBudget : PhoneStartupBudget(now)).RemainingCameraMs(now);
            if (!cameraBudgetMs) throw std::runtime_error("Phone camera startup budget exhausted during USB negotiation");
            Command(control, {{"op", "start"}, {"startupBudgetMs", cameraBudgetMs}}, &canceled_);
            { std::lock_guard lock(mutex_); pendingZoom_ = desiredZoom_; }
            std::map<quint32, QJsonObject> modes;
            std::unique_ptr<PhoneVideoDecoder> decoder;
            PhoneVideoCodec decoderCodec = PhoneVideoCodec::Avc;
            PhoneFrameProgress progress(Clock::now());
            quint32 decoderGeneration = 0;
            qint64 previousSequence = -1, firstTimestamp = -1, previousTimestamp = -1;
            auto lastHeartbeat = Clock::now();
            const auto checkProgress = [&] {
                const auto failure = progress.Check(Clock::now());
                if (failure == PhoneFrameProgress::Failure::HevcStalled)
                    throw HevcDecoderError("HEVC decoder stopped producing frames");
                if (failure != PhoneFrameProgress::Failure::None)
                    throw std::runtime_error("Phone camera stopped delivering frames");
            };
            LONGLONG generationClock = 0;
            UINT fps = 30;
            std::optional<Packet> pending;
            while (!canceled_.load()) {
                checkProgress();
                while (control.bytesAvailable() || control.waitForReadyRead(1)) {
                    const auto packet = ReadPacket(control, kControlLimit, &canceled_);
                    if (packet.flags || (packet.type != 3 && packet.type != 4 && packet.type != 5)) throw std::runtime_error("Invalid phone control message");
                    const auto json = Json(packet);
                    if (packet.type == 5) throw PhoneProtocolError(json["code"].toString().toStdString(), json["error"].toString().toStdString());
                    if (packet.type == 4) {
                        ApplyTorchAcknowledgment(json);
                    }
                    if (packet.type == 3 && json["event"].toString() == "retrying") {
                        qInfo() << "Phone startup candidate" << json["attempt"].toInt() << "/" << json["candidates"].toInt()
                                << json["mime"].toString() << json["width"].toInt() << "x" << json["height"].toInt()
                                << "rejected:" << json["reason"].toString() << "remaining ms:" << json["remainingMs"].toInt();
                    }
                    if (packet.type == 3 && json["event"].toString() == "configured") {
                        const UINT width = json["width"].toInt(), height = json["height"].toInt();
                        fps = json["fps"].toInt();
                        const auto mime = json["mime"].toString();
                        if (!width || !height || width > 4096 || height > 2304 || !fps || fps > 60 ||
                            (mime != "video/avc" && !(hevcSupported && mime == "video/hevc")))
                            throw std::runtime_error("Phone negotiated an unsupported stream format");
                        qInfo() << "Phone negotiated" << mime << width << "x" << height << "@" << fps << "FPS";
                        const quint32 generation = static_cast<quint32>(json["generation"].toInt());
                        modes[generation] = json;
                        while (modes.size() > 4) modes.erase(modes.begin());
                        {
                            std::lock_guard lock(mutex_);
                            format_ = {MFVideoFormat_NV12, width, height, fps, 1, L"okuflow-phone-auto"};
                        }
                    }
                }
                if (Clock::now() - lastHeartbeat > std::chrono::seconds(5)) {
                    Command(control, {{"op", "ping"}}, &canceled_);
                    lastHeartbeat = Clock::now();
                }
                float zoom = -1;
                std::string camera;
                { std::lock_guard lock(mutex_); std::swap(zoom, pendingZoom_); camera.swap(pendingCamera_); }
                if (zoom > 0) Command(control, {{"op", "zoom"}, {"ratio", std::clamp(zoom, range.first, range.second)}}, &canceled_);
                if (!camera.empty()) Command(control, {{"op", "camera"}, {"cameraId", QString::fromStdString(camera)}}, &canceled_);
                SendPendingTorch(control);
                if (control.state() != QAbstractSocket::ConnectedState || video.state() != QAbstractSocket::ConnectedState)
                    throw std::runtime_error("Phone USB connection lost");
                if (!pending && !video.bytesAvailable() && !video.waitForReadyRead(10)) {
                    checkProgress();
                    continue;
                }
                // Configure is sent before codec output; allow the independent control channel to catch up.
                if (modes.empty()) continue;
                auto packet = pending ? std::move(*pending) : ReadPacket(video, kVideoLimit, &canceled_);
                pending.reset();
                if (packet.type != 6 || packet.flags & ~3 || packet.data.size() <= 20) throw std::runtime_error("Invalid phone video packet");
                const qint64 timestamp = qFromLittleEndian<qint64>(packet.data.constData());
                const qint64 sequence = qFromLittleEndian<qint64>(packet.data.constData() + 8);
                const quint32 generation = qFromLittleEndian<quint32>(packet.data.constData() + 16);
                if (!modes.count(generation)) { pending = std::move(packet); continue; }
                if (!decoder || generation != decoderGeneration) {
                    const auto mode = modes.at(generation);
                    fps = mode["fps"].toInt();
                    decoderCodec = mode["mime"].toString() == "video/hevc" ? PhoneVideoCodec::Hevc : PhoneVideoCodec::Avc;
                    try {
                        decoder = std::make_unique<PhoneVideoDecoder>(mode["width"].toInt(), mode["height"].toInt(),
                            fps, decoderCodec, manager.Get());
                    } catch (const std::exception& exception) {
                        if (decoderCodec == PhoneVideoCodec::Hevc) throw HevcDecoderError(exception.what());
                        throw;
                    }
                    progress.BeginGeneration(decoderCodec == PhoneVideoCodec::Hevc, Clock::now());
                    decoderGeneration = generation;
                    previousSequence = -1; firstTimestamp = previousTimestamp = -1;
                    if (lastDecodedClock == std::numeric_limits<LONGLONG>::max()) throw std::runtime_error("Phone timestamp overflow");
                    generationClock = std::max(lastDecodedClock + 1, MFGetSystemTime() - sessionClock);
                    if (generationClock < 0) throw std::runtime_error("Invalid phone session clock");
                }
                if (sequence != previousSequence + 1) throw std::runtime_error("Missing or reordered phone packet");
                previousSequence = sequence;
                const auto data = packet.data.mid(20);
                if (!data.startsWith(QByteArray("\0\0\1", 3)) && !data.startsWith(QByteArray("\0\0\0\1", 4))) throw std::runtime_error("Phone video is not Annex B");
                if (!(packet.flags & 2)) {
                    progress.EncodedFrameReceived(Clock::now());
                    if (timestamp < 0 || timestamp <= previousTimestamp) throw std::runtime_error("Invalid phone frame timestamp");
                    previousTimestamp = timestamp;
                    if (firstTimestamp < 0) firstTimestamp = timestamp;
                    if (timestamp - firstTimestamp > (std::numeric_limits<LONGLONG>::max() - generationClock) / 10) throw std::runtime_error("Phone timestamp overflow");
                }
                const auto normalized = (packet.flags & 2) ? generationClock : generationClock + (timestamp - firstTimestamp) * 10;
                progress.PacketReceived(Clock::now());
                try {
                    decoder->Decode(std::span(reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<size_t>(data.size())),
                    normalized, [&](MediaFrame&& frame) {
                        progress.FrameDecoded(Clock::now());
                        Runtime().lastSessionFrameMs.store(NowMs());
                        lastDecodedClock = frame.captureTimestamp100ns;
                        frame.sequenceNumber = outputSequence++;
                        if (!started) {
                            started = true;
                            TracePhoneLink("session started " + PhoneSerialTag(descriptor.phoneSerial));
                        }
                        { std::lock_guard lock(mutex_); opened_ = true; }
                        ready_.notify_all();
                        if (callback) callback(std::move(frame));
                    });
                } catch (const std::exception& exception) {
                    if (decoderCodec == PhoneVideoCodec::Hevc) throw HevcDecoderError(exception.what());
                    throw;
                }
            }
        };
        bool hevcSupported = PhoneVideoDecoder::IsSupported(PhoneVideoCodec::Hevc, manager.Get());
        for (;;) {
            try { stream(hevcSupported); break; }
            catch (const HevcDecoderError& exception) {
                if (!hevcSupported || canceled_.load()) throw;
                hevcSupported = false;
                qWarning() << "Phone HEVC decode failed; reconnecting once with H.264:" << exception.what();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        }
        // The stream loop ends without an error only when OkuFlow stops it.
        TracePhoneLink(std::string(started ? "session ended " : "session start failed ") +
                       PhoneSerialTag(descriptor.phoneSerial) + ": stopped by OkuFlow");
    } catch (const std::exception& exception) {
        TracePhoneLink(std::string(started ? "session ended " : "session start failed ") +
                       PhoneSerialTag(descriptor.phoneSerial) + ": " + SessionEndReason(exception, canceled_.load()));
        // Sockets/decoder have left scope before recovery. Coded camera errors
        // preserve pause, permission, thermal and other-host ownership states.
        if (!canceled_.load() && !dynamic_cast<const PhoneProtocolError*>(&exception))
            RecoverStalledUsbTransport(descriptor, &canceled_);
        {
            std::lock_guard lock(mutex_);
            failed_ = true;
            error_ = exception.what();
            if (const auto* protocol = dynamic_cast<const PhoneProtocolError*>(&exception)) problemCode_ = protocol->code;
        }
        ready_.notify_all();
        if (!canceled_.load() && errorCallback) { try { errorCallback(exception.what()); } catch (...) {} }
    }
    RetirePendingTorch();
    if (SUCCEEDED(apartment)) CoUninitialize();
}
} // namespace okuflow
#endif
