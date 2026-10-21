# Hard-Coded Paths and Defaults

The optional `OKUFLOW_UCT_LOCAL_BF16_MANIFEST` selector is read only by the app's
private production factory. Its schema-1/prepare=false manifest is at most
64 KiB; converted graphs/plans are strictly below 256 MiB and hash-pinned.
Only the reviewed RTX 4090 UUID/driver/TRT medium/medium N8 fixed1024 variant is
eligible; detector and fallback retain FP32 and existing cache keys. A nonempty
selector prohibits all engine preparation/cache writes. See
[exact local identities and fallback policy](uct_local_bf16.md).

Active regional UCT admission keeps a ten-input-pixel margin at internal crop
edges: shared `kDetailCropEdgeMarginPx=8` plus two pixels of detector jitter
slack (zero at image borders). Ownership includes original detector bounds,
not just tighter fitted ink. It owns complete lines only, retaining crossing
sentences and masks verbatim without renewing their validation clocks. See
[regional policy](uct_active_regional_ocr.md).

Oriented UCT ink fitting keeps at least 35% of the detected word width and
25% of the padded search height, with a four-source-pixel height floor.
The separate minimum word height, confidence and strict pixel-proof gates
retain their existing defaults. See [geometry repair](uct_oriented_detail_repair.md).
Minimum-height comparison permits 1e-5 source pixels of arithmetic error.
An unfitted padded single-scalar baseline with absolute cosine below 0.5 keeps
original pixels under the existing high-risk disposition.

## Owner workstation work storage (2026-10-21)

Large private work folders physically reside under `Z:\Projects\OkuFlow`;
legacy LocalAppData and inactive T3 checkout paths use directory junctions.
The active checkout retains about 20 MiB of source on C: while its build
directory redirects to Z:. Private credentials/app state and small loose files
remain on C:. These are local filesystem aliases, not new application defaults.
Use Z: for new large outputs; see [storage layout](local_work_storage.md).

## Optional embedded video SDK (development)

`scripts/ffmpeg/pins.json` pins FFmpeg 7.1.1, dav1d 1.5.1 and private build
tools. `prepare_ffmpeg_playback.ps1` confines source/tool/build caches to
LocalAppData (`OkuFlow/embedded-video-sdk` by default), using existing GCC 16.2.0
MSYS2. `OKUFLOW_FFMPEG_SDK_ROOT` enables the CMake adapter;
`OKUFLOW_VIDEO_BACKEND=ffmpeg` and absolute `OKUFLOW_FFMPEG_RUNTIME=<sdk>/bin`
select it at runtime. No ambient Qt/PATH DLL discovery or deployment occurs.
Runtime majors are avcodec/avformat 61, avutil 59, swresample 5 and swscale 8.
Decoder threads are four; AVIO reads 64 KiB; probe/index 8 MiB; streams eight;
per-step packet attempts 65,536; dimensions 16,777,216 pixels.
Container inspection is limited to 1,024 top-level and 1,024 child boxes;
exhaustion refuses admission. Upstream internal
allocations are outside the pixel quota. Finite comparison receipts use
`OkuFlow/embedded-video-evidence/<guid>`, the global runner lease and a
180-second child deadline. See [contract and limits](video_ffmpeg_playback.md).

## Selected-video resource limits (development)

The separate GPU OCR recognizer admits true batch capacities `1`, `8`, `16`
through `OKUFLOW_UCT_TRT_BATCH`. With no override, medium/small recognition uses
batch eight and tiny uses one; sixteen remains experimental.
N16 output tensors have a 1 GiB allocation ceiling; detector/N1/N8 tensors
retain 256 MiB. This is an upper
validation bound, not eager allocation or total VRAM budget; TensorRT builder
workspace remains 256 MiB. See [batching and measurements](uct_gpu_batching.md).

Playback uses resolution/stride/PAR-aware decoded admission with a 96 MiB
minimum allowance and 256 MiB absolute ceiling, including consumer-held
frames. This is a cap, not eager allocation. A complete working set must fit
before decoding. Saved-file lookahead is at most two frames (one at larger
admitted sizes); archive lookahead is one. Seek invalidates it without draining.
Optional neighbor preparation waits for a 50 ms quiet period after target
publication; immediate scrubs supersede it. Proven same-frame seeks share pixels
and require neither native decode nor another full-frame allocation.
Additional limits are a 64 MiB byte-read window, an 8 MiB metadata/index budget and
4096 native reads per seek, generation cancellation, a two-second no-progress
deadline and sixty-second total request cap. Only strictly advancing decoded
video PTS renews the progress window; pre-target frames do not allocate
presentation copies. Audio-aligned Resume uses the same bounded preroll policy.
Audio queues include in-flight PCM and are
bounded to 2 MiB, 32 packets and 500 ms; the native output endpoint is capped
at 250 ms. These are admission limits, not latency promises. The selected
reader keeps an ordinary 200 ms lead and remembers handed-off PCM independently
of queue draining. See [video/audio repair](video_audio_preroll_repair.md). The
transport polls at 16 ms only while a recording/file document exists; normal
unrecorded camera mode does not run a history collector.

