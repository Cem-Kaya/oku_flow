# OpenZoom Documentation Guide

OpenZoom is a Windows-only live magnification application that combines:
- Qt 6 for the desktop shell and input handling
- Media Foundation for camera and microphone discovery, frame capture, and
  live media encoding
- Direct3D 12 for presentation and GPU texture management
- CUDA for optional GPU processing through D3D12 external-memory interop
- Qt Linguist catalogs embedded in the executable for live English, Turkish,
  and German UI switching

## Architecture At A Glance
The current frame flow is:

1. Initial device opening runs on an independent COM worker while the native
   window/presenter initializes. Mode discovery reuses the streaming reader;
   cancellation revokes delivery without retaining the application object.
   `MediaCapture` enumerates cameras and streams frames from the selected
   device, preferring NV12, then YUY2, before BGRA formats. Automatic mode
   first creates a D3D11/DXGI-backed Media Foundation source reader, validates
   timestamps and image range for 30 startup frames, and transparently reopens
   the conservative system-memory reader if that camera/driver fails. The
   accelerated reader retains DXGI textures. Matching BGRA uses a direct GPU
   rectangle copy; D3D11 converts other supported formats to a reusable
   NT-shareable BGRA allocation, D3D12 opens that allocation to provide its
   exact resource description and allocation size, and CUDA imports it as
   external memory. A delayed D3D11 completion query retains the same MF
   frame for event-loop retries after a single nonblocking query; the existing
   25 ms budget bounds the D3D11 handoff; three consecutive deadlines select
   safe-copy input for that session. CUDA copy ownership waits retain their
   independent completion/fault rules. CPU buffers preserve native pitch before tight
   normalization, and YUV color matrix/range metadata follows each frame.
2. The direct-GPU rung copies that imported BGRA array device-to-device into
   the established CUDA working surface without a CPU bounce. A completion
   event retains a producer lease until the copy finishes, so the UI need not
   synchronize the processing stream and the conversion texture cannot be
   reused early. Lower rungs
   upload compact NV12/YUY2 planes; conversion and rotation still run in CUDA.
   Host uploads use a two-slot page-locked ring whose per-slot event guards
   reuse. `CpuFramePipeline` converts/rotates only remaining formats, debug
   composite view, GPU-unavailable passthrough, and fallback frames.
3. `CudaInteropSurface` runs the GPU effect chain when the interop surface is
   valid and publishes the completed camera generation as a persistent scene
   texture. Its first image stage is a device-resident fixed-reference
   stabilizer: CUDA Harris/Lucas-Kanade tracking at up to 640x360 feeds a
   bounded translation RANSAC and a uniform affine warp. A prepared anchor map,
   projection seed used only for wide-range anchor reacquisition, and bounded
   relative fallback preserve lock through bumps without integrating a
   long-running pairwise path.
   Stateful effects, SuperRes inference, recording and assistive
   scheduling advance only on this camera clock.
   NIS/FSR enlargement uses the visible ROI with bounded existing-buffer
   reuse, up to 2x per pass; partial-cache border guards prevent stale pixels.
4. `PipelineOrchestrator` runs a separate viewport clock. During pan, zoom,
   focus animation, or resize it can re-present the latest complete scene up
   to 120 FPS or the active display rate; while idle it reduces to the camera
   rate. This improves navigation smoothness without synthesizing camera
   frames or rerunning temporal effects.
5. `D3D12Presenter` keeps its swap chain at the render window's native pixel
   size and draws the persistent scene through one canonical `ViewTransform`.
   Fill crops uniformly and Fit letterboxes uniformly, so camera frames are
   never independently stretched on X or Y. Its frame-slot signal is folded
   back into the same strictly increasing CUDA/D3D12 fence timeline after
   every present, including viewport-only motion. Busy frame-slot admission
   returns immediately and retains the scene for a later Qt tick. Recording
   clones queue their own CUDA dependency even when the viewport is busy.
   Recording reads the completed
   processed scene before viewport scaling; periodic assistive grabs use the
   asynchronous readback ring (`RequestReadback` /
   `TryGetCompletedReadback`), as do photos and on-demand analysis. Request ids
   match results to the originating request/frame. Fence and CUDA stream
   drains have deadlines; failed completion retains live resources and stops
   GPU submission until restart. Camera shutdown runs on shared session-owned
   workers with a separate deadline and revocable UI delivery.

