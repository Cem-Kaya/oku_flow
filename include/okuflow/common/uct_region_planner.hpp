#pragma once

#include "okuflow/common/ultra_clear_text.hpp"

#include <QRectF>
#include <QVector>

#include <cstdint>
#include <limits>
#include <optional>

namespace okuflow::uct {

// Shadow regional planner for Ultra Clear Text (roadmap Design B, stage 4).
//
// This is a pure-metadata observer of already-computed full OCR results. It
// keeps persistent source-space spatial occurrence identities, a bounded
// coverage grid, requested-focus priority and explicit coverage debt, and
// emits an immutable *hypothetical* work plan per accepted result. It never
// skips, reuses, publishes or mutates recognition output: full OCR stays
// authoritative and every plan says so (`authoritativeAction`). Similarity
// or repetition recorded here is bookkeeping for shadow comparison, never
// pixel proof that text is unchanged; in particular it does not revive the
// NCC change verifier measured unsafe in docs/uct_character_verification.md.
//
// Threading and purity contract: one planner is confined to the single OCR
// worker thread that owns it (no internal locking, threads, I/O, timers or
// wall clock). The only time source is `SourceStamp::timestampMs` of the
// observed result. `Observe` takes its input by const reference and never
// writes to it. Call `Observe` only with a validated full result whose
// `prepared` lines are parallel to `output.lines`, word for word (i.e.
// after scheduler preparation / `PrepareLines`); anything else is rejected
// fail-closed. Text, unchanged confidence and mapped geometry are checked;
// conservative candidate omissions are allowed without guessing the
// scheduler's size threshold, but source omissions cannot be promoted.
// A result flagged `inputFault` or `backendPreparing` is
// rejected as not-ok for shadow purposes even when `output.ok` is set:
// faulted input is not full evidence, and a preparing backend means the
// authoritative backend mix has not settled.
//
// Identity contract: occurrences are individual candidate words matched
// across observations by source-scene geometry (IoU), never by text alone,
// so repeated identical strings at different places stay distinct. Tickets
// (`jobId`, `authority`, `sourceEpoch`, `geometryGeneration` and the
// evidence acquisition from `EvidenceAcquisition`) must be fresh and in
// order; stale, out-of-order, composite/unknown and held-duplicate
// evidence is rejected without touching tracked state. All monotonic
// checks run before any automatic reset: `jobId` must strictly increase
// and `timestampMs` must not go backwards even when authority, source or
// geometry advances, and within one `sourceEpoch` the evidence
// acquisition must strictly increase too; only a `sourceEpoch` advance
// may restart acquisition numbering. A seek or an appearance/processing
// change is not visible in `SourceStamp`, so the caller must call `Reset`
// for those; source/geometry/authority advances reset automatically.
// Pan/zoom (`viewTransform`) never resets, matches or renews evidence
// clocks, and a held duplicate never counts as fresh.

enum class RegionPlanStatus : std::uint8_t {
    kAccepted,
    kRejected,
};

enum class RegionRejectReason : std::uint8_t {
    kNone,
    kResultNotOk,           // !ok, or inputFault/backendPreparing flagged
    kResultCancelled,
    kCompositeInput,        // output.compositeInput: recognition not attempted
    kUnknownAcquisition,    // Crossfade/Unknown/inconsistent hold provenance
    kMissingJobTicket,      // jobId or authority is zero
    kInvalidStamp,          // invalid transform, non-positive extent or empty viewport
    kUnpreparedLines,       // prepared not word-parallel in identity/geometry
    kLineLimitExceeded,
    kWordLimitExceeded,
    kTextLimitExceeded,     // observer-only UTF-16 budget; full OCR is unchanged
    kStaleAuthority,
    kStaleSource,
    kStaleGeometry,
    kStaleOrOutOfOrderJob,  // jobId not greater than the last accepted job
    kDuplicateAcquisition,  // same evidence acquisition (incl. held repeats)
    kOutOfOrderAcquisition, // earlier acquisition without a seek Reset
    kOutOfOrderTimestamp,
};

enum class RegionResetReason : std::uint8_t {
    kNone,
    kExplicit,
    kSeek,
    kAppearance,
    kSourceChanged,
    kGeometryChanged,
    kAuthorityChanged,
};

// Shadow knowledge about one spatial occurrence. `kStable` means only that
// independent full-OCR observations repeated the same text at matching
// geometry; it is not current-frame pixel proof and authorizes nothing.
enum class RegionOccurrenceStatus : std::uint8_t {
    kCandidate,
    kStable,
    kChanged,
    kMissing,
};

enum class RegionPriorityClass : std::uint8_t {
    kRequestedFocus = 0,
    kChanged = 1,
    kCandidate = 2,
    kUnobservedCoverage = 3,
    kMissingRecheck = 4,
    kStableRefresh = 5,
};

enum class RegionSelectionReason : std::uint8_t {
    kPriority,        // chosen by priority class within the free budget
    kCoverageReserved // chosen by the reserved oldest-first coverage quota
};

enum class RegionAuthoritativeAction : std::uint8_t {
    kRunFullOcr, // the only value this shadow stage ever emits
};

// Every field is hard-clamped on construction into the range noted here,
// regardless of the value supplied (including INT_MAX or negatives), so no
// external limit can defeat the finite memory/work bounds. The plan and
// `Limits()` report the effective values. Eligible regions per plan are
// therefore bounded by 2048 occurrences + 256 grid cells.
struct RegionShadowLimits {
    int maxLines{1024};              // [1, 1024], existing UCT cap
    int maxWords{8192};              // [1, 8192], existing UCT cap
    int maxWordUtf16Units{4096};     // [1, 4096], before trimming, all dispositions
    // [1, 1048576] per observation, independently for output and prepared
    // text. Checked before content processing. Retained text is separately
    // bounded by maxOccurrences * maxWordUtf16Units (8 Mi units at most).
    int maxTotalUtf16Units{1 << 20};
    int maxOccurrences{2048};        // [1, 2048]
    int maxSelectionsPerPlan{24};    // [1, 256]
    int reservedCoverageQuota{6};    // [1, maxSelectionsPerPlan], oldest-first
    int gridColumns{8};              // [1, 16]
    int gridRows{8};                 // [1, 16]; at most 256 cells in total
    int staleCellAfterObservations{4}; // [0, 1000000]
    int maxMissingObservations{4};   // [0, 1000000]; evict when unseen longer
    int stableAfterObservations{2};  // [1, 1000000] verified fresh acquisitions
    double matchIou{0.5};            // aligned with ModelTuning::agreementIou
    // Highest occurrence id that may be minted (test seam for exhaustion).
    // Zero means the full uint64 range. Id 0 is never minted; once the
    // highest permitted id is handed out, minting fails permanently
    // (counted as overflow) instead of wrapping or reusing an id.
    std::uint64_t maxOccurrenceId{std::numeric_limits<std::uint64_t>::max()};
};

// One hypothetically selected region: a tracked word occurrence, or one
// coverage-grid cell that periodically owes a detailed look so unseen
// content cannot starve behind known text. Rects are normalized scene
// coordinates clamped to the unit scene square.
struct RegionPlannedSelection {
    bool isGridCell{};
    std::uint64_t occurrenceId{}; // valid when !isGridCell; never reused
    int cellIndex{-1};            // row-major grid index when isGridCell
    QRectF sceneRect;
    RegionPriorityClass priority{RegionPriorityClass::kStableRefresh};
    RegionSelectionReason reason{RegionSelectionReason::kPriority};
    // Occurrence status at plan time; for cells kMissing means "no candidate
    // seen there in the latest full result", kCandidate means text is there.
    RegionOccurrenceStatus status{RegionOccurrenceStatus::kCandidate};
    std::uint64_t lastSeenAcquisition{};
    std::uint64_t observations{}; // distinct fresh acquisitions (occurrences)
    bool smoothedUnverified{};    // latest line lacked exact-current matching
};

// Full result versus shadow knowledge for the observed result. Counts of
// repeated stable work are hypothetical work units only; no time, energy or
// speedup gain is measured or claimed by this stage.
struct RegionShadowComparison {
    int fullCandidateWords{};
    int fullOmittedWords{};       // non-candidate prepared words
    int invalidWords{};           // non-finite / empty-after-normalization
    int clampedWords{};           // rects clamped into the unit scene square
    int duplicatesSuppressed{};   // same job, exact geometry + exact text
    int newOccurrences{};
    int matchedOccurrences{};
    int changedOccurrences{};     // same place, different full-OCR text
    int missingOccurrences{};     // retained but absent from this result
    int evictedOccurrences{};
    int overflowDropped{};        // fresh words dropped at finite caps
    int retainedOccurrences{};    // occurrences retained after this observe
    // Candidates full OCR repeated although shadow already held them stable.
    int repeatStableCandidates{};
    // Changed occurrences that were stable before this result and were not
    // selected by the previous accepted plan: a future reuse policy would
    // have missed these edits. This is the shadow safety diagnostic.
    int changedPreviouslyStableUnselected{};
};

struct RegionCoverage {
    int eligibleRegions{};   // retained occurrences + due grid cells
    int selectedRegions{};
    int coverageDebt{};      // eligibleRegions - selectedRegions, finite
    int reservedQuota{};     // effective reserved selections this plan
    int gridCellCount{};
    int cellsWithCandidates{}; // in the latest accepted full result
    int dueGridCells{};
    std::uint64_t acceptedObservations{}; // fresh full results accepted so far
    // No-starvation bound. The reserved quota selects by effective age
    // anchor: an occurrence's creation observation until first selected,
    // then its last selection; a cell's due-since observation (0 before
    // its first selection). At equal anchors a region still waiting
    // outranks one selected at that observation. Anchors of
    // not-yet-eligible or newly created regions therefore never precede
    // the anchor of a region already waiting, the
    // eligible membership is finitely capped, and each accepted plan
    // spends the reserved quota on the oldest anchors. Every region
    // counted in `coverageDebt` that stays eligible is therefore selected
    // within this many further accepted observations, even under a
    // constant requested focus and continuous fresh-word churn. A region
    // leaves the bound only by leaving membership (missing-age eviction).
    // Rejected/held observations never advance this clock.
    std::uint64_t worstCasePlansToCoverAll{};
    std::uint64_t coverageDeadlineObservation{}; // acceptedObservations + bound
    std::uint64_t oldestUnselectedAgePlans{};    // 0 when debt is 0
};

struct RegionShadowStats {
    std::uint64_t plansEmitted{};
    std::uint64_t acceptedObservations{};
    std::uint64_t rejectedObservations{};
    std::uint64_t resets{};
    std::uint64_t occurrencesCreated{};
    std::uint64_t occurrencesEvicted{};
    std::uint64_t duplicatesSuppressed{};
    std::uint64_t changedDetected{};
    std::uint64_t repeatStableCandidates{};
    std::uint64_t changedPreviouslyStableUnselected{};
};

// Immutable plan value returned by `Observe`. On rejection `selections` is
// empty, tracked state is untouched, and coverage describes prior state.
struct RegionShadowPlan {
    RegionPlanStatus status{RegionPlanStatus::kRejected};
    RegionRejectReason rejectReason{RegionRejectReason::kNone};
    RegionResetReason resetReason{RegionResetReason::kNone};
    RegionAuthoritativeAction authoritativeAction{
        RegionAuthoritativeAction::kRunFullOcr};
    std::uint64_t planGeneration{}; // strictly increasing per Observe call
    std::uint64_t plannerEpoch{};   // increments on every reset; starts at 1
    // Authoritative ticket copied from the observed full result.
    SourceStamp stamp;
    std::uint64_t jobId{};
    std::uint64_t authority{};
    std::optional<std::uint64_t> evidenceAcquisition;
    bool focusApplied{};  // a finite requested rect intersected the scene
    bool focusIgnored{};  // requested rect was non-finite or outside scene
    bool truncated{};     // finite caps dropped fresh occurrences
    QVector<RegionPlannedSelection> selections; // <= maxSelectionsPerPlan
    RegionShadowComparison comparison;
    RegionCoverage coverage;
    RegionShadowLimits limits; // effective (clamped) limits in force
};

class RegionShadowPlanner {
public:
    explicit RegionShadowPlanner(RegionShadowLimits limits = {});
    ~RegionShadowPlanner(); // out-of-line: state types are private to the .cpp
    RegionShadowPlanner(const RegionShadowPlanner&) = delete;
    RegionShadowPlanner& operator=(const RegionShadowPlanner&) = delete;

