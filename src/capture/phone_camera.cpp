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
#include <codecapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <iphlpapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <charconv>
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
struct AdbServerUnavailable : std::runtime_error { using std::runtime_error::runtime_error; };
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
    std::map<std::string, std::uint64_t> launched;
    std::map<std::pair<std::uint16_t, std::string>, Clock::time_point> nextUsbRecovery;
    // Suppresses repeated shell health probes after a definitive healthy or
    // other-host/phone-state answer. Separate from the reconnect-send slot so
    // a healthy answer does not consume the one-minute recovery budget.
    std::map<std::pair<std::uint16_t, std::string>, Clock::time_point> nextUsbHealthProbe;
    std::map<std::string, unsigned> occupied;
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
std::optional<AdbListenerIdentity> ListenerIdentity(std::uint16_t port)
{
    if (!port) return std::nullopt;
    ULONG bytes = 0;
    if (GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)
        != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;
    std::vector<std::uint8_t> buffer;
    ULONG result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        buffer.resize(bytes);
        result = GetExtendedTcpTable(buffer.data(), &bytes, FALSE, AF_INET,
                                     TCP_TABLE_OWNER_PID_LISTENER, 0);
    }
    if (result != NO_ERROR) return std::nullopt;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) != port ||
            ntohl(row.dwLocalAddr) != INADDR_LOOPBACK) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, row.dwOwningPid);
        if (!process) return std::nullopt;
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool valid = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                           WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        if (valid) return AdbListenerIdentity{port, row.dwOwningPid,
            (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
    }
    return std::nullopt;
}
void ObserveServerIdentity()
{
    const auto identity = ListenerIdentity(Endpoint().port);
    if (!identity) return;
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    if (runtime.lastIdentity == identity) return;
    runtime.lastIdentity = identity;
    ++runtime.serverEpoch;
    runtime.launched.clear();
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
bool MayLaunch(const std::string& serial, std::uint64_t transport) {
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    const auto previous = runtime.launched.find(serial);
    if (previous != runtime.launched.end() && previous->second == transport) return false;
    runtime.launched[serial] = transport;
    return true;
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
void Connect(QTcpSocket& socket, int timeout)
{
    socket.setReadBufferSize(32 * 1024 * 1024);
    const auto endpoint = Endpoint();
    if (!endpoint.supported) throw std::runtime_error("USB phone support requires a local TCP adb server");
    if (endpoint.port == 0) throw AdbServerUnavailable("The owned adb server is unavailable");
    socket.connectToHost(QHostAddress::LocalHost, endpoint.port);
    if (!socket.waitForConnected(timeout)) throw AdbServerUnavailable("The local adb server is unavailable");
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
// listener at all.
OwnedServerProbe ProbeAdbHostVersion(std::uint16_t port, const std::atomic<bool>* canceled)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(100)) return OwnedServerProbe::NoListener;
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
    const QString& executable, std::vector<std::wstring> fixtureArguments = {})
{
    OwnedAdbServerPolicy policy;
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
OwnedAdbServer::EnsureResult EnsureAdbServer(const std::atomic<bool>* canceled,
                                             PhoneServerIntent intent = PhoneServerIntent::PhoneUse)
{
    using Result = OwnedAdbServer::EnsureResult;
    const bool allowOwnedBootstrap = intent == PhoneServerIntent::PhoneUse;
    auto& runtime = Runtime();
    std::unique_lock lifecycleLock(runtime.lifecycleMutex, std::defer_lock);
    while (!lifecycleLock.try_lock_for(std::chrono::milliseconds(20))) {
        CheckCanceled(canceled);
        if (runtime.shutdownRequested.load()) return Result::Stopped;
    }
    CheckCanceled(canceled);
    std::optional<PhoneAdbTestHooks> hooks;
    std::shared_ptr<OwnedAdbServer> owned;
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        hooks = runtime.hooks;
        owned = runtime.ownedServer;
    }
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
            owned->Stop();
            { std::lock_guard lock(runtime.mutex);
              if (runtime.ownedServer == owned) runtime.ownedServer.reset();
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
          shadow = runtime.shadowServer; occupied = !runtime.occupied.empty(); }
        if (shadow && !occupied && owned->OwnsRunningServer()) {
            bool empty = false;
            try {
                // Empty raw protobuf proves there are no transports at all,
                // including unauthorized USB entries filtered from the picker.
                empty = AdbHostReply("host:track-devices-proto-binary", canceled).isEmpty();
            } catch (...) { CheckCanceled(canceled); }
            const bool readyDefault = empty &&
                ProbeAdbHostVersion(env.port, canceled) == OwnedServerProbe::Responsive;
            bool handback = false;
            { std::lock_guard lock(runtime.mutex);
              runtime.shadowHandbackProbes = readyDefault ? runtime.shadowHandbackProbes + 1 : 0;
              handback = runtime.shadowHandbackProbes >= 2; }
            if (handback) {
                CheckCanceled(canceled);
                owned->Stop();
                std::lock_guard lock(runtime.mutex);
                if (runtime.ownedServer == owned) runtime.ownedServer.reset();
                runtime.shadowServer = false; runtime.shadowHandbackProbes = 0;
                runtime.launched.clear();
                runtime.nextServerStart = Clock::now() + std::chrono::seconds(30);
                return Result::Running;
            }
        }
        // Relinquish a dead/failed child before borrowing a newly available
        // default server. Never migrate a running child's transport IDs.
        if (!owned->OwnsRunningServer()) {
            if (occupied) return Result::RestartDeferredLiveSession;
            if (ProbeAdbHostVersion(env.port, canceled) == OwnedServerProbe::Responsive) {
                owned->Stop();
                std::lock_guard lock(runtime.mutex);
                if (runtime.ownedServer == owned) runtime.ownedServer.reset();
                runtime.launched.clear();
                return Result::Running;
            }
        }
        const auto result = owned->EnsureRunning(canceled);
        if (result == Result::Launched || result == Result::Restarted) {
            { std::lock_guard lock(runtime.mutex); runtime.launched.clear(); }
            qInfo() << "OkuFlow owned adb is ready on private port" << owned->Port().value_or(0);
        }
        return result;
    }
    // First engagement borrows a responsive default listener. Repeated host
    // hangs may reclaim only a reviewed legacy OkuFlow bundle; an unknown
    // listener is untouched while a separate private child retries USB.
    const auto defaultProbe = ProbeAdbHostVersion(env.port, canceled);
    CheckCanceled(canceled);
    if (defaultProbe == OwnedServerProbe::Responsive) {
        std::lock_guard lock(runtime.mutex);
        runtime.borrowedFailures = 0;
        runtime.borrowedFailureSince = {};
        runtime.borrowedFailureIdentity.reset();
        return Result::Running;
    }
    if (defaultProbe == OwnedServerProbe::Unresponsive) {
        const auto identity = ListenerIdentity(env.port);
        bool confirmed = false;
        {
            std::lock_guard lock(runtime.mutex);
            if (!runtime.borrowedFailures || runtime.borrowedFailureIdentity != identity) {
                runtime.borrowedFailureIdentity = identity;
                runtime.borrowedFailureSince = Clock::now();
                runtime.borrowedFailures = 0;
            }
            ++runtime.borrowedFailures;
            const auto confirmation = std::chrono::milliseconds(hooks ? hooks->borrowedHangConfirmationMs : 5000);
            confirmed = runtime.borrowedFailures >= 2 && Clock::now() - runtime.borrowedFailureSince >= confirmation;
            if (!runtime.occupied.empty()) confirmed = false;
        }
        if (!allowOwnedBootstrap || !confirmed) return Result::BorrowedUnresponsive;
        bool recoveredLegacy = false;
        if (!hooks && qEnvironmentVariableIsEmpty("OKUFLOW_ADB_PATH")) {
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
            const auto result = TryReclaimLegacyBundledAdbServer(request);
            if (result.status == LegacyAdbRecoveryStatus::Canceled) return Result::Canceled;
            if (result.status == LegacyAdbRecoveryStatus::Busy) return Result::BorrowedUnresponsive;
            recoveredLegacy = result.status == LegacyAdbRecoveryStatus::Recovered;
            qInfo() << "Legacy OkuFlow USB recovery:" << QString::fromStdString(result.diagnostic);
        }
        // An unknown listener is never terminated. The private server keeps
        // trying USB and can hand back after the default recovers safely.
        { std::lock_guard lock(runtime.mutex); runtime.shadowServer = !recoveredLegacy; }
    } else {
        std::lock_guard lock(runtime.mutex);
        runtime.borrowedFailures = 0; runtime.borrowedFailureSince = {};
        runtime.borrowedFailureIdentity.reset();
    }
    if (!allowOwnedBootstrap) return Result::NotReady;
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        if (runtime.ownedServer) { owned = runtime.ownedServer; }
        else {
            if (Clock::now() < runtime.nextServerStart) return Result::Throttled;
            runtime.nextServerStart = Clock::now() + std::chrono::seconds(30);
        }
    }
    if (owned) return owned->EnsureRunning(canceled);
    const QString executable = hooks ? QString::fromStdWString(hooks->ownedServerExecutable)
                                     : FindAdbExecutable();
    if (executable.isEmpty()) return Result::LaunchFailed;
    owned = CreateOwnedAdbServer(executable, hooks ? hooks->ownedServerArguments : std::vector<std::wstring>{});
    {
        auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
        if (runtime.shutdownRequested.load()) return Result::Stopped;
        if (!runtime.ownedServer) runtime.ownedServer = owned;
        // A concurrent first engagement wins; the unstored manager has no
        // child yet, so dropping it here starts nothing.
        owned = runtime.ownedServer;
    }
    CheckCanceled(canceled);
    const auto result = owned->EnsureRunning(canceled);
    if (result == Result::Launched || result == Result::Restarted)
        qInfo() << "OkuFlow owned adb is ready on private port" << owned->Port().value_or(0);
    return result;
}
void RequireAdbServer(const std::atomic<bool>* canceled)
{
    using Result = OwnedAdbServer::EnsureResult;
    const auto result = EnsureAdbServer(canceled);
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
        throw std::runtime_error(Read(socket, size, timeout, canceled).toStdString());
    }
    if (status != "OKAY") throw std::runtime_error("Invalid adb response");
}
void Transport(QTcpSocket& socket, const std::string& serial, const QByteArray& service,
               const std::atomic<bool>* canceled = nullptr, int timeout = 3000, std::uint64_t transportId = 0)
{
    Connect(socket, 300);
    AdbRequest(socket, transportId ? "host:transport-id:" + QByteArray::number(transportId) :
        "host:transport:" + QByteArray::fromStdString(serial), timeout, canceled);
    AdbRequest(socket, service, timeout, canceled);
}
QByteArray AdbHostReply(const QByteArray& service, const std::atomic<bool>* canceled)
{
    CheckCanceled(canceled);
    QTcpSocket socket;
    Connect(socket, 100);
    AdbRequest(socket, service, 150, canceled);
    bool valid = false;
    const int size = Read(socket, 4, 150, canceled).toInt(&valid, 16);
    if (!valid || size < 0 || size > 65535) throw std::runtime_error("Invalid adb reply length");
    return Read(socket, size, 150, canceled);
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
    runtime.launched.clear(); runtime.nextUsbRecovery.clear(); runtime.nextUsbHealthProbe.clear();
}
void PhoneCameraTransport::AllowAppLaunch(const std::string& serial)
{
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); runtime.launched.erase(serial);
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

