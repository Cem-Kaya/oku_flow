# OkuRead latency ideas brought back to OkuFlow

Status: development record, started 15 November 2026. OkuRead imported its OCR
components from OkuFlow at `c0e33a2` and then spent thirteen rounds on latency.
This records which of those ideas were evaluated here, round by round, with
what was changed, what was measured and what stays switched off.

Every timing below uses the protocol and scene set in
[corpus timing](uct_corpus_timing.md): scheduler worker time on file fixtures,
two reversed cohorts of five warm reads, RTX 4090 (driver 621.14), Ryzen 9
5900X, MSVC 19.44 Release, CUDA 12.9.86, TensorRT 11.4.0.106, medium detector
and recognizer in BF16. `nvidia-smi` reported `display_active: Disabled` for
every run, so the display was asleep or not driven; worker time has no
composition step, but the state is recorded with each run. The scene set
contains no owner camera capture; twelve scenes are generated and six are real
chalkboard lecture frames. Worker time is not camera-to-paint latency.

## What applied and what did not

| OkuRead idea | Here |
| --- | --- |
| Path-compressing component roots, four-bin ribbon merge | Round 1, on by default, identical output |
| Cooperative box scoring | Round 1, rewritten: OkuRead's exact variant gave no gain here. On by default with CUDA 12.9 on sm_86 and sm_89 (RTX 30 and 40 series) |
| Fixed-tree box scoring | Round 1, private comparison switch only: it rounds differently |
| Collect detections across tiles before recognition; a complete detection supersedes its covered edge fragment | Round 2, private switch, off: changes output |
| Composition flush before capture, capture targets, overlay raster | Not applicable: OkuFlow analyses camera, video and file frames it already owns |
| Speculative ink retries | Not applicable: OkuFlow has no retry fits |
| Reuse-cache crop order defect | Not applicable: OkuFlow has no OCR reuse cache and never reorders crops |
| Keep word-merge state across one read instead of rebuilding it per crop | Round 7, on by default, identical output |

## Round 1: detector post-processing

Target stated before the work: the "regions" stage of the textless scene from
59.9 ms to under 15 ms and the bent page from 43 ms to under 20 ms, with
identical output. Result: 61.5 to 26.4 ms and 39.1 to 17.5 ms. The bent page
met the target; the textless scene did not.

Changes in `src/cuda/uct_kernels.cu`, selected by `gpu::DetectKernels`:

- **Path-compressing roots.** `root` shortens each visited parent link to its
  grandparent with an atomic minimum. Links only decrease towards the
  component's smallest index, so the root reached is unchanged.
- **Box scoring per warp.** `boxesCooperative` gives each component to one warp
  that loads and tests 32 pixels at a time. The accepted values are added by
  `boxesAddGroup`, which produces exactly the sum of adding them one at a time
  with float additions, without the serial chain: inside one binade each
  addition is an integer step of the mantissa, and only an exact half depends
  on earlier values, through the mantissa's parity. A group that leaves its
  binade, or contains a negative, non-finite or large value, is added serially
  by lane zero.
- **Four ribbon bins per merge round** instead of one, each with the original
  thread-order fold.

The original kernels remain as the comparison oracle
(`DetectKernels::Reference()`), and `OKUFLOW_UCT_TEST_DETECT_KERNELS` selects
them or any single new kernel for timing (`reference`, `roots`, `boxes`,
`ribbons`, `tree`).

Two things were learned on the way and are kept in the code comments:

1. OkuRead's exact cooperative box kernel (parallel loads, serial fold through
   shuffles) was ported first and measured no gain here: 61.6 ms for the
   original against 63.4 ms on the textless scene's regions stage
   (`r1-kernels-attribution-1`). The serial additions are the cost, not the
   loads.
2. The original `boxes` kernel is not self-consistent as compiled. For sm_61
   to sm_90 the compiler unrolls its column loop by four and fuses the column
   product into the sum for the unrolled columns only; the leading
   `width % 4` columns of each row add two separately rounded products. For
   sm_100 and later it fuses every column. A pixel that defined a component's
   extent can sit exactly on the bound, so this decides membership. The first
   port differed from the original by one component on a threshold-sensitive
   map. `boxesInside` now reproduces the sm_61 to sm_90 rule.

### Where the new box scoring applies

The box kernel is identical to the original only where the original's compiled
rounding is the one `boxesInside` reproduces. What is established:

