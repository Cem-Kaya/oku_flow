# Independent Opus review of the UCT GPU pipeline (2026-10-23)

Reviewer: Opus, independent of the supplied GPU review and of the three active
Sol leads. Source: `bb53ac884c24d15f0c77b2dc43cace0e8e650c24`, tree
`e8b8aab0e822e8820dfeefd7d6dbc06a32851c76`. That tree is byte-identical to the
snapshot the supplied report declares, so both reviews read the same code.
Supplied report SHA-256:
`e80914ef00f54325a6cb9fe45bc41bab0e5d33088b5f43604eb460543f6166e3`.

**Method and limits.** This was source and archived-evidence inspection only.
No configure, build, test, GPU job, inference, model preparation or profile was
run, and no owner process, setting, cache or engine was touched. Every number
below comes from an existing evidence file listed in section 13. Each was
re-derived by read-only parsing and labelled with its sample size and
conditions. Projections are marked **(projection)**. Nothing here is a new
measurement, and nothing is a live FPS claim.

## 0. Bottom line

1. **The largest omission is host-side line merging.** Neither the supplied
   report nor the leads measure or rank it.
   `PaddleMergeRecognition` compares every new word against every accumulated
   word, using polygon clipping and four heap allocations per comparison.
   Archived profiles show:
   - on the BF16 book page, **49–53 ms** of a **~95 ms** worker, against
     27–28 ms of recognizer GPU time; the stream sat idle for the same ~50 ms;
   - on dense FP32 frame-55, **59–78 ms**.

   The merge is pure host arithmetic and can be made exactly equivalent while
   much cheaper. Muse discarded this idea using a **3.9 ms** frame-55 figure.
   That figure was recorded before commit `826f142` moved the merge to
   oriented-quad clipping.
2. **There are two regimes, not one.**
   - Dense multi-region FP32 frames are GPU-recognizer-bound.
   - Text-heavy pages, and BF16 generally, are host-bound: merge plus TensorRT
     enqueue host time.

   Optimizations that shave GPU time (width buckets, detector precision, small
   recognizer) cannot help a host-bound request until the host side is fixed.
3. **Batching the helper launches is correct and exact, but small.** On dense
   FP32 its GPU-side ceiling is roughly the rectify + CTC + metadata event spans,
   about 13–16 ms of ~250 ms of stream time. Finish it because it is already in
   flight. It should rank below the merge fix.
4. **Urgent for lead (2): the existing mixed-FP16 detector is already measured
   slower.** Graph `24e3f6f4…` compiled to **735** TensorRT layers against 278
   for FP32. Its detector stage took **26–30 ms** against **16–19 ms** for FP32,
   on the same scenes, and worker time rose. Reusing that graph is expected to
   regress.
5. **Four look-ahead slots are not justified.**
   - Two slots already cut the dense FP32 stream gap from 61 ms to 24–28 ms.
   - Most of the remaining gap comes from region boundaries, tails and shape
     changes. Slot count cannot touch those.
   - In host-bound requests, extra slots let the GPU idle further ahead and
     change nothing.
   - Extra slots lengthen the drain on cancellation.
6. **CUDA graph replay deserves a second test, on the BF16 arm.** The earlier
   "no N8 gain" result was measured on GPU-bound FP32, and its single graph slot
   was invalidated 54 times per network lifetime by tail shapes. TensorRT
   enqueue host time is 52–88 ms per dense FP32 request, about 2–3 ms per call.
7. **Driver pinning should be removed.** Plan keys and BF16 approval include the
   CUDA driver integer. On the current driver (13050) the approved BF16 plan is
   refused, and the owner's app runs FP32 recognition. Approve plan bytes per
   TensorRT version and GPU, not per driver (see section 8.2).

## 1. Scope and identity

- Required guidance read: `AGENTS.md`, `README.md`, `docs/developer_guide.md`,
  `docs/README.md`, `docs/hardcoded_paths.md`, `docs/THIRD_PARTY_LICENSES.md`,
  plus the UCT look-ahead, BF16, width, integral, timing and resident documents.
- Read in full: `src/common/uct_tensorrt.cpp`, `src/cuda/uct_kernels.cu`,
  `include/okuflow/cuda/uct_kernels.hpp`,
  `src/common/uct_runtime_precision.hpp`, `src/common/uct_recognizer_width.hpp`,
  `src/common/uct_tensorrt_profile.hpp`.
