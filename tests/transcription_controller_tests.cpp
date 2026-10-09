// Unit tests for TranscriptionSessionController with fake service/carrier.
// Covers negotiation, the reducer, audio gating/chunking, gap accounting,
// stale-generation rejection, and failure isolation. No Codex, no network.

#include <QSignalSpy>
#include <QtTest>

#include "okuflow/app/transcription_session_controller.hpp"

using namespace okuflow;

namespace {

AudioFrame MakeFrame(int samples, qint64 clock100ns)
{
    AudioFrame frame;
    frame.pcm.assign(static_cast<std::size_t>(samples) * 2, 0x11);
    frame.sampleRate = 48000;
    frame.channels = 1;
    frame.bitsPerSample = 16;
    frame.captureClock100ns = clock100ns;
    frame.duration100ns = static_cast<qint64>(samples) * 10000000LL / 48000;
    return frame;
}

} // namespace

class FakeService : public RealtimeTranscriptionService {
    Q_OBJECT
public:
    using RealtimeTranscriptionService::RealtimeTranscriptionService;

    void SetExecutablePath(const QString& path) override { executable = path; }
    bool IsSessionActive() const override { return active; }
    void BeginSession(quint64 generation) override
    {
        active = true;
        beginGenerations.push_back(generation);
    }
    void StartRealtime(quint64 generation, const QString& offerSdp) override
    {
        startGenerations.push_back(generation);
        offers.push_back(offerSdp);
    }
    void StopRealtime(quint64 generation) override { stopGenerations.push_back(generation); }
    void EndSession(quint64 generation) override
    {
        active = false;
        endGenerations.push_back(generation);
    }
    void RefreshRateLimits() override { ++rateLimitCalls; }

    // Signal helpers (signals are protected outside the class).
    void EmitSessionReady(quint64 g) { emit SessionReady(g); }
    void EmitAnswerSdp(quint64 g, const QString& s) { emit AnswerSdp(g, s); }
    void EmitRealtimeStarted(quint64 g) { emit RealtimeStarted(g); }
    void EmitDelta(quint64 g, const QString& d) { emit UserTranscriptDelta(g, d); }
    void EmitDone(quint64 g, const QString& t, bool trunc = false)
    {
        emit UserTranscriptDone(g, t, trunc);
    }
    void EmitClosed(quint64 g, const QString& r) { emit RealtimeClosed(g, r); }
    void EmitFailed(quint64 g, const QString& m) { emit SessionFailed(g, m); }

    QString executable;
    bool active{false};
    std::vector<quint64> beginGenerations;
    std::vector<quint64> startGenerations;
    std::vector<quint64> stopGenerations;
    std::vector<quint64> endGenerations;
    QStringList offers;
    int rateLimitCalls{0};
};

class FakeCarrier : public RealtimeAudioCarrier {
    Q_OBJECT
public:
    using RealtimeAudioCarrier::RealtimeAudioCarrier;

    void PrepareOffer(quint64 generation) override { prepareGenerations.push_back(generation); }
    void ApplyAnswer(quint64 generation, const QString& sdp) override
    {
        applyGenerations.push_back(generation);
        answers.push_back(sdp);
    }
    bool SendAudioChunks(quint64, const QByteArray& pcm, int sampleRate) override
    {
        if (rejectAudio) {
            return false;
        }
        sentBatches.push_back(pcm.size());
        sentBytes += pcm.size();
        lastSampleRate = sampleRate;
        return true;
    }
    void FinishAudioInput(quint64 generation) override
    {
        finishGenerations.push_back(generation);
        if (autoDrain) {
            emit AudioDrained(generation);
        }
    }
    void StopCarrier(quint64 generation) override { stopGenerations.push_back(generation); }

    void EmitOfferReady(quint64 g, const QString& s) { emit OfferReady(g, s); }
    void EmitAnswerApplied(quint64 g) { emit AnswerApplied(g); }
    void EmitChannelOpen(quint64 g) { emit ChannelOpen(g); }
    void EmitAudioDropped(quint64 g, qint64 samples) { emit AudioDropped(g, samples); }
    void EmitAudioDrained(quint64 g) { emit AudioDrained(g); }
    void EmitCarrierFailed(quint64 g, const QString& r) { emit CarrierFailed(g, r); }

