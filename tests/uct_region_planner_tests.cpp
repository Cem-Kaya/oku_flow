// Desktop-free shadow region planner contract fixture (roadmap Design B,
// stage 4). Pure metadata: scripted RecognitionResult values only, no OCR,
// pixels, camera, GPU or native windows. Root registers this file through
// tests/headless_cases.cmake (okuflow_add_headless_cases); every slot below
// is an independently selectable CTest case.

#include "okuflow/common/uct_region_planner.hpp"

#include <QByteArray>
#include <QDataStream>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace okuflow::uct;

namespace {

struct WordSpec {
    QString text;
    QRectF rect;
    TokenDisposition disposition{TokenDisposition::kCandidate};
    double confidence{0.99};
};

struct ResultSpec {
    std::uint64_t jobId{1};
    std::uint64_t authority{1};
    std::uint64_t sourceEpoch{1};
    std::uint64_t geometryGeneration{1};
    std::uint64_t frameSeq{1};
    std::int64_t timestampMs{1000};
    HoldDisplay holdDisplay{HoldDisplay::NotHeld};
    std::uint64_t holdAcquisition{};
    bool temporallySmoothed{};
    QVector<bool> currentInputMatches;
    bool ok{true};
    bool cancelled{};
    bool compositeInput{};
    bool inputFault{};
    bool backendPreparing{};
    bool validTransform{true};
    QVector<WordSpec> words; // one word per line, in order
};

RecognitionResult MakeResult(const ResultSpec& spec)
{
    RecognitionResult result;
    result.jobId = spec.jobId;
    result.authority = spec.authority;
    result.stamp.sourceEpoch = spec.sourceEpoch;
    result.stamp.geometryGeneration = spec.geometryGeneration;
    result.stamp.frameSeq = spec.frameSeq;
    result.stamp.timestampMs = spec.timestampMs;
    result.stamp.viewportWidth = 1920;
    result.stamp.viewportHeight = 1080;
    result.stamp.viewTransform.valid = spec.validTransform;
    result.stamp.viewTransform.sourceWidth = 1.0f;
    result.stamp.viewTransform.sourceHeight = 1.0f;
    result.stamp.viewTransform.destinationWidth = 1.0f;
    result.stamp.viewTransform.destinationHeight = 1.0f;
    result.stamp.holdDisplay = spec.holdDisplay;
    result.stamp.holdAcquisition = spec.holdAcquisition;
    result.stamp.temporallySmoothed = spec.temporallySmoothed;
    result.output.ok = spec.ok;
    result.output.cancelled = spec.cancelled;
    result.output.compositeInput = spec.compositeInput;
    result.output.inputFault = spec.inputFault;
    result.output.backendPreparing = spec.backendPreparing;
    result.currentInputMatches = spec.currentInputMatches;
    for (const WordSpec& word : spec.words) {
        RecognizedLine line;
        RecognizedWord recognized;
        recognized.text = word.text;
        recognized.boundsPx = QRectF(word.rect.x() * 1920, word.rect.y() * 1080,
                                     word.rect.width() * 1920, word.rect.height() * 1080);
        recognized.disposition = word.disposition;
        recognized.confidence = word.confidence;
        line.words.push_back(recognized);
        result.output.lines.push_back(line);
        PreparedLine prepared;
        prepared.sceneRect = word.rect;
        prepared.words.push_back(
            {word.text.trimmed(), word.rect, word.disposition, word.confidence,
             word.rect, std::nullopt});
        result.prepared.push_back(prepared);
    }
    return result;
}

// Serializes every planner-visible field so a test can prove Observe left
// its const input byte-identical.
QByteArray Serialize(const RecognitionResult& result)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << quint64(result.jobId) << quint64(result.authority)
           << quint64(result.stamp.sourceEpoch) << quint64(result.stamp.geometryGeneration)
           << quint64(result.stamp.frameSeq) << qint64(result.stamp.timestampMs)
           << quint8(result.stamp.holdDisplay) << quint64(result.stamp.holdAcquisition)
           << result.stamp.temporallySmoothed << result.output.ok
           << result.output.cancelled << result.output.compositeInput
           << result.output.inputFault << result.output.backendPreparing
           << result.output.error
           << result.currentInputMatches << qint32(result.output.lines.size())
           << qint32(result.prepared.size());
    const auto& transform = result.stamp.viewTransform;
    stream << quint32(result.stamp.viewportWidth) << quint32(result.stamp.viewportHeight)
           << transform.valid << transform.sourceX << transform.sourceY
           << transform.sourceWidth << transform.sourceHeight
           << transform.destinationX << transform.destinationY
           << transform.destinationWidth << transform.destinationHeight;
    for (const RecognizedLine& line : result.output.lines) {
        stream << qint32(line.words.size());
        for (const RecognizedWord& word : line.words)
            stream << word.text << word.boundsPx << word.protectedBoundsPx
                   << quint8(word.disposition) << word.confidence;
    }
    for (const PreparedLine& line : result.prepared) {
        stream << qint32(line.words.size());
        for (const PreparedWord& word : line.words) {
            stream << word.text << word.sceneRect << quint8(word.disposition)
                   << word.confidence << word.protectedSceneRect;
        }
    }
    return bytes;
}

RegionShadowLimits TestLimits()
{
    RegionShadowLimits limits;
    limits.gridColumns = 2;
    limits.gridRows = 2;
    limits.maxSelectionsPerPlan = 64;
    limits.reservedCoverageQuota = 6;
    return limits;
}

QVector<RegionPlannedSelection> OccurrenceSelections(const RegionShadowPlan& plan)
{
    QVector<RegionPlannedSelection> out;
    for (const RegionPlannedSelection& selection : plan.selections)
        if (!selection.isGridCell) out.push_back(selection);
    std::sort(out.begin(), out.end(), [](const RegionPlannedSelection& a,
                                         const RegionPlannedSelection& b) {
        return a.occurrenceId < b.occurrenceId;
    });
    return out;
}

const RegionPlannedSelection* FindAt(const QVector<RegionPlannedSelection>& selections,
                                     const QRectF& rect)
{
    for (const RegionPlannedSelection& selection : selections)
        if (selection.sceneRect == rect) return &selection;
    return nullptr;
}

constexpr QRectF kWordA{0.05, 0.05, 0.10, 0.04};
constexpr QRectF kWordB{0.60, 0.50, 0.10, 0.04};
constexpr QRectF kWordC{0.05, 0.60, 0.10, 0.04};

} // namespace

