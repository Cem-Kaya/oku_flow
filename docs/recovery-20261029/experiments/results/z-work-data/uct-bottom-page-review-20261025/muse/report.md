# Bottom-of-page UCT review — Muse (read-only)

- Frozen HEAD: `b4930956949d08bafcd88e76124a88f169350a68` (`feat: apply curved OCR to live and advancing jobs`), clean tree verified before and after.
- Scope obeyed: no code edits, builds, tests, inference, GPU/native desktop, network, or extra agents. Only source reading plus offline JSON arithmetic in this directory (`01`–`08` scripts + `*_out.txt`).
- Docs read first: `AGENTS.md` end-to-end, `README.md`, `docs/uct_ribbon_and_current_ink.md`, `docs/ultra_clear_text.md`, `docs/uct_recognizer_width.md`, `docs/uct_tilted_paint_gaps.md`, `docs/uct_coverage_stability_repair.md`, `docs/uct_surface_perspective.md`, token/detail, oriented-detail, uncertainty notes, `docs/code_reference.md` (UCT sections), index entries in `docs/README.md` / `docs/developer_guide.md`, and prior review `uct-ribbon-ink-20261024/t3-b137fab1/review-notes.md`.
- Supplied images inspected with the image reader: `...-081277c5-...png` is the bottom closeup (rectangle left = no boxes; curved right = sparse cyan boxes, merged/wrong text); `...-e2aea05c-...png` is the two-page overview. Also viewed the evidence `original.png` and ribbon-ON `uct.png` for book source 2.
- Evidence hashes re-verified offline: both book arms `original.png`/`uct.png`/`jobFile` SHA-256 match `sample.json` (`07_out.txt`).

## Top finding

**Bottom lines are not missing from detection or recognition. On the ribbon-ON book job, essentially every bottom word has a CTC candidate; the remaining loss is (a) clipped/merged recognition at component ends, (b) the absolute 7-source-pixel `kTooSmall` gate killing correctly recognized foreshortened words, and (c) admitted words left undrawn by the painter. The ribbon chord-error gate — the most "narrow assumption" on paper — is empirically non-binding on this page (max word sagitta 0.46 px, no `kInvalidGeometry`, no `-1` errors, zero unsupported crops). Relaxing chord/safety thresholds would manufacture almost no bottom coverage; fixing endpoint sampling, size handling, and axis-aligned protection geometry would.**

Numbers below are the actual BF16 warmup jobs behind the gallery (OFF `...201314...` job 0096, ON `...201337...` job 0095; book 1120×800), funnelled candidate → admitted (`SceneWords`) → drawn. This is standalone immutable admission + offscreen snapshot (`tests/uct_resident_diagnostic.hpp:1756`), not live temporal proof; live can only be stricter (see Limitations).

## What the images actually show (reconciled with JSON)

- Closeup left (rectangle): zero painted words in the last four left-page lines. JSON: OFF admitted 25 / drawn 4 at y 560–640, admitted 1 / drawn 0 at y 640–800 (`03_out.txt`). The words exist as garbled candidates (`translatio`, `et,`, `ony`, `farme`, heights 34–42 px) but do not paint.
- Closeup right (curved): sparse boxes with visible errors — `painfullyacura` (merged + truncated), `attand`, `popolar(i)`, `he`/`th`, clipped line starts (`sion`, `lian`, `road`). JSON: ON admitted 70 / drawn 56 at y 560–640 and 9 / 7 at y 640–800. The closeup crops the left page only; many drawn bottom words are on the right page or outside the crop, so "sparse" overstates the remaining gap. The real remaining left-bottom gap is specific words, enumerated below.
- ON `uct.png` (full page): bottom paragraph is still mostly original thin serif with isolated bold replacements, plus defects visible even mid-page: spaceless painted joins (`thoughinitself`), a floating `a` above its baseline, black-square glyphs. Coverage counts alone overstate bottom quality; placement of the words that do paint also needs the experiment's eye check.

## End-to-end trace (confirmed facts, with stage ownership)