Lecture-notes document/image storage uses one bounded ordered background
queue. Queued jobs retain their paths and immutable data through settings
changes; cancellation releases unsubmitted images. The UI reports failures
and waits for actual storage completion before opening notes or announcing
that transcription was saved.

`scripts/profile_startup.ps1` profiles first-camera presentation, Qt heartbeat
delays, actual camera arrival FPS, application drops, and processing/presentation
timings. It uses explicit isolated settings and normal child-process shutdown.
See the root README for invocation and measurement limits. Initial platform
discovery, graphics resource creation, and manual device changes still include
synchronous work; ordinary frame-slot admission and initial device opening no
longer add blocking waits to the Qt event loop.

## Language And Locale

`LanguageManager` installs an embedded `QTranslator` before the main window is
constructed and re-translates existing widgets when the user changes
`Advanced > Image > Application > Application language`. The stable persisted
values are `en`, `tr`, and `de`; a profile without that key adopts a supported
Windows locale once. The language setting is global and never part of a quick
image profile.

Hand-built widgets retain their English source properties through
`ui_translation`, while `SetLiveText` retains source state for dynamic status
and accessibility updates. Data-driven values such as camera and microphone
names, Codex model ids, file paths, telemetry, and serialization tokens are
not translated. Turkish and German use locale-aware number presentation
without changing JSON or media metadata.

Built-in AI instructions remain in English for model reliability. Turkish or
German adds a final response-language directive; user-authored assistant
instructions remain unmodified and retain higher specificity. Read Aloud
remains manual-only. A language change prefers an installed matching speech
voice and reports once through visible status when no matching voice exists.

The localization layer uses Unicode Qt widgets and embedded Qt Linguist
catalogs rather than Latin-only string handling. Additional LTR languages,
including Chinese and Japanese, can reuse the same architecture. Locale
metadata also drives the process layout direction, and OpenZoom's manually
positioned floating chrome and Draw toolbars use logical leading/trailing
anchors. Developers can run the English catalog with `--rtl-test` or
`OPENZOOM_FORCE_RTL=1` to exercise that path before an RTL catalog exists.
Shipping Arabic, Hebrew, Persian, Urdu, or another RTL language still requires
a native translation, font/line-break review, and live NVDA/Narrator
validation; none is currently shown in the language picker.

Catalog source parity, complete translations, LF endings, and `lrelease`
success are enforced by `scripts/check_translations.ps1`, which runs at the
start of `scripts/agent_build.bat`.
6. `RecordingManager` maps those matched pairs into a fixed processed-video
   canvas and sends them through a bounded worker queue. Two `VideoRecorder`
   instances encode synchronized original/processed output live through Media
   Foundation using the camera timestamps and exact negotiated fractional
   rate. A selected `AudioCapture` endpoint is normalized to 48 kHz mono PCM
   on its own capture thread, mapped onto the same monotonic clock, and encoded
   as AAC into both MP4 files. Both recorders probe AV1 first, fall back
   together to H.264, write fragmented MP4 with free-disk-space guards, check
   finalization, report drops by cause, and start matching `_partN` files after
   a camera-format change.
   Microphone callbacks cross an independently owned, generation-tagged target
   that Stop cancels before waiting; a detached reader can therefore deliver
   to neither a destroyed app nor a later recording session. Recorder
   abandonment is sticky into process teardown even when the worker later
   recovers and joins, so intentionally leaked MF objects are never followed
   by `MFShutdown`.
7. Paired photos encode both JPEGs to `.writing` names before the two-rename
   commit. Startup reconciles every stale transaction: it completes the
   missing second rename when the counterpart temp proves both encodes
   finished, otherwise it removes the whole partial set and reports anything
   the filesystem would not let it clean.

CUDA is the processing path and the CPU effects pipeline is deprecated: when the GPU pipeline is unavailable the app presents unprocessed passthrough video with a persistent "GPU required" notice instead of running effects on the CPU. The debug composite view remains CPU-only as a diagnostic.

The UI now has two states:
- Simple: a full-size live view with three flush, auto-fading primary clusters,
  contextual keystone history controls, a numbered quick-mode grid, and large
  visual/accessibility mode announcements
