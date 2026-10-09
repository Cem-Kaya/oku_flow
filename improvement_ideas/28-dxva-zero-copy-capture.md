# Plan 28 — DXVA and Zero-Copy Capture (plan 20 P6, promoted to its own plan)

Status: **STAGES 0-4 IMPLEMENTED — EXTERNAL-MEMORY ×100 AND LONG NO-TEAR
SOAK PASSED; CAMERA-SWITCH/DEVICE-REMOVAL CHECKS REMAIN (2026-07-29).**
This is the "separate mini-plan" that plan 20 §I / P6 kept pointing at. Owner
decision of 2026-07-24 stands: DXVA on by default, automatic per-camera
fallback, compatibility check in the camera UI. The production ladder is now
direct GPU transfer -> accelerated copy -> compatibility capture. The direct
path and isolated stress gate are implemented. The owner completed a long
physical-camera moving-picture run without visible tearing or stale frames;
camera-switch and device-removal churn remain owner work.

## Why this kept being deferred — and why each reason is now spent

Name the fear precisely, because "hard" is not it:

1. **The failure is invisible to an agent.** Every other plan had a gate an
   agent could watch (compile, CTest, replay metrics). DXVA's failure modes
   are driver hangs and black-but-streaming frames on *specific* cameras —
   observable only with a physical camera attached. An agent cannot plug in
   a webcam, so it could ship the risk but never see the result. Rational
   response: defer. **Spent by:** the staged sandbox harness below gives the
   agent a probe it *can* build and the owner a 10-minute run that produces
   a machine-readable verdict; plus the ladder makes every failure
   self-recovering, so an unseen failure degrades instead of bricking.
2. **The blast radius was the app's core function.** Until yesterday,
   touching `OpenSource`/`ConfigureReader` risked the only capture path
   while it was still carrying uncorrected correctness debt, and a blind
   user whose app opens to a silent black frame has no app. **Spent by:**
   plan 20 P0-P4 landed and gated (plan 24); regressions in the shared code
   are now attributable and test-caught, and stage 2's ladder guarantees
   every path ends in a picture or an announced explanation.
3. **Three-API interop is this repo's most expensive defect class.** The
   F1-F4 fence bugs (plan 16) all came from GPU lifetime/sync seams;
   MF→D3D11→CUDA adds another. **Spent by:** the spike happens in an
   isolated sandbox process first (stage 3); production code changes only
   after the sandbox proves the exact texture/sync recipe on real cameras.
4. **The payoff is performance, and nothing measured it.** Plan 22's rule is
   measure first; implementing zero-copy without telemetry risks weeks for
   an unquantified win. **Spent by:** stage 0 lands the minimal counters
   first, so stage 4's win is a number, not a belief.

So: not fear — sequencing. The sequence has now arrived at this plan.

## What already exists (verified 2026-07-29 — build on it)

- **CUDA already understands raw camera formats.** NV12/YUY2 frames go
  straight to CUDA for conversion+rotation today — from *CPU* memory
  (`src/app/app_pipeline_runtime.cpp:366-440`, GPU fast path at `:1094`,
  device staging at `src/cuda/cuda_interop.cpp:1284`). Zero-copy's delta is
  therefore only "deliver the same NV12 bytes without the RAM bounce" — the
  kernels need nothing new.
- **Frame identity survives** (plan 20 P0): `MediaFrame` carries
  `captureTimestamp100ns`, `sequenceNumber`, negotiated rate, signed stride
  (`include/okuflow/capture/media_capture.hpp:19-31`). A GPU-resident frame
  must carry the same fields — the struct grows a texture handle variant, it
  does not get replaced.
- **Failure classification exists** (`CameraFailureKind`,
  `media_capture.hpp:51`) and the degenerate-frame idea (04 U7) matches the
  black-frame detector needed here.
- **The optional-sandbox slot exists and is empty**:
  `scripts/run_minimal_test.bat:26` already knows how to build and run
  `sandbox/<target>` when present. Stage 1/3 harnesses live there — the
  infrastructure cost is already paid.
- **Recording is decoupled** (plan 20 P3): recording consumes the bounded
  queue, not the preview path, so capture-path surgery cannot silently
  change what gets recorded.

## Ground rules (non-negotiable)

- The CPU capture path is never deleted and never degrades — it is the
  bottom rung of the ladder forever.
