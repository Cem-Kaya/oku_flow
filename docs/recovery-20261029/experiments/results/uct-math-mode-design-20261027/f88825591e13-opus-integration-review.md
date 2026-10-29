# UCT Math mode (PP-FormulaNet_plus-L): where to integrate it

Reviewer: Claude Opus 5.5, acting as the design sub-agent. This is a read-only analysis.
Date: 2026-10-27.

- **App source:** `C:/Users/USER/.t3/worktrees/oku_flow/feature-uct-recap-player`
  at HEAD `8af646428d47b96eebe79e37cea3745d5d0d841d`. The working tree was clean.
- **Experiment:** `Z:/Projects/OkuFlow/local-work-data/worktrees/formula-tensorrt-20261027`,
  branch `experiment/formula-tensorrt-20261027`, HEAD `9a0f9a9a`, clean. Its diff
  against `8af64642` touches only docs and `scripts/uct/formula_trt/*`.
- **Evidence:** `Z:/Projects/OkuFlow/local-work-data/formula-tensorrt-20261027/`, including
  `precision-report.md`, `precision-comparison.json`, `PP-FormulaNet_plus-L-tf32.json`
  and `partition-tf32/PP-FormulaNet_plus-L/{partition.json,*.plan.json}`.

No code was edited, built, tested or run. Nothing used the GPU, the GUI or a browser, and
no agents were spawned. Every statement below falls into one of two kinds:

- **[source]**: read directly from the files and line numbers given.
- **[unrun]**: a design proposal or inference that nobody has executed yet.

Experiment numbers are quoted from the receipts and were not re-measured.

---

## 1. Top recommendation

Add Math as an **optional, per-job formula stage inside the existing
`RecognitionScheduler` worker**. It gets its own job kind,
`RecognitionJobKind::Formula`. It takes the **current single-frame validation half of
the owned analysis image**, cropped to one **explicitly selected formula region**.

The formula engine is a native C++ TensorRT runner that loads the prepared plus-L
encoder and one-step decoder partitions. It does not join the Paddle OCR `Recognizer`
fallback chain. Its typed result is `FormulaObservation`. This is a third artifact
class in `OverlayModel`, separate from OCR lines/words and from source-ink outlines.
It is laid out once into a cached vector path by a **pinned native C++ LaTeX math
layout library (MicroTeX-class) drawing into `QPainterPath`**. It is painted in the
overlay's existing ink-outline pass, using the existing measured-colour, readable-colour
and display-palette mapping.

**Start with immutable sources only:** paused video, Freeze and selected files, with a
user-selected region. Extend to live camera and advancing media only after that path
works.

The reasons, all from source:

- The worker already models "one optional extra stage per job" (detail focus, regional
  slot, ink outlines) with cancellation, quarantine and no UI waits
  (`src/common/ultra_clear_text.cpp:2292-2549`).
- The validation half is already the authority for proof and ink shape
  (`uct_device_image.hpp:58-66`, `docs/uct_ink_outlines.md:66-67`). Recognizing the
  formula from those exact pixels sidesteps the smoothed-OCR self-proof problem
  entirely.
- Plus-L takes 77.6 ms for a short equation and 496 ms for the complex formula (model
  call only, TF32). A 2560-token bound extrapolates to several seconds. That is
  incompatible with folding formula work into every full live OCR read. On immutable
  frames, OCR already runs once (`docs/ultra_clear_text.md:21-31`).

---

## 2. Current UCT contracts a Math stage must respect [source]

