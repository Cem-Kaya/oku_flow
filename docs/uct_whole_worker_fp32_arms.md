# Frozen cumulative FP32 whole-worker comparison

Status: offline source preparation only. No build, test, native API, cache
access, inference or measurement is authorized by this document. Each execution
window requires a new finite root START and the root's corrected owned-job
harness and tracked runner lease. Desktop permission remains revoked.

This separately supersedes the scheduling proposal in
[helper batching](uct_helper_batching.md); the original bb53-versus-3ecc plan,
commits, failures and receipts remain preserved. These are three private source
arms and three separate binaries, with no product switches or fourth arm:

| Arm | Merge | Recognition helper submissions |
| --- | --- | --- |
| A | Existing merge/geometry | Scalar rectification/CTC, three copies per sample |
| B | Exact host merge from c3e3fea | Same scalar calls/copies as A |
| C | Same exact merge as B | Batched rectification/CTC, three copies per neural batch |

All arms start from the tested `b002b67a472b983eb95f3b57701efdf4f64309a1`
composition. Its driver-independent cache policy, factory failure classification,
dictionary snapshot check and unrelated source are held constant. A restores
only the scalar helper call/copy portions of issueGroup from bb53, retaining
the final explicit slot-lifetime guards and launch-error check. Batch kernels
remain compiled, unused by issueGroup in A/B. C restores uct_tensorrt.cpp
exactly to b002. B/C add only c3's two production-file changes: call-local merge
geometry/decision caching and allocation-free quad area/clipping. The merge
lead's 1,816 CPU visits are function-replay evidence, not whole-worker results;
this composition is unvalidated. Old merge is never reintroduced into public
runtime by a flag. Frozen commits/trees and canonical patches are in the new
private source-arm receipt.

The B step applies the exact two-file production diff from c3e3fea, preserving
the common composition's dictionary snapshot and Resume changes outside those
hunks. Its original oracle/replay target is not copied or rerun in this private
comparison; the future build gate exercises the existing Paddle contracts and
whole-worker all-field comparison supplies the matched real-source check.

Every arm retains the existing **two-slot, same-size AND same-canvas** lookahead,
event plus host-consumption fence, cooperative integral, actual [N,T,C], detector
order/tails, cancellation drain, input leases and whole-State fault quarantine.
No precision, sorting, sampling arithmetic, row skipping, graph or capacity arm
is mixed into this comparison. No root/combined-compositor source is edited.

## Test-only exact N8 receipt

The existing unregistered optionalTensorRtResidentBaseline fixture gains
`OKUFLOW_UCT_RESIDENT_EXACT_N8=1`. It requires medium:medium, only N8,
fixed1024, one round, media stamp, single resident input, no profiling,
frozen-crop replay or shadow planner. Other selection modes retain their
historical N1 gate and thresholds. This selector exists only in test code.

After callback timing, every job serializes exactOrderedOutputV1: Qt_6_0
QDataStream bytes, SHA-256 and base64. This includes ordered rows/words, every
RecognizedWord field, raw double bits for rectangles/confidence/all quad points,
both quad flags, optional full QColor values, paddedBounds and inkFitted;
output flags/error/detail region are included. Volatile job identity, timers
and struct padding are excluded. The comparison is byte equality, with zero
tolerance. Model-free selection/mutation fixtures are prepared in the existing
residentDiagnosticSelectionAndComparison slot, not run.

Historical JSON omitted quads/inkFitted and allowed 0.001 numeric variation;
it cannot establish this experiment's exactness. New output receipts still do
not expose pre-merge detector/crop descriptors or raw compact CTC token IDs,
time spans and confidences. They prove final worker-output equality only.
Do not infer crop/token equality from word counts or from these receipts. If
that extra proof is needed, propose a separate correctness-only test seam that
collects ordered descriptors and initialized token prefixes after each existing
slot event, with no extra copies/events in timed visits; freeze that common seam
and its overhead separately. Existing scalar/batch CUDA oracle fixtures provide
independent helper correctness coverage when separately granted. No profiled
visit is included merely to obtain a crop hash.

## Sources and counterbalance

The private source-inputs.json inventories six existing files with fresh
offline SHA-256: book_001, frame-55, atlas_0, menu_7000, sparse and long-lines.
Use three existing real captures for the primary bounded run, in this canonical
order: book_001 (1120x800), frame-55 (1920x1080), menu_7000 (1280x720). They cover
book density, dense screen text and a sparse menu/tail case without new pages,
extraction, transforms, truth creation or cold model preparation. Atlas is a
reserve; sparse/long-lines are historical constructed controls, not new real
captures. Their provenance/coverage is limited; the primary run does not claim
long-line, corpus or physical-camera acceptance. Missing/changed selected files
refuse the run; do not substitute a reserve after viewing results.

