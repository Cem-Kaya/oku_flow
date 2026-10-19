# Ultra Clear Text (experimental, development)

## Selected-video and held-frame authority (development)

Saved playback and same-session recording rewind use the same UCT scheduler
with owned immutable source pixels, document identity and selection generation.
Each selected frame is recognized once, with at most one genuinely distinct
neighbor pass for glyph corroboration. Duplicate pixels or a new job identifier
alone cannot corroborate an immutable frame. Without independent evidence,
reading text can be shown in the pane while the original source ink remains.
Completed unchanged stills schedule no periodic OCR or validation jobs; pan and
zoom only reproject their retained scene artifact. The immutable job deadline
does not expire an otherwise valid still because the live camera aged.

Rejected or uncertain OCR spans remain ordered omissions (Ã¢â‚¬Â¦). Their source
rectangles protect original ink from replacement plates. Valid replacements
and their plates share one drawability decision. See
[uncertainty handling](uct_uncertainty_repair.md) and
[immutable controller](uct_immutable_controller.md).

UCT also supplies passive, positioned text elements to Qt's Windows UI
Automation bridge. A render-viewport group contains read-only text lines;
character geometry comes from the last actual glyph paint. Semantic additions
use a one-second cadence with a bounded three-second reader hold, while loss
of validation immediately removes text. It never requests speech or focus.
See [screen-reader text and native acceptance limits](uct_screen_text_accessibility.md).

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

- **Advanced Ã¢â€ â€™ Image Ã¢â€ â€™ Text clarity Ã¢â€ â€™ Ultra Clear Text (experimental)**. Off by
  default and not persisted: it starts off on every launch.
- **Show original camera text**: always available while UCT is on; hides the
  sharp letters for comparison without stopping recognition.
- **UCT advanced settings** below those switches has independent GPU and CPU
  text-detector and text-recognizer selectors. Each offers PP-OCRv6 tiny, small
  and medium. GPU defaults to medium/medium; CPU defaults to tiny/small. Choices
  persist across launches, including migration from settings without these keys.
  Changing a choice while UCT is on cancels the old worker's authority and
  starts a fresh backend chain asynchronously. Camera presentation keeps running;
  recognition must become ready and revalidate text before replacement glyphs return.
- A polite status line (starting, waiting for steady text, reading, showing N
  words, text moved, paused, unavailable) and a read-only, keyboard-selectable
  **Recognized text** pane with the accepted words in reading order. An
  ellipsis (Ã¢â‚¬Â¦) marks words that were left as the original image.
- The status text and its accessible name change immediately together. Routine
  states update silently; important states retain polite announcements. A
  diagnostic timer no longer delays the status exposed to a cached screen-reader
  interface. The recognized-text pane keeps its stable label while exposing the
  current recognized content through its text interface. Offscreen regression
  coverage is separate from native Windows UIA/NVDA acceptance.
- Glyph weight follows the existing Text Clarity **Stroke weight** slider.
  Plate colors are measured from the analysis pixels, using a bounded
  surrounding margin and its background-class median to identify dense/bold print. CUDA's
  neutral analysis branch precedes display color effects, so its plates preserve
  source polarity rather than a display LUT. Below 7:1 contrast, ink moves to
  white or black first (neutral ink always snaps to that extreme) while retaining
  the source paper/board color. Only when
  that cannot reach 7:1 does the background move the minimum distance toward
  its polarity endpoint. Moderate gray backgrounds therefore stay gray. Accepted
  background brightness uses a fixed 64-level grid preserving hue; small color
  changes remain within an 8-level hysteresis band rather than shimmering on
  every recognition. This affects replacement colors, not the camera image.
- Paddle detector/CTC boxes include padding. After inference, bounded worker-side
  source-ink fitting uses row bands to avoid adjacent-line ink and a one-CTC-step
  search bounded by the line and neighbouring words to recover clipped edges
  before glyph layout; native
  Windows OCR boxes keep their existing geometry. This is a local printed-ink
  estimate, not character outlines or a guarantee for handwriting/curved text.

## Recognition backend

### PaddleOCR development backend (2026-10-18)