- Every acceleration decision is **per camera**, keyed by symbolic link
  (enumeration indexes reorder), persisted with reason + date.
- Every failure path ends in a working picture or a plain-language announced
  message (announcement, never TTS — standing rule).
- The per-camera **Camera acceleration** dropdown is the only user control:
  Automatic, Always use GPU acceleration, or Compatibility mode. The removed
  global compatibility checkbox is load-only migration data and must not
  reappear.
- Owner-in-the-loop gates are marked ⛔ below; the agent stops there and
  reports rather than claiming them.

## Stage 0 — measure the baseline (IMPLEMENTED 2026-07-29)

Minimal counters, not plan 22's full telemetry: per-frame capture→present
CPU milliseconds, bytes copied per frame through the CPU path, and capture
thread CPU %. Log a one-line summary every 5 s while a debug flag is on.
Purpose: stage 4's before/after is a number. (Coordinate with plan 22
Phase 1 so the counters are the same ones it will keep.)

Implementation: set `OKUFLOW_CAPTURE_DIAGNOSTICS=1` before starting a debug
build. `MediaCapture` reports camera FPS, bytes copied per frame,
ReadSample/copy wall time, and capture-thread CPU use; the app reports
capture-to-present average and maximum latency. Both summaries are throttled
to once every five seconds.

## Stage 1 — the probe harness (`sandbox/mf_dxva_minimal`, IMPLEMENTED 2026-07-29)

A standalone console executable, deliberately outside the app process:

1. Enumerate cameras (same MF activation code path).
2. For a chosen camera and mode: open with DXVA **enabled**, read N frames
   with a hard watchdog timeout (separate thread + `TerminateProcess`-safe
   design — a hung driver must kill the probe, not the caller).
3. Validate frames: non-constant luma over 1.5 s, plausible stride/size,
   timestamps advancing. This validator is written once and reused by the
   in-app ladder and the "Test this camera" button later.
4. Repeat with DXVA disabled; emit a machine-readable verdict
   (JSON to stdout): per-config ok/black/hang/error + timings.
5. Exit codes: 0 verdict produced, 77 no camera present (clean skip — CI
   safe), nonzero probe infrastructure failure.

The agent can build and unit-test everything except the physical result.
⛔ **Owner gate 1:** run the probe against the real set (Camo/DroidCam/
Iriun phone cameras, USB webcam, laptop internal, OBS virtual camera) and
paste the JSON back. This is minutes per camera and settles which failure
modes are real *before* the app changes.

Implementation: `sandbox/mf_dxva_minimal` runs every camera twice in isolated
child processes, once with a D3D11/DXGI device manager and once with DXVA
disabled. Each child must deliver 45 plausible frames with advancing
timestamps; the parent enforces a hard timeout and emits one JSON object per
result. Exit code 77 is the clean no-camera result. The regular minimal-test
script builds this target but deliberately does not open owner hardware.

Run owner gate 1 from a Visual Studio x64 developer prompt:

```bat
build\msvc-release\sandbox_mf_dxva_minimal\mf_dxva_minimal.exe
```

Paste the complete JSON output back into this plan before Stage 2 begins.

### Owner gate 1 result (2026-07-29)

The first run exposed and fixed two probe-only defects before any production
capture code was enabled:

- Basic and advanced source-reader video processing were both enabled even
  though Media Foundation defines them as mutually exclusive.
- The compatibility comparison forced BGRA, unlike OkuFlow's established
  `NV12 -> YUY2 -> ARGB32 -> RGB32` negotiation order. The probe now mirrors
  production and validates luma according to the negotiated subtype.

The corrected probe produced:

```json
{"status":"complete","cameras":[{"index":0,"name":"DroidCam Video","accelerated":{"status":"ok","detail":"Healthy frames","camera":"DroidCam Video","mode":"accelerated","dxgiSamples":true,"frames":45,"timestampsAdvanced":44,"averageReadMs":32.047,"averageLuma":1.193,"spatialRange":255.000},"compatibility":{"status":"ok","detail":"Healthy frames","camera":"DroidCam Video","mode":"compatibility","dxgiSamples":false,"frames":45,"timestampsAdvanced":44,"averageReadMs":47.005,"averageLuma":1.774,"spatialRange":235.000}},{"index":1,"name":"USB2.0 HD UVC WebCam","accelerated":{"status":"ok","detail":"Healthy frames","camera":"USB2.0 HD UVC WebCam","mode":"accelerated","dxgiSamples":true,"frames":45,"timestampsAdvanced":44,"averageReadMs":39.562,"averageLuma":85.889,"spatialRange":217.304},"compatibility":{"status":"ok","detail":"Healthy frames","camera":"USB2.0 HD UVC WebCam","mode":"compatibility","dxgiSamples":false,"frames":45,"timestampsAdvanced":44,"averageReadMs":71.379,"averageLuma":87.911,"spatialRange":219.000}},{"index":2,"name":"Cem's Pixel  (Windows Virtual Camera)","accelerated":{"status":"black","detail":"Frames were constant or nearly blank; point the camera at a detailed scene and retry","camera":"Cem's Pixel  (Windows Virtual Camera)","mode":"accelerated","dxgiSamples":true,"frames":45,"timestampsAdvanced":44,"averageReadMs":31.027,"averageLuma":0.000,"spatialRange":0.000},"compatibility":{"status":"black","detail":"Frames were constant or nearly blank; point the camera at a detailed scene and retry","camera":"Cem's Pixel  (Windows Virtual Camera)","mode":"compatibility","dxgiSamples":false,"frames":45,"timestampsAdvanced":44,"averageReadMs":32.776,"averageLuma":16.000,"spatialRange":0.000}}]}
```

The Pixel virtual camera was disconnected during the run, so equal `black`
verdicts in accelerated and compatibility modes are expected and confirm that
the validator detects an enumerated-but-inactive source. Both active cameras
passed both paths. Accelerated average reads were lower in this sample:
DroidCam 32.047 ms versus 47.005 ms, and USB webcam 39.562 ms versus
71.379 ms. **Owner gate 1 passes for the two connected cameras; Stage 2 may
begin with the Pixel retained as a deliberate fallback/black-frame test.**

## Stage 2 — DXVA Phase 1 in the app (IMPLEMENTED 2026-07-29; owner gate 2 remains)

1. Replace the constant at `media_capture.cpp:440` with a per-camera
   decision: `auto | forceOn | forceOff`, default `auto`.
2. `auto` ladder on open: try DXVA → run the stage-1 validator on the live
   stream → on creation/negotiation failure retry same camera without DXVA
   (silent); on no-frames-within-window or degenerate frames, reopen
   without DXVA and record the reason.
3. **Crash-safe marker**: persist `dxvaAttemptInProgress=<symlink>` before
   an accelerated open, clear after validation. Found set at startup →
   previous attempt hung/crashed the process → that camera goes
   `forceOff(auto)` with reason "previous attempt did not complete", said
   in the UI. This is the only defense against driver-level hangs and the
   same pattern browsers use for GPU blocklists.
4. Settings: `cameras: { "<symlink>": { acceleration, reason, decidedOn } }`
   in settings.json via `SettingsStore` (round-trip + migration tests).
