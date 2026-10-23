# UCT shadow region planner

The planner now also supplies the explicitly opt-in
[supplemental executor](uct_active_regional_ocr.md). Shadow mode described here
remains observation-only. Active mode runs current-image regions between
baseline full deadlines, never skipping full OCR or treating tracked text as
evidence. The planner's `authoritativeAction` remains `kRunFullOcr`.

This shadow planner is a metadata occurrence/grid prioritizer, not optical
flow and not a multi-frame OCR image stack. Supplemental execution now permits
up to a 704-pixel aligned detector long edge over at most 480 source pixels,
with nominal enlargement 22/15 through the existing engine. It neither stacks
source images nor uses planner text as OCR evidence. See the executor's
[realized dimension and source-margin rules](uct_active_regional_ocr.md).

Stage 4 of the [UCT performance roadmap](uct_performance_roadmap_2026_10_20.md)
(revised roadmap Design B) is a **shadow** regional planner: a bounded,
pure-metadata observer that runs beside full OCR and reports what a future
regional scheduler *would* prioritize. It is implemented in
`include/okuflow/common/uct_region_planner.hpp` and
`src/common/uct_region_planner.cpp` as `okuflow::uct::RegionShadowPlanner`,
with the desktop-free contract fixture `tests/uct_region_planner_tests.cpp`.

Default operation allocates no planner. The optional observer never skips
inference, never reuses or publishes text, never mutates a
`RecognitionResult`, and every emitted plan carries
`authoritativeAction == kRunFullOcr`. Repetition tracked here is full-OCR
bookkeeping, not pixel proof: the character-verifier baseline in
[character verification](uct_character_verification.md) measured the NCC
matcher unsafe for single-character edits, and this planner does not revive
it or authorize OCR reuse in any form. It claims no latency, energy or
accuracy gain; repeated-work counters are hypothetical work units used to
judge whether a later, separately accepted design is worth building.

## Interface

```cpp
RegionShadowPlan RegionShadowPlanner::Observe(
    const RecognitionResult& validatedFull,
    std::optional<QRectF> requestedSceneRect = std::nullopt);
void RegionShadowPlanner::Reset(
    RegionResetReason reason = RegionResetReason::kExplicit);
RegionShadowStats RegionShadowPlanner::Stats() const;
```

One planner instance is confined to the single OCR worker thread that owns
it. There is no internal locking, thread, I/O, timer or wall clock; the only
time source is `SourceStamp::timestampMs`, and ages advance only on accepted
fresh acquisitions. `Observe` must be called only with a validated full
result after preparation: `prepared` must be parallel to `output.lines`,
line for line and word for word (the scheduler fills it via
`PrepareLines`). The optional requested rect is
the user's current reading focus in normalized scene coordinates; it is a
pure priority parameter and never becomes evidence. The returned
`RegionShadowPlan` is an immutable value containing the authoritative stamp,
job and authority copied from the result, a strictly increasing
`planGeneration`, the `plannerEpoch`, status/reject/reset reasons, bounded
`selections`, a full-versus-shadow `comparison`, finite `coverage` debt and
deadline fields, and the effective `limits`.

## Occurrence identity

Tracked units are individual candidate words from the prepared lines
(`TokenDisposition::kCandidate`, non-empty text, finite confidence). Boxes
are normalized and clamped to the unit scene square; non-finite or
empty-after-clamping words are counted as invalid and skipped, and
non-candidate words are counted as omissions. Each retained occurrence gets
a persistent source-space id (monotonic from 1, never reused, including
across resets). Identity is spatial, never textual:

- Across observations, occurrences pair one-to-one with fresh words by
  scene-space IoU (`matchIou`, default 0.5, the existing
  `ModelTuning::agreementIou`). Words are bucketed by covered grid cells and
  each occurrence keeps at most its eight best overlaps in a fixed array,
  preserving the IoU/word-order prefix without sorting all overlaps. Pairing
  is greedy by IoU with deterministic id/index tie-breaks. Missing occurrences
  are ranked once by last-seen observation and id for capacity eviction;
  an empty queue does not rescan every retained occurrence for overflow words.
