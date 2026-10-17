# Ultra Clear Text (experimental, development)

Ultra Clear Text (UCT) recognizes steady printed camera text on this computer
and draws sharp, high-contrast letters over the original words. It is an
opt-in, session-only development feature. It is **not** release-accepted, and
nothing here claims recognition accuracy beyond the fixtures listed below.

OCR is not optical sharpening. UCT replaces only words it has recognized and
validated; other pixels stay as the original image. It is intended for steady,
flat printed words. Token filters conservatively omit some risky notation, but
there is no reliable handwriting/math/diagram classifier: two agreeing OCR
reads can repeat the same mistake. Complex notation and curved pages are not
accepted use cases. Use Show original to compare, especially for numbers.

## What the user gets

- **Advanced → Image → Text clarity → Ultra Clear Text (experimental)**. Off by
  default and not persisted: it starts off on every launch.
- **Show original camera text**: always available while UCT is on; hides the
  sharp letters for comparison without stopping recognition.
- A polite status line (starting, waiting for steady text, reading, showing N
  words, text moved, paused, unavailable) and a read-only, keyboard-selectable
  **Recognized text** pane with the accepted words in reading order. An
  ellipsis (…) marks words that were left as the original image.
- Glyph weight follows the existing Text Clarity **Stroke weight** slider.
  Plate colors are measured from the processed camera pixels, using a bounded
  surrounding margin to identify the background of dense/bold print, so the display
  color scheme is preserved; if that pair reads below 7:1 contrast the plate
  falls back to pure black/white in the scene's polarity.

## Recognition backend

UCT uses the built-in Windows OCR engine, `Windows.Media.Ocr`, through
C++/WinRT headers from the Windows SDK (10.0.26100 on the development
machine) and `windowsapp.lib`. No model file, download, network call or
provider account is involved, and nothing is redistributed: the engine and its
language data are part of Windows.

Runtime availability:

- Windows 10/11 desktop. The engine is created from the user's profile
  languages, falling back to the first installed OCR-capable language.
- If Windows has no OCR-capable language, UCT reports *Text recognition is
  unavailable: Windows has no OCR language installed* and shows the original
  image. Users add OCR support with a Windows display language that includes
  optical character recognition (Settings → Time & language).
- Recognition runs on the CPU on a dedicated worker thread. It does not use
  CUDA, tensor cores or Maxine, so the GTX 1070 / CUDA 12.9 optical path is
  unaffected; the CUDA pipeline keeps running independently.
- Measured on the development machine (en-US, synthetic 1600×900 page):
  about 25–32 ms per recognition. This is one data point, not a budget.

Repeated backend errors (three in a row) switch UCT to *Text recognition
stopped working*; it then shows only the original image.
An explicit off/on retries a failed or unavailable initialization with a fresh
recognizer; queued callbacks from the retired scheduler cannot overwrite the
new attempt. Ordinary slow recognition does not trigger restart.

## Architecture

```text
camera -> CUDA/CPU processing -> presenter -----------------> viewport
                                 |
                     stamped asynchronous viewport readback
                                 |
             UI: immutable pooled-buffer handoff (vector swap)
                                 |
             ProbeWorker: luma/integral, similarity, residual checks
                                 |
             UI: apply matching outcomes / latest-only OCR admission
                                 |
             RecognitionScheduler: Windows OCR + line preparation
                                 |
             UI: bounded admission / cached scene-space words
                                 |
             overlay reprojects and paints on the presentation clock
```

There is no second capture or render pipeline. UCT reuses:

- `PresentLatestCudaScene` / `PresentFitted` for the per-presentation hook,
  the annotation `ViewTransform` (full scene → viewport) and the presenter's
  existing four-slot asynchronous readback ring
  (`ViewportPresentationOptions::requestReadback`). The CPU presentation path
  already owns system-memory viewport pixels and offers them directly.
- The annotation overlay's coordinate space and floating tool-window pattern.

### Clocks and non-blocking guarantees

- `OnPresented` runs before each presentation attempt. It compares a small geometry key,
  forwards the current transform to the overlay, enforces freshness and
  decides whether to request one more copy. It never waits for recognition.
  `OnPresentationSucceeded` separately records the presenter's successful
  submission receipt; this does not measure GPU scanout or sensor-to-photon time.
