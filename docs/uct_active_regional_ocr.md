# Supplemental regional UCT execution (development)

`OKUFLOW_UCT_REGIONAL_OCR=1` explicitly enables the first production executor
for `RegionShadowPlan` selections. Default is off. The separate
`OKUFLOW_UCT_SHADOW_PLANNER=1` switch remains observation-only; active mode
enables the planner without requiring the shadow switch.

The intended benefit is focused current text arriving sooner between full
reads. No full-frame inference saving, accuracy improvement, native screen-reader
latency or 120 FPS inference is established. Character-change verification
remains unsafe for skipping OCR; see the [false accepts](uct_character_verification.md).

## Baseline deadlines and conservative admission

Advancing media retains its **350 ms minimum** between demanded full submissions,
**1,500 ms stable refresh** and 80 ms probes. Live camera retains its existing
stable/demand gates and 1,500 ms refresh, without a universal 250 ms OCR period.
Actual service depends on input stability, leases, backend duration and probes.
Initially and after source/seek/geometry/processing/authority changes, full OCR
is mandatory. Pending text, uncovered views, actual pixel contradictions and
due refresh still demand full OCR. The character verifier cannot defer it.

Only an idle worker, with no pending/demanded full request, can receive one
region from the latest successful full plan: **one submission per baseline
full interval**. Regions never reset the full timer, coverage or acquisition
markers. They read current owned pixels; planner text is never evidence.

The first cost estimate is **750 ms**, plus a probe-period controller reserve
and **80 ms** worker reserve. Later estimates retain the high-water maximum
of `2 * actual regional worker duration + 80 ms`, never decreasing after a faster
call or source change. Full fallbacks do not update that estimate. This normally
permits regions in stable 1,500 ms intervals, **not**
350 ms demand-driven intervals. Insufficient slack leaves baseline work alone.

Each descriptor carries a `steady_clock` full deadline from the remaining
controller wall-clock interval. Immediately before backend execution, after
hold metadata/readback, the worker rechecks slack, exact retained plan identity,
job ordering, authority/source/geometry/image mapping, processing history and
a strictly later acquisition. Queued full replaces queued region; region cannot
replace queued full. Before inference the descriptor yields without a backend
call if a newer full is queued or execution slack no longer fits. Unsupported
detail or an unusable/stale plan can run full OCR once on the same current input
when sufficient slack remains; a more expensive full call cannot replace an ROI
that no longer fits its reservation.

The measured high-water estimate cannot predict every future cost. Deadline
overruns are counted and disable optional regions until explicit worker
recreation. Full work continues. These are unchanged baseline deadlines and
priority, not hard real-time completion guarantees or native-call preemption.

## Ownership and geometry

There is one latest-only recognition worker and one pending slot. The existing
independent probe-only worker rejects all active descriptors and never runs
OCR. No second CPU OCR worker, preparation bridge, model, download or engine
profile is added.

`RegionalDetailFocus` chooses one planned occurrence/grid cell using current
focus and bounded context. Source-crop edges remain at most **480 pixels**;
the detector's longest edge is now at most **704 pixels**, the nearest supported
32-pixel-aligned size below the requested roughly 720 crop pixels. This is not
1280×720 video or a 720p full-frame profile. Nominal detector enlargement is
**704/480 = 22/15**, about 1.47 times, on the same bounded source area.
`DetailFocus::detectorLongEdge` still defaults to 960, preserving paused
960-pixel/six-times detail. Paddle CPU/TensorRT reuse the existing detector
planning and warm profile with a 960-pixel maximum; no engine/profile change,
download or cold preparation is added. Boxes/references/stamps remain in the full job image/processed scene.
Inconsistent processed-ROI metadata or words outside it fail closed.

