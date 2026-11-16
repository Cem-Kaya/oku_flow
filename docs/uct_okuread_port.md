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
chalkboard lecture frames. Worker time is not camera-to-paint latency. Reads are issued back to back unless a
gap is stated; round 13 found that this matters a great deal for sparse frames
(see "Where the branch stands").

## What applied and what did not

| OkuRead idea | Here |
| --- | --- |
| Path-compressing component roots, four-bin ribbon merge | Round 1, on by default, identical output |
| Cooperative box scoring | Round 1, rewritten: OkuRead's exact variant gave no gain here. Exact, selectable; superseded as the default by the tree sum in round 9 |
| Fixed-tree box scoring | Round 9, on by default on every GPU: it rounds differently, which the owner accepted; no corpus page changed |
| Conditional flat fallback for curved lines (issue #18) | Rounds 12 and 12b: the cause here is false curve refusals on long flat lines; a length-scaled, capped straight-line bound for region-edge refusals behind a private switch, off (placement criteria not met); the fallback itself was not needed |
| Collect detections across tiles before recognition; a complete detection supersedes its covered edge fragment | Round 2, private switch, off: changes output |
| Replay TensorRT enqueue as CUDA graphs, one execution context per cached shape | Round 3, made robust in round 10 and its follow-up (no startup or first-read cost, failures fall back, contexts made in idle gaps), private switch, off: identical output; back to back, reads that overlap a context creation pay for it |
| Small recognizer first, Medium retries for doubtful crops | Round 4, built and measured, not merged: misreads `O0` as `00`, slower on sparse frames |
| Streaming verification, hashing each file once, faster SHA-256 | Round 5, on by default: startup to first GPU result 3.85 s to 0.85 s, identical output |
| Composition flush before capture, capture targets, overlay raster | Not applicable: OkuFlow analyses camera, video and file frames it already owns |
| Speculative ink retries | Not applicable: OkuFlow has no retry fits |
| Reuse-cache crop order defect | Not applicable: OkuFlow has no OCR reuse cache and never reorders crops |
| Keep word-merge state across one read instead of rebuilding it per crop | Round 7, on by default, identical output |
| Run two inferences of one engine at once on two streams (not an OkuRead idea; this program's own) | Round 8, measured first and not built: on the RTX 4090 two streams finish a set of recognizer or detector inferences no sooner, and graph launches show the GPU loses throughput when they overlap |
| Overlap verification and loading of the two engines | Round 11 and its follow-up, on by default: startup to first GPU result 887 to 605 ms (forward) and 674 to 424 ms (reverse), identical output |

## Where the branch stands

Integration branch `perf/okuread-port` at `32a27d1` (rounds 13 and 14 are not
in it; see the end of this section). Nothing was pushed or published.

| Round | What | State | Default | Result (RTX 4090) |
| --- | --- | --- | --- | --- |
| 1 | Detector post-processing kernels | merged | on | 1.126x warm, identical output |
| 2 | One recognition per line across regions | merged | off, `OKUFLOW_UCT_TEST_REGION_RECONCILE=1` | 1.28x to 1.33x warm, dense pages 1.7x to 1.9x; changes text (55 of 62 titles complete instead of 43; +30 exact words over 112 pages) |
| 3 and 10 | Network calls replayed as CUDA graphs | merged | off, `OKUFLOW_UCT_TEST_TRT_GRAPH=1` | 1.11x to 1.18x warm back to back, identical output, no startup or first-read cost |
| 4 | Small recognizer first | not merged (`perf/okuread-port-r4`) | | misreads `O0` as `00`; slower on sparse frames |
| 5 and 11 | Verified startup of the GPU reader | merged | on | startup to first GPU result about 3.85 s to 0.39 s to 0.58 s, identical output |
| 6 | Reconciliation rules that keep the first version's reading gain | merged (tests and record) | | negative: that gain was withheld words shown again |
| 7 | Word merge without per-call rework | merged | on | merge time 89.5 to 14.9 ms on the dense 3840x2160 page, identical output; whole read about 1.02x |
| 8 | Two inferences in flight on two streams | merged (measurement slot and record) | | negative: slower than one after the other |
| 9 | Faster box score, every GPU | merged | on | textless frame 47.4 to 31.8 ms, bent page 73.5 to 65.2 ms; no page of 212 changed |
| 12 | Flat lines no longer refused as curved (issue #18) | merged | off, `OKUFLOW_UCT_TEST_FLAT_RIBBON=straight-refused` | +324, +454 and +117 exact words on the three screen-page groups, +545 on bent pages, no page worse; placement of drawn words not fully within the reviewers' criteria on dense bent pages |
| 13 | Reads at the application's pacing; keep-warm | on its branch (`perf/okuread-port-r13`) | off, `OKUFLOW_UCT_TEST_KEEP_WARM` | paced sparse reads 0.96x to 1.06x of back to back instead of 4.3x to 4.9x, for 45 to 63 W while reading |
| 14 | Warm on view change instead of continuously; paced re-measurement | in progress (`perf/okuread-port-r14`) | off | not finished |

Measured on the merged build with every default as above (`int9-ab-v1`, two
cohorts of five warm reads, back to back), each switch against switch-off:
graph replay 1.168x warm and 1.146x first read, identical output on 18 of 18
scenes; region reconciliation 1.328x and 1.335x; both together 1.475x and
1.469x; the flat-line switch 1.024x with identical output on these scenes.
Against the build from before round 11 the defaults measure 1.010x warm with
identical output and startup to first result 837 to 583 ms and 656 to 391 ms
(`int9-pair-v1`).

What the back-to-back protocol does not show (round 13): the application
reads on timers, and with reads spaced 350 ms or more apart the GPU lowers its
clocks between reads of a sparse frame. A lecture-frame read that takes 24 ms
back to back then takes about 110 ms. Dense pages keep the GPU busy and are
unaffected. So on sparse live frames every warm figure in this record
describes the GPU at full clock, which the application reaches only with the
round 13 switch, with the driver's own power setting changed by the owner, or
if the real display and video path hold the clocks, which was not measurable
here.

Council scores (Claude Opus 5.5, GPT-6.1 Sol, Gemini 4 Hydrogen), as submitted:

| Round | Opus | Sol | Gemini | Outcome |
| --- | --- | --- | --- | --- |
| 1 | 7 | 7 | 8 | fixes applied, merged |
| 2 | 6 | 6 | 6 | rules rewritten, merged off |
| 3 | 7 | 7 | 8 | merged off; conditions became round 10 |
| 4 | 7 | 7 | 7 | kept on its branch, unanimous |
| 5 | 7 | 7 | 8 | fixes applied, merged on |
| 6 | 7 | 8 | 8 | negative result accepted |
| 7 | 8 | 7 | 9 | fixes applied, merged on |
| 8 | 8 | 8 | 9 | corrections applied, merged |
| 9 (and the fixes for 3 and 5) | 7 (7) | 8 (7) | 8 (6) | default kept; one defect in the fixes found and fixed |
| 10 | 7 | 6 | 7 | follow-up fixed the blocker; merged off |
| 11, first pass | 7 | 7 | 8 | follow-up |
| 11, second pass | 8 | 7 | 9 | merged on; one known race recorded |
| 12 | 7 | 6 | 7 | follow-up narrowed the rule; merged off |
| 13 | 8 | 6 | 8 | findings carried into round 14 |

The tracked gate (`scripts/agent_build.bat`) was run on `32a27d1` with desktop
tests excluded (`OKUFLOW_AGENT_EXCLUDE_GUI=1`), which is partial validation by
the project's own rule: release compile passed; the CUDA gate passed (17
passed, 5 skipped for missing codec inputs); in the CPU gate 246 of 248
background tests passed and 2 (`file_document_contract`,
`pdf_export_distribution_contract`) could not run because the pinned qpdf
12.4.2 runtime is not installed on this machine, so the script reports the CPU
gate as failed. Neither test's inputs were changed by this work.

Known and open:

- Round 11: a preparation started immediately after one with the same
  detector was cancelled can run the FP32 detector for the life of that reader
  (8 of 48 test points; see round 11's review). The agreed remedy, a bounded
  wait for an attempt that is in flight, is not built.
- Graph replay (rounds 3 and 10) stays off: back to back it misses its 1.17x
  target at 1.11x to 1.17x, and its paced gain on sparse frames could not be
  measured while the clocks fall.
- Round 12's switch needs the owner's eyes on drawn-word placement before it
  can be a default.
- Rounds 13 and 14 are not merged.

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
rounding, so they are not treated as proof. `DetectKernels::ForDevice` (since round 9: `DetectKernels::Exact`) enables
the exact box kernel only when built with the CUDA 12.9 toolkit
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

### Review

Council on `83e2b30`: Claude Opus 5.5 7/10, GPT-6.1 Sol 8/10, Gemini 4 Hydrogen
8/10; all three accept the negative result and keep the reviewed rules. They
recounted the diagnosis table and the ablation from the evidence and found
both correct, and they supplied what the diagnosis above leaves open:

- **What the first version's gain was.** The scorer counts only confident
  words. Every word read from a refused ribbon is marked invalid geometry with
  a protection box, and word merging passes that mark to whichever overlapping
  word wins. In the first reads of the tuning ablation the three arms have 584,
  333 and 584 invalid-geometry words (switch off, first rules, first rules
  with curved detections excluded) against 7,228, 7,421 and 7,195 confident
  ones. So most of the +206 was about 251 withheld words counted again because
  their protection never reached the merge: the bypass the first council
  objected to, not better reading.
- **Which side was curved.** Of the 201 blocked decisions, 100 drop a straight
  detection whose stand-in is curved-flagged, 47 drop a curved-flagged
  detection in favour of a straight one (the bypass), and 54 are curved on
  both sides. The ablation excluded curved detections in every role, so "the
  exclusion alone removes the gain" holds for that combination, not for each
  role.
- **Fitted and refused ribbons are different cases** and were not separated. A
  refused ribbon keeps the ordinary rectangle as its box, so as a stand-in its
  geometry is as good as a straight detection's; a fitted ribbon's box is only
  an envelope.
- **Flat text is flagged as curved very often.** On the 20 flat tuning pages
  299 of 1,234 detections carry a ribbon flag, and one read has about 880
  fitted and 168 refused ribbon crops. This is a reading question before it is
  a speed question; see round 12.

Applied while merging: the random-layout test also compares `quad.valid` and
every statistics counter under the shuffle, shuffles with its own generator,
and fails unless the layouts actually drop and keep curved detections. Not
done: an independent span check in the test file (the coverage check still
calls the production `Replaces`).

Measured afterwards on the merged build, curved processing on (default)
against off (`OKUFLOW_UCT_CURVED_RIBBON=0`), one read per page, RTX 4090
(`issue18-flat-*`): with curved processing off, exact label matches rise from 6,163
to 6,544 on the 20 tuning pages, from 9,225 to 9,965 and from 3,076 to 3,523 on
the two development groups and from 7,328 to 7,341 on fresh set 1, and fall
from 4,106 to 4,101 on this repository's scenes, which include a really bent
page. With curved processing on those sets have 584, 946, 805, 13 and 36
invalid-geometry words. That is the defect of issue #18, at scale on flat
pages, and it is what round 12 addresses.

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

### Review

Council on `d66814e`: Claude Opus 5.5 8/10, GPT-6.1 Sol 7/10, Gemini 4 Hydrogen
9/10. Opus and Gemini recommended merging it on; Sol wanted one case closed
first. All three checked every order-dependent decision (first overlapping
row, the rejecting overlap, preferred-row updates, the last empty row,
distance ties, the sort shortcut) against the original and found them
reproduced, the frozen bb53 files untouched and the ec7282d copy equal to the
function it freezes.

Applied while merging:

- Sol's case: a reference to one row's word list kept across a call and then
  used to clear or change the row left the outer list's buffer alone, so the
  context kept indices into words that were gone. The context now also keeps a
  shallow copy of every row's word list and compares each row's buffer and
  size on every call; the replay test drives clear, remove, reverse and append
  through such a reference and compares with the frozen merge after every
  call.
- Retained capacity: after 8,192 words have been added since the last rebuild
  the next call rebuilds from scratch, so rows and grid cells cannot keep the
  capacity of an unbounded history of merged-away words.
- The public header described a bit-for-bit comparison of every word that the
  code does not make; it now says what is and is not detected.
- The reader releases the context before its final row sort, which otherwise
  copied the list of rows once per read.
- The replay tool held every loaded corpus to a byte round trip, which a
  format-1 corpus cannot pass after being rewritten as format 2; the check now
  applies to the current format only.

The merged build with these fixes was compared with the integration build
from before the merge (`int6-r7-*`): one identical complete-output hash on 18
scenes (two cohorts of five passes), 88 development pages and 62 title
placements, each with region reconciliation off and on in both arms, 336
comparisons in all; warm 1.023x and 1.010x on the scene set; the replay test
passes.

Not done: moving the per-call scratch vectors into the context, routing the
four-argument overload to the uncached merge, and the oracle cases the
reviewers listed as still missing (mixed rectangle and quad words on a tilted
row, a sort that changes the first word's angle, a check that the context is
actually reused).

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
out its own membership test and summation order, so the sum itself no longer
depends on how the compiler built it for each architecture, as the original's
did (round 1). A component's extents and scan bounds are still ordinary
compiled float arithmetic, so scores are not claimed equal across GPU
generations. The
exact raster-order variant remains selectable with
`OKUFLOW_UCT_TEST_DETECT_KERNELS=roots,boxes,ribbons`, and
`DetectKernels::Exact` names where that variant equals the original bit for
bit.

How large the difference is: the original adds the values of a component one
at a time in float, so its own sum drifts with the number of pixels; the
double tree does not. The scores therefore differ by the original's
accumulated rounding, which is far more than one last bit on a large
component: on the kernel test's 50 crafted detector maps with
values in [0, 1], 16,605 of 58,451 components that both kernels accept have
scores with other bits, the largest difference is 0.025 percent of the score,
and no component is accepted by one kernel only (`int3-gpu.log`). A difference
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

Detector scores do change on real pages: the profile's crop-descriptor hash,
which includes them, differs between the two sums on 5 of 5 profiled scenes
(`r1-kernels-attribution-4`). The score is used only for acceptance and is not
part of the output, so a page changes only if a component is accepted by one
sum and not the other, and that can add or remove a whole line.

Output, tree sum against the exact default, same executable: one identical
complete-output hash on 18 of 18 scenes (two cohorts), 20 tuning pages, 88
development pages, 24 fresh generated pages and 62 title placements, 212 in
all (`r9-tree-ab-v1`, `r9-tree-id-*`; two of those runs lost their summary step
to a script update under them and were compared from their raw reports with
`raw_identity.py`). No page changed, so no recognized text changed.

Not established: the kernel ran only on the RTX 4090. It uses no
architecture-specific code, but RTX 20, 30 and 50 series cards were not run.
The corpora contain no owner camera capture.

### Review

Council on the default (`8686ca3`, `7b6ed8a`): Claude Opus 5.5 7/10, GPT-6.1
Sol 8/10, Gemini 4 Hydrogen 8/10; all three for keeping it as the default.
None found a defect in the tree kernel. Applied: the kernel test now matches
components by root and checks every one (same labels, moments and extents;
scores within 2^-24 per pixel of the component's own box, twice over, instead
of per pixel of the whole map; a component only one kernel accepts must sit on
the threshold; repeatability including labels and components; the same boxes
without ribbon scratch); the exact box kernel is compared only on devices
where `DetectKernels::Exact` selects it, so the test can run on other GPU
generations; the claim that scores are equal across GPU generations is
withdrawn (extents and scan bounds are still compiled float arithmetic); the
record states that detector scores do change on real pages. In the same pass
the reviewers checked the fixes made for rounds 3 and 5 (7/10, 7/10, 6/10) and
found one defect in them, now fixed: a verification interrupted by a
cancellation left its BF16 attempt latched for the session, so the next setup
of the same model would have fallen back to FP32. Also applied for round 3: a
capture put off for lack of memory is not retried for the next 64 network
calls, and running out of memory during a capture puts it off instead of
refusing the shape. Kept as it is: a TensorRT enqueue that fails inside a
capture with no CUDA status is treated as a fault, not a refusal, as the round
3 reviewers asked; round 10 exercises captures on the other model tiers.

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
desktop ran remote-desktop, game-launcher and GPU-monitoring software. Every
timing in this section comes from the `uct_paddle_tests` executable, which
compiles the test-only fault hooks (`OKUFLOW_UCT_GRAPH_FAULT_HOOKS`): with
replay on, each network call also loads the (empty) fault plan a few times.
The application does not compile them. The follow-up below supersedes the
creation policy and some statements of this section; where it does, it says so.

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
  retires. As built in this round neither of the last two statements held
  everywhere, as the council found: a creation could start just before a read
  and run beside it, a failing read let one start on its doomed State, and a
  reader destroyed between "ready" and its first read left the thread asleep
  for good with the State never retired. The follow-up below fixes both and
  states the policy that now holds.
- **Allocation failures fall back instead of retiring.** An activation arena
  that cannot be allocated leaves the preparation context owning its memory and
  that engine on the plain enqueue; so does replay output storage that cannot
  be allocated (`DeviceBuffer::TryReserve`; as built here it released the
  plain buffer before the larger allocation, fixed in the follow-up). A failed
  background creation stops the production; misses below capacity then run on
  the overflow context. Real device faults are still fatal and retire the
  State as before.
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
(one cohort of each arm was disturbed at the same time by other load). The
orchestrator's own paced runs of the whole set on this build show the same
bursts in both arms and are not usable either: `r10c-v1-gap350-ab-1` (warm
1.021x), `r10c-v1-gap350-ab-2` (1.298x) and `r10c-v1-gap1000-ab-1` (0.663x),
identical output in all three. Round 13 later traced the bursts to the GPU
clocking itself down between sparse reads (`gapdiag-1-*`).

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
thread while the worker allocates and overstate it. The 53 MiB for all
sixteen slots is an extrapolation, not a measurement.

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
The cause in round 3 is therefore not isolated, and this round does not close
that condition; round 13's finding that this GPU clocks down between sparse
reads is a candidate it did not test. With this round's design the
reader's startup is the switch-off startup and the effect is gone from the
same title run (`r10-titles-1`): the first three 1280x720 placements of the
reverse cohort read 17 to 21 ms against 17 to 18 ms off, with two slower
reads (41.7 and 23.3 ms) in the stretch where the process makes its contexts.

### Default

Off. Most conditions the round 3 council set are met; the speed target under
the standard protocol is not, the slow start of round 3 is not isolated, the
memory with every slot in use is extrapolated, and no clean paced run exists
(this section originally said every other condition was met; see the
follow-up for what its council found). Moving context creation out of startup
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

### Follow-up after the council review

Council on this round (`c65bb1b`): GPT-6.1 Sol 6/10, Gemini 4 Hydrogen 7/10,
Claude Opus 5.5 7/10; Sol for keeping replay off, the other two for turning it
on once their findings were fixed. The follow-up itself was not sent through a
second council pass; the switch is off, and a targeted review belongs with any
later decision to turn it on. The
follow-up, on the same branch, first merged the integration branch (rounds 5,
6, 7 and 9 and the fixes other councils asked for) and then worked through the
findings. Replay stays off; the orchestrator decides the default after this
follow-up and round 13.

**Merge.** The integration branch's capture changes are kept: a failed
`enqueueV3` inside a capture is a refusal only with a capture-specific CUDA
status; less than 256 MiB of free memory, or running out of memory during the
capture, puts the capture off (both now count as `deferrals` in the receipt);
after a deferral the engine holds captures off. So are the cancellation of an
interrupted verification, round 7's per-read merge context and round 5's
preparation profile and hashing. The hold-off is now counted in reads, not in
network calls: after a deferral every call of that engine that would capture
runs plainly for the rest of the read and the next fifteen reads
(`graph_cache::kCaptureHoldOffReads`). How many calls want a capture per read
depends on the scene; a read count does not, and the low-memory fault test
reads long enough for it (below).

**What was fixed.**

| Finding | Fix | Shown by |
| --- | --- | --- |
| Blocker (all three reviewers): after "ready" and before the first read, the context thread slept on a condition that cancellation never woke, while holding the last reference to the `Preparation`. Destroying the reader in that window left the State unretired for the rest of the session | The thread lets go of the `Preparation` when it publishes the State and keeps only the State. Destroying a reader whose State was never adopted destroys the `Preparation`, which retires that State and so stops and wakes the thread (`ReplayWork::Stop`); `Retire` also stops it directly. A cancelled live `Preparation` is part of the wait's condition | `optionalGraphReplayRetireUnadopted` (`r10b-retire-1`): ready, destroyed without a read, once while the thread waits for a wanted context and once with the thread held between its completion check and its wait; the State retires (one completed receipt, also while the thread is held), the thread ends, and a fresh reader of the same model is ready and reads on `[BF16 / BF16]`. The same test on a build without the fix fails ("the unadopted State was not retired", no receipt; `r10b-retire-negative-control-1`, build not kept) |
| "Only while no read is in flight" was not enforced | A stated policy (`ReplayWork::Turn`, below), a read marker that also covers `Recognize`'s upload, and a marker held past `Retire` on a failing read, so a retiring State stops the thread before its read ends | `graphReplayCreationTurns` (model-free); receipts per read (`creation`) |
| The output-storage fallback freed the plain buffer before the larger allocation | `DeviceBuffer::TryReserve` allocates first and keeps the old buffer when that fails; the bound addresses are cleared before any early return. The fault hook now makes that larger allocation fail | `optionalGraphReplayFaults` (`r10b-faults-1`): the recognizer runs plainly, its plain output buffer survives (`outputBytesKeptAfterFailure`), output identical, no retirement |
| The fault test freed its plan after a fixed 500 ms delay | Fault plans are shared references each hook loads (`FaultPlanForTest`); the tests also wait for every context thread to end (`CreatorThreadsForTest`) | every GPU slot above |
| The missed warm target was not attributed per call | Receipts per read: overflow calls, warm-up, captures, and whether a creation ran at the read's start or end or started during it | below |
| Record and comments | Corrected in place above (marked) and in the code comments; the round 8 note below | — |

**The creation policy.** A context creation starts only (a) in an idle gap: no
read in flight and at least 10 ms since the last read ended, or (b) under
continuous load: once two reads have ended since the last creation, or since a
context became wanted, without such a gap; then it starts at once. A read that
begins while a creation runs proceeds and is slowed. One creation runs at a
time. With the application's pacing (a read submitted every 350 ms) any read
shorter than about 340 ms leaves a gap, so creations land between reads; a read
of 350 ms or more leaves none, and rule (b) bounds the wait at two reads.

Measured on scene set v1 back to back (`r10b-v1-profile-1` receipts mapped by
scene and pass onto the timed processes of `r10b-v1-ab-1`): every process made
the contexts it wanted (detector 5 of 5, recognizer 8 of 8) with no eviction,
refusal or deferral. A creation took 136 to 281 ms beside reads (63 to 91 ms
at preparation with nothing running). The cost lands on the reads it overlaps:

| Read | Reads | Excess over settled replay, median | Range |
| --- | ---: | --- | --- |
| overlaps a creation, settled 50 ms or more | 23 | +26.9 ms (+13 %) | +0.1 to +128 ms (+0 to +36 %) |
| overlaps a creation, settled under 50 ms | 19 | +16.8 ms (+76 %) | +4.3 to +45 ms (+20 to +207 %) |
| only overflow calls (the plain path) | 22 | at switch-off speed | |
| neither | 152 | +0.0 ms | |

**Attribution of the missed target.** In round 10's own run the reverse
cohort's lecture frames were slow over whole scenes (board-lecture-04 59.0,
44.0, 30.1, 29.2, 78.1, 37.7 ms against 19.4 to 20.1 in the forward cohort),
which the "about a fifth" of the churn diagnostic does not explain. The new
receipts do: a read whose shapes still run on the overflow context runs at the
switch-off time (board-lecture-05's 22.9 and 22.5 ms reads, switch-off 22.3),
and of the 13 lecture reads 10 ms or more above settled replay (+10.4 to +45
ms), 12 had a creation running (the flags come from the separate profiled
process, mapped by scene and pass).
On short reads a creation acts as a stall of tens of milliseconds, not as a
proportional slowdown. In reads (reads more than 8 % above settled replay; `r10b_transition.py`):

| Process | Transition | Excess over settled replay | Against switch-off, same reads |
| --- | --- | ---: | ---: |
| forward cohort (dense documents first) | first 25 reads (6.1 s), 11 of them slow | +469 ms | -40 ms |
| reverse cohort (lecture frames first) | until read 97 (10.2 s), 38 slow | +870 ms | -6 ms |
| round 10's design, forward (`r10-v1-ab-1`) | first 20 reads, 8 slow | +329 ms | -107 ms |
| round 10's design, reverse | until read 63, 32 slow | +831 ms | +320 ms |

So a v1 process pays about half a second to 0.9 s over settled replay while it
makes its 13 contexts; with this policy that is spread over more reads, and
over the transition the replay process is no slower in total than the
switch-off process, where round 10's design was 0.3 s behind it.

**Timing after the fixes.** RTX 4090 (driver 621.14), Ryzen 9 5900X, MSVC
19.44 Release, CUDA 12.9.86, TensorRT 11.4.0.106, medium BF16 pair; build of
`253c676` (later commits move the creation policy into the cache header, skip
the read marker with replay off and add tests and documentation), the
`uct_paddle_tests` executable with the fault hooks compiled in. Arms
`OKUFLOW_UCT_TEST_TRT_GRAPH=0` and `=1`. `nvidia-smi` sampled P-state, clocks,
power and utilisation every 100 ms (read-only query) in every run.

Back to back, the standard two reversed cohorts of five (`r10b-v1-ab-1`) and a
second run of six cohorts alternating direction and arm order, three each way
(`r10b-v1-alt-1`). Warm median of all warm reads per arm, the range of the
replay arm's cohort medians, and warm and first-read speedups:

| Scene | two cohorts: off / on warm ms (on cohorts) | warm / first | six cohorts: off / on warm ms (on cohorts) | warm / first |
|---|---|---|---|---|
| doc-camera-1080 | 205.0 / 192.4 (188.4..211.7) | 1.065 / 1.045 | 203.9 / 193.0 (188.1..233.2) | 1.056 / 1.024 |
| doc-camera-tilt-1080 | 156.9 / 145.8 (144.1..154.8) | 1.076 / 1.035 | 156.7 / 147.7 (144.1..164.4) | 1.061 / 1.066 |
| doc-camera-2160 | 464.4 / 431.9 (431.0..459.2) | 1.075 / 1.019 | 464.2 / 435.3 (432.1..462.0) | 1.066 / 1.049 |
| doc-camera-turkish-1080 | 177.7 / 164.0 (163.8..164.2) | 1.084 / 0.956 | 178.1 / 164.5 (164.0..166.0) | 1.083 / 1.086 |
| book-curved-1080 | 63.3 / 55.1 (55.0..55.1) | 1.149 / 1.064 | 63.0 / 55.6 (55.0..57.6) | 1.134 / 1.141 |
| slide-light-1080 | 68.9 / 59.9 (59.7..60.4) | 1.150 / 1.110 | 67.4 / 60.1 (60.0..63.2) | 1.122 / 1.188 |
| slide-dark-1080 | 69.8 / 60.9 (60.7..60.9) | 1.146 / 1.090 | 69.8 / 61.1 (60.4..63.7) | 1.142 / 1.113 |
| slide-light-2160 | 113.0 / 98.3 (98.2..102.8) | 1.149 / 1.120 | 113.6 / 98.4 (97.8..107.4) | 1.154 / 1.097 |
| table-1080 | 140.3 / 127.8 (127.0..142.9) | 1.098 / 1.033 | 140.5 / 127.9 (126.9..150.9) | 1.098 / 1.100 |
| page-portrait-1654x2339 | 371.6 / 377.5 (356.1..420.2) | 0.984 / 0.997 | 373.2 / 367.5 (352.5..400.3) | 1.016 / 1.021 |
| signs-sparse-1080 | 32.0 / 27.7 (27.3..36.3) | 1.155 / 1.126 | 31.0 / 28.2 (26.8..33.2) | 1.099 / 1.067 |
| no-text-1080 | 30.9 / 26.3 (26.1..26.5) | 1.176 / 1.195 | 30.4 / 26.1 (25.7..26.3) | 1.166 / 1.189 |
| board-lecture-00 | 23.7 / 18.8 (18.7..18.8) | 1.265 / 1.240 | 22.7 / 18.6 (18.6..18.8) | 1.223 / 1.250 |
| board-lecture-01 | 23.7 / 18.5 (18.3..18.5) | 1.284 / 1.276 | 23.1 / 18.2 (17.9..20.5) | 1.266 / 1.251 |
| board-lecture-02 | 25.4 / 21.8 (21.8..22.0) | 1.166 / 0.801 | 26.0 / 21.5 (21.3..29.2) | 1.210 / 1.049 |
| board-lecture-03 | 26.1 / 23.7 (21.4..34.0) | 1.104 / 1.126 | 27.2 / 22.0 (20.9..28.8) | 1.237 / 1.094 |
| board-lecture-04 | 25.3 / 23.0 (20.0..36.5) | 1.100 / 0.839 | 24.9 / 21.5 (19.4..51.2) | 1.157 / 0.910 |
| board-lecture-05 | 22.3 / 20.8 (18.9..33.2) | 1.067 / 1.090 | 22.8 / 20.3 (18.4..39.0) | 1.123 / 1.119 |
| geometric mean | | 1.125 / 1.058 | | 1.132 / 1.098 |

Identical output on 18 of 18 scenes in both runs. Every process of both arms
ran in P2 at a median 2.57 to 2.64 GHz with no sample below 1.5 GHz; the last
switch-off process of the six-cohort run was slower on every scene at lower
utilisation (other load), not at lower clocks. The warm target (1.17x, each
lecture frame 1.20x) is still not met back to back: 1.125x and 1.132x, and the
lecture frames that open the reverse cohorts carry the creation transient
(board-lecture-04's replay cohort medians 19.4 to 51.2 ms). Where a cohort
read a lecture frame with its contexts made, it ran at 17.9 to 21.4 ms against
22.3 to 27.6 ms off.

Paced like the application's advancing media, 350 ms before every read, six
alternating cohorts (`r10b-v1-gap350-1`):

| Scene | paced: off / on warm ms (on cohorts) | warm / first |
|---|---|---|
| doc-camera-1080 | 202.7 / 185.6 (185.3..185.7) | 1.092 / 1.002 |
| doc-camera-tilt-1080 | 155.8 / 142.5 (142.1..142.8) | 1.093 / 1.089 |
| doc-camera-2160 | 461.8 / 425.6 (423.9..426.5) | 1.085 / 1.079 |
| doc-camera-turkish-1080 | 175.8 / 162.6 (161.9..163.5) | 1.081 / 1.037 |
| book-curved-1080 | 79.6 / 77.2 (55.4..79.6) | 1.031 / 1.100 |
| slide-light-1080 | 82.4 / 81.3 (80.2..81.7) | 1.014 / 1.200 |
| slide-dark-1080 | 84.3 / 80.9 (60.2..81.9) | 1.042 / 1.092 |
| slide-light-2160 | 112.0 / 97.9 (97.5..98.5) | 1.143 / 1.076 |
| table-1080 | 137.9 / 125.9 (125.8..126.6) | 1.095 / 1.097 |
| page-portrait-1654x2339 | 368.2 / 344.2 (342.0..344.7) | 1.070 / 1.209 |
| signs-sparse-1080 | 59.3 / 61.1 (26.8..137.4) | 0.972 / 0.406 |
| no-text-1080 | 70.7 / 67.2 (66.9..124.5) | 1.052 / 1.057 |
| board-lecture-00 | 108.5 / 53.0 (43.1..109.9) | 2.047 / 1.067 |
| board-lecture-01 | 53.4 / 51.7 (19.1..107.9) | 1.033 / 1.018 |
| board-lecture-02 | 67.3 / 63.7 (22.3..112.4) | 1.055 / 0.709 |
| board-lecture-03 | 113.7 / 110.5 (57.3..120.0) | 1.029 / 1.517 |
| board-lecture-04 | 68.6 / 31.2 (20.7..114.4) | 2.200 / 1.019 |
| board-lecture-05 | 23.1 / 19.6 (19.5..111.1) | 1.177 / 1.571 |
| geometric mean | | 1.150 / 1.040 |

Identical output on 18 of 18. The clock state was the same in both arms:
P2 for 70 and 71 percent of the samples, P3 12 and 12, P5 16 and 14, P8 2 and
2; 29 and 28 percent of the samples below 1.5 GHz (10th percentile 660 and 705
MHz). That is round 13's finding: between the reads of a sparse frame the GPU
clocks itself down, and a short read then runs at a fraction of its speed. So
the sparse scenes (the lecture frames, signs, no-text, the 1080 slides, the
curved book) swing between about 19 and 120 ms within an arm and support no
paced gain or loss. The dense scenes, whose reads keep the clocks up, are
consistent within 2 percent across cohorts and agree with back to back:
doc-camera 1.08x to 1.09x, table 1.10x, slide-light-2160 1.14x, page-portrait
1.07x. The geometric mean, 1.150x, is not a claim. In the paced profiled pass
(`r10b-v1-gap350-profile-1`) all 13 creations of each process landed in idle
gaps (59 to 90 ms each, as at preparation), no creation started under load,
and over each whole paced process the replay arm took 0.25 to 1.5 s less than
the switch-off arm of the same cohort.

Receipts from separate profiled passes, never used for timing
(`r10b-v1-profile-1`, back to back, with and without region reconciliation,
and `r10b-v1-gap350-profile-1`): in every process contexts created equal
contexts wanted (detector 5 of 5, recognizer 8 of 8), no eviction, no refusal,
no deferral.

**Tests on the final build** (`c8cbb5a`): `optionalGraphReplayFaults`
(`r10b-faults-2`, all ten paths; the low-memory case reads 78 times, its three
deferrals pass and captures follow the last one), `optionalGraphReplayRetireUnadopted`
(`r10b-retire-2`), `optionalGraphReplayCheckpoints` (`r10b-checkpoints-2`:
cancelled 13, 2, 6 and 13 reads at planned, captured, drained and launched),
`optionalGraphReplayLifetime` (`r10b-lifetime-2`) and all nine
`optionalGraphReplayConfigurations` (`r10b-configurations-2`) pass, every one
with identical output against the switch-off read; the model-free
`graphReplayCreationTurns` passed 20 runs of 20. Contexts made by the end of
each configuration's process (about 26 reads back to back), detector and
recognizer, against those wanted:

| Configuration | Contexts made / wanted | Evictions, refusals | Replay keeps (switch-on minus off, free memory) |
| --- | --- | --- | ---: |
| Medium BF16, batch 8 | 3/3, 6/6 | 0, 0 | 42 MiB |
| Medium BF16, batch 1 | 3/3, 1/1 | 0, 0 | 16 MiB |
| Medium, batch 16 (FP32 recognizer) | 3/3, 4/7 | 0, 0 | 24 MiB |
| Medium FP32 | 3/3, 4/6 | 0, 0 | 14 MiB |
| Small BF16 | 3/3, 6/6 | 0, 0 | 30 MiB |
| Tiny BF16 | 3/3, 1/1 | 0, 0 | 12 MiB |
| Bucketed width (recognizer plain) | 3/3, - | 0, 0 | 12 MiB |
| Paused-detail reads | 3/3, 6/6 | 0, 0 | 42 MiB |
| `medium-bf16-fill` (both images and twelve detail views, 300 ms gaps) | 6/6, 8/8 | 0, 0 | 66 MiB |

In round 10 the same short runs ended with the small tier at 3 of 6 recognizer
contexts; under continuous load the policy now makes one context per two
reads, so a process needs about twice as many reads as it wants contexts, and
the two configurations with the most (batch 16, FP32, whose creations are also
the slowest) end these runs short of them, their remaining shapes on the
overflow context. `medium-bf16-fill` was run: 14 slots in use (6 of the
detector's 8, all 8 of the recognizer's), 66 MiB, where round 10's run of the
same configuration measured 50 MiB for the same slot count. Free device memory
on this shared desktop varies by that much between runs, so the figure is an
upper reading, not a precise one; all sixteen slots were not reached and the
53 MiB extrapolation of round 10 is withdrawn in favour of "between 50 and 66
MiB measured with 14 slots".

**Which conditions are closed.**

| Condition | State |
| --- | --- |
| Council blocker: unadopted State never retired | Closed: fixed, GPU test, negative control |
| Creation "only while no read runs" not enforced | Closed: stated policy, enforced, tested without a model, measured cost per overlapped read |
| Creator starved under continuous load | Closed with a bound: one creation per two reads; short processes with many contexts wanted still end before all exist |
| Output-storage fallback freed the plain buffer | Closed: fixed, the fault reaches the allocation |
| Fault test freed its plan after a delay | Closed |
| Attribution of the missed warm target | Closed: per-read receipts; the excess sits on reads that overlap a creation |
| Hold-off and the low-memory test | Closed: hold-off in reads, test reads long enough and asserts recovery |
| Warm target back to back (1.17x, lecture frames 1.20x) | **Not met**: 1.125x and 1.132x |
| Paced gain | **Not established**: the GPU clocks down between sparse reads in both arms (round 13); dense scenes 1.07x to 1.14x |
| Round 3's slow start | **Not isolated** |
| Memory with every slot in use | **Not reached** (14 of 16 slots, 50 to 66 MiB) |

Default: off, as the brief requires; the orchestrator decides after this and
round 13.

**Round 8 note.** The contexts of one engine share one activation arena,
including its plain overflow context. That is safe only while every context of
the engine runs in order on the State's one stream. Round 8, which runs
recognition groups concurrently, must give each context that can run beside
another its own arena (or keep both on one stream) before it is combined with
replay.

## Round 12: readable text withheld on flat pages (issue #18; private switch, off)

Targets stated before the work: on the 20 tuning pages at least +300 exact
label matches with curved processing on (no page worse by more than 3); on
every confirmation set no fewer exact matches than today; on the bent book
pages also no lower recall and the same or more ribbon-read words; no title
placement that is complete today becoming incomplete; every protection box of a
line that is still refused still present; no `v1` scene slower beyond the noise
floor, and a stated cost for any added recognition.

Result: met with `OKUFLOW_UCT_TEST_FLAT_RIBBON=straight-refused`, which stays
off by default. Tuning pages +386 with no page worse; development pages +566
and +172; fresh pages 0, +8, +13 and +17; this repository's scenes unchanged;
nine new bent book pages +555; titles 43 of 62 in every arm; no protection box
of a still-refused line lost on any page; speed unchanged. The flat fallback
the issue proposes was not built: in OkuFlow it would re-read the crop a
refused line is already read from (below). The speed target's wish that flat
pages get faster is not met: neither mode removes measurable work.

### Where the words were lost

Curved processing on against off showed the defect at scale on flat labelled
pages (`issue18-flat-*`, round 6 review). Counted on the tuning pages with
temporary instrumentation (`r12-diag-tune`, one read, RTX 4090):

| Detection fit outcome | Detections | Words decoded (detection crops) |
| --- | ---: | ---: |
| Straight (ordinary rectangle) | 859 | 2,877 |
| Fitted ribbon | 230 | 851, 66 over the chord limit |
| Refused: ribbon outside its source region | 60 | 276, all withheld |
| Refused: residual 8, height 1 | 9 | 12, all withheld |
| Fit not tried (short, gaps) | 76 | 82 |

Every flagged line on these pages is a false curve: they are flat screen
pages. The 584 withheld (invalid-geometry) words in the final output are 268
straight words that inherited a refused line's protection in the merge, 315
ribbon words (a refused line's own words, or fitted words that inherited) and 1
fitted word over its chord limit. Attribution arms on the same build
(`r12-tune-k`): leaving refused lines unmarked recovers +435 exact matches, no
chord limit +45, both +483. So the loss is refusal protection, mostly reached
through the region-edge check, not bad fits.

Why straight lines are flagged: the straight-line test is a fixed sagitta,
max(0.75 px, 8% of the mean envelope height) in detector pixels, whatever the
line's length. A long line has a large sagitta for a tiny curvature. The falsely
flagged flat lines have a median fitted bow of 0.15 of their height over a
median of 91 heights; the real curves of `book-curved-1080`, which the detector
cuts into short pieces, have the same bow (median 0.13) over 20 heights, a
curvature about 13 times larger. A significance test of the quadratic term did
not separate them (median z about 5.6 for both); length does.

### What changed

`ribbon::StraightBound` scales the established bound by
max(1, length / (K x mean height)) with K = 20. Lines up to K heights are judged
exactly as today. For a longer parabolic line under the scaled bound, every
piece of K heights departs from its own chord by less than the established
bound (a sub-span's sagitta falls with its squared length), and the end angle
between curve and chord (4 x sagitta / length) stays within what the
established bound accepts on a line K heights long. Words and the recognition
windows of a long line are therefore at most as curved as lines today's rule
already sends to the rectangle path.

`ribbon::StraightRule` applies it two ways, each its own switch value:

- `straight-refused`: the scaled bound replaces only a refusal. A valid ribbon
  fit stays a ribbon, and a line that is still refused keeps exactly today's
  geometry and protection box. A moved line was already read from its
  rectangle crop (refused ribbons always were), so recognition work does not
  change; it loses the ribbon flag and protection box.
- `straight`: the scaled bound applies before any ribbon check, so long flat
  lines with a valid fit become rectangles too.

`ribbon::Fit` now records its outcome (straight, fitted, the first failed
check, or moved by a rule) and the fitted bow in former padding bytes; the
layout keeps its 144 bytes. With `OKUFLOW_UCT_TRT_PROFILE=1` the profile line
gains `ribbonFit`: crops and decoded words per outcome for detections and
their sub-windows, a bow histogram per outcome class, and (bounded) the
protection box of each refused detection and the box of each moved one.
`PaddleDetect` mirrors the rule for the CPU path.

K was chosen on the tuning pages and fresh set 1 only (`r12-tune-k`,
`r12-fresh1-k`, `r12-sr-tune`, `r12-sr-fresh1`):

| K | `straight-refused` tuning | `straight` tuning (worst page) | Fresh set 1 (both) |
| ---: | ---: | --- | ---: |
| 10 | +429 | +369 (-15) | +13 |
| 15 | +407 | +347 (-15) | +13 |
| 20 | +386 | +328 (-15) | +13 |
| 25 | +363 | +305 (-15) | +13 |
| 30 | +350 | +292 (-15) | +13 |

`straight-refused` makes no tuning page worse at any K. Smaller K reads more,
but moves lines whose curvature is larger: at K = 10 the end angle may reach
twice what today's rule accepts on a 20-height line. K = 20 keeps the
statements above at the scale of a recognition window and of any word, and
costs 43 tuning words against K = 10. `straight` loses up to 15 words on three
dense portrait pages with mixed type sizes in one line: there the valid
ribbon reads better because its crop height follows the size change.

The diagnosis also ran on `v1` (which holds the bent page) before the
confirmation, so `v1` is not untouched in this round.

### Not built: the flat fallback and a narrower inheritance

The issue's fallback keeps the rectangular candidate of a curve-related
detection and re-reads it when the curved read lacks confident coverage. In
OkuFlow a refused ribbon is already recognized from its original rectangle;
only its words are withheld. A re-read would decode the same crop, and its
words would meet the same protection in the merge. For fitted lines the most a
rectangle re-read could add, page by page (the better of `straight` and
`straight-refused`), is 7 words on the tuning pages. After `straight-refused`,
122 words remain withheld there: 27 refused detections (24 at a region edge, 2
residual, 1 height) and 43 straight words that inherited their protection.
Neither the inheritance share nor the fallback's ceiling justified changing
the owner's protection design, so neither was built. Making such words
readable but not drawable would be a reading-path change (every non-candidate
word is omitted from reading), not a ribbon change.

### Confirmation (final binary)

All rows: build `3379ffa` (`uct_paddle_tests.exe` SHA-256
`f8f67268de7cfd71d0df702172a8dafb207a761c18ab600ab6bd957501a7eabb`), RTX 4090,
driver 621.14, Ryzen 9 5900X, MSVC 19.44 Release, Qt 6.12.0, CUDA 12.9.86,
TensorRT 11.4.0.106, medium/medium BF16, one first read per page, profiled
(`r12-final2-q-*`). Fresh sets 5 and 6 and bent set 1
(`perf_corpus.py --bent 1`, new in this round) were generated after the final
source was committed. Exact = case-sensitive label matches; invalid = withheld
words; boxes = distinct protection boxes in the output.

| Set | Labels | Exact today | `straight-refused` | `straight` | Worse pages (refused / straight) | Invalid words today / refused | Protection boxes today / refused |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| Tuning, 20 | 8,154 | 6,163 | 6,549 (+386) | 6,491 (+328) | 0 / 3 (worst -15) | 584 / 122 | 399 / 95 |
| Fresh 1, 12 | 7,381 | 7,328 | 7,341 (+13) | 7,341 (+13) | 0 / 0 | 13 / 0 | 10 / 0 |
| Development A, 40 | 12,180 | 9,225 | 9,791 (+566) | 9,809 (+584) | 0 / 2 (worst -11) | 946 / 267 | 580 / 223 |
| Development B, 48 | 5,323 | 3,076 | 3,248 (+172) | 3,222 (+146) | 0 / 5 (worst -4) | 805 / 541 | 586 / 439 |
| `v1`, 12 labelled | 4,394 | 4,106 | 4,106 (0) | 4,106 (0) | 0 / 0 | 36 / 36 | 13 / 13 |
| Fresh 2, 12 | 6,610 | 6,577 | 6,577 (0) | 6,577 (0) | 0 / 0 | 0 / 0 | 0 / 0 |
| Fresh 5, 12 | 6,967 | 6,912 | 6,920 (+8) | 6,923 (+11) | 0 / 0 | 10 / 2 | 9 / 2 |
| Fresh 6, 12 | 6,016 | 5,954 | 5,971 (+17) | 5,971 (+17) | 0 / 0 | 20 / 0 | 15 / 0 |
| Bent book pages, 9 | 5,976 | 3,648 | 4,203 (+555) | 4,189 (+541) | 0 / 2 (worst -7) | 1,863 / 1,293 | 783 / 445 |

- `book-curved-1080`: 163 exact matches, recall 0.396, in all three arms.
  Ribbon-read candidate words: `v1` 114 today and with `straight-refused`, 74
  with `straight`; bent set 1,173 today, 1,215 with `straight-refused`, 439
  with `straight`. Bent-set recall 0.610 today, 0.703 with `straight-refused`.
  The bent set gains most on its milder pages (one goes from 150 to 379 of 394
  words), where today's rule refuses long, slightly bent lines at region edges.
- Titles (62 private placements, counts only): complete in 43 in every arm;
  none changes.
- Protection, per page (`r12_protection.py`): every refused detection's box of
  today's read is, with the switch, either a refused detection's box again or
  the box of a line the rule moved; none is missing on any set. Moved/kept
  refused detections: tuning 42/27, development A 63/51, development B 36/139,
  `v1` 0/14, fresh 1 4/4, fresh 2 1/0, fresh 5 3/0, fresh 6 4/0, bent 73/233,
  titles 0/9.
- Work: `straight-refused` changes no crop. On the tuning pages the read keeps
  880 ribbon crops and 3,811 recognizer samples; refused crops fall from 168 to
  27. `straight` cuts ribbon crops to 32 and samples to 3,736.

Speed (`r12-final2-speed-v1`: two reversed cohorts of five warm reads;
`r12-final2-speed-tune`: two cohorts of two), same build and machine:

| Arm against today | `v1` warm | `v1` first read | Tuning warm | Tuning first read |
| --- | ---: | ---: | ---: | ---: |
| `straight-refused` | 0.999x | 0.993x | 1.000x | 1.002x |
| `straight` | 0.955x | 0.959x | 1.008x | 1.001x |

`straight-refused` gives byte-identical output on 18 of 18 `v1` scenes, so its
`v1` differences are noise: the largest dense-scene change is doc-camera-1080
at 1.8 percent (inside the 2 percent floor); sparse lecture scenes move by up
to 6 percent (inside 12). `straight` is slower on several sparse scenes in one
cohort only; that run was not repeated. Neither mode makes flat pages
measurably faster: `straight-refused` removes no ribbon work, and the ribbon
crops that `straight` removes cost no more to rectify than rectangles.

The first confirmation attempt measured the previous diagnostic build: the
build of `3379ffa` had stopped on a misnamed target. Its quality results
(`r12-final-q-*`) are identical to the rerun; its speed runs
(`r12-final-speed-*`) were heavily contended and are not used.

Tests: `curvedRibbonStraightSpanBound` (model-free: the bound and its two
geometric statements, the outcome of each refusal, and over a sweep of
lengths, bows, slopes and source sizes that a rule only ever moves a component
to the rectangle path, never changes a fitted or refused geometry or its
protection, and under `straight-refused` never changes a valid fit);
`gpuRibbonStraightRuleMatchesCpu` (GPU and CPU agree on the outcome for each
rule). Existing ribbon, reconcile and detector-kernel slots and the exact merge
replay (`uct_merge_replay_tests`, frozen baseline unchanged) pass. The merge
itself is unchanged; with the switch on it decides differently only because
moved lines' words arrive as ordinary candidates without a protection box.
SCOPED: no full matrix, GUI or native-app run.

### Not established

No pixel-level check that words drawn on a moved line sit on their ink: the
argument is geometric (the bound above, plus the per-word ink fit every
rectangle word already gets), and the moved lines on the bent set are real,
mildly curved lines. No owner camera or document capture is in any set. The
issue's OkuRead title failure does not reproduce in OkuFlow (0 of 62
placements change with curved processing on or off).

### Default

Off. Recommendation: make `straight-refused` the default. It meets every
quality and protection target on every set, makes no page worse anywhere,
keeps every valid ribbon and every protection box of a line that is still
refused, and costs no time. `straight` should stay off: it loses words on
mixed-size lines and ribbon-read words on bent pages.

### Round 12b: the rule narrowed to what its argument covers, and where moved words are drawn

The round 12 council (GPT-6.1 Sol 6/10, Claude Opus 5.5 7/10, Gemini 4
Hydrogen 7/10; Sol for keeping it off, the other two for turning it on after a
placement check) accepted the reading gains and the
argument against the flat re-read, and found that the rule went further than
its argument: `straight-refused` turned every refusal into a rectangle when the
fitted bow was under the scaled bound, including steep and residual refusals,
where the parabola says nothing about the line's real shape; the scaled bound
had no ceiling and its 0.75 px floor grew with length, loosening it for small
type; and nothing measured where the newly shown words are drawn. The owner's
condition is that replacement text is never drawn over ink that could not be
rectified. Changes (switch still off):

- **Only region-edge refusals move.** `straight-refused` now moves only a line
  refused because its padded ribbon leaves its source region (`kOutside`), the
  one refusal after every shape check. Steep and residual refusals stay
  refused; so do height refusals (the envelope tapers by more than half its
  height, so one rectangle height cannot fit its words, the failure measured
  below) and degenerate ones (the height regression failed; unreachable with
  at least 20 distinct bins, and the rule moves only lines whose fit completed
  every check). The review's photographed line (flat, dropping half a text
  height near the gutter: least-squares bow under the bound, residual over its
  limit) is a test case and stays refused.
- **A cap and a floor that does not scale.** No line whose fitted sagitta
  exceeds 0.32 mean heights is treated as straight, whatever its length
  (`ribbon::kStraightMaximumBow`, an edge of the existing bow histogram). Only
  the 8% part of the bound scales with length; the 0.75 px floor is a fixed
  noise allowance. At a detector height of 5 px and 100 heights the bound was
  0.75 h, now 0.4 h; above 80 heights the cap binds.
- **Words drawn only on their ink.** A word of a moved line that the worker's
  ink fitting cannot place (`RefineWordInkBounds` / `FitOrientedWordInk`
  keep the detector span) is withheld as invalid geometry over that span, as
  when its line was refused (`FinalizeRecognizedGeometry`). Words of other
  lines keep today's disposition, fitted or not.
- **Both readers.** `PaddleFlatRibbonRule` parses the switch for the TensorRT
  and the CPU reader; the CPU reader ignored it before.

Each step was committed with a test: `curvedRibbonStraightSpanBound` (floor,
cap, the residual and height cases kept refused, only `kOutside` moves),
`curvedRibbonFlatSwitchBothReaders` (one parser; both readers refuse an
invalid value with the same message; `PaddleDetect` moves a region-edge line
and its words carry the bow), `curvedRibbonMovedWordsHeldToInk` (only an
unfitted word of a moved line changes disposition) and
`gpuRibbonStraightRuleMatchesCpu`, which now also covers the region-edge case
and both sides of the cap, with the default kernels (`parallelRibbonMerge`) and
the reference ones. Over its 40 comparisons GPU and CPU agree exactly on
outcome and status and bit for bit on the bow; 24 geometries are
bit-identical and the others differ by at most 4.5e-5 px, which is the
intended agreement (the GPU sums bin positions in float per thread and merges,
the CPU in double).

The cap was chosen on the tuning pages and fresh set 1 only: no moved line
there bows more than 0.32 h, and fitted moved words show no placement trend
with bow up to it (edges within 0.1 h: 88%, 95% and 85% for bows up to 0.16,
0.24 and 0.32 h). Narrowing to region-edge refusals costs 32 tuning words
(+386 to +354 before the placement fix).

#### Where moved words are drawn

Measured after the worker's ink fitting, from the read's own output (the
corpus timing evidence now records `inkFitted`, the final quad and
`movedLineBow` per word) and the page pixels (`r12b_placement.py` in the
program's tools). Ink is the pixels on the word's own side of the midpoint
between its measured foreground and background; h is the height of the
word's ink. "Same pages" weights the ordinary straight-line words (not ribbon,
not moved) of each page by that page's moved words, because moved words
cluster on a few pages: on the tuning pages 47 of 246 are on one dark, noisy
page where ordinary words fit at 28 percent.

Before the fix, 34 of 246 moved tuning words (33 on that page) failed ink
fitting and were painted with the unrefined detector span, up to three times
their ink height. Ordinary words on the same pages fail at the same rate and
are painted the same way. Under the narrowed rule the case the review found on
tuning page 18's top line no longer occurs: 48 of its 49 moved words fit. Final
binary, painted words, moved / ordinary on the same pages:

| Set | Moved (fit-eligible) | Fit rate | Withheld by the fix | Edges within 0.1 h | Box over another row by more than 0.1 h | Another line's ink in the box |
| --- | ---: | --- | ---: | --- | --- | --- |
| Tuning | 246 | 86.2% / 83.8% | 34 | 90.1% / 82.0% | 1.4% / 2.3% | 0.5% / 0.2% |
| Development A | 356 | 82.9% / 83.2% | 61 | 92.2% / 81.7% | 1.4% / 1.0% | 0% / 0.2% |
| Development B | 111 | 89.2% / 95.7% | 12 | 92.9% / 90.0% | 6.1% / 2.8% | 0% / 1.0% |
| Bent | 497 | 99.6% / 99.2% | 2 | 96.6% / 97.5% | 21.2% / 13.7% | 0.2% / 0.7% |
| Gutter | 313 | 99.0% / 99.6% | 3 | 97.1% / 98.2% | 7.4% / 2.6% | 1.0% / 0.1% |

Fresh sets 1, 5 and 6 move 8, 6 and 9 words: all fitted, all within 0.1 h,
none over another row. The median box height equals the ink height in both
groups. The row overlaps inspected are on dense pages with tight leading,
where fitted boxes of ascenders and descenders touch the adjacent row; ordinary
words do the same, and on the tuning pages one overlap is a title word's own
box reaching down onto a moved line.

Against the review's pass criteria: the fit rate is within two points of
ordinary words on the tuning, development A, bent and gutter pages, but 6.5
points below on development B (12 words over 14 pages). Final boxes are within
0.1 h of their ink for 90 to 97 percent of drawn moved words: as often as
ordinary words on the flat sets, slightly less often on the bent and gutter
pages, and not for every word. Boxes of moved words do intersect neighbouring
rows' boxes, and more often than ordinary words on the development B, bent and
gutter pages. **The criteria are not met.**

#### Protection in the final output

`r12b_protection.py` checks, per page of every set, that every detection that
is still refused has its protection box inside the union of the
`protectedBounds` of the read's invalid-geometry words. Today's read already
fails this on some pages (tuning 4 detections plus 3 that decoded no word,
fresh 1 6, development A 15, development B 26, bent 8, gutter 3, `v1` 1,
titles 7): a cut-edge fragment at an internal region edge loses its words in
the merge and the overlapping region's complete read is kept. The decisive
figure is a detection covered today and not with the switch: 0 on tuning,
fresh 1, 2, 5 and 6, development B, `v1`, gutter and titles; 1 on development A
(`held2-12`) and 2 on the bent set (`bent-1440-b1`, `bent-2160-b1`). In all
three the still-refused detection is a shorter fragment (99 to 100 percent of
its area inside) of a line that an overlapping detection reads in full, and in
neither arm does any word carry the fragment's own box. Today the coverage came
from the full line's refusal. The rule moves that line, because every shape
check passed over its full length, and its words are drawn fitted to their
ink. Round 12's detector-level check is still clean: every refused box of
today's read is refused again or moved, and none is missing.

#### Confirmation (final binary)

Build `fcc12a7` (code as of `5187f54`; `uct_paddle_tests.exe` SHA-256
`28827ae7f1ecbbe89ee6c778a3d88e290d5bd8053b61c2e0fd2afd07af029188`), RTX 4090,
driver 621.14, Ryzen 9 5900X, MSVC 19.44 Release, Qt 6.12.0, CUDA 12.9.86,
TensorRT 11.4.0.106, medium/medium BF16, one first read per page, profiled
(`r12b-final-q-*`). The gutter set (`perf_corpus.py --gutter 1`, new) was
generated after this build. `straight` was not re-measured.

| Set | Labels | Exact today | `straight-refused` | Worse / better pages | Invalid words today / switch | Protection boxes | Moved / still refused |
| --- | ---: | ---: | ---: | --- | --- | --- | --- |
| Tuning, 20 | 8,154 | 6,163 | 6,487 (+324) | 0 / 10 | 584 / 198 | 399 / 148 | 36 / 33 |
| Fresh 1, 12 | 7,381 | 7,328 | 7,341 (+13) | 0 / 2 | 13 / 0 | 10 / 0 | 4 / 4 |
| Development A, 40 | 12,180 | 9,225 | 9,679 (+454) | 0 / 18 | 946 / 391 | 580 / 307 | 53 / 61 |
| Development B, 48 | 5,323 | 3,076 | 3,193 (+117) | 0 / 18 | 805 / 617 | 586 / 487 | 29 / 146 |
| `v1`, 12 labelled | 4,394 | 4,106 | 4,106 (0) | 0 / 0 | 36 / 36 | 13 / 13 | 0 / 14 |
| Fresh 2, 12 | 6,610 | 6,577 | 6,577 (0) | 0 / 0 | 0 / 0 | 0 / 0 | 1 / 0 |
| Fresh 5, 12 | 6,967 | 6,912 | 6,920 (+8) | 0 / 2 | 10 / 2 | 9 / 2 | 3 / 0 |
| Fresh 6, 12 | 6,016 | 5,954 | 5,971 (+17) | 0 / 2 | 20 / 0 | 15 / 0 | 4 / 0 |
| Bent, 9 | 5,976 | 3,648 | 4,193 (+545) | 0 / 5 | 1,863 / 1,304 | 783 / 448 | 72 / 234 |
| Gutter, 9 (new) | 5,440 | 4,464 | 4,949 (+485) | 0 / 7 | 871 / 373 | 280 / 119 | 52 / 53 |

- Titles (62 private placements, counts only): complete in 43 in both arms;
  none of their refused detections moves.
- Recall: bent 0.610 to 0.702, gutter 0.821 to 0.910; `book-curved-1080`
  unchanged.
- Newly shown words that match no label ("other words"): tuning 14.1% against
  14.7% for the rest of the shown words, development A 15.9 against 14.6,
  development B 29.6 against 31.3, bent 1.1 against 2.6, gutter 1.0 against
  1.5, fresh sets 0.
- Speed (`r12b-final-speed-v1`, two reversed cohorts of five;
  `r12b-final-speed-tune`, two of two): `v1` warm 1.000x, first read 1.007x,
  output byte-identical on 18 of 18 scenes and no line moved on any `v1`
  scene, so every difference is noise; the largest is `book-curved-1080` at
  3.3 percent warm (1.9 first read), above the 2 percent dense floor in this
  run. Tuning pages warm 0.985x, first read 0.996x; the cohorts
  of sparse pages disagree by up to 38 percent there (contention), so that
  run shows no change beyond the noise and no more.

Interaction with region reconciliation: a moved line is no longer `curved`
(`detection.curved` means a valid or refused ribbon), so with
`OKUFLOW_UCT_TEST_REGION_RECONCILE` also on it becomes eligible for reconcile
drops like any rectangle. The two switches were not measured together.

Correction to round 12: the bent pages have one or two columns, not one to
three.

#### Default after 12b

Off. The reading gains hold (+324, +454 and +117 on the flat groups, +545 and
+485 on bent and one-sided gutter pages, no page worse anywhere, speed
unchanged), no drawn moved word uses an unrefined box any more, and drawn
moved words sit on their ink about as often as ordinary words. But the
review's pass criteria are not met: the fit rate on development B, boxes
touching neighbouring rows more often than ordinary words on the bent, gutter
and development B pages, and three fragments of moved lines that lose the
output protection they had through the full line. As the brief asks in that
case, the recommendation is to keep the switch off. Pages to look at by hand:
`bent-1080-mild-b1`, `bent-1440-b1`, `bent-2160-b1`, tuning pages 18 and 01,
`gutter-1080-late-g1`, and the development pages whose moved lines bow most
(`held-out-07`, `held-out-03`, `held5-02`, `held4-02`; `held2-12` for the
protection case).

## Round 8: two inferences in flight on two streams (negative result, not built)

After rounds 1 to 3 most of a read is GPU time inside the two networks, and
every inference runs after the previous one on the State's one CUDA stream.
The reader already keeps two recognition groups in flight, but both are
queued on that stream and run in sequence. One recognizer call (at most eight
crops on the fixed 8x3x48x1024 canvas) and one detector pass (at most 960
pixels on the long edge) each take about 4 ms on the RTX 4090. The hypothesis:
one such inference does not fill the GPU, so two of them on two streams, each
with its own execution context, finish sooner than one after the other, with
identical results.

The brief made a measurement the first step, with a stop rule fixed before
anything was built: two streams must finish a set of recognizer inferences at
least 1.25x sooner AND a set of detector inferences at least 1.15x sooner than
one stream, or the round ends with the measurement and this record. **Result:
not met, by a wide margin.** Two streams are no faster for the recognizer
(0.89x to 1.01x) and slower for the detector (0.42x to 0.88x). Nothing in the
reader changed, no switch was added, and the targets of step 2 (warm worker
time, first reads, startup, identical corpus output) were not attempted.

### What was measured

`optionalStreamConcurrency` (`tests/uct_stream_probe.hpp`; the probe is in
`src/common/uct_tensorrt.cpp` and compiled only into `uct_paddle_tests`) takes
the prepared engines of a ready reader and creates its own execution
contexts, each with its own activation memory (the engine's default
allocation), input and output buffers and its own non-blocking stream; the
reader's contexts, stream and buffers are not used. For each engine and shape
it times a set of inferences on one context and one stream (today's
arrangement), on two contexts fed alternately from one host thread, and on
three. Each inference first copies one of four deterministic inputs into its
context's input buffer on that context's stream, as the reader writes its
canvas before each call. Each configuration runs with the plain `enqueueV3`
(the reader's default) and with each context's enqueue captured once as a
CUDA graph and launched, which takes the host's enqueue cost out and leaves
what the GPU does. One untimed set comes first; the configurations alternate
their order between the nine timed repeats. Afterwards every inference of
every configuration runs again untimed and its output is copied back and
compared byte for byte with the output of the same input on one stream, as is
each context's last output of every timed set.

Shapes: the recognizer at batch eight on the 1024 canvas, 64 inferences per
set; the detector at the two shapes of a 1920x1080 read, 32 per set: the whole
frame (544x960) and one of the two equal tiles (1040x1080 pixels, detector
input 960x928).

RTX 4090 (driver 621.14), Ryzen 9 5900X, MSVC 19.44 Release, CUDA 12.9.86
runtime, TensorRT 11.4.0.106, medium BF16 detector and recognizer, branch
`perf/okuread-port-r8` at `f04a60d`. The GPU was held through the shared lease,
so no other job used it. Evidence `r8-stream-probe-1` and `r8-stream-probe-2`;
the second sampled `nvidia-smi` every 100 ms: with the GPU busy (utilisation
at least 50 percent, 457 samples) it ran in P2 at a median 2,640 MHz (10th
percentile 2,505 MHz), display not driven. The sets ran back to back, so the
GPU had no idle gap to clock down in (round 13).

Median wall time of a set over nine repeats on one stream, and the speedup of
two and three streams against it, run 1 / run 2; in brackets the range of the
paired per-repeat speedups over both runs:

| Engine, shape, set | Launch | One stream ms | Two streams | Three streams |
| --- | --- | --- | --- | --- |
| Recognizer 8x3x48x1024, 64 | plain | 272.9 / 271.3 | 1.010x / 0.998x (0.985 to 1.016) | 0.941x / 0.935x (0.924 to 0.955) |
| Recognizer 8x3x48x1024, 64 | graph | 252.2 / 251.5 | 0.896x / 0.893x (0.888 to 0.905) | 0.838x / 0.833x (0.827 to 0.845) |
| Detector 544x960 (whole frame), 32 | plain | 106.3 / 102.5 | 0.882x / 0.801x (0.708 to 1.143) | 0.819x / 0.725x (0.633 to 1.153) |
| Detector 544x960 (whole frame), 32 | graph | 80.2 / 80.3 | 0.621x / 0.616x (0.570 to 0.677) | 0.463x / 0.447x (0.429 to 0.491) |
| Detector 960x928 (tile), 32 | plain | 162.2 / 160.6 | 0.720x / 0.792x (0.537 to 0.888) | 0.513x / 0.538x (0.465 to 0.715) |
| Detector 960x928 (tile), 32 | graph | 143.0 / 142.8 | 0.424x / 0.430x (0.416 to 0.467) | 0.414x / 0.416x (0.392 to 0.430) |

Output: every compared output was byte-identical to the single-stream output
of the same input, 1,128 comparisons per run over every engine, shape, stream
count and launch mode, 0 differences; the slot fails on any. Of those, 316 are
the one-stream configurations compared with themselves (repeatability) and
812 compare a second or third context with the first. For the patterns and
configurations tested, the inferences themselves would therefore not change if
they ran concurrently. The reader read again after the probe and its State retired
cleanly.

### What the numbers say

- **Overlapping inferences of one engine loses throughput.** With graph
  launches the host does almost nothing (under 1 ms per set). One stream then takes 3.93 ms per
  recognizer inference, 2.51 ms per whole-frame detection and 4.46 ms per tile
  detection; two streams take 4.40, 4.07 and 10.4 ms per inference.
  The loss is small for the recognizer and large for the detector at the tile
  shape, where two streams take more than twice as long as one after the
  other: the overlapping inferences get in each other's way. Why is not
  established: one inference filling the GPU, the working set of two detector
  inferences (about 280 MiB of activation memory each) against the 72 MB L2
  cache, and TensorRT tactics chosen for a GPU of their own are candidates; no
  kernel profile was taken. What is shown is that this way of overlapping does
  not help, not that the GPU is saturated.
- **Host time in the plain submission loop is not all CPU work.** On one
  stream the host spends about 250 of the recognizer set's 272 ms in the loop
  that copies the input and calls `enqueueV3` (about 3.9 ms per call when 64
  are issued back to back; round 3 measured about 2 ms per call inside a
  read). That time falls as streams are added for the recognizer (250, 223,
  214 ms) and rises for the tile detector (147, 172, 225 ms), which a fixed
  CPU cost per call would not do: part of it is the host waiting for room in
  the GPU's queue. It should not be read as 3.9 ms of removable CPU work per
  call. The graph rows, where the host is out of the picture, carry the
  verdict.
- **Memory.** Each added context with its own activation memory took 164 to
  166 MiB for the recognizer (plus 80 MiB for its input and its 19.2-million
  float output) and 280 to 282 MiB for the detector (plus 8 or 16 MiB of
  buffers), as the change in free device memory (2 MiB granularity); a
  captured graph up to 2 MiB more. A dual-stream reader would have needed
  roughly 440 MiB more before any scratch.

### Not established

- One GPU, the RTX 4090, and the medium BF16 pair. The small and tiny tiers
  and recognizer batch one, which make smaller inferences, were not probed.
- Overlapping the two different engines (the next region's detection beside
  the current region's recognition) was not measured; the brief's hypothesis
  and stop rule concern two inferences of one engine. Whatever overlap would
  do there, its ceiling is low: on the dense 1920x1080 document the detector
  is 11.7 of 193.5 ms, so hiding all of it gives about 1.06x, and on a lecture
  frame the recognizer is 2.6 of 19.8 ms; detection also has to precede
  recognition of the same region.
- The inputs are deterministic patterns, not image crops. Inference time does
  not depend on the values, but the outputs compared are those of the
  patterns.
- Stream priorities, other TensorRT tactic choices per context and limiting
  each context to part of the GPU were not tried.

### Default and code

Nothing in the read path changed and there is no new reader switch. The
measurement slot stays (opt-in, not in any CTest list, compiled only into
`uct_paddle_tests` through `OKUFLOW_UCT_GRAPH_FAULT_HOOKS`). Round 10's note
about one activation arena per engine stands unchanged: nothing runs the
contexts of one engine concurrently.

### Review

Council on `8efffd1`: Claude Opus 5.5 8/10, GPT-6.1 Sol 8/10, Gemini 4 Hydrogen
9/10. All three found the measurement sound (separate contexts, activation
memory, buffers and streams; timing to completion of all work; graph mode
isolating the GPU) and the stop correct under the rule; two recommended
merging the slot and the record, one after the corrections below. Applied
while merging: the headline no longer claims the GPU is saturated; the host
time of the plain loop is described without attributing it to CPU work; the
count of cross-context comparisons is stated; the ceiling argument for
overlapping the two engines replaces "unpromising"; the slot now fails unless
both plans were loaded from the cache, and its description says it initializes
the ordinary reader instead of promising that nothing is ever prepared.

## Round 11: detector and recognizer prepared side by side (on by default, exact)

Target: startup to first result (unprofiled, median of at least five fresh
warm-file-cache processes, forward and reverse cohorts separately) at least
200 ms less than the base build in each order; byte-identical output on all 18
scenes and an identical backend detail string; warm worker time unchanged
within noise; preparation cancelled at any point retires cleanly and nothing is
joined at exit. **Met**: 878 to 614 ms forward and 670 to 428 ms reverse.

Every pin, digest, bound, ownership and smoke check that ran before still runs,
on the same bytes, before the artifact it guards is used and before the State
is published. Nothing verified is kept across calls or processes. There is no
switch.

This section records round 11 as reviewed (`aff2b2a`). The council found the
recognizer overlap's decisions not yet equal to the sequential code's; "Round
11 follow-up" below changes the latch, the deadline, the fallback and fault
paths, the asset check and the context order, and gives the current numbers.
Where this section describes the early latch, the shared deadline, the
speculative second graph check or the decision test, the follow-up supersedes
it.

### What changed

1. **Asset files hashed side by side.** `LoadPaddleAssets` hashed up to six
   files one after the other on the production root (each role's original
   graph and config to choose its directory, then the chosen graphs again).
   Each of those reads now runs as its own task, all joined before the call
   returns. The second graph check starts at once on the original path and is
   used only when the original directory is chosen; otherwise the chosen path
   is verified afterwards. Results are taken in the original order, so the
   chosen paths and the error are the sequential loader's.
2. **The production recognizer loads beside the detector.** On the BF16
   production path (`preparation_overlap::Ahead`,
   `src/common/uct_preparation_overlap.hpp`) the preparation thread reads the
   recognizer's failure memo, takes its attempt latch and starts one helper
   thread, then prepares and smoke-checks the detector exactly as before. The
   helper selects the device and runs the same `Network::PreparePinned` call
   (source read and verify, runtime, plan read and verify, deserialization,
   engine contract, execution context) into a Network attached to the State
   before its first vendor call. It never builds and never touches the stream:
   a cache miss ends the load as "needs build". At the original recognizer
   point the preparation thread joins the helper, reads the memo again and
   hands the outcome to the original code: a loaded engine goes on to the
   original zero-input compatibility check, a cache miss to the original build
   (a fresh `PreparePinned` that repeats every check), a failure is rethrown
   unchanged into the original catch. If the detector side fails or is
   cancelled first, the helper is stopped and joined and its latch is
   released, because the sequential code never reached the recognizer. A memo,
   a latch already held or a thread that cannot be started means no helper;
   the original code then runs unchanged. Other paths (FP32-only GPUs, the
   local BF16 manifest, candidate validation, FP32 control, experiments,
   bucketed width) stay sequential.
3. **Completion polls no longer wait a whole timer tick.** `ConfirmPublicCompletion`
   slept 1 ms between polls; on Windows that lasts a system timer tick
   (15.6 ms median measured on this machine), so each wait for a few
   milliseconds of preparation GPU work (four detector smoke shapes, the
   recognizer check) cost about one tick. `CompletionPollPause` waits 250 us on
   a high-resolution waitable timer, without changing the system timer
   resolution, and falls back to the 1 ms sleep. The poll, its cancellation and
   its 30 s deadline are unchanged. It is used only during preparation.

### Vendor rules

The SDK at `Z:\Projects\OkuRead\local-work-data\sdks\tensorrt-11.4.0.106` ships
headers, binaries and libraries only; the headers are the shipped vendor
documentation. `NvInferRuntime.h`, `ILogger`: "The Logger object implementation
must be thread safe ... TensorRT does not hold any synchronization primitives
when calling the interface functions", and `log` "may be called from multiple
threads when multiple execution contexts are used during runtime, or if the
same logger is used for multiple runtimes, builders, or refitters". The
`IErrorRecorder` methods are likewise required to be thread-safe; this code
sets none, so errors go to the logger, whose `log` and `Error` take a mutex.
The headers contain no statement that one `IRuntime`, `ICudaEngine` or
`IExecutionContext` may be used from two threads, and none is: each Network
creates its own runtime, engine and context, so the two threads share only the
logger, the read-only runtime factory pointer and version string of
`Libraries`, the device and the stream handle (which the helper never uses).
The builder never runs on the helper. CUDA runtime state is per host thread,
so the helper calls `cudaSetDevice` first and reads its own
`cudaPeekAtLastError` before it ends. One consequence remains: TensorRT
serializes much of the two engines' context creation internally (below), so
the overlap that matters is verification and deserialization against the
other engine's vendor work, as the brief anticipated.

### Ownership, cancellation and decisions

- The helper is joined by the preparation thread on every path, normal or
  exceptional, before that thread drops its State reference. The preparation
  thread itself is still detached and still never joined: a reader destroyed
  during preparation sets the cancel flag and returns.
- The helper's stop is the preparation's own cancellation and deadline plus
  "abandoned". Objects the load creates may keep that stop callable after the
  helper is gone (it holds no reference into the joined object).
- The early latch is released only when the recognizer point is never
  reached or a memo appears there; a released latch has no queued work, so its
  completion is known. Adopted, it belongs to the original code and follows
  its rules: `AllowsTensorRtCapacityFallback`, `VerificationInterrupted` and
  `ArtifactError` reach the same catches as before, failure memos are written
  only there, and a cancellation releases latches only after the drain.
- Two deliberate differences, both timing only: the recognizer's 600 s deadline
  starts when the helper starts (earlier by the detector's preparation time);
  and when two preparations of the same recognizer race, the latch is taken
  earlier, so which of them loses the race can differ. A memo or a held latch
  at the start defers both decisions to the original point, as before.

### Measured

RTX 4090 (driver 621.14), Ryzen 9 5900X, MSVC 19.44 Release, Qt 6.12.0, CUDA
12.9.86, TensorRT 11.4.0.106, medium/medium BF16 cached plans, batch eight,
fixed1024, warm file cache, GPU use serialized through the shared lease.
Base: a build of `6e454cc`; overlap: a build of `a5c8190` (changes 1 and 2);
new: `8c64ffa` (all three). Evidence `r11-startup-ab`: one process per arm and
cohort, ten cohorts (five forward, five reverse), five warm reads per scene.

| Startup to first result, unprofiled | base ms | overlap ms | new ms | new - base |
| --- | ---: | ---: | ---: | ---: |
| forward (first scene doc-camera-1080), median of 5 | 878.1 | 667.9 | 613.6 | -264.5 |
| reverse (first scene board-lecture-05), median of 5 | 669.5 | 486.7 | 427.6 | -241.9 |
| forward processes | 862, 878, 880, 885, 867 | 725, 720, 666, 668, 651 | 651, 605, 613, 620, 614 | |
| reverse processes | 752, 664, 716, 670, 667 | 497, 464, 487, 512, 460 | 462, 420, 425, 454, 428 | |

Every new process is at least 211 ms (forward) and 202 ms (reverse) below
every base process of the same order. The harness polls a preparing reader
with a 20 ms sleep, which Windows rounds up to whole timer ticks, so startup
figures carry that granularity.

Wall phases, profiled, attribution only (`r11-preparation-profile-final`, ten
fresh processes per build on doc-camera-1080; nested rows overlap, do not add;
the recognizer rows of the new build run on the helper beside the detector
rows):

| Role / phase | base median ms | new median ms |
| --- | ---: | ---: |
| assets: verify-and-dictionary | 168.76 | 55.51 |
| preparation: runtime-dll-load | 19.50 | 20.12 |
| detector: source-read-and-verify | 41.33 | 44.80 |
| detector: plan-read-and-verify | 23.23 | 24.38 |
| detector: deserialize | 17.56 | 27.23 |
| detector: execution-context | 92.31 | 168.15 |
| preparation: detector-prepare | 182.33 | 273.03 |
| preparation: detector-smoke-four-shapes | 70.14 | 35.42 |
| recognizer: source-read-and-verify | 51.56 | 55.46 |
| recognizer: plan-read-and-verify | 27.70 | 28.97 |
| recognizer: deserialize | 5.42 | 12.59 |
| recognizer: execution-context | 73.46 | 76.76 |
| preparation: recognizer-prepare (preparation thread) | 171.77 | 8.69 |
| preparation: completion-and-receipts | 4.77 | 4.47 |
| preparation total (elapsed at last checkpoint) | 448.45 | 342.13 |

The detector's execution context takes 76 ms longer and its deserialization
10 ms longer while the recognizer's are created at the same time: TensorRT
largely serializes context creation. What the overlap gains is the
recognizer's verification and the parts that do not contend. The earlier
profile of the overlap alone (`r11-preparation-profile`, build `a5c8190`) gave
474.01 to 389.93 ms with the smoke unchanged (69.66 to 66.27 ms); the poll pause
halves the smoke and shortens the recognizer check.

What is left on the path, in order: assets 56 ms, runtime DLL 20 ms, detector
273 ms (of which about 85 ms is contention with the recognizer's vendor work),
smoke 35 ms, the recognizer check 9 ms, receipts 5 ms, then the first read.
The detector and recognizer context creations together are sequential vendor
time.

### Correctness evidence

- Output: byte-identical complete output on **18/18** scenes for both the
  overlap and the new build against the base in all ten cohorts
  (`r11-startup-ab`); the backend detail string (plan keys, payload digests,
  cache status, graph identity, four smoke shapes) is identical in all 30
  processes. Warm geometric mean new versus base 1.009x, first read 1.008x
  (overlap 0.992x and 0.989x): within the 1 percent noise floor.
- Decisions: `preparationOverlapMatchesSequentialDecisions` (CTest
  `uct_paddle_processing`) drives the real `Ahead` with real threads through
  504 scenarios: seven load outcomes (loaded, needs build, typed refusal,
  verification interrupted, CUDA fault, allocation failure, unknown exception),
  the helper ending before, after or together with the preparation thread,
  the detector side ready, failing or cancelled, a memo at the start and at the
  recognizer point, and a latch held by another preparation. For each it
  compares the path handed to the original code, the rethrown exception type
  and the latch state with the sequential order. `preparationOverlapAbandonStopsJoinsAndReleases`
  checks stop, join, release, a stop kept after the unit is gone, and
  double adoption.
- Assets: `optionalAssetVerificationMatchesSequential` compares
  `LoadPaddleAssets` with a frozen copy of the `6e454cc` loader on the real
  medium files in seven layouts (intact production root, tampered with and
  without downloaded roles, a downloaded role with a changed config, explicit
  roots, an empty root): identical in all seven.
- Cancellation (`r11-cancellation`): `optionalPreparationCancellation` cancels
  the production preparation at 24 points from 0 to 560 ms (19 while it was
  still preparing; corrected from 17). The points are measured from before
  `Initialize()`, which returns only after the asset check, so no point
  interrupts the asset hashing. Destroying the reader took at most 0.64 ms (it never waits
  for the preparation), every cancelled State's retirement completed, and after
  each cancellation a fresh reader became ready with the identical backend
  detail and identical output.
- CTest `uct_paddle_processing`, `uct_precision_artifact_contract`,
  `uct_model_installation_contract` and `uct_merge_replay`: 4/4 pass.

### Not established

- Only the RTX 4090 and this CPU were measured. The gain depends on spare CPU
  cores for hashing and on how much the GPU vendor library serializes context
  creation on other GPUs and drivers.
- Cold file cache, the first session after boot and the real application's
  model hand-off were not measured; the harness initializes CUDA first.
- The overlap applies to the BF16 production path only; Turing and other
  FP32-only GPUs keep the sequential engine order and gain only from the asset
  hashing and the poll pause.
- No GPU fault, allocation failure or cache miss was injected on the helper;
  those paths are covered by the model-free decision test, not end to end.
- No GUI, native close or shipping/CPU/CUDA matrix run (the orchestrator runs
  the full matrix).

### What did not help

Context creation does not overlap well: with both engines creating contexts at
once the detector's took 168 ms instead of 92 ms. No other variant was built
and dropped; reordering the two context creations was not tried (the follow-up
does it).

## Round 11 follow-up: the recognizer overlap made decision-exact (on by default)

Target, from the council review of round 11 (scores 7, 7, 8): the claim
"failure, fallback and cancellation decisions are unchanged" was not yet true
of the recognizer loaded beside the detector. Make it true (items 1 to 6),
remove the duplicate graph read of the asset check (item 7), prove the
recognizer point's decisions on the production wiring, run the cold-cache
build path and overlapping preparations on the GPU, and measure again. The
overlap stays on by default only if every item is closed. **All seven are
closed; it stays on.** One finding outside the seven is reported below
("Overlapping preparations").

### What changed

1. **Deadline.** The helper's deadline starts when it starts and governs only
   its own work. The recognizer point starts its own 600 s deadline, as the
   sequential code did, for every outcome. A helper load stopped by its own
   deadline is not adopted: the original load runs in place under the fresh
   deadline (item 4 below), so no load or build is refused for a deadline the
   sequential code would not have hit.
2. **Latch.** The helper takes no latch and writes nothing (its load never
   builds, creates no cache directory and queues no stream work). `Decide`
   (`src/common/uct_preparation_overlap.hpp`) runs the recognizer point in the
   original order: join the helper, read the memo, take the latch. A memo or a
   latch held elsewhere discards the helper's Network and refuses exactly as
   before. The helper reads the memo once at its start only to skip pointless
   work. Two preparations of the same recognizer therefore meet the latch only
   across their recognizer stages, as in the sequential order
   (`preparationOverlapAbandonedOwnerDoesNotRefuseAnother`; GPU below).
3. **Nothing left in a published State.** With the latch, only an engine the
   helper loaded completely is adopted. A cache miss, an artifact refusal, an
   interrupted verification or a plain runtime error is discarded and the
   original load runs in place, so the sequential code decides it again
   itself. Every refusal path discards, the FP32 fallback resets
   `State::recognizerAhead` again, and publication throws while a helper is
   pending or its Network is set.
4. **Publication.** The helper is settled (stopped, joined, discarded) after
   the recognizer point on every path, before any other recognizer is
   prepared. `Decide` never adopts a load started for another attempt key; it
   settles it and runs the original load.
5. **Faults.** A helper that ended in a fault (CUDA, TensorRT API, allocation,
   unknown) is never discarded. At the recognizer point the Network is
   attached and the fault raised as the original load would have raised it.
   When the detector side ends first (`DetectorSide`), before the detector's
   FP32 fallback (so a helper fault is never recorded in the detector's
   failure memo) and before a detector build, `Settle` raises the fault as a
   fault of the same class with the detector side's failure first and the
   helper's message after it, and holds the recognizer as after a terminal
   fault in its own stage: latch taken unless a memo or another holder refuses
   it, never released, no memo written.
6. **Loggers.** The helper reports to `State::recognizerAheadLogger`. When its
   load is adopted, `Logger::Absorb` copies its last error into
   `State::logger`, which then holds what one shared logger would have held
   after the sequential order. No message of one engine reaches the other's
   failure text or memo.
7. **Assets.** One task reads and hashes each role's original graph and serves
   both the directory choice and the final check. The recognizer's
   downloaded graph is not read when the detector's already failed, as in the
   sequential `||`. Paths and errors are unchanged (7/7 layouts, three runs,
   `r11b-assets`). A damaged production root with valid downloads now costs
   what the sequential loader costs (120 to 127 ms against 121 to 126 ms; round
   11 cost 154 against 129). The only extra work left on a damaged root is the
   original config's hash when the original graph already failed.

Two further changes came out of the GPU runs:

- **Detector builds and context creation come first.** While the helper is
  pending, a detector first loads without building. A cache miss settles the
  helper before the build, so no build shares the device with it, and the
  build runs through the ordinary call on a fresh Network. The helper waits at
  a gate right before its execution context until the detector's context
  exists (`Ahead::Open`). TensorRT largely serializes two context creations,
  and beside the helper's the detector's took 168 ms instead of 92 ms, which
  lengthened the detector stage and the time its latch was held. With the gate
  it takes 94.7 ms (base 93.5) and preparation is shorter overall.
- **Cancellation releases before the join.** The outer handler releases a
  cancelled preparation's latches once its stream is confirmed complete.
  `DetectorSide` now runs the same release (shared and idempotent) before it
  settles the helper, so a helper inside a vendor call never keeps the
  detector's latch held longer than the sequential code did.

Round 10 note: round 10 constructs every recognizer with
`state->recognizerArena`; the helper's construction site carries a
`ROUND 10 MERGE` comment.

### Decisions on the production wiring

`preparationOverlapMatchesSequentialDecisions` (CTest `uct_paddle_processing`)
drives the production `Ahead`, `Decide`, `Settle` and `DetectorSide` in the
order of `BeginPreparation`, with only the vendor calls and the unchanged catch
bodies faked. Each of 7104 scenarios runs twice: with the helper, and without
it (the production code with no helper is the sequential code). Decision,
recognizer and detector latch state and memo writes must be equal. Scenarios:
the helper's load loaded, needing a build, refused (`ArtifactError`), a plain
`runtime_error`, `VerificationInterrupted` (its own deadline), `CudaFailure`,
`TensorRtApiFailure`, `bad_alloc` or an unknown exception, each before or
after its gate as in production; the original load's outcome where it differs
(any outcome after a discarded helper); the helper ending before, after or
with the preparation thread; the detector ready, needing a build, falling back
to FP32, failing or cancelled; a memo at the start or at the recognizer point;
the latch held by another preparation; cancellation at the recognizer point.
The one deliberate rule: a helper that actually ended in a fault is raised and
holds the recognizer as above, and no memo is written. A mutation that
rethrows a non-fault helper failure, as round 11 did, fails the test.
`preparationOverlapAbandonedOwnerDoesNotRefuseAnother` runs two preparations
sharing a recognizer: A is cancelled while its helper is inside a vendor call,
B decides while A still waits for it and adopts BF16, and A takes nothing (or,
if A's helper then faults, A holds the latch as after its own terminal fault).
`preparationOverlapAbandonStopsJoinsAndReleases` covers the memo at the start,
a helper of another key, stop, join, the gate and `RaiseFault`.

### GPU evidence

Hardware and build as in round 11 (RTX 4090, driver 621.14, Ryzen 9 5900X,
MSVC 19.44 Release, Qt 6.12.0, CUDA 12.9.86, TensorRT 11.4.0.106, medium and
medium BF16, batch eight, fixed1024, warm file cache, shared GPU lease). New:
a build of `03d2cd7`. Base: the same `6e454cc` build as round 11.

**Startup** (`r11b-startup-ab`, unprofiled, one process per arm and cohort,
ten cohorts, five warm reads per scene):

| Startup to first result | base ms | new ms | new - base |
| --- | ---: | ---: | ---: |
| forward (first scene doc-camera-1080), median of 5 | 886.9 | 605.0 | -281.9 |
| reverse (first scene board-lecture-05), median of 5 | 674.4 | 423.7 | -250.7 |
| forward processes | 989, 887, 853, 947, 857 | 605, 613, 585, 588, 617 | |
| reverse processes | 714, 664, 674, 661, 694 | 386, 385, 471, 424, 431 | |

Every new process is at least 236 ms (forward) and 190 ms (reverse) below
every base process of its order. Warm geometric mean new versus base 1.005x,
first read 1.006x (noise floor 1 percent). Output byte-identical on **18/18**
scenes in all ten cohorts; one backend detail string in all 20 processes,
equal between the arms.

**Phases**, profiled, attribution only (`r11b-profile-gated`, ten processes per
build on doc-camera-1080; nested rows overlap):

| Role / phase | base median ms | new median ms |
| --- | ---: | ---: |
| assets: verify-and-dictionary | 170.16 | 53.14 |
| preparation: runtime-dll-load | 19.79 | 20.27 |
| detector: source-read-and-verify | 41.52 | 45.82 |
| detector: plan-read-and-verify | 23.03 | 24.02 |
| detector: deserialize | 17.38 | 23.31 |
| detector: execution-context | 93.50 | 94.73 |
| preparation: detector-prepare | 184.87 | 195.77 |
| preparation: detector-smoke-four-shapes | 64.64 | 51.99 |
| recognizer: source-read-and-verify | 53.05 | 55.34 |
| recognizer: plan-read-and-verify | 28.10 | 28.79 |
| recognizer: deserialize | 5.40 | 12.10 |
| recognizer: wait-for-detector-context | - | 97.43 |
| recognizer: execution-context | 75.73 | 85.39 |
| preparation: recognizer-prepare (preparation thread) | 179.42 | 41.42 |
| preparation total (elapsed at last checkpoint) | 455.28 | 316.37 |

Round 11 as reviewed gave 342.13 ms here; the gate saves the context
contention (the helper's context is now created during the detector smoke).

**Cold cache** (`r11b-cold`, slot `optionalNativePrecisionSetup`,
`LOCALAPPDATA` and the recognizer cache in new real directories under the
evidence folder; `$w\tools\run-paddle-lad.bat` is `run-paddle.bat` with
`LOCALAPPDATA` from `R11B_LAD`):

| Case | base | new |
| --- | --- | --- |
| both caches empty: first process | ready after 85.9 s; both plans `saved` | ready after 88.2 s; both plans `saved` |
| the same caches: second process | 603 ms; both `loaded` | 501 ms; both `loaded` |
| detector cached, recognizer cache empty: first / second | 39.8 s `saved` / 604 ms `loaded` | 38.2 s `saved` / 484 ms `loaded` |

Plan keys, cache states, context sizes, graph identities and the rest of the
detail are identical between base and new in every case; only plan payload
digests differ, and a second base build on its own empty cache
(`base-both-2`) also produced different digests, so that is TensorRT build
nondeterminism. The second case exercises the helper's "needs build" outcome
(the original build then runs in place); the first exercises the detector's
load-then-build with the helper settled first. The scratch plan caches were
deleted afterwards.

**Cancellation** (`r11b-cancellation`, `optionalPreparationCancellation`, 24
points over 0 to 596 ms measured from before `Initialize()`): 13 points still
preparing, destruction at most 0.51 ms, every cancelled State closed, every
fresh reader afterwards had the identical detail and output. As in round 11,
`Initialize()` returns only after the asset check, so no point interrupts the
asset hashing.

**Overlapping preparations** (`optionalOverlappingPreparation`): A is
cancelled at 24 points from its start to the end of its detector stage and B,
with the same detector and recognizer, starts at once. B's recognizer came up
on BF16 with the plan receipt of an undisturbed start at **48/48** points (two
runs, `r11b-overlap-gated`). At 40/48 B's whole detail and output were those
of an undisturbed start. At 8/48 (A cancelled 118 to 256 ms after its start)
B's BF16 detector found A's own detector attempt still in flight ("BF16
detector already failed or in flight this session") and chose the FP32
detector. That is the detector latch rule of the sequential code, which this
round does not change: A holds its detector attempt until its detector vendor
call returns and the cancellation is confirmed. How often a back-to-back B meets
it depends on how soon B reaches its detector. The same slot built on the base
(`r11b-overlap-base-probe`, diff kept there) never met it in 48 points:
there B's own sequential asset hashing (170 ms) gave A that much more time. On
`52edbf5`, asset hashing alone with no overlap (`r11b-overlap-hash-probe`), it
met it at 4/48. Round 11 without the gate (`r11b-overlap`, build `512c052`)
met it at 5/24 and failed the first, stricter version of this check. B's FP32
detector is a per-State choice: no memo is written, and the next preparation
tries BF16 again. Making B wait for A's detector attempt would change the
latch rule and is not part of this round.

### Not established

- Only the RTX 4090 and this CPU were measured, with a warm file cache.
- Faults on the helper (CUDA, allocation, out of memory while both contexts
  allocate) were exercised model-free only; none was injected on the GPU.
  Which side reports an exhausted device can still differ from the sequential
  order. Both orders end the preparation with a terminal fault, but the side
  whose latch stays held can differ.
- The detector-latch race above occurs in the base, in round 11's hashing
  change and here. Its frequency in the real application, where a new
  preparation follows a settings change, was not measured.
- No GUI run and no full build matrix (the orchestrator runs them). CTest
  `uct_paddle_processing`, `uct_precision_artifact_contract`,
  `uct_model_installation_contract` and `uct_merge_replay` pass 4/4.

### Review

First council pass (`aff2b2a`): Claude Opus 5.5 7/10, GPT-6.1 Sol 7/10, Gemini 4
Hydrogen 8/10. All three accepted the parallel hashing and the completion
wait; the recognizer overlap was held for the seven items the follow-up above
closes. Second pass (`708a968`): Opus 8/10, Sol 7/10, Gemini 9/10. All three
confirmed the seven items closed; Opus and Gemini recommended merging it on,
Sol wanted the latch race below handled first.

Applied at the merge: both Opus and Sol found one path on which a cancelled
preparation still joined its helper before releasing the detector attempt (a
cancellation first seen inside the detector's fallback handler); the
cancellation check now comes before the helper is settled there, in the code
and in the test driver. The recognizer loaded ahead and the detector rebuilt
after a cache miss are constructed with graph replay's arena flag, and the
helper's wait for the detector's context stays immediately before round 10's
arena-aware context creation.

Known and not fixed, the detector attempt race. When a preparation is
cancelled and another with the same detector starts at once (text reading
switched off and on, or only the recognizer tier changed, while a preparation
is still running), the new one can find the cancelled one's detector attempt
still in flight and then runs the FP32 detector for the life of that reader.
The rule is the existing one; the base build never met it in 48 test points,
parallel hashing alone meets it in 4 and the full round in 8, because the new
preparation now arrives about 75 ms after the cancellation instead of about
190 ms while the old one is still inside a vendor call it cannot interrupt. In
all 8 cases the output differed from the BF16 reference. No failure memo is
written and the next preparation tries BF16 again. The reviewers agree on the
remedy and that it changes the session latch's rule: a preparation that finds
an attempt in flight, as distinct from failed, waits a bounded, cancellable
time for it to resolve and then decides again; releasing the attempt at the
cancellation itself is unsound while a vendor call or a build may still be
running. Sol wanted that before the merge; Opus and Gemini as its own change.
It is not in this program and is listed as remaining work.
