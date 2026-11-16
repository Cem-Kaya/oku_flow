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
two owner-supplied stylized title crops the unchanged build returns the
complete title in 43 and a fragment of it in 19, with curved processing on or
off.

Targets stated before the work, and the result with the final rules:

| Target | Result |
| --- | --- |
| Halve the recognizer crops on text scenes | Met on the dense scenes (329 to 151, 731 to 343, 617 to 319, 211 to 97); slides 78 to 53 and 79 to 58: not met |
| Dense 1920x1080 document from 204 ms to 120 ms or less | Met: 205.8 to 111.0 ms |
| Repair most of the 19 fragmented titles | Met: 12 of 19 repaired (55 of 62 complete), none worse |
| Lose no labelled words overall on pages the rules were not chosen on | Met in total (+30 exact matches over 112 pages); not met on one group of 40 pages (-7) |

`OKUFLOW_UCT_TEST_REGION_RECONCILE=1` selects the experiment. It is off by
default because it changes recognized output, and no owner camera material was
available to check it on. `OKUFLOW_UCT_TEST_REGION_RECONCILE=keep` runs the
same two-phase path but recognizes every detection; it exists to show that the
path itself changes nothing.

### What changed

With the switch on, `TensorRtRecognizer::RecognizeDevice` runs detection for
every region first. Regions with the same detector shape queue behind each
other and a shape change waits for the stream. Each region's recognition
windows come back to the host with its count.
`region_reconcile::Reconcile` (`src/common/uct_region_reconcile.hpp`) then
decides from detector geometry alone which detections not to recognize. One
relation carries every decision: detection S may stand in for detection D when
both are straight, come from different regions, lie on the same line (turn at
most 0.175 rad, centre lines within 0.35 of the smaller height), have
comparable height (ratio at least 0.7) and S spans everything D spans along
the line, within a quarter of D's height and at most 48 px. Then:

- **A.** A tile detection that ends at one of its tile's internal edges is
  left out when a larger detection stands in for it and continues past that
  edge. The whole-frame pass may stand in only for boxes at least 24 px tall.
- **B.** A whole-frame detection is left out when an unclipped tile detection
  stands in for it.
- **C.** An unclipped tile detection is left out when an unclipped detection
  from another tile stands in for it; if each stands in for the other, the
  lower region index keeps it.

Each rule is evaluated on the full input, so no rule sees another's outcome
and the order of detections cannot matter. A final pass restores every
detection that no kept detection stands in for, repeated until nothing
changes, so no chain of decisions can leave text that only a dropped detection
covered. Curved detections, fitted or refused, take no part: their quad is
only an envelope and a refused ribbon carries ink protection that must reach
decoding. The remaining windows are uploaded per region and recognition and
word merging proceed unchanged. No detection is moved, joined or invented.
With the switch off the original path runs untouched. Regional and
paused-detail reads have a single region, and crop-diagnostic and frozen-replay
reads bypass the experiment.

### How the rules got here

The first form of rule A dropped a fragment when another detection merely
covered 60 percent of it; that could discard text the other detection never
saw and was replaced by containment before any measurement was kept. Letting
the whole-frame pass replace tile fragments at any size lost 36 and 26
labelled words on two pages with 7 to 9 px type: the whole-frame pass sees the
frame at half scale or less and its boxes for small type are too tall to read
well. The 24 px floor was chosen among 24, 32 and 40 on twenty tuning pages.

The version first submitted for review (`ec7282d`) used looser tests: an
overlap test for B and C, height ratio 0.5, no coverage pass, curved
detections included. It measured 1.286x warm, +206 exact label matches on the
tuning pages, +483 on 88 other pages and 57 of 62 titles. The council (below)
showed that it could lose text through chains of decisions, drop a refused
ribbon's protection and depend on visiting order, so the rules above replaced
it. They keep the speed and the title repairs but not the reading gain: the
tuning pages now gain 3 exact matches instead of 206. Which of the stricter
conditions removed the gain is not established; it is the subject of round 6.

After the review a floor in whole-frame detector pixels (12, which is 24 px at
1920 wide and 48 px at 3840 wide) was tried in place of the 24 px floor, as a
reviewer suggested. On the tuning pages and one fresh page set it kept fewer
words (+4 and +6 exact matches against +3 and +10) and gave up the speed gain
on 3840x2160 pages (crops 731 to 729), so the frame-pixel floor stayed
(`r2k-tune`, `r2k-fresh1`). Limits of 16, 32 and 48 px and no limit on the
containment tolerance read the labelled pages identically; 48 is the smallest
that repairs as many titles as no limit (51, 54, 55, 55 of 62; `r2k2-*`).

### Speed

Evidence `r2z-ab-v1`, build `1b580f5`, switch off against on, same executable.
Crop counts are from a separate profiled run of the same build
(`r2z-profile-v1`).

