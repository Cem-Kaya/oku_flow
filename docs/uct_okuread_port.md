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

## Round 6: reconcile safety versus reading gain (negative result)

Targets: tuning gain at least 150 exact words with no page losing more than
three; each confirmation set must retain total exact matches, not increase
unmatched words, and lose at most five per page; at least 57/62 complete titles
without regression; v1 warm geometric mean at least 1.28x and dense 1080p at
least 1.8x, without a scene regression beyond the noise floor. Retain coverage,
order independence and all fitted/refused ribbons.

Result: retain the reviewed `1b580f5` rules. No production code or threshold
changes. The experiment remains OFF by default. The earlier reading gain
cannot be recovered simply by weakening containment: adding only the required
curved/refused-ribbon protection to the original rules removes its aggregate
tuning gain, before adding the reviewed height/span/coverage restrictions.
This is a negative result for that mechanism, not a proof that every future
geometry-only design must fail.

### Diagnosis

Built `ec7282d` and `1b580f5` in separate external source/build trees. Temporary
geometry-only dumps were removed from the final binary. Evidence:
`r6-diagnosis-tune2`, `r6-diagnosis-decisions`, `r6-tune-ablation` under the
private program evidence root documented above. No recognized title text or
owner imagery enters Git.

The external CPU replay checks both builds' actual decisions against the
reconstructed rules, verifies equal detector geometry, and records every
input quad, rule witness and restoration in `all-detections.json` (2,468
records, first and warm reads). `actual-old-witnesses.json` lists each
old-drop/reviewed-keep decision and every old-rule witness's failed reviewed
conditions. Counts below use first reads only and each detection once; where
several old witnesses exist, classify by the one with fewest failed conditions.

| Obstruction to the best old witness | Detections |
| --- | ---: |
| Curved/refused ribbon, alone or with another condition | 201 |
| Height ratio alone | 46 |
| Baseline span alone | 5 |
| Height ratio and span | 2 |
| Final coverage/rule selection | 2 |
| Total old-drop/reviewed-keep | 256 |

There is no offset-only obstruction in this classification; one combined
curved/height/offset case occurs. This distinguishes relevant old witnesses
from arbitrary nearby boxes. Both valid and refused ribbon flags were
collapsed to the existing `Detection::curved`; this diagnosis does not
identify which of those two flags accounts for each difference.

RTX 4090, driver 621.14, Ryzen 9 5900X; MSVC 19.44 Release, Qt 6.12,
CUDA 12.9.86, TensorRT 11.4.0.106, medium/medium cached BF16-linear plans,
mixed arithmetic, N8 fixed1024. Tuning-only ablation `r6-tune-ablation`,
one first and one warm read, identical executable across these arms:

| Rules | Exact / 8,154 labels | Other | Worse / better pages | Worst delta |
| --- | ---: | ---: | --- | ---: |
| Switch off | 6,163 | 1,065 | 0 / 0 | 0 |
| Original, unprotected | 6,369 | 1,052 | 1 / 14 | -1 |
| Original plus curved/refused protection only | 6,163 | 1,032 | 7 / 5 | -3 |

For example, tuning pages 07, 08 and 18 have off/original/protected exact counts
104/133/104, 440/458/437 and 426/459/423 respectively. The exact quads and
witnesses are in private evidence. Keeping envelope geometry that looks
straight is insufficient reason to discard the refused-ribbon ink-protection
contract. No asymmetric tolerance or joint-span rule was selected: only five
changed straight detections are blocked solely by the span condition, while
the required ribbon protection alone removes all aggregate old-rule gain.

### Retained contract and tests

Invariant: no detection is omitted unless a kept straight detection from
another region spans its baseline extent within the stated tolerance and
passes the same-line/height/whole-frame-resolution checks. This is detector
geometry coverage, not a proof of correct recognized text or pixel coverage.
The strict `Replaces` relation and final monotone restoration pass remain
unchanged; fitted and refused ribbons take no part in any removal.