New builds select **TensorRT CUDA Ã¢â€ â€™ ONNX Runtime CPU Ã¢â€ â€™ Windows OCR**. The GPU
and CPU paths use their own saved model pairs (medium/medium and tiny/small by
default). CPU OCR serves while GPU engines prepare. A failed or unavailable GPU
uses the configured CPU pair; Windows OCR is the final fallback. Model size is
not a guarantee of recognition quality. Language coverage also differs by tier;
the tiny recognizer, for example, lacks Japanese support offered by larger tiers.
Unavailable/failed backends retire for the current recognition session;
copy/input faults drop only that frame. Cancellation does not start fallback.
Each fallback candidate initializes at most once per session; GPU promotion
does not resurrect a CPU backend that already failed. A rejected owned-input
or readiness check preserves healthy GPU engines.
UCT off/on starts a fresh session without joining a native call on the UI.

Run `pwsh -NoProfile -File scripts/prepare_uct_runtime.ps1`, optionally with
`-TensorRT` for the external CUDA-12.9 TensorRT SDK. Files stay in
`%LOCALAPPDATA%/OkuFlow/ocr-deps` and `ocr-runtime`. Reconfigure CMake after
preparation. No network/model downloads happen when enabling UCT.
Preparation verifies all six models and their model cards. Legacy `det/` and
`rec/` hold tiny detection and small recognition; `det/small`, `det/medium`,
`rec/tiny` and `rec/medium` contain the other choices. Missing or tampered
selected files make that backend unavailable; arbitrary paths/models are rejected.
`OKUFLOW_UCT_ROOT` overrides the model/CPU directory;
`OKUFLOW_UCT_TENSORRT_ROOT` overrides the external TensorRT SDK.
Build-time `OKUFLOW_UCT_SDK_ROOT` selects the private header cache.
SDK-free builds retain Windows OCR. See [third-party terms](THIRD_PARTY_LICENSES.md).
`OKUFLOW_UCT_REQUIRE_PADDLE_CPU` and `OKUFLOW_UCT_REQUIRE_TENSORRT` make missing
build dependencies fatal for an explicitly required adapter. CUDA DLL discovery
uses runtime `OKUFLOW_UCT_CUDA_BIN`, then `CUDA_PATH_V12_9/bin`, then the local
documented CUDA-12.9.2 installation, never a compiled-in developer path.

TensorRT uses a private low-priority stream, float32 graphs, device I/O,
CUDA normalization, connected regions, rectification and CTC reduction.
Only a region count is downloaded between networks; token/box metadata
comes back afterward, never the full model logits. Engines prepare on a
separate owned worker; CPU OCR serves until preparation
finishes, then GPU is promoted between jobs. TensorRT's progress monitor
cooperatively cancels when the backend session ends; it cannot preempt every
vendor call. Cache identity binds model, GPU UUID, driver,
TensorRT version and precision; cached bytes also have an integrity digest.

CUDA analysis captures the geometry-correct scene before appearance effects,
SuperRes and display zoom into two owned device slots. Busy slots skip an
opportunity. CPU presentation also offers its pre-zoom scene. Complete-scene
context preserves words even when only part is visible at 16x. Source,
processing and frame identity remain bound through worker submission.

**Transfer boundary:** TensorRT device input, normalization, detector/recognizer
outputs, color sampling, ink fitting and validation remain on CUDA. The independent
validation reader builds a resident summed-area probe (long edge at most 960),
not a host image. Line descriptors also stay resident. Workers return compact
colors, fitted rectangles, match/similarity scores, text and provenance; neither
full frames, decimated images nor feature arrays are downloaded by this path.
The explicit CPU recognizer/fallback can still request one full image. A failed
resident-analysis operation fails that input; it never silently retries through
CPU readback. A TensorRT recognizer runtime failure can still retire that backend
and run the explicit CPU fallback on the same job, requesting its one image and
reporting the fallback in backend status. This describes the UCT device path, not every capture/export feature
or a claim of literally no device-to-device copies.

