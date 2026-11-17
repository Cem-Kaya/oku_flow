#include "okuflow/capture/phone_camera.hpp"
#include "okuflow/capture/phone_frame_progress.hpp"
#include "okuflow/capture/phone_startup_budget.hpp"
#include "okuflow/capture/adb_owned_server.hpp"
#include "okuflow/capture/adb_legacy_recovery.hpp"
#include <QtTest/QtTest>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <QProcess>
#include <QTemporaryDir>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QtEndian>
#include <future>
#include <functional>
#include <thread>
#include <atomic>
#include <cstring>
#include <mutex>

namespace okuflow {
// Capture-only seam: use the real queued record and guarded socket write, with
// private loopback sockets. No transport Start, device, decoder or adb probe.
struct PhoneTorchTestAccess {
    static bool Send(PhoneCameraTransport& transport, QTcpSocket& socket) { return transport.SendPendingTorch(socket); }
    static auto Take(PhoneCameraTransport& transport) {
        std::lock_guard lock(transport.mutex_);
        auto command=transport.pendingTorch_;transport.pendingTorch_.reset();return command;
    }
    template<typename Command>
    static bool SendTaken(PhoneCameraTransport& transport, QTcpSocket& socket, const Command& command) {
        return command && transport.SendTorchCommand(socket,*command);
    }
    static void Acknowledge(PhoneCameraTransport& transport,const QJsonObject& status) { transport.ApplyTorchAcknowledgment(status); }
    static void Reconnect(PhoneCameraTransport& transport) { transport.RetirePendingTorch(); }
};
}

namespace {
std::int64_t TorchDeadline(int offsetMs=1000) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()+offsetMs;
}
struct PrivateTorchSocket {
    QTcpServer listener;
    QTcpSocket client;
    std::unique_ptr<QTcpSocket> peer;
    bool Open() {
        if(!listener.listen(QHostAddress::LocalHost,0))return false;
        client.connectToHost(QHostAddress::LocalHost,listener.serverPort());
        if(!client.waitForConnected(1000) || !listener.waitForNewConnection(1000))return false;
        peer.reset(listener.nextPendingConnection());return bool(peer);
    }
    static QByteArray Read(QTcpSocket& socket,int size) {
        QByteArray bytes;
        while(bytes.size()<size) {
            bytes+=socket.read(size-bytes.size());
            if(bytes.size()==size)return bytes;
            if(!socket.waitForReadyRead(1000))return {};
        }
        return bytes;
    }
    QJsonObject Request() {
        const auto header=Read(*peer,8);if(header.size()!=8 || qFromLittleEndian<quint16>(header.constData())!=2)return {};
        const auto size=qFromLittleEndian<quint32>(header.constData()+4);if(size>65536)return {};
        return QJsonDocument::fromJson(Read(*peer,int(size))).object();
    }
    bool Ack(okuflow::PhoneCameraTransport& transport,bool enabled) {
        const auto body=QJsonDocument(QJsonObject{{"torchAvailable",true},{"torchEnabled",enabled}}).toJson(QJsonDocument::Compact);
        QByteArray header(8,0);qToLittleEndian<quint16>(4,header.data());qToLittleEndian<quint32>(quint32(body.size()),header.data()+4);
        if(peer->write(header+body)!=header.size()+body.size() || (peer->bytesToWrite() && !peer->waitForBytesWritten(1000)))return false;
        const auto received=Read(client,8);if(received.size()!=8 || qFromLittleEndian<quint16>(received.constData())!=4)return false;
        const auto status=QJsonDocument::fromJson(Read(client,int(qFromLittleEndian<quint32>(received.constData()+4)))).object();
        if(status.isEmpty())return false;
        okuflow::PhoneTorchTestAccess::Acknowledge(transport,status);return true;
    }
};
// Private random port, no bootstrap: these fixtures can never reach a real adb or phone.
class FakePhoneAdb {
public:
    enum class LaunchMode { None, Play, Legacy, Blocked };
    enum class RecoveryMode { Healthy, Stalled, ServerStalled, SelectionStalled, Replugged,
                              Unsupported, LegacyListing, Unauthorized, Wifi,
                              PartialEcho, SlowHealthy, AckWithoutEffect, CompanionBusy };
    enum class ArrivalMode { Normal, BufferedRequest, QueuedConnections };
    explicit FakePhoneAdb(bool old = false, bool stall = false, bool busy = false,
                         LaunchMode mode = LaunchMode::None, RecoveryMode recovery = RecoveryMode::Healthy,
                         bool installHooks = true, quint16 listenPort = 0,
                         ArrivalMode arrival = ArrivalMode::Normal,
                         std::function<void()> healthProbeReceived = {})
        : deviceStalled(recovery != RecoveryMode::Healthy && recovery != RecoveryMode::SlowHealthy),
          old(old), stall(stall), busy(busy),
          launchMode(mode), recoveryMode(recovery), healthProbeReceived(std::move(healthProbeReceived)), installedHooks(installHooks),
          hostUnresponsive(recovery == RecoveryMode::ServerStalled) {
        std::promise<quint16> promise;
        auto future = promise.get_future();
        worker = std::thread([this, listenPort, arrival, promise = std::move(promise)]() mutable {
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, listenPort)) { promise.set_value(0); return; }
            promise.set_value(server.serverPort());
            while (arrival == ArrivalMode::QueuedConnections && !releaseQueuedConnections.load() && !stopped.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            while (!stopped.load() && !closeListener.load()) {
                if (!server.hasPendingConnections() && !server.waitForNewConnection(20)) continue;
                std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
                if (arrival == ArrivalMode::BufferedRequest) {
                    if (!socket->bytesAvailable() && !socket->waitForReadyRead(1000)) continue;
                    ++bufferedRequests;
                }
                try { Serve(*socket); } catch (...) {}
            }
        });
        port = future.get();
        if (!port) throw std::runtime_error("Cannot listen on test socket");
        if (installedHooks) okuflow::SetPhoneAdbTestHooks(okuflow::PhoneAdbTestHooks{port, {}});
    }
    ~FakePhoneAdb() {
        stopped.store(true);
        worker.join();
        if (installedHooks) okuflow::SetPhoneAdbTestHooks(std::nullopt);
    }
    bool installedHooks{true};
    quint16 port{};
    std::atomic<bool> hello{false};
    std::atomic<int> probes{0};
    std::atomic<int> bufferedRequests{0};
    std::atomic<bool> releaseQueuedConnections{false};
    std::atomic<int> reconnects{0}, healthProbes{0};
    std::atomic<std::uint64_t> transportId{9};
    std::atomic<bool> deviceStalled{false};
    std::atomic<bool> hostUnresponsive{false};
    // Link-doctor fixtures: raw devices.proto state of "abc", an empty
    // (blind) listing, a second served device, host:kill obedience, the
    // companion problem code and the read-only shell facts reply.
    std::atomic<int> listingState{8};
    std::atomic<bool> listEmpty{false};
    std::atomic<bool> otherDevice{false};
    std::atomic<bool> exitOnKill{false};
    std::atomic<bool> closeListener{false};
    // A second USB phone "xyz" in state DEVICE (otherDevice is a Wi-Fi one).
    std::atomic<bool> secondUsbPhone{false};
    // adb FAIL texts often carry the serial: the trace must not.
    std::atomic<bool> reconnectFailsWithSerial{false};
    std::string problemCode;
    std::string shellFacts{"W:\n  mWakefulness=Awake\nK:\n mShowingLockscreen=false\nP:\npackage:/data/app/base.apk\nR:\nEND\n"};
    QStringList Services() {
        std::lock_guard lock(launchMutex);
        return services;
    }
    QStringList Launches() {
        std::lock_guard lock(launchMutex);
        return launches;
    }
    void SetShellFacts(std::string facts) { std::lock_guard lock(launchMutex); shellFacts = std::move(facts); }
    void SetProblem(std::string code) { std::lock_guard lock(launchMutex); problemCode = std::move(code); }
    int ServiceCount(const QString& service) { return static_cast<int>(Services().count(service)); }
private:
    QByteArray Read(QTcpSocket& socket, int size) {
        QByteArray result;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (result.size() < size && !stopped.load()) {
            result += socket.read(size - result.size());
            if (result.size() == size) return result;
            if (std::chrono::steady_clock::now() >= deadline) break;
            socket.waitForReadyRead(20);
        }
        throw std::runtime_error("Incomplete fixture request");
    }
    void Write(QTcpSocket& socket, const QByteArray& bytes) {
        socket.write(bytes);
        while (socket.bytesToWrite() && !stopped.load()) socket.waitForBytesWritten(20);
    }
    void Serve(QTcpSocket& socket) {
        // EnsureAdbServer opens a harmless TCP probe with no data.
        if (!socket.bytesAvailable() && !socket.waitForReadyRead(100)) return;
        const auto size = Read(socket, 4).toInt(nullptr, 16);
        const auto service = Read(socket, size);
        { std::lock_guard lock(launchMutex); services.append(QString::fromUtf8(service)); }
        const auto waitForClose = [&] {
            while (!stopped.load() && socket.state() == QAbstractSocket::ConnectedState)
                socket.waitForReadyRead(20);
        };
        if (hostUnresponsive.load()) { waitForClose(); return; }
        if (service == "host:version") { Write(socket, "OKAY00040029"); return; }
        if (service == "host:kill") {
            if (exitOnKill.load()) { Write(socket, "OKAY"); closeListener.store(true); }
            else Write(socket, "FAIL0006denied");
            return;
        }
        if (service == "host-transport-id:" + QByteArray::number(transportId.load()) + ":reconnect") {
            ++reconnects;
            if (recoveryMode == RecoveryMode::Unsupported) { Write(socket, "FAIL000bunsupported"); return; }
            if (reconnectFailsWithSerial.load()) { Write(socket, "FAIL0016device 'abc' not found"); return; }
            const QByteArray reply("reconnecting abc [device]\n");
            // Acknowledged but never re-registers: recovery must not call this success.
            if (recoveryMode != RecoveryMode::AckWithoutEffect) {
                ++transportId; deviceStalled.store(false);
                if (listingState.load() == 6) listingState.store(8); // Offline transport re-registers.
            }
            Write(socket, "OKAY" + QByteArray::number(reply.size(), 16).rightJustified(4, '0') + reply);
            return;
        }
        if (service == "host:track-devices-proto-binary") {
            if (recoveryMode == RecoveryMode::LegacyListing) { Write(socket, "FAIL000bunsupported"); return; }
            const char data[]{10, 11, 10, 3, 'a', 'b', 'c', 16, 8, 56, 1, 80, 9};
            QByteArray body(data, sizeof(data));
            body[sizeof(data) - 1] = static_cast<char>(transportId.load());
            body[8] = static_cast<char>(listingState.load());
            if (recoveryMode == RecoveryMode::Unauthorized) body[8] = 3;
            if (recoveryMode == RecoveryMode::Wifi) body[10] = 2;
            if (listEmpty.load()) body.clear();
            if (otherDevice.load()) {
                // Another program's device over Wi-Fi (not USB) in state DEVICE.
                const char other[]{10, 11, 10, 3, 'w', 'f', 'i', 16, 8, 56, 2, 80, 20};
                body.append(other, sizeof(other));
            }
            if (secondUsbPhone.load()) {
                const char second[]{10, 11, 10, 3, 'x', 'y', 'z', 16, 8, 56, 1, 80, 21};
                body.append(second, sizeof(second));
            }
            Write(socket, "OKAY" + QByteArray::number(body.size(), 16).rightJustified(4, '0') + body);
            return;
        }
        if (service == "host:devices-l") {
            const QByteArray reply("abc device usb:1-1 model:Fixture transport_id:" +
                                   QByteArray::number(transportId.load()) + "\n");
            Write(socket, "OKAY" + QByteArray::number(reply.size(), 16).rightJustified(4, '0') + reply);
            return;
        }
        if (!service.startsWith("host:transport")) return;
        if (recoveryMode == RecoveryMode::SelectionStalled && deviceStalled.load()) {
            ++healthProbes; waitForClose(); return;
        }
        Write(socket, "OKAY");
        const auto nextSize = Read(socket, 4).toInt(nullptr, 16);
        const auto next = Read(socket, nextSize);
        { std::lock_guard lock(launchMutex); services.append(QString::fromUtf8(next)); }
        if (next == "shell:echo OKUFLOW_USB_HEALTH") {
            ++healthProbes;
            if (recoveryMode == RecoveryMode::Replugged) ++transportId;
            Write(socket, "OKAY");
            if (healthProbeReceived) healthProbeReceived();
            if (recoveryMode == RecoveryMode::PartialEcho) { Write(socket, "O"); waitForClose(); }
            else if (recoveryMode == RecoveryMode::SlowHealthy) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                Write(socket, "OKUFLOW_USB_HEALTH\n");
            }
            else if (deviceStalled.load()) waitForClose();
            else Write(socket, "OKUFLOW_USB_HEALTH\n");
            return;
        }
        if (next.startsWith("shell:echo W:")) {
            Write(socket, "OKAY");
            std::string facts;
            { std::lock_guard lock(launchMutex); facts = shellFacts; }
            Write(socket, QByteArray::fromStdString(facts));
            return;
        }
        if (next.startsWith("shell:am start")) {
            { std::lock_guard lock(launchMutex); launches.append(QString::fromUtf8(next)); }
            Write(socket, "OKAY");
            if (launchMode == LaunchMode::Blocked) Write(socket, "Error: Activity start blocked");
            else if (launchMode == LaunchMode::Legacy && !next.contains("com.okuflow.camera/.MainActivity"))
                Write(socket, "Error type 3\nError: Activity class does not exist.");
            else { launchReady = true; Write(socket, "Starting: Intent"); }
            return;
        }
        if (next != "localabstract:okuflow_camera") return;
        if (launchMode != LaunchMode::None && !launchReady) {
            const QByteArray message("listener absent");
            Write(socket, "FAIL" + QByteArray::number(message.size(), 16).rightJustified(4, '0') + message);
            return;
        }
        Write(socket, "OKAY");
        const auto header = Read(socket, 12);
        const auto body = Read(socket, static_cast<int>(qFromLittleEndian<quint32>(header.constData() + 8)));
        if (body.contains("probe")) ++probes;
        hello.store(true);
        // A stalled transport stalls the companion channel too, except when the
        // fixture models a session another host claimed in the meantime.
        if (stall || (deviceStalled.load() && recoveryMode != RecoveryMode::CompanionBusy)) {
            while (!stopped.load() && socket.state() == QAbstractSocket::ConnectedState) socket.waitForReadyRead(20);
            return;
        }
        std::string problem;
        { std::lock_guard lock(launchMutex); problem = problemCode; }
        if (!problem.empty()) {
            const auto coded = QByteArray(R"({"protocol":1,"probe":true,"busy":false,"problem":")") +
                QByteArray::fromStdString(problem) + R"(","cameras":[{"id":"0"}]})";
            QByteArray packet(8, 0);
            qToLittleEndian<quint16>(1, packet.data());
            qToLittleEndian<quint32>(static_cast<quint32>(coded.size()), packet.data() + 4);
            Write(socket, packet + coded);
            return;
        }
        const QByteArray reply = old ? QByteArray(R"({"protocol":1,"token":"old-token","cameras":[{"id":"0"}]})") :
            (busy || recoveryMode == RecoveryMode::CompanionBusy) ? QByteArray(R"({"protocol":1,"probe":true,"busy":true,"problem":null,"cameras":[{"id":"0"}]})") :
                   QByteArray(R"({"protocol":1,"probe":true,"busy":false,"problem":null,"automaticCameraId":"auto","automaticCamera":"0","cameras":[{"id":"0","role":"telephoto","zoomMin":1,"zoomMax":4}]})");
        QByteArray packet(8, 0);
        qToLittleEndian<quint16>(1, packet.data());
        qToLittleEndian<quint32>(static_cast<quint32>(reply.size()), packet.data() + 4);
        Write(socket, packet + reply);
    }
    bool old, stall, busy;
    LaunchMode launchMode;
    RecoveryMode recoveryMode;
    std::function<void()> healthProbeReceived;
    bool launchReady{false};
    std::mutex launchMutex;
    QStringList launches;
    QStringList services;
    std::atomic<bool> stopped{false};
    std::thread worker;
};
}