- The overlay holds only accepted scene-space words. Pan, zoom, Fit/Fill and
  resize repaint through the newest transform on the presentation clock.
- Probe construction, similarity and residual searching run on `ProbeWorker`,
  with one executing and one latest-only waiting request. A 60 ms per-probe
  search budget leaves unchecked lines unverified/hidden; a cancellation is
  checked between line searches. OCR preparation also runs on its worker.
- GPU readback bytes move into a shared immutable pooled buffer after all other
  readback consumers finish. The handoff is a vector swap, with recycled
  buffers returned after their last consumer. CPU fallback still copies its
  reused presentation buffer; this remaining copy has not been called zero-copy.
- OCR results carry prepared scene-space boxes/reference patches. Admission
  against a newer probe has a 3 ms search budget; unverified lines stay hidden
  until a later worker outcome verifies them. Rendering never waits for either
  worker. These are scheduling budgets, not sensor-to-photon guarantees.
- Source retirement immediately revokes both worker generations and clears
  glyphs/reading text. If presentations otherwise stop for 600 ms, a watchdog
  hides cached words and reading-pane text. Only an explicit successful
  presenter receipt restores an unchanged, validated scene; submission attempts,
  control toggles and worker completions cannot restore it. Failed presentations
  hide both surfaces immediately. The watchdog uses successful receipts only.
- Probes are requested at most every 80 ms and only when no UCT copy is
  outstanding (1 s timeout, like the other readback users). If the ring is
  full the presentation is not blocked; the next presentation retries.
- Recognition runs on one worker with at most one job in flight and one
  latest-only pending slot. A busy worker only receives a replacement after
  the probes saw the scene change, so slow OCR cannot accumulate work.
- Cancellation (source switch, geometry change, disable, unavailable backend)
  increments the scheduler authority immediately. In-flight input copies stay
  owned by the worker until the native call returns; Windows OCR is asked to
  cancel and is still waited for (bounded) before the bitmap reference drops.
- Scheduler destruction waits at most 1.5 s and otherwise detaches the worker,
  which keeps its own state and inputs alive until it finishes.

### Provenance and coordinate invariants

- `UctSceneProvenance` snapshots the applied source, rotation, warp revision,
  exact display-LUT generation, dimensions and frame only on successful scene
  processing. Busy/deferred submissions keep the earlier snapshot. CPU fallback
  records rotation without claiming CUDA warps/colors. A mismatch between the
  requested and applied scene revokes words until a matching scene arrives.
  Failed presentations hide glyphs rather than projecting onto unseen pixels.
- Every copy carries a `uct::SourceStamp`: camera session epoch, UCT geometry
  generation, camera frame sequence, request time, the exact transform and the
  copy's pixel size. Jobs, results and accepted lines keep the stamp of the
  pixels they came from; nothing is re-stamped with newly sampled identity.
- Recognized boxes are converted once from copy pixels to normalized
  *processed-scene* coordinates (after rotation, stabilization and keystone;
  before zoom/pan/Fit/Fill/SuperRes sampling). Rendering maps scene → current
  transform → normalized view → overlay logical pixels, so DPI is applied
  exactly once.
- The full-scene annotation transform is used, never the SuperRes ROI remap;
  SR crop geometry therefore never enters text coordinates.
- Rotation, keystone and stabilization are already applied inside the
  processed scene, so UCT never needs to invert keystone matrices: it does not
  leave processed-scene space. Changes to the geometry key (camera session,
  processed scene size, rotation, keystone on/off, stabilization on/off) start
  a new generation and immediately clear the overlay, model and reading pane.
  Continuous keystone tracking and stabilization drift are not trusted from
  matrices; they are verified by the pixel residual check below.

### Acceptance (fail closed)

A word is drawn only when all of these hold:

1. **Same scene**: the result's epoch and geometry generation are current, and
   its scheduler authority was not revoked.
2. **Age**: the result arrived within 4 s of its copy.
3. **Residual**: each recognized line builds a box-filtered reference patch
   from the job's own pixels. If a newer probe exists, the line must still
   correlate (NCC ≥ 0.82) within a ±6 probe-pixel translation search;
   otherwise the result is rejected as *content moved*. Flat (textureless)
   lines are untrackable and never drawn.
4. **Temporal agreement**: the same text at an overlapping scene position
   (IoU ≥ 0.5) from two separate recognitions, or a previously accepted word.
   This checks stability, not correctness: the same engine can repeat a
   mistake.