Freeze six three-visit blocks in this order: **ABC, BCA, CAB, CBA, ACB, BAC**.
Each arm occupies each position twice, and each directed within-block transition
occurs twice. Across-block boundaries are not independently balanced. Each visit
is one normally exiting process, one capacity, one scheduler lifetime and the
same three source arguments/seed 20261023. Keep the fixture's seeded source
order identical across arms/visits and record it; sources are compared by pinned
identity, never their shuffled index alone.

Per visit/source: retain one explicit warmup and 20 raw pass records; exclude
the first four pass records from steady timing by index, regardless of value.
Also exclude the cached-load/first-result startup record. Retain and compare
successful startup/warmup/excluded outputs for exactness as well. There are
18 visits, 1,152 jobs maximum including startup/warmups, 96 steady samples per
arm/source, and no adaptive extension/outlier removal/retry-until-pass.

Report schedulerWorkerMs and submitToCallbackMs separately: actual SubmitDevice
worker versus submission through the timestamp at callback entry. Upload and
AnalysisPool capture completion are setup outside both; startup, capture setup,
dispatch/callback remainder and exact State retirement remain raw evidence.
No kernel FPS, paint/display latency or delivered OCR FPS is measured. Publish
per-source raw visit distributions, medians, nearest-rank p95, within-block A-B,
B-C and A-C differences, with the six block values retained. Do not pool away
source differences or claim significance. No 5%/10% performance threshold was
accepted; report gains/regressions descriptively. FP32 first does not guarantee
a conservative bound on BF16 gains because overlap/workload interactions differ.

## Plans, backend and provenance

Use the **same existing historical 13050 detector N1 and recognizer N8 plan
bytes** for every arm, FLOAT I/O and fixed1024. Driver-free lookup/minimum ABI11
policy is constant. Exact-N8 preflight requires the explicit historical 13050
locator, and startup/every warmup/pass must report that accepted key and its
verified payload digest; another valid candidate is refused for this comparison.
The loader itself and its stable-first order are unchanged. Required cached files and accepted backend key/payload must
be matched to a frozen root preflight before the first timed visit; do not
replace the required historical locator with a stable or 13040 key silently.
No cache read is performed during offline preparation; current payload/full-file
hashes remain explicitly pending that authorized preflight. Missing/invalid or
deserialization-refused plans stop the run, never trigger preparation/copy/rekey.

Use the existing inference-only SDK (no parser/builder resources). Record actual
model/config/dictionary hashes, backend name/capacity/FP32 factory, accepted
plan keys/payload hashes, before/after full-file cache inventory, GPU UUID,
actual loaded TensorRT DLL tuple/path/hash and CUDA driver/runtime provenance.
The loaded runtime/driver is not an equality approval gate; actual plan loading
still controls compatibility. Match accepted receipts on every visit, not merely
file preflight. Preserve the same source/binary/compile-command evidence for
all three arms. Shared desktop/video load is allowed and must be disclosed;
never ask to close the owner's apps or treat idle snapshots as isolation proof.

## Proposed finite execution scopes (not grants)

1. **Build/headless window: 1,800 seconds including 30 seconds cleanup, at most
   eight compiler jobs, serial arms.** Root creates three isolated local source
   snapshots/build directories from the exact refs, same Qt 6.12/MSVC/CUDA tier,
   Release settings and msvc-cuda-tests preset. Configure once per arm and
   build only uct_paddle_tests plus dependencies; no app/release/package build.
   Run tracked uct_paddle_processing and mandatory shared_source_equivalence
   for each arm with GUI excluded and resume disabled, archiving all detailed
   logs before the next selection. No GPU/optional diagnostic/model operation in
   this window. Record source trees/raw files, CMake/Ninja/compile commands,
   compiler/runtime dependencies, objects and exact executable SHA-256; all new
   large output stays under local-work-data/uct-whole-worker-fp32-20261023.
2. **Correctness/preflight/unprofiled window: 1,200 seconds including 30 seconds
   cleanup, serial owned children, no compiler.** Start only after window 1
   succeeds. Run the tracked uct_paddle_cuda helper-oracle case once using C
   (kernel source is identical across arms), then one cache-only/source/model/
   runtime precheck. Freeze actual historical 13050 file/payload pins and common
   runtime dependencies before any timed visit. Execute the 18 preregistered
   visits above, no more than 64 jobs each, 30-second per-job/60-second cached
   startup/10-second retirement bounds. Set QTEST_FUNCTION_TIMEOUT=360000; the
   outer owned-job deadline remains the finite window and retains partial
   evidence on timeout. Compare decoded exact-output bytes across all arms and
   all repetitions at the same source/capacity; a single difference or invalid
   backend/plan/retirement stops later visits and blocks performance acceptance.

These are scoped validation proposals, not the shipping/CPU/CUDA release matrix.
The root may split them into smaller finite grants without changing selection,
counts or exclusions. Stop on the first failure/refusal; retain original log,
partial visits and child/lease cleanup proof. No new execution controller is
introduced. No further GPU profiling is proposed unless completed unprofiled
results require attribution under a new, separate grant.
