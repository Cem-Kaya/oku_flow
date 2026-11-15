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
| Replay TensorRT enqueue as CUDA graphs, one execution context per cached shape | Round 3, private switch, off: identical output; startup and the first read of a new frame size cost more |
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

## Round 3: TensorRT enqueue replayed as CUDA graphs (private switch, off)

Targets stated before the work: byte-identical complete output on all 18
scenes in both cohorts, on the 62 title placements and on one labelled set;
warm worker time at least 1.20x on the six real lecture frames and at least
1.05x on dense scenes; the first read of each scene no more than 10 percent
slower, or an accounting of what the first use costs and after how many reads
it is repaid, also with `OKUFLOW_UCT_TEST_REGION_RECONCILE=1` in both arms.

Result: output identical everywhere it was measured; lecture frames 1.23x to
1.35x and dense documents 1.09x to 1.10x warm. The first-read target is met
except where a scene brings a frame size the process has not read before:
2 of 18 scenes, accounted for below. Startup takes 0.36 to 0.98 s longer.

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
  again; after 32 refused shapes capture stops. Capture is also refused with
  less than 256 MiB free. Every other failure is fatal, as before, and goes
  through the existing State retirement. No refusal occurred in any run.
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
are more than 10 percent slower, while the first-read geometric means are
1.101x and 1.061x faster. A live source keeps one frame size, so after
startup it pays this at most once.

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

About 55 MB more in total with eight shapes. One graph per engine adds about
2 MB.

### Default

Off in this branch. Recommendation: enable it by default for the TensorRT
reader. Output was identical on every set measured, the warm gain is largest
on the live lecture frames this reader runs on continuously, and the costs
are bounded: preparation is about 1 s longer while the CPU reader serves, a
frame size first seen after startup costs one read 80 to 180 ms, and about
55 MB of device memory are added. Before enabling it, check on one more GPU
generation: every measurement here is one RTX 4090.

### Not established

Measured on one RTX 4090 with the medium BF16 pair only. The small and tiny
tiers, FP32 plans, the bucketed recognizer width (up to 32 shapes with batch
eight, more than the bound) and batch sixteen were not run. Paused-detail
reads use other detector shapes; they were not run with the switch on and
would create or evict detector contexts. No capture refusal or 256 MiB
headroom refusal was observed, so those fallbacks are read and covered by the
model-free test, not exercised on the GPU. No owner camera capture was
available.
