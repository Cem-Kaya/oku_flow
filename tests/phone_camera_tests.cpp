#include "okuflow/capture/phone_camera.hpp"
#include "okuflow/capture/phone_frame_progress.hpp"
#include "okuflow/capture/adb_owned_server.hpp"
#include <QtTest/QtTest>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QScopeGuard>
#include <QtEndian>
#include <future>
#include <thread>
#include <atomic>
#include <cstring>
#include <mutex>

namespace {
// Private random port, no bootstrap: these fixtures can never reach a real adb or phone.
class FakePhoneAdb {
public:
    enum class LaunchMode { None, Play, Legacy, Blocked };
    enum class RecoveryMode { Healthy, Stalled, ServerStalled, SelectionStalled, Replugged,
                              Unsupported, LegacyListing, Unauthorized, Wifi,
                              PartialEcho, SlowHealthy, AckWithoutEffect, CompanionBusy };
    explicit FakePhoneAdb(bool old = false, bool stall = false, bool busy = false,
                         LaunchMode mode = LaunchMode::None, RecoveryMode recovery = RecoveryMode::Healthy,
                         bool installHooks = true, quint16 listenPort = 0)
        : deviceStalled(recovery != RecoveryMode::Healthy && recovery != RecoveryMode::SlowHealthy),
          old(old), stall(stall), busy(busy),
          launchMode(mode), recoveryMode(recovery), installedHooks(installHooks),
          hostUnresponsive(recovery == RecoveryMode::ServerStalled) {
        std::promise<quint16> promise;
        auto future = promise.get_future();
        worker = std::thread([this, listenPort, promise = std::move(promise)]() mutable {
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, listenPort)) { promise.set_value(0); return; }
            promise.set_value(server.serverPort());
            while (!stopped.load()) {
                if (!server.waitForNewConnection(20)) continue;
                std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
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
    std::atomic<int> reconnects{0}, healthProbes{0};
    std::atomic<std::uint64_t> transportId{9};
    std::atomic<bool> deviceStalled{false};
    std::atomic<bool> hostUnresponsive{false};
    QStringList Services() {
        std::lock_guard lock(launchMutex);
        return services;
    }
    QStringList Launches() {
        std::lock_guard lock(launchMutex);
        return launches;
    }
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
        if (!socket.waitForReadyRead(100)) return;
        const auto size = Read(socket, 4).toInt(nullptr, 16);
        const auto service = Read(socket, size);
        { std::lock_guard lock(launchMutex); services.append(QString::fromUtf8(service)); }
        const auto waitForClose = [&] {
            while (!stopped.load() && socket.state() == QAbstractSocket::ConnectedState)
                socket.waitForReadyRead(20);
        };
        if (hostUnresponsive.load()) { waitForClose(); return; }
        if (service == "host:version") { Write(socket, "OKAY00040029"); return; }
        if (service == "host-transport-id:" + QByteArray::number(transportId.load()) + ":reconnect") {
            ++reconnects;
            if (recoveryMode == RecoveryMode::Unsupported) { Write(socket, "FAIL000bunsupported"); return; }
            const QByteArray reply("reconnecting abc [device]\n");
            // Acknowledged but never re-registers: recovery must not call this success.
            if (recoveryMode != RecoveryMode::AckWithoutEffect) { ++transportId; deviceStalled.store(false); }
            Write(socket, "OKAY" + QByteArray::number(reply.size(), 16).rightJustified(4, '0') + reply);
            return;
        }
        if (service == "host:track-devices-proto-binary") {
            if (recoveryMode == RecoveryMode::LegacyListing) { Write(socket, "FAIL000bunsupported"); return; }
            const char data[]{10, 11, 10, 3, 'a', 'b', 'c', 16, 8, 56, 1, 80, 9};
            QByteArray body(data, sizeof(data));
            body[sizeof(data) - 1] = static_cast<char>(transportId.load());
            if (recoveryMode == RecoveryMode::Unauthorized) body[8] = 3;
            if (recoveryMode == RecoveryMode::Wifi) body[10] = 2;
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
            if (recoveryMode == RecoveryMode::PartialEcho) { Write(socket, "O"); waitForClose(); }
            else if (recoveryMode == RecoveryMode::SlowHealthy) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                Write(socket, "OKUFLOW_USB_HEALTH\n");
            }
            else if (deviceStalled.load()) waitForClose();
            else Write(socket, "OKUFLOW_USB_HEALTH\n");
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
        const QByteArray reply = old ? QByteArray(R"({"protocol":1,"token":"old-token","cameras":[{"id":"0"}]})") :
            (busy || recoveryMode == RecoveryMode::CompanionBusy) ? QByteArray(R"({"protocol":1,"probe":true,"busy":true,"problem":null,"cameras":[{"id":"0"}]})") :
                   QByteArray(R"({"protocol":1,"probe":true,"busy":false,"problem":null,"cameras":[{"id":"0"}]})");
        QByteArray packet(8, 0);
        qToLittleEndian<quint16>(1, packet.data());
        qToLittleEndian<quint32>(static_cast<quint32>(reply.size()), packet.data() + 4);
        Write(socket, packet + reply);
    }
    bool old, stall, busy;
    LaunchMode launchMode;
    RecoveryMode recoveryMode;
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
        auto child = okuflow::OwnedChildProcess::Launch({fixture.toStdWString(), arguments, {}, {}});
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

class PhoneCameraTests : public QObject {
    Q_OBJECT
private slots:
    void ownedChildStopsExactlyOnStopAndScopeExit() {
        const QString fixture = OwnedChildFixturePath();
        QVERIFY2(!fixture.isEmpty() && QFile::exists(fixture), "Owned child fixture must be built");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ready = directory.filePath(QStringLiteral("ready.txt"));
        unsigned long pid = 0;
        {
            auto child = okuflow::OwnedChildProcess::Launch(
                {fixture.toStdWString(), {L"sleep", ready.toStdWString()}, {}, {}});
            QVERIFY(child);
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
            auto child = okuflow::OwnedChildProcess::Launch(
                {fixture.toStdWString(), {L"sleep", ready2.toStdWString()}, {}, {}});
            QVERIFY(child);
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
        QVERIFY(freshPid.load() != 0);
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
    void canceledUsbRecoveryStopsPromptly() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Stalled);
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        std::atomic<bool> canceled{false};
        auto result = std::async(std::launch::async, [&] {
            return okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(phone, &canceled);
        });
        auto cancelOnExit = qScopeGuard([&] { canceled.store(true); });
        QTRY_VERIFY_WITH_TIMEOUT(server.healthProbes.load() == 1, 1000);
        canceled.store(true);
        QVERIFY(result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
        QVERIFY(!result.get()); QCOMPARE(server.reconnects.load(), 0);
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
    void packetSilenceRetainsTransportFailureClassification()
    {
        using Progress = okuflow::PhoneFrameProgress;
        const auto start = Progress::TimePoint{};
        Progress progress(start);
        progress.BeginGeneration(true, start);
        progress.FrameDecoded(start);
        QCOMPARE(progress.Check(start + std::chrono::seconds(12)), Progress::Failure::TransportStalled);
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
        QCOMPARE(commands.front(), QString("shell:am start -n com.okuflow/com.okuflow.camera.MainActivity"));
        if (commands.size() == 2) QCOMPARE(commands.back(), QString("shell:am start -n com.okuflow.camera/.MainActivity"));
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
