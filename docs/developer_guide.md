# OkuFlow developer guide

Public BF16 preparation and its versioned new-only cache are documented in
[uct_public_bf16.md](uct_public_bf16.md). The production corpus catalog is empty;
the private validation factory does not activate public BF16. Actual worker
measurements and model-quality limits are in
[uct_worker_pilots_2026_10_23.md](uct_worker_pilots_2026_10_23.md).

OkuFlow was formerly named OpenZoom. Website: <https://okuflow.com>. Repository: <https://github.com/Cem-Kaya/oku_flow>.

OkuFlow is a Windows-only camera magnifier built around Qt 6, Media Foundation, Direct3D 12, and an optional CUDA processing path. The current codebase already supports live camera capture, GPU (CUDA) frame processing with CPU format-conversion support paths, a two-stage preset/advanced UI, rotation-aware presentation, persistent settings, paired photo snapshots, and live AV1/H.264 recording with optional microphone audio in fragmented MP4 containers.

## Current Capabilities

- A development [host UCT merge candidate](uct_merge_optimization.md) removes
  repeated geometry/overlap work inside the existing GPU recognizer pipeline.
  Frozen CPU replay/timing is prepared; it remains uncompiled and unmeasured
  pending parent START, with no worker, fallback or FPS gain established.
- Development [live RAM recap](live_ram_recap.md) keeps up to five minutes of
  compressed, video-only camera history independently of recording. It reuses
  native codecs/player/transport, reports actual rolling bounds and warmup,
  and retains an exact displayed scene for manual Freeze before coded history
  is ready. Camera source/format/stop changes reset this temporary authority.
- The owner's explicit [local BF16 recognition selection](uct_local_bf16.md)
  uses one reviewed cache-only medium/medium N8 fixed1024 plan with a pinned
  FP32 detector. Empty/unset `OKUFLOW_UCT_LOCAL_BF16_MANIFEST` and public
  diagnostic factories retain the normal FP32 baseline. This is mixed
  arithmetic with FLOAT I/O, not a general precision default or release claim.
- An optional [embedded FFmpeg/dav1d saved-file backend](video_ffmpeg_playback.md)
  uses a separately pinned private SDK and explicit runtime opt-in. The default
  and all recording archive facades retain Media Foundation. No new SDK/DLL is
  automatically installed or deployed; HDR tone mapping remains unsupported.
- Development video playback uses one bounded Media Foundation decoder and the
  Original recording archive for same-session rewind. Opening a saved video
  first finalizes ongoing recording; a new recording returns to the camera.
  The native bottom transport, immutable UCT and exact frame saving share the
  selected document. New video is Original-only; photo pairs and legacy notes
  are preserved. See [the implementation contract](accessible_video_implementation.md).
  CUDA builds prefer native GPU video textures and a private CUDA scene,
  retaining CPU compatibility. See [GPU playback](gpu_video_playback.md) for
  device ownership, format limits and explicit image-action transfers.
  This is not a new published Windows installer.
  Paused saved-file UCT can use a genuine following sample at its known native
  frame duration, capped at 2500 ms, without pressing Play. Unknown duration,
  archive/RAM and advancing modes retain ordinary proof timing; see
  [source identity and sparse-cadence bounds](uct_initial_paused_frame.md).
- Development UCT retains tilted/perspective word geometry through ink fit and
  painting. Paused navigation can request bounded visible-region detail after
  settling, up to eight-times detector enlargement under the existing 960-pixel
  limit. See [geometry and detail repair](uct_oriented_detail_repair.md).
- UCT advanced settings persist separate PP-OCRv6 detector/recognizer tiers
  for GPU (medium/medium by default) and CPU fallback (tiny/small).
  Medium/small GPU recognizers default to batch eight; tiny retains batch one.
  Existing saved model choices and explicit batch overrides win. All three
  tiers use compiled immutable pins, native combo data and the same four
  Assistant/MCP image-setting fields. Model replacement retires old workers
  asynchronously. The shared UCT opt-in preference persists; Text Clarity gates
  runtime activation without clearing that preference. See [UCT](ultra_clear_text.md).
  The regional planner supports separate observation-only and explicitly
  enabled supplemental execution modes on the same OCR worker. Active regions
  preserve baseline full deadlines/priority and current-pixel admission; they
  never authorize skipping full OCR. See [shadow planning](uct_region_planner.md)
  and [active execution and limits](uct_active_regional_ocr.md).
  An opt-in [narrower recognition canvas](uct_recognizer_width.md) reduces
  padding within ordered batches. It uses separate cached dynamic-width plans;
  the fixed-width default remains available and unchanged.