- Read the relevant parts of: `src/common/uct_paddle_processing.cpp` (merge,
  regions, token decode), `src/common/ultra_clear_text.cpp` (quad geometry and
  scheduler run loop), `src/cuda/uct_analysis_pool.cpp` (slot count).
- Root's five in-progress bug fixes were not reviewed; none touches the files
  above as far as their descriptions indicate.
- No duplicate implementation of the leads' work was attempted.

## 2. Urgent interim notes for the active leads

**Lead (2), precision/detector.** The "role-aware mixed-FP16 medium detector"
already has negative evidence on this machine.

- `uct-quantization-20261022` configuration 4 used the same detector graph,
  `24e3f6f4f003…` (FLOAT I/O, 244 `Cast` ops). Its TensorRT engine had **735
  layers**, against **278** for FP32 (`engine-summary.json`).
- `stage.detectorMs` took **26.0–29.7 ms** against **16.2–19.1 ms** for the FP32
  detector, on menus and atlas (n=3 medians per scene, 8 scene runs, all
  consistent).
- This stage is recorded before any recognition in the same region, so it
  isolates the detector even though that configuration also changed the
  recognizer.
- `networkHostMs` also rose: 55–131 ms against 23–66 ms. More layers mean more
  kernel launches per enqueue.
- For contrast, the BF16-linear recognizer compiled to **161** layers against
  **319** for FP32.

The conversion recipe and the resulting fusion matter more than nominal
precision. Do not build role-aware loading for graph `24e3f6f4` without first
showing a conversion that keeps or reduces the FP32 layer count. Sensible
candidates: the existing BF16-linear detector graph `5699b5d6…` (never
engine-built in the archive), or a cast-at-boundary-only conversion.

**Lead (1), batched helpers.** No bug was found in the proposed batch design
(see section 6). One guard is worth adding now. Today, `issueGroup` calls
`Reserve()` on device buffers (`recognizerInput`, `ids`, `scores`, `tokens`,
`tokenCount`), and `Network::Run` can call `outputBuffer_.Reserve()` on a shape
change. That is safe only because look-ahead requires an identical shape, so no
buffer grows while an earlier group is still queued. If width buckets, sorting
or tail look-ahead ever relax that guard, a growing `Reserve` would `cudaFree`
memory that queued kernels still read. Pre-size these buffers to the profile
maximum when preparing, and assert inside a look-ahead issue that no growth is
needed.

**All leads and the parent: driver identity.** On the current driver (13050),
BF16 is refused before inference, so the app runs FP32. Any "production BF16"
timing taken on this machine today is really FP32 unless the backend status
string says BF16. The FP32 plans for 13050 now exist (written 2026-10-23
03:54–03:56: detector `948b1728…`, recognizer N1 `490d8f1b…`, N8 `ee04fec2…`,
width N1 `8213694c…`, width N8 `7f7a02d0…`).

## 3. The supplied report: agreements, disagreements and corrections