| Scene | Off ms | On ms | Speedup | Crops off / on | First read ms, off / on |
| --- | ---: | ---: | ---: | --- | --- |
| doc-camera-1080 | 205.83 | 110.98 | 1.855 | 329 / 151 | 206.61 / 112.02 |
| doc-camera-turkish-1080 | 182.05 | 101.84 | 1.788 | 285 / 140 | 184.67 / 100.64 |
| doc-camera-2160 | 478.53 | 268.54 | 1.782 | 731 / 343 | 477.19 / 266.12 |
| page-portrait-1654x2339 | 382.24 | 223.41 | 1.711 | 617 / 319 | 381.99 / 224.02 |
| table-1080 | 141.05 | 84.19 | 1.675 | 211 / 97 | 140.27 / 81.45 |
| doc-camera-tilt-1080 | 158.54 | 95.74 | 1.656 | 243 / 122 | 157.59 / 102.69 |
| slide-light-2160 | 117.15 | 90.70 | 1.292 | 79 / 38 | 116.72 / 89.95 |
| slide-dark-1080 | 71.83 | 56.56 | 1.270 | 79 / 58 | 71.68 / 56.76 |
| slide-light-1080 | 68.66 | 55.24 | 1.243 | 78 / 53 | 69.34 / 60.79 |
| signs-sparse-1080 | 34.78 | 30.06 | 1.157 | 10 / 5 | 34.56 / 32.16 |
| board-lecture-03 (real) | 28.67 | 25.26 | 1.135 | 5 / 4 | 27.89 / 25.50 |
| board-lecture-02 (real) | 28.59 | 25.34 | 1.128 | 3 / 2 | 29.56 / 25.70 |
| board-lecture-04 (real) | 27.87 | 25.74 | 1.083 | 5 / 5 | 27.21 / 25.93 |
| no-text-1080 | 48.36 | 46.97 | 1.030 | 3 / 3 | 49.57 / 47.42 |
| board-lecture-00 (real) | 24.60 | 24.26 | 1.014 | 3 / 3 | 26.86 / 25.39 |
| board-lecture-05 (real) | 23.88 | 24.09 | 0.992 | 4 / 3 | 24.61 / 23.75 |
| board-lecture-01 (real) | 24.79 | 25.33 | 0.978 | 3 / 3 | 25.78 / 24.03 |
| book-curved-1080 | 74.05 | 75.92 | 0.975 | 53 / 53 | 74.55 / 73.75 |

Geometric mean 1.285x warm and 1.284x for first reads. Other jobs were using
the machine during this run and the two cohorts of the short scenes differ by
up to 25 percent, so the rows from signs-sparse down are inside the noise.
The bent page, where every detection is curved and nothing is dropped, is 2.5
percent slower in this run; the added cost of the path (reading the windows
back and uploading the kept ones) is not separately measured.

### Output

The switch changes output, so it is compared on labelled pages rather than
claimed identical. Exact-match counts are case-sensitive whole words against
the labels, as a multiset; "other words" are recognized words with no label
left to match. The metric does not see word position or order. All rows are
the final build `1b580f5` (`r2z-q-*`, `r2z-ab-v1`).

| Pages | Labels | Exact off | Exact on | Other words off / on | Pages worse / better | Worst page |
| --- | ---: | ---: | ---: | --- | --- | ---: |
| Tuning, 20 (constants chosen here) | 8,154 | 6,163 | 6,166 | 1,065 / 1,042 | 6 / 6 | -4 |
| Fresh generated set 1, 12 (constants chosen here) | 7,381 | 7,328 | 7,338 | 65 / 39 | 2 / 7 | -1 |
| Development, 40 | 12,180 | 9,225 | 9,218 | 1,581 / 1,537 | 14 / 11 | -6 |
| Development, 48 | 5,323 | 3,076 | 3,094 | 1,399 / 1,346 | 3 / 17 | -1 |
| This repository's 12 generated scenes | 4,394 | 4,106 | 4,117 | 81 / 62 | 1 / 3 | -2 |
| Fresh generated set 2, 12 (not looked at before the final build) | 6,610 | 6,577 | 6,585 | 65 / 36 | 1 / 6 | -2 |

The tuning and development pages are OkuRead's generated labelled screen pages
at 1920x1080 to 5120x1440, read in place. The two development groups were
used while the first rule forms were being worked out, so they are not
held-out; only fresh set 2 is. The fresh sets come from
`scripts/uct/perf_corpus.py --variant N`, which redraws this repository's
twelve generated scenes with other text. Fewer other words on every set is
the tile-edge fragments and duplicates disappearing. On the Turkish page three
words lose a dotless i and recall falls from 0.9463 to 0.9403.

Title placements (private owner crops, 62; `r2z-q-titles`): the complete title
is read in 43 with the switch off and 55 with it on, and no placement gets
worse. The seven that still fail are the largest placements.

Pass-through mode (`keep`) is byte-identical to the switch-off path on 18 of
18 scenes, 40 of 40 development pages and 62 of 62 title placements
(`r2z-keep-*`).

Tests: `regionReconcileRules` (model-free, in `uct_paddle_processing`) covers
each rule, the cases where nothing may be dropped (one region, an unknown
layout, two halves of one line with no whole-frame detection, a fragment that
sticks out of its neighbour, another row or angle, a whole-frame box that
merged two lines, small type against the whole-frame pass at 1920 and 3840
wide, curved detections on either side), the reviewers' chains, tile order
independence and 300 random layouts checked for coverage.
`gpuRecognitionWindowsMarkEverySubWindow` (in `uct_detect_kernels_cuda`) holds
the GPU window layout the host parser relies on.

### Not established

No owner camera or document capture was available, and the real frames in the
scene set hold almost no text, so the effect on real boards and documents is
unmeasured. Cancellation during the detection phase drains the stream before
the input is released; that was read and reviewed, not exercised by a test.

### Review

Council, as submitted (`ec7282d`): Claude Opus 5.5 6/10, GPT-6.1 Sol 6/10,
Gemini 4 Hydrogen 6/10. Applied: the single relation with a coverage pass and
order-independent evaluation, curved detections excluded, height ratio 0.7, a
bounded containment tolerance, kept-window upload without the frozen-replay
queue accounting, a stream drain before buffers are released on cancellation,
restorations and window-layout validity in the profile, the pass-through mode
and its identity runs, the GPU window-layout test, a fresh untouched page set
for the final check, crop counts taken from the final build, and the wording
of the page groups. Tried and not kept: the floor in detector pixels (above).
The reviewed rules have not been through a second council pass; round 6
revisits them.