- Experimental UCT has optional PP-OCRv6 TensorRT CUDA and ONNX Runtime CPU
  adapters, with built-in Windows OCR as the final fallback. Prepare pinned
  development inputs with `scripts/prepare_uct_runtime.ps1` (`-TensorRT` opts
  into the external SDK), then reconfigure. See [UCT](ultra_clear_text.md) for
  dependencies, independent analysis/validation workers and scoped acceptance.
  These components are not yet a published installer inventory.
  The app's GPU tiers now prefer BF16 when supported, with same-tier FP32
  loading/build fallback. Accuracy and FP32-agreement tests are diagnostic only;
  no perfect-text corpus controls startup. Tiny recognition remains N1 and CPU
  remains FP32. See the owner-policy update in [BF16 runtime work](uct_public_bf16.md).
  Runtime discovery also supports a private `ocr/` beside the inner application:
  original model/configuration/CPU files plus `pins.json`, and TensorRT under
  `ocr/tensorrt/bin/` (optional CUDA dependency directory `ocr/tensorrt/cuda/bin/`).
  Explicit OCR/TensorRT/CUDA environment overrides take priority; legacy
  LocalAppData development installations remain the fallback. Graphs and corpus
  assets live separately under `models/uct/`. Discovery does not stage or approve
  these payloads. Keep the reviewed license/dependency/SBOM and clean-VM gates.
  The exact first-party converter source is retained in
  `scripts/uct/precision_models.py`; Python tooling is developer-only.
