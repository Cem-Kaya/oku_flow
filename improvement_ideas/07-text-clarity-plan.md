# Text Clarity Plan — 15 Proposed Features (2026-07-22)

## Implementation status

Implemented in the CUDA pipeline: items 1-12 and 15, including persistence,
Advanced controls, the Simple master toggle, the `Document` preset, and
focus-aware OCR gating. Item 13 now has the off-by-default
`OKUFLOW_ENABLE_TEXT_SR` integration boundary; a functional ML stage remains
blocked on selecting licensed weights/runtime and meeting the latency budget.
The original proposal contains no item 14, so there is no item 14 to
implement. The plan below is retained as the research/design record.

Research-grounded plan for the next imaging wave: making *text* maximally legible
from a phone camera, the way dedicated low-vision tools do. Context: ZoomText's
famous "xFont" re-renders *screen* fonts vectorially (not applicable to camera
pixels), but its user-facing controls — sharp, **bold**, condensed text at any
zoom — are the target experience. For camera input, the equivalent "black
magic" comes from document-image processing: local adaptive binarization
(Sauvola and successors), illumination correction, and stroke-aware filtering.
All items below are designed for the existing CUDA pipeline (device-resident,
no per-frame readback) and slot between stabilization/keystone and the display
color grade.

Priority key: ★★★ = do first, ★★ = second wave, ★ = later/stretch.

---

1. **★★★ Adaptive (Sauvola) binarization mode.** Per-pixel threshold from
   local mean/stddev (box-filtered stats or integral images on GPU) instead of
   today's single global threshold. This is the core magic of every document
   camera: crisp black/white text that survives shadows, lamp hotspots, and
   uneven projector brightness. Sauvola is the standard (used by OpenCV/
   scikit-image); window ~1/16 of width, k≈0.2–0.34 as the strength slider.

2. **★★★ Background flattening (illumination correction).** Estimate the
   low-frequency background (large-radius box/Gaussian of the luma, or
   morphological closing) and divide/subtract it out before any thresholding
   or contrast stage. Makes page shadows and screen glare gradients vanish.
   Cheap on GPU; pairs with #1 and improves every downstream stage.

3. **★★★ Smart sharpen: bilateral denoise + halo-clamped unsharp mask.**
   Edge-preserving smoothing first (kills camera sensor noise), then unsharp
   masking with an overshoot clamp so glyphs get crisp edges without white
   halos/ringing. Replaces "sharpen amplifies noise" with "sharpen text only".

4. **★★★ Stroke weight control — real "Make Text Bold".** GPU morphological
   dilation (or erosion for thinning) applied to the text-polarity channel,
   radius 1–3 px. The ZoomText Bold/Condense experience for camera text; the
   single most-requested control on CCTV magnifiers.

5. **★★ Text-polarity auto-detect.** Per-frame histogram skew decides
   dark-on-light vs light-on-dark, so #1/#4/#8 auto-orient. Removes a user
   decision; falls back to a manual toggle in Advanced.

6. **★★ True CLAHE local contrast.** Tile-based (e.g. 8×8) histogram
   equalization with clip limit, bilinearly blended between tiles. Stronger
   than the current global-percentile auto-contrast for mixed scenes (bright
   slide + dark room in the same frame).

7. **★★ Anti-aliased (soft) binarization.** Smoothstep around the local
   threshold instead of a hard cut — binarized text without staircase jaggies
   at high magnification. One extra parameter on #1's kernel.

8. **★★ Two-color reading mode.** Route the #1 binarized mask through the
   existing display-color schemes: ink→foreground color, paper→background
   (yellow-on-black etc.). This is exactly the classic CCTV-magnifier look and
   composes from stages we already have.

9. **★★ Temporal hysteresis for binarized text (anti-shimmer).** Per-pixel
   hysteresis using the previous frame's mask (different on/off thresholds) so
   letter edges don't flicker while the camera trembles. Text-mode counterpart
   of temporal smoothing; without it, binarization "boils" and is tiring to
   read.

10. **★★ Focus detection + refocus prompt.** Laplacian-variance sharpness
    metric on the small luma image (infrastructure exists from stabilization).
    When blurry: status hint "Image out of focus — tap your phone screen to
    refocus", and gate OCR/notes capture so blurry frames are never OCR'd.
    Phone autofocus hunting is a top real-classroom failure mode.

11. **★★ Stroke-masked selective sharpening.** Use #1's binarization
    confidence as a spatial mask so sharpening/thickening applies only near
    glyph edges — slide photos and diagrams stay natural while text pops.
    Solves the mixed-content-slide problem.

12. **★ Glare/specular suppression.** Detect blown highlights (laminated
    paper, glossy whiteboards), clamp with a local tone-map or fill from
    neighborhood luma. Bounded scope: suppress, don't inpaint.

13. **★ ML text super-resolution (stretch).** A small quantized SR network
    (ESPCN-class or a text-specialized model) run on the zoomed region only,
    as an alternative to NIS/FSR at high magnification. Real "black magic" but
    heavy: needs TensorRT/cuDNN integration and a latency budget; prototype
    behind a build flag, benchmark against FSR+#3 before committing.

14. *(Removed by project owner — reading-ruler/typoscope overlay was proposed
    here and deliberately cut. Do not implement.)*

15. **★★★ "Document" preset + Auto Text Clarity master toggle.** New built-in
    quick mode bundling #1+#2+#3+#5+#7+#9 for paper/handout reading, and a
    single "Text Clarity" master switch in Simple mode that picks the right
    stack from simple scene stats (slide vs paper vs board). Ships the magic
    without asking the user to understand any of it.

## Suggested implementation order

Wave 1 (one CUDA agent + integrator, same playbook): #1, #2, #3, #4, #15
(preset only for the implemented stages). Wave 2: #5–#9, #11, extend #15's
auto logic. Wave 3: #10, #12. #13 is a separate research spike.

## Sources

- Sauvola & Pietikäinen, "Adaptive Document Image Binarization" (Pattern
  Recognition 33) — the standard local-threshold method.
- ISauvola / modified-Sauvola literature (dynamic windows, contrast-aware) for
  the strength-slider variants.
- SauvolaNet (learned Sauvola) — background for the ML direction (#13).
- ZoomText marketing/docs — xFont sharp/bold/condense controls define the
  target UX for #3/#4.
