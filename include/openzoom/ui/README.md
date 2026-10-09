# ui public headers

Public Qt UI interfaces live here.

Currently exported:
- `main_window.hpp`: auto-fading Simple-mode corner controls, mode
  announcements, and the right-side Advanced Image/Assistant inspector,
  including compact SuperRes factor and performance-guard controls
- `render_widget.hpp`: native D3D12 surface with coalesced native-pixel resize
- `assistive_overlay.hpp`: movable/resizable streaming result, follow-up,
  manual Read Aloud, Close, left/right docking with drag previews, and floating-geometry persistence
- `annotation_overlay.hpp`: scene-anchored Draw tools with live native input
  and paint exclusion for the Assistant panel
- `joystick_overlay.hpp`: on-canvas panning input
- `responsive_slider_row.hpp`: narrow-panel label/slider reflow
- `color_scheme_picker.hpp`: accessible reading-color swatches and custom editor
- `wheel_safe_combo_box.hpp`: selectors and sliders that do not consume panel
  wheel scrolling
- `live_status_text.hpp`: synchronized dynamic widget text and accessibility
  names with silent/polite/assertive announcement policy and trailing-edge
  coalescing
- `ai_settings_dialog.hpp`: scrollable, sectioned provider settings with a
  visible built-in Codex prompt, dynamic model/reasoning catalog,
  permissions/workspace, VLM prompt, installed Windows voice/speed,
  manual speech preview, and notes settings

See [`docs/ui_modes_design.md`](../../../docs/ui_modes_design.md) for the layout
and global-versus-profile settings contract.
