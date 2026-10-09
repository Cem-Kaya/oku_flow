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
};

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

} // namespace okuflow

QTEST_GUILESS_MAIN(okuflow::PipelineOrchestratorTests)

#include "pipeline_orchestrator_tests.moc"