UCT screen text has at most 128 lines, 512 UTF-16 units per line and 16 KiB
total. Semantic additions publish at one second, with a three-second maximum
reader hold; authority loss revokes immediately. Geometry is queried from the
last actual paint and is independent of OCR cadence. Native UIA/DPI behaviour
remains separately validated. See [screen text](uct_screen_text_accessibility.md).

Advancing-media UCT uses real selected PTS for scene validation, an 80 ms probe
admission interval, a 350 ms minimum OCR interval and the existing 1,500 ms
stable refresh. Its job deadline is separately 30 seconds of wall time.
One scheduler and one latest-only probe worker serve all modes; a paused still
returns to one-shot admission. These are bounded scheduling defaults, not an
OCR-rate or native display-latency guarantee.

Immutable paused-word corroboration considers neighbours within 250 ms, limits
translation to 0.25 normalized scene units per axis, and requires each dimension
to retain at least 75% of the larger word box (intrinsic edges for tilted
quads). Oriented patches sample 8–64 columns by 4–12 rows in the detected
quad frame, with 0.15-height horizontal and 0.3-height vertical margins.
Word registration uses NCC 0.94,
visibility 0.95 and a two-probe-pixel local refinement. Requests are capped at
1024 selected lines / 8192 words, with resident reference/match batches <= 1024.
These limits do not expand the live camera's motion search. Completed worker
checks are consumed even after the UI's 3 ms inline match budget expires.

## Optional PaddleOCR (2026-10-18)

`scripts/uct/pins.json` pins upstream model revisions, model/config/card hashes,
ONNX Runtime CPU 1.31.0 archive/DLL and TensorRT 11.4.0.106 CUDA-12.9 archive.
`prepare_uct_runtime.ps1` defaults to `%LOCALAPPDATA%/OkuFlow/ocr-deps` for SDK
archives/extraction and `ocr-runtime` for verified models, CPU runtime and
notices. `-CacheRoot`/`-RuntimeRoot` select other local roots. No download occurs
on UCT enable or CMake configuration.

`uct_model_pins.hpp` compiles the same six immutable graph/config identities;
the model-free fixture reconciles them against `pins.json`. Tiny detector uses
`det/`, small recognizer uses `rec/`; other tiers use `det/{small,medium}` and
`rec/{tiny,medium}`. Persistent `ultraClearTextModels` stores independent
`gpuDetector`, `gpuRecognizer`, `cpuDetector`, `cpuRecognizer` tokens from
`tiny|small|medium`. Missing/invalid values independently fall back to
medium/medium on GPU and tiny/small on CPU. The shared Boolean
`ultraClearTextEnabled` persists the opt-in; missing/non-Boolean values default
off. Runtime use additionally requires the current Text Clarity master to be on.

`OKUFLOW_UCT_ROOT` selects model/CPU files; otherwise an `ocr` directory beside
the inner application is preferred when it contains a detector, then LocalAppData.
`OKUFLOW_UCT_TENSORRT_ROOT` selects the external SDK at runtime; otherwise the
pinned SDK extraction under `ocr-deps` is used. CMake's `OKUFLOW_UCT_SDK_ROOT`
and `OKUFLOW_UCT_TENSORRT_ROOT` locate optional headers; absent headers compile
fallback stubs. Runtime SDK overrides must match the pinned version. The
runtime `OKUFLOW_UCT_CUDA_BIN`, then `CUDA_PATH_V12_9/bin`, then the documented
private CUDA-12.9.2 toolkit `bin` supplies optional CUDA DLL dependencies.
No compiler-machine absolute path is baked into the OCR loader.

Engine plans stay in `%LOCALAPPDATA%/OkuFlow/ocr-engines`, keyed by graph hash,
GPU UUID, driver and TensorRT/precision identity, with a payload integrity hash.
Model/license data never belongs in Git or capture output. The analysis pool
has two slots, at most 128 MiB each; input dimensions are limited to 8192 per
axis. A Temporal Smooth lease counts both OCR and current-validation images
against that per-slot limit (at most 64 MiB per half, without pitch padding).
It discloses EMA alpha, history epoch, seed and blend count; capture remains
throttled to 80 ms while the neutral EMA advances on actual processed frames.
Neutral EMA/scratch consumes about 20 bytes/pixel per initialized surface;
media replay adds 20 bytes/pixel, before pitch padding. These buffers allocate
lazily and remain cached when disabled. Allocation failures latch per size
(EMA) or surface (snapshots), retrying only on explicit reset or size change.
Smoothed/current input registration requires every candidate word to reach
NCC 0.94 and visible fraction 0.95 with zero translation/refinement. It is
separate from the ordinary 0.82, six-probe-pixel motion tracker. Resident input
registration batches at most 1024 word descriptors and refuses over 8192 words.
Detector long edge is 960 per invocation. Complete-scene context is followed
by at most 16 overlapping detail crops (desired source edge 1280, overlap 160,
at most four crops per axis). Recognition uses a 1024×48 canvas; each of at
most 128 detected regions per pass can add eight overlapping windows for long
lines (targeting 20 source heights wide, eight heights overlap; capped windows
widen to preserve coverage, internal-edge fragments
discarded). CUDA retains all crops/windows on device, with at most 1152 compact
quad descriptors per pass. Merged output is bounded to 1024 lines. Detector
thresholds remain 0.2/0.4 and word confidence at least 0.7.
These are bounded experimental defaults, not accuracy or latency promises.