The helper uses `nextafter(704.0/480.0, +infinity)`, one adjacent double, to
represent the intended 22/15: otherwise reciprocal roundoff makes
`ceil(704 / magnification)` return 481 instead of 480. Shared rounding policy
is unchanged. Aligned detector axes round down after enlargement. A 480×288
crop therefore gives 704×416, and 480×480 gives 704×704. Small-source clamping
can cause the backend's context pass to expand a helper descriptor further:
for a 300×800 source and tiny selection, 300×288 becomes an actual 300×461 crop
and a 416×672 detector input. Fixtures inspect this realized pass.

The existing minimum detector short edge is 32 pixels. On a tiny source axis
this can interpolate by more than 22/15 on that axis (an 8×480 crop becomes
32×704), while the long-axis enlargement and both tensor edges remain bounded.
This is transparent sizing behavior, not a new tiny-source acceptance policy.
Additional detector work has not been benchmarked; cadence, slack estimates,
full priority and overrun handling remain unchanged, with no speed or accuracy
improvement claimed.

Only successful complete **full outputs** feed `RegionShadowPlanner::Observe`.
Partial outputs never advance missing counts, occurrence ages or coverage debt.
One immutable full plan is retained; a new full request or authority cancellation
invalidates its active ticket.

An optional backend failure preserves healthy text/backend state and requests
full OCR on the next current input. The automatic chain cannot promote, retire
or invoke another candidate for a supplemental call. Optional regions latch off
until worker recreation; full reads keep their normal failover policy. Existing
CUDA ownership/quarantine still applies to genuine device faults. A failed
region adds no CPU retry or readback.

## Admission

The no-withdrawal transaction also checks actual conservative raster coverage.
A geometrically disjoint original mark can share a pixel column with an old
word: `[200.75,200.95)` protects column 200 when it contains no horizontal
pixel centre, while a word ending at 200.7 covers centre 200.5. Original masks
keep that conservative rule; classifying them as carved strips is not a fix.

For novel protection (or changed word geometry near existing protection), the
controller supplies an optional UI-thread renderer preflight before model
commit. It matches old glyph identities to proposed identities, uses the
actually painted old plan and prepares one proposed plan with the production
layout, viewport, exclusions, exact quad rasterization and protection closure.
The old paint must still match the current view and style. No valid current
paint, cold typography, an exception or insufficient remaining admission budget
yields an optional no-op. Model-only callers without a renderer conservatively
refuse novel protection while earlier glyphs exist. Full OCR and independent
pixel probes retain correction authority.

This remains optional work within the existing approximately 3 ms admission
budget, with cooperative deadline checks rather than a hard real-time promise.
Unchanged typography/group membership reuses cached glyph layout; other warm
layouts use cached font measurements only. No cold shaping, inference, event
pumping, native widget creation, public content assignment, visibility change,
paint scheduling or timing publication happens inside the scratch check.
RAII restores temporary content/layout/protection scratch on every exit. The
current painted generation, ticket and visible geometry are untouched. Dense
work can yield; synthetic timings do not establish a speed improvement.

### Transaction and raster regressions, 2026-10-22

The archived `30c6538` baseline plus the new fixtures failed all four selected
regression rows: a late semantic-risk artifact and an append/fragment limit
left `Added` in the reading line; ordinary and tilted original thin-mask
updates were admitted despite the no-withdrawal requirement. Initial actual
`PaintedWord.drawn` assertions passed in both controller rows. These direct
Qt function selections are diagnostic failures, not registered CTest acceptance.
Their source/binary SHA-256 and UTC mtimes were verified before execution.

The fixed source built both fixture executables from scratch, with actual
compilation/linking, VS2022, Qt 6.12, CPU only, PCH off and two compiler jobs.
The tracked `run_test_gate.ps1` selection then passed **4/226 cases** in
**121.58 seconds**, `-Parallel 1 -ExcludeGui`, with zero skipped Qt rows,
skips or unresolved failures: `ultra_clear_text_contract` (303 Qt passes),
`immutable_uct_contract` (115), `uct_timing_controller_contract` (3) and
`uct_timing_pane_publication_contract` (3). The first fixed build/gate passed;
no unchanged failure was retried. Pre/post source and binary hashes match.
The positive safe scratch-plan row prevents an always-refuse implementation
from satisfying the renderer checks. Tests also preserve generation/count,
publication tickets, visible/character geometry, pending evidence and future
ID allocation after refusal. The existing timing mutation fixture now obtains
and waits for genuine offscreen paint authority before adding new masks.

