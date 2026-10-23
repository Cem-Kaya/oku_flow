# Selected video document lifecycle

`app_video_playback.cpp` owns selection and transport intent; `VideoPlayback`
owns the single native reader/decoder worker. Camera capture, clean Original
recording admission, microphone input and camera health retain their own
owners. A selected file or retained recording scene never marks a camera as
healthy. This implementation follows the owner's amended interaction contract:
opening a video first stops and asynchronously finalizes an ongoing recording.
Starting a recording from file view returns to the camera only after recording
preflight and the first Original frame admission succeed. A Starting reservation
keeps the selected file, cursor and playback intent while clean camera capture
and microphone setup continue independently. Codec/path failure before that
first admission cannot erase the file selection.

## Ownership and authority

`OkuFlowApp::VideoDocumentState` contains one selected source, a stable opaque
document id, the current native request generation, the last successfully
presented owned frame, optional decoder corroboration, and recording-part
receipts. A 16 ms timer runs only for a recording, selected file or pending
operation. Camera view now also owns the separate bounded silent RAM recap and
its transport polling; its encoder is independent of the recording/file
document and creates no temporary capture archive. See [live recap](live_ram_recap.md).

`InitializeVideoPlayback` connects the native Open video/drop actions and the
bottom `VideoTransport`. `ShutdownVideoPlayback` stops the timer and cancels
the player/output workers without joining a synchronous native operation.
The app must call shutdown before global Media Foundation/COM teardown.

`InvalidateMediaSelection` advances `mediaDocumentEpoch_`, drops immutable UCT
scene authority, cancels pending Assistant/image observations and annotation
captures, and calls the control service's `SourceChanged`. Source changes and
seeks cannot deliver a result against an obsolete coordinate mapping.
An obsolete exact-hold readback is retired through its bounded discard slot;
the presenter retains its resource owner until that readback completes.

`MediaViewSelected` distinguishes the selected viewport from camera liveness.
`PresentMediaView` passes owned decoded/held Original pixels through
`PresentSelectedMediaPixels` and only updates the presented receipt after the
native presenter succeeds. Cached scenes continue to support viewing when the
camera is disconnected. `CameraOriginalRotationQuarterTurns` keeps independent
camera orientation while a media view has its own rotation. File rotation
metadata is applied once on selection; native decode normalizes pixel aspect.
`MediaPlaybackPlaying` reports forward playback intent, including an ordered
Resume and a wait for more verified media. Presentation passes this intent to
the UCT controller's paced advancing-media path; a paused selection retains its
one-shot immutable scene and optional neighbouring-frame corroboration.
The selected CPU media recipe currently supports zoom/pan, rotation,
black-and-white, reading colours, contrast/brightness and UCT. Advanced CUDA
effects, debug composites and automatic hidden-camera AI analysis are
unavailable in this view. Native controls preserve their requested values but
disable these effects until camera view returns. Source transitions refresh
control availability/status once; playback polling does not rebuild the full
settings UI. Camera-offline/startup placeholders and the keystone tracking
strip cannot cover or imply processing of selected media.

## Recording hooks

The production wiring calls these hooks on the UI thread:

- `BeforeRequestRecording` rejects an outstanding finalization/open handoff.
- `OnVideoRecordingStarted` is delivered by the recording manager's queued
  `SessionStartedCallback` after its first successful Original admission and
  timeline publication. It returns from file to camera view and attaches the
  new recording timeline. A revocable exact-session ticket suppresses delivery
  after stop, replacement, failure, abandonment or shutdown; each session
  acknowledges only once. A preflight reservation alone never calls this hook.
- `OnVideoRecordingStopping` runs before stopping admission. A following-live
  view requests an exact full Original readback before entering a held state.
  Review playback keeps its current position and pause/play intent.
  Unacknowledged reservations and stale stop callbacks cannot alter file review.
- `OnVideoSegmentSaved` updates only the matching recording session/part. It
  keeps the existing archive identity, selected scene, cursor and newer seek.
- `OnVideoRecordingEnded` resolves only that matching session and any latest
  open request explicitly waiting for it. Failed/abandoned sessions retain
  earlier independently verified media.

`OpenVideoFile` accepts an absolute local path lexically; existence, stable
identity and decoding checks run exclusively on the native worker. The newest pending
path replaces earlier pending requests while finalization runs. The matching
session-ended callback starts native opening; unrelated callbacks cannot do
so. Selecting this session's own output reuses its existing archive/part
identity rather than treating a mutable recording path as a fixed file.

`ActiveRecordingTimeline` supplies exact admitted sequence/capture-clock/PTS
receipts. A held live scene stores both its capture sequence and original
capture-clock value, then binds only an exact receipt from the matching part.
No host-clock subtraction, zero PTS or nearest-sample estimate creates that
binding. Resume waits for that exact PTS to become verified/seekable. A
terminal unrecorded still remains owned and has an explicit unavailable
status; it cannot silently resume at a different moment. An intact bounded
admission window can prove a skipped source frame. If that evidence expired,
the terminal state says its recorded position could not be verified instead
of claiming the frame was never recorded.

