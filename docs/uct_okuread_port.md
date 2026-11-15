# OkuRead latency ideas brought back to OkuFlow

Status: development record, started 15 November 2026. OkuRead imported its OCR
components from OkuFlow at `c0e33a2` and then spent thirteen rounds on latency.
This records which of those ideas were evaluated here, in five measured rounds,
with what was changed, what was measured and what stays switched off.

Every timing below uses the protocol and scene set in
[corpus timing](uct_corpus_timing.md): scheduler worker time on file fixtures,
two reversed cohorts of five warm reads, RTX 4090 (driver 621.14), Ryzen 9
5900X, MSVC 19.44 Release, CUDA 12.9.86, TensorRT 11.4.0.106, medium detector
and recognizer in BF16, display on. The scene set contains no owner camera
capture; twelve scenes are generated and six are real chalkboard lecture
frames. Worker time is not camera-to-paint latency.

## What applied and what did not

| OkuRead idea | Here |
| --- | --- |
| Path-compressing component roots, four-bin ribbon merge | Round 1, on by default, identical output |
| Cooperative box scoring | Round 1, rewritten: OkuRead's exact variant gave no gain here |
| Fixed-tree box scoring | Round 1, private comparison switch only: it rounds differently |
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
   shuffles) was ported first and measured no gain here (61.6 against 63.4 ms on
   the textless scene). The serial additions are the cost, not the loads.
2. The original `boxes` kernel is not self-consistent as compiled. For sm_61
   to sm_90 the compiler unrolls its column loop by four and fuses the column
   product into the sum for the unrolled columns only; the leading
   `width % 4` columns of each row add two separately rounded products. For
   sm_100 and later it fuses every column. A pixel that defined a component's
   extent can sit exactly on the bound, so this decides membership. The first
   port differed from the original by one component on a threshold-sensitive
   map. `boxesInside` now reproduces the compiled rule for each architecture.
   The sm_100+ rule was read from generated code and has not run on that
   hardware.

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
- `gpuDetectKernelsMatchReferenceBytes` (in `uct_paddle_cuda`) compares the new
  kernels with the originals bit for bit on 216 detector maps at four sizes,
  with and without ribbon fitting: quads, ribbons, labels and components. The
  maps include spirals and combs for long parent chains, more than 128
  components, tilted and curved bands, small rotated boxes at the score
  threshold, quantized values that make rounding ties common, a nonzero
  background, and negative, large, subnormal and non-finite values. It reported
  17,552 eligible boxes and 34 valid ribbon fits.

### Not done

The tree box sum would bring the textless scene to about 11 ms of regions, but
it changes detector scores in the last bits and can flip the 0.4 threshold, so
it stays a private switch. The remaining exact cost on that scene is one warp
per very large component; real lecture frames do not have such components
(their regions stage is now 1.7 to 2.5 ms), so it was not pursued.
