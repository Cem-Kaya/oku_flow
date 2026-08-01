# OpenZoom

OpenZoom is a Windows-only camera magnifier built around Qt 6, Media Foundation, Direct3D 12, and an optional CUDA processing path. The current codebase already supports live camera capture, GPU (CUDA) frame processing with CPU format-conversion support paths, a two-stage preset/advanced UI, rotation-aware presentation, persistent settings, paired photo snapshots, and live AV1/H.264 recording with optional microphone audio in fragmented MP4 containers.

## Current Capabilities
- Live-switchable English, Turkish, and German UI under
  `Advanced > Image > Application`. The flag-and-native-name selector changes
  visible labels, accessible names, status announcements, AI response
  language, locale-formatted values, and the preferred Read Aloud voice
  immediately without restarting. Debug logs, file names, device names, model
  ids, and persisted setting tokens remain stable English/data values.
- Media Foundation camera enumeration with per-device mode listing (`width x height @ fps`), restart-safe device activation, plain-language failure reporting, and automatic reconnection: when a camera drops mid-lecture, OpenZoom quietly retries the same physical device for about 30 seconds (2s/4s/8s backoff) without any modal dialogs, and only reports failure if the device never comes back.
- CPU frame pipeline for format conversion and rotation of formats the GPU path does not consume, plus the legacy debug composite view. NV12 and YUY2 camera frames bypass it entirely: color conversion and rotation run in CUDA.
- Direct3D 12 presenter for swap-chain output plus GPU texture readback.
  Processed and rotation-correct unenhanced video recording use a bounded
  384 MiB pool of shareable GPU canvases and feed Media Foundation hardware
  encoders without a GPU-to-CPU-to-GPU round trip. Encoder sample release
  recycles each texture; pool pressure drops and counts a recording frame
  instead of stalling preview. DXGI recording samples are converted to
  encoder-native NV12 and submitted with an explicit valid-buffer length, as
  required by hardware Media Foundation encoders. Photos and on-demand
  analysis use the readback path, then hand image encoding, serialization, and
  file writes to bounded worker pools so that work does not stall the UI.
- CUDA external-memory interop path with GPU color conversion (NV12/YUY2) and rotation, video stabilization, automatic keystone correction for projected screens, black-and-white, zoom, Gaussian blur, temporal smoothing, auto contrast (percentile level stretch), low-vision display color modes with contrast/brightness, focus marker, and spatial sharpening via NVIDIA NIS or AMD FSR 1.0 style kernels.
- GPU video stabilization uses full-strength CUDA Harris/Lucas-Kanade feature
  tracking and device-side RANSAC to lock a mounted camera to one fixed GPU
  reference, so long-run estimator drift is impossible by construction.
  Enabling `Stabilize Image` automatically enables this fixed-reference lock.
  The former pairwise path filter, NVIDIA Optical Flow engine,
  projection fallback, rolling-shutter heuristic, and their controls have
  been removed. Optional
  `Extra Stable` keeps the tracker live but temporarily presents the last
  sharp, settled GPU frame during a detected impact or focus loss, then
  crossfades back after recovery. Because it can briefly freeze moving people
  or content, it is explicit, off by default, and never saved in a preset.
- CUDA Text Clarity for camera text: background flattening, soft adaptive
  Sauvola thresholding, automatic text polarity, stroke weight, smart
  sharpening, CLAHE, two-color reading, anti-shimmer hysteresis, selective
  text-edge sharpening, glare suppression, and asynchronous focus detection.
- Optional NVIDIA Maxine SuperRes replaces NIS/FSR for zoomed text when the
  separately installed Video Effects runtime is available. It runs on the
  existing CUDA stream, discards 10 warmup samples, and automatically falls
  back if its next 60-frame average exceeds the 24 ms latency target. Advanced
  reports the measured average and offers a compact checkbox override when
  latency is the only failure.
