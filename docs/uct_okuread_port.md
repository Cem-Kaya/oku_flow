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
| Cooperative box scoring | Round 1, rewritten: OkuRead's exact variant gave no gain here. Exact, selectable; superseded as the default by the tree sum in round 9 |
| Fixed-tree box scoring | Round 9, on by default on every GPU: it rounds differently, which the owner accepted; no corpus page changed |
| Collect detections across tiles before recognition; a complete detection supersedes its covered edge fragment | Round 2, private switch, off: changes output |
| Replay TensorRT enqueue as CUDA graphs, one execution context per cached shape | Round 3, made robust in round 10 (no startup or first-read cost, failures fall back), private switch, off: identical output; reads right after new shapes appear pay for making contexts |
| Small recognizer first, Medium retries for doubtful crops | Round 4, built and measured, not merged: misreads `O0` as `00`, slower on sparse frames |
| Streaming verification, hashing each file once, faster SHA-256 | Round 5, on by default: startup to first GPU result 3.85 s to 0.85 s, identical output |
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
warps could share one large component and stay exact. That route was not
needed: round 9 makes the tree sum the default instead.

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

## Round 3: TensorRT enqueue replayed as CUDA graphs (private switch, off)

Targets stated before the work: byte-identical complete output on all 18
scenes in both cohorts, on the 62 title placements and on one labelled set;
warm worker time at least 1.20x on the six real lecture frames and at least
1.05x on dense scenes; the first read of each scene no more than 10 percent
slower, or an accounting of what the first use costs and after how many reads
it is repaid, also with `OKUFLOW_UCT_TEST_REGION_RECONCILE=1` in both arms.

Result: output identical everywhere it was measured; lecture frames 1.23x to
1.35x pooled (one frame is 1.197x in one of its two cohorts) and dense
documents 1.09x to 1.10x warm. First reads, as this harness measures them, are
within target except where a scene brings a frame size the process has not
read before: 2 of 18 scenes, accounted for below. The harness reads its first
scene during startup, so later scenes' first reads inherit contexts and
graphs; a scene's first read in a fresh process was not measured. Startup
takes 0.36 to 0.98 s longer.

`OKUFLOW_UCT_TEST_TRT_GRAPH=1` selects it. It stays off by default in this
round; see "Default" below.

### Why it matters here

The baseline profile spends 13 to 18 ms of a 22 to 26 ms lecture-frame read
on the host inside `enqueueV3`, and 93 of 204 ms on the dense page. A
1920x1080 read runs the detector at two shapes (the whole frame and two equal
tiles) and the recognizer at batch eight plus one partial batch per region, so
one graph per engine would be recaptured on almost every call.

### What changed

`Network::Run` (`src/common/uct_tensorrt.cpp`) has a second path, decided by
`graph_cache::Policy` (`src/common/uct_graph_cache.hpp`):

- **One context per cached shape, at most one graph per context.** Keys are
  batch, height and width; the input and output addresses are checked on
  every call. Reshaping, readdressing or recycling a context first destroys
  its graph. No context is shared by two live graphs (OkuRead found that this
  changed later output).
- **One activation arena per engine.** The contexts are created with
  `kUSER_MANAGED` memory and all point at one buffer of
  `getDeviceMemorySizeV2()` bytes. That is legal because they all run in order
  on the State's one stream. The preparation context, which owns the same
  amount of memory, is released when replay is enabled, so the arena adds
  nothing.
- **Output storage never moves.** The shared output tensor is sized once for
  the profile's largest input (960x960 for the detector, batch eight for the
  recognizer), so no graph can hold a stale output address.
- **Plain first, capture later, never behind queued work.** A new binding runs
  as a plain enqueue; a later call with the same binding is captured
  (`cudaStreamBeginCapture` in thread-local mode, `enqueueV3`,
  `cudaStreamEndCapture`, instantiate, upload) and launched; after that it
  replays. `RecognizeDevice` forbids capture for a recognition look-ahead
  group and for a detection queued behind an earlier equal-shaped detection of
  the same read, so nothing is captured while earlier inference is in flight.
  The existing rule that a shape change waits for the stream is unchanged.