| Supplied claim | Verdict | Evidence |
|---|---|---|
| A full N8 group makes 24 helper kernels and 24 copies; batching saves 6×(C−B) submissions | **Agree** (exact count) | `uct_tensorrt.cpp:937-991`, `uct_kernels.cu:470-481` |
| Helper batching should be the first implementation | **Disagree on rank** | Merge is a larger, exact host item (section 5, E1). Helper GPU spans total ~13–16 ms per dense FP32 request |
| `mergeHostMs` can't be subtracted when it overlaps GPU | Agree, but **incomplete**: it is the critical path on text-heavy pages | BF16 book: merge 49–53 ms, stream gap 50–54 ms, recognizer 27–28 ms |
| Resident preparation is roughly 1.2–1.9 ms | **Correction**: that is older, lighter workloads | Dense frame-55 `residentAnalysisMs` is 5.8–6.4 ms (look-ahead runs), still small |
| Rank 3: mixed-FP16 medium detector | **Disagree** unless a different conversion is shown first | 735 vs 278 layers; detector stage +8–11 ms (section 2) |
| Rank 6: compact output ABI and softmax traffic | Agree it ranks low; **demote further** | Arithmetic: 76.6 MB written and read per N8 group is ~0.15 ms at peak bandwidth. Needs a per-layer profile showing projection/softmax above ~10% before any work |
| Rank 5: active-prefix CTC skip | **Agree**, exact | `compact` reads only `t < limit`. CTC span ~7.8–8.1 ms per dense FP32 request. Use the same float `ceilf` expression as `compact` (`uct_kernels.cu:410-412`) |
| Graphs: "no consistent useful N8 gain" | **Agree for FP32; re-test on BF16** | Single graph slot: 54 captures / 360 replays / 54 invalidations per network lifetime. Menu N1 enqueue host time fell 16.69 → 0.44 ms |
| Small recognizer is the strongest model lever | Agree as an approximate track, **after** host fixes | 162.87 → 85.07 ms historical FP32 at equal crops; O/0 confusions; sensitive-token recall 0.9245 vs 0.9434 |
| BF16 driver 13040 vs 13050 prerequisite | **Agree on the fact; disagree with "prepare a separate current-driver record"** | Remove driver from approval; re-verify the same plan bytes instead of rebuilding (section 8.2) |
| Muse: "quad-intersection early-out already present" | **Partly wrong** | An AABB reject exists, but only after four `QuadArea` calls, each allocating a `QVector` (`ultra_clear_text.cpp:1061-1093`). The pair loop is still O(W²) per request |
| `networkHostMs` is shape/address work plus enqueue | Agree; **add** that it rises under look-ahead | Dense FP32 rose 51.9 → 84.3 ms with look-ahead on, so enqueue may block on a full GPU queue (to instrument, section 10) |

## 4. The current critical path

### 4.1 What one request does (worker thread, one stream, one context)

1. **Dequeue.** The latest-only pending job is taken
   (`ultra_clear_text.cpp:2000-2013`), and the worker stopwatch starts at 2022.
2. **Ownership.** `WaitReady` on the input, plus hold provenance if any.
3. **Per detector region.** Regions are an overview plus tiles; 1080p gives
   three. For each region, enqueue normalize → detector `enqueueV3` → connected
   components and boxes → recognition windows → count copy. Then
   **`cudaStreamSynchronize`** (`uct_tensorrt.cpp:890`). This is a full drain
   at every region boundary.
4. **Recognition groups of 8.**
   - Issue each group: rectify ×8, `enqueueV3`, argmax+compact ×8, 24 small
     D2H copies, event (921-998).
   - If the next group has the same width and size, issue it into the other
     pinned slot first (1020-1025).
   - Then `cudaEventSynchronize` on the current group (1026).
   - Then on the host: token decode and **`PaddleMergeRecognition`** for each
     crop (1036-1076).
5. **Shape changes are serial.** A partial tail group or a width change runs
   with an idle stream (1078-1087).
6. Sort lines, run resident analysis (references, colors, ink, surface; ~6 ms
   on dense), and record `durationMs` (2289). The callback is queued to the UI.

### 4.2 Measured composition (archived profiles, profiling enabled)

Dense frame-55, FP32 medium/medium N8 fixed1024, 197 crops, 3 regions, 94 lines,
262 candidate words. "All" medians, n=8. Source:
`uct-throughput-opus-20261023/summary-tables.txt`.

| ms | before look-ahead | two-slot look-ahead |
|---|---|---|
| profiled worker | 300.7 | 284.6 |
| recognizer stream span | 196.0 | 209.2 |
| stream idle gap | **61.4** | **24.1** |
| host: TensorRT enqueue (`networkHostMs`) | 51.9 | 84.3 |
| host: merge (`mergeHostMs`) | 59.5 | 77.7 |
| host waiting on groups | 161.4 | 81.1 |
| CTC / metadata copies / rectify spans | 8.0 / 4.2 / 1.1 | 7.9 / 5.7 / 2.5 |
| detector / regions spans | 17.5 / 4.2 | 18.7 / 4.5 |

The unprofiled headline result is p50 299.51 → 252.92 ms (n=24 per arm).

Four-scene precision comparison, before look-ahead. n=3 medians, contended
shared desktop. Source: `uct-quantization-20261022/final-report/report.json`.