Chain (unchanged by anything proposed here): `H` once → analysis `A` → detector region → ribbon `P(u,v)`/rectify → CTC `u` spans → source words → inverse `A` once (`PrepareLines`) → present once. References/proof/ink fitting sample original analysis pixels (`docs/uct_ribbon_and_current_ink.md:97-126`).

### 1. Detector (low-res connectivity / map / crop) — bottom lines ARE found

- Book 1120×800 yields one whole-scene region: `DetectorRegions` (`src/common/ultra_clear_text.cpp:1181`, desiredEdge 1280, overlap 160) returns `{whole}`; detector input 960×672 (`DetectorInputSize:1163`, scale 0.857, 32-aligned). Bottom ink height ≈6–7 analysis px ⇒ ≈5–6 detector px; line pitch ≈20 px ⇒ ≈17 detector px.
- GPU `Detect` (`src/cuda/uct_kernels.cu:52` threshold `>.2f`, union-find 8-connectivity `:86`, PCA `:121`, score/expand `:171`, first-128 raster order `:231`) mirrors CPU `PaddleDetect` (`src/common/uct_paddle_processing.cpp:506`: 8-connect, min 9 px, `w,h>=3`, mean score `>=.4`, unclip `expand=w*h*1.4/(2(w+h))`).
- Both arms: `detectorPasses=1`, `recognizerSamples=47`, `recognizerCalls=6`. Same crop count; candidates 373 → 449. The ribbon does not add detections — it re-samples the same components so CTC segments them into more, cleaner words. `ribbonRecognitionCrops=27/47`, `unsupportedRibbonRecognitionCrops=0` ON.
- OFF bottom failure mode is visible in candidate geometry: bounds heights 30–47 px (two-line-tall rectangle envelopes), bottom confidence median 0.701 vs 0.998 mid-page; ON bounds-h p90 falls 23.2 → 12.9 and bottom confidence median rises to 0.971 (`05_out.txt`, `06_out.txt`). Rectangle crops of curved lines feed the recognizer tilted double-height strips; that, not detector absence, was the original bottom collapse.
- Residual detector-level loss is narrow and specific: the gutter dead zone. Left-page line ends truncate at x≈517–524 and right-page starts at x≈562; final left word `which` (after `for`, x≈525+) has no candidate at all. Gutter shadow + steep foreshortening plausibly splits/weakens components there (hypothesis H5 below; no detector map is exported, so component-level cause is unproven).

### 2. Ribbon support / fit / height / endpoint — fit succeeds; endpoints clip

- Fit: 24 bins, ≥20 occupied incl. both ends, length ≥24 detector px, meanHeight ≥3, length ≥2·height, slope/residual/height-stability refusals (`include/okuflow/common/uct_ribbon.hpp:87-150`); GPU one 256-thread block/component (`uct_kernels.cu:307`). Straightness gate `|c|/4 < max(.75 det px, .08·meanHeight)` keeps rectangles (`uct_ribbon.hpp:125`).
- On book ON: every bottom line fit `valid` (304/454 words `fromRibbon`, all bottom lines 30–35 fully ribbon). No refusal path exercised.
- **Endpoint asymmetry (confirmed by construction + symptom pattern):** rectangle quads are expanded by `expand` on all sides before rectification (`uct_paddle_processing.cpp:592,604-607`; GPU `uct_kernels.cu:207,277`), but the ribbon's `t∈[0,1]` spans exactly the *unexpanded* component extents `u0..u1` (`Fit` bins normalized `(u-u0)/(u1-u0)`, `ox=u0·…`, `ux=length·…`). Vertical padding is restored (`h0 += padding + maxResidual`, `uct_ribbon.hpp:141-142`); along-track padding is not. For a bottom line (`w≈274, h≈10` detector px) `expand≈6.8` detector px ≈ 7.9 analysis px ≈ 1–2 glyphs per end.
- Symptoms match at component ends repeatedly: `ranslation` (missing `t`), `popolari` (missing `ty`), `peo` (for `poetical`), `mediu` (missing `m`), `n` for `in` at a right-page line start, `painfullyacura` (missing `te`), dense URL `r-tide-…` (missing `embe`). Mid-line words do not show this pattern.
- Height model side effect: `h0` absorbs `maxResidual`, so worse centerline fit ⇒ taller crop ⇒ more neighbour ink in the tensor and taller protections. Bottom `inkSearch`/`protected` heights median ≈24 px vs line pitch ≈20 px (reading-point medians L33 613.7, L34 634.3, L35 653.8; `08_out.txt`). Protections are taller than the line spacing they must coexist with.