- Repeated identical strings at different places are distinct occurrences
  and can never merge, because text is compared only after geometry pairs.
- Same place with the same full-OCR text increments the repetition streak;
  at `stableAfterObservations` (default 2) distinct fresh acquisitions the
  status becomes `kStable`, meaning only “full OCR repeated this”.
- Same place with different text becomes `kChanged` and resets the streak:
  full OCR changing local tokens always wins over shadow stability.
- A word that moves beyond the IoU threshold leaves its old occurrence
  `kMissing` (streak reset, still eligible for recheck) and mints a new
  occurrence at the new place. Missing occurrences are evicted once unseen
  for more than `maxMissingObservations` accepted observations (default 4).
- Temporally smoothed lines whose `currentInputMatches` proof is absent or
  false are recorded as seen-but-unverified: they neither advance the
  repetition streak nor stabilize from their own blended pixels. The same
  unverified sighting also **demotes** a previously `kStable` occurrence
  with unchanged text back to `kCandidate` and restarts its streak from
  zero; stability must be rebuilt from the full count of new verified
  acquisitions, and an unverified repeat never counts as a stable repeat.

Occurrence id `0` is never minted and ids are never reused. When the
highest permitted id (`maxOccurrenceId`, by default the full `uint64_t`
range) has been handed out, minting stops permanently — across later
observations and resets — and further fresh words are counted as
`overflowDropped` instead of wrapping the counter to a reusable value.

Within one job, duplicate suppression is deliberately conservative: only a
word with exactly equal normalized geometry **and** exactly equal text to
an earlier word of the same fresh result is suppressed (and counted).
Near-duplicates and distinct occurrences are never discarded.

## Ticket, reset and freshness rules

Validation is fail-closed and rejected observations never touch tracked
state (plan generations still advance so consumers can order plans).
Rejected, with a typed `RegionRejectReason`: cancelled, composite
(`output.compositeInput`) or not-ok results — where “not-ok” for shadow
purposes also covers `output.inputFault` and `output.backendPreparing`
flagged alongside `ok`, because faulted input is not full evidence and a
preparing backend means the authoritative backend mix has not settled;
zero job/authority tickets; invalid/non-finite transforms, non-positive source
or destination extents, or zero job-image dimensions;
prepared lines that are not parallel to `output.lines` in both line count
and per-line word count (`PrepareLines` maps every recognized word exactly
once, so a mismatch means the prepared data did not come from this
result). Same-count words must also match trimmed source text, unchanged
confidence, and the stamp-mapped scene/protected boxes before scene clamping.
Only arithmetic roundoff (1e-12 scene units) is allowed between these two
representations of one job; recognition-quality tolerances do not apply.
Preparation may conservatively omit a candidate but cannot promote an omitted
source word. Invalid source spans remain omissions;
over-limit line/word/text counts; unknown acquisitions
(`EvidenceAcquisition` empty, e.g. crossfade/unknown hold provenance);
stale authority/source/geometry (lower than tracked); non-increasing job
ids; duplicate evidence acquisitions — including a held Extra Stable image
presented again, even after a pan that only changed the stamp transform;
earlier acquisitions; and backwards timestamps. Held or rejected repeats
therefore never renew evidence clocks, and pan/zoom alone never resets or
revalidates anything.

Every monotonic check runs **before** any automatic reset, so a replayed
result cannot launder itself into a fresh scene by bumping an epoch: job
ids must strictly increase and readback timestamps must not go backwards
across authority, source and geometry advances alike, and within one
`sourceEpoch` the evidence acquisition must strictly increase too. Only a
`sourceEpoch` advance may restart acquisition numbering (a new camera
session renumbers its frames). Advancing authority, `sourceEpoch` or
`geometryGeneration` with in-order tickets resets tracking automatically
(new planner epoch, typed `resetReason`, fresh ids still monotonic). A seek
or an appearance/processing change is not expressible in `SourceStamp`:
the standalone caller must reset first (for example `kSeek` or `kAppearance`), after
which earlier acquisitions are accepted as a new scene and old identities
are never resurrected. This relies on the existing monotonic
epoch/authority/job counters of the scheduler and controller.
The integrated scheduler performs that reset through its authority change as
described below.