| Contract | Location |
|---|---|
| One worker, one in-flight job, one latest-only pending slot; `Submit` never blocks | `include/okuflow/common/ultra_clear_text.hpp:537-602`; `docs/ultra_clear_text.md:388-392` |
| Two owned analysis device slots; busy slot skips; unknown completion quarantines the pool | `docs/ultra_clear_text.md:187-191, 322-326, 694-698` |
| Job kinds `Full / PausedDetail / Regional`; per-job scoped `SetDetailFocus` reset by RAII | `ultra_clear_text.hpp:471`; `ultra_clear_text.cpp:2325-2330` |
| Regional work yields to queued full work and to its deadline (`yieldToFull`) | `ultra_clear_text.cpp:2292-2321` |
| Post-OCR worker stages: colours, ink fit, `FinalizeRecognizedGeometry`, ink outlines (resident or CPU), `PrepareLines`, smoothed→current registration, corroboration | `ultra_clear_text.cpp:2380-2547` |
| Resident ink outlines via a `DeviceImage` virtual; CUDA implementation | `uct_device_image.hpp:73-75`; `src/cuda/uct_analysis_pool.cpp:126-135` |
| Validation half = exact current frame (never smoothed) | `uct_device_image.hpp:58-66`; `uct_analysis_pool.cpp:110-125` |
| Scene mapping once (pixels → normalized processed scene) at preparation | `ultra_clear_text.cpp:1677-1766` (words 1692-1721, outlines 1738-1761) |
| Smoothed candidates and protected spans need in-place current registration (0.94 / 95% / zero translation) | `ultra_clear_text.hpp:447-449, 479-485, 513-515`; `ultra_clear_text.cpp:1616-1625, 2496-2517` |
| Admission variants (live / advancing / immutable Standalone / Corroborated) | `ultra_clear_text.hpp:916-955`; `ultra_clear_text.cpp:3055-3134, 3708-3716` |
| Probe revalidation of accepted line references; 350 ms freshness | `ultra_clear_text.hpp:960-971`; `docs/ultra_clear_text.md:453-460` |
| Ink outline planning (protected dispositions only, ≤8 regions, 131,072 px) | `ultra_clear_text.cpp:1627-1675`; `docs/uct_ink_outlines.md:16-24` |
| Overlay: measured → `ReadableColors` (7:1 or 3:1 colored paper, minimal background move) → palette map (Source / Inverted / LumaLut) | `src/ui/ultra_clear_text_overlay.cpp:113-168, 308-339`; `include/okuflow/ui/ultra_clear_text_overlay.hpp:72-79` |
| Outline paint pass: fill only, no plate, `visibleClip` | `ultra_clear_text_overlay.cpp:985-1011` |
| Palette changes recolor cached artifacts without recognition | `ultra_clear_text_overlay.cpp:308-320`; `src/app/app_ultra_clear_text.cpp:46-57` |
| Controller refresh assigns words and outlines | `src/app/ultra_clear_text_controller.cpp:2181-2233`; results at 1515 / 1855 |
| TRT 11 loader (explicit DLL dirs, ≥11.4.0.106, process-lifetime modules) | `src/common/uct_tensorrt.cpp:184-281` |
| Plan cache file bound **256 MiB**, context bound 256 MiB | `src/common/uct_engine_cache.hpp:9-19` |
| Payload ceilings: 512 MiB per file, 3 GiB total; current inventory 2,394,527,371 bytes | `docs/uct_distribution.md:88-90, 165-171` |
| Settings and typed controls `image.uct.*`; intent vs effective readiness | `src/app/settings_store.cpp:662, 1001-1004`; `src/app/agent/app_control_image_parameters.cpp:38-43, 121-126, 493-531, 611-617, 820-882` |
| UCT panel: enable, Show original, advanced disclosure | `src/ui/ultra_clear_text_panel.cpp:55-115`; placed in Text clarity at `src/ui/main_window.cpp:1101-1102` |

### Experiment facts relevant to the interface [source: evidence]

- **Encoder input:** `x` is `{1,1,768,768}` FLOAT on the device. There are 16 invariant
  `{1,144,512}` cross-attention projections.
- **Decoder:** 59 I/O tensors and 21 recurrent states. The KV state profile max is
  `{1,16,2568,32}`. Token state max is `{1,2568}`. `maxSequenceLength = 2560`.
  The output names `conditionOutput` and `finalTokenOutput` come from `partition.json`.
- **Plus-L TF32 sizes:** `encoder.plan` 410,464,844 bytes; `decoder.plan` 329,386,964
  bytes; `encoder.onnx` 400,370,567 bytes; `decoder.onnx` 328,124,899 bytes.
- **Build times** on the RTX 4090 (receipts): encoder 17.3 s, decoder 7.0 s.
- **Iteration counts:** the kinetic case took 29 decoder iterations at 77.58 ms. The
  complex case took 198 iterations at 496.33 ms, so about 2.5–2.7 ms per step.
  - Worst case at 2560 steps is about 6.4 s. This is an **extrapolation and was not
    measured**.
- **Host loop:**
  - one stop byte D2H and stream sync per iteration;
  - final int64 tokens D2H;
  - two output banks with `IOutputAllocator`;
  - overrun is a failure;
  - any exception quarantines the runtime (`scripts/uct/formula_trt/benchmark.py:56-189`).
- **Tokenizer:** the HF fast-tokenizer JSON is embedded in the model's
  `config.json → PostProcess.character_dict.fast_tokenizer_file`. EOS id is 2, and
  decoding uses `skip_special_tokens` (`prepare_overlay.py:16-35`).