class UctRegionPlannerTests : public QObject {
    Q_OBJECT
private slots:
    void fullResultBytesUnchangedAndPlanAuthoritative()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("hello"), kWordA}, {QStringLiteral("world"), kWordB}};
        const RecognitionResult result = MakeResult(spec);
        const QByteArray before = Serialize(result);
        const RegionShadowPlan plan = planner.Observe(result, kWordA);
        QCOMPARE(Serialize(result), before);
        QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
        QCOMPARE(plan.authoritativeAction, RegionAuthoritativeAction::kRunFullOcr);
        QCOMPARE(plan.jobId, result.jobId);
        QCOMPARE(plan.authority, result.authority);
        QCOMPARE(plan.stamp.frameSeq, result.stamp.frameSeq);
        QVERIFY(plan.evidenceAcquisition.has_value());
        QCOMPARE(*plan.evidenceAcquisition, result.stamp.frameSeq);
        QCOMPARE(plan.comparison.fullCandidateWords, 2);
        QCOMPARE(plan.comparison.newOccurrences, 2);
        QVERIFY(plan.focusApplied);
        QVERIFY(plan.selections.size() <= plan.limits.maxSelectionsPerPlan);
    }

    void repeatedSameWordTwoPlacesStayDistinct()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("the"), kWordA}, {QStringLiteral("the"), kWordB}};
        const RegionShadowPlan first = planner.Observe(MakeResult(spec));
        const auto firstSelections = OccurrenceSelections(first);
        QCOMPARE(firstSelections.size(), 2);
        QVERIFY(firstSelections[0].occurrenceId != firstSelections[1].occurrenceId);

        // An exact same-job duplicate (same geometry and text) is the only
        // suppression; the two distinct occurrences both survive and match.
        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        spec.words.push_back({QStringLiteral("the"), kWordA});
        const RegionShadowPlan second = planner.Observe(MakeResult(spec));
        QCOMPARE(second.comparison.duplicatesSuppressed, 1);
        QCOMPARE(second.comparison.matchedOccurrences, 2);
        QCOMPARE(second.comparison.newOccurrences, 0);
        QCOMPARE(second.comparison.retainedOccurrences, 2);
        const auto secondSelections = OccurrenceSelections(second);
        const RegionPlannedSelection* atA = FindAt(secondSelections, kWordA);
        const RegionPlannedSelection* atB = FindAt(secondSelections, kWordB);
        QVERIFY(atA != nullptr && atB != nullptr);
        QCOMPARE(atA->occurrenceId, FindAt(firstSelections, kWordA)->occurrenceId);
        QCOMPARE(atB->occurrenceId, FindAt(firstSelections, kWordB)->occurrenceId);
        QCOMPARE(atA->status, RegionOccurrenceStatus::kStable);
        QCOMPARE(atB->observations, std::uint64_t{2});
    }

    void equalOverlapTiesKeepDeterministicOccurrences()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        for (int i=0;i<10;++i)
            spec.words.push_back({QStringLiteral("label%1").arg(i),kWordA});
        QCOMPARE(planner.Observe(MakeResult(spec)).comparison.newOccurrences,10);
        spec.jobId=2; spec.frameSeq=2; spec.timestampMs=1400;
        const auto plan=planner.Observe(MakeResult(spec));
        // More than eight equal spatial overlaps must keep the declared
        // word-order tie break, including the cap's unmatched occurrences.
        QCOMPARE(plan.comparison.matchedOccurrences,8);
        QCOMPARE(plan.comparison.missingOccurrences,2);
        QCOMPARE(plan.comparison.newOccurrences,2);
        const auto selections=OccurrenceSelections(plan);
        QCOMPARE(selections.size(),12);
        for (int i=0;i<12;++i) {
            QCOMPARE(selections[i].occurrenceId,std::uint64_t(i+1));
            const auto expected=i<8 ? RegionOccurrenceStatus::kStable
                : i<10 ? RegionOccurrenceStatus::kMissing : RegionOccurrenceStatus::kCandidate;
            QCOMPARE(selections[i].status,expected);
        }
    }

    void missingEvictionKeepsAgeThenIdOrder()
    {
        auto limits=TestLimits();
        limits.maxOccurrences=3;
        limits.maxMissingObservations=10;
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        spec.words={{QStringLiteral("first"),kWordA},{QStringLiteral("second"),kWordB},
                    {QStringLiteral("third"),kWordC}};
        QCOMPARE(planner.Observe(MakeResult(spec)).comparison.newOccurrences,3);
        spec.jobId=2; spec.frameSeq=2; spec.timestampMs=1400;
        spec.words={{QStringLiteral("first"),kWordA}};
        QCOMPARE(planner.Observe(MakeResult(spec)).comparison.missingOccurrences,2);
        const QRectF places[]={{.3,.7,.1,.04},{.5,.8,.1,.04},{.72,.85,.1,.04}};
        const std::uint64_t expectedIds[][3]={{1,3,4},{1,4,5},{4,5,6}};
        for (int step=0;step<3;++step) {
            spec.jobId=std::uint64_t(step+3); spec.frameSeq=spec.jobId;
            spec.timestampMs=1800+step*400;
            spec.words={{QStringLiteral("new%1").arg(step),places[step]}};
            const auto plan=planner.Observe(MakeResult(spec));
            QCOMPARE(plan.comparison.evictedOccurrences,1);
            QCOMPARE(plan.comparison.newOccurrences,1);
            const auto selections=OccurrenceSelections(plan);
            QCOMPARE(selections.size(),3);
            for (int i=0;i<3;++i)
                QCOMPARE(selections[i].occurrenceId,expectedIds[step][i]);
        }
    }

    void movementCreatesNewOccurrenceAndMissingAgesOut()
    {
        RegionShadowLimits limits = TestLimits();
        limits.maxMissingObservations = 2;
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        spec.words = {{QStringLiteral("move"), kWordA}};
        QCOMPARE(planner.Observe(MakeResult(spec)).comparison.newOccurrences, 1);

        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        spec.words = {{QStringLiteral("move"), kWordB}}; // same text, far away
        const RegionShadowPlan moved = planner.Observe(MakeResult(spec));
        QCOMPARE(moved.comparison.matchedOccurrences, 0);
        QCOMPARE(moved.comparison.newOccurrences, 1);
        QCOMPARE(moved.comparison.missingOccurrences, 1);
        QCOMPARE(moved.comparison.retainedOccurrences, 2);

        // The abandoned occurrence was last seen at observation 1, so with
        // maxMissingObservations = 2 it survives observations 2-3 and is
        // evicted at observation 4 (age 3 > 2).
        for (int step = 3; step <= 4; ++step) {
            spec.jobId = std::uint64_t(step);
            spec.frameSeq = std::uint64_t(step);
            spec.timestampMs = 1000 + step * 400;
            const RegionShadowPlan plan = planner.Observe(MakeResult(spec));
            if (step == 3) QCOMPARE(plan.comparison.retainedOccurrences, 2);
            if (step == 4) {
                QCOMPARE(plan.comparison.evictedOccurrences, 1);
                QCOMPARE(plan.comparison.retainedOccurrences, 1);
            }
        }
    }

    void editAtSamePlaceMarksChanged()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("cat"), kWordA}};
        planner.Observe(MakeResult(spec));
        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RegionShadowPlan stable = planner.Observe(MakeResult(spec));
        QCOMPARE(OccurrenceSelections(stable).front().status,
                 RegionOccurrenceStatus::kStable);

        spec.jobId = 3;
        spec.frameSeq = 3;
        spec.timestampMs = 1800;
        spec.words = {{QStringLiteral("bat"), kWordA}}; // one-token local edit
        const RegionShadowPlan edited = planner.Observe(MakeResult(spec));
        QCOMPARE(edited.comparison.changedOccurrences, 1);
        QCOMPARE(edited.comparison.newOccurrences, 0);
        QCOMPARE(OccurrenceSelections(edited).front().status,
                 RegionOccurrenceStatus::kChanged);
        QCOMPARE(OccurrenceSelections(edited).front().occurrenceId,
                 OccurrenceSelections(stable).front().occurrenceId);
    }

    void changedPreviouslyStableUnselectedIsCounted()
    {
        RegionShadowLimits limits = TestLimits();
        limits.gridColumns = 1;
        limits.gridRows = 1;
        limits.maxSelectionsPerPlan = 2;
        limits.reservedCoverageQuota = 1;
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        for (int i = 0; i < 6; ++i) {
            spec.words.push_back(
                {QStringLiteral("word%1").arg(i), {0.02 + i * 0.15, 0.05, 0.10, 0.04}});
        }
        planner.Observe(MakeResult(spec));
        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RegionShadowPlan stablePlan = planner.Observe(MakeResult(spec));
        int selectedOccurrences = 0;
        for (const RegionPlannedSelection& selection : stablePlan.selections)
            if (!selection.isGridCell) ++selectedOccurrences;
        QVERIFY(selectedOccurrences < 6);

        spec.jobId = 3;
        spec.frameSeq = 3;
        spec.timestampMs = 1800;
        for (WordSpec& word : spec.words) word.text += QStringLiteral("x");
        const RegionShadowPlan edited = planner.Observe(MakeResult(spec));
        QCOMPARE(edited.comparison.changedOccurrences, 6);
        // Every stable word changed; a hypothetical reuse policy would have
        // missed exactly those the previous plan had not selected.
        QCOMPARE(edited.comparison.changedPreviouslyStableUnselected,
                 6 - selectedOccurrences);
        QVERIFY(edited.comparison.changedPreviouslyStableUnselected > 0);
    }

    void focusPriorityAndReservedQuotaNeverStarve()
    {
        RegionShadowLimits limits = TestLimits();
        limits.maxSelectionsPerPlan = 3;
        limits.reservedCoverageQuota = 1;
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        for (int i = 0; i < 8; ++i) {
            spec.words.push_back({QStringLiteral("w%1").arg(i),
                                  {0.02 + (i % 4) * 0.24, 0.05 + (i / 4) * 0.4, 0.10, 0.04}});
        }
        QVector<std::uint64_t> everSelected;
        bool focusPrioritized = false;
        for (int step = 1; step <= 40; ++step) {
            spec.jobId = std::uint64_t(step);
            spec.frameSeq = std::uint64_t(step);
            spec.timestampMs = 1000 + step * 400;
            const RegionShadowPlan plan =
                planner.Observe(MakeResult(spec), spec.words.front().rect);
            QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
            QVERIFY(plan.selections.size() <= 3);
            QCOMPARE(plan.coverage.coverageDebt,
                     plan.coverage.eligibleRegions - plan.coverage.selectedRegions);
            QCOMPARE(plan.coverage.coverageDeadlineObservation,
                     plan.coverage.acceptedObservations + plan.coverage.worstCasePlansToCoverAll);
            for (const RegionPlannedSelection& selection : plan.selections) {
                if (selection.isGridCell) continue;
                if (selection.sceneRect == spec.words.front().rect &&
                    selection.priority == RegionPriorityClass::kRequestedFocus)
                    focusPrioritized = true;
                if (!everSelected.contains(selection.occurrenceId))
                    everSelected.push_back(selection.occurrenceId);
            }
        }
        QVERIFY(focusPrioritized);
        QCOMPARE(int(everSelected.size()), 8); // constant focus starved nobody
    }

    void initialCellsServicedUnderChurnAndConstantFocus()
    {
        // Regression for age-anchor starvation: never-selected fresh
        // occurrences must not permanently outrank the initial grid cells
        // in the reserved quota. Every frame mints brand-new occurrences
        // at shifted geometry while the requested focus never moves.
        RegionShadowLimits limits = TestLimits();
        limits.gridColumns = 4;
        limits.gridRows = 4; // 16 initial cells
        limits.maxSelectionsPerPlan = 3;
        limits.reservedCoverageQuota = 1; // weakest possible reserved drain
        limits.staleCellAfterObservations = 1000; // serviced cells stay done
        limits.maxMissingObservations = 2;        // churn cohorts age out
        RegionShadowPlanner planner(limits);

        QVector<int> servicedCells;
        std::uint64_t promisedDeadline = 0;
        int completedAtStep = -1;
        for (int step = 1; step <= 64; ++step) {
            ResultSpec spec;
            spec.jobId = std::uint64_t(step);
            spec.frameSeq = std::uint64_t(step);
            spec.timestampMs = 1000 + step * 400;
            spec.words.push_back({QStringLiteral("focus"), kWordA});
            for (int i = 0; i < 4; ++i) {
                // Shifted by more than the word width every frame, so IoU
                // never matches the previous cohort: always new identities.
                const double x = 0.30 + 0.07 * double((step + i * 3) % 9);
                const double y = 0.08 + 0.20 * double(i);
                spec.words.push_back({QStringLiteral("churn%1_%2").arg(step).arg(i),
                                      {x, y, 0.04, 0.04}});
            }
            const RegionShadowPlan plan = planner.Observe(MakeResult(spec), kWordA);
            QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
            QVERIFY(plan.focusApplied);
            if (step == 1) {
                promisedDeadline = plan.coverage.coverageDeadlineObservation;
                QVERIFY(promisedDeadline > 1);
                QVERIFY(plan.coverage.coverageDebt > 0);
            }
            for (const RegionPlannedSelection& selection : plan.selections) {
                if (selection.isGridCell && !servicedCells.contains(selection.cellIndex))
                    servicedCells.push_back(selection.cellIndex);
            }
            if (servicedCells.size() == 16) {
                completedAtStep = step;
                break;
            }
        }
        QCOMPARE(int(servicedCells.size()), 16);
        QVERIFY(completedAtStep > 0);
        // The first plan's stated finite deadline must actually bind the
        // initial cells despite unbounded later churn membership turnover.
        QVERIFY(std::uint64_t(completedAtStep) <= promisedDeadline);
    }

    void zeroStaleCellCannotRejoinAheadOfPromisedDebt()
    {
        RegionShadowLimits limits = TestLimits();
        limits.gridColumns = limits.gridRows = 1;
        limits.maxSelectionsPerPlan = limits.reservedCoverageQuota = 1;
        limits.staleCellAfterObservations = 0; // selected cells are immediately due
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        spec.words = {{QStringLiteral("alpha"), kWordA},
                      {QStringLiteral("beta"), kWordB}};
        const RegionShadowPlan first = planner.Observe(MakeResult(spec));
        QCOMPARE(first.selections.size(), 1);
        QVERIFY(first.selections.front().isGridCell);
        QCOMPARE(first.coverage.coverageDebt, 2);
        QCOMPARE(first.coverage.coverageDeadlineObservation, std::uint64_t{3});

        QVector<std::uint64_t> selectedWords;
        for (int step = 2; step <= 3; ++step) {
            spec.jobId = spec.frameSeq = std::uint64_t(step);
            spec.timestampMs = 1000 + step * 400;
            const RegionShadowPlan plan = planner.Observe(MakeResult(spec), kWordA);
            QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
            QCOMPARE(plan.selections.size(), 1);
            QVERIFY(!plan.selections.front().isGridCell);
            const std::uint64_t id = plan.selections.front().occurrenceId;
            QVERIFY(!selectedWords.contains(id));
            selectedWords.push_back(id);
        }
        QCOMPARE(selectedWords.size(), 2); // both serviced by the original deadline
    }

    void staleAndOutOfOrderTicketsRejected()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.jobId = 5;
        spec.frameSeq = 10;
        spec.words = {{QStringLiteral("anchor"), kWordA}};
        const RegionShadowPlan accepted = planner.Observe(MakeResult(spec));
        QCOMPARE(accepted.status, RegionPlanStatus::kAccepted);
        const std::uint64_t anchorId = OccurrenceSelections(accepted).front().occurrenceId;
        std::uint64_t generation = accepted.planGeneration;

        const auto expectReject = [&](const ResultSpec& bad, RegionRejectReason reason) {
            const RegionShadowPlan plan = planner.Observe(MakeResult(bad));
            QCOMPARE(plan.status, RegionPlanStatus::kRejected);
            QCOMPARE(plan.rejectReason, reason);
            QVERIFY(plan.selections.isEmpty());
            QVERIFY(plan.planGeneration > generation); // generations stay monotonic
            generation = plan.planGeneration;
            QCOMPARE(planner.RetainedOccurrences(), 1); // state untouched
        };
        ResultSpec bad = spec;
        bad.frameSeq = 11;
        bad.timestampMs = 1400; // same job id 5 again
        expectReject(bad, RegionRejectReason::kStaleOrOutOfOrderJob);
        bad = spec;
        bad.jobId = 4;
        bad.frameSeq = 11;
        bad.timestampMs = 1400;
        expectReject(bad, RegionRejectReason::kStaleOrOutOfOrderJob);
        bad = spec;
        bad.jobId = 6;
        bad.frameSeq = 11;
        bad.timestampMs = 1400;
        bad.authority = 0;
        expectReject(bad, RegionRejectReason::kMissingJobTicket);
        bad = spec;
        bad.jobId = 6;
        bad.frameSeq = 11;
        bad.timestampMs = 900; // time ran backwards within the same scene
        expectReject(bad, RegionRejectReason::kOutOfOrderTimestamp);

        spec.jobId = 6;
        spec.frameSeq = 11;
        spec.timestampMs = 1400;
        const RegionShadowPlan next = planner.Observe(MakeResult(spec));
        QCOMPARE(next.status, RegionPlanStatus::kAccepted);
        QCOMPARE(OccurrenceSelections(next).front().occurrenceId, anchorId);
    }

    void staleAuthoritySourceGeometryRejected()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.authority = 3;
        spec.sourceEpoch = 4;
        spec.geometryGeneration = 5;
        spec.words = {{QStringLiteral("anchor"), kWordA}};
        QCOMPARE(planner.Observe(MakeResult(spec)).status, RegionPlanStatus::kAccepted);

        ResultSpec bad = spec;
        bad.jobId = 2;
        bad.frameSeq = 2;
        bad.timestampMs = 1400;
        bad.authority = 2;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kStaleAuthority);
        bad = spec;
        bad.jobId = 2;
        bad.frameSeq = 2;
        bad.timestampMs = 1400;
        bad.sourceEpoch = 3;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kStaleSource);
        bad = spec;
        bad.jobId = 2;
        bad.frameSeq = 2;
        bad.timestampMs = 1400;
        bad.geometryGeneration = 4;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kStaleGeometry);
        QCOMPARE(planner.RetainedOccurrences(), 1);
        QCOMPARE(planner.Stats().rejectedObservations, std::uint64_t{3});
    }

    void advancingSourceGeometryAuthorityResets()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("anchor"), kWordA}};
        const std::uint64_t firstId =
            OccurrenceSelections(planner.Observe(MakeResult(spec))).front().occurrenceId;
        const std::uint64_t firstEpoch = planner.PlannerEpoch();

        spec.jobId = 2;
        spec.frameSeq = 1; // new source restarts frame numbering
        spec.timestampMs = 1400;
        spec.sourceEpoch = 2;
        const RegionShadowPlan sourcePlan = planner.Observe(MakeResult(spec));
        QCOMPARE(sourcePlan.status, RegionPlanStatus::kAccepted);
        QCOMPARE(sourcePlan.resetReason, RegionResetReason::kSourceChanged);
        QVERIFY(planner.PlannerEpoch() > firstEpoch);
        const std::uint64_t secondId = OccurrenceSelections(sourcePlan).front().occurrenceId;
        QVERIFY(secondId > firstId); // identities are never reused

        spec.jobId = 3;
        spec.frameSeq = 2;
        spec.timestampMs = 1800;
        spec.geometryGeneration = 2;
        QCOMPARE(planner.Observe(MakeResult(spec)).resetReason,
                 RegionResetReason::kGeometryChanged);
        spec.jobId = 4;
        spec.frameSeq = 3;
        spec.timestampMs = 2200;
        spec.authority = 2;
        QCOMPARE(planner.Observe(MakeResult(spec)).resetReason,
                 RegionResetReason::kAuthorityChanged);
    }

    void automaticResetCannotReplayStaleTickets()
    {
        // An epoch bump must not launder a replayed ticket into a fresh
        // scene: every monotonic check runs before the automatic reset,
        // and a rejected bump leaves prior tracking completely intact.
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.jobId = 5;
        spec.authority = 3;
        spec.sourceEpoch = 4;
        spec.geometryGeneration = 5;
        spec.frameSeq = 10;
        spec.timestampMs = 5000;
        spec.words = {{QStringLiteral("anchor"), kWordA}};
        const RegionShadowPlan accepted = planner.Observe(MakeResult(spec));
        QCOMPARE(accepted.status, RegionPlanStatus::kAccepted);
        const std::uint64_t epochBefore = planner.PlannerEpoch();
        const std::uint64_t resetsBefore = planner.Stats().resets;

        ResultSpec bad = spec;
        bad.sourceEpoch = 5; // source advance replaying the same job
        bad.frameSeq = 1;
        bad.timestampMs = 5400;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kStaleOrOutOfOrderJob);
        bad = spec;
        bad.jobId = 6;
        bad.geometryGeneration = 6; // same-source advance, duplicate evidence
        bad.timestampMs = 5400;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kDuplicateAcquisition);
        bad = spec;
        bad.jobId = 6;
        bad.authority = 4; // authority advance with time running backwards
        bad.frameSeq = 11;
        bad.timestampMs = 4000;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kOutOfOrderTimestamp);
        QCOMPARE(planner.PlannerEpoch(), epochBefore); // no reset happened
        QCOMPARE(planner.Stats().resets, resetsBefore);
        QCOMPARE(planner.RetainedOccurrences(), 1);

        // The genuine advance still works afterwards, and only the source
        // advance is allowed to restart acquisition numbering.
        spec.jobId = 6;
        spec.sourceEpoch = 5;
        spec.frameSeq = 1;
        spec.timestampMs = 5400;
        const RegionShadowPlan advanced = planner.Observe(MakeResult(spec));
        QCOMPARE(advanced.status, RegionPlanStatus::kAccepted);
        QCOMPARE(advanced.resetReason, RegionResetReason::kSourceChanged);
        QVERIFY(planner.PlannerEpoch() > epochBefore);
    }

    void seekNeedsExplicitResetAndMintsFreshIds()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.frameSeq = 100;
        spec.timestampMs = 5000;
        spec.words = {{QStringLiteral("later"), kWordA}};
        const std::uint64_t beforeSeekId =
            OccurrenceSelections(planner.Observe(MakeResult(spec))).front().occurrenceId;

        spec.jobId = 2;
        spec.frameSeq = 40; // rewound without telling the planner
        spec.timestampMs = 5400;
        QCOMPARE(planner.Observe(MakeResult(spec)).rejectReason,
                 RegionRejectReason::kOutOfOrderAcquisition);

        planner.Reset(RegionResetReason::kSeek);
        spec.jobId = 3;
        const RegionShadowPlan afterSeek = planner.Observe(MakeResult(spec));
        QCOMPARE(afterSeek.status, RegionPlanStatus::kAccepted);
        QCOMPARE(afterSeek.resetReason, RegionResetReason::kSeek);
        QVERIFY(OccurrenceSelections(afterSeek).front().occurrenceId > beforeSeekId);
    }

    void heldDuplicateAndCompositeNeverRenewEvidence()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.holdDisplay = HoldDisplay::Held;
        spec.holdAcquisition = 10;
        spec.frameSeq = 20;
        spec.words = {{QStringLiteral("held"), kWordA}};
        const RegionShadowPlan held = planner.Observe(MakeResult(spec));
        QCOMPARE(held.status, RegionPlanStatus::kAccepted);
        QCOMPARE(*held.evidenceAcquisition, std::uint64_t{10});

        // Same held acquisition presented again (new job, later time, even
        // after a pan expressed only through the stamp transform) is not
        // fresh evidence and must not advance any evidence clock.
        spec.jobId = 2;
        spec.frameSeq = 21;
        spec.timestampMs = 1400;
        RecognitionResult panned = MakeResult(spec);
        panned.stamp.viewTransform.sourceX = 0.25f;
        // Keep preparation parallel to the panned job's own mapping. The
        // duplicate acquisition, rather than malformed preparation, rejects it.
        panned.prepared[0].words[0].sceneRect.translate(0.25, 0.0);
        panned.prepared[0].words[0].protectedSceneRect.translate(0.25, 0.0);
        const RegionShadowPlan duplicate = planner.Observe(panned);
        QCOMPARE(duplicate.rejectReason, RegionRejectReason::kDuplicateAcquisition);
        QCOMPARE(duplicate.coverage.acceptedObservations, std::uint64_t{1});

        spec.jobId = 3;
        spec.frameSeq = 22;
        spec.timestampMs = 1800;
        spec.holdDisplay = HoldDisplay::Crossfade;
        QCOMPARE(planner.Observe(MakeResult(spec)).rejectReason,
                 RegionRejectReason::kUnknownAcquisition);
        spec.holdDisplay = HoldDisplay::NotHeld;
        spec.compositeInput = true;
        spec.ok = false;
        QCOMPARE(planner.Observe(MakeResult(spec)).rejectReason,
                 RegionRejectReason::kCompositeInput);

        spec.jobId = 4;
        spec.compositeInput = false;
        spec.ok = true;
        const RegionShadowPlan live = planner.Observe(MakeResult(spec));
        QCOMPARE(live.status, RegionPlanStatus::kAccepted);
        // The held repeat contributed nothing: this is observation 2, not 3.
        QCOMPARE(OccurrenceSelections(live).front().observations, std::uint64_t{2});
    }

    void malformedResultsAndWordsFailClosed()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("ok"), kWordA}};

        RecognitionResult unprepared = MakeResult(spec);
        unprepared.prepared.clear();
        QCOMPARE(planner.Observe(unprepared).rejectReason,
                 RegionRejectReason::kUnpreparedLines);

        ResultSpec bad = spec;
        bad.ok = false;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kResultNotOk);
        bad = spec;
        bad.cancelled = true;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kResultCancelled);
        bad = spec;
        bad.validTransform = false;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kInvalidStamp);
        bad = spec;
        bad.jobId = 0;
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kMissingJobTicket);
        QCOMPARE(planner.RetainedOccurrences(), 0);

        // Real preparation preserves malformed spans as invalid-geometry
        // omissions, while a valid edge box is clamped only by the observer.
        spec.words.push_back({QStringLiteral("nan"),
                              {std::nan(""), 0.5, 0.1, 0.04},
                              TokenDisposition::kInvalidGeometry});
        spec.words.push_back({QStringLiteral("edge"), {-0.05, 0.60, 0.10, 0.04}});
        spec.words.push_back({QStringLiteral("risky"), kWordC,
                              TokenDisposition::kHighRisk, 0.99});
        const RegionShadowPlan plan = planner.Observe(MakeResult(spec));
        QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
        QCOMPARE(plan.comparison.invalidWords, 0);
        QCOMPARE(plan.comparison.clampedWords, 1);
        QCOMPARE(plan.comparison.fullOmittedWords, 2);
        QCOMPARE(plan.comparison.retainedOccurrences, 2); // ok + clamped edge
        const auto selections = OccurrenceSelections(plan);
        bool sawClamped = false;
        for (const RegionPlannedSelection& selection : selections) {
            // Stored rect is the normalized intersection with the scene.
            if (std::abs(selection.sceneRect.y() - 0.60) < 1e-9)
                sawClamped = selection.sceneRect.x() == 0.0 &&
                             std::abs(selection.sceneRect.width() - 0.05) < 1e-9;
        }
        QVERIFY(sawClamped);
    }

    void faultPreparingAndNonParallelWordsReject()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("anchor"), kWordA}};
        QCOMPARE(planner.Observe(MakeResult(spec)).status, RegionPlanStatus::kAccepted);

        // ok cannot rehabilitate faulted input or a preparing backend.
        ResultSpec bad = spec;
        bad.jobId = 2;
        bad.frameSeq = 2;
        bad.timestampMs = 1400;
        bad.inputFault = true; // output.ok stays true
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kResultNotOk);
        bad = spec;
        bad.jobId = 2;
        bad.frameSeq = 2;
        bad.timestampMs = 1400;
        bad.backendPreparing = true; // output.ok stays true
        QCOMPARE(planner.Observe(MakeResult(bad)).rejectReason,
                 RegionRejectReason::kResultNotOk);

        // Prepared words must be parallel to output words, line by line.
        bad = spec;
        bad.jobId = 2;
        bad.frameSeq = 2;
        bad.timestampMs = 1400;
        RecognitionResult nonParallel = MakeResult(bad);
        nonParallel.prepared[0].words.push_back(
            {QStringLiteral("ghost"), kWordB, TokenDisposition::kCandidate, 0.99,
             kWordB, std::nullopt});
        QCOMPARE(planner.Observe(nonParallel).rejectReason,
                 RegionRejectReason::kUnpreparedLines);

        RecognitionResult badTransform = MakeResult(bad);
        badTransform.stamp.viewTransform.sourceWidth =
            std::numeric_limits<float>::quiet_NaN();
        QCOMPARE(planner.Observe(badTransform).rejectReason,
                 RegionRejectReason::kInvalidStamp);
        QCOMPARE(planner.RetainedOccurrences(), 1); // nothing above touched state

        const RegionShadowPlan accepted = planner.Observe(MakeResult(bad));
        QCOMPARE(accepted.status, RegionPlanStatus::kAccepted);
        QCOMPARE(accepted.comparison.matchedOccurrences, 1);
    }

    void nonPositiveStampExtentsAndEmptyViewportReject()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("anchor"), kWordA}};
        const RegionShadowPlan first = planner.Observe(MakeResult(spec));
        const std::uint64_t id = OccurrenceSelections(first).front().occurrenceId;
        const std::uint64_t epoch = planner.PlannerEpoch();
        spec.jobId = spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RecognitionResult valid = MakeResult(spec);
        const auto expectInvalid = [&](RecognitionResult bad) {
            // An invalid stamp must not clear an established scene even
            // when its authority/source ticket would otherwise reset it.
            bad.authority = 2;
            bad.stamp.sourceEpoch = 2;
            const RegionShadowPlan rejected = planner.Observe(bad);
            QCOMPARE(rejected.rejectReason, RegionRejectReason::kInvalidStamp);
            QCOMPARE(rejected.coverage.acceptedObservations, std::uint64_t{1});
            QCOMPARE(planner.PlannerEpoch(), epoch);
            QCOMPARE(planner.RetainedOccurrences(), 1);
        };
        float okuflow::ViewTransform::* const extents[] = {
            &okuflow::ViewTransform::sourceWidth, &okuflow::ViewTransform::sourceHeight,
            &okuflow::ViewTransform::destinationWidth,
            &okuflow::ViewTransform::destinationHeight};
        for (const auto extent : extents) {
            for (const float value : {0.0f, -1.0f}) {
                RecognitionResult bad = valid;
                bad.stamp.viewTransform.*extent = value;
                expectInvalid(bad);
            }
        }
        RecognitionResult bad = valid;
        bad.stamp.viewportWidth = 0;
        expectInvalid(bad);
        bad = valid;
        bad.stamp.viewportHeight = 0;
        expectInvalid(bad);

        const RegionShadowPlan second = planner.Observe(valid);
        QCOMPARE(second.status, RegionPlanStatus::kAccepted);
        QCOMPARE(OccurrenceSelections(second).front().occurrenceId, id);
        QCOMPARE(OccurrenceSelections(second).front().observations, std::uint64_t{2});
    }

    void sameCountPreparedMismatchCannotRenewEvidence()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("alpha"), kWordA},
                      {QStringLiteral("beta"), kWordB}};
        const RegionShadowPlan first = planner.Observe(MakeResult(spec));
        const auto firstWords = OccurrenceSelections(first);
        const std::uint64_t epoch = planner.PlannerEpoch();
        spec.jobId = spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RecognitionResult valid = MakeResult(spec);
        const auto expectMismatch = [&](const RecognitionResult& bad) {
            const RegionShadowPlan rejected = planner.Observe(bad);
            QCOMPARE(rejected.status, RegionPlanStatus::kRejected);
            QCOMPARE(rejected.rejectReason, RegionRejectReason::kUnpreparedLines);
            QCOMPARE(rejected.coverage.acceptedObservations, std::uint64_t{1});
            QCOMPARE(planner.PlannerEpoch(), epoch);
            QCOMPARE(planner.RetainedOccurrences(), 2);
        };
        RecognitionResult bad = valid;
        bad.output.lines[0].words[0].text = QStringLiteral("edited");
        expectMismatch(bad); // stale prepared text would falsely stabilize alpha
        bad = valid;
        std::swap(bad.prepared[0].words[0], bad.prepared[1].words[0]);
        expectMismatch(bad); // same counts do not establish word identity
        bad = valid;
        bad.prepared[0].words[0].confidence = 0.98;
        expectMismatch(bad);
        bad = valid;
        bad.prepared[0].words[0].sceneRect.translate(0.01, 0.0);
        expectMismatch(bad);
        bad = valid;
        bad.prepared[0].words[0].sceneRect = {0.15, 0.05, -0.10, 0.04};
        expectMismatch(bad); // preparation has already normalized orientation
        bad = valid;
        bad.prepared[0].words[0].sceneRect.setX(std::numeric_limits<double>::quiet_NaN());
        expectMismatch(bad);
        bad = valid;
        bad.prepared[0].words[0].protectedSceneRect.translate(0.01, 0.0);
        expectMismatch(bad);
        bad = valid;
        bad.output.lines[0].words[0].disposition = TokenDisposition::kHighRisk;
        expectMismatch(bad); // source omission cannot become a candidate
        bad = valid;
        bad.stamp.sourceEpoch = 2;
        bad.prepared[0].words[0].text = QStringLiteral("stale");
        expectMismatch(bad); // mismatch is checked before automatic reset

        const RegionShadowPlan second = planner.Observe(valid);
        QCOMPARE(second.status, RegionPlanStatus::kAccepted);
        QCOMPARE(second.coverage.acceptedObservations, std::uint64_t{2});
        const auto secondWords = OccurrenceSelections(second);
        QCOMPARE(secondWords.size(), firstWords.size());
        for (int i = 0; i < secondWords.size(); ++i) {
            QCOMPARE(secondWords[i].occurrenceId, firstWords[i].occurrenceId);
            QCOMPARE(secondWords[i].observations, std::uint64_t{2});
            QCOMPARE(secondWords[i].status, RegionOccurrenceStatus::kStable);
        }
    }

    void preparationMappingTrimmingAndOmissionsRemainParallel()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("  alpha  "), kWordA},
                      {QStringLiteral("small"), kWordB},
                      {QStringLiteral("weak"), kWordC, TokenDisposition::kLowConfidence,
                       std::numeric_limits<double>::quiet_NaN()}};
        RecognitionResult result = MakeResult(spec);
        result.stamp.viewportWidth = 960;
        result.stamp.viewportHeight = 540;
        auto& transform = result.stamp.viewTransform;
        transform.sourceX = 0.2f;
        transform.sourceY = 0.1f;
        transform.sourceWidth = 0.6f;
        transform.sourceHeight = 0.8f;
        transform.destinationX = transform.destinationY = 0.1f;
        transform.destinationWidth = transform.destinationHeight = 0.8f;
        // Geometry belongs to this job extent and letterboxed transform,
        // not the dimensions of an independently decimated reference.
        result.reference.stamp.viewportWidth = 96;
        result.reference.stamp.viewportHeight = 54;
        result.output.lines[0].words[0].boundsPx = {-480.0, 160.0, 960.0, 80.0};
        result.output.lines[0].words[0].protectedBoundsPx = {-481.0, 159.0, 962.0, 82.0};
        for (int i = 0; i < result.output.lines.size(); ++i) {
            const auto& source = result.output.lines[i].words[0];
            auto& prepared = result.prepared[i].words[0];
            prepared.sceneRect = ViewRectToScene(transform, 960, 540, source.boundsPx);
            prepared.protectedSceneRect = ViewRectToScene(transform, 960, 540,
                source.protectedBoundsPx.isEmpty() ? source.boundsPx : source.protectedBoundsPx);
        }
        // A scheduler-specific minimum height may conservatively omit a
        // candidate. The observer must not guess that threshold.
        result.prepared[1].words[0].disposition = TokenDisposition::kTooSmall;
        const QByteArray before = Serialize(result);
        const RegionShadowPlan plan = planner.Observe(result);
        QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
        QCOMPARE(Serialize(result), before);
        QCOMPARE(plan.comparison.fullCandidateWords, 1);
        QCOMPARE(plan.comparison.fullOmittedWords, 2);
        QCOMPARE(plan.comparison.clampedWords, 1);
        QCOMPARE(plan.comparison.retainedOccurrences, 1);
        const auto words = OccurrenceSelections(plan);
        QCOMPARE(words.size(), 1);
        QCOMPARE(words.front().sceneRect.x(), 0.0);
    }

    void lineAndWordLimitsRejectWholeResult()
    {
        RegionShadowLimits limits = TestLimits();
        limits.maxLines = 2;
        limits.maxWords = 3;
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        spec.words = {{QStringLiteral("a"), kWordA},
                      {QStringLiteral("b"), kWordB},
                      {QStringLiteral("c"), kWordC}};
        QCOMPARE(planner.Observe(MakeResult(spec)).rejectReason,
                 RegionRejectReason::kLineLimitExceeded);

        RecognitionResult manyWords;
        manyWords = MakeResult([] {
            ResultSpec two;
            two.words = {{QStringLiteral("a"), kWordA}, {QStringLiteral("b"), kWordB}};
            return two;
        }());
        PreparedWord extra{QStringLiteral("extra"), kWordC,
                           TokenDisposition::kCandidate, 0.99, kWordC, std::nullopt};
        PreparedWord extra2{QStringLiteral("more"), {0.4, 0.6, 0.1, 0.04},
                            TokenDisposition::kCandidate, 0.99,
                            {0.4, 0.6, 0.1, 0.04}, std::nullopt};
        manyWords.prepared[0].words.push_back(extra);
        manyWords.prepared[0].words.push_back(extra2); // 4 words in 2 lines
        // Keep the result word-parallel so the word limit, not the
        // prepared/output structure check, is what rejects it.
        for (const PreparedWord& word : {extra, extra2}) {
            RecognizedWord recognized;
            recognized.text = word.text;
            recognized.disposition = word.disposition;
            recognized.confidence = word.confidence;
            manyWords.output.lines[0].words.push_back(recognized);
        }
        QCOMPARE(planner.Observe(manyWords).rejectReason,
                 RegionRejectReason::kWordLimitExceeded);
        QCOMPARE(planner.RetainedOccurrences(), 0);
    }

    void observerTextLimitsAcceptExactBoundaries()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        const QString text(4096, QChar('a'));
        for (int i = 0; i < 256; ++i)
            spec.words.push_back({text,
                {0.01 + (i % 16) / 16.0, 0.01 + (i / 16) / 16.0, 0.02, 0.02}});
        // Both representations independently contain exactly 1 Mi UTF-16
        // units. A combined budget would incorrectly halve useful capacity.
        const RecognitionResult result = MakeResult(spec);
        const QByteArray before = Serialize(result);
        const RegionShadowPlan plan = planner.Observe(result);
        QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
        QCOMPARE(plan.limits.maxWordUtf16Units, 4096);
        QCOMPARE(plan.limits.maxTotalUtf16Units, 1 << 20);
        QCOMPARE(plan.comparison.fullCandidateWords, 256);
        QCOMPARE(plan.comparison.retainedOccurrences, 256);
        QCOMPARE(plan.authoritativeAction, RegionAuthoritativeAction::kRunFullOcr);
        QCOMPARE(Serialize(result), before);
    }

    void oversizeRawAndPreparedTextRejectBeforePreparation()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("anchor"), kWordA}};
        const RegionShadowPlan first = planner.Observe(MakeResult(spec));
        const std::uint64_t id = OccurrenceSelections(first).front().occurrenceId;
        const std::uint64_t epoch = planner.PlannerEpoch();
        spec.jobId = spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RecognitionResult valid = MakeResult(spec);
        const auto expectTextLimit = [&](const RecognitionResult& bad) {
            const QByteArray before = Serialize(bad);
            const RegionShadowPlan rejected = planner.Observe(bad);
            QCOMPARE(rejected.rejectReason, RegionRejectReason::kTextLimitExceeded);
            QCOMPARE(rejected.coverage.acceptedObservations, std::uint64_t{1});
            QCOMPARE(rejected.authoritativeAction, RegionAuthoritativeAction::kRunFullOcr);
            QCOMPARE(planner.PlannerEpoch(), epoch);
            QCOMPARE(planner.RetainedOccurrences(), 1);
            QCOMPARE(Serialize(bad), before);
        };
        RecognitionResult bad = valid;
        bad.output.lines[0].words[0].text = QString(4097, QChar(' '));
        expectTextLimit(bad); // reject raw size before trim or identity comparison
        bad = valid;
        bad.prepared[0].words[0].text = QString(4097, QChar('x'));
        expectTextLimit(bad); // malformed prepared text has its own size budget
        bad = valid;
        bad.stamp.sourceEpoch = 2;
        bad.authority = 2;
        bad.output.lines[0].words[0].text = QString(4097, QChar('x'));
        bad.output.lines[0].words[0].disposition = TokenDisposition::kHighRisk;
        bad.prepared[0].words[0].disposition = TokenDisposition::kHighRisk;
        expectTextLimit(bad); // omissions and would-be resets cannot bypass caps

        const RegionShadowPlan second = planner.Observe(valid);
        QCOMPARE(second.status, RegionPlanStatus::kAccepted);
        QCOMPARE(OccurrenceSelections(second).front().occurrenceId, id);
        QCOMPARE(OccurrenceSelections(second).front().observations, std::uint64_t{2});
    }

    void aggregateTextBudgetsIncludeOmittedWords()
    {
        RegionShadowLimits limits = TestLimits();
        limits.maxWordUtf16Units = 8;
        limits.maxTotalUtf16Units = 16;
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        for (int i = 0; i < 4; ++i)
            spec.words.push_back({QStringLiteral(" aa "), kWordA,
                                  TokenDisposition::kLowConfidence, 0.5});
        // Raw text totals exactly 16; trimmed prepared text totals 8.
        const RegionShadowPlan first = planner.Observe(MakeResult(spec));
        QCOMPARE(first.status, RegionPlanStatus::kAccepted);
        QCOMPARE(first.comparison.fullOmittedWords, 4);
        QCOMPARE(first.comparison.retainedOccurrences, 0);
        spec.jobId = spec.frameSeq = 2;
        spec.timestampMs = 1400;
        ResultSpec oversizedRaw = spec;
        for (auto& word : oversizedRaw.words) word.text = QStringLiteral("  aa ");
        const RegionShadowPlan rawRejected = planner.Observe(MakeResult(oversizedRaw));
        // No individual word exceeds eight units; the raw aggregate is 20
        // while its prepared aggregate remains eight.
        QCOMPARE(rawRejected.rejectReason, RegionRejectReason::kTextLimitExceeded);
        QCOMPARE(rawRejected.coverage.acceptedObservations, std::uint64_t{1});

        RecognitionResult oversizedPrepared = MakeResult(spec);
        for (auto& line : oversizedPrepared.prepared)
            line.words[0].text = QStringLiteral("aaaaa");
        const RegionShadowPlan preparedRejected = planner.Observe(oversizedPrepared);
        // Prepared aggregate is 20; raw aggregate is still exactly 16.
        // Its independent size guard precedes the text-parity rejection.
        QCOMPARE(preparedRejected.rejectReason, RegionRejectReason::kTextLimitExceeded);
        QCOMPARE(preparedRejected.coverage.acceptedObservations, std::uint64_t{1});
        QCOMPARE(planner.Stats().acceptedObservations, std::uint64_t{1});

        const RegionShadowPlan second = planner.Observe(MakeResult(spec));
        QCOMPARE(second.status, RegionPlanStatus::kAccepted);
        QCOMPARE(second.coverage.acceptedObservations, std::uint64_t{2});
    }

    void adversarialLimitsAreHardClamped()
    {
        const int huge = std::numeric_limits<int>::max();
        RegionShadowLimits limits;
        limits.maxLines = huge;
        limits.maxWords = huge;
        limits.maxWordUtf16Units = huge;
        limits.maxTotalUtf16Units = huge;
        limits.maxOccurrences = huge;
        limits.maxSelectionsPerPlan = huge;
        limits.reservedCoverageQuota = huge;
        limits.gridColumns = huge;
        limits.gridRows = huge;
        limits.staleCellAfterObservations = huge;
        limits.maxMissingObservations = huge;
        limits.stableAfterObservations = huge;
        RegionShadowPlanner planner(limits);
        const RegionShadowLimits& effective = planner.Limits();
        QCOMPARE(effective.maxLines, 1024);
        QCOMPARE(effective.maxWords, 8192);
        QCOMPARE(effective.maxWordUtf16Units, 4096);
        QCOMPARE(effective.maxTotalUtf16Units, 1 << 20);
        QCOMPARE(effective.maxOccurrences, 2048);
        QCOMPARE(effective.maxSelectionsPerPlan, 256);
        QCOMPARE(effective.reservedCoverageQuota, 256);
        QCOMPARE(effective.gridColumns, 16);
        QCOMPARE(effective.gridRows, 16); // 256 cells, never huge*huge
        QCOMPARE(effective.staleCellAfterObservations, 1'000'000);
        QCOMPARE(effective.maxMissingObservations, 1'000'000);
        QCOMPARE(effective.stableAfterObservations, 1'000'000);

        // Oversize input is rejected by counting, at the hard caps.
        ResultSpec manyLines;
        for (int i = 0; i < 1025; ++i)
            manyLines.words.push_back({QStringLiteral("w"), kWordA});
        QCOMPARE(planner.Observe(MakeResult(manyLines)).rejectReason,
                 RegionRejectReason::kLineLimitExceeded);

        ResultSpec one;
        one.words = {{QStringLiteral("w"), kWordA}};
        RecognitionResult manyWords = MakeResult(one);
        PreparedWord extra{QStringLiteral("x"), kWordB, TokenDisposition::kCandidate,
                           0.99, kWordB, std::nullopt};
        RecognizedWord extraRecognized;
        extraRecognized.text = extra.text;
        extraRecognized.confidence = extra.confidence;
        for (int i = 0; i < 8192; ++i) { // 8193 words in one parallel line
            manyWords.prepared[0].words.push_back(extra);
            manyWords.output.lines[0].words.push_back(extraRecognized);
        }
        QCOMPARE(planner.Observe(manyWords).rejectReason,
                 RegionRejectReason::kWordLimitExceeded);
        QCOMPARE(planner.RetainedOccurrences(), 0);

        // Negative limits clamp to the documented minima, not to chaos.
        RegionShadowLimits negative;
        negative.maxLines = -1;
        negative.maxWords = -1;
        negative.maxWordUtf16Units = -1;
        negative.maxTotalUtf16Units = -1;
        negative.maxOccurrences = -1;
        negative.maxSelectionsPerPlan = -1;
        negative.reservedCoverageQuota = -1;
        negative.gridColumns = -1;
        negative.gridRows = -1;
        negative.staleCellAfterObservations = -1;
        negative.maxMissingObservations = -1;
        negative.stableAfterObservations = -1;
        RegionShadowPlanner flooredPlanner(negative);
        const RegionShadowLimits& floored = flooredPlanner.Limits();
        QCOMPARE(floored.maxLines, 1);
        QCOMPARE(floored.maxWords, 1);
        QCOMPARE(floored.maxWordUtf16Units, 1);
        QCOMPARE(floored.maxTotalUtf16Units, 1);
        QCOMPARE(floored.maxOccurrences, 1);
        QCOMPARE(floored.maxSelectionsPerPlan, 1);
        QCOMPARE(floored.reservedCoverageQuota, 1);
        QCOMPARE(floored.gridColumns, 1);
        QCOMPARE(floored.gridRows, 1);
        QCOMPARE(floored.staleCellAfterObservations, 0);
        QCOMPARE(floored.maxMissingObservations, 0);
        QCOMPARE(floored.stableAfterObservations, 1);
    }

    void occurrenceCapAndIdOverflowAreBounded()
    {
        RegionShadowLimits limits = TestLimits();
        limits.maxOccurrences = 3;
        RegionShadowPlanner planner(limits);
        ResultSpec spec;
        for (int i = 0; i < 5; ++i) {
            spec.words.push_back(
                {QStringLiteral("w%1").arg(i), {0.02 + i * 0.18, 0.05, 0.10, 0.04}});
        }
        const RegionShadowPlan plan = planner.Observe(MakeResult(spec));
        QCOMPARE(plan.comparison.retainedOccurrences, 3);
        QCOMPARE(plan.comparison.overflowDropped, 2);
        QVERIFY(plan.truncated);

        RegionShadowLimits idLimits = TestLimits();
        idLimits.maxOccurrenceId = 2; // exercise the id-overflow guard finitely
        RegionShadowPlanner idPlanner(idLimits);
        const RegionShadowPlan idPlan = idPlanner.Observe(MakeResult(spec));
        QCOMPARE(idPlan.comparison.newOccurrences, 2);
        QCOMPARE(idPlan.comparison.overflowDropped, 3);
        for (const RegionPlannedSelection& selection : OccurrenceSelections(idPlan))
            QVERIFY(selection.occurrenceId >= 1 && selection.occurrenceId <= 2);

        // Exhaustion is permanent: minting must never wrap to 0 or reuse
        // 1..2, in later observations or after an explicit reset.
        ResultSpec moved;
        moved.jobId = 2;
        moved.frameSeq = 2;
        moved.timestampMs = 1400;
        for (int i = 0; i < 5; ++i) {
            moved.words.push_back(
                {QStringLiteral("m%1").arg(i), {0.02 + i * 0.18, 0.50, 0.10, 0.04}});
        }
        const RegionShadowPlan exhausted = idPlanner.Observe(MakeResult(moved));
        QCOMPARE(exhausted.status, RegionPlanStatus::kAccepted);
        QCOMPARE(exhausted.comparison.newOccurrences, 0);
        QCOMPARE(exhausted.comparison.overflowDropped, 5);
        QVERIFY(exhausted.truncated);
        QCOMPARE(exhausted.comparison.retainedOccurrences, 2); // ids 1..2, now missing
        for (const RegionPlannedSelection& selection : OccurrenceSelections(exhausted))
            QVERIFY(selection.occurrenceId >= 1 && selection.occurrenceId <= 2);

        idPlanner.Reset(RegionResetReason::kExplicit);
        moved.jobId = 3;
        moved.frameSeq = 3;
        moved.timestampMs = 1800;
        const RegionShadowPlan afterReset = idPlanner.Observe(MakeResult(moved));
        QCOMPARE(afterReset.comparison.newOccurrences, 0);
        QCOMPARE(afterReset.comparison.overflowDropped, 5);
        QCOMPARE(afterReset.comparison.retainedOccurrences, 0);
        QVERIFY(OccurrenceSelections(afterReset).isEmpty());
    }

    void deterministicIdsGenerationsAndResetEpoch()
    {
        const auto runSequence = [] {
            RegionShadowPlanner planner(TestLimits());
            QVector<std::uint64_t> ids;
            ResultSpec spec;
            spec.words = {{QStringLiteral("one"), kWordA}, {QStringLiteral("two"), kWordB}};
            for (int step = 1; step <= 3; ++step) {
                spec.jobId = std::uint64_t(step);
                spec.frameSeq = std::uint64_t(step);
                spec.timestampMs = 1000 + step * 400;
                for (const RegionPlannedSelection& selection :
                     OccurrenceSelections(planner.Observe(MakeResult(spec))))
                    ids.push_back(selection.occurrenceId);
            }
            return ids;
        };
        QVERIFY(runSequence() == runSequence()); // same inputs, same identities

        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.words = {{QStringLiteral("x"), kWordA}};
        const std::uint64_t epochBefore = planner.PlannerEpoch();
        const std::uint64_t idBefore =
            OccurrenceSelections(planner.Observe(MakeResult(spec))).front().occurrenceId;
        planner.Reset();
        QCOMPARE(planner.PlannerEpoch(), epochBefore + 1);
        QCOMPARE(planner.RetainedOccurrences(), 0);
        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RegionShadowPlan afterReset = planner.Observe(MakeResult(spec));
        QCOMPARE(afterReset.resetReason, RegionResetReason::kExplicit);
        QVERIFY(OccurrenceSelections(afterReset).front().occurrenceId > idBefore);
    }

    void smoothedUnverifiedLinesDoNotStabilize()
    {
        RegionShadowPlanner planner(TestLimits());
        ResultSpec spec;
        spec.temporallySmoothed = true;
        spec.words = {{QStringLiteral("blend"), kWordA}};
        spec.currentInputMatches = {false}; // smoothed OCR not proven current
        planner.Observe(MakeResult(spec));
        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RegionShadowPlan unverified = planner.Observe(MakeResult(spec));
        const RegionPlannedSelection word = OccurrenceSelections(unverified).front();
        QCOMPARE(word.status, RegionOccurrenceStatus::kCandidate);
        QVERIFY(word.smoothedUnverified);
        QCOMPARE(word.observations, std::uint64_t{2}); // seen, not self-proven

        spec.currentInputMatches = {true};
        spec.jobId = 3;
        spec.frameSeq = 3;
        spec.timestampMs = 1800;
        planner.Observe(MakeResult(spec));
        spec.jobId = 4;
        spec.frameSeq = 4;
        spec.timestampMs = 2200;
        const RegionShadowPlan verified = planner.Observe(MakeResult(spec));
        QCOMPARE(OccurrenceSelections(verified).front().status,
                 RegionOccurrenceStatus::kStable);
    }

    void smoothedUnverifiedDemotesStableAndRestartsStreak()
    {
        // Regression: a previously stable occurrence seen again with the
        // same text but without exact-current proof must demote to
        // candidate, not keep kStable on its old streak.
        RegionShadowPlanner planner(TestLimits()); // stable after 2 verified
        ResultSpec spec;
        spec.words = {{QStringLiteral("blend"), kWordA}};
        planner.Observe(MakeResult(spec));
        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        const RegionShadowPlan stable = planner.Observe(MakeResult(spec));
        QCOMPARE(OccurrenceSelections(stable).front().status,
                 RegionOccurrenceStatus::kStable);

        spec.jobId = 3;
        spec.frameSeq = 3;
        spec.timestampMs = 1800;
        spec.temporallySmoothed = true;
        spec.currentInputMatches = {false}; // same text, unverified pixels
        const RegionShadowPlan demoted = planner.Observe(MakeResult(spec));
        const RegionPlannedSelection demotedWord = OccurrenceSelections(demoted).front();
        QCOMPARE(demotedWord.status, RegionOccurrenceStatus::kCandidate);
        QVERIFY(demotedWord.smoothedUnverified);
        QCOMPARE(demotedWord.observations, std::uint64_t{3});
        // An unverified repeat is not a verified stable repeat.
        QCOMPARE(demoted.comparison.repeatStableCandidates, 0);

        // One verified observation starts a new streak; it is not enough.
        spec.jobId = 4;
        spec.frameSeq = 4;
        spec.timestampMs = 2200;
        spec.currentInputMatches = {true};
        const RegionShadowPlan rebuilding = planner.Observe(MakeResult(spec));
        QCOMPARE(OccurrenceSelections(rebuilding).front().status,
                 RegionOccurrenceStatus::kCandidate);

        spec.jobId = 5;
        spec.frameSeq = 5;
        spec.timestampMs = 2600;
        const RegionShadowPlan restable = planner.Observe(MakeResult(spec));
        QCOMPARE(OccurrenceSelections(restable).front().status,
                 RegionOccurrenceStatus::kStable);
    }

    void gridCellsCarryUnseenCoverage()
    {
        RegionShadowPlanner planner(TestLimits()); // 2x2 grid
        ResultSpec empty;
        const RegionShadowPlan emptyPlan = planner.Observe(MakeResult(empty));
        QCOMPARE(emptyPlan.status, RegionPlanStatus::kAccepted);
        QCOMPARE(emptyPlan.coverage.gridCellCount, 4);
        QCOMPARE(emptyPlan.coverage.cellsWithCandidates, 0);
        QCOMPARE(emptyPlan.coverage.dueGridCells, 4);
        bool sawCell = false;
        for (const RegionPlannedSelection& selection : emptyPlan.selections)
            sawCell = sawCell || selection.isGridCell;
        QVERIFY(sawCell); // unseen scene area owes coverage work

        ResultSpec spec;
        spec.jobId = 2;
        spec.frameSeq = 2;
        spec.timestampMs = 1400;
        spec.words = {{QStringLiteral("here"), kWordA}}; // top-left cell only
        const RegionShadowPlan plan = planner.Observe(MakeResult(spec));
        QCOMPARE(plan.coverage.cellsWithCandidates, 1);
        QVERIFY(plan.coverage.coverageDebt >= 0);
        QVERIFY(plan.coverage.worstCasePlansToCoverAll <
                std::uint64_t{1000}); // explicit and finite
    }
};

QTEST_APPLESS_MAIN(UctRegionPlannerTests)
#include "uct_region_planner_tests.moc"
