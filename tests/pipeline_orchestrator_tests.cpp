#include "okuflow/app/pipeline_orchestrator.hpp"
#include "okuflow/d3d12/frame_readiness.hpp"

#include <QtTest>

#include <limits>

namespace okuflow {

class PipelineOrchestratorTests : public QObject {
    Q_OBJECT

private slots:
    void reportsNearestRankPercentiles();
    void keepsStageTimingWindowsIndependent();
    void rejectsInvalidLatencySamples();
    void coalescesCameraFrameWakeups();
    void busyFrameRetriesLeaveQtHeartbeatResponsive();
    void sceneArrivalsDoNotExtendUserMotionTail();
    void asyncWorkPollsBetweenCameraFramesWithoutNavigation();
    void interactionAdmissionDistinguishesNavigationFromSceneArrival();
    void cameraHandoffRetriesStillPresentHeldViewport();
    void zeroFrameReconnectRetriesKeepOriginalDeadline();
};

void PipelineOrchestratorTests::zeroFrameReconnectRetriesKeepOriginalDeadline()
{
    PipelineOrchestrator orchestrator(*this, {});
    QVERIFY(orchestrator.BeginCameraReconnect(1000));
    for (qint64 failureAt : {4000, 15000, 26000}) {
        QVERIFY(!orchestrator.BeginCameraReconnect(failureAt));
        orchestrator.ScheduleCameraReconnectRetry(failureAt);
        QVERIFY(!orchestrator.CameraReconnectExpired(30999));
        QVERIFY(orchestrator.CameraReconnectExpired(31000));
    }
    // A retry's next-attempt time may exceed expiry, but may not extend it.
    QVERIFY(!orchestrator.CameraReconnectDue(31000));
    QVERIFY(orchestrator.CameraReconnectExpired(31000));
    orchestrator.CancelCameraReconnect();
    QVERIFY(!orchestrator.IsCameraReconnectPending());
}

void PipelineOrchestratorTests::reportsNearestRankPercentiles()
{
    PipelineOrchestrator orchestrator(*this, {});
    for (int value = 1; value <= 100; ++value) {
        orchestrator.RecordCaptureToPresentSample(
            static_cast<float>(value));
    }

    const TimingPercentiles values =
        orchestrator.CaptureToPresentPercentiles();
    QCOMPARE(values.sampleCount, std::size_t{100});
    QCOMPARE(values.p50Ms, 50.0f);
    QCOMPARE(values.p95Ms, 95.0f);
    QCOMPARE(values.p99Ms, 99.0f);
}

void PipelineOrchestratorTests::rejectsInvalidLatencySamples()
{
    PipelineOrchestrator orchestrator(*this, {});
    orchestrator.RecordCaptureToPresentSample(-1.0f);
    orchestrator.RecordCaptureToPresentSample(
        std::numeric_limits<float>::quiet_NaN());

    QVERIFY(!orchestrator.CaptureToPresentPercentiles().IsValid());
}

void PipelineOrchestratorTests::keepsStageTimingWindowsIndependent()
{
    PipelineOrchestrator orchestrator(*this, {});
    for (int value = 1; value <= 20; ++value) {
        orchestrator.RecordStageSample(
            FrameTimingStage::CaptureHandoff,
            static_cast<float>(value));
        orchestrator.RecordStageSample(
            FrameTimingStage::Presentation,
            static_cast<float>(value * 10));
    }
    orchestrator.RecordStageSample(
        FrameTimingStage::CudaSubmission,
        std::numeric_limits<float>::quiet_NaN());

    const TimingPercentiles capture =
        orchestrator.StagePercentiles(FrameTimingStage::CaptureHandoff);
    const TimingPercentiles presentation =
        orchestrator.StagePercentiles(FrameTimingStage::Presentation);
    QCOMPARE(capture.sampleCount, std::size_t{20});
    QCOMPARE(capture.p95Ms, 19.0f);
    QCOMPARE(presentation.sampleCount, std::size_t{20});
    QCOMPARE(presentation.p95Ms, 190.0f);
    QVERIFY(!orchestrator
                 .StagePercentiles(FrameTimingStage::CudaSubmission)
                 .IsValid());
}

void PipelineOrchestratorTests::coalescesCameraFrameWakeups()
{
    int ticks = 0;
    PipelineOrchestrator orchestrator(
        *this,
        PipelineOrchestrator::Callbacks{
            .tick = [&ticks](double) {
                ++ticks;
                return true;
            },
        });
    orchestrator.Start();
    orchestrator.NotifyCameraFrameAvailable();
    orchestrator.NotifyCameraFrameAvailable();
    orchestrator.NotifyCameraFrameAvailable();

    QTRY_COMPARE_WITH_TIMEOUT(ticks, 1, 50);
    orchestrator.Stop();
}

void PipelineOrchestratorTests::busyFrameRetriesLeaveQtHeartbeatResponsive()
{
    int heartbeats = 0;
    int busyTicks = 0;
    int admissionPolls = 0;
    int presentations = 0;
    std::uint64_t completed = 9;
    PipelineOrchestrator* scheduler = nullptr;
    PipelineOrchestrator orchestrator(*this,
        PipelineOrchestrator::Callbacks{
            .tick = [&](double) {
                if (presentations != 0) return false;
                const auto readiness = PollFrameReadiness(10,
                    [&] { return completed; }, [] { return false; }, [&] {
                        ++admissionPolls;
                        return FenceEventResult::Signaled;
                    });
                if (readiness == FrameReadiness::Busy) {
                    ++busyTicks;
                    scheduler->MarkViewportDirty();
                    scheduler->NotifyCameraFrameAvailable(1);
                } else if (readiness == FrameReadiness::Ready) {
                    ++presentations;
                    scheduler->MarkViewportPresented();
                }
                return false;
            },
        });
    scheduler = &orchestrator;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, this, [&] {
        ++heartbeats;
        if (heartbeats == 5) completed = 10;
    });
    heartbeat.start(2);
    orchestrator.Start();
    orchestrator.NotifyCameraFrameAvailable();
    QTRY_COMPARE_WITH_TIMEOUT(presentations, 1, 1000);
    orchestrator.Stop();
    heartbeat.stop();
    QVERIFY(heartbeats >= 5);
    QVERIFY(busyTicks > 0);
    QCOMPARE(admissionPolls, 1);
    QVERIFY(!orchestrator.IsViewportDirty());
}

