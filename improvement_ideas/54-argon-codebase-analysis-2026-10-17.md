# 54 — Argon Codebase Analysis & Improvement Recommendations (2026-10-17)

**Author / Reviewer:** Argon (`gemini-4-argon-high`)
**Baseline commit:** `7dd90ac` ("Add desktop-fitting native visual checks")
**Status:** **REVIEW COMPLETE — read-only source analysis, no code modified during analysis.**

---

## 1. Executive Summary & Core Strengths

OkuFlow is a mature, thoughtfully engineered Windows assistive magnifier (`C++20` / `Qt 6.12` / `CUDA 12.9` / `D3D12` / `Media Foundation`) paired with a Kotlin/Camera2 Android USB companion (`android/`) and an MCP/Agentic control surface (`src/mcp/`, `src/app/agent/`).

**Strengths worth preserving:**
- **Low-latency GPU pipeline & fault isolation:** The camera $\rightarrow$ CUDA effect surface ([`src/cuda/cuda_interop.cpp`](../src/cuda/cuda_interop.cpp)) $\rightarrow$ D3D12 presenter ([`include/okuflow/d3d12/presenter.hpp`](../include/okuflow/d3d12/presenter.hpp)) with a 4-slot async readback ring and shared fence timelines keeps recording and AI frame capture off the presentation critical path, while quarantining resources on GPU fence faults rather than crashing on teardown.
- **Defensive platform & process management:** Static `/MT` preflight launcher ([`src/launcher/main.cpp`](../src/launcher/main.cpp)), DPAPI secret storage ([`src/app/protected_secret_store.cpp`](../src/app/protected_secret_store.cpp)), and strict process-identity verification when reclaiming legacy `adb.exe` listeners ([`include/okuflow/capture/adb_legacy_recovery.hpp`](../include/okuflow/capture/adb_legacy_recovery.hpp)).
- **Rigorous test & translation infrastructure:** 50+ test targets across CPU/CUDA presets, preprocessing-equivalence guards ([`cmake/SharedSourceEquivalence.cmake`](../cmake/SharedSourceEquivalence.cmake)), and automated 25-language catalog parity checks ([`scripts/check_translations.ps1`](../scripts/check_translations.ps1)).

---

## 2. Verified Bugs & High-Priority Fixes (P1)

### 2.1 `bumpHoldEnabled_` is missing from `AdvancedConfig` and UI state sync (`Confirmed` — New Finding)
- **Evidence:**
  - [`OkuFlowApp::OnBumpHoldToggled`](../src/app/app_controls.cpp) (lines 786–791) updates `bumpHoldEnabled_`, which feeds the CUDA pipeline at [`src/app/app_pipeline_runtime.cpp:958`](../src/app/app_pipeline_runtime.cpp) (`settings.enableBumpHold = stabilizationEnabled_ && bumpHoldEnabled_`).
  - However, `OnBumpHoldToggled` never calls [`SyncCurrentConfigToPersistence()`](../src/app/app_settings.cpp).
  - [`settings::AdvancedConfig`](../include/okuflow/app/settings_store.hpp) has no `bumpHoldEnabled` field.
  - [`UIStateManager::ReadConfigFromUI`](../src/app/ui_state_manager.cpp) (lines 141–195) and [`UIStateManager::ApplyConfigToUI`](../src/app/ui_state_manager.cpp) (lines 197–246) never read or restore `bumpHoldCheckbox()`.
- **Impact:** Toggling Bump Hold is lost across app restarts, ignored when switching or saving Quick Modes, and excluded from section-changed badges.
- **Concrete Fix:** Add `bool bumpHoldEnabled{false};` to `settings::AdvancedConfig`, serialize/deserialize it in [`src/app/settings_store.cpp`](../src/app/settings_store.cpp) and include it in `AreConfigsEquivalent`, bind `bumpHoldCheckbox_` in `UIStateManager`, and call `SyncCurrentConfigToPersistence()` in `OkuFlowApp::OnBumpHoldToggled`.

### 2.2 Owned `adb` server launch accepts unpinned binaries from `PATH` / environment (`Confirmed`)
- **Evidence:**
  - [`EnsureAdbServer`](../src/capture/phone_camera.cpp) (lines 472–476) resolves the executable via [`FindAdbExecutable()`](../src/capture/phone_camera.cpp) (lines 214–231) and passes it directly to [`CreateOwnedAdbServer`](../src/capture/phone_camera.cpp) (lines 254–288), which spawns `<executable> -L tcp:<port> server nodaemon`.
  - `FindAdbExecutable()` accepts `OKUFLOW_ADB_PATH`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `PATH` (`QStandardPaths::findExecutable("adb.exe")`), and `%LOCALAPPDATA%/Android/Sdk/platform-tools/adb.exe` based solely on `file.isFile()`.