UI admission never waits on CUDA: a new line needing newer-scene validation waits
for the probe worker, while unchanged already-validated lines retain their own
proof and freshness deadline. Immutable selected-frame neighbour registration is
computed on the OCR worker before publication and bound to the selected job ID,
line count and exact matching policy. References and validation launch one GPU
block per line in a single bounded batch, rather than waiting on a host fence
for each line. Validation uses 64-line chunks, checking cancellation/time budget
between chunks and preserving completed results; native batches are capped at
1024. Completed camera probes remain reusable after normal consumer cancellation.
Completion deadlines stay on the worker. CPU validation retains its per-line time budget. Probe/patch allocations are
independent of the two analysis slots; retaining overlays does not retain camera
snapshots. Private nonblocking low-priority streams, pinned metadata staging,
stream-ordered allocation on capable devices and one retirement worker keep this
work separate from presentation. Windows fence polling uses a private high-resolution
one-shot wait, avoiding a coarse scheduler sleep per line; it does not change global
timer resolution. Legacy allocation remains a compatibility path.
Unknown completion retains its exact inputs, outputs and DMA endpoints; probe
worker destruction is bounded with callbacks revoked before a detached retirement.

Live capture does not hash pixels. Saved/unknown GPU OCR computes a domain-separated
SHA-256 tree over **all** packed BGRA bytes on CUDA (4096-byte leaves prefixed by
0x00; parents contain up to 16 ordered child digests prefixed by 0x01). Only the
32-byte root returns to the host. CPU input retains active-row SHA-256. Digest kinds
must match before different roots count as independent observations: switching
backends cannot manufacture distinct-content evidence. Allocation padding never
counts as content.

Stable accepted scenes honor the refresh interval instead of inferring on every
capture. Device OCR never retains a pending snapshot while inference runs, keeping
the pool available for live validation. Copy/allocator faults with unknown
completion latch that pool until an explicit UCT reset. No numerical FPS or
latency improvement is claimed from these architecture/functional checks.

GTX 1070 uses CPU OCR: CUDA 12.9 optical-kernel support does not imply Pascal
support in TensorRT. CTC character spans are estimates, not glyph outlines.
Recognition of handwriting/math/curves is not established. Full-scene detection
caps its longest dimension at 960; tiny text in high-resolution frames may
still need a future context/tile strategy. No performance claim follows from
runtime compatibility alone.

### Built-in final fallback

When optional Paddle components are absent or fail, UCT uses `Windows.Media.Ocr` through
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
  optical character recognition (Settings Ã¢â€ â€™ Time & language).
- Recognition runs on the CPU on a dedicated worker thread. It does not use
  CUDA, tensor cores or Maxine, so the GTX 1070 / CUDA 12.9 optical path is
  unaffected; the CUDA pipeline keeps running independently.
- Measured on the development machine (en-US, synthetic 1600Ãƒâ€”900 page):
  about 25Ã¢â‚¬â€œ32 ms per recognition. This is one data point, not a budget.

Repeated backend errors (three in a row) switch UCT to *Text recognition
stopped working*; it then shows only the original image.
An explicit off/on retries a failed or unavailable initialization with a fresh
recognizer; queued callbacks from the retired scheduler cannot overwrite the
new attempt. Ordinary slow recognition does not trigger restart.

## Architecture

```text
camera -> geometry-correct scene -> appearance/presenter -> viewport
                       |
                owned pre-zoom snapshot
                       |-------------------------------|
       latest-only recognition worker        bounded validation reader
       TensorRT / Paddle CPU / Windows         -> CPU ProbeWorker
       + final reference/color preparation     -> motion checks
                       |-------------------------------|
                    UI: bounded admission, cached scene-space words
                       |
                overlay paints on the presentation clock
```

There is no second capture or render pipeline. UCT reuses:

- `PresentLatestCudaScene` / `PresentFitted` for the per-presentation hook,
  the annotation `ViewTransform` (full scene Ã¢â€ â€™ viewport) and a separate owned
  pre-zoom analysis snapshot. UCT no longer requests the presenter's final
  zoomed viewport readback. CPU presentation supplies its pre-zoom scene.
- The annotation overlay's coordinate space and floating tool-window pattern.

### Clocks and non-blocking guarantees

- `OnPresented` runs before each presentation attempt. It compares a small geometry key,
  forwards the current transform to the overlay, enforces freshness and
  decides whether to request one more copy. It never waits for recognition.
  `OnPresentationSucceeded` separately records the presenter's successful
  submission receipt; this does not measure GPU scanout or sensor-to-photon time.
