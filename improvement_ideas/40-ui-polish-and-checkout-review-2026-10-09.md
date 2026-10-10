# 40 — UI Polish and Checkout Review (2026-10-09)

Status: **ACTIVE — W1 resolved 2026-10-09; polish in progress.** The owner has
asked for a UI polish pass; the UI is explicitly not final. This note records
the findings from running the shipped bundle and reading the source on
2026-10-09, in the order they should be worked.

**Evidence terminology.**
`Observed` means seen in the running `dist/OkuFlow/oku_flow.exe` bundle
(built 2026-09-10) at 1942x1136 on the owner's RTX 4090 rig, with screenshots
in [`ui ideas/review-2026-10-09/`](../ui%20ideas/review-2026-10-09/).
`Confirmed in code` means the control flow was inspected in source.
`Reported` means plausible from the evidence but the cause was not traced.

**Source baseline.** Code references are to
`agent/okuflow-integrity-hardening` at `078cdde` (pushed 2026-10-09).
Relocate by symbol name after edits.

## Current implementation checkpoint

The working tree now contains S1–S4 (Codex-only startup prompting, dialog
stacking/content sizing, and friendlier dependency copy), the initial-state
portion of S5, M1's carousel badge/elision and Custom Setup correction, M3's
larger checkbox target, M4's help/recording/reset icon corrections, A1's
profile-first order, A3's readouts, A4's Zoom-dependent Focus X/Y, A5's reduced
tab strip, A6's search feedback, and A7's middle elision/full-name tooltips.
Dynamic widget labels also survive Show/Polish retranslation, covered by a
language-manager regression test. Documentation describes the current behavior.

Still open: S5's post-first-frame stall timeout; M2's redesigned grid (the
existing selection highlight was not the reported bug—the restored setup was
Custom); A2's unified headings and measured contrast; C1's shared theme module;
M5's canonical palette decision; W2 build identity; and broader C2/C3 cleanup.
M3 retains checkbox semantics and a visible checkmark rather than replacing it
with a QPushButton. A7 leaves the full model value available to Qt accessibility;
it does not replace designed accessible descriptions with device names.

Fresh 100/150/200% DPI captures and Narrator/NVDA acceptance remain outstanding.
The historical screenshots and earlier test counts do not validate this batch.

Validation on 2026-10-09: the tracked `scripts/agent_build.bat` matrix passed
on the final source: release compile, CPU 24/24, CUDA 28/28, no skipped tests,
and 674/674 complete Turkish/German entries. `git diff --check` passed. The
final incremental build emitted no compiler warnings. Logs are local at
`build/ui-polish-agent-build-final.log`. CMake/Ninja were taken from the local
Visual Studio installation; relocated preset caches from `W:` were preserved
under timestamped names before regenerating them for `Y:`. No release bundle
was published and no user OkuFlow instance was terminated.