Raw baseline diagnostic logs/receipts, both fresh compile logs, the full tracked
per-attempt log/JUnit/ledger and hash/mtime receipts remain under
`Z:/Projects/OkuFlow/local-work-data/uct-protection-transaction-r19-20261022/`.
The key receipts are `baseline-red-diagnostic-receipt-1.json`,
`fixed-four-case-gate-receipt-1.json`,
`fixed-gate-input-binary-receipt-1.json` and
`fixed-gate-post-input-binary-receipt-1.json`.
This is scoped model-free CPU/offscreen evidence only. PCH-on equivalence,
combined shipping/CPU/CUDA integration, native UIA/NVDA, camera, audible output
and release acceptance were not run in this child.

### Complete-line source ownership

Regional admission owns complete lines only. At crop edges inside the image,
candidate words, protection and original detector bounds must lie at least
**ten input pixels** inside the actual processed ROI: the backend's shared
eight-pixel edge exclusion plus two pixels for detector jitter. The backend
maps detector boxes back to source-crop coordinates before applying its
eight-pixel exclusion, including with enlarged detector input. These remain
**ten source pixels**, with no division by 22/15. Image-border edges have no
extra margin. Tight ink fitting does not enlarge the trustworthy interior.
Rejected edge candidates supply no lines, pending words or temporal observations.

An existing line whose detector union, words or masks are not wholly inside that safe coverage
is retained verbatim, including sentences crossing either axis and mask-only
remainders. Candidates intersecting such a retained line are ignored. Outside
lines do not require a new probe merely to survive a region that never observed
them. No identity, reference, offset, clock or proof is renewed; ordinary
freshness, independent current-pixel validation and real contradictions still
revoke stale/changed text. The same preserves reference-less agreement memory.

If a rejected crop candidate intersects a line that would otherwise be owned,
the whole optional admission is left unapplied, before changing pending tickets
or accepted identities. This avoids treating the rejected candidate as a
negative observation and needs no iterative ownership closure. A later baseline
full read and independent probes retain their ordinary authority.

Before committing, the complete capped regional proposal must also preserve
every still-active stored word in an owned line. Matching is exact-text,
one-to-one in reading order with the established geometry agreement threshold;
repeated words cannot share one replacement. Intersecting protection is allowed
only for the unchanged old word geometry and unchanged old mask, without
strengthening a carved remainder to original protection; an existing tiny mask
never authorizes enlarged protection. Resource caps cannot withdraw
existing words. Confirmed words and words still named by a
reading token are protected even when expired, hidden or reading-only; retired,
unmapped slots are excluded. A refused transaction leaves whole accepted
objects, masks, pending observation tickets, proof clocks and the next line ID
unchanged. It creates no pending disagreement or extra full-demand loop and
does not restore visibility or renew freshness. The one-region attempt remains
spent. This deliberately defers corrections to baseline full reads and
independent probes, rather than letting optional crop disagreements erase text.

Maintained regressions exercise the actual `PaddleMergeRecognition` edge filter
with tight fitted ink and wider detector bounds, including a wholly omitted
top-edge line without a new probe. A separate overlapping wide/short sentence
fixture checks that rejected padding cannot erase the owned short sentence.
These new cases await the coordinated combined gate; they are not a new pass.

This conservative ownership keeps reading order intact and preserves all
crossing uncertainty without carving it. On prose wider than 480 pixels a
regional read may therefore contribute nothing. Full scans remain authoritative;
word-level replacement into a spanning line is not implemented. Regional unions
retain 1,024 lines, 8,192 words and 32,768 tokens/protection rectangles.

