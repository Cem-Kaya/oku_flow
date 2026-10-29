# OCR design: curved pages and character-change detection

Opus design sub-agent, 2026-10-24. Baseline `8243864` (Windows 0.3.1). This is a
read-only source and literature review. No code, build, test, GPU, model, phone
or GUI work was run. No gain is measured or implied.

## 1. Source facts behind the design

**Geometry.** Each line is a single four-corner quad. `PaddleDetect` fits a PCA
minimum-area rectangle to each DB component and then unclips it
(`src/common/uct_paddle_processing.cpp:448`; CUDA `axes`/`extents`,
`src/cuda/uct_kernels.cu:121,156`). Rectification, recognition windows and the
mapping from CTC spans to words all sample through the bilinear
`At(quad,u,v)` (`uct_paddle_processing.cpp:32,160,560,615`; CUDA
`rectify`/`rectifyBatch`, `uct_kernels.cu:341,360`). The painter maps each word
with one `quadToQuad` (`src/ui/ultra_clear_text_overlay.cpp:615,630`), and
accessibility derives its geometry from that last actual paint. On a curved
line this produces a rectangular crop with the text bent inside it, and the word
quads lie on the chord rather than on the ink. DB detection handles
arbitrary shapes [2], so the component masks already follow curves; the
rectangle fit is the step that throws that away. Stock PaddleOCR does the same thing: it crops
non-`quad` polygons by minimum-area rectangle [1]. I found no curved-text-line
fixture in `tests/`.

**Verifier.** A reference is at most 64×12 box-filtered cells
(`src/common/ultra_clear_text.cpp:1364-1365`) taken from a probe decimated by at
least 2 to a width of 960 or less (`src/app/ultra_clear_text_controller.cpp:128,1257`). It is scored
by one global NCC (`ultra_clear_text.cpp:1393`). A single-character edit
changes only a few cells, so the score barely moves. That matches the documented
108/108 false accepts for tracking/line. Live glyphs stay drawable for as long as the
80 ms probes keep matching. Stable scenes are re-read every 1500 ms
(`UctTiming`, `ultra_clear_text_controller.hpp:191-197`;
`FullReadNeeded`, controller `:1268`). From that cadence alone (not measured), a
missed edit can leave a stale replacement on screen until the next full read
supersedes it. Paused and Freeze frames are standalone and immutable, so they
need no change detection.

## 2. Main recommendations

1. **Character change:** add a native-resolution, jitter-tolerant, max-window
   current-ink verifier (CIV) alongside NCC. At first, use it only as an
   additional hide-and-reread trigger, which is strictly more conservative.
   It must not authorize reuse unless the owner later decides that it may.
2. **Curved pages:** use a per-line *ribbon* geometry inside the existing detector,
   rectifier and CTC chain, and emit locally straight word quads. Painter, proof,
   accessibility and protection then stay unchanged. Defer page meshes and
   learned dewarping.
3. Both changes keep the single TensorRT worker, context and stream with its two
   look-ahead slots, and the two analysis slots. There is no new worker, model,
   precision change or live/Freeze model switch.

## 3. Character-change detection

**CIV per accepted word**

- *Reference.* Build it in the worker's existing resident reference/color/ink stage.
  Sample the fitted word quad from the exact current single frame at native
  resolution (for smoothed OCR, use the paired current half). Cap the number of
  rows and the total memory budget. It stays GPU-resident and is never
  downloaded. The word's foreground/background colors are already measured and
  are kept with it.
- *Check.* Run it in the independent probe reader before the device candidate
  is released (on CPU, from `ProbeRequest.pixels`). Resample the same quad
  at the NCC-validated offset, then normalize both samples to ink fraction using
  the stored colors. Compute a symmetric tolerant residual: the minimum over
  ±1 px shifts, in both directions. This is a grayscale analogue of the Hausdorff
  distance's position tolerance [6]. Sum it over windows scaled to the word's
  x-height, and decide on the **maximum** window, never a mean, so that a single
  changed stroke dominates.
- *Verdicts.* Unchanged, changed, or unverifiable. Unverifiable covers weak
  reference detail, blur, scale change, occlusion and text out of view.
  Thresholds must come from measured null distributions with a noise floor taken
  from the paper pixels of the same comparison. None are proposed here.