| scene (crops, words) | arm | worker | merge | enqueue host | recognizer | gap |
|---|---|---|---|---|---|---|
| book_001 (48, 383) | FP32 | 118.9 | 57.0 | 14.0 | 46.8 | 57.7 |
| book_001 | BF16 | 94.4 | **49.0** | 10.3 | **28.4** | 49.8 |
| book_002 (48, 383) | BF16 | 95.7 | **53.3** | 9.3 | **27.0** | 53.8 |
| book_001 | FP8 | 113.2 | 71.9 | 16.7 | 21.0 | 72.6 |
| atlas_0 (136, 175) | FP32 | 209.0 | 29.2 | 50.8 | 137.8 | 31.2 |
| atlas_0 | BF16 | 147.4 | 25.1 | 37.2 | 82.4 | 26.9 |
| menu_001 (46, 26) | BF16 | 63.7 | 0.7 | 21.1 | 29.3 | 1.3 |

On book pages the stream idle gap is almost exactly the merge time: the GPU
waits while the host merges. FP8 made the recognizer faster, yet the book
worker got *slower* than BF16. These were profiled runs with n=3 on a shared
desktop. They are strong directional evidence, not distributions.

### 4.3 Regimes

- **GPU-bound:** dense multi-region FP32 with few words per crop, such as video
  frames and menus. Recognizer GPU is about 70% of the worker.
- **Host-bound:** text-heavy pages (book) at any precision, and very likely
  dense BF16. Merge grows with the number of accumulated words in a request,
  not with crop count.

  **(Projection)** On frame-55, BF16's measured ~40% recognizer reduction
  elsewhere (atlas 137.8 → 82.4 ms) would put GPU stream time near 165 ms. Host
  non-wait time is about 180 ms (enqueue 84 + merge 78 + ~20 other). If so, the
  request becomes host-bound. Measure this before relying on it (section 11,
  action 2).

### 4.4 Time to currently visible text, versus the worker stopwatch

The stopwatch (`durationMs`) starts after dequeue and stops before the UI
callback. It excludes:

- the wait for an analysis opportunity (80 ms throttle; skipped when both slots
  of the **two-slot** analysis pool are leased, `uct_analysis_pool.cpp:248`);
- queue residence: a new job waits for the in-flight job, between 0 and one
  full worker duration W;
- UI admission and current-frame proof (the next probe, ≤80 ms cadence);
- overlay paint and positioned-accessibility publication.

**(Projection)** New text therefore appears after roughly
residual(0..W) + W + proof + paint. A saving of ΔW improves average
time-to-visible by about 1.5×ΔW and the worst case by about 2×ΔW. Shorter
workers also shorten lease holds. A lease-starved capture can delay
revalidation, and lines are hidden once validation is more than 350 ms old, so
shorter workers may improve visible continuity on live sources. That last
effect is a hypothesis to instrument, not a measured result. Advancing media
keeps its 350 ms minimum between demanded full submissions and its 1,500 ms
stable refresh. 1000 / W is not delivered OCR updates per second, and has
nothing to do with display FPS, because presentation never waits on OCR.

## 5. Ranked opportunities

Ceilings are the affected stage's measured contribution, not forecasts.

### Exact track (no intended output change)

**E1. Make `PaddleMergeRecognition` cheaper (highest priority).**

Where the time goes (`uct_paddle_processing.cpp:174-309`):
- for each new word, `overlaps()` runs against every old word in every line
  (first pass);
- when the word is used, the erase pass at 289 repeats the identical set of
  `overlaps()` calls;
- `rowDistance` makes a third O(W) pass with trigonometry when there is no
  overlap target;
- each `overlaps()` computes `QuadArea` twice, then `QuadIntersectionArea`
  computes `QuadArea` twice more before its AABB reject. Each `QuadArea` calls
  `PolygonArea(QVector)`, a heap allocation (`ultra_clear_text.cpp:1061-1093`).

Changes, each exact:
1. Use a `std::array` `PolygonArea` overload with the same summation order, so
   results are bit-identical.
2. Compute area, bounds and angle once per word and reuse them.
3. Test the AABB with the same `QRectF::intersects` predicate *before* the area
   work. If the boxes don't intersect, the intersection is 0 and `overlaps()`
   is already false.
4. When `use` is true, the first pass has already evaluated every (line, word)
   pair against an unchanged output. Reuse that overlap set for the erase pass.
5. Later, if profiling still shows it, a uniform-grid index. Iterate candidates
   in the original (line index, word index) order so the first-match, `break`
   and `preferredLine` semantics are preserved.

Ceiling (archived): merge is 49–53 ms of a ~95 ms BF16 book worker and 59–78 ms
of a dense FP32 frame-55 worker. On host-bound requests, nearly all of that is
on the critical path.

