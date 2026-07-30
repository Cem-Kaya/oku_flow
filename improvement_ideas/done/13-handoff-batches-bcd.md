# Plan 13 — Handoff: Batches B, C, D for the implementing agent (2026-07-23)

This is the orchestration document for the next three work batches. It tells
you (the implementing agent) WHAT to do in WHICH order, where the detailed
specifications live, how to build, and exactly what to report back. It
assumes you have NO prior context: everything you need is in this file plus
the two plan files it points into — plan 11
(improvement_ideas/11-hardening-refactor-plan.md) and plan 12
(improvement_ideas/12-color-picker-redesign.md). Read both END TO END before
writing any code; they contain the per-item rationale, the concrete code
touchpoints with file/line anchors, the known pitfalls, and the acceptance
checklists. This file does not repeat all of that detail — it binds the
batches together and adds the process rules.

## Execution order is MANDATORY and sequential

Batch B (performance), then Batch C (color system), then Batch D
(architecture). This is not a preference. All three batches touch the same
files — `src/app/app.cpp`, `src/cuda/cuda_interop.cpp`,
`src/cuda/cuda_kernels.cu` — and parallel or reordered work will conflict
textually and semantically: Batch D moves the very functions Batches B and C
edit, so running D early would strand the other batches' diffs, and running
C before B would make every Batch-B measurement incomparable to its
baseline. Finish each batch completely — build green, smoke test passed,
report written — before starting the next.

**Precondition:** Batch A (the SuperRes fixes, the P8 frame-timing
instrumentation, the S6b `FenceSequencer`, and the S4 threading audit) is
already merged. Before you start, VERIFY this state instead of trusting it:

1. `FenceSequencer` exists in `include/openzoom/app/app.hpp` (~line 89) and
   `OpenZoomApp::RunCudaPipeline` in `src/app/app.cpp` (~3209–3256) issues
   tickets through it (`BeginCudaFrame` / `CudaSignaled` / `CudaFailed`).
2. Run the app on hardware, hover the colored processing-status text in the
   corner, and confirm the tooltip shows the timing line
   `"NN.N ms/frame - CUDA - GPU NN.N ms"` (produced by
   `UpdateProcessingStatusLabel`, app.cpp ~2790–2803).

If either check fails, STOP and report that the precondition does not hold.
Rebase your entire reading of the code on the post-Batch-A state: line
numbers in the plans are anchors, not gospel — re-locate every symbol by
name before editing near it.

## Batch B — Performance — DONE 2026-07-23

(Plan 11 Wave 3, implemented through its benchmark gates. Measurements and
the P4/P6 decisions are recorded in plan 11.)

Scope: plan 11 "Wave 3 — Performance remainder". The full per-item
specifications (WHY / HOW / PITFALLS / ACCEPTANCE, with code anchors) are in
plan 11; do not improvise beyond them.

Order inside the batch, fixed:

1. **P11 pinned staging ring** — biggest win, do first so every later
   measurement benefits from the improved baseline. Two pinned host buffers
   (`cudaMallocHost`), alternating; the capture path memcpys the sample into
   the current pinned slot; `ProcessFrame` uploads from pinned memory so
   `cudaMemcpy2DAsync` becomes truly asynchronous. Applies to the BGRA path
   AND the raw NV12/YUY2 path. Slot reuse MUST be guarded against the
   in-flight upload copy (per-slot `cudaEvent` — plan 11 explains why the
   shared-fence values cannot protect the host-side memcpy) and the
   thread-ownership rules must be documented at the member declarations
   (the MF capture callback and the Qt tick share this data via
   `cameraMutex_` today; see app.cpp ~3372–3375 and ~3663–3667).
2. **P12 dead symbol** — delete `gGaussianKernelSize`
   (`src/cuda/cuda_kernels.cu` ~348 and its upload ~2019–2023) after
   grepping that nothing reads it.
3. **P13 async kernel upload** — `UploadGaussianKernel()`
   (cuda_kernels.cu ~1983–2029) switches to
   `cudaMemcpyToSymbolAsync(..., stream)` and DROPS its
   `cudaDeviceSynchronize()`; mind the source-buffer-lifetime pitfall
   documented in plan 11.