`regionReconcileRules` adds a chain where unequal height-based tolerances
require restoring a fragment, plus a large straight-looking ribbon-envelope
case. All 300 deterministic random layouts now also check shuffled keep
choices, unchanged quads/regions/ribbon flags, retained curved detections and
statistics conservation. Existing two-line, large-title, 3840-wide and other
chain tests remain. The final slot passes; this is scoped model-free evidence,
not the shipping matrix. No GUI tests, installed OkuRead, Magnifier, camera,
network publishing or default activation were used.

### Final confirmation

Final binary SHA-256:
`dc9b8f4edacef577479759929ca1808bc235eec7440203bdda5f15cbaa222ad9`.
The command transcript and complete results are in the private `r6-handback.md`.
Variants 3 and 4 were generated only after that binary existed; constants were
not changed after confirmation. Quality runs use one first and one warm read
with profiling; their timings are not performance evidence. Speed uses the
separate unprofiled two reversed cohorts of five warm reads.

All numbers: RTX 4090 / driver 621.14 / Ryzen 9 5900X; MSVC 19.44 Release, Qt 6.12.0, CUDA 12.9.86, TRT 11.4.0.106, cached medium/medium BF16-linear mixed arithmetic, N8 fixed1024.

| Set | Exact off / on | Other off / on | Worse / better | Worst | Restorations first / all reads | Quality target |
|---|---:|---:|---:|---:|---:|---|
| okuread-tune | 6163 / 6166 | 1065 / 1042 | 6 / 6 | -4 | 2 / 4 | NOT MET |
| v1-variant1 | 7328 / 7338 | 65 / 39 | 2 / 7 | -1 | 2 / 4 | MET |
| okuread-labelled-a | 9225 / 9218 | 1581 / 1537 | 14 / 11 | -6 | 2 / 4 | NOT MET |
| okuread-labelled-b | 3076 / 3094 | 1399 / 1346 | 3 / 17 | -1 | 3 / 6 | MET |
| v1-variant3 | 7889 / 7901 | 95 / 66 | 1 / 6 | -1 | 4 / 8 | MET |
| v1-variant4 | 7256 / 7268 | 77 / 42 | 2 / 7 | -1 | 4 / 8 | MET |
| v1-speed | 4106 / 4117 | 81 / 62 | 1 / 3 | -2 | 2 / 4 | MET |

Keep complete ordered output identical: okuread-labelled-a: 40/40 scenes.


Keep complete ordered output identical: v1-speed: 18/18 scenes.


Titles (counts only): base 43/62, on 55/62, keep 43/62; complete-off becoming incomplete-on: 0.
Title restorations first / all reads: 0 / 0.
Keep byte identity on titles: 62/62.

Unprofiled worker timing (ms); two reversed cohorts, five warm passes each.

