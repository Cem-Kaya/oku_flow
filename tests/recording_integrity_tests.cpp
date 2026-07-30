#include "openzoom/common/frame_pipeline.hpp"
#include "openzoom/common/recording_contract.hpp"

#include <QtTest>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using openzoom::RecordingCanvasMode;
using openzoom::RecordingCompletionOutcome;
using openzoom::RecordingState;

class RecordingIntegrityTests : public QObject {
    Q_OBJECT

private slots:
    void exactNominalFrameDurations();
    void fractionalRateDoesNotDrift();
    void cameraTimestampsPreserveRealGaps();
    void missingAndBackwardTimestampsStayMonotonic();
    void dropCountsExcludeEstimatedTimestamps();
    void completionNeverSavesAfterFinalizeFailure();
    void stateMachineRejectsIllegalTransitions();
    void recordingCanvasModesAreFixedAndOrientationAware();
    void canvasResamplerPreservesIdentityAndFitBars();
    void negativeStrideRestoresTopDownRows();
};

void RecordingIntegrityTests::exactNominalFrameDurations()
{
    struct Case {
        std::uint32_t numerator;
        std::uint32_t denominator;
        std::int64_t expectedDuration;
    };
    constexpr std::array cases{
        Case{15, 1, 666'667},
        Case{30, 1, 333'333},
        Case{60, 1, 166'667},
        Case{30'000, 1'001, 333'667},
    };

    for (const Case& test : cases) {
        openzoom::RecordingTimeline timeline;
        timeline.Reset(test.numerator, test.denominator);
        QCOMPARE(timeline.NominalDuration100ns(),
                 test.expectedDuration);
    }
}

void RecordingIntegrityTests::fractionalRateDoesNotDrift()
{
    openzoom::RecordingTimeline timeline;
    timeline.Reset(30'000, 1'001);

    std::int64_t timestamp = -1;
    for (int frame = 0; frame <= 30'000; ++frame) {
        timestamp = timeline.MapTimestamp(-1);
    }

    QCOMPARE(timestamp, 10'010'000'000LL);
    QCOMPARE(timeline.UnknownTimestampCount(), 30'001ULL);
}

void RecordingIntegrityTests::cameraTimestampsPreserveRealGaps()
{
    openzoom::RecordingTimeline timeline;
    timeline.Reset(30, 1);
    constexpr std::int64_t sourceOrigin = 50'000'000;

    QCOMPARE(timeline.MapTimestamp(sourceOrigin), 0LL);
    QCOMPARE(timeline.MapTimestamp(sourceOrigin + 333'333), 333'333LL);
    QCOMPARE(timeline.MapTimestamp(sourceOrigin + 20'000'000),
             20'000'000LL);
}

void RecordingIntegrityTests::missingAndBackwardTimestampsStayMonotonic()
{
    openzoom::RecordingTimeline timeline;
    timeline.Reset(30, 1);

    QCOMPARE(timeline.MapTimestamp(-1), 0LL);
    const std::int64_t estimated = timeline.MapTimestamp(-1);
    QCOMPARE(estimated, 333'333LL);
    const std::int64_t firstKnown = timeline.MapTimestamp(8'000'000);
    QVERIFY(firstKnown > estimated);
    const std::int64_t backward = timeline.MapTimestamp(7'000'000);
    QCOMPARE(backward,
             firstKnown + timeline.NominalDuration100ns());
    QCOMPARE(timeline.UnknownTimestampCount(), 2ULL);
}

void RecordingIntegrityTests::dropCountsExcludeEstimatedTimestamps()
{
    const openzoom::RecordingDropCounts drops{
        1, 2, 3, 4, 5, 6, 99,
    };
    QCOMPARE(drops.Total(), 21ULL);
    QCOMPARE(drops.unknownTimestamp, 99ULL);
}

void RecordingIntegrityTests::completionNeverSavesAfterFinalizeFailure()
{
    QCOMPARE(openzoom::ClassifyRecordingCompletion(
                 true, true, true, false),
             RecordingCompletionOutcome::Saved);
    QCOMPARE(openzoom::ClassifyRecordingCompletion(
                 true, true, false, false),
             RecordingCompletionOutcome::FailedTruncated);
    QCOMPARE(openzoom::ClassifyRecordingCompletion(
                 true, true, true, true),
             RecordingCompletionOutcome::FailedTruncated);
    QCOMPARE(openzoom::ClassifyRecordingCompletion(
                 false, true, true, false),
             RecordingCompletionOutcome::FailedTruncated);
    QCOMPARE(openzoom::ClassifyRecordingCompletion(
                 true, false, true, false),
             RecordingCompletionOutcome::FailedBeforeFirstFrame);
}

void RecordingIntegrityTests::stateMachineRejectsIllegalTransitions()
{
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Idle, RecordingState::Starting));
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Starting, RecordingState::Recording));
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Recording, RecordingState::Stopping));
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Stopping, RecordingState::Finalizing));
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Finalizing, RecordingState::Completed));
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Failed, RecordingState::Starting));
    // The stop watchdog escapes a wedged worker by forcing Failed from
    // either non-terminal stopping state; both must stay legal.
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Stopping, RecordingState::Failed));
    QVERIFY(openzoom::IsValidRecordingStateTransition(
        RecordingState::Finalizing, RecordingState::Failed));

    QVERIFY(!openzoom::IsValidRecordingStateTransition(
        RecordingState::Idle, RecordingState::Completed));
    QVERIFY(!openzoom::IsValidRecordingStateTransition(
        RecordingState::Completed, RecordingState::Recording));
    QVERIFY(!openzoom::IsValidRecordingStateTransition(
        RecordingState::Finalizing, RecordingState::Recording));
}

