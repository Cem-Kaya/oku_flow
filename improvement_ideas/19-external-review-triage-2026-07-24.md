# Plan 19 — External Review Triage (2026-07-24)

Status: TRIAGED — every finding below was re-checked against the working tree
on `agent/stabilization-v2-rtx-optical-flow` on 2026-07-24. This file is the
index and the verdict list; the work itself is split into plans 20-24.

## What this is

An external engineering review of an `oku_flow.zip` snapshot produced a
long findings list (architecture, recording, performance, security, build).
The review is **substantially correct** — most findings reproduce in the
current tree — but it was written against a snapshot that predates the recent
stabilization rounds, and a few items are either overstated or are artifacts
of how the archive was produced rather than repository defects.

Nothing here is accepted on the reviewer's authority. Each item below is
marked with what I verified myself today, with file:line evidence.

## Verdicts

### Confirmed in the current tree (re-verified today)

| # | Finding | Evidence I checked | Plan |
|---|---|---|---|
| 1 | Camera modes are display-only; no mode is ever selected | `ConfigureReader` sets only `MF_MT_MAJOR_TYPE` + `MF_MT_SUBTYPE`, never `MF_MT_FRAME_SIZE`/`MF_MT_FRAME_RATE` (src/capture/media_capture.cpp:455-461); `StartCapture` has no mode parameter (include/okuflow/capture/media_capture.hpp:62) | 20 |
| 2 | Recording discards camera timestamps, always writes 30 FPS | `MediaFrame` has no timestamp field (include/okuflow/capture/media_capture.hpp:19-26); `ReadSample` fills `timestamp` then drops it (src/capture/media_capture.cpp:620-628); `constexpr UINT fps = 30` (src/app/recording_manager.cpp:113) | 20 |
| 3 | Recording silently drops frames (single latest-frame slot + 2-slot readback ring) | latest-frame overwrite (src/app/app_pipeline_runtime.cpp:715-719) | 20 |
| 4 | Recording output size follows the window; resize can stop recording | viewport-sized readback + dimension-change stop (src/app/recording_manager.cpp:247-256) | 20 |
| 5 | `Finalize()` HRESULT discarded — "recording saved" can be a lie | src/common/media_writer.cpp:132-140, return value unchecked | 20 |
| 6 | Signed stride collapsed to absolute value | `outFormat.stride = static_cast<UINT>(std::abs(rawStride))` (src/capture/media_capture.cpp:522) | 20 |
| 7 | D3D12/CUDA can pick different GPUs | only `EnumAdapterByGpuPreference(0, ...)` is tried, the iterate-and-verify loop runs only if that call *fails* (src/d3d12/presenter.cpp:473-499); CUDA falls back to device 0 with a warning when no LUID matches (src/cuda/cuda_interop.cpp:~520) | 20 |
| 8 | Output files written under the install directory | `QCoreApplication::applicationDirPath()` for photos (src/app/app_pipeline_runtime.cpp:1211), recordings (src/app/recording_manager.cpp:292), notes (src/app/assistive_feature_manager.cpp:146) | **21** |
| 9 | API key stored in plaintext | `object.insert("vlmApiKey", assistive.vlmApiKey)` (src/app/settings_store.cpp:480) | 23 |
| 10 | Everything runs on the Qt UI thread | pipeline_orchestrator tick → CUDA, present, readback, two synchronous encoder writes, JPEG/PNG encode | 22 |
| 11 | Lecture notes rewrite the whole HTML file per append | read-all + insert-at-marker + full rewrite (src/common/assistive_runtime.cpp:1015-1050) | 22 |
| 12 | Tests off in release; test presets can report success with zero tests | `OKUFLOW_ENABLE_TESTS: OFF` in `msvc-debug`/`msvc-release`; `msvc-debug-tests`/`msvc-release-tests` point at those presets; no `noTestsAction` anywhere | **24** |
| 13 | Settings load/save failures are silent; future schema versions accepted | src/app/settings_store.cpp:627-750, save result ignored in app_settings.cpp | 23 |
| 14 | Assistant turn has no completion watchdog; unbounded protocol/transcript buffers | src/common/codex_app_server_client.cpp:325-338, 554-570 | 23 |
| 15 | Temp OCR/Codex frames survive a crash (`setAutoRemove(false)`) | src/common/assistive_runtime.cpp:672-688, 863-875 | 23 |

### My own finding — worse than the review's #12

**The CUDA stabilization regression tests cannot be built or run by any
preset in the repository.** `tests/CMakeLists.txt:47` guards
`stabilization_cuda_tests` and `nvidia_optical_flow_tests` with
`if (WIN32 AND OKUFLOW_ENABLE_CUDA)`. The only preset with
`OKUFLOW_ENABLE_TESTS=ON` is `msvc-cpu`, which sets
`OKUFLOW_ENABLE_CUDA=OFF`. `tests/README.md` documents these tests and then
gives only the `msvc-cpu` recipe to run them; `build/agent_build.bat` runs
`ctest --preset msvc-cpu-tests`. So the regression tests written specifically
to stop stabilization from regressing — including the one replaying the
owner's measured 1.165 Hz drift profile — have **never run in any automated
build**. Given that stabilization has now been retuned three separate times
(see CHANGELOG), this is the structural reason regressions keep reaching the
owner's hands. Fixing this is the highest-leverage item in plan 24.