- Lecture notes have larger text, remembered text-size and colour controls,
  media-type filters, grouped conversations, and timestamped transcripts.
  Preview the viewer locally with `python -B scripts/render_notes_preview.py`;
  it writes sample notes/media under `%LOCALAPPDATA%\OkuFlow\notes-preview\`.
- New recorded-video lecture notes include a Transcribe button. Open the notes
  in Chrome and allow access to the OkuFlow folder containing the recording;
  a second first-use click opens the local-only tool at okuflow.com/transcribe/.
  Pick the speech model and processor explicitly there. Audio/text are never
  uploaded by the tool. Returned text is shown under the recording and saved
  beside it as a `.okuflow-transcript-<UUID>.json` sidecar. Existing notes files are
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
  An in-flight native `ReadSample` also has a 10-second working-time watchdog
  (four negotiated frame periods for slower sources, up to 60 seconds). It is
  polled without waiting on the driver; sleep/hibernation and image processing
  do not consume that pending-read budget. Recovery uses the existing bounded
  split stop. If the driver never quiesces, its resources stay retained and a
  restart is required rather than attempting unsafe reuse.
  Reopening a webcam reader does not announce reconnection: the first successful
  frame presentation does. Zero-frame attempts retain the original 30-second
  recovery window; an adopted reader waits for video instead of reopening on
  every tick, and expiry retires it even while opening or waiting for video.
  Working manual or fallback video also retires an older recovery window,
  without announcing that the original camera returned.
- CPU frame pipeline for format conversion and rotation of formats the GPU path does not consume, plus the legacy debug composite view. NV12 and YUY2 camera frames bypass it entirely: color conversion and rotation run in CUDA.
- Direct3D 12 presenter for swap-chain output plus GPU texture readback.
  Rotation-correct clean Original video recording uses a bounded
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
  separately installed Video Effects runtime is available. Its independent
  producer owns a private CUDA stream and input/output buffers; model loading,
  inference and teardown do not wait on the UI. It discards 10 warmup samples,
  including completed stale jobs for the matching configuration, and falls
  back if its next 60-frame average exceeds the 24 ms latency target. Advanced
  reports the measured average and offers a compact checkbox override when
  latency is the only failure. New model loads and neural frame calls are
  deferred during pan/zoom and a 150 ms navigation tail; the same camera
  frame uses the normal spatial/base fallback in that interval. Generic
  redraw, photo and readback retries do not count as navigation. This does
  not preempt an SDK call already running, but it no longer waits for that
  call. The presentation clock publishes only exact-current-scene results;
  photos/recordings keep immediate same-frame fallback while SR is pending.
  See [ownership and capture policy](maxine_async_producer_design.md).
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
- CMake 3.25 or newer (the checked-in schema-6 presets require it).
- NVIDIA GPU plus CUDA Toolkit 12.9 if you want the default CUDA path.
  CUDA 12.9 is the compatibility build for Pascal GTX 10-series (`sm_61`)
  through RTX 50-series. CUDA 13 comparison builds require an explicit toolkit
  root and retain Turing and newer targets; normal discovery cannot silently
  select CUDA 13.
  Physical GTX 1070 acceptance remains separate from compilation coverage.
  `OKUFLOW_CUDA_TOOLKIT_ROOT` optionally selects a side-by-side toolkit in a
  fresh build directory. See the [one-time toolchain comparison](cuda_toolchain_comparison.md)
  for measured kernel results and the portable local installation.
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

Routine checks now run in [GitHub Actions](https://github.com/Cem-Kaya/oku_flow/actions/workflows/windows-checks.yml)
on a disposable Windows VM, including a CPU build and serial native GUI tests.
MSVC setup uses Microsoft's installed Visual Studio 2022 developer shell through
`scripts/initialize_windows_ci_toolchain.ps1`, exporting its x64 environment to
later Actions steps without a JavaScript setup-action dependency. Android host
protocol checks use the explicit `ubuntu-24.04` image rather than a moving alias.
The Actions page also permits individual GUI groups/two-case diagnostics and
stores logs, JUnit and screenshots as artifacts. This does not use the owner's
desktop or establish CUDA/physical-device acceptance; see
[remote checks and their scope](../tests/README.md#github-actions-away-from-the-owner-desktop).

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
CUDA configuration, runs the CPU suite, and then builds the CUDA-enabled suite
and runs the tests the CPU suite cannot cover (`gpu`, `cuda-variant` and
CUDA-only names; `OKUFLOW_FULL_CUDA_SUITE=1` runs all of them). Background tests
run in parallel; GPU/codec and GUI tests stay serial. The two real CTest presets
treat an empty test directory as an error.

For the edit loop, check only what a change affects:

```powershell
pwsh -NoProfile -File scripts/agent_check.ps1
```

It selects tests from the changed files through the configured Ninja graph,
builds only those targets in `msvc-cpu`, runs the selected background tests and
reports a SCOPED result with any selected GUI cases still pending. Build-system,
runner and unknown changes require the full helper above. See
[fast scoped checks](../tests/README.md#fast-scoped-checks-while-editing).

Production sources shared between the CPU application and test fixtures compile
once per explicit OBJECT group. Share a source only after its consumers'
preprocessed output and code-generation flags match; the headless
`shared_source_equivalence` test enforces that after edits to shared sources,
definitions, includes or toolchain flags. See
[shared production objects](../tests/README.md#shared-production-objects-and-precompiled-headers).

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

On the owner's workstation, use `Z:\Projects\OkuFlow\local-work-data` for new
large work folders. Existing C: cache and checkout paths may redirect there;
the active source directory has a small Windows-lock exception. See
[local work storage](local_work_storage.md) before creating or moving builds.

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

Release provenance now comes from CMake's per-configuration
`okuflow-build-facts/Release.json`. The bundle checks that manifest against the
cache and built application, uses its effective CPU/CUDA tier for capability
selection, and stages the license from the compiler's actual toolkit. Metadata
requires `-BuildDirectory` and checks the staged application and CUDA notice;
ambient `CUDA_PATH` cannot change the recorded toolkit version or target
architectures. A successful application build seals the facts and executable
hashes; configure-only changes cannot relabel old CPU/CUDA bytes. Reconfigure
and build an older tree to create this evidence. See
[packaging provenance fixtures](../tests/README.md#packaging-provenance-fixtures-rb-001rb-002).

For a complete build-to-GitHub release, use `scripts/publish_windows_release.ps1`
with a new version, reviewed notes, a previous MSI fixture and `-Publish`.
It calls the tracked matrix and bundle gates, verifies both packages, exercises
the installer only inside a disposable guest, then rehashes uploaded draft assets
before publication. See [release automation](windows_distribution.md#automated-build-package-and-publication).
The ordinary GitHub CPU/GUI workflow produces test evidence, not CUDA release
downloads. Run the release command on a suitable NVIDIA/Windows machine with an
explicitly free test desktop; it refuses partial/bypassed gates.

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
For CI-installed SDKs, the bundle also recognizes `QT_ROOT_DIR` after the
explicit `QT_PREFIX` / `Qt6_DIR` overrides and before its local default. It logs
the selected deployment prefix and still validates the deployment tools, Qt
license, module SPDX files and FFmpeg source notice. Hosted Windows checks
install just the matching Qt Multimedia source archive for that notice and
its `LICENSES/LGPL-3.0-only.txt`. The latter supplies the Qt notice when an
archive-installed SDK lacks the SDK/online-installer license directories.
Missing all three license locations still fails preflight.

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
  provenance, the required small-agent translation review policy and the native
  wording/layout/screen-reader checks still pending.
- `Ultra Clear Text (experimental)` at the end of `Advanced > Image > Text
  clarity` recognizes steady printed text with optional local PaddleOCR or the built-in Windows OCR engine
  and draws sharp letters over validated words; everything else stays the
  original image. Its saved preference is gated by the Text Clarity master
  switch. It offers `Show original camera text` and a selectable `Recognized
  text` pane, and reports its active CPU/GPU backend. Temporal Smooth and Extra
  Stable retain exact analysis/held-frame identity; paused video can use a
  distinct real neighbouring sample without advancing the held view. It reports
  when Windows has no OCR language. Development feature, not release-accepted; see
  [Ultra Clear Text](ultra_clear_text.md).
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
- Recording remains available while Draw is active. The single Original MP4
  stays clean, without viewing effects, annotations or selection handles.
  Selected-frame saves can include the admitted view's UCT and ink.
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
  clean Original recording, and pressing Draw again leaves drawing mode.
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
  magnification from whole-picture Fit through Fill (1x) to 16x. Existing zoom amounts and saved profiles keep
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
- `Start Recording` writes one `VID_<timestamp>_original.mp4` under
  `Documents\OkuFlow\Recordings\YYYY-MM-DD\`, with one encoder and optional
  capture-clock-synchronized 48 kHz mono AAC. Conversion and source orientation
  are included; view crop, colors, UCT, Super Resolution and Draw ink are not.
  CPU fallback within a GPU-started writer preserves that Original timeline.
  AV1 remains preferred with H.264 fallback; Stop does not transcode.
  Source-format changes create `_partN` Originals. The 12-hour duration and
  disk-space guards stop the session. Stop releases queued work and finalizes
  the in-flight sample; failed attempts/nonempty outputs remain preserved.
  Only a successfully finalized selected Original is added once to notes.
  Session-owned regular zero-byte outputs with no accepted samples may be
  removed; old recording folders and legacy pairs remain untouched.
- A current recording exposes verified fragments for same-session rewind.
  Pausing owns the last successfully displayed scene and its exact admitted
  PTS or explicit capture-time mapping. Opening a saved file first stops and
  finalizes recording. Starting a new recording leaves saved playback only
  after preflight succeeds. Stop preserves the selected recording cursor and
  play/pause intent. The forward transport follows live for a recording or
  holds the last decodable saved frame; Resume continues the held cursor.
  Unrecorded camera use now keeps the separate bounded silent RAM recap. See
  [archive and player ownership](video_archive_backend.md) and
  [document lifecycle](video_document_lifecycle.md).

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
  Assistant conversations. Detailed save status propagates through the settings
  controller without serializing again to diagnose a refusal. Each discrete
  save still freshly validates the existing file before rotating its backup.
- User-created files share one root: `Documents\OkuFlow\` by default.
  Advanced Assistant can change it to another writable folder outside the
  application directory. The choice persists as `paths.userDataRoot`.
- Snapshot pairs and annotated captures are written under
  `Photos\YYYY-MM-DD\`; new Original recordings are written under
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
- [`docs/ultra_clear_text.md`](ultra_clear_text.md) for the experimental OCR glyph overlay, its
  clocks, provenance invariants and validation boundaries.
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

Phone startup retains one 12-second Windows deadline, including listener setup
and any initial codec retry. The receiver sends its remaining phone budget with
a one-second decoding reserve; Android divides that remainder across at most
six fallback modes and reports candidate progress. Queues, discovery, setup and
retry gaps consume the same budget. HEVC's four-second decoder watchdog begins
with real encoded input rather than codec headers. See the
[startup timing contract](../android/PROTOCOL.md#startup-budget-and-candidate-progress).
Validate changes with `scripts/build_android.ps1 -Lint`, the host protocol suite
and the tracked Windows `phone_camera_contract` gate; fake clocks and protocol
fixtures do not establish real-phone startup or stalled-driver recovery.

### Progress deadlines

Steady-state GPU completion and deferred camera readback have one-second
progress deadlines. They observe actual completion, not camera FPS or VSync.
A GPU fault retains unfinished resources and displays a persistent restart
message. A readback failure instead enters the existing camera reconnect path.
Phone packet and decoded-output progress are independent, so arriving compressed
packets cannot indefinitely hide a decoder that stopped producing pictures.

Plan 52 implementation is staged on the agentic-control feature branch. The
headless `agent_control_contract` and `codex_agent_profile_contract` fixtures
exercise typed policy and offline compatibility boundaries; they do not establish
working MCP interoperability, live provider acceptance or complete UI/hardware
acceptance. Keep the dedicated agent child independent of Read/Explain history.

### Agent-control development

Plan 52 is being implemented in an isolated feature worktree. The typed core
and shared production capture entry points have desktop-free fixtures; this is
not a published MCP API or completed Assistant-control feature. Built-in
Assistant control defaults on; External MCP control defaults off.
Saved version-17 choices are preserved. Missing keys and pre-control settings
use these defaults. Settings schema 17 records only switches and conversation kind/
toolset; session grants, approvals, leases, operations and undo never persist.
Unknown conversation kinds are tool-free. Unsaved changes remain live until
exit; restart loads the last successfully committed settings file.

`agent_control_contract` uses QCoreApplication and no camera/provider. The
exact-session recording-manager fixture uses a null button and no encoder/input
frames. Run these through the tracked matrix; set OKUFLOW_AGENT_EXCLUDE_GUI=1
while the desktop is reserved and report partial validation. Native controls,
screen readers, camera transitions, observation freshness, MCP interoperability
and provider acceptance remain separate gates. See src/app/agent/README.md and
code_reference.md for interfaces and current implementation limits.

### Opt-in offscreen Assistant screenshots

Build `agent_control_capture` explicitly and run `--output` with an absolute
LocalAppData evidence folder. It constructs production Qt widgets with fixture
settings, forces offscreen and registers installed Segoe UI. The 1672×941 state
matches the approved chat-first concept: chat, contextual photo approval, settings
drawer and separate AI Settings masters. The built-in control default is on;
external MCP remains off. It creates no app service,
provider, camera or owner foreground window. Actual native widget peers are
composed without repairing pixels. Preserve raw originals and compare the same
viewport/state before recording visual findings. This does not replace native
GUI or hardware acceptance; see [the UI contract](agent_control_ui.md).

### Shared Explain and Assistant chat (development)

Advanced Assistant and floating Explain render the same pixel-free
`VisibleChatSession` snapshot. Draft/attachment/busy state is shared; each view
owns its scrolling and selection. Explain always sends the current viewport
through the tool-free Explain path. A typed followup can use the restricted
agent adapter when the owner has enabled it, within the same visible chat and
with isolated backend IDs. Read and periodic analysis never add, select or
clear conversation rows. Historical Assistant/Coding continuation and existing
rename/export/delete remain kind-safe; agent history is a presentation restart.

Provider/configuration transitions retire backend bindings and old request
tickets. New/history also stop manual TTS and retire owned control resources.
The notes writer remains the sole completion recorder. Headless local fixtures
exercise this contract; no live provider, camera or native desktop acceptance
is implied by a fixture pass.
## Agent-control packaging under development (0.3.1)

The Plan52 worktree intentionally remains on the `dev-camera` catalog. Its
new packaging guard refuses development catalogs before running bundle tests.
After both-adapter/native/hardware acceptance, the release configuration must
explicitly select `OKUFLOW_CONTROL_CATALOG_EDITION=v1` and
`OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY=ON`. This does not bypass the existing full
matrix, bundle or disposable installer gates.

The accepted bridge is deployed inside `app/` beside `OkuFlowApp.exe`, leaving
one public root launcher. Signing, native dependency checks, staged/published
byte comparisons and manifest/SBOM/MSI inventory include it. Metadata requires
the matching build directory whenever the bridge is present. MSI 0.3.1 and
later refuse a bundle without that contract. `scripts/test_mcp_bundle.ps1`
validates declaration/cache/inventory rejection without creating a release;
`test_bundle_layout.ps1` preserves earlier layouts. Current published downloads
remain separate from this feature work.

Logical Explain/Agent chats are now durable first-party text history under the
owned application-data `visible-chat` folder. Saved records contain completed,
failed or interrupted text, draft/title/timestamps and bounded plain receipts;
they contain no images, tool request payloads, secrets, grants or provider thread
references. Restore is a tool-free Explain logical chat, and the next restricted
Agent turn opens a fresh child/thread using completed text context only.
Legacy Codex histories remain separately kind-safe. Atomic writes retain the
previous record on replacement failure; oldest owned records enforce the
64-record/32 MiB quota. Store/restore never replay tools or append notes.

Agent-owned Scene Explain is context-scoped rather than a saved periodic-mode
preference. Each analysis consumes the existing Observe frame/byte budget and
rechecks core authority at actual provider submission and completion. Ending or
revoking that scope does not alter the native-owned scene checkbox. Assistant
provider/model/prompt configuration remains an exact native Configuration
approval domain and cannot widen external source/capture/frame grants.

Expanded assistant settings conservatively deny actor overlay enabling and
changed runtime/output preferences while native periodic Scene Explain intent
is enabled; configuration cannot silently resume unbounded legacy egress.
Turn that native intent off through its native control before changing those
preferences. The separate scoped scene tool remains context/frame-budget
bound. Native editor strings above finite wire limits remain unchanged;
values too large for a projection explicitly require native UI inspection.

Release manifest freshness preserves the typed UTC timestamp returned by
PowerShell JSON parsing. Run `pwsh -NoProfile -File scripts/test_publication_time.ps1`
after changing publisher timestamp handling; its offline fixtures cover UTC,
explicit offsets, JSON date conversion and rejection of ambiguous timestamps.

Annotation snapshots now encode into a private same-directory temporary and
publish by atomic no-clobber rename. Timestamp collisions receive unique suffixes;
only committed files are added to notes. Failed encode bytes stay unpublished.


## Recording encoder fallback evidence

Failed AV1 or native-GPU initialization retains its output while the next
codec/input attempt uses an exclusively created GUID sibling. Notes use the
selected writers' actual paths after both finalize. See [recording attempts
and audio timeline validation](recording_attempts_and_timeline.md) for file
ownership, regression fixtures and the opt-in marked Windows AAC diagnostic.

### Transactional publication and toolchain checks (RB-003 / RB-004)

The bundle entrypoint now defaults to Ninja and initializes Visual Studio 2022
x64 itself. Ninja is required because the mandatory shared-source equivalence
fixture consumes `compile_commands.json`. Other generators, including Visual
Studio and Ninja Multi-Config, fail before configuration. Generator options in
`CMAKE_ARGS` are refused; use `CMAKE_GENERATOR=Ninja`. Keep an old cache intact
and select a fresh `OKUFLOW_BUNDLE_BUILD_DIR` when migrating generators.

Run `scripts\build_release_bundle.bat --check-toolchain` to check VS2022 x64,
Ninja, CMake, Qt deployment/license prerequisites and any existing cache without
configuring, compiling or modifying `dist`. Discovery uses the registered VS
root and fixed initializer path rather than recursively scanning its files.
The batch disables the initializer's auxiliary telemetry within its own local
environment and streams setup diagnostics; full compiler/Qt checks still run.
Normal builds force Release, tests and compilation-database export after
optional CMake arguments.

Each invocation owns a fresh `dist\OkuFlow.staging-<guid>\` directory. The
publisher validates every prior runtime file against its manifest and refuses
owner `output`/`captures`, unlisted files/empty directories, reparse paths,
running images and locked files. A primary refusal leaves it untouched and
selects exactly `dist\OkuFlow2\`; an unsafe secondary also stays untouched and
fails publication. The switch uses same-parent directory renames with rollback,
then verifies all new bytes. The complete preceding runtime remains in
`OkuFlow.previous-<guid>` or `OkuFlow2.previous-<guid>` for recovery; packaging
never recursively deletes these generations. Transaction-failure staging is
retained for diagnosis. Review retained generations manually when reclaiming
space, with the app closed and owner captures kept separately.

Private regressions are documented in
[tests/README.md](../tests/README.md#bundle-publication-and-toolchain-fixtures-rb-003--rb-004).
They do not substitute for the tracked release matrix or real installer checks.

## Exact notes recording selection (audit PW-004)

New notes cards bind the finalized Original file to an immutable UUID/size/content
fingerprint in `<original-video>.okuflow-media.json`. The existing serial notes
worker creates/reuses that marker and embeds it in the card; hashing is never on
the application UI thread. The notes browser checks the selected folder's marker
and actual bytes in bounded 1 MiB slices before handoff, restoration or saving.
Two unrelated trees with the same folder/video names cannot substitute content,
even when size and modification time match. Keep the marker with its recording
when moving/copying the notes tree. Version-2 transcript sidecars carry the same
identity. The filename is `.okuflow-transcript-<UUID>.json` (61 characters,
independent of the video basename), so an old writer cannot overwrite a new
recording's transcript after an awaited file operation. Legacy basename-only
sidecars, size/time sidecars and unbound old notes do not restore/transcribe
automatically; playback and direct selection in Transcribe remain available.

The website verifies the transferred File before model/cache work and echoes its
identity on the private channel. Stop during fingerprint I/O uses its existing
job fence; stale work cannot create a worker or publish results. These changes
preserve media-relative transcript times and immutable serialized sidecar saves.
Run the desktop-free `node --test scripts/test_notes_transcribe_bridge.mjs` and
sibling `npm run check`, then the tracked native notes fixture when permitted.
This patch's Node contracts are local boundary evidence, not actual Chrome grant,
codec, native-UI or release acceptance. Preview regeneration now preserves
immutable markers; use a fresh `--out` folder after changing generated media.

## Opt-in chat and Maxine performance evidence

Use the excluded `assistant_chat_benchmark` target for bounded offline chat
render tails and controlled `DeadlineWorker` waits. `OKUFLOW_MAXINE_PROFILE=1`
separately collects bounded actual Maxine host-call distributions during an
authorized native session. Eligible append-only chat updates now reparse only
the maximum-width final assistant body, with full-render fallbacks and reading
anchor/geometry parity checks. Neither diagnostic proves native
pan-to-present/stale-frame performance. See [measurement commands and
limitations](performance_diagnostics.md).

Native marked AAC/video verification reproduced and then closed three-second
AAC gap compaction on the tested Windows codec. Missing PCM intervals now use
bounded worker-side silence with sample-count accounting. The fixed synthetic
markers differed by about -0.633 ms; this does not establish physical microphone
clock accuracy or long-soak fidelity. See the linked timeline evidence.
