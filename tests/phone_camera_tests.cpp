#include "okuflow/capture/phone_camera.hpp"
#include "okuflow/capture/phone_frame_progress.hpp"
#include "okuflow/capture/phone_startup_budget.hpp"
#include "okuflow/capture/adb_owned_server.hpp"
#include "okuflow/capture/adb_legacy_recovery.hpp"
#include "phone_adb_test_guard.hpp"
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
#include <QJsonArray>
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
    // Paused: `am start` succeeds but the listener stays closed (a paused
    // companion installed today).
    enum class LaunchMode { None, Play, Legacy, Blocked, Paused };
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
            const auto listenPortNow = server.serverPort();
            // A host client can wait for the result of a second connection
            // before it sends its first request. Keep idle sockets pending
            // while the fake accepts and serves the listing connection.
            std::vector<std::unique_ptr<QTcpSocket>> idle;
            while (!stopped.load() && !closeListener.load()) {
                PumpStream();
                std::unique_ptr<QTcpSocket> socket;
                std::erase_if(idle, [&](auto& waiting) {
                    if (!socket && (waiting->bytesAvailable() || waiting->waitForReadyRead(1)))
                        socket = std::move(waiting);
                    return !waiting || waiting->state() != QAbstractSocket::ConnectedState;
                });
                bool reopen = false;
                if (!socket) {
                    if (!server.hasPendingConnections() &&
                        !server.waitForNewConnection(streamControl || !idle.empty() ? 5 : 20)) continue;
                    socket.reset(server.nextPendingConnection());
                    // Gone from the listener table before it answers this request.
                    const bool gone = goneAtNextConnection.exchange(false);
                    if (gone) {
                        server.close();
                        reopen = reopenAfterGone.load();
                        if (!reopen) closeListener.store(true);
                    }
                    if (arrival == ArrivalMode::BufferedRequest) {
                        if (!socket->bytesAvailable() && !socket->waitForReadyRead(1000)) continue;
                        ++bufferedRequests;
                    } else if (!gone && !socket->bytesAvailable() && !socket->waitForReadyRead(1)) {
                        if (socket->state() == QAbstractSocket::ConnectedState) idle.push_back(std::move(socket));
                        continue;
                    }
                }
                try { Serve(*socket); } catch (...) {}
                // A streaming companion's channels stay open beside the
                // requests that follow (served by this same thread).
                if (keepRole == 1) { streamControl = std::move(socket); streamVideo.reset(); streamStarted = false; controlBuffer.clear(); }
                else if (keepRole == 2) streamVideo = std::move(socket);
                keepRole = 0;
                // An adb the lookup cannot identify: gone at every ask, back
                // at the next listing.
                if (reopen) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (!server.listen(QHostAddress::LocalHost, listenPortNow)) closeListener.store(true);
                }
            }
            streamVideo.reset();
            streamControl.reset();
        });
        port = future.get();
        if (!port) throw std::runtime_error("Cannot listen on test socket");
        if (installedHooks) okuflow::SetPhoneAdbTestHooks(okuflow::PhoneAdbTestHooks{port, {}});
    }
    ~FakePhoneAdb() {
        stopped.store(true);
        worker.join();
        if (installedHooks) okuflow_test::UseClosedAdbPort();
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
    // The next connection is still answered, but the listener closes first.
    std::atomic<bool> goneAtNextConnection{false};
    // ... and listens again on the same port after answering it.
    std::atomic<bool> reopenAfterGone{false};
    // Iteration 8: both listing forms 0 answer, 1 hang (never answered),
    // 2 answer FAIL, 3 close the socket at once; host:version still answers.
    std::atomic<int> listingMode{0};
    // Invoked when the fake has prepared a protobuf listing.
    void SetBeforeListingReply(std::function<void()> callback) {
        std::lock_guard lock(launchMutex);
        beforeListingReply = std::move(callback);
    }
    // An older adb: FAIL to the protobuf listing, the text one answers
    // (empty while listEmpty).
    std::atomic<bool> textOnly{false};
    // A second USB phone "xyz" in state DEVICE (otherDevice is a Wi-Fi one).
    std::atomic<bool> secondUsbPhone{false};
    // Same transport ID, but the only USB phone is now "xyz".
    std::atomic<bool> replacementPhone{false};
    void SetOnHostKill(std::function<void()> callback) {
        std::lock_guard lock(launchMutex);
        onHostKill = std::move(callback);
    }
    void SetBeforeTransportReply(std::function<void()> callback) {
        std::lock_guard lock(launchMutex);
        beforeTransportReply = std::move(callback);
    }
    // adb FAIL texts often carry the serial: the trace must not.
    std::atomic<bool> reconnectFailsWithSerial{false};
    // After round 10: a companion that streams. A session's control hello
    // is answered with a token, its video channel is bound, and after the
    // start command the access units given here (H.264, from
    // data/phone-camera.h264) are sent in a loop at about 30 frames per
    // second. Discovery's probes are answered as before.
    std::atomic<bool> streaming{false};
    std::vector<QByteArray> accessUnits;
    std::atomic<int> videoPacketsSent{0};
    // The companion's listener is closed (adbd refuses it at once).
    std::atomic<bool> companionRefuses{false};
    std::atomic<int> launchReplyDelayMs{0};
    std::string problemCode;
    bool problemAsHelloError{false};
    std::function<void()> beforeListingReply, onHostKill, beforeTransportReply;
    std::string shellFactsLater;
    std::string shellFacts{"W:\n  mWakefulness=Awake\nK:\n mShowingLockscreen=false\nP:\npackage:/data/app/base.apk\nR:\nOKUFLOW_FACTS_END\n"};
    QStringList Services() {
        std::lock_guard lock(launchMutex);
        return services;
    }
    QStringList Launches() {
        std::lock_guard lock(launchMutex);
        return launches;
    }
    // `later` is written 300 ms after `facts`, as a second chunk.
    void SetShellFacts(std::string facts, std::string later = {}) {
        std::lock_guard lock(launchMutex); shellFacts = std::move(facts); shellFactsLater = std::move(later); }
    void SetProblem(std::string code, bool helloError = false) {
        std::lock_guard lock(launchMutex);
        problemCode = std::move(code);
        problemAsHelloError = helloError;
    }
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
    void WritePacket(QTcpSocket& socket, quint16 type, const QByteArray& body) {
        QByteArray packet(8, 0);
        qToLittleEndian<quint16>(type, packet.data());
        qToLittleEndian<quint32>(static_cast<quint32>(body.size()), packet.data() + 4);
        Write(socket, packet + body);
    }
    // The streaming companion, between requests on the worker thread:
    // commands are read (a start is answered with the stream's
    // configuration) and the next access unit is sent when it is due.
    void PumpStream() {
        if (!streamControl) return;
        if (!streamControl->bytesAvailable()) streamControl->waitForReadyRead(1);
        if (streamControl->state() != QAbstractSocket::ConnectedState) {
            streamVideo.reset(); streamControl.reset(); streamStarted = false; controlBuffer.clear();
            return;
        }
        controlBuffer += streamControl->readAll();
        while (controlBuffer.size() >= 8) {
            const auto size = qFromLittleEndian<quint32>(controlBuffer.constData() + 4);
            if (controlBuffer.size() < 8 + static_cast<qsizetype>(size)) break;
            const auto body = controlBuffer.mid(8, size);
            controlBuffer.remove(0, 8 + size);
            if (body.contains(R"("op":"start")") && !streamStarted) {
                streamStarted = true;
                streamSequence = 0;
                streamTimestampUs = 0;
                WritePacket(*streamControl, 3, R"({"event":"configured","width":64,"height":48,"fps":30,"mime":"video/avc","generation":1})");
            }
        }
        if (!streamStarted || !streamVideo || accessUnits.empty()) return;
        if (streamVideo->state() != QAbstractSocket::ConnectedState) { streamVideo.reset(); return; }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStreamFrame < std::chrono::milliseconds(33)) return;
        lastStreamFrame = now;
        QByteArray data(20, 0);
        qToLittleEndian<qint64>(streamTimestampUs, data.data());
        qToLittleEndian<qint64>(streamSequence, data.data() + 8);
        qToLittleEndian<quint32>(1, data.data() + 16);
        data += accessUnits[static_cast<size_t>(streamSequence) % accessUnits.size()];
        ++streamSequence;
        streamTimestampUs += 33333;
        WritePacket(*streamVideo, 6, data);
        ++videoPacketsSent;
    }
    int keepRole{0};
    std::unique_ptr<QTcpSocket> streamControl, streamVideo;
    QByteArray controlBuffer;
    bool streamStarted{false};
    qint64 streamSequence{0}, streamTimestampUs{0};
    std::chrono::steady_clock::time_point lastStreamFrame{};
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
            { std::lock_guard lock(launchMutex); if (onHostKill) onHostKill(); }
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
        if (service == "host:track-devices-proto-binary" || service == "host:devices-l") {
            const int mode = listingMode.load();
            if (mode == 1) { waitForClose(); return; }
            if (mode == 2) { Write(socket, "FAIL0006broken"); return; }
            if (mode == 3) return; // Closed without a byte.
        }
        if (service == "host:track-devices-proto-binary") {
            if (recoveryMode == RecoveryMode::LegacyListing || textOnly.load()) { Write(socket, "FAIL000bunsupported"); return; }
            const char data[]{10, 11, 10, 3, 'a', 'b', 'c', 16, 8, 56, 1, 80, 9};
            QByteArray body(data, sizeof(data));
            body[sizeof(data) - 1] = static_cast<char>(transportId.load());
            if (replacementPhone.load()) body.replace(4, 3, "xyz");
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
            { std::lock_guard lock(launchMutex); if (beforeListingReply) beforeListingReply(); }
            Write(socket, "OKAY" + QByteArray::number(body.size(), 16).rightJustified(4, '0') + body);
            return;
        }
        if (service == "host:devices-l") {
            const QByteArray reply = textOnly.load() && listEmpty.load() ? QByteArray() :
                QByteArray("abc device usb:1-1 model:Fixture transport_id:" + QByteArray::number(transportId.load()) + "\n");
            Write(socket, "OKAY" + QByteArray::number(reply.size(), 16).rightJustified(4, '0') + reply);
            return;
        }
        if (!service.startsWith("host:transport")) return;
        if (replacementPhone.load() && service == "host:transport:abc") {
            Write(socket, "FAIL0007missing");
            return;
        }
        { std::lock_guard lock(launchMutex); if (beforeTransportReply) beforeTransportReply(); }
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
            std::string facts, later;
            { std::lock_guard lock(launchMutex); facts = shellFacts; later = shellFactsLater; }
            Write(socket, QByteArray::fromStdString(facts));
            if (!later.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                Write(socket, QByteArray::fromStdString(later));
            }
            return;
        }
        if (next.startsWith("shell:am start")) {
            { std::lock_guard lock(launchMutex); launches.append(QString::fromUtf8(next)); }
            Write(socket, "OKAY");
            if (launchMode == LaunchMode::Blocked) Write(socket, "Error: Activity start blocked");
            else if (launchMode == LaunchMode::Paused) Write(socket, "Starting: Intent");
            else if (launchMode == LaunchMode::Legacy && !next.contains("com.okuflow.camera/.MainActivity"))
                Write(socket, "Error type 3\nError: Activity class does not exist.");
            else {
                launchReady = true;
                if (const auto delay = launchReplyDelayMs.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                Write(socket, "Starting: Intent");
            }
            return;
        }
        if (next != "localabstract:okuflow_camera") return;
        if (companionRefuses.load() || (launchMode != LaunchMode::None && !launchReady)) {
            const QByteArray message("listener absent");
            Write(socket, "FAIL" + QByteArray::number(message.size(), 16).rightJustified(4, '0') + message);
            return;
        }
        Write(socket, "OKAY");
        const auto header = Read(socket, 12);
        const auto body = Read(socket, static_cast<int>(qFromLittleEndian<quint32>(header.constData() + 8)));
        if (body.contains("probe")) ++probes;
        hello.store(true);
        if (streaming.load() && !body.contains("probe")) {
            // Role 1: the session's control channel; role 2: its video channel.
            keepRole = qFromLittleEndian<quint16>(header.constData() + 6) == 2 ? 2 : 1;
            WritePacket(socket, 1, keepRole == 2 ? QByteArray(R"({"channel":"video"})")
                                                 : QByteArray(R"({"protocol":1,"token":"stream","cameras":[{"id":"0"}]})"));
            return;
        }
        // A stalled transport stalls the companion channel too, except when the
        // fixture models a session another host claimed in the meantime.
        if (stall || (deviceStalled.load() && recoveryMode != RecoveryMode::CompanionBusy)) {
            while (!stopped.load() && socket.state() == QAbstractSocket::ConnectedState) socket.waitForReadyRead(20);
            return;
        }
        std::string problem;
        bool helloError = false;
        { std::lock_guard lock(launchMutex); problem = problemCode; helloError = problemAsHelloError; }
        if (!problem.empty()) {
            const auto code = QString::fromStdString(problem);
            const QJsonObject reply = helloError
                ? QJsonObject{{"error", "fixture problem"}, {"code", code}}
                : QJsonObject{{"protocol", 1}, {"probe", true}, {"busy", false}, {"problem", code},
                              {"cameras", QJsonArray{QJsonObject{{"id", "0"}}}}};
            WritePacket(socket, helloError ? 5 : 1, QJsonDocument(reply).toJson(QJsonDocument::Compact));
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
// Observes the fixture's siblings until `wanted` of them are seen. One look is
// not enough on every machine: ObserveOkuFlowSiblings skips a candidate whose
// image file cannot be opened at that instant, and these rows observe a
// freshly copied executable that a security scan may hold for a moment (a
// hosted Windows runner observed none right after the fixture wrote its ready
// file). Production observes again on every discovery cycle.
std::vector<okuflow::OkuFlowSiblingObservation> ObserveSiblingsEventually(const std::wstring& image,
                                                                           const std::wstring& tag, size_t wanted,
                                                                           int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto siblings = okuflow::ObserveOkuFlowSiblings(image, tag);
    while (siblings.size() < wanted && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        siblings = okuflow::ObserveOkuFlowSiblings(image, tag);
    }
    return siblings;
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
    ~ScopedOwnedAdbHooks() { okuflow_test::UseClosedAdbPort(); }
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
    policy.unknownInterfaceBlindMs = 300;
    policy.companionFailedGraceMs = 300;
    policy.launchSettleMs = 150;
    return policy;
}
// Owned-server budgets short enough that several start graces fit in a row
// (production: 30 s grace, 5 s readiness, 5 s cooldown doubling to 30 s).
okuflow::OwnedAdbServerPolicy FastOwnedPolicy()
{
    okuflow::OwnedAdbServerPolicy owned;
    owned.startGrace = std::chrono::milliseconds(300);
    owned.readinessDeadline = std::chrono::milliseconds(200);
    owned.restartCooldown = std::chrono::milliseconds(100);
    owned.maxRestartCooldown = std::chrono::milliseconds(200);
    owned.confirmationSpacing = std::chrono::milliseconds(10);
    return owned;
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
    ~ScopedLinkHooks() { okuflow_test::UseClosedAdbPort(); }
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
    const std::function<bool(const okuflow::PhoneLinkReport&)>& until = {}, const std::string& serial = "abc",
    const okuflow::PhoneLinkRequest* base = nullptr)
{
    std::vector<okuflow::PhoneLinkReport> reports;
    okuflow::PhoneLinkRequest request;
    if (base) request = *base;
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
// The owned-child fixture started with --fixture-exit-on-kill exits on a
// host:kill: the test ends OkuFlow's own (fixture) child after its start, as
// security software that kills adb.exe would. Waits until it is gone.
void SendTestHostKill(std::uint16_t port)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (socket.waitForConnected(1000)) {
        socket.write("0009host:kill");
        socket.waitForBytesWritten(1000);
        socket.waitForReadyRead(3000);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (okuflow::OwnedAdbServerPort() == port && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
bool Saw(const std::vector<okuflow::PhoneLinkReport>& reports, okuflow::PhoneLinkAction action)
{
    return std::any_of(reports.begin(), reports.end(), [&](const auto& r) { return r.decision.action == action; });
}
bool Saw(const std::vector<okuflow::PhoneLinkReport>& reports, okuflow::PhoneLinkDiagnosis diagnosis)
{
    return std::any_of(reports.begin(), reports.end(), [&](const auto& r) { return r.decision.diagnosis == diagnosis; });
}
// The four access units of an Annex B H.264 test file (split at the access
// unit delimiters), for the streaming FakePhoneAdb.
std::vector<QByteArray> H264AccessUnits(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    const QByteArray delimiter("\0\0\0\1\x09", 5);
    std::vector<qsizetype> starts;
    for (qsizetype pos = 0; (pos = bytes.indexOf(delimiter, pos)) >= 0; pos += delimiter.size()) starts.push_back(pos);
    starts.push_back(bytes.size());
    std::vector<QByteArray> units;
    for (size_t index = 0; index + 1 < starts.size(); ++index)
        units.push_back(bytes.mid(starts[index], starts[index + 1] - starts[index]));
    return units;
}
// COM and Media Foundation for a real phone session's decoder (production:
// the capture code starts them).
struct ScopedMediaFoundation {
    ScopedMediaFoundation()
        : com(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))), mf(SUCCEEDED(MFStartup(MF_VERSION))) {}
    ~ScopedMediaFoundation() { if (mf) MFShutdown(); if (com) CoUninitialize(); }
    bool com{false};
    bool mf{false};
};
okuflow::CameraDescriptor FixturePhone()
{
    okuflow::CameraDescriptor phone;
    phone.phoneSerial = "abc";
    phone.symbolicLink = L"okuflow-phone:abc";
    return phone;
}
} // namespace

class PhoneCameraTests : public QObject {
    Q_OBJECT
private slots:
    // Iteration 7 (4): no test of this target can reach the machine's
    // default adb port (see phone_adb_test_guard.hpp).
    void initTestCase() { okuflow_test::UseClosedAdbPort(); }
    void aTestWithoutHooksReachesOnlyTheClosedPort() {
        // Every scoped fixture restores the closed-port default instead of no
        // hooks. ADB_SERVER_SOCKET names a decoy listener here, so even
        // without any hooks (as when this row fails) discovery goes there,
        // never to port 5037.
        okuflow_test::LoopbackListener decoy;
        QVERIFY(decoy.Listening());
        qputenv("ADB_SERVER_SOCKET", QByteArray("tcp:127.0.0.1:") + QByteArray::number(decoy.Port()));
        auto restore = qScopeGuard([] { qunsetenv("ADB_SERVER_SOCKET"); });
        { ScopedLinkHooks scoped(PickTestPort()); } // A test's own hooks come and go.
        okuflow_test::LoopbackListener closed(okuflow_test::ClosedAdbPort());
        QVERIFY(closed.Listening());
        bool listed = false;
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, okuflow::PhoneServerIntent::BorrowOnly);
        QVERIFY(closed.Accepted(1000));
        QVERIFY(!decoy.Accepted(0));
        QVERIFY(!listed);
    }
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
        // Changed deliberately (iteration 3, C2): 20 s, above the longest
        // bounded critical teardown that runs before the exit signal.
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 19999)), int(V::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(hidden, track, 20000)), int(V::Leftover));
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
        const auto siblings = ObserveSiblingsEventually(image.toStdWString(), tag, 3);
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
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(w, tw, 20000)), int(V::Leftover));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(u, tu, 20000)), int(V::UnregisteredWindowless));
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
        // Iteration 3 (C5): shutdown cancels the step before anything stops.
        const std::atomic<bool> shuttingDown{true};
        QVERIFY(!okuflow::StopLeftoverAdbChild(parent->ProcessId(), parentCreation, image.toStdWString(),
                                               std::chrono::milliseconds(2000), &shuttingDown));
        QVERIFY(!FixtureProcessExited(childPid));
        QVERIFY(okuflow::StopLeftoverAdbChild(parent->ProcessId(), parentCreation, image.toStdWString()));
        QVERIFY(WaitFixtureProcessExited(childPid));
        QVERIFY(parent->IsRunning());
    }
    void leftoverThatCannotBeOpenedStopsNothing() {
        // Iteration 3 (C2): the leftover is gone (its PID cannot be opened):
        // whoever is a child of that PID is never walked or stopped.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const QString childReady = directory.filePath(QStringLiteral("child.txt"));
        const QString parentReady = directory.filePath(QStringLiteral("parent.txt"));
        std::wstring command = L"\"" + image.toStdWString() + L"\" spawn \"" + childReady.toStdWString() + L"\" \"" +
                               parentReady.toStdWString() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        QVERIFY(CreateProcessW(image.toStdWString().c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
        FILETIME created{}, exited{}, kernel{}, user{};
        QVERIFY(GetProcessTimes(process.hProcess, &created, &exited, &kernel, &user));
        const std::uint64_t creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        QVERIFY(WaitForFixtureFile(parentReady) && WaitForFixtureFile(childReady));
        QCOMPARE(WaitForSingleObject(process.hProcess, 3000), DWORD{WAIT_OBJECT_0});
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        const auto childPid = ReadFixturePid(childReady);
        QVERIFY(childPid);
        okuflow::LeftoverInstanceStopRequest request{process.dwProcessId, creation, image.toStdWString(), image.toStdWString()};
        const auto status = okuflow::StopLeftoverOkuFlowInstance(request);
        QVERIFY(status != okuflow::LeftoverInstanceStopStatus::AdbChildStopped);
        QVERIFY(!FixtureProcessExited(childPid));
        QVERIFY(!okuflow::StopLeftoverAdbChild(process.dwProcessId, creation, image.toStdWString()));
        QVERIFY(!FixtureProcessExited(childPid));
        QVERIFY(WaitFixtureProcessExited(childPid, 6000)); // Ends by itself; the folder can then be removed.
    }
    void leftoverWithAWindowNowIsNotStopped() {
        // C2: the visible-window check is repeated right before termination.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const std::wstring tag = L"window" + std::to_wstring(GetCurrentProcessId());
        const QString ready = directory.filePath(QStringLiteral("a.txt"));
        std::string error;
        auto instance = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"instance", tag, L"1", ready.toStdWString()}, {}, {}}, &error);
        QVERIFY2(instance, error.c_str());
        QVERIFY(WaitForFixtureFile(ready));
        const auto siblings = ObserveSiblingsEventually(image.toStdWString(), tag, 1);
        QCOMPARE(siblings.size(), size_t{1});
        okuflow::LeftoverInstanceStopRequest request{siblings[0].processId, siblings[0].creation, image.toStdWString(), {}};
        request.visibleWindowForTest = [](DWORD) { return true; };
        QCOMPARE(int(okuflow::StopLeftoverOkuFlowInstance(request)), int(okuflow::LeftoverInstanceStopStatus::IdentityChanged));
        QVERIFY(instance->IsRunning());
    }
    void leftoverIsJudgedByItsUiThread() {
        // Changed deliberately (iteration 4, A7; was
        // leftoverIsJudgedByEveryTopLevelWindow): both fixture instances have
        // a hidden window that pumps (main thread) and one that never pumps
        // (helper thread). The registered UI thread decides: a pumping
        // helper does not vouch for a stuck UI (iteration 3, C2), and a
        // helper that never pumps does not make a healthy UI a leftover.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const std::wstring tag = L"windows" + std::to_wstring(GetCurrentProcessId());
        const QString a = directory.filePath(QStringLiteral("a.txt")), b = directory.filePath(QStringLiteral("b.txt"));
        std::string error;
        auto stuck = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"instance-windows", tag, L"1", a.toStdWString()}, {}, {}}, &error);
        auto alive = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"instance-windows", tag, L"0", b.toStdWString()}, {}, {}}, &error);
        QVERIFY2(stuck && alive, error.c_str());
        QVERIFY(WaitForFixtureFile(a) && WaitForFixtureFile(b));
        const auto siblings = ObserveSiblingsEventually(image.toStdWString(), tag, 2);
        QCOMPARE(siblings.size(), size_t{2});
        for (const auto& sibling : siblings) {
            QVERIFY(sibling.registered && !sibling.hasVisibleWindow);
            QCOMPARE(sibling.answersWindowMessage, sibling.processId == alive->ProcessId());
        }
        // The production record (written by OkuFlowInstanceRegistration)
        // uses the name and layout the fixture and the observer use.
        const std::wstring selfTag = L"record" + std::to_wstring(GetCurrentProcessId());
        HANDLE record = okuflow::RecordOkuFlowUiThread(selfTag, GetCurrentProcessId(), GetCurrentThreadId());
        QVERIFY(record);
        HANDLE opened = OpenFileMappingW(FILE_MAP_READ, FALSE,
                                         okuflow::OkuFlowInstanceUiThreadName(selfTag, GetCurrentProcessId()).c_str());
        QVERIFY(opened);
        const void* view = MapViewOfFile(opened, FILE_MAP_READ, 0, 0, sizeof(DWORD));
        QVERIFY(view);
        QCOMPARE(*static_cast<const DWORD*>(view), GetCurrentThreadId());
        UnmapViewOfFile(view);
        CloseHandle(opened);
        CloseHandle(record);
    }
    void recordedUiThreadWithoutAWindowIsNotALeftover() {
        // Iteration 5 (7): the recorded UI thread owns no window at all, so
        // nothing answers or fails to answer a window message: Pending for
        // as long as that lasts, never a leftover.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const std::wstring tag = L"nowindow" + std::to_wstring(GetCurrentProcessId());
        const QString a = directory.filePath(QStringLiteral("a.txt"));
        std::string error;
        auto instance = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"instance-nowindow", tag, a.toStdWString()}, {}, {}}, &error);
        QVERIFY2(instance, error.c_str());
        QVERIFY(WaitForFixtureFile(a));
        const auto siblings = ObserveSiblingsEventually(image.toStdWString(), tag, 1);
        QCOMPARE(siblings.size(), size_t{1});
        QVERIFY(siblings.front().registered && !siblings.front().hasVisibleWindow);
        QVERIFY(siblings.front().uiThreadHasNoWindow);
        okuflow::OkuFlowSiblingTrack track;
        for (std::int64_t now = 0; now <= 120000; now += 5000)
            QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(siblings.front(), track, now)),
                     int(okuflow::OkuFlowSiblingVerdict::Pending));
        // Its exit signal still proves a leftover after the exiting grace.
        auto exiting = siblings.front();
        exiting.exiting = true;
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(exiting, track, 200000)), int(okuflow::OkuFlowSiblingVerdict::Pending));
        QCOMPARE(int(okuflow::ClassifyOkuFlowSibling(exiting, track, 205000)), int(okuflow::OkuFlowSiblingVerdict::Leftover));
    }
    void leftoverInstanceIsStoppedThroughDiscovery() {
        // C2/A3 through Enumerate and the production RuntimeSiblings path
        // (fixture image and tag only): an unregistered windowless sibling
        // is named, never stopped and does not end the ladder; a proven
        // leftover is stopped only while the phone is missing; a refused
        // termination falls back to the leftover's own child.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.listingState.store(6); // Listed (offline): the leftover holds nothing.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const std::wstring tag = L"ladder" + std::to_wstring(GetCurrentProcessId());
        const QString a = directory.filePath(QStringLiteral("a.txt")), c = directory.filePath(QStringLiteral("c.txt"));
        std::string error;
        auto exiting = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"instance", tag, L"1", a.toStdWString()}, {}, {}}, &error);
        auto unregistered = okuflow::OwnedChildProcess::Launch({image.toStdWString(), {L"sleep", c.toStdWString()}, {}, {}}, &error);
        QVERIFY2(exiting && unregistered, error.c_str());
        QVERIFY(WaitForFixtureFile(a) && WaitForFixtureFile(c));
        bool refuse = false;
        const auto customize = [&](okuflow::PhoneAdbTestHooks& h) {
            h.siblingImage = image.toStdWString(); h.siblingTag = tag; h.siblingAdbImage = image.toStdWString();
            h.siblingExitingGraceMs = 100; h.siblingWindowlessMs = 200; h.refuseSiblingTermination = refuse;
        };
        {
            ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000, customize);
            okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
            LinkCycles(8);
            QVERIFY(exiting->IsRunning()); // The phone is listed: nothing plausibly held.
        }
        server.listEmpty.store(true);
        {
            ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000, customize);
            okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
            const auto reports = LinkCycles(30, [&](const auto&) { return !exiting->IsRunning(); });
            QVERIFY(exiting->WaitForExit(std::chrono::milliseconds(3000)));
            QVERIFY(unregistered->IsRunning());
            QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
            const auto later = LinkCycles(6);
            QVERIFY(!Saw(later, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
            QVERIFY(unregistered->IsRunning());
        }
        // Termination refused: only the leftover's own child of the adb image.
        const QString b = directory.filePath(QStringLiteral("b.txt")), child = directory.filePath(QStringLiteral("child.txt"));
        auto holder = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"instance", tag, L"1", b.toStdWString(), child.toStdWString()}, {}, {}}, &error);
        QVERIFY2(holder, error.c_str());
        QVERIFY(WaitForFixtureFile(b) && WaitForFixtureFile(child));
        const auto childPid = ReadFixturePid(child);
        QVERIFY(childPid);
        refuse = true;
        {
            ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000, customize);
            okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
            LinkCycles(30, [&](const auto&) { return FixtureProcessExited(childPid); });
            QVERIFY(WaitFixtureProcessExited(childPid));
            QVERIFY(holder->IsRunning());
            QVERIFY(unregistered->IsRunning());
        }
    }
    void leftoverStopIsRetriedAfterItsIdentityChanged() {
        // Iteration 4 (B1): the single stop attempt ended IdentityChanged (a
        // window showed at the last check). A later observation that still
        // proves the leftover tries again instead of never.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.listEmpty.store(true);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString image = directory.filePath(QStringLiteral("okuflow_instance_fixture.exe"));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), image));
        const std::wstring tag = L"retry" + std::to_wstring(GetCurrentProcessId());
        const QString a = directory.filePath(QStringLiteral("a.txt"));
        std::string error;
        auto exiting = okuflow::OwnedChildProcess::Launch({image.toStdWString(),
            {L"instance", tag, L"1", a.toStdWString()}, {}, {}}, &error);
        QVERIFY2(exiting, error.c_str());
        QVERIFY(WaitForFixtureFile(a));
        auto checks = std::make_shared<std::atomic<int>>(0);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.siblingImage = image.toStdWString(); h.siblingTag = tag; h.siblingAdbImage = image.toStdWString();
            h.siblingExitingGraceMs = 100; h.siblingWindowlessMs = 200;
            h.siblingVisibleWindowForTest = [checks](DWORD) { return checks->fetch_add(1) == 0; };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        LinkCycles(40, [&](const auto&) { return !exiting->IsRunning(); });
        QVERIFY(checks->load() >= 2);
        QVERIFY(exiting->WaitForExit(std::chrono::milliseconds(3000)));
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
        // Changed deliberately in iteration 8 (A2): this row asserted the
        // endless outcome (a private adb that starts, stays alive and never
        // answers read "recovering" for ever). The first cycle still reports
        // automatic recovery; two starts that outlive their start grace
        // unanswered now end in the AdbCannotStart sentence.
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-hang"}, 5000, [](okuflow::PhoneAdbTestHooks& h) { h.ownedPolicy = FastOwnedPolicy(); });
        bool listed = true;
        okuflow::PhoneDiscoveryStatus status{};
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, okuflow::PhoneServerIntent::PhoneUse).empty());
        QVERIFY(!listed);
        QCOMPARE(status, okuflow::PhoneDiscoveryStatus::OwnedServerRecovering);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(400, [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(60); });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
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
    void privateServerBesideAHungDefaultCountsTowardTheHandOverCap() {
        // Iteration 4 (A3): the transport's own start beside a hung default
        // counts toward the per-episode hand-over cap and is not made at the
        // cap (cap 0); its hand-back to a default that lists the phone is
        // still made at the cap (cap 1: the start reached it).
        for (const int cap : {0, 1}) {
            FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                                  FakePhoneAdb::RecoveryMode::ServerStalled, false);
            ScopedLinkHooks hooks(borrowed.port, okuflow::PhoneAdbInterface::Present,
                                  OwnedChildFixturePath().toStdWString(), {}, 0,
                                  [cap](okuflow::PhoneAdbTestHooks& h) { h.linkPolicy->handOverCap = cap; });
            using Intent = okuflow::PhoneServerIntent;
            bool listed = false;
            for (int attempt = 0; attempt < 3 && !okuflow::OwnedAdbServerPort(); ++attempt)
                okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
            if (cap == 0) {
                QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
                continue;
            }
            QVERIFY(okuflow::OwnedAdbServerPort().has_value());
            borrowed.hostUnresponsive.store(false);
            borrowed.deviceStalled.store(false);
            for (int attempt = 0; attempt < 3 && okuflow::OwnedAdbServerPort(); ++attempt)
                okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
            QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        }
    }
    void hangRightAfterAHandBackMeetsAPrivateServerAgain() {
        // Iteration 5 (3): the transport's own hand-back no longer arms the
        // 30 s start throttle (the ladder's never did). A default that hangs
        // right after it gets a private server once the hang is confirmed,
        // instead of burning the hand-over cap on throttled starts.
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false);
        ScopedLinkHooks hooks(borrowed.port, okuflow::PhoneAdbInterface::Present,
                              OwnedChildFixturePath().toStdWString(), {}, 0);
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        for (int attempt = 0; attempt < 3 && !okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        borrowed.hostUnresponsive.store(false);
        borrowed.deviceStalled.store(false);
        for (int attempt = 0; attempt < 3 && okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value()); // Handed back.
        borrowed.hostUnresponsive.store(true); // Hangs again at once.
        for (int attempt = 0; attempt < 6 && !okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void ownHandBackCountedAfterAnEventIsDropped() {
        // Iteration 7 (4): the transport's own hand-back of the private
        // server it started beside a hung default, with an event raised while
        // that server is retired (the mutex is released around Stop()). The
        // hand-back belongs to the old episode and is not counted in the new
        // one: with a hand-over cap of one, a default that hangs again still
        // gets a private server beside it.
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false);
        auto fired = std::make_shared<std::atomic<bool>>(false);
        ScopedLinkHooks hooks(borrowed.port, okuflow::PhoneAdbInterface::Present,
                              OwnedChildFixturePath().toStdWString(), {}, 0, [fired](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->handOverCap = 1;
            h.afterPrivateServerRetiredForTest = [fired] {
                if (!fired->exchange(true)) okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::ManualPick);
            };
        });
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        for (int attempt = 0; attempt < 3 && !okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value()); // Beside the hung default: the cap's one start.
        borrowed.hostUnresponsive.store(false);
        borrowed.deviceStalled.store(false);
        for (int attempt = 0; attempt < 3 && okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value()); // The transport's own hand-back...
        QVERIFY(fired->load()); // ...with an event in between.
        borrowed.hostUnresponsive.store(true); // Hangs again.
        for (int attempt = 0; attempt < 6 && !okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void privateServerStartThatCannotHappenIsNotCharged() {
        // Iteration 5 (3): beside a hung default, a start that cannot happen
        // (no executable yet) does not count toward the hand-over cap. With
        // the cap at one start, the start that finally can happen is made.
        QTemporaryDir directory;
        const QString later = directory.filePath(QStringLiteral("adb_later.exe"));
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false);
        ScopedLinkHooks hooks(borrowed.port, okuflow::PhoneAdbInterface::Present, later.toStdWString(), {}, 0,
                              [](okuflow::PhoneAdbTestHooks& h) { h.linkPolicy->handOverCap = 1; });
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        for (int attempt = 0; attempt < 4; ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(QFile::copy(OwnedChildFixturePath(), later));
        for (int attempt = 0; attempt < 3 && !okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void deadPrivateServerThatYieldsToTheDefaultClearsTheShadow() {
        // Iteration 6 (4): the private server beside a hung default dies and
        // the default answers again: the private server is dropped and so is
        // the shadow state, as both hand-back paths do. An empty default
        // listing is a listing, not "OkuFlow's own server recovering", and a
        // default that hangs again meets a private server at once.
        ScopedOwnedAdbHooks hooks({L"--fixture-exit-after-list"}, 0);
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false, hooks.defaultPort);
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        okuflow::PhoneDiscoveryStatus status{};
        for (int attempt = 0; attempt < 4 && !listed; ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, Intent::PhoneUse);
        QVERIFY(listed); // The private server beside the hung default answered once...
        QCOMPARE(status, okuflow::PhoneDiscoveryStatus::OwnedServerRecovering);
        QTRY_VERIFY_WITH_TIMEOUT(!okuflow::OwnedAdbServerPort().has_value(), 2000); // ...and died.
        borrowed.hostUnresponsive.store(false);
        borrowed.deviceStalled.store(false);
        borrowed.listEmpty.store(true);
        listed = false;
        QVERIFY(okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, Intent::PhoneUse).empty());
        QVERIFY(listed);
        QCOMPARE(status, okuflow::PhoneDiscoveryStatus::Listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        borrowed.hostUnresponsive.store(true); // Hangs again.
        listed = false;
        for (int attempt = 0; attempt < 4 && !listed; ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, Intent::PhoneUse);
        QVERIFY(listed); // A new private server started and listed (no 30 s start throttle left over).
    }
    void requestedPrivateServerThatCannotStartYetStartsLater() {
        // Iteration 6 (5): the doctor's start beside a blind default (charged
        // once, by its rung, reaching the hand-over cap of one) cannot launch
        // yet: no executable. The request stays until a start passes the
        // checks, so it is made once the executable appears, instead of being
        // dropped and asked for again at the cap.
        QTemporaryDir directory;
        const QString later = directory.filePath(QStringLiteral("adb_later.exe"));
        DefaultAdbFixture other({});
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, later.toStdWString(), {}, 5000,
                              [](okuflow::PhoneAdbTestHooks& h) { h.linkPolicy->handOverCap = 1; });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        LinkCycles(3);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(QFile::copy(OwnedChildFixturePath(), later));
        LinkCycles(10, [](const auto&) { return okuflow::OwnedAdbServerPort().has_value(); });
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(!other.Exited());
    }
    void requestOfADefaultThatWentAwayDoesNotOutliveIt() {
        // Iteration 7 (3): the doctor decides a private server beside a blind
        // default, which goes away before the start runs (and there is
        // nothing to launch yet). The start that follows is not beside
        // anything, so the request ends there: when the default comes back
        // responsive, phone-use discovery borrows it and starts no private
        // server beside it that no decision asked for.
        QTemporaryDir directory;
        const QString later = directory.filePath(QStringLiteral("adb_later.exe"));
        DefaultAdbFixture other({});
        QVERIFY(other.copied && !other.Exited());
        DefaultAdbFixture* fixture = &other;
        auto gone = std::make_shared<std::atomic<bool>>(false);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, later.toStdWString(), {}, 5000,
                              [fixture, gone](okuflow::PhoneAdbTestHooks& h) {
            h.beforeShadowStartForTest = [fixture, gone] { if (!gone->exchange(true)) fixture->child.reset(); };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(gone->load());
        QVERIFY(other.Exited());
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(other.Start()); // Back, responsive and blind, in the same episode.
        QVERIFY(QFile::copy(OwnedChildFixturePath(), later));
        bool listed = false;
        for (int attempt = 0; attempt < 3; ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, okuflow::PhoneServerIntent::PhoneUse);
        QVERIFY(listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
    }
    void requestIsDroppedWhenUsbIsReleasedForAWebcam() {
        // Iteration 7 (3): a requested start beside a blind default that
        // cannot launch yet stays requested (iteration 6), but releasing USB
        // for a webcam voids it: a later phone-use call with the executable
        // in place starts no private server that no decision asked for.
        QTemporaryDir directory;
        const QString later = directory.filePath(QStringLiteral("adb_later.exe"));
        DefaultAdbFixture other({});
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, later.toStdWString(), {}, 5000);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::ReleaseForWebcam);
        QVERIFY(listed);
        QVERIFY(QFile::copy(OwnedChildFixturePath(), later));
        for (int attempt = 0; attempt < 3; ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(!other.Exited());
    }
    void handOverCountedAfterAnEventIsDropped() {
        // Iteration 6 (5): an event arrives while the private server is being
        // retired for a default that lists the phone (the mutex is released
        // around Stop()). That hand-back belongs to the old episode and is not
        // counted in the new one: with a hand-over cap of one, the default
        // that loses the phone again still gets a private server beside it.
        DefaultAdbFixture other({});
        other.child.reset();
        const QString device = other.File(QStringLiteral("device"));
        other.flags = {L"--fixture-device-file", device.toStdWString()};
        QVERIFY(other.copied && other.Start());
        auto fired = std::make_shared<std::atomic<bool>>(false);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [fired](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->handOverCap = 1;
            h.afterPrivateServerRetiredForTest = [fired] {
                if (!fired->exchange(true)) okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::ManualPick);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        { QFile file(device); QVERIFY(file.open(QIODevice::WriteOnly)); } // The default lists the phone.
        reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::HandBackToDefault));
        QVERIFY(fired->load());
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(QFile::remove(device)); // It loses the phone again.
        reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer ||
                r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
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
                                                   "P:\npackage:/data/app/x/base.apk\nR:\n12345\nOKUFLOW_FACTS_END\n");
        QCOMPARE(int(facts.screenAwake), int(okuflow::PhoneFact::No));
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(facts.appInstalled), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::Yes));
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\nisKeyguardShowing=false\nP:\nR:\nOKUFLOW_FACTS_END\n");
        QCOMPARE(int(facts.screenAwake), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::No));
        QCOMPARE(int(facts.appInstalled), int(okuflow::PhoneFact::No));
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::No));
        // Android 12 and later: KeyguardController and DeviceIdleController lines.
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\n  KeyguardShowing=true AodShowing=false\nP:\nR:\nOKUFLOW_FACTS_END\n");
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::Yes));
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\n  mScreenLocked=true\nP:\nR:\nOKUFLOW_FACTS_END\n");
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::Yes));
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\n  mKeyguardShowing=false\n  mScreenLocked=false\nP:\nR:\nOKUFLOW_FACTS_END\n");
        QCOMPARE(int(facts.keyguardShowing), int(okuflow::PhoneFact::No));
        // Truncated output (no END) never claims absence.
        facts = okuflow::ParsePhoneShellFacts("W:\nK:\nP:\nR:\n");
        QCOMPARE(int(facts.appInstalled), int(okuflow::PhoneFact::Unknown));
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::Unknown));
        QCOMPARE(int(okuflow::ParsePhoneShellFacts("").screenAwake), int(okuflow::PhoneFact::Unknown));
        // Iteration 5 (4): only the unique sentinel line ends the sections; a
        // line that reads END (or contains it) is ordinary output. Changed
        // deliberately: the rows above end with the sentinel, not "END".
        facts = okuflow::ParsePhoneShellFacts("W:\nmWakefulness=Awake\nK:\nmPendingEND=0\nP:\nEND\npackage:/data/app/END/base.apk\n"
                                              "R:\n4242\nOKUFLOW_FACTS_END\n");
        QCOMPARE(int(facts.appInstalled), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::Yes));
        facts = okuflow::ParsePhoneShellFacts("W:\nK:\nP:\nR:\nEND\n");
        QCOMPARE(int(facts.appRunning), int(okuflow::PhoneFact::Unknown));
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
        server.SetShellFacts("W:\n mWakefulness=Asleep\nK:\n mShowingLockscreen=true\nP:\npackage:/x\nR:\nOKUFLOW_FACTS_END\n");
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
    void ladderLaunchUsesTheObservedServerAndPhone_data() {
        QTest::addColumn<int>("change");
        QTest::newRow("other server reuses the ID for a different phone") << 0;
        QTest::newRow("endpoint changes even though the phone matches") << 1;
        QTest::newRow("same listener lists a different phone under that ID") << 2;
        QTest::newRow("intent changes during transport selection") << 3;
    }
    void ladderLaunchUsesTheObservedServerAndPhone() {
        QFETCH(int, change);
        std::atomic<std::uint64_t> generation{1};
        std::atomic<bool> launching{false};
        FakePhoneAdb first(false, false, false, FakePhoneAdb::LaunchMode::Play,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        FakePhoneAdb second(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        second.replacementPhone.store(change == 0);
        first.SetBeforeTransportReply([&] {
            if (change == 3 && launching.load()) generation.store(2);
        });
        ScopedLinkHooks hooks(first.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.beforeCompanionLaunchForTest = [&] {
                launching.store(true);
                if (change < 2)
                    okuflow::SetPhoneAdbTestHooks(okuflow::PhoneAdbTestHooks{second.port, {}});
                if (change == 2) first.replacementPhone.store(true);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::DeviceArrival);
        okuflow::PhoneLinkRequest request;
        request.intentGeneration = 1;
        request.liveIntentGeneration = &generation;
        const auto reports = LinkCycles(1, {}, "abc", &request);
        QCOMPARE(reports.back().decision.action, okuflow::PhoneLinkAction::WakeAndLaunchApp);
        QVERIFY(launching.load());
        for (auto* server : {&first, &second}) {
            QCOMPARE(server->ServiceCount("shell:input keyevent KEYCODE_WAKEUP"), 0);
            QVERIFY(server->Launches().isEmpty());
        }
        if (change == 2) QCOMPARE(first.ServiceCount("host:transport:abc"), 0);
        if (change == 3) {
            QCOMPARE(generation.load(), std::uint64_t{2});
            QCOMPARE(reports.back().actionResult, "dropped: the source changed");
        }
    }
    void ladderLaunchVerifiesOnceOnBorrowedDefault_data() {
        QTest::addColumn<bool>("unopenable");
        QTest::newRow("readable listener") << false;
        QTest::newRow("unopenable listener") << true;
    }
    void ladderLaunchVerifiesOnceOnBorrowedDefault() {
        QFETCH(bool, unopenable);
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present,
                              OwnedChildFixturePath().toStdWString(), {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.defaultListenerUnopenable = [unopenable] { return unopenable; };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(1);
        QCOMPARE(reports.back().observation.server, okuflow::PhoneServerKind::BorrowedDefault);
        QCOMPARE(reports.back().decision.action, okuflow::PhoneLinkAction::WakeAndLaunchApp);
        QCOMPARE(server.ServiceCount("shell:input keyevent KEYCODE_WAKEUP"), 1);
        QCOMPARE(server.Launches().size(), qsizetype{1});
        QCOMPARE(server.ServiceCount("host:track-devices-proto-binary"), 2); // Discovery + one verification.
        QCOMPARE(server.ServiceCount("host:devices-l"), 0);
    }
    void ladderGivesBackOnlyAnUnwrittenLaunch_data() {
        QTest::addColumn<bool>("wakeSent");
        QTest::newRow("nothing written") << false;
        QTest::newRow("wake written") << true;
    }
    void ladderGivesBackOnlyAnUnwrittenLaunch() {
        QFETCH(bool, wakeSent);
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        auto facts = server.shellFacts;
        facts.insert(facts.find("OKUFLOW_FACTS_END"), "123\n");
        server.SetShellFacts(facts);
        std::atomic<int> selection{0};
        std::atomic<bool> launching{false}, refuse{false};
        server.SetBeforeTransportReply([&] {
            if (launching.load() && ++selection == (wakeSent ? 2 : 1)) refuse.store(true);
        });
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->launchPeriodicMs = 500;
            h.defaultListenerUnopenable = [&] { return refuse.load(); };
            h.beforeCompanionLaunchForTest = [&] { launching.store(true); };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto first = LinkCycles(1);
        QCOMPARE(first.back().decision.action, okuflow::PhoneLinkAction::WakeAndLaunchApp);
        QCOMPARE(first.back().observation.shell.appRunning, okuflow::PhoneFact::Yes);
        QVERIFY(refuse.load());
        QCOMPARE(server.ServiceCount("shell:input keyevent KEYCODE_WAKEUP"), wakeSent ? 1 : 0);
        QVERIFY(server.Launches().isEmpty());

        refuse.store(false); // Same listener, same selected phone, no new event.
        const auto later = LinkCycles(15, [&](const auto& r) {
            return wakeSent ? r.decision.action == okuflow::PhoneLinkAction::WaitForUser
                            : r.decision.action == okuflow::PhoneLinkAction::WakeAndLaunchApp;
        });
        QCOMPARE(later.back().decision.action, wakeSent ? okuflow::PhoneLinkAction::WaitForUser
                                                       : okuflow::PhoneLinkAction::WakeAndLaunchApp);
        QCOMPARE(later.back().decision.episode, first.back().decision.episode);
        QCOMPARE(server.ServiceCount("shell:input keyevent KEYCODE_WAKEUP"), 1);
        QCOMPARE(server.Launches().size(), wakeSent ? qsizetype{0} : qsizetype{1});
    }
    void shellFactsAreReadToTheirSentinelLine() {
        // Iteration 5 (4): a package path and a dumpsys line containing END
        // arrive first; the processes and the sentinel follow in a second
        // chunk. The read goes on to the sentinel line, so "running" is known.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false mPendingEND=0\n"
                             "P:\npackage:/data/app/~~ENDx==/com.okuflow-1/base.apk\nEND\n",
                             "R:\n4242\nOKUFLOW_FACTS_END\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(1);
        QCOMPARE(int(reports.front().observation.shell.appInstalled), int(okuflow::PhoneFact::Yes));
        QCOMPARE(int(reports.front().observation.shell.appRunning), int(okuflow::PhoneFact::Yes));
    }
    void ladderManualPickAfterFailureStartsAFreshLadder() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Blocked,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mShowingLockscreen=true\nP:\npackage:/x\nR:\nOKUFLOW_FACTS_END\n");
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
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\nP:\nR:\nOKUFLOW_FACTS_END\n");
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
    void absentUserEndpointIsNamedAndCanRecover() {
        const auto port = PickTestPort();
        QVERIFY(port);
        ScopedLinkHooks hooks(port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->userEndpointUnresponsiveMs = 150;
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(10, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::ReplugCable;
        });
        QCOMPARE(reports.front().decision.diagnosis, okuflow::PhoneLinkDiagnosis::ServerStarting);
        QCOMPARE(reports.back().observation.server, okuflow::PhoneServerKind::Override);
        QCOMPARE(reports.back().observation.serverProbe, okuflow::PhoneServerProbe::NoListener);
        QCOMPARE(reports.back().decision.diagnosis, okuflow::PhoneLinkDiagnosis::ReplugCable);
        QCOMPARE(reports.back().decision.action, okuflow::PhoneLinkAction::WaitForUser);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false, port);
        const auto later = LinkCycles(1);
        QCOMPARE(later.back().decision.diagnosis, okuflow::PhoneLinkDiagnosis::Connected);
        QCOMPARE(server.ServiceCount("host:kill"), 0);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
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
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false\nP:\npackage:/x\nR:\n4242\nOKUFLOW_FACTS_END\n");
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
    void pausedCompanionOnALockedPhoneIsLaunchedOncePerEvent() {
        // Iteration 3 (C3): paused, phone locked, app process running. One
        // wake + launch per event, then the user is asked; never every 20 s.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Paused,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Asleep\nK:\n mKeyguardShowing=true\nP:\npackage:/x\nR:\n4242\nOKUFLOW_FACTS_END\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::CompanionListenerClosed));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QCOMPARE(server.Launches().size(), qsizetype{1});
        QCOMPARE(server.ServiceCount("shell:input keyevent KEYCODE_WAKEUP"), 1);
    }
    void pausedCompanionThatDiesIsRelaunched() {
        // Iteration 4 (A5): paused (process runs, listener refuses): one
        // launch, held. Then the app process dies: relaunched on the same
        // ladder without waiting for an event.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Paused,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false\nP:\npackage:/x\nR:\n4242\nOKUFLOW_FACTS_END\n");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(10);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::CompanionListenerClosed));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QCOMPARE(server.Launches().size(), qsizetype{1});
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false\nP:\npackage:/x\nR:\nOKUFLOW_FACTS_END\n");
        reports = LinkCycles(6, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::WakeAndLaunchApp; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WakeAndLaunchApp));
        QCOMPARE(server.Launches().size(), qsizetype{2});
    }
    void manualPickWithThePausedCompanionLaunchesOncePerEvent() {
        // C3: the session start path takes its launch from the same
        // per-event budget as the ladder: repeated starts do not relaunch.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Paused,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false\nP:\npackage:/x\nR:\n4242\nOKUFLOW_FACTS_END\n");
        ScopedLinkHooks hooks(server.port);
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(apartment)); QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        auto teardown = qScopeGuard([&] { MFShutdown(); CoUninitialize(); });
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        const auto start = [&] {
            okuflow::PhoneCameraTransport transport;
            const bool started = transport.Start(phone, nullptr, {}, {});
            transport.Stop();
            return started;
        };
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::ManualPick);
        okuflow::PhoneCameraTransport::AllowAppLaunch("abc");
        QVERIFY(!start());
        QCOMPARE(server.Launches().size(), qsizetype{1});
        LinkCycles(6); // The ladder names it and does not launch again.
        QVERIFY(!start()); // Another automatic attempt within the episode.
        QCOMPARE(server.Launches().size(), qsizetype{1});
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::ManualPick);
        QVERIFY(!start());
        QCOMPARE(server.Launches().size(), qsizetype{2});
    }
    void hangingCompanionListenerIsNamed() {
        // A15: the listener exists and accepts, but never answers.
        FakePhoneAdb server(false, true, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetShellFacts("W:\n mWakefulness=Awake\nK:\n mKeyguardShowing=false\nP:\npackage:/x\nR:\n4242\nOKUFLOW_FACTS_END\n");
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
    void stalledPhoneOnTheDefaultIsTakenOver() {
        // Iteration 4 (A1): another adb on the default port lists the phone
        // "device" but its adbd never answers. Once OkuFlow's own server runs
        // beside it, that listing is not serving the phone: no hand-back
        // (which swapped back and forth), the takeover completes.
        DefaultAdbFixture other({L"--fixture-device", L"--fixture-exit-on-kill", L"--fixture-stalled-shell"});
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit ||
                r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::TransportStalled));
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::HandBackToDefault));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY(reports.back().observation.defaultListsPhone && reports.back().observation.defaultPhoneStalled);
        QVERIFY2(reports.back().actionResult.find("exited") != std::string::npos, reports.back().actionResult.c_str());
        QVERIFY(other.child->WaitForExit(std::chrono::milliseconds(3000)));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void stalledPhoneThatRecoversOnTheDefaultIsHandedBack() { recoveredStallIsHandedBack(false); }
    void stalledPhoneThatRecoversWithACrlfReplyIsHandedBack() {
        // Iteration 6 (3): the recovered shell ends its echo with CR LF (a
        // shell through a PTY). Any reply within the budget is an answer:
        // the same hand-back, no host:kill, nothing stopped.
        recoveredStallIsHandedBack(true);
    }
    void stalledPhoneThatRecoversBeforeTheTerminationIsHandedBack() {
        // Iteration 6 (8): the re-check on the termination path. The shell
        // is still silent at the ask, so host:kill goes out and is ignored;
        // it answers by the time the termination runs. The termination is
        // aborted before any byte ("the phone answers"), the server keeps
        // running and USB goes back to it.
        DefaultAdbFixture other({});
        other.child.reset();
        const QString stall = other.File(QStringLiteral("stall"));
        const QString kills = other.File(QStringLiteral("kills.log"));
        { QFile file(stall); QVERIFY(file.open(QIODevice::WriteOnly)); }
        other.flags = {L"--fixture-device", L"--fixture-stalled-shell-file", stall.toStdWString(),
                       L"--fixture-kill-log", kills.toStdWString()};
        QVERIFY(other.copied && other.Start());
        auto stops = std::make_shared<std::atomic<int>>(0);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [stall, stops](okuflow::PhoneAdbTestHooks& h) {
            // The first stop step is the ask; the shell recovers before the next.
            h.beforeDefaultStopForTest = [stall, stops] { if (stops->fetch_add(1) == 1) QFile::remove(stall); };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(80, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QCOMPARE(QString::fromStdString(reports.back().actionResult),
                 QStringLiteral("kept: the phone answers on the default server now"));
        const auto asked = std::find_if(reports.begin(), reports.end(), [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QVERIFY(asked != reports.end());
        QVERIFY2(asked->actionResult.find("outlived host:kill") != std::string::npos, asked->actionResult.c_str());
        reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::HandBackToDefault));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QTest::qWait(300);
        QVERIFY(!other.Exited());
        QFile log(kills);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count('\n'), 1); // Only the ask's host:kill; the termination sent nothing.
    }
    void keptAskIsNotFollowedByATermination() {
        // Iteration 6 (2): the stalled phone's shell answers at the ask, so
        // no host:kill is sent, and the next listing misses the phone for a
        // moment. The kept ask is not remembered as sent: no termination as
        // if host:kill had been ignored, no other-program sentence, and the
        // ask is not used up (one ask per episode here): the blind default
        // beside is asked again, as any is.
        DefaultAdbFixture other({});
        other.child.reset();
        const QString stall = other.File(QStringLiteral("stall"));
        const QString device = other.File(QStringLiteral("device"));
        const QString kills = other.File(QStringLiteral("kills.log"));
        { QFile file(stall); QVERIFY(file.open(QIODevice::WriteOnly)); }
        { QFile file(device); QVERIFY(file.open(QIODevice::WriteOnly)); }
        other.flags = {L"--fixture-device-file", device.toStdWString(), L"--fixture-stalled-shell-file",
                       stall.toStdWString(), L"--fixture-exit-on-kill", L"--fixture-kill-log", kills.toStdWString()};
        QVERIFY(other.copied && other.Start());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [stall](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->askExitCap = 1;
            h.beforeDefaultStopForTest = [stall] { QFile::remove(stall); }; // adbd answers from the ask on.
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(80, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY2(reports.back().actionResult.find("answers on the default server") != std::string::npos,
                 reports.back().actionResult.c_str());
        QVERIFY(!QFile::exists(kills));
        QVERIFY(QFile::remove(device)); // The next listing misses the phone.
        reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit ||
                r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer ||
                r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; });
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
    }
    void hungDefaultReasonUsesTheSameServesTest() {
        // Iteration 5 (8): a default whose listing shows the phone "device"
        // while its adbd timed out there, then hangs on version probes. The
        // pre-termination listing keeps it (any answer does); the reason uses
        // the same test as the other pre-checks: it answers, it does not
        // serve the phone.
        DefaultAdbFixture other({});
        other.child.reset();
        const QString slow = other.File(QStringLiteral("slow"));
        other.flags = {L"--fixture-device", L"--fixture-stalled-shell", L"--fixture-slow-version-file", slow.toStdWString()};
        QVERIFY(other.copied && other.Start());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        { QFile file(slow); QVERIFY(file.open(QIODevice::WriteOnly)); }
        reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QCOMPARE(QString::fromStdString(reports.back().actionResult), QStringLiteral("kept: the default server answers a listing"));
        QVERIFY(!other.Exited());
    }
    void shadowIsHandedBackOnlyForTheWantedPhone() {
        // Iteration 4 (B2): the hung default recovers and lists another USB
        // phone ready, not the wanted one: the private server stays.
        ScopedOwnedAdbHooks hooks({}, 0);
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false, hooks.defaultPort);
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        for (int attempt = 0; attempt < 3 && !okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        const auto shadow = okuflow::OwnedAdbServerPort();
        QVERIFY(shadow.has_value());
        borrowed.hostUnresponsive.store(false);
        borrowed.deviceStalled.store(false);
        borrowed.listEmpty.store(true);
        borrowed.secondUsbPhone.store(true); // Lists "xyz" ready only.
        for (int attempt = 0; attempt < 4; ++attempt) {
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
            QCOMPARE(okuflow::OwnedAdbServerPort(), shadow);
        }
    }
    void hungDefaultThatAnswersFailIsNeverStopped() {
        // Iteration 4 (B3): the version probes hang, the listing answers
        // with an adb FAIL reply: the server is alive and is never stopped.
        DefaultAdbFixture slow({});
        slow.child.reset();
        const QString fail = slow.File(QStringLiteral("fail"));
        { QFile file(fail); QVERIFY(file.open(QIODevice::WriteOnly)); }
        slow.flags = {L"--fixture-slow-version", L"1100", L"--fixture-fail-file", fail.toStdWString()};
        QVERIFY(slow.Start());
        ScopedLinkHooks hooks(slow.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(20, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("answers a listing with FAIL") != std::string::npos,
                 reports.back().actionResult.c_str());
        QTest::qWait(300);
        QVERIFY(!slow.Exited());
    }
    void abortedTerminationsDoNotSpendTheTerminationCap() {
        // Iteration 5 (3), changed deliberately in iteration 6 (1): each
        // termination aborted because the listing answered stops nothing and
        // does not spend the termination cap, but the aborted stops have an
        // end: the other-program sentence after abortedStopCap of them
        // (set above the termination cap here, so a spent cap would end it
        // sooner). A server that recovers ends the sentence.
        DefaultAdbFixture slow({});
        slow.child.reset();
        const QString fail = slow.File(QStringLiteral("fail"));
        const QString slowFile = slow.File(QStringLiteral("slow"));
        { QFile file(fail); QVERIFY(file.open(QIODevice::WriteOnly)); }
        { QFile file(slowFile); QVERIFY(file.open(QIODevice::WriteOnly)); }
        slow.flags = {L"--fixture-slow-version-file", slowFile.toStdWString(), L"--fixture-fail-file", fail.toStdWString()};
        QVERIFY(slow.Start());
        const int cap = okuflow::PhoneLinkPolicy{}.terminateCap + 2;
        ScopedLinkHooks hooks(slow.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100,
                              [cap](okuflow::PhoneAdbTestHooks& h) { h.linkPolicy->abortedStopCap = cap; });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        int aborted = 0;
        auto reports = LinkCycles(120, [&](const auto& r) {
            if (r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer &&
                r.actionResult.find("answers a listing with FAIL") != std::string::npos)
                ++aborted;
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QCOMPARE(aborted, cap);
        for (const auto& report : reports)
            QVERIFY2(report.decision.action != okuflow::PhoneLinkAction::TerminateDefaultServer ||
                     report.actionResult.find("kept:") == 0, report.actionResult.c_str());
        // The sentence stands while the server stays as it is.
        reports = LinkCycles(6);
        for (const auto& report : reports) {
            QCOMPARE(int(report.decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
            QCOMPARE(int(report.decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        }
        QVERIFY(!slow.Exited());
        // It recovers (prompt versions, plain listings): the sentence ends.
        QVERIFY(QFile::remove(slowFile));
        QVERIFY(QFile::remove(fail));
        reports = LinkCycles(20, [](const auto& r) {
            return r.decision.diagnosis != okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; });
        QVERIFY(reports.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone);
        QVERIFY(!slow.Exited());
        // Iteration 7 (2): it answers again but still does not list the
        // phone. OkuFlow's private server already runs beside it (started
        // when the hang was first confirmed), so this is the takeover branch,
        // whose count is not the hung branch's: after the takeover grace the
        // default is asked to exit, not named at once.
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit ||
                r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; });
        QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::NotListedByServer));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY(!slow.Exited());
    }
    void askThatFindsTheDefaultGoneIsGivenBack() {
        // Iteration 7 (4): the ask finds the default's listener already gone
        // (it answered the pre-ask listing and closed). Nothing was asked, so
        // the ask is given back; nothing was kept, so it is not an aborted
        // stop. With one ask and one aborted stop per episode, another adb
        // that appears beside OkuFlow's server is still asked to exit.
        // The default is the in-process fake: it closes its listener before
        // it answers the ask's pre-ask listing.
        auto first = std::make_unique<FakePhoneAdb>(false, false, false, FakePhoneAdb::LaunchMode::None,
                                                    FakePhoneAdb::RecoveryMode::Healthy, false);
        first->listEmpty.store(true);
        const quint16 port = first->port;
        FakePhoneAdb* gone = first.get();
        auto armed = std::make_shared<std::atomic<bool>>(false);
        ScopedLinkHooks hooks(port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [gone, armed](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->askExitCap = 1;
            h.linkPolicy->abortedStopCap = 1;
            h.beforeDefaultStopForTest = [gone, armed] {
                if (!armed->exchange(true)) gone->goneAtNextConnection.store(true);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QCOMPARE(QString::fromStdString(reports.back().actionResult), QStringLiteral("default server already gone"));
        QVERIFY(armed->load());
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        first.reset();
        // Another adb on the port, blind beside OkuFlow's server.
        FakePhoneAdb second(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false, port);
        second.listEmpty.store(true);
        reports = LinkCycles(80, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit ||
                r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; });
        QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
    }
    void hostKillIsAbortedWhenTheInterfaceIsGoneAtExecution() {
        // Iteration 4 (B4): like termination, host:kill re-checks the
        // Windows ADB interface when it runs.
        DefaultAdbFixture other({L"--fixture-exit-on-kill"});
        QVERIFY(other.copied && !other.Exited());
        auto present = std::make_shared<std::atomic<bool>>(true);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [present](okuflow::PhoneAdbTestHooks& h) {
            h.adbInterface = [present] {
                return present->load() ? okuflow::PhoneAdbInterface::Present : okuflow::PhoneAdbInterface::Absent; };
            h.beforeDefaultStopForTest = [present] { present->store(false); }; // Unplugged in between.
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY2(reports.back().actionResult.find("interface is gone") != std::string::npos, reports.back().actionResult.c_str());
        QTest::qWait(300);
        QVERIFY(!other.Exited());
    }
    void slowDefaultServingAnotherDeviceIsNamed() {
        // Iteration 4 (B6): version probes stay slow, the listing answers and
        // shows another device ("abc", while the wanted phone is "xyz"): it
        // serves someone. Named once, instead of "relist, keep" for ever.
        DefaultAdbFixture slow({L"--fixture-slow-version", L"1100", L"--fixture-device"});
        QVERIFY(slow.copied && !slow.Exited());
        ScopedLinkHooks hooks(slow.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(30, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; }, "xyz");
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QVERIFY(!slow.Exited());
    }
    void slowDefaultVerdictEndsWhenTheOtherDeviceGoes() {
        // Iteration 5 (6): the other device (an emulator or TCP device) goes
        // away without any Windows event. While the doctor waits for the
        // user, that listener is listed again; the verdict ends with it.
        DefaultAdbFixture slow({});
        slow.child.reset();
        const QString device = slow.File(QStringLiteral("device"));
        { QFile file(device); QVERIFY(file.open(QIODevice::WriteOnly)); }
        slow.flags = {L"--fixture-slow-version", L"1100", L"--fixture-device-file", device.toStdWString()};
        QVERIFY(slow.copied && slow.Start());
        ScopedLinkHooks hooks(slow.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(30, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; }, "xyz");
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(QFile::remove(device)); // The other device is gone; no event.
        reports = LinkCycles(10, [](const auto& r) {
            return r.decision.diagnosis != okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; }, "xyz");
        QVERIFY(reports.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone);
        QVERIFY(!reports.back().observation.defaultHasOtherDevices);
        QVERIFY(!slow.Exited());
    }
    void slowDefaultWithoutItsOtherDeviceEndsInTheSentence() {
        // Iteration 6 (1): past the end of iteration 5's item 6. The other
        // device goes away from a slow default; the verdict ends, and every
        // termination after it is aborted because the listing answers. Those
        // aborted stops end in the other-program sentence, no longer resting
        // on another device, instead of "Restoring" for ever.
        DefaultAdbFixture slow({});
        slow.child.reset();
        const QString device = slow.File(QStringLiteral("device"));
        { QFile file(device); QVERIFY(file.open(QIODevice::WriteOnly)); }
        slow.flags = {L"--fixture-slow-version", L"1100", L"--fixture-device-file", device.toStdWString()};
        QVERIFY(slow.copied && slow.Start());
        ScopedLinkHooks hooks(slow.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(30, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; }, "xyz");
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(reports.back().observation.defaultHasOtherDevices);
        auto all = reports;
        QVERIFY(QFile::remove(device)); // The other device is gone; no event.
        reports = LinkCycles(10, [](const auto& r) {
            return r.decision.diagnosis != okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; }, "xyz");
        QVERIFY(reports.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone);
        all.insert(all.end(), reports.begin(), reports.end());
        reports = LinkCycles(80, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone; }, "xyz");
        all.insert(all.end(), reports.begin(), reports.end());
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QVERIFY(!reports.back().observation.defaultHasOtherDevices);
        QVERIFY(std::any_of(reports.begin(), reports.end(), [](const auto& r) {
            return r.actionResult == "kept: the default server answers a listing"; }));
        // Every termination of the row was aborted, and their number is the cap.
        const auto terminations = std::count_if(all.begin(), all.end(), [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(terminations, std::ptrdiff_t{okuflow::PhoneLinkPolicy{}.abortedStopCap});
        for (const auto& report : all)
            QVERIFY2(report.decision.action != okuflow::PhoneLinkAction::TerminateDefaultServer ||
                     report.actionResult.find("kept:") == 0, report.actionResult.c_str());
        QVERIFY(!slow.Exited());
    }
    void slowButListingDefaultIsNeverStopped() {
        // Iteration 3 (C1): two host:version answers slower than the budget
        // confirm a "hang", but the server lists at once when asked: it is
        // never stopped, and the hang has to be confirmed again.
        DefaultAdbFixture slow({L"--fixture-slow-version", L"1100"});
        QVERIFY(slow.copied && !slow.Exited());
        ScopedLinkHooks hooks(slow.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(20, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("answers a listing") != std::string::npos, reports.back().actionResult.c_str());
        const auto later = LinkCycles(6);
        for (const auto& report : later)
            QVERIFY2(report.actionResult.find("terminated") == std::string::npos, report.actionResult.c_str());
        QVERIFY(!slow.Exited());
    }
    void hungDefaultIsKeptWhenTheInterfaceIsGoneAtExecution() {
        // C1: the Windows ADB interface is re-checked when the stop runs.
        DefaultAdbFixture hung({L"--fixture-hang"});
        QVERIFY(hung.copied && !hung.Exited());
        auto present = std::make_shared<std::atomic<bool>>(true);
        ScopedLinkHooks hooks(hung.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              100, [present](okuflow::PhoneAdbTestHooks& h) {
            h.adbInterface = [present] {
                return present->load() ? okuflow::PhoneAdbInterface::Present : okuflow::PhoneAdbInterface::Absent; };
            h.beforeDefaultStopForTest = [present] { present->store(false); }; // Unplugged in between.
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("interface is gone") != std::string::npos, reports.back().actionResult.c_str());
        QTest::qWait(300);
        QVERIFY(!hung.Exited());
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
        // Iteration 3 (A2): the phone unplugged reads as such, not "Restoring".
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::NoAdbInterface));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::ServerUnresponsive));
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
        // Iteration 4 (A4): Windows cannot tell; the phone is wanted once the
        // server lists any USB phone, and not while it lists none.
        for (const bool listed : {true, false}) {
            server.listEmpty.store(!listed);
            ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Unknown);
            okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
            okuflow::PhoneLinkReport report;
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, nullptr, {}, nullptr,
                                                     okuflow::PhoneServerIntent::PhoneUse, &request, &report);
            QCOMPARE(int(report.decision.diagnosis), int(listed ? okuflow::PhoneLinkDiagnosis::Unauthorized
                                                                : okuflow::PhoneLinkDiagnosis::NotWanted));
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
        // A16; changed deliberately (iteration 3, A7): no manual pick to
        // renew a throttle. Nothing to launch arms no start throttle, so the
        // sentence comes within the first cycles.
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Present, L"Z:\\okuflow-missing\\adb.exe");
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(4, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart; });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
    }
    void adbThatExitsAtOnceIsNamed() {
        // A7 (suspected): an antivirus lets adb start and kills it before it
        // answers. Those early exits are launch failures of the private
        // server and end in the same sentence (no change was needed); the
        // second failure waits for the private server's own launch back-off
        // (5 s doubling: about 10 s after the first).
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-exit-at-start"});
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(400, [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(20); });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
    }
    void blindOwnedServerIsRestartedWhenWindowsCannotTell() {
        // A4: the interface query answers Unknown; after the stated blind
        // period OkuFlow's own server is restarted, ending in a re-plug.
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Unknown, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::DeviceArrival);
        LinkCycles(1);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        const auto reports = LinkCycles(80, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::ReplugCable; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::RestartOwnedServer));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::ReplugCable));
    }
    void windowsCannotTellNeverStopsAnotherServer() {
        // Iteration 4 (A4): Windows answers Unknown, another adb on the
        // default port lists nothing. After the stated blind period OkuFlow
        // starts its own private server beside it, never asks it to exit or
        // stops it, and ends in the re-plug sentence instead of "Restoring".
        DefaultAdbFixture other({L"--fixture-exit-on-kill"});
        QVERIFY(other.copied && !other.Exited());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Unknown, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(80, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::ReplugCable; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::ReplugCable));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QVERIFY(!other.Exited());
    }
    void hungDefaultEndsInTheReplugSentenceWhenWindowsCannotTell() {
        // A4: a hung default under Unknown is never terminated; it ends in
        // the re-plug sentence after the stated time.
        DefaultAdbFixture hung({L"--fixture-hang"});
        QVERIFY(hung.copied && !hung.Exited());
        ScopedLinkHooks hooks(hung.port, okuflow::PhoneAdbInterface::Unknown, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(40, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::ReplugCable; });
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::ReplugCable));
        QVERIFY(!hung.Exited());
    }
    void companionFailureIsNamedEndToEnd() {
        // A5: the camera is held by another phone app ("busy").
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetProblem("busy");
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(30, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::WaitForUser; });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::CompanionFailed));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        QCOMPARE(server.Launches().size(), qsizetype{0});
    }
    void anotherAttachedPhoneIsUsedWhenAnyPhoneIsChosen() {
        // A6: with "any phone" there is no saved serial: the attached phone
        // is the target. A saved phone that is absent gets its own sentence.
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.listEmpty.store(true);
        server.secondUsbPhone.store(true);
        ScopedLinkHooks hooks(server.port);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(1, {}, "");
        QCOMPARE(QString::fromStdString(reports.back().targetSerial), QStringLiteral("xyz"));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Connected));
        reports = LinkCycles(1, {}, "abc");
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::WantedPhoneNotAttached));
        // A ready other phone is what automatic selection uses either way.
        std::vector<okuflow::CameraDescriptor> cameras(1);
        cameras[0].phoneSerial = "xyz"; cameras[0].symbolicLink = L"okuflow-phone:xyz"; cameras[0].phoneAppReady = true;
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex(cameras, L"okuflow-phone:", true, true), 0);
        QCOMPARE(okuflow::ResolveAutomaticPhoneIndex(cameras, L"okuflow-phone:abc", true, true), 0);
    }
    void shadowIsNotHandedBackToADefaultThatOnlyAnswers() {
        // A8: the hung default recovers but lists no phone: answering is
        // not serving, the private server stays.
        ScopedOwnedAdbHooks hooks({}, 0);
        FakePhoneAdb borrowed(false, false, false, FakePhoneAdb::LaunchMode::None,
                              FakePhoneAdb::RecoveryMode::ServerStalled, false, hooks.defaultPort);
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        for (int attempt = 0; attempt < 3 && !okuflow::OwnedAdbServerPort(); ++attempt)
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
        const auto shadow = okuflow::OwnedAdbServerPort();
        QVERIFY(shadow.has_value());
        borrowed.hostUnresponsive.store(false);
        borrowed.deviceStalled.store(false);
        borrowed.listEmpty.store(true);
        for (int attempt = 0; attempt < 4; ++attempt) {
            okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, nullptr, Intent::PhoneUse);
            QCOMPARE(okuflow::OwnedAdbServerPort(), shadow);
        }
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
    void listingFailureOfTheDefaultIsAConfirmedHang_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("listing hangs") << 1;
        QTest::newRow("listing answers FAIL") << 2;
        QTest::newRow("listing closes") << 3;
    }
    void listingFailureOfTheDefaultIsAConfirmedHang() {
        // Iteration 8 (A1): another program's adb answers host:version but
        // its device listing hangs, fails or closes (an adb stuck in a USB
        // transport after sleep). It was relisted for ever ("Restoring", or
        // "starting" for FAIL and close). Now a listing that stays failed for
        // the confirmation time is a confirmed hang: a private server starts
        // beside it and the hung-default rules end it. This default is the
        // test process itself, so it can never be stopped: a silent one ends
        // in the other-program sentence after a refused stop, one that
        // answers FAIL after the aborted stops.
        QFETCH(int, mode);
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listingMode.store(mode);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 300);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        bool privateServer = false;
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(400, [&](const auto& r) {
            privateServer = privateServer || okuflow::OwnedAdbServerPort().has_value();
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(90); });
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::ServerUnresponsive));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::ServerStarting));
        QVERIFY2(privateServer, "No private server was started beside the hung default.");
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY(other.ServiceCount(QStringLiteral("host:version")) > 0);
    }
    void ownedServerWhoseListingFailsIsRestartedThenNamed() {
        // Iteration 8 (A1): OkuFlow's own server answers host:version (its
        // manager keeps it) while both listing forms answer FAIL. It read
        // "starting" for ever; now the restart rung and its cap, then the
        // re-plug sentence.
        QTemporaryDir directory;
        const QString fail = directory.filePath(QStringLiteral("fail"));
        { QFile file(fail); QVERIFY(file.open(QIODevice::WriteOnly)); }
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-fail-file", fail.toStdWString()});
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(400, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::ReplugCable; });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::ReplugCable));
        QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::ServerStarting));
        const auto restarts = std::count_if(reports.begin(), reports.end(), [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::RestartOwnedServer &&
                r.actionResult.find("owned server restarted") != std::string::npos; });
        QCOMPARE(int(restarts), okuflow::PhoneLinkPolicy{}.blindRestartCap);
    }
    void ownedAdbThatNeverAnswersIsNamed() {
        // Iteration 8 (A2): security software lets OkuFlow's adb.exe run but
        // blocks its socket: the child stays alive and never answers. It was
        // replaced after every start grace and read "starting" for ever.
        // Now two starts that outlive their start grace unanswered end in
        // the AdbCannotStart sentence while recovery goes on, and a server
        // that answers later ends it. (Version answers after 1100 ms while
        // the file exists: past the 1000 ms probe budget, so never in time.)
        QTemporaryDir directory;
        const QString slow = directory.filePath(QStringLiteral("slow"));
        { QFile file(slow); QVERIFY(file.open(QIODevice::WriteOnly)); }
        ScopedLinkHooks hooks(PickTestPort(), okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-slow-version-file", slow.toStdWString()}, 5000, [](okuflow::PhoneAdbTestHooks& h) { h.ownedPolicy = FastOwnedPolicy(); });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(400, [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(60); });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::WaitForUser));
        // The child answers now: the sentence ends.
        QVERIFY(QFile::remove(slow));
        const auto later = LinkCycles(60, [](const auto& r) {
            return r.decision.diagnosis != okuflow::PhoneLinkDiagnosis::AdbCannotStart &&
                r.decision.diagnosis != okuflow::PhoneLinkDiagnosis::ServerStarting; });
        QVERIFY(later.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::AdbCannotStart);
        QCOMPARE(int(later.back().observation.serverLaunchFailures), 0);
    }
    void adbThatCannotStartBesideABlindDefaultIsNamed_data() {
        QTest::addColumn<bool>("missing");
        QTest::newRow("executable missing") << true;
        QTest::newRow("executable exits at start") << false;
    }
    void adbThatCannotStartBesideABlindDefaultIsNamed() {
        // Iteration 8 (A3): another program's adb answers and is blind to the
        // phone; OkuFlow's own adb.exe cannot be started beside it. The end
        // was the other-program sentence after the hand-over cap (the
        // default's Running reset the count); now it is AdbCannotStart.
        QFETCH(bool, missing);
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present,
                              missing ? std::wstring(L"Z:\\okuflow-missing\\adb.exe") : OwnedChildFixturePath().toStdWString(),
                              missing ? std::vector<std::wstring>{} : std::vector<std::wstring>{L"--fixture-exit-at-start"});
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(400, [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(60); });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::StartShadowServer));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        if (!missing) return;
        // Iteration 9 (A8): the start beside the default stays requested
        // while adb.exe is missing, and a session's server request took the
        // same path: every session start through the default failed until an
        // event, also once the default lists the phone. A session uses a
        // responsive default whatever start is requested.
        QVERIFY(okuflow::PhoneAdbShadowRequestedForTest());
        other.listEmpty.store(false);
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(apartment)); QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        auto teardown = qScopeGuard([&] { MFShutdown(); CoUninitialize(); });
        okuflow::PhoneCameraTransport transport;
        okuflow::CameraDescriptor phone; phone.phoneSerial = "abc"; phone.phoneTransportId = 9;
        std::atomic<bool> canceled{false};
        auto result = std::async(std::launch::async, [&] { return transport.Start(phone, nullptr, {}, {}, &canceled); });
        auto cancelOnExit = qScopeGuard([&] { canceled.store(true); });
        QTRY_VERIFY_WITH_TIMEOUT(other.hello.load(), 3000);
        canceled.store(true);
        QVERIFY(result.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        result.get();
        transport.Stop();
    }
    void staleIntentDropsTheLadderSideEffects_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("ask the default to exit") << QStringLiteral("ask");
        QTest::newRow("terminate the default") << QStringLiteral("terminate");
        QTest::newRow("start beside the default") << QStringLiteral("beside");
        QTest::newRow("wake and launch") << QStringLiteral("launch");
    }
    void staleIntentDropsTheLadderSideEffects() {
        // Iteration 8 (A4): the user picks another source while a discovery
        // cycle runs. The request's intent generation is checked right before
        // every side effect: nothing is asked, stopped, started or launched.
        QFETCH(QString, action);
        auto live = std::make_shared<std::atomic<std::uint64_t>>(1);
        okuflow::PhoneLinkRequest request;
        request.intentGeneration = 1;
        request.liveIntentGeneration = live.get();
        const auto changeSource = [live] { live->store(2); };
        QTemporaryDir directory;
        const QString killLog = directory.filePath(QStringLiteral("kills.txt"));
        std::unique_ptr<DefaultAdbFixture> fixture;
        std::unique_ptr<FakePhoneAdb> fake;
        std::unique_ptr<ScopedLinkHooks> hooks;
        okuflow::PhoneLinkAction expected{};
        if (action == QStringLiteral("ask") || action == QStringLiteral("terminate")) {
            const bool ask = action == QStringLiteral("ask");
            fixture = std::make_unique<DefaultAdbFixture>(ask ? std::vector<std::wstring>{L"--fixture-kill-log", killLog.toStdWString()}
                                                              : std::vector<std::wstring>{L"--fixture-hang"});
            QVERIFY(fixture->copied && !fixture->Exited());
            hooks = std::make_unique<ScopedLinkHooks>(fixture->port, okuflow::PhoneAdbInterface::Present,
                OwnedChildFixturePath().toStdWString(), std::vector<std::wstring>{}, ask ? 5000u : 100u,
                [changeSource](okuflow::PhoneAdbTestHooks& h) { h.beforeDefaultStopForTest = changeSource; });
            expected = ask ? okuflow::PhoneLinkAction::AskDefaultServerToExit : okuflow::PhoneLinkAction::TerminateDefaultServer;
        } else if (action == QStringLiteral("beside")) {
            fake = std::make_unique<FakePhoneAdb>(false, false, false, FakePhoneAdb::LaunchMode::None,
                                                  FakePhoneAdb::RecoveryMode::Healthy, false);
            fake->listEmpty.store(true);
            hooks = std::make_unique<ScopedLinkHooks>(fake->port, okuflow::PhoneAdbInterface::Present,
                OwnedChildFixturePath().toStdWString(), std::vector<std::wstring>{}, 5000u,
                [changeSource](okuflow::PhoneAdbTestHooks& h) { h.beforeShadowStartForTest = changeSource; });
            expected = okuflow::PhoneLinkAction::StartShadowServer;
        } else {
            fake = std::make_unique<FakePhoneAdb>(false, false, false, FakePhoneAdb::LaunchMode::Play,
                                                  FakePhoneAdb::RecoveryMode::Healthy, false);
            hooks = std::make_unique<ScopedLinkHooks>(fake->port, okuflow::PhoneAdbInterface::Present, std::wstring{},
                std::vector<std::wstring>{}, 5000u,
                [changeSource](okuflow::PhoneAdbTestHooks& h) { h.beforeCompanionLaunchForTest = changeSource; });
            expected = okuflow::PhoneLinkAction::WakeAndLaunchApp;
        }
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(80, [expected](const auto& r) { return r.decision.action == expected; }, "abc", &request);
        QCOMPARE(int(reports.back().decision.action), int(expected));
        QCOMPARE(QString::fromStdString(reports.back().actionResult), QStringLiteral("dropped: the source changed"));
        QTest::qWait(300);
        if (fixture) {
            QVERIFY(!fixture->Exited());
            QVERIFY2(!QFile::exists(killLog), "host:kill was sent after the source changed.");
        }
        if (action == QStringLiteral("beside")) QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        if (action == QStringLiteral("launch")) {
            QVERIFY(fake->Launches().isEmpty());
            for (const auto& service : fake->Services())
                QVERIFY2(!service.startsWith(QStringLiteral("shell:input")) && !service.startsWith(QStringLiteral("shell:am start")),
                         qPrintable(service));
        }
    }
    void olderDefaultThatListsOnlyTextIsHandedBack() {
        // Iteration 8 (B1): an older adb on the default port answers FAIL to
        // the protobuf listing and lists in text. Its listing beside
        // OkuFlow's server used the protobuf form only, so a default that
        // lists the phone was never handed back to (nor asked, nor stopped).
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.textOnly.store(true);
        other.listEmpty.store(true);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(60, [](const auto&) { return okuflow::OwnedAdbServerPort().has_value(); });
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        other.listEmpty.store(false); // The older adb lists the phone now (text form only).
        reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::HandBackToDefault));
        QCOMPARE(QString::fromStdString(reports.back().actionResult),
                 QStringLiteral("the default server lists the phone; private server retired"));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
    }
    void handBackWaitsForASessionServerRequest() {
        // Iteration 8 (B3): the ladder's hand-back checks for live sessions
        // and stops the private server under the lifecycle lock, as the
        // transport's own hand-back does: a session's server request made in
        // between waits for the hand-back (it then uses the default) instead
        // of registering on a private server that is stopped under it.
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        auto finishedDuringHandBack = std::make_shared<std::atomic<int>>(-1);
        auto session = std::make_shared<std::thread>();
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [finishedDuringHandBack, session](okuflow::PhoneAdbTestHooks& h) {
            h.afterHandBackCheckForTest = [finishedDuringHandBack, session] {
                auto done = std::make_shared<std::atomic<bool>>(false);
                *session = std::thread([done] {
                    bool listed = false;
                    okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr,
                                                             okuflow::PhoneServerIntent::PhoneUse);
                    done->store(true);
                });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
                while (!done->load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                finishedDuringHandBack->store(done->load() ? 1 : 0);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        LinkCycles(60, [](const auto&) { return okuflow::OwnedAdbServerPort().has_value(); });
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        other.listEmpty.store(false);
        const auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        if (session->joinable()) session->join();
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::HandBackToDefault));
        QCOMPARE(finishedDuringHandBack->load(), 0);
    }
    void silentDefaultBesideOkuFlowsServerIsStoppedNotSpent() {
        // Iteration 8 (B2): a blind default ignored host:kill; at the
        // termination's pre-stop check its listing is silent while its
        // version still answers. That check returned "kept: listing failed"
        // without giving the attempt back, so the terminations were spent
        // without one stop (with one termination per episode the hung
        // default was named, never stopped). Now it is an aborted stop until
        // the listing hang is confirmed, then the hung default is stopped
        // through its exact handle. The default is the in-process fake (its
        // version answers, its listing falls silent); its listener rows name
        // a decoy adb.exe this test started, which stands for its process.
        DefaultAdbFixture decoy({});
        QVERIFY(decoy.copied && !decoy.Exited());
        const DWORD decoyPid = decoy.child->ProcessId();
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        const auto port = other.port;
        okuflow::SetTcpListenerRowsTransformForTest([port, decoyPid](std::vector<okuflow::TcpListenerRow>& rows) {
            for (auto& row : rows) if (row.port == port) row.processId = decoyPid;
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        FakePhoneAdb* fake = &other;
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              300, [fake](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->terminateCap = 1;
            // After the ignored ask, the listing falls silent.
            h.beforeDefaultStopForTest = [fake] {
                if (fake->ServiceCount(QStringLiteral("host:kill")) > 0) fake->listingMode.store(1);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(400, [&](const auto& r) {
            return decoy.Exited() || r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(90); });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::AskDefaultServerToExit));
        const auto silent = std::find_if(reports.begin(), reports.end(), [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer &&
                r.actionResult == "kept: the default server listing failed"; });
        QVERIFY2(silent != reports.end(), "The termination never met the silent listing.");
        QVERIFY2(decoy.child->WaitForExit(std::chrono::milliseconds(3000)), "The silent default was never stopped.");
        QVERIFY(!Saw(reports, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
    }
    void anyAddressDefaultListenerIsTerminatedThroughItsExactHandle() {
        // Iteration 8 (B8): the listener lookups read only 127.0.0.1 rows. An
        // adb bound to every address answers OkuFlow's probes but was "gone"
        // at every ask and stop. With no loopback row, the any-address row
        // of the port is used under the same identity proof. (The fixture
        // binds loopback; the rows are rewritten so it stands for an
        // any-address listener without binding every address.)
        DefaultAdbFixture hung({L"--fixture-hang"});
        QVERIFY(hung.copied && !hung.Exited());
        const auto port = hung.port;
        okuflow::SetTcpListenerRowsTransformForTest([port](std::vector<okuflow::TcpListenerRow>& rows) {
            for (auto& row : rows)
                if (row.port == port && row.loopback) { row.loopback = false; row.anyAddress = true; }
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        ScopedLinkHooks hooks(hung.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 100);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(reports.back().actionResult.find("terminated through its exact handle") != std::string::npos,
                 reports.back().actionResult.c_str());
        QVERIFY(hung.child->WaitForExit(std::chrono::milliseconds(3000)));
    }
    void adbListenerRowsPreferLoopbackThenAnyAddress() {
        // Iteration 8 (B8): the one rule both lookups use.
        using Row = okuflow::TcpListenerRow;
        const auto row = [](bool ipv6, bool loopback, bool any, std::uint16_t port, DWORD pid) {
            Row r; r.ipv6 = ipv6; r.loopback = loopback; r.anyAddress = any; r.port = port; r.processId = pid; return r; };
        const auto pids = [](const std::vector<Row>& rows) {
            std::vector<DWORD> out;
            for (const auto& r : rows) out.push_back(r.processId);
            return out;
        };
        // Loopback wins over an any-address row of another process.
        QCOMPARE(pids(okuflow::AdbListenerRows({row(false, false, true, 5037, 7), row(false, true, false, 5037, 9)}, 5037)),
                 (std::vector<DWORD>{9}));
        // No loopback row: 0.0.0.0 and :: both stand for it.
        QCOMPARE(pids(okuflow::AdbListenerRows({row(false, false, true, 5037, 7)}, 5037)), (std::vector<DWORD>{7}));
        QCOMPARE(pids(okuflow::AdbListenerRows({row(true, false, true, 5037, 8)}, 5037)), (std::vector<DWORD>{8}));
        // ::1 alone is never reached by 127.0.0.1; other ports never count.
        QVERIFY(okuflow::AdbListenerRows({row(true, true, false, 5037, 8)}, 5037).empty());
        QVERIFY(okuflow::AdbListenerRows({row(false, true, false, 5038, 8), row(false, false, true, 5038, 9)}, 5037).empty());
    }
    void startRequestIsMootWhenAPrivateServerAlreadyRuns() {
        // Iteration 8 (B9), for the post-round-7 clear: the doctor decided a
        // private server beside a blind default, and by the time the start
        // runs OkuFlow's own server already exists (the default went away
        // and a phone-use call engaged it). The request is met or moot; it
        // must not wait for a later call and start a server nobody asked for.
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        FakePhoneAdb* fake = &other;
        auto armed = std::make_shared<std::atomic<bool>>(false);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [fake, armed](okuflow::PhoneAdbTestHooks& h) {
            h.beforeShadowStartForTest = [fake, armed] {
                if (armed->exchange(true)) return;
                fake->closeListener.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                for (int i = 0; i < 20 && !okuflow::OwnedAdbServerPort(); ++i)
                    okuflow::PhoneCameraTransport::Enumerate({}, nullptr, nullptr, "abc", nullptr,
                                                             okuflow::PhoneServerIntent::PhoneUse);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(armed->load());
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        QVERIFY2(!okuflow::PhoneAdbShadowRequestedForTest(), "A start request outlived the private server that met it.");
    }
    void defaultGoneAtEveryAskEndsInTheSentenceEndToEnd() {
        // Iteration 8 (B9): the post-round-7 change through the executor: a
        // default the lookup finds gone at every ask (it answers listings,
        // then is not in the table when the ask looks) ends in the
        // other-program sentence instead of being asked for ever.
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        other.reopenAfterGone.store(true);
        FakePhoneAdb* gone = &other;
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [gone](okuflow::PhoneAdbTestHooks& h) {
            h.beforeDefaultStopForTest = [gone] { gone->goneAtNextConnection.store(true); };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(600, [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(90); });
        const auto asks = std::count_if(reports.begin(), reports.end(), [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit &&
                r.actionResult == "default server already gone"; });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QCOMPARE(int(asks), okuflow::PhoneLinkPolicy{}.abortedStopCap + 1);
    }
    // ---- Iteration 9 (council round 9) ----
    void staleIntentInsideTheServerRequestStartsNothing_data() {
        QTest::addColumn<QString>("stage");
        QTest::newRow("while the lifecycle lock is awaited") << QStringLiteral("locked");
        QTest::newRow("while the default is probed") << QStringLiteral("default-probed");
    }
    void staleIntentInsideTheServerRequestStartsNothing() {
        // Iteration 9 (A1): the start beside a blind default asks the server
        // request (EnsureAdbServer), which took the lifecycle lock, probed
        // the default and started a private server with no intent check of
        // its own. The user changes the source inside that request: nothing
        // is started and the start request does not outlive it.
        QFETCH(QString, stage);
        auto live = std::make_shared<std::atomic<std::uint64_t>>(1);
        okuflow::PhoneLinkRequest request;
        request.intentGeneration = 1;
        request.liveIntentGeneration = live.get();
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        auto armed = std::make_shared<std::atomic<bool>>(false);
        const std::string wantedStage = stage.toStdString();
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [armed, live, wantedStage](okuflow::PhoneAdbTestHooks& h) {
            h.beforeShadowStartForTest = [armed] { armed->store(true); };
            h.linkStageForTest = [armed, live, wantedStage](std::string_view at) {
                if (armed->load() && at == wantedStage) live->store(2);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(80, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; }, "abc", &request);
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(armed->load());
        QCOMPARE(QString::fromStdString(reports.back().actionResult), QStringLiteral("dropped: the source changed"));
        QTest::qWait(300);
        QVERIFY2(!okuflow::OwnedAdbServerPort().has_value(), "A private server started after the source changed.");
        QVERIFY(!okuflow::PhoneAdbShadowRequestedForTest());
    }
    void hostKillIsNotWrittenAfterTheSourceChanged() {
        // Iteration 9 (A1): the ask checked the request's authority, then the
        // raw host:kill connected, waited for writability and checked the
        // listener before it wrote, without asking again. The user changes
        // the source between that connect and the write: nothing is written.
        auto live = std::make_shared<std::atomic<std::uint64_t>>(1);
        okuflow::PhoneLinkRequest request;
        request.intentGeneration = 1;
        request.liveIntentGeneration = live.get();
        QTemporaryDir directory;
        const QString killLog = directory.filePath(QStringLiteral("kills.txt"));
        DefaultAdbFixture fixture({L"--fixture-kill-log", killLog.toStdWString()});
        QVERIFY(fixture.copied && !fixture.Exited());
        ScopedLinkHooks hooks(fixture.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              5000, [live](okuflow::PhoneAdbTestHooks& h) {
            h.afterHostKillConnectForTest = [live] { live->store(2); };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(120, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; }, "abc", &request);
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::AskDefaultServerToExit));
        QCOMPARE(QString::fromStdString(reports.back().actionResult), QStringLiteral("dropped: the source changed"));
        QTest::qWait(300);
        QVERIFY2(!QFile::exists(killLog), "host:kill was written after the source changed.");
        QVERIFY(!fixture.Exited());
    }
    void staleCycleLeavesTheEpisodeBudgetToTheNextCycle_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("reconnect") << QStringLiteral("reconnect");
        QTest::newRow("restart") << QStringLiteral("restart");
        QTest::newRow("hand-back") << QStringLiteral("hand-back");
        QTest::newRow("wake and launch") << QStringLiteral("launch");
    }
    void staleCycleLeavesTheEpisodeBudgetToTheNextCycle() {
        // Iteration 9 (A2): a cycle whose source changed after its
        // observation still decided, reserved the action's budget and then
        // dropped the action; reconnect, restart, hand-back and launch kept
        // the reservation. After an event the next cycle had to wait for the
        // retry spacing instead of acting at once. The stale cycle now never
        // reaches the doctor. Retry spacings here are far above a cycle.
        QFETCH(QString, action);
        auto live = std::make_shared<std::atomic<std::uint64_t>>(1);
        auto armed = std::make_shared<std::atomic<bool>>(false);
        const auto customize = [live, armed](okuflow::PhoneAdbTestHooks& h) {
            h.linkPolicy->reconnectRetryMs = h.linkPolicy->reconnectCeilingMs = 60000;
            h.linkPolicy->restartRetryMs = h.linkPolicy->restartCeilingMs = 60000;
            h.linkPolicy->launchRetryMs = {60000, 60000};
            h.linkPolicy->launchPeriodicMs = 60000;
            // The source changes once the cycle observed everything.
            h.linkStageForTest = [live, armed](std::string_view at) {
                if (at == "decide" && armed->exchange(false)) live->fetch_add(1);
            };
        };
        std::unique_ptr<FakePhoneAdb> fake;
        std::unique_ptr<ScopedLinkHooks> hooks;
        okuflow::PhoneLinkAction expected{};
        const auto request = [live] {
            okuflow::PhoneLinkRequest r;
            r.intentGeneration = live->load();
            r.liveIntentGeneration = live.get();
            return r;
        };
        if (action == QStringLiteral("reconnect")) {
            fake = std::make_unique<FakePhoneAdb>(false, false, false, FakePhoneAdb::LaunchMode::None,
                                                  FakePhoneAdb::RecoveryMode::Healthy, false);
            fake->listingState.store(6); // Offline.
            hooks = std::make_unique<ScopedLinkHooks>(fake->port, okuflow::PhoneAdbInterface::Present, std::wstring{},
                                                      std::vector<std::wstring>{}, 5000u, customize);
            expected = okuflow::PhoneLinkAction::ReconnectTransport;
        } else if (action == QStringLiteral("launch")) {
            fake = std::make_unique<FakePhoneAdb>(false, false, false, FakePhoneAdb::LaunchMode::Play,
                                                  FakePhoneAdb::RecoveryMode::Healthy, false);
            hooks = std::make_unique<ScopedLinkHooks>(fake->port, okuflow::PhoneAdbInterface::Present, std::wstring{},
                                                      std::vector<std::wstring>{}, 5000u, customize);
            expected = okuflow::PhoneLinkAction::WakeAndLaunchApp;
        } else if (action == QStringLiteral("restart")) {
            hooks = std::make_unique<ScopedLinkHooks>(PickTestPort(), okuflow::PhoneAdbInterface::Present,
                                                      OwnedChildFixturePath().toStdWString(), std::vector<std::wstring>{},
                                                      5000u, customize);
            expected = okuflow::PhoneLinkAction::RestartOwnedServer;
        } else {
            fake = std::make_unique<FakePhoneAdb>(false, false, false, FakePhoneAdb::LaunchMode::None,
                                                  FakePhoneAdb::RecoveryMode::Healthy, false);
            fake->listEmpty.store(true);
            hooks = std::make_unique<ScopedLinkHooks>(fake->port, okuflow::PhoneAdbInterface::Present,
                                                      OwnedChildFixturePath().toStdWString(), std::vector<std::wstring>{},
                                                      5000u, customize);
            expected = okuflow::PhoneLinkAction::HandBackToDefault;
        }
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        if (action == QStringLiteral("restart")) {
            // The blind grace runs from the first blind decision.
            auto r = request();
            LinkCycles(20, [](const auto& report) {
                return report.decision.diagnosis == okuflow::PhoneLinkDiagnosis::NotListedByServer; }, "abc", &r);
            QTest::qWait(300);
        } else if (action == QStringLiteral("hand-back")) {
            auto r = request();
            LinkCycles(60, [](const auto&) { return okuflow::OwnedAdbServerPort().has_value(); }, "abc", &r);
            QVERIFY(okuflow::OwnedAdbServerPort().has_value());
            fake->listEmpty.store(false); // The default lists the phone again.
        }
        armed->store(true);
        auto staleRequest = request();
        const auto stale = LinkCycles(1, {}, "abc", &staleRequest);
        QVERIFY(!armed->load());
        auto freshRequest = request();
        const auto fresh = LinkCycles(1, {}, "abc", &freshRequest);
        // The budget: the next cycle acts at once, as after the event.
        QCOMPARE(int(fresh.back().decision.action), int(expected));
        QVERIFY(fresh.back().actionResult.find("dropped") == std::string::npos);
        QCOMPARE(QString::fromStdString(stale.back().actionResult),
                 QStringLiteral("dropped before its decision: the source changed"));
        QCOMPARE(int(stale.back().decision.action), int(okuflow::PhoneLinkAction::None));
    }
    void privateServerThatDiesAfterItsReadinessBesideADefaultIsCounted() {
        // Iteration 9 (A3): OkuFlow's private server beside a responsive,
        // blind default outlives its readiness deadline silent, then dies
        // (security software that lets adb.exe run for a few seconds). The
        // next cycle borrowed the default and dropped the manager with its
        // count: every try charged a hand-over and the end was the
        // other-program sentence. The death is kept with the launch
        // failures: the end is "cannot start", and once adb.exe answers the
        // sentence ends without an event. (The child's version answers
        // after 1100 ms while the file exists, past the probe budget; it
        // exits on host:kill, which the test sends once it went silent.)
        QTemporaryDir directory;
        const QString slow = directory.filePath(QStringLiteral("slow"));
        { QFile file(slow); QVERIFY(file.open(QIODevice::WriteOnly)); }
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-slow-version-file", slow.toStdWString(), L"--fixture-exit-on-kill"}, 5000,
                              [](okuflow::PhoneAdbTestHooks& h) {
            auto owned = FastOwnedPolicy();
            owned.startGrace = std::chrono::minutes(10); // Only deaths count here.
            h.ownedPolicy = owned;
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        int killed = 0;
        const auto reports = LinkCycles(400, [&](const auto& r) {
            if (r.decision.action == okuflow::PhoneLinkAction::StartShadowServer) {
                if (const auto port = okuflow::OwnedAdbServerPort()) {
                    SendTestHostKill(*port);
                    ++killed;
                }
            }
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(90); });
        QVERIFY(killed >= 1);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        // adb.exe answers now: the rung's retry starts it, the sentence ends.
        QVERIFY(QFile::remove(slow));
        const auto later = LinkCycles(200, [&](const auto& r) {
            return r.decision.diagnosis != okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(150); });
        QVERIFY(later.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::AdbCannotStart);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void previouslyHealthyOwnedListenerMustRemainVerifiable_data() {
        QTest::addColumn<bool>("shadow");
        QTest::newRow("private server") << false;
        QTest::newRow("idle shadow") << true;
    }
    void previouslyHealthyOwnedListenerMustRemainVerifiable() {
        QFETCH(bool, shadow);
        const auto defaultPort = PickTestPort();
        QVERIFY(defaultPort);
        std::optional<std::uint16_t> privatePort;
        bool foreignOwner = false;
        int privateListings = 0;
        std::unique_ptr<FakePhoneAdb> borrowed;
        if (shadow)
            borrowed = std::make_unique<FakePhoneAdb>(false, false, false, FakePhoneAdb::LaunchMode::None,
                FakePhoneAdb::RecoveryMode::ServerStalled, false, defaultPort);
        const auto flags = shadow ? std::vector<std::wstring>{} : std::vector<std::wstring>{L"--fixture-device"};
        ScopedLinkHooks hooks(defaultPort, okuflow::PhoneAdbInterface::Present,
                              OwnedChildFixturePath().toStdWString(), flags, shadow ? 0 : 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            auto policy = FastOwnedPolicy();
            policy.readinessDeadline = std::chrono::seconds(2);
            h.ownedPolicy = policy;
            h.transformAdbEntriesForTest = [&](std::uint16_t port, auto&) {
                if (privatePort && port == *privatePort) ++privateListings;
            };
        });
        okuflow::SetTcpListenerRowsTransformForTest([&](std::vector<okuflow::TcpListenerRow>& rows) {
            if (!foreignOwner || !privatePort) return;
            for (auto& row : rows)
                if (row.port == *privatePort) row.processId = GetCurrentProcessId();
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        if (shadow) {
            using Intent = okuflow::PhoneServerIntent;
            bool listed = false;
            okuflow::PhoneDiscoveryStatus status{};
            const auto enumerate = [&] {
                return okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", &status, Intent::PhoneUse);
            };
            for (int i = 0; i < 2; ++i)
                (void)okuflow::PhoneCameraTransport::Enumerate({}, nullptr, nullptr, {}, nullptr, Intent::BorrowOnly);
            QVERIFY(enumerate().empty());
            QVERIFY(listed);
            privatePort = okuflow::OwnedAdbServerPort();
            QVERIFY(privatePort.has_value());

            borrowed->hostUnresponsive.store(false);
            borrowed->deviceStalled.store(false);
            foreignOwner = true;
            for (int i = 0; i < 2; ++i) {
                QVERIFY(enumerate().empty());
                QCOMPARE(status, okuflow::PhoneDiscoveryStatus::OwnedServerRecovering);
                QVERIFY(!listed);
                QCOMPARE(okuflow::OwnedAdbServerPort(), privatePort);
            }
            QCOMPARE(privateListings, 0);

            foreignOwner = false;
            QVERIFY(enumerate().empty()); // One verified observation is insufficient.
            QCOMPARE(okuflow::OwnedAdbServerPort(), privatePort);
            QVERIFY(!enumerate().empty());
            QVERIFY(listed);
            QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
            return;
        }
        const auto healthy = LinkCycles(1);
        privatePort = okuflow::OwnedAdbServerPort();
        QVERIFY(privatePort.has_value());
        QCOMPARE(healthy.back().observation.serverProbe, okuflow::PhoneServerProbe::Responsive);
        QCOMPARE(healthy.back().observation.serverLaunchFailures, 0);
        foreignOwner = true;
        const auto failed = LinkCycles(30, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart;
        });
        QCOMPARE(failed.front().decision.diagnosis, okuflow::PhoneLinkDiagnosis::ServerStarting);
        QCOMPARE(failed.back().decision.diagnosis, okuflow::PhoneLinkDiagnosis::AdbCannotStart);
        QCOMPARE(failed.back().decision.action, okuflow::PhoneLinkAction::WaitForUser);
        QCOMPARE(privateListings, 0);
        QCOMPARE(okuflow::OwnedAdbServerPort(), privatePort);

        foreignOwner = false;
        const auto recovered = LinkCycles(1);
        QCOMPARE(recovered.back().observation.serverProbe, okuflow::PhoneServerProbe::Responsive);
        QCOMPARE(recovered.back().observation.serverLaunchFailures, 0);
        QVERIFY(recovered.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::AdbCannotStart);
        QVERIFY(privateListings > 0);
        QCOMPARE(okuflow::OwnedAdbServerPort(), privatePort);
    }
    void legacyReclaimOfAListingHangNeedsAFreshSilentListing() {
        // Iteration 9 (A4): a confirmed LISTING hang (the version answers)
        // entered the hung branch and the legacy reclamation with no fresh
        // listing: an old bundled adb whose listings failed twice and that
        // recovered and serves another device was stopped. One fresh listing
        // right before the reclamation now: an answer ends the hang and
        // nothing is reclaimed. The reclamation runs in production only; the
        // fixture reaches the branch through its seam.
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listingMode.store(3); // Closed at once: a failed listing, without a hang's time-out.
        auto reclaims = std::make_shared<std::atomic<int>>(0);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              300, [reclaims](okuflow::PhoneAdbTestHooks& h) {
            h.legacyReclaimForTest = [reclaims] { ++*reclaims; };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        // Two failed listings of the same listener 300 ms apart confirm it.
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        QVERIFY(!listed);
        QTest::qWait(350);
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        QVERIFY(!listed);
        QCOMPARE(reclaims->load(), 0);
        // It recovered and serves another device.
        other.listingMode.store(0);
        other.otherDevice.store(true);
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        QCOMPARE(reclaims->load(), 0);
        QVERIFY(listed);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        // The counterpart: a fresh listing that stays silent lets it go on.
        other.otherDevice.store(false);
        other.listingMode.store(3);
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        QTest::qWait(350);
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        QCOMPARE(reclaims->load(), 1);
    }
    void listingHangOfOneListenerIsNotAnothers() {
        // Iteration 9 (A5): a listing hang confirmed for listener A, A exits
        // and B listens at once, slow at start (its version and listing
        // hang), with no observation of "no listener" between them. The
        // listing evidence stayed and the ladder read it with no identity:
        // B was stopped in its first cycle, without its own confirmation.
        // Listener A is the in-process fake; B is a decoy adb.exe this test
        // started, which the rewritten listener rows name for the same port.
        DefaultAdbFixture decoy({});
        QVERIFY(decoy.copied && !decoy.Exited());
        const DWORD decoyPid = decoy.child->ProcessId();
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listingMode.store(3); // Closed at once: a failed listing, without a hang's time-out.
        const auto port = other.port;
        auto replaced = std::make_shared<std::atomic<bool>>(false);
        okuflow::SetTcpListenerRowsTransformForTest([port, decoyPid, replaced](std::vector<okuflow::TcpListenerRow>& rows) {
            if (!replaced->load()) return;
            for (auto& row : rows) if (row.port == port) row.processId = decoyPid;
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {}, 300);
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        using Intent = okuflow::PhoneServerIntent;
        bool listed = false;
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        QTest::qWait(350);
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, "abc", nullptr, Intent::PhoneUse);
        QVERIFY(!listed); // A's listing hang is confirmed now.
        replaced->store(true);
        other.hostUnresponsive.store(true);
        const auto first = LinkCycles(1);
        QVERIFY(first.back().decision.action != okuflow::PhoneLinkAction::TerminateDefaultServer);
        QVERIFY(!first.back().observation.defaultUnresponsiveConfirmed);
        QVERIFY2(!decoy.Exited(), "A new listener was stopped on the old listener's evidence.");
        // B is stopped once its own hang is confirmed (as every hung default:
        // hungDefaultIsTerminatedOnlyThroughItsExactHandle and its siblings).
    }
    void listingHungDefaultAtTheHandOverCapIsListedAgain() {
        // Iteration 9 (A6): at the hand-over cap a listing-hung default
        // returned BorrowedUnresponsive and discovery never listed it again:
        // the other-program sentence outlived a default that recovered. It
        // is borrowed and listed again; a listing that succeeds ends the hang
        // and the phone is used. (Cap 0: no private server beside it at all;
        // the in-process default cannot be stopped, so its stop is refused.)
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listingMode.store(3); // Closed at once: a failed listing, without a hang's time-out.
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              300, [](okuflow::PhoneAdbTestHooks& h) { h.linkPolicy->handOverCap = 0; });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(200, [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(60); });
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        other.listingMode.store(0); // It recovered.
        const auto later = LinkCycles(60, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::Connected; });
        QCOMPARE(int(later.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Connected));
    }
    void silentPrivateServerIsHandedBackToADefaultThatListsThePhone() {
        // Iteration 9 (A7): OkuFlow's private server beside a default that
        // was blind once stays alive and silent (blocked socket); the
        // default lists the phone again. ServerStarting never looked at the
        // default: OkuFlow stayed on its own broken server. It is handed back.
        QTemporaryDir directory;
        const QString slow = directory.filePath(QStringLiteral("slow"));
        { QFile file(slow); QVERIFY(file.open(QIODevice::WriteOnly)); }
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-slow-version-file", slow.toStdWString()}, 5000, [](okuflow::PhoneAdbTestHooks& h) {
            auto owned = FastOwnedPolicy();
            owned.startGrace = std::chrono::minutes(10);
            h.ownedPolicy = owned;
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        LinkCycles(60, [](const auto& r) { return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QVERIFY(okuflow::OwnedAdbServerPort().has_value()); // Alive and silent.
        other.listEmpty.store(false);
        const auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::Connected; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkAction::HandBackToDefault));
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Connected));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
    }
    void adbThatCannotStartBesideADefaultEndsOnceItCanStart() {
        // Iteration 9 (B1), for the orchestrator's retry after round 8: the
        // "cannot start" sentence beside an answering default stands, an
        // event drops any pending start request, adb.exe becomes startable
        // (the user allowed it), and the sentence ends with no further event:
        // only the rung's own retry starts the private server.
        QTemporaryDir directory;
        const QString later = directory.filePath(QStringLiteral("adb_later.exe"));
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, later.toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto started = std::chrono::steady_clock::now();
        const auto named = [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(60); };
        auto reports = LinkCycles(400, named);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::DeviceArrival);
        reports = LinkCycles(400, named);
        QCOMPARE(int(reports.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::AdbCannotStart));
        QVERIFY(QFile::copy(OwnedChildFixturePath(), later));
        const auto after = LinkCycles(200, [&](const auto& r) {
            return r.decision.diagnosis != okuflow::PhoneLinkDiagnosis::AdbCannotStart ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(120); });
        QVERIFY(after.back().decision.diagnosis != okuflow::PhoneLinkDiagnosis::AdbCannotStart);
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
    }
    void onlyALivePhoneSessionForbidsAStop() {
        // After iteration 9: the ask and termination executors refused for
        // any REGISTERED session. A manual pick or an automatic reconnect
        // that retries against a hung default is registered for most of that
        // time, so the hung server was named ("another program") instead of
        // stopped. Only a session that delivers frames forbids the stop.
        using okuflow::PhoneSessionBlocksStop;
        const std::int64_t now = 100000;
        QVERIFY(PhoneSessionBlocksStop(true, now - 100, now));                              // Live.
        QVERIFY(!PhoneSessionBlocksStop(true, -1, now));                                    // Starting: no frame yet.
        QVERIFY(!PhoneSessionBlocksStop(true, now - okuflow::kLivePhoneSessionFrameMs, now)); // Reconnecting: frames stopped.
        QVERIFY(!PhoneSessionBlocksStop(false, now - 100, now));                            // Ended a moment ago.
    }
    void stopsAreRefusedWhileAPhoneSessionIsLive_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("ask the default to exit") << QStringLiteral("ask");
        QTest::newRow("terminate the default") << QStringLiteral("terminate");
    }
    void stopsAreRefusedWhileAPhoneSessionIsLive() {
        // Iteration 9 (A9): the termination executor never looked at the
        // sessions, and the doctor's "a live session" measured presented
        // frames (none behind a PDF page): a hidden live session's server
        // could be stopped. Ask and terminate refuse while a phone session
        // is live (delivering frames; the hook reports one), as aborted
        // stops: the rung ends in the sentence, never in a stop. (The
        // session goes live between the decision and the stop.) Renamed
        // after round 10 (it said "registered", the rule the orchestrator
        // reverted); onlyALiveRealSessionKeepsAConfirmedHungDefault covers
        // the same refusal with real sessions.
        QFETCH(QString, action);
        const bool ask = action == QStringLiteral("ask");
        auto session = std::make_shared<std::atomic<bool>>(false);
        QTemporaryDir directory;
        const QString killLog = directory.filePath(QStringLiteral("kills.txt"));
        DefaultAdbFixture fixture(ask ? std::vector<std::wstring>{L"--fixture-kill-log", killLog.toStdWString()}
                                      : std::vector<std::wstring>{L"--fixture-hang"});
        QVERIFY(fixture.copied && !fixture.Exited());
        ScopedLinkHooks hooks(fixture.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              ask ? 5000u : 100u, [session](okuflow::PhoneAdbTestHooks& h) {
            h.beforeDefaultStopForTest = [session] { session->store(true); };
            h.phoneSessionLiveForTest = [session] { return session->load(); };
            h.linkPolicy->abortedStopCap = 1;
        });
        const auto expected = ask ? okuflow::PhoneLinkAction::AskDefaultServerToExit
                                  : okuflow::PhoneLinkAction::TerminateDefaultServer;
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(120, [expected](const auto& r) { return r.decision.action == expected; });
        QCOMPARE(int(reports.back().decision.action), int(expected));
        QCOMPARE(QString::fromStdString(reports.back().actionResult), QStringLiteral("kept: a live phone session uses the connection"));
        const auto started = std::chrono::steady_clock::now();
        const auto later = LinkCycles(400, [&](const auto& r) {
            return r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(60); });
        QCOMPARE(int(later.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone));
        QTest::qWait(300);
        QVERIFY2(!fixture.Exited(), "The server of a live session was stopped.");
        QVERIFY2(!QFile::exists(killLog), "host:kill was sent while a session was live.");
    }
    // ---- After round 10 (final council) ----
    void interruptedPrivateServerStartIsNotCountedAtRetirement_data() {
        QTest::addColumn<QString>("start");
        QTest::newRow("interrupted inside its readiness wait") << QStringLiteral("interrupted");
        QTest::newRow("readiness wait ran to its deadline") << QStringLiteral("deadline");
        QTest::newRow("interrupted, then silent to a probe past the deadline") << QStringLiteral("probed");
    }
    void interruptedPrivateServerStartIsNotCountedAtRetirement() {
        // After round 10 (1): a child retired alive and silent was counted as
        // a failed launch by its age alone (past the readiness deadline),
        // also when its readiness wait was cut short by a source change and
        // never judged. Only what was observed counts now: a readiness wait
        // that ran out, or a probe that found the child silent past it.
        QFETCH(QString, start);
        QTemporaryDir directory;
        auto state = std::make_shared<OwnedLaunchState>();
        state->fixture = OwnedChildFixturePath(); state->directory = directory.path();
        QVERIFY(QFile::exists(state->fixture));
        auto policy = TestOwnedPolicy(std::chrono::milliseconds(0));
        policy.readinessDeadline = std::chrono::milliseconds(300);
        policy.startGrace = std::chrono::minutes(10); // No count for a start grace here.
        okuflow::OwnedAdbServer server(
            [](std::uint16_t, const std::atomic<bool>*) { return okuflow::OwnedServerProbe::Unresponsive; },
            [state](std::uint16_t port) { return state->Launch(port); },
            [] { return PickTestPort(); }, [] { return false; }, policy);
        using Result = okuflow::OwnedAdbServer::EnsureResult;
        if (start == QStringLiteral("deadline")) {
            QCOMPARE(int(server.EnsureRunning(nullptr)), int(Result::NotReady));
        } else {
            // The source changes 100 ms after the child was launched.
            std::optional<std::chrono::steady_clock::time_point> launchedAt;
            const auto stillWanted = [state, &launchedAt] {
                if (state->launches.load() == 0) return true;
                if (!launchedAt) launchedAt = std::chrono::steady_clock::now();
                return std::chrono::steady_clock::now() - *launchedAt < std::chrono::milliseconds(100);
            };
            QCOMPARE(int(server.EnsureRunning(nullptr, true, stillWanted)), int(Result::Canceled));
        }
        QCOMPARE(server.LaunchCount(), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(400)); // Retired past the deadline.
        if (start == QStringLiteral("probed"))
            QCOMPARE(int(server.EnsureRunning(nullptr, false)), int(Result::NotReady));
        QCOMPARE(server.UnansweredStartsAtRetirement(), start == QStringLiteral("interrupted") ? 0 : 1);
        server.Stop();
    }
    void webcamReleaseOfAStartingPrivateServerCountsNothing() {
        // After round 10 (1, 5): OkuFlow's private server beside a blind
        // default was started and has not answered (its readiness wait ran
        // out); the user picks a webcam. The release added the silent child
        // to the launch failures (twice and the next phone pick said "cannot
        // start"), and left the shadow flags set, so the empty default
        // listing read as OkuFlow's own server recovering. (The child's
        // version answers after 1100 ms while the file exists, past the probe
        // budget.)
        QTemporaryDir directory;
        const QString slow = directory.filePath(QStringLiteral("slow"));
        { QFile file(slow); QVERIFY(file.open(QIODevice::WriteOnly)); }
        FakePhoneAdb other(false, false, false, FakePhoneAdb::LaunchMode::None,
                           FakePhoneAdb::RecoveryMode::Healthy, false);
        other.listEmpty.store(true);
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-slow-version-file", slow.toStdWString()}, 5000, [](okuflow::PhoneAdbTestHooks& h) {
            auto owned = FastOwnedPolicy();
            owned.startGrace = std::chrono::minutes(10);
            h.ownedPolicy = owned;
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value()); // Alive and silent past its readiness.
        okuflow::PhoneDiscoveryStatus status{};
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, nullptr, {}, &status, okuflow::PhoneServerIntent::ReleaseForWebcam);
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        // Item 5: no private server shadows the default any more.
        QCOMPARE(int(status), int(okuflow::PhoneDiscoveryStatus::Listed));
        // Item 1: the user's choice says nothing about adb.exe.
        const auto next = LinkCycles(1);
        QCOMPARE(next.back().observation.serverLaunchFailures, 0);
    }
    void defaultStopKeepsAListenerThatReplacedTheOneListed_data() {
        QTest::addColumn<bool>("terminate");
        QTest::newRow("ask") << false;
        QTest::newRow("terminate after an ignored ask") << true;
    }
    void defaultStopKeepsAListenerThatReplacedTheOneListed() {
        QFETCH(bool, terminate);
        DefaultAdbFixture first({}), replacement({});
        QVERIFY(first.copied && !first.Exited() && replacement.copied && !replacement.Exited());
        const DWORD firstPid = first.child->ProcessId();
        const DWORD replacementPid = replacement.child->ProcessId();
        std::atomic<bool> stopping{false}, armed{false}, replaced{false};
        FakePhoneAdb fake(false, false, false, FakePhoneAdb::LaunchMode::None,
                          FakePhoneAdb::RecoveryMode::Healthy, false);
        fake.listEmpty.store(true);
        fake.SetBeforeListingReply([&] {
            if (armed.exchange(false)) {
                // The empty reply has already been prepared for the old
                // identity. The replacement serves another device.
                fake.otherDevice.store(true);
                replaced.store(true);
            }
        });
        const auto port = fake.port;
        okuflow::SetTcpListenerRowsTransformForTest([&](std::vector<okuflow::TcpListenerRow>& rows) {
            for (auto& row : rows)
                if (row.port == port) row.processId = replaced.load() ? replacementPid : firstPid;
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        ScopedLinkHooks hooks(port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              100, [&](okuflow::PhoneAdbTestHooks& h) {
            h.defaultListenerUnopenable = [&] {
                // The termination must be protected even when earlier
                // discovery couldn't record a decision-time identity.
                return terminate && (!stopping.load() ||
                    (!armed.load() && !replaced.load() && fake.ServiceCount("host:kill") > 0));
            };
            h.beforeDefaultStopForTest = [&] {
                stopping.store(true);
                if (!terminate || fake.ServiceCount("host:kill") > 0) armed.store(true);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto expected = terminate ? okuflow::PhoneLinkAction::TerminateDefaultServer
                                        : okuflow::PhoneLinkAction::AskDefaultServerToExit;
        const auto reports = LinkCycles(40, [&](const auto& r) { return r.decision.action == expected; });
        QCOMPARE(reports.back().decision.action, expected);
        QVERIFY(replaced.load());
        QCOMPARE(fake.ServiceCount("host:kill"), terminate ? 1 : 0);
        QVERIFY(!replacement.Exited());
        QCOMPARE(reports.back().actionResult, "refused: the listener changed since the decision");
    }
    void defaultStopRefusesAmbiguousListingOwner_data() {
        QTest::addColumn<bool>("terminate");
        QTest::newRow("ask") << false;
        QTest::newRow("terminate after an ignored ask") << true;
    }
    void defaultStopRefusesAmbiguousListingOwner() {
        QFETCH(bool, terminate);
        DefaultAdbFixture first({}), second({});
        QVERIFY(first.copied && !first.Exited() && second.copied && !second.Exited());
        const DWORD firstPid = first.child->ProcessId(), secondPid = second.child->ProcessId();
        FakePhoneAdb fake(false, false, false, FakePhoneAdb::LaunchMode::None,
                          FakePhoneAdb::RecoveryMode::Healthy, false);
        fake.listEmpty.store(true);
        bool ambiguousNext = false;
        int ambiguousLookups = 0;
        okuflow::SetTcpListenerRowsTransformForTest([&](std::vector<okuflow::TcpListenerRow>& rows) {
            for (auto& row : rows) {
                if (row.port != fake.port || row.ipv6 || !row.loopback) continue;
                row.processId = firstPid;
                if (std::exchange(ambiguousNext, false)) {
                    auto other = row;
                    other.processId = secondPid;
                    rows.push_back(other);
                    ++ambiguousLookups;
                }
                break;
            }
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        ScopedLinkHooks hooks(fake.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              100, [&](okuflow::PhoneAdbTestHooks& h) {
            h.beforeDefaultStopForTest = [&] {
                if (!terminate || fake.ServiceCount("host:kill") > 0) ambiguousNext = true;
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        const auto expected = terminate ? okuflow::PhoneLinkAction::TerminateDefaultServer
                                        : okuflow::PhoneLinkAction::AskDefaultServerToExit;
        const auto reports = LinkCycles(40, [&](const auto& r) { return r.decision.action == expected; });
        QCOMPARE(reports.back().decision.action, expected);
        QCOMPARE(ambiguousLookups, 1);
        QCOMPARE(fake.ServiceCount("host:kill"), terminate ? 1 : 0);
        QVERIFY(!first.Exited());
        QVERIFY(!second.Exited());
        QCOMPARE(reports.back().actionResult, "kept: the listener table cannot be read");
    }
    void companionProblemTraceUsesOnlyFixedCodes_data() {
        QTest::addColumn<bool>("helloError");
        QTest::newRow("probe problem") << false;
        QTest::newRow("control hello error") << true;
    }
    void companionProblemTraceUsesOnlyFixedCodes() {
        QFETCH(bool, helloError);
        FakePhoneAdb fake;
        ScopedLinkHooks hooks(fake.port);
        const std::string raw = "FakeSerial42\nforged trace";
        fake.SetProblem(raw, helloError);
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("phone-link.log"));
        okuflow::SetPhoneLinkTraceFile(path.toStdWString());
        auto restore = qScopeGuard([] { okuflow::SetPhoneLinkTraceFile({}); });
        // The trace file and debug output share the same formatted line.
        const auto reports = LinkCycles(1);
        QCOMPARE(reports.back().observation.problem, raw);
        QVERIFY(okuflow::FlushPhoneLinkTrace(std::chrono::milliseconds(3000)));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto trace = file.readAll();
        QVERIFY(!trace.contains("FakeSerial42"));
        QVERIFY(!trace.contains("forged trace"));
        QVERIFY(trace.contains("companion=problem:unknown"));
    }
    void onlyALiveRealSessionKeepsAConfirmedHungDefault_data() {
        QTest::addColumn<bool>("decoding");
        QTest::newRow("registered without a frame: the hung default is stopped") << false;
        QTest::newRow("decoding frames: the stop is refused") << true;
    }
    void onlyALiveRealSessionKeepsAConfirmedHungDefault() {
        // After round 10 (2): the live-session rule had no test of its
        // wiring (the executors' rows use a hook). A real session on the
        // default-port fake: registered and silent (the companion's listener
        // is closed, the session retries its connect), or decoding H.264
        // frames. Then the default's listings fail until the hang is
        // confirmed and the ladder decides to terminate it. The default's
        // listener rows name a decoy adb.exe this test started, which the
        // termination reaches through its exact handle.
        QFETCH(bool, decoding);
        const auto units = H264AccessUnits(QFINDTESTDATA("data/phone-camera.h264"));
        QCOMPARE(units.size(), size_t{4});
        ScopedMediaFoundation media;
        QVERIFY(media.mf);
        DefaultAdbFixture decoy({});
        QVERIFY(decoy.copied && !decoy.Exited());
        const DWORD decoyPid = decoy.child->ProcessId();
        FakePhoneAdb fake(false, false, false, FakePhoneAdb::LaunchMode::None,
                          FakePhoneAdb::RecoveryMode::Healthy, false);
        fake.accessUnits = units;
        fake.streaming.store(decoding);
        fake.companionRefuses.store(!decoding);
        const auto port = fake.port;
        okuflow::SetTcpListenerRowsTransformForTest([port, decoyPid](std::vector<okuflow::TcpListenerRow>& rows) {
            for (auto& row : rows) if (row.port == port) row.processId = decoyPid;
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        ScopedLinkHooks hooks(fake.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              100, [decoding](okuflow::PhoneAdbTestHooks& h) {
            if (!decoding) h.sessionConnectBudgetForTest = std::chrono::seconds(30);
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        okuflow::PhoneCameraTransport transport;
        std::atomic<int> frames{0};
        auto session = std::async(std::launch::async, [&] {
            return transport.Start(FixturePhone(), nullptr, [&](okuflow::MediaFrame&&) { ++frames; }, {});
        });
        auto stopSession = qScopeGuard([&] { transport.Cancel(); session.wait(); transport.Stop(); });
        if (decoding) {
            QTRY_VERIFY_WITH_TIMEOUT(frames.load() >= 3, 4000);
        } else {
            // Registered, and retrying its connect to the closed listener.
            QTRY_VERIFY_WITH_TIMEOUT(okuflow::ReadPhoneSessionStateForTest().registered &&
                                     fake.ServiceCount(QStringLiteral("localabstract:okuflow_camera")) > 0, 3000);
        }
        QCOMPARE(okuflow::ReadPhoneSessionStateForTest().live, decoding);
        fake.listingMode.store(3); // Closed at once: a failed listing, without a hang's time-out.
        const auto started = std::chrono::steady_clock::now();
        const auto reports = LinkCycles(60, [&](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(10); });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY2(okuflow::ReadPhoneSessionStateForTest().registered, "The session ended before the stop was decided.");
        if (decoding) {
            QCOMPARE(QString::fromStdString(reports.back().actionResult),
                     QStringLiteral("kept: a live phone session uses the connection"));
            QVERIFY2(!decoy.child->WaitForExit(std::chrono::milliseconds(300)), "The server of a live session was stopped.");
            // The application reports the delivered frames: Connected.
            okuflow::PhoneLinkRequest streaming;
            streaming.sessionStreaming = true;
            const auto next = LinkCycles(1, {}, "abc", &streaming);
            QCOMPARE(int(next.back().decision.diagnosis), int(okuflow::PhoneLinkDiagnosis::Connected));
        } else {
            QVERIFY2(reports.back().actionResult != "kept: a live phone session uses the connection",
                     "A registered session without a frame kept the hung default.");
            QVERIFY2(decoy.child->WaitForExit(std::chrono::milliseconds(3000)), reports.back().actionResult.c_str());
        }
    }
    void aSessionThatRegistersRightAfterALiveOneIsNotLive() {
        // After round 10 (2): a decoded frame of a real session makes it
        // live (the frame time is written where a frame is decoded), and the
        // frame time is forgotten when the last session ends: a session that
        // registers within 2 s of the previous one's last frame is starting,
        // not live.
        const auto units = H264AccessUnits(QFINDTESTDATA("data/phone-camera.h264"));
        QCOMPARE(units.size(), size_t{4});
        ScopedMediaFoundation media;
        QVERIFY(media.mf);
        FakePhoneAdb fake;
        fake.accessUnits = units;
        fake.streaming.store(true);
        std::atomic<int> frames{0};
        {
            okuflow::PhoneCameraTransport first;
            QVERIFY(first.Start(FixturePhone(), nullptr, [&](okuflow::MediaFrame&&) { ++frames; }, {}));
            QTRY_VERIFY_WITH_TIMEOUT(frames.load() >= 3, 3000);
            QVERIFY(okuflow::ReadPhoneSessionStateForTest().live);
            first.Stop();
        }
        const auto ended = okuflow::ReadPhoneSessionStateForTest();
        QVERIFY(!ended.registered);
        QCOMPARE(ended.lastFrameMs, std::int64_t{-1});
        fake.streaming.store(false);
        fake.companionRefuses.store(true);
        okuflow::PhoneCameraTransport second;
        auto session = std::async(std::launch::async, [&] { return second.Start(FixturePhone(), nullptr, {}, {}); });
        auto stopSession = qScopeGuard([&] { second.Cancel(); session.wait(); second.Stop(); });
        QTRY_VERIFY_WITH_TIMEOUT(okuflow::ReadPhoneSessionStateForTest().registered, 3000);
        QVERIFY2(!okuflow::ReadPhoneSessionStateForTest().live, "A starting session read as live from the previous session's frames.");
    }
    void terminationCanceledWithoutAStopIsGivenBack_data() {
        QTest::addColumn<int>("change");
        QTest::newRow("transient live sessions reach the aborted stop cap") << 0;
        QTest::newRow("source changes before host kill is written") << 1;
        QTest::newRow("source changes in the host kill wait") << 2;
    }
    void terminationCanceledWithoutAStopIsGivenBack() {
        QFETCH(int, change);
        const bool stale = change != 0;
        DefaultAdbFixture decoy({});
        QVERIFY(decoy.copied && !decoy.Exited());
        const DWORD pid = decoy.child->ProcessId();
        FakePhoneAdb hung(false, false, false, FakePhoneAdb::LaunchMode::None,
                          FakePhoneAdb::RecoveryMode::Healthy, false);
        hung.listingMode.store(3);
        okuflow::SetTcpListenerRowsTransformForTest([&](std::vector<okuflow::TcpListenerRow>& rows) {
            for (auto& row : rows) if (row.port == hung.port) row.processId = pid;
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        constexpr int abortedCap = 3;
        std::atomic<std::uint64_t> generation{1};
        std::atomic<int> vetoes{0}, liveReads{0};
        if (change == 2) hung.SetOnHostKill([&] { generation.store(2); });
        ScopedLinkHooks hooks(hung.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(), {},
                              100, [&](okuflow::PhoneAdbTestHooks& h) {
            h.afterHostKillConnectForTest = [&] {
                ++vetoes;
                if (stale) {
                    if (change == 1 && vetoes.load() == 1) generation.store(2);
                } else {
                    // Live at the first-byte check, gone by the next read.
                    liveReads.store(1);
                }
            };
            h.phoneSessionLiveForTest = [&] { return liveReads.exchange(0) > 0; };
            h.linkPolicy->terminateCap = 1;
            h.linkPolicy->abortedStopCap = abortedCap;
            if (change == 2) h.linkPolicy->hostKillWaitMs = 1000;
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        okuflow::PhoneLinkRequest request;
        request.intentGeneration = 1;
        request.liveIntentGeneration = &generation;
        const auto first = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer; }, "abc", &request);
        QCOMPARE(first.back().decision.action, okuflow::PhoneLinkAction::TerminateDefaultServer);
        QCOMPARE(vetoes.load(), 1);
        if (stale) QCOMPARE(first.back().actionResult, "dropped: the source changed");
        QCOMPARE(hung.ServiceCount("host:kill"), change == 2 ? 1 : 0);
        QCOMPARE(liveReads.load(), 0);
        QVERIFY(!decoy.Exited());

        request.intentGeneration = generation.load();
        const auto started = std::chrono::steady_clock::now();
        const auto later = LinkCycles(60, [&](const auto& r) {
            return (stale && r.decision.action == okuflow::PhoneLinkAction::TerminateDefaultServer) ||
                r.decision.diagnosis == okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone ||
                vetoes.load() > abortedCap || decoy.Exited() ||
                std::chrono::steady_clock::now() - started > std::chrono::seconds(15); }, "abc", &request);
        if (stale) {
            QCOMPARE(later.back().decision.action, okuflow::PhoneLinkAction::TerminateDefaultServer);
            QVERIFY(decoy.child->WaitForExit(std::chrono::milliseconds(3000)));
            QCOMPARE(hung.ServiceCount("host:kill"), change == 2 ? 2 : 1);
        } else {
            QCOMPARE(later.back().decision.diagnosis, okuflow::PhoneLinkDiagnosis::OtherProgramHoldsPhone);
            QCOMPARE(vetoes.load(), abortedCap);
            QVERIFY(!decoy.Exited());
            QCOMPARE(hung.ServiceCount("host:kill"), 0);
            QCOMPARE(first.back().actionResult, "kept: a live phone session uses the connection");
        }
    }
    void offlineReconnectUsesTheObservedTransport_data() {
        QTest::addColumn<int>("change");
        QTest::newRow("ready at the fresh listing") << 0;
        QTest::newRow("endpoint changed before connect") << 1;
        QTest::newRow("listener changed after connect") << 2;
        QTest::newRow("unauthorized at the fresh listing") << 3;
        QTest::newRow("connecting at the fresh listing") << 4;
        QTest::newRow("ready after connecting the write socket") << 5;
        QTest::newRow("listener changed in the fresh listing") << 6;
    }
    void offlineReconnectUsesTheObservedTransport() {
        QFETCH(int, change);
        FakePhoneAdb first(false, false, false, FakePhoneAdb::LaunchMode::None, FakePhoneAdb::RecoveryMode::Healthy, false);
        FakePhoneAdb second(false, false, false, FakePhoneAdb::LaunchMode::None, FakePhoneAdb::RecoveryMode::Healthy, false);
        std::unique_ptr<DefaultAdbFixture> replacement;
        DWORD replacementPid = 0;
        if (change == 2 || change == 6) {
            replacement = std::make_unique<DefaultAdbFixture>(std::vector<std::wstring>{});
            QVERIFY(replacement->copied && !replacement->Exited());
            replacementPid = replacement->child->ProcessId();
        }
        std::atomic<bool> swapOwner{false}, listingArmed{false};
        if (change == 2 || change == 6) {
            const auto firstPort = first.port;
            okuflow::SetTcpListenerRowsTransformForTest([&, firstPort](std::vector<okuflow::TcpListenerRow>& rows) {
                if (!swapOwner.load()) return;
                for (auto& row : rows)
                    if (row.port == firstPort) row.processId = replacementPid;
            });
            first.SetBeforeListingReply([&] {
                if (listingArmed.load()) swapOwner.store(true);
            });
        }
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        first.listingState.store(6);
        second.listingState.store(6);
        ScopedLinkHooks hooks(first.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.linkStageForTest = [&](std::string_view stage) {
                if (stage != "decide") return;
                if (change == 0) first.listingState.store(8);
                if (change == 3) first.listingState.store(3);
                if (change == 4) first.listingState.store(1);
                if (change == 1) {
                    okuflow::PhoneAdbTestHooks replacementHooks{second.port, {}};
                    replacementHooks.linkPolicy = FastLinkPolicy();
                    okuflow::SetPhoneAdbTestHooks(replacementHooks);
                }
            };
            h.afterReconnectConnectForTest = [&] {
                if (change == 2) swapOwner.store(true);
                if (change == 5) first.listingState.store(8);
                if (change == 6) listingArmed.store(true);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        const auto reports = LinkCycles(1);
        QCOMPARE(reports.back().decision.action, okuflow::PhoneLinkAction::ReconnectTransport);
        if (change == 6) QVERIFY(swapOwner.load());
        QCOMPARE(first.ServiceCount("host-transport-id:9:reconnect"), 0);
        QCOMPARE(second.ServiceCount("host-transport-id:9:reconnect"), 0);
        QCOMPARE(reports.back().actionResult, "kept: the observed transport changed or recovered");
    }
    void unopenableDefaultReconnectRequiresATransportId_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("targeted offline reconnect is allowed") << 0;
        QTest::newRow("server-wide reconnect is refused") << 1;
        QTest::newRow("targeted stalled-device reconnect is allowed") << 2;
    }
    void unopenableDefaultReconnectRequiresATransportId() {
        QFETCH(int, mode);
        const bool stalled = mode == 2;
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            stalled ? FakePhoneAdb::RecoveryMode::Stalled
                                    : FakePhoneAdb::RecoveryMode::Healthy, false);
        if (!stalled) server.listingState.store(6);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present,
                              OwnedChildFixturePath().toStdWString(), {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.defaultListenerUnopenable = [] { return true; };
            if (mode == 1) h.transformAdbEntriesForTest = [](std::uint16_t, auto& entries) {
                for (auto& entry : entries) entry.transportId = 0;
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        if (stalled) {
            auto descriptor = FixturePhone();
            descriptor.phoneTransportId = server.transportId.load();
            QVERIFY(okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(descriptor));
        } else {
            const auto reports = LinkCycles(1);
            QCOMPARE(reports.back().observation.server, okuflow::PhoneServerKind::BorrowedDefault);
            QCOMPARE(reports.back().decision.action, okuflow::PhoneLinkAction::ReconnectTransport);
        }
        QCOMPARE(server.ServiceCount("host-transport-id:9:reconnect"), mode == 1 ? 0 : 1);
        QCOMPARE(server.ServiceCount("host:reconnect-offline"), 0);
    }
    void privateReconnectRequiresTheOwnedListener_data() {
        QTest::addColumn<bool>("afterConnect");
        QTest::newRow("foreign owner before connect") << false;
        QTest::newRow("foreign owner before fresh listing") << true;
    }
    void privateReconnectRequiresTheOwnedListener() {
        QFETCH(bool, afterConnect);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile offline(directory.filePath(QStringLiteral("offline")));
        QVERIFY(offline.open(QIODevice::WriteOnly));
        offline.close();
        std::optional<std::uint16_t> privatePort;
        bool foreignOwner = false;
        int privateListings = 0, connects = 0, foreignRows = 0;
        okuflow::SetTcpListenerRowsTransformForTest([&](std::vector<okuflow::TcpListenerRow>& rows) {
            if (!foreignOwner || !privatePort) return;
            for (auto& row : rows) {
                if (row.port != *privatePort || row.ipv6 || !row.loopback) continue;
                row.processId = GetCurrentProcessId();
                ++foreignRows;
            }
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        const auto defaultPort = PickTestPort();
        ScopedLinkHooks hooks(defaultPort, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString(),
                              {L"--fixture-device", L"--fixture-offline-file", offline.fileName().toStdWString()}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.transformAdbEntriesForTest = [&](std::uint16_t port, std::vector<okuflow::AdbDeviceEntry>& entries) {
                if (port == defaultPort || entries.empty()) return;
                ++privateListings;
                // Existing child fixture serves an offline phone. Present the
                // same reply without a transport ID to exercise the fallback.
                for (auto& entry : entries) entry.transportId = 0;
            };
            h.linkStageForTest = [&](std::string_view stage) {
                if (stage != "decide") return;
                privatePort = okuflow::OwnedAdbServerPort();
                if (!afterConnect) foreignOwner = true;
            };
            h.afterReconnectConnectForTest = [&] {
                ++connects;
                if (afterConnect) foreignOwner = true;
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        const auto reports = LinkCycles(1);
        QVERIFY(privatePort.has_value());
        QCOMPARE(okuflow::OwnedAdbServerPort(), privatePort); // Child is still alive.
        QCOMPARE(reports.back().observation.server, okuflow::PhoneServerKind::Owned);
        QCOMPARE(reports.back().observation.phoneState, okuflow::PhoneAdbState::Offline);
        QCOMPARE(reports.back().decision.action, okuflow::PhoneLinkAction::ReconnectTransport);
        QVERIFY(foreignRows > 0);
        QCOMPARE(connects, afterConnect ? 1 : 0);
        QCOMPARE(privateListings, 1); // Foreign ownership prevented a fresh listing or a reconnect write.
        QCOMPARE(reports.back().actionResult, "kept: the observed transport changed or recovered");
    }
    void stalledReconnectChecksTheAttachmentAfterConnect() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            FakePhoneAdb::RecoveryMode::SelectionStalled, false);
        int connects = 0;
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.afterReconnectConnectForTest = [&] {
                ++connects;
                // The formerly stalled phone re-registers as a ready device.
                server.transportId.store(10);
                server.deviceStalled.store(false);
            };
        });
        auto descriptor = FixturePhone();
        descriptor.phoneTransportId = 9;
        QVERIFY(!okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(descriptor));
        QCOMPARE(connects, 1);
        QCOMPARE(server.ServiceCount("host-transport-id:9:reconnect"), 0);
    }
    void transportReconnectIsNotWrittenAfterTheSourceChanged_data() {
        QTest::addColumn<bool>("offline");
        QTest::newRow("offline transport") << true;
        QTest::newRow("stalled device") << false;
    }
    void transportReconnectIsNotWrittenAfterTheSourceChanged() {
        // After round 10 (3): both reconnect paths checked the request's
        // authority, then connected (which can wait) and wrote the request
        // without asking again. The user changes the source between that
        // connect and the write: nothing is written, the result is dropped.
        QFETCH(bool, offline);
        auto live = std::make_shared<std::atomic<std::uint64_t>>(1);
        okuflow::PhoneLinkRequest request;
        request.intentGeneration = 1;
        request.liveIntentGeneration = live.get();
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::None,
                            offline ? FakePhoneAdb::RecoveryMode::Healthy : FakePhoneAdb::RecoveryMode::SelectionStalled, false);
        if (offline) server.listingState.store(6);
        auto connected = std::make_shared<std::atomic<int>>(0);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [live, connected](okuflow::PhoneAdbTestHooks& h) {
            h.afterReconnectConnectForTest = [live, connected] {
                if (++*connected == 1) live->store(2);
            };
        });
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::Resume);
        const auto reports = LinkCycles(12, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::ReconnectTransport; }, "abc", &request);
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::ReconnectTransport));
        QCOMPARE(int(reports.back().decision.diagnosis),
                 int(offline ? okuflow::PhoneLinkDiagnosis::Offline : okuflow::PhoneLinkDiagnosis::TransportStalled));
        QCOMPARE(connected->load(), 1);
        QCOMPARE(QString::fromStdString(reports.back().actionResult), QStringLiteral("dropped: the source changed"));
        QCOMPARE(server.reconnects.load(), 0);
        for (const auto& service : server.Services()) QVERIFY2(!service.contains("reconnect"), qPrintable(service));
        if (!offline) {
            auto descriptor = FixturePhone();
            descriptor.phoneTransportId = server.transportId.load();
            QVERIFY(okuflow::PhoneCameraTransport::RecoverStalledUsbTransport(descriptor, nullptr,
                                                                             [] { return true; }));
            QCOMPARE(server.reconnects.load(), 1);
        }
    }
    void refusedConnectWithOnlyAnIpv6RowIsNoListener() {
        // Iteration 9 (B3): an unrelated program listens on [::]:<port>
        // only; nothing listens on IPv4. The refused 127.0.0.1 connect was
        // read as a listener that does not accept (Unresponsive), which cost
        // the confirmation time before OkuFlow's own server started. Only an
        // IPv4 row stands for a listener that refused an IPv4 connect.
        const auto port = PickTestPort();
        const DWORD self = GetCurrentProcessId();
        okuflow::SetTcpListenerRowsTransformForTest([port, self](std::vector<okuflow::TcpListenerRow>& rows) {
            okuflow::TcpListenerRow row;
            row.ipv6 = true; row.anyAddress = true; row.port = port; row.processId = self;
            rows.push_back(row);
        });
        auto restore = qScopeGuard([] { okuflow::SetTcpListenerRowsTransformForTest({}); });
        ScopedLinkHooks hooks(port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::PhoneDiscoveryStatus status{};
        bool listed = false;
        okuflow::PhoneCameraTransport::Enumerate({}, nullptr, &listed, {}, &status, okuflow::PhoneServerIntent::PhoneUse);
        QVERIFY(status != okuflow::PhoneDiscoveryStatus::ServerUnresponsive);
        QVERIFY2(okuflow::OwnedAdbServerPort().has_value(), "OkuFlow's own server did not start at once.");
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
    void sessionLaunchUsesOneBoundedListingThenRetriesControl_data() {
        QTest::addColumn<bool>("listingFits");
        QTest::newRow("slow listing and activity cross the connection window") << true;
        QTest::newRow("listing exceeds the caller's remaining budget") << false;
    }
    void sessionLaunchUsesOneBoundedListingThenRetriesControl() {
        QFETCH(bool, listingFits);
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.accessUnits = H264AccessUnits(QFINDTESTDATA("data/phone-camera.h264"));
        QCOMPARE(server.accessUnits.size(), size_t{4});
        server.streaming.store(true);
        server.launchReplyDelayMs = listingFits ? 850 : 0;
        int listing = 0; // Only the fake server thread updates this.
        server.SetBeforeListingReply([&] {
            if (++listing == 2) std::this_thread::sleep_for(std::chrono::milliseconds(700));
        });
        ScopedMediaFoundation media;
        QVERIFY(media.com && media.mf);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [&](okuflow::PhoneAdbTestHooks& h) {
            h.sessionConnectBudgetForTest = std::chrono::milliseconds(listingFits ? 1200 : 400);
        });
        okuflow::PhoneCameraTransport transport;
        QCOMPARE(transport.Start(FixturePhone(), nullptr, [](okuflow::MediaFrame&&) {}, {}), listingFits);
        transport.Stop();
        QCOMPARE(server.hello.load(), listingFits);
        // Session listing and one verification. A failed start also lists
        // during the existing stalled-transport recovery, after the launch.
        QCOMPARE(server.ServiceCount("host:track-devices-proto-binary"), listingFits ? 2 : 3);
        QCOMPARE(server.ServiceCount("host:devices-l"), 0);
        QCOMPARE(server.Launches().size(), listingFits ? qsizetype{1} : qsizetype{0});
    }
    void sessionLaunchUsesTheSelectedUsbSerial() {
        FakePhoneAdb server(false, false, false, FakePhoneAdb::LaunchMode::Play,
                            FakePhoneAdb::RecoveryMode::Healthy, false);
        server.SetBeforeTransportReply([&] { server.replacementPhone.store(true); });
        ScopedMediaFoundation media;
        QVERIFY(media.com && media.mf);
        ScopedLinkHooks hooks(server.port, okuflow::PhoneAdbInterface::Present, {}, {}, 5000,
                              [](okuflow::PhoneAdbTestHooks& h) {
            h.sessionConnectBudgetForTest = std::chrono::milliseconds(500);
        });
        okuflow::PhoneCameraTransport transport;
        auto descriptor = FixturePhone();
        descriptor.phoneTransportId = server.transportId.load();
        QVERIFY(!transport.Start(descriptor, nullptr, {}, {}));
        transport.Stop();
        QVERIFY(server.replacementPhone.load());
        QCOMPARE(server.ServiceCount("host:transport:abc"), 0);
        QCOMPARE(server.ServiceCount("shell:input keyevent KEYCODE_WAKEUP"), 0);
        QVERIFY(server.Launches().isEmpty());
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
private:
    void recoveredStallIsHandedBack(bool crlf) {
        // Iteration 5 (1): the default lists the phone "device" and its
        // shell is silent until OkuFlow's own server runs beside it, then
        // answers. Before host:kill the shell is asked once more through
        // that listener: the stall is over, nothing is stopped, USB goes back.
        DefaultAdbFixture other({});
        other.child.reset();
        const QString stall = other.File(QStringLiteral("stall"));
        const QString kills = other.File(QStringLiteral("kills.log"));
        { QFile file(stall); QVERIFY(file.open(QIODevice::WriteOnly)); }
        other.flags = {L"--fixture-device", L"--fixture-exit-on-kill", L"--fixture-stalled-shell-file",
                       stall.toStdWString(), L"--fixture-kill-log", kills.toStdWString()};
        if (crlf) other.flags.push_back(L"--fixture-shell-crlf");
        QVERIFY(other.copied && other.Start());
        ScopedLinkHooks hooks(other.port, okuflow::PhoneAdbInterface::Present, OwnedChildFixturePath().toStdWString());
        okuflow::NotePhoneLinkEvent(okuflow::PhoneLinkEvent::AppStart);
        auto reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::StartShadowServer; });
        QVERIFY(Saw(reports, okuflow::PhoneLinkDiagnosis::TransportStalled));
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::StartShadowServer));
        QVERIFY(okuflow::OwnedAdbServerPort().has_value());
        QVERIFY(QFile::remove(stall)); // adbd answers on the default from now on.
        reports = LinkCycles(60, [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::HandBackToDefault; });
        QCOMPARE(int(reports.back().decision.action), int(okuflow::PhoneLinkAction::HandBackToDefault));
        // The memory was cleared by the re-check: the listing serves the phone.
        QVERIFY(reports.back().observation.defaultListsPhone && !reports.back().observation.defaultPhoneStalled);
        const auto asked = std::find_if(reports.begin(), reports.end(), [](const auto& r) {
            return r.decision.action == okuflow::PhoneLinkAction::AskDefaultServerToExit; });
        QVERIFY(asked != reports.end());
        QVERIFY2(asked->actionResult.find("answers on the default server") != std::string::npos, asked->actionResult.c_str());
        QVERIFY(!Saw(reports, okuflow::PhoneLinkAction::TerminateDefaultServer));
        QVERIFY(!okuflow::OwnedAdbServerPort().has_value());
        QTest::qWait(300);
        QVERIFY(!other.Exited());
        QVERIFY(!QFile::exists(kills)); // No host:kill was ever sent.
    }
};
QTEST_GUILESS_MAIN(PhoneCameraTests)
#include "phone_camera_tests.moc"