    bool rejectAudio{false};
    bool autoDrain{true};
    std::vector<quint64> prepareGenerations;
    std::vector<quint64> applyGenerations;
    std::vector<quint64> stopGenerations;
    std::vector<quint64> finishGenerations;
    QStringList answers;
    std::vector<qsizetype> sentBatches;
    qsizetype sentBytes{0};
    int lastSampleRate{0};
};

class TranscriptionControllerTests : public QObject {
    Q_OBJECT

    RecordingSessionInfo TestSession(const QString& id = QStringLiteral("session-1"))
    {
        return RecordingSessionInfo{id, QStringLiteral("20260808_120000_000")};
    }

    // Drives the fake handshake to Listening and returns the generation.
    quint64 Negotiate(TranscriptionSessionController& controller,
                      FakeService& service, FakeCarrier& carrier)
    {
        controller.StartSession(TestSession(), QStringLiteral("en"));
        const quint64 generation = service.beginGenerations.back();
        service.EmitSessionReady(generation);
        carrier.EmitOfferReady(generation, QStringLiteral("v=0 offer"));
        service.EmitAnswerSdp(generation, QStringLiteral("v=0 answer"));
        carrier.EmitAnswerApplied(generation);
        carrier.EmitChannelOpen(generation);
        service.EmitRealtimeStarted(generation);
        return generation;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<TranscriptionState>();
        qRegisterMetaType<TranscriptSegment>();
    }

    void negotiationReachesListeningOnlyWhenComplete()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        QSignalSpy states(&controller, &TranscriptionSessionController::StateChanged);
        controller.SetEnabled(true);
        controller.StartSession(TestSession(), QStringLiteral("en"));
        QCOMPARE(service.beginGenerations.size(), std::size_t(1));
        const quint64 generation = service.beginGenerations.back();

        service.EmitSessionReady(generation);
        QCOMPARE(carrier.prepareGenerations.size(), std::size_t(1));
        carrier.EmitOfferReady(generation, QStringLiteral("v=0 offer"));
        QCOMPARE(service.offers, QStringList{QStringLiteral("v=0 offer")});
        service.EmitAnswerSdp(generation, QStringLiteral("v=0 answer"));
        QCOMPARE(carrier.answers, QStringList{QStringLiteral("v=0 answer")});