- **Bounded, least recently used.** Eight shapes per engine by default (the
  recognizer's eight batch sizes on the fixed canvas); when full, the least
  recently used slot is drained, loses its graph and is reshaped. Its context
  is reused, not recreated.
- **Refusals fall back.** Capture-specific CUDA statuses (unsupported,
  invalidated, isolation, implicit, merge, unjoined, out of memory, not
  supported, and an invalid value from instantiate or upload) end the capture,
  run the call as a plain enqueue and mark the shape so it is never captured
  again; with the 32nd refused shape capture stops. With less than 256 MiB
  free a capture is only put off: the call runs plainly and the shape is
  offered again later. A TensorRT enqueue that fails inside a capture without
  a capture-specific CUDA status is a fault, like any failed enqueue. Every
  other failure is fatal, as before, and goes through the existing State
  retirement. No refusal occurred in the profiled runs; the timing runs
  recorded no receipts.
- **Lifetime.** A drain (`cudaStreamSynchronize`) precedes every graph
  destruction and every reshape of a used context. Otherwise graphs and
  contexts are destroyed with their Network, which the State releases only
  after its stream is known complete (`RetireState`). The capture is always
  ended inside `Network::Capture`, before any cancellation check or exception
  can intervene.
- **Contexts are made during preparation.** Creating an execution context
  took about 90 ms for the detector and 60 ms for the recognizer. The first
  version created them on demand, which added up to 270 ms to first reads (for
  example doc-camera-tilt 160 to 271 ms, `r3-graph-smoke-1`). Now preparation
  creates three detector contexts (the whole frame and the tiles of one frame
  size) and one recognizer context per batch size. That runs no inference.

With the switch off, `Run` takes the original path unchanged.
`OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES` (1 to 16) changes the bound, for the
comparison below only.

### Cache shape, chosen by measurement

Evidence `r3-graph-ab-1`: two reversed cohorts of five warm reads, four arms
on one executable. "Single" is one graph per engine (bound 1).

| Arm | Warm, geometric mean | First read, geometric mean | Output |
| --- | ---: | ---: | --- |
| Single (one graph per engine) | 1.015x | 1.004x | identical 18/18 |
| Four shapes per engine | 1.174x | 1.111x | identical 18/18 |
| Eight shapes per engine (default of the switch) | 1.178x | 1.103x | identical 18/18 |

One graph per engine recaptures on nearly every shape change and adds a drain
each time, so it gains nothing. Four shapes evict recognizer batch sizes about
twice per 1920x1080 read, each eviction a drain plus a plain enqueue; on this
scene set that cost is inside the noise, but eight shapes never evict on the
fixed canvas, so eight is the switch's default.

### Measured, eight shapes

Evidence `r3-graph-ab-1`, RTX 4090 (driver 621.14, display not driven), Ryzen
9 5900X, MSVC 19.44 Release, CUDA 12.9.86, TensorRT 11.4.0.106, medium BF16
detector and recognizer, commit `4dcbbb4`. Warm is the median of ten warm
reads; the cohort medians of the new arm are in brackets. First read is the
median over the two cohorts of the scene's first read in each process.

| Scene | Off ms | On ms | Warm speedup | First read ms, off / on |
| --- | ---: | ---: | ---: | --- |
| board-lecture-01 (real) | 24.99 | 18.54 (18.33, 18.69) | 1.348 | 23.28 / 18.42 |
| board-lecture-04 (real) | 26.33 | 19.93 (20.07, 19.90) | 1.321 | 26.56 / 26.58 |
| board-lecture-03 (real) | 27.37 | 21.19 (21.17, 21.21) | 1.292 | 26.98 / 21.15 |
| board-lecture-00 (real) | 23.75 | 19.10 (19.05, 19.14) | 1.244 | 25.44 / 19.08 |
| board-lecture-02 (real) | 27.17 | 21.96 (21.71, 22.17) | 1.237 | 28.97 / 22.44 |
| board-lecture-05 (real) | 23.85 | 19.33 (18.89, 19.95) | 1.234 | 23.76 / 20.61 |
| signs-sparse-1080 | 32.58 | 27.13 | 1.201 | 32.86 / 28.72 |
| slide-light-2160 | 120.01 | 100.77 | 1.191 | 116.78 / 149.41 |
| slide-dark-1080 | 72.34 | 62.01 | 1.167 | 72.67 / 61.13 |
| slide-light-1080 | 69.47 | 60.88 | 1.141 | 69.36 / 60.77 |
| no-text-1080 | 47.31 | 41.53 | 1.139 | 47.43 / 41.57 |
| book-curved-1080 | 75.05 | 66.05 | 1.136 | 76.72 / 67.41 |
| table-1080 | 142.21 | 127.77 | 1.113 | 145.30 / 127.96 |
| doc-camera-2160 | 484.97 | 440.04 | 1.102 | 480.84 / 449.84 |
| doc-camera-turkish-1080 | 182.48 | 165.60 | 1.102 | 181.94 / 167.91 |
| doc-camera-tilt-1080 | 159.99 | 145.72 | 1.098 | 160.29 / 149.57 |
| page-portrait-1654x2339 | 386.34 | 352.78 | 1.095 | 380.34 / 506.29 |
| doc-camera-1080 | 206.89 | 189.53 | 1.092 | 206.88 / 190.27 |

Geometric mean 1.178x warm and 1.103x for first reads. Every lecture frame
is at least 1.23x pooled; compared cohort by cohort the lowest is
board-lecture-05 at 1.26x and 1.197x. The dense documents gain 1.09x to 1.10x, more
than the 2 percent dense-scene noise floor. In a profiled run
(`r3-graph-smoke-2`, timing not quoted) host time inside the network calls of
a warm doc-camera-1080 read fell from about 62 ms to 0.3 ms; the rest of the
read is GPU work, which replay does not change.

With `OKUFLOW_UCT_TEST_REGION_RECONCILE=1` in both arms (`r3-graph-reconcile-ab-1`)
the gain is 1.196x warm and 1.113x for first reads: lecture frames 1.20x to
1.38x, the six dense documents 1.10x to 1.15x, identical output on 18 of 18.

### First use: what it costs and when it is repaid

| Cost | Size | Repaid after |
| --- | --- | --- |
| Engine preparation creates 3 detector and 8 recognizer contexts | Startup to first result 3.87 to 3.94 s off, 4.73 to 4.91 s on (`r3-graph-ab-1`); 0.36 to 0.98 s more across all eight cohort pairs | Runs on the preparation thread while the CPU reader serves; at the lecture-frame gain of 5 to 6 ms per read, after about 170 reads |
| A frame size with detector shapes beyond the prepared three: about 80 to 90 ms per new detector context | 2 of 18 scenes: page-portrait +94 to +158 ms; the first 3840x2160 scene in the reverse cohort +83 ms | 5 warm reads; with region reconciliation on, 7 to 8 |
| First capture of a shape | about 1 to 2 ms, once per shape | the next replay |

On the varied page sizes of the title placements and the labelled set, more
first reads pay for a new detector context: 7 of 62 and 8 of 40 first reads
are more than 10 percent slower (the largest is +204 ms on a 1080x1920
page), while the first-read geometric means are 1.101x and 1.061x faster. A
live source keeps one frame size, so after startup it pays this at most once.

One thing in the title run is not explained: the first few reads of a process
with the switch on take about twice as long as with it off even after the
scene's first read (the first three 1280x720 placements of the reverse cohort:
31 to 41 ms against 18 ms; the same placements later in the process take 15 to
16 ms). Four placements therefore show 0.64x to 0.76x warm in that run, which
has one warm pass. Round 10 is to find the cause.

### Output

| Set | Arms | Result |
| --- | --- | --- |
| Scene set v1, 18 scenes, two cohorts, six reads each | off, single, four, eight shapes | identical (`r3-graph-ab-1`) |
| Scene set v1 with region reconciliation in both arms | off, eight shapes | identical 18/18 (`r3-graph-reconcile-ab-1`) |
| 62 title placements (private owner crops), two cohorts | off, eight shapes | identical 62/62 (`r3-graph-titles-1`) |
| Held-out labelled set A, 40 pages, two cohorts | off, eight shapes | identical 40/40 (`r3-graph-labelled-a-1`) |

"Identical" means one complete-output hash (text, order, dispositions,
geometry, confidence, colours) for every read of the scene in every process of
every arm. The opt-in `optionalGraphReplayLifetime` (`r3-graph-lifetime-2`)
read doc-camera-1080 with the switch off, with one graph per engine and with
eight shapes. In each replay mode it cancelled 40 reads from another thread
at delays spread from 0 to 1.2 times a warm read (32 cancelled, 8 completed).
Every completed read, and the read after each cancellation, reproduced the
switch-off output byte for byte; the engines stayed ready, and each State
retired with its exact receipt. Its first version (`r3-graph-lifetime-1`)
cancelled only within the first 10 ms of a read and so never reached the
recognizer; that run is kept.

### Device memory

From the profile's receipts (`r3-graph-smoke-2`; `cudaMemGetInfo` deltas
with 2 MiB granularity, approximate, not exclusive):

| Engine | Arena | Contexts (eight-shape run) | Graphs | Output storage |
| --- | --- | --- | --- | --- |
| Detector | 289.6 MB, replaces the preparation context's own | 5, about 12.6 MB | 5, about 10.5 MB | 3.7 MB (960x960) instead of 2.1 MB for a 1920x1080 frame |
| Recognizer | 169.9 MB, replaces the preparation context's own | 8, about 18.9 MB | 8, about 12.6 MB | unchanged (batch eight) |

About 55 MB more in total in that run, with five detector and eight recognizer
slots in use. With all eight slots of both engines in use the same per-slot
figures give about 70 MB; that bound was not measured. One graph per engine
adds about 2 MB.

### Default

Off. The implementer recommended turning it on after a check on one more GPU
generation. The council (below) agreed on merging it switched off and named
what is missing before it can be on: the configurations and failure paths
listed under "Not established", memory pressure that today retires the reader
where the plain path would carry on, and true first-read measurements. Round
10 takes those conditions. Round 10 below meets all of them except the speed target and leaves
the default off.

### Not established

Measured on one RTX 4090 with the medium BF16 pair only. The small and tiny
tiers, FP32 plans, the bucketed recognizer width (up to 32 shapes with batch
eight, more than the bound) and batch sixteen were not run. Paused-detail
reads use other detector shapes; they were not run with the switch on and
would create or evict detector contexts. No capture refusal or 256 MiB
headroom refusal was observed, so those fallbacks are read and covered by the
model-free test, not exercised on the GPU. No owner camera capture was
available. Creating an execution context on the read path, or failing to
create the replay contexts during preparation, throws and retires the GPU
State where the plain path would have continued. The lifetime test cancels
after three warm reads of one image, so it exercises replays, not captures or
evictions.

