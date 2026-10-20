# UCT performance roadmap implementation

This tracks implementation of the owner's revised 2026-10-20 roadmap against
source base `96eba92337216286d27d68ce94e74558705fc652`. The revised scope excludes
video text indexing and recordings-as-searchable-documents. It retains ordinary
video playback, Original recording, paused-frame UCT and recording rewind.

The work is staged. Proposed gains are not measurements, and a diagnostic is
not an accepted production optimization. Model defaults remain GPU medium/medium
and CPU tiny/small. Subsequent quality acceptance enabled batch eight for medium
and small recognizers; tiny retains batch one, as recorded below.

## Order and evidence

1. **Correct the paused-frame admission defect.** Static areas could be replaced
   while words in a scrolling area remained original. Their readable selected
   image was being compared with an unaligned neighbour; dense completed checks
   could also lose eligibility to a UI time budget. CPU and resident-protocol
   regressions reproduced the scrolling problem before the repair. Word-local
   worker registration now preserves the selected coordinates and refuses changed
   ink; all eight selected CPU cases and the real CUDA shifted-reference test
   pass. An additional fault-injection case refuses partial proof after worker
   registration fails. Final integration evidence is recorded separately.
   See [immutable UCT](uct_immutable_controller.md).
2. **Establish the resident-input baseline and width telemetry.** Opus 5.5
   xhigh authored the opt-in resident benchmark; root completed integration
   after a provider rate limit interrupted the handoff. The ordinary
   CPU-image experiment remains available as distinct historical evidence. The
   candidate pair is medium detection plus small recognition, with cached plans,
   owned device input and no production-default change. Instrumentation adds
   crop-width histograms and host wait intervals only under the existing explicit
   profiling switch; no extra readback or synchronization is added. The finite
   three-source run completed with identical ordered crops and zero image egress:
   dense M/M N1 269.54 ms, M/S N8 85.07 ms, M/S N16 80.40 ms median worker time.
   Wider dense outputs still failed strict agreement, including one N16 candidate
   disposition change. See [resident results](uct_resident_baseline.md).
3. **Measure character-sensitive current-frame verification.** Cover individual
   signs, decimals, code operators and similar letters, with genuine jitter
   controls. The known high-NCC single-letter counterexample remains unresolved
   until a measured verifier detects changes without excessive false hides. A
   finite CPU/resident diagnostic is implemented; resolution-verified runs confirm
   unsafe false accepts even with local extents and stricter thresholds. See
   [character baseline](uct_character_verification.md). Synthetic translation
   controls do not establish physical-camera jitter acceptance.
4. **Build a shadow regional planner.** Keep source/geometry/acquisition identity,
   spatial occurrences, requested-region priority and finite coverage debt. Its
   first implementation must leave the full recognition path authoritative and
   must not skip inference based on an unaccepted change verifier.
5. **Use the evidence to choose optimizations.** Width buckets require graph
   support; compact metadata/batched kernels require attribution; precision
   changes require compatible typed exports and a labelled quality evaluation.
   Extra buffers, arbitrary queues and larger memory ceilings are not speedups
   by themselves. Each experiment has finite inputs, work and retained failures.

## Measurement boundaries

Separate cached preparation, source upload, GPU stage intervals, host count/group
waits, semantic merge, resident reference/color/ink work, admission, actual paint
and native accessibility publication. Stream intervals include launch gaps and
contention. Three timed observations cannot establish p95 or p99. Complete State
retirement receipts do not include nested analysis storage or vendor threads;
normal process exit remains independent evidence.

Profiler crop widths are the existing descriptor's requested 48-high rectified
width before canvas clamping. Histogram bounds are 64/128/256/384/512/768/1024,
with separate above-1024/invalid counts. Requested, used and canvas-width sums
yield padding fraction; this does not predict a proportional inference speedup.
Descriptors already cross as compact metadata, so width instrumentation adds no
image transfer. The two host-wait counters wrap existing synchronizations only.

## Product features