- **Inputs:** the chalk inputs were percentile-stretched and **inverted** before the
  standard preprocessing (`docs/formula_tensorrt_experiment.md`). The reference capture
  stores exact model inputs (`*-input.npy`) and tokens (`*-tokens.npy`)
  (`capture_reference.py`).

---

## 3. Answers to the eight questions

### Q1. Worker, backend and scheduler integration

**Where:** `RecognitionScheduler::State` (in `ultra_clear_text.cpp`, around the worker
body at 2245-2549). Do **not** add a candidate to `AutomaticRecognizer`
(`uct_paddle.cpp:144-167`). That chain means "exactly one OCR backend runs this job,
with failover". A formula fault must never retire OCR, and an OCR failover must never
silently run math on the CPU.

Proposed contracts [unrun]:

- **Scheduler construction.** Add an optional `FormulaEngineFactory` to the scheduler
  constructor (`ultra_clear_text.hpp:548-555`).
  - The engine object is created and prepared lazily, worker-confined, on its own
    preparation thread. This mirrors `TensorRtRecognizer::BeginPreparation`
    (`uct_tensorrt.cpp:1137`). It is preparation, not a second inference worker.
  - Math off means the factory is never invoked: no DLL load, no VRAM, no time.
- **Submission.** Add `std::optional<FormulaRequest>` to `Submit` / `SubmitDevice`
  (`ultra_clear_text.hpp:565-577`). It occupies the same latest-only pending slot as
  every other job. The request carries:
  - the source region in image pixels;
  - the token cap;
  - the wall deadline;
  - the model identity.
- **Job kind.** Add `RecognitionJobKind::Formula` (`ultra_clear_text.hpp:471`). A
  formula job runs **no Paddle OCR**. It does only:
  1. the hold-provenance read (unchanged, 2251-2257);
  2. the `EvidenceAcquisition` refusal for crossfade/unknown (2268-2273);
  3. resident preparation of the formula input from `ValidationData()`;
  4. the engine call;
  5. the reference patch from the same validation pixels;
  6. measured colours;
  7. vector layout;
  8. a scene mapping performed once.
- **Priority.** Apply the regional rule: if a full job is pending, or the deadline has
  no slack, the formula job yields without inference (`result.output.cancelled`, same
  as 2316-2321). Formula never delays a queued full read by more than an
  already-started formula call.
- **Cancellation.**
  - `cancelInFlight` is checked between decoder iterations. The host already
    synchronizes per iteration, so this costs nothing extra.
  - `Cancel()` / authority changes revoke results as they do today.
  - The owned image stays held until the call returns, under the existing lifetime rule.
- **Fault containment.**
  - An exception or uncertain CUDA completion in the formula engine marks **only the
    formula engine** quarantined. Its owned buffers are retained until process exit,
    as in the experiment.
  - Math status shows "unavailable". OCR and backend status are untouched.
  - If the CUDA context itself is sticky-faulted, the next OCR call fails through its
    existing retirement path. Do not add a special cross-retirement rule.
  - A typed pre-inference artifact or plan refusal is not a fault: Math is
    "unavailable" and nothing retries.
- **Separate status.** Add `FormulaStatus` next to `BackendStatus`, published through a
  new optional callback. This keeps the OCR backend label and the existing
  `IsUctControl` readiness semantics unchanged.
- **Stream.** Use the engine's own private low-priority non-blocking stream, waiting on
  `WaitValidationReady` (`uct_device_image.hpp:63-66`). Never use the presenter stream.
- **No UI blocking.**
  - The controller submits only metadata plus the existing lease.
  - Layout and colour decisions happen on the worker.
  - The UI does rect checks and assignment only, under the existing admission budget.
- **Device input.**
  - Add `virtual bool PrepareResidentFormulaInput(...)` beside
    `PrepareResidentInkOutlines` (`uct_device_image.hpp:75`; CUDA implementation next
    to `uct_analysis_pool.cpp:132-135`).
  - It writes the cropped, polarity-normalized, resized, normalized 768×768 float
    tensor directly into the engine's input buffer, so no host image leaves the device.
  - The CPU-image path (no resident analysis) may use a worker-side QImage crop and one
    upload, matching the existing explicit CPU rule (`docs/ultra_clear_text.md:196-203`).
- **Two slots.**
  - Immutable sources already keep their selected lease
    (`docs/ultra_clear_text.md:872-877`), so a formula job adds no lease.
  - Live sources take a normal pool lease exactly like OCR. A busy slot skips the
    opportunity.

### Q2. Isolating whole 2D formula regions

**An explicit region is needed first.** No auto-detection in v1. The Paddle text
detector is line-oriented:

- On the equation fixture it produced `K=mv2` across a fraction (rectangle control)
  or protected spans (`docs/uct_ink_outlines.md:9-14`).
- Numerator, bar, denominator and superscripts land in separate or fragmented detector
  lines.
- A union of protected spans is not a formula boundary. It can omit a small exponent
  the detector never boxed, or pull in an adjacent diagram label.

Plan [unrun]:

1. **The user selects a scene-space rectangle.** The annotation overlay already shares
   processed-scene coordinates (`docs/ultra_clear_text.md:296`;
   `include/okuflow/common/annotation_model.hpp:22-28`). Store it in normalized scene
   coordinates; the source authority and geometry generation revoke it, as with any
   UCT state.
   - Optional convenience: "use the visible viewport", reusing `DetailRegion` logic
     (`ultra_clear_text.hpp:289-297`).
2. **The worker tightens the region using the existing ink policy.**
   - Project onto the measured background→ink colour axis, as the outline code does.
   - Find ink components inside the selection.
   - The formula plate is the bounding box of components **wholly inside** the
     selection, plus a small margin.
3. **Refuse rather than clip.** If any component crosses the selection edge, the
   formula is refused with `BorderInk`, the same semantics as `ink::Refusal::BorderInk`
   (`uct_ink_outline_types.hpp:18`). A crossing component signals a cut exponent, a
   continuing fraction bar or a diagram line, so the original remains. The user is
   told to widen the selection.
   - Optionally allow one bounded automatic expansion. A loop is not allowed.
4. **Never cover recognized text or unread neighbours at admission.**
   - If any glyph-eligible accepted word, or any other accepted artifact, intersects
     the formula plate, refuse to draw it, for both the old and the new artifact.
   - Protected spans *inside* the plate become owned by the formula only while the
     formula is shown.
   - Ink outlines inside an admitted formula plate are suppressed for painting only;
     they remain in the model, so refusal or revocation restores them.
5. **Later stage, optional and needing separate review:** automatic proposals.
   - Either cluster protected spans with ink-component growth and present them as
     *suggestions only*, or use a layout model (PP-DocLayout-class "formula" region).
   - A layout model needs its own pins, notices and payload review.
   - Neither should silently define a plate.

### Q3. Reusing colour measurement, contrast and LUT mapping

No hardcoded black plate or yellow ink [source + unrun]:

- **Measure** on the worker. `MeasureLineColors(validationPixels, formulaInkRect)`
  (`ultra_clear_text.cpp:240`) gives foreground/background.
  - On the resident path, reuse the resident colour sampler that already serves words
    (`docs/ultra_clear_text.md:193-197`). The integer policy is in
    `src/common/uct_color_policy.hpp`.
  - Store the measurement as `LineColors` in the formula artifact.
- **Polarity for the model:** apply the experiment's percentile-stretch inversion when
  the measured ink is lighter than the background (chalkboard). This decision comes
  from the same measurement, not a new heuristic.
- **Render** through `UltraClearTextOverlay::GlyphColors(measured)`
  (`ultra_clear_text_overlay.cpp:322-339`). That applies:
  - `ReadableColors`: ink snaps to black/white first, keeping the source board/paper
    colour, and the background moves the minimum needed for 7:1;
  - then `DisplayColors::Source / Inverted / LumaLut`.

  So "normal" appearance is the measured original colours with a contrast floor.
  Yellow-on-black appears **only** if the user selected that display palette.
- **Recolor** on palette change: extend `SetDisplayColors` (308-320) to recompute
  formula colours, as it already does for `inkOutlineColors_`. There is no recognition
  or layout change.
- **Plate:** the formula plate is required, because the vector math does not coincide
  with the source ink. It uses `GlyphColors(...).background`, so a green board stays
  green, moved only as far as contrast requires.
  - It is painted in the plate pass (before glyphs) and clipped by `visibleClip`.
  - It never extends outside the tightened formula rectangle.

### Q4. Native LaTeX math vector layout

| Option | Verdict |
|---|---|
| **A. Pinned native C++ math layout library (MicroTeX, the successor of cLaTeXMath), Qt/QPainter backend, OpenType math font** | **Recommended.** It produces paths in-process with no runtime process or Node, and it caches per exact LaTeX. [unrun] License and font must be verified before vendoring: the repository is believed MIT, and fonts would be Latin Modern Math (GUST FL), STIX Two Math or XITS (OFL). These are **not verified here**. |
| B. Qt-native mini typesetter (frac, sup/sub, sqrt, Greek, operators, `\operatorname`, `\left\right`, arrays) on `QPainterPath` + `QRawFont` | Fallback if A fails review. It is a large maintenance surface because the model vocabulary is broad, and it must refuse unsupported commands. |
| C. MathJax/KaTeX via Qt WebEngine | Rejected. WebEngine is not in the documented Qt baseline (AGENTS "Qt Toolchain Baseline"), and it is heavy. |
| D. Private helper process (Node/MathJax/Sharp) | Rejected. It ships Node/LGPL Sharp, which the experiment's `THIRD_PARTY_LICENSES` addition explicitly says is development-only. Cold start was measured at about 168 ms. |