### Review

Council, as submitted (`8b64733`): Claude Opus 5.5 7/10, GPT-6.1 Sol 7/10,
Gemini 4 Hydrogen 8/10; all three for merging it switched off. None found a
lifetime or ordering defect or a change with the switch off. Applied on the
integration branch: a failed capture enqueue is a refusal only with a
capture-specific status; low free memory defers instead of refusing; a moved
tensor address drains whenever the slot was used; capture stops with the 32nd
refused shape as documented; a graph handle is cleared before its destroy is
checked; the output receipt used for "identical" now also covers the ribbon
diagnostics (`ExactOutputBytes`; the runs above predate that and cover text,
order, dispositions, geometry, confidence and colours); the wording of the
result, first reads, refusals and memory above. Left for round 10: fallback
instead of retirement under memory pressure, the untested configurations,
fault injection on the GPU, a lifetime test that cancels during captures and
evictions, first reads in fresh processes, and the slow start in the title
run.

On the merged integration build (`int1-ab-v1`, same protocol) the switch
measures 1.178x warm and 1.131x for first reads with identical output on 18 of
18 scenes, and 1.511x warm together with region reconciliation.

## Round 4: Small recognizer first, Medium for doubtful crops (not merged)

OkuRead recognizes with its Small model first and retries doubtful crops on
Medium. Target stated before the work: at least 1.30x warm on every dense
scene, no scene slower, and no new misreading of the critical symbols the
owner's earlier acceptance work guards (`docs/uct_batch8_quality_acceptance.md`).

Built by GPT-6.1 Sol behind `OKUFLOW_UCT_TEST_RECOGNIZER_CASCADE=1` on branch
`perf/okuread-port-r4`, commit `5a9b8c9`. It is not merged. Measured on the
RTX 4090 (evidence `r4-cascade-ab-1`, `r4-cascade-reconcile-ab-1`,
`r4-cascade-heldout-a-1`, `-b-1`, `r4-cascade-profile-1`):

| Measure | Result |
| --- | --- |
| Warm, geometric mean over 18 scenes | 1.083x; 1.123x with region reconciliation on in both arms |
| Dense scenes | 1.14x to 1.56x: target met on some, not all |
| Sparse real frames | up to 21.3 percent slower: the cascade waits for each batch and gives up the look-ahead group |
| Critical symbols | a serial `O0` is read as `00` on both 1920x1080 slide scenes: the guard fails |
| Labelled exact matches, 88 pages | 12,301 to 12,398; with reconciliation 12,784 to 12,861; one group of 48 pages gets worse |
| Device memory | +158 MiB for the second recognizer |
| Startup to first result | +1.1 to +1.3 s |

Council on the branch: Claude Opus 5.5 7/10, GPT-6.1 Sol 7/10, Gemini 4
Hydrogen 7/10, all three for keeping it on its branch. The `O0` misreading is
the same one that kept the Small recognizer from being promoted before, and a
better scheduler would not change it, so the code is not carried in the
reader. The branch and its evidence are kept.


## Round 5: cached-engine startup verification

Target: construction to first GPU result <=2800 ms (median of at least five
fresh warm-file-cache processes), identical complete output on all 18 scenes,
unchanged plan keys/payload digests, warm worker time within noise, no slower
first worker read per scene, and continued refusal of tampered artifacts.

Windows CNG SHA-256 replaces Qt's hashing in the existing source-file checks,
owned source-snapshot check and cached plan check. Every original check runs
in the original order before activation; pins, engine recipes, cache bounds,
ownership and all four detector smoke shapes are unchanged. The exact source
snapshot digest is reused for pin equality and recipe identity, eliminating a
second hash of the same bytes. Streaming verification uses 1 MiB scratch;
no verified-file receipt survives the call or process. Source bytes are still
retained through preparation, as before. This exact change is on by default;
there is no new environment switch. System CNG failures fail closed.

`OKUFLOW_UCT_TRT_PROFILE=1` emits separate `UCT_TRT_PREPARE` JSON wall phases.
No timer or JSON work runs when off. Network and outer preparation intervals
are nested, so do not sum them. Logging overhead is excluded from the next
wall interval. Phase rows identify work completed at checkpoints, not vendor
CPU sampling. Failure retains preceding checkpoints; no success is inferred
from a partial trace. The cached production path does not call DLL provenance
hashing or golden attestation; provenance code remains unchanged. DLL loading
already was runtime-only and its provenance hashing already streamed.

### Startup and phase evidence

RTX 4090, driver 621.14, Ryzen 9 5900X, MSVC 19.44 Release, Qt 6.12.0,
CUDA 12.9.86, TensorRT 11.4.0.106, medium/medium BF16 cached plans, batch eight,
fixed1024. GPU use serialized through the shared lease; no GUI/camera load.
Evidence `r5-startup-ab-five`: five reversed cohorts, five warm reads per
scene/build/cohort, immutable r2 executable versus this checkout. Unprofiled
startup median **3854.97 -> 847.74 ms**, 78.0% less, meeting the 2800 ms target.
Forward/reverse cohorts begin on different scenes, hence their different
initial worker cost. These are fresh processes with warm files, not cold disk.

| Process / corpus order | r2 startup ms | r5 startup ms |
| --- | ---: | ---: |
| 0 / forward | 3898.11 | 862.56 |
| 1 / reverse | 3790.40 | 703.55 |
| 2 / forward | 3854.97 | 866.10 |
| 3 / reverse | 3766.67 | 673.20 |
| 4 / forward | 4131.09 | 847.74 |

Evidence `r5-preparation-profile-before-2` and `r5-preparation-profile-after`:
five fresh processes each on doc-camera-1080. The first before process is the
first instrumented process in this task, after baseline reads and a build; it
is NOT a first-session or cold-file-cache measurement. The owner was already
using the machine and caches were not purged. Processes 1-4 are repeated
warm-file-cache observations. The first after process likewise has warm files.
The harness initializes CUDA/uploads before constructing the recognizer, so
these traces do not measure cold process CUDA-context creation. Profile timing
is attribution only; it is not substituted for unprofiled startup timing.

| Role / wall phase | Before median ms | After median ms |
| --- | ---: | ---: |
| assets: verify-and-dictionary | 1579.86 | 174.29 |
| preparation: cuda-device | 0.01 | 0.01 |
| preparation: state-and-stream | 0.04 | 0.05 |
| preparation: runtime-dll-load | 21.38 | 20.23 |
| detector: source-read-and-verify | 342.79 | 42.85 |
| detector: runtime-and-source-rehash | 338.75 | 6.80 |
| detector: plan-read-and-verify | 190.02 | 23.43 |
| detector: deserialize | 21.04 | 17.65 |
| detector: engine-contract | 1.55 | 1.17 |
| detector: execution-context | 99.40 | 93.54 |
| preparation: detector-prepare | 998.94 | 186.12 |
| preparation: detector-smoke-four-shapes | 64.62 | 67.71 |
| preparation: detector-finish | 0.02 | 0.02 |
| recognizer: source-read-and-verify | 420.84 | 52.90 |
| recognizer: runtime-and-source-rehash | 402.35 | 0.02 |
| recognizer: plan-read-and-verify | 225.73 | 28.73 |
| recognizer: deserialize | 5.99 | 5.41 |
| recognizer: engine-contract | 1.92 | 1.37 |
| recognizer: execution-context | 78.16 | 72.88 |
| preparation: recognizer-prepare | 1166.27 | 172.06 |
| preparation: completion-and-receipts | 4.83 | 4.93 |

