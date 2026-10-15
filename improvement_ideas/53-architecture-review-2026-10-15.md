# 53 — Architecture Review (2026-10-15)

Status: **REVIEW COMPLETE — recommendations open, nothing implemented.**

## Top finding

The core design is sound — a camera → CPU prep → CUDA effects → D3D12
presentation pipeline is the right shape for a low-latency magnifier — but it
is implemented as **one monolithic executable with three god-objects**
(`OkuFlowApp`, `MainWindow`, and the pipeline runtime). That, not the GPU
code, is now the main risk to build times, testability, and safe feature
growth. No P0 (ship-blocking) defect was found by inspection; the highest
items are P1 maintainability/security issues below.

## Method and verification status

Eight parallel reviewers (one per module area: system map, app/UI,
capture/phone, GPU pipeline, common/AI, build/release, tests, code metrics)
plus a critic pass; the coordinator then spot-verified the highest-severity
claims directly against the source.

- `Confirmed` = verified against source by the coordinator (line numbers
  below were read in this session).
- `Reported` = subsystem-reviewer finding, plausible but not re-verified;
  re-check against the code before implementing (per this directory's
  convention).

No build, profiling run, or hardware test was performed. Every
millisecond-level cost claim below is therefore code-derived inference, not
measurement.

## 1. Architecture snapshot

```
 Webcam (Media Foundation) ───┐
 Phone camera (adb/USB, H.264) ─┤→ capture → common (frame prep, view transform)
 Virtual/OBS lecture camera ───┘        → cuda (stabilize, keystone, color,
                                            blur/sharpen, grading, Maxine SR)
                                          → d3d12 presenter (async readback ring)
                                          → Qt UI (Simple/Advanced, overlays)
 Side paths: recording (fMP4 writer) ← readback
             AI assistant (Codex app-server, TTS, notes HTML +
             browser transcription) ← snapshots/audio
 Shell: okuflow_launcher (/MT, runtime preflight) → OkuFlowApp.exe
```

Measured size (`Confirmed`, lines under `src/`): app 14,543 · ui 10,730 ·
cuda 8,046 · common 7,449 · capture 5,560 · d3d12 1,795 · launcher 545.

Largest files (`Confirmed`):

- [`src/ui/main_window.cpp`](../src/ui/main_window.cpp) — 222 KB, 4,443 lines
- [`src/cuda/cuda_interop.cpp`](../src/cuda/cuda_interop.cpp) — 189 KB
- [`src/cuda/cuda_kernels.cu`](../src/cuda/cuda_kernels.cu) — 159 KB
- [`src/app/app_pipeline_runtime.cpp`](../src/app/app_pipeline_runtime.cpp) — 121 KB
- [`src/common/assistive_runtime.cpp`](../src/common/assistive_runtime.cpp) — 82 KB
- [`src/d3d12/presenter.cpp`](../src/d3d12/presenter.cpp) — 77 KB
- [`src/capture/phone_camera.cpp`](../src/capture/phone_camera.cpp) — 77 KB
- [`src/capture/media_capture.cpp`](../src/capture/media_capture.cpp) — 74 KB

Hygiene (`Confirmed`): 32 TODO/FIXME/HACK across `src/` + `include/`,
59 files under `tests/`, 54 documents in this directory at review time.

## 2. Strengths to keep

- **Clear module map mirrored in headers** —
  `src/{app,capture,common,cuda,d3d12,ui,launcher}` ↔ `include/okuflow/…`,
  so ownership is discoverable even though the build does not yet enforce it.
- **Pipelined presenter with async readback ring** — the correct way to keep
  recording/AI snapshots off the present path.
- **Defensive platform engineering** — /MT launcher with VC-runtime
  preflight; owned adb server in a kill-on-close Windows job on a private
  port; legacy-adb recovery with pinned hashes; device-loss classification
  with `ConsumeDeviceLost` polling (`Confirmed`,
  [`media_capture.cpp:1702`](../src/capture/media_capture.cpp)).
- **Validation culture** — tracked CPU/CUDA CTest matrix, test-gate evidence
  with resumable inventories, remote Windows CI for GUI tests, translation
  parity checker across 24 catalogs.