| Evidence | Scope |
| --- | --- |
| Executed against the original, bit for bit | RTX 4090 (sm_89), CUDA 12.9.86 |
| Final machine code of both kernels has the identical float-instruction sequence | sm_86 and sm_89 (`r1-sass`) |
| Generated intermediate code of the original identical | sm_61, 75, 80, 86, 89, 90 (`r1-ptx`) |
| Final machine code: equal counts of fused, multiply and add instructions, different order | sm_75, 80, 90; sm_61 has one multiply fewer in both kernels |
| Original fuses every column | sm_100, 103, 120, 121 |

Equal intermediate code and equal instruction counts do not settle the final
rounding, so they are not treated as proof. `DetectKernels::ForDevice` enables
the new box kernel only when built with the CUDA 12.9 toolkit
(`kCooperativeBoxesToolkit`) and running on compute capability 8.6 or 8.9, that
is RTX 30 and RTX 40 series cards. On everything else, including the GTX 10
series test system, RTX 20 and RTX 50 series, and under any other toolkit, the
original box kernel stays in use. Roots and ribbons are exact by construction
and are on everywhere. The measurements below are for the RTX 4090.

### Measured

Evidence `r1-kernels-ab-2`. Reference kernels against the new defaults, same
executable.

| Scene | Reference ms | New ms | Speedup | First read ms, reference / new |
| --- | ---: | ---: | ---: | --- |
| no-text-1080 | 80.93 | 45.66 | 1.772 | 80.60 / 46.12 |
| book-curved-1080 | 94.79 | 71.86 | 1.319 | 95.24 / 72.61 |
| board-lecture-02 (real) | 30.56 | 25.34 | 1.206 | 31.28 / 25.99 |
| slide-light-2160 | 131.78 | 113.48 | 1.161 | 131.46 / 113.06 |
| board-lecture-00 (real) | 25.30 | 22.60 | 1.119 | 25.75 / 23.09 |
| slide-light-1080 | 74.40 | 66.59 | 1.117 | 74.20 / 66.45 |
| board-lecture-01 (real) | 25.27 | 22.64 | 1.116 | 25.03 / 21.76 |
| slide-dark-1080 | 75.91 | 68.60 | 1.106 | 75.92 / 68.81 |
| board-lecture-05 (real) | 24.87 | 22.53 | 1.104 | 25.01 / 22.35 |
| signs-sparse-1080 | 33.94 | 30.86 | 1.100 | 33.65 / 31.30 |
| board-lecture-03 (real) | 27.44 | 25.01 | 1.097 | 27.81 / 26.28 |
| board-lecture-04 (real) | 25.53 | 24.16 | 1.057 | 25.16 / 23.81 |
| doc-camera-2160 | 488.19 | 470.12 | 1.038 | 489.37 / 472.34 |
| doc-camera-tilt-1080 | 161.98 | 156.37 | 1.036 | 161.21 / 156.21 |
| doc-camera-turkish-1080 | 182.73 | 176.91 | 1.033 | 182.84 / 178.22 |
| page-portrait-1654x2339 | 385.59 | 374.41 | 1.030 | 384.05 / 375.05 |
| doc-camera-1080 | 208.63 | 203.28 | 1.026 | 208.53 / 203.76 |
| table-1080 | 141.28 | 138.42 | 1.021 | 140.54 / 138.70 |

The table is the build as submitted for review (`81a22c2`). The build with the
review fixes was measured again (`r1-kernels-ab-3-final`): 1.122x warm and
1.122x for first reads, identical output on 18 of 18 scenes, with noisier
cohorts because other work was using the machine.

Geometric mean 1.126x warm and 1.122x for first reads. The A/A noise floor is
about 2 percent on dense scenes and up to 12 percent on the short ones, so the
dense-document rows are at the edge of what this protocol can show.

"Regions" stage, profiled separately (`r1-kernels-attribution-4`), in ms:

| Kernels | doc-camera-1080 | book-curved | slide-2160 | no-text | board-lecture-02 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Reference | 7.5 | 39.1 | 25.8 | 61.5 | 6.9 |
| New defaults | 2.1 | 17.5 | 7.6 | 26.4 | 2.5 |
| Tree box sum (not exact, off) | 1.9 | 8.0 | 5.7 | 11.1 | 1.7 |

### Output