Contract [unrun]:

- Layout runs **on the worker**. Store glyph outlines in em units inside an immutable
  `std::shared_ptr<const FormulaLayout>`, with a bounded LRU keyed by the exact LaTeX
  string and font. The experiment used 64 entries. A cache hit grants no frame
  authority.
  - Confirm the library is reentrant off the GUI thread with `QRawFont`/`QPainterPath`.
    If it is not, do layout in a UI-thread idle step outside the paint path, as glyph
    preparation does today (`docs/ultra_clear_text.md:647-652`).
- Parse or layout failure gives `FormulaRefusal::Layout`. The original ink remains, and
  the reading pane may still show the LaTeX string.
- **Placement:** uniform scale (never anisotropic) to fit the tightened ink rectangle,
  centred. The transform is computed once per paint from scene→view, exactly like
  `plan.ink[i].transform` (`ultra_clear_text_overlay.cpp:1007-1009`).
- **Keep the experimental Python/Node out of the build:** the CMake/bundle staging
  manifests must not reference `scripts/uct/formula_trt/`. Add a model-free check that
  the release inventory contains no `*.py`, `*.mjs` or `node_modules` under the app
  payload. The existing staging is closed-list (`docs/uct_distribution.md:88-114`), so
  it already refuses unlisted files.

### Q5. Typed math result, proof and retention

The math result gets new types, separate from `RecognizedWord` and `ink::Outline`
[unrun]:

```text
namespace okuflow::uct::math
enum class Refusal { None, NotReady, Region, BorderInk, Overlap, TokenBound,
                     Cancelled, EngineFault, Decode, Layout, LowContrast };
struct Observation {            // worker output, image pixels
  QRect selectionPx, inkPx;     // selection and tightened plate
  QVector<std::int64_t> tokens; // through first EOS, bounded
  QString latex;                // detokenized literal model output, never corrected
  LineColors colors;            // measured from validation pixels
  std::shared_ptr<const FormulaLayout> layout;
  QString modelId, precision;   // e.g. PP-FormulaNet_plus-L, "FP32 graph; TF32 permitted"
  Refusal refusal; int iterations; double modelMs, layoutMs;
};
RecognitionResult::formulas    // QVector<Observation>, NOT parallel to lines
PreparedFormula { sceneRect, protectedSceneRect, ReferencePatch reference, ... }
AcceptedFormula { id, sceneRect, latex, tokens, layout, colors, reference,
                  provenance(SourceStamp), valid, sceneOffset, validatedAtMs, validatedFrameSeq }
OverlayModel::formulas_         // separate vector; SceneFormulas(now, frame)
```

Proof [unrun, mirroring existing rules]:

- **Same scene, authority and age gates** as lines (`docs/ultra_clear_text.md:429-433`).
- **Current-frame authority comes by construction.** Input, reference and colours all
  come from `ValidationData()` of one stamped acquisition. A temporally smoothed lease
  contributes only its exact current half, so no smoothed self-proof is possible.
  Crossfade or unknown acquisitions are refused before inference.
- **Live and advancing sources** need two formula observations from strictly
  increasing acquisitions with **identical token arrays** and plate IoU ≥ 0.5. This is
  the live text agreement rule (`ultra_clear_text.hpp:920-932`). It checks stability,
  not correctness.
  - New live formulas stay hidden until a probe at or after the acquisition validates
    the reference.
- **Immutable Standalone** admits on the selected read alone, as words do today
  (`ultra_clear_text.hpp:940-955`).
- **Revalidation:** expose formula references through `LineReferences()` /
  `ApplyChecks()` with a distinct id namespace, so `ProbeWorker` stays unchanged.
  Failure withdraws the formula at once, and freshness is 350 ms.
- **Retention:** the plate rectangle protects source ink only while shown, and is never
  carved.
- **Separation:**
  - Formulas are never in `lines_`, `readingTokens`, `ScreenLines`,
    `AcceptedWordCount` or the painted-word count, and are never "sketched regions".
  - Outlines stay non-text (`docs/uct_ink_outlines.md:98-104`).
  - OCR words never read from formula tokens.
- **Reading surface:** open decision 2 below.

