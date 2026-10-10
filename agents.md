# Agent Operation Guide

OkuFlow evolves as an **AI-assisted, GPU-accelerated magnifier** for people
with low vision. Agents help us deliver that mission by building features,
keeping the pipeline fast, and ensuring the docs stay accurate. Read this
guide end-to-end before you touch the repo.

This project expects any autonomous or semi-autonomous agent (LLM, scripting
bot, CI assistant) to work within the following guardrails. Keep this document
open while contributing and update it whenever the workflow evolves.

## Core Principles
1. **Read the docs first** – Always consult `README.md`, `docs/README.md`,
   `docs/hardcoded_paths.md`, and `docs/THIRD_PARTY_LICENSES.md` before making
   changes. They describe the architecture, build matrix, and licensing rules.
2. **Keep documentation current** – Any code or script change that affects
   usage, outputs, dependencies, or licensing must update the relevant doc.
   Never commit features or fixes without doc updates.
   - Changes to classes, functions, or public structs require updating
     `docs/code_reference.md` in the same PR/commit so the reference stays
     authoritative.
3. **Respect the dual license** – All contributions are accepted under GPL-3.0
   plus the commercial license. Do not add third-party code unless the license
   is compatible and you note it in `docs/THIRD_PARTY_LICENSES.md`.

## Naming
- The product is **OkuFlow**, formerly OpenZoom. Website:
  <https://okuflow.com>; repository: <https://github.com/Cem-Kaya/oku_flow>.
- Code identifiers use `okuflow` (namespace, include directory, file
  prefixes), `oku_flow` (executable, repository), and `OKUFLOW_` (build
  options, environment variables).
- The one rename record that intentionally keeps the old name is
  `improvement_ideas/17-project-rename-plan.md`, along with the
  `CHANGELOG.md` rename entry.

## Module Map
- `src/app/` – Qt entry point, application wiring, settings persistence.
- `src/cuda/` – CUDA interop surface and the GPU effect pipeline (stabilization,
  keystone, color conversion, blur/sharpen, display color grading).
- `src/d3d12/` – pipelined D3D12 presenter with async readback ring.
- `src/capture/` – Media Foundation camera enumeration/capture with retry and
  device-loss recovery.
- `src/ui/` – Simple/Advanced main window, floating chrome, AI settings dialog.
- `src/common/` – CPU frame prep, assistive runtime (vision reading/TTS/notes), Codex
  app-server client, fragmented-MP4 video recorder.
- Public headers mirror the source tree under `include/okuflow/`.

## Workflow Expectations
- Align every change with the mission: produce a responsive magnifier that
  helps visually impaired users read content with AI assistance (temporal
  smoothing, upcoming VLM overlays, adaptive sharpening).
- Maintain coding style and structure; mirror the source layout in
  `include/okuflow/…` and keep module READMEs updated as you populate them.
- Update `CHANGELOG.md` and licensing notices when shipping user-visible or
  legal-impacting changes.
- Ensure build scripts (`scripts/build_and_run.bat` and
  `scripts/build_release_bundle.bat`) remain functional on Windows 10/11 with
  the documented toolchain.
- From the WSL/Linux agent shell, launch Windows-side commands with PowerShell
  7 via `pwsh.exe -NoProfile -Command '...'`, for example
  `pwsh.exe -NoProfile -Command 'Get-Date'`. Use this PowerShell 7 bridge for
  Windows build tooling and batch scripts when a native Linux command is not
  enough; do not use the legacy `powershell.exe` bridge.
- CUDA from the WSL shell needs the WSL driver stub on the library path. A
  working `nvidia-smi` is not sufficient: without it `torch.cuda.is_available()`
  returns False, cupy fails to initialise, and CUDA code silently falls back to
  the CPU. Prefix the command:
  `LD_LIBRARY_PATH=/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-} python your_script.py`
  (same prefix for `jupyter lab`).
- Create Python virtualenvs on local ext4 (`/home/...`), never under the repo.
  The working copy lives on a 9p Google Drive mount: large installs are slow
  there and would also be synced to the cloud. Point the local interpreter at
  the project instead, e.g. `/home/<user>/.venvs/<name>/bin/python /mnt/.../x.py`.
- Run the appropriate build or test command locally before submitting
  automated changes; note the result in your summary.
- Use `scripts/agent_build.bat` for the tracked Windows release/CPU/CUDA test
  matrix. Do not use or recreate machine-specific test runners under the
  gitignored `build/` directory.
- Keep `translations/okuflow_tr.ts`, `translations/okuflow_de.ts`, and
  `src/ui/translation_catalog.cpp` in exact source-key parity. Run
  `scripts/check_translations.ps1`; use its `-UpdateManifest` switch after
  intentionally adding or removing catalog entries.

## Locked Release Bundle Fallback
- Never terminate or relaunch a running OkuFlow instance merely to replace
  `dist\OkuFlow\oku_flow.exe`.