UCT's optional padded-box ink fit samples about 16K pixels per word, requires
at least 24 luma levels of separation, and refuses a fit under 35% of its input
width/height. Near-identical refresh retention requires IoU >= 0.8 and edges/
dimensions within 10% of word height, plus current independent validation.
The existing 350 ms freshness and 600 ms presentation watchdog are unchanged.
Source-color contrast adjustment targets 7:1 and uses 16 bounded search steps.
Accepted background brightness uses 64 fixed levels; same-polarity foreground/
background changes up to 8 RGB channel levels retain the prior color. A neutral
ink channel spread up to 16 snaps to pure black/white. Only replacement colors
are stabilized; the camera image is not quantized to a global palette.
Oriented word colors use the detected bilinear frame for body/corner/perimeter
samples. Oriented ink fitting removes eight-connected components spanning
opposite search edges, with at most 32768 cells, 8192 columns and 4096 rows.
CUDA uses about 46 KiB fixed shared scratch and at most 64 full propagation
scans; incomplete propagation preserves detector geometry. CPU queue fitting
is exact/linear; incomplete CUDA fitting is conservative, not a parity claim.

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
saves `.okuflow-transcript-<mediaId>.json` beside the recording. The
desktop does not infer a fixed Documents path or read/transcribe those files.

This document tracks machine-specific defaults, generated output locations, and other fixed values that should be reviewed when the toolchain or deployment story changes.

## Installer and icon tooling

Verified WiX 4.0.6 tool files are extracted directly from the pinned NuGet into
`installer-tools/wix-4.0.6/verified-<package-sha256>/`; every file is compared
with the archive before invocation. The prior tool-path shim and ambient
NuGet sources are ignored. CMake writes
`<build>/okuflow-build-facts/<configuration>.json`; the application's successful
post-build step binds it to the executable in `<configuration>.seal.json`.
Metadata requires that matching seal. Its effective compiler,
linked static runtime, architecture list and toolkit-license path replace
ambient CUDA variables as packaging provenance. These paths are build-tool
evidence, not application settings or additional shipped dependencies.

Release orchestration uses `%LOCALAPPDATA%\OkuFlow\release-runs\<version>-<guid>`
for logs, immutable payload snapshots, upload verification and temporary notes.
Its default artifact directory is `%LOCALAPPDATA%\OkuFlow\releases\<version>`;
`-OutputDirectory` must name a fresh directory. Publication targets only
`Cem-Kaya/oku_flow` and immutable `v<version>` tags. Authentication comes from
the existing GitHub CLI session; no credentials enter packages or receipts.

The GitHub Windows workflow installs Qt 6.12.0 under the runner's temporary
directory and takes its actual prefix from `QT_ROOT_DIR`. It pins aqtinstall to
`076e1659807d0b362a3ed684d54c2e9c775eb9c7` for the Qt 6.11+ architecture-split
repository layout. CI logs/screenshots go under `RUNNER_TEMP/okuflow-evidence`,
never the owner's desktop or fixture settings. Action commits are pinned in the
workflow; update them deliberately with CI validation.
The same Qt action installs only the `qtmultimedia` source archive for the
bundle preflight's matching FFmpeg and Qt LGPL notices, with a separate source-aware cache
prefix. Bundle Qt resolution is `QT_PREFIX`, then `Qt6_DIR`, then `QT_ROOT_DIR`,
then the documented local default. An invalid explicit override fails rather
than falling through. CTest supplies its configured SDK/source root to the
preflight fixture and fingerprints the module SPDX and license files it reads.
Qt license resolution checks `<prefix>/LICENSES/LGPL-3.0-only.txt`, then
`<prefix>/../../Licenses/LICENSE`, then
`<QT_SOURCE_ROOT>/qtmultimedia/LICENSES/LGPL-3.0-only.txt`. The final location
supports archive SDKs without requiring the interactive Qt installer.
MSVC setup discovers only Visual Studio 2022 (`[17.0,18.0)`) and uses its installed
`Common7/Tools/Launch-VsDevShell.ps1` with x64 host/target. Android host protocol
checks pin `ubuntu-24.04`; review that image deliberately rather than inheriting
`ubuntu-latest` migrations.
The native GUI runner requests a 1920×1080 display because fixtures include
1600-pixel windows. This changes only the disposable GitHub VM's current mode,
before inventory hashing; it is guarded against local invocation and does not
change the owner's display. Unsupported modes fail with retained evidence.

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
- `cmake/CudaTargets.cmake` selects one shared native architecture inventory:
  CUDA 12.9: `61;75;80;86;89;90;100;103;120;121`; CUDA 13:
  `75;80;86;89;90;100;103;120;121`. The application, CUDA fixtures and cubin
  inventory all consume it. `sm_61` covers the intended GTX 1070 test system;
  it does not establish hardware acceptance or add Maxwell/Volta targets.