- Byte-identical complete output (text, order, dispositions, geometry,
  confidence, colours) on 18 of 18 scenes, in both cohorts.
- `gpuDetectKernelsMatchReferenceBytes` (its own CTest case,
  `uct_detect_kernels_cuda`) compares the new kernels with the originals bit
  for bit on 60 distinct detector maps (30 at 960x544 and 10 at each of three
  other sizes), each with and without ribbon fitting: quads and ribbons always,
  labels and components when ribbon scratch keeps them. The maps include
  spirals and combs for long parent chains, more than 128 components, dozens of
  roots in one block, tilted and curved bands, small rotated boxes at the score
  threshold, quantized values that make rounding ties common, a nonzero
  background, and negative, large, subnormal and non-finite values. It reported
  146,996 eligible boxes and 32 valid ribbon fits.
- `gpuBoxSumMatchesSequentialFloatAdds` (same case) runs the exact group sum
  alone on 160,000 crafted groups and compares it with adding the values one at
  a time: 93,927 groups took the fast sum and matched bit for bit, including
  243,012 rounding ties, 12,543 ties in lane zero and 35,358 groups with three
  or more ties; 66,073 were correctly refused, 5,671 of them because the sum
  lands exactly on the next binade. The test fails if those counts collapse.
  Both tests together take about 8 s on the RTX 4090.

### Not done

The tree box sum would bring the textless scene to about 11 ms of regions, but
it changes detector scores in the last bits and can flip the 0.4 threshold, so
it stays a private switch. The remaining exact cost on that scene is one warp
per very large component; real lecture frames do not have such components
(their regions stage is now 1.7 to 2.5 ms), so it was not pursued. The group
sum is an associative scan over a step total and a parity state, so several
warps could share one large component and stay exact; that is the route to
the missed target if textless frames with large blobs turn out to matter.

### Review

Council, as submitted (`81a22c2`): Claude Opus 5.5 7/10, GPT-6.1 Sol 7/10,
Gemini 4 Hydrogen 8/10. None found an input on which the exact sum differs
from sequential adds. Applied afterwards: the toolkit and architecture guard
and the scope table above (after comparing final machine code, as asked), an atomic final label store in the compressing variant, the
direct group-sum test and its own CTest case, the block with dozens of roots,
lighter read-back in the map test, a run of the tree variant, refusal of an
empty switch value, and corrected wording (elapsed-time percentages, map
counts, display state). Not applied: making every label read atomic (the
plain reads are unchanged from the original kernels), and an explicit rounding
contract shared by `extents` and `boxes`, which would remove the compiler
dependence at the cost of one documented output change.

## Round 2: one recognition per line across regions (private switch, off)

OkuRead collects detections from all of its tiles before it recognizes
anything, and a complete detection there supersedes the edge fragment it
covers. OkuFlow instead recognizes every detection of every region and
reconciles words afterwards by overlap and confidence. A full read of a
1920x1080 frame has three regions (the whole frame and two overlapping tiles)
and a 3840x2160 frame nine, so most lines are recognized two or three times.
The word-level rule also has a defect: a confident fragment cut by a tile edge
can replace the complete word that another region read. On 62 placements of
two owner-supplied stylized title crops the unchanged build returns fragments
such as a three-letter prefix in 19 cases, with curved processing on or off.

Target stated before the work: halve the recognizer crops on text scenes, take
the dense 1920x1080 document from 204 ms to 120 ms or less, repair most of the
19 fragmented titles, and lose no labelled words overall on pages the rules
were not chosen on.

`OKUFLOW_UCT_TEST_REGION_RECONCILE=1` selects the experiment. It is off by
default because it changes recognized output, and no owner camera material was
available to check it on.

### What changed

With the switch on, `TensorRtRecognizer::RecognizeDevice` runs detection for
every region first. Regions with the same detector shape queue behind each
other and a shape change waits for the stream, so a 1920x1080 read waits twice
instead of three times and a 3840x2160 read twice instead of nine times. Each
region's recognition windows come back to the host with its count.
`region_reconcile::Reconcile` (`src/common/uct_region_reconcile.hpp`) then
decides from detector geometry alone which detections not to recognize:

- **A.** A tile detection that ends at one of its tile's internal edges, when
  a larger detection of the same line from another region contains its whole
  extent along the line and continues past that edge. A whole-frame detection
  counts only when the fragment's box is at least 24 px tall.
