# UCT stacked-fraction / superscript review (Opus, read-only)

Checkout: `C:/Users/USER/.t3/worktrees/oku_flow/feature-uct-recap-player`,
branch `fix/uct-symbol-coverage`, HEAD `fccc7f8181dfb27b6fee42bc04213918a86c8085`
(clean). Nothing was edited, built, run or inferred. Only source code and the
archived evidence named below were read.

## 1. Bottom line

The current pipeline gives a span of ink only two possible outcomes: a font glyph
drawn from recognized text, or the untouched original. In this frame,
`½ m v²` fails to become trustworthy text for structural reasons (§2). That leaves
the original blurry ink as the only outcome. Neither a threshold change, the PR16
character relaxation nor a ribbon-classification repair can sharpen the fraction
and exponent without inventing or dropping structure.

**Recommendation: first implement a third outcome, a "sharpened ink sketch".**
On the worker, take the protected original pixels and extract a sub-pixel vector
contour of the actual ink, gated by a topology-stability proof. The painter then
fills that contour with the existing high-contrast plate and foreground colors.
Reprojection uses the same affine scene-to-view transform, so the strokes stay
crisp at every magnification. The sketch is not text: it adds no reading text,
UIA text or painted-word count, and the reading pane keeps `…`. It keeps the real
topology (numerator, bar, denominator, raised exponent) because it **is** the
source ink, redrawn sharply. It does not depend on the model reading the formula
correctly.

**Do not ship a ribbon reclassification first.** In this evidence the
"unsupported ribbon" misclassification is currently the only thing that stops
`K=mv2` being painted over the ½ (§2.3).

## 2. Exact causes (with evidence)

### 2.1 The recognizer is 1D CTC and collapses 2D notation (model limit)

- Full frame, original source (`…174454…/paint/block-0-source-0/sample.json`,
  job `0104…warmup.json`): one detector box spans the whole formula. CTC returns
  `K=mv2` (0.832). The ½ is dropped and the exponent becomes a baseline `2`.
  The video frame gives the same text (0.815, `fromRibbon:true`).
- x1 crop diagnostic (`…172536…/paint/block-0-source-0/sample.json`): the
  detector splits the formula into three boxes. The fraction box `[162,73,38,44]`
  is read as **`21` at 0.972**, a confident misread of a vertical stack.
  `mv²` (0.883) is read correctly as a literal `²`.
- So no confidence threshold, height rule or character rule can recover `½`
  from this model's output for the whole-formula box. PR16 only lets literal
  `²`/`½`/`△` through *when the model emits them*.

### 2.2 Envelope-centre ribbon fit misclassifies 2D notation as a curve (geometry bug, currently protective)

`include/okuflow/common/uct_ribbon.hpp` `Fit()` fits a quadratic to the per-bin
envelope **centre** `(low+high)/2` of the DB component. The fraction raises the
top and lowers the bottom over a few bins. The superscript raises only the top.
Both move the centre, which produces `|c|/4 ≥ max(.75, 8% h)`. The fit then sets
`unsupported=true` and fails the residual check (`> max(1.5, 15% h)`). Every word
in the box becomes `kInvalidGeometry` and protects the full envelope
(`uct_paddle_processing.cpp` ≈ lines 601–622 and 844–855; the GPU path uses the
same `Fit` at `uct_kernels.cu:362`).

Evidence:
- Job profile: `ribbonRecognitionCrops: 0`, `unsupportedRibbonRecognitionCrops: 1`.
  The K=mv2 word has `ribbonErrorPx: -1` and `protectedBounds [289.9,301.7,343.8,134.3]`.
- x1 crop: `mv²` is an unsupported curve (the exponent lifts the top envelope).
  `K=` is fitted as a *valid* curve and then refused at chord error 3.85 px. That
  is a straight two-glyph word: the tall `K` next to a short `=` produces false
  curvature.

So sparse handwriting, digits next to operators and super/subscripts do trigger
curve hypotheses on straight text. In this formula that misclassification
happens to give the safe outcome.