std::vector<CameraDescriptor> ParseAdbUsbDevices(std::span<const std::uint8_t> protobuf)
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
    std::vector<CameraDescriptor> devices;
    while (root.position < root.bytes.size()) {
        const auto tag = root.Varint();
        if (!(tag >> 3)) throw std::runtime_error("Invalid adb protobuf field");
        if (tag != 10) { root.Skip(static_cast<unsigned>(tag & 7)); continue; }
        const auto size = root.Varint();
        Reader device{root.Bytes(size)};
        std::string serial, model;
        std::uint64_t state = 0, type = 0, transportId = 0;
        while (device.position < device.bytes.size()) {
            const auto field = device.Varint();
            if (!(field >> 3)) throw std::runtime_error("Invalid adb device field");
            if (field == 10 || field == 42) {
                const auto length = device.Varint();
                const auto text = device.Bytes(length);
                if (length > 512) throw std::runtime_error("Oversized adb device name");
                (field == 10 ? serial : model).assign(reinterpret_cast<const char*>(text.data()), text.size());
            } else if (field == 16) state = device.Varint();
            else if (field == 56) type = device.Varint();
            else if (field == 80) transportId = device.Varint();
            else device.Skip(static_cast<unsigned>(field & 7));
        }
        if (type != 1 || state != 8 || serial.empty() || serial.starts_with("emulator-")) continue;
        CameraDescriptor descriptor;
        descriptor.phoneSerial = serial;
        descriptor.phoneTransportId = transportId;
        descriptor.symbolicLink = QString("okuflow-phone:%1").arg(QString::fromStdString(serial)).toStdWString();
        descriptor.name = QString("OkuFlow Camera · %1").arg(QString::fromStdString(model.empty() ? serial : model).replace('_', ' ')).toStdWString();
        devices.push_back(std::move(descriptor));
        if (devices.size() > 128) throw std::runtime_error("Too many USB devices");
    }
    return devices;
}