## Priority, coverage debt and the no-starvation bound

Each accepted observation proposes at most `maxSelectionsPerPlan`
(default 24) hypothetical regions: retained occurrences plus due cells of a
scene coverage grid (default 8×8; configurable 1–16 per axis so fixtures can
shrink it). A cell is due when never selected or not selected within
`staleCellAfterObservations` accepted observations (default 4), so scene
area without known text keeps owing periodic coverage instead of starving
behind known occurrences. Priority classes order the free budget:
requested focus, changed, candidate, unobserved coverage cells, missing
recheck, then stable refresh.

Before any priority spending, a reserved quota
(`reservedCoverageQuota`, default 6 of 24) takes the globally oldest
eligible regions by **effective age anchor** — at equal anchors a region
still waiting outranks one selected at that observation, then cells come
before occurrences, then ids. The anchor is the observation from which a
region has been waiting: for an occurrence, its creation observation
until it is first selected and its last selection afterwards; for a cell,
observation 0 until first selected and afterwards the observation at
which it became due again (last selection plus
`staleCellAfterObservations`). Anchoring never-selected occurrences at
their creation — not at “never” — is what makes the bound real under
churn: a fresh occurrence created this observation queues behind every
region already waiting, and a cell that was not yet due queues behind them
too when it later becomes due. Eligible membership is finitely capped
(at most 2048 retained occurrences plus 256 cells), and every accepted
plan spends the whole reserved quota on the oldest anchors, so the set of
regions ahead of any waiting region strictly shrinks. Every region counted
in a plan's `coverageDebt` that stays eligible is therefore selected
within `ceil(coverageDebt / reservedQuota)` further **accepted**
observations, even while the requested focus never moves and new-word
geometry arrives every frame; a region leaves the bound only by leaving
membership (missing-age eviction). The plan states this bound as
`worstCasePlansToCoverAll` and `coverageDeadlineObservation`, together with
`coverageDebt` and the oldest unselected age (observations since the
oldest waiting anchor). Rejected or held-duplicate observations do not
advance the observation clock these ages use. The fixture's
`initialCellsServicedUnderChurnAndConstantFocus` case drives exactly this
adversary — shifting new-word geometry every frame, one reserved slot per
plan, constant focus — and requires all 16 initial cells to be serviced by
the first plan's stated deadline.

## Shadow-versus-full comparison

`RegionShadowComparison` judges the shadow against the full result it just
observed: candidate/omitted/invalid/clamped word counts, suppressed exact
duplicates, new/matched/changed/missing/evicted occurrences, overflow drops
at the finite caps, and retained totals. Two counters exist specifically to
keep future work honest:

- `repeatStableCandidates` counts words full OCR repeated while shadow
  already held them stable — the hypothetical repeated work, in words only.
- `changedPreviouslyStableUnselected` counts occurrences that held
  `kStable` immediately before this result, changed text in it, and were
  **not** selected by the previous accepted plan. A future reuse policy
  would have missed exactly these edits; this is the safety diagnostic any
  “skip inference” proposal must drive to an accepted level with an
  independent character-sensitive verifier first.

