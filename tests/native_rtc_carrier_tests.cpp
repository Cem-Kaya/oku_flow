// Loopback tests for RealtimeNativeRtcCarrier (plan 36 Carrier D). An
// in-process answering rtc::PeerConnection stands in for the realtime
// service: it accepts the carrier's offer, answers, receives the data
// channel, and counts RTP packets. Loopback only — no external network, no
// Codex, no microphone.
//
// If a hardened machine ever surfaces a firewall prompt for loopback UDP,
// set OKUFLOW_SKIP_NATIVE_RTC_TESTS=1 to skip visibly.

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <rtc/rtc.hpp>

#include "okuflow/common/realtime_native_rtc_carrier.hpp"

using namespace okuflow;

namespace {

// Answering peer: accepts an offer SDP, produces an answer, records opened
// channels/tracks and incoming RTP packets.
class AnsweringPeer {
public:
    AnsweringPeer()
    {
        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        peer = std::make_shared<rtc::PeerConnection>(configuration);
        peer->onDataChannel([this](std::shared_ptr<rtc::DataChannel> incoming) {
            incoming->onMessage([](rtc::message_variant) {});
            std::lock_guard lock(mutex);
            channel = std::move(incoming);
            channelSeen = true;
        });
        peer->onTrack([this](std::shared_ptr<rtc::Track> incoming) {
            std::lock_guard lock(mutex);
            track = std::move(incoming);
            track->onMessage([this](rtc::message_variant message) {
                if (!std::holds_alternative<rtc::binary>(message)) {
                    return;
                }
                const rtc::binary& packet = std::get<rtc::binary>(message);
                if (packet.size() < 12) {
                    return;
                }
                // The RTCP sender reports from RtcpSrReporter share this
                // stream; count only RTP v2 packets with the Opus payload.
                if ((std::to_integer<uint8_t>(packet[0]) >> 6) != 2 ||
                    (std::to_integer<uint8_t>(packet[1]) & 0x7F) != 111) {
                    return;
                }
                std::lock_guard inner(mutex);
                ++rtpPackets;
                payloadType = static_cast<int>(std::to_integer<uint8_t>(packet[1]) & 0x7F);
                uint32_t timestamp = 0;
                for (int i = 4; i < 8; ++i) {
                    timestamp = (timestamp << 8) |
                                std::to_integer<uint8_t>(packet[static_cast<size_t>(i)]);
                }
                timestamps.push_back(timestamp);
                uint32_t packetSsrc = 0;
                for (int i = 8; i < 12; ++i) {
                    packetSsrc = (packetSsrc << 8) |
                                 std::to_integer<uint8_t>(packet[static_cast<size_t>(i)]);
                }
                ssrcs.push_back(packetSsrc);
            });
        });
    }

    QString Answer(const QString& offerSdp)
    {
        peer->setRemoteDescription(
            rtc::Description(offerSdp.toStdString(), "offer"));
        peer->setLocalDescription();
        // Host-candidate gathering on loopback completes quickly.
        for (int i = 0; i < 200; ++i) {
            if (peer->gatheringState() ==
                rtc::PeerConnection::GatheringState::Complete) {
                break;
            }
            QTest::qWait(10);
        }
        auto description = peer->localDescription();
        if (!description.has_value()) {
            return {};
        }
        return QString::fromStdString(std::string(description.value()));
    }

    int RtpPacketCount()
    {
        std::lock_guard lock(mutex);
        return rtpPackets;
    }

    int PayloadType()
    {
        std::lock_guard lock(mutex);
        return payloadType;
    }