Outside pending words keep a shared immutable **per-observation** stamp,
acquisition, authority, digest kind and full-image digest. ROI cannot reticket
them uniformly. Full A, region B, full C confirms an outside word as A/C did
without B; region B and full B cannot self-confirm a new inside word. At most
8,192 pending words/tickets are retained, without source images. New text keeps
the existing independent agreement and exact-current/smoothed-pixel policies.

Paused immutable navigation remains separate: selected **and real neighbour**
detail, original acquisitions/digests, 300 ms settle, up to six-times enlargement
under 960 pixels and 32 non-evicting attempts. Active mode cannot replace the
pair with one half or repeated held pixels.

## Diagnostics and validation

`RecognitionResult::jobKind` names actual full/paused-detail/regional operation;
optional flags describe requested/fallback/failure/overrun paths. `SchedulerStats`
retains detail attempts, actual regional/full/paused calls, priority refusals,
fallbacks, failures, overruns and maximum worker durations. Ignored detail that
returns full output increments `fullCalls` and `regionalAttempts`, not
`regionalCalls`. Durations include recognition/reference preparation, exclude
planner time and do not measure screen-reader publication. Metadata is bounded;
there is no per-frame file I/O.

A successful full submission clears the superseded regional controller ticket,
including when the scheduler coalesces a queued region with no completion.
Exact job IDs fence any late old regional completion from a newer region.
Timing admission distinguishes new-glyph verdicts from actual model changes:
pending/empty changes use the current result's causal identity, with a mixed
retained-artifact flag when older proof survives. Optional no-ops leave existing
tickets unchanged; each unchanged publication surface retains its own ticket.

The shadow planner tracks bounded word occurrences and grid-cell priorities
from full OCR metadata. It is neither optical flow nor a multi-frame OCR
image stack. This input-density change adds neither capability and does not
turn repeated/held frames into independent evidence.

Model-free scheduler fixtures count actual backend calls, full priority/no queue
growth, opt-out/probe exclusion, stale/source/geometry/authority/backward tickets,
execution slack, held exclusion, unsupported/failed/ignored detail and overruns.
Controller fixtures compare baseline deadlines under focus pressure, latch
optional failure, fence stale completion and retain the paused pair. Admission
fixtures cover current-pixel other-pane retention, spanning masks, pending
tickets and same-acquisition exclusion. Existing thresholds/caps are unchanged.

New model-free regressions cover three-pixel edge omission, contained spelling
disagreement, repeated-word count/order, protection/cap withdrawal, expired and
invalid agreement memory, pending-ticket/ID rollback, and absence of extra full
demand. A one-shot pre-dequeue fixture barrier exercises real queued-region
coalescing and subsequent regional admission without timing-based resubmission.
Direct consumer-ticket assertions cover regional and full pending/empty changes,
retained proof, optional refusal and pan. The isolated CPU/PCH-off build and
four-case tracked gate passed on 2026-10-22: `ultra_clear_text_contract`,
`immutable_uct_contract`, `uct_timing_controller_contract` and
`uct_timing_pane_publication_contract`, with zero skipped Qt rows, skips or
unresolved failures in 121.05 seconds. This is scoped CPU evidence only;
combined parent, shipping/CUDA, native UIA and physical-camera acceptance remain
unrun here. Logs, including the repaired unsupported rectangle-hash compile
attempt, remain under
`Z:/Projects/OkuFlow/local-work-data/uct-regional-r11-20261022/`.
That scoped result predates the 704-pixel follow-up. The separate CPU/PCH-off
follow-up gate passed all **51/226 selected cases** in **88.19 seconds**, with
zero skipped Qt rows, skips or unresolved failures: 32 planner slots, 18 scheduler
slots and the complete offscreen controller contract. This includes fifteen
realized-dimension rows, ten malformed-input rows and unchanged paused dimensions.
Its first configure/build and gate passed, without retries. Exact selection and
logs remain under
`Z:/Projects/OkuFlow/local-work-data/uct-regional-704-20261022/`.
GPU/model execution at 704, native UIA, source accuracy/performance and combined
parent shipping acceptance remain unrun here.