### 3. CTC span, word geometry, chord constraint — non-binding here

- Decode maps CTC spans in `u`, builds chord quads, refuses if `Error(searchSpan) > min(1px, .08·QuadHeight)` (`uct_paddle_processing.cpp:718-755`). Search span = word ±1 CTC step, neighbour-clipped to gap midpoints (`:777-778,831-833`). Unsupported fits mark all words `kInvalidGeometry`, `ribbonErrorPx=-1`, full envelope (`:844-853`).
- Book ON: `ribbonErrorPx` median 0.0054, p90 0.068, max 0.462 (`accompanying`, ratio ≈0.46 of its 1 px limit using AABB-height proxy; short bottom words ≤0.02 vs limits ≈0.5). Zero `kInvalidGeometry` (disposition 4), zero `-1`. **The chord gate rejects nothing on this page.** Search-span errors are not exported, but every bottom word still produced a candidate, so the search gate also passed wherever a word exists.
- Recognition errors that remain are segmentation/spelling, accepted as normal ML error: merges `painfullyacura`, `butabroad,`, `literaloyalty`, `itsintegrity`; splits/substitutions `attand`+`reat` (attained great), `embodic`, `athor,`, `tb`, `th`. Merges are consistent with along-track foreshortening compressing inter-word gaps in PCA-`t` sampling (the design explicitly does not correct gutter foreshortening, `docs/uct_ribbon_and_current_ink.md:55`) — hypothesis H4, not proven by JSON alone.

### 4. Confidence disposition — small, clean, bottom-concentrated in OFF only

- `disposition=1 (kLowConfidence)` ⇔ mean confidence `<.7` exactly, both arms (40/40 words; decode `uct_paddle_processing.cpp:790`, rechecked `ultra_clear_text.cpp:1603`). OFF: 35 (25 of them at y 560–640). ON: 5 total, 4 at bottom: `0` (0.360), `[vi]` (0.690), `peo` (0.522), `is` (0.610), `orgial,` (0.657) — all genuinely garbled/truncated. Keeping the .7 gate loses no correct bottom word in this sample. Do not relax it for coverage.

### 5. Ink fitting — mostly fixed by ribbons; a tall-quad residue remains

- `FitOrientedWordInk` (`ultra_clear_text.cpp:589`) refuses (returns padded quad) when ink mass `<8` samples (`:672`), main row-run `<55%` of ink (`:683`), fitted height `<max(4, .25·searchHeight)` (`:709`), or ambiguous interior ink (`:662`). Scheduler applies it worker-side to candidate padded words (`:2242-2248`; resident GPU equivalent via `PrepareResidentAnalysis:2203`), then `FinalizeRecognizedGeometry:745`, then `PrepareLines:2264`. JSON `bounds` are post-fit.
- Unshrunk words (`boundsH ≈ inkSearchH`): OFF bottom 34/52 (65%) vs ON bottom 8/91 (9%). The ON residue — `an` h22.5, `at` h23.2, `a` h23.7, `n` h25.2, plus low-confidence `[vi]/peo/is/orgial,` h24–26 — keeps full-height quads. In a straight chord frame, a curved word's ink band itself curves across rows, and converging neighbour ink enters the band; the 55% main-run rule is the likely refusal (hypothesis H2; refusal reason is not exported).

### 6. `PrepareLines` size gate — the largest *correct-word* bottom loss

