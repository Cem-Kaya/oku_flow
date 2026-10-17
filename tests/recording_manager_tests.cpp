// This target is Windows-only in CMake. Keep its QObject visible to moc,
// which does not inherit the compiler's implicit _WIN32 predefined macro.
#include "okuflow/app/recording_manager.hpp"

#include <QTemporaryDir>
#include <QFileInfo>
#include <QtTest>
#include <limits>
#include <chrono>

namespace okuflow {
struct RecordingManagerTestAccess {
    static void PrimeRetirement(RecordingManager& manager) {
        // Isolate the real retirement code from codecs and the idle worker.
        // The fixture subsequently drives FinishSession on a controlled thread.
        manager.ShutdownForProcessExit();
        manager.shutdownRequested_ = false;
        manager.SetState(RecordingState::Stopping);
        manager.sessionHadOutput_ = true;
        manager.segmentFinalized_ = true;
        manager.frameQueue_.emplace_back();
        manager.audioQueue_.emplace_back();
        manager.stopRequested_ = true;
        manager.stopMessage_ = QStringLiteral("session-a-stop");
        manager.workerWriting_ = true;
    }
    static void Observe(RecordingManager& manager,
                        std::function<void(RecordingState)> observer) {
        manager.statePublicationObserver_ = std::move(observer);
    }
    static bool Retired(RecordingManager& manager) {
        std::lock_guard lock(manager.queueMutex_);
        return manager.frameQueue_.empty() && manager.audioQueue_.empty()
            && !manager.stopRequested_ && !manager.workerWriting_
            && manager.stopMessage_.isEmpty();
    }
    static void Finish(RecordingManager& manager, bool success) {
        manager.FinishSession(success, success ? QString() : QStringLiteral("fixture failure"));
    }
    static void SeedReplacement(RecordingManager& manager) {
        std::lock_guard lock(manager.queueMutex_);
        manager.frameQueue_.emplace_back();
        manager.audioQueue_.emplace_back();
        manager.stopRequested_ = true;
        manager.stopMessage_ = QStringLiteral("session-b-stop");
    }
    static bool ReplacementIntact(RecordingManager& manager, const QString& id) {
        std::lock_guard lock(manager.queueMutex_);
        return manager.sessionInfo_.id == id
            && manager.frameQueue_.size() == 1 && manager.audioQueue_.size() == 1
            && manager.stopRequested_
            && manager.stopMessage_ == QStringLiteral("session-b-stop");
    }
};
} // namespace okuflow

namespace {

// Deliberately separate input, encoder output and sink delivery, matching the
// documented GetStatistics contract. No COM encoder or camera is opened.
struct FakeSinkWriterStatistics {
    MF_SINK_WRITER_STATISTICS value{};
    bool unavailable{};
    int queries{};

    const MF_SINK_WRITER_STATISTICS* Query() {
        ++queries;
        return unavailable ? nullptr : &value;
    }
    void Submit() { ++value.qwNumSamplesReceived; }
    void Encode(std::uint64_t count) { value.qwNumSamplesEncoded = count; }
};

} // namespace

