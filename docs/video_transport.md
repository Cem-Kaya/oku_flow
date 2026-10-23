# Video transport

This development UI places `VideoTransport` in a vertical view column directly
below `RenderWidget`, alongside the Advanced inspector. It spans the render
column's width. The native corner-control windows remain inside the render
rectangle, so they cannot cover the timeline. Live camera use now also has the
same transport for a separate silent RAM recap, even with recording off.
Its first/end values are actual rolling bounds and exact Freeze can precede
the first coded segment. See [the recap contract](live_ram_recap.md).

Collapsing the controls hides their chrome and removes hidden actions from
Tab order. Viewport Space still issues Freeze/Resume through the same source
and busy-state gates, without moving viewport focus or expanding the bar.
Native Space activation on the chevron and other visible buttons is unchanged.
Permanent recap failure uses the existing **Video unavailable** status; its
existing native reason is also included in the polite state announcement.

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

The bar uses a slim source/status line above one horizontal row of icon actions,
a scrubber and the time readout. Actions have 44-DIP minimum targets; play and
the rewind badge are wider. The scrubber takes the remaining width. At narrow
widths save, mute and return move below, leaving play and the scrubber together.
Time moves to the metadata line at that width. Full source names remain in
accessible names and tooltips when the visible label elides. Time readouts retain
hours for long recordings and use
the active locale's digits. The displayed selected time can exceed the verified
seekable range while a live scene is pending; the slider never exceeds it.
While following live recap or an ongoing recording, the filled groove and
playhead stay at the right edge. This indicates live viewing even when the
camera is ahead of the last finalized segment. The displayed time and seekable
bounds stay exact; no pending tail becomes seekable. A focused reader's numeric
inspection remains still. Scrubbing or seeking immediately restores the target
playhead, and Jump to live restores the right-edge marker. An expired paused
hold keeps its exact image/time with an amber, dashed marker pinned left. That
marker indicates an expired hold, not the first available frame; numeric
RangeValue remains absent until a real seek returns inside history. A held
frame ahead of finalized history still has no ordinary timestamp playhead.

Releasing the scrubber at its far-right endpoint, or pressing End, requests
Jump to live for recap and recording. It still requests live when finalized
history grows during the drag: the frozen right endpoint represents that
intent rather than an older timestamp. Saved video keeps ordinary end seeking.

A neutral chevron at the leading edge of the Custom Setup quick-mode controls collapses the entire
transport, including filename, state, jump action and all reserved height. Its
native keyboard/touch target remains 44 DIP. Collapse preserves the playback
snapshot and running playback; it cancels an unfinished drag without seeking.
The arrow remains in that corner group while the transport is hidden. Expanding
restores the current timeline and Jump to live. Metadata updates cannot reopen it.
Checked state changes the arrow direction without adding a blue fill; keyboard
focus retains its visible ring. The adjacent action group contains Photo,
Record, Explain and Draw, wrapping two by two at narrow widths.
The state survives the application's global UI hide/show for this session.
The button reuses the translated `Hide UI` / `Show UI` action names with
`Video controls` as its accessible description; no new translation keys are added.

The step-forward icon (`>>`) requests Jump to live for the selected ongoing
recording, and Jump to end
for a selected saved video. It never changes speed. Resume continues from the
held position. End of video disables Resume until the user seeks away. Resume
of the same selected file can retain exact held OCR references while independent
advancing-frame probes revalidate them; seeks and source changes still invalidate
the old scene. See [zoom and Resume ownership](viewport_zoom_and_resume.md).
Playback mute is independent of microphone capture. Return to camera and Save frame
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
Native failure detail is metadata on the existing status tooltip and accessible
description. It never enables an empty timeline or changes its verified bounds.

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
The internal `TimelineSlider::SetEdge` / `initStyleOption` supply endpoint markers only
to native style painting/hit testing. Before a live pointer press, `RefreshTimeline`
refreshes the actual range and value; dragging then uses normal slider geometry
and freezes that mapping until release. This does not emit an autonomous seek.
`CommitSeek` receives explicit right-end intent from drag/value interaction;
ordinary timestamp seeks and source snapshots cannot invent a live transition.

## Keyboard and accessibility

The 2026-10-23 combined integration passed CPU/PCH-off SCOPED 5/240 and
CUDA/PCH-on SCOPED 5/246 with zero skips/unresolved results. Actual-app offscreen
rows verify full-height reclamation, wide/compact corner placement, no playback
requests on hide/show, metadata/global-hide preservation and viewport Space.
Saved offscreen widget captures show both layouts; they do not establish native
desktop focus, UIA/NVDA or full release acceptance. All 24 translation catalogs
passed source-key/format validation without new source strings.

The chevron follows the quick-mode Next button. Transport Tab order follows the
row: rewind, play, jump, timeline, save, mute and return. Collapsed transport has
no focus targets; its corner chevron remains keyboard reachable.
MainWindow
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

### Live-edge validation (2026-10-22)

Frozen runtime source `8fdb8d2` compiled in the separate shipping build. The
transport contract passed all 35 Qt results with zero failures/skips in both
CPU/PCH-off and CUDA/PCH-on builds. It covers live marker pixels, right-end
live intent during growing-history drags, ordinary saved-file endpoints,
light/dark expired-hold marker pixels, truthful out-of-window time and no
autonomous seek on eviction.

The final CPU selection passed four cases: transport, both actual-app offscreen
control cases and shared-source equivalence (26.49 seconds). PCH-on transport
and equivalence passed two cases (16.52 seconds). These are scoped background
results. An earlier broader run on `203cb3e` passed shipping compilation and
the CUDA delta, but its CPU gate retained an unrelated `phone_camera_contract`
failure: the owned-child fixture PID read as zero instead of 45408. Its cause
is unresolved; the original attempt is archived and was not retried unchanged.
The complete matrix is therefore not a pass. Native GUI, screen-reader and
physical-camera acceptance remain unrun for this repair.