    // Observes one validated full OCR result and returns the next shadow
    // plan. `requestedSceneRect` is the user's current reading focus in
    // normalized scene coordinates (a pure parameter; it changes priority
    // only, never evidence). Never blocks, never touches pixels, and never
    // mutates `validatedFull`.
    RegionShadowPlan Observe(const RecognitionResult& validatedFull,
                             std::optional<QRectF> requestedSceneRect = std::nullopt);

    // Drops all tracked occurrences/cells and ticket memory, advancing the
    // planner epoch. Occurrence ids are never reused across resets, so a
    // seek back to earlier content mints fresh identities. The reason is
    // reported once by the next plan. Caller invokes this for seek and
    // appearance/processing changes, which stamps cannot express.
    void Reset(RegionResetReason reason = RegionResetReason::kExplicit);

    RegionShadowStats Stats() const { return stats_; }
    const RegionShadowLimits& Limits() const noexcept { return limits_; }
    std::uint64_t PlannerEpoch() const noexcept { return plannerEpoch_; }
    int RetainedOccurrences() const;

private:
    struct Occurrence;
    struct Cell;

    RegionShadowPlan Reject(RegionRejectReason reason,
                            const RecognitionResult& result,
                            std::optional<std::uint64_t> acquisition);
    void ClearTracking();
    int EffectiveReservedQuota() const;
    RegionCoverage CurrentCoverage(int selectedRegions) const;
    bool CellDue(const Cell& cell) const;
    // Effective age anchors for the reserved oldest-first quota. An
    // occurrence ages from its creation observation until selected, then
    // from its last selection; a cell ages from observation 0 until first
    // selected, then from the observation it (re)became due. Anchors are
    // what make the no-starvation bound hold under new-word churn: fresh
    // occurrences anchor at the current observation, behind every region
    // already waiting, instead of at "never selected".
    static std::uint64_t OccurrenceAgeAnchor(const Occurrence& occurrence);
    std::uint64_t CellAgeAnchor(const Cell& cell) const;

    RegionShadowLimits limits_;
    QVector<Occurrence> occurrences_;
    QVector<Cell> cells_;
    QVector<std::uint64_t> previousSelectedIds_;
    RegionShadowStats stats_{};
    std::uint64_t planGeneration_{};
    std::uint64_t plannerEpoch_{1};
    // Next id to mint; 0 is the permanent exhausted sentinel (never a
    // valid occurrence id, and minting never wraps past the limit to it
    // as a reusable value).
    std::uint64_t nextOccurrenceId_{1};
    std::uint64_t acceptedObservations_{};
    // Last accepted ticket; meaningful only while hasScene_ is true.
    bool hasScene_{};
    std::uint64_t sceneSourceEpoch_{};
    std::uint64_t sceneGeometryGeneration_{};
    std::uint64_t lastAuthority_{};
    std::uint64_t lastJobId_{};
    std::uint64_t lastAcquisition_{};
    std::int64_t lastTimestampMs_{};
    RegionResetReason pendingResetReason_{RegionResetReason::kNone};
};

} // namespace okuflow::uct