- Two-speed UI: Simple mode gives the full client area to the live view and overlays three auto-fading primary clusters plus contextual screen-correction controls; Advanced keeps the camera visible beside a narrow inspector containing every parameter and pipeline diagnostics. The chosen mode persists.
- Stage-1 quick modes backed by full stage-2 advanced configurations, including promotion of advanced tuning into user-defined quick options.
- Local OCR via Tesseract plus scene explanations through either a signed-in Codex CLI/ChatGPT subscription or an OpenAI-compatible HTTP endpoint. Results stream into a focusable assistive panel and Advanced Assistant, can be spoken aloud, and can be written to `Documents\OpenZoom\Notes\`. The non-blocking Setup Assistant can install Tesseract, Codex CLI, and NVIDIA Video Effects without putting those optional runtimes in an OpenZoom release bundle.
- OCR and VLM frame preparation is bounded and asynchronous: PNG/JPEG
  encoding, resizing, base64/JSON creation, and temporary image writes happen
  outside the UI thread, with cancellation tokens preventing stale work from
  starting a request.
- Session persistence in `%APPDATA%\OpenZoom\OpenZoom\settings.json`.
- One user-owned root at `Documents\OpenZoom\` for photos, recordings, notes,
  analysis exports, and opt-in console debug logs. The root is configurable,
  and `Ctrl+Shift+O` opens it from anywhere in the application.
- A branded, high-contrast magnifier icon embedded in the Windows executable
  at multiple display sizes and assigned through Qt for the title bar,
  Alt-Tab, and taskbar.

## Status
CUDA is the processing path; the CPU effects pipeline is deprecated. When the D3D12/CUDA interop surface cannot initialize, the app presents unprocessed passthrough video and shows a persistent "GPU required — processing disabled (showing raw video)" notice instead of silently degrading. The CPU debug composite view remains available as a diagnostic.

## Prerequisites
- Windows 10 or Windows 11.
- Visual Studio 2022 with the Desktop C++ workload and Windows SDK.
- Qt 6.9.3 for `msvc2022_64`, or matching overrides via `QT_PREFIX` / `Qt6_DIR`.
- CMake 3.23 or newer.
- NVIDIA GPU plus CUDA Toolkit 13.x if you want the CUDA path.
- Optional: NVIDIA Video Effects runtime for Maxine SuperRes, Tesseract OCR
  for local text recognition, and Codex CLI for subscription-backed Explain
  and Assistant features. OpenZoom offers verified vendor downloads for all
  three from `Setup & Downloads`.

## Build And Run
From a Visual Studio x64 developer prompt or a PowerShell 7 session (`pwsh.exe`) with MSVC, Qt, and optionally CUDA on `PATH`:

```bat
scripts\build_and_run.bat
```

From the WSL/Linux agent shell, run Windows-side commands through PowerShell 7
with `pwsh.exe -NoProfile -Command '...'`, for example
`pwsh.exe -NoProfile -Command 'Get-Date'`.

The helper script:
- configures `build\` with the Visual Studio 2022 generator,
- clears stale CMake cache entries when the source path changes,
- builds `open_zoom`,
- prefers `build\cmake\Release\open_zoom.exe` when launching,
- runs `windeployqt` automatically when it can find the Qt runtime.

If Windows reports missing `Qt6*.dll` files, add the Qt `bin` directory to `PATH` or point `QT_PREFIX` / `Qt6_DIR` at the correct installation.

## Alternative Builds
### CMake presets

```powershell
cmake --preset msvc-release
cmake --build --preset msvc-release-build
```

Available presets live in [`cmake/CMakePresets.json`](cmake/CMakePresets.json):
- `msvc-debug`
- `msvc-release`
- `msvc-cpu`
- `msvc-cuda-tests`

Run the complete local compile and test matrix from a plain Windows command
prompt with:

```bat
scripts\agent_build.bat
```

The tracked helper locates Visual Studio with `vswhere`, compiles the shipping
CUDA configuration, runs the CPU suite, and then builds and runs the
CUDA-enabled suite. The two real CTest presets treat an empty test directory as
an error:

```powershell
cmake --preset msvc-cpu
cmake --build --preset msvc-cpu-build
ctest --preset msvc-cpu-tests

