# Agent Operation Guide

OpenZoom evolves as an **AI-assisted, GPU-accelerated magnifier** for people
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

## Module Map
- `src/app/` – Qt entry point, application wiring, settings persistence.
- `src/cuda/` – CUDA interop surface and the GPU effect pipeline (stabilization,
  keystone, color conversion, blur/sharpen, display color grading).
- `src/d3d12/` – pipelined D3D12 presenter with async readback ring.
- `src/capture/` – Media Foundation camera enumeration/capture with retry and
  device-loss recovery.
- `src/ui/` – Simple/Advanced main window, floating chrome, AI settings dialog.
- `src/common/` – CPU frame prep, assistive runtime (OCR/VLM/TTS/notes), Codex
  app-server client, fragmented-MP4 video recorder.
- Public headers mirror the source tree under `include/openzoom/`.

## Workflow Expectations
- Align every change with the mission: produce a responsive magnifier that
  helps visually impaired users read content with AI assistance (temporal
  smoothing, upcoming VLM overlays, adaptive sharpening).
- Maintain coding style and structure; mirror the source layout in
  `include/openzoom/…` and keep module READMEs updated as you populate them.
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

## Locked Release Bundle Fallback
- Never terminate or relaunch a running OpenZoom instance merely to replace
  `dist\OpenZoom\open_zoom.exe`.
- When Windows reports that the primary release executable is in use after a
  successful build, create a complete sibling bundle at `dist\OpenZoom2\`.
  Copy the deployed runtime files from `dist\OpenZoom\`, exclude its
  user-owned `output\` captures, and replace `OpenZoom2\open_zoom.exe` with the
  newly built executable.
- Verify that the SHA-256 of the new `OpenZoom2\open_zoom.exe` exactly matches
  the release-build executable. Report that `dist\OpenZoom\` remains the
  running/older bundle and give the new bundle path.
- Use exactly `OpenZoom2`; do not create ambiguous alternatives such as
  `OpenZoom-next`.

## Useful References
- Qt moc and object model: <https://doc.qt.io/qt-6/moc.html>
- NVIDIA CUDA interop (runtime API): <https://docs.nvidia.com/cuda/>
- Media Foundation capture overview: <https://learn.microsoft.com/windows/win32/medfound/>
- Direct3D 12 best practices: <https://learn.microsoft.com/windows/win32/direct3d12/>
- Accessible magnification guidance: <https://www.w3.org/WAI/standards-guidelines/>
- VLM-assisted magnification ideas: <https://arxiv.org/abs/2102.09576>

Agents that modify the workflow must append to this guide so future runs remain
aligned with the project goals.

## Validation Script Behavior
- `scripts/agent_build.bat` compiles the shipping release configuration, then
  runs the CPU and CUDA CTest presets. Its PASS/FAIL summary is the normal
  pre-submission gate for code changes that touch shared behavior.
- `scripts/build_release_bundle.bat` runs CTest by default and publishes only
  from a validated staging directory. `OPENZOOM_SKIP_BUNDLE_TESTS=1` is an
  explicit emergency escape hatch and must remain visibly marked untested.
- `scripts/run_minimal_test.bat` always requires the main application build to
  succeed. It runs `dx12_cuda_minimal` when
  `sandbox/dx12_cuda_minimal/CMakeLists.txt` exists and otherwise reports the
  optional harness as skipped without failing the validation run.