class RecordingManagerTests : public QObject {
    Q_OBJECT
private slots:
    void terminalPublicationRetiresOldSession_data()
    {
        QTest::addColumn<bool>("success");
        QTest::newRow("normal-stop") << true;
        QTest::newRow("video-error") << false;
        QTest::newRow("audio-error") << false;
    }
    void terminalPublicationRetiresOldSession()
    {
        QFETCH(bool, success);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        okuflow::UserDataPaths paths(directory.path(), QStringLiteral("C:/fixture-install"));
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        okuflow::RecordingSessionInfo first;
        first.id = QStringLiteral("session-a");
        manager.SetRequested(true, first);
        QVERIFY(manager.CurrentSessionInfo());
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeRetirement(manager);
        std::mutex gateMutex;
        std::condition_variable gate;
        bool published = false, resume = false, retiredAtPublication = false;
        Access::Observe(manager, [&](okuflow::RecordingState state) {
            if (state != okuflow::RecordingState::Completed
                && state != okuflow::RecordingState::Failed) return;
            const bool retired = Access::Retired(manager);
            std::unique_lock lock(gateMutex);
            retiredAtPublication = retired;
            published = true;
            gate.notify_all();
            gate.wait_for(lock, std::chrono::seconds(5), [&] { return resume; });
        });
        std::thread finishing([&] { Access::Finish(manager, success); });
        bool reached = false;
        {
            std::unique_lock lock(gateMutex);
            reached = gate.wait_for(lock, std::chrono::seconds(3), [&] { return published; });
        }
        okuflow::RecordingSessionInfo second;
        second.id = QStringLiteral("session-b");
        bool replacementAdmitted = false;
        if (reached) {
            manager.SetRequested(true, second);
            const auto session = manager.CurrentSessionInfo();
            replacementAdmitted = session && session->id == second.id;
            Access::SeedReplacement(manager);
        }
        {
            std::lock_guard lock(gateMutex);
            resume = true;
        }
        gate.notify_all();
        finishing.join();
        // Assert after joining so a failed assertion cannot strand a worker.
        QVERIFY(reached);
        QVERIFY(retiredAtPublication);
        QVERIFY(replacementAdmitted);
        QVERIFY(Access::ReplacementIntact(manager, second.id));
    }
    void exactSessionStopCannotTouchReplacement()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        okuflow::UserDataPaths paths(directory.path(),QStringLiteral("C:/fixture-install"));
        okuflow::RecordingManager manager(nullptr,[](const QString&,int) {},[](const auto&) {},paths);
        okuflow::RecordingSessionInfo first; first.id = QStringLiteral("reserved-session-a");
        first.ownerKind = okuflow::RecordingOwnerKind::AgentSession; first.ownerId = QStringLiteral("owner-a");
        first.requestedAudio = okuflow::RecordingAudioRequest::None;
        manager.SetRequested(true,first);
        QVERIFY(manager.CurrentSessionInfo()); QCOMPARE(manager.CurrentSessionInfo()->id,first.id);
        QCOMPARE(manager.CurrentSessionInfo()->ownerId,first.ownerId);
        QVERIFY(!manager.StopSession(QStringLiteral("foreign-session"))); QVERIFY(manager.IsActive());
        manager.SetSessionAudioResult(QStringLiteral("foreign-session"),true);
        QVERIFY(!manager.CurrentSessionInfo()->actualAudio);
        QVERIFY(manager.StopSession(first.id));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.CurrentSessionInfo().has_value(),3000);
        okuflow::RecordingSessionInfo second; second.id = QStringLiteral("reserved-session-b");
        manager.SetRequested(true,second); QVERIFY(manager.CurrentSessionInfo());
        QVERIFY(!manager.StopSession(first.id)); QCOMPARE(manager.CurrentSessionInfo()->id,second.id);
        QVERIFY(manager.StopSession(second.id)); QVERIFY(manager.ShutdownForProcessExit());
    }
    void startupBudgetRequiresPendingAndNextFrameToFit()
    {
        constexpr std::uint64_t oneFrameLimit =
            okuflow::RecordingEncoderBacklog::kVideoByteLimit / 2;
        QVERIFY(okuflow::RecordingEncoderBacklog::CanStart(oneFrameLimit));
        QVERIFY(!okuflow::RecordingEncoderBacklog::CanStart(oneFrameLimit + 1));
        QVERIFY(okuflow::VideoRecorder::InputFitsEncoderBudget(4096, 4096, false));
        QVERIFY(!okuflow::VideoRecorder::InputFitsEncoderBudget(7680, 4320, false));
        QVERIFY(okuflow::VideoRecorder::InputFitsEncoderBudget(7680, 4320, true));
        QVERIFY(!okuflow::VideoRecorder::InputFitsEncoderBudget(8192, 8192, true));
        const UINT maximumDimension = std::numeric_limits<UINT>::max();
        QVERIFY(!okuflow::VideoRecorder::InputFitsEncoderBudget(
            maximumDimension, maximumDimension, false));
        QVERIFY(!okuflow::VideoRecorder::InputFitsEncoderBudget(
            maximumDimension, maximumDimension, true));
        QVERIFY(!okuflow::VideoRecorder::InputFitsEncoderBudget(0, 4096, false));

        FakeSinkWriterStatistics sink;
        okuflow::RecordingEncoderBacklog exactBoundary;
        QVERIFY(exactBoundary.CanAccept(sink.Query(), 0, 0, oneFrameLimit));
        QVERIFY(exactBoundary.CanAccept(sink.Query(), 0, 1, oneFrameLimit));
        okuflow::RecordingEncoderBacklog exceedsBoundary;
        QVERIFY(!exceedsBoundary.CanAccept(sink.Query(), 0, 0, oneFrameLimit + 1));
        QVERIFY(!exceedsBoundary.CanAccept(sink.Query(), 0, 1, oneFrameLimit + 1));
    }

    void rejectedStartupDoesNotCreateOutputFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cpuPath = directory.filePath(QStringLiteral("oversized-cpu.mp4"));
        const QString gpuPath = directory.filePath(QStringLiteral("oversized-gpu.mp4"));
        // No MFStartup, codec, GPU device, actual image buffer or hardware is
        // needed: validation precedes native sink/probe creation.
        okuflow::VideoRecorder cpu;
        QVERIFY(!cpu.Start(cpuPath.toStdWString(), 7680, 4320, 30, 1,
                           okuflow::VideoRecorder::Codec::H264));
        QVERIFY(!cpu.IsRecording());
        QVERIFY(QString::fromStdString(cpu.LastError()).contains(QStringLiteral("64 MiB")));
        QVERIFY(!QFileInfo::exists(cpuPath)); // not even an empty output
        const auto stopped = cpu.Stop();
        QCOMPARE(stopped.videoSamplesWritten, 0ULL);
        QVERIFY(!QFileInfo::exists(cpuPath));

        okuflow::VideoRecorder gpu;
        QVERIFY(!gpu.StartGpu(gpuPath.toStdWString(), 8192, 8192, 30, 1,
                              okuflow::VideoRecorder::Codec::Av1, {}));
        QVERIFY(!gpu.IsRecording());
        QVERIFY(QString::fromStdString(gpu.LastError()).contains(QStringLiteral("64 MiB")));
        QVERIFY(!QFileInfo::exists(gpuPath));
        QCOMPARE(gpu.Stop().videoSamplesWritten, 0ULL);
        QVERIFY(!QFileInfo::exists(gpuPath));
    }

    void emptyLimitsAndArithmeticExtremesRejectSafely()
    {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        QVERIFY(!okuflow::RecordingEncoderBacklog::CanStart(0));
        QVERIFY(!okuflow::RecordingEncoderBacklog::CanStart(1, 0, 128));
        QVERIFY(!okuflow::RecordingEncoderBacklog::CanStart(1, 1, 128));
        QVERIFY(!okuflow::RecordingEncoderBacklog::CanStart(1, 2, 0));
        QVERIFY(!okuflow::RecordingEncoderBacklog::CanStart(1, 2, 1));
        QVERIFY(okuflow::RecordingEncoderBacklog::CanStart(maximum / 2, 2, maximum));
        QVERIFY(!okuflow::RecordingEncoderBacklog::CanStart(maximum / 2 + 1, 2, maximum));
        FakeSinkWriterStatistics sink;
        for (const std::uint64_t sampleLimit : {0ULL, 1ULL}) {
            okuflow::RecordingEncoderBacklog limited(sampleLimit, 128);
            QVERIFY(!limited.CanAccept(sink.Query(), 0, 0, 1));
        }
        okuflow::RecordingEncoderBacklog noBytes(12, 0);
        QVERIFY(!noBytes.CanAccept(sink.Query(), 0, 0, 1));
        okuflow::RecordingEncoderBacklog backlog;
        QVERIFY(!backlog.CanAccept(sink.Query(), 0, maximum, 1));
    }

    void encoderAdmissionDropsPairAndPreservesTimeline()
    {
        okuflow::RecordingEncoderBacklog processed(4, 1024), original(4, 1024);
        FakeSinkWriterStatistics processedSink, originalSink;
        bool havePending = false;
        okuflow::RecordingTimeline processedTime, originalTime;
        processedTime.Reset(30, 1);
        originalTime.Reset(30, 1);
        const auto admitPair = [&](std::int64_t timestamp) {
            // Both queries run even if one side is under pressure.
            const bool p = processed.CanAccept(processedSink.Query(),
                processedSink.value.qwNumSamplesReceived, havePending, 16);
            const bool o = original.CanAccept(originalSink.Query(),
                originalSink.value.qwNumSamplesReceived, havePending, 16);
            if (!p || !o) return false;
            processedTime.MapTimestamp(timestamp);
            originalTime.MapTimestamp(timestamp);
            if (havePending) {
                processedSink.Submit(); originalSink.Submit();
            }
            havePending = true;
            // Processed encoder is fast; original emits nothing yet.
            processedSink.Encode(processedSink.value.qwNumSamplesReceived);
            return true;
        };
        for (int frame = 0; frame < 4; ++frame) QVERIFY(admitPair(frame * 100'000));
        QVERIFY(!admitPair(400'000));
        QCOMPARE(processedSink.value.qwNumSamplesReceived, 3ULL);
        QCOMPARE(originalSink.value.qwNumSamplesReceived, 3ULL);
        QCOMPARE(processedTime.LastTimestamp100ns(), 300'000LL);
        QCOMPARE(originalTime.LastTimestamp100ns(), 300'000LL);
        QCOMPARE(processedSink.queries, 5);
        QCOMPARE(originalSink.queries, 5);
        originalSink.Encode(2);
        QVERIFY(admitPair(600'000));
        // Recovered admission keeps the dropped-frame gap on BOTH timelines.
        QCOMPARE(processedTime.LastTimestamp100ns(), 600'000LL);
        QCOMPARE(originalTime.LastTimestamp100ns(), 600'000LL);
    }

    void encoderByteBudgetIncludesPendingAndIncoming()
    {
        constexpr std::uint64_t bgra4k = 3840ULL * 2160 * 4;
        FakeSinkWriterStatistics sink;
        okuflow::RecordingEncoderBacklog backlog;
        QVERIFY(backlog.CanAccept(sink.Query(), 0, 0, bgra4k));
        QVERIFY(backlog.CanAccept(sink.Query(), 0, 1, bgra4k));
        sink.value.qwNumSamplesReceived = 2;
        QVERIFY(backlog.CanAccept(sink.Query(), 2, 1, bgra4k));
        sink.value.qwNumSamplesReceived = 3;
        QVERIFY(!backlog.CanAccept(sink.Query(), 3, 1, bgra4k));
        sink.Encode(2);
        QVERIFY(backlog.CanAccept(sink.Query(), 3, 1, bgra4k));

        backlog.Reset(); sink.value = {};
        sink.value.dwByteCountQueued = 128u * 1024 * 1024 - 16;
        QVERIFY(!backlog.CanAccept(sink.Query(), 0, 1, 16));
        sink.value.dwByteCountQueued = 128u * 1024 * 1024 - 32;
        QVERIFY(backlog.CanAccept(sink.Query(), 0, 1, 16));
    }

    void sinkDeliveryDoesNotRetireEncoderInputs()
    {
        FakeSinkWriterStatistics sink;
        sink.value.qwNumSamplesReceived = 11;
        sink.value.qwNumSamplesProcessed = 11;
        sink.value.dwByteCountQueued = 0;
        okuflow::RecordingEncoderBacklog backlog;
        QVERIFY(!backlog.CanAccept(sink.Query(), 11, 1, 16));
        sink.Encode(1);
        QVERIFY(backlog.CanAccept(sink.Query(), 11, 1, 16));
    }

    void failedStatisticsCannotGrantUnlimitedAdmission()
    {
        FakeSinkWriterStatistics sink;
        sink.unavailable = true;
        okuflow::RecordingEncoderBacklog backlog;
        for (std::uint64_t submitted = 0; submitted < 11; ++submitted) {
            QVERIFY(backlog.CanAccept(sink.Query(), submitted, 1, 16));
            QVERIFY(!backlog.StatisticsConfirmed());
        }
        QVERIFY(!backlog.CanAccept(sink.Query(), 11, 1, 16));
        sink.unavailable = false;
        sink.value.qwNumSamplesReceived = 11;
        sink.Encode(10);
        QVERIFY(backlog.CanAccept(sink.Query(), 11, 1, 16));
        QVERIFY(backlog.StatisticsConfirmed());
        sink.unavailable = true;
        QVERIFY(backlog.CanAccept(sink.Query(), 20, 1, 16));
        QVERIFY(!backlog.CanAccept(sink.Query(), 21, 1, 16));
    }

    void inconsistentStatisticsAndOversizedSamplesFailConservatively()
    {
        FakeSinkWriterStatistics sink;
        okuflow::RecordingEncoderBacklog backlog;
        sink.value.qwNumSamplesReceived = 11;
        sink.Encode(12); // output cannot grant credit beyond local input
        QVERIFY(!backlog.CanAccept(sink.Query(), 11, 1, 16));
        QVERIFY(!backlog.StatisticsConfirmed());
        sink.Encode(10);
        QVERIFY(backlog.CanAccept(sink.Query(), 11, 1, 16));
        sink.Encode(0); // stale snapshot must not replace confirmed progress
        QVERIFY(!backlog.CanAccept(sink.Query(), 21, 1, 16));
        QVERIFY(!backlog.StatisticsConfirmed());
        backlog.Reset(); sink.value = {};
        QVERIFY(!backlog.CanAccept(sink.Query(), 0, 0,
            std::numeric_limits<std::uint64_t>::max()));
        backlog.Reset();
        QVERIFY(backlog.CanAccept(sink.Query(), 0, 0, 16));
    }

    void uploadBusyAndCoalescedOverwritesCountEveryDrop()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        manager.NotifyCaptureGpuUploadBusy();
        manager.NotifyCaptureFrames(1, 6);
        QCOMPARE(manager.DropCounts().Total(), 0ULL);
        manager.SetRequested(true);
        QCOMPARE(manager.State(), okuflow::RecordingState::Starting);
        manager.NotifyCaptureFrames(1, 6);
        manager.NotifyCaptureGpuUploadBusy();
        manager.NotifyCaptureGpuUploadBusy();
        manager.NotifyCaptureFrame(true);
        manager.NotifyCaptureGpuRetry();
        manager.NotifyCaptureSafeCopyFallback();
        QCOMPARE(manager.DropCounts().captureSlotOverwrite, 7ULL);
        QCOMPARE(manager.DropCounts().captureGpuUploadBusy, 2ULL);
        QCOMPARE(manager.DropCounts().Total(), 9ULL);
        manager.Stop();
        QTRY_COMPARE(manager.State(), okuflow::RecordingState::Failed);
        QVERIFY(manager.ShutdownForProcessExit());
        manager.NotifyCaptureGpuUploadBusy();
        QCOMPARE(manager.DropCounts().Total(), 9ULL);
        // Timestamp estimates and unavailable-statistics subsets cannot inflate
        // the number of dropped video frames.
        okuflow::RecordingDropCounts drops{};
        drops.encoderBacklog = 3;
        drops.encoderStatisticsUnavailable = 3;
        drops.unknownTimestamp = 99;
        drops.audioQueueOverflow = 42;
        drops.audioEncoderBacklog = 1;
        QCOMPARE(drops.Total(), 3ULL);
    }

    void aacPacketsRetireInputByPcmFramesNotBlockCount()
    {
        okuflow::RecordingAudioBacklog audio(3, 30'000, 300);
        FakeSinkWriterStatistics sink;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(audio.CanAccept(sink.Query(), i * 100, 100, 8192));
            QVERIFY(audio.RecordAccepted(i * 100, 100, 8192));
            sink.Submit();
        }
        sink.Encode(5); // packets exceed blocks; (5 - 2) * 1024 < first block
        sink.value.llLastTimestampEncoded = 1'000'000; // irrelevant clock domain
        QVERIFY(!audio.CanAccept(sink.Query(), 300, 100, 8192));
        QCOMPARE(audio.OutstandingBlocks(), std::size_t(3));
        sink.Encode(6); // four 1024-frame packets retire the 4096-frame block
        QVERIFY(audio.CanAccept(sink.Query(), 300, 100, 8192));
        QCOMPARE(audio.OutstandingBytes(), 16384ULL);
        QVERIFY(audio.RecordAccepted(300, 100, 8192));
        sink.Submit();
        sink.Encode(5); // stale/regressed packet counter earns no credit
        QVERIFY(!audio.CanAccept(sink.Query(), 400, 100, 8192));
        QVERIFY(!audio.StatisticsConfirmed());
        sink.Encode(300); // output cannot cover more PCM than was submitted
        QVERIFY(!audio.CanAccept(sink.Query(), 400, 100, 8192));
        sink.Encode(10);
        QVERIFY(audio.CanAccept(sink.Query(), 400, 100, 8192));
        QCOMPARE(audio.OutstandingBlocks(), std::size_t(2));
    }

    void aacUnavailableOrSinkDeliveryCannotGrantCredit()
    {
        okuflow::RecordingAudioBacklog audio(2, 20'000, 200);
        FakeSinkWriterStatistics sink;
        sink.unavailable = true;
        for (int i = 0; i < 2; ++i) {
            QVERIFY(audio.CanAccept(sink.Query(), i * 100, 100, 8192));
            QVERIFY(audio.RecordAccepted(i * 100, 100, 8192));
            sink.Submit();
        }
        QVERIFY(!audio.CanAccept(sink.Query(), 200, 100, 8192));
        sink.unavailable = false;
        sink.value.qwNumSamplesProcessed = 99;
        sink.value.llLastTimestampProcessed = 200;
        QVERIFY(!audio.CanAccept(sink.Query(), 200, 100, 8192));
        sink.Encode(6);
        QVERIFY(audio.CanAccept(sink.Query(), 200, 100, 8192));
        QVERIFY(audio.RecordAccepted(200, 100, 8192)); sink.Submit();
        sink.unavailable = true;
        QVERIFY(!audio.CanAccept(sink.Query(), 300, 100, 8192));
        audio.Reset();
        QCOMPARE(audio.OutstandingBytes(), 0ULL);
        QVERIFY(audio.CanAccept(nullptr, 1'000'000, 100, 100));
    }

    void aacDurationBytesGapsAndArithmeticBounds()
    {
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        okuflow::RecordingAudioBacklog audio(4, 200, 200);
        QVERIFY(audio.CanAccept(nullptr, 0, 100, 10));
        QVERIFY(audio.RecordAccepted(0, 100, 10));
        // An absolute capture gap is not outstanding PCM duration.
        QVERIFY(audio.CanAccept(nullptr, 1'000'000, 100, 10));
        QVERIFY(audio.RecordAccepted(1'000'000, 100, 10));
        QVERIFY(!audio.CanAccept(nullptr, 1'000'100, 1, 2));
        audio.Reset();
        QVERIFY(!audio.CanAccept(nullptr, maximum, 1, 2));
        QVERIFY(!audio.CanAccept(nullptr, -1, 1, 2));
        QVERIFY(!audio.CanAccept(nullptr, 0, 0, 2));
        QVERIFY(!audio.CanAccept(nullptr, 0, 1, 202));
        QVERIFY(!audio.CanAccept(nullptr, 0, 1, 3)); // PCM alignment
        QVERIFY(audio.RecordAccepted(100, 100, 100));
        QVERIFY(audio.CanAccept(nullptr, 99, 1, 2)); // jitter doesn't grant credit
        FakeSinkWriterStatistics sink;
        sink.value.dwByteCountQueued = 200;
        QVERIFY(!audio.CanAccept(sink.Query(), 200, 1, 2));
        QVERIFY(!okuflow::RecordingAudioBacklog(0, 200, 200).CanAccept(nullptr, 0, 1, 2));
    }

    void aacOldProgressCannotRetireNewBlockOrInflateAudioCredit()
    {
        okuflow::RecordingAudioBacklog audio(1, 10'000, 100);
        FakeSinkWriterStatistics sink;
        QVERIFY(audio.RecordAccepted(0, 100, 8192)); sink.Submit();
        sink.Encode(6);
        QVERIFY(audio.CanAccept(sink.Query(), 50, 50, 4096));
        QVERIFY(audio.RecordAccepted(50, 50, 4096)); sink.Submit();
        // Identical old statistics do not cover the cumulative end of NEW PCM.
        QVERIFY(!audio.CanAccept(sink.Query(), 100, 50, 4096));
        QCOMPARE(audio.OutstandingBlocks(), std::size_t(1));
        sink.Encode(8);
        QVERIFY(audio.CanAccept(sink.Query(), 100, 50, 4096));

        // A padded packet can round past the last submitted PCM frame. Its
        // unchanged output count must not retire later tiny blocks in that tail.
        okuflow::RecordingAudioBacklog tiny(1, 16, 1000);
        FakeSinkWriterStatistics padded;
        QVERIFY(tiny.RecordAccepted(0, 209, 2)); padded.Submit();
        padded.Encode(3); // One partial packet plus the two retained packets.
        QVERIFY(tiny.CanAccept(padded.Query(), 1000, 209, 2));
        QVERIFY(tiny.RecordAccepted(1000, 209, 2)); padded.Submit();
        QVERIFY(!tiny.CanAccept(padded.Query(), 2000, 209, 2));
        QCOMPARE(tiny.OutstandingBlocks(), std::size_t(1));
    }

    void aacAdmissionIgnoresClockDriftAndDeliveryGaps()
    {
        for (const std::int64_t tickStep : {99'000, 101'000}) {
            okuflow::RecordingAudioBacklog audio;
            FakeSinkWriterStatistics sink;
            for (std::uint64_t block = 0; block < 100'000; ++block) {
                const std::int64_t jitteredTime = block * tickStep +
                    (block / 5000) * 30'000'000;
                QVERIFY(audio.CanAccept(sink.Query(), jitteredTime, 106'666, 1024));
                QVERIFY(audio.RecordAccepted(jitteredTime, 106'666, 1024));
                sink.Submit();
                // 512-frame input blocks -> one 1024-frame raw AAC packet
                // every TWO inputs, with a different drifting timestamp domain.
                sink.Encode((block + 1) / 2);
                sink.value.llLastTimestampEncoded = block * 106'666;
                QVERIFY(audio.OutstandingBlocks() <= 6);
            }
        }
        // Stereo changes PCM frame bytes, not AAC frames per packet.
        okuflow::RecordingAudioBacklog stereo(1, 20'000, 100);
        stereo.Reset(4);
        FakeSinkWriterStatistics sink;
        QVERIFY(stereo.RecordAccepted(0, 100, 16'384)); sink.Submit();
        sink.Encode(5);
        QVERIFY(!stereo.CanAccept(sink.Query(), 100, 100, 16'384));
        sink.Encode(6);
        QVERIFY(stereo.CanAccept(sink.Query(), 100, 100, 16'384));
    }

    void audioQueueOverflowDoesNotCountDroppedVideoFrames()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        manager.SetRequested(true); // No video yet: no encoder or camera opens.
        manager.SetAudioCaptureEnabled(true);
        // Exceed the 512-block pre-worker queue without starting an encoder.
        for (int block = 0; block < 1'024; ++block) {
            okuflow::AudioFrame audio;
            audio.pcm.resize(960);
            audio.captureClock100ns = block * 100'000LL;
            audio.duration100ns = 100'000;
            manager.AddAudioFrame(std::move(audio));
        }
        QVERIFY(manager.DropCounts().audioQueueOverflow > 0);
        QCOMPARE(manager.DropCounts().queueOverflow, 0ULL);
        QCOMPARE(manager.DropCounts().Total(), 0ULL);
        manager.Stop();
        QTRY_COMPARE(manager.State(), okuflow::RecordingState::Failed);
        QVERIFY(manager.ShutdownForProcessExit());
    }

    void repeatedConstructionAndShutdown()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        for (int iteration = 0; iteration < 256; ++iteration) {
            okuflow::RecordingManager manager(nullptr, {}, {}, paths);
            QCOMPARE(manager.State(), okuflow::RecordingState::Idle);
            QVERIFY(manager.ShutdownForProcessExit());
            QVERIFY(manager.ShutdownForProcessExit());
            QVERIFY(!manager.IsWorkerAbandoned());
        }
    }

    void canvasSelectionAppliesToNextSession()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        manager.SetCanvasMode(okuflow::RecordingCanvasMode::Hd720);
        manager.SetRequested(true);
        QCOMPARE(manager.State(), okuflow::RecordingState::Starting);
        manager.SetCanvasMode(okuflow::RecordingCanvasMode::FullHd1080);
        QCOMPARE(manager.ResolveCanvas(640, 480).width, 1280u);
        QCOMPARE(manager.ResolveCanvas(480, 640).height, 1280u);
        manager.Stop();
        QTRY_COMPARE(manager.State(), okuflow::RecordingState::Failed);
        QCOMPARE(manager.ResolveCanvas(640, 480).width, 1920u);
        manager.SetRequested(true);
        QCOMPARE(manager.State(), okuflow::RecordingState::Starting);
        QCOMPARE(manager.ResolveCanvas(480, 640).height, 1920u);
        QVERIFY(manager.ShutdownForProcessExit());
    }

    void cpuFallbackUsesNv12LayoutAndColor()
    {
        // Padding must not become pixels; all four red pixels have alpha 255.
        const std::uint8_t red[] = {
            0, 0, 255, 255, 0, 0, 255, 255, 7, 7, 7, 7,
            0, 0, 255, 255, 0, 0, 255, 255, 7, 7, 7, 7};
        std::vector<std::uint8_t> nv12;
        QVERIFY(okuflow::ConvertRecordingBgraToNv12(red, 12, 2, 2, nv12));
        QCOMPARE(nv12.size(), std::size_t(6));
        for (int i = 0; i < 4; ++i) QCOMPARE(nv12[i], std::uint8_t(82));
        QCOMPARE(nv12[4], std::uint8_t(90));
        QCOMPARE(nv12[5], std::uint8_t(240));
        QVERIFY(!okuflow::ConvertRecordingBgraToNv12(red, 7, 2, 2, nv12));
        QVERIFY(!okuflow::ConvertRecordingBgraToNv12(red, 12, 1, 2, nv12));
        QVERIFY(!okuflow::ConvertRecordingBgraToNv12(nullptr, 12, 2, 2, nv12));
    }

    void processedInkUsesSnapshotAndPannedTransform()
    {
        constexpr UINT size = 100;
        std::vector<std::uint8_t> original(size * size * 4u, 0);
        auto processed = original;
        okuflow::AnnotationModel model;
        model.BeginStroke({0.75, 0.4}, Qt::red, 0.08,
                          okuflow::AnnotationItemKind::kLine);
        model.SetStraightStrokeEnd({0.75, 0.6});
        model.EndStroke();
        const auto snapshot = model.strokes();
        QVERIFY(model.Clear());
        QVERIFY(!model.hasInk());
        const okuflow::RecordingViewTransform pan{
            0.5f, 0.0f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, true};
        okuflow::ComposeRecordingAnnotations(processed, size, size, pan, snapshot);
        const std::size_t center = (50u * size + 50u) * 4u;
        QVERIFY(processed[center + 2] > 200);
        QCOMPARE(original[center + 2], std::uint8_t(0));
        auto clearedFrame = original;
        okuflow::ComposeRecordingAnnotations(clearedFrame, size, size, pan,
                                              model.strokes());
        QVERIFY(clearedFrame == original);
        QVERIFY(model.Undo());
        auto undoFrame = original;
        okuflow::ComposeRecordingAnnotations(undoFrame, size, size, pan,
                                              model.strokes());
        QVERIFY(undoFrame == processed);
    }
};

QTEST_GUILESS_MAIN(RecordingManagerTests)
#include "recording_manager_tests.moc"