- **B.** A whole-frame detection that an unclipped tile detection reproduces.
- **C.** The later of two unclipped detections of the same line from two tiles.

The remaining windows are uploaded per region, as frozen-crop replay already
does, and recognition and word merging proceed unchanged. No detection is
moved, joined or invented. With the switch off the original path runs
untouched.

Rule A's first form dropped a fragment when another detection merely covered
60 percent of it; that could discard text the other detection never saw, so it
was replaced by containment before any measurement was kept. Its second form
let the whole-frame pass replace tile fragments at any size. On two held-out
pages with 7 to 9 px type that lost 36 and 26 labelled words: the whole-frame
pass sees the frame at half scale or less and its boxes for small type are too
tall to read well. The 24 px floor was chosen among 24, 32 and 40 on twenty
separate tuning pages and then checked on the held-out pages below.

### Speed

Evidence `r2-reconcile-ab-2-final`, switch off against on, same executable.

| Scene | Off ms | On ms | Speedup | Crops off / on | First read ms, off / on |
| --- | ---: | ---: | ---: | --- | --- |
| doc-camera-1080 | 206.67 | 107.95 | 1.914 | 329 / 147 | 206.58 / 107.01 |
| doc-camera-2160 | 476.01 | 261.02 | 1.824 | 731 / 340 | 473.38 / 262.51 |
| page-portrait-1654x2339 | 376.37 | 207.43 | 1.814 | 617 / 247 | 374.60 / 206.86 |
| doc-camera-turkish-1080 | 177.68 | 98.77 | 1.799 | 285 / 138 | 177.26 / 98.28 |
| doc-camera-tilt-1080 | 158.74 | 90.31 | 1.758 | 243 / 121 | 158.77 / 90.91 |
| table-1080 | 139.00 | 79.21 | 1.755 | 211 / 98 | 139.39 / 79.25 |
| slide-dark-1080 | 69.75 | 53.04 | 1.315 | 79 / 42 | 69.25 / 53.76 |
| slide-light-1080 | 67.24 | 51.79 | 1.298 | 78 / 41 | 67.29 / 52.11 |
| slide-light-2160 | 113.46 | 88.51 | 1.282 | 79 / 37 | 113.88 / 89.05 |
| board-lecture-05 (real) | 22.41 | 20.47 | 1.095 | 4 / 2 | 23.36 / 20.84 |
| signs-sparse-1080 | 31.11 | 28.72 | 1.083 | 10 / 5 | 30.99 / 28.55 |
| board-lecture-00 (real) | 22.77 | 21.81 | 1.044 | 3 / 2 | 23.16 / 22.03 |
| board-lecture-02 (real) | 25.52 | 24.60 | 1.037 | 3 / 2 | 26.30 / 24.88 |
| board-lecture-03 (real) | 25.52 | 24.81 | 1.028 | 5 / 4 | 25.74 / 24.60 |
| book-curved-1080 | 73.09 | 73.47 | 0.995 | 53 / 53 | 73.45 / 73.30 |
| board-lecture-01 (real) | 22.90 | 23.16 | 0.989 | 3 / 3 | 22.59 / 23.09 |
| board-lecture-04 (real) | 23.99 | 24.73 | 0.970 | 5 / 5 | 23.60 / 25.40 |
| no-text-1080 | 43.97 | 46.36 | 0.948 | 3 / 3 | 45.07 / 46.72 |

Geometric mean 1.286x warm and 1.286x for first reads. Crop counts are from a
separate profiled run with the earlier rule form (`r2-reconcile-profile-2`).
The real lecture frames and the scenes where nothing is dropped are inside the
noise floor. The textless scene is about 5 percent slower in both cohorts:
with nothing to drop, the experiment still copies every region's window buffer
to the host.

### Output

The switch changes output, so it is compared on labelled pages rather than
claimed identical. Exact-match counts are case-sensitive whole words against
the labels, as a multiset; "other words" are recognized words with no label
left to match. Evidence `r2-tune-1`, `r2-validate-heldout-a`, `-b`,
`r2-validate-v1`, `r2-validate-titles`.