### 2.3 Turning the misclassification off would paint destructive text (proven)

Rectangle control `…172114…/paint/block-0-source-0/sample.json`
(`ribbon:false`): `K=mv2` was prepared with disposition 0 at bounds
`[316,328,292,85]` and **`drawn: true`**. That box covers the ½, so the painter
replaced the fraction with a font string that contains no fraction. In the x1
crop, `21` (disposition 0, 0.972) was stopped only by its neighbours'
`protected_ink`. Any change that frees `mv²`/`K=` would also let `21` be painted
over the ½.

**Consequence:** a ribbon-classification repair on its own turns "blurry but
correct" into "crisp but wrong", and drops a factor of ½ from a physics formula.
It must not ship before a structure guard (§5.1).

### 2.4 Owner screenshot

`…c7f3a209….png` shows `K=` painted as a glyph while `½ m v²` stays original. No
receipt exists for that live run. The most likely explanation (inference, not
verified) is that the magnified live view's detector produced a separate `K=` box
(as in the x1 crop), while the remaining span stayed protected. Either way the
½mv² span reaches the painter as protected original ink. That is exactly the
input the ink sketch would sharpen.

## 3. Direction to implement first: protected-ink vector sketch

### 3.1 Behaviour

For each eligible protected span, the worker builds an **InkSketch** from the
exact current analysis pixels. It is closed sub-pixel polygons in scene
coordinates plus fill rule and colors, or a typed refusal. When the sketch's line
is drawable under the *same* admission and freshness decision as glyphs, the
painter does the following, clipped to the visible clip:

1. Fill the sketch region with the plate (background) color.
2. Fill the contour path with the glyph foreground color, with antialiasing.

If the sketch is refused, missing, stale or the line is not admitted, nothing
changes: the original ink is shown exactly as today.

### 3.2 Eligibility (initial, conservative)

- Spans whose words are `kInvalidGeometry`, `kLowConfidence` or `kUnrecognized`.
  Use the merged `protectedBoundsPx` envelope per line, union of overlapping
  spans, capped at 8 regions per job.
- Exclude `kTooSmall`. Require region height ≥ 12 analysis px and an estimated
  stroke width ≥ 1.5 px. Contours of 5-px glyphs are blobs and would mislead.
- Region size: at most 512×256 and at most 65,536 samples per region,
  131,072 per job. If a region is oversize, **refuse; never shrink or decimate**,
  which mirrors the current-ink oracle's rule.
- Only `SingleFrame` / exact-held acquisitions, using validation (exact current)
  pixels. Temporal Smooth uses `ValidationData()`/`ReadValidation`, never the EMA.
  Composite or unknown acquisitions never produce a sketch.

### 3.3 Algorithm (one shared, integer-exact implementation)

New header `include/okuflow/common/uct_ink_sketch.hpp`. Mark it HD-capable like
`uct_ribbon.hpp` and add a CPU implementation file.

1. **Coverage** per pixel, integer only. Take `d = fg − bg` (8-bit RGB from the
   measured word/line colors) and `n = d·d`. Require `sqrt(n) ≥` a minimum
   contrast, otherwise refuse with `LowContrast`. Then
   `q = clamp(round_half_up(255·(p−bg)·d / n), 0, 255)` using int64. This is the
   *only* per-pixel step that the GPU also performs, so CPU and GPU produce
   **byte-identical** grids by construction.
2. **Levels** from a 256-bin histogram of `q` in the region:
   `bgL` = 20th percentile, `inkL` = 98th percentile. Require
   `inkL − bgL ≥ 64`. Then `τ = (bgL+inkL)/2`, `τlow = bgL + 0.3·(inkL−bgL)` and
   `δ = 0.1·(inkL−bgL)`.