5. **High-risk tokens fail closed**: standalone `l I | 1 0 O o`, digits mixed
   with confusable letters (`O o I l S s B Z z G g q`), superscripts, modifier
   letters and any symbol outside plain punctuation (math operators, `^`, `√`,
   `∑` …) are left as original pixels. Plain numbers such as `3.14`, `-12`,
   `1,000` may be shown.

Afterwards every probe revalidates accepted lines. A line that no longer
matches (occlusion, changed board, motion beyond the search window) is removed
at once. A matched line uses the measured translation as its validated motion
`W`. Lines outside a probe are marked unverified and hidden until a probe sees
them. A line is drawn only if its last validation is at most 350 ms old, or
no newer camera frame has been presented since then. During unvalidated
motion the original image therefore shows instead of floating text. The
staleness bound for sudden motion is one probe interval plus readback latency.

Box-filtered cells (via a per-probe summed-area table) cover the same scene
area at any zoom, so a pan or zoom after acceptance revalidates without new
recognition.

### Unsupported modes

- **Temporal Smooth** and **Extra Stable** (bump hold) present multi-frame
  composites whose provenance UCT cannot state truthfully. UCT pauses, clears
  its text and says so.
- The debug composite view registers consistently (same pixels and transform)
  but is not a supported reading mode.

## Files

| File | Role |
| --- | --- |
| `include/okuflow/common/ultra_clear_text.hpp`, `src/common/ultra_clear_text.cpp` | Stamps, geometry, probes, patches, scheduler, overlay model (Qt Core/Gui only) |
| `include/okuflow/common/uct_windows_ocr.hpp`, `src/common/uct_windows_ocr.cpp` | Qt-free C++/WinRT `Windows.Media.Ocr` wrapper (no PCH) |
| `src/common/uct_default_recognizer.cpp` | Recognizer adapter: grayscale, dark-background inversion, size limit |
| `include/okuflow/app/ultra_clear_text_controller.hpp`, `src/app/ultra_clear_text_controller.cpp` | UI-thread runtime: scheduling, freshness, status |
| `src/app/app_ultra_clear_text.cpp` | Production wiring and per-presentation context |
| `include/okuflow/ui/ultra_clear_text_overlay.hpp`, `src/ui/ultra_clear_text_overlay.cpp` | Click-through glyph overlay |
| `include/okuflow/ui/ultra_clear_text_panel.hpp`, `src/ui/ultra_clear_text_panel.cpp` | Accessible controls and reading pane |
| `tests/ultra_clear_text_tests.cpp`, `tests/ultra_clear_text_test_support.hpp` | Desktop-free contract fixture and synthetic page generator |
| `tests/ultra_clear_text_windows_ocr_tests.cpp` | Optional real-backend acceptance |

Shared-file touch points: `app.hpp` (context helper, controller member
declared last), `app_bootstrap.cpp` (one initialization call),
`app_pipeline_runtime.cpp` (present/readback hooks), `main_window.{hpp,cpp}`
(panel in the Text clarity section, overlay creation), `cmake/CMakeLists.txt`
(`windowsapp`, no PCH for the WinRT unit), `tests/CMakeLists.txt`.

## Validation

Desktop-free, safe to run beside other work (`QT_QPA_PLATFORM=offscreen`,
no native windows, no camera, no network):

- `ultra_clear_text_contract` (label `headless`): geometry round trips through
  Fit/Fill/zoom/pan and DPI; patch tracking, motion and change rejection;
  high-risk tokens; temporal agreement; stale-scene, late and moved-content
  rejection; freshness expiry; reprojection; reading order and omissions;
  latest-only coalescing; cancellation without releasing input; bounded
  destruction with a held recognizer; and the production controller with real
  overlay/panel widgets: held OCR while 300 presentations pan continuously
  (overlay follows every transform, one job in flight), coalescing while the
  scene changes, reprojection at 200% DPI, source switch and rotation
  invalidation, motion and starved-probe hiding, composite pause, Show
  original, unavailable backend, CPU-path pixels left unmodified, no plate
  over the synthetic curve, the presentation watchdog and bounded 4K probe
  work.
