#include "okuflow/capture/phone_camera.hpp"
#include <QtTest/QtTest>
#include <QFile>
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
    explicit FakePhoneAdb(bool old = false, bool stall = false, bool busy = false, LaunchMode mode = LaunchMode::None)
        : old(old), stall(stall), busy(busy), launchMode(mode) {
        std::promise<quint16> promise;
        auto future = promise.get_future();
        worker = std::thread([this, promise = std::move(promise)]() mutable {
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, 0)) { promise.set_value(0); return; }
            promise.set_value(server.serverPort());
            while (!stopped.load()) {
                if (!server.waitForNewConnection(20)) continue;
                std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
                try { Serve(*socket); } catch (...) {}
            }
        });
        port = future.get();
        if (!port) throw std::runtime_error("Cannot listen on test socket");
        okuflow::SetPhoneAdbTestHooks(okuflow::PhoneAdbTestHooks{port, {}});
    }
    ~FakePhoneAdb() {
        stopped.store(true);
        worker.join();
        okuflow::SetPhoneAdbTestHooks(std::nullopt);
    }
    quint16 port{};
    std::atomic<bool> hello{false};
    std::atomic<int> probes{0};
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
        if (service == "host:track-devices-proto-binary") {
            const char data[]{10, 11, 10, 3, 'a', 'b', 'c', 16, 8, 56, 1, 80, 9};
            QByteArray body(data, sizeof(data));
            Write(socket, "OKAY" + QByteArray::number(body.size(), 16).rightJustified(4, '0') + body);
            return;
        }
        if (!service.startsWith("host:transport")) return;
        Write(socket, "OKAY");
        const auto nextSize = Read(socket, 4).toInt(nullptr, 16);
        const auto next = Read(socket, nextSize);
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
        if (stall) {
            while (!stopped.load() && socket.state() == QAbstractSocket::ConnectedState) socket.waitForReadyRead(20);
            return;
        }
        const QByteArray reply = old ? QByteArray(R"({"protocol":1,"token":"old-token","cameras":[{"id":"0"}]})") :
            busy ? QByteArray(R"({"protocol":1,"probe":true,"busy":true,"problem":null,"cameras":[{"id":"0"}]})") :
                   QByteArray(R"({"protocol":1,"probe":true,"busy":false,"problem":null,"cameras":[{"id":"0"}]})");
        QByteArray packet(8, 0);
        qToLittleEndian<quint16>(1, packet.data());
        qToLittleEndian<quint32>(static_cast<quint32>(reply.size()), packet.data() + 4);
        Write(socket, packet + reply);
    }
    bool old, stall, busy;
    LaunchMode launchMode;
    bool launchReady{false};
    std::mutex launchMutex;
    QStringList launches;
    std::atomic<bool> stopped{false};
    std::thread worker;
};
}

class PhoneCameraTests : public QObject {
    Q_OBJECT
private slots:
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
