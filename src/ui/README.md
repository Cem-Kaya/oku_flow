# ui module

This module owns the Qt widget layer.

Floating chrome remains in native tool windows above both CPU and D3D12 render
surfaces. The owner's native `WM_MOVE` repositions these peers before Qt's
queued geometry event, translating their current positions by the viewport-origin
delta without
translating or refitting layouts on each drag step. Qt owner/render-origin events
also reconcile placement. Size or device-pixel-ratio changes run the full fit;
the native path converts the current Windows client origin around the matching
monitor origin into Qt logical coordinates, avoiding Qt's stale position at 1×.
At other scales Qt already maps from the current render HWND, so the fast path
keeps that native rounding. The 1× fallback retains the render-to-owner offset
from completed global geometry, including native RTL placement.
Moving actual peer positions preserves any native pixel rounding already applied
to RTL/right-edge controls; a move does not restore a pre-rounding panel offset.
For pure native moves, `WM_WINDOWPOSCHANGING` snapshots the current viewport
origin before Windows moves the owner. This avoids treating a late child-origin
rounding adjustment after fitting as part of the first owner displacement.
RTL, responsive strips, opacity, visibility and focus retain their existing
contracts. Assistant and annotation overlays retain their own anchor handling.
The serial native GUI regression checks immediate Qt and HWND displacement
without draining deferred events, plus resize refitting. This verifies geometry,
not a compositor frame rate or multi-monitor hardware acceptance.

Inspector text follows the Windows light/dark palette, including expanded
section headers, current quick mode, subtitles, disabled labels and placeholders.
The tab panes and all three inspector pages explicitly paint the palette's
Window surface. Native styles that otherwise retain a white pane cannot leave
white dark-theme labels on that white background, including after a scheme switch.
`theme_contrast.hpp` supplies paired colors, contrast calculations, a complete
dark palette for the color editor, and surface-colored rasterized SVG icons.
`MainWindow::ApplyThemeContrast` refreshes these on palette/scheme changes;
floating chrome retains its deliberately dark surfaces. Actual native light,
dark and runtime theme-switch acceptance remains part of the serial GUI gate.

Settings > Camera provides a phone-only light button whose label follows capture
metadata. The light starts off each connection and is never a saved preference.

Current contents:
- `MainWindow` for the persistent render surface, three auto-fading Simple-mode
  corner clusters, the shared Simple/Advanced quick-mode carousel and numbered
  grid/toast, and right-side Advanced Image, Assistant, and Settings
  tabs. Image owns mode tuning and Settings owns shared setup, with independent
  search fields and cross-tab match feedback. Fine-tune text keeps detailed
  algorithms inside a nested group. The inspector also provides
  SuperRes source/target status plus compact 2x/performance controls,
  diagnostics, and Chat/History workflows. The
  inspector uses a persistent draggable splitter, responsive slider rows, and
  width-constrained wrapping status text. Selectors and sliders ignore wheel
  edits so wheel motion continues to the scroll panel
- `ColorSchemePicker` for the compact accessible reading-color/effects grid and
  persistent custom 2-8 stop gradient/posterize editor
- `WheelSafeComboBox` and `WheelSafeSlider` for settings controls that remain
  click/drag/keyboard editable without intercepting panel scrolling. Long combo
  entries elide in the middle and expose their complete name on hover.
- Camera-state placeholders, separate carousel shortcut badges, localized
  slider readouts, and visible Advanced search results. Owned dialogs are
  raised above chrome; tab arrows appear only for five or more sections.
- Deliberate quick-mode activation through `quickModeActivated`, explicit
  Explain/Stop state through `setExplainBusy`, modal-safe keyboard shortcuts,
  and F6/Shift+F6 navigation between control regions.
- A persistent Hide UI/Show UI control and Ctrl+H temporarily hide controls,
  assistant, and drawing while retaining their state. Mouse-focused buttons
  no longer block Simple-mode idle fading; keyboard navigation still does.
- `SetLiveText` and `SetLiveTextCoalesced` for UI-thread-only dynamic text,
  role-qualified accessible names, label text/name invalidation events,
  severity-aware announcements, deduplication, and diagnostic coalescing