- `OKUFLOW_CUDA_TOOLKIT_ROOT` selects an explicit toolkit for a fresh build
  directory. Without it normal CMake CUDA discovery must resolve CUDA 12.9;
  CUDA 13 requires an explicit comparison root. The local
  side-by-side CUDA 12.9.2 developer installation is under
  `%LOCALAPPDATA%/OkuFlow/toolchains/cuda-12.9.2/toolkit`, assembled from
  NVIDIA's SHA-256-verified Windows component archives. CUDA 13.4 and the
  NVIDIA driver remain unchanged; no toolkit is shipped with the app.
  No PTX fallback is embedded; `cuda_architecture_coverage` (CTest) checks
  the built executable carries a cubin for each listed architecture.
- The Windows target uses the static CUDA runtime, so release bundling does not
  copy `cudart`, `nvrtc`, `nvJitLink`, or other CUDA Toolkit DLLs.
  Its license comes from the selected toolkit's `EULA.txt` or newer `LICENSE`
  file; the staged notice remains `licenses/NVIDIA_CUDA_EULA.txt`.
- Supported CUDA staging buffer flag values:
  - CLI: `--cuda-buffer-format=rgba8|fp16`
  - Environment: `OKUFLOW_CUDA_BUFFER_FORMAT=rgba8|fp16`
  - Current implementation still resolves `fp16` back to `rgba8`

## Build Directories