4. **P14 projection shared-memory reduction** —
   `StabilizationProjectionKernel` (cuda_kernels.cu ~773–785) accumulates
   into per-block `__shared__` partial sums, then one global `atomicAdd`
   per bin per block (model: the auto-contrast histogram kernel
   ~1199–1205).
5. **P4 box-chain blur — BENCHMARK-GATED.** New 3× iterated box-blur kernel
   behind the SAME `LaunchGaussianBlurLinear` signature; old kernels stay
   compiled; the launcher picks by radius (> 8 → box chain). The gate, using
   the Batch-A timing numbers: the box chain must be FASTER at radius 25 and
   above AND the golden-frame diff of the approximation must be acceptable.
   If either fails, REVERT and close P4 in plan 11 as CLOSED-WONTFIX with
   the measured numbers. Do not argue with the gate in either direction —
   the numbers decide.
6. **P6** only if trivial (single-allocation device buffers); otherwise
   write one line in the report deferring it.

Measurement protocol for EVERY item: read the P8 tooltip numbers (CPU
ms/frame rolling average and GPU ms) before and after, same camera, same
scene, same settings, letting the averages settle for 10–15 seconds
(the GPU number samples every 30th frame). Report every before/after pair.
No visual changes are permitted anywhere in this batch EXCEPT the approved
P4 blur approximation — and that one only if its gate passes.

## Batch C — Color system (plan 12, WITH its LUT design — the LUT contract supersedes the original two-color kernel contract)

Scope: plan 12 in its entirety. Plan 12 has been rewritten as one merged
specification; its §2 "Ground truth" section is required reading because it
corrects two details a naive reading would get wrong (kernel modes 0/1 are
full-color paths that stay outside the LUT, and the legacy pair schemes are
SMOOTH luma lerps, so migration tables must be 2-stop smooth to pass the
golden-frame gate).

Order inside the batch, fixed:

1. **Backend first.** 256-entry luma-LUT kernel path: stops[2..8],
   stepped|smooth, LUT precomputed app-side ONLY on scheme change and
   uploaded once (constant memory, async on `stream_`, generation-cached —
   never per frame). Built-in schemes collapse into ONE app-side stop
   table (which must reproduce the kernel's exact float colors, not the Qt
   combo's — plan 12 §3.3). Old `displayColorMode` ints migrate with
   identical visuals — golden-frame check (max per-channel delta ≤ 1,
   report the measured delta) BEFORE starting the UI.
2. **Then the picker UI.** Swatch trigger button + Sheets-style popover:
   grid tiles custom-painted background + "A" glyph; an Effects section
   whose tiles are painted as mini step/gradient strips; Reset row
   (= Normal colors); Custom editor with mode selector (Two colors /
   Posterize / Gradient), stop-count spinner (2–8), per-stop color wells
   (QColorDialog), stepped/smooth toggle, live mini-strip preview, and a
   persistent pencil-badged custom tile. Reuse the mode-grid popup pattern
   exactly (same window flags, Esc-to-close, focus-restore-on-close, no
   fade — reference implementation cited in plan 12 §4.2). Full
   accessibility per plan 12 §5: focusable tiles, spoken names ("Yellow
   text on black background", "Color stop 3 of 6"), arrow-key grid
   navigation, 32 px targets, check+border selection, announcements via
   `QAccessibleAnnouncementEvent`. **NO hover live-preview — owner
   decision; the camera view updates on selection only.**
3. **Acceptance** (full checklist in plan 12 §7): posterize-6, grayscale,
   and yellow-tint gray each buildable in ≤ 6 interactions via Custom; old
   settings load pixel-identical; the old bar-list UI fully gone; custom
   scheme survives restart; and a SCREENSHOT of the open popover included
   in your report for owner approval.

## Batch D — Architecture (plan 11, Wave 4 — the discipline matters more than the code)

Scope: plan 11 "Wave 4 — Architecture decomposition". The rules are the
deliverable as much as the code: ONE extraction per step; build green plus a
manual smoke test after EACH step; NO behavior changes mixed with moves;
record the `src/app/app.cpp` line count after every step in your report
(it starts around 4,169 lines and must shrink monotonically).

Status: **Code complete, rebuilt, and basic Windows-smoke verified on
2026-07-23.** The CUDA and CPU configurations build, CPU tests pass, and the
CUDA application completed a 45-second live-camera run followed by a clean
window-close exit. The required managers, `Initialize()` split, app
responsibility translation units, and additional standalone UI widgets are
present. `src/app/app.cpp` is 5 composition-only lines. Intermediate per-step
line counts were not preserved in the continuous uncommitted worktree; this is
recorded as a process deviation rather than reconstructed.

Sequence, fixed:

1. **SuspendGuard** (A2) — 10-line RAII guard replacing every same-scope
   manual set/clear of the three suspend flags
   (`configTrackingSuspended_`, `presetSelectionSyncSuspended_`,
   `suspendControlSync_`). One site is a cross-function latch that must
   stay a plain assignment — plan 11 step 1 identifies it.
2. **RecordingManager** (A3 + A1.1) — new
   `src/app/recording_manager.cpp/hpp`, owning `videoRecorder_` and
   friends, with `enum class RecordingState {Idle, Starting, Recording,
   Stopping, Error}` and a single `SetRecordingState()` transition point;
   async-readback drain consumption, disk-full messaging, output paths.
   app.cpp keeps only the button slot delegating in.
3. **GATE:** create minimal Catch2/QtTest `settings_store` round-trip tests
   FIRST — plan 06 item B2 — wired to the existing `OPENZOOM_ENABLE_TESTS`
   cmake option (which currently warns that `tests/` is missing,
   cmake/CMakeLists.txt ~123–128). Round-trip, legacy-v1 migration,
   corrupt-file, out-of-range-clamp cases. These tests must run CPU-only.
   Do NOT proceed to step 4 until they exist and pass.
4. **SettingsController** (A1.3) — PersistentSettings load/save, config
   apply/capture, preset resolution/promotion. The UI-blocker choreography
   stays in app.cpp in this step (it moves in step 5).
5. **UIStateManager** (A1.4) — owns ALL widget pointers and ALL
   QSignalBlocker logic behind `ApplyConfigToUI()` / `ReadConfigFromUI()`;
   enforces the A5 policy (non-null references after construction; drop
   the ~71 scattered null checks). CRITICAL pitfall: the blocked-write +
   manual-slot-call pairs in `ApplyAdvancedConfig` must move WHOLESALE or
   every control double-fires or never fires — plan 11 step 4 spells this
   out with line anchors.
6. **AssistiveFeatureManager** (A1.2).
7. **PipelineOrchestrator** (A1.5) — frame tick, CUDA path selection,
   absorbs the `FenceSequencer` + failure recovery and the camera
   reconnect state machine.
8. **A4 constructor→Initialize() split** — last, once everything owns its
   own setup.

Target end state: `app.cpp` under 800 composition-only lines.

## Build recipe — fully spelled out (every batch, every step)

The build runs on Windows via a batch file you create, invoked from the
WSL/Linux agent shell through the PowerShell 7 bridge (per agents.md: use
`pwsh.exe`, never the legacy `powershell.exe`).

1. Create `build/agent_build.bat` with exactly this shape (the `build/`
   directory exists and is untracked; adjust nothing else):

   ```bat
   call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64
   if errorlevel 1 exit /b 1
   cd /d <repo>
   cmake --preset msvc-release
   if errorlevel 1 exit /b 1
   cmake --build --preset msvc-release-build
   ```

   That is: initialize the VS 2022 Community x64 developer environment
   (VsDevCmd), then configure with the `msvc-release` preset, then build
   with the `msvc-release-build` preset. The presets are defined in
   `cmake/CMakePresets.json` (the root `CMakePresets.json` is only a 4-line
   include shim — edit the one under `cmake/` if you ever must, which you
   should not for these batches). `msvc-release` uses Ninja, CUDA ON,
   TEXT_SR ON, Qt at `C:/Qt/6.9.3/msvc2022_64`, CUDA architectures
   75;86;89, and builds into `build/msvc-release`.

2. Invoke it from the agent shell with:

   ```
   pwsh.exe -NoProfile -Command 'cmd /c "<repo>\scripts\agent_build.bat" 2>&1; exit $LASTEXITCODE'
   ```

   with a command timeout of 600000 ms (10 minutes — a clean configure plus
   full CUDA build needs it; incremental rebuilds are much faster). The
   `2>&1` matters: MSVC and nvcc write errors to stderr and you want them
   in the captured output. The `exit $LASTEXITCODE` matters: without it the
   bridge swallows the failure code and you will believe a red build is
   green.

3. Fix ALL errors and warnings you introduced; rebuild until green.

4. **Delete `build/agent_build.bat` when the build is green** (each batch
   may recreate it; it must not be left behind or committed).

5. **CPU-only build (Batch D, step 3 and later):** the settings tests must
   run without CUDA, so from Batch D step 3 onward ALSO build the CPU
   preset at least once per step:

   ```bat
   cmake --preset msvc-cpu
   cmake --build --preset msvc-cpu-build
   ```

   (same bat-file + pwsh-bridge mechanics; `msvc-cpu` configures with
   `OPENZOOM_ENABLE_CUDA=OFF` into `build/msvc-cpu`; enable
   `OPENZOOM_ENABLE_TESTS` for this preset when wiring the tests, and run
   them via ctest from the build directory). A CPU-only compile gate has
   historically caught real breakage in this repo — do not skip it.

## Documentation duties (every batch)

- `CHANGELOG.md`: add entries under `[Unreleased]` for user-visible or
  behavioral changes, following the existing entry style (complete
  sentences, mention the plan/wave item).
- Mark completed items DONE with the date in plans 11/12 (the same style
  the existing DONE items use — keep their status lines intact); if P4's
  gate fails, mark it CLOSED-WONTFIX with the numbers.
- `docs/code_reference.md`: an entry for every new class/struct
  (RecordingManager, SettingsController, UIStateManager,
  AssistiveFeatureManager, PipelineOrchestrator, the color-scheme picker,
  the scheme table...) in the file's established format — it is the
  authoritative code map and agents.md makes updating it mandatory in the
  same change.

## How to report back (the reviewing agent parses this structure)

Write ONE report per batch, delivered as your final message for that batch
(not as a committed file). The reviewing agent will check it line-by-line
against plans 11/12 and this document, so use exactly these sections:

1. **Summary** — one paragraph: what the batch did and its overall outcome.
2. **Changes** — per work item (P11, P12, ... / C-backend, C-ui / D-step-N):
   files touched, symbols added/removed, and a one-line description of the
   approach actually taken. Note explicitly where you followed the plan's
   HOW verbatim and where you had to diverge (divergences also go in
   section 7).
3. **Build results** — the exact commands run and their exit status for
   `msvc-release` (and `msvc-cpu` + ctest output where Batch D requires
   it); confirmation that `build/agent_build.bat` was deleted after the
   final green build.
4. **Measurements (Batch B only)** — a table with one row per item:
   item, CPU ms/frame before, after, GPU ms before, after, scene/settings
   used. For P4 additionally: the radius-8/25/50 timing table, the
   golden-frame verdict, and the gate decision (kept or reverted with
   CLOSED-WONTFIX).
5. **Screenshot (Batch C only)** — the open picker popover (grid + effects
   + custom section visible), plus the golden-frame max-delta number for
   the legacy-mode migration. The screenshot is for owner approval; the
   batch is not accepted without it.
6. **Line counts (Batch D only)** — a table: step name, app.cpp line count
   after that step (the count must be monotonically decreasing; final row
   states the end count vs the < 800 target).
7. **Deviations** — every place the implementation differs from plans
   11/12/13, with the reason. An empty section means "implemented exactly
   as planned" and you are asserting that.
8. **Docs** — the CHANGELOG entries added, plan items marked DONE, and
   code_reference.md sections added/updated.
9. **Smoke test log** — the manual checks performed (camera on, preset
   switching, recording start/stop, photo, reconnect-by-unplug where
   relevant) and their outcomes.

Keep raw command output out of the report body except where a section asks
for it; state results in your own words with the numbers inline.