- `TranslateUi`, `SetLiveTranslationSource`, and
  `RetranslateWidgetTree` for live switching among all 25 UI languages without
  reconstructing hand-built widgets; data-driven combo entries opt out. The
  app locale also controls layout direction, while manual Simple chrome and
  annotation rails use logical leading/trailing anchors so an English RTL
  diagnostic run exercises the same path as a future RTL catalog
- `AiSettingsDialog` for Codex subscription or OpenAI-compatible provider,
  dynamically discovered model/reasoning choices, a visible read-only built-in
  Codex prompt, shared language/behavior instructions, Advanced Assistant
  permissions, separately grouped VLM/speech/notes settings, installed
  Windows voice/speed selection, and manual speech preview. Its content
  scrolls independently from the fixed confirmation buttons
- `RenderWidget` for native D3D12 presentation
- Draw keyboard routing distinguishes filtered propagation from explicit owner
  forwarding. Inactive/hidden Draw leaves owner shortcuts alone; inline text
  entry owns navigation. Forwarded held pan arrows are released on hide,
  Draw-off, focus loss or window deactivation. Offscreen routing regressions
  supplement the deferred native focus gate.
- `AnnotationOverlay` for scene-anchored drawing, selection, text entry, and
  annotation capture. Its native Windows hit test uses physical panel
  rectangles so persistent camera actions remain clickable at non-100% DPI
- `AnnotationOverlay::SetExcludedWidget` tracks the Assistant's visibility,
  movement, and resize in a native window mask and paint clip, so ink and Draw
  controls cannot cover or intercept the floating chat. Viewport-ancestor
  movement also updates the canvas when docking shifts the central layout
- `AssistiveOverlay`, a `QDockWidget` with Floating/Dock left/Dock right choices
  that reserve space beside the camera when docked. Floating mode uses native move/resize
  handling without letting streamed text reapply its geometry, persists its
  camera-relative position and size separately from its persisted dock side,
  initially clears the top controls and uses 43% viewport width and 75% height
  (bounded to available space). MainWindow passes a camera-relative `SetSafeArea`
  hint so untouched defaults clear live chrome; saved or user-moved geometry
  takes precedence, and the layout update has a recursion guard. Temporary
  `SetUiSuppressed` hides streamed results without dismissing the answer. The
  panel uses 20-point result text at 135% line spacing, a prominent manual
  Read Aloud footer action, a secondary New Conversation action, a compact
  Panel position menu, a follow-up question field, and high-contrast Close.
  The focused question field handles unmodified Escape directly: it clears a
  nonempty draft or returns focus to the camera, while an active drag cancels
  first and an open placement menu retains its own Escape behavior
- Floating Assistant header drags also dock on release at either app edge.
  A translucent, high-contrast owned tool window highlights the target above
  native camera/Draw surfaces without intercepting input or taking focus.
  Windows move-loop notifications commit/cancel the drop; resizing and ordinary
  placement updates do not trigger docking
- Assistant header dragging uses one controller for both floating and docked
  states. A docked header needs a 32-pixel pull held for 350 ms before release;
  returning, releasing early, or Escape cancels it. Preview acquisition/release
  thresholds differ (36/96 logical pixels), with a debounced latch (180 ms to
  acquire, 250 ms to release). Moves that keep the same target do not restart
  its timer. The just-released side must be left before it can re-arm. Qt's
  automatic dock dragging is disabled
- Presenter resizing uses a 16 ms throttle that is not restarted by subsequent
  resize events, so continuous splitter motion cannot starve back-buffer updates
- `JoystickOverlay` for on-canvas panning input
- `ResponsiveSliderRow` for keeping labels and complete slider tracks usable
  while the Advanced splitter changes width

The UI layout and settings-ownership contract is recorded in
[`docs/ui_modes_design.md`](../../docs/ui_modes_design.md).

Input routing from the window into app-level behavior stays split between this module and `src/app/interaction_controller.cpp`.

`MainWindow::refreshTextClarityUi()` gates Fine-tune text content from the Text Clarity master without clearing stored refinement values. Call it after signal-blocked settings restoration.

The Draw inline text editor retires pending entry on actual FocusOut,
including unchanged empty input. The guarded commit path inserts typed text
once and cancels empty input, then releases keyboard ownership to the camera.