- **Impact:** While [`TryReclaimLegacyBundledAdbServer`](../include/okuflow/capture/adb_legacy_recovery.hpp) strictly verifies `kReviewedAdbBuildManifestSha256` and the `adb.exe` SHA-256 before touching an existing process, the **owned server launch path** does not verify that the binary it spawns is the reviewed bundled `platform-tools/adb.exe`.
- **Concrete Fix:** Restrict owned-server spawning (`CreateOwnedAdbServer`) to the bundled `platform-tools/adb.exe` verified against `adb-build.json` (`kReviewedAdbBuildManifestSha256`), and treat external/SDK/PATH `adb` installations as borrow-only unless an explicit test hook is active.

### 2.3 Advisory per-frame metadata overflow disconnects the Android USB video stream (`Confirmed`)
- **Evidence:**
  - In [`android/app/src/main/java/com/okuflow/camera/link/UsbSocketServer.kt`](../android/app/src/main/java/com/okuflow/camera/link/UsbSocketServer.kt) (lines 189–203), `metadata(json)` routes through `control(WireProtocol.METADATA, json)` $\rightarrow$ `send(channel, type, json)`.
  - When the control channel queue (128 packets / 512 KB) fills up during a momentary USB hiccup, `send()` logs `"Control queue limit reached"` and calls `disconnect(channel.socket)`, terminating the entire active camera session (`session = null`, `onDisconnect()`).
- **Concrete Fix:** Per-frame metadata (AF/AE/telemetry) is advisory. Drop or coalesce `WireProtocol.METADATA` packets when the control queue is full or above a high-water mark instead of disconnecting the session; reserve control-channel disconnect for `STATE` / `ERROR` / `ACK` failures or watchdog write timeouts.

### 2.4 Synchronous `ReadSample` in `MediaCapture::CaptureLoop` lacks a wedged-driver watchdog (`Confirmed`)
- **Evidence:**
  - [`MediaCapture::CaptureLoop`](../src/capture/media_capture.cpp) (lines 1689–1705) blocks synchronously inside `reader->ReadSample(...)`.
  - If a USB webcam or virtual camera driver wedges inside `ReadSample` without returning an error `HRESULT`, `deviceLost_` is never set and `StopCapture()`'s `Flush()` can hang until detach timeout.
- **Concrete Fix:** Track `lastSampleReturnSteadyMs_` in `MediaCapture` (updated on every `ReadSample` return, including `MF_SOURCE_READERF_STREAMTICK`) and check it on the app's frame/orchestrator tick so a silent driver stall triggers `ConsumeDeviceLost()` and automatic camera recovery.

---

## 3. Architectural & Performance Improvements (P2)

### 3.1 Eliminate 4-way state duplication for image parameters (`Confirmed`)
- **Problem:** Every image parameter is currently maintained in four places:
  1. Individual scalar members on [`OkuFlowApp`](../include/okuflow/app/app.hpp) (lines 455–505: `blurSigma_`, `contrast_`, `sauvolaStrength_`, etc.)
  2. `settingsController_->MutableSettings().currentConfig` ([`settings::AdvancedConfig`](../include/okuflow/app/settings_store.hpp))
  3. Individual widget states in [`UIStateManager`](../src/app/ui_state_manager.cpp) (lines 141–246)
  4. Agent control descriptors in [`src/app/agent/parameter_registry.cpp`](../src/app/agent/parameter_registry.cpp) and [`src/app/agent/app_control_image_parameters.cpp`](../src/app/agent/app_control_image_parameters.cpp)
- **Concrete Fix:** Make `settings::AdvancedConfig` the single live struct on `OkuFlowApp` (replacing the ~45 loose scalar members in `app.hpp`). Bind widgets and `UIStateManager` directly to `AdvancedConfig` member pointers or a shared descriptor table.

