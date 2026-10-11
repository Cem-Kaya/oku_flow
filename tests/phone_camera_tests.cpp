#include "okuflow/capture/phone_camera.hpp"
#include <QtTest/QtTest>
#include <QFile>

class PhoneCameraTests : public QObject {
    Q_OBJECT
private slots:
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
};
QTEST_GUILESS_MAIN(PhoneCameraTests)
#include "phone_camera_tests.moc"