- Advanced: the same live view beside a narrow inspector with separate `Image`
  and `Assistant` tabs, wrapping section arrows, a full-width AI Settings row
  below the tabs, and Image-side pipeline diagnostics; Assistant provides subscription
  status, camera-aware chat, and OpenZoom-owned history

## Module Map
- `src/app` / `include/openzoom/app`: composition root plus focused pipeline,
  recording, settings, UI-state, assistive, and interaction managers. The
  `OpenZoomApp` implementation is split by responsibility across `app_*`
  translation units.
- `src/capture` / `include/openzoom/capture`: Media Foundation camera and
  microphone enumeration, mode discovery, and capture.
- `src/common` / `include/openzoom/common`: CPU image conversion/effects,
  canonical aspect/view transforms, frame pipeline, and media writing.
- `src/d3d12` / `include/openzoom/d3d12`: swap chain, upload, presentation, and texture readback.
- `src/cuda` / `include/openzoom/cuda`: CUDA interop surface, kernels, and fence synchronization.
- `src/ui` / `include/openzoom/ui`: Qt widgets, overlays, and event routing.

## Build Matrix
- `scripts/build_and_run.bat`: default local Windows build and launch helper.
  It explicitly enables CUDA and the runtime-loaded Text-SR adapter unless
  either option is overridden in the environment, preventing stale CMake
  caches from silently disabling NVIDIA Super Resolution.
- `scripts/build_release_bundle.bat`: packages a distributable `dist/OpenZoom`
  folder and explicitly enables CUDA plus the runtime-loaded Text-SR adapter
  unless either option is overridden in the environment. It builds and runs
  CTest before staging, requires `windeployqt` to succeed, validates the
  deployed Qt platform runtime, and publishes only a complete bundle. Existing
  `dist/OpenZoom/output` user captures are preserved. A locked primary bundle
  produces the complete sibling `dist/OpenZoom2` without stopping the app.
- `scripts/agent_build.bat`: tracked Windows compile/test matrix. It locates
  Visual Studio with `vswhere`, compiles `msvc-release`, then runs the CPU and
  CUDA-enabled CTest presets with explicit PASS/FAIL summaries.
- `scripts/run_minimal_test.bat`: builds the app without launching it, then
  runs the DX12/CUDA sandbox harness when its `CMakeLists.txt` is present;
  otherwise it reports a successful optional-harness skip. It also builds the
  isolated `mf_dxva_minimal` probe but deliberately does not open physical
  cameras; that owner-gated probe is run explicitly. CUDA builds also expose
  `mf_dxva_minimal --interop external --iterations 100` for watchdog-isolated
  external-memory lifetime stress. The dangerous legacy registration repro is
  opt-in and never part of automated build gates.
- `cmake/CMakePresets.json`: includes `msvc-debug`, `msvc-release`,
  `msvc-cpu`, and `msvc-cuda-tests`. Only `msvc-cpu-tests` and
  `msvc-cuda-tests` are CTest presets; both fail when no tests are discovered.

Core CMake options:
- `OPENZOOM_ENABLE_CUDA=ON|OFF`
- `OPENZOOM_ENABLE_TESTS=ON|OFF`
- `OPENZOOM_ENABLE_TEXT_SR=ON|OFF` (runtime-only NVIDIA Maxine SuperRes adapter;
  enabled by CUDA presets and disabled by the CPU preset)

When operating from the WSL/Linux agent shell, invoke Windows-side tooling with
PowerShell 7 via `pwsh.exe -NoProfile -Command '...'`, for example
`pwsh.exe -NoProfile -Command 'Get-Date'`. This PowerShell 7 bridge can also
run the Windows build helpers, such as
`pwsh.exe -NoProfile -Command '& .\scripts\build_and_run.bat'`; do not use the
legacy `powershell.exe` bridge.

## Runtime Behavior
- Camera modes are listed in the UI for the selected device.
- Global Advanced viewport controls select aspect-safe Fill/crop or
  Fit/letterbox plus Auto-up-to-120, fixed 60/90/120, or Match-display motion.
  Explicit rates clamp to the monitor and are reported once. Diagnostics
  distinguish camera FPS, measured/target viewport FPS, display Hz, viewport
  pixels, scene pixels, framing mode, and missed presents.