void PipelineOrchestratorTests::asyncWorkPollsBetweenCameraFramesWithoutNavigation()
{
    for (bool fenceInterop : {false, true}) {
        bool pending = true;
        int polls = 0;
        PipelineOrchestrator orchestrator(*this,
            PipelineOrchestrator::Callbacks{
                .tick = [](double) { return false; }, // no new camera input
                .presentPendingScene = [&] { if (++polls == 3) pending = false; },
                .hasAsyncSceneWork = [&] { return pending; },
                .cameraActive = [] { return true; },
                .cameraFrameRate = [] { return 5.0; },
                .usingCuda = [] { return true; },
                .queryDisplayRefreshRate = [] { return 120; },
            });
        orchestrator.SetFenceInteropEnabled(fenceInterop);
        orchestrator.displayRefreshHz_ = 120;
        orchestrator.viewportMotionTailTimer_.invalidate();
        orchestrator.MarkViewportPresented();
        orchestrator.UpdateTimerPolicy();
        QCOMPARE(orchestrator.timer_.interval(), 8);
        for (int tick = 0; tick < 3; ++tick) {
            QVERIFY(!orchestrator.IsViewportDirty());
            QVERIFY(!orchestrator.IsViewportInteractionActive());
            orchestrator.OnTick();
        }
        QCOMPARE(polls, 3);
        QCOMPARE(orchestrator.timer_.interval(), 200); // completion restores camera-idle policy
        QVERIFY(!orchestrator.viewportMotionTailTimer_.isValid());
    }
}

void PipelineOrchestratorTests::sceneArrivalsDoNotExtendUserMotionTail()
{
    PipelineOrchestrator orchestrator(*this,
        PipelineOrchestrator::Callbacks{
            .cameraActive = [] { return true; },
            .cameraFrameRate = [] { return 30.0; },
            .usingCuda = [] { return true; },
            .queryDisplayRefreshRate = [] { return 120; },
        });
    orchestrator.SetFenceInteropEnabled(true);
    orchestrator.displayRefreshHz_ = 120;
    // Represent an expired interaction tail without a wall-clock sleep.
    orchestrator.viewportMotionTailTimer_.invalidate();
    for (int frame = 0; frame < 20; ++frame) {
        orchestrator.MarkSceneDirty();
        QVERIFY(orchestrator.IsViewportDirty());
        QVERIFY(!orchestrator.viewportMotionTailTimer_.isValid());
        orchestrator.MarkViewportPresented();
        orchestrator.UpdateTimerPolicy();
        QCOMPARE(orchestrator.timer_.interval(), 33);
    }
    orchestrator.MarkViewportDirty();
    QVERIFY(orchestrator.viewportMotionTailTimer_.isValid());
    QCOMPARE(orchestrator.timer_.interval(), 8);
    orchestrator.MarkViewportPresented();
    orchestrator.UpdateTimerPolicy();
    QCOMPARE(orchestrator.timer_.interval(), 8);
}