### 3.2 Redundant UI work on slider drags & redundant disk reads/serialization on `SaveDetailed` (`Confirmed` — Refines Plan 53 Item 9)
- **Correction to [`53-architecture-review-2026-10-15.md`](53-architecture-review-2026-10-15.md) item 9:** Image slider drags call [`SyncCurrentConfigToPersistence()`](../src/app/app_settings.cpp) (lines 103–118), which does **not** write to disk (`SavePersistentSettings()` is only called on discrete actions and app exit).
- **Actual slider-drag bottleneck (`Confirmed`):** When `OnBlurToggled`, `OnSpatialSharpenToggled`, `OnTextClarityControlsChanged`, etc. run, they call [`UpdateControlEnabledStates()`](../src/app/app_controls.cpp) (lines 1099–1146, which calls `UpdateSectionChangedCounts()` at line 1145) and then immediately call [`SyncCurrentConfigToPersistence()`](../src/app/app_settings.cpp) (which calls `UpdateSectionChangedCounts()` a **second time** at line 117, along with `RefreshPresetSelection()` across all built-in and custom presets on every slider tick).
- **Actual disk-save bottleneck (`Confirmed`):**
  1. When [`settings::SaveDetailed`](../src/app/settings_store.cpp) (lines 1101–1140) runs, it re-reads and re-parses the existing settings file on disk (`LoadDetailed(path)`) on every save just to check its `LoadStatus` before rotating `.backup`.
  2. When `Save` fails because of size, [`SettingsController::Save`](../src/app/settings_controller.cpp) (lines 300–315) calls `ExceedsSettingsLimit(persisted)`, which runs `settings::Serialize(persisted)` a second time because `storage_.saveSettings` returns `bool` instead of `SaveStatus`.
- **Concrete Fix:**
  1. Remove the duplicate `UpdateSectionChangedCounts()` call when `UpdateControlEnabledStates()` is immediately followed by `SyncCurrentConfigToPersistence()`.
  2. Change `StorageOperations::saveSettings` to return `settings::SaveStatus` so `SettingsController::Save` never re-serializes on failure.

### 3.3 Decompose `OkuFlowApp` and `MainWindow` (`Confirmed`)
- **Problem:** While `OkuFlowApp`'s implementation is split across 14 `src/app/app_*.cpp` files, [`include/okuflow/app/app.hpp`](../include/okuflow/app/app.hpp) remains a single 520-line class with ~120 private members and 5 `friend` classes (`MainWindow`, `InteractionController`, `UIStateManager`, `AppControlTarget`, `NativeControlFixture`). Similarly, [`src/ui/main_window.cpp`](../src/ui/main_window.cpp) is 228 KB (~4,440 lines) with >100 raw widget getters.
- **Concrete Fix:**
  - Extract cohesive sub-controllers from `OkuFlowApp`:
    1. `AgentSessionCoordinator` (moving the ~25 `agent*` members and methods at [`include/okuflow/app/app.hpp:143-173`](../include/okuflow/app/app.hpp) out of `OkuFlowApp`).
    2. `CameraSessionManager` (encapsulating `cameras_`, `phoneDiscovery_`, `cameraIngress_`, `pendingCameraStart_`, and `captureHandoffPolicy_`).
  - Split `MainWindow` by its three Advanced tabs (`ImageInspectorPanel`, `AssistantPanel`, `SettingsPanel`) and Simple overlay chrome (`SimpleChromeOverlay`), leaving `MainWindow` responsible only for top-level layout, mode switching, and native event filtering.

### 3.4 Convert CMake `GLOB_RECURSE` + `shared_sources.cmake` into proper module targets (`Confirmed`)
- **Problem:** [`cmake/CMakeLists.txt:46`](../cmake/CMakeLists.txt) globs all `src/*.cpp` into `oku_flow`, and [`tests/shared_sources.cmake`](../tests/shared_sources.cmake) works around test build times by manually carving out 30+ `okuflow_share_equivalent_sources(...)` object groups verified via `MSVC /EP`.
- **Concrete Fix:** Define explicit CMake library targets mirroring the directory architecture (`okuflow_common`, `okuflow_capture`, `okuflow_d3d12`, `okuflow_cuda`, `okuflow_agent`, `okuflow_ui`). Link `oku_flow` and the test executables against those module targets directly. This enforces module include boundaries at compile time and eliminates manual source-group maintenance in `tests/shared_sources.cmake`.

---

## 4. Documentation & Workspace Hygiene (P3)

1. **Update stale localization docs in [`docs/README.md`](../docs/README.md) (`Confirmed`):**
   - `docs/README.md` (lines 234–264) still states that *"The stable persisted values are `en`, `tr`, and `de`"* and *"before an RTL catalog exists... none is currently shown in the language picker"*, whereas [`cmake/CMakeLists.txt:185-210`](../cmake/CMakeLists.txt) and [`docs/localization.md`](../docs/localization.md) ship 25 languages including RTL catalogs (`ar`, `fa`, `he`, `ur`).
2. **Reconcile or remove root [`TODO.md`](../TODO.md) (`Confirmed`):**
   - The 5-line `TODO.md` in the repo root is left over from early development and contradicts the active status in `improvement_ideas/`. Either point it to [`improvement_ideas/README.md`](README.md) or remove it.
3. **Clean stray untracked build artifacts in repo root (`Confirmed`):**
   - `maxine_probe.obj` (313 KB), `maxine_superres.obj` (359 KB), and `aqtinstall.log` (4 KB) are sitting directly in the repository root directory.