- **Privacy-conscious features** — secrets in DPAPI/local storage,
  browser-side transcription with versioned sidecars instead of concurrent
  edits to app-owned notes HTML.

## 3. Prioritized improvements

### P1 — do next (structural / trust)

1. **Split the monolith into per-module libraries.** `Confirmed`:
   [`cmake/CMakeLists.txt:22`](../cmake/CMakeLists.txt) globs all of
   `src/**.cpp` into one `oku_flow` executable, so module boundaries are
   convention only — any file can include any header, incremental builds
   relink everything, and unit tests cannot link a single module.
   *Fix:* `okuflow_capture`, `okuflow_cuda`, `okuflow_d3d12`,
   `okuflow_common`, `okuflow_ui`, `okuflow_app` STATIC/OBJECT libraries with
   explicit `target_link_libraries`, same source list per directory.
   *Risk low (build-only). Payoff: enforced layering, faster
   iteration, real unit-test targets.*
2. **Decompose `OkuFlowApp`.** `Confirmed`:
   [`app.hpp:93`](../include/okuflow/app/app.hpp) is a 484-line header
   declaring a `QObject` with individual UI-control slots
   (`OnBlurSigmaChanged`, `OnZoomCenterXChanged`, …) and
   `friend class MainWindow/InteractionController/UIStateManager` —
   composition root, controller, and state store in one class.
   *Fix:* extract `CameraSession` and `AssistantSession` services behind
   small interfaces; widgets emit typed parameter changes.
   *Risk medium — after item 1, one service at a time.*
3. **Split `MainWindow`.** `Confirmed` size: 4,443 lines in one `.cpp`;
   `Reported`: ~172 widget pointers. Chrome, inspector/advanced panels, and
   the assistant panel should be separately owned sub-widgets, leaving
   `MainWindow` as layout + wiring. *Risk medium — the existing
   GUI interaction tests are the safety net; keep them green in remote CI.*
4. **Single source of truth for settings state.** `Reported`:
   `AdvancedConfig` is triple-mirrored in app scalars,
   [`ui_state_manager.cpp:141`](../src/app/ui_state_manager.cpp), and widget
   values, with per-tick preset matching.
   *Fix:* make the config struct canonical; bind widgets through a
   descriptor table so adding a setting is one row, not three edits.
5. **Pin the adb binary actually launched as the owned server.**
   `Confirmed` discovery: [`phone_camera.cpp:214`](../src/capture/phone_camera.cpp)
   `FindAdbExecutable()` accepts `OKUFLOW_ADB_PATH`, `ANDROID_HOME`, and
   `PATH` candidates by file existence only.
   *Fix:* the bundled-adb hash pinning must apply to the owned-launch path;
   a PATH/env adb is borrowed-only (probe, never spawn/manage). This is the
   one security-relevant item — adb can run device shell commands.
   *Risk low. `Unverified`: which candidate the owned launcher
   ultimately starts was not traced; do that first.*
6. **Phone metadata must never kill the video session.** `Reported`:
   `UsbSocketServer.kt:203,218` disconnects the session when per-frame
   METADATA overflows its queue. Metadata is advisory: drop/coalesce it;
   reserve disconnect for the video channel.
   *Large reliability win on slow phones/USB hubs.*

### P2 — robustness and performance hygiene

7. **Capture watchdog.** `Confirmed`: the capture loop blocks in synchronous
   `ReadSample` ([`media_capture.cpp:1689`](../src/capture/media_capture.cpp)).
   Device loss after an `HRESULT` is classified well, but a wedged driver
   that never returns has no timeout.
   *Fix:* frame-arrival watchdog feeding the existing
   `ClassifyCameraFailure`/reconnect path, plus a bounded drop-oldest
   handoff queue so a slow consumer never backs up into the reader.
8. **Extract readback/recording from the presenter.** `Reported`:
   [`presenter.hpp:16,71`](../include/okuflow/d3d12/presenter.hpp) — the
   presenter owns the readback/recording pool, coupling presenting to
   recording lifetimes.
   *Fix:* fence/scene interface on the presenter; move the pool to a
   `ReadbackService`.