Hashing, rather than deserialization, dominated the old path. CNG and digest
reuse were measured together, so their isolated gains are unproven. The
remaining context creation and smoke work is retained.

### Worker and correctness evidence

Complete ordered output is byte-identical on **18/18** scenes, with repeated
outputs deterministic. Every backend detail string in all ten processes is
identical, including both plan keys and payload digests, loaded cache status,
source graph identity and four detector smoke shapes. This is fixture output
identity, not new camera quality acceptance.

Warm worker geometric-mean speedup is **0.99348x** and first-scene worker
geometric-mean speedup **0.99306x**. Warm change is within the ~1% overall noise
floor (dense scenes about 2%, sparse scenes up to 12%). The literal requirement
that no scene's first-read point estimate increases is **not established**:
several do, with a largest relative increase of about 6% on a short real-board
scene. There is no established overall first-worker regression, but neither
an equivalence confidence bound nor a claim that every read is faster.
The table includes medians and each cohort's own warm median; first reads are
the harness's first timed per-scene reads after its startup probe.

| Scene | base warm ms (cohorts) | new warm ms (cohorts) | base first ms | new first ms | speedup warm / first | output | words lost/gained | truth recall |
|---|---|---|---|---|---|---|---|---|
| doc-camera-1080 | 207.19 (205.95, 206.94, 207.99, 205.85, 207.82) | 206.73 (205.73, 208.49, 208.94, 206.31, 206.06) | 207.22 | 207.05 | 1.002 / 1.001 | identical | 0/0 | 1.0000 / 1.0000 |
| doc-camera-tilt-1080 | 158.85 (159.38, 160.16, 157.57, 158.85, 158.54) | 159.90 (157.80, 159.98, 161.06, 159.06, 160.44) | 158.89 | 160.29 | 0.993 / 0.991 | identical | 0/0 | 1.0000 / 1.0000 |
| doc-camera-2160 | 477.88 (479.97, 479.03, 475.85, 477.30, 476.58) | 483.50 (484.52, 479.50, 485.86, 480.31, 483.80) | 475.14 | 484.60 | 0.988 / 0.980 | identical | 0/0 | 0.9916 / 0.9916 |
| doc-camera-turkish-1080 | 181.16 (180.25, 178.37, 181.42, 181.84, 181.67) | 181.84 (186.20, 180.24, 185.46, 179.61, 179.12) | 181.48 | 181.34 | 0.996 / 1.001 | identical | 0/0 | 0.9463 / 0.9463 |
| book-curved-1080 | 75.67 (74.87, 74.03, 76.03, 76.28, 76.06) | 76.74 (78.08, 77.62, 82.12, 75.24, 75.32) | 76.38 | 77.06 | 0.986 / 0.991 | identical | 0/0 | 0.3956 / 0.3956 |
| slide-light-1080 | 70.57 (70.74, 68.26, 70.36, 69.00, 70.78) | 69.67 (70.93, 69.85, 70.28, 68.01, 69.37) | 69.29 | 69.16 | 1.013 / 1.002 | identical | 0/0 | 0.9934 / 0.9934 |
| slide-dark-1080 | 71.51 (73.88, 69.75, 71.51, 71.47, 80.32) | 71.44 (72.35, 70.93, 75.20, 70.35, 70.01) | 70.95 | 71.61 | 1.001 / 0.991 | identical | 0/0 | 0.9934 / 0.9934 |
| slide-light-2160 | 118.95 (123.28, 116.94, 120.89, 116.75, 121.25) | 116.85 (133.74, 116.92, 118.50, 115.00, 115.49) | 119.45 | 117.55 | 1.018 / 1.016 | identical | 0/0 | 0.9737 / 0.9737 |
| table-1080 | 141.56 (144.06, 140.48, 142.35, 139.69, 143.53) | 141.67 (144.28, 140.89, 143.96, 141.31, 141.68) | 141.77 | 144.73 | 0.999 / 0.980 | identical | 0/0 | 1.0000 / 1.0000 |
| page-portrait-1654x2339 | 380.81 (384.49, 378.65, 382.08, 379.55, 382.00) | 381.69 (384.11, 382.62, 381.69, 381.53, 378.07) | 382.65 | 382.39 | 0.998 / 1.001 | identical | 0/0 | 0.9949 / 0.9949 |
| signs-sparse-1080 | 32.32 (34.73, 31.28, 33.52, 31.06, 32.19) | 32.73 (33.58, 32.76, 33.35, 32.23, 30.86) | 33.14 | 33.64 | 0.987 / 0.985 | identical | 0/0 | 1.0000 / 1.0000 |
| no-text-1080 | 46.68 (46.80, 45.23, 47.39, 46.48, 48.34) | 46.92 (47.29, 46.92, 50.98, 46.36, 46.53) | 47.49 | 48.73 | 0.995 / 0.975 | identical | 0/0 | - / - |
| board-lecture-00 | 24.09 (24.07, 23.48, 24.88, 23.74, 25.41) | 24.32 (24.82, 23.75, 25.13, 25.21, 23.33) | 24.95 | 24.24 | 0.990 / 1.029 | identical | 0/0 | - / - |
| board-lecture-01 | 24.20 (24.32, 24.06, 24.45, 23.95, 24.15) | 24.99 (25.09, 24.70, 26.22, 24.54, 23.48) | 23.70 | 24.24 | 0.969 / 0.978 | identical | 0/0 | - / - |
| board-lecture-02 | 26.96 (28.52, 26.22, 28.35, 26.33, 27.77) | 27.26 (27.90, 28.43, 27.15, 26.57, 26.50) | 27.06 | 28.67 | 0.989 / 0.944 | identical | 0/0 | - / - |
| board-lecture-03 | 26.78 (26.99, 25.55, 27.05, 26.08, 27.32) | 27.71 (27.81, 28.58, 27.72, 26.60, 26.60) | 27.26 | 27.51 | 0.967 / 0.991 | identical | 0/0 | - / - |
| board-lecture-04 | 25.48 (27.41, 24.63, 26.85, 24.27, 26.05) | 25.66 (27.01, 25.99, 25.40, 25.71, 24.19) | 24.99 | 24.37 | 0.993 / 1.025 | identical | 0/0 | - / - |
| board-lecture-05 | 23.34 (23.42, 22.37, 23.53, 23.38, 22.81) | 23.36 (23.26, 24.34, 23.73, 24.71, 21.83) | 24.23 | 24.27 | 0.999 / 0.998 | identical | 0/0 | - / - |

Geometric mean new versus base: warm 0.993x, first read 0.993x; identical output on 18/18 scenes.


### Validation and limits

Prepared-toolchain `uct_paddle_tests` and `uct_model_installation_tests` built.
CTest `uct_paddle_processing`, `uct_model_installation_contract` and
`uct_precision_artifact_contract` pass (3/3). The new model-free SHA contract
compares Qt digests for empty/boundary/257 MiB logical inputs, cancelled reads,
closed/error/stalled devices and an on-disk tampered artifact. Existing
manifest, pin and artifact tamper refusals also pass. CNG failure injection,
non-Windows fallback compilation and real app model handoff are unproven.

`scripts/agent_check.ps1` requested FULL due to validation infrastructure edits;
it did not report a SCOPED pass. The delegate guide forbids the complete
matrix, so shipping/CPU/CUDA matrix, shared-source PCH ON/OFF, native desktop,
normal-close and cold-disk acceptance remain for the orchestrator. No GUI was
run. No source model, engine plan, installer or measurement tool was changed.