Boundary: `uct_paddle_processing.cpp` and the `QuadArea`/`PolygonArea` helpers.
It also benefits the CPU Paddle path (`uct_paddle.cpp:118`). It does not touch
lead (1)'s files.

Risk: low, CPU-only, no GPU ownership or cancellation changes. Floating-point
order must be preserved exactly.

Smallest falsifier: add a private diagnostic that records the
`PaddleMergeRecognition` call sequence (lines, region, source) for book_001,
atlas_0 and frame-55. Replay it offline, comparing old against new: require
bit-identical output (word order, text, every quad/rect field, confidence,
disposition) and report CPU time. If the new merge is not dramatically faster
on the book sequence, stop.

**E2. Per-group timeline instrumentation (prerequisite for E3/E4 and the slot
question).**

Add host timestamps for issue start/end, wait start/end and merge start/end per
group. Add per-group GPU events. Split the first `enqueueV3` after idle from
enqueues made while the GPU is busy. Count `AnalysisPool` capture refusals
caused by both slots being leased. Keep it opt-in and bounded, like
`TensorRtProfile`.

This turns "host-bound or GPU-bound" from inference into a per-group fact, and
shows whether `networkHostMs` is launch cost or backpressure.

**E3. Recognizer graph replay with a bounded per-shape cache, on the BF16
arm.**

Under fixed1024, shapes are N8×1024 plus tails N1..N7×1024, so a cache of at
most 8 entries per network removes the measured churn (54 invalidations).
Addresses are already stable because buffers only grow, and should be pinned at
their maximum (see section 2).

Ceiling: the exposed share of enqueue host time. That is 37 ms on BF16 atlas
and 9–10 ms on BF16 book, which is only useful once E1 removes the merge
bottleneck.

Risk: medium. Graph and context lifetime, drain before retirement, capture
refusal. Sol's prototype already handles typed refusal. Falsifier: BF16 dense
and atlas ABBA with graphs on/off and identical plans. Stop if worker time does
not improve beyond visit spread.

**E4. Lead (1)'s batched rectify/CTC with three bulk copies, plus the
active-prefix CTC skip as a separate commit.**

Ceiling: rectify + CTC + metadata spans of 13.3–16.1 ms per dense FP32 request,
plus about 1,100 fewer API calls of host time on dense (6×(197−26)). The prefix
skip removes argmax reads of padded rows. Padding is 65% on dense fixed1024
(`paddedWidthFraction` 0.653), so most of the ~8 ms CTC span is reachable.

**E5. Prefetch the next region's detector before the final merge (same
stream).**

After issuing a region's last group, enqueue normalize, detector, boxes,
windows and the count copy for the next region, then wait for and merge the
last group. This is safe because stream order already sequences the device
`quads`/`count` reuse after the final group's copies, and host merge reads only
the pinned copies. Cancellation must drain this extra work.

Ceiling: part of the 24–28 ms residual stream gap on dense. Low risk, modest
return.

### Approximate track (owner accepts a small measured quality loss for a significant gain)

**A1. Ordered BF16 width buckets (lead 2).**

Dense padding is 65%, but historical FP32 bucketed widths were mixed: frame-55
p50 302.2 → 315.3 ms *slower* in `uct-recognizer-width-20261021`, while menus
gained. Run this after E1/E3, because on host-bound requests GPU savings are
invisible.

Approval requires a new dynamic-profile plan (section 8). Keep look-ahead
limited to the same shape. Pre-size buffers to the maximum before any
cross-shape look-ahead (section 2).

**A2. Small recognizer, keeping the medium detector.** FP32 first, BF16-linear
small only if the FP32 quality–cost point is promising. It is the largest GPU
lever, but only after the host fixes.

**A3. Pad tail groups to N8.** This keeps look-ahead running through tails and
avoids two shape sets per region. It is approximate: batch-size-dependent
tactics change numerics, as the strict N8-vs-N1 gate already shows (confidence
deltas of about 0.001).

Ceiling: one serial tail bubble per region. Instrument with E2 first.

**A4. Builder optimization level 3–5 instead of 2.** This needs offline
rebuilds. The BF16 approval key pins `opt2`, so a re-approval is required.
Ceiling: recognizer GPU share. Possible regression on shared-desktop load.

**A5. Deduplicate overview and tile detections before recognition.** Run all
detector passes first, then drop overview quads already covered by a tile
detection.