9. **Debounce settings persistence.** `Reported`:
   [`settings_store.cpp:1090`](../src/app/settings_store.cpp) and
   [`settings_controller.cpp:24,314`](../src/app/settings_controller.cpp) —
   full re-parse/re-serialize on save, possible double write on some paths.
   *Fix:* cache serialized bytes, debounce ~250 ms during slider drags,
   write once. *Cost under real slider use is inferred, not
   profiled — measure with the existing startup profiler first.*
10. **Narrow the UI↔app contract.** Forward-declare and pass a small
    view-model/signals interface instead of `MainWindow` depending on
    `OkuFlowApp` and the presenter (`Reported`:
    [`main_window.cpp:8`](../src/ui/main_window.cpp)). Falls out of items
    1+2; listed separately because it lets UI tests run without the whole app.
11. **Fix documentation drift on localization.** `Confirmed`:
    [`docs/README.md:255`](../docs/README.md) describes RTL as a future test
    path ("before an RTL catalog exists") while
    [`cmake/CMakeLists.txt:186`](../cmake/CMakeLists.txt) ships 24 catalogs
    including `ar`, `he`, `fa`, `ur`.
    *Fix:* rewrite that section from `docs/localization.md` as the source of
    truth; keep stating that new catalogs are unreviewed machine-assisted
    drafts.
12. **Add one pipeline diagram + thread/fence ownership table** to the
    developer guide. `Reported`: the pipeline exists as dense prose
    ([`docs/README.md:46`](../docs/README.md)). For CUDA↔D3D12 interop,
    "which thread signals which fence, who owns each buffer" in one table
    prevents the most expensive bug class.

### P3 — polish

13. Generate the hand-maintained header list in
    [`cmake/CMakeLists.txt:39`](../cmake/CMakeLists.txt) from the per-module
    targets created by item 1.
14. Split `cuda_kernels.cu` / `cuda_interop.cpp` by effect family once
    item 1 makes it cheap.
15. Triage this directory's documents into shipped/superseded/open so it
    stays a plan, not an archive.

## 4. Quick wins

Item 6 (metadata drop), item 11 (docs fix), item 12 (diagram), item 9
(debounce, if profiling confirms), and the borrowed-only guard in item 5.

## 5. Unverified — open questions, not findings

- No profiling: GPU copy/stall cost at 1080p/4K and slider-drag sync cost
  are inferences from code structure.
- Screen-reader behavior (NVDA/JAWS) and keyboard-only task completion were
  not exercised; accessibility verdicts are code-inspection only. For a
  low-vision product this deserves a dedicated pass.
- CPU-fallback effect parity when CUDA is unavailable (which
  `ProcessingSettings` effects `cpuPipeline_` applies) was not inspected.
- Maxine SuperRes internal sync/copy behavior is opaque (external SDK);
  `media_writer.cpp` and `maxine_superres.cpp` were not fully read.
- The app/UI caller of `ConsumeDeviceLost` and the actual reconnect loop
  were not traced.
- `adb_owned_server.cpp`, `adb_legacy_recovery.cpp` (beyond headers), and
  Android `CameraStreamService.kt` were not fully read.
- Real-device disconnect and long-soak behavior was not tested (no
  hardware run).
- Internals of `annotation_overlay.cpp` (~1,778 lines) and
  `recording_manager.cpp` (~1,750 lines) were sized, not deep-read.
- Runtime call graph/thread ownership in `app_pipeline_runtime.cpp` and
  `cuda_interop.cpp` bodies was inferred from headers and byte size.

## 6. Suggested sequence

- **Phase A (safe, high leverage):** 5 → 6 → 11 → 12 → 9 (after one
  profiling run).
- **Phase B (build restructure):** 1 → 13 → 7 → 8.
- **Phase C (object decomposition, behind existing GUI/contract tests):**
  2 → 4 → 3 → 10 → 14.

Each phase is independently shippable through the existing
`scripts/agent_build.bat` matrix. When an item is implemented, mark it
`DONE <date>` with a commit reference here, per this directory's rules.
