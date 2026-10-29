# Magnifier UI cleanup — design findings

Reviewed read-only at `cf7eaac3315bfaab93fcfdb5c310a77af79cfd52`. No implementation blocker. Decisions below are settled for the parent; do not ask the owner to pick among them.

Evidence: three desktop screenshots (other apps and a pointer are visible, so these are not Qt-offscreen grabs) plus the cited sources. They are not screen-reader, contrast-meter, or native-UIA evidence. The UCT crop starts at Save / Reset, so the experimental checkbox above Show original was not in frame. No app was launched.

## What the screenshots show

Image tab, Text Clarity area (`ultra_clear_text_panel.cpp`, `UltraClearTextPanel::UltraClearTextPanel`): Show original sits above a collapsed `uctAdvancedSettings` tool button. Under that, still outside the disclosure, are `ultraClearTextBackend` (“Using GPU backend: PaddleOCR / TensorRT CUDA”), `ultraClearTextStatus` (“Showing sharp text for 12 words.”), and the Recognized text pane. `modelControls` is the only widget the toggle shows. The pane’s 96–220 DIP minimum then pushes Steady image, Straighten screen, Sharpness, Assistant behavior, and Diagnostics down. That matches the complaint.

Transport (`video_transport.cpp`, `VideoTransport::VideoTransport` / `UpdateLayout`): `videoAudioRow` is a full-width third band — “Playback volume”, `videoVolumeSlider` near 16%, and `videoVolumeValue`. It is visible because `UpdateActions` shows it whenever `!collapsed_ && muteShown_` (saved video with audio). The speaker is already on the control row, 44 DIP (`kButtonExtent`), with a focus ring and normal ink, so the row is a second volume control that steals image height. `docs/ui_modes_design.md` says the transport must not permanently eat the picture.

Preset grid (`main_window.cpp`, `PlainPresetLabel`, `kSimpleShortcutCount = 9`): ten stock tiles. Rows 0–8 are numbered Read a Page through Whiteboard. Document is the tenth tile and has no badge. The carousel says Custom Setup while Read a Page is only highlighted: `gridBrowsing_` keeps the applied mode until `ActivatePresetRow`. Do not treat that as a stock preset or “fix” it.

## Four featured modes, unchanged blobs

Keep every built-in id, config blob, and custom preset. `SettingsController::MatchPreset` and `ResolvePreset` must still resolve demoted ids. `app_control_target.cpp` must still list every built-in for `mode.activate`. Do not rewrite `BuiltInConfigsStorage`, rename labels (the plain names are already in the 24 catalogs), or change `DefaultPresetId()`. A blob edit would retune every saved `selectedPresetId` on next launch.

`PopulatePresetList` order for the grid and for number keys:

1. `preset-reading` → `preset-reading-config`. Plain name Read a Page. Zoom 1.6, temporal alpha 0.25, NIS sharpen 0.25 (`spatialUpscaler` 1). Text Clarity, keystone, stabilization, and VLM are off. This stays the new-install mode.
2. `preset-document` → `preset-document-config`. Document. Zoom 1.6 and `autoTextClarityEnabled`, flatten 0.85, adaptive binarization, Sauvola 0.28, smart sharpen 0.42, two-color, hysteresis, selective sharpen, focus detection, glare 0.45. No spatial NIS, no temporal, no keystone.
3. `preset-projector` → `preset-projector-config`. Projector Screen. Zoom 1.4, temporal 0.25, keystone, auto contrast 0.7, stabilization. No sharpen.
4. `preset-whiteboard` → `preset-whiteboard-config`. Whiteboard. Zoom 1.5, NIS 0.35, keystone, auto contrast 0.85, stabilization. No temporal.

Then saved custom presets, in their existing order. Keys 1–4 are the four above. Keys 5–9 apply the next custom rows only, and only in Simple mode with no text focus (`MainWindow` key filter). Demoted stock ids get no number.

A “More modes” disclosure under the grid, same checkable header pattern as the UCT tool button, holds the other six. It is not a preset row. If `selectedPresetId` is one of those six, the carousel keeps that plain name and More opens with that row selected. Switching away does not delete it.

## Why the other six are not modes

They stay reachable. They are the wrong front door.