Failed attempts are retained: `r5-preparation-profile-before` was started before
the initial executable existed; a direct CTest call used a nonexistent default
CMake path, then passed via the prepared Visual Studio CTest. Initial edits
also hit a documentation encoding error before any document write; UTF-8 append
fixed it. No preload, verification deferral, concurrency, context cache or
smoke-shape removal was attempted. No default changed recognized output.

### Scope of the startup figure

"Startup to first result" is measured from constructing the reader to its
first GPU result in the corpus harness: CUDA is already initialized, the file
cache is warm, and the figure includes the first read of the cohort's first
scene (about 24 ms in the reverse cohorts and about 207 ms in the forward
ones, which is why the two orders differ). With three forward and two reverse
processes the median is a forward process. Not measured: a cold file cache,
the first session after boot, the real application's model hand-off, and a
CPU without SHA instructions (the test CPU has them; Intel desktop CPUs before
the 11th generation do not and will gain less from the hash change).
`LoadPaddleAssets` is shared with the CPU Paddle reader, whose startup changes
with it and was not measured.

### Review

Council, as submitted (`09ba2bd`): Claude Opus 5.5 7/10, GPT-6.1 Sol 7/10,
Gemini 4 Hydrogen 8/10. All three confirmed that the reused digest covers the
same owned bytes and that every pin, plan and smoke check still runs in order
before activation, and all three supported having it on by default once the
following were fixed. Applied while merging: an empty digest (a stopped or
failed hash) is never compared; it raises `VerificationInterrupted`, which is
terminal, is not remembered as a changed artifact and cannot fall through to
another plan, an engine build or the FP32 fallback (before, cancelling during a
plan hash could reach the builder libraries); a cached plan needs a 32-byte
digest prefix and a non-empty payload; `sha256::Digest` latches every rejected
operation and gives one result per object, with or without CNG; the header
links `bcrypt` itself, so the test targets that compile the OCR sources
without the shared OCR link settings still link; tests with varied content, a
read error after several chunks and direct failure sequences. Not done: CNG
failure injection, an input above 4 GiB, an end-to-end GPU run with a tampered
plan, and reporting a failed system hash as such where assets are loaded (it
reports as missing or changed files and disables both Paddle readers).

## Round 9: the faster box score as the default, on every GPU

Round 1 missed its target for the textless scene because its exact box score
still walks one large component on one warp. A second exact kernel that shares
a component between warps was briefed and then dropped: the owner decided that
differences at the level of float rounding are acceptable for speed, and that
this holds for every GPU generation. The fixed-tree sum that round 1 kept as a
private comparison is therefore the default now.

Target (round 1's, carried over): "regions" stage of the textless scene under
15 ms, with identical recognized output on every corpus page. Met: 26.4 to
11.1 ms (`r1-kernels-attribution-4`), and no page changed.

What changed: `DetectKernels` defaults to `treeBoxes` on every device, and the
device-specific selection is gone from the reader. Each lane of a warp sums its
pixels in double and a fixed shuffle tree combines the lanes. The kernel spells
out its own membership test and summation order, so its score is the same on
every GPU generation, which the original kernel's was not (its membership test
depends on how the compiler built it for each architecture, see round 1). The
exact raster-order variant remains selectable with
`OKUFLOW_UCT_TEST_DETECT_KERNELS=roots,boxes,ribbons`, and
`DetectKernels::Exact` names where that variant equals the original bit for
bit.

How large the difference is: the original adds the values of a component one
at a time in float, so its own sum drifts with the number of pixels; the
double tree does not. The scores therefore differ by the original's
accumulated rounding, which is far more than one last bit on a large
component: on the kernel test's 60 crafted detector maps 930 of 2,179 compared box scores differ in their bits, the largest by 0.025 percent of the score, and no map's box count changed. A difference
reaches the output only when a component's mean lands that close to the 0.4
acceptance threshold.

Measured (`r9-tree-ab-v1`, build `bcd7082`, exact round 1 default against the
tree sum selected by the switch, two cohorts of five warm reads, RTX 4090):

| Scene | Exact ms | Tree ms | Speedup | First read ms, exact / tree |
| --- | ---: | ---: | ---: | --- |
| no-text-1080 | 47.40 | 31.84 | 1.489 | 48.15 / 33.67 |
| book-curved-1080 | 73.53 | 65.22 | 1.127 | 76.28 / 66.06 |
| every other scene | | | 0.96 to 1.03, inside the noise | |

Geometric mean 1.031x warm and 1.035x for first reads. On the merged
integration build the same comparison in the other direction gives 1.067x for
the default against the exact selection (`int1-ab-v1`).

Output, tree sum against the exact default, same executable: one identical
complete-output hash on 18 of 18 scenes (two cohorts), 20 tuning pages, 88
development pages, 24 fresh generated pages and 62 title placements, 212 in
all (`r9-tree-ab-v1`, `r9-tree-id-*`; two of those runs lost their summary step
to a script update under them and were compared from their raw reports with
`raw_identity.py`). No page changed, so no recognized text changed.

Not established: the kernel ran only on the RTX 4090. It uses no
architecture-specific code, but RTX 20, 30 and 50 series cards were not run.
The corpora contain no owner camera capture.

## Round 10: graph replay made robust; default left off

Round 3's replay stayed behind `OKUFLOW_UCT_TEST_TRT_GRAPH=1` because its
council named what was missing before it could be on by default. This round
closes those points and then measures whether it can be on. Targets, stated
before the work, and the result:

| Target | Result |
| --- | --- |
| Byte-identical complete output against switch-off on scene set v1 (two cohorts of five passes), labelled sets A and B and the 62 title placements, each also with region reconciliation in both arms, and in every configuration that runs replay | Met: 18/18, 40/40, 48/48, 62/62, the same with reconciliation, and all eight configurations |
| Warm geometric mean on v1 at least 1.17x, every lecture frame at least 1.20x | **Not met**: 1.123x; lecture frames 0.975x to 1.260x, three of six below 1.20x |
| Startup to first result with replay at most 100 ms above switch-off (median of five fresh processes) | Met: -39 to +3 ms on six scenes of six frame sizes |
| First read of a frame size not seen before at most 30 ms above switch-off | Met: true first reads in fresh processes -3.3 to +6.0 ms on the same six; in the v1 processes the worst first read of a new frame size is +19.5 ms |
| Every failure path driven on the GPU at least once, outcome asserted | Met: ten paths, `optionalGraphReplayFaults` |
| If all hold, replay on by default | **Not done**: the warm target does not hold, so the default stays off |

Measured on the RTX 4090 (driver 621.14, display not driven), Ryzen 9 5900X,
MSVC 19.44 Release, CUDA 12.9.86, TensorRT 11.4.0.106, medium BF16 detector and
recognizer unless a row says otherwise, branch `perf/okuread-port-r10` at
`af7ff1f` (reader code as of `6d79985`), two reversed cohorts of five warm
reads. Other agents' jobs shared the machine through the GPU lease, and the
desktop ran remote-desktop, game-launcher and GPU-monitoring software.

### What a context costs, measured first

Round 3 made every execution context while preparing: three detector and
eight recognizer contexts at 91 ms and 63 ms each, 0.81 s in all
(`completion-and-receipts` 812.6 ms against 5.0 ms with the switch off,
`r10-titles-slowstart-profile-1`). Three ways of moving that cost were measured
before one was built:

| Measurement | Result |
| --- | --- |
| Every context made before ready on 1, 4 or 14 threads (`r10-diag-parallel-1`, profiled) | startup 1.75 to 1.90 s, 1.99 to 2.12 s and 2.58 to 2.82 s, against 0.70 to 0.85 s off: creation is serialised inside TensorRT or the driver, and more threads make it slower |
| Contexts created and destroyed on another thread while full-replay reads run (`r10-diag-churn-1`) | doc-camera-1080 reads 223 ms, against 188 ms with that thread idle; allocating and freeing 2.5 MB at the same rate 192 ms; a CPU-bound thread 188 ms |
| Every context made on the preparation thread right after the reader is published (commit `f5b2e85`, `r10-smoke-timing-1`) | startup within 18 to 45 ms of switch-off, but for about 1.5 s reads are slower than the plain enqueue (doc-camera 200 to 235 ms against 206 ms off), fast afterwards (188 ms) |

So creating a TensorRT execution context slows inference running at the same
time by about a fifth, from whichever thread, and allocation is not the reason.
Nothing in the TensorRT 11.4 API makes a context cheaper: there is no clone, and
the runtime configuration only selects the allocation strategy. The cost can be
moved, not removed. The two diagnostics were temporary code; their patch is kept
with the evidence.

### What changed

In `src/common/uct_tensorrt.cpp` and `src/common/uct_graph_cache.hpp`:

- **No context at preparation.** With replay requested, each engine's
  preparation context is created on the activation arena that the engine's
  replay contexts share (`CreatePreparationContext`, `kUSER_MANAGED`). It stays
  as the engine's plain **overflow** context. `EnableReplay` only sizes the
  fixed output storage; it creates no context.
- **A shape without a slot runs plainly.** A miss while the cache is not full
  and no context is ready (`Plan(..., canCreate)`) runs on the overflow context
  exactly as the plain path would (`RunPlain`) and evicts or drains nothing. A
  reader that is still waiting for contexts is never slower than the plain
  enqueue for want of one; it is slower only while a context is being created.
- **Contexts on demand, made while no read runs.** Each distinct shape asked
  for (`Policy::Seen`) raises the number of contexts the engine wants, up to
  what it can use (`UsefulContexts`). The preparation thread stays alive after
  publishing the State, sleeps until a context is wanted and makes them one at
  a time, each started only while no read is in flight (`ReplayWork`,
  `ReplayReading` in `RecognizeDevice`, which is the only change there). The
  read path never creates a context. The thread ends when both engines have
  every context they can use, or at the latest one creation after the State
  retires.
- **Allocation failures fall back instead of retiring.** An activation arena
  that cannot be allocated leaves the preparation context owning its memory and
  that engine on the plain enqueue; so does replay output storage that cannot
  be allocated (`DeviceBuffer::TryReserve`). A failed background creation stops
  the production; misses then run on the overflow context. Real device faults
  are still fatal and retire the State as before.
- **Configurations by construction.** With the bucketed recognizer width (up to
  32 shapes at batch eight) the recognizer runs plainly and the detector
  replays. A fixed-canvas recognizer gets at least one slot per batch size,
  sixteen at batch sixteen (`ForConfiguration`). The comparison bound
  `OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES` applies to both engines as given.
- **Test-only fault hooks** (`graph_cache::FaultPlan`, `Checkpoint`), called
  only where `OKUFLOW_UCT_GRAPH_FAULT_HOOKS` is defined, which is the
  `uct_paddle_tests` executable. No environment variable reaches them.
- The corpus harness records `startupWorkerMs`, the worker time of its startup
  read, which is scene zero's true first read in a fresh process (its own
  commit, `7526eb8`).

With the switch off, `Run` takes the original plain path (`RunPlain` is that
code, moved) and no replay code runs.

### Startup and true first reads

One scene per fresh process, five processes per arm, the reader built and read
from nothing each time (`r10-first-<scene>-1`). Medians over the five:

| Scene | Frame size | Startup to first result, off / on ms | Difference | True first read, off / on ms | Difference | Output |
| --- | --- | --- | ---: | --- | ---: | --- |
| board-lecture-01 (real) | 1920x1080 | 669 / 670 | +0 | 34.5 / 33.6 | -0.9 | identical |
| doc-camera-1080 | 1920x1080 | 864 / 867 | +3 | 216.9 / 219.8 | +2.8 | identical |
| slide-light-2160 | 3840x2160 | 762 / 758 | -4 | 121.1 / 124.2 | +3.1 | identical |
| page-portrait-1654x2339 | 1654x2339 | 1073 / 1035 | -39 | 396.2 / 392.9 | -3.3 | identical |
| title-1225x292-1280-1.0-centre (private) | 1280x720 | 718 / 706 | -12 | 24.6 / 30.7 | +6.0 | identical |
| title-1225x292-raw (private) | 1225x292 | 724 / 689 | -36 | 23.1 / 22.8 | -0.4 | identical |

Startup includes the first read. Single first reads vary by several
milliseconds from process to process in both arms (the 1280x720 title: 24.5 to
33.1 ms off, 26.3 to 36.1 ms on), so the differences are within noise; none is
near the 30 ms bound. Round 3, which made its contexts while preparing, added
0.36 to 0.98 s to startup.

### Warm reads

Scene set v1, standard protocol (`r10-v1-ab-1`). First reads are the first
timed read of the scene in each process, after the harness's startup read.

| Scene | Off ms | On ms (cohorts) | Warm speedup | First read ms, off / on (forward, reverse cohort) |
| --- | ---: | ---: | ---: | --- |
| doc-camera-1080 | 207.13 | 190.23 (216.89, 189.44) | 1.089 | 209.6, 205.2 / 230.5, 189.5 |
| doc-camera-tilt-1080 | 160.74 | 147.67 (148.08, 147.63) | 1.089 | 157.7, 161.4 / 145.9, 151.6 |
| doc-camera-2160 | 483.44 | 448.91 (469.81, 448.46) | 1.077 | 484.2, 482.8 / 445.7, 450.0 |
| doc-camera-turkish-1080 | 181.09 | 167.88 (165.34, 168.31) | 1.079 | 181.8, 178.2 / 166.9, 167.9 |
| book-curved-1080 | 63.87 | 56.83 (56.25, 57.75) | 1.124 | 64.5, 68.2 / 57.9, 56.6 |
| slide-light-1080 | 69.16 | 61.23 (60.70, 62.03) | 1.130 | 73.2, 70.6 / 60.5, 61.7 |
| slide-dark-1080 | 72.71 | 61.90 (61.56, 62.72) | 1.175 | 72.6, 73.4 / 61.9, 64.5 |
| slide-light-2160 | 120.24 | 99.88 (98.61, 101.93) | 1.204 | 119.3, 117.2 / 98.8, 109.7 |
| table-1080 | 142.01 | 128.03 (127.59, 130.47) | 1.109 | 141.6, 142.8 / 127.4, 147.8 |
| page-portrait-1654x2339 | 381.70 | 386.85 (362.49, 426.57) | 0.987 | 379.7, 381.8 / 357.6, 401.3 |
| signs-sparse-1080 | 31.61 | 28.21 (26.76, 34.38) | 1.121 | 31.5, 31.3 / 27.0, 27.8 |
| no-text-1080 | 30.33 | 25.93 (25.80, 25.99) | 1.170 | 30.7, 30.9 / 26.2, 26.0 |
| board-lecture-00 (real) | 23.37 | 18.77 (18.83, 18.70) | 1.245 | 22.9, 25.3 / 18.3, 18.6 |
| board-lecture-01 (real) | 22.87 | 18.14 (18.07, 18.24) | 1.260 | 22.3, 21.4 / 17.7, 18.3 |
| board-lecture-02 (real) | 25.07 | 21.56 (21.18, 21.61) | 1.163 | 25.7, 25.8 / 21.5, 22.1 |
| board-lecture-03 (real) | 26.58 | 21.40 (21.00, 26.77) | 1.242 | 26.4, 27.7 / 21.0, 27.4 |
| board-lecture-04 (real) | 25.45 | 24.69 (19.57, 37.69) | 1.031 | 24.5, 25.3 / 19.5, 59.0 |
| board-lecture-05 (real) | 22.81 | 23.41 (18.82, 30.29) | 0.975 | 21.8, 22.7 / 18.5, 35.2 |