- *Limit.* No pixel verifier, and no OCR, can see a change the camera does not
  resolve.

**Touchpoints**

- Add a resident ink-reference handle to `PreparedWord`/`AcceptedLine`.
- Add per-word verdicts to `LineReference`/`LineCheck` (`ultra_clear_text.hpp:633-643`).
- Add a CUDA kernel beside the existing matches in `uct_validation_kernels.cuh`, with a CPU oracle beside `MatchPatch`.
- Route "changed" through the existing line-removal path and `probeInvalidatedSinceSubmit_`.
- Add a fourth policy to `tests/uct_character_diagnostic.hpp`.

**Phases**

- **C0 (offline):** add CIV to the existing diagnostic, keeping the baseline table
  unchanged. Add held-out fonts, subpixel and scale jitter, blur and noise.
- **C1 (shadow):** behind an explicit switch and trace-only. Record CIV time,
  NCC/CIV disagreements and skips caused by busy slots.
- **C2 (conservative):** a "changed" verdict invalidates the line, so original ink
  returns, and requests a full read. This can only remove glyphs sooner.
- **C3 (owner decision):** use "unchanged" to carry proven words through motion or
  to defer reads. This remains blocked by the documented safety result.

**Alternatives**

- *Re-run only the recognizer on retained crops.* This is character-sensitive by
  construction, and its misses become ordinary ML errors. But recognition
  dominates cost: the documented dense medium-N8 recognizer stage takes
  125.95 ms of a 162.87 ms worker median, so skipping detection saves little. It
  fits best as a C3 confirmation for flagged lines.
- *Stricter NCC or smaller extents.* Already measured as unsafe: the best result
  still had 67/108 false accepts, with 108 or 32 false hides under jitter.
- *Learned siamese detector.* Needs a new model, training data and licence
  review. Not recommended.

## 4. Curved pages

**Recommended: per-line ribbon**

- *Detect.* In the existing component pass, bin pixels along the PCA axis and fit
  a low-order centreline c(u) and half-height h(u). Use integer atomics for
  CPU/GPU parity, as the moment sums already do. If the deviation from straight
  stays below a gate tied to line height, emit today's rectangle unchanged.
  Otherwise, also emit compact ribbon coefficients.
- *Rectify.* For curved lines only, replace `At` with
  `P(u,v) = c(u) + (v−½)·2h(u)·n(u)`. Keep the rectangle kernels as oracles.
  This sampling function *is* the backward map: every crop pixel comes from the
  original at an analytic coordinate, and recognized spans map back exactly by
  evaluating P. No dewarped intermediate image ever feeds proof, color, ink or
  paint. ABCNet's BezierAlign applies the same idea with Bézier boundaries [3].
- *Words.* CTC u-spans become word-chord quads on the ribbon, each with a
  measured sagitta. Within a bound expressed as a fraction of height, the
  existing oriented ink fit, `RefineWordSurface`, references, protection, painter
  and accessibility apply unchanged. Beyond that bound the word is marked invalid
  geometry and fails closed. A gate on radius of curvature against half-height
  keeps the sampling normals from crossing.
- *Also touched.*
  - Recognition windows (`PaddleRecognitionWindows` and CUDA `recognitionWindows`) must split along u.
  - `OrderLinesForReading` (`ultra_clear_text.cpp:4236`) clusters rows by
    bounding-box centre. Tall curved boxes can misorder lines, so it should use
    the centreline instead.
  - Foreshortening near a gutter remains uncorrected.

**Alternatives**

- *Model-free page surface,* such as a general cylinder fitted to many lines [7].
  It handles foreshortening, but it needs many visible lines and page structure,
  which magnified partial views often lack, and its failures are global.
- *Learned dewarp:* UVDoc, which ships in PaddleOCR's doc preprocessor [5].
  - Mapping: it predicts a coarse 45×31 backward grid from a 488×712 input and
    upsamples it bilinearly. It assumes the entire document is visible [4].
  - Vendor-reported cost (T4): 19.05 ms on GPU, 869.82 ms on CPU, 30.3 MB [5].
    These are not OkuFlow measurements.
  - What it would add: a model, licence, TensorRT and precision decision; a
    per-job inference on the single worker; an approximate map that can displace
    plates; and mesh painting. Defer.

**Phases**