- `preset-high-contrast`: global threshold 0.62, zoom 1.5, temporal 0.18, sharpen off. It does not touch `displayColorMode`, contrast, or brightness. The name collides with display palettes. Document is the text-contrast tool.
- `preset-steady-text` (Keep It Steady): temporal 0.45 only. `stabilizationEnabled` is false. Projector and Whiteboard are the presets that actually stabilize. Extra Stable is a separate checkbox, not a preset field. The stored description already says “more smoothing”; the big label overclaims. Demote rather than rename in this pass.
- `preset-sharp-text`: NIS 0.45, zoom 1.55, temporal off. Read a Page is the same NIS backend at 0.25 with smoothing. Strength belongs on the Advanced Sharpness slider. No stock preset selects FSR (`spatialUpscaler` 0) or `mlSuperResEnabled`.
- `preset-large-zoom` (Zoom In More): one presentation action, zoom 2.3, plus `focusMarker`, and it turns sharpen and temporal off. Wheel zoom does not reclassify the mode (`ui_modes_design.md`). Featuring this makes “zoom” discard other processing.
- `preset-low-light`: blur sigma 1.6, radius 3, temporal 0.35, zoom 1.35. `brightness` stays 0. It softens text. It is the blur control, not a brighter picture.
- `preset-scene-explain` (Describe the Scene): `vlmAssistEnabled` and overlay on, zoom 1.25, temporal 0.22. The stored description says it is reserved for future summaries. Applying it calls `OnVlmAssistToggled` → `AssistiveFeatureManager::SetModes` → `SetPeriodicAnalysisEnabled`. That function does not itself send a request; it arms periodic analysis. Explain on the corner is the one-shot. Do not feature this, and do not clear the flag for someone who already saved it.

None of these are duplicates of each other on paper. They are misleading as peer “modes”, and their CUDA-only pieces do nothing on the raw passthrough path.

## Defaults

Missing settings already call `SettingsController::InitializeDefaults`, which selects `preset-reading` and copies that config. Keep that. Also already correct and must stay: `simpleUiMode` true, `ultraClearTextEnabled` false, UCT tiers GPU Medium/Medium and CPU Tiny/Small (`independentModelSelectorsKeepActivationSessionOnly`), Show original session-only, language preference untouched, no startup inference, no accuracy gate, no BF16 or worker-policy change.

`ResetCurrentConfigToDefaults` applies an empty `AdvancedConfig` (zoom off, clarity off) and clears `selectedPresetId`. That is Reset, not the new-install path. Leave it.

Playback gain stays session-only at 100 (`video_audio_output` starts at 100 and survives seek and document close). Do not write volume or mute into `settings.json`.

## Front versus disclosure

Front, both modes: the picture; the corner carousel with the four modes; Photo, Record, Explain, Draw; the Text Clarity switch; the existing transport row (play, timeline, speaker, save frame). Explain and Read stay actions. Zoom stays a gesture plus the Advanced Zoom group.

Behind disclosure: the six older stock presets; UCT model tiers, backend line, status line, and recognized-text pane; numeric clarity, sharpen, blur, and stabilization sliders, as they already are.

UCT move: put `backendLabel_`, `statusLabel_`, the “Recognized text” label, and `readingPane_` inside the advanced body, under the four combos. Leave the enable checkbox and Show original outside. Default remains collapsed (`uctAdvancedSettings` starts unchecked). Do not auto-expand. Do not persist a new section key.

`UltraClearTextController::UpdateStatus` passes `important=true` for unavailable language, recognition failure, and Extra Stable pause. Those strings move inside with the rest. While the section is collapsed, keep the visible button label “UCT advanced settings” and put the current important status in its accessible description, announced once, politely. When the status is ordinary, restore a static description that the section contains model size, backend, status, and recognized text. `SetLiveText` must keep that description in sync. Hidden pane text stays available to the accessibility tree once expanded; do not invent a second always-visible status line.

`panelStatusUpdatesCachedAccessibleTextImmediately` asserts the backend label is not hidden after the enable checkbox alone. Qt’s `isHidden()` is true when the new parent is collapsed. Update that test to expand the disclosure first. Do not keep the label outside to satisfy the old assertion.

## Volume popover

Remove `videoAudioRow` from the layout. The slider lives in a popup opened above `videoMuteButton`, centered on that button, with a small gap so the second click still hits the button. Opaque fill and a 3 px border, same idea as the corner chrome. A short percent readout inside the popup replaces the full-width “16%” label. No permanent band.

Settled clicks: if the popup is closed, click, Space, or Enter opens it and does not change mute, including when already muted. Focus stays on the speaker so the next click toggles. If the popup is open, click, Space, or Enter toggles mute and leaves the popup open. Tabbing onto the speaker does not open it. Tab from the speaker reaches the slider only while open; closed, the slider is not a tab stop.

