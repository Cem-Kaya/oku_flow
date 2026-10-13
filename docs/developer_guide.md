# OkuFlow developer guide

OkuFlow was formerly named OpenZoom. Website: <https://okuflow.com>. Repository: <https://github.com/Cem-Kaya/oku_flow>.

OkuFlow is a Windows-only camera magnifier built around Qt 6, Media Foundation, Direct3D 12, and an optional CUDA processing path. The current codebase already supports live camera capture, GPU (CUDA) frame processing with CPU format-conversion support paths, a two-stage preset/advanced UI, rotation-aware presentation, persistent settings, paired photo snapshots, and live AV1/H.264 recording with optional microphone audio in fragmented MP4 containers.

## Current Capabilities
- Lecture notes have larger text, remembered text-size and colour controls,
  media-type filters, grouped conversations, and timestamped transcripts.
  Preview the viewer locally with `python -B scripts/render_notes_preview.py`;
  it writes sample notes/media under `%LOCALAPPDATA%\OkuFlow\notes-preview\`.
- New recorded-video lecture notes include a Transcribe button. Open the notes
  in Chrome and allow access to the OkuFlow folder containing the recording;
  a second first-use click opens the local-only tool at okuflow.com/transcribe/.
  Pick the speech model and processor explicitly there. Audio/text are never
  uploaded by the tool. Returned text is shown under the recording and saved
  beside it as an `.okuflow-transcript.json` sidecar. Existing notes files are
  preserved; transcription no longer runs in the Windows app.
- Camera selection follows the saved physical device rather than its changing
  enumeration index; older settings use their numeric index until an identity
  is saved. Failed opens retry the requested camera. Unsupported live format
  changes report failure and enter recovery.
  A first launch with no saved camera prefers an authorized USB phone whose
  OkuFlow Camera app answers discovery. Existing camera choices stay intact;
  select the phone once under Settings → Camera to replace a saved DroidCam
  or webcam preference. Phone light starts off on every connection and is
  controlled by the explicit phone-light button in the same section.
- Streamed Assistant answers preserve selected text and a scrolled reading
  position. Draw rails scroll in short views.
  An unavailable speech voice retains its saved choice.
- Live-switchable [25-language UI](localization.md) under
  `Advanced > Settings > Language`. The flag-and-native-name selector changes
  visible labels, accessible names, status announcements, AI response
  language, locale-formatted values, and the preferred Read Aloud voice
  immediately without restarting. Debug logs, file names, device names, model
  ids, and persisted setting tokens remain stable English/data values.
- Media Foundation camera enumeration with per-device mode listing (`width x height @ fps`), restart-safe device activation, plain-language failure reporting, and automatic reconnection: when a camera drops mid-lecture, OkuFlow quietly retries the same physical device for about 30 seconds (2s/4s/8s backoff) without any modal dialogs, and only reports failure if the device never comes back.
- CPU frame pipeline for format conversion and rotation of formats the GPU path does not consume, plus the legacy debug composite view. NV12 and YUY2 camera frames bypass it entirely: color conversion and rotation run in CUDA.
- Direct3D 12 presenter for swap-chain output plus GPU texture readback.
  Processed and rotation-correct unenhanced video recording use a bounded
  384 MiB pool of shareable GPU canvases and feed Media Foundation hardware
  encoders without a GPU-to-CPU-to-GPU round trip. Confirmed GPU conversion
  completion releases the source lease; encoder samples own their separate
  NV12 output. The recording worker retires readers even without new frames.
  Pool pressure drops and counts a recording frame
  instead of stalling preview. DXGI recording samples are converted to
  encoder-native NV12 and submitted with an explicit valid-buffer length, as
  required by hardware Media Foundation encoders. Photos and on-demand
  analysis use the readback path, then hand image encoding, serialization, and
  file writes to bounded worker pools so that work does not stall the UI.
- CUDA external-memory interop path with GPU color conversion (NV12/YUY2) and rotation, video stabilization, automatic keystone correction for projected screens, black-and-white, zoom, Gaussian blur, temporal smoothing, auto contrast (percentile level stretch), low-vision display color modes with contrast/brightness, focus marker, and spatial scaling/sharpening via pinned CUDA adaptations of NVIDIA Image Scaling 1.0.3 or AMD FSR 1.0.2 EASU + RCAS.
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
  Pan, zoom, and viewport resizing continue over the held scene, including
  while the next camera GPU handoff is waiting.
- CUDA Text Clarity for camera text: background flattening, soft adaptive
  Sauvola thresholding, automatic text polarity, stroke weight, smart
  sharpening, CLAHE, two-color reading, anti-shimmer hysteresis, selective
  text-edge sharpening, glare suppression, and asynchronous focus detection.
  Text Clarity is the master switch in both Simple and Advanced views. When
  off, its nested Fine-tune text options are inactive and disabled; their
  saved choices return when it is enabled. Independent Colors and contrast
  controls and NVIDIA Super Resolution keep their own switches.
- Optional NVIDIA Maxine SuperRes replaces NIS/FSR for zoomed text when the
  separately installed Video Effects runtime is available. It runs on the
  existing CUDA stream, discards 10 warmup samples, and automatically falls
  back if its next 60-frame average exceeds the 24 ms latency target. Advanced
  reports the measured average and offers a compact checkbox override when
  latency is the only failure.
- Two-speed UI: Simple mode gives the full client area to the live view and overlays three auto-fading primary clusters plus contextual screen-correction controls; Advanced keeps the camera visible beside a narrow inspector containing every parameter and pipeline diagnostics. The chosen mode persists.
- Stage-1 quick modes backed by full stage-2 advanced configurations, including promotion of advanced tuning into user-defined quick options.
- Text reading and scene explanations use a signed-in Codex CLI/ChatGPT
  subscription (Luna 6 with low reasoning by default) or the configured
  OpenAI-compatible vision endpoint. Results stream into the assistive panel,
  support manual Read Aloud, and can be saved in `Documents\OkuFlow\Notes\`.
  The larger floating panel presents 20-point text with 135% line spacing,
  keeps Read Aloud prominent below the answer, and offers panel placement in
  a compact menu. Escape clears an unfinished question or returns focus to
  the camera; the answer remains available until Close.
- Vision frame preparation is bounded and asynchronous: resizing, JPEG
  encoding, base64/JSON creation, and temporary image writes happen outside
  the UI thread. Cancellation prevents stale work from starting a request.
- Session persistence in `%APPDATA%\OkuFlow\OkuFlow\settings.json`.
- One user-owned root at `Documents\OkuFlow\` for photos, recordings, notes,
  analysis exports, and opt-in console debug logs. The root is configurable,
  and `Ctrl+Shift+O` opens it from anywhere in the application.
- The star magnifier icon embedded in the Windows executable
  at multiple display sizes and assigned through Qt for the title bar,
  Alt-Tab, and taskbar.
- MSI packaging installs the complete bundle with desktop/Start menu shortcuts
  and versioned upgrades. Signing hooks, winget manifest generation and a Store
  identity/review-package workflow are documented in
  [Windows distribution](../docs/windows_distribution.md). Store certification,
  a packaged runtime strategy and automatic updates remain pending.

## Status
CUDA is the processing path; the CPU effects pipeline is deprecated. When the D3D12/CUDA interop surface cannot initialize, the app presents unprocessed passthrough video and shows a persistent "GPU required — processing disabled (showing raw video)" notice instead of silently degrading. The CPU debug composite view remains available as a diagnostic.

## Prerequisites
- Windows 10 or Windows 11.
- Visual Studio 2022 with the Desktop C++ workload and Windows SDK.
- Qt 6.12.0 for `msvc2022_64`, or matching overrides via `QT_PREFIX` / `Qt6_DIR`.
- CMake 3.23 or newer.
- NVIDIA GPU plus CUDA Toolkit 13.x if you want the CUDA path.
- Optional: NVIDIA Video Effects runtime for Maxine SuperRes and Codex CLI
  for subscription-backed Read, Explain, and Assistant features. OkuFlow
  offers verified vendor downloads through `Setup & Downloads`.

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
- builds `oku_flow`,
- prefers `build\cmake\Release\oku_flow.exe` when launching,
- runs `windeployqt` automatically when it can find the Qt runtime.

If Windows reports missing `Qt6*.dll` files, add the Qt `bin` directory to `PATH` or point `QT_PREFIX` / `Qt6_DIR` at the correct installation.

## Alternative Builds
### CMake presets

```powershell
cmake --preset msvc-release
cmake --build --preset msvc-release-build
```

Available presets live in [`cmake/CMakePresets.json`](../cmake/CMakePresets.json):
- `msvc-debug`
- `msvc-release`
- `msvc-cpu`
- `msvc-cuda-tests`

Run the complete local compile and test matrix from a plain Windows command
prompt with:

```bat
scripts\agent_build.bat
```

The tracked helper locates Visual Studio 2022 (17.x) with `vswhere`, compiles the shipping
CUDA configuration, runs the CPU suite, and then builds and runs the
CUDA-enabled suite. The two real CTest presets treat an empty test directory as
an error.

GUI main-window, annotation and AI-settings slots are independently selectable;
all native GUI fixtures remain serial, including across separate CTest
processes. The tracked helpers archive CTest evidence through
`scripts/run_test_gate.ps1`. After an explicitly identified desktop interruption,
`scripts\agent_build.bat --retry-interrupted msvc-cpu-tests EXACT_CTEST_NAME`
retries that one case without rebuilding. `OKUFLOW_TEST_RESUME=1` then reuses
only matching source/build/runtime/environment evidence while retaining normal
compile gates. Ordinary assertions and unfinished runs stay blocking; changed
inputs require a fresh gate. CPU/CUDA/bundle results remain separate. See
[selectable GUI tests and resume rules](../tests/README.md#selectable-gui-cases-and-exact-inventory-resumes)
for commands, evidence paths and hardware/desktop limits.

If the desktop is reserved, `set OKUFLOW_AGENT_EXCLUDE_GUI=1` before running the
helper excludes tests labelled `gui` and marks the summary as partial validation.
Leave it unset for the complete release gate.

Set `OKUFLOW_AGENT_RELEASE_BUILD_DIR` to an isolated build directory when the
development executable is running. The tracked helper applies the same release
preset there and keeps the CPU/CUDA test presets unchanged; it never closes the
running app to replace its executable.

You can also run each preset directly:

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
cmake -S . -B build-cpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.12.0/msvc2022_64" -DOKUFLOW_ENABLE_CUDA=OFF
cmake --build build-cpu
```

