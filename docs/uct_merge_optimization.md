# Exact host merge candidate (2026-10-23, unmeasured)

This development candidate reduces repeated host work in
`PaddleMergeRecognition`, including its use inside the GPU recognizer pipeline.
It has not been compiled, executed or timed: the parent has withheld START.
There is no measured worker, CPU fallback, GPU, visible-text or FPS gain.

The base is `bb53ac884c24d15f0c77b2dc43cace0e8e650c24`. Its bounded two-slot
recognition look-ahead and cooperative resident integral remain intact. The
production edits cover the merge function and area/intersection helpers only;
later model admission/Resume code and dictionary loading are outside this change.

## Hypothesis and exactness boundary

The old overlap scan allocates temporary polygons, repeatedly derives word
geometry, and repeats overlap decisions during erasure. Removing that work
should make merging cheaper. A current pipeline bottleneck or a particular
gain remains a hypothesis until matched measurement.

`PolygonArea` reads a span and preserves vertex-zero directed shoelace summation,
starting at zero and ending with `abs(twice) * 0.5`. `QuadArea` retains the existing
flag, finite-point and exact-zero/sign-bit turn checks. No epsilon, intermediate
finiteness check or geometry normalization is added. Finite coordinates can
still produce infinite/NaN areas.

Clipping retains winding, operands, inside/entering predicates, intersection
expression and emission order. Two stack arrays each hold 64 points: each input
vertex emits at most two points per edge, so four points and four edges require
at most `4 * 2^4`. This also bounds malformed/overflow intermediates without
relying on the ideal convex eight-vertex bound. The same `QRectF::intersects`
predicate runs before area work. This preserves returned values, not exception
or evaluation order under trapping floating-point arithmetic.

The merge caches the exact selected shape and its `Bounds()`, lazy area, angle,
height and center. Original candidate traversal, strict confidence comparison,
disposition vetoes, first target, preferred-row updates on rejection, last-empty
fallback and stable reading sort are retained. A usable word completed the full
scan against unchanged output, so its overlap flags drive stable erasure. Words
and cache entries compact together; empty rows retain their indices until the
original final prune. Incoming geometry is made after crop translation.

There is no spatial index or persistent cache. Caching is restricted to 1024
rows and 8192 words, accounting for spare vector capacity left by erasure.
Oversized input or exhausted cache capacity uses uncached decisions without
rejecting/truncating output. Cache vectors are released on fallback and return.
This budget does not bound caller-owned recognition data.

Text fragmentation, numeric thresholds, line association, CTC confidence, quad
validity, source proof, acquisition, freshness, quality budgets and GPU ownership
are unchanged. No dependencies or licenses are added.

## Frozen CPU replay and timing

`tests/uct_merge_baseline_bb53.hpp` declares the oracle. Its two sibling `.cpp`
files freeze the exact base merge and area/clip bodies, retaining separate
translation units and only `Frozen` function-name substitutions. They retain
old allocations and repeated scans. Do not refresh them from the candidate.
Unchanged `TextQuad` accessors, height and angle come from production code.

`uct_merge_replay_tests` is a dedicated QCoreApplication executable with CPU
production sources and no OCR backend, models, native windows or device work.
Ordinary `uct_merge_replay` CTest compares geometry and checks every merge call
against that oracle. Snapshots include ordered text, all rectangle scalars, both
quad flags/all points, confidence, disposition, colors and every word flag.
Floating scalars serialize their 64-bit representations, including signed zero
and NaN payloads; structure padding is excluded.

Generated scenarios include book-sized 383-word, atlas-sized 175-word and
frame-55-sized 262-word workloads; high density, rotated overlaps, fragments,
columns/type sizes, duplicates, disjoint words and malformed controls. Focused
cases cover boundaries/adjacent doubles, rejection preference, first target/later
veto, same-call cross-row compaction/empty reuse, sort ties, the line cap and
8191/8192/8193-word cache boundaries. They are constructed sequences, not captured
detector calls or accuracy ground truth.

Explicit `--benchmark --output <absolute-path>` performs five warmup visits per
arm, then 31 four-visit blocks alternating ABBA/BAAB. A is the frozen base and B
the candidate. Every visit checks all fields after every merge call; per-call
and summed merge nanoseconds are retained. Comparison/serialization is outside
timing. The extra candidate correctness visit is block -2; warmups are block -1.
Exclude both from statistics. Visits start from identical immutable data.

Before timing, `<report>.inputs.bin` freezes all initial outputs/call inputs in
versioned Qt-6.0 QDataStream form. `--frozen-input <path>` replays those bytes;
the report records their SHA-256. The executable checks a four-minute execution
budget between calls; the parent separately bounds configure/build/run time.
Partial reports/frozen inputs remain on failure. Correctness must pass before
interpreting timing. Retain every raw visit and failure.

Optional `--archive-manifest <path>` verifies SHA-256-pinned historical job
receipts (one to eight, at most 16 MiB each), using their final rows as regrouped
calls followed by an identical second pass. These archives omit original
pre-merge order, quads and `inkFitted`; missing fields keep defaults. These are
**output-derived function microbenchmarks**, not reconstructed actual call traces.
Generated oriented controls separately cover geometry fields. Private OCR
text/receipts stay outside Git/release payloads. Real call-trace capture and
GPU/full-worker reruns need a separate parent grant.

After START, build the dedicated target with the CPU preset and at most eight
jobs, then run it and `uct_paddle_processing` through tracked
`run_test_gate.ps1` as SCOPED evidence. Registration changes select the full
gate under normal `agent_check.ps1`; a narrow run is not that full gate or release
acceptance. The shipping/CPU/CUDA matrix remains required before integration.

## Historical evidence and limits

The independent Opus docs-only review is commit
`a6f041ff3d5cdbafc9d40b39d71c5b5c5461c740`, private report
`uct-review-implementation-20261023/opus/uct_gpu_independent_opus_review_2026_10_23.md`
under the owner's local-work-data root. Archived evidence hashes were verified
read-only during implementation:

- `uct-throughput-opus-20261023/summary-tables.txt`:
  `809e361106ffeea72c04a586119d5c7f2439b9dc6ce1d4d4a1166082b78c2f3b`.
- Its `analysis.json`:
  `5f0506dc8cd3ecb8c0db6f70f39d61170bae49fe08c10d26e24182c73fd69692`.
- `uct-quantization-20261022/final-report/report.json`:
  `67e57b4eef1928b1f42bb88152421c727cb6f122f8b842cd9d29e1badf99b51f`.
- Its `engine-summary.json`:
  `e156cc4be7a097e0b7c844608802df6ffdf091a6c07a7c85cfc90ad7605b0ec8`.

Historical profiles motivated this work: BF16 book merges about 49–53 ms within
about 95 ms workers, and dense FP32 merges about 59–78 ms. They span different
look-ahead conditions and predate the complete integrated baseline. They are
directional historical evidence, not this candidate's before/after result or a
guarantee of the current bottleneck. CPU merge microtime cannot establish GPU
utilization, worker/callback gain, CPU fallback speed, paint latency, native
accessibility or delivered display/OCR FPS.