| Pages | Labels | Exact off | Exact on | Other words off / on | Pages worse / better | Worst page |
| --- | ---: | ---: | ---: | --- | --- | ---: |
| Tuning, 20 (rule chosen here) | 8,154 | 6,163 | 6,369 | 1,065 / 1,052 | 1 / 14 | -1 |
| Held-out, 40 (first two groups) | 12,180 | 9,225 | 9,525 | 1,581 / 1,596 | 3 / 28 | -6 |
| Held-out, 48 (last three groups) | 5,323 | 3,076 | 3,259 | 1,399 / 1,408 | 7 / 38 | -10 |
| This repository's 12 generated scenes | 4,394 | 4,106 | 4,118 | 81 / 61 | 1 / 3 | -2 |

The 108 tuning and held-out pages are OkuRead's generated labelled screen
pages at 1920x1080 to 5120x1440, read in place; they were not built for this
work. On the 88 held-out pages exact matches rise from 12,301 to 12,784 and
ten pages get worse, the worst by ten words. On this repository's scenes the
words that disappear are mostly fragments and duplicates and the words that
appear are the completed forms; the Turkish page is the exception, where three
words lose a dotless i and recall falls from 0.9463 to 0.9403.

Title placements (private owner crops, 62): complete title read in 43 before
and 57 after, with no placement getting worse. The five that still fail are
the largest placements, where the title is wider than a tile and the
whole-frame pass does not detect it as one line either.

`regionReconcileRules` (model-free, in `uct_paddle_processing`) covers each
rule, the cases where nothing may be dropped (one region, an unknown layout,
two halves of one line with no whole-frame detection, a fragment that sticks
out of its neighbour, another row or angle, small type against the whole-frame
pass) and independence from detection order.

### Not established

No owner camera or document capture was available, and the real frames in the
scene set hold almost no text, so the effect on real boards and documents is
unmeasured. The experiment has no GPU contract test of its own beyond the
scene-set runs; cancellation during the detection phase drains the stream
before the input is released and was read, not exercised. Regional and
paused-detail reads have a single region and are unaffected.

## Round 7: host word merging without per-call rework (on by default, exact)

After every recognized crop the recognizer merges that crop's words into
everything already read (`PaddleMergeRecognition`). Each call rebuilt the
geometry of every word merged so far, compared the incoming words with every
word and every row, and sorted every row again, so a read with several
hundred crops repeated that work several hundred times. On the
[corpus timing](uct_corpus_timing.md) profile this host merging took 20.3 ms
of a 217 ms read on the dense 1920x1080 document, 56.8 ms of 394 ms on the
portrait page and 80.7 ms of 499 ms on the dense 3840x2160 document.

Targets stated before the work, for an exact change (no decision may change):

- Profiled `mergeHostMs`: 3840x2160 document 20 ms or less, portrait page
  15 ms or less, 1920x1080 document 6 ms or less.
- Unprofiled warm worker time at least 1.10x on `doc-camera-2160` and
  `page-portrait-1654x2339`, and no scene slower beyond the noise floor.
- Byte-identical complete output against the base build on the 18 scenes, the
  88 held-out labelled pages and the 62 title placements, with
  `OKUFLOW_UCT_TEST_REGION_RECONCILE` unset and set; `uct_merge_replay` and
  `uct_paddle_processing` pass.
- The replay benchmark reported for the old and the new merge.

| Target | Result |
| --- | --- |
| `mergeHostMs` 3840x2160 at most 20 ms | **Met**: 89.5 to 14.9 ms |
| `mergeHostMs` portrait at most 15 ms | **Met**: 63.4 to 10.2 ms |
| `mergeHostMs` 1920x1080 at most 6 ms | **Met**: 16.5 to 4.8 ms |
| Warm worker at least 1.10x on 3840x2160 and portrait | **Not met**: 1.028x and 1.026x (recheck 1.033x and 1.023x) |
| No scene slower beyond the noise floor | **Met** after a recheck (below) |
| Identical output everywhere listed | **Met**: 336 of 336 scene comparisons identical |
| `uct_merge_replay`, `uct_paddle_processing` pass | **Met** |
| Replay benchmark old versus new | **Reported** (below) |

There is no switch: the change is exact and is on by default.

### What changed

`PaddleMergeContext` (`include/okuflow/common/uct_paddle.hpp`, implemented in
`src/common/uct_paddle_processing.cpp`) keeps the merge's state for all calls
into one output. The GPU recognizer (`TensorRtRecognizer::RecognizeDevice`)
and the CPU recognizer each create one per read and pass it to the new
five-argument overload; the call site in `src/common/uct_tensorrt.cpp` changes
by two lines. The four-argument overload keeps its behaviour for every other
caller. The context holds:

- each merged word's shape, bounds, area, angle, height and centre, computed
  once;
- a uniform grid over word bounds. The overlap scan visits only words whose
  bounds pass `QRectF::intersects` against the incoming word's bounds, which
  is the merge's own first overlap test, and visits them in output order, so
  the first target, the rejection that stops the scan, the erased words and
  the preferred-row updates are those of a full scan;
- a grid over word centres for the row search. A word with no overlap and no
  usable preferred row computed its distance to every word of every row. A
  cached word can only give a finite distance when its centre lies in a band
  around the incoming word, because the distance counts a word only at a
  height within a factor of two, a centre within .35 of the larger height
  across the line and a gap of at most four times the smaller height along
  it. Only rows holding such a word are evaluated, exactly and in output
  order; the last empty row is found as before;
- per-row sort state. A row that gained or lost no word, was last sorted
  under a strict weak order and still gives the same baseline angle from its
  first word is not sorted again: sorting a sorted sequence again is then the
  identity. Every other row is sorted with the original comparison.

The context notices outside changes without comparing words. Between calls
it holds a shallow copy of the output, so any change through the container
API gives the output a new buffer, and the next call rebuilds the context
from the output. Above 1024 rows or 8192 words the context is released and
the unchanged per-call merge runs. The method, the bounds and the arguments
for each step are in [the merge optimization record](uct_merge_optimization.md).

No threshold, association rule, sort order, recognition or detection step
changed.

### Correctness evidence

`uct_merge_replay` compares every ordered word field, with exact double bits,
after every call. It now also has a frozen copy of the ec7282d merge
(`tests/uct_merge_baseline_ec72.cpp`), because the bb53 oracle predates
refused-ribbon protection inheritance. Every case is compared with ec7282d,
and with bb53 wherever no input carries inheritable protection. The candidate
runs with one context per case and through the four-argument overload. The
new cases are read-shaped call sequences built from every detector region of
a full read (3840x2160, 1654x2339 and 1920x1080, with tile-edge fragments,
overlapping recognition windows, refused ribbons, tilt, rectangle-only and
mixed shapes), context checks (outside edits between calls, one context
shared by two outputs, a moved context, a source size change), the 8192-word
and 1024-row bounds crossed in both directions, and row-search edges. See
`tests/README.md`. `uct_merge_replay`, `uct_paddle_processing` (49 slots) and
`uct_ribbon_ink_contract` (12 slots) pass on the final build.

End to end, the final build (`c1d4765`) was compared with a build of
`1b580f5` (round 2 as finalized), same corpus, complete ordered output
(text, order, dispositions, geometry, confidence, colours):

| Scene set | Reconcile unset | Reconcile set to 1 |
| --- | --- | --- |
| This repository's 18 scenes | 18 / 18 identical | 18 / 18 identical |
| Held-out labelled pages, first 40 | 40 / 40 identical | 40 / 40 identical |
| Held-out labelled pages, last 48 | 48 / 48 identical | 48 / 48 identical |
| Title placements (private owner crops), 62 | 62 / 62 identical | 62 / 62 identical |

Every arm was deterministic across its reads. Evidence `r7-final-v1-timing`
and `r7-final-identity-v1-reconcile`, `-okuread-labelled-a`, `-a-reconcile`,
`-okuread-labelled-b`, `-b-reconcile`, `-issue18-variants`,
`-issue18-variants-reconcile`. The intermediate build with the context but
without the row search (`c0e6261`) was compared the same way and was also
identical everywhere (`r7-v1-timing`, `r7-identity-*`).

### Merge host time

Profiled runs (`--profile`; timing from these is not quoted), base `1b580f5`
against the final build, `doc-camera-1080`, `doc-camera-2160` and
`page-portrait-1654x2339`, two reversed cohorts of five warm reads. Medians of
`mergeHostMs` over all ten warm reads; the cohort medians follow in
brackets. The field also covers CTC token decoding of each group, which this
round did not change.

| Scene | Words | Base ms | New ms | Ratio |
| --- | ---: | --- | --- | ---: |
| doc-camera-1080 | 491 | 16.45 (16.21, 16.87) | 4.77 (4.90, 4.52) | 3.4 |
| doc-camera-2160 | 1,199 | 89.48 (88.87, 89.77) | 14.94 (15.10, 14.38) | 6.0 |
| page-portrait-1654x2339 | 983 | 63.42 (72.70, 60.14) | 10.20 (11.84, 8.16) | 6.2 |

