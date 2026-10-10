# ui public headers

Public Qt UI interfaces live here.

Currently exported:
- `main_window.hpp`: auto-fading Simple-mode corner controls, mode
  announcements, deliberate quick-mode activation, explicit Explain/Stop state,
  and the right-side Advanced Image/Assistant/Transcript/Settings inspector,
  including scoped search and compact SuperRes factor/performance controls
  and state-preserving `setUiHidden` / `isUiHidden` controls
- `render_widget.hpp`: native D3D12 surface with coalesced native-pixel resize
- `assistive_overlay.hpp`: movable/resizable streaming result, follow-up,
  prominent manual Read Aloud, secondary New Conversation, compact position
  menu, Close, left/right docking with drag previews, larger default floating
  geometry, saved-geometry persistence, `SetSafeArea` default-placement hint,
  and `SetUiSuppressed` without losing streamed content
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