3. **Topology proof.** All mask comparisons use 8-connected ink and 4-connected
   background. Every failure is a typed refusal that keeps the original ink.
   - *No vanished strokes:* every component of `q ≥ τlow` with area ≥ 4 contains
     a pixel `≥ τ`.
   - *Threshold stability:* the component count and hole count (Euler) of the
     masks at `τ−δ`, `τ` and `τ+δ` are identical. This blocks loops that close or
     strokes that merge only at the chosen cut.
   - *Closed region:* `q ≥ τlow` has no pixel on the region border unless that
     border is the image border. A stroke continuing outside would be cut by the
     plate, so refuse (`BorderInk`). One bounded expansion to the line envelope
     is allowed before refusing.
   - *Texture:* components at `τ` with area < 3 **and** peak < `τ+δ` are
     treated as board texture and dropped. Small but strong dots (decimal
     points, i-dots) are kept.
   - *Bounds:* refuse if there are more than 64 components, more than 16,384
     vertices per job, or any value is nonfinite.
4. **Contour:** marching squares on `q` at `τ`, with a fixed saddle rule (cell
   integer mean vs `τ`). Edge crossings are stored as fixed point,
   `cell + ((τ−q0)·256)/(q1−q0)` with integer division, so loops are identical
   on every backend. Orient outer loops CCW and holes CW, and use the winding
   fill rule. One optional Chaikin pass is permitted only if root measures it.
   Its displacement is ≤ ¼ segment and it must re-check that no loop
   self-intersects. Not required for the first PR.
5. **Map** vertices analysis→scene once, through the job's stamped view mapping
   (the same mapping `PrepareLines` uses for quads). The painter later applies
   only the current affine scene→view transform. This adds no rotation, keystone
   or stabilization: the analysis image is already post-geometry, as documented
   in `uct_ribbon_and_current_ink.md`.

### 3.4 Touchpoints

| Area | File | Change |
|---|---|---|
| Algorithm | new `include/okuflow/common/uct_ink_sketch.hpp` (+ `src/common/uct_ink_sketch.cpp`) | integer coverage, levels, proof, marching squares, typed refusals and stats |
| Data | `include/okuflow/common/ultra_clear_text.hpp` | `InkSketch` in analysis px on `RecognitionResult`/line; scene-space sketches on `PreparedLine`; a carrier on the model/overlay scene structs (`uct::SceneWord` is per word, and a protected-only line may have no drawable word, so add a separate `SceneSketch` list rather than overloading `SceneWord`) |
| CPU worker | `src/common/ultra_clear_text.cpp`, the non-resident block after `MeasureLineColors` (≈2321–2366) | select regions from final dispositions after `FinalizeRecognizedGeometry`; build from `*referencePixels` (validation) |
| Resident worker | `src/cuda/uct_device_analysis.hpp` `resident::Prepare` + new kernel in `src/cuda/uct_kernels.cu` | one per-pixel coverage kernel over the region list from `ValidationData()`, returned through the existing `CopyResult` metadata copy, then the same host proof/contour |
| Preparation | `PrepareLines` | map loops to scene; attach to the line; a refused sketch carries only its reason |
| Admission/model | overlay model where `protectedSceneRects` are assembled | the sketch follows the line's existing admitted/withdrawn state and validated offset exactly; no new admission path |
| Painter | `src/ui/ultra_clear_text_overlay.cpp` `ComputePaintPlan` / `PaintWords` | build `QPainterPath` at assignment time, never per paint; plan sketch drawability once; paint after word plates and before glyphs, clipped to `visibleClip ∩ sketchView`; never through `paintClip`, which excludes protected ink by design; record a sketch receipt (`drawn` / reason) |
| Accessibility/reading | screen-reader projection | **no change**; add assertions that sketches add no tokens, text or positions |
| Diagnostics | `tests/uct_resident_diagnostic.hpp` paint export | `sketches[]` in `sample.json`: region, decision/refusal, C/H at τ±δ, vertex count, coverage-grid SHA256 |
| Docs | new `docs/uct_ink_sketch.md`, `docs/code_reference.md`, `docs/uct_token_policy_and_detail.md` cross-link, `CHANGELOG.md`, AGENTS addendum | include the "not recognition" wording |

