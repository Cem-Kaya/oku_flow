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
of `2 * worker duration + 80 ms`, never decreasing after a faster call or source
change. This normally permits regions in stable 1,500 ms intervals, **not**
350 ms demand-driven intervals. Insufficient slack leaves baseline work alone.

Each descriptor carries a `steady_clock` full deadline from the remaining
controller wall-clock interval. Immediately before backend execution, after
hold metadata/readback, the worker rechecks slack, exact retained plan identity,
job ordering, authority/source/geometry/image mapping, processing history and
a strictly later acquisition. Queued full replaces queued region; region cannot
replace queued full. An executing descriptor yields if a newer full is queued.
Unsupported detail, unusable/stale plan or insufficient execution slack runs
full OCR once on the same current input instead of region OCR.

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
focus and bounded context. Source and detector edges are at most **480 pixels**,
magnification one. `DetailFocus::detectorLongEdge` defaults to 960, preserving
paused detail defaults. Paddle CPU/TensorRT reuse their existing detector
planning. Boxes/references/stamps remain in the full job image/processed scene.
Inconsistent processed-ROI metadata or words outside it fail closed.

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

Only the actual processed ROI is observed. Outside words survive only after an
independent current probe proves their old line at/after the regional input.
Their id, source, reference, offset and validation clocks remain unchanged.
Contradictions/rejected spans cannot erase current-proved other-pane words.
A spanning line may retain its outside words with the exact old reference;
later real contradiction or ordinary freshness expiry still revokes it.

Crossing protection masks are carved into bounded residual rectangles, retaining
uncovered uncertainty/punctuation. Saturation keeps the original mask. Regional
unions retain 1,024 lines, 8,192 words and 32,768 tokens/protection rectangles.

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

Model-free scheduler fixtures count actual backend calls, full priority/no queue
growth, opt-out/probe exclusion, stale/source/geometry/authority/backward tickets,
execution slack, held exclusion, unsupported/failed/ignored detail and overruns.
Controller fixtures compare baseline deadlines under focus pressure, latch
optional failure, fence stale completion and retain the paused pair. Admission
fixtures cover current-pixel other-pane retention, spanning masks, pending
tickets and same-acquisition exclusion. Existing thresholds/caps are unchanged.

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
