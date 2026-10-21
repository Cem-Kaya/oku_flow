#include "okuflow/common/uct_region_planner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace okuflow::uct {

struct RegionShadowPlanner::Occurrence {
    std::uint64_t id{};
    QString text;
    QRectF sceneRect; // normalized, clamped to the unit scene square
    RegionOccurrenceStatus status{RegionOccurrenceStatus::kCandidate};
    std::uint64_t observations{};          // distinct fresh acquisitions
    std::uint64_t consecutiveSameText{};   // verified-line repetitions
    std::uint64_t firstSeenObservation{};  // accepted-observation ordinal
    std::uint64_t lastSeenObservation{};
    std::uint64_t lastSeenAcquisition{};
    std::uint64_t lastSelectedObservation{}; // 0 = never selected
    bool smoothedUnverified{};
};

struct RegionShadowPlanner::Cell {
    int candidateWordsLatest{};
    bool everCandidate{};
    bool selectedOnce{};
    std::uint64_t lastSelectedObservation{};
    std::uint64_t lastCandidateAcquisition{};
};

namespace {

std::uint64_t SaturatingIncrement(std::uint64_t value)
{
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1;
}

std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b)
{
    return b > std::numeric_limits<std::uint64_t>::max() - a
               ? std::numeric_limits<std::uint64_t>::max()
               : a + b;
}

bool IsFiniteRect(const QRectF& rect)
{
    return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
           std::isfinite(rect.width()) && std::isfinite(rect.height());
}

bool SameMappedRect(const QRectF& a, const QRectF& b)
{
    // These are two representations of the same job, not a recognition
    // quality comparison. Permit only arithmetic roundoff in scene units.
    constexpr double kMappingTolerance = 1e-12;
    return IsFiniteRect(a) && IsFiniteRect(b) &&
           std::abs(a.x() - b.x()) <= kMappingTolerance &&
           std::abs(a.y() - b.y()) <= kMappingTolerance &&
           std::abs(a.width() - b.width()) <= kMappingTolerance &&
           std::abs(a.height() - b.height()) <= kMappingTolerance;
}

bool PreparedWordMatchesSource(const PreparedWord& prepared,
                               const RecognizedWord& source,
                               const SourceStamp& stamp)
{
    // PrepareLines copies confidence unchanged and trims text. NaN may be
    // preserved in an omitted low-confidence span; it is never evidence.
    if (prepared.text != source.text.trimmed() ||
        !(prepared.confidence == source.confidence ||
          (std::isnan(prepared.confidence) && std::isnan(source.confidence))))
        return false;
    if (source.disposition != TokenDisposition::kCandidate &&
        prepared.disposition == TokenDisposition::kCandidate)
        return false;

    // The stamp extent is the full job-image extent. Reference probes may
    // be decimated and must never supply the dimensions for this mapping.
    const QRectF scene = ViewRectToScene(stamp.viewTransform, stamp.viewportWidth,
                                        stamp.viewportHeight, source.boundsPx);
    const QRectF protectedScene = ViewRectToScene(
        stamp.viewTransform, stamp.viewportWidth, stamp.viewportHeight,
        source.protectedBoundsPx.isEmpty() ? source.boundsPx : source.protectedBoundsPx);
    if (!IsFiniteRect(scene) || scene.isEmpty() ||
        !IsFiniteRect(protectedScene) || protectedScene.isEmpty()) {
        // Preparation deliberately preserves invalid source spans as
        // omissions. Their unusable coordinates are never tracked.
        return prepared.disposition == TokenDisposition::kInvalidGeometry;
    }
    // Compare before the observer clamps a valid mapped box into the scene.
    // Candidate words may be conservatively omitted by preparation (e.g.
    // its scheduler-specific size threshold); no threshold is guessed here.
    return SameMappedRect(prepared.sceneRect, scene) &&
           SameMappedRect(prepared.protectedSceneRect, protectedScene);
}

// Returns the rect inside the unit scene square, or nullopt when nothing
// usable remains. Sets `clamped` when intersection changed the rect.
std::optional<QRectF> NormalizeSceneRect(const QRectF& rect, bool& clamped)
{
    clamped = false;
    if (!IsFiniteRect(rect)) return std::nullopt;
    const QRectF normalized = rect.normalized();
    const QRectF inside = normalized.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (inside.isEmpty()) return std::nullopt;
    clamped = inside != normalized;
    return inside;
}

double IntersectionOverUnion(const QRectF& a, const QRectF& b)
{
    const QRectF overlap = a.intersected(b);
    if (overlap.isEmpty()) return 0.0;
    const double intersection = overlap.width() * overlap.height();
    const double unionArea = a.width() * a.height() + b.width() * b.height() - intersection;
    return unionArea > 0.0 ? intersection / unionArea : 0.0;
}

QRectF CellSceneRect(const RegionShadowLimits& limits, int cellIndex)
{
    const int column = cellIndex % limits.gridColumns;
    const int row = cellIndex / limits.gridColumns;
    return {double(column) / limits.gridColumns, double(row) / limits.gridRows,
            1.0 / limits.gridColumns, 1.0 / limits.gridRows};
}

// Calls `visit(cellIndex)` for every grid cell the (unit-square) rect covers.
template <typename Visit>
void ForEachCoveredCell(const RegionShadowLimits& limits, const QRectF& rect, Visit&& visit)
{
    const int columns = limits.gridColumns;
    const int rows = limits.gridRows;
    const auto axisRange = [](double low, double high, int divisions, int& first, int& last) {
        first = std::clamp(int(std::floor(low * divisions)), 0, divisions - 1);
        last = std::clamp(int(std::floor((high - 1e-12) * divisions)), first, divisions - 1);
    };
    int firstColumn = 0, lastColumn = 0, firstRow = 0, lastRow = 0;
    axisRange(rect.left(), rect.right(), columns, firstColumn, lastColumn);
    axisRange(rect.top(), rect.bottom(), rows, firstRow, lastRow);
    for (int row = firstRow; row <= lastRow; ++row)
        for (int column = firstColumn; column <= lastColumn; ++column)
            visit(row * columns + column);
}