- `ultra_clear_text_windows_ocr` (labels `headless;ocr-runtime`): real
  `Windows.Media.Ocr` through the production scheduler on synthetic printed
  pages, dark-on-light and light-on-dark, each also tested with an
  opposite-brightness surround that dominates the frame. It skips when Windows has no OCR
  language; a skip is not a pass.

The native `main_window_interactions` UCT cases check control geometry and
overlay alignment through Fill-mode pan/zoom, resize and owner hiding, using
synthetic accepted words and composed native-window screenshots. They do not
exercise a physical camera or the recognition engine.

Not yet validated: NVDA/Narrator reading of the pane and status, real camera
text on paper and boards, GTX 1070 and RTX
desktops, high-DPI multi-monitor overlay placement, long-session CPU cost,
p50/p95/p99 presentation tails with UCT on, and any CER/WER measurement.

## Not implemented (deferred)

Curved pages, handwriting/cursive, complex math or diagrams, per-character
confidence (Windows OCR exposes none), languages beyond the installed Windows
OCR languages, rotation-aware tracking beyond translation, a frozen-region
reading mode, persistence of the opt-in, and translations of the new strings
(see [Localization](localization.md#ultra-clear-text-strings-pending-review)).
Cross-decimation rounding/centroid precision also remains follow-up: measured
synthetic production pairings stayed above the match threshold but showed up to
3.35 source pixels of residual offset (under one probe pixel). See the
[independent review evidence](engineering_audit_closure_2026_10_17.md#independent-uct-review-and-regression-repairs).

## Follow-up verification

The parent independently exercised the original contract and real Windows OCR
fixture, then completed the interrupted Opus worker/provenance refactor. Added
contracts cover deferred-frame snapshots, CPU applied settings, immediate
source retirement, stable mixed-height reading order across all permutations,
stale reading-pane exclusion and measured-color updates without word movement.
The cached-glyph test actually repaints 300 panned frames while OCR is held,
checking that accepted glyphs remain and no OCR completion advances rendering.
Its p95/p99 callback/paint observations are fixture measurements on this PC,
not acceptance on a GTX 1070 or physical moving page. Dense 4K residual searches
are measured separately from their small UI handoff. Raw failures from the
unfinished refactor and fixture corrections remain in the private audit logs.

A recorded parent CPU fixture run showed cached-glyph `OnPresented` p95/p99
0.005/0.006 ms and 1920x1080 Qt paint p95/p99 2.829/2.943 ms while OCR was held.
The separate 52-line 4K residual diagnostic measured p95 58.12 ms; this is why
that work runs on the probe worker. These observations are one development-PC
fixture run, exclude capture/GPU/display scanout, and are not end-to-end FPS.

Live synthetic-camera review found that independently fitting every word's
height distorted ascenders/descenders. Glyph layout now fits a shared type size
and baseline per accepted OCR line using cached reference-font ink metrics.
Pan/zoom only reprojects that cached geometry; it does not measure fonts or run
recognition. Word backgrounds cover both original ink and the replacement ink,
and all backgrounds are painted before letters.

Both passes use the same eligibility decision: the original box and fitted ink
must be usable at the current scale and survive native clipping. Otherwise the
original camera pixels remain visible. Font ink metrics, readable colors and
the reference font are cached when words or weight change; viewport updates
only project and paint that cached data. Recognition updates also preserve the
reading pane's selection direction, caret and scroll through unchanged text.
Cursor endpoints are snapshotted as integer UTF-16 offsets before replacing
the document: copied `QTextCursor` objects remain live and otherwise move with
the replacement. Changed-middle remapping avoids splitting surrogate pairs.

Shared line height and baseline do not preserve each font's natural horizontal
aspect: words still fit their recognized widths. A future aspect-preserving
layout must work in physical pixel space and protect omitted neighbors before
widening any replacement. Foreground/background inference remains heuristic;
the dense-word inversion regression is repaired, while textured surroundings
and subpixel strokes need broader real-camera evidence. Stroke-control
placement and translated UCT controls also need separate follow-up.

### Reading-pane omission positions

Accepted lines retain source token order separately from drawable glyphs.
Unconfirmed and high-risk tokens leave an ellipsis at their original position
(consecutive omissions collapse), so `Take 1 tablet` reads `Take ... tablet`
with a single typographic ellipsis, never `Take tablet ...`. Show original
keeps verified reading text available while hiding glyphs; presentation failure
or timeout hides both until a confirmed successful presentation.