- `textHeight = QuadHeight(quad)` for ribbon words; `< minimumWordHeightPx (7.0)` ⇒ `kTooSmall` (`ultra_clear_text.cpp:1597,1606-1607`; default `ModelTuning::minimumWordHeightPx{7.0}` `:842`, scheduler `:534`). Admitted `sceneBounds` height minimum is exactly 7.00 px in both arms (`06_out.txt`).
- ON candidate→admitted loss = 61 words page-wide. The 10 bottom losses (y-center ≥540) are almost all *correct, high-confidence, tiny* words: `to` 1.000, `a` 1.000, `a` 1.000, `text,` 0.976, `once` 0.973, `not` 0.997, `to` 0.977, `of` 0.973 (AABB h 6.0–7.6; true `QuadHeight` smaller under slope), plus garbled `tb`, `the` 0.762. Foreshortening shrinks true ink to ≈5–6 px at the page foot while mid-page equivalents pass. This is an absolute-pixel assumption applied to perspective-scaled text.
- Note the product mismatch: the painter's own minimum is *projected* 6 px (`kMinProjectedHeight`, `ultra_clear_text_overlay.cpp:31,638`). A 6-scene-px word is 24 view px at 4× zoom in a magnifier, yet `kTooSmall` removes it before any zoom. The 7 px rule mixes "too small to trust/prove" with "too small to draw"; only the first is a safety property, and proof (reference/NCC) is evaluated separately downstream.

### 7. Merge / retained protection — no bottom effect in this sample; do not regress

- Overlap replacement inherits protection only for `kInvalidGeometry && (fromRibbon || ribbonErrorPx<0)` (`uct_paddle_processing.cpp:320-324,365-373,439-445`) — the narrowed rule from the prior review. Book has no such words, so merge inheritance neither helps nor harms bottom here. Broadening it again would re-break the frozen rectangle oracle; explicitly out of scope for every fix below.
- Cross-gutter row grouping is real: prepared lines 15–19, 22, 24–28, 31 span x≈205–890 (both pages one "line"), via `rowDistance` (`:276-314`: comparable height, `dy<=.35·large`, `gap<=4·small`) plus `preferredLine` chaining through overlapping windows (`:364,376`). Consequences: one reference patch and one layout median across the gutter, interleaved reading order. It does not change candidate counts, but it couples proof/layout across two differently-sloped surfaces (hypothesis H6 for some paint/layout oddities, e.g. floating `a`).

### 8. Admission → paint — 16 bottom words admitted but undrawn; sub-cause not exported

- Sample admission is standalone: `glyphEligible = standalone && line.reference.valid` (`ultra_clear_text.cpp:3716`); non-candidates become ordered omissions + `protectedSceneRects` (`:3693-3708`, non-carvable only for invalid-geometry/high-risk). Protection carving by a proved quad requires the quad to cover the *full rectangular* cut (`:3095-3098`) — a sloped ribbon quad almost never does, so carving fails whole for curved words overlapping any protection.
- Painter: glyph layout shares a median type size per oriented line (`ultra_clear_text_overlay.cpp:455-491`); `ResolveOverlaps` marks collision when quad intersection `>=0.2·smaller` (`:545-580`); `ComputePaintPlan` rasterizes AABB protections, then iteratively suppresses any drawn word touching protection and adds its own polygon to protection (≤8 passes, else all suppressed) (`:677-785`).
- ON admitted-not-drawn bottom (16): tall-quad group (`an`, `n`, `itsintegrity`, `at`, `a`) and normal-height group (`it`, `was`, `what"`, `accepted`, `reat`, `only`, `popularity`, `largely`, `due`, `athor,`, `every`), confidences 0.72–0.999. My AABB proxy test (`08_out.txt`) finds only tiny static protection intersections (0–40 px², five words zero) and one ≥20% pair overlap (`n`×`itsintegrity`). **So naive AABB-protection overlap does not explain most suppressions.** Remaining candidates are glyph-ink (not scene-rect) overlap after median scaling, cascade from initially-undrawn tall neighbours, or `PlanOriented`/`ReplacementIsDrawable` size/clip failures. The harness exports only `drawn` booleans — per-word reason is unobserved. Treat painter sub-cause as hypothesis H3 until instrumented replay (experiment step 0).

## Confirmed vs hypothesis