| Scene | Warm off / on | First off / on | Warm x | Cohort 0 / 1 warm x |
|---|---:|---:|---:|---:|
| doc-camera-1080 | 208.67 / 109.29 | 208.86 / 109.75 | 1.909 | 1.919 / 1.895 |
| doc-camera-tilt-1080 | 159.84 / 95.64 | 160.00 / 96.43 | 1.671 | 1.668 / 1.674 |
| doc-camera-2160 | 479.16 / 272.66 | 478.38 / 280.30 | 1.757 | 1.795 / 1.718 |
| doc-camera-turkish-1080 | 180.39 / 102.12 | 179.27 / 104.69 | 1.766 | 1.787 / 1.749 |
| book-curved-1080 | 75.66 / 76.68 | 73.48 / 75.00 | 0.987 | 0.983 / 1.029 |
| slide-light-1080 | 67.91 / 57.38 | 72.96 / 62.35 | 1.184 | 1.186 / 1.179 |
| slide-dark-1080 | 69.80 / 58.81 | 70.66 / 61.12 | 1.187 | 1.213 / 1.180 |
| slide-light-2160 | 117.56 / 92.06 | 115.62 / 94.27 | 1.277 | 1.256 / 1.305 |
| table-1080 | 141.25 / 82.27 | 142.16 / 81.94 | 1.717 | 1.742 / 1.712 |
| page-portrait-1654x2339 | 380.17 / 229.27 | 381.63 / 225.21 | 1.658 | 1.635 / 1.664 |
| signs-sparse-1080 | 32.46 / 30.93 | 32.50 / 29.85 | 1.049 | 1.063 / 1.103 |
| no-text-1080 | 46.07 / 47.64 | 47.45 / 49.02 | 0.967 | 0.967 / 0.966 |
| board-lecture-00 | 23.50 / 25.03 | 25.13 / 26.06 | 0.939 | 0.929 / 0.971 |
| board-lecture-01 | 24.59 / 25.31 | 25.63 / 24.82 | 0.971 | 0.957 / 0.988 |
| board-lecture-02 | 27.36 / 26.36 | 27.90 / 26.89 | 1.038 | 1.005 / 1.013 |
| board-lecture-03 | 28.10 / 26.56 | 27.53 / 35.47 | 1.058 | 1.051 / 1.054 |
| board-lecture-04 | 27.10 / 27.15 | 25.71 / 27.42 | 0.998 | 0.944 / 1.049 |
| board-lecture-05 | 23.23 / 24.79 | 23.42 / 24.87 | 0.937 | 0.953 / 0.977 |

Geometric means: {"warm": 1.2391162151378239, "firstRead": 1.2142088106554645, "scenesIdentical": 6, "scenes": 18}
Cohort warm geometric means: [1.235631167111847, 1.2515299168366205]

Target decisions: tuning NOT MET (+3, worst -4); labelled A NOT MET (total -7,
worst -6); labelled B, v1, fresh variants 3 and 4 quality MET; titles NOT MET
(55 rather than 57, no regression). V1 warm aggregate NOT MET (1.239x versus
1.28x); dense 1080p MET (1.909x versus 1.8x). The no-scene-regression target
is NOT MET conservatively: no-text is 3.4% slower in both cohorts, beyond the
2% dense-scene reference floor. Sparse lecture regressions remain within the
12% sparse-scene floor. First-read geometric mean is 1.214x; the first-read
columns exclude engine startup and are not cold-cache measurements.

The geometry invariant is retained by construction and passes all random
layouts and the complete tuning-geometry replay. Final corpus profile lines
show the restorations above (v1 uses its separate profile run); they are not
an independent pixel/text coverage audit of every confirmation page. No
recognition correctness follows from the relation. The retained coverage
algorithm ensures every omitted detection has a kept `Replaces` witness;
curved/refused detections are always kept. No general camera safety claim is
made from that formal invariant.

`uct_paddle_processing` through `run_test_gate.ps1` is SCOPED PASS: 1/349
CTest cases, no skipped Qt rows or unresolved failures; full matrix and GUI
acceptance were not run. `agent_check.ps1` configured CPU then returned
FULL-required for missing dependency inventory; the delegate guide forbids
starting that full matrix. The first chain fixture had incorrect tile-edge
geometry and failed; its corrected, rebuilt fixture passes. All failed logs
remain external. An initial dump hit QtTest's warning cap and was replaced
by direct diagnostic output; its evidence remains, but only the second full
dump supports the diagnosis. Variant generation initially failed because the
lease helper's Python lacked NumPy; the existing Anaconda Python generated
both untouched sets without installing packages or changing system settings.

All 24 final arm/cohort receipts bind the same executable and report cached
medium BF16 plans, deterministic output and zero malformed profile lines.
Private diagnostic and ablation binaries were rebuilt during investigation;
their evidence hashes identify those historical builds, not the final binary.
No new runtime switch was shipped and no default or dependency changed.

Turning this experiment on still needs the missed tuning/held-out/title/speed
criteria resolved while retaining ribbon protection and the geometry invariant,
plus independent coverage review and owner camera/document captures. The
corpora contain no owner camera capture. Worker timing, generated-page scores,
model-free geometry tests and private title counts do not establish native
presentation, real-camera quality, accessibility or release acceptance.
