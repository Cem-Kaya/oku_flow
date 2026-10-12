# Hard-Coded Paths and Defaults

## Browser notes transcription

The opt-in notes renderer defaults to `%LOCALAPPDATA%\OkuFlow\notes-preview\`.
`--out` selects another fixture folder; `--lang en|tr|de` renders a local draft
whose media and transcription folder grants follow the real data-root layout.
It uses PowerShell 7 for optional synthetic speech and FFmpeg when available;
these are development helpers, not bundled runtime dependencies.

New notes use `https://okuflow.com/transcribe/?ui=<document-language>` only to
open the static browser tool. Recording bytes and segments travel over a
private MessageChannel, not URL parameters or a local server. Chrome stores
the user-selected directory handle in `okuflow-notes-files` IndexedDB and
saves `<original-video>.okuflow-transcript.json` beside the recording. The
desktop does not infer a fixed Documents path or read/transcribe those files.

This document tracks machine-specific defaults, generated output locations, and other fixed values that should be reviewed when the toolchain or deployment story changes.

## Installer and icon tooling

WiX 4.0.6 is SHA-256-pinned in `scripts/windows-installer/tools.json`; its
developer-only binaries live under `%LOCALAPPDATA%\OkuFlow\installer-tools`.
MSI/MSIX snapshots and extraction evidence use `installer-build`, `store-build`
and `installer-validation` in the same local root. `-OutputDirectory` controls
installer outputs (default `dist\Installers`); no credentials enter those files.
The fixed MSI UpgradeCode represents the product family, not a device.
Store package identity is public account metadata in `store-identity.json`,
not a signing key. `OKUFLOW_SIGN_CERT_SHA1` is a certificate pointer only.
The SVG exporter caches compilation under `icon-build` and defaults to the
documented Qt 6.12.0 SDK; `-QtPrefix` overrides it.

## Recording admission defaults
- `RecordingEncoderBacklog` admits at most 12 AV1/H.264 video samples or
  128 MiB estimated raw input payload per writer, counting pending and incoming
  samples. Both streams must admit a frame before writing the pair.
- Startup reserves pending plus next input: each payload must fit within
  64 MiB. CPU and compatibility input count BGRA bytes; native GPU input counts
  NV12 bytes. This eligibility check does not certify texture-pool or driver
  support for an otherwise eligible resolution.
- The capture burst retains six pending frames while recording and catches up
  on saturation. Diagnostic refresh is 400 ms and skips hidden-label computation.
- The recording worker wakes every 10 ms while GPU conversion readers hold
  source leases, so retirement can progress without another complete frame pair.
  No timed wake runs for empty readers or CPU-only recording.
  Ordinary admission uses `DONOTFLUSH`; idle retirement with no queued video
  permits query-command flushing without waiting for GPU completion.
- AAC input admission permits at most 128 outstanding PCM blocks, 4 MiB of
  estimated PCM input, and two seconds of PCM per writer. Cumulative PCM-frame
  accounting uses confirmed 1024-frame LC AAC packet progress with two retained
  packets of conservative credit; stale/padded output cannot retire later input.
  These are input estimates, not bounds on undocumented driver/MFT allocations.
- Limits do not certify codec quality; low-latency encoding is a request.
  Synchronous COM/Finalize latency and decoded AAC playback gaps remain separate
  issues. See [review 47](../improvement_ideas/47-sequential-priority-fixes-2026-10-11.md).