    bool TimestampsAdvanceBy(uint32_t samples)
    {
        std::lock_guard lock(mutex);
        if (timestamps.size() < 2) {
            return false;
        }
        // UDP loopback may reorder; the timestamp sequence must still be a
        // contiguous 960-sample grid once sorted.
        std::vector<uint32_t> sorted = timestamps;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i] - sorted[i - 1] != samples) {
                return false;
            }
        }
        return true;
    }

    bool SsrcConstant()
    {
        std::lock_guard lock(mutex);
        if (ssrcs.empty()) {
            return false;
        }
        for (uint32_t value : ssrcs) {
            if (value != ssrcs.front()) {
                return false;
            }
        }
        return true;
    }

    bool ChannelSeen()
    {
        std::lock_guard lock(mutex);
        return channelSeen;
    }

    std::shared_ptr<rtc::PeerConnection> peer;

private:
    std::mutex mutex;
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::Track> track;
    bool channelSeen{false};
    int rtpPackets{0};
    int payloadType{-1};
    std::vector<uint32_t> timestamps;
    std::vector<uint32_t> ssrcs;
};

QByteArray MakePcm(int frames)
{
    // Alternating ramp so Opus has real signal to encode.
    QByteArray pcm;
    pcm.resize(frames * 960 * 2);
    auto* samples = reinterpret_cast<qint16*>(pcm.data());
    const int count = frames * 960;
    for (int i = 0; i < count; ++i) {
        samples[i] = static_cast<qint16>((i % 200 - 100) * 120);
    }
    return pcm;
}

} // namespace

class NativeRtcCarrierTests : public QObject {
    Q_OBJECT

    bool SkipRequested()
    {
        return qEnvironmentVariableIntValue("OKUFLOW_SKIP_NATIVE_RTC_TESTS") == 1;
    }

    // Drives one carrier through offer/answer against a loopback peer.
    bool Negotiate(RealtimeNativeRtcCarrier& carrier, AnsweringPeer& peer,
                   quint64 generation, QSignalSpy& offers, QSignalSpy& applied,
                   QSignalSpy& channelOpen)
    {
        carrier.PrepareOffer(generation);
        if (!QTest::qWaitFor([&]() { return offers.count() >= 1; }, 10000)) {
            return false;
        }
        const QString offerSdp = offers.last().at(1).toString();
        if (!offerSdp.contains(QStringLiteral("opus/48000/2")) ||
            !offerSdp.contains(QStringLiteral("sendrecv"))) {
            return false;
        }
        const QString answer = peer.Answer(offerSdp);
        if (answer.isEmpty()) {
            return false;
        }
        carrier.ApplyAnswer(generation, answer);
        if (!QTest::qWaitFor([&]() { return applied.count() >= 1; }, 10000)) {
            return false;
        }
        return QTest::qWaitFor([&]() { return channelOpen.count() >= 1; }, 10000);
    }

private slots:
    void offerAnswerChannelAndRtpFlow()
    {
        if (SkipRequested()) {
            QSKIP("OKUFLOW_SKIP_NATIVE_RTC_TESTS=1");
        }
        RealtimeNativeRtcCarrier carrier;
        AnsweringPeer peer;
        QSignalSpy offers(&carrier, &RealtimeNativeRtcCarrier::OfferReady);
        QSignalSpy applied(&carrier, &RealtimeNativeRtcCarrier::AnswerApplied);
        QSignalSpy channelOpen(&carrier, &RealtimeNativeRtcCarrier::ChannelOpen);
        QSignalSpy drained(&carrier, &RealtimeNativeRtcCarrier::AudioDrained);
        QSignalSpy failed(&carrier, &RealtimeNativeRtcCarrier::CarrierFailed);

        QVERIFY(Negotiate(carrier, peer, 5, offers, applied, channelOpen));
        // The offer also carries the server-expected data channel.
        QVERIFY(peer.ChannelSeen());

        // Two seconds of audio in controller-shaped batches (<= 5 frames).
        const QByteArray batch = MakePcm(5);
        for (int i = 0; i < 20; ++i) {
            QVERIFY(carrier.SendAudioChunks(5, batch, 48000));
            QTest::qWait(5);
        }
        QTRY_VERIFY_WITH_TIMEOUT(peer.RtpPacketCount() >= 95, 10000);
        QCOMPARE(peer.PayloadType(), 111);
        QVERIFY(peer.TimestampsAdvanceBy(960));
        QVERIFY(peer.SsrcConstant());
        QCOMPARE(failed.count(), 0);

        carrier.FinishAudioInput(5);
        QTRY_COMPARE_WITH_TIMEOUT(drained.count(), 1, 3000);
        // Input is sealed as soon as finalization begins.
        QVERIFY(!carrier.SendAudioChunks(5, batch, 48000));

        carrier.StopCarrier(5);
        // After stop, audio is refused.
        QVERIFY(!carrier.SendAudioChunks(5, batch, 48000));
    }