Cumulative `RegionShadowStats` mirrors the same counters over the planner's
lifetime. Default bounds are 1024 lines and 8192 words per result (the
existing UCT caps), 2048 retained occurrences, 256 grid cells at most, and
24 selections per plan. These are also **hard** caps: every
`RegionShadowLimits` field is clamped on construction into its documented
range (lines ≤ 1024, words ≤ 8192, occurrences ≤ 2048, selections ≤ 256,
each grid axis ≤ 16, age/streak limits ≤ 1,000,000 observations), so an
externally supplied `INT_MAX` cannot expand memory or work, oversize input
is rejected by counting before any per-word storage is built, and the plan
reports the effective limits actually in force. Occurrence-id exhaustion
and all count arithmetic are guarded, with drops surfaced as
`overflowDropped`/`truncated` rather than silent truncation. Candidate
pairing work per observation is bounded by the cell buckets and the
eight-overlap cap (worst case quadratic only for adversarial degenerate
geometry, still inside the finite caps).

Text has separate observer-only hard limits: 4096 UTF-16 units per raw or
prepared word and 1 Mi units per observation independently for each
representation, including omitted spans. Size-only checks precede trimming,
comparison, sorting, copying and evidence-clock changes. Oversize text rejects
the observation as `kTextLimitExceeded`; full OCR is never truncated. Retained
text has a separate worst-case bound of 2048 occurrences × 4096 units (8 Mi
UTF-16 units, about 16 MiB of text backing), rather than a global 1 Mi-unit cap.
Tracked candidates receive bounded owned string storage, so a short input
string cannot retain an unrelated oversized reserved allocation.

## Worker integration

`OKUFLOW_UCT_SHADOW_PLANNER=1` explicitly enables observation on the existing
OCR worker. `RecognitionScheduler` owns one planner and observes only complete,
current, successful full results after ordinary preparation. Partial results
are not observations and cannot age/evict unseen occurrences. The separate motion-probe
worker cannot enable it. The result carries an immutable `shadowPlan`, separate
`shadowPlannerMs`, and an error field; an observer exception retires that observer
and preserves full OCR. Ordinary `durationMs` excludes observation, while actual
submit-to-callback time includes it.

Scheduler authority changes reset the planner before the next observation with
`kAuthorityChanged`, covering seek, appearance and source changes even when a
backward seek restarts frame/source numbering. A second explicit seek reset is
not needed. Pan/zoom supplies one overwritten normalized focus descriptor for
the next job and cannot submit OCR or renew evidence. Glyph, reading and UIA
admission never consume plans or their selections.

The application source glob and explicit header inventory include the planner.
`uct_region_planner.*` and `uct_region_scheduler.*` use the existing headless
helper to register every Qt slot with an independent 30-second deadline. They
require no model, camera, GPU, network or native window. The real scheduler
fixture also covers opt-out, probe-worker exclusion, held identity, backward
seek and injected observer failure.

The explicit resident selector `OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER=1` requires
frozen inputs. Startup observes the first lease once; that same source's warmup
and all timed copies must reject as duplicate acquisitions with unchanged
accepted-observation count. Only an actual selected-source boundary resets
authority and accepts its first observation. Raw jobs retain these assertions,
plan tickets, coverage debt and observer timing beside the full OCR output.