Potentially large: 1080p has 3 regions, and 197 crops produce only 94 lines.
The duplicate rate has never been measured. Measure it first offline from
recorded crop descriptors. The approximate cost is that merge winners change,
so report omissions separately.

### Rejected or deferred (low ceiling, or weak against complexity)

- **Four-slot look-ahead** (section 7).
- **Output-ABI or TopK plugin.** Low arithmetic ceiling; first require a
  per-layer profile.
- **Changes to the `boxes` component layout.** Regions span 4–7 ms.
- **Resident fence coalescing.** ~6 ms of resident work in total.
- **Paint-plan caching and hold-provenance wakeups.** Not GPU OCR throughput;
  instrument first.
- **More OCR workers or auxiliary streams.** VRAM and contention with
  presentation; violates the single-worker contract.
- **FP8/INT8/INT4.**
  - INT8 output was nearly entirely wrong: 4–21 words where FP32 found 26–383.
  - FP8 made book pages slower.
  - INT4 was flat.
- **Temporal OCR reuse or the character verifier deferring full reads.** These
  would weaken proof.
- **Changing the worker's event-wait mode.** Spin waiting burns a core during
  81–161 ms of waits per dense request. Only revisit if E2 shows CPU contention
  hurting presentation.

## 6. Batched-helper design audit (lead 1)

- **Actual [N,T,C] shape.** Read the shape from `Network::Run`. The current
  check is `recShape.d[0]==batchSize`, `d[2]==dictionary`, `d[1]>0`, with T
  stable within a group. T must never be inferred from W. Today W=1024 gives
  T=128, and buckets give T=W/8, but the output shape is the authority.
- **Argmax semantics to preserve.** Keep one block per (sample, t), with 256
  threads and class striding `c += 256`. Keep strict `>` so the lowest class
  wins within a thread, the reduction tie rule that prefers the lower index,
  and the rule that a non-finite, negative or >1.0001 value anywhere in a
  thread's slice turns the row into NaN. Changing only the block→sample mapping
  leaves every block's arithmetic identical, so output is bit-exact.
- **Compact semantics to preserve.** Use one lane per sample.
  `limit = min(T, ceilf(T*min(W,valid)/(float)W))` in float; the host's double
  `validSteps` agrees for T=W/8 and integer widths. A non-finite score inside
  the limit gives count −1. Blank is id 0. A repeat extends `last` and keeps the
  first confidence. Rows at or past the limit are never read, which is what
  makes the prefix skip exact.
- **Contiguous copies.** Device `tokenCount[0..N)`, `quads[first+batchFirst ..]`
  and `tokens[0..N·T)` are already contiguous, and the host slot layout is
  `slotIndex*steps`. Three copies carry exactly the same bytes as today; token
  capacity past `count` remains unread garbage.
- **Two-slot lifetimes are unchanged.** Device scratch (input, output, ids,
  scores, tokens, counts) is single-instance and ordered by the stream. Group
  g+2 reuses g's pinned slot only after g's host merge returns.
- **Check launch errors per launch, not per sample.** On an uncertain stream,
  never retry with the old kernels; use the existing retirement path.
- **The N1 profile is unaffected** (B=C means no reduction), and N16 uses
  `submissionGroup` 16.

No correctness bug was found in the proposed design.

## 7. Two look-ahead slots versus four

Extra slots do **not** run extra neural batches concurrently. One context, one
stream and single-instance device scratch serialize all GPU work. A slot only
lets the host issue further ahead of the GPU.

**(Projection, a pipeline argument)** With per-group host time H_g (issue +
wake + merge) and GPU time G_g:

- one-ahead gives roughly Σ max(H_g, G_g);
- an unbounded queue gives roughly max(ΣH, ΣG) plus fill and drain;
- the difference is non-zero only when H_g and G_g *alternate* in which one is
  larger.

Under fixed1024, G_g is nearly constant. H_g grows within a request as merged
words accumulate, which is monotonic, not alternating. Monotonic patterns gain
nothing from extra depth. Host jitter from desktop or video load is the only
plausible source of alternation.

Evidence: two slots cut the dense FP32 gap from 61.4 to 24.1 ms. The residual
includes three full region-boundary drains, one tail group per region and shape
changes, none of which slot depth affects. The upper bound for deeper same-shape
look-ahead is therefore well below 24 ms, and in host-bound book and BF16
requests it is about zero.

