# Plan 12 — Display Colors Picker Redesign + Luma-LUT Color System (2026-07-23)

**Status: IMPLEMENTED (Batch C, 2026-07-23).** Legacy modes 0-16 passed the
1-LSB migration gate (measured maximum per-channel delta: 1). The compact
picker, custom scheme persistence, accessibility wiring, and generation-cached
CUDA LUT upload are in place. Final owner screenshot/screen-reader review is a
release-validation artifact, not remaining implementation work.

This document is the SINGLE merged specification for the display-colors
rework. It originally consisted of a base plan (two-color fg/bg kernel
contract + Sheets-style picker) and a later revision (generalize to a luma
LUT). The revision SUPERSEDES the base plan's kernel contract: there is no
fg/bg-parameter kernel step anymore — the backend goes straight to the
256-entry luma LUT described below. The picker UI from the base plan
survives intact and gains the "Effects" section and the richer Custom
editor. Everything in this file is the current owner-approved design; where
an owner decision is recorded, it is marked as such and must not be
re-litigated by the implementing agent.

## 1. Problem and goal

The current "Display colors" control renders as a `QComboBox` whose items
are huge full-width preview bars (a custom `ColorSchemeDelegate` paints
50px-tall swatch rows; screenshot reviewed by the owner 2026-07-23). It is
visually noisy, consumes the entire inspector column when open, gives poor
feedback about which item is selected, and scales badly as schemes multiply
— 17 modes already produce a scroll list. The owner wants a Google-Sheets-
style compact swatch grid, adapted to this app's domain: most schemes here
are a foreground/background TEXT pair (not a single color), and — per the
revision — the underlying model is more general still: every scheme is a
mapping from pixel luminance through a small palette.

Why the generalization matters: two colors is a special case, not the
model. The unifying abstraction is a **luma LUT** — every scheme maps pixel
luminance through a 256-entry lookup table built from 2–8 palette *stops*,
either STEPPED (posterize/quantize into hard bands) or SMOOTH (gradient
interpolation). This one mechanism covers everything requested:

- **Duotone** (all current fg/bg schemes) = 2 stops — behavior unchanged,
  now data instead of code. (See the ground-truth note in §2 about
  stepped-vs-smooth for these: the CURRENT kernel lerps smoothly, and the
  migration must reproduce that exactly.)
- **Posterize to N colors** (e.g. a 6-color pastel look) = N stops, stepped.
- **Grayscale** = black→white, 2 stops, smooth.
- **Tinted grayscale** ("gray with a yellow hue") = black→warm-yellow→white,
  3 stops, smooth. Sepia, blue-light-comfort, etc. are just stop tables.
- Future anything = another stop table; zero kernel changes.

For the users this app serves (legally blind students reading projected
lecture content through a camera), the color system is not cosmetic: a
well-chosen luminance remap is often the difference between "can read the
slide" and "cannot". The picker must therefore be fast to operate, fully
keyboard-accessible, screen-reader friendly, and must never disturb the
live video while the user is merely browsing options.

## 2. Ground truth — what the code does today (read before coding)

The implementing agent has no other context, so here is the exact current
state, with file/line anchors (verified 2026-07-23; re-locate by symbol
name if lines have drifted):

**The kernel.** `DisplayColorGradeKernel` in `src/cuda/cuda_kernels.cu`
(~978–1034) runs in place near the END of the GPU pipeline (launched from
`CudaInteropSurface::ProcessFrame`, `src/cuda/cuda_interop.cpp` ~2183–2198,
via `LaunchDisplayColorGradeLinear`, cuda_kernels.cu ~1340–1358). Per
pixel it does, in order:

1. Optional auto-contrast stretch (`ApplyAutoContrast`, ~927–931), reading
   the device-resident levels — no host readback.
2. Contrast/brightness (`ApplyContrastBrightness`, ~920–922).
3. A `switch (colorMode)`:
   - mode 0: no remap (full color preserved);
   - mode 1: full-color per-channel INVERSION (`1 - channel`, ~1010–1014);
   - modes 2–16: compute float luma (`0.299r + 0.587g + 0.114b`, ~1020),
     resolve a hardcoded fg/bg pair (`ResolveTextColorScheme`, ~933–965 — a
     BGR `float3` table) plus a dark-end selector (`SchemeUsesDarkText`,
     ~967–970), then LINEARLY INTERPOLATE between the dark end and the
     light end by luma (~1026–1028).