### Q6. Toggle placement, persistence and assistant discovery

All proposals in this section are [unrun].

**Placement**

- Add a `Math mode (experimental)` checkbox inside `UltraClearTextPanel`, directly
  below `Show original` (`ultra_clear_text_panel.cpp:65-71`).
- It is indented and enabled only while UCT is effectively enabled, the same
  dependency as Show original (`UpdateDependentState`).
- Its accessible description says it recognizes a selected formula, can be wrong, and
  that Show original compares.
- Add a "Select formula region" action next to it, keyboard reachable.

**Persistence**

- Add `ultraClearTextMathEnabled` (default **false**) beside `ultraClearTextEnabled`
  and the model tiers in `settings_store.cpp` (load around 662, save around 1001).
- Missing keys mean off. There is no migration code, per the owner's no-in-app-migration
  rule.
- The selected region is session state, never persisted.

**Wiring**

- In `app_ultra_clear_text.cpp:24-30`, connect the panel signal to settings save and
  to a controller `SetMathEnabled(bool)`.
- `SetMathEnabled` must **not** call `ReplaceRecognizerFactory` (line 42), because
  toggling Math must not restart OCR. It only:
  - enables or disables formula submissions;
  - revokes formula artifacts;
  - lets the scheduler lazily prepare or idle the formula engine.

**Assistant discovery**

- Add `image.uct.math.enabled` to the typed field table
  (`app_control_image_parameters.cpp:38-43`) and to `IsUctControl` (121-126).
- Report requested intent separately from effective readiness, extending the reasons
  at 611-617 with `uct_disabled`, `text_clarity_disabled`, `math_unavailable` and
  `math_preparing`.
- Keep the field out of `IsUctModel`, so a write never triggers
  `OnUltraClearTextModelsChanged`.
- Region selection is not an assistant-writable field in v1.

**Scope**

- Default UCT is byte-for-byte unaffected when Math is off.
- Add a contract test: same submissions, same scheduler stats, no formula factory
  invocation.

**Localization**

- New strings need the 25-catalog parity and translation review policy
  (`AGENTS.md`: Localization; `scripts/check_translations.ps1`).

### Q7. Prepared payload, cache and runtime vs current app TensorRT APIs

Gaps found in source:

1. **Cache bound blocker.** `engine_cache::kFileLimit = 256 MiB`
   (`uct_engine_cache.hpp:9`). Plus-L plans are about 391 MiB (encoder) and 314 MiB
   (decoder), so they cannot be written or read through `WriteNew` / `FileFits`.
   - A separate formula cache bound is needed, scoped to the formula role (for example
     768 MiB per plan), with the same digest-prefixed atomic-rename format.
   - `kContextLimit` (256 MiB) must also be checked against actual plus-L context
     memory. This was not measured here.
2. **Different execution shape.** The app's TRT network wrapper serves static or
   bounded detector/recognizer profiles. Formula needs:
   - `IOutputAllocator` with capped buffers, rejecting oversize allocations;
   - two output banks;
   - shape-tensor plumbing per step;
   - a host stop-flag loop;
   - an optional fixed-shape encoder CUDA graph, which belongs later and only after an
     ABBA measurement.

   This is new code in a new translation unit, `src/common/uct_formula_tensorrt.cpp`.
   It **reuses** `Libraries` (`uct_tensorrt.cpp:184-281`) for DLL loading, version
   gating and the builder resource, and `tensorrt_identity` for cache keys. Moving
   `Libraries` into a shared private header is a needed refactor.
3. **What to ship versus build locally.**
   - Ship the **partitioned FP32 ONNX pair**, not plans, plus:
     - `initial-states` converted from `.npz` into a raw little-endian blob and a JSON
       manifest at developer preparation time;
     - `partition.json`, which carries the profiles, the state↔output map, invariants,
       the condition/final outputs and `maxSequenceLength`;
     - the tokenizer table extracted from `config.json`;
     - a pin catalog.
   - The app builds plans locally on a cache miss with the existing builder path, using
     the strongly typed FP32 graph with TF32 permitted. That is the experiment's
     fastest setting: equation 77.58 ms vs BF16 86.69 ms; complex 496.33 vs 580.19 ms.
     A TF32 build needs no converted BF16 graph to ship.
   - The cache key binds model, partition and graph hashes, GPU UUID, builder TRT
     version, profile and the TF32 policy, following the existing cache policy
     (`docs/uct_tensorrt_cache_policy.md`).