Evidence `r7-final-profile-merge`. With the context only (`r7-profile-merge`,
a busier run: its base cohorts disagree by up to 50 percent) the new medians
were 4.92, 21.69 and 10.23 ms. The 3840x2160 document then missed its target
in one cohort (24.08 ms), which is why the row search was added.

### Replay benchmark

`uct_merge_replay_tests --benchmark --only <case>`: five warm-up visits per
arm, then 31 alternating ABBA/BAAB blocks, all-field check after every call.
Median summed merge time of one visit (all calls of a case), base = the
frozen ec7282d merge, CPU only (Ryzen 9 5900X). The same frozen inputs
(SHA-256 `8b4d0600...a53986` for the 3840x2160 case) were used for both
builds. The benchmark had to run one case per process: all cases in one
process exceed the replay's four-minute execution budget, because the base
merge is slow on the read-shaped cases (kept as
`r7-replay-benchmark-rowsearch-full-budget-exceeded`).

| Case | Calls | Final words | Base ms | Context ms | Context and row search ms | Base / final |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| read-like-dense-3840x2160 | 599 | 1,607 | 85.05 / 81.72 | 6.43 | 5.32 | 15.4 |
| read-like-portrait-1654x2339 | 505 | 1,532 | 68.44 / 69.34 | 5.14 | 4.80 | 14.5 |
| read-like-dense-1920x1080 | 275 | 878 | 22.77 / 22.28 | 2.65 | 2.67 | 8.4 |
| read-like-ribbons-1920x1080 | 313 | 864 | 24.62 / 23.09 | 2.76 | 2.21 | 10.5 |
| read-like-tilted-1920x1080 | 321 | 1,022 | 29.63 / 28.80 | 3.40 | 3.29 | 8.8 |
| read-like-rectangles-1920x1080 | 170 | 524 | 8.56 / 8.38 | 1.30 | 1.31 | 6.4 |
| read-like-mixed-shapes-1920x1080 | 189 | 606 | 11.28 / 11.23 | 1.66 | 1.67 | 6.7 |
| generated-book-383 | 64 | 383 | 3.28 / 3.36 | 0.67 | 0.64 | 5.3 |
| generated-high-density-768 | 64 | 768 | 9.21 / 16.70 | 1.33 | 2.74 | 6.1 |

The base column gives the base arm of each build's own run (context run /
final run); the ratio is within the final run. `generated-atlas-175` and
`generated-frame55-262` gave 3.6 and 3.9 in the final run. The high-density case's
base arm doubled in the final run as well, so other load on the machine
rather than the change is the likely cause there. Evidence
`r7-replay-benchmark-context` and `r7-replay-benchmark-rowsearch`. The final
benchmark binary was built from the final merge source before the three
row-search replay cases were added; those cases are untimed.

### Worker time

Unprofiled, base `1b580f5` against the final build, this repository's 18
scenes, two reversed cohorts of five warm reads (evidence
`r7-final-v1-timing`).