Subsequent owner-requested toolchain migration: all active build/deployment,
translation, and profiler defaults now use Qt 6.12.0. Required Multimedia,
TextToSpeech, and Image Formats modules were added to that existing SDK.
The final tracked matrix passed release compilation, CPU 24/24, CUDA 28/28,
and 674 translations. The bundle script independently built/tested 28/28
using `C:\Users\cemka\.oz-build\release-qt612`, then published
`dist\OkuFlow\` with verified Qt 6.12.0 runtime files and a matching executable
SHA-256. Positive five-module and negative missing-PDF-notice metadata checks
passed with the new hash-suffixed Qt SPDX IDs. Logs: `build/qt612-agent-build-final.log`
and `build/qt612-release-bundle.log`. No running OkuFlow instance was stopped.

---

## W. Working copy integrity

### W1. This checkout is a hybrid and cannot build — RESOLVED 2026-10-09

**Resolution.** The owner pushed the complete tree from the build machine as
`078cdde` on `agent/okuflow-integrity-hardening`. Before switching, every
untracked source file in this folder was compared with `078cdde`:

- All of them were byte-identical, except `tests/README.md` and
  `tests/render_widget_tests.cpp`, where the commit has the newer
  nonblocking-admission retry.
- Two fixture CSVs differed only in line endings.

The folder was then checked out to `078cdde`; the only remaining untracked
file was this note. The findings below are kept for provenance.

`Confirmed in code.`

**Problem.** `HEAD` is `main` at `9e069d9` (2026-07-21). That is 6 commits
behind `origin/main` (`b0db008`) and behind the newest branch,
`agent/okuflow-integrity-hardening` (`bd52975`, 2026-08-02, pushed). The
roughly 120 untracked paths are newer files (dated up to 2026-09-10) layered on
top of `main`'s old tracked files.

**Evidence.**

| File | This checkout | `bd52975` |
|---|---|---|
| `src/ui/main_window.cpp` | 687 lines | 3229 lines |
| `src/app/app.cpp` | 2233 lines (pre-split) | 5-line stub |
| `include/okuflow/app/app.hpp` | 265 lines | 437 lines |
| `cmake/CMakeLists.txt` | 109 lines | 194 lines |
| `cmake/NativeRtc.cmake` | untracked | absent from every branch |

**Consequences.**

- `cmake/CMakeLists.txt` uses `GLOB_RECURSE src/*.cpp`, so it compiles both
  the old `app.cpp` and the split `app_*.cpp` files. They define the same
  `OkuFlowApp` methods twice (`Run`, `BuildCompositeAndPresent`,
  `OnPresetSelectionChanged`, `UpdateMousePan`, and others), which fails at
  link time.
- `tests/CMakeLists.txt` links `OkuFlow::NativeRtc`, but nothing includes
  `cmake/NativeRtc.cmake`.
- `scripts/agent_build.bat` calls the `msvc-cuda-tests` preset, which the
  checked-out `CMakePresets.json` does not define.
- `build/CMakeCache.txt` points at a stale checkout under
  `W:/Google_drive/sync/UU/projects/`. That checkout did not exist on
  this machine on 2026-10-09. It is presumably where the complete 2026-09-10
  tree and the `dist/` bundle came from.

**Fix.**

1. Owner: find the complete 2026-09-10 tree (the `W:` checkout or a Google
   Drive version). If it exists, commit it on a branch created from `bd52975`
   and push. Rebuild this folder from that branch.
2. If it does not exist:
   1. Copy the untracked files aside.
   2. Switch to `bd52975`.
   3. Overlay the copied files.
   4. Diff against the plan 33-39 notes to find which edits from 2026-08-02 to
      2026-09-10 to files *tracked* on `main` were lost (`main_window.cpp`,
      `presenter.cpp`, CMake, and others). Re-implement those edits.
3. Either way, rerun the `scripts/agent_build.bat` matrix before any UI work,
   so the polish starts from a green, committed baseline.

### W2. The shipped bundle does not match the source — P1

`Observed` plus `Confirmed in code`. Codex is installed on the rig, yet the
bundle's Setup Assistant labels its buttons **Install / Remove**.
`SetupAssistantDialog::RefreshStatus` in `src/app/setup_assistant.cpp` sets
**Update / Open Install Folder** whenever Codex is detected. The bundle was
built from a different tree than this one.

**Fix.** Do W1 first. Then add build identity (inbox seed S: commit hash and
dirty flag shown in Help and the debug log), so a bundle can always be traced
to its source.

### W3. Build output lives in Google Drive — P2

The 2026-08-02 to 2026-09-10 work was committed and pushed as `078cdde` on
2026-10-09. What remains: the 4.4 GB `build/` folder (plus `build-*` and
`dist/`) still syncs with the source.

**Fix.**

- Move build directories outside the synced folder, or exclude them from sync.
- Delete or ignore the root stray objects `maxine_probe.obj` and
  `maxine_superres.obj`.

---

## S. Startup and Setup Assistant

### S1. The Setup Assistant opens on every launch over the camera — P1

`Observed` plus `Confirmed in code`. `app_bootstrap.cpp` opens it when
`SetupAssistantDialog::NeedsSetup` is true. `NeedsSetup` returns true if
Codex is missing **or** an NVIDIA GPU is present without the optional Maxine
runtime. On the owner's rig, Codex is installed but Maxine is not, so the
dialog appears at every start until "Don't ask again automatically" is ticked.

**Fix.**

- Do not open it at startup for optional dependencies.
- Offer it when the user invokes a feature that needs a missing dependency:
  - Read, Explain, or Assistant need Codex.
  - Super Resolution needs Maxine.
- Otherwise show a single quiet, dismissible notice.
- Coordinate with inbox seed J (first-run onboarding), which owns the
  first-launch experience.

### S2. Dialog placement and stacking — P1

`Observed`; drift cause `Reported`.

**Problem.**

- The dialog is non-modal (`setModal(false)`) and has no anchored position.
- The Simple corner clusters are owned frameless tool windows. They stacked
  above the dialog and clipped its title and heading
  (`setup-assistant-over-chrome.png`).
- Escape did not close it.
- After interaction, it ended up partly off-screen at the main window's
  bottom-right (`simple-mode.png`).

**Fix.**

- Centre the dialog on the camera rectangle and clamp it to the available
  screen geometry.
- Keep it above the chrome windows, or hide the chrome while it is open.
- Make Escape close it.
- Trace why its position changed before treating the fix as complete.

### S3. The scroll area clips rows mid-line — P2

`Observed` in both the owner's screenshot and the review captures: the NVIDIA
status row "Not installed - ada installer selected" is cut through its text
at the default dialog size.

**Fix.** Size the dialog to its content up to the available height, scroll
only when needed, and leave enough bottom padding that no row is cut in half.

### S4. Setup Assistant copy — P2

`Confirmed in code`.

- `"Not installed - %1 installer selected"` shows the internal architecture
  key ("ada"). Show a user name such as "RTX 40-series package".
- The Codex path is shown raw with forward slashes. Use
  `QDir::toNativeSeparators`, and move it to a secondary line or a tooltip.
- These strings are `QStringLiteral`. Confirm that `check_translations.ps1`
  and the Turkish/German catalogs cover them (plan 33).

### S5. No camera status before the first frame — P2

`Observed`. The camera area is plain black between "window shown" (826 ms)
and "first camera frame presented" (1185 ms) in the startup log. It would stay
black indefinitely if no frames arrived. Simple mode deliberately hides
processing status, so nothing explains the black screen.

**Fix.**

- Draw a centred camera-state placeholder ("Starting camera…", "No picture
  from <camera>") until the first frame, with `SetLiveText` semantics
  (plan 32).
- Clear it on the first presented frame.
- Bring it back after a frame-arrival timeout.

---

## M. Simple mode

### M1. The profile carousel clips and shows a noisy badge — P1

`Observed`.

**Problem.**

- The current-profile label reads "Read a Page [1]", and the brackets add
  noise.
- In Advanced, the carousel is narrower and clips the label at both ends
  ("ʀead a Page [1", `advanced-top.png`).

**Fix.**

- Render the shortcut number as a separate small badge.
- Elide the name with font metrics instead of clipping it.
- Keep a minimum width that fits the longest built-in name ("Describe the
  Scene") at 100/150/200% scaling.

### M2. Mode grid states — P1

`Observed` (`mode-grid.png`).

**Problem.**

- The active mode is not marked.
- The number and the name sit on two lines at equal weight.
- Tiles have no icon.
- The grid is anchored to the left instead of centred over the camera.
- The tenth entry ("Document") has no number. This is by design, but it reads
  as an inconsistency.

**Fix.**

- Give the active tile a checked state: accent border plus a
  non-colour cue, and an accessible "current" state.
- Move the number to a corner badge and make the name the dominant text.
- Add an optional glyph per built-in mode.
- Centre the grid on the camera rectangle.

### M3. Text Clarity is undersized — P1

`Observed`. It is a ~20 px checkbox inside a cluster of ~60 px buttons, which
is the smallest target in the low-vision primary view.

**Fix.** Make it a checkable toggle button the same height as Simple/Advanced,
with on/off shown by more than colour (icon or label change).

### M4. Iconography is inconsistent — P2

`Observed`.

- Record is a filled red dot, while Photo, Explain, Read, and Draw are
  outline glyphs.
- Help is a filled blue circle.
- Reset Tuning has a stray emoji-like glyph that no other inspector button
  has.

**Fix.** Choose one icon family and stroke weight. Keep red for Record only
as a state colour, ideally only while recording.

### M5. The accent colour disagrees with the docs — P3

`Observed`. The bundle shows blue accents. `docs/ui_modes_design.md`,
`design-qa.md`, and the July captures describe purple. This is probably the
colour-scheme picker default.

**Fix.** Decide which is canonical and update the other. Fold the decision
into C1.

---

## A. Advanced inspector

### A1. Section order buries the image controls — P1

`Observed` (`advanced-top.png`, `advanced-profile.png`). The order is
Application (language) → Device → Recording → Profile. The controls the user
tunes most (Magnification, Readability) start several screens down, behind
settings that rarely change.

**Fix.** Either put Profile first, or keep Image = Profile and move
Application, Device, and Recording into a separate Settings tab. Keep plan
27's ownership scopes unchanged; this is ordering and navigation only.

### A2. Heading hierarchy and contrast — P2

`Observed`.

- "Profile" is a plain accent label with a subtitle, while every other
  section is a collapsible bar.
- The grey subtitles ("Shared by every profile", "Saved into the selected
  quick option") look low-contrast on the dark panel.

**Fix.**

- Use one heading component and level for all four scopes.
- Measure the subtitle contrast. Target 7:1 for this audience, never below
  4.5:1.

### A3. Slider value readouts are inconsistent — P1

`Observed`.

- Sigma and Radius show their values.
- Zoom, Focus X, Focus Y, and Black & White show none.

**Fix.** Route every slider through `ResponsiveSliderRow` with a formatted
value and unit (×, %, px), locale-formatted per plan 33.

### A4. Dependent controls look enabled — P1

`Observed`. Focus X/Y render as active while Zoom is unchecked. This is
already **U4** in [plan 04](04-accessibility-ux.md); fix it there and verify
it visually here.

### A5. Tab strip — P3

`Observed`.

- Wrap-around previous/next arrows frame only three tabs (Image, Assistant,
  Transcript).
- The filled blue "?" does not match the other icons.

**Fix.** Drop the arrows while there are three or fewer tabs; Ctrl+Tab still
covers keyboard use. Restyle help per M4.

### A6. Settings search — P2

`Observed`. The placeholder is faint and the field has only a thin underline.

**Fix.** Raise the contrast, give it a full field border and a search glyph,
and show a match count or a "No matching settings" state.

### A7. Long device names are cut off — P2

`Observed`. The microphone combo shows "SteelSeries Sonar - Microphone
(SteelSeries Sonar Virtual A…" with the end cut off and no way to read the
full name.

**Fix.** Elide in the middle, put the full name in the tooltip and the
accessible description, and do the same for the camera combo.

---

## C. Code structure that the polish depends on

### C1. One theme module — P1, do first after W1

`Confirmed in code`. Styling is spread across 16 `setStyleSheet` calls in
`main_window.cpp`, `setup_assistant.cpp`, `annotation_overlay.cpp`,
`assistive_overlay.cpp`, `color_scheme_picker.cpp`, `ai_settings_dialog.cpp`,
and `app_interaction.cpp`. These sit alongside `color_schemes.cpp`.

**Fix.**

- Add one theme module with tokens per colour scheme (colours, border width,
  radius, control heights, spacing, icon size) that generates the stylesheet.
- Make M3, M4, M5, A2, and A6 token changes instead of per-file edits.

### C2. `OkuFlowApp` is still the hub — P2

`Confirmed in code`.

- `OkuFlowApp::Initialize` in `app_bootstrap.cpp` is about 1,100 lines.
- `app_pipeline_runtime.cpp` is 2,722 lines.
- `main_window.cpp` is 3,401 lines (inbox seed R).
- `MainWindow`, `InteractionController`, and `UIStateManager` are friends of
  `OkuFlowApp` (`app.hpp`).

**Fix.** Extend inbox seed **R**, which covers only `main_window.cpp`:

- Split `Initialize` into staged builders (platform, settings, UI, capture,
  services).
- Replace the friend access with narrow interfaces as panels move out.

### C3. Explicit source lists — P2

`GLOB_RECURSE` silently pulled the stale `app.cpp` into the W1 build.

**Fix.** List sources explicitly in `cmake/CMakeLists.txt`, so stale or
duplicate files fail loudly in review rather than at link time.

(A README-drift finding was dropped: the README in `078cdde` already
describes the Documents storage root and Codex vision reading.)

---

## Suggested order

1. **W1** (resolved), then a green build on `078cdde`. W2's build identity
   comes with inbox S; W3 is housekeeping.
2. **C1**: the theme module.
3. **S1-S5**: startup and Setup Assistant.
4. **M1-M5**: Simple mode.
5. **A1-A7**: the Advanced inspector. A4 lands through plan 04 U4.
6. C2-C3 as standing maintenance.

## Verification for each polish step

- `scripts/agent_build.bat` matrix green (release, CPU, CUDA,
  translation parity).
- Before/after screenshots of Simple, the mode grid, Advanced top, Advanced
  Profile, and the Setup Assistant at 100%, 150%, and 200% scaling. Take them
  with Windows Magnifier off, because it corrupts window captures.
- Keyboard-only pass over every changed control, plus an NVDA/Narrator spot
  check of changed names and states (plan 32 rules).
- Update `docs/ui_modes_design.md` and `design-qa.md` whenever layout or
  colour decisions change.