        // Two of three conditions are not Listening.
        carrier.EmitAnswerApplied(generation);
        carrier.EmitChannelOpen(generation);
        QCOMPARE(controller.State(), TranscriptionState::Starting);
        service.EmitRealtimeStarted(generation);
        QCOMPARE(controller.State(), TranscriptionState::Listening);
    }

    void staleGenerationEventsAreIgnored()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        controller.StartSession(TestSession(), QStringLiteral("en"));
        const quint64 generation = service.beginGenerations.back();
        service.EmitSessionReady(generation + 99);
        QCOMPARE(carrier.prepareGenerations.size(), std::size_t(0));
        carrier.EmitOfferReady(generation + 99, QStringLiteral("v=0"));
        QCOMPARE(service.startGenerations.size(), std::size_t(0));
        QCOMPARE(controller.State(), TranscriptionState::Starting);
    }

    void reducerAccumulatesPartialsAndFinalizesSegments()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy partials(&controller, &TranscriptionSessionController::PartialChanged);
        QSignalSpy segments(&controller, &TranscriptionSessionController::SegmentFinalized);
        const quint64 generation = Negotiate(controller, service, carrier);

        service.EmitDelta(generation, QStringLiteral("hello "));
        service.EmitDelta(generation, QStringLiteral("world"));
        QTRY_VERIFY(partials.count() >= 1);
        QCOMPARE(partials.at(partials.count() - 1).at(2).toString(),
                 QStringLiteral("hello world"));

        service.EmitDone(generation, QStringLiteral("hello world"));
        QCOMPARE(segments.count(), 1);
        const auto segment = segments.at(0).at(0).value<TranscriptSegment>();
        QCOMPARE(segment.recordingSessionId, QStringLiteral("session-1"));
        QCOMPARE(segment.sequence, quint64(1));
        QCOMPARE(segment.languageCode, QStringLiteral("en"));
        QCOMPARE(segment.text, QStringLiteral("hello world"));
        // Partial cleared after the final.
        QCOMPARE(partials.at(partials.count() - 1).at(2).toString(), QString());

        service.EmitDone(generation, QStringLiteral("second phrase"));
        QCOMPARE(segments.count(), 2);
        QCOMPARE(segments.at(1).at(0).value<TranscriptSegment>().sequence, quint64(2));
    }

    void immediateDuplicateFinalIsSuppressedButLaterRepetitionIsAllowed()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy segments(&controller, &TranscriptionSessionController::SegmentFinalized);
        const quint64 generation = Negotiate(controller, service, carrier);
        service.EmitDone(generation, QStringLiteral("repeat me"));
        service.EmitDone(generation, QStringLiteral(" repeat me "));
        QCOMPARE(segments.count(), 1);
        QTest::qWait(1050);
        service.EmitDone(generation, QStringLiteral("repeat me"));
        QCOMPARE(segments.count(), 2);
    }

    void audioIsChunkedTo960SampleBoundaries()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QVERIFY(!controller.TryEnqueueAudio(MakeFrame(960, 0)));  // not started
        const quint64 generation = Negotiate(controller, service, carrier);
        Q_UNUSED(generation);

        // Arbitrary Media Foundation boundaries: 1000 + 920 = 1920 samples
        // = exactly two 960-sample chunks.
        QVERIFY(controller.TryEnqueueAudio(MakeFrame(1000, 0)));
        QVERIFY(controller.TryEnqueueAudio(MakeFrame(920, 1000LL * 10000000LL / 48000)));
        QTRY_VERIFY(carrier.sentBytes >= 3840);
        QCOMPARE(carrier.sentBytes, qsizetype(3840));
        for (qsizetype batch : carrier.sentBatches) {
            QCOMPARE(batch % 1920, qsizetype(0));
        }
        QCOMPARE(carrier.lastSampleRate, 48000);

        // Wrong-format audio is refused.
        AudioFrame stereo = MakeFrame(960, 0);
        stereo.channels = 2;
        QVERIFY(!controller.TryEnqueueAudio(stereo));
    }

    void overflowRecordsOneGapAndClearsPartial()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy gaps(&controller, &TranscriptionSessionController::GapDetected);
        controller.StartSession(TestSession(), QStringLiteral("en"));
        const quint64 generation = service.beginGenerations.back();

        // Pre-roll only holds five seconds; the seventh second must evict.
        for (int second = 0; second < 7; ++second) {
            controller.TryEnqueueAudio(
                MakeFrame(48000, static_cast<qint64>(second) * 10000000LL));
        }
        QCOMPARE(gaps.count(), 0);  // published only once draining resumes

        service.EmitSessionReady(generation);
        carrier.EmitOfferReady(generation, QStringLiteral("v=0 offer"));
        service.EmitAnswerSdp(generation, QStringLiteral("v=0 answer"));
        carrier.EmitAnswerApplied(generation);
        carrier.EmitChannelOpen(generation);
        service.EmitRealtimeStarted(generation);
        QTRY_COMPARE(gaps.count(), 1);
        QVERIFY(gaps.at(0).at(1).toLongLong() >= 0);
    }

    void carrierRejectionBecomesGap()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy gaps(&controller, &TranscriptionSessionController::GapDetected);
        const quint64 generation = Negotiate(controller, service, carrier);
        Q_UNUSED(generation);
        carrier.rejectAudio = true;
        QVERIFY(controller.TryEnqueueAudio(MakeFrame(960, 0)));
        QTRY_COMPARE(gaps.count(), 1);
        QCOMPARE(carrier.sentBytes, qsizetype(0));
    }

    void downstreamCarrierOverflowBecomesGap()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy gaps(&controller, &TranscriptionSessionController::GapDetected);
        const quint64 generation = Negotiate(controller, service, carrier);
        carrier.EmitAudioDropped(generation, 960);
        QCOMPARE(gaps.count(), 1);
    }

    void finishDuringNegotiationDrainsCapturedPreroll()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        controller.StartSession(TestSession(), QStringLiteral("en"));
        const quint64 generation = service.beginGenerations.back();
        QVERIFY(controller.TryEnqueueAudio(MakeFrame(1920, 0)));
        controller.FinishInput();
        QCOMPARE(controller.State(), TranscriptionState::Starting);
        QVERIFY(!controller.TryEnqueueAudio(MakeFrame(960, 400000)));

        service.EmitSessionReady(generation);
        carrier.EmitOfferReady(generation, QStringLiteral("v=0 offer"));
        service.EmitAnswerSdp(generation, QStringLiteral("v=0 answer"));
        carrier.EmitAnswerApplied(generation);
        carrier.EmitChannelOpen(generation);
        service.EmitRealtimeStarted(generation);
        QCOMPARE(controller.State(), TranscriptionState::Finalizing);
        QTRY_COMPARE(carrier.sentBytes, qsizetype(3840));
        QTRY_VERIFY_WITH_TIMEOUT(!service.stopGenerations.empty(), 4000);
    }

    void offsetsRemainRelativeToFirstRecordingAudioAfterPrerollEviction()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy segments(&controller, &TranscriptionSessionController::SegmentFinalized);
        controller.StartSession(TestSession(), QStringLiteral("en"));
        const quint64 generation = service.beginGenerations.back();
        for (int second = 0; second < 7; ++second) {
            QVERIFY(controller.TryEnqueueAudio(
                MakeFrame(48000, static_cast<qint64>(second) * 10000000LL)));
        }
        service.EmitSessionReady(generation);
        carrier.EmitOfferReady(generation, QStringLiteral("v=0 offer"));
        service.EmitAnswerSdp(generation, QStringLiteral("v=0 answer"));
        carrier.EmitAnswerApplied(generation);
        carrier.EmitChannelOpen(generation);
        service.EmitRealtimeStarted(generation);
        QTRY_VERIFY_WITH_TIMEOUT(carrier.sentBytes >= 5 * 48000 * 2, 3000);
        service.EmitDone(generation, QStringLiteral("after pre-roll"));
        QCOMPARE(segments.count(), 1);
        const TranscriptSegment segment =
            segments.at(0).at(0).value<TranscriptSegment>();
        QVERIFY(segment.approximateOffset100ns >= 69000000LL);
    }

    void finishInputFinalizesWithBoundedGraceAndAcceptsLateFinal()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy segments(&controller, &TranscriptionSessionController::SegmentFinalized);
        QSignalSpy finished(&controller, &TranscriptionSessionController::SessionFinished);
        const quint64 generation = Negotiate(controller, service, carrier);

        controller.FinishInput();
        QCOMPARE(controller.State(), TranscriptionState::Finalizing);
        QVERIFY(!controller.TryEnqueueAudio(MakeFrame(960, 0)));
        // A final that arrives during the grace window is still durable.
        service.EmitDone(generation, QStringLiteral("last phrase"));
        QCOMPARE(segments.count(), 1);
        // The bounded grace elapses, StopRealtime goes out, closed completes.
        QTRY_VERIFY_WITH_TIMEOUT(!service.stopGenerations.empty(), 4000);
        service.EmitClosed(generation, QStringLiteral("requested"));
        QCOMPARE(controller.State(), TranscriptionState::Completed);
        // The authoritative server final cleared its pending delta, so close
        // cannot create a duplicate local tail segment.
        QCOMPARE(segments.count(), 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("session-1"));
    }

    void cleanClosePreservesUnfinalizedTailLocally()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy segments(&controller,
                           &TranscriptionSessionController::SegmentFinalized);
        const quint64 generation = Negotiate(controller, service, carrier);

        service.EmitDelta(generation, QStringLiteral("remaining "));
        service.EmitDelta(generation, QStringLiteral("lecture phrase"));
        controller.FinishInput();
        QTRY_VERIFY_WITH_TIMEOUT(!service.stopGenerations.empty(), 4000);
        service.EmitClosed(generation, QStringLiteral("requested"));

        QCOMPARE(controller.State(), TranscriptionState::Completed);
        QCOMPARE(segments.count(), 1);
        const TranscriptSegment segment =
            segments.at(0).at(0).value<TranscriptSegment>();
        QCOMPARE(segment.text, QStringLiteral("remaining lecture phrase"));
        QVERIFY(!segment.truncated);
    }

    void finishWaitsForCarrierWorkerDrain()
    {
        FakeService service;
        FakeCarrier carrier;
        carrier.autoDrain = false;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        const quint64 generation = Negotiate(controller, service, carrier);

        QVERIFY(controller.TryEnqueueAudio(MakeFrame(960, 0)));
        controller.FinishInput();
        QTRY_COMPARE(carrier.sentBytes, qsizetype(1920));
        QCOMPARE(carrier.finishGenerations,
                 std::vector<quint64>{generation});
        QTest::qWait(1800);  // longer than the transcript grace itself
        QVERIFY(service.stopGenerations.empty());

        carrier.EmitAudioDrained(generation + 1);  // stale drain ignored
        QTest::qWait(50);
        QVERIFY(service.stopGenerations.empty());
        carrier.EmitAudioDrained(generation);
        QTRY_VERIFY_WITH_TIMEOUT(!service.stopGenerations.empty(), 3000);
    }

    void serviceFailureEndsTranscriptOnly()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy states(&controller, &TranscriptionSessionController::StateChanged);
        QSignalSpy finished(&controller, &TranscriptionSessionController::SessionFinished);
        const quint64 generation = Negotiate(controller, service, carrier);
        service.EmitFailed(generation, QStringLiteral("account policy rejected"));
        QCOMPARE(controller.State(), TranscriptionState::Failed);
        QCOMPARE(finished.count(), 1);
        // Teardown reached both sides.
        QVERIFY(!service.endGenerations.empty());
        QVERIFY(!carrier.stopGenerations.empty());
        const auto lastState = states.at(states.count() - 1);
        QCOMPARE(lastState.at(1).toString(), QStringLiteral("account policy rejected"));
    }

    void transientFailureCreatesGapAndReconnectsSameRecording()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        QSignalSpy gaps(&controller, &TranscriptionSessionController::GapDetected);
        const quint64 firstGeneration = Negotiate(controller, service, carrier);
        service.EmitFailed(firstGeneration, QStringLiteral("network connection lost"));
        QCOMPARE(controller.State(), TranscriptionState::Starting);
        QCOMPARE(gaps.count(), 1);
        QVERIFY(controller.TryEnqueueAudio(MakeFrame(960, 10000000LL)));
        QTRY_COMPARE_WITH_TIMEOUT(service.beginGenerations.size(), std::size_t(2), 2500);
        const quint64 recoveredGeneration = service.beginGenerations.back();
        QVERIFY(recoveredGeneration != firstGeneration);
        QCOMPARE(controller.CurrentSessionId(), QStringLiteral("session-1"));
        service.EmitSessionReady(recoveredGeneration);
        carrier.EmitOfferReady(recoveredGeneration, QStringLiteral("v=0 offer"));
        service.EmitAnswerSdp(recoveredGeneration, QStringLiteral("v=0 answer"));
        carrier.EmitAnswerApplied(recoveredGeneration);
        carrier.EmitChannelOpen(recoveredGeneration);
        service.EmitRealtimeStarted(recoveredGeneration);
        QCOMPARE(controller.State(), TranscriptionState::Listening);
    }

    void finishForSessionMatchesOnlyCurrentRecording()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        Negotiate(controller, service, carrier);
        controller.FinishForSession(QStringLiteral("some-other-session"));
        QCOMPARE(controller.State(), TranscriptionState::Listening);
        controller.FinishForSession(QStringLiteral("session-1"));
        QCOMPARE(controller.State(), TranscriptionState::Finalizing);
    }

    void disabledControllerNeverStarts()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.StartSession(TestSession(), QStringLiteral("en"));
        QCOMPARE(service.beginGenerations.size(), std::size_t(0));
        QCOMPARE(controller.State(), TranscriptionState::Off);
    }

    void unavailableOverridesEnabled()
    {
        FakeService service;
        FakeCarrier carrier;
        TranscriptionSessionController controller(&service, &carrier);
        controller.SetEnabled(true);
        controller.SetUnavailable(QStringLiteral("WebRTC component unavailable."));
        controller.StartSession(TestSession(), QStringLiteral("en"));
        QCOMPARE(service.beginGenerations.size(), std::size_t(0));
        QCOMPARE(controller.State(), TranscriptionState::Unavailable);
    }
};

QTEST_MAIN(TranscriptionControllerTests)
#include "transcription_controller_tests.moc"
