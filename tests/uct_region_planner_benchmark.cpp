// Opt-in, desktop-free CPU metadata diagnostic for the UCT shadow planner.
// It constructs no OCR backend, pixels, fonts, camera, model or native window.
// Observe timings describe bookkeeping cost only, never an OCR speedup.

#include "okuflow/common/uct_region_planner.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using namespace okuflow::uct;

namespace {

constexpr int kMinimumRepetitions = 4;
constexpr int kMaximumRepetitions = 64;
constexpr int kMaximumWarmups = 8;
constexpr int kMaximumFailureMessages = 24;
constexpr int kMaximumJsonBytes = 256 * 1024;
constexpr int kWordsPerLine = 8;
constexpr int kChurnMinimumRepetitions = 24;
constexpr int kChurnMaximumObservations = kMaximumRepetitions + kMaximumWarmups;

struct Options {
    int repetitions{12};
    int warmups{2};
    QString outputPath;
};

enum class Layout {
    kStable,
    kShifted,
    kRepeated,
    kChurn,
    kOverlapping,
};

struct Scenario {
    QString name;
    Layout layout{Layout::kStable};
    int wordCount{};
};

// Distinct synthetic metadata tickets represent independent observations in
// this diagnostic. They are not camera/pixel evidence and never originate in
// held copies. One sequence is shared across every scenario and warmup.
struct Tickets {
    std::uint64_t next{1};
};

struct Checks {
    int failures{};
    QJsonArray messages;