Diagnostic control: `OKUFLOW_UCT_INK_SKETCH=0` (unset/1 = on), mirroring the
ribbon's A/B switch. It needs no UI setting or saved preference, and it is not a
live/Freeze model switch.

### 3.5 Safety, geometry and reprojection

- **Pixels are never invented.** Every filled pixel lies inside a stable
  iso-contour of the actual current ink. Every ink component visible at `τlow` is
  redrawn, and anything else under the plate fails the border/texture/vanishing
  checks instead. The proof refuses rather than guesses.
- **Freshness:** the sketch uses the same line reference, registration offset,
  source/geometry stamp and withdrawal rules as glyphs. Motion, geometry
  generation change, source change and composite input all remove it. Freeze and
  paused paths behave as immutable selected frames.
- **No text exposure:** no `ScreenToken`, no UIA positioned text, no reading-pane
  change and no "painted words" count. Use a separate `sketchedRegions` counter.
- **Geometry/proof unchanged:** no change to `Fit`, chord limits, protection
  inheritance, confidence 0.7, minimum height 5 or source proof. Protected spans
  stay non-carvable for glyphs. Only the sketch may draw inside them.
- **Cost:** one coverage pass over ≤ 131k samples plus CPU contouring of small
  grids on the existing worker. There is no second worker, no synchronous
  readback and no presenter interaction. Painting fills cached paths under the
  existing deadline.
- **Transfer contract.** `uct_resident_diagnostic.hpp` meters only *full-image*
  egress (`Readback`/`ReadValidation`/`ReadProbe`), and that stays zero. The
  bounded per-region 8-bit coverage grid is new pixel-derived metadata. Document
  it, meter it separately (`inkSketchBytes`) and cap it. If root or the owner
  reads "zero image egress" as forbidding any raster, the alternative is to also
  run marching squares on the GPU and copy back only fixed-capacity edge
  segments, using fixed-order compaction with no atomics. That is more code for
  the same output.

### 3.6 CPU/CUDA parity

- The coverage grid must be **byte-equal**. It is integer math on 8-bit inputs
  with no float, so assert this with `memcmp` in the CUDA contract.
- Proof and contour are the same host code on both backends, so loops must be
  **vertex-equal**. Assert exact equality, not a tolerance.
- Region selection depends on final dispositions. Those already come from shared
  `Fit` and CTC code, with the documented fit-coefficient rounding tolerance. The
  parity test should therefore feed both backends the *same* region list.

## 4. Frozen-image tests and measurements

**Synthetic, analytic truth** (new rows in `uct_ribbon_ink_contract` or a sibling
offscreen contract). Draw polylines of known width, not fonts:

- numerator / bar / denominator; a small raised mark; a two-arch "m" (0 holes);
  an "o" (1 hole);
- blur σ ∈ {1.0, 1.5}, seeded chalk texture, two downscales.

Assert:
- component and hole counts equal the truth;
- sketch boundary Hausdorff ≤ 0.75 analysis px from the true stroke edges;
- the raised mark's bounding box stays above the baseline band;
- the bar stays between numerator and denominator (no flattening).

**Refusal rows**, each with an exact typed reason: low contrast; stroke crossing
the region border; faint stroke (peak between `τlow` and `τ`); unstable
threshold; oversize region (refused, not shrunk); temporal-smooth input whose EMA
differs from the current frame (the sketch must match current pixels or refuse).

**Painter/accessibility:**
- offscreen snapshot pixels show plate+fill only inside the sketch region;
- glyph words are unchanged;
- reading text is identical, including `…`;
- Qt positioned-text count is identical;
- a moved or withdrawn line removes the sketch;
- a chrome-excluded region is never covered.

**CPU/CUDA:** byte-equal coverage and vertex-equal loops for the synthetic
fixtures and for the real equation region of both frozen images.

**Real frozen images**, using the existing BF16 production-factory resident
diagnostic with paint samples and warm plans only:
`generated-board.png` (2a92…) and `board-video-frame-0.png` (5033…).
Record, without asserting success in advance:
- the sketch decision and reason for the `K=mv2` envelope;
- C/H at `τ±δ`;
- vertex count;
- painted-word count (expected unchanged at 8);
- reading text (expected unchanged);
- crops of `uct.png` at 1×, 4× and 8× view scale.