Reading Anchor, source-slide fidelity and board-state recovery remain future
product work after the initial performance and quality stages. The roadmap's
exact-symbol, reading-lane, table and correction ideas require their own UX,
licensing and acceptance work. No PDF dependency or new camera-content upload
has been introduced by this stage.

## Completed initial stages and remaining work

The paused-scrolling repair, resident baseline/telemetry and character-verifier
baseline are implemented in this phase. The tracked background matrix passed
Release compilation, 108 CPU cases (two with optional skipped Qt rows) and all
17 CUDA delta cases, with no whole-case skips or unresolved failures. Scoped
CPU/controller, injected-failure, zero-budget and actual CUDA shifted-reference
checks preceded integration. Exact matching evidence fingerprints were reused.
Native GUI and screen-reader acceptance remain separate and pending; the owner's
desktop was not driven.

At that initial checkpoint, the shadow regional planner and frozen-crop entry
were not implemented. The subsequent sections record their separate integration.
Graph-width support checks, broader labelled quality evaluation, failure/long-soak
traces and contention measurements remain research/acceptance work. Equal-crop
complete-path measurements do not silently complete those experiments.

Both requested Opus xhigh tasks hit the provider's rate limit. The implementation
task left its new helper/document; root repaired and tested their integration.
The independent review attempt did not deliver a completed review. No completed
Opus safety sign-off is claimed. Production batching was promoted only after the
separate quality acceptance below.


## Subsequent batch acceptance and completed frozen entry

The owner explicitly allowed <=10 source pixels of box-edge variation and
finite confidence variation, while preserving exact text, order, dispositions
and padding. That separate policy passed all 88 comparisons in the four-source
frozen-crop follow-up; the original strict verdict/assertion remains unchanged
(18 failures retained). The new diagnostic replays exact detector windows on
retained immutable source leases and asserts zero detector calls for all 96
timed jobs. It completes the previously missing recognizer-isolation entry,
without introducing OCR reuse into the app.

Medium and small recognizers now default to batch eight (tiny retains one),
with explicit overrides and batch-one preparation fallback preserved. Model
defaults remain GPU medium/medium and CPU tiny/small: the new serial-symbol
fixture exposed small recognition's `O0` → `00` error, blocking its promotion
under this roadmap's critical-symbol guard despite lower total character edits.
See [the decision](uct_batch8_quality_acceptance.md) and the
[frozen result](uct_resident_baseline.md#completed-frozen-crop-run).

## Shadow regional planner integration

The metadata-only planner is implemented behind the explicit
`OKUFLOW_UCT_SHADOW_PLANNER=1` diagnostic switch, disabled by default. The OCR
worker observes complete current results after preparation; full inference,
overlay admission, reading text and accessibility publication retain their
existing authority. No plan can skip OCR or reuse a recognized word. The
independent motion-probe worker never runs the planner.

Spatial word occurrences, acquisition freshness, source/geometry/authority
resets, bounded focus priority and reserved oldest-region coverage are tracked
without retaining pixels. Held duplicates cannot advance stability or coverage
age. Every plan includes its job/authority ticket, full-versus-shadow counters
and a finite accepted-observation coverage deadline. A failed observer retires
without failing full OCR. Planner time is reported separately from ordinary
worker time and remains part of end-to-end callback latency.

Model-free planner and actual scheduler fixtures cover these contracts. An
optional frozen-input resident diagnostic checks the worker integration without
pretending repeated held images are fresh evidence. No measured OCR saving is
claimed by this shadow stage. See [planner policy](uct_region_planner.md) for
limits, tests and the current validation status.

The final background integration matrix passes Release compilation, 146 CPU
cases (two include optional skipped Qt rows) and 17 CUDA delta cases, with no
whole-case skips or unresolved failures. PCH-off contracts and the shared-source
guard pass separately. The finite real resident observer run verifies source
boundaries and duplicate rejection on all 18 full outputs, zero image egress
and unchanged cached plans. All four owner-quality comparisons pass; the two
original strict failures and normal exit one remain preserved. Hosted native
GUI and real screen-reader/camera acceptance are separate from these results.
