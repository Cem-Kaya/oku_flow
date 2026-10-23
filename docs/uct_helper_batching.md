# UCT contiguous helper batching candidate

This offline implementation starts at
`bb53ac884c24d15f0c77b2dc43cace0e8e650c24`. Both arms retain the integrated
two-slot recognition look-ahead and cooperative resident integral. The attached
23 October review is a source-backed hypothesis, not new performance evidence.
No build, configure, test, inference, GPU inspection, model preparation or
download has run for this candidate; root must first grant a finite START slot.

`gpu::RectifyBatch` uses grid Y for contiguous detector-order samples and the
unchanged scalar bilinear arithmetic, channel order, zero padding and
`min(canvas, validWidth)` behavior. It writes `[N,3,48,W]` into the existing
recognizer input allocation. There is no index upload, sorting, new precision,
vectorized sampling or padded-row optimization. The original scalar API/kernel
is retained for exact-bit comparison. Shapes are bounded before launch.

`issueGroup` still uses one stream and one recognition context. Only equal-size,
equal-canvas groups look ahead. Shape transitions and tails retain the existing
serial boundary. Device input/output and scratch reuse stay stream ordered;
each pinned slot's event fences its own copies, and host consumption finishes
before that slot is reused. The owned image and whole State remain subject to
the existing cancellation drain, asynchronous retirement and unknown-completion
quarantine. No release or current-driver speed/quality acceptance is claimed.

The registered CUDA fixture compares scalar and batched output bits at each
128/256/512/1024 canvas and sample counts 1/3/7/8/15/16. It uses fractional,
tilted and out-of-image coordinates, nonpositive/oversized useful widths,
nonzero descriptor offsets, poisoned unused descriptors and guard/high-water
tails. Tests are prepared but have not run.

`gpu::DecodeBatch` uses actual TensorRT `[N,T,C]`, never an inferred T, with
the original 256-lane finite/range checking, lowest-class tie reduction and
serial per-sample CTC loop. All T rows are scanned; only the same float `ceilf`
prefix is consumed. Invalid consumed values reject that sample, repeats keep
their first confidence and extend the last step, and blanks break repeats.
Scalar kernels remain independent comparison oracles. Counts, contiguous
quads and `N*T` token capacity return in exactly three transfers per neural
batch with unchanged byte count. N1 still makes eight batches per full group.
For C crops in B neural batches, helper/copy submissions decrease by
`6*(C-B)`; that source-derived count is not a measured speedup.

`detail::RecognitionGroups` makes existing pinned-slot phases explicit:
Queued, Reading after the event, Free only after host consumption. No new
CUDA buffer, host index staging or context is added. Cancellation drains any
Queued slot before releasing the input. Failed drain or partial submission
escapes to normal whole-State retirement; it never retries scalar helpers.
The headless fixtures use deterministic gates/throws for three-group reuse,
cancellation after queueing, each of the three copy/event boundaries and
retirement-handoff quarantine, retaining the exact input and both pinned slots.
These are production bookkeeping/retirement tests with fake native operations,
not injected native CUDA failure or hardware acceptance.

The CTC CUDA fixture covers actual T=1/7/17/33/128, C=1/4/257/519, singleton,
full N8/N16 and 3/7/15 tails, all canvases, nonzero descriptor offsets and
poisoned unused capacity. It compares IDs, scores, counts and seeded token
storage exactly, with independent blank/repeat/first-confidence/tie assertions.
NaN, Inf, negative and above-range nonwinning classes, ignored invalid suffixes
and nonpositive useful widths exercise per-sample rejection isolation. Scope
and results remain pending until a serialized START slot.

## Frozen measurement plan

1. Root grants a finite compute slot and the tracked runner lease is available.
   Use isolated candidate/baseline build directories under work-data; no owner
   process, desktop, setting, device, auth or configuration change. Preserve
   the first configure/compiler/test failure and all per-attempt tracked logs.
2. Run scoped CPU `uct_paddle_processing`/frozen-upload ownership and CUDA
   `uct_paddle_cuda` plus scheduler/controller/source-proof contracts through
   tracked runners. Require actual enabled TensorRT compilation. Retain both
   PCH modes and cooperative-integral oracle coverage for the later shared gate.
   This is scoped evidence; full matrix/release acceptance stays separate.