- The overlay holds only accepted scene-space words. Pan, zoom, Fit/Fill and
  resize repaint through the newest transform on the presentation clock.
  The glyph tool window also participates in the immediate native `WM_MOVE`
  path used by floating controls. Its retained pixels translate with the
  viewport before queued Qt move events run, so dragging the owner cannot
  leave text outside the viewport until an OCR/status refresh. This positioning
  is event-driven; pan/zoom continues at the configured presentation rate
  (Auto/120 FPS is capped by the display). No independent OCR or 120 Hz idle
  repaint loop is added; scanout cadence still depends on Windows/the display.
- Probe construction, similarity and residual searching run on `ProbeWorker`,
  with one executing and one latest-only waiting request. A 60 ms per-probe
  search budget leaves new unchecked lines hidden; an already validated line
  retains its prior proof only until the unchanged freshness deadline. A cancellation is
  checked between line searches. OCR preparation also runs on its worker.
- CUDA analysis uses two independently owned device snapshots. Stream events
  protect copies; slots held by a reader/inference job are never overwritten.
  Downloads use private nonblocking streams and pinned host memory on workers.
  Unknown GPU completion retains the complete input/output closure. CPU input
  still uses a bounded immutable pixel pool and an owned scene copy.
- OCR results carry prepared scene-space boxes/reference patches. Admission
  against a newer probe has a 3 ms search budget; unverified lines stay hidden
  until a later worker outcome verifies them. A near-identical refresh can keep
  the complete old object while its existing proof remains fresh and the candidate
  is unverified or no fresher: its
  reference, geometry, identity and validation clock remain together. Proof is
  never transferred to the new unverified reference. Rendering never waits for either
  worker. Each old identity is retained at most once. Probe checks prioritize
  unverified lines, then the oldest proof, so dense-page tails can progress.
  These are scheduling budgets, not sensor-to-photon guarantees.
- Source retirement immediately revokes both worker generations and clears
  glyphs/reading text. If presentations otherwise stop for 600 ms, a watchdog
  hides cached words and reading-pane text. Only an explicit successful
  presenter receipt restores an unchanged, validated scene; submission attempts,
  control toggles and worker completions cannot restore it. Failed presentations
  hide both surfaces immediately. A busy frame slot is a deferred presentation,
  not a fault: the overlay keeps the last successful transform and neither hides
  nor renews the watchdog. An invalid transform or changed viewport size refuses
  that reuse. A different source/geometry still invalidates at once.
  The watchdog uses successful receipts only.
- Analysis opportunities are throttled to 80 ms. Busy snapshot slots skip an
  opportunity instead of blocking presentation. Source/frame/processing
  provenance must match the successfully presented scene before handoff.
- Recognition runs on one worker with at most one job in flight and one
  latest-only pending slot. CUDA input replaces only that pending descriptor;
  CPU admission also uses probe stability. Slow OCR cannot accumulate a queue.
  The independent device-validation reader has the same bounded ownership and
  cancellation rules. Probe outcomes cannot replace a newer scene reference.
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
  before zoom/pan/Fit/Fill/SuperRes sampling). Rendering maps scene Ã¢â€ â€™ current
  transform Ã¢â€ â€™ normalized view Ã¢â€ â€™ overlay logical pixels, so DPI is applied
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
   correlate (NCC Ã¢â€°Â¥ 0.82) within a Ã‚Â±6 probe-pixel translation search;
   otherwise the result is rejected as *content moved*. Flat (textureless)
   lines are untrackable and never drawn.
4. **Temporal agreement**: the same text at an overlapping scene position
   (IoU Ã¢â€°Â¥ 0.5) from two separate recognitions, or a previously accepted word.
   This checks stability, not correctness: the same engine can repeat a
   mistake.
