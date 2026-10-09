# UI Redesign Spec — "Option 7 Plus" (agreed 2026-07-21, refined 2026-07-22)

> **Status: implemented and review-verified, 2026-07-22.** Kept as the design contract; hotkeys have since been extended to `1`–`9`.

Decision from reviewing `ui ideas/ui-option-01..15.png`: **option 7 is the base**
(full-bleed video, floating corner clusters), upgraded with the strongest ideas
from options 9, 13, and 14. This file is the contract for the implementing
agent; the reviewing agent will check the implementation against it point by
point.

## Layout (Simple mode)

Full-bleed render widget fills the window. Three floating clusters parented to
the render widget (same child-widget + `resizeEvent` positioning pattern
already used by `AssistiveOverlay` / `JoystickOverlay`):

1. **Top-left:** app logo + `Simple | Advanced` segmented toggle.
2. **Bottom-left:** mode carousel — [Modes grid button] [◀] [current mode name,
   large] [▶].
3. **Bottom-right:** action cluster — Photo, Record, Explain, Read (icon +
   label, ≥48 px tall).

The former top-right processing pill was removed after use testing because it
covered camera content. The same state is available under Advanced Image
diagnostics.

Requirements:
- Clusters sit **flush to the window corners/edges** (no floating margin) so
  corners act as infinite Fitts targets.
- **Solid** dark backgrounds (no translucency) with a ≥3 px high-contrast
  border so they never melt into the video behind them.
- **Auto-fade after ~5 s of inactivity** (opacity animation); any mouse move,
  key press, or focus change restores them instantly. While any cluster has
  keyboard focus, no fading. `Ctrl+H` toggles whether chrome stays pinned.
- All controls remain real Qt widgets with the existing accessible
  names/descriptions; logical tab order: mode toggle → carousel → actions.

## Mode switching

- Carousel ◀/▶ cycles presets (wraps around).
- **Hotkeys `1`–`7`** select modes directly (QShortcut); the grid popup shows
  the number badge on each tile (idea from option 14).
- **Modes grid button** opens a centered overlay of large tiles (2-column,
  option 4/13 style) with icon + number + plain-language label; Esc closes.
- **Plain-language labels in Simple mode** (idea from option 13):
  Reading → "Read a Page", High Contrast → "High Contrast", Steady Text →
  "Keep It Steady", Sharp Text → "Sharpen Text", Large Zoom → "Zoom In
  More", Low Light → "See in Low Light".
- Use a speaker icon for the `Read` action so it is visually distinct from photo
  capture and communicates that recognized text can be spoken aloud.
  (Advanced mode keeps the technical names.)
- Use the bundled Lucide camera, record-circle, question-message, and speaker
  icons for the four immediate actions rather than unrelated platform icons.
- On every mode change: **center-screen toast** with the mode name in very
  large type (~1 s, fading; reuse the AssistiveOverlay pattern) plus an
  assertive screen-reader announcement. Do not start text-to-speech.

## Advanced mode

Switching the top-left toggle to Advanced shows the existing full control set
(current advanced page/inspector). Simple-mode lower clusters hide while
Advanced is open, leaving only the top-left mode switch. The Advanced tab bar
adds wrapping previous/next arrows and a pop-out AI Settings button; processing
status appears beside the Image diagnostics.

## Explicitly rejected (do not implement)

- Radial/pie mode menu (option 10) — no reading order, poor screen-reader fit.
- Left/right docked sidebars in Simple mode (options 3/12) — they steal width
  from 16:9 video.
- Auto-appearing context suggestions (option 15) — unpredictable UI; may
  return later as an opt-in.

## Review checklist (for the reviewing agent)

- [ ] Video is truly full-bleed in Simple mode; zero permanent chrome bands.
- [ ] Clusters flush to corners; solid backgrounds; visible border.
- [ ] Auto-fade works and is interrupted by mouse/key/focus; no fade while
      focused; chrome-toggle shortcut exists.
- [ ] `1`–`7` hotkeys work; grid popup shows number badges.
- [ ] Plain-language labels in Simple, technical names in Advanced.
- [ ] Mode-change toast + screen-reader announcement fire on carousel, hotkey,
      and grid selection alike without starting text-to-speech.
- [ ] All new controls have accessible names/descriptions and sane tab order.
- [ ] Existing MainWindow accessors still return live widgets (app.cpp wiring
      unbroken); settings persistence (simpleUiMode, presets) still works.
- [ ] Build green via `cmake --preset msvc-release` + msvc-release-build.