Gain and mute stay separate signals (`app_video_playback.cpp`). Remember the last nonzero in the transport, session-only, updated whenever the snapshot volume is above zero, initial 100. Mute toggle does not move a nonzero thumb. Drag or key to zero emits `VolumeChanged(0)` and then `MutedChanged(true)`, and does not forget the previous nonzero. Drag or key to N > 0 emits `VolumeChanged(N)` and then `MutedChanged(false)`; N becomes the remembered level. Do not snap back to an older level. If a toggle unmutes while the thumb is at zero, emit the remembered volume first, then unmute, so `PollVideoPlayback` never unmutes into silence. Emit mute only on a real edge, not on every repeat. Up and Right increase; the top of the vertical slider is 100. Steps stay 5 and 10. Home and End are 0 and 100. Wheel adjusts volume only while the popup is open.

Escape closes and focuses the speaker. Do not call `VideoTransport::ReturnFocus`; that parks focus on the magnifier. A click outside the popup and the speaker closes it. If that click focused another control, leave focus there. If it hit non-focusable video, focus the speaker. A slider drag that releases outside still commits. Collapse, hide-UI, or `hasAudio` false closes the popup; `MoveFocusFrom` already runs when the speaker hides. Recording review stays force-muted and the speaker stays hidden (`MutedChanged` handler). Recap has no audio control.

Icon: keep `ApplyFace`. Muted uses `theme_contrast::WarningTextColor` plus the slash; audible uses `QPalette::ButtonText`. The existing `mutedSpeakerUsesYellowAndRestoresThemeInk` test stays. Do not paint the slider yellow. The popup handle must be at least 44 DIP. The current horizontal handle is 18×28 and the stylesheet only skins `QSlider::groove:horizontal`, so add a vertical rule rather than copying the short handle. Drag ticks stay silent; a mute edge announces “Mute playback” or “Unmute playback” plus the percent, reusing those catalog strings. Locale percent stays `RefreshVolumeText`. Anchor to the button so RTL does not pin the popup to the wrong corner.

`playbackVolumeIsAccessibleAndIndependentOfMuteAndSeek` currently requires the slider to be visible and a Left step from 50 not to mute. Keep the nonzero step independent. Change the visibility expectation: hidden until opened, hidden when collapsed or when there is no audio.

## Presets versus the GPU

`docs/gpu_video_playback.md`: media uses the same `BuildProcessingSettings` path as the camera (blur, spatial and smart sharpen, Text Clarity, auto contrast, stabilization, keystone, temporal, reading colors, Super Resolution). Presentation owns zoom, so in-CUDA zoom is off. A CPU-decoded file still runs those CUDA effects when the adapter is healthy. Decoder fallback is not “presets do nothing.”

The raw passthrough path is the other case. `UpdateProcessingStatusLabel` shows GPU Required and unprocessed video when the interop surface is down (`docs/developer_guide.md`). On that path, threshold, blur, NIS, temporal, keystone, stabilization, and Text Clarity do not change pixels. Zoom In More’s 2.3 still changes framing. UCT is not a preset; it is the shared opt-in, off by default, with its own CPU `Windows.Media.Ocr` backend. The screenshot’s TensorRT line is the GPU backend and is exactly the label that should sit inside the disclosure.

## Small change list

1. `UltraClearTextPanel` constructor: parent the backend label, status label, pane label, and reading pane under the advanced body. Extend the toggle to that body. Important-status description on the tool button only while collapsed. No tier, BF16, or worker edits.
2. `PopulatePresetList` and the grid’s number badges: featured four, then custom, then More for the six demoted ids. `ActivatePresetRow` / keys 1–9 follow that visible order. Carousel still resolves a demoted current id. Agent catalog and config blobs unchanged.
3. `VideoTransport`: delete the audio row; add the vertical popup and the gesture rules above. App mute/volume handlers stay; recording force-mute stays.
4. Tests named above, plus any GUI shot that assumes ten numbered tiles or a visible `videoAudioRow`. Help sentence that says “number keys 1 through 9” (`translation_catalog.cpp` and the 24 catalogs; one source-string edit, full parity). `docs/ui_modes_design.md`, `docs/README.md`, `docs/developer_guide.md`, and `docs/code_reference.md` where they promise nine stock shortcuts. `docs/video_transport.md` for the popup.
5. Do not reset settings, drop custom presets, migrate ids, enable UCT or periodic VLM, add an accuracy gate, or restyle unrelated chrome.

Accessible names stay tied to visible text through `SetLiveText` / `NameButton`. New copy is limited to the UCT button description and, if needed, “More modes”. Both need the 24 catalogs. High contrast and 44 DIP targets stay on the speaker, the popup handle, and the mode tiles.