    void rejectsWrongFormatAndStaleGeneration()
    {
        if (SkipRequested()) {
            QSKIP("OKUFLOW_SKIP_NATIVE_RTC_TESTS=1");
        }
        RealtimeNativeRtcCarrier carrier;
        AnsweringPeer peer;
        QSignalSpy offers(&carrier, &RealtimeNativeRtcCarrier::OfferReady);
        QSignalSpy applied(&carrier, &RealtimeNativeRtcCarrier::AnswerApplied);
        QSignalSpy channelOpen(&carrier, &RealtimeNativeRtcCarrier::ChannelOpen);
        QVERIFY(Negotiate(carrier, peer, 9, offers, applied, channelOpen));

        const QByteArray good = MakePcm(1);
        QVERIFY(carrier.SendAudioChunks(9, good, 48000));
        QVERIFY(!carrier.SendAudioChunks(8, good, 48000));   // stale generation
        QVERIFY(!carrier.SendAudioChunks(9, good, 24000));   // wrong rate
        QByteArray ragged = good;
        ragged.chop(2);                                       // not 20 ms aligned
        QVERIFY(!carrier.SendAudioChunks(9, ragged, 48000));
        carrier.StopCarrier(8);                               // stale stop ignored
        QVERIFY(carrier.SendAudioChunks(9, good, 48000));
        // A single hand-off larger than the five-second carrier budget is
        // rejected atomically rather than creating an unbounded pacer queue.
        QVERIFY(!carrier.SendAudioChunks(9, MakePcm(251), 48000));
        carrier.StopCarrier(9);
    }