namespace {
#ifndef OKUFLOW_ADB_OWNED_CHILD_FIXTURE
#error "phone_camera_tests requires adb_owned_child_fixture"
#endif
QString OwnedChildFixturePath() { return QStringLiteral(OKUFLOW_ADB_OWNED_CHILD_FIXTURE); }
bool WaitForFixtureFile(const QString& path, int timeoutMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (QFile::exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return QFile::exists(path);
}
unsigned long ReadFixturePid(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    return file.readAll().trimmed().toULong();
}
// Observation only: tests never terminate a process they found by PID.
bool FixtureProcessExited(unsigned long pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}
bool WaitFixtureProcessExited(unsigned long pid, int timeoutMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (FixtureProcessExited(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return FixtureProcessExited(pid);
}
struct OwnedLaunchState {
    QString fixture;
    QString directory;
    bool exitAfter{false};
    std::atomic<int> launches{0};
    std::mutex mutex;
    std::vector<unsigned long> pids;
    std::unique_ptr<okuflow::OwnedChildProcess> Launch(std::uint16_t)
    {
        const int index = ++launches;
        const QString ready = QDir(directory).filePath(QStringLiteral("ready-%1.txt").arg(index));
        std::vector<std::wstring> arguments;
        if (exitAfter) arguments = {L"exit-after", L"150", ready.toStdWString()};
        else arguments = {L"sleep", ready.toStdWString()};
        std::string error;
        auto child = okuflow::OwnedChildProcess::Launch({fixture.toStdWString(), arguments, {}, {}}, &error);
        if (!child) qWarning().noquote() << "Private owned-child fixture launch failed:" << QString::fromStdString(error);
        if (child) {
            std::lock_guard lock(mutex);
            pids.push_back(child->ProcessId());
        }
        return child;
    }
    unsigned long Pid(size_t index)
    {
        std::lock_guard lock(mutex);
        return index < pids.size() ? pids[index] : 0;
    }
};
okuflow::OwnedAdbServerPolicy TestOwnedPolicy(std::chrono::milliseconds cooldown)
{
    okuflow::OwnedAdbServerPolicy policy;
    policy.restartCooldown = cooldown;
    policy.readinessDeadline = std::chrono::milliseconds(1000);
    policy.pollSlice = std::chrono::milliseconds(10);
    policy.unresponsiveProbesToConfirm = 2;
    policy.confirmationSpacing = std::chrono::milliseconds(10);
    policy.startGrace = std::chrono::milliseconds(0);
    return policy;
}
std::uint16_t PickTestPort()
{
    QTcpServer picker;
    return picker.listen(QHostAddress::LocalHost, 0) ? picker.serverPort() : std::uint16_t{0};
}
struct ScopedOwnedAdbHooks {
    explicit ScopedOwnedAdbHooks(std::vector<std::wstring> arguments = {},
                                std::uint32_t borrowedConfirmationMs = 5000)
        : defaultPort(PickTestPort())
    {
        okuflow::SetPhoneAdbTestHooks(okuflow::PhoneAdbTestHooks{
            defaultPort, {}, OwnedChildFixturePath().toStdWString(), std::move(arguments), borrowedConfirmationMs});
    }
    ~ScopedOwnedAdbHooks() { okuflow::SetPhoneAdbTestHooks(std::nullopt); }
    std::uint16_t defaultPort;
};
} // namespace

namespace {
okuflow::PhoneLinkPolicy FastLinkPolicy()
{
    okuflow::PhoneLinkPolicy policy;
    policy.pollBackoffMs.fill(50);
    policy.userWaitPollMs = policy.healthyPollMs = policy.idlePollMs = 50;
    policy.reconnectRetryMs = 100;
    policy.reconnectCeilingMs = 1000;
    policy.blindGraceMs = 150;
    policy.stuckAfterReconnectMs = 200;
    policy.restartRetryMs = 400;
    policy.restartCeilingMs = 2000;
    policy.takeoverGraceMs = 200;
    policy.hostKillWaitMs = 200;
    policy.takeoverCeilingMs = 2000;
    policy.launchRetryMs = {100, 200};
    policy.launchPeriodicMs = 2000;
    return policy;
}
// Borrowed fixture endpoint (user endpoint semantics) with doctor hooks.
struct ScopedLinkHooks {
    explicit ScopedLinkHooks(std::uint16_t port, okuflow::PhoneAdbInterface present = okuflow::PhoneAdbInterface::Present,
                             std::wstring ownedExecutable = {}, std::vector<std::wstring> arguments = {},
                             std::uint32_t confirmationMs = 5000,
                             const std::function<void(okuflow::PhoneAdbTestHooks&)>& customize = {})
    {
        okuflow::PhoneAdbTestHooks hooks{port, {}, std::move(ownedExecutable), std::move(arguments), confirmationMs};
        hooks.adbInterface = [present] { return present; };
        hooks.linkPolicy = FastLinkPolicy();
        if (customize) customize(hooks);
        okuflow::SetPhoneAdbTestHooks(hooks);
    }
    ~ScopedLinkHooks() { okuflow::SetPhoneAdbTestHooks(std::nullopt); }
};
// Another program's default-port adb server, out of process: a copy of the
// owned-child fixture named adb.exe (or kept under its own name to prove
// the production image rule), on a private port. The production adb.exe
// rule applies to it without any hook. Its exact child handle is the test's.
struct DefaultAdbFixture {
    explicit DefaultAdbFixture(std::vector<std::wstring> flags, std::uint16_t fixedPort = 0, bool namedAdb = true)
        : port(fixedPort ? fixedPort : PickTestPort())
    {
        image = directory.filePath(namedAdb ? QStringLiteral("adb.exe") : QFileInfo(OwnedChildFixturePath()).fileName());
        copied = QFile::copy(OwnedChildFixturePath(), image);
        this->flags = std::move(flags);
        Start();
    }
    bool Start()
    {
        std::vector<std::wstring> arguments{L"-L", L"tcp:" + std::to_wstring(port)};
        arguments.insert(arguments.end(), flags.begin(), flags.end());
        std::string error;
        child = okuflow::OwnedChildProcess::Launch({image.toStdWString(), arguments, {}, {},
            {{L"ADB_EMU", L"0"}, {L"ADB_REJECT_KILL_SERVER", L"1"}}}, &error);
        if (!child) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, port);
            if (probe.waitForConnected(100)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
    bool Exited() const { return !child || !child->IsRunning(); }
    QString File(const QString& name) const { return directory.filePath(name); }
    QTemporaryDir directory;
    QString image;
    bool copied{false};
    std::uint16_t port{};
    std::vector<std::wstring> flags;
    std::unique_ptr<okuflow::OwnedChildProcess> child;
};
// A loopback listener that never accepts, its accept queue filled: Windows
// refuses (or never completes) further connects although the listener is
// in the table, as for an adb frozen with a full queue.
struct StalledListener {
    StalledListener()
    {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0) return;
        int length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        for (int i = 0; i < 512 && !full; ++i) {
            SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            u_long nonBlocking = 1;
            ioctlsocket(client, FIONBIO, &nonBlocking);
            connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            fd_set writable{}, failed{};
            FD_SET(client, &writable);
            FD_SET(client, &failed);
            timeval wait{0, 300 * 1000};
            const int ready = select(0, nullptr, &writable, &failed, &wait);
            if (ready > 0 && FD_ISSET(client, &writable)) { clients.push_back(client); continue; }
            closesocket(client);
            full = true;
        }
    }
    ~StalledListener()
    {
        for (const auto client : clients) closesocket(client);
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
    }
    SOCKET listener{INVALID_SOCKET};
    std::vector<SOCKET> clients;
    std::uint16_t port{};
    bool full{false};
};
// Runs doctor-engaged discovery cycles at the doctor's own cadence.
std::vector<okuflow::PhoneLinkReport> LinkCycles(int count,
    const std::function<bool(const okuflow::PhoneLinkReport&)>& until = {}, const std::string& serial = "abc")
{
    std::vector<okuflow::PhoneLinkReport> reports;
    okuflow::PhoneLinkRequest request;
    request.phoneWanted = true;
    for (int i = 0; i < count; ++i) {
        okuflow::PhoneLinkReport report;
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, nullptr, serial, nullptr,
            okuflow::PhoneServerIntent::PhoneUse, &request, &report);
        reports.push_back(report);
        if (until && until(report)) break;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<std::int64_t>(report.decision.nextPollMs - now, 0, 300)));
    }
    return reports;
}
bool Saw(const std::vector<okuflow::PhoneLinkReport>& reports, okuflow::PhoneLinkAction action)
{
    return std::any_of(reports.begin(), reports.end(), [&](const auto& r) { return r.decision.action == action; });
}
bool Saw(const std::vector<okuflow::PhoneLinkReport>& reports, okuflow::PhoneLinkDiagnosis diagnosis)
{
    return std::any_of(reports.begin(), reports.end(), [&](const auto& r) { return r.decision.diagnosis == diagnosis; });
}
} // namespace