Geometric mean 1.123x warm and 1.070x for first reads; identical output on 18
of 18 scenes. With region reconciliation in both arms (`r10-v1-reconcile-ab-1`)
1.149x warm and 1.077x for first reads, identical on 18 of 18 (round 3:
1.196x).

Where the target is missed each cohort's own median shows why. The reverse
cohort opens with the lecture frames, and its first scenes are read while the
reader is still creating the contexts for their shapes: board-lecture-05 and
-04 take 30.3 and 37.7 ms in that cohort and 18.8 and 19.6 ms in the forward
one. The first scenes of a new frame size pay the same (doc-camera-2160 in the
forward cohort, page-portrait in the reverse one). Where a cohort read a scene
with its contexts made, the gains match round 3: lecture frames 1.21x to 1.30x,
dense documents 1.08x to 1.11x, slides 1.12x to 1.22x (each cohort's median
against the pooled switch-off median). board-lecture-02 is the exception at
1.18x and 1.16x in its two cohorts; its switch-off time is lower than in
round 3 (25.1 against 27.2 ms) after rounds 5 and 9. A v1 process creates 13 contexts in all,
5 for the detector and 8 for the recognizer (`r10-v1-profile-1`).

The harness reads back to back, so there is no gap for a context to be made
in. The application paces its reads (`UctTiming`: a refresh interval of 1.5 s,
paced media every 350 ms), so it normally makes them between reads. A v1 run
with a 350 ms gap before every read (`r10-v1-gap350-ab-1`) was meant to show
that and is not usable: reads of both arms slowed by up to five times in bursts
(board-lecture-00 switch-off reads of 108 ms against 23 ms back to back) while
other software on the desktop was active. A shorter run of three scenes with
the same gap (`r10-idle-gap350-1`) measured 1.089x (doc-camera-1080), 1.140x
(slide-light-2160) and 1.267x (board-lecture-01), identical output; it is too
small to stand in for the target. With a 500 ms gap before every read (`r10-gap500-repeat-1`, two scenes, two
cohorts) replay settles within two or three reads: doc-camera-1080 at 187 to
191 ms against 205 ms off, board-lecture-01 at 19.1 to 19.2 ms against 24 ms
(one cohort of each arm was disturbed at the same time by other load).

### Output

| Set | Arms | Result |
| --- | --- | --- |
| v1, 18 scenes, two cohorts, six reads each | off, on | identical 18/18 (`r10-v1-ab-1`) |
| v1 with region reconciliation in both arms | off, on | identical 18/18 (`r10-v1-reconcile-ab-1`) |
| Labelled set A, 40 pages, two cohorts | off, on; and both with reconciliation | identical 40/40 and 40/40 (`r10-labelled-a-1`, `-reconcile-1`) |
| Labelled set B, 48 pages, two cohorts | off, on; and both with reconciliation | identical 48/48 and 48/48 (`r10-labelled-b-1`, `-reconcile-1`) |
| 62 title placements (private owner crops), two cohorts | off, on; and both with reconciliation | identical 62/62 and 62/62 (`r10-titles-1`, `-reconcile-1`) |
| Eight configurations (below) | off, on, on with the profile | identical (`r10-configurations-2`) |

"Identical" is one `ExactOutputBytes` hash (text, order, dispositions,
geometry, confidence, colours and ribbon diagnostics) for every read of a scene
in every process of both arms. Warm geometric means on these sets, for the
record: 1.092x, 1.076x and 1.094x, with reconciliation 1.070x, 1.042x and
1.079x; one warm pass each, so each process spends a large share of its reads
in the creation transient.

Receipts from separate profiled runs, never used for timing: on v1, labelled sets A and B and the titles, each with and without
reconciliation (`r10-v1-profile-1`, `r10-labelled-a-profile-1`,
`r10-labelled-b-profile-1`, `r10-titles-profile-1`), no process evicted or
refused anything after a scene's first read. A process creates 13 contexts on
v1 (5 detector, 8 recognizer), 15 on the labelled sets (7 and 8) and 6 on the
titles (5 and 1); every other call of a new shape ran on the overflow context.

### Configurations

`optionalGraphReplayConfigurations` reads `slide-light-1080` and
`slide-light-2160` (the paused-detail rows: eight detail views of them) in one
process with the switch off, on, and on with the profile, in three rounds, and
requires every read to equal the switch-off read and, except for detail reads,
no eviction in the last round. All eight pass on the measurement build
(`r10-configurations-2`) and again on the final one (`r10-configurations-3`);
the test's first build, before the overflow context, also passed identity in
all eight (`r10-configurations-1`):

| Configuration | Engines selected | Replay | Contexts made (detector, recognizer) |
| --- | --- | --- | --- |
| Medium, batch 8 | BF16 detector, BF16 recognizer | 8 + 8 shapes | 3, 6 |
| Medium, batch 1 (`OKUFLOW_UCT_TRT_BATCH=1`) | BF16, BF16 at batch 1 | 8 + 8 | 3, 1 |
| Medium, batch 16 | BF16 detector, FP32 recognizer at batch 16 | 8 + 16 | 3, 4 of 7 shapes |
| Medium, FP32 plans (pinned FP32 factory) | FP32, FP32 | 8 + 8 | 3, 4 of 6 |
| Small tier | BF16, BF16 | 8 + 8 | 3, 5 of 6 |
| Tiny tier | BF16, BF16 | 8 + 8 | 3, 1 |
| Bucketed recognizer width | BF16 detector, FP32 bucketed recognizer | detector only, recognizer plain by construction | 3, none |
| Paused-detail reads, eight views at 704 and 960 long edges | BF16, BF16 | 8 + 8 | 3, 6 |

No refusal and no capture blocked anywhere; no eviction at all, also in the
detail and two-size reads. "Of n shapes" means the process ended before every
shape had its context (the rest ran on the overflow context).

The plans that did not exist were prepared on the way under the GPU lease: the
small and tiny BF16 pairs, the medium BF16 recognizer at batch one, an FP32
recognizer at batch sixteen (the existing context-memory bound refuses the BF16
recognizer at batch sixteen, so that configuration runs an FP32 recognizer) and
the FP32 bucketed recognizer.

Timing from the repeat on the final build (`r10-configurations-3`, all eight
identical again), indicative only: one process per arm, `Recognize` wall time
including the upload, and only nine reads per image, so most of each run falls
in the stretch where contexts are still being made (the configuration with
"4 of 7" above ended before it had them all). Settled is the median of the
last four reads of each image (`slide-light-1080` / `-2160`); the run before it
(`r10-configurations-2`) was disturbed by other load and is not quoted.