4. **Size.**
   - Adding about 728.5 MB of plus-L ONNX to the 2,394,527,371-byte inventory gives
     about 3.12 GB, which is under the 3 GiB (3,221,225,472) ceiling with about 98 MB
     of headroom.
   - Both files are under the 512 MiB per-file limit.
   - Recommendation: a **separate optional "math" payload component** with its own
     catalog and receipt, rather than consuming the default payload headroom. This is
     an owner decision.
5. **VRAM [unrun estimate from shapes].**
   - Weights are on the order of 0.7 GB.
   - Each KV bank state at max is about 5.3 MB. With 21 states and 2 banks that is on
     the order of 0.2 GB.
   - The total is roughly 1 GB on top of the OCR engines, which matters on 8 GB cards.
   - Measure it; do not assume.
6. **Licensing and notices.** The model cards declare Apache-2.0
   (`models.json`, experiment). The app payload needs:
   - the Apache-2.0 license text and any upstream NOTICE;
   - a generated `MODIFICATIONS`-style notice, because partitioned, mask-rewritten and
     invariant-lowered graphs are **derivative graphs**. This mirrors the BF16 graphs
     (`docs/uct_distribution.md:11-16`);
   - the first-party converter (`rewrite_mask.py`, `partition_formula.py`,
     `precision_variants.py`) retained **as source** in the payload provenance, never
     executed by the app;
   - the layout library license and font license (OFL/GUST) in
     `docs/THIRD_PARTY_LICENSES.md` and the SBOM.

   Paddle, Paddle2ONNX, ORT, tokenizers, MathJax and Sharp stay developer-only and
   unshipped. TensorRT/CUDA notices are already covered by the existing private subset.

### Q8. Smallest functional rollout, tests, latency and risks

Scope is stated as components and tests. No schedule is given.

**Stage 0: prepared-artifact freeze (developer tooling, experiment branch)**

- Emit C++-loadable artifacts: raw initial states, a JSON partition and the vocab
  table.
- Write a pin catalog of exact hashes, a `MODIFICATIONS` notice and an extended
  `stage_uct_payload.ps1` or a sibling math payload script.
- Tests: the existing Python contracts plus a payload model-free fixture.

**Stage 1: native engine, no UI**

- New files:
  - `src/common/uct_formula_tensorrt.{hpp,cpp}` (runner);
  - `uct_formula_tokenizer.hpp` (byte-level BPE *decode only*, special-token skip, EOS
    stop);
  - a CUDA preprocessing kernel in `src/cuda/` (crop, polarity, resize/pad, normalize);
  - `math::Observation` types.
- Model-free tests:
  - partition JSON parsing and refusals;
  - tokenizer decode against fixed id→string vectors taken from captured references;
  - token-bound overrun is a refusal, not truncation;
  - cancellation between iterations;
  - fault leading to quarantine with no retry;
  - cache bound and digest.
- Optional GPU row (opt-in, serial):
  - Load prepared plus-L and compare token IDs against `reference/<model>/*-tokens.npy`
    using the captured `*-input.npy`. This is **backend parity, diagnostic**, not a
    startup or quality gate.
  - Compare the preprocessing kernel output against `*-input.npy` within a declared
    tolerance. The source crops exist in the experiment's assets.

**Stage 2: smallest functional user feature (immutable sources only)**

- Add the formula job kind and request; the resident input virtual; the worker stage;
  colours; layout through the pinned library; `PreparedFormula`/`AcceptedFormula`; and
  Standalone admission.
- Add overlay plate and path painting, palette recolor, Show original, revocation on
  source, geometry, Show original, modal or inactive state.
- Add the panel checkbox and region selection, the setting, the typed control and
  translations.
- Tests:
  - controller/model contract: admission, revocation, overlap refusal, BorderInk
    refusal, no change to OCR words, reading text, UIA text or the painted-word count;
  - palette mapping rows mirroring the existing ink-outline rows;
  - export snapshot inclusion;
  - Math-off invariance;
  - actual-pixel paint assertions, not counts (AGENTS UCT paint rules).
- Native GUI and camera acceptance stay separate and serial, with no desktop use
  without fresh owner permission.

**Stage 3: live camera and advancing media**

- Two-acquisition token agreement, probe revalidation and region stability.
- One formula job at most per full interval, yielding to full work.

**Stage 4 (optional): automatic region suggestions**

- Gated on its own review (Q2.5).

**Latency measurement (honest scopes)**

Report each scope separately:

- engine model-call warm medians (upload, encoder, decode loop, tokens), comparable to
  the receipts;
- preprocessing kernel time;
- detokenize plus layout (first-time and cache-hit, separately);
- whole-worker submit-to-callback for formula jobs;
- paint cost with formula paths;
- OCR full-read delay caused by a concurrent formula job, measured live.