namespace {
// Listing without companion probes: recovery must not touch other phones.
std::vector<CameraDescriptor> ListUsbPhones(const std::atomic<bool>* canceled)
{
    std::vector<CameraDescriptor> attached;
    try {
        const auto data = AdbHostReply("host:track-devices-proto-binary", canceled);
        attached = ParseAdbUsbDevices(std::span(reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<size_t>(data.size())));
    } catch (...) {
        for (const auto& line : AdbHostReply("host:devices-l", canceled).split('\n')) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 3 || fields[1] != "device" || fields[0].startsWith("emulator-")) continue;
            bool usb = false;
            QByteArray model = fields[0];
            for (const auto& field : fields) {
                if (field.startsWith("usb:")) usb = true;
                if (field.startsWith("model:")) model = field.mid(6);
            }
            if (!usb || attached.size() >= 8) continue;
            CameraDescriptor descriptor;
            descriptor.phoneSerial = fields[0].toStdString();
            for (const auto& field : fields) {
                if (field.startsWith("transport_id:")) {
                    bool valid = false;
                    const auto id = field.mid(13).toULongLong(&valid);
                    if (valid) descriptor.phoneTransportId = id;
                }
            }
            descriptor.symbolicLink = QString("okuflow-phone:%1").arg(QString::fromUtf8(fields[0])).toStdWString();
            descriptor.name = QString("OkuFlow Camera · %1").arg(QString::fromUtf8(model).replace('_', ' ')).toStdWString();
            attached.push_back(std::move(descriptor));
        }
    }
    std::uint64_t epoch = 0;
    { auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex); epoch = runtime.serverEpoch; }
    for (auto& descriptor : attached) descriptor.phoneServerEpoch = epoch;
    return attached;
}

