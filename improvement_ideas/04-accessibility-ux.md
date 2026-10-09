# Accessibility & UX

OkuFlow's mission (per `agents.md`) is an AI-assisted magnifier for people with low
vision — yet the app itself currently has almost no assistive-technology support. These
are mostly small-effort, high-mission-value items.

---

## U1. Screen-reader labels on all interactive controls

- **Status: DONE 2026-07-21.** All interactive widgets in `MainWindow` now set
  accessible names and descriptions. Still open from this item: explicit
  `setTabOrder` audit and a Narrator/NVDA hardware test.
- **Priority:** HIGH · **Effort:** small · **Status:** Confirmed (zero `setAccessibleName` calls in the codebase)
- **Evidence:** `src/ui/main_window.cpp` constructor (~lines 310–563) creates 40+ controls with no `setAccessibleName()` / `setAccessibleDescription()`

**Problem.** Windows Narrator/NVDA users get anonymous sliders and checkboxes. For this
product's audience, that is a core defect, not a polish item.

**Fix.** In the `MainWindow` constructor, set an accessible name (and description where
the purpose isn't obvious) on every interactive widget, e.g.
`zoomSlider_->setAccessibleName(tr("Zoom amount"));`. Add a small helper to keep it to
one line per widget. Also verify tab order (`setTabOrder`) walks the controls logically:
quick modes first, then advanced panel. Test with Narrator (Win+Ctrl+Enter).

---

## U2. Respect system High Contrast and palette instead of hardcoded colors

- **Priority:** HIGH · **Effort:** medium · **Status:** Reported
- **Evidence:** `src/ui/main_window.cpp` — `AssistiveOverlay::paintEvent` (~127–155) hardcodes `QColor(18,18,18,212)` background / `QColor(255,247,214)` text; focus marker colors similarly fixed in `src/app/app.cpp` (~2109–2111)

**Problem.** Users who run Windows High Contrast or an inverted scheme — heavily
overrepresented in this app's audience — get overlay colors that ignore their settings.

**Fix.** Derive overlay colors from `QPalette` (`Window`/`WindowText` roles), which Qt
maps to the system theme, and check `QStyleHints::colorScheme()`. Offer an explicit
overlay-style choice (System / Dark / Light / Yellow-on-black) as an advanced setting —
fixed high-legibility palettes are themselves an accessibility feature. Make overlay font
size configurable while there.

---

## U3. Keyboard-only operation audit

- **Priority:** MEDIUM · **Effort:** medium · **Status:** Reported (partially improved 2026-07-22: number keys `1`–`9` apply quick modes, `Ctrl+H` pins Simple chrome, `Esc` closes the grid/panel, `Tab` traverses the corner clusters; zoom in/out, photo, record, and B&W toggle still require the mouse)
- **Evidence:** README "Navigation And Interaction"; `src/ui/main_window.cpp` key handling (~2075–2137)

**Problem.** Zoom in/out, preset switching, photo capture, and recording all require
mouse interaction (Ctrl+wheel, buttons). Low-vision users are disproportionately
keyboard-dependent.

**Fix.** Add `QShortcut`s: zoom in/out (`Ctrl+=`/`Ctrl+-`), cycle quick modes
(`Ctrl+1..9`), toggle B&W (`Ctrl+B`), capture photo (`Ctrl+P`), toggle recording
(`Ctrl+R`), read OCR result aloud / re-run analysis (`Ctrl+O`). Document them in the
README and expose them in tooltips.

---

## U4. Dependent-control enabled state can desync from loaded config

> **IMPLEMENTED 2026-07-26 through
> [plan 27](27-advanced-panel-redesign.md) Phase 0.**
> Enabled state now comes from `OkuFlowApp::UpdateControlEnabledStates()` after
> config application and toggle changes. Kept here for the evidence.

- **Priority:** MEDIUM · **Effort:** small · **Status:** Implemented
- **Evidence:** `src/ui/main_window.cpp` (~410–462) initializes sub-sliders (`bwSlider_`, `zoomSlider_`, `blurSigmaSlider_`, …) as disabled; enablement is wired in app.cpp slots

**Problem.** If settings load with a feature enabled but the corresponding toggle slot
isn't invoked during startup sync, the slider stays disabled while the feature is active
(UI lies about state). Verify by loading a settings.json with blur enabled.

**Fix.** After applying persisted config at startup, run one explicit
`UpdateControlEnabledStates()` pass that derives every dependent widget's enabled state
from the config flags. Call the same function from each toggle slot so there is exactly
one source of truth.

---

## U5. Surface pipeline/assistive errors to the user instead of silent fallback

- **Priority:** MEDIUM · **Effort:** small–medium · **Status:** Reported
- **Evidence:** `src/app/app.cpp` — CUDA failure silently flips to CPU (~1776–1781); readback failures silently skip analysis (~1983–1984); frame-conversion failures return silently (~1948–1955)

**Problem.** Users see "Processing: CPU (fallback)" with no reason, or an assistive
overlay that just stops updating. For this audience, silent degradation is
indistinguishable from breakage.

**Fix.** Introduce a small error-category enum propagated up from the pipeline, and show
the last error with a timestamp in the status area ("GPU path unavailable: CUDA surface
init failed — using CPU"). Add exponential backoff on CUDA re-attempts instead of
retrying every frame.

---

## U6. Assistive analysis cadence: fixed 1.6 s interval, no feedback

- **Priority:** LOW · **Effort:** medium · **Status:** Reported
- **Evidence:** `src/app/app.cpp` (~966): `constexpr qint64 kAssistiveIntervalMs = 1600;`; busy-check drops frames rather than queueing (~962)

**Problem.** OCR/scene-description lags the live view by up to 1.6 s regardless of
content or load, and the user can't tell whether analysis is running, queued, or dead.

**Fix.** Make the interval a setting (range ~500–5000 ms); show analysis state in the
overlay ("analyzing…", "OCR idle", age of the current result). Optionally trigger
analysis immediately on significant view change (zoom center moved, mode switched)
rather than purely on a timer.

---

## U7. Reject degenerate frames before spending VLM quota

- **Priority:** LOW · **Effort:** small · **Status:** Reported
- **Evidence:** `src/common/assistive_runtime.cpp` — `SubmitFrame()` (~205–216) checks null/zero only

**Fix.** Skip analysis for frames below a useful size (e.g. <64×64) and downscale very
large frames (>2048 px on the long edge) before JPEG-encoding for the VLM — faster,
cheaper per request, and most VLM endpoints downscale anyway.