- Camera mode probes release their Media Foundation activation session before capture starts, so camera restarts and virtual-camera sources receive a fresh media source.
- Mid-stream device loss triggers an automatic reconnect state machine (2s/4s/8s backoff for about 30 seconds) that re-finds the same physical device by symbolic link; no modal dialogs appear while it runs. Unsupported dynamic format changes and startup failures are reported in plain language in the status label.
- The Simple interaction surface is a bottom-left preset carousel and temporary
  tile grid; each preset maps to a full advanced configuration. Keys `1`-`9`
  activate the first nine entries, including the built-in `Projector Screen`
  and `Whiteboard` modes.
- Simple chrome fades after about five seconds idle and returns on mouse,
  keyboard, focus, or application activity. Mode changes produce a centered
  toast plus a Qt accessibility announcement; they do not start speech.
  Qt and native activity notifications share a fast path while chrome is
  already visible, so high-frequency pointer input does not repeatedly move
  or raise its owned tool windows and starve viewport presentation.
- Advanced edits update a live config and can be promoted into user-defined quick modes without hiding the camera.
- Advanced places the global Virtual Joystick control near the top, provides a
  question-mark Help window ordered as Controls then Features, and offers
  Reset Tuning for restoring profile-owned values without changing global
  device or interaction choices.
- Keystone correction retains up to 32 accepted warps. Previous freezes and
  restores older history, Stop/Continue controls live detection, and Next
  restores newer history or requests one fresh correction while stopped.
- Text Clarity runs after stabilization/keystone and before legacy BW/spatial
  sharpening. A shared pitched GPU workspace supplies local luma mean and
  variance to background flattening, Sauvola, glare suppression, focus
  scoring, and the stroke mask. The `Document` preset enables the complete
  classical stack. Simple auto mode classifies paper/board/mixed content from
  the device histogram, replacing paper/board with the reading mask while
  preserving natural color for mixed scenes.
- At zoom levels of 1.33x or more, profile-owned NVIDIA Super Resolution can
  replace the NIS/FSR stage. The wrapper loads NVIDIA Video Effects only at
  runtime, resolves models from the runtime's `models` subdirectory, and runs
  a supported 4/3x pass on the shared CUDA stream; additional magnification is
  applied afterward. It performs no host frame readback and returns to NIS
  when unavailable or when its 60-frame steady-state average exceeds the 24 ms
  latency target. Ten warmup samples are excluded. Advanced shows the source
  crop, viewport target, final zoom, and measured average. Maximum source
  detail is the default. Optional `Faster 2x mode (narrower view)` uses at
  least a 2x stage, changing a 1280x720 target's visible source crop from
  960x540 to 640x360. This is explicitly a speed/field-of-view tradeoff. When
  latency is the only problem, a compact checkbox can keep SuperRes active
  until the feature is turned off. Optional `Ultra quality (full frame, up to
  1440p)` creates a distinct high-resolution D3D12/CUDA cache and applies
  viewport zoom/crop only after full-frame inference: 720p -> 1440p uses 2x,
  1080p -> 1440p uses 4/3x, and native 1440p input bypasses unnecessary AI
  enlargement. The ordinary scene and cache dimensions follow the negotiated
  post-rotation camera mode; 720p is not an internal fixed resolution.
- Focus scoring reduces Laplacian statistics on-device and asynchronously
  copies only two floats about every 15 frames; no image readback or render
  stall is introduced. A low score suppresses text reading and shows a
  refocus prompt.
- Camera selection and orientation are global. Stabilization, display colors,
  contrast, sharpening, zoom, and other image treatment are profile-owned.
  The shipped `Stabilize Image` switch always uses full-strength CUDA
  feature/RANSAC registration and automatically activates the fixed-reference
  lock. Obsolete strength, engine, rolling-shutter, and separate tripod
  settings are ignored when encountered in older profile JSON and are no
  longer written. Their runtime implementations and UI adapters have been
  removed.
- Stabilization's fixed-reference lock for clamped cameras tracks from the captured
  reference to the current frame. Per-feature tracking is seeded by the last
  accepted absolute transform and checked in both directions; RANSAC prefers an
  expanded visible-zoom ROI when it has enough texture and otherwise falls back
  to the whole frame. A zoom-scaled display-pixel deadband suppresses
  subpixel shimmer without integrating pairwise drift. Rejected frames freeze
  the last correction and never auto-replace the reference. Reference capture
  and rebuild occur automatically when stabilization starts or the camera
  pipeline resets.