// Hard upper caps: an externally supplied limit can only shrink the
// planner, never expand it past these finite bounds, so construction and
// every Observe allocate in terms of them (at most 2048 occurrences and
// 256 grid cells are ever eligible at once).
constexpr int kHardMaxLines = 1024;
constexpr int kHardMaxWords = 8192;
constexpr int kHardMaxWordUtf16Units = 4096;
constexpr int kHardMaxTotalUtf16Units = 1 << 20;
constexpr int kHardMaxOccurrences = 2048;
constexpr int kHardMaxSelectionsPerPlan = 256;
constexpr int kHardMaxGridAxis = 16; // 16 * 16 = 256 cells at most
constexpr int kHardMaxAgeObservations = 1'000'000;

RegionShadowLimits ClampLimits(RegionShadowLimits limits)
{
    limits.maxLines = std::clamp(limits.maxLines, 1, kHardMaxLines);
    limits.maxWords = std::clamp(limits.maxWords, 1, kHardMaxWords);
    limits.maxWordUtf16Units =
        std::clamp(limits.maxWordUtf16Units, 1, kHardMaxWordUtf16Units);
    limits.maxTotalUtf16Units =
        std::clamp(limits.maxTotalUtf16Units, 1, kHardMaxTotalUtf16Units);
    limits.maxOccurrences = std::clamp(limits.maxOccurrences, 1, kHardMaxOccurrences);
    limits.maxSelectionsPerPlan =
        std::clamp(limits.maxSelectionsPerPlan, 1, kHardMaxSelectionsPerPlan);
    limits.reservedCoverageQuota =
        std::clamp(limits.reservedCoverageQuota, 1, limits.maxSelectionsPerPlan);
    limits.gridColumns = std::clamp(limits.gridColumns, 1, kHardMaxGridAxis);
    limits.gridRows = std::clamp(limits.gridRows, 1, kHardMaxGridAxis);
    limits.staleCellAfterObservations =
        std::clamp(limits.staleCellAfterObservations, 0, kHardMaxAgeObservations);
    limits.maxMissingObservations =
        std::clamp(limits.maxMissingObservations, 0, kHardMaxAgeObservations);
    limits.stableAfterObservations =
        std::clamp(limits.stableAfterObservations, 1, kHardMaxAgeObservations);
    if (!std::isfinite(limits.matchIou) || limits.matchIou <= 0.0 || limits.matchIou > 1.0)
        limits.matchIou = 0.5;
    if (limits.maxOccurrenceId == 0)
        limits.maxOccurrenceId = std::numeric_limits<std::uint64_t>::max();
    return limits;
}

std::uint64_t CeilDiv(std::uint64_t value, std::uint64_t divisor)
{
    return divisor == 0 ? 0 : value / divisor + (value % divisor != 0 ? 1 : 0);
}

} // namespace

RegionShadowPlanner::RegionShadowPlanner(RegionShadowLimits limits)
    : limits_(ClampLimits(std::move(limits)))
{
    cells_.resize(limits_.gridColumns * limits_.gridRows);
}

RegionShadowPlanner::~RegionShadowPlanner() = default;

int RegionShadowPlanner::RetainedOccurrences() const
{
    return int(occurrences_.size());
}

int RegionShadowPlanner::EffectiveReservedQuota() const
{
    return std::min(limits_.reservedCoverageQuota, limits_.maxSelectionsPerPlan);
}

bool RegionShadowPlanner::CellDue(const Cell& cell) const
{
    if (!cell.selectedOnce) return true;
    return acceptedObservations_ - cell.lastSelectedObservation >=
           std::uint64_t(limits_.staleCellAfterObservations);
}

std::uint64_t RegionShadowPlanner::OccurrenceAgeAnchor(const Occurrence& occurrence)
{
    // Never selected: age from creation, not from 0. Anchoring fresh
    // occurrences at "never" would let a constant stream of new words
    // permanently outrank waiting grid cells in the reserved quota.
    return occurrence.lastSelectedObservation == 0 ? occurrence.firstSeenObservation
                                                   : occurrence.lastSelectedObservation;
}

std::uint64_t RegionShadowPlanner::CellAgeAnchor(const Cell& cell) const
{
    if (!cell.selectedOnce) return 0; // initial cells are the oldest debt
    // Age from when the cell (re)became due, not from its last selection:
    // a cell that was not due in an earlier plan must not jump ahead of
    // regions already waiting in that plan when it later becomes due.
    return SaturatingAdd(cell.lastSelectedObservation,
                         std::uint64_t(limits_.staleCellAfterObservations));
}

void RegionShadowPlanner::ClearTracking()
{
    occurrences_.clear();
    previousSelectedIds_.clear();
    for (Cell& cell : cells_) cell = {};
    hasScene_ = false;
    lastAuthority_ = 0;
    lastJobId_ = 0;
    lastAcquisition_ = 0;
    lastTimestampMs_ = 0;
    acceptedObservations_ = 0;
}

void RegionShadowPlanner::Reset(RegionResetReason reason)
{
    ClearTracking();
    plannerEpoch_ = SaturatingIncrement(plannerEpoch_);
    ++stats_.resets;
    pendingResetReason_ = reason;
}

