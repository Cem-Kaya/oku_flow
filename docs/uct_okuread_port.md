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