The scoped CPU handoff records local validation separately. Actual cached-model
GPU/source proof, combined CPU/CUDA integration, native UIA, physical camera,
optical quality and release acceptance remain independent gates.

### Scoped CPU evidence, 2026-10-21

Fresh Qt 6.12/VS2022 CPU configuration, PCH enabled, built only the five selected
test executables. The tracked gate passes **51/224 cases, zero skipped Qt rows,
zero skips and zero unresolved failures** in 82.38 seconds: all 32 planner and
16 scheduler slots plus `ultra_clear_text_contract`, `immutable_uct_contract`
and `uct_paddle_processing`. The UCT model contract passed 216 Qt rows.

Original evidence remains intact: the first gate passed 49 cases and failed
two new scheduler rows because a fake owned hold record returned `NotHeld`
instead of protocol-valid `Live`. The repaired fixture and production eligibility
for `Live` were rebuilt; original assertions and thresholds were retained.
The second gate used changed fingerprints, not an unchanged failure retry.
`agent_check.ps1`'s initial new-tree FULL-required refusal was preserved; no
`-AllowFull` was used. Private helper invocation errors/exit receipts are also
retained under `Z:/Projects/OkuFlow/local-work-data/active-regional-r7-20261021`.

No GPU, native desktop, application shipping build or complete matrix ran in
this child. `shared_source_equivalence` was not run because its complete target
inventory was not built. Parent integration owns those gates and the separate
real-source proof; this is explicitly **SCOPED CPU** evidence only.

### Explicit cached GPU execution proof

The historical parent receipts below use the original 480-pixel regional
detector budget. They do not validate the new 704-pixel request, which requires
separate cached-engine execution after parent integration.

`uct_paddle_tests optionalActiveRegionalProof` requires explicit
`OKUFLOW_UCT_ACTIVE_DIAGNOSTIC=1`, the existing absolute selected/neighbour PNG
paths and PTS variables from the paused diagnostic, an absolute
`OKUFLOW_UCT_PAUSE_OUTPUT_ROOT`, and an inference-only TensorRT SDK. Compatible
medium/medium batch-eight plans must already be cached. The diagnostic refuses
a missing plan instead of preparing it, and verifies the complete cache inventory
after exact TensorRT retirement.

It runs full OCR on the first extracted acquisition, then actual regional OCR
on the distinct next acquisition under the first submit's 1500 ms deadline.
An ordinary full read then uses that same second lease, frame identity and
digest: it is explicitly not a third temporal observation. JSON retains actual
ROI, raw outputs, candidate counts, model profiles, worker durations and image
egress. No text is admitted by this diagnostic. It does not measure controller
cadence, time-to-paint, screen-reader publication or native display latency.
The controller and admission fixtures separately cover those scheduling and
evidence rules. This optional model run is absent from ordinary CTest.

The maintained model-free Paddle processing group also covers the automatic
chain directly: a failed supplemental operation does not initialize a fallback
or retire the preferred backend, and a preparing supplemental operation does
not start either CPU bridge. The next ordinary full operation remains usable.

### Parent integration evidence, 2026-10-22

The combined CPU/PCH-off gate passed 53 selected CTest cases in 83.19 seconds,
including 241 UCT model rows, 110 paused/controller rows and both direct
automatic-backend regressions. The two offscreen accessibility scales each
passed 25 rows and skipped their explicitly opt-in timing row. The combined
CUDA/PCH-on gate passed all four selected cases without skipped rows; the
serial kernel case took 7.39 seconds. These remain scoped results.

The cached GPU execution proof on the book pair passed with actual region
`[448,125,480,288]`: first full worker 458.263 ms, regional worker 49.548 ms,
then a full worker on the same second acquisition 109.726 ms. There was one
regional call, no regional fallback/failure/overrun and zero image egress.
Exact TensorRT retirement and unchanged engine-cache inventory passed.
The first full includes startup effects; this single sequence is not a
controlled speed comparison or a controller/display latency measurement.