5. **High-risk tokens fail closed**: standalone `l I | 1 0 O o`, digits mixed
   with confusable letters (`O o I l S s B Z z G g q`), superscripts, modifier
   letters and any symbol outside plain punctuation (math operators, `^`, `Ã¢Ë†Å¡`,
   `Ã¢Ë†â€˜` Ã¢â‚¬Â¦) are left as original pixels. Plain numbers such as `3.14`, `-12`,
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
| `src/common/uct_default_recognizer.cpp` | Default automatic selection and Windows adapter: grayscale, inversion, size limit |
| `uct_paddle.hpp`, `uct_paddle*.cpp`, `uct_onnx_cpu.cpp`, `uct_tensorrt.cpp` | Shared Paddle contracts/algorithms, dynamic CPU/TensorRT adapters and fallback |
| `uct_device_image.hpp`, `uct_analysis_pool.{hpp,cpp}`, `uct_kernels.{hpp,cu}` | Owned device snapshot, bounded independent probes, resident GPU pre/postprocessing |
| `scripts/prepare_uct_runtime.ps1`, `scripts/uct/pins.json` | Explicit private dependency preparation and exact upstream pins/notices |
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
- `uct_paddle_processing` and serial `uct_paddle_cuda` cover model-free fallback
  and CPU/device algorithm/ownership contracts. Explicit real model rows and
  independent-device-probe cases are described in [tests/README](../tests/README.md#optional-paddleocr-acceptance-2026-10-18).

The native `main_window_interactions` UCT cases check control geometry and
overlay alignment through Fill-mode pan/zoom, resize and owner hiding, using
synthetic accepted words and composed native-window screenshots. They do not
exercise a physical camera or the recognition engine.

Not yet validated: NVDA/Narrator reading of the pane and status, real camera
text on paper and boards, GTX 1070 and RTX
desktops, high-DPI multi-monitor overlay placement, long-session CPU cost,
p50/p95/p99 presentation tails with UCT on, and any CER/WER measurement.

## Not implemented (deferred)

Curved pages, handwriting/cursive, complex math or diagrams, exact character
outlines, validated multilingual quality beyond the model/installed Windows
language coverage, rotation-aware tracking beyond translation, a frozen-region
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

### Probe progress and resource recovery (2026-10-18)

Only one device candidate may traverse the independent readback and probe
workers at a time. Its exact frame remains owned until validation decides
whether OCR is needed; a newer camera frame cannot replace it during a slow
probe. The readback deadline releases stale admission without a UI wait.
Motion during inference is checked by admission against the newest independent
probe, rather than by comparing the candidate with itself.

Clean snapshot allocation failures retry after 250 ms. Unknown copy completion
quarantines that pool until an explicit UCT reset and retains uncertain memory.
The controller exposes pool failure and repeated probe/OCR input failures using
the existing unavailable-text status; healthy frames recover without retiring
models. Busy leased slots are normal backpressure, not a failure.

Dense detector output retains the first 128 eligible component roots in raster
order on both CPU and GPU. GPU prefix compaction is deterministic and reports
the full eligible count before truncation; it never downloads a probability map.

The helper verifies original SDK extraction against pinned archives. Runtime
TensorRT DLL checks enforce the expected version, not individual DLL hashes;
CUDA dependency folders are owner-controlled. These local paths and the engine
cache trust other processes running as the same Windows user. The exact graph
bytes read by the TensorRT builder are rehashed before parsing.

A valid current CPU analysis image clears a previous CUDA pool failure when the
presentation path falls back to CPU. Invalid buffers cannot clear it. The slow-
probe/input-health fixture also exercises this transition; backend health stays
separate from either input path.

### Final review follow-up

GPU crop emission uses the same PCA-height/width orientation rule as CPU and
GPU classification. The tilted/vertical regression first failed on the prior
implementation, then passed after repair. Component moment sums now use exact
64-bit integer atomics with double covariance evaluation, preventing order-
dependent angle changes from float accumulation.

Snapshot and decimated-readback allocation checks CUDA memory-pool capability.
Unsupported devices use cached legacy allocations; the forced legacy fixture
exercises this branch on the development GPU, including reduced-image readback.
This removes that API prerequisite but does not establish physical GTX 1070
acceptance. Legacy allocation may synchronize on first use or growth; steady
frames reuse buffers, and enable/resize tail performance remains unmeasured.

The CPU-input TensorRT path keeps a persistent upload snapshot pool and preserves
healthy engines for pre-enqueue resource faults. The scheduler chooses device vs
CPU input once per job. Clean allocation pressure becomes visibly failed only
after three consecutive attempts; unknown completion remains immediately failed.
Rejected device submission releases its candidate immediately. Unused same-frame
similarity state/computation was removed; device-input provenance remains a test
and diagnostic field.


## Steady live-camera regression repair (2026-10-19)

The saved-video duplicate-evidence policy had also been applied to live capture:
UCT required two different complete image hashes before confirming a new word.
A steady virtual camera or screen-based lecture feed can legitimately deliver
byte-identical frames, leaving text permanently pending despite repeated OCR.
The original three offscreen CPU/device controller cases reproduced that failure
on the pre-fix code. Two more resident-oracle rows cover the new GPU protocol.

Live UCT now uses the trusted capture owner's acquisition sequence: two matching
recognitions from strictly increasing samples under the same camera/geometry and
scheduler authority can confirm unchanged text. Repainting one sample, changing
only a job ID/timestamp, or crossing a source/settings/admission-mode boundary
cannot supply new proof. Freshness, content registration and high-risk-token
rules remain active. File playback and selected-frame neighbors retain their
stricter exact-pixel duplicate checks.

A `liveCameraCapture` stamp also lets the worker skip full-image SHA-256 on live
OCR inputs; validation-only probes no longer hash pixels either. Default/saved
OCR inputs keep their worker hash. Hashing previously scanned the active BGRA
rows of the already available post-recognition reference image; it did not
require an additional GPU readback, and it never ran on the UI/presentation
thread. Removing it saves that CPU scan, but no numerical latency improvement
is claimed. The resident transfer boundary above replaces the former post-GPU
CPU color/reference and validation-image copies. CPU fallback still uses pixels.


### Resident-path regression coverage (2026-10-19)

The window-free CUDA fixture guards both image-download entry points and checks
GPU scheduler/probe publication with no host image or descriptor array. It compares
fitted words and sampled colors with the CPU implementation across three lines,
both polarities, detached i/j/accent marks and rejected middle words. It validates
resident references, compact CPU-reference uploads, invalid-entry batch indexing,
cancellation, and motion/occlusion/cropped-view parity at decimations 1, 2 and 4.
Scene-edge box filtering uses matching double precision and a 1e-7-pixel roundoff
allowance, preventing algebraically identical border samples from changing visibility.
A CPU SHA-256-tree oracle checks every GPU digest, including source pitch padding,
multiple tree levels and a changed pixel outside probe sampling. Retaining eight
resident probes must leave the two capture slots reusable. Offscreen controller
oracles exercise live camera, return from video and immutable-neighbour registration
without UI-thread analysis or image readbacks. These are not physical camera,
interactive screen-reader, or measured presentation-latency acceptance.


### One-time resident batching measurement (2026-10-19)

On the development RTX 4090 (CUDA 12.9, Qt 6.12/MSVC 2022), the explicit
60-line 1280Ã—2160 synthetic-page experiment compared the first resident version
(one host fence per line) with batched references/checks. One warm-up preceded
three measured trials; all 60 registrations passed. Inputs and known OCR output
were identical, with already fitted/unpadded word boxes (no ink-refinement work
in this experiment). Times include colors/reference preparation and checks, excluding
real inference, input capture/upload, display/VSync and sensor-to-photon latency.

| Worker operation | Per-line GPU fences | Batched GPU | CPU oracle in batched run |
| --- | ---: | ---: | ---: |
| Prepare 60 references + metadata | 69.0â€“80.1 ms | 2.8â€“3.8 ms | 8.1â€“8.5 ms |
| Validate 60 lines | 65.2â€“65.7 ms | 1.29â€“1.45 ms | 3.5â€“3.7 ms |

This establishes the batching benefit for this fixture/GPU, not a general OCR
speed or frame-rate claim. Ordinary CTest does not repeat the timing experiment.
Raw attempts are retained under LocalAppData's `uct-camera-repair-20261019`
evidence folder (`resident-dense60-latency.txt` and
`resident-dense60-batched-latency.txt`). Real medium/medium TensorRT OCR is checked
separately by `optionalRealGpu` with resident preparation and registration.


Background acceptance for this repair: tracked `agent_build.bat` with the desktop
excluded passed Release compilation, 108 CPU cases (two cases have skipped Qt
rows) and the 17-case CUDA delta, with exact-fingerprint pass reuse. Scoped
8-case CPU and actual CUDA/real TensorRT resident checks also passed. The original
steady-camera fault and the intermediate crop-parity failure are retained in
local evidence alongside their repairs. No native GUI, physical camera/phone or
interactive screen-reader acceptance was performed in this desktop-reserved run;
this is partial validation, not the full release gate or a published installer.