USB recovery uses two consecutive 2000ms shell-response timeouts, a fresh
1000ms non-claiming companion probe and a 2000ms post-ack transport-ID check.
Definitive health/phone responses suppress further health probes for 30 seconds;
reconnect requests have a 60-second cooldown per endpoint/phone. Both budgets
are in-process and reset on restart. Ordinary discovery polls every 5 seconds.
Capture shutdown bounds its D3D11 completion-query poll to 1000ms while retaining
the facade's shared 1500ms stop deadline. These limits avoid unbounded GPU waits
and never authorize killing or replacing a foreign/shared adb server.

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
- Release bundle staging directory: `dist\OkuFlow.staging-<32-hex-guid>\`
  The tracked release batch script is checked out with CRLF on Windows.
- Locked-primary fallback bundle: `dist\OkuFlow2\`
- CUDA replay output and traces generated by CTest:
  `build\msvc-cuda-tests\tests\clamp_bump_replay_*`

## Output Directories
- Test-gate evidence: `%LOCALAPPDATA%\OkuFlow\test-evidence\`, with one hashed
  directory per canonical build/configuration and one `ledger-v2.json` containing
  per-test fingerprints. `hash-cache.json` caches file hashes by full path,
  length and UTC modification ticks. Attempt JUnit/output/detailed CTest log
  names include a timestamp and random suffix. Old failure blockers remain even
  after reusable pass history expires.
  `runner.lock` excludes concurrent tracked runners; `desktop.lock` is the
  per-user Qt fixture lease, shared even across direct CTest invocations.
  `OKUFLOW_TEST_RESUME=1` requests reuse, never an input mismatch override.
  `scripts/test_test_gate.ps1` uses private GUID directories under
  `%LOCALAPPDATA%\OkuFlow\test-gate-contract\`; nothing enters release bundles.
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
- Desktop zoom range: `1x..16x`, with slider hundredths (`100..1600`).
  Modern/legacy settings, custom profiles and UI application use the same
  bound; existing values up to the former 12x ceiling need no migration.
  Fit framing shows the whole source at 1x and retains the saved zoom for Fill.
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
  then retains the base scene (NIS/FSR remains active only if enabled).
  Ten warmup runs are excluded. Strength changes settle for 200 ms before
  reloading. Model load/inference/teardown host calls each have a 1000 ms
  deadline on an owned serial worker; timeout requires a restart and does
  not authorize releasing unfinished GPU allocations.
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
- New bundles have one root launcher, `oku_flow.exe`; supporting EXEs, Qt DLLs,
  plugins, runtime installer, platform-tools and third-party notices/source
  archives live under `app/`. Flat published bundles remain readable. File
  inventories use full root-relative paths; reviewed adb manifests retain their
  original payload-relative paths and exact bytes.
- The bundle script copies the executable, Qt deployment output, `LICENSE`,
  `COMMERCIAL.md`, `README.md` as `README.txt`,
  `docs/portable_start_here.txt` as `START_HERE.txt`,
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
- `--startup-profile-ms` defaults to 15000 ms, bounded to 1000–600000 ms in the
  app. The script accepts 1–600 seconds and 1–5 runs. Only profiling enables
  its precise 20 ms Qt heartbeat with at most 3000 delay samples. That sample
  cap still applies to longer manual cable-recovery sessions. Ordinary app
  launches have no profiling exit timer; UI capture delays remain 1–60000 ms.
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
  prepared version code `4`, name `0.1.3` (published code 3 / 0.1.2 is separate);
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
- Language preference: Android 13+ framework `LocaleManager.applicationLocales`;
  Android 11–12 private preference `language` / `tag`, absent = System default.
  The 25 canonical language codes and Android qualifiers are documented in
  [localization](localization.md). Windows persists `system` or an explicit code;
  `pt-BR` and `zh-Hans` remain full tags. Embedded Qt resources use underscore
  filenames (`okuflow_pt_BR.qm`, `okuflow_zh_Hans.qm`), never machine paths.
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
- Native phone startup has one 12 s deadline including negotiation and initial
  codec retry. `start.startupBudgetMs` grants the phone its actual remainder
  minus a 1 s decoder reserve. Android clamps it to 1–12000 ms, retains at most
  six candidates with at least 500 ms each, and fairly divides remaining time
  after 200 ms retry gaps; every candidate is capped at 4 s. Queue, discovery,
  encoder setup and teardown time count, and pre-video settings restarts keep
  the original deadline. See `android/PROTOCOL.md` for progress fields.
  Windows packet-silence and decoded-output deadlines are independently 12 s;
  HEVC first output in each generation has a 4 s deadline starting at real
  encoded input, not codec configuration. Reconfiguration and
  packet arrivals never reset the decoded-output deadline. Decoded buffers are
  bounded to 64 MiB; Windows
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
  These are trusted same-user executable inputs, including an explicit override;
  runtime lookup is not a signature/pinning security boundary. SDK/PATH fallback
  supports development trees without bundled adb. Owned launches log the exact
  absolute executable path. Packaging and legacy reclamation retain their
  separate reviewed-manifest checks.
  With no default listener/override, hidden direct server starts select a
  private ephemeral loopback port. An exact owned child in a non-inherited
  kill-on-close job dies at normal exit or process crash. Failed launch, child
  exit and confirmed-hang relaunch attempts retain a 30 s cooldown. Existing
  unknown listeners/overrides remain borrowed, never replaced or killed; their runtime
  files may remain locked after OkuFlow exits (`OkuFlow2` fallback).
  Owned health probes use a 100 ms connect deadline and 1000 ms per bounded
  host-version read, with two consecutive failures before an idle restart.
  Readiness polling uses a 5 s deadline and 50 ms slices (an in-progress bounded
  probe can finish beyond that deadline). Live/starting phone sessions and
  cancellation suppress restart. Lifecycle/port/budget state is process-local.
  Bootstrap requires phone-use intent. Explicit webcam selection relinquishes
  idle ownership. The child overrides `ADB_EMU=0` and `ADB_REJECT_KILL_SERVER=1`, logs its private port and
  verifies the IPv4 loopback listener's PID against its held process. A dead
  child can yield to a responsive default listener after its sessions finish; shutdown forbids any
  later owned bootstrap. WinUSB exclusivity is documented in adb_distribution.md.
- Cancellable adb lock waits use 20 ms slices; Port reads use a synchronize-only
  atomic child snapshot. Server epochs observe loopback port/PID/creation time.
  Borrowed hangs need two failures spanning 5 s before phone-intent recovery.
  Legacy bundle reclamation pins `adb-build.json` SHA-256
  `f91532bab5d146a1dd0dc646629afb8ce77a626c0cde0b015e71c8dfcbb00b1c`
  (same as `scripts/adb-source/distribution.json`) and public v0.2.3 launcher
  SHA-256 `8bff15f54bfe5111762cda157a3df9d9716f549b3c0bf2d9f612100ccc288b1d`
  as its metadata-free fallback. The public ZIP matched
  `4ec5e2605e70e021b8211a782abcefbffd9543519234dbe060fa80b16a428148`.
  Keep reviewed pins and provenance docs aligned when changing releases.
  Default reclamation mutex is `Local\OkuFlow.LegacyAdbRecovery.<SID>.<session>`;
  acquisition is capped at 3 s, host-kill connect at 500 ms, graceful/forced exit
  waits at 2 s each. Private fixtures inject their own manifest/launcher hashes,
  root, scan scope, mutex and port, never the owner's port 5037. An unused shadow
  hands back after an empty raw transport list and two healthy default probes.
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

## Agent-control development limits

Development catalogs dev-core, dev-capture and dev-camera remain outside
release/MSI inventories; v1 requires the complete plan-52 gate. Clocks are
monotonic and test-injectable; OS wall time never renews consent. Masters default
off. Bounds: 16 KiB arguments; 32 KiB structured results; eight queued calls;
one async mutation/four reads per context; 128 idempotency reservations;
256 events and 256 operation records; terminal operation retention 900 s.
Embedded mutation budget 20, lease 60; token bucket two per second/burst four;
composites one per two seconds; color/contrast/brightness interval 250 ms.
Three approvals per context/one pending, five-minute maximum lifetime and
thirty-second post-turn grace. Leases expire after 300 s absolute or 60 s idle;
tools cannot widen them. Recording start needs 30 s absolute lease remaining.
Observation limits: three images/turn or six/lease, 256 KiB base64/image,
768 KiB/turn or 1536 KiB/lease. Observation deadline 3 s; other async delivery
5 s (wait uses its 0-5000 ms timeout plus a 250 ms delivery margin).

## HTTP agent loop bounds

The new HTTP agent adapter has no provider URL/model/key default. Caller-owned
configuration supplies the exact completion endpoint; Off is the tool-mode default.
Capability verdicts are session-only per endpoint/model/wire mode and a credential
change clears them. Explicit Test sends one frame-free request (128 response tokens,
15-second deadline); optional parallel-flag acceptance is tested only when requested.
Agent turns use non-streaming 2048-token rounds, at most 8 rounds and 32 calls,
a 180-second wall deadline and 60-second per-request transfer timeout. Each response
is capped at 1 MiB; each serialized outbound request at 2 MiB; arguments at 16 KiB
and result JSON at 32 KiB. At most 3 observation images, 256 KiB base64 each and
768 KiB aggregate are admitted, independently of the authorized initial viewport.
Initial images also obey the 256 KiB per-image cap. Earlier images are stripped.
Context ownership is rechecked between calls/requests and every 100 ms while active.
Origin keys are app-issued, at most 100 characters; model IDs at most 256 characters;
prompt strings at most 65536 characters each and header credentials at most 8192
bytes, accepting visible ASCII only and rejecting whitespace/control bytes. HTTP
redirects are never followed. These are transport
bounds, not live-provider acceptance or public-release evidence.

## Agent-control offscreen evidence defaults

The opt-in `agent_control_capture` fixture writes only to an explicitly named
absolute LocalAppData folder. It registers the installed OS font
`C:/Windows/Fonts/segoeui.ttf` because Windows Qt's offscreen platform does not
enumerate native system fonts. No font is bundled and no stored app settings are
read. The MainWindow fixture uses 1672x941 (the approved concept viewport), the AI Settings dialog uses 820x900,
and a 300-second synthetic approval is confined to the disconnected widget.
An additional 360px Arabic RTL view and floating Explain use the same native
classes. `QT_SCALE_FACTOR=2` writes physical 2× captures to a separate folder;
`OKUFLOW_UI_CAPTURE_DARK=1` applies only a fixture palette.
## MCP development and future packaging contract (0.3.1)

The local endpoint descriptor is `%LOCALAPPDATA%/OkuFlow/mcp-instances/<selector>.json`
(128-bit hex selector, exactly three fields, at most 1024 bytes), with pipe
`\\.\pipe\OkuFlow.Mcp.<selector>`. These files contain neither secrets nor
permissions. The finite build catalogs are `dev-core`, `dev-capture`,
`dev-camera` (current default), `v1` and `dev-full`, indices 0–4. App, bridge and fixtures
use `BuiltControlCatalogEdition()`; a mismatch is rejected.

Development peers verify sibling `oku_flow.exe`; deployed-policy builds verify
`OkuFlowApp.exe`. Packaging requires V1 plus the deployed policy explicitly,
then checks the built bridge's `--build-info` and native imports. Diagnostics
open no endpoint. Accepted packages place `okuflow_mcp.exe` in `app/`, sign and
hash it alongside the launcher/application, and declare its exact bytes and
policy in the manifest/SBOM/MSI inventory. Earlier non-MCP layouts remain
readable. This development work has not promoted or published V1.

Protocol limits and native timeout/quarantine constants are documented in
[MCP control](mcp_control.md): 1 MiB stdio lines, 512 KiB IPC, depth 32,
65536 nodes, 256-byte IDs, eight pending requests, ten outbound messages and
eight retained native-I/O slots of at most 512 KiB. The final image fence is
one second with 20 ms stop checks; reply/frame/heartbeat and native-write
deadlines remain bounded and separate.

Agent observation metrics have a 1000 ms monotonic freshness bound (ObservationService::MaximumFreshFrameAgeMs), independent of image request/transport deadlines.

Development agent camera operation metadata polling uses 50 ms intervals and a 30-second first-frame deadline. These do not replace the facade's 1500 ms total stop/consumer-cleanup deadline.

Processed unique-frame rate uses at most 120 timestamps within two seconds; one second without a unique frame reports zero. This is an application processing rate, not negotiated camera FPS or sensor-to-photon latency.

Shared logical chat text is stored below Qt's owned
`QStandardPaths::AppLocalDataLocation/visible-chat` directory (normally
LocalAppData for OkuFlow on Windows), using only lowercase opaque UUID filenames.
It is outside Drive and release bundles; it contains no images, credentials,
backend references or control grants. Retention is at most 64 conversations /
32 MiB, with each record capped at 1 MiB. This is text history, not notes or
capture media; legacy Codex index/storage paths remain unchanged.

## Bundle transaction paths (RB-003 / RB-004)

- New staging uses one unpredictable `dist/OkuFlow.staging-<32-hex-guid>` name
  per invocation. Packaging never clears an unknown old staging directory.
- Successful replacements retain a complete same-parent backup at
  `dist/OkuFlow.previous-<guid>` or `dist/OkuFlow2.previous-<guid>`. No old runtime
  or owner data is recursively deleted to publish the new generation.
- Publication and toolchain fixtures use GUID roots below
  `%LOCALAPPDATA%/OkuFlow/bundle-publication-fixtures/` and
  `%LOCALAPPDATA%/OkuFlow/bundle-toolchain-fixtures/`. Their logs, fake runtimes
  and injected failures are private evidence, never release payloads.
- `build_release_bundle.bat` accepts only Ninja for its required compiler
  database and selects VS2022 17.x x64. `--check-toolchain` exits before any
  configure/build/dist write; a stale generator cache requires a fresh
  `OKUFLOW_BUNDLE_BUILD_DIR`, without automatic cache deletion.

## Exact recording markers (audit PW-004)

Beside each finalized Original video used by new notes, the serial storage worker
publishes `<actual-video-name>.okuflow-media.json`. This immutable version-1 marker
holds its UUID, basename, exact byte size and `sha256-chain-1m-v1` fingerprint;
chunk size is fixed at 1 MiB, with a 32-zero-byte initial chain state. It has no
machine path or transcript text. A selected root's basename is only a lookup hint;
the exact marker and freshly hashed file bytes authorize handoff/sidecar access.
Transcript sidecars are version 2 and include that complete identity. Their
UUID-addressed filename `.okuflow-transcript-<mediaId>.json` is always 61 ASCII
characters; it never shares a write target with a successor UUID at the same
video path. Legacy `<video>.okuflow-transcript.json` files remain untouched and
are not trusted or migrated automatically. Keep markers
with recordings when moving the tree. No fixed Documents location is inferred.

## Optional performance diagnostic bounds

- `OKUFLOW_MAXINE_PROFILE=1` enables one in-memory ring of 16,384 host-call
  samples per Maxine worker. Destruction emits stage/outcome counts and
  nearest-rank p50/p95/p99/max microseconds to existing Qt stderr logging;
  omitted earlier calls are counted. It is disabled by default and writes no
  per-frame file. Timed-out resources retain their existing ownership.
- `assistant_chat_benchmark` forces Qt offscreen and registers
  `%WINDIR%/Fonts/segoeui.ttf`. It accepts 20..1000 measured deltas, 4..128 even
  messages, 128..2048 initial characters per message and 1..2 views, with a
  production 256 Ki-character total bound and at most eight synthetic previews.
  Default: 200 deltas after ten warmups, 40 messages × 1024 characters, two
  views of 900×720 and 430×720. `--output` explicitly selects the evidence file.
  See [diagnostic commands](performance_diagnostics.md); these are offline CPU
  measurements, not native/Maxine acceptance.
- Optional `--trace-events` keeps at most 32 Qt events taking at least 500 µs
  per delta, with inclusive duration and phase. It is a separate diagnostic
  workload; default benchmark timing excludes this tracing overhead.


## AAC timeline repair bounds
After native Windows marker verification showed AAC timestamp-gap compaction,
recording fills holes with zero PCM on its worker. Each block has at most
4,800 PCM frames; ordinary channel layouts use no more than 64 KiB scratch.
A single hole is limited to ten seconds, with one 250 ms admission-loop budget
for its repair and following speech. Oversized gaps or unavailable credit stop
and finalize existing recording output rather than compacting speech. Native
COM/driver calls retain their separate watchdog limits. See
[recording timeline evidence](recording_attempts_and_timeline.md).


## GPU selected-video resource policy

Extra Stable's analysis lease includes one 32-byte acquisition record copied
with its pixels into pinned slot storage. Workers consume it after the same
completion fence; this is metadata egress, not an image download. Held copies
remain one observation and recovery crossfades name no independent acquisition.

The GPU decoder admits eight logical BGRA texture allocations / 256 MiB across
published leases, pool storage, optional private conversion target and deferred
native release. The CPU decoded allowance remains adaptive 96-256 MiB. These
are separate from CUDA scene/scratch, analysis/model buffers, driver decoder DPB,
Original recording and one explicit Save snapshot. No allocation equals a total
RAM/VRAM ceiling. UCT advancing gap retention uses 350 ms wall validation plus
real-media freshness; gap receipts cannot renew either. Save's owned GPU clone
has a three-second asynchronous readback deadline and retains exact identity
through resize retry/late-result discard. See [GPU playback](gpu_video_playback.md).

UCT readiness limits: eight batch-one TensorRT recognitions per completion wait;
up to 1024 line colors plus 8192 word-local color descriptors in the existing
resident metadata scratch. Paused unsuccessful jobs permit three ordinary
retries at 100-ms spacing; preferred-backend preparation retries use 500-ms
spacing within the existing 30-second horizon. Successful final reads do not
poll. Builder/parser runtime resources load only when a cached engine is absent
or invalid; cache identity and runtime pins are unchanged.

The explicit `OKUFLOW_UCT_CHARACTER_DIAGNOSTIC=1` fixture writes unique
`%LOCALAPPDATA%/OkuFlow/uct-character-<timestamp>-<uuid>` evidence. Its synthetic
source is 960 x 128, with 12 edits, sizes 14/24/44, requested and asserted actual
decimation 1/2/4, line/token extents and three existing matcher policies. The
installed `C:/Windows/Fonts/consola.ttf` is loaded and hashed for local provenance,
never redistributed. Synthetic translation is (2,1) pixels; it is not physical
camera acceptance. No ordinary-runtime setting or OCR-reuse policy is enabled.


## Optional paused-video UCT diagnostic (2026-10-21)

The separate explicit paused-video diagnostic uses
`OKUFLOW_UCT_PAUSE_DIAGNOSTIC=1`, absolute private image paths in
`OKUFLOW_UCT_PAUSE_SELECTED` / `OKUFLOW_UCT_PAUSE_NEIGHBOUR`, and their actual
millisecond PTS in `OKUFLOW_UCT_PAUSE_SELECTED_MS` /
`OKUFLOW_UCT_PAUSE_NEIGHBOUR_MS`. It writes unique
`%LOCALAPPDATA%/OkuFlow/uct-pause-proof-<timestamp>-<uuid>` reports.
`OKUFLOW_UCT_PAUSE_OUTPUT_ROOT` overrides only that evidence parent and must be
absolute. The existing
`OKUFLOW_UCT_TENSORRT_ROOT` must name an inference-only SDK, and pinned M/M N1/N8
plans must already be cached; no download or engine build is permitted.
`OKUFLOW_UCT_PAUSE_BRIDGE=1` adds a real Windows OCR pair before TensorRT to
inspect same-source refinement; its two explicit CPU OCR readbacks are counted
separately from the zero-egress GPU-only mode.
Its frame-sequence mapping assumes this private source's 60 FPS. The selected
and neighbour images must differ and be within 250 ms. Inputs and reports are
never bundled, and ordinary CTest does not select the diagnostic. See
[paused continuity repair](uct_paused_continuity_repair.md) for its scope.

## Optional UCT shadow diagnostics (2026-10-20)

`OKUFLOW_UCT_REGIONAL_OCR=1` opts into supplemental execution and implicitly
enables the planner; shadow alone stays observation-only. Active work is at
most one region per full interval, <=480 source/detector pixels, magnification
one. The first cost estimate is 750 ms, later max(previous, 2*duration+80 ms),
with a controller probe-period reserve and 80 ms execution reserve. It normally
fits 1500 ms stable refreshes, not 350 ms demanded reads. Full deadlines/priority
remain unchanged; execution rechecks steady-clock slack and counts overruns.
Failure/overrun disables optional work until worker recreation. Outside pending
evidence retains exact shared source tickets at an 8192-word cap; regional
unions retain 1024 lines/8192 words/32768 tokens or masks. See
[active policy and limits](uct_active_regional_ocr.md).

`OKUFLOW_UCT_TIMING_TRACE=1` enables a cached process-wide metadata trace.
Normal teardown writes once with exclusive creation under
`%LOCALAPPDATA%/OkuFlow/uct-timing/uct-timing-<pid>-<steady-ns>-<n>.jsonl`.
It retains at most 4096 numeric records, reports dropped/incomplete chains,
and never records pixels, recognized text, source paths or credentials.
Tracing does not request models or uploads. See [trace contract](uct_timing_trace.md).

`OKUFLOW_UCT_SHADOW_PLANNER=1` opts the app's existing OCR worker into a
metadata-only observer. Default operation allocates no planner. One optional
focus descriptor is copied per submitted job; pan/zoom cannot enqueue OCR or
refresh acquisition age. Default tracking uses 2048 occurrences, 1024 lines,
8192 words, an 8x8 unseen-coverage grid and 24 hypothetical selections (six
reserved for old coverage debt). Every plan leaves full OCR authoritative.
These are experimental planner limits, not a text cache or a latency claim.
Observer text is capped at 4096 UTF-16 units per word and 1 Mi units per
observation for each raw/prepared representation; retained occurrences have a
separate finite 8 Mi-unit worst case. Oversize observations reject without
truncating full recognition output.
The finite resident diagnostic has a separate
`OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER=1` selector that requires retained frozen
source identities and records plans beside full outputs; it never fabricates
fresh evidence by assigning new frame IDs to copies.
## Experimental recognizer width profiles

UCT crop merging retains 70% ordinary overlap. Contained literal prefix/suffix
fragments smaller than 35% of the larger box can match at 65%; row association
requires at least half the larger word height and a gap no greater than four
smaller heights. Stable exclusion-box jitter is bounded to 10% of its smaller
height, with unioned protection and unchanged proof clocks. File-token extension
exceptions admit one to sixteen ASCII letters. See
[coverage and stability](uct_coverage_stability_repair.md).

`OKUFLOW_UCT_TRT_WIDTH` accepts empty/`fixed1024` (existing keys) or `bucketed`.
`OKUFLOW_UCT_TRT_PREPARE_WIDTH=1` permits a separate one-time preparation of
only the selected N; normal bucketed inference is cache-only. The new cache
identity appends `-rec-width-v1-buckets128-256-512-1024-min1x3x48x128-optNx3x48x512-maxNx3x48x1024-n`
and actual capacity, retaining model/GPU UUID/driver/TRT 11.4.0.106/FP32 pins.
The minimum, optimum and maximum shapes must match after deserialization.
`OKUFLOW_UCT_RESIDENT_WIDTH` selects the same two policies in the diagnostic,
which refuses preparation. Group metadata is bounded to 144 integers / 576
bytes in each device/host buffer. See [width validation](uct_recognizer_width.md).

## Paused visible UCT detail bounds (2026-10-21)

`UctTiming::immutableDetailSettleMs` defaults to 300 ms (clamped 0–60000).
`immutableDetailMagnification` defaults to `kMaximumDetailMagnification` (6);
region selection clamps it to 1–6 and retains the 960-pixel detector long edge.
A detail pass needs at least 1.25 times the established detector scale.
Coverage retains up to 32 attempted regions per immutable selection, including
failed attempts, without eviction. A containing region at 90% of the requested
scale counts as already covered. See [geometry/detail repair](uct_oriented_detail_repair.md).

`AcceptedLine::carvedSceneRects` classifies a subset of the existing 32768
protected-mask union cap. It adds no independent history or mask allowance.
Original masks round outward when either projected axis has no pixel centres;
carved remainders do so only when both axes lack centres. Exact original/carved
rectangle collisions retain original protection through transfers/projection.
