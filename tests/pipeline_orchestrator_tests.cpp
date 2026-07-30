#include "openzoom/app/pipeline_orchestrator.hpp"

#include <QtTest>

#include <limits>

namespace openzoom {

class PipelineOrchestratorTests : public QObject {
    Q_OBJECT

private slots:
    void reportsNearestRankPercentiles();
    void keepsStageTimingWindowsIndependent();
    void rejectsInvalidLatencySamples();
    void coalescesCameraFrameWakeups();
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

} // namespace openzoom

QTEST_GUILESS_MAIN(openzoom::PipelineOrchestratorTests)

#include "pipeline_orchestrator_tests.moc"