**Sharpness measurement:** median 10–90% edge-rise width in screen px along
~200 sampled contour normals, painted vs original, at each scale.

**Topology check:** components of the binarized painted region equal the
source-derived C. Derive this from the source grid, **never** from a hardcoded
"K=½mv²" truth.

**Worker cost:** sketch on vs off using the same binary, same images and visit
order ABBA, with steady samples as in the existing ribbon tables.

**Preserve** the x1/x4/x8 crop failures and the 172114/172536 receipts
unchanged. Running the sketch on x1/x4 is an extra diagnostic only. The "zero
Candidate" refusal remains a refusal.

**Honest outcome language:** if chalk texture or blur causes a refusal, report
that refusal. Do not lower `δ`, the texture limit or the contrast floor merely to
obtain a sketch.

## 5. Follow-ups (ordered, separate PRs)

### 5.1 Structure guard, then ribbon discriminator (current model)

1. **Structure guard before any text paint.** Run the sketch's component analysis
   on *candidate* words whose detector envelope shows excursions. Flag layouts
   that a 1D CTC string cannot represent:
   - stacked (two components with >50% x-overlap, vertically disjoint, each
     ≥25% line height, optionally a bar between them);
   - raised (a component wholly above the median band centre and <60% of the
     median component height).

   A flagged word becomes protected and is sketched, **unless** its recognized
   text already contains the matching literal (`½`/`²`…) and the component count
   agrees. This also covers the `21` hazard and the ribbon-off `K=mv2` paint.
2. **Only then** change `Fit`. Fit `low` and `high` envelopes separately (the
   same 3×3 normal equations with `v=low`/`v=high`). Accept a curve hypothesis
   only when both boundaries curve with the same sign and comparable magnitude
   (e.g. `|c_low−c_high| ≤ 0.5·max(|c_low|,|c_high|)`) and per-bin height CV is
   ≤ 0.35. Otherwise return `{}` (rectangle). This is header code shared by CPU
   and `uct_kernels.cu`, so parity stays automatic. Keep the existing
   bin-order/rounding tests.

   This would let `K=` (x1 false curve) and a correctly read `mv²` paint as text,
   with the guard keeping fractions sketched. Re-run the frozen merge replay; the
   straight-path output must stay byte-identical.

### 5.2 Per-component recognition (current model, optional)

Split a protected envelope by sketch components into horizontal runs and stacks.
Recognize each sub-box as an extra crop in the same N8 batch on the same worker.
Paint numerator and denominator at their own positions, with the bar as a line
and the exponent small and raised. This is achievable with the existing model, but
isolated-glyph reads (`1`/`l`/`|`) are weak and grouping is heuristic. Every
sub-box that fails falls back to its sketch. This is not required for the owner's
visible-sharpness goal.

### 5.3 Math OCR model (separate dependency)

A formula recognizer emitting LaTeX plus a 2D renderer would add:
- a new model and license review in `THIRD_PARTY_LICENSES.md`;
- TRT preparation and approval;
- autoregressive decode latency on the single worker.

It is the only route to semantic `½mv²` in the reading pane and screen reader.
Keep it out of the current work. The sketch remains its fallback either way.

## 6. Explicitly not recommended

- Turning off or relaxing the ribbon, the chord limit or protection inheritance
  as a "fix". §2.3 shows it paints `K=mv2` over the ½.
- Lowering confidence or height gates, or adding string-specific handling of
  `K=mv2`/`½mv²`.
- Raster upscaling or unsharp masking of the original in the overlay. It stays
  blurry at 8× and can halo. The vector iso-contour is what gives crisp edges at
  any zoom.
- Presenter-shader thresholding: it would couple UCT region masks into the D3D12
  present path.
- Any claim that the formula is "recognized". The sketch is sharpened original
  ink only.