## Lecture Camera Test Harness
- `scripts/lecture_camera.ps1` uses `%LOCALAPPDATA%\OkuFlow\lecture-camera\`
  for downloaded media, private portable OBS copies, logs, session identity,
  and video selection; override with `-StateDirectory`.
- Default OBS executable: `%ProgramFiles%\obs-studio\bin\64bit\obs64.exe`;
  override with `-ObsPath`. Default file: `media\lecture.mp4`; override with
  `-VideoPath`. Start seeks to `1500` seconds (`-StartSeconds`), loops playback,
  and outputs an aspect-preserving `1280x720` camera at `30 FPS`.
- `scripts/lecture_camera_sample.json` records the selected YouTube source,
  publisher offline URL, byte count, SHA-1, and attribution. Downloads of
  alternative YouTube URLs use external `yt-dlp` (`-DownloaderPath`, PATH,
  then `%USERPROFILE%\.oz-tools\venv\Scripts\yt-dlp.exe`).
- Private DroidCam Virtual Output plugin package: official release `0.2.2`,
  SHA-256 `49cbeff0037046336e04bffd0e4499a5419be74580c1a9bd882e0cb197b6e527`.
  Extract with `%ProgramFiles%\7-Zip\7z.exe` (`-SevenZipPath` override), verify
  x64 PE machine type, and enable the private profile's
  `DroidCamVirtualOutput/AutoStart`. The existing modern DroidCam Video driver
  exposes the scene to OkuFlow's Media Foundation capture; native OBS Virtual
  Camera also runs for DirectShow clients. No kernel driver is installed.
- Control uses authenticated OBS WebSocket v5, a random available TCP port,
  2-second connection and 5-second response timeouts, and a 40-second startup
  deadline. Shutdown allows 10 seconds for normal exit before a verified
  owned-process force-stop. Files remain local and outside release bundles.

## Assistant Capture Deadline
- Queued Explain/Read and Advanced Assistant frame captures expire after
  5 seconds, independently of rendering. Individual GPU readbacks retry after
  1 second inside that deadline. A generation token invalidates deadlines on
  completion, cancellation, camera shutdown, and presenter faults.

## Qt
- Default Qt prefix: `C:\Qt\6.12.0\msvc2022_64`
- Referenced by:
  - `scripts/build_and_run.bat`
  - `scripts/build_release_bundle.bat`
  - `cmake/CMakePresets.json`
  - `README.md` examples
- Override with `QT_PREFIX` or `Qt6_DIR`.
- Release packaging requires the matching Qt `Sources` component. Its normal
  sibling location is `C:\Qt\6.12.0\Src`; override that discovery with
  `QT_SOURCE_ROOT`. The bundle copies the exact FFmpeg
  `LICENSE.LGPL-2.1-or-later.txt` from that source tree and the `qtbase`,
  `qtimageformats`, `qtmultimedia`, `qtspeech`, and `qtsvg` SPDX JSON
  documents from the selected runtime's `sbom` directory.
  Qt PDF's document is copied when installed and required when `Qt6Pdf.dll`
  is deployed; PDF is not needed to build OkuFlow.

## CUDA
- CMake default: `OKUFLOW_ENABLE_CUDA=ON`
- Text-SR adapter default: `OKUFLOW_ENABLE_TEXT_SR=ON` for CUDA presets and
  Windows helper builds, and `OFF` for the CPU preset. Both Windows build
  scripts pass the value explicitly so a stale CMake cache cannot silently
  disable it; set the environment variable to override this default. It
  runtime-loads NVIDIA Video Effects and never links or redistributes its
  proprietary libraries or model files.
- Preferred environment variable: `%CUDA_PATH%`
- Secondary probe: `%CUDA_PATH_V*%`
- Hard-coded CUDA architectures in `cmake/CMakeLists.txt`: `75;86;89`
- The Windows target uses the static CUDA runtime, so release bundling does not
  copy `cudart`, `nvrtc`, `nvJitLink`, or other CUDA Toolkit DLLs.
  Its license comes from the selected toolkit's `EULA.txt` or newer `LICENSE`
  file; the staged notice remains `licenses/NVIDIA_CUDA_EULA.txt`.
- Supported CUDA staging buffer flag values:
  - CLI: `--cuda-buffer-format=rgba8|fp16`
  - Environment: `OKUFLOW_CUDA_BUFFER_FORMAT=rgba8|fp16`
  - Current implementation still resolves `fp16` back to `rgba8`

## Build Directories
- `scripts/build_and_run.bat` configures `build\`
- The helper prefers launching:
  1. `build\cmake\Release\oku_flow.exe`
  2. `build\Release\oku_flow.exe`
  3. `build\oku_flow.exe`
- CMake presets build into:
  - `build/msvc-debug`
  - `build/msvc-release`
  - `build/msvc-cpu`
  - `build/msvc-cuda-tests`
- Release bundle build directory: `build/release-bundle`
  (`OKUFLOW_BUNDLE_BUILD_DIR` overrides it, for example with a local directory
  outside the synced checkout).
- Tracked agent release compile: `build/msvc-release`, or
  `OKUFLOW_AGENT_RELEASE_BUILD_DIR` for an isolated output while that executable
  is running. CPU/CUDA test build directories retain their preset defaults.
- Release bundle staging directory: `dist\OkuFlow.staging\`
  The tracked release batch script is checked out with CRLF on Windows.
- Locked-primary fallback bundle: `dist\OkuFlow2\`
- CUDA replay output and traces generated by CTest:
  `build\msvc-cuda-tests\tests\clamp_bump_replay_*`

## Output Directories
- Release bundle output: `dist\OkuFlow\`
- Default user-data root:
  `%USERPROFILE%\Documents\OkuFlow\` through
  `QStandardPaths::DocumentsLocation`
- Root override: persisted as `paths.userDataRoot`; an empty value selects the
  default. The selected root must be writable and outside the application
  directory.
- Snapshot output: paired
  `Photos\YYYY-MM-DD\IMG_*_original.jpg` and `IMG_*_processed.jpg`
  (commit temps append `.writing`, and an active transaction uses
  `IMG_*.pair.lock`; `.pair.pending` certifies both encodes completed before
  renaming. Occupied stems receive a UUID suffix, with at most 32 reservation
  attempts. Recovery completes marked pairs where possible, removes abandoned
  temps, and preserves final JPEGs while reporting unresolved marked pairs.)
- Recording output: paired
  `Recordings\YYYY-MM-DD\VID_*_original.mp4` and `VID_*_processed.mp4`
- Lecture notes output: `Notes\NOTES_<timestamp>_<UUID>.html`; captured-image URLs are stored
  relative to each notes file. Assistant-analyzed frames are saved to
  `Notes\images\AI_<UUID>.jpg` (long side capped at 1920 px, JPEG quality 88)
  only when the associated answer is ready for storage. Cancellation releases
  the retained frame; failed HTML appends remove their newly encoded images.
- Assistant export default: `Analysis\OkuFlow_Assistant_*.txt`
- Console-attached diagnostic logs:
  `Debug\OkuFlow_<timestamp>_pid<process-id>.log`; the newest 20 are retained
- Legacy `<install>\output\` files are never migrated or touched. Release
  publishing never transfers, removes, or preserves user artifacts.

## Runtime Defaults
- Camera shutdown worker deadline: 1500 ms shared across producer quiescence
  and final cleanup, plus caller-side GPU draining bounded to 1000 ms. A
  timed-out session is retained until process exit and further capture is
  refused. Presenter fence waits and CUDA stream drains each cap at 1000 ms;
  fence removal probes use 50 ms slices and frame-latency waits cap at 100 ms.
- Asynchronous CUDA camera-copy lease deadline: 1000 ms. Completion permits
  producer reuse; failure/expiry retains ownership and requires restart.
- NIS/FSR ROI enlargement: up to 2x per pass, bounded by existing scene/cache
  buffers and 2560x1440 (or portrait 1440x2560). Partial caches have a one-texel
  guard for bilinear edge sampling. Further magnification is display scaling.
- Untagged or unknown camera YUV matrix/range defaults to BT.601 limited.
  Explicit BT.601/BT.709 and limited/full-range metadata are respected by
  every converter. Other explicitly tagged encodings are rejected.
- Notes storage: one serial background queue, at most 128 outstanding jobs /
  256 MiB retained payload. Pending analysis images have a separate three-frame /
  192 MiB cap. Paths and content are captured when submitted. Queue or storage
  failure is reported to the UI; accepted work drains at normal app shutdown.
- CUDA surface initialization retries use a steady-clock backoff of
  1, 2, 4, 8, 16, then 30 seconds, capped at 30 seconds. Device, fence, camera
  session, scene extent, or upscale-cache extent changes allow immediate retry.
- Settings path: `%APPDATA%\OkuFlow\OkuFlow\settings.json`; organization domain: `okuflow.com`; Windows AppUserModelID: `CemKaya.OkuFlow`
- Settings schema version: `16`. Newer versions fail closed; invalid or
  unreadable settings are preserved with a timestamped suffix and the latest
  valid `.backup` is restored when available.
- Settings file size limit: `16 MiB` (`kMaxSettingsFileBytes`, 16,777,216 raw
  bytes with CRLF line endings counted) for `settings.json` and its `.backup`.
  Loading reads at most one byte past the limit in binary mode, so a file that
  grows during the read is still bounded. An oversized primary is preserved as
  `settings.json.too-large-<UTC timestamp>` and the valid backup is restored.
  Save refuses a larger document before touching either file. An oversized
  primary found during a save is set aside under the same suffix and is never
  rotated into the backup. Replacement staging happens before moving it;
  commit failure attempts a no-replace restore. A missing primary recovers
  its valid backup at startup (including an interrupted sidecar publication). The conversation index has no count limit. A
  regression measures 10,000 maximum-length entries (60-character titles and
  160-character previews of three-byte UTF-8 text) plus 500 custom quick modes
  at 10,325,496 bytes.
- `cameraSymbolicLink` stores physical camera selection; documents without it
  retain legacy `cameraIndex` startup selection until an identity is saved.
  A missing identified camera falls back to the first available device.
- `assistive.pendingCredentialRemovals` stores only opaque credential ids.
  Failed removal or settings cleanup remains durable until a later save retries.
- Host CUDA upload admission has three event-guarded pinned slots; busy slots
  cause an immediate frame drop while retaining the previous completed scene.
- Recording canvas selection is captured at session start; live selection
  changes affect the next session. CPU fallback for a GPU-started writer is
  tightly packed BT.601 limited-range NV12 with averaged 2x2 chroma.
- SuperRes timing restarts for changed source/output dimensions or effective
  strength: discard the preceding configuration's pending timing, exclude ten
  warmup runs, average sixty, and apply the existing 24 ms guard. Its advertised
  1.33x step maps to exact 4/3 with at most one source pixel of SR pan snapping.
- Recording frame rate target: `30 FPS`
- Recording codec order: live AV1 first, then live H.264 fallback; the same
  codec must initialize for both original and processed files
- Recording duration cap per file: `12 hours`
- Rotation options: `0`, `90`, `180`, `270` degrees clockwise
- Viewport framing default: `Fill (crop)`; the global alternative is
  `Fit (show all)` with symmetric black bars.
- Viewport motion default: `Auto (up to 120 FPS)`. Explicit choices are
  `60`, `90`, `120`, and `Match display`; all are clamped to the active
  monitor's current refresh rate without changing camera FPS.
- Viewport motion remains in the high-rate state for `150 ms` after the last
  input update, then idles at the negotiated camera rate. Native-window resize
  requests are coalesced for `16 ms` before resizing the swap chain.
- Assistive analysis cadence: roughly `1600 ms` between periodic HTTP vision submissions
- Pipeline timing windows retain `240` samples. Advanced Diagnostics reports
  nearest-rank p50/p95/p99 for camera-processing tick time and
  capture-to-present latency; the processing warning budget is the negotiated
  camera frame period.
- Codex scene explanations are on-demand only; periodic assistive polling does not spend Codex subscription usage.
- Default Codex model: `gpt-6-luna`; the client uses the current image-capable
  app-server default when that id is unavailable.
- Default Codex reasoning effort: `low`.
- Default Assistant Instructions: reply in the request's language unless asked
  otherwise, using concise and easy-to-understand answers. The field is editable
  and shared by Codex and the OpenAI-compatible provider.
- Read Aloud engine preference: Qt `winrt`, with `sapi` used only after a WinRT
  engine error; default speech rate is `0.0` (normal) on Qt's `-1.0..1.0` scale.
- The voice picker enumerates every locale exposed by the selected public
  Windows speech engine. Narrator/Magnifier Natural voice AppX packages are not
  exposed to desktop apps through `Windows.Media.SpeechSynthesis`.
- Speech never starts automatically; only the result panel's Read Aloud button
  or the AI Settings Preview button invokes text-to-speech.
- Text local-statistics radius: `clamp(frame width / 32, 8, 128)`, producing a
  local window approximately 1/16 of the frame width.
- Text scene classes: paper at mean luma above `0.62`, board below `0.34`, and
  mixed content otherwise. Automatic polarity follows luma-histogram skew,
  with mean luma below `0.46` used only for near-symmetric histograms.
- Focus statistics are sampled every fourth pixel and copied asynchronously
  every 15 frames; default variance threshold is `0.012`.
- CLAHE uses an `8 x 8` tile grid and 256-bin clipped histograms.
- Display Colors contains `17` stable persisted indices (`0..16`).
- Keystone correction history retains at most `32` accepted corner sets.
- Viewport-target Maxine SuperRes activates at zoom `>= 1.33x`; its smallest
  vendor-supported exact ratio is `4/3x`. The mutually exclusive, profile-owned
  faster mode raises minimum zoom to `2.0x`, changing a `1280x720` target's
  crop from `960x540` to `640x360`. Ultra full-frame mode does not require
  viewport zoom and caps its separate cache at landscape `2560x1440` (or
  portrait `1440x2560`): `1280x720` uses `2x`, `1920x1080` uses `4/3x`, and
  an input already at the cap remains native. All modes read models from the
  runtime's `models` subdirectory. After 60 timed runs SuperRes disables
  itself for the current CUDA surface when average runtime exceeds `24 ms`,
  then uses NIS/FSR. Ten warmup runs are excluded.
- NVIDIA architecture mapping for Setup Assistant downloads: compute 7.5 =
  Turing, 8.9 = Ada, other 8.x = Ampere, and 10.x or newer = Blackwell.

## Assistive Runtime Environment
- Optional Maxine runtime directory override: `OKUFLOW_MAXINE_PATH` (with
  `NV_VIDEO_EFFECTS_PATH` supported for NVIDIA SDK compatibility).
- Default Maxine runtime root:
  `%ProgramFiles%\NVIDIA Corporation\NVIDIA Video Effects\`, followed by the
  matching 32-bit/64-bit uninstall registry entry.
- Setup Assistant URLs and SHA-256 values are pinned in
  `src/app/setup_assistant.cpp`: the official Codex Windows bootstrap script
  and NVIDIA Video Effects 0.7.6 Turing/Ampere/Ada/Blackwell installers.
  Qt downloads time out after 60 seconds without activity, then retry through
  Windows `%SystemRoot%\System32\curl.exe` with the same pinned digest.
  The verified Codex bootstrap uses `CODEX_NON_INTERACTIVE=1` and verifies
  its selected release package against OpenAI's checksum manifest.
- Optional Codex executable override: `OKUFLOW_CODEX_PATH`
- Codex discovery fallbacks:
  `%LOCALAPPDATA%\Programs\OpenAI\Codex\bin\codex.exe` and
  `%LOCALAPPDATA%\Microsoft\WinGet\Links\codex.exe`
- Codex app-server working directory: `%TEMP%\OkuFlow\assistant\`
- Optional Advanced Assistant coding workspace: user-selected in AI Settings; no default folder is granted.
  Enabling it changes persistent Assistant sessions to `workspaceWrite` for
  that folder, which intentionally permits local coding tools including Python
  and shell commands. Simple Explain and non-coding sessions remain read-only.
- Attached Codex frames use temporary
  `okuflow_codex_<pid>_*.jpg` files. They are removed on completion, cancellation,
  and shutdown, and startup removes leftovers whose owner process is no
  longer running.
- Codex control-plane replies time out after `60 s`. A turn idles out after
  `90 s`; on-demand vision turns cap at `180 s`, persistent Assistant turns at
  `30 minutes`, and an unanswered interrupt gets a forced local reset after
  `5 s`.
- Codex protocol buffering is capped at `4 MiB`, one message at `2 MiB`, one
  answer at `256 Ki` characters, and loaded transcripts at `200` messages of
  at most `32 Ki` characters each.
- Optional VLM endpoint configuration:
  - `OKUFLOW_VLM_API_URL`
  - `OKUFLOW_VLM_API_KEY`
  - `OKUFLOW_VLM_MODEL`
  - `OKUFLOW_VLM_PROMPT`
- A VLM key entered in AI Settings is stored under Windows Credential Manager
  target `OkuFlow/VLM API Key`. Settings JSON stores only
  `vlmCredentialId`; plaintext `vlmApiKey` fields are ignored because no
  plaintext-key release was ever deployed. `OKUFLOW_VLM_API_KEY` is a
  process-only development override and is never persisted.

## Redistributed Runtime Files
- The bundle script copies the executable, Qt deployment output, `LICENSE`,
  `COMMERCIAL.md`, `README.md` as `README.txt`,
  `docs/THIRD_PARTY_LICENSES.md`, the Lucide
  notice, the applicable Qt/FFmpeg texts, exact Qt module SPDX documents,
  CUDA/FSR/NIS/Maxine-header notices for CUDA builds.
- `scripts/build_release_bundle.bat` validates `Qt6Core.dll`,
  `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Network.dll`, and
  `platforms\qwindows.dll` before publishing, and verifies the published
  executable against the tested build by SHA-256.
- Every bundle contains `SHA256SUMS.txt`, `release-manifest.json`, and
  `SBOM.spdx.json`. The top-level SPDX document refers to the copied Qt module
  documents through checksummed `externalDocumentRefs`, models separately
  installed tools as optional dependencies, and omits CUDA-only packages from
  CPU-only bundles. `OKUFLOW_SIGN_CERT_SHA1` optionally selects a
  current-user Windows code-signing certificate; `OKUFLOW_PUBLIC_RELEASE=1`
  rejects a bundle when no certificate is configured. The default remains an
  unsigned private/team bundle.
- It explicitly removes Qt's optional `opengl32sw.dll` and copies no NVIDIA,
  CUDA Toolkit, Maxine, model, or language-data binaries.
- It does not copy an install-relative `output` folder. All current user
  artifacts live outside `dist`, and legacy migration is owned by the app. If
  a pre-migration primary bundle still contains `output`, publishing leaves it
  untouched and uses `OkuFlow2`; an `OkuFlow2` legacy tree is never deleted
  automatically.

## Startup Profiling Defaults
- `OKUFLOW_UI_CAPTURE_PATH` enables one raw composed UI PNG only during an
  explicit `--startup-profile` session. It must be an absolute `.png` path with
  an existing parent. `OKUFLOW_UI_CAPTURE_DELAY_MS` defaults to `15000`, accepts
  integer `1..60000`, and must precede profiling exit. Capture requires a
  successful camera presentation at least `80 ms` earlier, a complete frame on
  one screen, and own-process foreground with no overlapping foreign window
  above the main frame. Z-order traversal is bounded to `4096` windows.
  See [`ui_capture.md`](ui_capture.md) for isolation and retake instructions.
- `scripts/profile_startup.ps1` defaults to two 15-second trials and
  `build/startup-profile/<mode>-<run>/` reports, logs, settings, and data. It
  passes an explicit report path; the app uses its sibling `settings.json`
  instead of the normal Windows known-folder settings path. A child APPDATA
  environment override alone does not isolate Qt's Windows settings lookup.
- `--startup-profile-ms` defaults to 15000 ms, bounded to 1000–60000 ms in the
  app. The script accepts 1–30 seconds and 1–5 runs. Only profiling enables
  its precise 20 ms Qt heartbeat with at most 3000 delay samples.
- Initial setup notices/checks wait 1500 ms after the window is shown. The
  initial camera worker starts before show, after app services/settings exist.
  D3D11 conversion retries remain bounded to 25 ms, with a single query per
  event-loop attempt. Normal D3D12 frame-slot admission uses zero-timeout polls.
- Three consecutive D3D11 query deadlines latch safe-copy input for the current
  capture session. Query success resets the streak; camera stop/start resets
  the latch. CUDA copy ownership waits do not consume this deadline policy.
- Optional capture diagnostics report native source and converted reader modes
  separately.

Update this file whenever these defaults move or new fixed paths are introduced.

## Android Companion

- Package/application id: `com.okuflow`, Kotlin namespace `com.okuflow.camera`,
  version code `2`, name `0.1.1`;
  min API `30`, compile API `37.2`, target API `37`. Pins live in
  `android/gradle/libs.versions.toml` and `android/app/build.gradle.kts`.
- User-owned JDK: `%LOCALAPPDATA%\Programs\jdk-21`;
  `OKUFLOW_ANDROID_JDK` overrides the build script's process-only Java choice.
  Persistent `JAVA_HOME` and PATH are not changed.
- User-owned SDK: `%LOCALAPPDATA%\Android\Sdk`; user `ANDROID_HOME` is set.
  `android/local.properties` stores only the ignored SDK path.
- Build/cache root: `%LOCALAPPDATA%\OkuFlow\android-build`, overridden by
  `OKUFLOW_ANDROID_BUILD_DIR` (absolute path outside the checkout/Y: drive).
  APK: `app\outputs\apk\debug\app-debug.apk`; project cache:
  `project-cache`; Kotlin persistent data: `kotlin-persistent`.
  Gradle distributions/dependencies use `%USERPROFILE%\.gradle`.
- Setup archives/logs: `%LOCALAPPDATA%\OkuFlow\android-setup`.
- `scripts/build_android.ps1` requires PowerShell 7, checks Android translation
  key parity, then assembles/tests; `-Lint` adds lint. `-Install` uses the SDK
  adb and requires exactly one authorized attached device. Its port `5037`
  protocol probe has `3 s` connection/read/write deadlines, a `1..16`-byte
  version payload, and refuses conflicting server endpoint overrides.
- Settings preference file: `setup`; Boolean `camera_permanently_denied`
  defaults false and is cleared on camera grant. Backup and device-transfer
  rules exclude installation-specific data.
- Exact tool versions, vendor SHA-256 values, and build commands:
  [`android/BUILDING.md`](../android/BUILDING.md).
- Camera preferences: shared-preference file `camera`; defaults are automatic
  camera/resolution/bitrate, maximum 60 FPS, preferred HEVC with AVC fallback,
  optical stabilization if available, autofocus on and zero exposure compensation.
  Light is session-only, always off at connection/startup; legacy `torch`
  preferences are removed rather than restored.
- Runtime endpoint: abstract socket `okuflow_camera`, uid 2000/0 only;
  loopback adb server `127.0.0.1:5037`. Control payload 64 KiB, video 16 MiB,
  video prefix 20 bytes. Hello deadline 5 s; keepalive inactivity 30 s;
  blocked writer 3 s; video queue 32 packets/32 MiB; control 128/512 KiB.
- USB-disconnected broadcast grace: 2 s before retiring the host session.
  An already authorized foreground listener remains bound across unplug/replug;
  Pause, permission loss or debugging explicitly off stops it. New service
  requests require a resumed activity; foreground refusal waits for a new resume.
- Encoder startup tries at most six candidates with 4 s first-frame deadlines.
  Windows startup deadline 12 s and decoded buffer bound 64 MiB; Windows
  H.264 limits 4096×2304 / 251658240 pixels per second. SEVERE thermal state
  requests at most 30 FPS; CRITICAL stops. See [`PROTOCOL.md`](../android/PROTOCOL.md).
- Probe output: `%LOCALAPPDATA%\OkuFlow\camera-probes\<timestamp>`;
  non-Windows fallback `~/.local/share/OkuFlow/camera-probes`, or `--output`.
  The Python probe reads one protobuf snapshot (64 KiB) and does not own adb's
  lifecycle. Windows can bootstrap a missing local server as described below.

### Windows phone discovery defaults

- No phone serial/model is embedded. Saved `cameraSymbolicLink` is a user
  preference; `cameraSelectionExplicit` distinguishes a manual webcam choice
  from an implicit fallback. Runtime transport IDs are never saved.
- Discovery runs asynchronously every 5 s, with a 100 ms UI completion pulse.
  Probe ACK deadline is 1500 ms; listing reads are 150 ms. Unknown listing
  results preserve the cache. Failure retries back off 5/10/20/60 s; busy/hot
  starts wait at least 30 s. A new transport or readiness transition resets it.
- `ADB_SERVER_SOCKET` accepts `tcp:<port>` or local `tcp:<host>:<port>`;
  `ANDROID_ADB_SERVER_PORT` and `ANDROID_ADB_SERVER_ADDRESS` accept local TCP.
  Default is `127.0.0.1:5037`; nonlocal/invalid overrides disable USB phones.
  Overrides are owner-managed and never trigger server bootstrap.
- Adb lookup: `OKUFLOW_ADB_PATH`, executable-relative `platform-tools/adb.exe`,
  `ANDROID_HOME`, `ANDROID_SDK_ROOT`, PATH, then environment-derived
  `%LOCALAPPDATA%/Android/Sdk/platform-tools/adb.exe`. No user path is compiled.
  Hidden direct loopback server starts are throttled to 30 s and never replace
  or kill an existing listener. A server can keep its runtime files locked after
  OkuFlow exits; use the documented `OkuFlow2` bundle fallback.
- Automatic activity launch is limited to one attempt per serial/transport.
  A manual phone selection renews it. Permission, busy and thermal probe
  problems wait for phone-state changes instead of forcing foreground every 5 s.
- Source builds use `%LOCALAPPDATA%/OkuFlow/adb-source-build` by default;
  `scripts/build_adb_from_source.ps1 -BuildRoot` selects another local root.
  Inputs and GCC 16.2.0 are pinned in `scripts/adb-source/pins.json`.
- Staging uses `-SourcePath`, `OKUFLOW_PLATFORM_TOOLS_DIR`, or the default
  `adb-source-build/build-output` directory, in that order. This is a build-output
  root containing `platform-tools`, `licenses` and `sources`, not an SDK directory.
  `scripts/adb-source/distribution.json` pins the reviewed manifest; every file
  is checked before and after copying. Google SDK binaries are not accepted.
- The source-built version is `37.0.0-okuflow.1` (protocol 41). Runtime closure
  includes adb, both USB DLLs and their dependency DLLs. Corresponding source
  and relinking inputs are `sources/adb-37.0.0-okuflow-source.zip`; notices are
  under `licenses/adb`. See [adb_distribution.md](adb_distribution.md).

## Microsoft Windows runtime pin

- `scripts/vc-runtime/pins.json` pins x64 Microsoft runtime 14.44.35211.0,
  the minimum supported version, required system DLL names, signed installer
  SHA-256 and exact embedded EULA hash. Newer compatible system versions pass.
- Runtime detection reads both x64 registry views and verifies DLL versions
  under the system directory. No machine-specific installation path is used.
- The deployed native launcher is `oku_flow.exe`; its Qt child is
  `OkuFlowApp.exe` beside it. The vendor installer is `vc_redist.x64.exe`.
  Neither a phone model nor serial is part of these pins.
- See [runtime setup](windows_runtime_setup.md) for user approval, temporary
  copy ownership and clean-VM acceptance limits.