3. Freeze both source trees, all changed-file bytes, compilation flags,
   executable/CUDA-object hashes, Qt/CUDA/TRT DLL hashes, engine/model/config/
   dictionary hashes, actual current-driver/GPU identity, capacity/width and
   actual backend status. Exact baseline is bb53ac8. If root's artifact task
   needs a diagnostic-only factory seam, apply and hash the identical seam in
   both arms and disclose that addition; never relabel the public FP32 factory
   as production BF16. No new engine prep occurs in a timing slot. Missing or
   refused artifacts block that arm rather than silently changing precision.
4. Compare the same exact engines and capacity, N8 fixed1024 first, with sparse
   and dense sources and long-line controls (at most four per fixture visit).
   Use the artifact specialist's reviewed current-backend diagnostic for the
   headline; keep default resident FP32 and strict N8-versus-N1 failures intact.
   Source hashes and exact crop descriptors/order/counts must match between arms.
5. Predeclare unprofiled visits **A B B A / B A A B**, one round, 20 timed
   passes per source. Retain startup and per-source warmup separately; retain
   but exclude the first four timed rows identically in each visit/source.
   This leaves 64 steady observations per arm/source across four visits.
   Keep all other samples and failures; no trimming, best-visit selection or
   unchanged-failure retry. The fixture's explicit minimum watchdog formula
   applies; root grants matching bounded slots, never a hidden open-ended run.
6. Report worker and submit-to-callback raw samples, per-visit counts, p50 and
   nearest-rank p95, paired block deltas and shared-desktop/video workload.
   Worker includes inference plus resident preparation and merge; callback also
   includes dispatch. Neither is native admission/paint, display FPS or photons.
   Counterbalancing does not remove owner-load interference. Do not sum
   overlapping profiler stages or infer accuracy from baseline agreement.
7. Separately run short profiled attribution/frozen-crop visits: assert
   `recognitionRectifyLaunches=B`, `recognitionCtcLaunches=2B`,
   `recognitionMetadataCopies=3B`, unchanged metadata bytes and crop fingerprint.
   Profiling is excluded from headline timing. The original frozen-crop mode
   requires profiling; do not bypass it to create an unprofiled frozen result.
8. Compare every retained same-capacity output's text/order/dispositions,
   token IDs/spans/first-confidence and full protected/search/fitted/projective
   geometry where recorded. Missing quad/provenance fields limit the claim;
   extend the paired diagnostic before claiming full spatial equality. Keep
   all truth/CER/WER/sensitive-token/omission findings separate from engine
   agreement and historical cross-capacity tolerances. This exact patch has
   no intentional quality budget; approximate width/model work stays separate.

## Owner runtime-policy update and separate arm

The exact helper commits remain `0c3433f` and `3eccb93`; their source review
does not cover the later [ordinary cache/runtime policy](uct_tensorrt_cache_policy.md).
Keep that policy in its own commit and arm. The original bb53ac8-versus-helper
comparison can use the same existing 13050 plan paths and exact payload bytes
on both sides. If a shared diagnostic/precision-policy seam is required for the
actual production backend, record its identical diff on both sides. Never
attribute a backend, precision, cache-policy or merge change to helper batching.

Driver identity is provenance, not a new plan key or approval condition. The
TensorRT runtime floor is 11.4.0.106, while plan builder identity stays bound
to each artifact. Newer runtimes may attempt verified older plans; factories,
deserialization, profiles and I/O remain authoritative. Freeze actual loaded
DLL/version/hash and driver facts for each visit, without treating a changed
driver as an automatic invalidation or rebuild request. This also revises the
schema-1 BF16 policy through the precision lead's separate work; do not describe
that approval policy as unchanged.

The independent Opus report at docs-only commit `a6f041f` supports the batch
mapping and same-shape lifetime guard. It identifies host merge cost as a
separate, potentially larger target. Its archived merge/precision numbers
predate this exact candidate and do not establish a new gain. The merge lead
owns that implementation; fresh combined measurements must retain the full
worker/callback protocol. Equal canvas and group size remain mandatory for
look-ahead because growing `Reserve()` can free queued device buffers. No
extra slot, cross-shape look-ahead or prefix-skip change belongs to this arm.

New large evidence/build artifacts belong under
`Z:/Projects/OkuFlow/local-work-data`; the owner app, desktop, settings,
devices and credentials remain untouched. Shared desktop/video load is allowed
and must be recorded when root authorizes measurement. There are no new
dependencies, downloaded assets or third-party code.