| # | Claim | Status |
|---|---|---|
| C1 | Bottom candidates exist for ~every true word ON; losses are post-CTC | Confirmed (funnel + line inventories, `04/05_out.txt`) |
| C2 | Chord/unsupported geometry rejects 0 book words ON | Confirmed (dispositions, error stats, crop counts) |
| C3 | `kLowConfidence` ⇔ conf<.7; ON bottom cost = 4 garbled words | Confirmed (40/40 equivalence) |
| C4 | Ribbon sampling lacks rectangle along-track unclip margin | Confirmed by construction (code above) |
| C5 | Admitted height floor is exactly 7.00 px; bottom not-admitted words are ≤7.6 px AABB, conf ≥.97 (8/10) | Confirmed (JSON) |
| C6 | Protections (~24 px) exceed bottom line pitch (~20 px) | Confirmed (JSON medians) |
| H1 | Endpoint clipping caused the listed first/last-char losses | Strong (pattern + C4; needs frozen-crop A/B to prove) |
| H2 | Tall quads = ink-fit 55% main-run refusal on converging/curved bands | Plausible (bounds==search; reason not exported) |
| H3 | Undrawn normal-height words = painter cascade/glyph-ink overlap | Unproven (AABB proxy negative; needs reason export) |
| H4 | Word merges = foreshortened gaps in PCA-t sampling | Plausible (design admits no deforeshortening) |
| H5 | `which` absent = gutter component split/score refusal | Unproven (no detector map exported) |
| H6 | Cross-gutter grouping harms proof/layout at bottom | Plausible (grouping confirmed; harm unmeasured) |

## Ranked next implementations (cheapest first; none relaxes a proof threshold by fiat)

All keep: `H`/`A` applied once, references/proof/ink on original analysis pixels, one latest-only worker + two-slot ownership, retained-ink semantics (refused spans still protect original pixels), straight-path arithmetic unless explicitly noted, and the narrowed merge-inheritance predicate.

### R0 (diagnostic, do first): export per-word paint/admission reasons in the resident harness only

Add to `ExportPaintSample` receipts (test code only): prepared disposition per candidate, ink-fit fitted/refusal stage, and painter sub-reason (invalid glyph / duplicate / collision / protection-touch / clip / projected-size). Zero production behavior change; converts H2/H3 into facts before any painter surgery. Effort: small. Risk: none (evidence-only, like existing profile fields).

### R1: ribbon along-track endpoint extension (unclip parity) — recommended first production change

After a valid `Fit`, extend the sampling domain by `delta = expand/length` each side (same `expand` formula as rectangles), i.e. `start=-delta, end=1+delta`, relaxing `Inside()`'s `start>=0/end<=1` to the extended domain while keeping exact boundary-extrema, positive-height-at-ends, and in-image checks. Coefficients unchanged (short quadratic extrapolation, ~2–3% per side on book lines). Windows (`PaddleRecognitionWindows`, GPU `recognitionWindows`) already remap `start/end` proportionally, so windowed lines inherit it; CTC `u` mapping, chord gate (word + widened search, now over slightly longer spans), and merge (+region origin once) are untouched in structure.

- Expected gain: first/last glyph recovery at every ribbon line end (H1 list), including the dense URL head. No effect on straight rectangles (ribbon.valid only) or on unsupported envelopes.
- Reprojection: none (still analysis pixels → inverse `A` once).
- Retained ink: unchanged; extended ends that fail chord/`Inside` keep current refusal → full envelope.
- Latency/buffers: `validWidth` grows ≈`2·delta` (~5%) on ribbon crops only; same fixed 1024 canvas, buffers, batching, one worker. Whole-worker ribbon cost today is +0.607 ms p50 book / +0.823 ms dense (doc table); expect a small fraction of the rectify/recognizer share, to be measured, not assumed.
- False-positive risk: extrapolated samples may pull gutter shadow/neighbour flourish into CTC as extra end tokens. Mitigations already present: confidence/.7, ink fit, proof. Measure new end-token errors explicitly (experiment).
- Code pointers: `uct_ribbon.hpp:56-74,141-149`; CPU `uct_paddle_processing.cpp:592-602`; GPU `uct_kernels.cu:362-368` (+ `boxes/emitBoxes` expand `:207,277` as the parity reference); tests `tests/uct_ribbon_ink_cases.hpp` (add endpoint-glyph analytic case), frozen merge oracle must stay byte-identical for rectangles.

### R2: stop discarding foreshortened small words — detail-first, gate-second