cmake --preset msvc-cuda-tests
cmake --build --preset msvc-cuda-tests-build
ctest --preset msvc-cuda-tests
```

### CPU-only build

```powershell
cmake -S . -B build-cpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DOPENZOOM_ENABLE_CUDA=OFF
cmake --build build-cpu
```

`OPENZOOM_ENABLE_TEXT_SR=ON` compiles the runtime-only NVIDIA Maxine SuperRes
adapter and its controls. CUDA presets and the release-bundle script enable it
by default; the CPU preset disables it. Set the environment override explicitly
to `OFF` only for a bundle that intentionally omits the adapter. OpenZoom links
no Maxine import library and ships no proprietary runtime or model files.

### Release bundle

```bat
scripts\build_release_bundle.bat
```

This builds the application and tests in one release tree, requires CTest to
pass, deploys into a staging directory, validates the Qt runtime and license
files, and only then publishes `dist\OpenZoom\`. If that bundle is locked by a
running app, the complete tested build is published as `dist\OpenZoom2\`
without terminating or relaunching OpenZoom. Set
`OPENZOOM_SKIP_BUNDLE_TESTS=1` only for an emergency build; the script prints
`WARNING: UNTESTED BUNDLE` when that explicit escape hatch is used.

The published bundle contains `open_zoom.exe`, Qt runtime files,
`LICENSE`, `README.txt`, `THIRD_PARTY_LICENSES.md`, and the bundled Lucide icon
notice. It also contains `SHA256SUMS.txt`, `release-manifest.json`, and
`SBOM.spdx.json`, generated from the exact staged files before publication.
Private/team bundles may remain unsigned. To sign with a certificate already
installed in the current user's Windows certificate store, set
`OPENZOOM_SIGN_CERT_SHA1` to its thumbprint before running the script. A
self-signed code-signing certificate is suitable for the current small team
when teammates explicitly trust its public certificate; never distribute the
certificate's private key. Setting `OPENZOOM_PUBLIC_RELEASE=1` makes a missing
or invalid signature a packaging failure rather than silently publishing an
unsigned public build.

Release currently retains a diagnostics terminal while stabilization
is being field-tuned. Stabilizer output is rate-limited to one detailed sample
per 30 camera frames rather than printing per frame. The CUDA runtime is linked
statically. NVIDIA Video Effects, Tesseract,
and Codex CLI binaries are never copied into the bundle; users obtain them from
their vendors through the Setup Assistant. The Tesseract installer and
OpenAI's Codex bootstrap script are pinned and SHA-256 verified before
execution. The verified Codex bootstrap then verifies the selected official
release package against OpenAI's checksum manifest.
An existing `dist\OpenZoom\output\` is preserved and restored around the
rebuild; photos, recordings, notes, and analysis files are user data and are
never intentionally removed by packaging.
If Qt's transfer fails, Setup retries with the Windows downloader and then the
vendor's alternate host without weakening verification.

## Runtime Controls
- `Simple` / `Advanced` switches between a full-view overlay UI and a right-side inspector. The live camera remains visible in both states.
- `Application language` under the Advanced Image tab's `Application`
  section offers `English`, `Türkçe`, and `Deutsch` with their flags. The
  selection is global, persists across restarts, and updates the open windows
  in place. On first run OpenZoom follows a supported Windows display
  language, otherwise it starts in English. An AI request already in progress
  finishes in its old language; the next request follows the new choice.
  The language registry also carries locale layout direction, so future RTL
  catalogs can use the same picker and persistence path. RTL languages are not
  offered yet because each still needs native wording, typography, and
  screen-reader validation.
- `Viewport framing` under `Advanced > Image > Device > More device options`
  is a global viewport preference:
  `Fill (crop)` fills the camera area without distorting it, while `Fit (show
  all)` preserves the complete frame with symmetric black bars. Resizing,
  rotating, or dragging the Advanced divider always preserves the camera
  aspect ratio.
- `Viewport motion rate`, beside framing under `More device options`, controls
  how smoothly pan and animated zoom move over
  the latest processed scene: Auto (up to 120 FPS), 60, 90, 120, or Match
  display. The active monitor clamps unsupported choices and OpenZoom reports
  the effective rate. This does not invent camera frames: a 30 FPS camera is
  still 30 FPS; only navigation over its newest completed frame is refreshed
  more often. Auto reduces to the camera rate while idle.
- `Processed recording resolution`, in the global `Recording` section, keeps
  the processed video independent of window, monitor, DPI, and inspector size.
  `Match camera (recommended)` follows the rotation-correct source; fixed
  640x360, 854x480, 1280x720, 1920x1080, 2560x1440, and 3840x2160 choices
  retain their dimensions for the entire segment. Portrait input swaps each
  fixed canvas to portrait orientation. The adjacent `Camera and original
  recording resolution & frame rate` selector controls the live camera,
  original photo, and original video dimensions.
- `Microphone`, in the same global Recording section,
  selects which Windows audio capture endpoint is recorded. New settings use
  the Windows system-default microphone; `No microphone (video only)` records
  silent video. OpenZoom opens the microphone only while recording.
- In Simple mode, the switch, profile carousel, and
  Photo/Record/Explain/Read/Draw actions occupy three flush view corners.
  Keystone profiles add a separate Previous/Stop/Next correction strip beside
  the carousel. Pipeline status stays in Advanced diagnostics so it never
  covers the camera. The solid, high-contrast chrome fades after about five
  seconds idle and returns on mouse, keyboard, focus, or application activity.
  Repeated pointer activity only extends that deadline; it does not relayout
  the floating controls or reduce viewport motion rate.
- `Draw` opens an accessible annotation canvas over the camera. Freehand,
  straight-line, rectangle, ellipse, and text items use processed-scene
  coordinates, so they stay attached to lecture content while the viewport
  pans or zooms. A compact icon-and-label tool rail sits flush against the
  left edge. Pen, Line, Shape, and Text open a transient options flyout with a
  large checked color grid, thickness or text-size control, Solid/Dashed
  style, and shape choice as applicable. The flyout closes when drawing
  starts and never hides on a timer.
- Recording remains available while Draw is active. Ink is alpha-composited
  into the processed MP4 at its selected recording resolution; the paired
  original MP4 remains a clean camera recording without annotations or
  selection handles.
- Middle-button drag continues to pan the camera while Draw is active. Text
  follows a canvas-first workflow: choose Text, click the desired scene
  position, type in the inline editor, then press Enter to place the label
  (or Escape to cancel). Pending text is committed before Save, Clear, or
  Done.
- Move selects one complete item, or drag an empty part of the view to draw a
  marquee around multiple items. A selected group moves, nudges, deletes, and
  undoes as one edit; a single selected item also shows eight resize handles.
  Hold Shift or Ctrl while clicking or drawing a marquee to extend the current
  selection. Erase removes whole
  items. A separate right rail provides Undo, Redo, annotated Save,
  capture-and-Clear, and Done. The persistent Photo/Record/Explain/Read/Draw
  bar remains above the drawing canvas and clickable in both Simple and
  Advanced; Photo can capture the current drawing, Record/Stop controls the
  annotated processed recording, and pressing Draw again leaves drawing mode.
  The bar compresses to icons when the viewport is narrow. Tool/state changes
  use screen-reader announcements and never start TTS.
- `Ctrl+H` pins the Simple controls on screen or restores automatic hiding; `Esc` closes the quick-mode grid.
- The grid button or current profile opens all quick modes as large tiles. Built-in modes use plain-language labels such as `Read a Page`, `High Contrast`, `Sharpen Text`, `Keep It Steady`, `See in Low Light`, `Projector Screen`, and `Whiteboard`.
- Number keys `1` through `9` apply the first nine quick modes except while an
  editable text field has focus. A mode change shows a large centered
  announcement and notifies screen readers without starting speech.
- `Text Clarity` in Simple mode automatically selects a paper, board, or
  mixed-content treatment. The `Document` quick mode applies the full
  page-reading stack; use the carousel/grid to reach it after the numbered
  first nine modes.
- `NVIDIA Super Resolution` and its strength slider are profile-owned Advanced
  controls under `Advanced > Image > Text Clarity`. Enabling
  it restores a useful strength when needed and raises zoom to the 1.33x
  minimum. Maxine runs its supported 4/3x AI pass, with any additional zoom
  applied afterward on the GPU. The result is consumed synchronously on the
  application CUDA stream and is not blended with a separately timed base
  frame; residual zoom maps the live pan focus into the AI crop. This prevents
  moving ghost layers. The status row distinguishes the source crop,
  viewport target, final magnification, and measured inference time.
  Maximum source detail is the default. The optional `Faster 2x mode (narrower
  view)` raises the minimum magnification to 2x; for a 1280x720 viewport this
  changes the visible source crop from 960x540 to 640x360. It is a speed and
  field-of-view tradeoff, not a higher-quality mode. `Ultra quality (full
  frame, up to 1440p)` instead allocates a separate high-resolution scene
  cache, runs SuperRes over the complete processed camera frame, and applies
  viewport zoom/cropping afterward. A 1280x720 camera runs 2x into
  2560x1440; a 1920x1080 camera runs 4/3x into 2560x1440; an already-1440p
  camera remains native. The regular scene texture always follows the actual
  post-rotation camera resolution and is never fixed at 720p. Ultra and Faster
  2x are mutually exclusive profile choices. Unavailable or failed runtimes
  fall back to NIS/FSR automatically and report the reason; a slow run can be
  kept on with the compact performance-limit checkbox until SuperRes is
  turned off.
- The bottom-left quick-mode carousel and its full preset grid remain available
  in Advanced mode, so presets can be changed without returning to Simple.
- Advanced has separate `Image` and `Assistant` tabs with wrapping previous/next navigation arrows. `Image` separates Device, Viewport, and Profile ownership, then groups profile tuning under persistent collapsible headings; `Assistant` contains a persistent camera-aware chat plus an OpenZoom-only history list. `Ctrl+F` focuses the pinned settings search, which reveals matching controls even inside collapsed groups. Non-default groups expand and show a changed count so active tuning is never hidden. Each page places the labeled `AI Settings` pop-out in a full-width row directly below the tab strip. Drag the high-contrast divider at the inspector's left edge to resize it; OpenZoom remembers the width. Long setting rows place their slider on a second line instead of clipping text or making part of the track unreachable.
- `Save As Quick Option` promotes the current advanced setup into a reusable stage-1 preset.
- `Reset Tuning` restores profile-owned image and assistive controls to their
  defaults after confirmation. It deliberately keeps the selected camera,
  orientation, viewport motion/framing, and Virtual Joystick preference.
- The question-mark button in the Advanced tab header opens a compact guide
  with Controls first and Features second.
- `Camera` selects the active Media Foundation device from Advanced. Camera
  selection, requested resolution/frame rate, and orientation are Device
  settings rather than part of a quick profile.
- `Resolution & frame rate` requests an actual Media Foundation capture mode.
  OpenZoom reads back the negotiated format and reports when the driver
  selected a different mode. `Automatic` preserves the driver's choice.
- `Camera acceleration`, under `Device > More device options`, defaults to
  Automatic. OpenZoom tries the lower-latency Media Foundation GPU path,
  validates the first 30 frames, and reopens the same camera in compatibility
  mode if negotiation fails or the image is blank. The decision and reason are
  remembered per physical camera. `Always use GPU acceleration` and
  `Compatibility mode` are explicit per-camera overrides.
- After validation, the accelerated preview keeps each Media Foundation
  texture on the GPU. D3D11 converts NV12/YUY2 into a reusable BGRA texture,
  exposes the same NT-shared allocation through D3D12, and CUDA imports it as
  external memory; one device-to-device copy feeds the existing processing
  pipeline. A delayed D3D11 completion query uses safe readback for that frame
  and retries the top rung instead of disabling it. If conversion or external
  import fails,
  OpenZoom remembers the result and drops to the accelerated-copy rung; the
  compatibility reader remains the permanent final fallback. Original
  photos/videos request GPU readback only while those captures are active.
- `Test this camera` temporarily releases the live camera and compares GPU and
  compatibility capture in a watchdog-isolated helper process, then restores
  the live view and reports both average frame-read times. A hung camera driver
  cannot freeze the OpenZoom process. The `Camera acceleration` dropdown is
  the single control for choosing automatic, forced GPU, or compatibility
  capture for the selected camera.
- `Rotation` rotates the pipeline in 90 degree clockwise steps before downstream processing.
- `Black & White` applies thresholded monochrome conversion.
- `Zoom` enables the magnifier and focus-point controls.
- `Gaussian Blur` applies the CUDA blur stage with configurable sigma and supported discrete radii.
- `Temporal Smooth` applies an exponential running average.
- `Stabilize Image` always uses the strongest mounted-camera fixed-reference
  mode at full strength. It registers every frame directly
  against a prepared reference built when the lock is enabled. Locking first
  selects the sharpest of five camera frames, aligns and averages four
  compatible frames while rejecting moving foreground pixels, then prepares a
  real Gaussian pyramid, adaptive subpixel corners, and inverse-compositional
  translation trackers once for the session. Each later solve starts from the
  last accepted absolute pose and prefers features near the visible zoom
  region. A zoom-scaled subpixel deadband and crop-backed correction authority
  suppress shimmer while retaining usable framing. Low-confidence frames hold the last good
  correction instead of drifting or silently replacing the anchor. The fixed
  reference is captured automatically when stabilization starts and is rebuilt
  when the camera or processing pipeline resets; there is no separate re-lock
  control.
- `Straighten Screen (Keystone)` automatically detects a projected slide or screen viewed at an angle and warps it fronto-parallel. Its Previous control freezes tracking and restores an earlier accepted correction; Stop/Continue holds or resumes the live detector; Next restores newer history or samples exactly one fresh correction while stopped. Used by the `Projector Screen` and `Whiteboard` quick modes.
- `Auto Contrast` stretches washed-out projector colors using a percentile level analysis; its strength slider blends toward the full stretch.
- `Text clarity` in Advanced exposes profile-owned controls for background
  flattening, adaptive text, edge softness, text polarity, stroke weight,
  smart sharpen, CLAHE, two-color reading, steady text edges, selective
  sharpening, focus warnings, and glare suppression.
- `Display Colors` opens a compact accessible swatch grid. Built-in reading
  pairs and effects share one GPU luma-LUT model, while the Custom editor can
  create and persist 2-8 stop gradients or stepped posterize schemes. Selection
  changes are announced; merely hovering never changes the camera view.
- Mouse-wheel input scrolls the Advanced panel without changing selectors or
  sliders. Click, drag, and keyboard input still edit those controls.
- `Ctrl+scroll` zoom uses fractional precision-trackpad deltas and geometric
  10% steps. Rapid same-direction wheel events accelerate up to 3x; reversing
  direction resets acceleration. The global `Zoom wheel acceleration` option
  under `Device > More device options` disables only the acceleration, while
  `Ctrl+=` and `Ctrl+-` always use reproducible unaccelerated geometric steps.
- `OCR Assist`, `Scene Explain`, and `Assistive Overlay` drive asynchronous assistive analysis and on-screen text overlays.
- `Read Text`, identified by a speaker icon, runs local OCR. `Explain` sends one temporary, non-history camera question and changes to `Stop` while Codex is working.
- The assistive result panel updates as an answer streams. It is an owned floating tool window: native window movement keeps dragging responsive over the D3D camera surface, and streamed text does not reset its geometry. Drag its header to move it and an edge or corner to resize it. The first placement clears the top Simple controls; later position and size changes persist relative to the camera view and are restored across restarts. Its text can be focused, selected, and read by a screen reader; the question field remains editable while an answer is streaming, but Ask and Enter submission stay blocked until that answer finishes. Follow-ups enter the shared persistent Assistant conversation with the current view attached. Speech starts only when `Read Aloud` is clicked and omits visible section labels such as `Scene Explain` and `OCR`, while the high-contrast Close control or `Esc` dismisses the panel.
- The Advanced Assistant can attach the current processed view, stream answers, stop a response, and manage persistent OpenZoom conversations with resume, rename, export, and delete actions.
- The Advanced Assistant subscription label reports the percentage left in the current Codex usage window.
- `Connect ChatGPT` uses the Codex app-server browser login flow. Existing Codex CLI sign-in is reused automatically.
- `AI Settings` is vertically scrollable and separates Codex subscription,
  OpenAI-compatible vision server, OCR, Read Aloud, and lecture-note controls.
  It shows OpenZoom's built-in Codex prompt read-only beside editable user
  instructions. The Codex model dropdown and its reasoning dropdown are
  populated from the signed-in app-server's `model/list` response, including
  each model's supported efforts and default; the saved model remains visible
  as unavailable when it is absent from the current catalog. Assistant
  Instructions can set a preferred response language, tone, or level of detail
  for both providers without changing Codex security permissions. OpenZoom
  lists every voice exposed to desktop applications by Windows Runtime speech
  synthesis, across installed languages. Windows 11 Narrator/Magnifier Natural
  voice packages are not currently exposed by that public API and therefore do
  not appear in OpenZoom. Advanced Assistant internet and coding permissions
  are separate opt-ins; coding also requires a workspace folder.
- `Open Notes` opens the current session's accessible HTML lecture notes from
  `Documents\OpenZoom\Notes\` (or the configured OpenZoom root). Notes contain
  timestamped OCR and scene explanations plus responsive original/processed
  photo grids. Every fully finalized recording segment adds original and
  processed MP4 players with direct relative links; failed or truncated
  finalizations are not linked.
- `Spatial Sharpen` enables the CUDA sharpening/upscaling stage and lets you choose NIS or FSR-style processing when the GPU path is active.
- `Debug View` switches to the CPU composite grid so intermediate stages can be inspected; the nearby pipeline status reports CPU/GPU, fallback, recording, OCR, and VLM state.
- `Show Focus Point` overlays the current zoom center on the presented output.
- `Capture Photo` waits for the next complete camera frame and saves
  `IMG_<timestamp>_original.jpg` plus `IMG_<timestamp>_processed.jpg` to
  `Documents\OpenZoom\Photos\YYYY-MM-DD\`. The original is decoded,
  rotation-corrected camera output without OpenZoom's enhancement stack; it is
  not the camera sensor's raw Bayer or compressed USB bitstream. Both JPEGs
  encode to `.writing` files before their paired commit. On the next startup,
  OpenZoom finishes an interrupted second rename or removes the entire
  incomplete set, so a crash-created final orphan is not kept as a successful
  capture.
- Annotation Save, Clear, and optional Save on exit write a lossless
  `ANNOTATION_<timestamp>.png` of the exact visible viewport to the same dated
  Photos folder
  and append the marked image to the current HTML lecture notes. The
  persistent Photo action remains a clean paired capture without ink.
  Annotation color, width, line style, shape choice, text size, and the
  Advanced Assistant `Save drawing to notes when leaving Draw` preference
  persist globally; strokes are session-only and are discarded after exit.
- `Start Recording` writes synchronized `VID_<timestamp>_original.mp4` and
  `VID_<timestamp>_processed.mp4` files to
  `Documents\OpenZoom\Recordings\YYYY-MM-DD\`. Both are encoded live:
  the original follows the selected camera mode and the processed file follows
  `Processed recording resolution`.
  When a microphone is selected, the same capture-clock-synchronized
  48 kHz mono AAC track is embedded in both files. Choosing
  `No microphone (video only)` omits the audio stream.
  OpenZoom tries AV1 first and falls back to H.264 when AV1 is unavailable. It
  does not transcode after Stop. Fragmented MP4 keeps each file playable up to
  its last completed fragment, the duration cap is 12 hours, and disk-space
  guards stop both files together. Stop discards queued frames and finalizes
  after the sample currently being encoded. A segment is kept and added to
  notes only when both files committed video samples and finalized; empty
  placeholders and incomplete pairs are removed. Paired capture approximately
  doubles output storage and adds CPU camera-format conversion while
  recording.

## Navigation And Interaction
- `Ctrl + mouse wheel`: zoom around the cursor.
- `Mouse wheel`: pan while zoomed.
- `Middle mouse drag`: pan the zoom focus.
- Arrow keys: nudge the zoom focus.
- `1` through `9` in Simple mode: apply the corresponding numbered quick mode.
- `Tab` / `Shift+Tab` in Simple mode: move through the corner controls.
- In annotation mode: `P` Pen, `L` Line, `S` Shape, `T` Text, `V` Move,
  `E` Erase, `Ctrl+Z` undo, and `Ctrl+Shift+Z` redo. Arrows nudge a selection,
  `Ctrl++`/`Ctrl+-` resize a single item, and `Delete` removes the selected
  item or group. Drag empty space with Move to marquee-select multiple items.
  With no selection,
  wheel and keyboard pan/zoom are forwarded to the normal viewport controls.
  `Esc` closes the options flyout, clears a selection, then leaves Draw on the
  third press. Text-field editing keeps its normal shortcuts.
- `Virtual Joystick`, near the top of Advanced Image with the other global
  controls, shows an on-canvas joystick overlay for panning.
- Wheel, arrow-key, joystick, and middle-drag panning keep the active quick mode selected; changing an actual Advanced processing control still creates a custom setup.

## Persistence And Output Paths
- Settings persist to `%APPDATA%\OpenZoom\OpenZoom\settings.json`. Camera and
  orientation are global; stabilization, colors, contrast, sharpening, zoom,
  and the other image treatments are stored in the current profile and
  user-created quick options. UI and assistive/AI configuration also persist,
  including annotation color, width, line style, shape choice, text size, and
  capture-on-exit preference (but never annotation strokes). A VLM API key
  entered in AI Settings is stored in Windows Credential Manager; the JSON
  contains only the credential identifier and ignores any plaintext
  `vlmApiKey` field. OpenZoom has never distributed or bundled an API key.
  OpenZoom stores
  only an index of the persistent
  Codex conversations it created; Codex stores their transcripts. Temporary
  Simple explanations are ephemeral and do not enter history.
- User-created files share one root: `Documents\OpenZoom\` by default.
  Advanced Assistant can change it to another writable folder outside the
  application directory. The choice persists as `paths.userDataRoot`.
- Snapshot pairs and annotated captures are written under
  `Photos\YYYY-MM-DD\`; original/processed recording pairs are written under
  `Recordings\YYYY-MM-DD\`; lecture notes are written as
  `Notes\NOTES_<timestamp>.html`; Assistant exports default to `Analysis\`.
  Console-attached launches also tee Qt diagnostics to one timestamped file
  under `Debug\`, retaining the newest 20 logs. Relative image references keep
  the complete OpenZoom root portable.
- Recording uses the camera's timestamps and exact negotiated fractional frame
  rate, so a capture stall becomes a real time gap instead of fabricated
  duplicate frames or wrong-speed playback. Encoding runs on a bounded worker
  queue. Camera frame arrival wakes processing directly, and a bounded
  six-frame recording burst queue absorbs short scheduling stalls while normal
  preview continues to prefer the newest frame. Recording reports any capture,
  readback, recording-texture-pool, pairing, queue, or encoder losses when it
  stops. Camera format changes finalize the current playable pair and continue
  in matching `_part2`, `_part3`, and later segment pairs.
- When OpenZoom is launched with a Windows console attached, recording prints
  one route line per segment and one completion summary. The output names the
  processed and original MP4 paths and distinguishes direct-GPU input,
  GPU-source worker readback, and CPU/system-memory input. Its final summary
  also separates asynchronous GPU-completion retries from frames that actually
  used the safe-copy capture rung. Ordinary Start-menu or Explorer launches
  remain quiet. All Qt console output from that diagnostic launch is also saved to
  `Documents\OpenZoom\Debug\OpenZoom_<timestamp>_pid<id>.log` (or the selected
  user-data root). Debug logs can include local paths, device names, camera
  identifiers, and timing/error details; review them before sharing.
- Set `OPENZOOM_CAPTURE_DIAGNOSTICS=1` before launching a debug build to print
  one capture summary every five seconds: camera FPS, CPU bytes copied per
  frame, Media Foundation capture work, capture-thread CPU use, and
  capture-to-present latency. Production Automatic mode uses the accelerated
  direct-GPU rung after its per-camera startup validation, then falls back to
  accelerated copy and compatibility capture in that order. For diagnostic
  fallback testing, `OPENZOOM_FORCE_CAPTURE_COPY_RUNG=1` disables only the
  direct-GPU rung for that run. The standalone probe at
  `build\msvc-release\sandbox_mf_dxva_minimal\mf_dxva_minimal.exe` compares
  accelerated and compatibility capture in an isolated watchdog process and
  prints JSON. Release bundles include the same helper beside
  `open_zoom.exe`; Advanced `Test this camera` invokes it directly.
  Maintainers can exercise the production lifetime path in a watchdog child:
  `mf_dxva_minimal.exe --camera N --interop external --iterations 100`.
  `--interop legacy` is an opt-in driver-reproduction mode and is never run by
  the normal camera test or build gates.
- Advanced Diagnostics reports rolling 240-sample p50/p95/p99 values for
  camera-processing tick time, capture-to-present latency, capture handoff,
  CPU frame preparation, CUDA submission overhead, presentation, recording
  texture cloning, and recording-worker encoder submission. Sampled CUDA
  events separately report input/upload, stabilization/geometry, effects, and
  output/cache GPU time without synchronizing the stream. CPU submission
  values are labelled as such and do not claim to measure asynchronous NVENC
  completion. The over-budget warning follows the negotiated camera frame
  period and prints the available stage p95 values instead of attributing an
  aggregate spike to CUDA.
- `Open my OpenZoom folder` is available in Advanced Assistant.
  `Ctrl+Shift+O` invokes it globally and recreates a missing root.
- On upgrade, OpenZoom can copy compatible files from a legacy
  install-relative `output\` folder. Copying is cancellable, never deletes the
  originals, and leaves a `MIGRATED.txt` breadcrumb beside them. Release
  publishing also refuses to replace a legacy bundle containing that folder.

## Assistive Runtime Configuration
The in-app `AI Settings` dialog is the primary way to configure the assistive
features; values are stored in `settings.json`. The default provider starts the
local `codex app-server` process and reuses Codex's ChatGPT-managed login. The
fallback provider accepts any OpenAI-compatible `chat/completions` server,
including local ones (LM Studio, Ollama, llama.cpp server), so image-to-text can
run fully offline. Local servers do not require an API key.

OpenZoom's Codex client starts in restricted mode: read-only sandboxing, no
approvals, no tool network access, and vision-only instructions. AI Settings
can independently allow internet access and coding for persistent Advanced
Assistant conversations. Coding requires an existing workspace folder and uses
Codex `workspaceWrite` sandboxing with that folder as the only writable root.
Simple Explain always stays restricted. MCP, dynamic, and collaboration tools
remain blocked in every mode, and unexpected approval requests are denied.
The current stable app-server `turn/start` surface does not expose a complete
per-turn tool allow-list, so OpenZoom also interrupts unexpected tool items as
defense in depth; it does not claim that developer instructions alone are a
protocol-level guarantee.
Codex app-server is still a local coding-agent process. `Setup & Downloads`
can install or update the official per-user distribution; ChatGPT sign-in is
then completed separately through `Connect ChatGPT` in AI Settings.

Environment variables remain as fallback for any field left empty in the
dialog:
- OCR first uses the configured path, then `OPENZOOM_TESSERACT_PATH`, the
  Setup Assistant-managed `%LOCALAPPDATA%\OpenZoom\tools\tesseract\tesseract.exe`,
  `PATH`, or standard Windows install directories. `Setup & Downloads` installs
  or removes the managed copy after verifying its pinned installer hash.
- Maxine discovery checks `OPENZOOM_MAXINE_PATH`, `NV_VIDEO_EFFECTS_PATH`, the
  standard Program Files location, and the NVIDIA Video Effects uninstall
  registry entry. The bottom of Advanced displays the required
  `SuperRes powered by NVIDIA Maxine™` attribution; this is attribution only
  and does not imply NVIDIA endorsement.
- Codex first uses the configured CLI path, then `OPENZOOM_CODEX_PATH`, `PATH`,
  the official standalone
  `%LOCALAPPDATA%\Programs\OpenAI\Codex\bin\codex.exe`, or
  `%LOCALAPPDATA%\Microsoft\WinGet\Links\codex.exe`. New configurations default
  to `gpt-5.6-tera` with `low` reasoning; explicit saved choices are preserved.
- Configure VLM with:
  - `OPENZOOM_VLM_API_URL`
  - `OPENZOOM_VLM_API_KEY`
  - `OPENZOOM_VLM_MODEL`
  - optional `OPENZOOM_VLM_PROMPT`

`OPENZOOM_VLM_API_KEY` is a development/runtime override and is never copied
into settings or a release bundle. A key entered in AI Settings is protected
by Windows Credential Manager instead.

## Repository Layout
- `src/app/` - application lifecycle, settings persistence, and interaction wiring.
- `src/capture/` - Media Foundation camera discovery and capture loop.
- `src/common/` - CPU frame pipeline, image processing helpers, and Media Foundation recording wrapper.
- `src/d3d12/` - Direct3D 12 presenter, swap chain, upload, and readback logic.
- `src/cuda/` - CUDA interop surface and kernels.
- `src/ui/` - Qt widgets, overlays, and event routing.
- `include/openzoom/` - public headers mirroring the source layout.
- `docs/` - architecture notes, code reference, progress tracking, and licensing docs.
- `scripts/` - build, bundle, and validation helpers.

## Documentation Map
- [`docs/README.md`](docs/README.md) for the architecture overview.
- [`docs/code_reference.md`](docs/code_reference.md) for the current class and file map.
- [`docs/hardcoded_paths.md`](docs/hardcoded_paths.md) for machine-specific defaults.
- [`improvement_ideas/00-status-and-priority.md`](improvement_ideas/00-status-and-priority.md)
  for implementation status and what to work on next — the single tracker.
- [`docs/THIRD_PARTY_LICENSES.md`](docs/THIRD_PARTY_LICENSES.md) for redistribution notes.

## License

This project is dual-licensed:

- GPL-3.0-only via [`LICENSE`](LICENSE), with an additional GPL §7 permission
  allowing linking with NVIDIA proprietary runtime libraries (CUDA runtime,
  Optical Flow SDK, Video Effects/Maxine SDK, TensorRT) — see the notice at
  the top of [`LICENSE`](LICENSE)
- Commercial licensing, support/SLA contracts, and sponsored development by
  direct arrangement with the project owner — see [`COMMERCIAL.md`](COMMERCIAL.md)

Contributions are accepted under the contributor license agreement in
[`CLA.md`](CLA.md): you keep ownership of your work, it is licensed for both
the GPL and commercial distributions, and it always remains available under
the GPL. Third-party notices are summarized in
[`docs/THIRD_PARTY_LICENSES.md`](docs/THIRD_PARTY_LICENSES.md).