    void Require(bool condition, const char* reason, int observation = 0)
    {
        if (condition) return;
        ++failures;
        if (messages.size() < kMaximumFailureMessages) {
            messages.append(QStringLiteral("observation %1: %2")
                                .arg(observation).arg(QString::fromLatin1(reason)));
        }
    }
};

QJsonValue JsonCount(std::uint64_t value)
{
    // All diagnostic counters are bounded by the finite loops below and fit
    // exactly in JSON's integer range; no uint64 ticket is supplied by a user.
    return QJsonValue(static_cast<qint64>(value));
}

QJsonObject Distribution(const std::vector<qint64>& samples)
{
    if (samples.empty()) return {{"count", 0}};
    std::vector<qint64> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto nearestRank = [&](double percentile) {
        const auto index = static_cast<std::size_t>(
            std::ceil(percentile * static_cast<double>(sorted.size()))) - 1;
        return sorted[index];
    };
    QJsonArray raw;
    for (const qint64 sample : samples) raw.append(sample);
    return {{"count", static_cast<int>(samples.size())},
            {"median_ns", nearestRank(0.5)},
            {"p95_ns", nearestRank(0.95)},
            {"max_ns", sorted.back()},
            {"samples_ns", raw}};
}

void AppendWord(RecognitionResult& result, const QString& text, const QRectF& rect)
{
    if (result.prepared.isEmpty() ||
        result.prepared.back().words.size() == kWordsPerLine) {
        result.prepared.push_back(PreparedLine{});
        result.output.lines.push_back(RecognizedLine{});
    }
    RecognizedWord recognized;
    recognized.text = text;
    recognized.boundsPx = QRectF(rect.x() * 1920.0, rect.y() * 1080.0,
                                 rect.width() * 1920.0, rect.height() * 1080.0);
    recognized.disposition = TokenDisposition::kCandidate;
    recognized.confidence = 0.99;
    result.output.lines.back().words.push_back(std::move(recognized));

    PreparedLine& line = result.prepared.back();
    line.sceneRect = line.words.isEmpty() ? rect : line.sceneRect.united(rect);
    line.words.push_back({text, rect, TokenDisposition::kCandidate, 0.99,
                          rect, std::nullopt});
}

RecognitionResult MakeResult(const Scenario& scenario, int observation,
                             std::uint64_t sourceEpoch, Tickets& tickets)
{
    RecognitionResult result;
    const std::uint64_t ticket = tickets.next++;
    result.jobId = ticket;
    result.authority = 1;
    result.stamp.sourceEpoch = sourceEpoch;
    result.stamp.geometryGeneration = 1;
    result.stamp.frameSeq = ticket;
    result.stamp.timestampMs = 1000 + static_cast<std::int64_t>(ticket) * 400;
    result.stamp.viewportWidth = 1920;
    result.stamp.viewportHeight = 1080;
    result.stamp.viewTransform.valid = true;
    result.stamp.viewTransform.sourceWidth = 1.0f;
    result.stamp.viewTransform.sourceHeight = 1.0f;
    result.stamp.viewTransform.destinationWidth = 1.0f;
    result.stamp.viewTransform.destinationHeight = 1.0f;
    result.stamp.holdDisplay = HoldDisplay::NotHeld;
    result.output.ok = true;
    const int lines = (scenario.wordCount + kWordsPerLine - 1) / kWordsPerLine;
    result.prepared.reserve(lines);
    result.output.lines.reserve(lines);

    if (scenario.layout == Layout::kChurn) {
        AppendWord(result, QStringLiteral("focus"), QRectF(0.05, 0.05, 0.10, 0.04));
        for (int i = 0; i < 4; ++i) {
            // Consecutive cohorts do not overlap; the old cohort ages out
            // before a geometry repeats. Constant focus competes with churn.
            const double x = 0.30 + 0.07 * double((observation + i * 3) % 9);
            const double y = 0.08 + 0.20 * double(i);
            AppendWord(result, QStringLiteral("churn-%1-%2").arg(observation).arg(i),
                       QRectF(x, y, 0.04, 0.04));
        }
        return result;
    }

    const int columns = scenario.wordCount <= 512 ? 16 : 64;
    const int rows = (scenario.wordCount + columns - 1) / columns;
    const double shift = scenario.layout == Layout::kShifted && observation % 2 == 0
                             ? 0.45 : 0.0;
    for (int i = 0; i < scenario.wordCount; ++i) {
        // Unique but almost coincident rectangles deliberately make every
        // word overlap every retained occurrence. This exposes the cost of
        // scanning/sorting candidates before the top-eight match cap.
        const QRectF rect = scenario.layout == Layout::kOverlapping
            ? QRectF(0.20 + double(i) * 0.000001, 0.20, 0.50, 0.10)
            : QRectF((double(i % columns) + 0.1 + shift) / columns,
                     (double(i / columns) + 0.1) / rows,
                     0.4 / columns, 0.4 / rows);
        const QString text = scenario.layout == Layout::kRepeated
                                 ? QStringLiteral("repeat")
                                 : QStringLiteral("word-%1").arg(i);
        AppendWord(result, text, rect);
    }
    return result;
}

bool ValidSceneRect(const QRectF& rect)
{
    constexpr double epsilon = 1e-12;
    return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
           std::isfinite(rect.width()) && std::isfinite(rect.height()) &&
           rect.width() > 0.0 && rect.height() > 0.0 &&
           rect.left() >= -epsilon && rect.top() >= -epsilon &&
           rect.right() <= 1.0 + epsilon && rect.bottom() <= 1.0 + epsilon;
}

void ValidatePlan(const RecognitionResult& input, const RegionShadowPlan& plan,
                  const RegionShadowPlanner& planner, int observation,
                  int expectedWords, int previousRetained, Checks& checks)
{
    const RegionShadowLimits& limits = plan.limits;
    checks.Require(plan.status == RegionPlanStatus::kAccepted,
                   "valid fresh metadata was rejected", observation);
    checks.Require(plan.rejectReason == RegionRejectReason::kNone,
                   "accepted plan has a reject reason", observation);
    checks.Require(plan.authoritativeAction == RegionAuthoritativeAction::kRunFullOcr,
                   "shadow plan changed full-OCR authority", observation);
    checks.Require(plan.jobId == input.jobId && plan.authority == input.authority &&
                       plan.stamp.frameSeq == input.stamp.frameSeq &&
                       plan.stamp.sourceEpoch == input.stamp.sourceEpoch &&
                       plan.stamp.timestampMs == input.stamp.timestampMs,
                   "plan did not preserve the input ticket", observation);
    checks.Require(plan.evidenceAcquisition == EvidenceAcquisition(input.stamp) &&
                       plan.evidenceAcquisition == std::optional<std::uint64_t>(input.stamp.frameSeq),
                   "plan did not preserve fresh evidence acquisition", observation);
    checks.Require(plan.planGeneration == static_cast<std::uint64_t>(observation) &&
                       plan.plannerEpoch == 1 && plan.resetReason == RegionResetReason::kNone,
                   "unexpected plan generation or scene reset", observation);
    checks.Require(limits.maxLines >= 1 && limits.maxLines <= 1024 &&
                       limits.maxWords >= 1 && limits.maxWords <= 8192 &&
                       limits.maxOccurrences >= 1 && limits.maxOccurrences <= 2048 &&
                       limits.maxSelectionsPerPlan >= 1 && limits.maxSelectionsPerPlan <= 256 &&
                       limits.gridColumns >= 1 && limits.gridColumns <= 16 &&
                       limits.gridRows >= 1 && limits.gridRows <= 16,
                   "effective limits exceeded finite hard caps", observation);
    checks.Require(plan.selections.size() <= limits.maxSelectionsPerPlan,
                   "selection count exceeded its cap", observation);

    const RegionShadowComparison& comparison = plan.comparison;
    checks.Require(comparison.fullCandidateWords == expectedWords &&
                       comparison.fullOmittedWords == 0 && comparison.invalidWords == 0 &&
                       comparison.clampedWords == 0 && comparison.duplicatesSuppressed == 0,
                   "well-formed distinct spatial words were lost or merged", observation);
    checks.Require(comparison.newOccurrences + comparison.matchedOccurrences +
                           comparison.overflowDropped == expectedWords,
                   "candidate accounting did not balance", observation);
    checks.Require(comparison.retainedOccurrences >= 0 &&
                       comparison.retainedOccurrences <= limits.maxOccurrences &&
                       comparison.retainedOccurrences == planner.RetainedOccurrences() &&
                       comparison.retainedOccurrences == previousRetained +
                           comparison.newOccurrences - comparison.evictedOccurrences,
                   "retained occurrence accounting exceeded bounds or did not balance", observation);
    checks.Require(plan.truncated == (comparison.overflowDropped > 0),
                   "overflow was not reported as truncation", observation);

    const RegionCoverage& coverage = plan.coverage;
    checks.Require(coverage.acceptedObservations == static_cast<std::uint64_t>(observation),
                   "accepted observation clock was not fresh and monotonic", observation);
    checks.Require(coverage.gridCellCount == limits.gridColumns * limits.gridRows &&
                       coverage.dueGridCells >= 0 &&
                       coverage.dueGridCells <= coverage.gridCellCount &&
                       coverage.eligibleRegions == comparison.retainedOccurrences +
                           coverage.dueGridCells &&
                       coverage.selectedRegions == plan.selections.size() &&
                       coverage.coverageDebt == coverage.eligibleRegions - coverage.selectedRegions &&
                       coverage.coverageDebt >= 0,
                   "coverage counts did not balance", observation);
    checks.Require(coverage.reservedQuota >= 1 &&
                       coverage.reservedQuota <= limits.maxSelectionsPerPlan,
                   "reserved quota was outside bounds", observation);
    if (coverage.reservedQuota > 0 && coverage.coverageDebt >= 0) {
        const auto expectedBound = static_cast<std::uint64_t>(
            (coverage.coverageDebt + coverage.reservedQuota - 1) / coverage.reservedQuota);
        checks.Require(coverage.worstCasePlansToCoverAll == expectedBound &&
                           coverage.coverageDeadlineObservation ==
                               coverage.acceptedObservations + expectedBound,
                       "coverage deadline disagreed with finite debt/quota bound", observation);
    }

    QSet<quint64> occurrenceIds;
    QSet<int> cellIds;
    for (const RegionPlannedSelection& selection : plan.selections) {
        checks.Require(ValidSceneRect(selection.sceneRect),
                       "selected rectangle was outside normalized scene", observation);
        if (selection.isGridCell) {
            checks.Require(selection.cellIndex >= 0 &&
                               selection.cellIndex < coverage.gridCellCount &&
                               !cellIds.contains(selection.cellIndex),
                           "selected cell was invalid or duplicated", observation);
            cellIds.insert(selection.cellIndex);
        } else {
            checks.Require(selection.occurrenceId > 0 &&
                               !occurrenceIds.contains(selection.occurrenceId) &&
                               selection.observations > 0 &&
                               selection.lastSeenAcquisition <= input.stamp.frameSeq,
                           "selected occurrence had an invalid or duplicate identity", observation);
            occurrenceIds.insert(selection.occurrenceId);
        }
    }
}

QJsonObject RunScenario(const Scenario& scenario, const Options& options,
                        std::uint64_t sourceEpoch, Tickets& tickets)
{
    RegionShadowLimits requestedLimits;
    const bool churn = scenario.layout == Layout::kChurn;
    if (churn) {
        requestedLimits.maxOccurrences = 64;
        requestedLimits.gridColumns = 4;
        requestedLimits.gridRows = 4;
        requestedLimits.maxSelectionsPerPlan = 3;
        requestedLimits.reservedCoverageQuota = 1;
        requestedLimits.staleCellAfterObservations = 1000;
        requestedLimits.maxMissingObservations = 2;
    }
    RegionShadowPlanner planner(requestedLimits);
    Checks checks;
    const bool overlapping = scenario.layout == Layout::kOverlapping;
    const int repetitions = churn ? std::max(options.repetitions, kChurnMinimumRepetitions)
                                  : overlapping ? 4 : options.repetitions;
    const int warmups = overlapping ? std::min(options.warmups, 1) : options.warmups;
    const int totalObservations = warmups + repetitions;
    std::vector<qint64> samples;
    samples.reserve(repetitions);
    std::vector<qint64> rejectedSamples;
    rejectedSamples.reserve(repetitions);
    int accepted = 0;
    int rejected = 0;
    int previousRetained = 0;
    int maxRetained = 0;
    int maxSelections = 0;
    int maxDebt = 0;
    std::uint64_t maxDeadline = 0;
    std::uint64_t maxPlansToCover = 0;
    std::uint64_t maxUnselectedAge = 0;
    std::uint64_t overflowDropped = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t selectedRegions = 0;
    std::uint64_t selectedOccurrences = 0;
    std::uint64_t selectedGridCells = 0;
    std::uint64_t firstCoverageDeadline = 0;
    int allInitialCellsServicedAt = 0;
    QSet<int> servicedCells;
    std::uint64_t previousJob = 0;
    std::uint64_t previousAcquisition = 0;

    for (int observation = 1; observation <= totalObservations; ++observation) {
        const RecognitionResult input = MakeResult(scenario, observation, sourceEpoch, tickets);
        const auto acquisition = EvidenceAcquisition(input.stamp);
        checks.Require(input.jobId > previousJob && acquisition &&
                           *acquisition > previousAcquisition &&
                           input.stamp.holdDisplay == HoldDisplay::NotHeld,
                       "fixture reused a job, held image or acquisition", observation);
        previousJob = input.jobId;
        if (acquisition) previousAcquisition = *acquisition;
        const std::optional<QRectF> focus = churn
            ? std::optional<QRectF>(QRectF(0.05, 0.05, 0.10, 0.04)) : std::nullopt;

        // Only the real Observe call and returned plan construction are timed.
        // Input generation, all checks, aggregation and serialization are not.
        const auto started = std::chrono::steady_clock::now();
        const RegionShadowPlan plan = planner.Observe(input, focus);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (observation > warmups) {
            if (plan.status == RegionPlanStatus::kAccepted)
                samples.push_back(static_cast<qint64>(elapsed));
            else
                rejectedSamples.push_back(static_cast<qint64>(elapsed));
        }

        if (plan.status == RegionPlanStatus::kAccepted) ++accepted;
        else ++rejected;
        ValidatePlan(input, plan, planner, observation, scenario.wordCount,
                     previousRetained, checks);
        previousRetained = planner.RetainedOccurrences();
        maxRetained = std::max(maxRetained, previousRetained);
        maxSelections = std::max(maxSelections, static_cast<int>(plan.selections.size()));
        maxDebt = std::max(maxDebt, plan.coverage.coverageDebt);
        maxDeadline = std::max(maxDeadline, plan.coverage.coverageDeadlineObservation);
        maxPlansToCover = std::max(maxPlansToCover, plan.coverage.worstCasePlansToCoverAll);
        maxUnselectedAge = std::max(maxUnselectedAge, plan.coverage.oldestUnselectedAgePlans);
        overflowDropped += static_cast<std::uint64_t>(std::max(0, plan.comparison.overflowDropped));
        matched += static_cast<std::uint64_t>(std::max(0, plan.comparison.matchedOccurrences));
        missing += static_cast<std::uint64_t>(std::max(0, plan.comparison.missingOccurrences));
        selectedRegions += static_cast<std::uint64_t>(plan.selections.size());
        for (const RegionPlannedSelection& selection : plan.selections) {
            if (selection.isGridCell) ++selectedGridCells;
            else ++selectedOccurrences;
        }

        if (churn) {
            checks.Require(plan.focusApplied, "constant requested focus was ignored", observation);
            if (observation == 1) {
                firstCoverageDeadline = plan.coverage.coverageDeadlineObservation;
                checks.Require(plan.coverage.coverageDebt > 0 && firstCoverageDeadline > 1 &&
                                   firstCoverageDeadline <= kChurnMaximumObservations,
                               "churn fixture lacked a finite testable initial deadline", observation);
            }
            for (const RegionPlannedSelection& selection : plan.selections)
                if (selection.isGridCell) servicedCells.insert(selection.cellIndex);
            if (servicedCells.size() == 16 && allInitialCellsServicedAt == 0)
                allInitialCellsServicedAt = observation;
        }
    }

    const RegionShadowStats stats = planner.Stats();
    checks.Require(stats.plansEmitted == static_cast<std::uint64_t>(totalObservations) &&
                       stats.acceptedObservations == static_cast<std::uint64_t>(accepted) &&
                       stats.rejectedObservations == static_cast<std::uint64_t>(rejected) &&
                       accepted == totalObservations && rejected == 0,
                   "cumulative accepted/rejected counts did not balance");
    checks.Require(stats.occurrencesCreated >= stats.occurrencesEvicted &&
                       stats.occurrencesCreated - stats.occurrencesEvicted ==
                           static_cast<std::uint64_t>(planner.RetainedOccurrences()),
                   "cumulative occurrence counts did not balance");
    if (scenario.layout == Layout::kStable || scenario.layout == Layout::kRepeated) {
        checks.Require(stats.occurrencesCreated == static_cast<std::uint64_t>(
                           std::min(scenario.wordCount, planner.Limits().maxOccurrences)) &&
                           stats.occurrencesEvicted == 0 && stats.changedDetected == 0,
                       "stationary distinct spatial words did not retain their identities");
        checks.Require(stats.repeatStableCandidates > 0,
                       "stationary fresh acquisitions never recorded stable repetition");
    }
    if (scenario.layout == Layout::kShifted)
        checks.Require(stats.occurrencesCreated > static_cast<std::uint64_t>(
                           std::min(scenario.wordCount, planner.Limits().maxOccurrences)) &&
                           missing + stats.occurrencesEvicted > 0,
                       "shifted layout did not exercise new/missing occurrence churn");
    if (churn) {
        checks.Require(allInitialCellsServicedAt > 0 && servicedCells.size() == 16 &&
                           static_cast<std::uint64_t>(allInitialCellsServicedAt) <= firstCoverageDeadline,
                       "initial coverage starved past its stated deadline under continual newcomers");
    }

    QJsonObject result{
        {"name", scenario.name}, {"input_words", scenario.wordCount},
        {"input_lines", (scenario.wordCount + kWordsPerLine - 1) / kWordsPerLine},
        {"warmup_observations", warmups}, {"measured_observations", repetitions},
        {"accepted", accepted}, {"rejected", rejected},
        {"observe", Distribution(samples)},
        {"rejected_observe", Distribution(rejectedSamples)},
        {"occurrences", QJsonObject{
            {"created", JsonCount(stats.occurrencesCreated)},
            {"evicted", JsonCount(stats.occurrencesEvicted)},
            {"matched_word_observations", JsonCount(matched)},
            {"missing_word_observations", JsonCount(missing)},
            {"retained_final", planner.RetainedOccurrences()}, {"retained_max", maxRetained},
            {"overflow_dropped", JsonCount(overflowDropped)},
            {"stable_repeat_candidates", JsonCount(stats.repeatStableCandidates)},
            {"changed_detected", JsonCount(stats.changedDetected)},
            {"changed_previously_stable_unselected", JsonCount(stats.changedPreviouslyStableUnselected)}}},
        {"coverage", QJsonObject{
            {"selection_cap", planner.Limits().maxSelectionsPerPlan},
            {"retained_cap", planner.Limits().maxOccurrences},
            {"selections_max", maxSelections}, {"debt_max", maxDebt},
            {"selected_regions_total", JsonCount(selectedRegions)},
            {"selected_occurrences_total", JsonCount(selectedOccurrences)},
            {"selected_grid_cells_total", JsonCount(selectedGridCells)},
            {"plans_to_cover_all_max", JsonCount(maxPlansToCover)},
            {"deadline_observation_max", JsonCount(maxDeadline)},
            {"oldest_unselected_age_plans_max", JsonCount(maxUnselectedAge)}}},
        {"ok", checks.failures == 0}, {"invariant_failures", checks.failures},
        {"failure_messages", checks.messages},
        {"failure_messages_omitted", std::max(0, checks.failures - kMaximumFailureMessages)}};
    if (churn) {
        result["churn_fairness"] = QJsonObject{
            {"initial_cells", 16}, {"initial_cells_serviced", servicedCells.size()},
            {"first_deadline_observation", JsonCount(firstCoverageDeadline)},
            {"all_initial_cells_serviced_at", allInitialCellsServicedAt},
            {"constant_focus", true}, {"fresh_words_per_observation", 4}};
    }
    if (overlapping)
        result["workload_note"] = QStringLiteral(
            "All 2048 distinct word rectangles overlap; four measured calls and at most one warmup.");
    return result;
}

bool BoundedInteger(const QCommandLineParser& parser, const QString& key,
                    int minimum, int maximum, int& value)
{
    bool ok = false;
    const qlonglong parsed = parser.value(key).toLongLong(&ok);
    if (!ok || parsed < minimum || parsed > maximum) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool EmitJson(const QJsonObject& result, const QString& outputPath)
{
    QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (bytes.size() > kMaximumJsonBytes) {
        std::fputs("Refusing JSON larger than the diagnostic's 256 KiB bound.\n", stderr);
        return false;
    }
    if (outputPath.isEmpty()) {
        const bool written = std::fwrite(bytes.constData(), 1,
            static_cast<std::size_t>(bytes.size()), stdout) == static_cast<std::size_t>(bytes.size());
        return written && std::fflush(stdout) == 0;
    }
    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        std::fprintf(stderr, "Cannot write benchmark JSON: %s\n", qPrintable(file.errorString()));
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("uct_region_planner_benchmark"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Finite opt-in UCT shadow-planner metadata timings. No OCR inference or native UI."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("run"), QStringLiteral("Explicitly run the synthetic diagnostic.")});
    parser.addOption({QStringLiteral("repetitions"),
                      QStringLiteral("Measured calls, 4..64 (churn at least 24; overlapping exactly 4)."),
                      QStringLiteral("count"), QStringLiteral("12")});
    parser.addOption({QStringLiteral("warmups"),
                      QStringLiteral("Discarded warmup observations, 0..8 (overlapping at most 1)."),
                      QStringLiteral("count"), QStringLiteral("2")});
    parser.addOption({{QStringLiteral("o"), QStringLiteral("output")},
                      QStringLiteral("Write JSON to this new file; otherwise use stdout."),
                      QStringLiteral("path")});
    if (!parser.parse(app.arguments())) {
        std::fprintf(stderr, "%s\n", qPrintable(parser.errorText()));
        return 2;
    }
    if (parser.isSet(QStringLiteral("help"))) parser.showHelp(0);
    Options options;
    if (!parser.isSet(QStringLiteral("run")) || !parser.positionalArguments().isEmpty() ||
        !BoundedInteger(parser, QStringLiteral("repetitions"), kMinimumRepetitions,
                        kMaximumRepetitions, options.repetitions) ||
        !BoundedInteger(parser, QStringLiteral("warmups"), 0, kMaximumWarmups, options.warmups) ||
        (parser.isSet(QStringLiteral("output")) && parser.value(QStringLiteral("output")).isEmpty())) {
        std::fputs("Use --run with --repetitions 4..64 and --warmups 0..8; see --help.\n", stderr);
        return 2;
    }
    options.outputPath = parser.value(QStringLiteral("output"));
    if (!options.outputPath.isEmpty() && QFileInfo::exists(options.outputPath)) {
        std::fputs("Refusing to replace existing benchmark evidence; choose a new --output file.\n", stderr);
        return 2;
    }

    QJsonArray scenarios;
    Tickets tickets;
    bool passed = true;
    std::uint64_t sourceEpoch = 1;
    for (const int words : {16, 128, 512, 2048, 8192}) {
        for (const auto& layout : {std::pair{Layout::kStable, "stable"},
                                   std::pair{Layout::kShifted, "shifted"},
                                   std::pair{Layout::kRepeated, "repeated"}}) {
            const Scenario scenario{QStringLiteral("%1_%2").arg(QString::fromLatin1(layout.second)).arg(words),
                                    layout.first, words};
            QJsonObject measured = RunScenario(scenario, options, sourceEpoch++, tickets);
            passed = passed && measured.value(QStringLiteral("ok")).toBool();
            scenarios.append(measured);
        }
    }
    QJsonObject churn = RunScenario({QStringLiteral("continual_newcomers_constant_focus"),
                                     Layout::kChurn, 5}, options, sourceEpoch, tickets);
    passed = passed && churn.value(QStringLiteral("ok")).toBool();
    scenarios.append(churn);
    QJsonObject overlapping = RunScenario({QStringLiteral("overlapping_2048"),
                                           Layout::kOverlapping, 2048}, options, ++sourceEpoch, tickets);
    passed = passed && overlapping.value(QStringLiteral("ok")).toBool();
    scenarios.append(overlapping);

    const QJsonObject result{
        {"schema_version", 1}, {"diagnostic", QStringLiteral("uct_shadow_region_planner")},
        {"scope", QStringLiteral("synthetic_metadata_only_no_ocr_performance_claim")},
        {"evidence", QStringLiteral("distinct_not_held_synthetic_acquisition_tickets")},
        {"timed_boundary", QStringLiteral("RegionShadowPlanner::Observe_call_and_return_only")},
        {"counter_scope", QStringLiteral("all_observations_including_warmups")},
        {"timing_scope", QStringLiteral("accepted_measured_observations_excluding_warmups")},
        {"percentile_method", QStringLiteral("nearest_rank")},
        {"clock", QStringLiteral("std::chrono::steady_clock")},
        {"qt_version", QString::fromLatin1(qVersion())},
        {"architecture", QSysInfo::currentCpuArchitecture()},
        {"build_abi", QSysInfo::buildAbi()},
        {"repetitions_requested", options.repetitions}, {"warmups", options.warmups},
        {"scenario_count", scenarios.size()}, {"total_observations", JsonCount(tickets.next - 1)},
        {"max_json_bytes", kMaximumJsonBytes}, {"ok", passed}, {"scenarios", scenarios}};
    if (!EmitJson(result, options.outputPath)) return 2;
    return passed ? 0 : 1;
}