The integrated paused 6x book proof retains 383 candidates, 351 eligible and
203 painted words, with fourteen painted in the detail view. Counts describe
coverage, not ground-truth accuracy. Original model misreads and weak ink-fit
geometry remain separate limitations. Native UIA, physical-camera, audible
playback and packaged-release acceptance are still pending.

Raw receipts and logs are retained under the workstation evidence root
`Z:/Projects/OkuFlow/local-work-data/uct-geometry-r3-20261021/`, including
`evidence/runs/active-book-gpu-1-receipt.json`,
`evidence/runs/active-integrated-book-detail-1-receipt.json` and
`logs/active-combined-{cpu,cuda}-gate-1.log`. The tested isolated source is
`9bea79e`; its integrated production/test blobs match parent `32d3c7d`.

### Review repairs, 2026-10-22

The independent review reproduced eleven admission failures: four sentence-edge
cases, two outside-line/probe races, four clipped-candidate edges and a mask
crossing the ROI with no outside glyph. All eleven failed before the complete-line
repair. Afterwards the CPU/PCH-off model contract passed all 252 Qt rows with
zero skips (34.09 seconds). Crossing-line assertions now require the entire old
sentence and mask, rather than its former fragmented remainder.

A separate expired-slack regression failed before the worker yielded without
inference. The combined focused selection now passes 18 CTest cases with zero
skips/unresolved failures: sixteen scheduler cases plus the 252-row model and
110-row paused/controller contracts. The final resume reused only the two
unchanged executable fingerprints; all scheduler cases reran after correcting
the older expired-deadline fixture to require cancellation and no backend call.
The original 17/18 gate and earlier red regressions remain in the evidence ledger.

Logs are under
`Z:/Projects/OkuFlow/local-work-data/uct-geometry-r3-20261021/logs/`, named
`active-r8-admission-{repro,fixed}-gate-1.log`,
`active-r8-slack-repro-gate-1.log` and `active-r8-final-cpu-gate-{1,2}.log`.
These are model-free scoped results; combined CUDA and actual native latency
acceptance remain separate. In particular, the earlier scheduler-only GPU proof
does not establish regional text admission or reading order.


### Integrated 704-pixel cache-only GPU execution (2026-10-22)

At `b129d8c`, enabled CUDA target compilation and the production-factory BF16
full/region/full diagnostic passed on the two owned book acquisitions. Its
480x288 source region uses the new704-pixel detector policy. Worker times were
305.019/33.5792/94.3459 ms; exact TensorRT retirement completed, both engine
cache inventories stayed unchanged, and image egress was zero. Receipt:
`uct-geometry-r3-20261021/evidence/runs/
bf16-production-active-704-book-1-receipt.json` under local work data.

This is functional execution evidence, not a speed/accuracy comparison or
controller-admission proof. The earlier480-input proof used different device
load. On this curved book region, raw output changed from107 words/106 candidate
words to102 words/69 candidates, with more low-confidence output. These counts
do not establish which strings are correct. Increasing detector sampling does
not guarantee better recognition. Atomic optional admission must preserve
previously confirmed words; full scans remain authoritative. The read-only
output comparison is retained in `evidence/regional-704-existing-output-comparison.json`.

The same raw receipts contain 1 low-confidence word at 480 versus 32 at 704.
For 704, one scores in `[0.60,0.70)`, eight in `[0.50,0.60)`, and 23 below 0.50;
the remaining non-candidate is a high-risk `1` above 0.70. Lowering the confidence
cutoff alone to 0.60 would expose one more raw output to the other checks; 0.50
would expose nine. Those include fragments such as `tro`, `earared` and `foun`.
This is read-only threshold sensitivity, not a change of policy or ground-truth
accuracy. Confidence is not a calibrated probability that the text is correct.
Full breakdown: `evidence/regional-704-confidence-sensitivity.json`.