- **G0:** build a model-free synthetic-warp fixture with an analytic ground-truth
  map, and measure word-geometry error in source pixels for today's geometry
  versus the ribbon. Collect private real gutter images.
- **G1:** ribbon detection, rectification and word-chord quads on CPU and CUDA,
  with straight-line outputs byte-identical.
- **G2:** review reading order and protection on curved lines. Today's protection
  masks are axis-aligned rectangles, and tilted menus already show
  over-suppression.
- **G3 (optional):** per-glyph quads for paint and accessibility, but only if long
  curved words stay unpainted.

## 5. What stays exact and what changes

**Unchanged:**

- Original pixels as the fallback.
- Stamps and provenance; zero image egress.
- Worker, context, stream and slot ownership.
- BF16 preference with FP32 fallback; tiers, batches and canvases.
- Thresholds: 0.70 confidence, 7 px, NCC 0.82/0.94.
- Live two-read agreement and the standalone Freeze.
- Full-read cadence and the Windows OCR path.
- Quad painting, and accessibility derived from actual paint.
- Straight-line outputs.

**Changed:**

- Curved-line detection metadata.
- Rectifier sampling, span geometry, recognition windows and reading order.
- Resident ink references and per-word verdicts.
- In C2: earlier hiding and extra reads.

## 6. Latency and cost risks

- **Ribbon.** The fitting and polynomial sampling per component are small. Arc
  length can widen crops and so add recognition windows (capped at 2–8). Lines
  that cross the gate may change their recognized text, so exact-output replays
  on the existing corpora are needed.
- **CIV.** It adds a kernel per probe, and each added kernel lengthens how long
  the candidate slot is held, which can mean skipped capture opportunities. The
  CPU fallback needs a budget, and the references cost memory. In C2, false hides
  mean flicker plus extra reads, which delay text elsewhere.
- **No timings yet.** None of this has been timed. The filtered surface kernel
  (0.21–0.27 ms warm) and the BF16 worker medians (34–275 ms) only set the scale
  of comparison.

## 7. Minimum useful experiments

1. CIV on the 12-edit corpus plus held-out fonts and jitter, on CPU and resident,
   reported beside the retained baseline.
2. The owner's real unchanged camera/phone pairs, for the false-hide null
   distribution. Optionally, scripted single-character slides through the lecture
   feed (needs a free desktop session).
3. The synthetic curved-line geometry fixture (model-free and desktop-free).
4. A GPU replay under an explicit grant: rectangle versus ribbon crops on the same
   detector output, for synthetic warps and real gutters. Record diagnostic text
   against truth, painted counts and ink outside plates, and confirm straight
   inputs stay byte-identical.
5. A shadow CIV trace inside the app.

## 8. Owner decisions

1. Priority, and the target content: book gutters, labels and cylinders, or both.
2. Curved words beyond the gate: omit them (as now), show them in the reading pane
   only, or fund G3.
3. The acceptable budget of false hides and extra reads for C2.
4. Whether C3 may ever be authorized, and on what evidence.
5. Whether any learned dewarp model is acceptable, given licence, payload and
   per-job latency.
6. Private test media stay in local-work-data.

## Sources

1. PaddleOCR 2.7 [`predict_system.py`](https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/release/2.7/tools/infer/predict_system.py), [`utility.py`](https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/release/2.7/tools/infer/utility.py)
2. Liao et al., DB, AAAI 2020, [arXiv:1911.08947](https://arxiv.org/abs/1911.08947)
3. Liu et al., ABCNet, CVPR 2020, [arXiv:2002.10200](https://arxiv.org/abs/2002.10200)
4. Verhoeven, Magne, Sorkine-Hornung, UVDoc, SIGGRAPH Asia 2023, [arXiv:2302.02887](https://arxiv.org/html/2302.02887v2)
5. PaddleOCR [doc preprocessor](https://www.paddleocr.ai/latest/en/version3.x/pipeline_usage/doc_preprocessor.html)
6. Huttenlocher, Klanderman, Rucklidge, TPAMI 15(9), 1993, [doi:10.1109/34.232073](https://ieeexplore.ieee.org/abstract/document/232073)
7. Meng et al., Metric Rectification of Curved Document Images, TPAMI 34(4), 2012 ([record](https://scholar.xjtu.edu.cn/en/publications/metric-rectification-of-curved-document-images/))