Do **not** simply lower 7.0. Two-step:

1. Route `kTooSmall` clusters (line-median height still ≥7, individual fitted heights 5–7, conf ≥.7) to the existing bounded detail machinery where the mode already allows it (paused visible-detail `DetailFocus`, 8× cap, 960 detector cap; `docs/uct_token_policy_and_detail.md`). Re-recognition at higher effective scale either produces ≥7 px fitted ink with the same proof chain or stays omitted. No new authority: detail results are refinements of the same observation, never independent evidence (existing contract).
2. Only if R2.1 is inapplicable (live full-scene), consider a *relative* size rule with measured false-positive data: e.g. candidate survives `kTooSmall` when fitted height ≥5 px **and** ≥0.5× its line's median fitted height **and** ink fit succeeded — still requiring identical confidence, reference validity, temporal/neighbour proof, and painter projected-size gates. This is a policy change needing owner sign-off and the experiment's FP counts; frame any threshold as calibrated from that data, never tuned to expected text.

- Expected gain: up to 8 correct bottom words on this page (`to/a/text,/once/not/to/of` class), more on real camera frames where foreshortening is stronger.
- Reprojection/straight: none (disposition only). Retained ink: words that still fail keep protections.
- Latency: R2.1 spends existing bounded detail attempts (32-region cap) only on affected lines; R2.2 zero. Buffers/worker unchanged.
- FP risk: small glyphs are where `l/I/1`, punctuation-as-letters, and dust live. The 4-px ink-fit floor and proof remain; count wrong-but-drawn small words as first-class experiment output.
- Code pointers: `ultra_clear_text.cpp:1597,1606-1608,842,534`; detail plumbing `PaddleDetailRegion/Magnification` (`uct_paddle.hpp:107-115`), scheduler detail scope; painter floor `ultra_clear_text_overlay.cpp:31,103-105,638`.

### R3: polygon (quad) protections for ribbon words in the painter path

Carry the search-quad polygon (dilated by exact sagitta) as protection geometry for `fromRibbon` omissions through `PreparedWord`→`AcceptedLine`→`SceneWord`, rasterizing with the existing `PolygonRegion` instead of the AABB; rectangles keep today's AABB storage and bytes. Model-side coverage/retirement can keep AABBs initially (conservative superset) — only painter touch-testing and carving use polygons, with carving generalized from "full rectangular coverage" to exact polygon intersection area (already computed via `QuadIntersectionArea`).

- Expected gain: removes false vertical touches where AABB height (24 px) > pitch (20 px); helps H3-class suppressions and cascade seeds. Magnitude unknown until R0 — do R0 first; skip R3 if reasons point elsewhere.
- Retained ink: the risk. A polygon is tighter than its AABB; under-dilation would let a plate cover original ink corners. Require sagitta-dilated quads + pixel tests proving every previously-protected source-ink pixel in fixtures stays protected (existing snapshot-pixel assertions pattern). Non-carvable rects stay non-carvable.
- Latency: painter-only polygon rasterization on layout (cached), not per pan/zoom frame beyond existing region ops. No worker/buffer change.
- Code pointers: protection creation `uct_paddle_processing.cpp:736-744`; `PrepareLines:1585-1586`; admission `:3696-3708,3078-3121`; painter `:677-785` (`ProtectedRegion:61`, `PolygonRegion:79`).

### R4: curve-following ink fit for ribbon words

Sample `FitOrientedWordInk` rows along the ribbon centerline (per-column `v` offset from `Geometry::At` midline) instead of straight chord-frame rows, for `fromRibbon` words only; mirror in the resident CUDA kernel with the same refusal rules (55% main run, floors). Targets the 8 unshrunk tall quads (H2): correct fitted heights ⇒ correct glyph scale/baseline, fewer painter collisions, fewer floating glyphs.

- Cost: medium (CPU+CUDA parity, tolerance tests like existing bin-order parity). Worker-side only; same sample caps (16K/32K) ⇒ bounded latency; no buffers/allocations change.
- FP risk: a too-eager band could shrink onto neighbour ink on converging lines; keep exterior-component removal and ambiguous-interior refusal (`:656-663`) verbatim.
- Code pointers: `ultra_clear_text.cpp:589-715,2244-2248`; resident equivalent in `PrepareResidentAnalysis` CUDA path (locate via `FitOrientedWordInk` GPU counterpart in `src/cuda/uct_validation_kernels.cuh` / device-image impl at implementation time).

