# Video transport

This development UI places `VideoTransport` in a vertical view column directly
below `RenderWidget`, alongside the Advanced inspector. It spans the render
column's width. The native corner-control windows remain inside the render
rectangle, so they cannot cover the timeline. Plain unrecorded camera use has
no transport and supplies no camera-history collection.

Saved-video selection lives in Settings > Camera. The camera dropdown includes
`View video`; choosing it opens the local file picker. While video is selected,
`Select video` appears directly beneath the dropdown. Orientation stays available
for the video; camera-only format controls and the phone light are hidden.
Camera discovery preserves this selection. Choosing a physical/phone camera
leaves file review, and canceling the picker keeps the previous viewed source.
There is no Open video action in the corner toolbar or Notes and files.
`Ctrl+O` still works on OkuFlow's main/owned control surfaces. A single local file URL may
also be dropped on the view or its owned chrome. Drag handling inspects MIME
data only and commits a CopyAction; asynchronous format validation and opening
belong to the application. Opening a saved file stops/finalizes a current
recording first. It does not start a recording.

The bar uses source text, a playback status, six 52-DIP minimum buttons, and a
separate full-width timeline row. The buttons reflow into two rows at narrow
widths. Full source names remain in accessible names and tooltips when the
visible label elides. Time readouts retain hours for long recordings and use
the active locale's digits. The displayed selected time can exceed the verified
seekable range while a live scene is pending; the slider never exceeds it.

`>>` requests Jump to live for the selected ongoing recording, and Jump to end
for a selected saved video. It never changes speed. Resume continues from the
held position. End of video disables Resume until the user seeks away. Playback
mute is independent of microphone capture. Return to camera and Save frame
are explicit semantic actions; the application controls their availability.
Stopping recording preserves the current review position as the same session
becomes saved media.

## Application contract

`SetSnapshot(Snapshot)` updates presentation only and emits no playback
commands. The application supplies source, state, millisecond position and
verified seekable end, playing/mute/audio flags, action availability, the full
source label and a document `sourceGeneration`. `Source::None` hides the bar.
Changing source or generation cancels an unfinished drag. `SetUiHidden` retains
the snapshot while hiding the bar; MainWindow returns transport focus to the
viewport when the user hides the UI.

`SeekRequested(qint64)`, `PlayPauseRequested`, `JumpToEndRequested`,
`ReturnToCameraRequested`, `SaveFrameRequested` and `MutedChanged(bool)` are
semantic requests. Only the application may acknowledge new frames, Live,
End, opening/seeking/waiting, finalization or unavailable states. The transport
owns no decoder, recording, timer-driven playback or file handle.

The QSlider's numeric range is actual milliseconds, with a one-second step and
ten-second page step. The Qt integer range admits up to approximately 24.8 days;
longer media endpoints clamp to `INT_MAX` milliseconds in this control. During
a drag both maximum and mapping stay fixed. Moving the thumb changes the local
readout only; releasing commits the latest absolute target exactly once.
Snapshots do not start decode work, and source replacement discards a pending
drag. During playback autonomous slider updates occur at most once a second
and pause while the slider has keyboard focus.

## Keyboard and accessibility

Tab order follows the visible transport actions, then the timeline. MainWindow
bridges it with the existing corner, inspector and annotation regions. F6 moves
between those regions and the transport. Space toggles playback only in the
viewport or timeline; buttons retain native Space activation, and text editors
retain their own input. Left/Right, Page Up/Down, Home and End seek only when the
timeline owns focus. Timeline wheel events do not seek.

Buttons retain native button roles. The timeline supplies a native Slider role,
numeric value/minimum/maximum/step through QAccessibleValueInterface and a
human-readable timestamp as accessible Value text. State/source changes use
polite announcements; elapsed playback changes are silent. The implementation
does not claim native Windows UI Automation, NVDA, Magnifier, mixed-monitor or
rendered localization acceptance from offscreen fixtures.

## Translation source inventory

All strings use the shared `OkuFlow` context. Filenames are data, not translated
copy. The following list contains every new/changed transport/main-window source
string; existing keys may already be present in the catalogs.

| Source | Context |
| --- | --- |
| `Current recording` | Fallback source label |
| `Video` | Saved-video source/state |
| `Live` | Fresh camera-following state |
| `Behind live` | Recorded playback behind the current camera |
| `Paused` | Held playback state |
| `End of video` | Saved-file endpoint |
| `Opening video…` | Asynchronous file opening |
| `Seeking…` | Pending selected-frame seek |
| `Waiting for recording` | Readable recording tail pending |
| `Finishing recording` | Original writer finalizing |
| `Video unavailable` | Playback unavailable |
| `Video playback` | Accessibility state role prefix |
| `Rewind 10 seconds` | Button action |
| `Pause video` | Button action while playing |
| `Resume video` | Button action while held |
| `Jump to live` | Recording `>>` action |
| `Jump to end` | Saved-file `>>` action |
| `Return to camera` | Selected-media exit action |
| `Save frame` | Exact selected-frame action |
| `Unmute playback` | Action when playback muted |
| `Mute playback` | Action when playback audible |
| `Video controls` | Transport accessible name |
| `Video timeline` | Slider accessible name |
| `Left and Right: one second. Page Up and Page Down: ten seconds. Home: start. End: recorded end.` | Timeline help |
| `%1 of %2` | Accessible current time and total time; preserve both placeholders |
| `Open video` | File dialog title |
| `View video` | Camera/source dropdown option |
| `Select video` | Conditional file selection button under the source dropdown |
| `Open a saved video (Ctrl+O)` | Open action help; preserve shortcut |
| `Record Original video` | Record tooltip |
| `Start or stop an Original MP4 recording` | Record accessible description |
| `Select sound for the Original video recording. The microphone is used only while recording.` | Recording microphone tooltip |
| `Select microphone audio for the Original MP4 recording, or choose no microphone.` | Recording microphone accessible description |

The existing full HTML help key changes its paragraph from `Photo and Record
save both original and processed versions.` to `Photo saves original and
processed versions. Record saves Original video.`; retain the surrounding guide
and markup when updating that existing catalog entry. The legacy processed-canvas
combo accessor remains for settings/control compatibility, but the combo is
disabled, hidden and absent from layout and keyboard navigation.

## Fixture scope

`video_transport_tests` runs with Qt's offscreen platform and fixture-only
state. It covers visibility, no-command snapshots, source-scoped jump wording,
drag mapping/release/cancellation, keyboard/wheel behavior, native numeric/time
accessibility and narrow minimum targets. It opens no camera, video, native
window, microphone or AI request. Native focus, NVDA and visual acceptance remain
separate serial desktop work.