void PipelineOrchestratorTests::interactionAdmissionDistinguishesNavigationFromSceneArrival()
{
    bool continuousMotion = false;
    bool mousePan = false;
    PipelineOrchestrator orchestrator(*this,
        PipelineOrchestrator::Callbacks{
            .hasContinuousMotion = [&] { return continuousMotion; },
            .isMousePanActive = [&] { return mousePan; },
        });
    QVERIFY(!orchestrator.IsViewportInteractionActive());
    orchestrator.MarkSceneDirty();
    QVERIFY(!orchestrator.IsViewportInteractionActive());
    continuousMotion = true;
    QVERIFY(orchestrator.IsViewportInteractionActive());
    continuousMotion = false;
    mousePan = true;
    QVERIFY(orchestrator.IsViewportInteractionActive());
    mousePan = false;
    QVERIFY(!orchestrator.IsViewportInteractionActive());

    // Redraw/readback/photo retries may request the fast viewport clock, but
    // cannot masquerade as navigation and indefinitely suppress inference.
    for (int retry = 0; retry != 20; ++retry) {
        orchestrator.MarkViewportDirty();
        QVERIFY(!orchestrator.IsViewportInteractionActive());
    }
    // A wheel/key update explicitly opens the navigation settling tail.
    // Camera/redraw activity neither resets nor extends that input lease.
    orchestrator.NotifyViewportMotion();
    QVERIFY(orchestrator.IsViewportInteractionActive());
    orchestrator.MarkViewportPresented();
    QVERIFY(orchestrator.IsViewportInteractionActive());
    orchestrator.viewportInteractionTailTimer_.invalidate();
    for (int frame = 0; frame != 20; ++frame) {
        orchestrator.MarkSceneDirty();
        orchestrator.MarkViewportDirty();
        QVERIFY(!orchestrator.IsViewportInteractionActive());
    }
}

void PipelineOrchestratorTests::cameraHandoffRetriesStillPresentHeldViewport()
{
    // Retained D3D11/CUDA handoffs return before consuming another camera
    // frame. The independently scheduled viewport must still see pan, zoom,
    // and resize requests over the scene that Extra Stable is holding.
    bool cameraHandoffPending = false;
    bool presenterNeedsFrame = false;
    int cameraFrames = 0;
    int temporalUpdates = 0;
    int recordingFrames = 0;
    int presentations = 0;
    int sceneGeneration = 0;
    int presentedSceneGeneration = -1;
    float focus = 0.5f;
    float presentedFocus = -1.0f;
    float zoom = 1.33f;
    float presentedZoom = -1.0f;
    int viewportWidth = 1108;
    int presentedWidth = 0;
    PipelineOrchestrator* scheduler = nullptr;
    PipelineOrchestrator orchestrator(*this,
        PipelineOrchestrator::Callbacks{
            .tick = [&](double) {
                if (cameraHandoffPending) return false;
                ++cameraFrames;
                ++temporalUpdates;
                ++recordingFrames;
                ++sceneGeneration;
                scheduler->MarkSceneDirty();
                return true;
            },
            .presentPendingScene = [&] {
                if (!scheduler->IsViewportDirty() && !presenterNeedsFrame)
                    return;
                ++presentations;
                presentedSceneGeneration = sceneGeneration;
                presentedFocus = focus;
                presentedZoom = zoom;
                presentedWidth = viewportWidth;
                presenterNeedsFrame = false;
                scheduler->MarkViewportPresented();
            },
            .needsScenePresent = [&] { return presenterNeedsFrame; },
            .cameraActive = [] { return true; },
            .cameraFrameRate = [] { return 30.0; },
            .usingCuda = [] { return true; },
        });
    scheduler = &orchestrator;
    orchestrator.Start();
    orchestrator.OnTick();
    QCOMPARE(presentations, 1);
    QCOMPARE(presentedSceneGeneration, 1);

    cameraHandoffPending = true;
    focus = 0.7f;
    orchestrator.MarkViewportDirty();
    orchestrator.OnTick();
    QCOMPARE(presentations, 2);
    QCOMPARE(presentedFocus, focus);
    zoom = 2.5f;
    orchestrator.MarkViewportDirty();
    orchestrator.OnTick();
    QCOMPARE(presentations, 3);
    QCOMPARE(presentedZoom, zoom);

    // Native resize requests can require a redraw without another camera
    // arrival or user input notification (for example closing Advanced).
    viewportWidth = 1600;
    presenterNeedsFrame = true;
    orchestrator.OnTick();
    QCOMPARE(presentations, 4);
    QCOMPARE(presentedWidth, viewportWidth);
    QCOMPARE(presentedSceneGeneration, 1);
    QCOMPARE(cameraFrames, 1);
    QCOMPARE(temporalUpdates, 1);
    QCOMPARE(recordingFrames, 1);
    QVERIFY(!orchestrator.IsViewportDirty());
    orchestrator.OnTick();
    QCOMPARE(presentations, 4);
    orchestrator.Stop();
}

} // namespace okuflow

QTEST_GUILESS_MAIN(okuflow::PipelineOrchestratorTests)

#include "pipeline_orchestrator_tests.moc"