Two consequences the design depends on:

- **Modes 0 and 1 are full-color paths, not luma remaps.** A luma LUT
  cannot express per-channel inversion (it collapses hue). Therefore modes
  0 ("Normal colors") and 1 ("Inverted colors") KEEP their existing
  non-LUT branches in the kernel. The LUT replaces modes 2–16 and carries
  all new effects. This is a constraint of the "identical visuals"
  migration requirement, not a new decision.
- **Ground-truth correction on "stepped duotone":** the revision text
  described current duotone schemes as "stepped (threshold)". The actual
  kernel LERPS SMOOTHLY between the pair (see ~1026–1028). Since the
  binding acceptance criterion is that old `displayColorMode` values load
  with IDENTICAL visuals (golden-frame checked), the migration tables for
  modes 2–16 MUST be 2-stop SMOOTH schemes, ordered dark-end-first
  according to `SchemeUsesDarkText`. A hard-threshold duotone would fail
  the golden-frame gate. Stepped 2-stop schemes remain available as a
  CHOICE in the Custom editor; they are simply not what the legacy modes
  map to.

**Launch-skip fast path.** The grade kernel is only launched when there is
something to do: `gradeColorMode != 0 || contrast != 1 || brightness != 0
|| autoContrastActive` (cuda_interop.cpp ~2185–2188). Additionally, when
adaptive binarization + two-color text is active, `gradeColorMode` is
forced to 0 to avoid double-remapping the already-composited text image
(~2183–2184). Both behaviors must survive: the LUT path needs an
"identity/no-remap" notion so "Normal colors" continues to skip the launch
entirely, and the binarization suppression must keep working.

**Text-clarity two-color interplay.** `LaunchTextMaskComposite`
(cuda_kernels.hpp ~190–195; call site cuda_interop.cpp ~1879–1885) receives
`settings.enableTwoColorText ? settings.displayColorMode : 0` and colors
binarized text with the scheme pair. Under the new model this consumer
reads **stops[0] and stops[count-1]** of the active scheme (the two extreme
stops) — one picker rules both features. Plumb the pair to that call site
app-side; do not teach the composite kernel about LUTs.