Use warm, unchanged plans with off/on/on/off visits and retain failed attempts. Native
presentation and sensor-to-photon latency are not claimed.

---

## 4. Blockers (must be resolved before Stage 2 ships)

1. **Engine cache bound** of 256 MiB per plan (`uct_engine_cache.hpp:9`). Plus-L plans
   exceed it. *Needs a formula-scoped bound.* [source]
2. **No native runner exists.** The formula path today is Python TensorRT bindings plus
   numpy `.npz` initial states and the HF tokenizer. All three need C++ equivalents.
   [source]
3. **No native math layout.** MathJax/Sharp are development-only. A pinned C++ library
   (or the B fallback) plus a math font need license review. [source + unrun]
4. **Preprocessing parity.** The app must reproduce the PaddleX UniMERNet preprocessing
   from the pinned `inference.yml` exactly, plus the experiment's polarity
   normalization. This was not inspected in detail here. Parity against `*-input.npy`
   is the check. [unrun]
5. **Region source.** No automatic formula detector exists. Region selection UI is new.
   [source]
6. **Payload and notice** inventory and the derivative-graph `MODIFICATIONS` notice.
   [source]

## 5. Specific risks (no accuracy gates invented)

- **Worker occupancy.**
  - About 2.5 ms per decoder step, and the complex formula took 496 ms.
  - Pathological long outputs could hold the single worker for seconds before the
    token cap.
  - Mitigations: token cap, a per-step cancellation check, yield-to-full, and
    immutable-first.
- **VRAM** of roughly 1 GB extra, an unmeasured estimate. 8 GB GPUs may degrade OCR
  engine headroom. TensorRT needs Turing or newer; GTX 1070 has no Math mode
  (`uct_tensorrt.cpp:1130-1136`).
- **Recognition errors are real and rendered faithfully.** For example, source `K`
  becomes `k`. Do not post-correct. Show original, the reading-pane LaTeX and the
  "can be wrong" description are the user-facing mitigations. Owner policy accepts
  normal ML errors; nothing here gates on exact text.
- **Plate erasure.** A selection containing diagram strokes plus formula is refused by
  BorderInk or Overlap, which can feel strict. Prefer refusal over erasing unread
  content.
- **Two-observation live agreement** may rarely succeed on noisy camera input, because
  autoregressive outputs vary. Formulas would then stay pending. This is an expected
  conservative outcome; do not relax proof to force a draw.
- **Cold build cost.** The 4090 receipt shows 17 s + 7 s with the full SDK. Slower GPUs
  and staged builder resources are unmeasured. Preparation must stay off the UI, be
  cancellable, and show "preparing".
- **Context-sticky CUDA faults** from a new runner can take OCR down through normal
  retirement. Isolate streams and buffers, and quarantine on uncertainty.
- **Library reentrancy** for off-thread layout is unverified.

## 6. Open owner decisions

1. **Payload:** a separate optional math component (recommended) or the main UCT
   payload (fits with about 98 MB headroom).
2. **Reading surface:**
   - (a) no formula text anywhere;
   - (b) LaTeX string in the reading pane only, labelled as a formula (recommended for
     v1);
   - (c) later MathML or spoken math via UIA, which needs separate design and native
     NVDA acceptance.
3. **Precision:** TF32-permitted FP32 build (recommended, fastest in the receipts, no
   converted graph) versus BF16.
4. **Region UI:** drag-select on the viewport (recommended) versus a "visible viewport"
   shortcut versus both.
5. **Stage 3 (live):** whether and when to enable it after immutable experience.
6. **Model choice:** plus-L only (owner's stated choice) versus also exposing plus-S for
   lower VRAM/latency. The receipts show plus-S equation at 11.36 ms TF32.

## 7. Confidence summary

- **High (source-read):**
  - the worker, scheduler, job-kind and resident-virtual insertion points;
  - the colour, readable-colour and palette pipeline and its reuse;
  - the outline separation contracts;
  - the settings and typed-control touchpoints;
  - the TRT loader reuse;
  - the cache-bound blocker;
  - payload ceilings;
  - the experiment tensor interface and sizes.
- **Medium (inference from source):**
  - formula current-frame authority via `ValidationData()`;
  - the formula refusal taxonomy;
  - probe revalidation reuse through `LineReferences`.
- **Unverified / unrun:**
  - MicroTeX license, font and reentrancy;
  - VRAM;
  - preprocessing parity;
  - worst-case decode time;
  - context memory against `kContextLimit`;
  - cold builds on other GPUs;
  - any native GUI, NVDA or camera behaviour.