### Partly true — narrower than stated

- **"Automatic stabilization runs multiple estimators per frame."** Verified,
  with a correction: in Automatic mode `cuda_interop.cpp:2400-2502` launches
  NVOF, *also* launches the feature-pairs kernel (which early-returns on the
  device-side gate `diagnostics.y >= 0.5`, so it costs a launch and a block
  scheduling round, not a full track), and *unconditionally* launches the
  projection profiles plus a projection-estimate kernel every frame. So there
  is redundant per-frame GPU work, but it is gated rather than three full
  estimators. Measured cost on the owner's RTX 4090: 1-3 ms total, occasionally
  ~10 ms. Worth profiling per engine before merge, not worth panic. → plan 22.
- **"Silent frame drops" as one defect.** The single latest-frame slot is the
  *correct* policy for a magnifier preview — a low-vision user wants the newest
  frame, not a queued backlog. The defect is that recording is fed from that
  same preview-shaped path. Framing matters for the fix: don't add a queue to
  preview, add a separate recording path. → plan 20.

### Disputed / downgraded

- **Memory-bandwidth crisis (review §3.2).** The arithmetic is right but it is
  computed for 4K60 dual-stream, which is not this product's operating point:
  the camera is a clamped phone, and the live app negotiates 1280x720 (my
  session log: "CUDA surface ready for 1280 x 720"). Treat §3.2 as a *scaling
  ceiling* argument for plan 22's Phase 3, not as a description of current
  behavior.
- **CPU fallback not scalable (§3.5).** True and deliberate. The CPU path is
  explicitly deprecated and the UI says "GPU Required — processing disabled".
  Not worth optimizing; worth deleting or freezing.
- **`MF_SOURCE_READER_DISABLE_DXVA` (§4.4).** ~~Almost certainly set for
  compatibility with phone-as-webcam sources; not a defect today.~~
  **Overruled by owner decision, 2026-07-24:** GPU capture should be *on* by
  default, with automatic detection of a bad camera and fallback to the CPU
  path, plus a compatibility check surfaced in the camera-selection UI. This is
  now an accepted work item, specified in
  [plan 20 section I](20-capture-recording-integrity.md). My original
  reliability concern survives only as the design constraint that fallback must
  be automatic, per-camera, crash-safe, and never leave a user staring at a
  black frame.
- **Runtime shader compilation (§4.6).** Real, startup-only, tiny. Backlog.
- **Archive hygiene (§7.6).** Not a repository defect — `.git`, `build/`,
  `dist/`, `output/` are all gitignored. This describes how the zip was made.
- **Per-frame descriptor work (§4.5).** Real but micro; fold into plan 22 only
  if profiling shows it.

### Not verified this pass

- **Reconnect shuts down the new activation (review Finding 8).** The
  `ActivationShutdownGuard` and the reuse of the activation object are real
  (src/capture/media_capture.cpp:70-89, 330-337), but I did not trace the
  reconnect ordering far enough to call it confirmed. Plan 20 treats it as a
  hypothesis with a concrete runtime test, not as a known bug.

## Priority order

1. **Plan 21 — user data locations.** Smallest, highest user-visible value,
   and the owner has already been bitten (release bundling deleted photos and
   notes stored under `dist/OkuFlow/output`; the current fix is a preservation
   hack in the bundle script that plan 21 makes unnecessary).
2. **Plan 24 — test and build gates.** Cheap, and until the CUDA tests can run
   nothing else can be verified without the owner's hands.
3. **Plan 20 — capture and recording integrity.** The largest correctness debt.
4. **Plan 23 — security, privacy, release integrity.** Must land before any
   public distribution (plaintext API key + unsigned binaries).
5. **Plan 22 — threading and performance.** Real, but it is a rearchitecture;
   do it after correctness and gates, and drive it with the telemetry that
   plan 22 Phase 1 adds rather than by intuition.

## Sub-plans

- [`20-capture-recording-integrity.md`](20-capture-recording-integrity.md)
- [`21-user-data-locations.md`](21-user-data-locations.md)
- [`22-threading-performance.md`](22-threading-performance.md)
- [`23-security-privacy-release.md`](23-security-privacy-release.md)
- [`24-test-build-gates.md`](24-test-build-gates.md)

## Preserved from the review: what is already good

Worth keeping visible so hardening does not regress it — atomic settings
writes via `QSaveFile`, numeric clamping on load, low-latency swap-chain
configuration, move-based frame ownership transfer out of the capture
callback, explicit CUDA/D3D12 teardown synchronization, plain-language camera
error classification, OCR watchdog, HTTP transfer timeouts, assistant
defaulting to restricted network/coding permissions, strong view-transform
tests, and the accessibility model (accessible names everywhere, wheel-safe
controls, user-triggered TTS only).