Part-local PTS remains part-local. The transport concatenates finalized part
durations to form a session range, while native seeks stay in the selected
part. Playback advances to the next verified part at end of the previous part;
pending next-part media shows Waiting. Part boundaries never borrow an earlier
part's capture origin. At the end of an active part's verified prefix, native
`VideoPlaybackState::Waiting` also shows Waiting while preserving Resume intent.
The same native worker resumes Playing as another complete fragment becomes
available. Pause cancels that intent and retains the presented cursor.

## Transport behavior

Pause while following live requests an asynchronous owned Original copy of
the exact last successful presentation. A deferred newer camera scene cannot
substitute for it. Readback failure is explicit and does not fabricate a frame.
Pause while reviewing retains the last successfully presented decoded frame;
a decoder frame that ran ahead cannot replace that held scene. Resume seeks
that owned cursor and then advances at 1x.
The held source restores its actual successful crop rather than a newer
deferred zoom/centre request. A viewport/aspect change that cannot reproduce
the retained transform reports Pause unavailable instead of accepting a
different framing. Clean Original retention stamps actual successfully
processed appearance and successful display geometry. It rejects displayed
composites, GPU geometry/effects the media path cannot reproduce, and newer
requested appearance that differs from the retained stamp. Owning the same
source frame alone does not prove an exact held view.

The recording's double-chevron action closes obsolete review work and follows
a fresh camera presentation. Until that presentation succeeds it reports
Waiting, rather than Live. With a disconnected camera it selects verified
recorded media and reports the interruption. On a finalized recording or
imported video, the action decodes to the actual end and stays paused.
Resume at that end does not restart. Returning to camera clears selected media
and restores camera orientation; it does not delete any user file.

Open and seek requests use one player worker and generation fences. The old
owned image remains visible until a current-generation valid frame is ready.
Open/seek failures preserve that image. If a failed replacement discarded the
old native reader, the old selected file is reopened/reseeked on its next
transport command. Initial file selection is paused and local viewing never
submits Read/Explain automatically.

## Sound, images and agent receipts

`VideoAudioOutput` receives bounded PCM packets independently of microphone
capture. Recording and recording-session review are always muted. File output
uses the sink's current-generation device-consumed clock when valid and
explicitly invalidates that clock on underflow/pause/mute/error; native playback
falls back to its host clock. Sink failures report unavailable playback sound
while video remains usable. Actual device A/V synchronization still requires
hardware validation. Forward intent during a verified-prefix wait does not run
the audio sink: output and PCM admission require current-generation native
Playing, and Waiting invalidates the consumed-audio clock.

`SaveSelectedVideoFrame` first presents the selected view successfully, then
owns its exact processed viewport buffer. It composes the current UCT overlay
at the viewport scale and source-aligned annotation strokes, and queues one PNG
through the bounded image pool and atomic `UserDataPaths::WriteAnnotationImage`.
Configured root, install directory, date, stem, owned image and save identity
are snapshotted as values on the UI thread. The image worker constructs its
own `UserDataPaths`, resolves/canonicalizes the destination, creates the dated
folder and publishes the PNG. Slow cloud/network filesystem providers never
perform these operations on the UI thread; final path/write errors return
asynchronously against the same save ticket.
It never asks the camera for a later frame, changes source or rewrites video.
One in-flight save is admitted. Completion retains its originating document id
and save revision even if selection changes. The revision and exact frame
identity are admitted only once the owned image payload is ready for a queue
attempt. Missing selection/presentation/image/paths leaves the previous save
receipt unchanged. A busy writer fails the newly admitted frame identity. Agent
Save commands require a new revision from their own call and cannot claim an
older committed receipt when no new payload was admitted.

`SelectedVideoState` exposes path-free native state: an opaque `media-` id,
selection/request/completed generations, actual presented frame sequence/PTS,
explicit provisional capture time when PTS is not proven, verified range and
save receipt. `SetSelectedVideoPlaying` is idempotent; agent commands need not
toggle an uncertain state. Open remains a native UI-only action without an
explicit per-process approved-file handle.

## Validation boundary

Static review and `git diff --check` were performed during implementation.
The opt-in production-app offscreen fixture now includes plain-camera/no-timer,
paused/playing Stop cursor retention, late callback/source ownership, failed
recording preflight, camera-offline native AVI decode, and latest-open matching
finalization rows. Additional real-app rows preserve paused/playing file review
through forced native-worker encoder startup failure, exercise first-admission
publication and queued acknowledgement (including stop before delivery), and
prevent unavailable Save calls or agent operations from reusing a committed
receipt. The acknowledgement row simulates the exact post-success admission
seam; it does not claim a successful codec run. Test-only document/receipt seeds
are compiled only with
`OKUFLOW_VIDEO_DOCUMENT_TESTS`; the AVI is generated from first-party raw colour
grids and opens through the actual native reader. Its offscreen decode result
does not claim a D3D presentation or codec/profile acceptance for public media.
Compilation, tracked CPU/CUDA checks, native codec round trips, serial desktop
fixtures, actual audio output, NVDA/Magnifier and high-DPI export comparison
remain centralized integration gates. In particular, validate deferred
presentation Pause, delayed/dropped admission, format-part handoff, Stop during
review, latest-open wins while finalizing, failed preflight preserving file
selection, camera-offline playback, exact frame exports and stale agent
observations. This document is an implementation contract, not evidence that
those device/desktop acceptance gates passed.