using RecoveryKey = std::pair<std::uint16_t, std::string>;
constexpr std::string_view kUsbHealthToken = "OKUFLOW_USB_HEALTH\n";
// Long enough for a slow first shell on a loaded phone; a reset now needs
// two consecutive timeouts at this budget, not one.
constexpr int kUsbHealthProbeTimeoutMs = 2000;
constexpr auto kUsbHealthProbeCooldown = std::chrono::seconds(30);
constexpr auto kUsbReconnectCooldown = std::chrono::seconds(60);
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
                                                      const std::atomic<bool>* canceled)
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
        {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            auto& next = runtime.nextUsbRecovery[key];
            if (Clock::now() < next) return false;
            // Reserve before sending, including unsupported/failed replies.
            next = Clock::now() + kUsbReconnectCooldown;
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

std::vector<CameraDescriptor> PhoneCameraTransport::Enumerate(
    const std::string& connectedSerial, const std::atomic<bool>* canceled, bool* listingSucceeded,
    const std::string& recoverySerial, PhoneDiscoveryStatus* status, PhoneServerIntent serverIntent)
{
    if (listingSucceeded) *listingSucceeded = false;
    if (status) *status = PhoneDiscoveryStatus::Unknown;
    std::vector<CameraDescriptor> devices;
    try {
        if (!Endpoint().supported) {
            if (status) *status = PhoneDiscoveryStatus::EndpointUnsupported;
            return devices;
        }
        const auto readiness = EnsureAdbServer(canceled, serverIntent);
        ObserveServerIdentity();
        using Result = OwnedAdbServer::EnsureResult;
        if (readiness == Result::BorrowedUnresponsive) {
            if (status) *status = PhoneDiscoveryStatus::ServerUnresponsive;
            return devices;
        }
        if (Endpoint().ownedByThisProcess && readiness != Result::Running &&
            readiness != Result::Launched && readiness != Result::Restarted) {
            if (status && !(canceled && canceled->load())) *status = PhoneDiscoveryStatus::OwnedServerRecovering;
            return devices;
        }
        const auto attached = ListUsbPhones(canceled);
        if (listingSucceeded) *listingSucceeded = true;
        if (status) *status = PhoneDiscoveryStatus::Listed;
        {
            auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
            if (!attached.empty()) runtime.shadowServer = false;
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
            try {
                QTcpSocket capabilitiesSocket;
                Transport(capabilitiesSocket, descriptor.phoneSerial, "localabstract:okuflow_camera", canceled, 150, descriptor.phoneTransportId);
                const auto capabilities = ControlHello(capabilitiesSocket, 1500, false, canceled, true);
                devices.back().phoneBusy = capabilities["busy"].toBool();
                devices.back().phoneProblem = capabilities["problem"].toString().toStdString();
                devices.back().phoneAppReady = !devices.back().phoneBusy && devices.back().phoneProblem.empty();
                const auto discoveredPhone = devices.back();
                const auto cameras = capabilities["cameras"].toArray();
                if (!cameras.empty()) {
                    for (const auto& camera : cameras) {
                        if (devices.size() >= 32) break;
                        const auto id = camera.toObject()["id"].toString();
                        if (id.isEmpty() || id.size() > 128) continue;
                        auto lens = discoveredPhone;
                        lens.phoneCameraId = id.toStdString();
                        lens.symbolicLink += L"/" + id.toStdWString();
                        lens.name += L" · " + id.toStdWString();
                        devices.push_back(std::move(lens));
                    }
                    if (!devices.empty() && devices.back().phoneSerial == descriptor.phoneSerial) continue;
                }
            } catch (const PhoneProtocolError& error) {
                auto& phone = devices.back(); phone.phoneProblem = error.code;
                phone.phoneBusy = error.code == "sessionBusy" || error.code == "busy";
            } catch (const UsbResponseTimeout&) {
                // A timed-out probe is the only discovery evidence consistent
                // with a stalled transport, and only the preferred idle phone
                // may act on it. Ordinary device browsing is read-only.
                if (descriptor.phoneSerial == recoverySerial)
                    RecoverStalledUsbTransport(descriptor, canceled);
            } catch (...) {
                // A prompt refusal (app closed/paused, listener absent) proves
                // adbd answered: no stall evidence and no recovery shell work.
                // App absent: retain the saved-camera entry for later startup.
            }
        }
    } catch (const UsbResponseTimeout&) {
        if (status && !(canceled && canceled->load()))
            *status = Endpoint().ownedByThisProcess ? PhoneDiscoveryStatus::OwnedServerRecovering
                                                   : PhoneDiscoveryStatus::ServerUnresponsive;
    } catch (const AdbServerUnavailable&) {
        if (status && !(canceled && canceled->load()))
            *status = Endpoint().ownedByThisProcess ? PhoneDiscoveryStatus::OwnedServerRecovering
                                                   : PhoneDiscoveryStatus::NoListener;
    } catch (...) { /* Optional companion discovery never makes webcam enumeration fail. */ }
    return devices;
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
    std::lock_guard lock(mutex_); pendingZoom_ = ratio;
}
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

void PhoneCameraTransport::Run(CameraDescriptor descriptor, ComPtr<IMFDXGIDeviceManager> manager,
                               FrameCallback callback, CaptureErrorCallback errorCallback, PhoneStartupBudget startupBudget)
{
    std::unique_ptr<OccupiedPhone> occupied;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Check(apartment, "Initialize phone decoder COM");
        // Repair an idle owned server before this new session counts itself
        // as occupied. Other live/starting sessions remain protected.
        RequireAdbServer(&canceled_);
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
            RequireAdbServer(&canceled_);
            const auto attached = ListUsbPhones(&canceled_);
            const auto selected = std::find_if(attached.begin(), attached.end(), [&](const auto& item) { return item.phoneSerial == descriptor.phoneSerial; });
            if (selected == attached.end())
                throw std::runtime_error("The selected phone is not connected and authorized over USB");
            descriptor.phoneTransportId = selected->phoneTransportId;
            descriptor.phoneServerEpoch = selected->phoneServerEpoch;
            QTcpSocket control, video;
            QJsonObject capabilities;
            const auto connectionDeadline = Clock::now() + std::chrono::seconds(5);
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
                    // Only an absent listener gets a single launch per USB attachment.
                    if (!endpointOpened && MayLaunch(descriptor.phoneSerial, descriptor.phoneTransportId)) {
                        // The Play app uses com.okuflow; retain the earlier
                        // debug package only when the new activity is absent.
                        // A paused/blocked installed app must not launch the
                        // legacy app instead and bypass the user's intent.
                        for (const char* command : {
                            "shell:am start -n com.okuflow/com.okuflow.camera.MainActivity",
                            "shell:am start -n com.okuflow.camera/.MainActivity"}) {
                            QTcpSocket launcher;
                            Transport(launcher, descriptor.phoneSerial, command,
                                      &canceled_, 1000, descriptor.phoneTransportId);
                            QByteArray response;
                            const auto launchDeadline = Clock::now() + std::chrono::seconds(2);
                            while (launcher.state() == QAbstractSocket::ConnectedState && Clock::now() < launchDeadline) {
                                CheckCanceled(&canceled_); launcher.waitForReadyRead(20); response += launcher.readAll();
                                if (response.size() > kControlLimit) throw std::runtime_error("Oversized app launch response");
                            }
                            response += launcher.readAll();
                            if (response.size() > kControlLimit) throw std::runtime_error("Oversized app launch response");
                            if (!response.contains("Error") && !response.contains("Exception")) break;
                            const bool legacy = std::string_view(command).find("com.okuflow.camera/.MainActivity") != std::string_view::npos;
                            if (legacy || !response.contains("Error type 3"))
                                throw std::runtime_error("Install OkuFlow Camera on the selected USB phone first");
                        }
                    }
                    if (Clock::now() >= connectionDeadline)
                        throw std::runtime_error("Open OkuFlow Camera, allow camera access and authorize USB debugging");
                }
                for (int i = 0; i < 5; ++i) { CheckCanceled(&canceled_); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            }
            const auto bound = ReadPacket(video, kControlLimit, &canceled_);
            if (bound.type != 1 || bound.flags || Json(bound)["channel"].toString() != "video") throw std::runtime_error("Phone video binding failed");
            if (!descriptor.phoneCameraId.empty()) Command(control, {{"op", "camera"}, {"cameraId", QString::fromStdString(descriptor.phoneCameraId)}}, &canceled_);
            Command(control, {{"op", "torch"}, {"enabled", false}}, &canceled_);
            const auto now = Clock::now();
            // Initial HEVC->AVC retry shares Start's original deadline. A
            // running stream's later renegotiation gets its normal fresh budget.
            const int cameraBudgetMs = (lastDecodedClock < 0 ? startupBudget : PhoneStartupBudget(now)).RemainingCameraMs(now);
            if (!cameraBudgetMs) throw std::runtime_error("Phone camera startup budget exhausted during USB negotiation");
            Command(control, {{"op", "start"}, {"startupBudgetMs", cameraBudgetMs}}, &canceled_);
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
                if (zoom > 0) Command(control, {{"op", "zoom"}, {"ratio", zoom}}, &canceled_);
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
                        lastDecodedClock = frame.captureTimestamp100ns;
                        frame.sequenceNumber = outputSequence++;
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
    } catch (const std::exception& exception) {
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