RegionCoverage RegionShadowPlanner::CurrentCoverage(int selectedRegions) const
{
    RegionCoverage coverage;
    coverage.reservedQuota = EffectiveReservedQuota();
    coverage.gridCellCount = int(cells_.size());
    coverage.acceptedObservations = acceptedObservations_;
    coverage.selectedRegions = selectedRegions;
    int dueCells = 0;
    std::uint64_t oldestAge = 0;
    for (const Cell& cell : cells_) {
        if (cell.candidateWordsLatest > 0) ++coverage.cellsWithCandidates;
        if (!CellDue(cell)) continue;
        ++dueCells;
        oldestAge = std::max(oldestAge, acceptedObservations_ - CellAgeAnchor(cell));
    }
    for (const Occurrence& occurrence : occurrences_) {
        oldestAge =
            std::max(oldestAge, acceptedObservations_ - OccurrenceAgeAnchor(occurrence));
    }
    coverage.dueGridCells = dueCells;
    coverage.eligibleRegions = int(occurrences_.size()) + dueCells;
    coverage.coverageDebt = std::max(coverage.eligibleRegions - selectedRegions, 0);
    coverage.worstCasePlansToCoverAll =
        CeilDiv(std::uint64_t(coverage.coverageDebt), std::uint64_t(coverage.reservedQuota));
    coverage.coverageDeadlineObservation =
        SaturatingAdd(acceptedObservations_, coverage.worstCasePlansToCoverAll);
    coverage.oldestUnselectedAgePlans = coverage.coverageDebt > 0 ? oldestAge : 0;
    return coverage;
}

RegionShadowPlan RegionShadowPlanner::Reject(RegionRejectReason reason,
                                             const RecognitionResult& result,
                                             std::optional<std::uint64_t> acquisition)
{
    ++stats_.rejectedObservations;
    RegionShadowPlan plan;
    plan.status = RegionPlanStatus::kRejected;
    plan.rejectReason = reason;
    plan.resetReason = pendingResetReason_;
    pendingResetReason_ = RegionResetReason::kNone;
    plan.planGeneration = planGeneration_;
    plan.plannerEpoch = plannerEpoch_;
    plan.stamp = result.stamp;
    plan.jobId = result.jobId;
    plan.authority = result.authority;
    plan.evidenceAcquisition = acquisition;
    plan.comparison.retainedOccurrences = int(occurrences_.size());
    plan.coverage = CurrentCoverage(0);
    plan.limits = limits_;
    return plan;
}

bool RegionalPlanMatches(const RegionShadowPlan& plan, const SourceStamp& current,
                         std::uint64_t authority)
{
    const auto acquisition = EvidenceAcquisition(current);
    const auto& a = plan.stamp.viewTransform;
    const auto& b = current.viewTransform;
    return plan.status == RegionPlanStatus::kAccepted && plan.jobId &&
        plan.authority == authority && current.SameScene(plan.stamp) &&
        current.viewportWidth == plan.stamp.viewportWidth && current.viewportHeight == plan.stamp.viewportHeight &&
        a.valid && b.valid && a.sourceX == b.sourceX && a.sourceY == b.sourceY &&
        a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight &&
        a.destinationX == b.destinationX && a.destinationY == b.destinationY &&
        a.destinationWidth == b.destinationWidth && a.destinationHeight == b.destinationHeight &&
        current.temporallySmoothed == plan.stamp.temporallySmoothed &&
        current.historyEpoch == plan.stamp.historyEpoch && current.temporalAlpha == plan.stamp.temporalAlpha &&
        (current.holdDisplay == HoldDisplay::NotHeld || current.holdDisplay == HoldDisplay::Live) &&
        acquisition && plan.evidenceAcquisition &&
        *acquisition > *plan.evidenceAcquisition && current.frameSeq > plan.stamp.frameSeq &&
        current.timestampMs >= plan.stamp.timestampMs;
}

std::optional<DetailFocus> RegionalDetailFocus(const RegionShadowPlan& plan,
                                             const SourceStamp& current,
                                             std::optional<QRectF> focus)
{
    if (!RegionalPlanMatches(plan, current, plan.authority)) return {};
    if (!current.viewportWidth || !current.viewportHeight ||
        current.viewportWidth > 8192 || current.viewportHeight > 8192) return {};
    const auto finite = [](const QRectF& r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) &&
               std::isfinite(r.height()) && !r.isEmpty();
    };
    if (focus && (!finite(*focus) || !focus->intersects(QRectF(0, 0, 1, 1)))) focus.reset();
    const RegionPlannedSelection* selected = nullptr;
    double best = std::numeric_limits<double>::max();
    for (const auto& region : plan.selections) {
        if (!finite(region.sceneRect)) continue;
        // Plan order resolves ties; the focus changes selection, never evidence.
        const auto delta = region.sceneRect.center() - focus.value_or(QRectF(0, 0, 1, 1)).center();
        const double score = focus ? delta.x()*delta.x() + delta.y()*delta.y() : 0;
        if (!selected || score < best) { selected = &region; best = score; }
    }
    if (!selected) return {};
    const QSize source(static_cast<int>(current.viewportWidth), static_cast<int>(current.viewportHeight));
    const QRectF pixels = SceneRectToView(current.viewTransform, source.width(), source.height(), selected->sceneRect);
    if (!finite(pixels) || !std::isfinite(pixels.center().x()) || !std::isfinite(pixels.center().y())) return {};
    // Bound metadata in double precision before integer conversion, including
    // finite but extreme selections. The source context remains at most 480.
    const int width = std::min(source.width(), static_cast<int>(std::ceil(std::clamp(pixels.width() + 96,160.0,480.0))));
    const int height = std::min(source.height(), static_cast<int>(std::ceil(std::clamp(pixels.height() + 64,96.0,480.0))));
    const int left = static_cast<int>(std::lround(std::clamp(pixels.center().x() - width/2.0,0.0,double(source.width()-width))));
    const int top = static_cast<int>(std::lround(std::clamp(pixels.center().y() - height/2.0,0.0,double(source.height()-height))));
    // About 720 crop pixels means the existing engine's aligned 704 edge,
    // not a 1280x720 frame. One ULP upward represents the intended 22/15:
    // ceil(704 / (704/480)) otherwise rounds to 481 in double precision and
    // lets a backend's repeated DetailRegion pass enlarge the source crop.
    const double magnification = std::nextafter(704.0/480.0,std::numeric_limits<double>::infinity());
    DetailFocus result{QRect(left, top, width, height), magnification, 704, true};
    result.regionPx = DetailRegion(source, result);
    if (result.regionPx.isEmpty() || result.regionPx.width() > 480 || result.regionPx.height() > 480 ||
        result.regionPx == QRect(QPoint{}, source)) return {};
    return result;
}