`OKUFLOW_ENABLE_TEXT_SR=ON` compiles the runtime-only NVIDIA Maxine SuperRes
adapter and its controls. CUDA presets and the release-bundle script enable it
by default; the CPU preset disables it. Set the environment override explicitly
to `OFF` only for a bundle that intentionally omits the adapter. OkuFlow links
no Maxine import library and ships no proprietary runtime or model files.

### Release bundle

```bat
scripts\build_release_bundle.bat
```

This builds the application and tests in one release tree, requires CTest to
pass, deploys into a staging directory, validates the Qt runtime and license
files, and only then publishes `dist\OkuFlow\`. If that bundle is locked by a
running app, the complete tested build is published as `dist\OkuFlow2\`
without terminating or relaunching OkuFlow. Set
`OKUFLOW_BUNDLE_BUILD_DIR` to use a local build directory outside the synced
checkout. Set
`OKUFLOW_SKIP_BUNDLE_TESTS=1` only for an emergency build; the script prints
`WARNING: UNTESTED BUNDLE` when that explicit escape hatch is used.

Release packaging also needs the matching Qt `Sources` component so it can
stage Qt Multimedia's exact FFmpeg LGPL-2.1-or-later text. The script discovers
the normal sibling `Src` directory automatically; set `QT_SOURCE_ROOT` when the
matching source tree is installed elsewhere.

The published bundle contains the native `oku_flow.exe` launcher,
`OkuFlowApp.exe`, the signed Microsoft `vc_redist.x64.exe` installer, Qt runtime files,
`LICENSE`, `COMMERCIAL.md`, `README.txt`, `THIRD_PARTY_LICENSES.md`, complete notices for the
bundled CUDA/reference code, Qt and FFmpeg license texts, and the
exact Qt module SPDX documents that cover every module and plugin deployed
by `windeployqt`. It also contains `SHA256SUMS.txt`,
`release-manifest.json`, and `SBOM.spdx.json`, generated from the exact staged
files before publication. The top-level SPDX document links the staged Qt SPDX
documents by namespace and SHA-1, distinguishes bundled dependencies from
optional user-installed runtimes, and records every staged file in the release
manifest.
The launcher checks the system-wide Microsoft runtime before loading Qt and
offers the included installer only when needed. Installation requires the
user's vendor-license/UAC approval and never forces a restart. Setup Assistant
shows the same dependency status. Keep the complete extracted ZIP together;
see [Windows runtime setup](../docs/windows_runtime_setup.md).
Private/team bundles may remain unsigned. To sign with a certificate already
installed in the current user's Windows certificate store, set
`OKUFLOW_SIGN_CERT_SHA1` to its thumbprint before running the script. A
self-signed code-signing certificate is suitable for the current small team
when teammates explicitly trust its public certificate; never distribute the
certificate's private key. Setting `OKUFLOW_PUBLIC_RELEASE=1` makes a missing
or invalid signature a packaging failure rather than silently publishing an
unsigned public build.

Normal Windows launch opens the GUI without a terminal. `--console` attaches
an existing parent terminal for diagnostics; debug logs also remain available
under the user's OkuFlow documents directory. Stabilizer output is rate-limited
to one detailed sample per 30 camera frames. The CUDA runtime is linked
statically. NVIDIA Video Effects and Codex CLI binaries are obtained from
vendors through Setup Assistant and are never copied into the bundle.
OpenAI's Codex bootstrap script is pinned and SHA-256 verified before execution;
that bootstrap verifies the selected official release package against OpenAI's
checksum manifest.
An existing `dist\OkuFlow\output\` leaves that bundle untouched; the new bundle
is published beside it as `dist\OkuFlow2\`. The same fallback applies when the
primary bundle is locked. Photos, recordings, notes and analysis files are user
data and are never intentionally removed by packaging.
If Qt's transfer fails, Setup retries with the Windows downloader and then the
vendor's alternate host without weakening verification.

## OkuFlow Camera Android Companion (in development)

The new [`android/`](../android/README.md) project contains native Kotlin/Compose
setup screens for Developer mode, USB debugging, camera permission, and
connecting the cable. It follows a supported phone language by default, offers
25 languages and a saved manual choice in Advanced settings, and shows one
instruction and one large action at a time.

The local Android toolchain is ready. Build the debug APK and run JVM tests
with `pwsh -NoProfile -File scripts/build_android.ps1 -Lint`; see
[`android/BUILDING.md`](../android/BUILDING.md) for local output paths and
device installation. [`Plan 51`](../improvement_ideas/51-okuflow-camera-android.md)
describes Camera2/MediaCodec USB streaming, Windows decoding, remote phone
zoom/camera switching, sustained-quality selection, and motion compensation.
The companion now streams Camera2/MediaCodec video through separate adb video
and control channels. Native Windows receives H.264 or optional HEVC through
Media Foundation, with capability-checked HEVC and an H.264 fallback; the Python
probe supports both codecs. Advanced phone settings save
a combined resolution/FPS mode, camera, stabilization, focus, exposure and light preferences.
See the [current protocol and limits](../android/PROTOCOL.md). A running local
adb server is required; the Windows bundle includes source-built adb and starts
a missing local server automatically. Automatic viewport-to-phone zoom
handoff remains pending. Pixel 9 Pro Fold streaming, remote
controls, codecs and native Windows integration have been exercised; see the
[device validation record](../android/DEVICE_VALIDATION.md) for measurements and
remaining hardware acceptance. The phone discovers combined resolution/FPS
choices locally from Camera2 and hardware encoder capabilities; host and thermal
limits can still reduce the actual stream mode. The main picker starts at
720p / 24 FPS; a separate More options screen retains all locally detected modes.
The companion is optional: normal USB cameras must work without Android
components, and an explicit webcam selection must survive phone hot-plug.
Phone sources are appended without changing the physical-camera default or
an explicitly saved webcam choice. Plan 51 retains the full hardware regression gate.

The first Google Play closed-test build uses package `com.okuflow`.
Pause streaming persists through reopening and USB retries; Resume is explicit.
Privacy information and reviewed open-source notices are readable offline.
Use `scripts/build_android_signed.ps1` for owner-controlled signed releases;
keys and its Windows-protected credential profile stay outside Drive/Git.
See [closed-test readiness](../android/PLAY_READINESS.md) for Console, tester and
device acceptance still required. A successful signed build is not Play approval.

## Lecture Video Test Camera

Double-click `scripts/start_lecture_camera.bat` to loop a downloaded lecture
through OBS Virtual Camera and open OkuFlow. On first use select
`Advanced > Settings > Camera > DroidCam Video`. OBS's standard virtual
camera runs alongside this Media Foundation output used by OkuFlow.
Double-click `scripts/stop_lecture_camera.bat` to stop the test feed.
The video remains available for replacement at
`%LOCALAPPDATA%\OkuFlow\lecture-camera\media\lecture.mp4`.
Requires installed OBS Studio, PowerShell 7, 7-Zip, and the modern DroidCam
Video driver (already installed on the setup machine); first run prepares a separate
portable OBS copy and downloads the sample. See
[`docs/lecture_camera.md`](../docs/lecture_camera.md) for source selection,
replacement videos, status, and limitations.

## Startup And Latency Profiling

With OkuFlow closed and a camera available, run the tracked profiler from
PowerShell 7 after building:

```powershell
./scripts/profile_startup.ps1 -Executable ./dist/OkuFlow/oku_flow.exe -Runs 2 -CompareLegacy
```

Each trial writes `build/startup-profile/<mode>-<run>/startup.json` and logs.
Its sibling `settings.json` starts from a copy of your settings; AI requests,
notes, and optional setup prompts are disabled for the trial, and generated
files use that trial's data folder. The source settings hash is checked after
each run. The profiler refuses to run alongside another OkuFlow instance and
lets its own instances exit normally after 15 seconds. `-DurationSeconds`,
`-CameraIndex`, `-SettingsPath`, and `-OutputDirectory` allow focused comparisons.
Durations from 1 through 600 seconds are supported for manual cable-recovery
checks; the default remains 15 seconds. Profiling always exits normally at its
chosen deadline. Its heartbeat retains only the first 3000 delay samples, so a
longer run's heartbeat percentiles do not cover the entire session.
`-DurationSeconds 1 -AllowNoFrame` also permits a short close-during-startup
check; absent a presented frame, its timings must not be used as a speed result.

`-CompareLegacy` alternates the old synchronous camera-opening sequence and the
new worker sequence in the same executable. Other rendering changes are shared
by both modes. Reports distinguish negotiated camera FPS from actual callback
arrival rate, processed scenes, and successful presents. The 20 ms Qt heartbeat
measures event-loop delays; camera-to-present latency begins after Media
Foundation returns a frame and ends at successful presentation submission.
Startup timing begins in the application constructor and excludes executable
loading. These are application timings, not sensor-to-photon measurements.

The diagnostic application flag `--startup-profile=<report-path>` uses
`settings.json` beside the report and writes no camera images. Its optional
`--startup-profile-ms=<duration>` controls normal automatic exit; omit all
profiling flags for ordinary operation.

## Runtime Controls
- Initial camera opening overlaps with window graphics initialization on a
  worker. Camera mode discovery reuses the streaming reader, and busy GPU
  frame slots return control to the UI for a later presentation attempt.
  Camera selectors are temporarily disabled while the initial device opens.
- `Simple` / `Advanced` switches between a full-view overlay UI and a right-side inspector. The live camera remains visible in both states.
- `Application language` under the Advanced Settings tab's `Language`
  section offers System default and 25 native language names with decorative
  flags. The selection is global, persists across restarts, and updates open
  windows in place. Missing/unknown preferences follow Windows display
  languages; an existing explicit choice wins. English is the unsupported
  language fallback. A failed catalog switch preserves the current language.
  Arabic, Persian, Hebrew and Urdu use RTL layout. An AI request already in
  progress finishes in its old language; the next request follows the new
  choice. [Localization](localization.md) lists codes, draft translation
  provenance and the native wording/layout/screen-reader checks still pending.
- `Viewport framing` under `Advanced > Settings > View and navigation`
  is a global viewport preference:
  `Fill (crop)` fills the camera area without distorting it, while `Fit (show
  all)` preserves the complete frame with symmetric black bars. Resizing,
  rotating, or dragging the Advanced divider always preserves the camera
  aspect ratio. Fit displays the whole frame at 1x; choose Fill to apply the
  saved Zoom amount and position.
- `Viewport motion rate`, beside framing under `Settings > View and navigation`, controls
  how smoothly pan and animated zoom move over
  the latest processed scene: Auto (up to 120 FPS), 60, 90, 120, or Match
  display. The active monitor clamps unsupported choices and OkuFlow reports
  the effective rate. This does not invent camera frames: a 30 FPS camera is
  still 30 FPS; only navigation over its newest completed frame is refreshed
  more often. Auto reduces to the camera rate while idle.
- `Processed recording resolution`, under `Settings > Recording`, keeps
  the processed video independent of window, monitor, DPI, and inspector size.
  `Match camera (recommended)` follows the rotation-correct source; fixed
  640x360, 854x480, 1280x720, 1920x1080, 2560x1440, and 3840x2160 choices
  retain their dimensions for the entire segment. Changes to this selection
  during recording apply to the next session. Portrait input swaps each fixed
  canvas to portrait orientation. The `Camera resolution & frame rate`
  selector under `Settings > Camera` controls the live camera,
  original photo, and original video dimensions.
- `Microphone`, in the same global Recording section,
  selects which Windows audio capture endpoint is recorded. New settings use
  the Windows system-default microphone; `No microphone (video only)` records
  silent video. OkuFlow opens the microphone only while recording.
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
- `Hide UI` at the top-right or `Ctrl+H` hides controls and the assistant for a clear camera view. `Show UI` restores the previous layout and answer. Simple controls also fade after five idle seconds; keyboard navigation keeps them visible. `Esc` closes the quick-mode grid.
- The grid button or current profile opens all quick modes as large tiles. Built-in modes use plain-language labels such as `Read a Page`, `High Contrast`, `Sharpen Text`, `Keep It Steady`, `See in Low Light`, `Projector Screen`, and `Whiteboard`.
- Number keys `1` through `9` apply the first nine quick modes except while an
  editable text field has focus. A mode change shows a large centered
  announcement and notifies screen readers without starting speech.
- `Text Clarity` in Simple mode automatically selects a paper, board, or
  mixed-content treatment. The `Document` quick mode applies the full
  page-reading stack; use the carousel/grid to reach it after the numbered
  first nine modes.
- `NVIDIA Super Resolution` and its strength slider are profile-owned Advanced
  controls under `Advanced > Image > Sharpness`. Enabling
  it restores a useful strength when needed and raises zoom to the 1.33x
  minimum. Maxine selects an exact supported AI scale that fits the requested
  crop and its model's resolution limit, with additional zoom applied afterward
  on the GPU. The 4x model accepts input through 540p and 3x through 720p;
  lower factors accept input through 2160p. A 4000x2256 phone stream therefore
  uses its valid 2x AI stage even at 16x desktop zoom, instead of attempting an
  unsupported 4x model. The result is consumed synchronously on the
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
  retain the base scene and report the reason; separately enabled NIS/FSR
  remains available. A slow run can be
  kept on with the compact performance-limit checkbox until SuperRes is
  turned off.
  Model load, synchronous SDK inference and teardown execute on one serial
  worker with a one-second host deadline. The caller still waits within that
  bound; this is not a fully asynchronous render pipeline. A deadline or SDK
  exception faults the surface and retains its allocations until process exit,
  rather than freeing resources a late SDK call may still access. Restart is
  required after that fault. Strength changes settle for 200 ms before reload.
- The bottom-left quick-mode carousel and its full preset grid remain available
  in Advanced mode, so presets can be changed without returning to Simple.
- Advanced has separate `Image`, `Assistant`, and `Settings`
  tabs; `Ctrl+Tab` cycles them. Image contains the current mode's adjustments,
  while Settings contains shared camera, recording, navigation, language,
  files, and AI setup. Detailed text algorithms sit under `Fine-tune text`.
  Sliders show locale-formatted values. `Ctrl+F` opens the relevant search,
  revealing Advanced when needed. Image and Settings each have a search field
  with match feedback and a link to results on the other tab. Changed counts
  identify modified groups without reopening groups you have collapsed.
  `F6` and `Shift+F6` move between control regions. Drag the inspector's left
  divider to resize it; OkuFlow remembers the width. Long selector names show
  their complete text on hover.
- A centered camera-state panel explains startup, reconnection, and stopped
  capture until a frame is presented. Setup Assistant no longer opens solely
  because optional NVIDIA Super Resolution is missing; `Setup & Downloads`
  remains available in Advanced.
- `Save as Quick Mode` promotes the current advanced setup into a reusable stage-1 preset.
- `Reset Mode` restores profile-owned image and assistive controls to their
  defaults after confirmation. It deliberately keeps the selected camera,
  orientation, viewport motion/framing, and Virtual Joystick preference.
- The question-mark button in the Advanced tab header opens a compact guide
  with Controls first and Features second.
- `Camera` selects the active Media Foundation device from Advanced. Camera
  selection, requested resolution/frame rate, and orientation are Device
  settings rather than part of a quick profile.
- `Resolution & frame rate` requests an actual Media Foundation capture mode.
  OkuFlow reads back the negotiated format and reports when the driver
  selected a different mode. `Automatic` preserves the driver's choice.
- `Camera acceleration`, under `Settings > Troubleshooting`, defaults to
  Automatic. OkuFlow tries the lower-latency Media Foundation GPU path,
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
  OkuFlow remembers the result and drops to the accelerated-copy rung; the
  compatibility reader remains the permanent final fallback. Original
  photos/videos request GPU readback only while those captures are active.
- `Test this camera` temporarily releases the live camera and compares GPU and
  compatibility capture in a watchdog-isolated helper process, then restores
  the live view and reports both average frame-read times. A hung camera driver
  cannot freeze the OkuFlow process. The `Camera acceleration` dropdown is
  the single control for choosing automatic, forced GPU, or compatibility
  capture for the selected camera.
- `Rotation` rotates the pipeline in 90 degree clockwise steps before downstream processing.
- `Black & White` applies thresholded monochrome conversion.
- `Zoom` enables the magnifier and focus-point controls, with desktop
  magnification from 1x to 16x. Existing zoom amounts and saved profiles keep
  their values within that range.
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
- `Image > Text clarity > Fine-tune text` exposes profile-owned controls for background
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
  under `Settings > View and navigation` disables only the acceleration, while
  `Ctrl+=` and `Ctrl+-` always use reproducible unaccelerated geometric steps.
- `Scene Explain` and `Assistive Overlay` drive asynchronous assistive analysis and on-screen text overlays.
- `Read Text`, identified by a speaker icon, asks the vision assistant to transcribe the current view verbatim. It preserves source language and marks unreadable portions; it does not summarize the page. Read Aloud speaks the result only when clicked. `Explain` sends one temporary, non-history camera question and changes to `Stop` while Codex is working.
- The assistive result panel updates as an answer streams. Drag the floating panel header to either app edge to show a purple docking preview, then release to dock there. Moving away or pressing Escape cancels the drop. The highlight uses a debounced latch: stay near the edge for 180 ms to select it, then stay outside its wider release zone for 250 ms to clear it. To undock by dragging, pull the docked header away and hold for about 350 ms; short movements keep it latched, and it cannot immediately snap back to the same side. Its Panel position selector also offers Floating, Dock left, and Dock right. Docking reserves space beside the camera viewport, with image proportions preserved during resizing; drag the separator to resize the dock, or choose Floating to restore the previous movable panel. The chosen dock side and floating geometry persist across restarts. In Draw mode, the floating panel stays clickable and movable, and ink is clipped around its current bounds even after moving or resizing it. In floating mode: native window movement keeps dragging responsive over the D3D camera surface, and streamed text does not reset its geometry. Drag its header to move it and an edge or corner to resize it. The first placement clears the top Simple controls; later position and size changes persist relative to the camera view and are restored across restarts. Its text can be focused, selected, and read by a screen reader; the question field remains editable while an answer is streaming, but Ask and Enter submission stay blocked until that answer finishes. Follow-ups enter the shared persistent Assistant conversation with the current view attached. Speech starts only when `Read Aloud` is clicked and omits visible section labels such as `Scene Explain` and `Read Text`, while the high-contrast Close control or `Esc` dismisses the panel.
- The Advanced Assistant can attach the current processed view, stream answers, stop a response, and manage persistent OkuFlow conversations with resume, rename, export, and delete actions.
- The Advanced Assistant subscription label reports the percentage left in the current Codex usage window.
- `Connect ChatGPT` uses the Codex app-server browser login flow. Existing Codex CLI sign-in is reused automatically.
- `AI Settings` groups the AI service, the selected provider's connection,
  shared Assistant instructions and Explain prompt, Advanced Assistant
  permissions, Read Aloud, and lecture notes. Codex and server-specific groups
  appear only for the selected provider. The built-in Codex prompt is read-only
  and collapsed by default; narrow dialogs place labels above their fields.
  The Codex model dropdown and its reasoning dropdown are
  populated from the signed-in app-server's `model/list` response, including
  each model's supported efforts and default; the saved model remains visible
  as unavailable when it is absent from the current catalog. Assistant
  Instructions can set a preferred response language, tone, or level of detail
  for both providers without changing Codex security permissions. OkuFlow
  lists every voice exposed to desktop applications by Windows Runtime speech
  synthesis, across installed languages. Windows 11 Narrator/Magnifier Natural
  voice packages are not currently exposed by that public API and therefore do
  not appear in OkuFlow. Advanced Assistant internet and coding permissions
  are separate opt-ins; coding—including Python and shell tools—also requires
  a workspace folder.
- `Open Notes` opens the current session's accessible HTML lecture notes from
  `Documents\OkuFlow\Notes\` (or the configured OkuFlow root). Notes contain
  timestamped text readings and scene explanations plus responsive original/processed
  photo grids. Every fully finalized recording segment adds original and
  processed MP4 players with direct relative links; failed or truncated
  finalizations are not linked.
- `Spatial Sharpen` enables the CUDA sharpening/upscaling stage and lets you
  choose the pinned NVIDIA Image Scaling 1.0.3 NVScaler or AMD FidelityFX FSR
  1.0.2 EASU + RCAS implementation when the GPU path is active. Magnified
  crops receive up to 2x enlargement per pass within the existing frame
  buffers and a 1440p cap; additional zoom uses display scaling. Native-size
  views retain sharpening. Pan/zoom reuses the cached camera result.
- `Debug View` switches to the CPU composite grid so intermediate stages can be inspected; the nearby pipeline status reports CPU/GPU, fallback, recording and VLM state.
- `Show Focus Point` overlays the current zoom center on the presented output.
- `Capture Photo` waits for the next complete camera frame and saves
  `IMG_<timestamp>_original.jpg` plus `IMG_<timestamp>_processed.jpg` to
  `Documents\OkuFlow\Photos\YYYY-MM-DD\`. The original is decoded,
  rotation-corrected camera output without OkuFlow's enhancement stack; it is
  not the camera sensor's raw Bayer or compressed USB bitstream. Both JPEGs
  encode to exclusively created `.writing` files before their paired commit.
  An occupied timestamp gets a unique suffix instead of overwriting another
  capture. A `.pair.pending` marker records that both encodes finished. On the
  next startup, OkuFlow can finish a marked interrupted second rename and
  remove abandoned temporary files. Existing final JPEGs are always preserved;
  a marked incomplete pair that cannot be completed is reported for review.
- Annotation Save, Clear, and optional Save on exit write a lossless
  `ANNOTATION_<timestamp>.png` of the exact visible viewport to the same dated
  Photos folder
  and append the marked image to the current HTML lecture notes. The
  persistent Photo action remains a clean paired capture without ink.
  Annotation color, width, line style, shape choice, text size, and the
  `Settings > Notes and files > Save drawing to notes when leaving Draw` preference
  persist globally; strokes are session-only and are discarded after exit.
- `Start Recording` writes synchronized `VID_<timestamp>_original.mp4` and
  `VID_<timestamp>_processed.mp4` files to
  `Documents\OkuFlow\Recordings\YYYY-MM-DD\`. Both are encoded live:
  the original follows the selected camera mode and the processed file follows
  `Processed recording resolution`.
  Processed recordings include Draw ink on both GPU and CPU routes; originals
  remain clean. CPU fallback within a GPU-started writer converts to its NV12
  input contract without resetting the paired timeline.
  When a microphone is selected, the same capture-clock-synchronized
  48 kHz mono AAC track is embedded in both files. Choosing
  `No microphone (video only)` omits the audio stream.
  OkuFlow tries AV1 first and falls back to H.264 when AV1 is unavailable. It
  does not transcode after Stop. Fragmented MP4 limits how much recent media a
  process failure can lose, but playback of an incomplete file is not assumed
  without checking it. The duration cap is 12 hours, and disk-space
  guards stop both files together. Stop discards queued frames and finalizes
  after the sample currently being encoded. Each nonempty original or
  processed file is retained independently after an encoder or finalization
  failure, including when its partner is missing. Only a fully finalized pair
  is added to lecture notes. OkuFlow removes only its own regular, zero-byte
  output with no accepted video samples; it does not sweep old recording
  folders at startup. Paired capture approximately
  doubles output storage and adds CPU camera-format conversion while
  recording.

## Navigation And Interaction
- `Ctrl + mouse wheel`: zoom around the cursor.
- `Mouse wheel`: pan while zoomed.
- `Middle mouse drag`: pan the zoom focus.
- Arrow keys: nudge the zoom focus.
- `1` through `9` in Simple mode: apply the corresponding numbered quick mode.
- `Tab` / `Shift+Tab` in Simple mode: move through the corner controls.
- `Ctrl+H` or the persistent Hide UI button: clear controls and the assistant
  from the camera, then restore the previous mode, tab, panel width, and
  answer with Show UI. Mouse movement does not undo an explicit hide.
- `F6` / `Shift+F6`: move between camera and control regions; `Ctrl+F` opens
  the relevant Advanced settings search. Either shortcut restores hidden UI.
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
A missing primary settings file now restores a valid `.backup`. Deleting only
`settings.json` therefore does not reset saved preferences; a deliberate
manual reset requires removing both primary and backup.

- Settings persist to `%APPDATA%\OkuFlow\OkuFlow\settings.json`. Camera and
  orientation are global; stabilization, colors, contrast, sharpening, zoom,
  and the other image treatments are stored in the current profile and
  user-created quick options. UI and assistive/AI configuration also persist,
  including annotation color, width, line style, shape choice, text size, and
  capture-on-exit preference (but never annotation strokes). A VLM API key
  entered in AI Settings is stored in Windows Credential Manager; the JSON
  contains only the credential identifier and ignores any plaintext
  `vlmApiKey` field. OkuFlow has never distributed or bundled an API key.
  Clearing a key first persists its opaque id for deletion retry; failed
  deletion or settings cleanup remains pending across later saves/restarts.
  OkuFlow stores
  only an index of the persistent
  Codex conversations it created; Codex stores their transcripts. Explain
  continues one saved conversation (`explainThreadId`, listed as "Explain
  conversation"); Read requests are ephemeral and do not enter history.
  The settings file is limited to 16 MiB. That holds more than 10,000
  conversation index entries. If a file exceeds the limit, OkuFlow keeps it as
  `settings.json.too-large-<timestamp>` and restores the last valid backup. A
  save that would exceed the limit changes nothing and asks you to delete old
  Assistant conversations.
- User-created files share one root: `Documents\OkuFlow\` by default.
  Advanced Assistant can change it to another writable folder outside the
  application directory. The choice persists as `paths.userDataRoot`.
- Snapshot pairs and annotated captures are written under
  `Photos\YYYY-MM-DD\`; original/processed recording pairs are written under
  `Recordings\YYYY-MM-DD\`; lecture notes are written as
  `Notes\NOTES_<timestamp>.html`; Assistant exports default to `Analysis\`.
  Console-attached launches also tee Qt diagnostics to one timestamped file
  under `Debug\`, retaining the newest 20 logs. Relative image references keep
  the complete OkuFlow root portable.
- Recording uses capture delivery-clock timestamps (not verified sensor exposure
  timestamps) and the exact negotiated fractional frame
  rate, so a capture stall becomes a real time gap instead of fabricated
  duplicate frames or wrong-speed playback. Encoding runs on a bounded worker
  queue. Camera frame arrival wakes processing directly, and a bounded
  six-frame recording burst queue absorbs short scheduling stalls and catches
  up to the newest arrival on saturation, while normal
  preview continues to prefer the newest frame. Recording reports any capture,
  readback, GPU-upload, recording-texture-pool, queue, or encoder losses when it
  stops. Camera format changes finalize the current playable pair and continue
  in matching `_part2`, `_part3`, and later segment pairs.
- Video writers admit a 12-sample / 128 MiB estimated input backlog each,
  including pending/incoming samples. Startup requires two payloads to fit,
  so each negotiated input frame must be at most 64 MiB (BGRA for CPU or
  compatibility readback, NV12 for native GPU input). Larger inputs fail with
  a clear error instead of starting a recording that cannot advance. Native
  NV12 eligibility still depends on the separate texture pool and driver limits.
  Pressure drops original/processed pairs
  together; audio continues. Failed statistics retain finite admission credit
  and show a notice when exhausted. Low-latency encoding is requested and may
  affect codec quality. Audio uses a separate PCM sample-count input budget per
  writer: 128 PCM blocks, 4 MiB, or two seconds of outstanding PCM duration.
  If exhausted, recording stops with an explicit message, preserving existing
  artifacts rather than silently omitting speech. Confirmed statistics are
  required for sustained audio recording, using the configured Microsoft raw LC
  AAC format's 1024-sample packets. Audio losses are counted separately
  from video frames. These policies do not bound internal MFT/driver allocations
  or blocking finalization time. See [review 47](../improvement_ideas/47-sequential-priority-fixes-2026-10-11.md).
- When OkuFlow is launched with a Windows console attached, recording prints
  one route line per segment and one completion summary. The output names the
  processed and original MP4 paths and distinguishes direct-GPU input,
  GPU-source worker readback, and CPU/system-memory input. Its final summary
  also separates asynchronous GPU-completion retries from frames that actually
  used the safe-copy capture rung. Ordinary Start-menu or Explorer launches
  remain quiet. Use `oku_flow.exe --console` from an existing terminal to attach
  diagnostics; this never creates a new terminal. Redirected diagnostic output
  also enables logging. All Qt console output from that diagnostic launch is also saved to
  `Documents\OkuFlow\Debug\OkuFlow_<timestamp>_pid<id>.log` (or the selected
  user-data root). Debug logs can include local paths, device names, camera
  identifiers, and timing/error details; review them before sharing.
- Set `OKUFLOW_CAPTURE_DIAGNOSTICS=1` before launching a debug build to print
  one capture summary every five seconds: camera FPS, CPU bytes copied per
  frame, Media Foundation capture work, capture-thread CPU use, and
  capture-to-present latency. Production Automatic mode uses the accelerated
  direct-GPU rung after its per-camera startup validation, then falls back to
  accelerated copy and compatibility capture in that order. For diagnostic
  fallback testing, `OKUFLOW_FORCE_CAPTURE_COPY_RUNG=1` disables only the
  direct-GPU rung for that run. The standalone probe at
  `build\msvc-release\sandbox_mf_dxva_minimal\mf_dxva_minimal.exe` compares
  accelerated and compatibility capture in an isolated watchdog process and
  prints JSON. Release bundles include the same helper beside
  `oku_flow.exe`; Advanced `Test this camera` invokes it directly.
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
- `Open my OkuFlow folder` is available in Advanced Assistant.
  `Ctrl+Shift+O` invokes it globally and recreates a missing root.
- OkuFlow never reads, copies, or deletes a legacy install-relative `output\`
  folder. Release publishing refuses to replace a legacy bundle containing
  that folder.

## Assistive Runtime Configuration
The in-app `AI Settings` dialog, opened from `Settings > AI and downloads`,
is the primary way to configure the assistive
features; values are stored in `settings.json`. The default provider starts the
local `codex app-server` process and reuses Codex's ChatGPT-managed login. The
fallback provider accepts any OpenAI-compatible `chat/completions` server,
including local ones (LM Studio, Ollama, llama.cpp server), so image-to-text can
run fully offline. Local servers do not require an API key.

OkuFlow's Codex client starts in restricted mode: read-only sandboxing, no
approvals, no tool network access, and vision-only instructions. AI Settings
can independently allow internet access and coding for persistent Advanced
Assistant conversations. Coding requires an existing workspace folder and uses
Codex `workspaceWrite` sandboxing with that folder as the only writable root.
Read and Explain always stay restricted. MCP, dynamic, and collaboration tools
remain blocked in every mode, and unexpected approval requests are denied.
The current stable app-server `turn/start` surface does not expose a complete
per-turn tool allow-list, so OkuFlow also interrupts unexpected tool items as
defense in depth; it does not claim that developer instructions alone are a
protocol-level guarantee.
Codex app-server is still a local coding-agent process. `Setup & Downloads`
can install or update the official per-user distribution; ChatGPT sign-in is
then completed separately through `Connect ChatGPT` on the Advanced Assistant tab.

Environment variables remain as fallback for any field left empty in the
dialog:
- Maxine discovery checks `OKUFLOW_MAXINE_PATH`, `NV_VIDEO_EFFECTS_PATH`, the
  standard Program Files location, and the NVIDIA Video Effects uninstall
  registry entry. The bottom of Advanced displays the required
  `SuperRes powered by NVIDIA Maxine™` attribution; this is attribution only
  and does not imply NVIDIA endorsement.
- Codex first uses the configured CLI path, then `OKUFLOW_CODEX_PATH`, `PATH`,
  the official standalone
  `%LOCALAPPDATA%\Programs\OpenAI\Codex\bin\codex.exe`, or
  `%LOCALAPPDATA%\Microsoft\WinGet\Links\codex.exe`. New configurations default
  to `gpt-6-luna` with `low` reasoning. The former misspelled
  `gpt-5.6-tera` value is migrated automatically; other explicit saved
  choices are preserved.
- Configure VLM with:
  - `OKUFLOW_VLM_API_URL`
  - `OKUFLOW_VLM_API_KEY`
  - `OKUFLOW_VLM_MODEL`
  - optional `OKUFLOW_VLM_PROMPT`

`OKUFLOW_VLM_API_KEY` is a development/runtime override and is never copied
into settings or a release bundle. A key entered in AI Settings is protected
by Windows Credential Manager instead.

## Repository Layout
- `src/app/` - application lifecycle, settings persistence, and interaction wiring.
- `src/capture/` - Media Foundation camera discovery and capture loop.
- `src/common/` - CPU frame pipeline, image processing helpers, and Media Foundation recording wrapper.
- `src/d3d12/` - Direct3D 12 presenter, swap chain, upload, and readback logic.
- `src/cuda/` - CUDA interop surface and kernels.
- `src/ui/` - Qt widgets, overlays, and event routing.
- `include/okuflow/` - public headers mirroring the source layout.
- `docs/` - architecture notes, code reference, progress tracking, and licensing docs.
- `scripts/` - build, bundle, and validation helpers.

## Documentation Map
- [`docs/README.md`](../docs/README.md) for the architecture overview.
- [`docs/code_reference.md`](../docs/code_reference.md) for the current class and file map.
- [`docs/hardcoded_paths.md`](../docs/hardcoded_paths.md) for machine-specific defaults.
- [`improvement_ideas/00-status-and-priority.md`](../improvement_ideas/00-status-and-priority.md)
  for implementation status and what to work on next — the single tracker.
- [`docs/THIRD_PARTY_LICENSES.md`](../docs/THIRD_PARTY_LICENSES.md) for redistribution notes.

## License

This project is dual-licensed:

- GPL-3.0-only via [`LICENSE`](../LICENSE), with an additional GPL §7 permission
  allowing linking with NVIDIA proprietary runtime libraries (CUDA runtime,
  Optical Flow SDK, Video Effects/Maxine SDK, TensorRT) — see the notice at
  the top of [`LICENSE`](../LICENSE)
- Commercial licensing, support/SLA contracts, and sponsored development by
  direct arrangement with the project owner — see [`COMMERCIAL.md`](../COMMERCIAL.md)

Contributions are accepted under the contributor license agreement in
[`CLA.md`](../CLA.md): you keep ownership of your work, it is licensed for both
the GPL and commercial distributions, and it always remains available under
the GPL. Third-party notices are summarized in
[`docs/THIRD_PARTY_LICENSES.md`](../docs/THIRD_PARTY_LICENSES.md).

### Android camera connection recovery

Windows discovers authorized USB phones in the background every five seconds.
Open OkuFlow Camera on the phone, connect a data cable and approve USB debugging.
A phone selected in Settings remains the preferred source across sessions;
choosing a webcam explicitly keeps that webcam selected. If a phone is late or
unavailable, a temporary webcam can run until the phone is ready. Normal USB
webcams retain their Media Foundation capture path. Device/lens discovery comes
from the connected phone, with no built-in model or serial list.

The Windows ZIP includes source-built adb, its USB support/dependency DLLs,
license notices and corresponding source/relinking materials. Users do not
need Android Studio or the SDK. OkuFlow starts a missing local adb server
without a terminal on a private ephemeral loopback port and preserves an
existing or overridden server. Its exact child is assigned to a non-inherited
Windows kill-on-close job before running: app close and crash both stop it.
An idle owned server can restart after two confirmed unresponsive version
probes, with a 30-second cooldown across failures/crashes; live/starting phone
sessions and cancellation suppress restart. Borrowed servers remain under
their owner's control. The Android APK is built
and installed separately; some phone vendors require a separately installed
Windows USB debugging driver. See [`docs/adb_distribution.md`](../docs/adb_distribution.md)
for build provenance and redistribution terms.

The owned server uses WinUSB exclusively, so a separate SDK server may not see
its phones. Start SDK adb first for borrowed mode, or use the private port
logged by OkuFlow for cooperating tools. Explicit webcam selection prevents
owned bootstrap and releases idle USB ownership; automatic phone selection is
phone-use intent. Emulator scanning is disabled. Owned startup has a five-second
readiness window and localized automatic-retry status. A dead owned child can
yield to a responsive new default listener after its sessions finish, while
shutdown prevents late worker launches. Cancellable lifecycle lock waits and
nonblocking live-child observations keep stop from waiting behind discovery.
Port observation is not a readiness guarantee; startup checks the ready result.
Server epochs reset retry/launch state even when a restarted adb reuses IDs.

Confirmed default-server hangs (two failures spanning five seconds) try narrow
reviewed-legacy OkuFlow reclamation: exact process handle, canonical bundle,
pinned adb manifest/image, product metadata or pinned public launcher hash,
no sibling/session, serialized and revalidated before stop effects. Unknown
listeners remain untouched while a private shadow tries USB. Empty raw private
transports plus two healthy default probes permit handback. `BorrowOnly` has no
lifecycle effects; explicit webcam intent only releases idle owned support.
See the provenance pins, private fixtures and limits in adb_distribution.md.

### Progress deadlines

Steady-state GPU completion and deferred camera readback have one-second
progress deadlines. They observe actual completion, not camera FPS or VSync.
A GPU fault retains unfinished resources and displays a persistent restart
message. A readback failure instead enters the existing camera reconnect path.
Phone packet and decoded-output progress are independent, so arriving compressed
packets cannot indefinitely hide a decoder that stopped producing pictures.