**The UI today.** `src/ui/main_window.cpp`: `DisplayColorOptions()`
(~82–104) is a 17-entry table of names + QColor fg/bg pairs
(indices match the kernel modes; entries 0/1 are "Normal colors" /
"Inverted colors"); `ColorSchemeDelegate` (~106–159) paints the oversized
bar rows (modes 0/1 get color stripes, others get bg fill + fg text
lines); the row itself is built at ~1415–1450 (combo created ~1418, 50px
min height, delegate + per-item roles `kColorForegroundRole`/
`kColorBackgroundRole` from ~73–74, accessible text per item). The combo
accessor is `MainWindow::displayColorCombo()` (~2178). ALL of this
delegate/bar machinery is deleted by this plan ("the old bar-list UI is
fully gone" is an acceptance criterion).

**App wiring.** `OpenZoomApp` harvests the combo at `src/app/app.cpp`
~405, connects `currentIndexChanged` → `OnDisplayColorModeChanged` at
~700–703; the slot (~2517) clamps into `displayColorMode_` (member,
`include/openzoom/app/app.hpp` ~437) and syncs persistence.
`ApplyAdvancedConfig` writes the combo back under `QSignalBlocker` and
manually invokes the slot once (~1447–1467). `RunCudaPipeline` copies
`displayColorMode_` into `ProcessingSettings.displayColorMode` (~3180).
`kDisplayColorModeCount` = 17 (`include/openzoom/app/constants.hpp` ~13).

**Persistence.** `settings::AdvancedConfig.displayColorMode`
(include/openzoom/app/settings_store.hpp ~37) round-trips through
`ConfigToJson`/`ConfigFromJson` (`src/app/settings_store.cpp` ~289 and
~348–350, clamped to the mode count on read) and participates in
`AreConfigsEquivalent` (~806). Presets capture/apply it via
`CaptureCurrentAdvancedConfig` (app.cpp ~1118) / `ApplyAdvancedConfig`.

## 3. Backend specification (implement FIRST, before any UI)

### 3.1 Kernel contract

The app precomputes a 256-entry RGBA LUT from `{stops[], count, stepped}`
whenever — and ONLY whenever — the scheme changes, and uploads it once. The
color-grade kernel's modes 2+ branch becomes a single `lut[lumaByte]`
lookup: cheaper than today's per-pixel branching and switch, and maximally
flexible. Contrast/brightness (and auto-contrast) still apply BEFORE the
LUT, exactly as they apply before the remap today; the LUT indexes the
post-adjustment luma quantized to 0–255.

Concretely:

- Store the table in `__constant__` memory (256 × 4 bytes = 1 KB — well
  within constant-memory budget) or a small persistent device buffer;
  constant memory is preferred because every thread in a warp reads the
  same-or-nearby entries and the constant cache broadcasts well.
- Upload via a new launcher-side function in cuda_kernels.cu (e.g.
  `UploadDisplayColorLut(const uchar4* lut256, cudaStream_t stream)`)
  using `cudaMemcpyToSymbolAsync(..., stream)` — follow the P13 lesson from
  plan 11: NO default-stream copies, NO `cudaDeviceSynchronize`. Stream
  FIFO order guarantees the copy lands before the next grade launch on the
  same stream.
- Cache the upload exactly the way `EnsureGaussianKernel` caches blur
  weights (`src/cuda/cuda_interop.cpp` ~1447–1469): `CudaInteropSurface`
  keeps a "current LUT generation/key" and re-uploads only when the app
  passes a different one. The app side bumps the generation when the user
  picks a scheme or edits a custom scheme. **Uploads happen only on scheme
  change — NEVER per frame.** A per-frame upload would serialize the
  stream and is the classic naive mistake here.
- Kernel behavior by mode after this change:
  - "Normal colors": no LUT, and the existing launch-skip condition keeps
    skipping the kernel when contrast/brightness/auto-contrast are neutral.
  - "Inverted colors": the existing per-channel inversion branch, verbatim.
  - Everything else (built-in pairs, effects, custom): `lut[lumaByte]`.
- `ProcessingSettings` (include/openzoom/cuda/cuda_interop.hpp ~69–116)
  grows whatever minimal fields the kernel needs (e.g. an enum
  none/invert/lut plus the LUT generation counter). `displayColorMode`
  remains as a field only as long as the composite-kernel call site and
  status label still read it; the goal state is that the int is a
  persistence/migration artifact, not a kernel input.

### 3.2 LUT construction (app-side, pure function — unit-testable)

`BuildColorLut(stops, count, stepped) -> std::array<uchar4, 256>`:

- SMOOTH: piecewise-linear interpolation across the stops placed evenly on
  [0, 255] (stop 0 at luma 0, stop count-1 at luma 255).
- STEPPED: index `i` maps to `stops[min(count-1, i * count / 256)]` — hard
  bands, no blending.
- Output channel order must match the kernel's buffer layout: the pipeline
  buffers are BGRA (`uchar4` x=B, y=G, z=R — see the comment at
  cuda_kernels.cu ~738–739 and the existing float3 BGR constants at
  ~934–942). Getting R and B swapped is the second classic mistake; the
  golden-frame check catches it, but write a unit check anyway (e.g.
  yellow-on-black scheme: lut[255] must have R=G=255, B=0).

Accuracy note for the golden-frame gate: the old kernel lerps in float and
rounds once per pixel; the LUT quantizes luma to 8 bits first. The results
can differ by at most 1 LSB per channel. "Pixel-identical" for the
migration acceptance therefore means: max per-channel absolute delta ≤ 1
across the frame, and the report must state the measured max delta.

### 3.3 Built-in scheme table (ONE app-side table)

Create one table (suggested location: a small new
`src/app/color_schemes.cpp/hpp` or alongside the picker) that becomes the
single source of truth for scheme id, display name, stop list, stepped
flag, and the legacy `displayColorMode` int it migrates from. It replaces
BOTH existing tables — the Qt-side `DisplayColorOptions()`
(main_window.cpp ~82–104) and the kernel-side `ResolveTextColorScheme()`
(cuda_kernels.cu ~933–965), which is deleted once the LUT path lands.
CRITICAL: the two existing tables agree on colors but in different
encodings (QColor hex vs BGR float3 — e.g. amber #ffa600 ==
make_float3(0.0, 0.65, 1.0) BGR). The new table must reproduce the KERNEL
float values (converted exactly to bytes) for modes 2–16, because those
are what current frames are rendered with and the golden-frame gate
compares against rendered output, not against the combo's icon colors.
Verify each pair against ~933–965 during implementation.

Built-in "Effects" schemes shipped in the same table: Grayscale
(black→white, smooth), Yellow-tint gray (black→warm-yellow→white, smooth,
3 stops), Sepia (smooth stop table), Posterize-6 (6 stops, stepped).

### 3.4 Settings schema and migration (replace-with-back-compat)

- KEEP `displayColorMode` (int) in `AdvancedConfig` for migration: old
  settings files contain only the int, and on load it maps to the
  corresponding built-in stop table with identical visuals.
- ADD `colorScheme` to `AdvancedConfig`, persisted inside the existing
  config JSON object:
  `colorScheme { mode: "duotone"|"posterize"|"gradient", stops:
  ["#RRGGBB", ...] (2–8 entries), stepped: bool }`.
  Follow the file's established patterns exactly: serialize in
  `ConfigToJson` (settings_store.cpp ~262–318) as a nested object with a
  `QJsonArray` of color strings; parse tolerantly in `ConfigFromJson`
  (~320–384) with defaults when absent (absent → derive from
  `displayColorMode` via the built-in table — that IS the migration, and
  it means no settings `version` bump is required; the loader at ~545+ is
  already tolerant field-by-field). Clamp stop count into [2, 8]; reject
  unparseable colors by falling back to the derived scheme.
- Equivalence: extend `AreConfigsEquivalent` (~778–834) to compare the
  stop LISTS (count, order, exact bytes) and flags — not just the legacy
  int. Two configs with the same int but different custom stops are NOT
  equivalent.
- Preset apply/capture: mirror the new fields through
  `CaptureCurrentAdvancedConfig` (app.cpp ~1089–1147) and
  `ApplyAdvancedConfig` (~1294–1526) following the surrounding per-field
  pattern; presets must round-trip the full scheme.
- The custom scheme the user last built persists and reappears as its
  pencil-badged tile after restart (acceptance below).

## 4. Picker UI specification (implement AFTER the backend is green)

New files: `src/ui/color_scheme_picker.cpp` +
`include/openzoom/ui/color_scheme_picker.hpp` (mirror the tree layout, as
all modules do).

### 4.1 Trigger control (replaces the combo in the Display colors row)

A normal-height button in the "Display colors" row (row built at
main_window.cpp ~1415–1450) showing the CURRENT scheme as: a mini swatch
(rounded square painted with the scheme — for pair schemes: background
fill + bold letter "A" in the foreground color; for effects: a mini
strip), the scheme name, and a dropdown arrow. Click, Enter, or Space
opens the popover. The trigger's own swatch is the only persistent
"preview" — the camera view itself is the real preview (owner decision:
NO giant preview bars anywhere, ever again).

Keep `MainWindow` compiling for `OpenZoomApp`: either replace the
`displayColorCombo()` accessor (~2178) with a picker accessor and adapt
app.cpp (harvest at ~405, connect at ~700–703, apply at ~1447–1467), or
keep a hidden shim during the transition — but VERIFY the wiring: the app
side must end up connected to the picker's `schemeChanged(...)` signal
(carrying the scheme id / stop table), and `ApplyAdvancedConfig` must be
able to set the picker's state under a `QSignalBlocker` and then invoke
the handler manually exactly once, preserving the file's established
blocked-write + manual-call pattern.

### 4.2 Popover (compact, ~320 px wide — the mode-grid popup pattern)

Reuse the existing chrome-popup infrastructure rather than inventing one.
The reference implementation is the mode-grid popup:

- Window flags: `Qt::Tool | Qt::FramelessWindowHint |
  Qt::NoDropShadowWindowHint` (`chromeFlags`, main_window.cpp ~1713–1717 —
  the comment there explains WHY: D3D presents into a native window, so
  frameless owned tool windows are the only reliable way to stack above
  the swap chain).
- Construction pattern: `modeGridPopup_` (~1795–1800) + its stylesheet
  block (`QWidget#modeGridPopup`, ~970–992) — give the picker popover its
  own objectName and a matching stylesheet entry.
- Open/close behavior: `ToggleModeGrid()` (~2329–2360) — on open: show,
  raise, focus the content; on close: hide and RESTORE FOCUS to the
  trigger (`setFocus(Qt::PopupFocusReason)`).
- Esc-to-close: the eventFilter handles `Qt::Key_Escape` for the mode grid
  at ~2728–2741; extend the same filter (or install one) so Esc closes the
  picker and restores focus to the trigger.
- The popover NEVER fades. It participates in the chrome-pinned
  interaction path (the idle-fade logic must treat an open picker like an
  open mode grid: interacting chrome does not fade away under the user).

Popover contents, top to bottom:

1. **Reset row**: a "Reset" action with a slash icon = "Normal colors"
   (no remap), exactly like Sheets' Reset. Activating it selects mode 0
   and closes the popover.
2. **Swatch grid**: tiles 6 per row, 32×32 px, 4 px gaps. Each tile is a
   custom-painted `QToolButton`: rounded-rect filled with the BACKGROUND
   color, bold "A" glyph in the FOREGROUND color — instantly readable as a
   text scheme (better than Sheets' plain circles for this domain). Rows
   grouped:
   - Row 1 (dark backgrounds): White/black, Yellow/black, Cyan/black,
     Green/black, Amber/black, Magenta/black
   - Row 2 (light backgrounds): Black/white, Black/yellow, Black/cyan,
     Blue/white, Black/amber, DarkRed/white
   - The exact set = whatever `displayColorMode` currently supports (the
     17-mode table) + sensible completions; keep it to ≤ 3 rows. "Inverted
     colors" (mode 1) needs a tile too — paint it distinctly (e.g. the
     stripe motif the old delegate used for it), since it is not a pair.
   - Selected tile: 3 px highlight border + a small check overlay drawn in
     a guaranteed-contrast corner chip (Sheets-style check). Selection is
     NEVER indicated by color alone.
3. **Effects section**: a second labeled group with tiles for Grayscale,
   Yellow-tint gray, Sepia, Posterize-6. These tiles are painted as mini
   step/gradient STRIPS (diagonal) instead of the "A" swatch, so the
   mapping type is visually distinct from pair schemes at a glance.
4. **CUSTOM section** (like Sheets' Custom), now the full editor:
   - Mode selector: Two colors / Posterize / Gradient.
   - Stop-count spinner (2–8) with add/remove reflected in a row of
     per-stop color wells; each well opens `QColorDialog` (native dialog —
     it has an eyedropper on Windows).
   - Stepped/smooth toggle.
   - A live mini-strip preview INSIDE the editor (this is a tiny painted
     widget, not a video preview).
   - "Apply custom" commits the scheme. The custom scheme persists and
     renders as ONE extra tile at the end of the grid with a pencil badge.
   - In "Two colors" mode the two wells are labeled "Text" and
     "Background", preserving the original design's vocabulary.

**Owner decision — NO hover live-preview.** The camera view updates on
SELECTION ONLY. Hover-previewing color remaps on live video was considered
and explicitly rejected as disorienting for this audience. Do not add it,
not even behind a setting. (The tile's own painted appearance is the hover
feedback.)

### 4.3 What gets deleted

`ColorSchemeDelegate` (main_window.cpp ~106–159), the combo-based row
construction (~1418–1437), the `kColorForegroundRole`/`kColorBackgroundRole`
item roles (~73–74) and the `DisplayColorOptions()` table (~82–104, folded
into the single backend table of §3.3). Acceptance requires the old
bar-list UI to be FULLY gone — no dead delegate code left compiled.

## 5. Accessibility (non-negotiable — this app's entire audience)

- Every tile is a REAL focusable button (`QToolButton` with
  `setFocusPolicy(Qt::StrongFocus)`), not a painted region with mouse
  handling.
- Accessible names on everything, in speakable English: tiles as
  "Yellow text on black background", effects as "Grayscale effect",
  the custom tile as "Custom colors", stop wells as "Color stop 3 of 6"
  (updated live when stops are added/removed). Use the existing
  `setA11y` name+description helper pattern (main_window.cpp ~1899–1901)
  and the per-item `Qt::AccessibleTextRole` pattern.
- Arrow-key GRID navigation between tiles (reuse/extend the mode-grid key
  handling); Tab moves between sections (Reset → grid → effects → custom
  controls); Home/End optional but welcome.
- Tooltips on every tile with the same speakable name.
- Selection changes are ANNOUNCED via the existing
  `QAccessibleAnnouncementEvent` pattern — see `ShowModeAnnouncement()`
  (main_window.cpp ~2390–2409, announcement at ~2406–2408) for the exact
  incantation including politeness level.
- Minimum 32 px hit targets (the tile size already guarantees this; keep
  every other interactive element in the popover at ≥ 32 px too).
- Check + border mark selection (never color alone).
- The popover participates in the app-wide 3 px focus outline styles
  (main_window.cpp ~909–952); verify the outline is visible on tiles.
- Fully keyboard operable end-to-end: open with Enter/Space on the
  trigger, navigate with arrows, select with Enter/Space, close with Esc,
  focus returns to the trigger.

## 6. Implementation order (single agent pass, backend strictly first)

1. Backend: `BuildColorLut` + built-in scheme table + LUT upload path in
   `CudaInteropSurface` (constant symbol, async upload, generation cache)
   + kernel modes 2+ replaced by the LUT lookup (modes 0/1 branches kept).
   Old `displayColorMode` ints resolve to built-in stop tables. GOLDEN-
   FRAME CHECK HERE, before any UI work: every legacy mode 0–16 must
   render identically (≤ 1 LSB, §3.2) to the pre-change build.
2. Settings: new `colorScheme` fields + migration + equivalence + preset
   apply/capture round-trip (unit-test the JSON round-trip if the plan-11
   Wave-4 test scaffolding already exists; otherwise at minimum a manual
   save/load/verify pass).
3. Picker UI: `color_scheme_picker.cpp/hpp` — trigger button + popover +
   painted tiles + effects strips + custom editor, wired per §4.1.
4. Replace the combo in the row; delete the delegate/bar machinery;
   adapt app.cpp wiring and `ApplyAdvancedConfig`.
5. Accessibility pass per §5.
6. Build green; CHANGELOG [Unreleased] + docs/code_reference.md entries
   for the new class(es) and table; screenshot of the open popover in the
   batch report for owner approval.

## 7. Acceptance checklist

- [ ] Popover: ≤ 3 grid rows + effects section + custom section, ~320 px
      wide, 32×32 tiles, 6 per row.
- [ ] Opens/closes cleanly with keyboard only; Esc closes; focus returns
      to the trigger; popover never fades.
- [ ] Posterize-6, Grayscale, and Yellow-tinted grayscale are each
      reproducible via the Custom editor in ≤ 6 interactions (count
      clicks/keystrokes from popover-open; report the sequences used).
- [ ] Old settings files (every legacy `displayColorMode` 0–16) load with
      identical visuals — golden-frame comparison, max per-channel delta
      ≤ 1, measured delta stated in the report.
- [ ] The custom scheme survives an app restart and reappears as the
      pencil-badged tile with its exact stops.
- [ ] Camera view updates on selection ONLY (no hover preview — owner
      decision).
- [ ] The old bar-list UI (delegate + oversized combo) is fully gone.
- [ ] LUT uploads occur only on scheme change (verify: log or breakpoint
      the upload; drag contrast slider — no uploads; switch schemes — one
      upload).
- [ ] Text-clarity two-color mode colors text with stops[0]/[count-1] of
      the active scheme.
- [ ] A11y verified with a screen reader (Narrator or NVDA): tile names,
      stop-well names, selection announcements.
- [ ] Screenshot of the open popover included in the report for owner
      approval.