| Configuration | Settled ms, off / on, 1080 | 2160 | Speedup 1080 / 2160 |
| --- | --- | --- | --- |
| Medium BF16, batch 8 | 59.8 / 61.1 | 104.8 / 93.3 | 0.98 / 1.12 |
| Medium BF16, batch 1 | 123.8 / 77.5 | 170.8 / 116.1 | 1.60 / 1.47 |
| Medium, batch 16 (FP32 recognizer) | 101.3 / 115.2 | 144.3 / 157.2 | 0.88 / 0.92 |
| Medium FP32 | 92.4 / 91.6 | 144.7 / 141.3 | 1.01 / 1.02 |
| Small BF16 | 45.2 / 37.0 | 67.7 / 67.6 | 1.22 / 1.00 |
| Tiny BF16 | 65.6 / 25.1 | 84.0 / 38.7 | 2.61 / 2.17 |
| Bucketed width (detector replay only) | 94.2 / 91.7 | 139.5 / 135.0 | 1.03 / 1.03 |

Batch one and the tiny tier, which make many small recognizer calls, gain the
most; batch sixteen was slower in this short run, with three of its seven
recognizer shapes still on the overflow context. None of this is a warm
measurement under the corpus protocol.

### Failure paths

`optionalGraphReplayFaults` (`r10-faults-3`, final build; `slide-light-1080` and
`slide-light-2160`, ten reads over both sizes per case) drives each path on the
GPU and asserts the outcome. In every case no replay call returned or threw
with its stream capturing.

| Path | How it is driven | Outcome asserted |
| --- | --- | --- |
| Capture refused by CUDA | an illegal synchronize inside the capture, which CUDA answers by invalidating it | identical output; one refusal; that shape is not captured again, others are |
| Capture put off for low free memory | the free-memory check reads zero, three times | identical output; three deferrals, no refusal; captures follow |
| Capture enqueue fails with a capture status | the enqueue reported failed after a real invalidation | identical output; one refusal |
| Capture enqueue fails with no CUDA reason | the enqueue reported failed with the capture intact | that read fails closed with its reason; the State retires (two retirements, both complete); the reader prepares again and every later read is identical |
| Eviction with work still queued | a 30 ms host function queued on the stream before each eviction drain (bound 1) | identical output; every drain finished after the queued work |
| Input buffer moved | the input copied to another address on twenty calls on cache slots | identical output; rebinds counted |
| Fatal error between Plan and Bound | TensorRT rejects an out-of-profile shape while a slot is reshaped | the read fails closed; the State retires; later reads identical |
| Background context creation fails | the first recognizer creation reports failure | identical output; recognizer production stops and misses run on the overflow context; the detector keeps its contexts; no retirement |
| Activation arena unavailable | the detector's arena allocation reports failure | identical output; detector plain, recognizer replays; no retirement |
| Replay output storage unavailable | the recognizer's output allocation reports failure | identical output; recognizer plain, detector replays; no retirement |

The moved buffer is injected because no real buffer moves at these sizes: the
detector input is sized for 960x960 by the preparation smoke check and the
recognizer input for a full batch at preparation.

`optionalGraphReplayCheckpoints` (`r10-checkpoints-2`; `doc-camera-1080` and
`slide-light-2160`, region reconciliation on, bound 2 so slots are recycled)
cancels a read at the k-th time, k from 1 to 34, that a replay call is planned,
captured, drained or launched, on both sizes. Cancelled reads: 13 at planned, 2
at captured, 6 at drained, 13 at launched (2,164 calls planned in all). Every
cancelled read returned cancelled with no error and the reader ready; every
completed read, and the read after each cancellation, equalled the switch-off
output; the State retired cleanly (again on the final build, `r10-checkpoints-3`).
`optionalGraphReplayLifetime` passes unchanged (`r10-lifetime-2`,
`r10-lifetime-3`).

### Device memory

Measured as free device memory after the same reads with the switch on minus
off, in one process each (`r10-memory-1`, `optionalGraphReplayConfigurations`,
`cudaMemGetInfo`, 2 MiB granularity):

| Reads | Slots in use (detector, recognizer) | Replay keeps |
| --- | --- | ---: |
| `slide-light-1080` and `-2160` | 3, 6 | 42 MiB |
| the same plus twelve paused-detail views, with 300 ms between reads (`medium-bf16-fill`) | 6, 8 | 50 MiB |

That is the contexts, their graphs and the larger fixed detector output
storage (3.7 MB instead of 2.1 MB for a 1920x1080 frame). The activation arena
adds nothing: it replaces the memory the preparation context would own, and
the overflow context is that preparation context. The marginal slot (a
context and its graph) costs about 1.6 MiB, so all sixteen slots of both
engines in use come to about 53 MiB; that full case was not reached in a run
(the detector used at most 7 of its 8 slots on any set). The receipts' own
per-context figures (`contextBytesObservedApprox`) are taken on the creating
thread while the worker allocates and overstate it.

### Round 3's slow first reads

Round 3's title run (`r3-graph-titles-1`) had the first three 1280x720
placements of the reverse cohort at 31 to 41 ms with the switch on against 18
ms off, and 15 to 16 ms later in the process. Rerun on the round 3 code
(`7b6ed8a`) with the profile on (`r10-titles-slowstart-profile-1`, the same
placements and the 3840x2160 ones, two cohorts) it did not occur: the first
reads were 16 to 18 ms, and the receipts show no capture, drain, eviction or
context creation in them beyond each shape's first use. Rerun without the
profile (`r10-titles-slowstart-timing-1`) a milder form occurred in one of the
two processes: for about 1.3 s after a 1.6 s startup reads ran at switch-off
speed and then sped up within a scene.

The likely candidate, GPU clocks after the long CPU-only preparation, was not
what this machine showed: sampled every 50 ms with `nvidia-smi` around reads
spaced by idle gaps (`r10-clocks-gap2000-1`), the GPU stayed in P2 at 2.5 to
2.65 GHz. What the gap runs did show is that on this machine reads of either
arm slow two- to five-fold in bursts while other desktop software is active,
in whichever process is reading at the time (`r10-clocks-gap2000-1`: the
replay process; its repeat `r10-gap2000-repeat-1`: the switch-off process).
The cause in round 3 is therefore not isolated. With this round's design the
reader's startup is the switch-off startup and the effect is gone from the
same title run (`r10-titles-1`): the first three 1280x720 placements of the
reverse cohort read 17 to 21 ms against 17 to 18 ms off, with two slower
reads (41.7 and 23.3 ms) in the stretch where the process makes its contexts.

### Default

Off. Every condition the round 3 council set is now met except the speed
target under the standard protocol. Moving context creation out of startup
moved its cost to the reads that meet new shapes, and with reads back to back
that costs three of six lecture frames their 1.20x and the geometric mean its
1.17x. Making the contexts before ready would meet the speed target and miss
the startup one by about a second. Startup, first reads and output are
unchanged with it on, so whether the one-time transient is acceptable is left
to the owner and the council.

### Not established

- One GPU, the RTX 4090. Other generations and drivers were not run.
- No owner camera capture; no measurement with the application's own pacing.
- Why creating a context slows concurrent inference is not isolated beyond
  "not allocation, not CPU load"; TensorRT does not document it.
- Warm timing of the configurations other than the default pair under the
  corpus protocol; the batch-sixteen configuration was slower in its short
  run and needs that measurement before it could be on.
- Spaced reads on this machine are disturbed by other desktop software; no
  clean paced run of the whole scene set exists.