- Optional `Extra Stable` is a second transient layer available with
  stabilization. It continuously runs
  registration while presenting a full-resolution CUDA copy of the last
  model-valid, sharp, settled stabilized frame whenever tracking rejects,
  focus falls below 72% of the lock reference, or the absolute transform
  jumps beyond the impact gate. The production motion-entry gate is 20% less
  sensitive than the initial implementation to avoid premature holds. Five
  settled frames arm recovery and a
  four-camera-frame blend returns to live video without a hard cut. The held
  frame is captured before keystone and later effects, so the normal pipeline
  still treats it consistently. The control is off by default, transient, and
  warns that moving people or content may be hidden while a frame is held.
  Display Colors uses an accessible compact swatch grid and a 256-entry GPU
  luma LUT; custom 2-8 stop gradients/posterize schemes persist globally for
  reuse by profiles. Its native popover has an opaque backing surface, so
  content beneath it never shows through the swatch and editor controls. Wheel
  scrolling never edits selectors or sliders.
- Orientation is applied before the rest of the processing pipeline.
- Settings persist to `%APPDATA%\OpenZoom\OpenZoom\settings.json`. A VLM API
  key entered in AI Settings is protected by Windows Credential Manager; JSON
  stores only an opaque credential id and ignores plaintext `vlmApiKey`
  fields. The environment override remains available for local development
  and is never persisted.