5. UI in the Device section (plan 27's structure): per-camera state line
   ("GPU accelerated" / "Compatibility mode since <date>: <reason>"),
   override combo, and **Test this camera** — which runs the stage-1
   harness *as a subprocess* with progress and an announced verdict, so a
   wedged driver kills the probe, never the app. Standard `setA11y`
   treatment on all three.
6. Tests (msvc-cpu): ladder decision table, marker lifecycle, settings
   round-trip, validator on synthetic black/constant/live-like buffers.

Implementation: production Automatic mode now creates the accelerated
D3D11/DXGI source reader first and validates 30 frames for advancing
timestamps and non-degenerate image range. Reader creation/negotiation failure
or an invalid startup image reopens the same camera in compatibility mode.
An attempt marker is written before accelerated open and cleared only after
validation; a marker found on the next launch records an automatic per-camera
fallback. Mode, fallback reason, and UTC decision date round-trip under the
camera symbolic link. Advanced Device > More exposes the per-camera override,
plain-language status, global compatibility escape hatch, and an asynchronous
`Test this camera` action. The latter temporarily releases the live camera,
runs the watchdog-isolated Stage 1 executable, persists the verdict, and
restores capture. Release packaging requires that probe beside the app.

The production top rung is now **direct GPU transfer**. The first validation
frames still use a controlled readback. Steady-state DXGI samples retain their
D3D11 texture in `MediaFrame`; D3D11 VideoProcessor converts NV12/YUY2 into one
reusable NT-shareable BGRA texture. The allocation is opened on D3D12 and
imported once through CUDA external memory; each frame performs one
device-to-device copy into the existing pitched processing buffer. No
system-memory copy occurs on this preview rung.

⛔ **Owner gate 2:** the plan 20 §I matrix — first-open per camera, forced
fallback, kill-mid-probe relaunch (marker path), override toggles, and one
full lecture-length session on the phone camera. Only then does DXVA-on
become the shipped default.

## Stage 3 — zero-copy route decision (IMPLEMENTED 2026-07-29)

Candidate A was selected for production:

1. `MFCreateDXGIDeviceManager` + a D3D11 device (MF's zero-copy contract is
   D3D11; D3D12 does not attach here), `MF_SOURCE_READER_D3D_MANAGER` on
   the reader; confirm samples arrive as `IMFDXGIBuffer` textures and
   log format/pool behavior per camera.
2. **NV12→CUDA route:** D3D11 VideoProcessor blits NV12/YUY2 into an owned,
   NT-shareable BGRA texture. The legacy
   `cudaGraphicsD3D11RegisterResource` prototype was rejected after a
   reproducible driver AV during unregister. Production opens the allocation
   on D3D12, obtains its exact allocation size, and imports it using
   `cudaExternalMemoryHandleTypeD3D12Resource`. Direct multi-plane NV12 import
   was not chosen because its driver/format support is materially narrower.
3. Prove synchronization: no tearing/stale frames across 1000 frames
   (checksum a moving pattern from a virtual camera), and clean device
   removal (unplug mid-stream) without leaking registrations.
4. Measure against stage 0: capture→CUDA latency and CPU % with the copy
   path vs zero-copy, same camera, same mode.

**Owner gate 3 moving-picture result (passed 2026-07-29):** the owner completed
the long production run and reported flawless live output, with no visible
tearing or stale frames. The remaining hardware checks are switching among the
available cameras and unplug/device removal while the direct path is active.

## Stage 4 — production zero-copy behind the ladder (IMPLEMENTED 2026-07-29)

Only what the spike proved, moved into the app:

- New top rung: zero-copy → accelerated-with-copy → CPU, same per-camera
  persistence, same announcements.
- `MediaFrame` grows a GPU variant (texture reference + the identity fields
  it already has); the keep-latest slot semantics are preserved — "latest"
  becomes "latest texture", with the previous one released, never a queue.
- Recording: the **original** stream needs CPU pixels only while recording —
  a bounded GPU→CPU readback feeds the plan-20 recording queue during
  recording sessions; steady-state preview stays copyless. (Later option,
  not this plan: hardware-encoder sink writers can accept DXGI samples
  directly — noted for plan 22 Phase 3.)
- Fences/lifetime reviewed against the plan-16 F1-F4 postmortems before
  merge; the presenter drain contract
  (`src/d3d12/presenter.cpp:90`) is the pattern to match.
- Stage 0 counters must show the win in production, and the ladder's
  fallback must be demonstrated live by force-failing the top rung.

Implementation notes:

- `OKUFLOW_FORCE_CAPTURE_COPY_RUNG=1` force-fails the top rung without
  disabling the D3D11 reader, so the accelerated-copy fallback can be compared
  against direct GPU transfer on the same camera and mode.
- The per-camera settings record the last winning rung and reason.
- Original paired recording/photos explicitly read the retained source sample
  back only while requested. Processed output remains driven by the GPU scene.
- "Zero-copy" in this plan means zero **CPU** copy. One GPU device-to-device
  copy remains between the CUDA-mapped BGRA texture and the linear processing
  surface required by the established kernels.
- A D3D11 event query protects the reusable conversion texture. If its bounded
  startup wait is exceeded, `PrepareGpuFrameForCuda` returns `Retry`: that
  frame uses safe readback, the pending texture is not overwritten, and the
  top rung resumes as soon as the query completes. Only genuine unsupported
  or external-import failures persist a session downgrade.

## Stage 4 blocker — shutdown AV in `cudaGraphicsUnregisterResource`: analysis and fix directive (2026-07-29)

> **RESOLVED the same day — see [plan 30](30-external-memory-capture-bridge.md)**,
> which records the as-built external-memory bridge (directives 1 and 2
> below were implemented, with the D3D12-hop recipe hardened by desc
> validation and `GetResourceAllocationInfo` sizing) and tracks the
> remaining hardware validation (R1) and the fence upgrade (R3). The
> analysis below stands as the defect record.

**Independent code review of the crash (all checks performed against the
working tree):** the implementation's hygiene is genuinely correct —

- `ID3D10Multithread::SetMultithreadProtected(TRUE)` is set
  (`media_capture.cpp:645-647`);
- the registered texture is plain `USAGE_DEFAULT`,
  `BIND_RENDER_TARGET | SHADER_RESOURCE`, BGRA, `MiscFlags` 0
  (`media_capture.cpp:1200-1212`) — the *supported* combination for the
  legacy API;
- `D3D11InteropState` holds the texture by `ComPtr`
  (`cuda_interop.cpp:35-40`), so texture and device outlive the unregister;
- teardown order is right: capture thread joined → VP views released →
  `ClearState` → `Flush` + event-query drain
  (`PrepareAccelerationInteropRelease`, `media_capture.cpp:773+`) →
  stream + device sync → unregister (`ResetCaptureInterop`,
  `cuda_interop.cpp:536-567`) → only then reader/source/device release;
- per-frame map/unmap is balanced including error paths, with a stream
  drain before D3D11 reuses the texture (`cuda_interop.cpp:2545-2589`).

With every documented precondition satisfied and the AV reproducible at a
fixed offset inside `nvwgf2umx.dll`, the conclusion stands: **a defect in
the legacy CUDA↔D3D11 graphics-interop path of the installed driver**,
plausibly specific to allocations written by the video engine
(VideoProcessor output). The legacy API is two generations old and barely
exercised by modern engines; do not keep fighting it.

### Directive 1 — shippable mitigation today (small, do first)

The crash is *only* in the final unregister. Registrations are reclaimed by
the OS/driver at process termination, so **skipping
`cudaGraphicsUnregisterResource` on the process-exit path is a harmless,
standard workaround** for exactly this defect class:

- Thread an `atExit` flag into `ResetCaptureInterop` (the
  `~CudaInteropSurface` path passes true; `StopCameraCapture`/identity-change
  paths pass false). When true: release the ComPtr, null the handle, skip
  the unregister call, log one line.
- **First test whether mid-session unregister (camera switch) also
  crashes.** If it does not — likely, since the report is shutdown-specific —
  mid-session keeps the real unregister and only exit skips. If it does,
  mid-session moves the ComPtr + registration into a small graveyard list
  released never (bounded by camera switches per session, ~8 MB each,
  reclaimed at exit).

This makes the shipped app stable immediately while the real fix lands, and
it is honest: the plan records it as a driver-defect workaround, not a fix.

### Directive 2 — the replacement: endorse external memory, but route it through the proven D3D12 import

The proposed `cudaImportExternalMemory` replacement is the right direction —
with one correction. Direct
`cudaExternalMemoryHandleTypeD3D11Resource` import has a weak joint: the
handle description requires the **allocation size**, and D3D11 has no public
`GetResourceAllocationInfo` equivalent — guessing `width*height*4` is
exactly the kind of plausible-but-undefined behavior this plan exists to
avoid. Instead, bridge to the import path this codebase has already run in
production for months (`externalMemory_`/`mipArray_`/`level0Array_`,
D3D12→CUDA):

1. Create `d3d11CudaTexture_` with
   `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`
   (no keyed mutex), bind flags unchanged.
2. `IDXGIResource1::CreateSharedHandle` (NT handle, read/write access).
3. `ID3D12Device::OpenSharedHandle` on the presenter's **existing** D3D12
   device → an `ID3D12Resource` view of the same allocation. Same adapter is
   guaranteed (plan 20 P4 fail-closed LUID matching).
4. Feed that resource into the existing D3D12-resource→CUDA external-memory
   import — which already answers the size/description questions correctly —
   map once as a mipmapped array, reuse per frame, and destroy with
   `cudaDestroyExternalMemory` at teardown exactly as the presenter surface
   already does. `cudaGraphicsUnregisterResource` never runs again.
5. Caveat to verify on hardware: `CreateVideoProcessorOutputView` on a
   shared-NT texture. If a driver refuses the VPOV, keep the VP output on a
   plain texture and add one `CopyResource` into the shared texture — still
   zero CPU copies.

### Directive 3 — prove it, both directions

- Extend `mf_dxva_minimal` with a register→map→unmap→unregister ×100 loop on
  the legacy API (expected: reproduces the AV in isolation — that artifact
  is the NVIDIA bug report) and the same loop on the external-memory route
  (expected: clean), plus camera-switch and device-removal cycles.
- Synchronization stays as designed for now: D3D11 event query before the
  CUDA read, CUDA stream drain before the next VP blit. The later
  fully-async upgrade imports a **shared `ID3D11Fence`** as a CUDA external
  semaphore (shared D3D11 fences interoperate through the D3D12-fence
  semaphore handle type — same kernel object); keep it out of the first
  landing.
- Acceptance for closing this blocker: 100 consecutive
  open→stream→close cycles plus 20 app launch/quit cycles with the
  accelerated rung active, zero faults in Event Viewer/WER; force-failed
  import degrades to the accelerated-copy rung with one announcement.

### Directive 3 measured result (2026-07-29)

`mf_dxva_minimal` now exposes an explicit watchdog-isolated lifetime stress
mode. The normal camera comparison never runs legacy CUDA interop.

```bat
mf_dxva_minimal.exe --camera 2 --interop external --iterations 100
```

On `USB2.0 HD UVC WebCam` the production
D3D11 -> D3D12 -> CUDA external-memory path completed **100/100** import,
device-copy, mip-array-free, and external-memory-destroy iterations with an
average of **0.684 ms** per iteration. The packaged release probe repeated
the gate at **0.747 ms** per iteration. Both children returned `status: ok`.
`--interop legacy` exists only as an opt-in NVIDIA driver-reproduction mode
and remains outside all automatic build and UI probes.

The first 20-app-cycle soak before the delayed-query retry produced 20 clean
exits and 18 immediate external imports; two cold starts exceeded the D3D11
query wait and unnecessarily disabled the top rung for that session.

The post-fix release-bundle soak ran **100 consecutive app
launch -> external-memory stream -> normal window close cycles** against the
physical USB webcam:

- 100/100 imported the camera texture through D3D11 -> D3D12 -> CUDA external
  memory;
- 100/100 exited normally with code 0;
- 7/100 encountered the delayed D3D11 query;
- 6/7 emitted the later recovery marker before close; the seventh delay
  occurred during shutdown after a successful import, so no later camera frame
  existed to emit recovery;
- 0/100 disabled the top rung or persisted an accelerated-copy downgrade;
- Windows Application Error / WER reported zero `oku_flow.exe` faults during
  the run.

This closes the transient-startup, launch/quit, and long moving-picture
tearing/staleness parts of the hardware gate. The remaining owner checks are
camera switching and device removal.

## Failure modes → behavior (the contract, all stages)

| Mode | Detection | Action | User hears |
|---|---|---|---|
| Reader creation/negotiation fails with DXVA | HRESULT | Silent same-camera retry without DXVA | nothing (by design) |
| Streams but no sample | watchdog window | Reopen without DXVA, persist reason | "Camera switched to compatibility mode" |
| Streams black/constant | stage-1 validator | same | same |
| Driver hang/crash | crash marker survives restart | forceOff(auto) that camera | "GPU capture disabled for this camera after a previous failure" |
| Zero-copy map/register fails | CUDA/D3D HRESULT | Drop one rung, persist | announced once |
| All rungs fail | — | CPU path (always exists) | plain-language message |

## Acceptance

Stage 2: every camera in the owner matrix reaches a picture with zero
manual intervention; the marker path survives a kill-9 mid-probe; overrides
persist across restarts; no black frame is ever silent. Stage 4: measured
capture→present CPU cost drops on the owner's phone camera with identical
visual output and identical recording files (bit-timing, not bit-exact);
force-failed top rung degrades within one second with one announcement;
72-hour soak (plan 24 D6) shows no leak growth in registrations or
textures.

## Relationship to other plans

Plan 20 §I is superseded by this file (pointer left there). Stage 0/4 share
counters with plan 22 Phase 1/3. The validator implements 04 U7's
degenerate-frame idea — close U7 against this when stage 2 lands. Plan 16's
fence postmortems are required reading before stage 4.
