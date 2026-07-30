# ui module

This module owns the Qt widget layer.

Current contents:
- `MainWindow` for the persistent render surface, three auto-fading Simple-mode
  corner clusters, the shared Simple/Advanced quick-mode carousel and numbered
  grid/toast, and right-side Advanced Image and Assistant tabs with section
  arrows, labeled top-level AI Settings, top-packed collapsible tuning,
  SuperRes source/target status plus compact 2x/performance controls,
  diagnostics, and Chat/History workflows. The
  inspector uses a persistent draggable splitter, responsive slider rows, and
  width-constrained wrapping status text. Selectors and sliders ignore wheel
  edits so wheel motion continues to the scroll panel
- `ColorSchemePicker` for the compact accessible reading-color/effects grid and
  persistent custom 2-8 stop gradient/posterize editor
- `WheelSafeComboBox` and `WheelSafeSlider` for settings controls that remain
  click/drag/keyboard editable without intercepting panel scrolling
- `SetLiveText` and `SetLiveTextCoalesced` for UI-thread-only dynamic text,
  role-qualified accessible names, change events, severity-aware
  announcements, deduplication, and diagnostic coalescing
- `AiSettingsDialog` for Codex subscription or OpenAI-compatible provider,
  dynamically discovered model/reasoning choices, a visible read-only built-in
  Codex prompt, shared language/behavior instructions, Advanced Assistant
  permissions, separately grouped VLM/OCR/speech/notes settings, installed
  Windows voice/speed selection, and manual speech preview. Its content
  scrolls independently from the fixed confirmation buttons
- `RenderWidget` for native D3D12 presentation
- `AssistiveOverlay`, an owned floating Assistant that uses native move/resize
  handling without letting streamed text reapply its geometry, persists its
  camera-relative position and size, initially clears the top controls, plus
  screen-reader streaming text, a follow-up question field, manual Read Aloud,
  high-contrast Close, and Escape dismissal
- `JoystickOverlay` for on-canvas panning input
- `ResponsiveSliderRow` for keeping labels and complete slider tracks usable
  while the Advanced splitter changes width

The UI layout and settings-ownership contract is recorded in
[`docs/ui_modes_design.md`](../../docs/ui_modes_design.md).

Input routing from the window into app-level behavior stays split between this module and `src/app/interaction_controller.cpp`.