RegionShadowPlan RegionShadowPlanner::Observe(const RecognitionResult& validatedFull,
                                              std::optional<QRectF> requestedSceneRect)
{
    planGeneration_ = SaturatingIncrement(planGeneration_);
    ++stats_.plansEmitted;
    const RecognitionResult& result = validatedFull;
    const auto acquisition = EvidenceAcquisition(result.stamp);

    if (result.output.cancelled)
        return Reject(RegionRejectReason::kResultCancelled, result, acquisition);
    if (result.output.compositeInput)
        return Reject(RegionRejectReason::kCompositeInput, result, acquisition);
    // An ok flag cannot rehabilitate faulted input or a still-preparing
    // backend mix: neither is a settled full result the shadow may learn
    // from, even though production may show a complete fallback output.
    if (!result.output.ok || result.output.inputFault || result.output.backendPreparing ||
        !result.output.detailRegionPx.isEmpty())
        return Reject(RegionRejectReason::kResultNotOk, result, acquisition);
    if (result.jobId == 0 || result.authority == 0)
        return Reject(RegionRejectReason::kMissingJobTicket, result, acquisition);
    const ViewTransform& transform = result.stamp.viewTransform;
    if (!transform.valid || !std::isfinite(transform.sourceX) ||
        !std::isfinite(transform.sourceY) || !std::isfinite(transform.sourceWidth) ||
        !std::isfinite(transform.sourceHeight) || !std::isfinite(transform.destinationX) ||
        !std::isfinite(transform.destinationY) ||
        !std::isfinite(transform.destinationWidth) ||
        !std::isfinite(transform.destinationHeight) ||
        transform.sourceWidth <= 0.0f || transform.sourceHeight <= 0.0f ||
        transform.destinationWidth <= 0.0f || transform.destinationHeight <= 0.0f ||
        result.stamp.viewportWidth == 0 || result.stamp.viewportHeight == 0)
        return Reject(RegionRejectReason::kInvalidStamp, result, acquisition);
    if (result.prepared.size() != result.output.lines.size())
        return Reject(RegionRejectReason::kUnpreparedLines, result, acquisition);
    if (result.prepared.size() > limits_.maxLines)
        return Reject(RegionRejectReason::kLineLimitExceeded, result, acquisition);
    qsizetype totalWords = 0;
    qsizetype outputUtf16Units = 0;
    qsizetype preparedUtf16Units = 0;
    for (qsizetype lineIndex = 0; lineIndex < result.prepared.size(); ++lineIndex) {
        // Preparation is word-parallel (PrepareLines maps every recognized
        // word exactly once); a prepared line whose word count disagrees
        // with its output line was not produced from this result.
        if (result.prepared[lineIndex].words.size() !=
            result.output.lines[lineIndex].words.size())
            return Reject(RegionRejectReason::kUnpreparedLines, result, acquisition);
        totalWords += result.prepared[lineIndex].words.size();
        if (totalWords > limits_.maxWords)
            return Reject(RegionRejectReason::kWordLimitExceeded, result, acquisition);
        for (qsizetype wordIndex = 0;
             wordIndex < result.prepared[lineIndex].words.size(); ++wordIndex) {
            const qsizetype outputUnits =
                result.output.lines[lineIndex].words[wordIndex].text.size();
            const qsizetype preparedUnits =
                result.prepared[lineIndex].words[wordIndex].text.size();
            // Size reads only: cap both representations, including omitted
            // spans, before trimming, comparisons, storage or scene resets.
            if (outputUnits > limits_.maxWordUtf16Units ||
                preparedUnits > limits_.maxWordUtf16Units ||
                outputUnits > limits_.maxTotalUtf16Units - outputUtf16Units ||
                preparedUnits > limits_.maxTotalUtf16Units - preparedUtf16Units)
                return Reject(RegionRejectReason::kTextLimitExceeded, result, acquisition);
            outputUtf16Units += outputUnits;
            preparedUtf16Units += preparedUnits;
        }
    }
    for (qsizetype lineIndex = 0; lineIndex < result.prepared.size(); ++lineIndex) {
        for (qsizetype wordIndex = 0;
             wordIndex < result.prepared[lineIndex].words.size(); ++wordIndex) {
            if (!PreparedWordMatchesSource(result.prepared[lineIndex].words[wordIndex],
                    result.output.lines[lineIndex].words[wordIndex], result.stamp))
                return Reject(RegionRejectReason::kUnpreparedLines, result, acquisition);
        }
    }
    if (!acquisition)
        return Reject(RegionRejectReason::kUnknownAcquisition, result, acquisition);

    RegionResetReason resetReason = RegionResetReason::kNone;
    if (hasScene_) {
        if (result.authority < lastAuthority_)
            return Reject(RegionRejectReason::kStaleAuthority, result, acquisition);
        if (result.stamp.sourceEpoch < sceneSourceEpoch_)
            return Reject(RegionRejectReason::kStaleSource, result, acquisition);
        if (result.stamp.sourceEpoch == sceneSourceEpoch_ &&
            result.stamp.geometryGeneration < sceneGeometryGeneration_)
            return Reject(RegionRejectReason::kStaleGeometry, result, acquisition);
        const bool authorityAdvanced = result.authority > lastAuthority_;
        const bool sourceAdvanced = result.stamp.sourceEpoch > sceneSourceEpoch_;
        const bool geometryAdvanced = !sourceAdvanced &&
            result.stamp.geometryGeneration > sceneGeometryGeneration_;
        // Monotonic tickets are checked before any automatic reset, so a
        // replayed result cannot forge a fresh scene by bumping an epoch.
        // Job ids and readback timestamps are global clocks and must
        // advance across every reset; the evidence acquisition is a
        // per-source clock, so only a source advance may restart it.
        if (result.jobId <= lastJobId_)
            return Reject(RegionRejectReason::kStaleOrOutOfOrderJob, result, acquisition);
        if (result.stamp.timestampMs < lastTimestampMs_)
            return Reject(RegionRejectReason::kOutOfOrderTimestamp, result, acquisition);
        if (!sourceAdvanced) {
            if (*acquisition == lastAcquisition_)
                return Reject(RegionRejectReason::kDuplicateAcquisition, result, acquisition);
            if (*acquisition < lastAcquisition_)
                return Reject(RegionRejectReason::kOutOfOrderAcquisition, result, acquisition);
        }
        if (authorityAdvanced || sourceAdvanced || geometryAdvanced) {
            ClearTracking();
            plannerEpoch_ = SaturatingIncrement(plannerEpoch_);
            ++stats_.resets;
            resetReason = authorityAdvanced ? RegionResetReason::kAuthorityChanged
                        : sourceAdvanced    ? RegionResetReason::kSourceChanged
                                            : RegionResetReason::kGeometryChanged;
        }
    }

    if (resetReason == RegionResetReason::kNone) resetReason = pendingResetReason_;
    pendingResetReason_ = RegionResetReason::kNone;

    // Accepted: this fresh acquisition advances the evidence clock. Held
    // duplicates and rejected results above never reach this point, and
    // pan/zoom-only transform changes do not count as acquisitions.
    acceptedObservations_ = SaturatingIncrement(acceptedObservations_);
    const std::uint64_t observation = acceptedObservations_;
    ++stats_.acceptedObservations;

    RegionShadowPlan plan;
    plan.status = RegionPlanStatus::kAccepted;
    plan.resetReason = resetReason;
    plan.planGeneration = planGeneration_;
    plan.plannerEpoch = plannerEpoch_;
    plan.stamp = result.stamp;
    plan.jobId = result.jobId;
    plan.authority = result.authority;
    plan.evidenceAcquisition = acquisition;
    plan.limits = limits_;
    RegionShadowComparison& comparison = plan.comparison;

    struct FreshWord {
        QString text;
        QRectF rect;
        bool unverified{};
    };
    QVector<FreshWord> fresh;
    fresh.reserve(int(totalWords));
    const bool lineMatchesParallel =
        result.currentInputMatches.size() == result.prepared.size();
    for (qsizetype lineIndex = 0; lineIndex < result.prepared.size(); ++lineIndex) {
        const bool unverified = result.stamp.temporallySmoothed &&
            (!lineMatchesParallel || !result.currentInputMatches[int(lineIndex)]);
        for (const PreparedWord& word : result.prepared[int(lineIndex)].words) {
            if (word.disposition != TokenDisposition::kCandidate) {
                ++comparison.fullOmittedWords;
                continue;
            }
            bool clamped = false;
            const auto rect = NormalizeSceneRect(word.sceneRect, clamped);
            if (word.text.isEmpty() || !rect || !std::isfinite(word.confidence) ||
                word.confidence < 0.0 || word.confidence > 1.0001) {
                ++comparison.invalidWords;
                continue;
            }
            if (clamped) ++comparison.clampedWords;
            // Own only the checked character range. Sharing a QString
            // whose small size hides a much larger reserved allocation
            // would defeat the retained-text memory bound.
            fresh.push_back({QString(word.text.constData(), word.text.size()),
                             *rect, unverified});
        }
    }
    comparison.fullCandidateWords = int(fresh.size());

    // Conservative within-job duplicate suppression: exact normalized
    // geometry and exact text only. Distinct occurrences (including the
    // same string elsewhere) are never suppressed.
    QVector<int> order;
    order.reserve(int(fresh.size()));
    for (int i = 0; i < fresh.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const QRectF& ra = fresh[a].rect;
        const QRectF& rb = fresh[b].rect;
        if (ra.x() != rb.x()) return ra.x() < rb.x();
        if (ra.y() != rb.y()) return ra.y() < rb.y();
        if (ra.width() != rb.width()) return ra.width() < rb.width();
        if (ra.height() != rb.height()) return ra.height() < rb.height();
        if (fresh[a].text != fresh[b].text) return fresh[a].text < fresh[b].text;
        return a < b;
    });
    QVector<bool> suppressed(fresh.size(), false);
    for (int i = 1; i < order.size(); ++i) {
        const FreshWord& earlier = fresh[order[i - 1]];
        const int current = order[i];
        if (fresh[current].rect == earlier.rect && fresh[current].text == earlier.text) {
            suppressed[current] = true;
            ++comparison.duplicatesSuppressed;
        }
    }
    stats_.duplicatesSuppressed += std::uint64_t(comparison.duplicatesSuppressed);
    QVector<FreshWord> words;
    words.reserve(int(fresh.size()) - comparison.duplicatesSuppressed);
    for (int i = 0; i < fresh.size(); ++i)
        if (!suppressed[i]) words.push_back(std::move(fresh[i]));

    // One-to-one spatial matching against retained occurrences. Text is
    // never an identity key: geometry pairs occurrences, then full-OCR text
    // decides whether the occurrence repeated or changed.
    struct Pair {
        double iou{};
        int occurrenceIndex{};
        int wordIndex{};
    };
    // Words are bucketed into every grid cell they cover; any intersecting
    // occurrence/word pair shares a covered cell, so no candidate is lost.
    // Each occurrence keeps at most its eight best overlaps, which bounds
    // pair storage even for adversarial degenerate geometry.
    QVector<QVector<int>> wordsByCell(cells_.size());
    for (int wi = 0; wi < words.size(); ++wi)
        ForEachCoveredCell(limits_, words[wi].rect,
                           [&](int cellIndex) { wordsByCell[cellIndex].push_back(wi); });
    QVector<Pair> pairs;
    pairs.reserve(occurrences_.size() * 8);
    QVector<int> visitedAt(words.size(), -1);
    const auto betterOverlap = [](const Pair& a, const Pair& b) {
        if (a.iou != b.iou) return a.iou > b.iou;
        return a.wordIndex < b.wordIndex;
    };
    for (int oi = 0; oi < occurrences_.size(); ++oi) {
        std::array<Pair, 8> best;
        int bestCount = 0;
        ForEachCoveredCell(limits_, occurrences_[oi].sceneRect, [&](int cellIndex) {
            for (int wi : wordsByCell[cellIndex]) {
                if (visitedAt[wi] == oi) continue;
                visitedAt[wi] = oi;
                const double iou =
                    IntersectionOverUnion(occurrences_[oi].sceneRect, words[wi].rect);
                if (!(iou >= limits_.matchIou)) continue;
                const Pair candidate{iou, oi, wi};
                if (bestCount == 8 && !betterOverlap(candidate, best.back())) continue;
                // Keep the exact prefix of the previous full sort, including
                // word-index tie breaks, without storing rejected overlaps.
                const auto position = std::lower_bound(
                    best.begin(), best.begin() + bestCount, candidate, betterOverlap);
                const int insertAt = int(position - best.begin());
                for (int i = std::min(bestCount, 7); i > insertAt; --i)
                    best[i] = best[i - 1];
                best[insertAt] = candidate;
                bestCount = std::min(bestCount + 1, 8);
            }
        });
        for (int i = 0; i < bestCount; ++i) pairs.push_back(best[i]);
    }
    std::sort(pairs.begin(), pairs.end(), [&](const Pair& a, const Pair& b) {
        if (a.iou != b.iou) return a.iou > b.iou;
        if (occurrences_[a.occurrenceIndex].id != occurrences_[b.occurrenceIndex].id)
            return occurrences_[a.occurrenceIndex].id < occurrences_[b.occurrenceIndex].id;
        return a.wordIndex < b.wordIndex;
    });
    QVector<bool> occurrenceMatched(occurrences_.size(), false);
    QVector<bool> wordMatched(words.size(), false);
    const auto wasSelectedPreviously = [&](std::uint64_t id) {
        return std::find(previousSelectedIds_.begin(), previousSelectedIds_.end(), id) !=
               previousSelectedIds_.end();
    };
    for (const Pair& pair : pairs) {
        if (occurrenceMatched[pair.occurrenceIndex] || wordMatched[pair.wordIndex]) continue;
        occurrenceMatched[pair.occurrenceIndex] = true;
        wordMatched[pair.wordIndex] = true;
        Occurrence& occurrence = occurrences_[pair.occurrenceIndex];
        const FreshWord& word = words[pair.wordIndex];
        const bool wasStable = occurrence.status == RegionOccurrenceStatus::kStable;
        const bool sameText = occurrence.text == word.text;
        occurrence.observations = SaturatingIncrement(occurrence.observations);
        occurrence.lastSeenObservation = observation;
        occurrence.lastSeenAcquisition = *acquisition;
        occurrence.sceneRect = word.rect;
        occurrence.smoothedUnverified = word.unverified;
        if (!sameText) {
            occurrence.text = word.text;
            occurrence.status = RegionOccurrenceStatus::kChanged;
            occurrence.consecutiveSameText = word.unverified ? 0 : 1;
            ++comparison.changedOccurrences;
            ++stats_.changedDetected;
            if (wasStable && !wasSelectedPreviously(occurrence.id)) {
                ++comparison.changedPreviouslyStableUnselected;
                ++stats_.changedPreviouslyStableUnselected;
            }
        } else {
            if (word.unverified) {
                // Same text on blended pixels is not independent verified
                // repetition: it demotes to candidate and restarts the
                // streak, even if the occurrence was previously stable.
                // Stability must be rebuilt from verified acquisitions.
                occurrence.consecutiveSameText = 0;
            } else {
                occurrence.consecutiveSameText =
                    SaturatingIncrement(occurrence.consecutiveSameText);
                if (wasStable) {
                    ++comparison.repeatStableCandidates;
                    ++stats_.repeatStableCandidates;
                }
            }
            occurrence.status =
                occurrence.consecutiveSameText >= std::uint64_t(limits_.stableAfterObservations)
                    ? RegionOccurrenceStatus::kStable
                    : RegionOccurrenceStatus::kCandidate;
        }
        ++comparison.matchedOccurrences;
    }

    // Retained occurrences absent from this full result become missing and
    // are evicted by age; missing regions stay eligible for recheck until
    // then, so a temporary detector miss cannot erase coverage debt.
    for (int oi = occurrences_.size() - 1; oi >= 0; --oi) {
        if (occurrenceMatched[oi]) continue;
        Occurrence& occurrence = occurrences_[oi];
        occurrence.status = RegionOccurrenceStatus::kMissing;
        occurrence.consecutiveSameText = 0; // a gap breaks the repetition streak
        if (observation - occurrence.lastSeenObservation >
            std::uint64_t(limits_.maxMissingObservations)) {
            occurrences_.remove(oi);
            ++comparison.evictedOccurrences;
            ++stats_.occurrencesEvicted;
        } else {
            ++comparison.missingOccurrences;
        }
    }

    // Make room for fresh occurrences at the finite cap by evicting the
    // oldest missing occurrences first; never evict this observation.
    struct MissingCandidate {
        std::uint64_t lastSeenObservation{};
        std::uint64_t id{};
    };
    QVector<MissingCandidate> rankedMissing;
    rankedMissing.reserve(comparison.missingOccurrences);
    for (const Occurrence& occurrence : occurrences_) {
        if (occurrence.status == RegionOccurrenceStatus::kMissing)
            rankedMissing.push_back({occurrence.lastSeenObservation, occurrence.id});
    }
    std::sort(rankedMissing.begin(), rankedMissing.end(),
              [](const MissingCandidate& a, const MissingCandidate& b) {
                  if (a.lastSeenObservation != b.lastSeenObservation)
                      return a.lastSeenObservation < b.lastSeenObservation;
                  return a.id < b.id;
              });
    qsizetype nextMissing = 0;
    const auto evictOldestMissing = [&]() {
        if (nextMissing == rankedMissing.size()) return false;
        const std::uint64_t id = rankedMissing[nextMissing++].id;
        // Monotonic minting and order-preserving removals keep occurrences
        // sorted by id, including while fresh words are appended below.
        const auto victim = std::lower_bound(
            occurrences_.cbegin(), occurrences_.cend(), id,
            [](const Occurrence& occurrence, std::uint64_t value) {
                return occurrence.id < value;
            });
        const int victimIndex = int(victim - occurrences_.cbegin());
        occurrences_.remove(victimIndex);
        ++comparison.evictedOccurrences;
        --comparison.missingOccurrences;
        ++stats_.occurrencesEvicted;
        return true;
    };
    for (int wi = 0; wi < words.size(); ++wi) {
        if (wordMatched[wi]) continue;
        while (occurrences_.size() >= limits_.maxOccurrences && evictOldestMissing()) {
        }
        if (occurrences_.size() >= limits_.maxOccurrences || nextOccurrenceId_ == 0 ||
            nextOccurrenceId_ > limits_.maxOccurrenceId) {
            ++comparison.overflowDropped;
            plan.truncated = true;
            continue;
        }
        Occurrence created;
        created.id = nextOccurrenceId_;
        // Handing out the highest permitted id exhausts minting: park the
        // counter on the reserved 0 sentinel so no later word, in this or
        // any later observation or reset epoch, can wrap to or reuse an
        // id. Below the limit the increment cannot overflow.
        nextOccurrenceId_ =
            created.id >= limits_.maxOccurrenceId ? 0 : created.id + 1;
        created.text = words[wi].text;
        created.sceneRect = words[wi].rect;
        created.observations = 1;
        created.consecutiveSameText = words[wi].unverified ? 0 : 1;
        created.firstSeenObservation = observation;
        created.lastSeenObservation = observation;
        created.lastSeenAcquisition = *acquisition;
        created.smoothedUnverified = words[wi].unverified;
        created.status =
            created.consecutiveSameText >= std::uint64_t(limits_.stableAfterObservations)
                ? RegionOccurrenceStatus::kStable
                : RegionOccurrenceStatus::kCandidate;
        occurrences_.push_back(std::move(created));
        ++comparison.newOccurrences;
        ++stats_.occurrencesCreated;
    }
    comparison.retainedOccurrences = int(occurrences_.size());

    for (Cell& cell : cells_) cell.candidateWordsLatest = 0;
    for (const FreshWord& word : words) {
        ForEachCoveredCell(limits_, word.rect, [&](int cellIndex) {
            Cell& cell = cells_[cellIndex];
            ++cell.candidateWordsLatest;
            cell.everCandidate = true;
            cell.lastCandidateAcquisition = *acquisition;
        });
    }

    std::optional<QRectF> focus;
    if (requestedSceneRect) {
        bool clamped = false;
        focus = NormalizeSceneRect(*requestedSceneRect, clamped);
        plan.focusApplied = focus.has_value();
        plan.focusIgnored = !focus.has_value();
    }
    const auto inFocus = [&](const QRectF& rect) {
        return focus && !rect.intersected(*focus).isEmpty();
    };

    struct Item {
        bool isCell{};
        int occurrenceIndex{-1};
        int cellIndex{-1};
        std::uint64_t sortId{};
        std::uint64_t ageAnchorObservation{};
        // True when the anchor is a selection at that observation rather
        // than a creation/due time. At equal anchors, regions that were
        // already waiting outrank regions selected at that observation,
        // so a just-selected region cannot rejoin ahead of the debt a
        // stated coverage deadline was computed from.
        bool anchorIsSelection{};
        RegionPriorityClass priority{RegionPriorityClass::kStableRefresh};
        bool selected{};
        RegionSelectionReason reason{RegionSelectionReason::kPriority};
    };
    QVector<Item> items;
    items.reserve(int(occurrences_.size() + cells_.size()));
    for (int oi = 0; oi < occurrences_.size(); ++oi) {
        const Occurrence& occurrence = occurrences_[oi];
        RegionPriorityClass priority = RegionPriorityClass::kStableRefresh;
        if (inFocus(occurrence.sceneRect)) {
            priority = RegionPriorityClass::kRequestedFocus;
        } else if (occurrence.status == RegionOccurrenceStatus::kChanged) {
            priority = RegionPriorityClass::kChanged;
        } else if (occurrence.status == RegionOccurrenceStatus::kCandidate) {
            priority = RegionPriorityClass::kCandidate;
        } else if (occurrence.status == RegionOccurrenceStatus::kMissing) {
            priority = RegionPriorityClass::kMissingRecheck;
        }
        items.push_back({false, oi, -1, occurrence.id, OccurrenceAgeAnchor(occurrence),
                         occurrence.lastSelectedObservation != 0, priority, false,
                         RegionSelectionReason::kPriority});
    }
    int dueCells = 0;
    for (int ci = 0; ci < cells_.size(); ++ci) {
        if (!CellDue(cells_[ci])) continue;
        ++dueCells;
        const QRectF rect = CellSceneRect(limits_, ci);
        items.push_back({true, -1, ci, std::uint64_t(ci), CellAgeAnchor(cells_[ci]),
                         cells_[ci].selectedOnce && limits_.staleCellAfterObservations == 0,
                         inFocus(rect) ? RegionPriorityClass::kRequestedFocus
                                       : RegionPriorityClass::kUnobservedCoverage,
                         false, RegionSelectionReason::kPriority});
    }

    // Reserved quota first: the globally oldest eligible regions by
    // effective age anchor are taken before any priority spending, so a
    // constant requested focus and a constant stream of fresh occurrences
    // can delay but never starve coverage. Fresh occurrences anchor at
    // this observation, behind every waiting region; on an exact anchor
    // tie a grid cell wins over an occurrence, so same-age churn cannot
    // push cells behind known text either. The remaining budget then
    // follows focus, changed, candidate, unobserved-cell and refresh
    // priority, oldest anchor first within each class.
    const int budget = std::min(limits_.maxSelectionsPerPlan, int(items.size()));
    const int reserved = std::min(EffectiveReservedQuota(), budget);
    QVector<int> byAge;
    byAge.reserve(int(items.size()));
    for (int i = 0; i < items.size(); ++i) byAge.push_back(i);
    std::sort(byAge.begin(), byAge.end(), [&](int a, int b) {
        if (items[a].ageAnchorObservation != items[b].ageAnchorObservation)
            return items[a].ageAnchorObservation < items[b].ageAnchorObservation;
        if (items[a].anchorIsSelection != items[b].anchorIsSelection)
            return !items[a].anchorIsSelection;
        if (items[a].isCell != items[b].isCell) return items[a].isCell;
        return items[a].sortId < items[b].sortId;
    });
    int selectedCount = 0;
    for (int i = 0; i < reserved; ++i) {
        items[byAge[i]].selected = true;
        items[byAge[i]].reason = RegionSelectionReason::kCoverageReserved;
        ++selectedCount;
    }
    QVector<int> byPriority;
    for (int i = 0; i < items.size(); ++i)
        if (!items[i].selected) byPriority.push_back(i);
    std::sort(byPriority.begin(), byPriority.end(), [&](int a, int b) {
        if (items[a].priority != items[b].priority) return items[a].priority < items[b].priority;
        if (items[a].ageAnchorObservation != items[b].ageAnchorObservation)
            return items[a].ageAnchorObservation < items[b].ageAnchorObservation;
        if (items[a].anchorIsSelection != items[b].anchorIsSelection)
            return !items[a].anchorIsSelection;
        if (items[a].isCell != items[b].isCell) return !items[a].isCell;
        return items[a].sortId < items[b].sortId;
    });
    for (int index : byPriority) {
        if (selectedCount >= budget) break;
        items[index].selected = true;
        ++selectedCount;
    }

    std::uint64_t oldestUnselectedAge = 0;
    previousSelectedIds_.clear();
    for (const Item& item : items) {
        if (!item.selected) {
            oldestUnselectedAge =
                std::max(oldestUnselectedAge, observation - item.ageAnchorObservation);
            continue;
        }
        RegionPlannedSelection selection;
        selection.isGridCell = item.isCell;
        selection.priority = item.priority;
        selection.reason = item.reason;
        if (item.isCell) {
            Cell& cell = cells_[item.cellIndex];
            cell.selectedOnce = true;
            cell.lastSelectedObservation = observation;
            selection.cellIndex = item.cellIndex;
            selection.sceneRect = CellSceneRect(limits_, item.cellIndex);
            selection.status = cell.candidateWordsLatest > 0
                                   ? RegionOccurrenceStatus::kCandidate
                                   : RegionOccurrenceStatus::kMissing;
            selection.lastSeenAcquisition = cell.lastCandidateAcquisition;
        } else {
            Occurrence& occurrence = occurrences_[item.occurrenceIndex];
            occurrence.lastSelectedObservation = observation;
            previousSelectedIds_.push_back(occurrence.id);
            selection.occurrenceId = occurrence.id;
            selection.sceneRect = occurrence.sceneRect;
            selection.status = occurrence.status;
            selection.lastSeenAcquisition = occurrence.lastSeenAcquisition;
            selection.observations = occurrence.observations;
            selection.smoothedUnverified = occurrence.smoothedUnverified;
        }
        plan.selections.push_back(selection);
    }
    std::sort(plan.selections.begin(), plan.selections.end(),
              [](const RegionPlannedSelection& a, const RegionPlannedSelection& b) {
                  if (a.priority != b.priority) return a.priority < b.priority;
                  if (a.isGridCell != b.isGridCell) return !a.isGridCell;
                  return a.isGridCell ? a.cellIndex < b.cellIndex
                                      : a.occurrenceId < b.occurrenceId;
              });

    RegionCoverage& coverage = plan.coverage;
    coverage.reservedQuota = EffectiveReservedQuota();
    coverage.gridCellCount = int(cells_.size());
    coverage.acceptedObservations = acceptedObservations_;
    coverage.selectedRegions = int(plan.selections.size());
    coverage.dueGridCells = dueCells;
    coverage.eligibleRegions = int(items.size());
    coverage.coverageDebt = coverage.eligibleRegions - coverage.selectedRegions;
    coverage.worstCasePlansToCoverAll =
        CeilDiv(std::uint64_t(coverage.coverageDebt), std::uint64_t(coverage.reservedQuota));
    coverage.coverageDeadlineObservation =
        SaturatingAdd(acceptedObservations_, coverage.worstCasePlansToCoverAll);
    coverage.oldestUnselectedAgePlans =
        coverage.coverageDebt > 0 ? oldestUnselectedAge : 0;
    for (const Cell& cell : cells_)
        if (cell.candidateWordsLatest > 0) ++coverage.cellsWithCandidates;

    hasScene_ = true;
    sceneSourceEpoch_ = result.stamp.sourceEpoch;
    sceneGeometryGeneration_ = result.stamp.geometryGeneration;
    lastAuthority_ = result.authority;
    lastJobId_ = result.jobId;
    lastAcquisition_ = *acquisition;
    lastTimestampMs_ = result.stamp.timestampMs;
    return plan;
}

} // namespace okuflow::uct
