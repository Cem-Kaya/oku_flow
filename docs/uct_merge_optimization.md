# Exact host merge candidate (2026-10-23)

This development candidate reduces repeated host work in
`PaddleMergeRecognition`, including its use inside the GPU recognizer pipeline.
Frozen implementation c3e3fea passed a scoped three-test CPU gate and matched
all-field replay on 2026-10-23. Constructed/regrouped function microtime results
and scope/contamination limits are in the
[CPU replay report](uct_merge_cpu_replay_2026_10_23.md).
There is no measured worker, CPU fallback, GPU, visible-text or FPS gain.

The base is `bb53ac884c24d15f0c77b2dc43cace0e8e650c24`. Its bounded two-slot
recognition look-ahead and cooperative resident integral remain intact. The
production edits cover the merge function and area/intersection helpers only;
later model admission/Resume code and dictionary loading are outside this change.

## Hypothesis and exactness boundary

The old overlap scan allocates temporary polygons, repeatedly derives word
geometry, and repeats overlap decisions during erasure. Removing that work
made the measured replay merging cheaper. A current pipeline bottleneck or
whole-worker gain remains a hypothesis until matched pipeline measurement.

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

This candidate had no spatial index or persistent cache; round 7 added both
as `PaddleMergeContext` (see the section at the end). Caching is restricted to 1024
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

The released Window 2 built the dedicated target and `uct_paddle_tests` with
the CPU preset and at most eight jobs, then ran `uct_merge_replay`,
`uct_paddle_processing` and mandatory `shared_source_equivalence` through tracked
`run_test_gate.ps1` as SCOPED evidence. Registration changes selected the expected
FULL-required exit 2 under `agent_check.ps1`; the root authorized the finite
scoped selection separately. It is not that full gate or release acceptance.
The grant is closed; further execution needs separate scheduling. The
shipping/CPU/CUDA matrix remains required before integration.

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

## Round 7: one context per read (2026-11-16)

The cache above still belonged to one call, so every call of a read rebuilt
the geometry of every word merged so far, scanned every word and re-sorted
every row. `PaddleMergeContext` keeps the cache for all calls into one output:
cached geometry, a uniform grid over word bounds (at least 64 px cells, at
most 256 per axis; words covering more than 64 cells and non-finite bounds
are kept in a list every query visits; a query covering more than 1024 cells
or with non-finite bounds scans every word), a second grid over word centres
for the row search, and per-row sort state. Candidates
from the grid are filtered with the same `QRectF::intersects` predicate the
overlap test starts with and sorted back into output order, so first target,
rejection, erasure order and preferred-row updates are those of a full scan.
The cell argument: two rectangles that intersect share an interior point, and
that point's cell lies in both cell ranges because division, floor and clamp
are monotone.

A word with no overlap and no usable preferred row searched every row. Row
distance counts a cached word only when its height and the incoming word's
height h are within a factor of two (so the larger is at most 2h), its centre
is within .35 of the larger height (at most .7h) across the line, and the gap
along the line is at most four times the smaller height (at most 4h). The
cached word's centre lies in the box of its points, so it is within that
box's width plus height of the point that meets the incoming word's extended
extent. The row search therefore visits only rows holding a word whose centre
lies in that band, with the reach capped at 512 px and a pixel of slack;
words beyond the reach or with coordinates beyond 2^24 px are visited by
every search, and an incoming word without a finite positive height, or whose
band covers more than 1024 cells, searches every row. Rows that are not
visited would return the initial maximum and are never chosen; visited rows
are evaluated exactly, in output order, and the last empty row is found by
scanning the row list as before.

A row is not re-sorted when no word was erased from it or added to it, its
last sort keys were finite and bounded (mixed rectangle and quad rows only on
an exactly upright baseline, where both forms compare the left edge), and its
first word still gives the same baseline angle bits: sorting a sorted
sequence again under a strict weak order is the identity. Other rows are
sorted with the original `std::stable_sort`, or with an index sort that yields
the same unique permutation when the order is strict.

Change detection does not compare words: between calls the context holds a
shallow copy of the output, so any change through the container API detaches
the output and the next call rebuilds. The copy is released before a call
writes, so the merge itself never copies. The four-argument overload, sizes
above 1024 rows or 8192 words, and any exception leave no state to trust and
fall back to a rebuild or the unchanged per-call merge. The ec7282d oracle and
the new replay cases are described in `tests/README.md`; results and
end-to-end identity are in [the OkuRead port record](uct_okuread_port.md).