- Snapshots are saved as timestamp-matched `_original.jpg` and `_processed.jpg`
  pairs under `Documents\OpenZoom\Photos\YYYY-MM-DD\` by default.
- Recordings are saved as timestamp-matched `_original.mp4` and
  `_processed.mp4` pairs under
  `Documents\OpenZoom\Recordings\YYYY-MM-DD\`; encoding is live AV1 when
  available and otherwise live H.264.
- The processing status label under Advanced Image diagnostics distinguishes
  CPU, GPU, fallback, debug-view, recording and VLM states without
  covering the Simple camera view. The collapsed Diagnostics group also
  reports rolling 240-sample p50/p95/p99 camera-processing and
  capture-to-present timings; budget warnings follow the negotiated camera
  frame period.
- Photo and annotation image writes use a bounded application image-I/O pool.
  VLM/Codex frame preparation use an independent bounded assistive
  pool, keeping PNG/JPEG encoding, resizing, base64/JSON construction, and
  temporary-file writes off the UI thread. Saturation is reported instead of
  accumulating unbounded work, and cancellation generations prevent stale
  preparation from launching a request.
- The Advanced inspector uses a draggable high-contrast splitter and persists
  its width. Text-clarity and display sliders reflow beneath their labels when
  the inspector is narrow, and feature status labels wrap within the panel.
- Read transcribes the current view through the configured vision provider.
  New Codex settings select Luna with low reasoning; existing explicit model
  choices remain available. There is no separate local recognition engine,
  language-data installation, or automatic text-recognition toggle.
- The non-blocking first-run Setup Assistant installs or updates the official
  Codex CLI and offers NVIDIA Video Effects on supported hardware. Downloads
  are pinned and verified, with a Windows downloader retry on transfer failure.
- Scene Explain defaults to a native Qt JSON-RPC client for `codex app-server`,
  reusing a ChatGPT-managed Codex login. Simple Explain threads are ephemeral
  and always use a read-only, no-network, no-approval policy. Advanced
  Assistant threads are persistent and can opt into internet access or
  workspace-scoped coding; only OpenZoom-created thread ids are indexed in
  settings. Server approval and permission-escalation requests are denied,
  unexpected tool items are interrupted, and turn watchdogs always return the
  UI to idle. The stable app-server surface currently lacks a complete
  per-turn tool allow-list, so the interrupt path remains necessary defense in
  depth. OpenAI-compatible HTTP servers remain an optional fallback.
- The streamed result panel is an owned floating tool window with native
  move/resize handling over the D3D camera surface. Streamed fragments update
  its text without reapplying geometry. Its camera-relative geometry persists
  across restarts, and its first-use position clears the top Simple controls.
  Its follow-up field remains editable during a streamed answer while Ask and
  Enter submission remain blocked; once ready, it attaches the current view
  and sends questions into the shared persistent Advanced Assistant
  conversation.
- Lecture notes are valid per-session HTML documents under
  `Documents\OpenZoom\Notes\`.
  They collect timestamped text readings, scene explanations, and relative captured
  image links that render in a browser and remain portable with the complete
  OpenZoom user-data root.
- Photos, recordings, notes, analysis exports, and console diagnostic logs
  share one user-owned root. Advanced Assistant can select another writable
  root outside the install directory; `Ctrl+Shift+O` and the Advanced action
  open it in Explorer. Console-attached launches tee Qt output to a timestamped
  file under `Debug\`, retain the newest 20 logs, and leave ordinary GUI
  launches quiet. These logs may contain local paths, device names, camera
  identifiers, and timing/error details, so review them before sharing.
  Legacy install-relative output can be copied without deleting its source.
- AI Settings uses a bounded, vertically scrollable dialog with distinct Codex,
  OpenAI-compatible VLM, speech, and notes sections. It displays the
  built-in OpenZoom Codex instruction read-only and persists separate user
  preferences for response language, tone, and detail. Those preferences are
  added to Codex developer instructions without weakening its permission
  policy and become a system message for the OpenAI-compatible fallback. The
  image-capable model list and per-model reasoning efforts are populated
  dynamically from Codex app-server `model/list`; a configured model missing
  from the current catalog remains identified as unavailable.
- Read Aloud is manual-only and uses Qt TextToSpeech over the Windows Runtime
  backend when available. AI Settings lists all voices exposed to desktop apps
  by that backend across installed languages, then persists the selected voice
  and speed. Windows 11 Narrator/Magnifier Natural voice packages are not
  exposed by the public Windows Runtime speech API and are not selectable here.
- `Setup & Downloads` in Advanced reopens the dependency assistant at any time.
  Dismissing its automatic first-run prompt is persisted independently of
  manually reopening it.
- Codex camera-frame temporary files include the owning process id, are
  removed on completion/cancellation/shutdown, and stale files from dead
  OpenZoom processes are swept on the next startup.
- Release publishing is designed for the current private/team distribution:
  unsigned bundles are allowed, an installed code-signing certificate can be
  selected through `OPENZOOM_SIGN_CERT_SHA1`, and
  `OPENZOOM_PUBLIC_RELEASE=1` rejects an unsigned build. Every bundle includes
  SHA-256 checksums, a release manifest, and an SPDX SBOM.

## Documentation Index
- [`README.md`](../README.md): top-level project overview and usage.
- [`docs/code_reference.md`](code_reference.md): authoritative file/class map.
- [`docs/ui_modes_design.md`](ui_modes_design.md): Simple/Advanced layout and settings-ownership contract.
- [`docs/hardcoded_paths.md`](hardcoded_paths.md): machine defaults and magic values.
- [`docs/rotation_ui_notes.md`](rotation_ui_notes.md): orientation ownership and the
  rotated-frame resample contract.
- [`improvement_ideas/00-status-and-priority.md`](../improvement_ideas/00-status-and-priority.md):
  the single implementation tracker — status of every plan and what to do next.
- [`improvement_ideas/08-ml-text-sr-options.md`](../improvement_ideas/08-ml-text-sr-options.md):
  ML text super-resolution research (supersedes the old GPU upscaling to-do).
- [`docs/THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md): third-party attribution and redistribution notes.

## Current Gaps
- Automated tests are still limited.
- CUDA interop still needs broader hardware validation across more driver/toolkit combinations.
- Maxine SuperRes requires a supported NVIDIA GPU and the user-installed
  NVIDIA Video Effects runtime; hardware/runtime and visual-quality validation
  remains necessary across Turing, Ampere, Ada, and Blackwell systems.
- SuperRes inference follows NVIDIA's synchronous sample path on OpenZoom's
  CUDA stream. Its enhanced frame is the sole zoom result rather than a layer
  blended over a separately timed conventional zoom frame. Additional zoom
  uses the live focus point mapped into the clamped 4/3x source crop.
- Subscription-backed AI depends on a compatible installed Codex CLI and
  available account usage. Setup can install or update the per-user official
  CLI from a pinned, verified OpenAI bootstrap script; authentication remains
  an explicit `Connect ChatGPT` action. The fallback VLM mode depends on a
  user-provided OpenAI-compatible `chat/completions` server.