- When Windows reports that the primary release executable is in use after a
  successful build, create a complete sibling bundle at `dist\OkuFlow2\`.
  Copy the deployed runtime files from `dist\OkuFlow\`, exclude its
  user-owned `output\` captures, and replace `OkuFlow2\oku_flow.exe` with the
  newly built executable.
- Verify that the SHA-256 of the new `OkuFlow2\oku_flow.exe` exactly matches
  the release-build executable. Report that `dist\OkuFlow\` remains the
  running/older bundle and give the new bundle path.
- Use exactly `OkuFlow2`; do not create ambiguous alternatives such as
  `OkuFlow-next`.

## Useful References
- Qt moc and object model: <https://doc.qt.io/qt-6/moc.html>
- NVIDIA CUDA interop (runtime API): <https://docs.nvidia.com/cuda/>
- Media Foundation capture overview: <https://learn.microsoft.com/windows/win32/medfound/>
- Direct3D 12 best practices: <https://learn.microsoft.com/windows/win32/direct3d12/>
- Accessible magnification guidance: <https://www.w3.org/WAI/standards-guidelines/>
- VLM-assisted magnification ideas: <https://arxiv.org/abs/2102.09576>

Agents that modify the workflow must append to this guide so future runs remain
aligned with the project goals.

## Lecture Test Camera Workflow
- Use `scripts/start_lecture_camera.bat` and `scripts/stop_lecture_camera.bat`
  for manual recorded-lecture capture testing. The harness owns a private
  portable OBS copy under `%LOCALAPPDATA%\OkuFlow\lecture-camera\`; never
  replace the user's ordinary OBS scenes or stop an unrelated OBS instance.
- Native OBS Virtual Camera is DirectShow-only. On the setup machine the
  existing modern DroidCam Video driver receives the same scene through a
  pinned private OBS output plugin, making it visible to Media Foundation.
  Validate actual lecture frames on that device, not its idle placeholder.
- Downloaded lectures and OBS copies are local test assets, excluded from git
  and release bundles. Keep sample provenance/license in
  `scripts/lecture_camera_sample.json` and `docs/lecture_camera.md`. This
  virtual feed does not validate camera optics, autofocus, or microphone audio.

## Qt Toolchain Baseline
- Windows presets and scripts now default to Qt 6.12.0 at
  `C:\Qt\6.12.0\msvc2022_64`. Use that SDK for translation validation,
  compilation, profiling, and deployment; do not install Qt 6.9.3 to satisfy
  old cache paths. Regenerate stale Qt CMake caches during an SDK migration.
- Keep Multimedia, TextToSpeech, SVG, Image Formats, and matching source
  license material installed. PDF is not an OkuFlow dependency; its exact
  SPDX document remains mandatory whenever PDF is deployed.
- `OKUFLOW_BUNDLE_BUILD_DIR` selects a local build directory for the tracked
  bundle script when the synced build tree has stale paths or file locks.
  This does not skip its compilation, CTest, staging, or publishing gates.

## Validation Script Behavior
- `scripts/agent_build.bat` compiles the shipping release configuration, then
  runs the CPU and CUDA CTest presets. Its PASS/FAIL summary is the normal
  pre-submission gate for code changes that touch shared behavior.
  Automatic Visual Studio discovery is limited to 17.x (2022), preventing
  newer installed headers from being mixed with the cached 2022 compiler.
- `scripts/build_release_bundle.bat` runs CTest by default and publishes only
  from a validated staging directory. `OKUFLOW_SKIP_BUNDLE_TESTS=1` is an
  explicit emergency escape hatch and must remain visibly marked untested.
- `scripts/run_minimal_test.bat` always requires the main application build to
  succeed. It runs `dx12_cuda_minimal` when
  `sandbox/dx12_cuda_minimal/CMakeLists.txt` exists and otherwise reports the
  optional harness as skipped without failing the validation run.

## Optional Startup Performance Validation
- Use tracked `scripts/profile_startup.ps1` for real-camera startup and UI
  latency measurements. It refuses an existing OkuFlow instance and launches
  only normally exiting profiling children with explicit isolated settings.
  Do not terminate a user's instance to obtain a measurement.
- `-CompareLegacy` compares camera-opening sequences in the same executable;
  it is not a benchmark of every change against an old release. Report actual
  arrival/presentation rates separately from negotiated camera FPS, and treat
  first-present timing as submission timing rather than sensor-to-photon latency.

## UI Regression Validation
- `main_window_interactions` exercises the production Qt window without
  opening a camera or submitting AI requests. Run it through the tracked CPU
  and CUDA CTest presets in an interactive Windows desktop session.
- Set `OKUFLOW_UI_TEST_SCREENSHOTS` to an evidence directory when reviewing
  layouts. Captures include the native corner-control windows; inspect them
  alongside the assertions. Use separate output directories for each
  `QT_SCALE_FACTOR` run and avoid concurrent GUI test runs.
- Preserve the four-tab ownership contract in `docs/ui_modes_design.md` and
  keep keyboard order, collapsed-section persistence, and English/Turkish/
  German regression coverage aligned when moving controls.
- `ai_settings_dialog_interactions` validates provider switching, credential-id
  retention, workspace validation, keyboard order, and narrow dialog layouts
  using fixture settings only. It never submits AI requests or changes stored
  secrets. Keep it serial with other GUI tests.
- Keep `src/ui/translation_catalog.cpp` as LF through its `.gitattributes`
  rule, matching the translation validator's generated-manifest contract.

## Native Promotional Captures
- Follow `docs/ui_capture.md` for opt-in captures during an isolated startup
  profiling session. Use separate fixture settings and normally exiting children;
  never replace user settings or terminate an existing instance for screenshots.
- Inspect the raw composed capture for complete native controls, genuine camera
  frames, and foreign overlays. Reset computer-use automation before capture to
  remove its pointer overlay; retake unsuitable images without repairing pixels.
- Keep public media provenance separate from real-camera quality and performance
  claims. An original generated scene through the private OBS feed validates the
  demonstrated UI only.