| Scene | Base warm ms (cohorts) | New warm ms (cohorts) | Speedup | First read ms, base / new |
| --- | --- | --- | ---: | --- |
| doc-camera-2160 | 491.27 (482.07, 495.97) | 477.74 (476.54, 479.06) | 1.028 | 489.90 / 477.86 |
| page-portrait-1654x2339 | 386.57 (382.08, 392.48) | 376.70 (376.02, 388.05) | 1.026 | 382.14 / 377.66 |
| doc-camera-1080 | 212.31 (212.25, 212.38) | 210.20 (210.49, 207.61) | 1.010 | 213.30 / 209.86 |
| doc-camera-tilt-1080 | 163.85 (160.41, 164.42) | 161.00 (161.88, 159.38) | 1.018 | 163.51 / 161.43 |
| doc-camera-turkish-1080 | 183.60 (182.45, 184.88) | 185.55 (182.86, 188.63) | 0.989 | 179.85 / 192.59 |
| table-1080 | 144.26 (142.14, 147.40) | 145.25 (140.52, 149.79) | 0.993 | 144.68 / 142.46 |
| slide-light-2160 | 122.39 (117.20, 128.80) | 128.05 (123.91, 129.00) | 0.956 | 126.83 / 126.19 |
| book-curved-1080 | 75.46 (75.10, 78.86) | 80.89 (75.82, 81.32) | 0.933 | 79.78 / 78.75 |
| slide-dark-1080 | 75.29 (72.42, 75.42) | 78.27 (72.50, 80.32) | 0.962 | 75.10 / 74.21 |
| slide-light-1080 | 72.77 (70.34, 73.09) | 72.62 (71.37, 75.10) | 1.002 | 71.48 / 71.98 |
| no-text-1080 | 46.78 (46.79, 46.77) | 47.40 (47.00, 48.50) | 0.987 | 48.33 / 47.78 |
| signs-sparse-1080 | 32.81 (32.22, 33.65) | 32.78 (32.88, 32.68) | 1.001 | 33.01 / 32.57 |
| board-lecture-00 (real) | 24.17 (23.63, 24.83) | 23.99 (23.77, 24.04) | 1.007 | 24.22 / 25.77 |
| board-lecture-01 (real) | 25.09 (25.56, 24.87) | 24.45 (23.84, 26.36) | 1.026 | 24.03 / 25.17 |
| board-lecture-02 (real) | 27.27 (26.40, 28.36) | 27.76 (26.50, 27.77) | 0.982 | 28.71 / 29.32 |
| board-lecture-03 (real) | 27.81 (26.50, 28.75) | 27.79 (29.34, 27.75) | 1.001 | 28.07 / 27.79 |
| board-lecture-04 (real) | 25.62 (24.72, 27.66) | 25.91 (25.86, 27.84) | 0.989 | 27.21 / 24.85 |
| board-lecture-05 (real) | 23.22 (22.67, 23.94) | 22.88 (22.23, 23.64) | 1.015 | 25.11 / 23.90 |

Geometric mean 0.996x warm and 1.005x for first reads. The three scenes that
read 4 to 7 percent slower had cohorts that disagree with each other, and
they were measured again in a separate run with the two target scenes
(`r7-final-v1-timing-recheck`, same protocol):

| Scene | Base warm ms (cohorts) | New warm ms (cohorts) | Speedup | First read ms, base / new |
| --- | --- | --- | ---: | --- |
| doc-camera-2160 | 487.73 (488.77, 481.39) | 471.94 (470.88, 478.38) | 1.033 | 485.62 / 476.09 |
| page-portrait-1654x2339 | 385.45 (386.86, 382.86) | 376.83 (376.70, 379.77) | 1.023 | 389.49 / 382.15 |
| doc-camera-turkish-1080 | 186.74 (187.95, 179.94) | 181.33 (179.80, 184.42) | 1.030 | 183.45 / 183.10 |
| book-curved-1080 | 77.78 (78.98, 74.38) | 75.66 (74.29, 78.38) | 1.028 | 76.39 / 78.98 |
| slide-dark-1080 | 73.88 (74.37, 71.35) | 73.81 (76.47, 72.83) | 1.001 | 76.38 / 72.89 |
| slide-light-2160 | 119.49 (125.47, 116.03) | 116.90 (116.47, 117.13) | 1.022 | 122.41 / 115.90 |
| no-text-1080 | 48.51 (48.31, 48.72) | 47.45 (47.04, 48.13) | 1.022 | 49.84 / 55.35 |

None of them is slower there; the single textless first read (55.35 ms) is
one sample. The intermediate context-only build measured 1.024x and 1.022x on
the two target scenes and 1.002x on the geometric mean (`r7-v1-timing`).

The worker gain is far smaller than the merge time removed: on the 3840x2160
document about 75 ms of host merging went away and the read became about
14 ms shorter. Recognition groups are issued ahead of the merge (a two-slot
look-ahead), so most of the merge already ran while the GPU worked on the
next group, and the read is limited by GPU work. That is an inference from
these two measurements; this round did not measure the overlap directly.
Reaching 1.10x on worker time would need a change on the GPU side of the
read, which is outside this round's scope.

### Not established

The replay cases are constructed call sequences, not captured detector
calls. The CPU recognizer uses the context too, but its speed was not
measured. Paused-detail and regional reads have one region and few calls and
were not measured separately. `CHANGELOG.md` is unchanged: output is
identical and the worker-time change is inside the noise floor on most
scenes.
