# Maxine SuperRes Implementation Plan (2026-07-22)

Premium tier for the "ML Text Super Resolution (experimental)" toggle, per the
SLA verification in 08-ml-text-sr-options.md. Legal architecture is fixed:
**runtime-loaded optional plugin; NVIDIA bits never enter the GPL build.**

## Assets in place
- `third_party/maxine/Maxine-VFX-SDK/` — MIT-licensed SDK headers + samples
  (v0.7.6, git history stripped). MIT is GPL-compatible → COMMIT this. Key
  headers: `nvvfx/include/nvVideoEffects.h`, `nvCVImage.h`.
- `third_party/maxine/redist/` — **gitignored** drop point for the EULA-licensed
  runtime installers (download per-GPU from nvidia.com/broadcast-sdk-resources;
  Turing/Ampere/Ada/Blackwell variants). Only the commercial bundler reads it.

## Architecture (the GPL-clean pattern)
1. New module `src/common/maxine_superres.cpp/hpp`: wraps the SDK **via
   LoadLibrary at runtime only** — no import library, no link-time dependency.
   The GPL binary contains only MIT headers + `LoadLibraryW` probing:
   - Discovery order: `OPENZOOM_MAXINE_PATH` env/setting → registry/default
     `%ProgramFiles%\NVIDIA Corporation\NVIDIA Video Effects\` → absent.
   - Resolve `NvVFX_*` / `NvCVImage_*` entry points with GetProcAddress; any
     miss → cleanly unavailable.
2. Effect setup: `NvVFX_CreateEffect(NVVFX_FX_SUPER_RES)`, set model dir,
   `NvVFX_SetCudaStream(stream_)` — **shares our existing CUDA stream**, so it
   slots into `ProcessFrame` without new synchronization.
3. Pipeline placement: replaces/augments the NIS/FSR spatial stage when
   (a) toggle on, (b) runtime available, (c) zoom ≥ ~1.5×. Input = current
   ping-pong buffer ROI; NvCVImage-wrap our device buffers (BGRA u8 → SDK's
   expected format via NvCVImage_Transfer on-device), run, transfer back,
   continue pipeline. Strength param maps to SuperRes mode (0 = conservative,
   1 = aggressive) + blend.
4. Fallback chain: Maxine unavailable → (future RLFN ONNX tier) → existing
   NIS/FSR. UI status line states which tier is active; when unavailable show
   "NVIDIA Video Effects runtime not installed" + installer link in AI/About.
5. State/teardown: effect handle owned by CudaInteropSurface-adjacent RAII
   wrapper; destroy with the existing SynchronizeStream discipline; re-create
   on resolution change (SuperRes fixes in/out dims at Load).

## Settings & UI
- `AdvancedConfig`: `mlSuperResEnabled` exists (verify name from the current
  experimental toggle); add `mlSuperResStrength` float 0..1 if absent.
- Keep `OPENZOOM_ENABLE_TEXT_SR` semantics: the *plugin loader* compiles in
  always (it's MIT-clean); the flag can retire or gate UI visibility.

## Compliance tasks (from the SLA — mandatory)
1. NVIDIA Maxine attribution in About + README per Supplement §3.1.
2. `scripts/build_release_bundle.bat`: commercial bundle copies
   `third_party/maxine/redist/` installers (or runtime files) — NEVER the
   GPL/public zip. Public README links NVIDIA's installer instead.
3. docs/THIRD_PARTY_LICENSES.md: add Maxine SDK (MIT headers) + runtime (2021
   NVIDIA SDK EULA) entries; note the GPU requirement.
4. Pin to 0.7.6-era runtime; do not auto-update to NGC "SDK Core" (paid AI
   Enterprise for production).

## Steps for the implementing agent
1. Wrapper module + dynamic loading + availability probe (test: reports
   unavailable cleanly on machines without the runtime — MUST not crash).
2. CudaInterop integration behind the toggle; latency instrumentation
   (cudaEvent) logging ms/frame — acceptance: ≤8 ms @640×360→2×, else auto-off
   with status message.
3. UI status + installer guidance; settings persistence.
4. Bundler + licenses + attribution.
5. Manual hardware test matrix: runtime present/absent, GPU arch variants,
   toggle mid-stream, resolution changes, camera switch.

Benchmark vs NIS/FSR on real slide footage before declaring victory; if SuperRes
does not visibly beat NIS on text at 2×, the RLFN tier becomes the priority.

---

## REVISED DELIVERY MODEL (2026-07-22, per owner): dynamic dependency setup

Do NOT bundle per-GPU runtimes or Tesseract. Instead, an in-app **Setup
Assistant** (first run + Settings entry) fetches dependencies dynamically:

1. **GPU detection → correct Maxine runtime.** We already know the CUDA
   compute capability from device props: 75 → Turing installer, 86 → Ampere,
   89 → Ada (owner's RTX 4090 mobile = Ada), 10x+ → Blackwell. Map to the
   matching Video Effects runtime installer URL from
   nvidia.com/broadcast-sdk-resources; download with QNetworkAccessManager
   (progress UI, resumable), verify SHA-256 against pinned hashes, run the
   installer (silent flags if supported, else interactive). If the pinned URL
   404s (NVIDIA reshuffles), fall back to opening the download page in the
   browser with instructions.
2. **Tesseract the same way**: download tesseract.exe build + eng.traineddata
   (+ osd) from the pinned upstream release, hash-verify, install to
   %LOCALAPPDATA%\OpenZoom\tools\tesseract, point the existing tesseractPath
   setting at it. Removes 175 MB from the bundle.
3. **Graceful offline path**: everything optional; app fully works without
   both (no OCR, NIS/FSR upscaling). Setup Assistant shows per-dependency
   status (Installed / Not installed / Download), is screen-reader friendly,
   and never blocks startup.
4. **Legal win**: OpenZoom distributions (GPL and commercial alike) contain
   zero NVIDIA-EULA bits and zero Tesseract binaries — users obtain each
   directly from its vendor under that vendor's license. The
   `third_party/maxine/redist/` drop dir becomes developer-convenience only.
5. Bundle math: dist shrinks to Qt + app (~120 MB → ~60-80 MB after the
   earlier trim list).

Implementation: `src/app/setup_assistant.cpp/hpp` (QDialog + QNetworkAccessManager
downloader with hash pinning), wired from first-run check and AI Settings.
Pinned URLs+hashes live in one table for easy maintenance.

---

# INSTRUCTIONS FOR THE IMPLEMENTING AGENT (authoritative, 2026-07-23)

Implement everything below in one pass. Read this whole file first — the SLA
conditions above are binding. Repo: Windows-only Qt6 + D3D12 + CUDA. Build
recipe at the end; iterate until green. Mixed CRLF/LF — ignore.

## Part 1 — Maxine SuperRes plugin (GPL-clean)
1. New `src/common/maxine_superres.cpp` + `include/openzoom/common/maxine_superres.hpp`:
   class `MaxineSuperRes`. Include headers ONLY from
   `third_party/maxine/Maxine-VFX-SDK/nvvfx/include/` (MIT — committable).
   NEVER link an import lib: resolve `NVVideoEffects.dll` + `NvCVImage.dll`
   at runtime with `LoadLibraryW`/`GetProcAddress`. Discovery order:
   settings override → `%ProgramFiles%\NVIDIA Corporation\NVIDIA Video Effects\`
   → SDK registry key if present → unavailable (clean, no crash, cached probe).
2. API: `bool IsAvailable()`, `bool Ensure(int srcW, int srcH, int scale,
   cudaStream_t stream)` (creates NVVFX_FX_SUPER_RES, sets model dir + our
   CUDA stream), `bool Run(const uchar4* srcDev, size_t srcPitch, uchar4*
   dstDev, size_t dstPitch)` (NvCVImage-wrap device buffers, on-device
   transfers only), `void Teardown()` (SynchronizeStream discipline).
3. Integration in CudaInteropSurface ProcessFrame: when
   `settings.enableMlSuperRes && zoom >= 1.5f` and plugin available, replace
   the NIS/FSR stage for the zoom ROI; else existing behavior. Add
   `enableMlSuperRes` + `mlSuperResStrength` (0..1 → mode 0/1 + blend) to
   ProcessingSettings; wire the existing experimental toggle to it; add
   cudaEvent latency measurement — if avg > 8 ms over 60 frames, auto-disable
   with a status message ("SuperRes too slow on this GPU — using NIS").
4. AdvancedConfig: `mlSuperResEnabled`, `mlSuperResStrength` — JSON round-trip,
   equivalence, apply/capture, per-preset like every other field.

## Part 2 — Setup Assistant (first-run installer + uninstaller)
1. New `src/app/setup_assistant.cpp` + `include/openzoom/app/setup_assistant.hpp`:
   `class SetupAssistantDialog : public QDialog`. Two dependency rows, each
   with status (Installed / Not installed), Download/Install button, progress
   bar, and a **Remove button**:
   - **Tesseract OCR**: download pinned release (UB Mannheim tesseract
     installer or portable zip; pin exact URL + SHA-256 in one constants
     table), verify hash, extract/install to
     `%LOCALAPPDATA%\OpenZoom\tools\tesseract\`, set the existing
     `tesseractPath` assistive setting to the installed exe. **Remove** =
     delete that directory + clear the setting (confirm dialog first).
   - **NVIDIA Video Effects runtime (SuperRes)**: detect GPU arch from the
     CUDA compute capability already queried at startup (7.5→Turing,
     8.6→Ampere, 8.9→Ada e.g. RTX 4090 mobile, ≥10.0→Blackwell); download the
     matching v0.7.6 installer from the pinned
     nvidia.com/broadcast-sdk-resources URL table, SHA-256 verify, run the
     NVIDIA installer interactively. **Remove** = invoke the runtime's
     registered uninstaller from
     `HKLM\...\Uninstall` (search DisplayName containing "NVIDIA Video
     Effects"); fallback: open `ms-settings:appsfeatures` with instructions.
     No CUDA/NVIDIA GPU detected → row hidden entirely.
   - Any pinned URL 404s → offer "Open download page in browser" fallback.
     Downloads via QNetworkAccessManager, resumable not required, transfer
     timeout 60 s, everything cancellable.
2. **First-run trigger**: after the main window shows, if (tesseract missing
   OR (NVIDIA GPU present AND Maxine runtime missing)) AND
   `setupAssistantDeclined == false` → open the dialog non-modally once.
   "Don't ask me again" checkbox persists `setupAssistantDeclined` (new
   PersistentSettings field, JSON round-trip). Also add a "Setup / Downloads…"
   button in AI Settings (or Advanced) to reopen it any time — Remove buttons
   live there too, satisfying the "button in the tool to delete them" owner
   requirement.
3. Fully accessible: accessible names/descriptions on every control, focus
   order, Esc closes, progress announced via the status label.

## Part 3 — NVIDIA attribution (SLA §3.1 — mandatory)
At the **very bottom of the Advanced settings panel/inspector**, add a small
static row: "SuperRes powered by NVIDIA Maxine™" (plus "NVIDIA Video Effects
runtime installed/not installed" as tooltip/subtext). Visible whenever the
Maxine feature exists in the build (not only when active). Also add the same
line to docs/THIRD_PARTY_LICENSES.md (Maxine SDK headers = MIT;
runtime = NVIDIA SDK EULA 2021, obtained by the user from NVIDIA) and a short
README note. Do not imply NVIDIA endorsement.

## Part 4 — Bundler & docs
- `scripts/build_release_bundle.bat`: ensure NO NVIDIA runtime bits and NO
  tesseract binaries are copied into dist (the Setup Assistant replaces
  bundling; delete the earlier tesseract-copy logic; also drop nvrtc/nvJitLink
  /opengl32sw from the copy list if still present).
- CHANGELOG [Unreleased] + docs/code_reference.md entries for the new classes.

## Build & verify loop (mandatory)
`build/agent_build.bat`: VsDevCmd 2022 Community x64 → `cmake --preset
msvc-release` → `cmake --build --preset msvc-release-build`; run via
`pwsh.exe -NoProfile -Command 'cmd /c "<repo>\scripts\agent_build.bat" 2>&1; exit $LASTEXITCODE'`
(timeout 600000). Fix all errors; delete the bat when green. Machines without
the runtime MUST run cleanly (probe returns unavailable, UI shows install
hint) — that path is testable on this machine before installing anything.

## Acceptance checklist
- [ ] Build green; app runs with runtime absent (no crash, hint shown).
- [ ] First run opens Setup Assistant; decline persists.
- [ ] Tesseract installs to LOCALAPPDATA, OCR works, Remove deletes it.
- [ ] GPU arch table picks Ada for compute 8.9; hash verification enforced.
- [ ] SuperRes runs on the CUDA stream, latency guard works, toggle+strength
      persist per preset.
- [ ] NVIDIA attribution at the very bottom of Advanced; licenses updated.
- [ ] dist bundle contains zero NVIDIA/tesseract binaries.
