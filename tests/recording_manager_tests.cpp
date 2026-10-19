// This target is Windows-only in CMake. Keep its QObject visible to moc,
// which does not inherit the compiler's implicit _WIN32 predefined macro.
#include "okuflow/app/recording_manager.hpp"
#include "okuflow/common/video_archive.hpp"

#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QDir>
#include <QDirIterator>
#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <mferror.h>
#include <wrl/client.h>
#include <QtTest>
#include <limits>
#include <chrono>

namespace okuflow {
struct RecordingManagerTestAccess {
    static void SetEncoderStart(RecordingManager& manager,
        std::function<bool(const QString&, VideoRecorder::Codec)> start) {
        manager.encoderStartOverride_ = std::move(start);
    }
    static bool StartSegment(RecordingManager& manager) {
        RecordingManager::QueuedFrame frame;
        frame.original.width = frame.original.height = 64;
        frame.original.identity.captureClock100ns = 100'000;
        return manager.StartSegment(frame);
    }
    static QString SelectedPath(RecordingManager& manager) {
        return manager.segmentOriginalPath_;
    }
    static QString RequestedPath(RecordingManager& manager) {
        return manager.SegmentPath();
    }
    static CapturedFrame QueuedOriginal(RecordingManager& manager, std::size_t index) {
        std::lock_guard lock(manager.queueMutex_);
        return manager.frameQueue_.at(index).original;
    }
    static bool WriteNextQueued(RecordingManager& manager) {
        RecordingManager::QueuedFrame frame;
        {
            std::lock_guard lock(manager.queueMutex_);
            if (manager.frameQueue_.empty()) return false;
            frame = std::move(manager.frameQueue_.front());
            manager.frameQueue_.pop_front();
        }
        return manager.WriteFrame(std::move(frame));
    }
    static bool WriteAudio(RecordingManager& manager, AudioFrame frame) {
        return manager.WriteAudioFrame(std::move(frame));
    }
    static const VideoRecorder& Recorder(const RecordingManager& manager) {
        return manager.originalRecorder_;
    }
    static void Abandon(RecordingManager& manager) {
        manager.AbandonWedgedWorker(QStringLiteral("fixture native call deadline"));
    }
    static void BeginPart(RecordingManager& manager, int part) {
        manager.segmentIndex_ = part;
        manager.segmentClockOrigin100ns_ = manager.segmentLastCaptureClock100ns_ = -1;
    }
    static void Admit(RecordingManager& manager, std::uint64_t sequence,
                      std::int64_t clock, std::int64_t pts) {
        RecordingFrameIdentity identity;
        identity.sequenceNumber = sequence; identity.captureClock100ns = clock;
        manager.PublishFrameAdmission(identity, pts);
    }
    static bool FailFirstFrame(RecordingManager& manager) {
        return manager.CompleteFrameSubmission(false, {}, -1,
                                               QStringLiteral("fixture first-frame rejection"));
    }
    static void PrimeStartingWithoutCodecWorker(RecordingManager& manager) {
        manager.ShutdownForProcessExit();manager.shutdownRequested_=false;
        manager.stopRequested_=false;manager.frameQueue_.clear();manager.audioQueue_.clear();
        manager.sessionStartTicket_ = std::make_shared<RecordingManager::SessionStartTicket>(manager.sessionInfo_.id);
        manager.sessionStartedPosted_ = false;
        manager.segmentFinalized_=true;manager.SetState(RecordingState::Starting);
    }
    static std::size_t QueuedVideo(RecordingManager& manager) {
        std::lock_guard lock(manager.queueMutex_);return manager.frameQueue_.size();
    }
    static bool AudioEnabled(RecordingManager& manager) {return manager.audioCaptureEnabled_.load();}
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
            && !manager.audioStartupPending_ && !manager.stopRequested_ && !manager.workerWriting_
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
struct VideoRecorderTestAccess {
    static std::uint64_t PcmFrames(const VideoRecorder& recorder) {
        return recorder.audioTimeline_.Frames();
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
    void failedCodecBytesCannotSuppressFallback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        okuflow::UserDataPaths paths(directory.path(), QStringLiteral("C:/fixture-install"));
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        okuflow::RecordingSessionInfo session;
        session.id = QStringLiteral("codec-fallback-fixture");
        manager.SetRequested(true, session);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        QStringList attempted;
        Access::SetEncoderStart(manager, [&](const QString& path,
                                             okuflow::VideoRecorder::Codec codec) {
            attempted.append(path);
            QFile output(path);
            if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                output.write("fixture encoder metadata") != 24) return false;
            return codec != okuflow::VideoRecorder::Codec::Av1;
        });
        QVERIFY(Access::StartSegment(manager));
        QCOMPARE(manager.State(), okuflow::RecordingState::Recording);
        QCOMPARE(attempted.size(), 2);
        const QString original = Access::SelectedPath(manager);
        QVERIFY(original != Access::RequestedPath(manager));
        for (const QString& path : attempted) {
            QFile output(path);
            QVERIFY(output.open(QIODevice::ReadOnly));
            QCOMPARE(output.readAll(), QByteArray("fixture encoder metadata"));
        }
        QCOMPARE(QSet<QString>(attempted.begin(), attempted.end()).size(), attempted.size());
        QVERIFY(manager.StopSession(session.id));
        Access::Finish(manager, false);
    }
    void preexistingRecordingOutputIsPreserved()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        okuflow::UserDataPaths paths(directory.path(), QStringLiteral("C:/fixture-install"));
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        okuflow::RecordingSessionInfo session;
        session.id = QStringLiteral("codec-collision-fixture");
        manager.SetRequested(true, session);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        const QString path = Access::RequestedPath(manager);
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(output.write("user-owned"), 10);
        output.close();
        int attempts = 0;
        Access::SetEncoderStart(manager, [&](const QString&,
                                             okuflow::VideoRecorder::Codec) {
            ++attempts;
            return true;
        });
        QVERIFY(!Access::StartSegment(manager));
        QCOMPARE(attempts, 0);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), QByteArray("user-owned"));
    }
    void abandonedStartupCannotPublishLateOriginalSuccess()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        manager.SetRequested(true);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        Access::SetEncoderStart(manager, [&](const QString&, okuflow::VideoRecorder::Codec) {
            // A synchronous native call returns only after its watchdog has
            // revoked recording. That return cannot restore Recording state.
            Access::Abandon(manager);
            return true;
        });
        QVERIFY(!Access::StartSegment(manager));
        QVERIFY(manager.IsWorkerAbandoned());
        QCOMPARE(manager.State(), okuflow::RecordingState::Failed);
        QVERIFY(!manager.CurrentArchive());
        QVERIFY(!manager.LastSavedSegment());
    }
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
    void microphoneDecisionHoldsVideoAndLateResultsCannotEnableAnotherSession()
    {
        QTemporaryDir directory;QVERIFY(directory.isValid());
        okuflow::UserDataPaths paths(directory.path(),QStringLiteral("C:/fixture-install"));
        okuflow::RecordingManager manager(nullptr,{},{},paths);
        okuflow::RecordingSessionInfo first;first.id="audio-pending-a";manager.SetRequested(true,first);
        using Access=okuflow::RecordingManagerTestAccess;Access::PrimeStartingWithoutCodecWorker(manager);
        manager.SetSessionAudioPending(first.id);
        const std::vector<uint8_t> pixels(64*64*4,127);
        const auto submit=[&] {
            okuflow::CapturedFrame original;original.width=original.height=64;original.pixels=pixels;
            original.identity.captureClock100ns=100000;okuflow::RecordingViewTransform transform;transform.valid=true;
            manager.AddSceneFrame(pixels.data(),64,64,transform,std::move(original));
        };
        submit();QCOMPARE(Access::QueuedVideo(manager),std::size_t(0));
        manager.SetSessionAudioResult("foreign",true);submit();QCOMPARE(Access::QueuedVideo(manager),std::size_t(0));
        QVERIFY(!manager.CurrentSessionInfo()->actualAudio);
        manager.SetSessionAudioResult(first.id,false);submit();QCOMPARE(Access::QueuedVideo(manager),std::size_t(1));
        QVERIFY(manager.StopSession(first.id));Access::Finish(manager,false);
        manager.SetSessionAudioResult(first.id,true);QVERIFY(!Access::AudioEnabled(manager));
        okuflow::RecordingSessionInfo second;second.id="audio-pending-b";manager.SetRequested(true,second);
        manager.SetSessionAudioResult(first.id,true);QVERIFY(!manager.CurrentSessionInfo()->actualAudio);
        QVERIFY(manager.StopSession(second.id));Access::Finish(manager,false);
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

    void encoderAdmissionPreservesDroppedSourceTime()
    {
        okuflow::RecordingEncoderBacklog original(4, 1024);
        FakeSinkWriterStatistics sink;
        bool havePending = false;
        okuflow::RecordingTimeline timeline;
        timeline.Reset(30, 1);
        const auto admit = [&](std::int64_t timestamp) {
            if (!original.CanAccept(sink.Query(), sink.value.qwNumSamplesReceived,
                                    havePending, 16)) return false;
            timeline.MapTimestamp(timestamp);
            if (havePending) sink.Submit();
            havePending = true;
            return true;
        };
        for (int frame = 0; frame < 4; ++frame) QVERIFY(admit(frame * 100'000));
        QVERIFY(!admit(400'000));
        QCOMPARE(sink.value.qwNumSamplesReceived, 3ULL);
        QCOMPARE(timeline.LastTimestamp100ns(), 300'000LL);
        QCOMPARE(sink.queries, 5);
        sink.Encode(2);
        QVERIFY(admit(600'000));
        QCOMPARE(timeline.LastTimestamp100ns(), 600'000LL);
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

    void deprecatedCanvasNeverChangesOriginalDimensions()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        manager.SetCanvasMode(okuflow::RecordingCanvasMode::Nhd360);
        QCOMPARE(manager.ResolveCanvas(1920, 1080).width, 1920u);
        manager.SetRequested(true);
        manager.SetCanvasMode(okuflow::RecordingCanvasMode::Hd720);
        QCOMPARE(manager.ResolveCanvas(1920, 1080).height, 1080u);
        QCOMPARE(manager.ResolveCanvas(1080, 1920).height, 1920u);
        manager.Stop();
        QTRY_COMPARE(manager.State(), okuflow::RecordingState::Failed);
        QCOMPARE(manager.ResolveCanvas(3840, 2160).width, 3840u);
        QVERIFY(manager.ShutdownForProcessExit());
    }

    void originalAdmissionOwnsOnlyCleanSourceAndRemainsBounded()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        manager.SetRequested(true);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        okuflow::CapturedFrame source;
        source.width = 64; source.height = 48;
        source.pixels.assign(64u * 48u * 4u, 37);
        source.identity.captureClock100ns = 100'000;
        source.identity.sequenceNumber = 71;
        // Invalid/absent legacy display payload cannot block clean admission.
        manager.AddSceneFrame(nullptr, 0, 0, {}, source);
        manager.AddGpuSceneFrame({}, {}, source);
        manager.AddOriginalFrame(source);
        QCOMPARE(Access::QueuedVideo(manager), std::size_t(3));
        for (std::size_t index = 0; index < 3; ++index) {
            const auto queued = Access::QueuedOriginal(manager, index);
            QCOMPARE(queued.width, 64u); QCOMPARE(queued.height, 48u);
            QVERIFY(queued.pixels == source.pixels);
            QCOMPARE(queued.identity.sequenceNumber, 71ULL);
        }
        source.pixels[0] = 99;
        QCOMPARE(Access::QueuedOriginal(manager, 0).pixels[0], std::uint8_t(37));
        auto malformed = source; malformed.pixels.pop_back();
        manager.AddOriginalFrame(std::move(malformed));
        QCOMPARE(Access::QueuedVideo(manager), std::size_t(3));
        for (unsigned index = 0; index < 20; ++index) manager.AddOriginalFrame(source);
        QCOMPARE(Access::QueuedVideo(manager), std::size_t(12));
        QCOMPARE(manager.DropCounts().queueOverflow, 11ULL);
        manager.Stop(); Access::Finish(manager, false);
    }

    void activeTimelinePublishesExactBoundedPartAdmissions()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        okuflow::RecordingSessionInfo session; session.id = QStringLiteral("admission-session");
        manager.SetRequested(true, session);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        QVERIFY(!manager.CurrentTimeline());
        Access::Admit(manager, 71, 1'000'000, 0);
        const auto first = manager.CurrentTimeline(); QVERIFY(first);
        QCOMPARE(first->session.id, session.id); QCOMPARE(first->segmentIndex, 1);
        QCOMPARE(first->firstCaptureClock100ns, 1'000'000LL);
        Access::Admit(manager, 74, 2'000'000, 1'000'000);
        auto current = manager.CurrentTimeline(); QVERIFY(current);
        QCOMPARE(current->lastCaptureClock100ns, 2'000'000LL);
        QCOMPARE(current->lastAdmittedSequence, 74ULL);
        QCOMPARE(current->lastAdmittedPts100ns, 1'000'000LL);
        QVERIFY(current->FindAdmission(71, 1'000'000));
        QVERIFY(!current->FindAdmission(72, 1'500'000)); // A dropped frame has no invented PTS.
        QVERIFY(!current->FindAdmission(71, 2'000'000)); // Sequence alone is not identity.
        QCOMPARE(first->recentAdmissions.size(), std::size_t(1)); // Earlier snapshots are immutable.
        for (unsigned index = 0; index < 130; ++index)
            Access::Admit(manager, 100 + index, 3'000'000 + index, 2'000'000 + index);
        current = manager.CurrentTimeline();
        QCOMPARE(current->recentAdmissions.size(), std::size_t(128));
        QVERIFY(!current->FindAdmission(71, 1'000'000));
        Access::BeginPart(manager, 2);
        Access::Admit(manager, 500, 8'000'000, 0);
        current = manager.CurrentTimeline();
        QCOMPARE(current->segmentIndex, 2); QCOMPARE(current->firstCaptureClock100ns, 8'000'000LL);
        QCOMPARE(current->recentAdmissions.size(), std::size_t(1));
        QCOMPARE(current->FindAdmission(500, 8'000'000)->pts100ns, 0LL);
        manager.Stop(); Access::Finish(manager, false);
        QCOMPARE(manager.CurrentTimeline()->session.completion, okuflow::RecordingSessionCompletion::Failed);
        manager.SetRequested(true);
        QVERIFY(!manager.CurrentTimeline());
        manager.Stop(); Access::Finish(manager, false);
    }

    void startAcknowledgementRejectsStartupFailures_data()
    {
        QTest::addColumn<bool>("encoderStartFails");
        QTest::newRow("encoder-start-failure") << true;
        QTest::newRow("first-frame-rejection") << false;
    }
    void startAcknowledgementRejectsStartupFailures()
    {
        QFETCH(bool, encoderStartFails);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        int starts = 0;
        okuflow::RecordingManager manager(nullptr, {}, {}, paths, {},
            [&](const okuflow::RecordingSessionInfo&) { ++starts; });
        manager.SetRequested(true);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        Access::SetEncoderStart(manager, [&](const QString&, okuflow::VideoRecorder::Codec) {
            return !encoderStartFails;
        });
        QCOMPARE(Access::StartSegment(manager), !encoderStartFails);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(starts, 0); // Encoder startup alone acknowledges nothing.
        if (!encoderStartFails) QVERIFY(!Access::FailFirstFrame(manager));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(manager.State(), okuflow::RecordingState::Failed);
        QCOMPARE(starts, 0); QVERIFY(!manager.CurrentTimeline());
    }

    void startAcknowledgementRunsOnceAfterTimelineAndAudioAreReady()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        int starts = 0; bool timelineReady = false;
        okuflow::RecordingSessionInfo observed;
        okuflow::RecordingManager* active = nullptr;
        okuflow::RecordingManager manager(nullptr, {}, {}, paths, {},
            [&](const okuflow::RecordingSessionInfo& session) {
                ++starts; observed = session;
                const auto timeline = active->CurrentTimeline();
                timelineReady = timeline && timeline->session.id == session.id &&
                    timeline->FindAdmission(7, 1'000'000).has_value();
            });
        active = &manager;
        okuflow::RecordingSessionInfo session;
        session.id = QStringLiteral("started-session");
        session.ownerKind = okuflow::RecordingOwnerKind::AgentSession;
        session.ownerId = QStringLiteral("started-owner");
        manager.SetRequested(true, session);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        manager.SetSessionAudioPending(session.id);
        manager.SetSessionAudioResult(session.id, true);
        Access::Admit(manager, 7, 1'000'000, 0);
        QCOMPARE(starts, 0); // Delivery is queued to the Qt owning thread.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(starts, 1); QVERIFY(timelineReady);
        QCOMPARE(observed.id, session.id); QCOMPARE(observed.ownerId, session.ownerId);
        QCOMPARE(observed.ownerKind, session.ownerKind); QVERIFY(observed.actualAudio);
        Access::Admit(manager, 8, 2'000'000, 1'000'000);
        Access::BeginPart(manager, 2); Access::Admit(manager, 9, 3'000'000, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(starts, 1); // More frames and source-format parts share one acknowledgement.
        manager.Stop(); Access::Finish(manager, false);
    }

    void queuedStartAcknowledgementCannotTargetEndedOrReplacedSession()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        QStringList started;
        okuflow::RecordingManager manager(nullptr, {}, {}, paths, {},
            [&](const okuflow::RecordingSessionInfo& session) { started.append(session.id); });
        okuflow::RecordingSessionInfo first; first.id = QStringLiteral("first-start-ticket");
        manager.SetRequested(true, first);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        Access::Admit(manager, 1, 1'000'000, 0); // Its callback remains queued.
        manager.Stop(); Access::Finish(manager, false);
        // Reusing even the same supplied id cannot revive the old ticket.
        manager.SetRequested(true, first);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(started.isEmpty());
        Access::Admit(manager, 2, 2'000'000, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(started, QStringList{first.id});
        manager.Stop(); Access::Finish(manager, false);
    }

    void revokedStartAcknowledgementsCannotOutliveManager_data()
    {
        QTest::addColumn<QString>("revoke");
        QTest::newRow("first-frame-then-failure") << QStringLiteral("failure");
        QTest::newRow("worker-abandonment") << QStringLiteral("abandon");
        QTest::newRow("process-shutdown") << QStringLiteral("shutdown");
        QTest::newRow("manager-destruction") << QStringLiteral("destroy");
    }
    void revokedStartAcknowledgementsCannotOutliveManager()
    {
        QFETCH(QString, revoke);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const okuflow::UserDataPaths paths(directory.path());
        int starts = 0;
        auto manager = std::make_unique<okuflow::RecordingManager>(nullptr,
            okuflow::RecordingManager::StatusCallback{},
            okuflow::RecordingManager::SegmentSavedCallback{}, paths,
            okuflow::RecordingManager::SessionEndedCallback{},
            [&](const okuflow::RecordingSessionInfo&) { ++starts; });
        manager->SetRequested(true);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(*manager);
        Access::Admit(*manager, 1, 1'000'000, 0);
        if (revoke == QStringLiteral("failure")) Access::Finish(*manager, false);
        else if (revoke == QStringLiteral("abandon")) Access::Abandon(*manager);
        else if (revoke == QStringLiteral("shutdown")) QVERIFY(manager->ShutdownForProcessExit());
        else manager.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(starts, 0);
    }

    void nativeOriginalRecordsOneVideoAndAudioTrack()
    {
        if (!qEnvironmentVariableIntValue("OKUFLOW_TEST_ORIGINAL_RECORDING_NATIVE"))
            QSKIP("Opt-in serial native codec acceptance; no camera or microphone.");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto comExit = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION)));
        const auto mfExit = qScopeGuard([] { MFShutdown(); });
        const QString evidenceRoot = qEnvironmentVariable("OKUFLOW_TEST_ORIGINAL_RECORDING_EVIDENCE_DIR",
            QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
                QStringLiteral("OkuFlow/test-evidence/original-recording")));
        QVERIFY(QDir().mkpath(evidenceRoot));
        QTemporaryDir directory(QDir(evidenceRoot).filePath(QStringLiteral("native-XXXXXX")));
        QVERIFY(directory.isValid()); directory.setAutoRemove(false);
        qInfo().noquote() << "Original recording evidence:" << directory.path();
        const okuflow::UserDataPaths paths(directory.path());
        okuflow::RecordingManager manager(nullptr, {}, {}, paths);
        okuflow::RecordingSessionInfo session; session.id = QStringLiteral("native-original-only");
        manager.SetRequested(true, session);
        using Access = okuflow::RecordingManagerTestAccess;
        Access::PrimeStartingWithoutCodecWorker(manager);
        manager.SetSessionAudioResult(session.id, true);
        manager.SetCanvasMode(okuflow::RecordingCanvasMode::Nhd360);
        constexpr std::int64_t origin = 1'000'000;
        for (unsigned index = 0; index < 30; ++index) {
            okuflow::CapturedFrame source;
            source.width = 64; source.height = 48;
            source.pixels.resize(64u * 48u * 4u);
            for (std::size_t at = 0; at < source.pixels.size(); at += 4) {
                source.pixels[at] = 0; source.pixels[at + 1] = 0;
                source.pixels[at + 2] = 0; source.pixels[at + 3] = 255;
                // The view stays black while Original advances red -> green
                // -> blue. A display hold must never freeze the recording.
                source.pixels[at + (index < 10 ? 2 : index < 20 ? 1 : 0)] = 255;
            }
            source.identity = {origin + index * 10'000'000LL / 30,
                               origin + index * 10'000'000LL / 30, index, 30, 1};
            const std::vector<std::uint8_t> processed(8u * 8u * 4u, 0);
            const okuflow::RecordingViewTransform cropped{
                .5f, .5f, .25f, .25f, 0, 0, 1, 1, true};
            manager.AddSceneFrame(processed.data(), 8, 8, cropped, std::move(source));
            QVERIFY(Access::WriteNextQueued(manager));
            QCOMPARE(manager.State(), okuflow::RecordingState::Recording);
            okuflow::AudioFrame audio;
            audio.pcm.assign(1600 * 2, 0); // mono 48 kHz, exactly 1/30 second
            audio.captureClock100ns = origin + index * 10'000'000LL / 30;
            audio.duration100ns = 10'000'000LL / 30;
            QVERIFY(Access::WriteAudio(manager, std::move(audio)));
            // Exercise ordinary paced input, rather than treating the bounded
            // encoder queue as an unbounded synthetic offline transcoder.
            QTest::qWait(34);
        }
        QCOMPARE(okuflow::VideoRecorderTestAccess::PcmFrames(Access::Recorder(manager)), 48'000ULL);
        const auto timeline = manager.CurrentTimeline(); QVERIFY(timeline);
        QCOMPARE(timeline->firstCaptureClock100ns, origin);
        QCOMPARE(timeline->lastAdmittedSequence, 29ULL);
        const auto admittedFirst = timeline->FindAdmission(0, origin); QVERIFY(admittedFirst);
        QCOMPARE(admittedFirst->pts100ns, 0LL);
        QVERIFY(manager.StopSession(session.id)); Access::Finish(manager, true);
        QCOMPARE(manager.State(), okuflow::RecordingState::Completed);
        const auto saved = manager.LastSavedSegment(); QVERIFY(saved.has_value());
        QCOMPARE(saved->session.id, session.id); QCOMPARE(saved->segmentIndex, 1);
        QVERIFY(saved->processedPath.isEmpty()); QCOMPARE(saved->framesWritten, 30ULL);
        QCOMPARE(saved->firstCaptureClock100ns, origin);
        QCOMPARE(saved->lastCaptureClock100ns, origin + 29 * 10'000'000LL / 30);
        QCOMPARE(saved->completion, okuflow::RecordingSessionCompletion::Completed);
        QVERIFY(saved->archive); QVERIFY(manager.CurrentArchive() == saved->archive);
        const auto archived = saved->archive->Snapshot();
        QCOMPARE(QString::fromStdWString(archived.path), saved->originalPath);
        QVERIFY(archived.finalized); QVERIFY(archived.complete);
        QStringList outputs;
        QDirIterator files(directory.path(), {QStringLiteral("*.mp4")}, QDir::Files,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) outputs.append(files.next());
        // std::filesystem's fallback path uses Windows separators; Qt's
        // enumerator returns slash-separated labels for the same file. Hold
        // exact file identities rather than treating spelling as identity.
        Microsoft::WRL::ComPtr<IMFByteStream> selectedFile;
        okuflow::VideoFileIdentity selectedIdentity;
        QVERIFY(SUCCEEDED(okuflow::OpenStableVideoReadStream(
            saved->originalPath.toStdWString(), selectedFile.GetAddressOf(), &selectedIdentity)));
        unsigned selectedOutputs = 0;
        for (const QString& output : outputs) {
            QVERIFY(!output.contains(QStringLiteral("_processed")));
            Microsoft::WRL::ComPtr<IMFByteStream> outputFile;
            okuflow::VideoFileIdentity outputIdentity;
            QVERIFY(SUCCEEDED(okuflow::OpenStableVideoReadStream(
                output.toStdWString(), outputFile.GetAddressOf(), &outputIdentity)));
            if (outputIdentity.SameFile(selectedIdentity)) {
                ++selectedOutputs;
                continue;
            }
            bool knownFailedAttempt = false;
            for (const QString& failedPath : saved->failedAttemptPaths) {
                Microsoft::WRL::ComPtr<IMFByteStream> failedFile;
                okuflow::VideoFileIdentity failedIdentity;
                QVERIFY(SUCCEEDED(okuflow::OpenStableVideoReadStream(
                    failedPath.toStdWString(), failedFile.GetAddressOf(), &failedIdentity)));
                if (outputIdentity.SameFile(failedIdentity)) {
                    knownFailedAttempt = true;
                    break;
                }
            }
            QVERIFY2(knownFailedAttempt, qPrintable(output));
        }
        QCOMPARE(selectedOutputs, 1u);
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        Microsoft::WRL::ComPtr<IMFAttributes> readerAttributes;
        QVERIFY(SUCCEEDED(MFCreateAttributes(readerAttributes.GetAddressOf(), 1)));
        QVERIFY(SUCCEEDED(readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)));
        QVERIFY(SUCCEEDED(MFCreateSourceReaderFromURL(saved->originalPath.toStdWString().c_str(),
            readerAttributes.Get(), reader.GetAddressOf())));
        unsigned videos = 0, audios = 0; DWORD videoStream = 0;
        for (DWORD stream = 0; stream < 16; ++stream) {
            Microsoft::WRL::ComPtr<IMFMediaType> type;
            const HRESULT result = reader->GetNativeMediaType(stream, 0, type.GetAddressOf());
            if (result == MF_E_INVALIDSTREAMNUMBER) break;
            QVERIFY(SUCCEEDED(result)); GUID major{};
            QVERIFY(SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)));
            if (major == MFMediaType_Video) {
                ++videos; videoStream = stream; UINT32 width = 0, height = 0;
                QVERIFY(SUCCEEDED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)));
                QCOMPARE(width, 64u); QCOMPARE(height, 48u);
            }
            if (major == MFMediaType_Audio) ++audios;
        }
        QCOMPARE(videos, 1u); QCOMPARE(audios, 1u);
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        QVERIFY(SUCCEEDED(reader->SetStreamSelection(videoStream, TRUE)));
        Microsoft::WRL::ComPtr<IMFMediaType> decodedType;
        QVERIFY(SUCCEEDED(MFCreateMediaType(decodedType.GetAddressOf())));
        QVERIFY(SUCCEEDED(decodedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)));
        QVERIFY(SUCCEEDED(decodedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)));
        QVERIFY(SUCCEEDED(reader->SetCurrentMediaType(videoStream, nullptr, decodedType.Get())));
        unsigned decoded = 0;
        bool sawRed = false, sawGreen = false, sawBlue = false, ended = false;
        LONGLONG previousPts = -1;
        for (unsigned attempt = 0; attempt < 64 && !ended; ++attempt) {
            DWORD flags = 0; LONGLONG pts = -1;
            Microsoft::WRL::ComPtr<IMFSample> sample;
            QVERIFY(SUCCEEDED(reader->ReadSample(videoStream, 0, nullptr, &flags, &pts,
                                                sample.GetAddressOf())));
            ended = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if (!sample) continue;
            QVERIFY(pts >= previousPts); previousPts = pts;
            Microsoft::WRL::ComPtr<IMFMediaBuffer> pixels;
            QVERIFY(SUCCEEDED(sample->ConvertToContiguousBuffer(pixels.GetAddressOf())));
            BYTE* bytes = nullptr; DWORD length = 0;
            QVERIFY(SUCCEEDED(pixels->Lock(&bytes, nullptr, &length)));
            const bool red = length >= 4 && bytes[2] > 200 && bytes[1] < 60 && bytes[0] < 60;
            const bool green = length >= 4 && bytes[1] > 200 && bytes[2] < 60 && bytes[0] < 60;
            const bool blue = length >= 4 && bytes[0] > 200 && bytes[1] < 60 && bytes[2] < 60;
            pixels->Unlock(); QVERIFY(red || green || blue);
            sawRed |= red; sawGreen |= green; sawBlue |= blue; ++decoded;
        }
        QVERIFY(ended); QVERIFY(decoded > 1);
        QVERIFY(sawRed && sawGreen && sawBlue);
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


};

QTEST_GUILESS_MAIN(RecordingManagerTests)
#include "recording_manager_tests.moc"