class PhoneCameraTests : public QObject {
    Q_OBJECT
private slots:
    void zoomUsesSelectedLensOrAutomaticCapabilities() {
        const auto caps = QJsonDocument::fromJson(R"({"automaticCamera":"9","cameras":[{"id":"9","zoomMin":0.5,"zoomMax":20},{"id":"2","zoomMin":1,"zoomMax":1}]})").object();
        QCOMPARE(okuflow::ResolvePhoneZoomRange(caps, ""), (std::pair{0.5f,20.0f}));
        QCOMPARE(okuflow::ResolvePhoneZoomRange(caps, "2"), (std::pair{1.0f,1.0f}));
        QCOMPARE(okuflow::ResolvePhoneZoomRange(caps, "missing"), (std::pair{1.0f,1.0f}));
        const auto invalid = QJsonDocument::fromJson(R"({"cameras":[{"id":"9","zoomMin":2,"zoomMax":1}]})").object();
        QCOMPARE(okuflow::ResolvePhoneZoomRange(invalid, "9"), (std::pair{1.0f,1.0f}));
        QCOMPARE(okuflow::ResolvePhoneZoomRange({}, ""), (std::pair{1.0f,1.0f}));
    }
    void alreadyBufferedFakeAdbRequestIsNotDiscarded() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false, 0,
                            FakePhoneAdb::ArrivalMode::BufferedRequest);
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, server.port);
        QVERIFY(client.waitForConnected(1000));
        QCOMPARE(client.write("000chost:version"), qint64{16});
        QVERIFY(!client.bytesToWrite() || client.waitForBytesWritten(1000));
        QVERIFY(client.bytesAvailable() || client.waitForReadyRead(1000));
        QCOMPARE(PrivateTorchSocket::Read(client, 12), QByteArray("OKAY00040029"));
        QCOMPARE(server.bufferedRequests.load(), 1);
    }
    void alreadyQueuedFakeAdbConnectionsAreHandled() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false, 0,
                            FakePhoneAdb::ArrivalMode::QueuedConnections);
        QTcpSocket first, second;
        for (QTcpSocket* client : {&first, &second}) {
            client->connectToHost(QHostAddress::LocalHost, server.port);
            QVERIFY(client->waitForConnected(1000));
            QCOMPARE(client->write("000chost:version"), qint64{16});
            QVERIFY(!client->bytesToWrite() || client->waitForBytesWritten(1000));
        }
        // Both handshakes complete in the listener backlog before the worker
        // accepts either. Qt accepts the batch into its pending queue.
        server.releaseQueuedConnections.store(true);
        for (QTcpSocket* client : {&first, &second}) {
            QVERIFY(client->bytesAvailable() || client->waitForReadyRead(1000));
            QCOMPARE(PrivateTorchSocket::Read(*client, 12), QByteArray("OKAY00040029"));
        }
    }
    void canceledQueuedTorchNeverWrites() {
        PrivateTorchSocket socket;QVERIFY(socket.Open());okuflow::PhoneCameraTransport transport;
        const auto token=std::make_shared<std::atomic<bool>>(false);
        QVERIFY(transport.SetTorch(true,token,TorchDeadline()));token->store(true);
        QVERIFY(!okuflow::PhoneTorchTestAccess::Send(transport,socket.client));
        QVERIFY(!socket.peer->waitForReadyRead(25));QCOMPARE(socket.peer->bytesAvailable(),qint64(0));QVERIFY(!transport.TorchEnabled());
    }
    void expiredDequeuedTorchNeverWrites() {
        PrivateTorchSocket socket;QVERIFY(socket.Open());okuflow::PhoneCameraTransport transport;
        const auto token=std::make_shared<std::atomic<bool>>(false);
        QVERIFY(transport.SetTorch(true,token,TorchDeadline(40)));
        const auto command=okuflow::PhoneTorchTestAccess::Take(transport);
        QTest::qWait(60);
        QVERIFY(!okuflow::PhoneTorchTestAccess::SendTaken(transport,socket.client,command));
        QVERIFY(!socket.peer->waitForReadyRead(25));QCOMPARE(socket.peer->bytesAvailable(),qint64(0));QVERIFY(!transport.TorchEnabled());
    }
    void nativeTorchSupersedesEvenDequeuedActorCommand() {
        PrivateTorchSocket socket;QVERIFY(socket.Open());okuflow::PhoneCameraTransport transport;
        const auto token=std::make_shared<std::atomic<bool>>(false);
        QVERIFY(transport.SetTorch(true,token,TorchDeadline()));const auto command=okuflow::PhoneTorchTestAccess::Take(transport);
        transport.SetTorch(false);QVERIFY(token->load());
        QVERIFY(!okuflow::PhoneTorchTestAccess::SendTaken(transport,socket.client,command));
        QVERIFY(okuflow::PhoneTorchTestAccess::Send(transport,socket.client));
        const auto request=socket.Request();QCOMPARE(request["op"].toString(),QString("torch"));QVERIFY(!request["enabled"].toBool());
        QVERIFY(!socket.peer->waitForReadyRead(25));QCOMPARE(socket.peer->bytesAvailable(),qint64(0));
    }
    void torchStateChangesOnlyWhenAcknowledged() {
        PrivateTorchSocket socket;QVERIFY(socket.Open());okuflow::PhoneCameraTransport transport;
        const auto token=std::make_shared<std::atomic<bool>>(false);
        QVERIFY(transport.SetTorch(true,token,TorchDeadline()));QVERIFY(!transport.TorchEnabled());
        QVERIFY(okuflow::PhoneTorchTestAccess::Send(transport,socket.client));QVERIFY(!transport.TorchEnabled());
        QVERIFY(socket.Request()["enabled"].toBool());token->store(true);
        QVERIFY(socket.Ack(transport,true));QVERIFY(transport.TorchAvailable());QVERIFY(transport.TorchEnabled());
        transport.SetTorch(false);QVERIFY(transport.TorchEnabled());QVERIFY(okuflow::PhoneTorchTestAccess::Send(transport,socket.client));
        QVERIFY(!socket.Request()["enabled"].toBool());QVERIFY(transport.TorchEnabled());
        QVERIFY(socket.Ack(transport,false));QVERIFY(!transport.TorchEnabled());
    }
    void torchQueueRejectsExpiredCanceledOrUnboundedActorAndRetiresOnRetry() {
        PrivateTorchSocket socket;QVERIFY(socket.Open());okuflow::PhoneCameraTransport transport;
        const auto token=std::make_shared<std::atomic<bool>>(false);
        QVERIFY(!transport.SetTorch(true,token,TorchDeadline(-1)));QVERIFY(!transport.SetTorch(true,token,0));
        QVERIFY(!transport.SetTorch(true,{},TorchDeadline()));token->store(true);
        QVERIFY(!transport.SetTorch(true,token,TorchDeadline()));token->store(false);
        QVERIFY(transport.SetTorch(true,token,TorchDeadline()));const auto command=okuflow::PhoneTorchTestAccess::Take(transport);
        okuflow::PhoneTorchTestAccess::Reconnect(transport);QVERIFY(token->load());
        QVERIFY(!okuflow::PhoneTorchTestAccess::SendTaken(transport,socket.client,command));
        QVERIFY(!socket.peer->waitForReadyRead(25));QCOMPARE(socket.peer->bytesAvailable(),qint64(0));
        transport.Cancel();QVERIFY(!transport.SetTorch(true,std::make_shared<std::atomic<bool>>(false),TorchDeadline()));
    }
    void ownedChildStopsExactlyOnStopAndScopeExit() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ready = directory.filePath(QStringLiteral("ready.txt"));
        unsigned long pid = 0;
        {
            std::string error;
            auto child = okuflow::OwnedChildProcess::Launch(
                {fixture.toStdWString(), {L"sleep", ready.toStdWString()}, {}, {}}, &error);
            QVERIFY2(child, error.c_str());
            QVERIFY(child->IsRunning());
            QVERIFY(WaitForFixtureFile(ready));
            pid = ReadFixturePid(ready);
            QCOMPARE(pid, static_cast<unsigned long>(child->ProcessId()));
            child->Stop();
            QVERIFY(child->WaitForExit(std::chrono::milliseconds(1000)));
            QVERIFY(!child->IsRunning());
        }
        QVERIFY(WaitFixtureProcessExited(pid));
        const QString ready2 = directory.filePath(QStringLiteral("ready2.txt"));
        unsigned long pid2 = 0;
        {
            std::string error;
            auto child = okuflow::OwnedChildProcess::Launch(
                {fixture.toStdWString(), {L"sleep", ready2.toStdWString()}, {}, {}}, &error);
            QVERIFY2(child, error.c_str());
            QVERIFY(WaitForFixtureFile(ready2));
            pid2 = ReadFixturePid(ready2);
        } // Destructor alone must stop the exact owned child.
        QVERIFY(WaitFixtureProcessExited(pid2));
    }
    void ownedChildAssignmentFailureCleansFreshChild() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ready = directory.filePath(QStringLiteral("ready.txt"));
        std::atomic<unsigned long> freshPid{0};
        okuflow::OwnedChildProcess::LaunchOptions options;
        options.executable = fixture.toStdWString();
        options.arguments = {L"sleep", ready.toStdWString()};
        options.assignOverride = [&freshPid](HANDLE, HANDLE process) {
            freshPid.store(GetProcessId(process));
            return false;
        };
        std::string error;
        const auto child = okuflow::OwnedChildProcess::Launch(options, &error);
        QVERIFY(!child);
        QVERIFY(!error.empty());
        QVERIFY2(freshPid.load() != 0, error.c_str());
        // The suspended fresh child was terminated, never resumed.
        QVERIFY(WaitFixtureProcessExited(freshPid.load()));
        QVERIFY(!QFile::exists(ready));
    }
    void ownedChildDiesWithOwner_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("normal owner exit") << QStringLiteral("exit");
        QTest::newRow("owner terminated (crash)") << QStringLiteral("hang");
    }
    void ownedChildDiesWithOwner() {
        QFETCH(QString, mode);
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString childReady = directory.filePath(QStringLiteral("child.txt"));
        const QString parentReady = directory.filePath(QStringLiteral("parent.txt"));
        const QString release = directory.filePath(QStringLiteral("release.txt"));
        QProcess parent;
        parent.setProgram(fixture);
        parent.setArguments({QStringLiteral("parent"), mode, childReady, parentReady, release});
        parent.start();
        QVERIFY(WaitForFixtureFile(parentReady));
        const unsigned long childPid = ReadFixturePid(childReady);
        QVERIFY(childPid != 0);
        QVERIFY(!FixtureProcessExited(childPid));
        if (mode == QStringLiteral("hang")) parent.kill();
        else { QFile releaseFile(release); QVERIFY(releaseFile.open(QIODevice::WriteOnly)); }
        QVERIFY(parent.waitForFinished(3000));
        // No destructor runs for a terminated owner; closing the last job
        // handle in the kernel must still take the owned child with it.
        QVERIFY(WaitFixtureProcessExited(childPid));
    }
    void ownedServerRestartsOnlyAfterConfirmedHang() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = fixture;
        state->directory = directory.path();
        std::atomic<int> probes{0};
        okuflow::OwnedAdbServer server(
            [&probes](std::uint16_t, const std::atomic<bool>*) {
                const int call = ++probes;
                return (call == 2 || call == 3) ? okuflow::OwnedServerProbe::Unresponsive
                                                : okuflow::OwnedServerProbe::Responsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(server.EnsureRunning(nullptr), Result::Launched);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Restarted);
        QCOMPARE(server.LaunchCount(), 2);
        QCOMPARE(server.RestartCount(), 1);
        QVERIFY(WaitFixtureProcessExited(state->Pid(0)));
        QVERIFY(!FixtureProcessExited(state->Pid(1)));
        server.Stop();
        QCOMPARE(server.EnsureRunning(nullptr), Result::Stopped);
        QVERIFY(WaitFixtureProcessExited(state->Pid(1)));
    }
    void ownedServerSingleSlowProbeDoesNotRestart() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = fixture;
        state->directory = directory.path();
        std::atomic<int> probes{0};
        okuflow::OwnedAdbServer server(
            [&probes](std::uint16_t, const std::atomic<bool>*) {
                return ++probes == 2 ? okuflow::OwnedServerProbe::Unresponsive
                                     : okuflow::OwnedServerProbe::Responsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(server.EnsureRunning(nullptr), Result::Launched);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Running);
        QCOMPARE(server.LaunchCount(), 1);
        QCOMPARE(server.RestartCount(), 0);
        QVERIFY(!FixtureProcessExited(state->Pid(0)));
    }
    void ownedServerNeverRestartsDuringLiveSession() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = fixture;
        state->directory = directory.path();
        std::atomic<bool> live{false};
        std::atomic<int> probes{0};
        okuflow::OwnedAdbServer server(
            [&probes](std::uint16_t, const std::atomic<bool>*) {
                const auto probe = ++probes;
                return probe == 1 || probe >= 6 ? okuflow::OwnedServerProbe::Responsive
                                                : okuflow::OwnedServerProbe::Unresponsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); },
            [&live] { return live.load(); },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(server.EnsureRunning(nullptr), Result::Launched);
        live.store(true);
        QCOMPARE(server.EnsureRunning(nullptr), Result::RestartDeferredLiveSession);
        QCOMPARE(server.LaunchCount(), 1);
        QVERIFY(!FixtureProcessExited(state->Pid(0)));
        live.store(false);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Restarted);
        QCOMPARE(server.LaunchCount(), 2);
        QVERIFY(WaitFixtureProcessExited(state->Pid(0)));
    }
    void ownedServerCancellationWinsOverLaunchAndRestart() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = fixture;
        state->directory = directory.path();
        std::atomic<bool> canceled{true};
        std::atomic<int> probes{0};
        okuflow::OwnedAdbServer server(
            [&probes](std::uint16_t, const std::atomic<bool>*) {
                return ++probes == 1 ? okuflow::OwnedServerProbe::Responsive
                                     : okuflow::OwnedServerProbe::Unresponsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(server.EnsureRunning(&canceled), Result::Canceled);
        QCOMPARE(server.LaunchCount(), 0);
        canceled.store(false);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Launched);
        canceled.store(true);
        QCOMPARE(server.EnsureRunning(&canceled), Result::Canceled);
        QCOMPARE(server.LaunchCount(), 1);
        QCOMPARE(server.RestartCount(), 0);
        QVERIFY(!FixtureProcessExited(state->Pid(0)));
    }
    void ownedServerRelaunchAfterChildExitHonorsCooldown() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        const auto responsive = [](std::uint16_t, const std::atomic<bool>*) {
            return okuflow::OwnedServerProbe::Responsive;
        };
        {
            auto state = std::make_shared<OwnedLaunchState>();
            state->fixture = fixture;
            state->directory = directory.path();
            state->exitAfter = true;
            okuflow::OwnedAdbServer server(
                responsive, [state](std::uint16_t port) { return state->Launch(port); },
                [] { return PickTestPort(); }, [] { return false; },
                TestOwnedPolicy(std::chrono::seconds(60)));
            QCOMPARE(server.EnsureRunning(nullptr), Result::Launched);
            QVERIFY(WaitFixtureProcessExited(state->Pid(0)));
            QCOMPARE(server.EnsureRunning(nullptr), Result::Throttled);
            QCOMPARE(server.EnsureRunning(nullptr), Result::Throttled);
            QCOMPARE(server.EnsureRunning(nullptr), Result::Throttled);
            QCOMPARE(server.LaunchCount(), 1);
        }
        {
            auto state = std::make_shared<OwnedLaunchState>();
            state->fixture = fixture;
            state->directory = directory.path();
            state->exitAfter = true;
            okuflow::OwnedAdbServer server(
                responsive, [state](std::uint16_t port) { return state->Launch(port); },
                [] { return PickTestPort(); }, [] { return false; },
                TestOwnedPolicy(std::chrono::milliseconds(0)));
            QCOMPARE(server.EnsureRunning(nullptr), Result::Launched);
            QVERIFY(WaitFixtureProcessExited(state->Pid(0)));
            QCOMPARE(server.EnsureRunning(nullptr), Result::Restarted);
            QCOMPARE(server.LaunchCount(), 2);
        }
    }
    void siblingInstanceClassification() {
        // A10/A12: a visible window is never a leftover; an unregistered one
        // is never terminated; registered ones are proven over time.
        using V = okuflow::OkuFlowSiblingVerdict;
        okuflow::OkuFlowSiblingObservation windowed{1, 1, true, true, true, true};
        okuflow::OkuFlowSiblingTrack track;
        for (std::int64_t now = 0; now < 120000; now += 5000)
            QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(windowed, track, now)), int(V::Windowed));
        windowed.exiting = true; windowed.answersWindowMessage = false; // Even a hung, exiting window.
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(windowed, track, 200000)), int(V::Windowed));
        okuflow::OkuFlowSiblingObservation unregistered{2, 2, false, false, false, false};
        track = {};
        for (std::int64_t now = 0; now < 120000; now += 5000)
            QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(unregistered, track, now)), int(V::UnregisteredWindowless));
        okuflow::OkuFlowSiblingObservation exiting{3, 3, true, true, false, false};
        track = {};
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(exiting, track, 0)), int(V::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(exiting, track, 4999)), int(V::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(exiting, track, 5000)), int(V::Leftover));
        okuflow::OkuFlowSiblingObservation hidden{4, 4, true, false, false, false};
        track = {};
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 0)), int(V::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 9999)), int(V::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 10000)), int(V::Leftover));
        // A hidden instance that answers a window message is alive (starting).
        hidden.answersWindowMessage = true;
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 20000)), int(V::Pending));
        hidden.answersWindowMessage = false;
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 21000)), int(V::Pending)); // Starts over.
        // A window appearing resets the proof.
        hidden.hasVisibleWindow = true;
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 40000)), int(V::Windowed));
    }
    void leftoverInstanceIsProvenThenStoppedExactly() {
        // A10 end to end with windowless fixture instances of one "installation".
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const std::wstring tag = L"test" + std::to_wstring(GetCurrentProcessId());
        auto launch = [&](std::vector<std::wstring> arguments) {
            std::string error;
            auto child = okuflow::OwnedChildProcess::Launch({image.toStdWString(), std::move(arguments), {}, {}}, &error);
            if (!child) qWarning() << QString::fromStdString(error);
            return child;
        };
        const QString a = directory.filePath(QStringLiteral("a.txt")), b = directory.filePath(QStringLiteral("b.txt")),
                      c = directory.filePath(QStringLiteral("c.txt"));
        auto exiting = launch({L"instance", tag, L"1", a.toStdWString()});
        auto windowless = launch({L"instance", tag, L"0", b.toStdWString()});
        auto unregistered = launch({L"sleep", c.toStdWString()});
        QVERIFY(exiting && windowless && unregistered);
        QVERIFY(WaitForFixtureFile(a) && WaitForFixtureFile(b) && WaitForFixtureFile(c));
        const auto siblings = okuflow::ObserveOkuFlowSiblings(image.toStdWString(), tag);
        QCOMPARE(siblings.size(), size_t{3});
        const auto find = [&](DWORD pid) {
            return *std::find_if(siblings.begin(), siblings.end(), [pid](const auto& s) { return s.processId == pid; });
        };
        const auto e = find(exiting->ProcessId()), w = find(windowless->ProcessId()), u = find(unregistered->ProcessId());
        QVERIFY(e.registered && e.exiting && !e.hasVisibleWindow && !e.answersWindowMessage);
        QVERIFY(w.registered && !w.exiting && !w.hasVisibleWindow);
        QVERIFY(!u.registered);
        // Another installation's tag never matches.
        for (const auto& other : okuflow::ObserveOkuFlowSiblings(image.toStdWString(), L"another"))
            QVERIFY(!other.registered);
        okuflow::OkuFlowSiblingTrack te, tw, tu;
        using V = okuflow::OkuFlowSiblingVerdict;
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(e, te, 0)), int(V::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(w, tw, 0)), int(V::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(u, tu, 0)), int(V::UnregisteredWindowless));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(e, te, 5000)), int(V::Leftover));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(w, tw, 10000)), int(V::Leftover));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(u, tu, 10000)), int(V::UnregisteredWindowless));
        // A different creation time is a different process: nothing done.
        okuflow::LeftoverInstanceStopRequest stale{e.processId, e.creation + 1, image.toStdWString(), {}};
        QCOMPARE(int(okuflow::StopLeftoverOkuFlowInstance(stale)), int(okuflow::LeftoverInstanceStopStatus::IdentityChanged));
        QVERIFY(exiting->IsRunning());
        // Another image is not this installation: nothing done.
        okuflow::LeftoverInstanceStopRequest foreign{e.processId, e.creation, OwnedChildFixturePath().toStdWString(), {}};
        QCOMPARE(int(okuflow::StopLeftoverOkuFlowInstance(foreign)), int(okuflow::LeftoverInstanceStopStatus::IdentityChanged));
        QVERIFY(exiting->IsRunning());
        okuflow::LeftoverInstanceStopRequest exact{e.processId, e.creation, image.toStdWString(), {}};
        QCOMPARE(int(okuflow::StopLeftoverOkuFlowInstance(exact)), int(okuflow::LeftoverInstanceStopStatus::Stopped));
        QVERIFY(exiting->WaitForExit(std::chrono::milliseconds(3000)));
        okuflow::LeftoverInstanceStopRequest hidden{w.processId, w.creation, image.toStdWString(), {}};
        QCOMPARE(int(okuflow::StopLeftoverOkuFlowInstance(hidden)), int(okuflow::LeftoverInstanceStopStatus::Stopped));
        QVERIFY(windowless->WaitForExit(std::chrono::milliseconds(3000)));
        QVERIFY(unregistered->IsRunning());
    }
    void leftoverAdbChildIsStoppedOnlyWhenProven() {
        // A10 fallback: only the leftover's own child of the bundled adb image.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const QString childReady = directory.filePath(QStringLiteral("child.txt"));
        const QString parentReady = directory.filePath(QStringLiteral("parent.txt"));
        std::string error;
        auto parent = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"parent", L"hang", childReady.toStdWString(), parentReady.toStdWString(),
             directory.filePath(QStringLiteral("release.txt")).toStdWString()}, {}, {}}, &error);
        QVERIFY2(parent, error.c_str());
        QVERIFY(WaitForFixtureFile(parentReady) && WaitForFixtureFile(childReady));
        const auto childPid = ReadFixturePid(childReady);
        QVERIFY(childPid);
        std::uint64_t parentCreation = 0;
        for (const auto& sibling : okuflow::ObserveOkuFlowSiblings(image.toStdWString(), L"none"))
            if (sibling.processId == parent->ProcessId()) parentCreation = sibling.creation;
        QVERIFY(parentCreation);
        // Not the bundled adb image: kept.
        QVERIFY(!okuflow::StopLeftoverAdbChild(parent->ProcessId(), parentCreation,
                                               OwnedChildFixturePath().toStdWString()));
        // A "parent" younger than the child (a reused PID): kept.
        QVERIFY(!okuflow::StopLeftoverAdbChild(parent->ProcessId(), ~std::uint64_t{0}, image.toStdWString()));
        QVERIFY(!FixtureProcessExited(childPid));
        QVERIFY(okuflow::StopLeftoverAdbChild(parent->ProcessId(), parentCreation, image.toStdWString()));
        QVERIFY(WaitFixtureProcessExited(childPid));
        QVERIFY(parent->IsRunning());
    }
    void ownedServerStillStartingIsNotKilled() {
        // A14: alive and never answered (antivirus delaying the start):
        // not judged hung within the start grace; answers later, kept.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = OwnedChildFixturePath();
        state->directory = directory.path();
        std::atomic<bool> answering{false};
        auto policy = TestOwnedPolicy(std::chrono::milliseconds(0));
        policy.readinessDeadline = std::chrono::milliseconds(100);
        policy.startGrace = std::chrono::milliseconds(1500);
        okuflow::OwnedAdbServer server(
            [&answering](std::uint16_t, const std::atomic<bool>*) {
                return answering.load() ? okuflow::OwnedServerProbe::Responsive : okuflow::OwnedServerProbe::Unresponsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; }, policy);
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(server.EnsureRunning(nullptr), Result::NotReady);
        for (int i = 0; i < 5; ++i) QCOMPARE(server.EnsureRunning(nullptr), Result::NotReady);
        QCOMPARE(server.LaunchCount(), 1);
        QCOMPARE(server.RestartCount(), 0);
        answering.store(true);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Running);
        QVERIFY(!FixtureProcessExited(state->Pid(0)));
        // A child that answered once and then stops answering is a hang
        // (its replacement never answers either, so it is not ready yet).
        answering.store(false);
        QCOMPARE(server.EnsureRunning(nullptr), Result::NotReady);
        QCOMPARE(server.RestartCount(), 1);
        // One that never answers is restarted only after the start grace.
        for (int i = 0; i < 3; ++i) QCOMPARE(server.EnsureRunning(nullptr), Result::NotReady);
        QCOMPARE(server.RestartCount(), 1);
        QTest::qWait(1600);
        QCOMPARE(server.EnsureRunning(nullptr), Result::NotReady);
        QCOMPARE(server.RestartCount(), 2);
    }
    void ownedServerBudgetResetNeverBlocks() {
        // C7: ResetBudgets is lock-free while a launch holds the manager.
        std::atomic<bool> release{false};
        okuflow::OwnedAdbServer server(
            [](std::uint16_t, const std::atomic<bool>*) { return okuflow::OwnedServerProbe::NoListener; },
            [&release](std::uint16_t) {
                while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return std::unique_ptr<okuflow::OwnedChildProcess>{};
            },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::seconds(60)));
        auto launch = std::async(std::launch::async, [&] { return server.EnsureRunning(nullptr); });
        auto unblock = qScopeGuard([&] { release.store(true); if (launch.valid()) launch.wait(); });
        QTest::qWait(100);
        // On its own thread with a deadline, so a regression fails instead of hanging.
        auto reset = std::async(std::launch::async, [&] { server.ResetBudgets(); });
        const bool prompt = reset.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
        release.store(true);
        reset.wait();
        QVERIFY2(prompt, "ResetBudgets waited for the launch holding the manager");
        QCOMPARE(launch.get(), okuflow::OwnedAdbServer::EnsureResult::LaunchFailed);
        // The reset requested during the launch clears its 60 s cooldown.
        server.ResetBudgets();
        QCOMPARE(server.EnsureRunning(nullptr), okuflow::OwnedAdbServer::EnsureResult::LaunchFailed);
    }
    void ownedServerFailedLaunchHonorsCooldown() {
        int attempts = 0;
        okuflow::OwnedAdbServer server(
            [](std::uint16_t, const std::atomic<bool>*) {
                return okuflow::OwnedServerProbe::NoListener;
            },
            [&attempts](std::uint16_t) {
                ++attempts;
                return std::unique_ptr<okuflow::OwnedChildProcess>{};
            },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::seconds(60)));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(server.EnsureRunning(nullptr), Result::LaunchFailed);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Throttled);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Throttled);
        QCOMPARE(attempts, 1);
        QVERIFY(!server.Port().has_value());
    }
    void ownedServerDeadChildDoesNotRelaunchDuringLiveSession() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = OwnedChildFixturePath(); state->directory = directory.path();
        state->exitAfter = true;
        bool live = false;
        okuflow::OwnedAdbServer server(
            [](std::uint16_t, const std::atomic<bool>*) { return okuflow::OwnedServerProbe::Responsive; },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [&live] { return live; },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(server.EnsureRunning(nullptr, false), Result::NotReady);
        QCOMPARE(server.LaunchCount(), 0);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Launched);
        QVERIFY(WaitFixtureProcessExited(state->Pid(0)));
        live = true;
        QCOMPARE(server.EnsureRunning(nullptr), Result::RestartDeferredLiveSession);
        QCOMPARE(server.LaunchCount(), 1);
        live = false;
        QCOMPARE(server.EnsureRunning(nullptr), Result::Restarted);
        QCOMPARE(server.LaunchCount(), 2);
    }
    void ownedServerCancellationDuringProbeWins() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = fixture;
        state->directory = directory.path();
        std::atomic<bool> canceled{false};
        bool cancelOnProbe = true;
        okuflow::OwnedAdbServer server(
            [&](std::uint16_t, const std::atomic<bool>*) {
                if (cancelOnProbe) canceled.store(true);
                return okuflow::OwnedServerProbe::Responsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        // Cancellation during readiness must beat a successful probe.
        QCOMPARE(server.EnsureRunning(&canceled), Result::Canceled);
        QCOMPARE(server.LaunchCount(), 1);
        canceled.store(false);
        cancelOnProbe = false;
        QCOMPARE(server.EnsureRunning(&canceled), Result::Running);
        cancelOnProbe = true;
        QCOMPARE(server.EnsureRunning(&canceled), Result::Canceled);
        QCOMPARE(server.LaunchCount(), 1);
        QCOMPARE(server.RestartCount(), 0);
    }
    void ownedWiringNeverBootstrapsWithoutPhoneIntent() {
        ScopedOwnedAdbHooks hooks;
        QVERIFY(hooks.defaultPort != 0);
        bool listed = true;
        okuflow::PhoneDiscoveryStatus status{};
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, okuflow::PhoneServerIntent::BorrowOnly).empty());
        QVERIFY(!listed);
        QCOMPARE(status, okuflow::PhoneDiscoveryStatus::NoListener);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
    }
    void ownedWiringRoutesPrivateServerAndShutdownLatches() {
        ScopedOwnedAdbHooks hooks;
        bool listed = false;
        okuflow::PhoneDiscoveryStatus status{};
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, okuflow::PhoneServerIntent::PhoneUse).empty());
        QVERIFY(listed);
        QCOMPARE(status, okuflow::PhoneDiscoveryStatus::Listed);
        const auto port = okuflow::OwnedAdbServerPort();
        QVERIFY(port.has_value());
        QVERIFY(*port != hooks.defaultPort);
        // Real loopback listener ownership was checked by the production manager.
        okuflow::ShutdownOwnedAdbServer();
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QTcpSocket gone;
        gone.connectToHost(QHostAddress::LocalHost, *port);
        QVERIFY(!gone.waitForConnected(100));
        listed = false;
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, okuflow::PhoneServerIntent::PhoneUse).empty());
        QVERIFY(!listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
    }
    void ownedWiringRelinquishesIdleServerForWebcam() {
        ScopedOwnedAdbHooks hooks;
        bool listed = false;
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, okuflow::PhoneServerIntent::PhoneUse).empty());
        QVERIFY(listed);
        const auto port = okuflow::OwnedAdbServerPort();
        QVERIFY(port.has_value());
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed).empty());
        QVERIFY(listed);
        QCOMPARE(okuflow::OwnedAdbServerPort(), port); // BorrowOnly never retires a server.
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, okuflow::PhoneServerIntent::ReleaseForWebcam).empty());
        QVERIFY(!listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QTcpSocket gone;
        gone.connectToHost(QHostAddress::LocalHost, *port);
        QVERIFY(!gone.waitForConnected(100));
    }
    void ownedWiringDeadChildBorrowsNewDefaultServer() {
        ScopedOwnedAdbHooks hooks({L"--fixture-exit-after-list"});
        bool listed = false;
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, okuflow::PhoneServerIntent::PhoneUse).empty());
        QVERIFY(listed);
        QTRY_VERIFY_WITH_TIMEOUT(!okuflow::OwnedAdbServerPort().has_value(), 2000);
        FakePhoneAdb replacement(false, false, false, FakePhoneAdb::LaunchMode::None,
                                 FakePhoneAdb::RecoveryMode::Healthy, false, hooks.defaultPort);
        listed = false;
        QCOMPARE(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, okuflow::PhoneServerIntent::PhoneUse).size(), size_t{2});
        QVERIFY(listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(replacement.probes.load() != 0);
    }
    void ownedWiringHungStartupReportsAutomaticRecovery() {
        ScopedOwnedAdbHooks hooks({L"--fixture-hang"});
        bool listed = true;
        okuflow::PhoneDiscoveryStatus status{};
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, okuflow::PhoneServerIntent::PhoneUse).empty());
        QVERIFY(!listed);
        QCOMPARE(status, okuflow::PhoneDiscoveryStatus::OwnedServerRecovering);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        // Borrowed hangs remain a distinct diagnostic status; UI retries automatically.
    }
    void ownedWiringShadowsHungDefaultThenHandsBackAfterConfirmation() {
        ScopedOwnedAdbHooks hooks({}, 0);
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false, hooks.defaultPort);
        bool listed = false;
        okuflow::PhoneDiscoveryStatus status{};
        using Intent = okuflow::PhoneServerIntent;
        // Ordinary enumeration neither terminates nor shadows a stalled listener.
        for (int attempt = 0; attempt < 2; ++attempt) {
            QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, Intent::BorrowOnly).empty());
            QCOMPARE(status, okuflow::PhoneDiscoveryStatus::ServerUnresponsive);
            QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        }
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, Intent::PhoneUse).empty());
        QVERIFY(listed);
        QCOMPARE(status, okuflow::PhoneDiscoveryStatus::OwnedServerRecovering);
        const auto shadow = okuflow::OwnedAdbServerPort();
        QVERIFY(shadow.has_value());
        QVERIFY(*shadow != borrowed.port);
        QVERIFY(!borrowed.Services().contains(QStringLiteral("host:kill")));
        borrowed.hostUnresponsive.store(false);
        borrowed.deviceStalled.store(false);
        // One healthy default answer is insufficient to migrate routing.
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, Intent::PhoneUse).empty());
        QCOMPARE(okuflow::OwnedAdbServerPort(), shadow);
        const auto recovered = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, Intent::PhoneUse);
        QCOMPARE(recovered.size(), size_t{2});
        QVERIFY(listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(recovered.front().phoneServerEpoch != 0);
        QVERIFY(!borrowed.Services().contains(QStringLiteral("host:kill")));
    }
    void ownedWiringShadowNeverHandsBackWithPrivateTransports() {
        ScopedOwnedAdbHooks hooks({L"--fixture-device"}, 0);
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false, hooks.defaultPort);
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse).empty());
        const auto first = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QCOMPARE(first.size(), size_t{1});
        const auto shadow = okuflow::OwnedAdbServerPort();
        QVERIFY(shadow.has_value());
        borrowed.hostUnresponsive.store(false);
        borrowed.deviceStalled.store(false);
        for (int attempt = 0; attempt < 3; ++attempt) {
            QCOMPARE(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse).size(), size_t{1});
            QCOMPARE(okuflow::OwnedAdbServerPort(), shadow);
        }
        QVERIFY(!borrowed.Services().contains(QStringLiteral("host:kill")));
    }
    void ownedWiringEpochChangesWhenNewServerReusesTransportId() {
        ScopedOwnedAdbHooks hooks({L"--fixture-device", L"--fixture-exit-after-list"});
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        const auto before = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QCOMPARE(before.size(), size_t{1});
        QVERIFY(before.front().phoneServerEpoch != 0);
        QTRY_VERIFY_WITH_TIMEOUT(!okuflow::OwnedAdbServerPort().has_value(), 2000);
        FakePhoneAdb replacement(false, false, false, FakePhoneAdb::LaunchMode::None,
                                 FakePhoneAdb::RecoveryMode::Healthy, false, hooks.defaultPort);
        const auto after = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QCOMPARE(after.size(), size_t{2});
        QCOMPARE(after.front().phoneTransportId, before.front().phoneTransportId);
        QVERIFY(after.front().phoneServerEpoch > before.front().phoneServerEpoch);
    }
    void ownedWiringCanceledCameraStopIsNotBlockedByDiscoveryStartup() {
        ScopedOwnedAdbHooks hooks({L"--fixture-hang"});
        std::atomic<bool> discoveryCanceled{false};
        auto discovery = std::async(std::launch::async, [&] {
            return okuflow::PhoneCameraTransport::Enumerate({}, &discoveryCanceled, nullptr, {}, nullptr,
                                                            okuflow::PhoneServerIntent::PhoneUse);
        });
        QTRY_VERIFY_WITH_TIMEOUT(okuflow::OwnedAdbServerPort().has_value(), 2000);
        // The private child is alive while discovery still awaits its silent host.
        QVERIFY(discovery.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
        okuflow::CameraDescriptor descriptor;
        descriptor.phoneSerial = "fixture-never-opened";
        descriptor.symbolicLink = L"okuflow-phone:fixture-never-opened";
        okuflow::PhoneCameraTransport transport;
        auto cancel = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); transport.Cancel();
        });
        const bool started = transport.Start(descriptor, nullptr, [](okuflow::MediaFrame&&) {}, {});
        cancel.get();
        const auto stopStarted = std::chrono::steady_clock::now();
        transport.Stop();
        const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
        discoveryCanceled.store(true);
        discovery.get();
        QVERIFY(!started);
        QVERIFY(stopElapsed < std::chrono::milliseconds(700));
    }
    void ownedServerConcurrentEnsureStartsOneChild() {
        QTemporaryDir directory;
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = OwnedChildFixturePath(); state->directory = directory.path();
        QVERIFY(QFile::exists(state->fixture));
        okuflow::OwnedAdbServer server(
            [](std::uint16_t, const std::atomic<bool>*) { return okuflow::OwnedServerProbe::Responsive; },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        auto a = std::async(std::launch::async, [&] { return server.EnsureRunning(nullptr); });
        auto b = std::async(std::launch::async, [&] { return server.EnsureRunning(nullptr); });
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        const auto first = a.get(), second = b.get();
        QVERIFY((first == Result::Launched && second == Result::Running) ||
                (second == Result::Launched && first == Result::Running));
        QCOMPARE(server.LaunchCount(), 1);
    }
    void ownedServerStopInterruptsReadiness() {
        QTemporaryDir directory;
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = OwnedChildFixturePath(); state->directory = directory.path();
        QVERIFY(QFile::exists(state->fixture));
        std::promise<void> firstProbe;
        auto probeStarted = firstProbe.get_future();
        std::atomic<bool> observed{false};
        auto policy = TestOwnedPolicy(std::chrono::milliseconds(0));
        policy.readinessDeadline = std::chrono::seconds(10);
        okuflow::OwnedAdbServer server(
            [&](std::uint16_t, const std::atomic<bool>*) {
                if (!observed.exchange(true)) firstProbe.set_value();
                return okuflow::OwnedServerProbe::Unresponsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; }, policy);
        auto ensure = std::async(std::launch::async, [&] { return server.EnsureRunning(nullptr); });
        QVERIFY(probeStarted.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto start = std::chrono::steady_clock::now();
        server.Stop();
        QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        QCOMPARE(ensure.get(), Result::Canceled);
        QCOMPARE(server.EnsureRunning(nullptr), Result::Stopped);
        QVERIFY(WaitFixtureProcessExited(state->Pid(0)));
    }
    void ownedServerObservationAndWaitingCancellationDoNotBlockOnProbe() {
        QTemporaryDir directory;
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = OwnedChildFixturePath(); state->directory = directory.path();
        QVERIFY(QFile::exists(state->fixture));
        std::promise<void> probing;
        auto probeEntered = probing.get_future();
        std::promise<void> release;
        auto mayFinish = release.get_future().share();
        okuflow::OwnedAdbServer server(
            [&](std::uint16_t, const std::atomic<bool>*) {
                probing.set_value();
                mayFinish.wait();
                return okuflow::OwnedServerProbe::Responsive;
            },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; },
            TestOwnedPolicy(std::chrono::milliseconds(0)));
        auto owner = std::async(std::launch::async, [&] { return server.EnsureRunning(nullptr); });
        QVERIFY(probeEntered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        auto observation = std::async(std::launch::async, [&] { return server.Port(); });
        std::atomic<bool> canceled{true};
        auto waiter = std::async(std::launch::async, [&] { return server.EnsureRunning(&canceled); });
        const bool observationReady = observation.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
        const bool waiterReady = waiter.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
        // Always release the private owner before assertions/destructors.
        release.set_value();
        QVERIFY(observationReady);
        QVERIFY(waiterReady);
        QVERIFY(observation.get().has_value());
        QCOMPARE(waiter.get(), okuflow::OwnedAdbServer::EnsureResult::Canceled);
        QCOMPARE(owner.get(), okuflow::OwnedAdbServer::EnsureResult::Launched);
    }
    void ownedServerResponsiveForeignListenerIsNotClaimed() {
        QTemporaryDir directory;
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = OwnedChildFixturePath(); state->directory = directory.path();
        QVERIFY(QFile::exists(state->fixture));
        QTcpServer foreign;
        QVERIFY(foreign.listen(QHostAddress::LocalHost, 0));
        auto policy = TestOwnedPolicy(std::chrono::milliseconds(0));
        policy.readinessDeadline = std::chrono::milliseconds(25);
        policy.verifyListenerOwnership = true;
        okuflow::OwnedAdbServer server(
            [](std::uint16_t, const std::atomic<bool>*) { return okuflow::OwnedServerProbe::Responsive; },
            [state](std::uint16_t port) { return state->Launch(port); },
            [&] { return foreign.serverPort(); }, [] { return false; }, policy);
        QCOMPARE(server.EnsureRunning(nullptr), okuflow::OwnedAdbServer::EnsureResult::NotReady);
        QCOMPARE(server.EnsureRunning(nullptr, false), okuflow::OwnedAdbServer::EnsureResult::NotReady);
        server.Stop();
        QVERIFY(foreign.isListening());
    }
    void borrowedFakeServerIsNeverOwnedOrStopped() {
        FakePhoneAdb server;
        bool listed = false;
        QCOMPARE(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed).size(), size_t{2});
        QVERIFY(listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        okuflow::ShutdownOwnedAdbServer();
        listed = false;
        QCOMPARE(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed).size(), size_t{2});
        QVERIFY(listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
    }
    void explicitEndpointOverrideIsBorrowedAndNeverOwned() {
        FakePhoneAdb server;
        const quint16 port = server.port;
        // Borrow the same private fixture through the endpoint override
        // instead of the test hooks; nothing here can reach port 5037.
        okuflow::SetPhoneAdbTestHooks(std::nullopt);
        qputenv("ADB_SERVER_SOCKET",
                QByteArray("tcp:127.0.0.1:") + QByteArray::number(port));
        auto restore = qScopeGuard([port] {
            qunsetenv("ADB_SERVER_SOCKET");
            okuflow::SetPhoneAdbTestHooks(okuflow::PhoneAdbTestHooks{port, {}});
        });
        bool listed = false;
        QCOMPARE(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed).size(), size_t{2});
        QVERIFY(listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        okuflow::ShutdownOwnedAdbServer();
        listed = false;
        QCOMPARE(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed).size(), size_t{2});
        QVERIFY(listed);
    }
    void discoveryDistinguishesHungServerFromMissingPhone() {
        using Status = okuflow::PhoneDiscoveryStatus;
        {
            FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                                FakePhoneAdb::RecoveryMode::ServerStalled);
            bool listed = true;
            Status status{};
            QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status).empty());
            QVERIFY(!listed); QCOMPARE(status, Status::ServerUnresponsive);
            QCOMPARE(server.reconnects.load(), 0);
        }
        {
            FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                                FakePhoneAdb::RecoveryMode::Wifi);
            bool listed = false;
            Status status{};
            QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status).empty());
            QVERIFY(listed); QCOMPARE(status, Status::Listed);
            QCOMPARE(server.reconnects.load(), 0);
        }
    }
    void stalledUsbRecovery_data() {
        QTest::addColumn<int>("mode"); QTest::addColumn<bool>("requested");
        using Mode = FakePhoneAdb::RecoveryMode;
        QTest::newRow("healthy device preserves camera/decoder state") << int(Mode::Healthy) << false;
        QTest::newRow("stalled selected USB device") << int(Mode::Stalled) << true;
        QTest::newRow("hung shared server is left alive") << int(Mode::ServerStalled) << false;
        QTest::newRow("stalled transport selection") << int(Mode::SelectionStalled) << true;
        QTest::newRow("replug during probe keeps new attachment") << int(Mode::Replugged) << false;
        QTest::newRow("unsupported reconnect is throttled") << int(Mode::Unsupported) << false;
        QTest::newRow("older server retains positive USB transport id") << int(Mode::LegacyListing) << true;
        QTest::newRow("unauthorized device is never reset") << int(Mode::Unauthorized) << false;
        QTest::newRow("Wi-Fi transport is never reset") << int(Mode::Wifi) << false;
        QTest::newRow("one echoed byte then stall is not health") << int(Mode::PartialEcho) << true;
        QTest::newRow("slow but healthy 1.5s shell is not reset") << int(Mode::SlowHealthy) << false;
        QTest::newRow("acknowledgement without transport change is not success") << int(Mode::AckWithoutEffect) << false;
        QTest::newRow("session claimed before send is not reset") << int(Mode::CompanionBusy) << false;
    }
    void stalledUsbRecovery() {
        QFETCH(int, mode); QFETCH(bool, requested);
        const auto policy = static_cast<FakePhoneAdb::RecoveryMode>(mode);
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None, policy);
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        QCOMPARE(okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone), requested);
        const bool commandSent = requested || policy == FakePhoneAdb::RecoveryMode::Unsupported ||
            policy == FakePhoneAdb::RecoveryMode::AckWithoutEffect;
        QCOMPARE(server.reconnects.load(), commandSent ? 1 : 0);
        if (commandSent) {
            // A new ID cannot bypass the per-phone/server cooldown.
            server.deviceStalled.store(true);
            phone.phoneTransportId = server.transportId.load();
            QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone));
            QCOMPARE(server.reconnects.load(), 1);
        }
        for (const auto& service : server.Services()) {
            QVERIFY(service != "host:kill" && service != "host:reconnect-offline");
            QVERIFY(!service.contains("host:reconnect"));
            if (service.contains(":reconnect")) QCOMPARE(service, QString("host-transport-id:9:reconnect"));
            QVERIFY(!service.contains("am start"));
        }
    }
    void busyAndProblemStatesDoNotResetUsb() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Stalled);
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        phone.phoneBusy = true;
        QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone));
        phone.phoneBusy = false;
        for (const auto* problem : {"busy", "blocked", "noMode", "failed", "hot", "sessionBusy"}) {
            phone.phoneProblem = problem;
            QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone));
        }
        phone.phoneProblem.clear(); phone.phoneTransportId = 0;
        QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone));
        QVERIFY(server.Services().empty());
        phone.phoneTransportId = 17;
        QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone));
        QCOMPARE(server.healthProbes.load(), 0); QCOMPARE(server.reconnects.load(), 0);
    }
    void onlyPreferredIdleDiscoveryMayRecoverUsb() {
        FakePhoneAdb server(false, true, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Stalled);
        bool listed = false;
        const auto ordinary = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed);
        QVERIFY(listed); QCOMPARE(ordinary.size(), size_t{1});
        QCOMPARE(server.reconnects.load(), 0);
        const auto preferred = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc");
        QVERIFY(listed); QCOMPARE(preferred.size(), size_t{1});
        QCOMPARE(server.reconnects.load(), 1);
        // Live/starting ownership must win even when recovery is requested.
        server.deviceStalled.store(true);
        const auto live = okuflow::PhoneCameraTransport::Enumerate("abc", nullptr, &listed, "abc");
        QVERIFY(listed); QCOMPARE(live.size(), size_t{1});
        QVERIFY(live[0].phoneAppReady); QCOMPARE(server.reconnects.load(), 1);
        // Ordinary + preferred discovery probes, plus recovery's fresh
        // non-claiming re-probe immediately before the reset was sent.
        QCOMPARE(server.probes.load(), 3);
    }
    void healthyRecoveryProbeIsNotRepeatedEveryDiscoveryCycle() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy);
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone));
        QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone));
        // The definitive healthy answer suppresses further shell probes for a
        // cooldown instead of running one on every 5-second discovery cycle.
        QCOMPARE(server.healthProbes.load(), 1);
        QCOMPARE(server.reconnects.load(), 0);
    }
    void promptCompanionRefusalNeverTriggersRecovery() {
        // Listener absent (app closed/not yet launched): adbd refuses promptly,
        // which is progress evidence, not a stalled transport.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Stalled);
        bool listed = false;
        const auto phones = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc");
        QVERIFY(listed); QCOMPARE(phones.size(), size_t{1});
        QVERIFY(!phones[0].phoneAppReady);
        QCOMPARE(server.healthProbes.load(), 0);
        QCOMPARE(server.reconnects.load(), 0);
    }
    void canceledUsbRecoveryStopsPromptly_data() {
        QTest::addColumn<int>("startupDelayMs");
        QTest::newRow("immediate-start") << 0;
        QTest::newRow("delayed-start") << 1250;
    }
    void canceledUsbRecoveryStopsPromptly() {
        QFETCH(int, startupDelayMs);
        using Clock = std::chrono::steady_clock;
        std::atomic<bool> canceled{false};
        std::promise<Clock::time_point> cancellation;
        auto probeEntered = cancellation.get_future();
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Stalled, true, 0,
                            FakePhoneAdb::ArrivalMode::Normal, [&] {
            // Cancel at the actual stalled-read boundary, independently of the
            // test/UI thread's scheduling and earlier host/listing exchanges.
            const auto when = Clock::now();
            canceled.store(true);
            cancellation.set_value(when);
        });
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        auto result = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(startupDelayMs));
            const bool recovered = okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone, &canceled);
            return std::make_pair(recovered, Clock::now());
        });
        auto cancelOnExit = qScopeGuard([&] { canceled.store(true); });
        QVERIFY2(probeEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                 qPrintable(QString("Health probe not reached; count=%1, services=%2")
                     .arg(server.healthProbes.load()).arg(server.Services().join(", "))));
        const auto canceledAt = probeEntered.get();
        QVERIFY(result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
        const auto [recovered, finishedAt] = result.get();
        QVERIFY2(finishedAt - canceledAt < std::chrono::milliseconds(500),
                 "Cancellation exceeded 500 ms after the health-probe boundary");
        QVERIFY(!recovered);
        QCOMPARE(server.healthProbes.load(), 1);
        QCOMPARE(server.reconnects.load(), 0);
    }
    void failedStartRequestsTargetedUsbRecovery() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::SelectionStalled);
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(apartment)); QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        auto teardown = qScopeGuard([&] { MFShutdown(); CoUninitialize(); });
        okuflow::PhoneCameraTransport transport;
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        QVERIFY(!transport.Start(phone, nullptr, {}, {})); transport.Stop();
        QCOMPARE(server.reconnects.load(), 1);
        // Normal discovery, rather than stale descriptor reuse, sees the new ID.
        bool listed = false;
        const auto phones = okuflow::PhoneCameraTransport::Enumerate("abc", nullptr, &listed);
        QVERIFY(listed); QCOMPARE(phones.size(), size_t{1});
        QCOMPARE(phones[0].phoneTransportId, std::uint64_t{10});
    }
    void packetsWithoutDecodedFramesExpireForBothCodecs()
    {
        using Progress = okuflow::PhoneFrameProgress;
        const auto start = Progress::TimePoint{};
        for (const bool hevc : {false, true}) {
            Progress progress(start);
            progress.BeginGeneration(hevc, start);
            progress.PacketReceived(start);
            progress.FrameDecoded(start);
            for (int second = 1; second < 12; ++second) {
                const auto now = start + std::chrono::seconds(second);
                progress.PacketReceived(now);
                QCOMPARE(progress.Check(now), Progress::Failure::None);
            }
            const auto deadline = start + std::chrono::seconds(12);
            progress.PacketReceived(deadline);
            QCOMPARE(progress.Check(deadline), hevc ? Progress::Failure::HevcStalled
                                                   : Progress::Failure::DecoderStalled);
        }
    }
    void onlyDecodedFramesRenewDecodedDeadline()
    {
        using Progress = okuflow::PhoneFrameProgress;
        const auto start = Progress::TimePoint{};
        Progress progress(start);
        progress.BeginGeneration(false, start);
        progress.FrameDecoded(start + std::chrono::seconds(11));
        progress.PacketReceived(start + std::chrono::seconds(12));
        QCOMPARE(progress.Check(start + std::chrono::seconds(12)), Progress::Failure::None);
        progress.PacketReceived(start + std::chrono::seconds(23));
        QCOMPARE(progress.Check(start + std::chrono::seconds(23)), Progress::Failure::DecoderStalled);
    }
    void generationChangesCannotHideStoppedFrames()
    {
        using Progress = okuflow::PhoneFrameProgress;
        const auto start = Progress::TimePoint{};
        Progress progress(start);
        progress.BeginGeneration(true, start);
        progress.EncodedFrameReceived(start);
        progress.PacketReceived(start + std::chrono::seconds(4));
        QCOMPARE(progress.Check(start + std::chrono::seconds(4)), Progress::Failure::HevcStalled);
        // The AVC replacement has a fresh transport deadline; later generation
        // changes within that stream cannot keep an empty decoder alive.
        Progress avc(start + std::chrono::seconds(4));
        avc.BeginGeneration(false, start + std::chrono::seconds(4));
        for (int second = 5; second <= 16; ++second) {
            avc.BeginGeneration(false, start + std::chrono::seconds(second));
            avc.PacketReceived(start + std::chrono::seconds(second));
        }
        QCOMPARE(avc.Check(start + std::chrono::seconds(16)), Progress::Failure::DecoderStalled);
    }
    void phoneStartupNegotiationAndCodecRetryShareOneDeadline()
    {
        using Budget = okuflow::PhoneStartupBudget;
        const auto start = Budget::TimePoint{};
        const Budget budget(start);
        QVERIFY(budget.Deadline() == start + std::chrono::seconds(12));
        QCOMPARE(budget.RemainingCameraMs(start), 11000);
        // adb/listener negotiation and a failed HEVC decoder spend the same
        // startup clock; an AVC retry cannot mint another twelve seconds.
        QCOMPARE(budget.RemainingCameraMs(start + std::chrono::milliseconds(4300)), 6700);
        QCOMPARE(budget.RemainingCameraMs(start + std::chrono::seconds(8)), 3000);
        QCOMPARE(budget.RemainingCameraMs(start + std::chrono::seconds(11)), 0);
        QCOMPARE(budget.RemainingCameraMs(start + std::chrono::seconds(15)), 0);
    }
    void codecConfigurationDoesNotConsumeTheHevcOutputDeadline()
    {
        using Progress = okuflow::PhoneFrameProgress;
        const auto start = Progress::TimePoint{};
        Progress progress(start);
        progress.BeginGeneration(true, start);
        // The phone is still trying modes. SPS/PPS is packet arrival, not
        // evidence that the system decoder has a real picture to decode.
        progress.PacketReceived(start);
        QCOMPARE(progress.Check(start + std::chrono::milliseconds(4199)), Progress::Failure::None);
        progress.BeginGeneration(true, start + std::chrono::milliseconds(4200));
        progress.PacketReceived(start + std::chrono::milliseconds(4200));
        progress.EncodedFrameReceived(start + std::chrono::milliseconds(4300));
        progress.FrameDecoded(start + std::chrono::milliseconds(4350));
        QCOMPARE(progress.Check(start + std::chrono::seconds(8)), Progress::Failure::None);

        Progress silent(start);
        silent.BeginGeneration(true, start);
        for (int second = 0; second <= 12; ++second)
            silent.PacketReceived(start + std::chrono::seconds(second));
        QCOMPARE(silent.Check(start + std::chrono::seconds(12)), Progress::Failure::HevcStalled);
    }
    void hevcOutputDeadlineStartsWithRealEncodedInput()
    {
        using Progress = okuflow::PhoneFrameProgress;
        const auto start = Progress::TimePoint{};
        Progress progress(start);
        progress.BeginGeneration(true, start);
        progress.PacketReceived(start);
        progress.EncodedFrameReceived(start + std::chrono::seconds(3));
        // More packets cannot reset the first encoded-frame clock.
        progress.EncodedFrameReceived(start + std::chrono::seconds(6));
        progress.PacketReceived(start + std::chrono::seconds(6));
        QCOMPARE(progress.Check(start + std::chrono::milliseconds(6999)), Progress::Failure::None);
        QCOMPARE(progress.Check(start + std::chrono::seconds(7)), Progress::Failure::HevcStalled);
    }
    void packetSilenceRetainsTransportFailureClassification()
    {
        using Progress = okuflow::PhoneFrameProgress;
        const auto start = Progress::TimePoint{};
        Progress progress(start);
        progress.BeginGeneration(true, start);
        progress.FrameDecoded(start);
        QCOMPARE(progress.Check(start + std::chrono::seconds(12)), Progress::Failure::TransportStalled);
    }
    void rawDeviceEntriesKeepEveryState() {
        // devices.proto ConnectionState: 1 connecting, 2 authorizing, 3 unauthorized, 6 offline, 8 device.
        for (const auto& [raw, state] : std::vector<std::pair<char, okuflow::PhoneAdbState>>{
                 {1, okuflow::PhoneAdbState::Connecting}, {2, okuflow::PhoneAdbState::Authorizing},
                 {3, okuflow::PhoneAdbState::Unauthorized}, {4, okuflow::PhoneAdbState::NoPermissions},
                 {6, okuflow::PhoneAdbState::Offline}, {8, okuflow::PhoneAdbState::Device},
                 {5, okuflow::PhoneAdbState::Other}}) {
            const char data[]{10, 11, 10, 3, 'a', 'b', 'c', 16, raw, 56, 1, 80, 9};
            const auto entries = okuflow::ParseAdbDeviceEntries(std::span(reinterpret_cast<const std::uint8_t*>(data), sizeof(data)));
            QCOMPARE(entries.size(), size_t{1});
            QCOMPARE(int(entries.front().state), int(state));
            QVERIFY(entries.front().usb);
            QCOMPARE(entries.front().transportId, std::uint64_t{9});
            // The camera list itself is unchanged: only USB + DEVICE.
            QCOMPARE(okuflow::ParseAdbUsbDevices(std::span(reinterpret_cast<const std::uint8_t*>(data), sizeof(data))).size(),
                     size_t(state == okuflow::PhoneAdbState::Device ? 1 : 0));
        }
        const auto text = okuflow::ParseAdbDevicesLongText(
            "abc unauthorized usb:1-1 transport_id:4\nxyz device product:p model:M transport_id:5\n"
            "emulator-5554 device transport_id:6\nqrs no permissions (user in plugdev group); see [x] usb:1-2\n");
        QCOMPARE(text.size(), size_t{4});
        QCOMPARE(int(text[0].state), int(okuflow::PhoneAdbState::Unauthorized));
        QVERIFY(text[0].usb); QCOMPARE(text[0].transportId, std::uint64_t{4});
        QVERIFY(!text[1].usb); // No usb: evidence.
        QVERIFY(!text[2].usb);
        QCOMPARE(int(text[3].state), int(okuflow::PhoneAdbState::NoPermissions));
    }
    void shellFactsParserIsConservative() {
        auto facts = okuflow::ParsePhoneShellFacts("W:\n  mWakefulness=Asleep\nK:\n mDreamingLockscreen=false mShowingLockscreen=true\n"
                                                   "P:\npackage:/data/app/x/base.apk\nR:\n12345\nEND\n");
        QCOMPARE(int(facts.screenAwake), int(okuflow::PhoneFact::No));
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(facts.appInstalled), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::Yes));
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\nisKeyguardShowing=false\nP:\nR:\nEND\n");
        QCOMPARE(int(facts.screenAwake), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::No));
        QCOMPARE(int(facts.appInstalled), int(okuflow::PhoneFact::No));
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::No));
        // Android 12 and later: KeyguardController and DeviceIdleController lines.
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\n  KeyguardShowing=true AodShowing=false\nP:\nR:\nEND\n");
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::Yes));
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\n  mScreenLocked=true\nP:\nR:\nEND\n");
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::Yes));
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\n  mKeyguardShowing=false\n  mScreenLocked=false\nP:\nR:\nEND\n");
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::No));
        // Truncated output (no END) never claims absence.
        facts = okuflow::ParsePhoneShellFacts("W:\nK:\nP:\nR:\n");
        QCOMPARE(int(facts.appInstalled), int(okuflow::PhoneFact::Unknown));
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::Unknown));
        QCOMPARE(int(okuflow::ParsePhoneShellFacts("").screenAwake), int(okuflow::PhoneFact::Unknown));
        QVERIFY(okuflow::PhoneSerialTag("abc").starts_with("phone-"));
        QVERIFY(okuflow::PhoneSerialTag("abc").find("abc") == std::string::npos);
    }
    void ladderUnauthorizedThenAuthorized() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::DeviceArrival);
        server.listingState.store(3);
        auto reports = LinkCycles(3);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Unauthorized));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QCOMPARE(server.ServiceCount("host:kill"), 0);
        server.listingState.store(8); // The user tapped Allow.
        reports = LinkCycles(1);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Connected));
    }
    void ladderReconnectsAnOfflineTransport() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        server.listingState.store(6);
        const auto reports = LinkCycles(6, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::Connected; });
        QCOMPARE(int(reports.front().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Offline));
        QCOMPARE(int(reports.front().decision.action), int(okuflow::PhoneLinkAction::ReconnectTransport));
        QCOMPARE(server.reconnects.load(), 1); // Targeted, although the device was not in state DEVICE.
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Connected));
    }
    void ladderWakesAndLaunchesOnALockedPhone() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Asleep\nK:\n mShowingLockscreen=true\nP:\npackage:/x\nR:\nEND\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::DeviceArrival);
        const auto reports = LinkCycles(6, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::Connected; });
        QCOMPARE(int(reports.front().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::PhoneLocked));
        QCOMPARE(int(reports.front().decision.action), int(okuflow::PhoneLinkAction::WakeAndLaunchApp));
        QVERIFY(server.ServiceCount("shell:input keyevent KEYCODE_WAKEUP") >= 1);
        QCOMPARE(server.Launches().size(), qsizetype{1});
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Connected));
    }
    void ladderManualPickAfterFailureStartsAFreshLadder() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Blocked,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mShowingLockscreen=true\nP:\npackage:/x\nR:\nEND\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(12);
        const auto launches = server.Launches().size();
        QVERIFY(launches >= 2);
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::WaitForUser)); // Budget spent: "Unlock your phone".
        reports = LinkCycles(2);
        QCOMPARE(server.Launches().size(), launches);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::ManualPick);
        reports = LinkCycles(1);
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WakeAndLaunchApp));
        QCOMPARE(server.Launches().size(), launches + 1);
    }
    void ladderNeverLaunchesForAMissingOrPausedCompanion() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\nP:\nR:\nEND\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(3);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AppNotInstalled));
        QCOMPARE(server.Launches().size(), qsizetype{0});
        FakePhoneAdb paused(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        paused.SetProblem("paused");
        ScopedLinkHooks pausedHooks(paused.port);
        reports = LinkCycles(3);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::CompanionPaused));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QCOMPARE(paused.Launches().size(), qsizetype{0});
    }
    void hostKillIsNeverSentToAUserEndpoint() {
        // Changed (iteration 2, E3): the real adb environment variable is
        // the guard, not the hook's stand-in. The hooks here allow owned
        // mode; ANDROID_ADB_SERVER_PORT names the default-port adb.exe.
        DefaultAdbFixture other({L"--fixture-exit-on-kill"});
        QVERIFY(other.copied && !other.Exited());
        qputenv("ANDROID_ADB_SERVER_PORT", QByteArray::number(other.port));
        auto restore = qScopeGuard([] { qunsetenv("ANDROID_ADB_SERVER_PORT"); });
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(25);
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::HandBackToDefault));
        QVERIFY(!other.Exited());
    }
    void blindDefaultIsShadowedThenAskedToExit() {
        // Changed (iteration 2, C1/E3): another program's adb.exe, out of
        // process; host:kill only after its exact identity and image checks.
        DefaultAdbFixture other({L"--fixture-exit-on-kill"});
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY2(reports.back().actionResult.find("exited") != std::string::npos, reports.back().actionResult.c_str());
        QVERIFY(other.child->WaitForExit(std::chrono::milliseconds(3000)));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value()); // The private server now holds USB.
    }
    void hostKillIsAbortedWhenTheRelistFails() {
        // C1: a failed fresh listing never falls through to host:kill.
        DefaultAdbFixture other({L"--fixture-exit-on-kill", L"--fixture-fail-file", L"placeholder"});
        other.child.reset();
        const QString fail = other.File(QStringLiteral("fail"));
        other.flags = {L"--fixture-exit-on-kill", L"--fixture-fail-file", fail.toStdWString()};
        QVERIFY(other.Start());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [fail](okuflow::PhoneAdbTestHooks& h) {
            h.beforeDefaultStopForTest = [fail] { QFile file(fail); file.open(QIODevice::WriteOnly); };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY2(reports.back().actionResult.find("listing failed") != std::string::npos, reports.back().actionResult.c_str());
        QTest::qWait(300);
        QVERIFY(!other.Exited());
    }
    void hostKillIsAbortedWhenThePhoneAppears() {
        // C1: the phone listed by the default between decision and request.
        DefaultAdbFixture other({L"--fixture-exit-on-kill"});
        other.child.reset();
        const QString device = other.File(QStringLiteral("device"));
        other.flags = {L"--fixture-exit-on-kill", L"--fixture-device-file", device.toStdWString()};
        QVERIFY(other.Start());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [device](okuflow::PhoneAdbTestHooks& h) {
            h.beforeDefaultStopForTest = [device] { QFile file(device); file.open(QIODevice::WriteOnly); };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY2(reports.back().actionResult.find("lists the phone") != std::string::npos, reports.back().actionResult.c_str());
        QTest::qWait(300);
        QVERIFY(!other.Exited());
    }
    void terminationDoesNotRepeatHostKill() {
        // C3: a blind default that ignores host:kill is terminated without a
        // second host:kill to the same listener.
        DefaultAdbFixture other({});
        other.child.reset();
        const QString log = other.File(QStringLiteral("kills.txt"));
        other.flags = {L"--fixture-kill-log", log.toStdWString()};
        QVERIFY(other.Start());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("terminated through its exact handle") != std::string::npos,
                 reports.back().actionResult.c_str());
        QVERIFY(other.child->WaitForExit(std::chrono::milliseconds(3000)));
        QFile kills(log);
        QVERIFY(kills.open(QIODevice::ReadOnly));
        QCOMPARE(kills.readAll().count("host:kill"), qsizetype{1});
    }
    void companionInstalledTodayWhilePausedIsLaunchedOncePerEvent() {
        // D1: no listener, the probe is refused, the app process runs.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Blocked,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false\nP:\npackage:/x\nR:\n4242\nEND\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(15);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::CompanionListenerClosed));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QCOMPARE(server.Launches().size(), qsizetype{1});
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::DeviceArrival);
        LinkCycles(3);
        QCOMPARE(server.Launches().size(), qsizetype{2});
    }
    void hangingCompanionListenerIsNamed() {
        // A15: the listener exists and accepts, but never answers.
        FakePhoneAdb server(false, true, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false\nP:\npackage:/x\nR:\n4242\nEND\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(20, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::CompanionNotResponding; });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::CompanionNotResponding));
        const auto launches = server.Launches().size();
        QVERIFY(launches <= 2);
        LinkCycles(4);
        QCOMPARE(server.Launches().size(), launches); // No relaunch loop.
    }
    void hostKillIsNeverSentToAnotherImage() {
        // C1/A6: a vendor-renamed server is never asked or terminated, and
        // the user is told at once instead of "Restoring" for ever.
        DefaultAdbFixture other({L"--fixture-exit-on-kill"}, 0, false);
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        const auto later = LinkCycles(10);
        QVERIFY(!Saw(later, okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY(!Saw(later, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QCOMPARE(int(later.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(!other.Exited());
        // A7: the refusal ends with its cause: the listener is gone.
        other.child->Stop();
        const auto after = LinkCycles(6);
        QVERIFY(after.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone);
    }
    void defaultAppearingBesideBlindOwnedServerIsUsed() {
        // A2/A8: OkuFlow's own server is blind; another adb appears on the
        // default port and lists the phone: hand back to it.
        const auto defaultPort = PickTestPort();
        ScopedLinkHooks hooks(defaultPort, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        LinkCycles(1);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        DefaultAdbFixture other({L"--fixture-device"}, defaultPort);
        QVERIFY(other.copied && !other.Exited());
        auto reports = LinkCycles(20, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::HandBackToDefault));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        reports = LinkCycles(1);
        QVERIFY(reports.back().observation.phoneState != okuflow::PhoneAdbState::NotListed);
        QVERIFY(!other.Exited());
    }
    void defaultThatComesBackAfterHostKillEndsInTheSentence() {
        // A2: Android Studio restarting its adb after every host:kill. Two
        // programs never fight without an end state.
        DefaultAdbFixture other({L"--fixture-exit-on-kill"});
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        int asks = 0, restarts = 0;
        okuflow::PhoneLinkReport last;
        for (int cycle = 0; cycle < 150; ++cycle) {
            if (other.Exited() && restarts < 4) { QVERIFY(other.Start()); ++restarts; }
            const auto reports = LinkCycles(1);
            last = reports.back();
            asks += last.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit;
            if (last.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone) break;
        }
        QCOMPARE(asks, 2);
        QCOMPARE(int(last.decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(last.decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QVERIFY(!other.Exited());
    }
    void shadowedDefaultThatLaterListsThePhoneIsHandedBack() {
        // A8: the default lists the phone after the shadow started (here
        // unauthorized): it is used, and its Allow sentence is shown.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.listEmpty.store(true);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(30, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        server.listingState.store(3);
        server.listEmpty.store(false);
        reports = LinkCycles(20, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::Unauthorized; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::HandBackToDefault));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Unauthorized));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
    }
    void stuckPhoneOnTheDefaultAtStartUpIsTakenOver() {
        // Iteration 3 (A1): the owner's scenario. Another adb on the default
        // port keeps the phone offline; OkuFlow starts and borrows it. Two
        // reconnects, then the private server beside it, then host:kill.
        DefaultAdbFixture other({});
        other.child.reset();
        const QString offline = other.File(QStringLiteral("offline"));
        { QFile file(offline); QVERIFY(file.open(QIODevice::WriteOnly)); }
        other.flags = {L"--fixture-device", L"--fixture-exit-on-kill", L"--fixture-offline-file", offline.toStdWString()};
        QVERIFY(other.Start());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(80, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::Offline));
        QCOMPARE(std::count_if(reports.begin(), reports.end(), [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::ReconnectTransport; }), std::ptrdiff_t{2});
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::HandBackToDefault));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY2(reports.back().actionResult.find("exited") != std::string::npos, reports.back().actionResult.c_str());
        QVERIFY(other.child->WaitForExit(std::chrono::milliseconds(3000)));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void stuckPhoneOnTheDefaultAfterAHandBackIsTakenOver() {
        // A1: the default that OkuFlow handed back to later keeps the phone
        // offline. The offline listing is never a reason to hand back again.
        const auto defaultPort = PickTestPort();
        ScopedLinkHooks hooks(defaultPort, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        LinkCycles(1);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        DefaultAdbFixture other({}, defaultPort);
        other.child.reset();
        const QString offline = other.File(QStringLiteral("offline"));
        other.flags = {L"--fixture-device", L"--fixture-exit-on-kill", L"--fixture-offline-file", offline.toStdWString()};
        QVERIFY(other.Start());
        auto reports = LinkCycles(20, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::HandBackToDefault));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        { QFile file(offline); QVERIFY(file.open(QIODevice::WriteOnly)); }
        reports = LinkCycles(80, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::Offline));
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::HandBackToDefault));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY(other.child->WaitForExit(std::chrono::milliseconds(3000)));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void connectStalledListenerIsUnresponsive() {
        // A3: a frozen listener with a full accept queue is a hang, not "no
        // listener": it is confirmed and acted on, not worked around.
        StalledListener stalled;
        QVERIFY(stalled.port);
        QVERIFY2(stalled.full, "the fixture listener's accept queue did not fill");
        ScopedLinkHooks hooks(stalled.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::ServerUnresponsive));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        // This process owns the fixture listener: never terminated, named.
        QVERIFY2(reports.back().actionResult.find("this process") != std::string::npos, reports.back().actionResult.c_str());
    }
    void eventDuringAHungDefaultTakeoverStillTerminates() {
        // A1: the shadow runs, the hang was confirmed, then an event: the
        // doctor's own probe confirms the hang again and termination follows.
        DefaultAdbFixture other({L"--fixture-hang"});
        QVERIFY(other.copied && !other.Exited());
        auto present = std::make_shared<std::atomic<bool>>(false);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              200, [present](okuflow::PhoneAdbTestHooks& h) {
            h.adbInterface = [present] {
                return present->load() ? okuflow::PhoneAdbInterface::Present : okuflow::PhoneAdbInterface::Absent; };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        // Without the Windows interface the hung default is never terminated (C2).
        auto reports = LinkCycles(10, [](const auto&) { return okuflow::OwnedAdbServerPort().has_value(); });
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        reports = LinkCycles(2);
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        present->store(true);
        reports = LinkCycles(10, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("terminated through its exact handle") != std::string::npos,
                 reports.back().actionResult.c_str());
        QVERIFY(other.child->WaitForExit(std::chrono::milliseconds(3000)));
    }
    void unopenableDefaultListenerIsNamedNotForgotten() {
        // A4: a listener of another user cannot be opened: "cannot stop",
        // the other-program sentence, the takeover state kept.
        DefaultAdbFixture other({L"--fixture-hang"});
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              100, [](okuflow::PhoneAdbTestHooks& h) { h.defaultListenerUnopenable = [] { return true; }; });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("cannot be opened") != std::string::npos, reports.back().actionResult.c_str());
        const auto later = LinkCycles(4);
        for (const auto& report : later)
            QCOMPARE(int(report.decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(!other.Exited());
    }
    void eventReturnsPromptlyWhileOwnedLaunchHoldsTheLock() {
        // C7: the UI thread's event never waits for a worker's slow launch.
        ScopedOwnedAdbHooks hooks({L"--fixture-hang"});
        std::atomic<bool> canceled{false};
        auto discovery = std::async(std::launch::async, [&] {
            return okuflow::PhoneCameraTransport::Enumerate({}, &canceled, nullptr, {}, nullptr,
                                                            okuflow::PhoneServerIntent::PhoneUse);
        });
        auto stop = qScopeGuard([&] { canceled.store(true); discovery.wait(); });
        QTRY_VERIFY_WITH_TIMEOUT(okuflow::OwnedAdbServerPort().has_value(), 3000);
        QVERIFY(discovery.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout); // Still launching.
        auto event = std::async(std::launch::async, [] { okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume); });
        QVERIFY2(event.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready,
                 "the event waited for the owned launch");
    }
    void ladderTargetsTheChosenPhone() {
        // C5: the connected serial is the target, also before a frame.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.secondUsbPhone.store(true);
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::ManualPick);
        okuflow::PhoneLinkRequest request;
        request.phoneWanted = true;
        request.targetSerial = "xyz";
        okuflow::PhoneLinkReport report;
        okuflow::PhoneCameraTransport::Enumerate("xyz", nullptr, nullptr, {}, nullptr,
                                                 okuflow::PhoneServerIntent::PhoneUse, &request, &report);
        QCOMPARE(QString::fromStdString(report.targetSerial), QStringLiteral("xyz"));
    }
    void firstStartWantsAnAttachedPhone() {
        // A13: no saved phone, automatic mode: an attached ADB interface
        // counts as wanting the phone; without one nothing runs.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.listingState.store(3);
        okuflow::PhoneLinkRequest request;
        request.phoneWantedIfAttached = true;
        for (const auto present : {okuflow::PhoneAdbInterface::Present, okuflow::PhoneAdbInterface::Absent}) {
            ScopedLinkHooks hooks(server.port, present);
            okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
            okuflow::PhoneLinkReport report;
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, nullptr, {}, nullptr,
                                                     okuflow::PhoneServerIntent::PhoneUse, &request, &report);
            QCOMPARE(int(report.decision.diagnosis), int(present == okuflow::PhoneAdbInterface::Present
                ? okuflow::PhoneLinkDiagnosis::Unauthorized : okuflow::PhoneLinkDiagnosis::NotWanted));
        }
    }
    void reconnectOfflineWithoutIdNeverReachesAnotherServer() {
        // C6: host:reconnect-offline affects every offline device of that
        // server: never sent to a server that is not OkuFlow's own.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.transportId.store(0);
        server.listingState.store(6);
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        const auto reports = LinkCycles(4);
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::ReconnectTransport));
        QCOMPARE(server.ServiceCount("host:reconnect-offline"), 0);
    }
    void traceCarriesNoSerialFromAdbErrors() {
        // E1: adb FAIL texts can carry the serial; the trace keeps a fixed reason.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.listingState.store(6);
        server.reconnectFailsWithSerial.store(true);
        ScopedLinkHooks hooks(server.port);
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("phone-link.log"));
        okuflow::SetPhoneLinkTraceFile(path.toStdWString());
        auto restore = qScopeGuard([] { okuflow::SetPhoneLinkTraceFile({}); });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        const auto reports = LinkCycles(2);
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::ReconnectTransport));
        QVERIFY(server.reconnects.load() >= 1);
        for (const auto& report : reports) QVERIFY(report.actionResult.find("abc") == std::string::npos);
        QVERIFY(okuflow::FlushPhoneLinkTrace(std::chrono::milliseconds(3000)));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto text = file.readAll();
        QVERIFY(text.contains("reconnect request failed"));
        QVERIFY(!text.contains("abc"));
    }
    void adbThatCannotBeLaunchedIsNamed() {
        // A16: a missing or quarantined adb.exe is said in one sentence.
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Present, L"Z:\\okuflow-missing\\adb.exe");
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        LinkCycles(2);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::ManualPick); // Renews the launch cooldown.
        const auto reports = LinkCycles(3, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart; });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
    }
    void blindDefaultServingAnotherDeviceIsNeverStopped() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.listEmpty.store(true);
        server.otherDevice.store(true);
        server.exitOnKill.store(true);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(30);
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QCOMPARE(server.ServiceCount("host:kill"), 0);
    }
    void hungDefaultIsTerminatedOnlyThroughItsExactHandle() {
        // Changed (iteration 2, E3): no image-name hook; the fixture runs as adb.exe.
        DefaultAdbFixture hung({L"--fixture-hang"});
        QVERIFY(hung.copied && !hung.Exited());
        ScopedLinkHooks hooks(hung.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("terminated through its exact handle") != std::string::npos,
                 reports.back().actionResult.c_str());
        QVERIFY(hung.child->WaitForExit(std::chrono::milliseconds(3000)));
    }
    void hungDefaultOfAnotherImageIsNeverTerminated() {
        // E3/A6: the same hung server under a name other than adb.exe is
        // never terminated; the user is told at once.
        DefaultAdbFixture hung({L"--fixture-hang"}, 0, false);
        QVERIFY(hung.copied && !hung.Exited());
        ScopedLinkHooks hooks(hung.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("not the adb server image") != std::string::npos,
                 reports.back().actionResult.c_str());
        const auto later = LinkCycles(3);
        QCOMPARE(int(later.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(!Saw(later, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY(!hung.Exited());
    }
    void hungListenerInThisProcessIsNeverTerminated() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::ServerStalled, false);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("this process") != std::string::npos, reports.back().actionResult.c_str());
    }
    void blindOwnedServerIsRestarted() {
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::DeviceArrival);
        const auto first = LinkCycles(1);
        const auto before = okuflow::OwnedAdbServerPort();
        QVERIFY(before.has_value());
        const auto reports = LinkCycles(20, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::RestartOwnedServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::RestartOwnedServer));
        QVERIFY2(reports.back().actionResult.find("restarted") != std::string::npos, reports.back().actionResult.c_str());
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(*okuflow::OwnedAdbServerPort() != *before);
    }
    void blindWithoutWindowsInterfaceNeverRestarts() {
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Absent, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12);
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::NoAdbInterface));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::RestartOwnedServer));
    }
    void resumeVoidsPreSleepTransportIdentity() {
        FakePhoneAdb server;
        auto phones = okuflow::PhoneCameraTransport::Enumerate();
        QVERIFY(!phones.empty());
        const auto before = phones.front().phoneServerEpoch;
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        phones = okuflow::PhoneCameraTransport::Enumerate();
        QVERIFY(!phones.empty());
        QVERIFY(phones.front().phoneServerEpoch > before);
    }
    void startWaitsForAListedPhoneToBecomeReady() {
        FakePhoneAdb server(false, true);
        server.listingState.store(6);
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(apartment)); QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        auto teardown = qScopeGuard([&] { MFShutdown(); CoUninitialize(); });
        okuflow::PhoneCameraTransport transport;
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        std::atomic<bool> canceled{false};
        auto result = std::async(std::launch::async, [&] { return transport.Start(phone, nullptr, {}, {}, &canceled); });
        auto cancelOnExit = qScopeGuard([&] { canceled.store(true); });
        QTest::qWait(600);
        QVERIFY(!server.hello.load());
        server.listingState.store(8); // Back from offline within the startup budget.
        QTRY_VERIFY_WITH_TIMEOUT(server.hello.load(), 3000);
        canceled.store(true);
        QVERIFY(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QVERIFY(!result.get()); transport.Stop();
    }
    void launchesRegisteredPackage_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("Play package") << static_cast<int>(FakePhoneAdb::LaunchMode::Play);
        QTest::newRow("legacy debug APK") << static_cast<int>(FakePhoneAdb::LaunchMode::Legacy);
        QTest::newRow("blocked Play app never starts legacy") << static_cast<int>(FakePhoneAdb::LaunchMode::Blocked);
    }
    void launchesRegisteredPackage() {
        QFETCH(int, mode);
        const auto policy = static_cast<FakePhoneAdb::LaunchMode>(mode);
        FakePhoneAdb server(false, true, false, policy);
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(apartment)); QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        auto teardown = qScopeGuard([&] { MFShutdown(); CoUninitialize(); });
        okuflow::PhoneCameraTransport transport;
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        std::atomic<bool> canceled{false};
        auto result = std::async(std::launch::async, [&] { return transport.Start(phone, nullptr, {}, {}, &canceled); });
        auto cancelOnExit = qScopeGuard([&] { canceled.store(true); });
        if (policy != FakePhoneAdb::LaunchMode::Blocked) QTRY_VERIFY_WITH_TIMEOUT(server.hello.load(), 3000);
        else QVERIFY(result.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        canceled.store(true);
        QVERIFY(result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QVERIFY(!result.get()); transport.Stop();
        const auto commands = server.Launches();
        QCOMPARE(commands.size(), policy == FakePhoneAdb::LaunchMode::Legacy ? 2 : 1);
        // Deliberately changed with the reconnect ladder: the launch carries the
        // host-launch extra (show over the keyguard on new companions) and is
        // preceded by a harmless wake key so a sleeping phone is not skipped.
        QCOMPARE(commands.front(), QString("shell:am start -n com.okuflow/com.okuflow.camera.MainActivity"
                                           " --ez com.okuflow.camera.extra.HOST_LAUNCH true"));
        if (commands.size() == 2) QCOMPARE(commands.back(), QString("shell:am start -n com.okuflow.camera/.MainActivity"
                                                                    " --ez com.okuflow.camera.extra.HOST_LAUNCH true"));
        const auto services = server.Services();
        const auto wake = services.indexOf(QString("shell:input keyevent KEYCODE_WAKEUP"));
        QVERIFY(wake >= 0);
        QVERIFY(wake < services.indexOf(commands.front()));
    }
    void localAdbOverridesAreRespected() {
        auto endpoint = okuflow::ParseAdbServerEndpoint("", "", "");
        QVERIFY(endpoint.supported); QVERIFY(!endpoint.overridden); QCOMPARE(endpoint.port, quint16{5037});
        endpoint = okuflow::ParseAdbServerEndpoint("tcp:localhost:5500", "9999", "remote");
        QVERIFY(endpoint.supported); QVERIFY(endpoint.overridden); QCOMPARE(endpoint.port, quint16{5500});
        QVERIFY(!okuflow::ParseAdbServerEndpoint("tcp:192.0.2.1:5037", "", "").supported);
        QVERIFY(!okuflow::ParseAdbServerEndpoint("localfilesystem:foo", "", "").supported);
        QVERIFY(!okuflow::ParseAdbServerEndpoint("", "65536", "").supported);
        QVERIFY(!okuflow::ParseAdbServerEndpoint("", "5037junk", "").supported);
        QVERIFY(!okuflow::ParseAdbServerEndpoint("", "", "192.0.2.1").supported);
    }
    void probeDoesNotRequireToken_data() {
        QTest::addColumn<bool>("old"); QTest::addColumn<bool>("busy");
        QTest::newRow("probe") << false << false;
        QTest::newRow("legacy token") << true << false;
        QTest::newRow("busy probe") << false << true;
    }
    void probeDoesNotRequireToken() {
        QFETCH(bool, old); QFETCH(bool, busy);
        FakePhoneAdb server(old, false, busy);
        bool listed = false;
        const auto phones = okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed);
        QVERIFY(listed); QCOMPARE(phones.size(), size_t{2});
        QCOMPARE(phones[0].phoneAppReady, !busy);
        QCOMPARE(phones[0].phoneBusy, busy);
        QCOMPARE(phones[1].phoneCameraId, std::string("0"));
        QVERIFY(phones[0].phoneCameraId.empty()); // separate Auto identity
        QCOMPARE(phones[1].phoneLensRole, old || busy ? std::string() : std::string("telephoto"));
        QCOMPARE(server.probes.load(), 1);
    }
    void canceledProbeKeepsListingAndStopsPromptly() {
        FakePhoneAdb server(false, true);
        std::atomic<bool> canceled{false};
        bool listed = false;
        auto result = std::async(std::launch::async, [&] { return okuflow::PhoneCameraTransport::Enumerate({}, &canceled, &listed); });
        QTRY_VERIFY_WITH_TIMEOUT(server.hello.load(), 2000);
        const auto start = std::chrono::steady_clock::now(); canceled.store(true);
        QVERIFY(result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
        const auto phones = result.get(); QVERIFY(listed); QCOMPARE(phones.size(), size_t{1});
        QVERIFY(!phones[0].phoneAppReady);
        QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    }
    void canceledStartupJoinsInsideStopDeadline() {
        FakePhoneAdb server(false, true);
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(apartment)); QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        auto teardown = qScopeGuard([&] { MFShutdown(); CoUninitialize(); });
        okuflow::PhoneCameraTransport transport;
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        std::atomic<bool> canceled{false};
        auto result = std::async(std::launch::async, [&] { return transport.Start(phone, nullptr, {}, {}, &canceled); });
        QTRY_VERIFY_WITH_TIMEOUT(server.hello.load(), 3000);
        const auto start = std::chrono::steady_clock::now(); canceled.store(true);
        QVERIFY(result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
        QVERIFY(!result.get()); transport.Stop();
        QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
        QCOMPARE(server.probes.load(), 0); // Streaming startup skips its own discovery probe.
    }
    void structuredUsbDiscoveryWorksWithoutWindowsBusPath()
    {
        const std::uint8_t data[]{10, 11, 10, 3, 'a', 'b', 'c', 16, 8, 56, 1, 80, 5,
                                 10, 9, 10, 3, 'x', 'y', 'z', 16, 8, 56, 2};
        const auto cameras = okuflow::ParseAdbUsbDevices(data);
        QCOMPARE(cameras.size(), size_t{1});
        QCOMPARE(cameras[0].phoneSerial, std::string("abc"));
        QCOMPARE(cameras[0].phoneTransportId, std::uint64_t{5});
    }
    void malformedDeviceMessagesAreRejected()
    {
        const std::uint8_t truncated[]{10, 127, 1};
        QVERIFY_EXCEPTION_THROWN(okuflow::ParseAdbUsbDevices(truncated), std::runtime_error);
        const std::uint8_t overflow[]{8, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
        QVERIFY_EXCEPTION_THROWN(okuflow::ParseAdbUsbDevices(overflow), std::runtime_error);
    }
    void systemDecoderProducesNv12FromOriginalFixture_data()
    {
        QTest::addColumn<bool>("hevc");
        QTest::newRow("H.264") << false;
        QTest::newRow("HEVC") << true;
    }
    void systemDecoderProducesNv12FromOriginalFixture()
    {
        QFETCH(bool, hevc);
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(apartment));
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        const auto codec = hevc ? okuflow::PhoneVideoCodec::Hevc : okuflow::PhoneVideoCodec::Avc;
        if (hevc && !okuflow::PhoneVideoDecoder::IsSupported(codec)) {
            MFShutdown(); CoUninitialize();
            QSKIP("Optional Windows HEVC decoder is unavailable; production advertises H.264 only");
        }
        QFile file(QFINDTESTDATA(hevc ? "data/phone-camera.hevc" : "data/phone-camera.h264"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        // Four independent access units generated from an original gray 64x48 scene.
        const QByteArray delimiter = hevc ? QByteArray("\0\0\0\1\x46\x01", 6) : QByteArray("\0\0\0\1\x09", 5);
        std::vector<int> starts;
        for (qsizetype pos = 0; (pos = bytes.indexOf(delimiter, pos)) >= 0; pos += delimiter.size()) starts.push_back(static_cast<int>(pos));
        QCOMPARE(starts.size(), size_t{4});
        starts.push_back(static_cast<int>(bytes.size()));
        int frames = 0;
        bool valid = true;
        try {
            okuflow::PhoneVideoDecoder decoder(64, 48, 30, codec);
            for (size_t index = 0; index + 1 < starts.size(); ++index) {
                const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData() + starts[index]);
                decoder.Decode(std::span(begin, static_cast<size_t>(starts[index + 1] - starts[index])),
                    static_cast<LONGLONG>(index) * 333333, [&](okuflow::MediaFrame&& frame) {
                        ++frames;
                        valid = valid && frame.width == 64 && frame.height == 48 && frame.stride == 64 &&
                            frame.subtype == MFVideoFormat_NV12 && frame.data.size() == 64 * 48 * 3 / 2 &&
                            frame.captureClock100ns > 0 && frame.data[0] >= 120 && frame.data[0] <= 135;
                    });
            }
        } catch (const std::exception& exception) {
            MFShutdown(); CoUninitialize();
            QFAIL(exception.what());
        }
        MFShutdown(); CoUninitialize();
        QVERIFY(valid);
        QVERIFY2(frames >= 3, "Low-latency decoder must produce frames without waiting for stream shutdown");
    }

    void phoneDescriptorDoesNotChangeSavedWebcamSelection()
    {
        okuflow::CameraDescriptor webcam, phone;
        webcam.symbolicLink = L"webcam-fixture";
        phone.symbolicLink = L"okuflow-phone:fixture";
        phone.phoneSerial = "fixture";
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"webcam-fixture", 1), 0);
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"okuflow-phone:fixture", 0), 1);
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"okuflow-phone:fixture/2", 0), 1);
    }
    void firstRunPrefersOnlyAReadyPhone()
    {
        okuflow::CameraDescriptor webcam, phone;
        webcam.symbolicLink = L"webcam-fixture";
        phone.symbolicLink = L"okuflow-phone:fixture";
        phone.phoneSerial = "fixture";
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"", -1), 0);
        phone.phoneAppReady = true;
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"", -1), 1);
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"webcam-fixture", 0), 0);
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"", 0), 0);
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"okuflow-phone:fixture", 0), 1);
    }

    void delayedPhoneArrivalKeepsTheSavedIntent()
    {
        okuflow::CameraDescriptor webcam, phone;
        webcam.symbolicLink = L"webcam-fixture";
        phone.symbolicLink = L"okuflow-phone:fixture";
        phone.phoneSerial = "fixture";
        const std::wstring saved = phone.symbolicLink;
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam}, saved, 0), 0);
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam}, saved, true), -1);
        // Same saved phone is retried even before its app listener is ready.
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, saved, true), 1);
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, saved, true, true), -1);
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, saved + L"/2", true), 1);
        // A deliberate webcam selection made while discovery was in flight wins.
        phone.phoneAppReady = true;
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, saved, false), -1);
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, L"webcam-fixture", false), -1);
    }

    void anotherPhoneCanReplaceAnUnavailablePhoneButFirstRunNeedsReadiness()
    {
        okuflow::CameraDescriptor webcam, phone;
        webcam.symbolicLink = L"webcam-fixture";
        phone.symbolicLink = L"okuflow-phone:replacement";
        phone.phoneSerial = "replacement";
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, L"", true), -1);
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, L"okuflow-phone:missing", true), -1);
        phone.phoneAppReady = true;
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, L"", true), 1);
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex({webcam, phone}, L"okuflow-phone:missing", true), 1);
        QCOMPARE(okuflow::ResolveSavedCameraIndex({webcam, phone}, L"okuflow-phone:missing", 0), 1);
    }

    void discoveryPreservesWebcamOrderAndBusyPhoneLensIdentity()
    {
        okuflow::CameraDescriptor first, second, phone, lens, vanished;
        first.symbolicLink = L"first-webcam";
        second.symbolicLink = L"second-webcam";
        phone.phoneSerial = "fixture";
        phone.symbolicLink = L"okuflow-phone:fixture";
        phone.phoneTransportId = 1;
        lens = phone;
        lens.phoneCameraId = "2";
        lens.symbolicLink += L"/2";
        vanished.phoneSerial = "vanished";
        vanished.symbolicLink = L"okuflow-phone:vanished";
        auto current = phone;
        current.phoneTransportId = 37; // A USB replug changes the transport, not identity.
        auto merged = okuflow::MergePhoneCameraDiscovery(
            {first, second, phone, lens, vanished}, {current}, L"");
        QCOMPARE(merged.size(), size_t{4});
        QCOMPARE(merged[0].symbolicLink, first.symbolicLink);
        QCOMPARE(merged[1].symbolicLink, second.symbolicLink);
        QCOMPARE(merged[3].symbolicLink, lens.symbolicLink);
        QCOMPARE(merged[3].phoneTransportId, std::uint64_t{37});
        QVERIFY(!merged[3].phoneAppReady);
        // A transient empty snapshot does not erase a camera still delivering frames.
        merged = okuflow::MergePhoneCameraDiscovery(merged, {}, lens.symbolicLink);
        QCOMPARE(merged.size(), size_t{4});
        // Once capture is stopped, a real absence removes its stale entries.
        merged = okuflow::MergePhoneCameraDiscovery(merged, {}, L"");
        QCOMPARE(merged.size(), size_t{2});
    }
};
QTEST_GUILESS_MAIN(PhoneCameraTests)
#include "phone_camera_tests.moc"