void RecordingIntegrityTests::recordingCanvasModesAreFixedAndOrientationAware()
{
    QCOMPARE(openzoom::ResolveRecordingCanvas(
                 RecordingCanvasMode::Source, 1'280, 720).width,
             1'280U);
    QCOMPARE(openzoom::ResolveRecordingCanvas(
                 RecordingCanvasMode::Source, 1'280, 720).height,
             720U);

    const auto nhd = openzoom::ResolveRecordingCanvas(
        RecordingCanvasMode::Nhd360, 1'920, 1'080);
    QCOMPARE(nhd.width, 640U);
    QCOMPARE(nhd.height, 360U);

    const auto sd = openzoom::ResolveRecordingCanvas(
        RecordingCanvasMode::Sd480, 1'920, 1'080);
    QCOMPARE(sd.width, 854U);
    QCOMPARE(sd.height, 480U);

    const auto hd = openzoom::ResolveRecordingCanvas(
        RecordingCanvasMode::Hd720, 1'920, 1'080);
    QCOMPARE(hd.width, 1'280U);
    QCOMPARE(hd.height, 720U);

    const auto fullHd = openzoom::ResolveRecordingCanvas(
        RecordingCanvasMode::FullHd1080, 640, 480);
    QCOMPARE(fullHd.width, 1'920U);
    QCOMPARE(fullHd.height, 1'080U);

    const auto portrait = openzoom::ResolveRecordingCanvas(
        RecordingCanvasMode::QuadHd1440, 720, 1'280);
    QCOMPARE(portrait.width, 1'440U);
    QCOMPARE(portrait.height, 2'560U);

    const auto portrait360 = openzoom::ResolveRecordingCanvas(
        RecordingCanvasMode::Nhd360, 720, 1'280);
    QCOMPARE(portrait360.width, 360U);
    QCOMPARE(portrait360.height, 640U);

    const auto invalid = openzoom::ResolveRecordingCanvas(
        RecordingCanvasMode::UltraHd2160, 0, 1'080);
    QCOMPARE(invalid.width, 0U);
    QCOMPARE(invalid.height, 0U);
}

void RecordingIntegrityTests::canvasResamplerPreservesIdentityAndFitBars()
{
    const std::vector<std::uint8_t> source{
        10, 11, 12, 255, 20, 21, 22, 255,
        30, 31, 32, 255, 40, 41, 42, 255,
    };
    const openzoom::RecordingViewTransform identity{
        0.0f, 0.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f,
        true,
    };
    std::vector<std::uint8_t> destination;
    QVERIFY(openzoom::ResampleRecordingCanvas(
        source.data(), 2, 2, identity, 2, 2, destination));
    QCOMPARE(destination, source);

    const openzoom::RecordingViewTransform fit{
        0.0f, 0.0f, 1.0f, 1.0f,
        0.0f, 0.25f, 1.0f, 0.5f,
        true,
    };
    QVERIFY(openzoom::ResampleRecordingCanvas(
        source.data(), 2, 2, fit, 4, 4, destination));
    QCOMPARE(destination.size(), std::size_t{64});
    for (int x = 0; x < 4; ++x) {
        QCOMPARE(destination[static_cast<std::size_t>(x) * 4u + 3u],
                 std::uint8_t{0});
        QCOMPARE(destination[(12u + static_cast<std::size_t>(x)) * 4u + 3u],
                 std::uint8_t{0});
        QCOMPARE(destination[(4u + static_cast<std::size_t>(x)) * 4u + 3u],
                 std::uint8_t{255});
        QCOMPARE(destination[(8u + static_cast<std::size_t>(x)) * 4u + 3u],
                 std::uint8_t{255});
    }
}

void RecordingIntegrityTests::negativeStrideRestoresTopDownRows()
{
    constexpr UINT width = 2;
    constexpr UINT height = 2;
    constexpr LONG stride = 8;
    // A negative stride describes this storage as bottom row first.
    const std::vector<std::uint8_t> bottomUp{
        30, 0, 0, 255, 40, 0, 0, 255,
        10, 0, 0, 255, 20, 0, 0, 255,
    };

    openzoom::processing::CpuFramePipeline pipeline;
    QVERIFY(pipeline.ConvertFrameToBgra(
        bottomUp,
        MFVideoFormat_ARGB32,
        width,
        height,
        -stride,
        bottomUp.size()));
    const std::vector<std::uint8_t>& topDown = pipeline.StageRaw();
    QCOMPARE(topDown.size(), bottomUp.size());
    QCOMPARE(topDown[0], std::uint8_t{10});
    QCOMPARE(topDown[4], std::uint8_t{20});
    QCOMPARE(topDown[8], std::uint8_t{30});
    QCOMPARE(topDown[12], std::uint8_t{40});

    QVERIFY(!pipeline.ConvertFrameToBgra(
        bottomUp,
        MFVideoFormat_ARGB32,
        width,
        height,
        std::numeric_limits<LONG>::min(),
        bottomUp.size()));
}

} // namespace

QTEST_GUILESS_MAIN(RecordingIntegrityTests)

#include "recording_integrity_tests.moc"