    void configurableBitrateAndFrameDuration()
    {
        if (SkipRequested()) {
            QSKIP("OKUFLOW_SKIP_NATIVE_RTC_TESTS=1");
        }
        const RealtimeNativeRtcProfile profile{64000, 40};
        QVERIFY(RealtimeNativeRtcCarrier::IsProfileSupported(profile));
        RealtimeNativeRtcCarrier carrier(profile);
        AnsweringPeer peer;
        QSignalSpy offers(&carrier, &RealtimeNativeRtcCarrier::OfferReady);
        QSignalSpy applied(&carrier, &RealtimeNativeRtcCarrier::AnswerApplied);
        QSignalSpy channelOpen(&carrier,
                               &RealtimeNativeRtcCarrier::ChannelOpen);
        QSignalSpy drained(&carrier, &RealtimeNativeRtcCarrier::AudioDrained);
        QSignalSpy failed(&carrier, &RealtimeNativeRtcCarrier::CarrierFailed);

        carrier.PrepareOffer(11);
        QTRY_COMPARE_WITH_TIMEOUT(offers.count(), 1, 10000);
        const QString offerSdp = offers.at(0).at(1).toString();
        QVERIFY(offerSdp.contains(QStringLiteral("a=ptime:40")));
        const QString answer = peer.Answer(offerSdp);
        QVERIFY(!answer.isEmpty());
        carrier.ApplyAnswer(11, answer);
        QTRY_COMPARE_WITH_TIMEOUT(applied.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(channelOpen.count(), 1, 10000);
        // Five controller chunks become 40 + 40 + 20 ms Opus packets; the
        // shorter final packet is legal and avoids dropping or padding speech.
        QVERIFY(carrier.SendAudioChunks(11, MakePcm(5), 48000));
        carrier.FinishAudioInput(11);
        QTRY_COMPARE_WITH_TIMEOUT(drained.count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(peer.RtpPacketCount() >= 3, 2000);
        QVERIFY(peer.TimestampsAdvanceBy(1920));
        QCOMPARE(failed.count(), 0);
        carrier.StopCarrier(11);
    }

    void startupBufferPreventsCaptureTimerStarvation()
    {
        if (SkipRequested()) {
            QSKIP("OKUFLOW_SKIP_NATIVE_RTC_TESTS=1");
        }
        RealtimeNativeRtcCarrier carrier;
        AnsweringPeer peer;
        QSignalSpy offers(&carrier, &RealtimeNativeRtcCarrier::OfferReady);
        QSignalSpy applied(&carrier, &RealtimeNativeRtcCarrier::AnswerApplied);
        QSignalSpy channelOpen(&carrier,
                               &RealtimeNativeRtcCarrier::ChannelOpen);
        QSignalSpy drained(&carrier, &RealtimeNativeRtcCarrier::AudioDrained);
        QSignalSpy failed(&carrier, &RealtimeNativeRtcCarrier::CarrierFailed);
        QVERIFY(Negotiate(carrier, peer, 13, offers, applied, channelOpen));

        // A controller with no negotiation pre-roll may initially hand over
        // capture-timer-sized bursts. Four hundred milliseconds must remain
        // queued instead of starting a sender that a 50 ms producer can
        // repeatedly starve. Reaching the 500 ms cushion starts pacing.
        QVERIFY(carrier.SendAudioChunks(13, MakePcm(20), 48000));
        QTest::qWait(200);
        QCOMPARE(peer.RtpPacketCount(), 0);
        QVERIFY(carrier.SendAudioChunks(13, MakePcm(5), 48000));
        QTRY_VERIFY_WITH_TIMEOUT(peer.RtpPacketCount() >= 25, 2000);

        // Let the next media deadline pass with no producer frame. A later
        // 400 ms burst must not restart a one-packet starvation loop; the
        // carrier rebuilds the same 500 ms continuity cushion.
        QTest::qWait(100);
        const int beforeRestart = peer.RtpPacketCount();
        QVERIFY(carrier.SendAudioChunks(13, MakePcm(20), 48000));
        QTest::qWait(200);
        QCOMPARE(peer.RtpPacketCount(), beforeRestart);
        QVERIFY(carrier.SendAudioChunks(13, MakePcm(5), 48000));
        QTRY_VERIFY_WITH_TIMEOUT(peer.RtpPacketCount() >= beforeRestart + 5,
                                 2000);

        carrier.FinishAudioInput(13);
        QTRY_COMPARE_WITH_TIMEOUT(drained.count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(peer.RtpPacketCount() >= beforeRestart + 25,
                                 2000);
        QCOMPARE(failed.count(), 0);
        carrier.StopCarrier(13);
    }

    void rejectsUnsupportedPacketIntervals()
    {
        QVERIFY(RealtimeNativeRtcCarrier::IsProfileSupported(
            RealtimeNativeRtcProfile{32000, 20}));
        QVERIFY(RealtimeNativeRtcCarrier::IsProfileSupported(
            RealtimeNativeRtcProfile{48000, 40}));
        QVERIFY(RealtimeNativeRtcCarrier::IsProfileSupported(
            RealtimeNativeRtcProfile{64000, 60}));
        // opus_encode accepts one 2.5/5/10/20/40/60 ms frame. The requested
        // 4x/8x/16x values would require packet aggregation beyond this
        // carrier contract and do not increase ASR context.
        QVERIFY(!RealtimeNativeRtcCarrier::IsProfileSupported(
            RealtimeNativeRtcProfile{64000, 80}));
        QVERIFY(!RealtimeNativeRtcCarrier::IsProfileSupported(
            RealtimeNativeRtcProfile{64000, 160}));
        QVERIFY(!RealtimeNativeRtcCarrier::IsProfileSupported(
            RealtimeNativeRtcProfile{64000, 320}));
    }

    void pacedDrainStaysRealtimeWithoutBursting()
    {
        if (SkipRequested()) {
            QSKIP("OKUFLOW_SKIP_NATIVE_RTC_TESTS=1");
        }
        RealtimeNativeRtcCarrier carrier;
        AnsweringPeer peer;
        QSignalSpy offers(&carrier, &RealtimeNativeRtcCarrier::OfferReady);
        QSignalSpy applied(&carrier, &RealtimeNativeRtcCarrier::AnswerApplied);
        QSignalSpy channelOpen(&carrier, &RealtimeNativeRtcCarrier::ChannelOpen);
        QSignalSpy drained(&carrier, &RealtimeNativeRtcCarrier::AudioDrained);
        QSignalSpy failed(&carrier, &RealtimeNativeRtcCarrier::CarrierFailed);
        QVERIFY(Negotiate(carrier, peer, 12, offers, applied, channelOpen));

        // Queue four seconds immediately. The sole worker must neither burst
        // it nor add encode/send overhead to every 20 ms pacing interval.
        const QByteArray batch = MakePcm(5);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 40; ++i) {
            QVERIFY(carrier.SendAudioChunks(12, batch, 48000));
        }
        carrier.FinishAudioInput(12);
        QTRY_COMPARE_WITH_TIMEOUT(drained.count(), 1, 7000);

        const qint64 elapsedMs = timer.elapsed();
        QVERIFY2(elapsedMs >= 3600,
                 qPrintable(QStringLiteral("RTP burst in %1 ms").arg(elapsedMs)));
        QVERIFY2(elapsedMs <= 4400,
                 qPrintable(QStringLiteral("RTP pacing drifted to %1 ms")
                                .arg(elapsedMs)));
        QTRY_VERIFY_WITH_TIMEOUT(peer.RtpPacketCount() >= 195, 2000);
        QCOMPARE(failed.count(), 0);
        carrier.StopCarrier(12);
    }

    void invalidAnswerFailsCleanly()
    {
        if (SkipRequested()) {
            QSKIP("OKUFLOW_SKIP_NATIVE_RTC_TESTS=1");
        }
        RealtimeNativeRtcCarrier carrier;
        QSignalSpy offers(&carrier, &RealtimeNativeRtcCarrier::OfferReady);
        QSignalSpy failed(&carrier, &RealtimeNativeRtcCarrier::CarrierFailed);
        carrier.PrepareOffer(3);
        QTRY_COMPARE_WITH_TIMEOUT(offers.count(), 1, 10000);
        carrier.ApplyAnswer(3, QStringLiteral("this is not sdp"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("WebRTC")));
    }

    void repeatedCyclesStayClean()
    {
        if (SkipRequested()) {
            QSKIP("OKUFLOW_SKIP_NATIVE_RTC_TESTS=1");
        }
        RealtimeNativeRtcCarrier carrier;
        QSignalSpy offers(&carrier, &RealtimeNativeRtcCarrier::OfferReady);
        QSignalSpy failed(&carrier, &RealtimeNativeRtcCarrier::CarrierFailed);
        // Twenty offer/stop cycles: no failures, no stuck state, a fresh
        // offer each round.
        for (quint64 generation = 100; generation < 120; ++generation) {
            const int before = offers.count();
            carrier.PrepareOffer(generation);
            QVERIFY(QTest::qWaitFor(
                [&]() { return offers.count() == before + 1; }, 10000));
            QCOMPARE(offers.last().at(0).toULongLong(), generation);
            carrier.StopCarrier(generation);
        }
        QCOMPARE(failed.count(), 0);
    }
};

QTEST_MAIN(NativeRtcCarrierTests)
#include "native_rtc_carrier_tests.moc"