Costs of more slots:
- cancellation must drain up to k−1 groups (about 8 ms per FP32 dense group,
  from 209 ms / 26 calls);
- more pinned slots and events, a negligible ~17 KB each;
- no new VRAM.

Condition that would justify slot 3 or 4: E2 timelines showing *slot-starved*
idle time, meaning the GPU idles inside a same-shape run while the host has
finished issuing and is merging a group whose successor could not be queued.
That idle must sum to more than the visit spread, in a GPU-bound arm, *after*
E1. Test k ∈ {2,3,4} in one executable, ABBA, with the cancel-to-idle latency
recorded. Expectation: after E1, host time per group is small, and two slots
already keep the GPU fed.

## 8. Precision, width and model alternatives

### 8.1 Selection and artifacts as built

- **BF16 approval.** It requires medium/medium, N8, fixed1024, no width
  preparation, compute capability major ≥ 8, an exact UUID, driver 13040 and
  TensorRT 11.4.0.106 (`uct_runtime_precision.hpp:82-93`). It also requires
  exact source/graph/plan hashes and a single min1/opt8/max8×3×48×1024 profile
  (`uct_tensorrt.cpp:424-435`).
- **Typed refusals select FP32** inside the same preparation; any CUDA fault
  retires.
- **With a BF16 request, all preparation is cache-only**
  (`uct_tensorrt.cpp:1190,1218`). If FP32 plans are missing, the GPU backend is
  unavailable and Automatic falls back to CPU/Windows OCR.
- **The FP32 baseline is effectively TF32.** No builder flag disables TF32, and
  the key says `tf32-default`. BF16-vs-"FP32" deltas are BF16-vs-TF32.

### 8.2 Driver pinning: recommend removal

TensorRT plans depend on the TensorRT version and the GPU, not on the driver
patch level. Including the driver integer in the FP32 key
(`uct_tensorrt.cpp:303-308`) and in the BF16 key and approval forces cold
rebuilds after every driver update. Rebuilds can select different tactics,
which changes numerics.

Recommendation, for lead (2) to own:
- Key and approve on TensorRT build, GPU UUID or SM, graph hashes and the exact
  plan-file hash. Record the driver as metadata only.
- On a driver change, optionally run a frozen-crop golden comparison with the
  *same* plan bytes, rather than rebuilding.

Keep one driver within a single comparison. That is experimental control, not
product policy.

### 8.3 Dynamic-width BF16 risks

- T varies per group. Per-slot `steps` must travel with the group, as
  `IssuedGroup` already does.
- `outputBuffer_` and host token slots grow. Pre-size them to the maximum.
- Each bucket is a new tactic set inside one profile, so narrower is not
  automatically faster.
- Approval needs a new plan hash and profile record. Do not wildcard.
- Report per-group N, W and T, plus look-ahead eligibility.

### 8.4 Quality metrics and the combined benchmark protocol

**Arms and controls.**
- Exact changes (E1, E4, E5) need bit-identical outputs at the same capacity.
  They consume no quality budget.
- Approximate arms need independent truth: human-verified strings and word
  regions; CER/WER; exact recall on sensitive tokens (decimals, signs, O/0,
  I/l/1, Turkish İ/ı, accents); spatial omissions and extras; and geometry
  error.
- Baseline agreement is not truth.
- Score every retained output, not only the first candidate.
- Freeze identity before timing: commit, executable and plan hashes, the actual
  `BackendStatus` precision string, driver and UUID, and cache inventories
  before and after.
- Order arms ABBA then BAAB with a pre-registered warmup exclusion. Run
  profiled attribution blocks separately from unprofiled headline blocks.
- Use dense, book, atlas, sparse, long-line, tilted and real advancing video
  workloads.
- Run with the owner's normal desktop and video load recorded, not suppressed.

**Combining arms.** Integrate in the order E1 → E4 → E3 → A1/A2. Re-measure
the combination fresh; never add isolated percentages. Promote only with a
material worker/callback gain on the target class, no tail regression on
controls, no ownership or stale-publication failure, and a quality cost the
owner signs off.

## 9. Overlooked options screen

- **Coverage multipliers.** Three 1080p regions (4.41× the overview's detector
  pixels) and long-line windows produce 1.5–2.8 crops per final line
  (book 48/33, atlas 136/48, frame-55 197/94).
  - Measure the duplicate rate before anything else (A5).
  - The original full-width crop of a long line that is already windowed may
    also be redundant. Measure how often it wins the merge.