`uct_region_planner_benchmark` is a separate excluded target, enabled only by an
explicit `--run`. It measures `Observe` on 17 bounded synthetic metadata
scenarios, keeping raw timings, nearest-rank summaries, rejected calls and
invariant failures. It runs no inference and measures no OCR saving. See
[test commands](../tests/README.md#shadow-regional-planner-and-scheduler).

`OKUFLOW_UCT_REGIONAL_OCR=1` also enables observation for the active executor;
without that flag the shadow switch alone never runs regional OCR. The executor
retains at most one immutable full plan and separately rechecks its tickets.

No third-party dependency, persisted setting, file format or user-visible string
is added; licensing and translation catalogs are unchanged.

## Validation status

`tests/uct_region_planner_tests.cpp` covers input immutability (serialized
bytes), repeated-string occurrences, exact-duplicate suppression, movement
and age eviction, same-place edits, the missed-change counter, focus plus
reserved-quota no-starvation over 40 observations, initial grid cells
serviced by the stated deadline under per-frame new-geometry churn and a
constant focus, stale/out-of-order tickets, advancing
source/geometry/authority resets including replayed tickets rejected
before any automatic reset, seek reset semantics, held duplicates and
composite/unknown evidence, malformed results and words (including
`inputFault`/`backendPreparing` flagged with `ok`, non-parallel prepared
words and non-finite stamp transforms), line/word/occurrence caps,
adversarial `INT_MAX`/negative limits hard-clamped with oversize input
rejected at the caps, permanent occurrence-id exhaustion without wrap or
reuse, deterministic ids and reset epochs, smoothed-unverified lines
including demotion of a previously stable occurrence and streak restart,
and grid coverage of unseen area.
The scoped CPU gate passes all 39 selected cases: 31 planner slots, seven real
scheduler slots and model-free Paddle processing (including four-source recorder
saturation). New tie and multi-age eviction regressions preserve deterministic
occurrence ordering. The held scheduler fixture now supplies the capture-owned
record; a separate case proves an unowned hold claim is rejected without
advancing planner evidence. Original intermediate compilation, fixture failure
and runner refusal logs remain retained. The independent PCH-off build passes
all 40 selected cases (the same contracts plus `shared_source_equivalence`).
The tracked integration matrix passes shipping Release compilation, 146 CPU
cases (two with optional skipped Qt rows) and all 17 CUDA delta cases, with no
whole-case skips or unresolved failures. Matching evidence was reused by exact
fingerprint. Local GUI cases were explicitly excluded; hosted native checks and
real screen-reader/camera acceptance remain separate.

The finite real GPU observer run completed two blocks, four immutable sources,
18 full results and eight timed results in 16.48 seconds, excluding backend
preparation polls from the full-result count. All eight new-source plans were
accepted and all ten repeated acquisitions were rejected, with accepted age
fixed at one in each source epoch. Every full result retained zero image-egress
calls; all timed jobs had zero detector calls. Four leases and 14 frozen replays
were retained, both exact State closures completed and cached plan bytes were
unchanged. All four owner-policy comparisons preserved text/order/disposition.
Two failed the original strict geometry/confidence check, retained unchanged;
normal process exit was one for that original assertion. This was an integration
check, not a new OCR speed comparison.

Real-run evidence is retained in
`%LOCALAPPDATA%/OkuFlow/uct-resident-20261020-212348-575f4a8b-2162-4ebb-bfb7-e5c89555e380`.
The derived audit and actual exit receipt are in `uct-shadow-20261020`.

### Finite metadata-cost experiment

Two runs on the AMD Ryzen 9 5900X used the same 17 scenarios and 241 distinct
synthetic acquisitions, two warmups and 12 measured calls per ordinary scenario.
Churn used 24 measured calls; adversarial overlap used four with one warmup.
Both accepted every observation and passed all invariants. Every per-scenario
non-timing JSON field (counters, debt, deadlines and checks) was identical.
The second run changed only bounded overlap selection and ranked missing eviction.

| Scenario | Before median ms | After median ms |
| --- | ---: | ---: |
| Stable 128 words | 0.10 | 0.09 |
| Stable 512 words | 0.47 | 0.45 |
| Shifted 2048 words | 11.76 | 2.11 |
| Stable 8192 words | 18.29 | 7.23 |
| Shifted 8192 words | 28.21 | 7.00 |
| Adversarial 2048 overlapping boxes | 174.87 | 103.07 |

These are empirical `Observe` timings on synthetic metadata, not OCR, native
input-to-display, energy or accuracy measurements. Small-case changes can be
measurement noise. The adversarial overlapping case remains expensive; observation
is still opt-in and full inference remains authoritative. The short samples do
not establish robust tail latency or physical-camera acceptance.

Raw before/after JSON, binary/source hashes, the reference executable, comparison
and original logs remain in `%LOCALAPPDATA%/OkuFlow/uct-shadow-20261020`.
Fixtures and this experiment do not authorize inference skipping or establish
screen-reader or native GUI acceptance.