### R5: straightness gate in source pixels (dense flat regression; P2-5)

Convert the `.75 detector px` floor to source units (`max(.75·max(scaleX,scaleY), .08·meanHeight·scale)`-style, exact form to be derived and oracle-checked) so a 1.5-source-px sagitta over a 225-px flat URL stays rectangle instead of becoming an unsupported/invalid ribbon (`disp4, ribErr=-1` on dense ON; OFF paints it). This *does* change near-gate straight outputs by design — the prior review deliberately deferred it for lack of evidence; the dense URL is now that evidence, but promotion needs the frozen-oracle diff reviewed explicitly (expected diff: only near-gate components switch branches).

- Gain: removes ribbon false-positives on flat dense text (screens/slides — the magnifier's common case). No bottom-book effect (real curves are far above gate).
- Risk: genuine shallow curves at high downscale stay rectangle (today's baseline behavior for sub-gate curves anyway). Latency nil.
- Code pointers: `uct_ribbon.hpp:125`; CPU/GPU share `Fit`, so one change covers both; straight control must remain byte-identical (it has no near-gate component — verify).

### R6 (defer): finer chords / arc-length deforeshortening / adaptive contour

- *Per-segment chords* (split long words at CTC gaps): chord gate has ≥2× headroom on book; only helps hypothetical high-curvature long tokens. Word-splitting breaks one-word-one-quad through reading/UIA. Defer until a sample actually hits the gate (export search-span error in R0 to detect).
- *Arc-length / deforeshortening reparameterization* (resample `t` by cumulative centerline length, or estimate local horizontal scale from bin ink density): could reduce merges (H4) but changes every ribbon tensor + `u` inverse map; needs its own bijectivity/error analysis (current exact sagitta proof relies on quadratic-in-`t`). Research item, not a cheap fix.
- *Cubic/spline centerline or per-bin height*: bottom fits already pass residual gates; more DOF risks fitting neighbour ink at converging lines and invalidates the exact chord-error formula. Defer; R1+R4 address the observed endpoint/band symptoms within the proven family.
- *Higher detector resolution / narrower regions*: book detector already finds bottom lines at 0.857 scale. For 1080p+ camera frames the 960 cap halves that; splitting `DetectorRegions` desiredEdge or adding a bottom-band supplemental region (existing regional executor, currently opt-in/off) is a latency-for-coverage trade to evaluate on camera captures, not on this still. Recognition canvas is fixed 1024×48 with `validWidth=48·W/H` (GPU `uct_kernels.cu:298,368`); bucketed widths (opt-in) change padding cost, not sampling density — not a bottom-quality lever here.

## Minimal isolated counterbalanced experiment (separates geometry / recognition / proof)

Pre-register metrics and the bottom-line transcript (read from `original.png` by a human *before* running arms; score blind to arm). Never tune constants from expected text — arms differ only by the named mechanism flags.

0. **Model-free replay (no inference):** feed saved ON/OFF `result.output` lines through `PrepareLines` + standalone `OverlayModel` + overlay painter with R0 reason export. Attributes every not-admitted/undrawn word to exactly one stage. Also replays R3 polygon variant offline. Pass criterion: reason census complete (0 unattributed).
1. **Frozen-crop recognition isolation:** existing `TensorRtCropDiagnostic` replay on identical detector quads (`uct_paddle.hpp:66-81`; diagnostic refuses frozen+immutable combos per doc — use its supported media mode): arms = rectangle / ribbon / ribbon+R1 endpoints. Detection held constant ⇒ text diffs are rectification/CTC only. Report endpoint-char recovery vs new end-token insertions.
2. **Whole-worker ABBA:** existing resident harness, production factory, warm BF16 plans, inference-only SDK, cache-inventory unchanged, `OKUFLOW_UCT_CURVED_RIBBON` + new R1 flag, visit order off/on/on/off, ≥20 timed passes, first 4 excluded (doc recipe). Sources: book, dense, straight (must stay byte-identical PNG + ordered output), synthetic curved (guard the `18→1` regression; success = no further loss, recovery counted separately). Metrics per source × y-band: candidate/disp0/disp1/admitted/drawn funnel; pre-registered bottom word accuracy (correct/wrong/missing, merges & splits counted as wrong); new false glyphs over hand/table/blank regions; worker p50/p95; crop counts; descriptor hashes.
3. **Admission/paint isolation:** same saved candidates from (2) through (a) current 7 px, (b) R2.2 relative rule (offline disposition recompute only), (c) R3 polygons. No inference ⇒ proof/paint effects separated from recognition. Report drawn-correct vs drawn-wrong deltas and any original-ink pixel newly covered by plates (must be 0 outside replaced words' own fitted ink).
4. **Decision rules (examples, fix before running):** promote R1 if endpoint recoveries > insertions and straight/dense funnels unchanged within noise; promote R2.2 only if drawn-wrong small words = 0 across sources in (3) and owner accepts the calibrated floor; promote R3 only if R0 attributes ≥5 bottom suppressions to protection touches and ink-preservation pixel tests pass.

Likely false positives to watch: extrapolated end glyphs from gutter shadow (R1); dust/punctuation promoted at 5–6 px (R2); plate corners covering neighbour ink if polygon dilation is under-estimated (R3); band shrink onto wrong line (R4); shallow real curves reverting to rectangles (R5). Each maps to an existing fail-closed guard — keep all of them; coverage bought by weakening proof is out of scope per owner instruction.

## Timing / buffer summary (measured, this evidence)

Whole-worker medium BF16 p50: book 38.617 → 39.225 ms (+0.607), dense 142.868 → 143.691 (+0.823) with ribbons on (doc table, 32 steady samples/arm). Warmup stage profile book: Regions 1.111 → 1.681 ms (fit), Rectify 0.066 → 0.165 ms, Detector ~5.1 → 4.3 ms (noise), Recognizer ~28.5 → 27.4 ms, CTC ~0.42 ms both. Recognizer input/output capacities unchanged (4,718,592 / 76,636,160 B). R1/R2/R5 add no allocations; R3 painter-only; R4 same sample caps. None adds a worker, queue, readback, or per-frame OCR — cadence and slot ownership untouched.

## Limitations

- One book photograph, one warmup job per arm for paint (timed-pass word variance unmeasured here); BF16 medium/medium on RTX 4090 only. No accuracy claim beyond the pre-registered bottom lines I read from `original.png`; the earlier ~550-word transcript was not used as truth.
- Standalone immutable admission only. Live/advancing modes additionally require temporal agreement, reference validity under motion, and (paired) current-frame registration; bottom words with jittering curved geometry may fare worse live. Nothing here validates live paint, NVDA/UIA, or Freeze latency.
- Detector probability map, ink-fit refusal reasons, painter sub-reasons, and search-span chord errors are not exported in current receipts — H2/H3/H5 remain hypotheses until R0.
- The tracked background matrix at this HEAD is **FAILED** with one unrelated UNRESOLVED `video_playback_contract` unmute/resume failure (`docs/uct_ribbon_and_current_ink.md:278-284`). Not rerun, not passed, untouched by this review.
- CPU current-ink verifier remains offline-only; nothing proposed wires it to admission or cadence.

## Recommended next implementation (single step)

**R0 + R1 together**: harness reason export, then ribbon endpoint unclip parity behind the existing ribbon flag family (new explicit diagnostic value, default following ribbon on/off), validated by experiment steps 0–2 with straight byte-identity and dense-funnel guards. It is the only ranked item that is simultaneously (a) confirmed by construction, (b) symptom-matched at every line end, (c) proof-preserving, and (d) confined to the ribbon branch. Follow with R2.1 detail routing for `kTooSmall` clusters; hold R3/R4 until R0 reasons land; schedule R5 with the frozen-oracle owners.

Artifacts in this directory: `01_schema_peek.py` … `08_paint_overlap.py` with `02_out.txt`–`08_out.txt` (read-only offline analyses; `01` output was superseded by `02`). No repository files were modified.