- **Staged publication.** Publishing per region would let some text appear
  earlier.
  - It needs a "partial" result type that can never cause removals and still
    passes current-frame proof.
  - Partial results add controller and model complexity, so this is research
    after E1.
- **Analysis-pool lease pressure.** Two slots, one OCR job in flight, one
  pending. Instrument refusals (E2) before changing pool size.
- **Enqueue backpressure.** `networkHostMs` rising under look-ahead suggests
  enqueue blocks on a full queue. E2 separates that from launch cost.
- **TF32 baseline.** This is a labelling fact for comparisons, not an
  optimization.

## 10. Instrument first

1. Per-group host/GPU timeline and enqueue split (E2).
2. A recorder for `PaddleMergeRecognition` call sequences, with offline replay
   (E1).
3. `AnalysisPool` refusal counts against in-flight and pending lease holds.
4. Cross-region duplicate rate from recorded crop descriptors.
5. Actual backend status string in every benchmark row, so BF16 and FP32 cannot
   be confused.

## 11. Experiment order, commits and the next three actions

**Commits (small and separate):**
1. `std::array` polygon area plus per-word geometry cache (exact).
2. AABB prefilter and reuse of the overlap set for the erase pass (exact).
3. Merge replay fixture (CPU-only, bit-identity check).
4. Per-group timeline profile fields.
5. Lead (1)'s batched helpers, unchanged.
6. Prefix-skip CTC.
7. BF16 graph cache experiment.
8. Next-region detector prefetch.
9. Approximate arms.

**Next three actions:**
1. **CPU-only: implement E1 with replay bit-identity.** No GPU needed. Gate it
   with scoped `uct_paddle_processing`.
2. **Parent-scheduled GPU run (FP32, current driver; plans exist).** Current
   head plus E1, ABBA, on frame-55, book_001, atlas_0 and one menu, profiled
   and unprofiled blocks. Confirm the merge share with look-ahead integrated,
   and whether BF16 would be host-bound (the projection in 4.3).
3. **Lead (2): remove the driver pin** (8.2) so the BF16 arm can run on
   13050. Then repeat action 2 on BF16, and before any detector conversion
   work, check its layer count against FP32.

Keep exact and approximate tracks separate. Keep failed and interrupted
evidence. Use one driver and cache identity per comparison. Use independent
ground truth for approximate arms.

## 12. What was not done

- No build, test, GPU, inference, preparation or profiling. No owner-process,
  settings, cache or engine interaction.
- The merge cost figures are archived profiled runs: n=3 per cell for the
  precision comparison, n=8 for look-ahead. They predate the integrated
  look-ahead and integral changes, except for the look-ahead candidate itself.
- No ground truth was evaluated. Projections in 4.3, 4.4 and 7 are arithmetic,
  not measurements.
- Root's five bug fixes and lead (3)'s research were not reviewed.

## 13. Evidence index

Under `Z:/Projects/OkuFlow/local-work-data/`:

| Evidence | SHA-256 |
|---|---|
| `uct-throughput-opus-20261023/summary-tables.txt` | `809e361106ffeea72c04a586119d5c7f2439b9dc6ce1d4d4a1166082b78c2f3b` |
| `uct-throughput-opus-20261023/analysis.json` | `5f0506dc8cd3ecb8c0db6f70f39d61170bae49fe08c10d26e24182c73fd69692` |
| `uct-quantization-20261022/final-report/report.json` | `67e57b4eef1928b1f42bb88152421c727cb6f122f8b842cd9d29e1badf99b51f` |
| `uct-quantization-20261022/engine-summary.json` | `e156cc4be7a097e0b7c844608802df6ffdf091a6c07a7c85cfc90ad7605b0ec8` |

Also used, without hashes:
- `uct-geometry-r3-20261021/evidence/runs/*book*.txt`: production BF16 book
  profiles.
- `uct-paused-continuity-20261021/gpu-profile-55-following.txt`: the 3.9 ms
  merge from before `826f142`.
- `uct-recognizer-width-20261021/full-*.txt`.
- `uct-optimization-race-20261023/muse/race-report.md`.

Read-only parsing scripts are in
`uct-review-implementation-20261023/opus/scripts/`.
