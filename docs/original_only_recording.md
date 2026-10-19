# Original-only recording migration

New Windows recordings have one selected successful clean Original MP4 for
each source-format part. AV1 remains preferred with H.264 fallback. A failed
attempt's diagnostic bytes remain on disk under the existing exclusive-output
and fresh-attempt naming rules. No processed companion encoder, processed
recording canvas, recording-only processed resampling or ink composition is
created by `RecordingManager`.

The Original producer supplies full source pixels after necessary color
conversion and source-orientation normalization. Recording does not reapply
rotation metadata. Playback crop, colors, Ultra Clear Text, Super Resolution
and annotations belong to viewing; they must not enter this producer.
Extra Stable holds only the displayed image: conversion/orientation publishes
the clean Original before stabilization and bump hold. Capture and recording
continue while that view is held; recovery returns the view to the current
source, leaving the intervening camera frames available in the recording.
Manual recording rewind remains an independent viewing action.
Automatic hold/recovery does not issue decoder pause or seek commands; the
camera pipeline keeps running and returns to current pixels on recovery. The
existing bounded recorder may still report ordinary encoder/queue drops; a
display hold must not introduce a separate recording pause.
Paired photo capture remains separate: the Original photo stays clean, while
the processed photo includes eligible visible UCT text and active vector ink.
Saved annotated images also include UCT below the drawn figures. Their vector
overlays are frozen with the capture, so delayed image saving cannot use later
OCR words or a later viewport. UI chrome and editing handles are excluded.

## Recording contracts

- `CapturedFrame::HasCpuPixels()` validates a complete tightly packed BGRA
  allocation and guards size overflow. GPU admission retains its owned scene
  lease and exact dimensions.
- `RecordingManager::AddOriginalFrame(CapturedFrame)` admits the owned source
  through the bounded 12-frame queue. The temporary `AddSceneFrame` and
  `AddGpuSceneFrame` compatibility wrappers forward only that source; processed
  payloads, transforms and strokes are ignored. They copy no processed pixels.
- One `originalRecorder_` handles video admission, GPU-reader retirement,
  microphone admission/submission and finalization. Existing timestamp-gap,
  bounded queue, encoder-credit, worker-watchdog and abandonment rules remain.
- `SetCanvasMode` is a deprecated no-op. `ResolveCanvas` returns the source's
  actual width and height, so old processed-size preferences cannot reduce
  Original resolution. Settings/UI/control catalog removal is separate wiring.
- `CurrentArchive()` cheaply snapshots the selected writer's shared archive
  under the queue mutex. It does not call an encoder on the UI thread and
  remains available after finalization until the next session starts.
- `CurrentTimeline()` returns an immutable `ActiveRecordingTimeline` snapshot
  only after actual successful writer-input admission. It carries exact
  session/part/archive identity, first/last admitted capture clocks, latest
  admitted sequence and its actual recorder PTS. At most 128
  `RecordingFrameAdmission` entries retain sequence, capture clock and actual
  writer-input PTS; `FindAdmission(sequence, captureClock)` requires both
  fields. Dropped or expired admissions have no inferred position. Input
  admission may still be pending inside the writer: archive
  `latestWriterAccepted100ns` and verified seekability are separate boundaries.
  This distinguishes a held camera scene that reached recording from one
  without current evidence, and avoids inventing a Pause position.
- `SavedRecordingSegment` retains session ownership and part index, canonical
  `originalPath`, the exact part's shared `archive`, first/last capture-clock
  anchors, video sample count, completed disposition and failed-attempt paths.
  Its `processedPath` is deprecated and always empty for new recordings.
  `LastSavedSegment()` exposes the latest successful current-session part
  without requiring a Qt callback. It resets with the next session.
- The saved callback captures its receipt before later sessions can start.
  A shared delivery fence also suppresses already-queued successful callbacks
  after worker abandonment. An abandoned encoder startup cannot publish a
  late successful route.
- The optional final constructor argument `SessionStartedCallback` receives
  `const RecordingSessionInfo&` once per session, only after the first
  successful Original frame reaches `PublishFrameAdmission`. That boundary
  first publishes the exact timeline and increments admitted-frame accounting.
  Merely accepting `SetRequested` or starting an encoder does not acknowledge
  recording, so callers preserve a selected saved video/cursor until this
  callback arrives. `CompleteFrameSubmission` routes rejected first frames
  through normal failure retirement without acknowledging a start.
  Delivery is queued to the record-button UI context (or the Qt application
  for desktop-free callers), protected by a `QPointer`, abandonment fence and
  a revocable exact-session ticket. Stop, failure, replacement, shutdown and
  manager destruction revoke pending delivery; later format parts never post
  a second acknowledgement. The payload includes the resolved audio result.

`ComposeRecordingAnnotations` and the paired-queue/processed-recorder fields
were removed. Annotation snapshots and photo pair rendering are unaffected.

## Notes contracts

`AssistiveRuntime::NoteCapturedVideo(originalPath, sessionId, segmentIndex,
sourceClockOrigin100ns)` renders one video element/source and one transcription
tool section. `AssistiveFeatureManager::NoteCapturedVideo` forwards the same
typed values. The bootstrap saved callback uses this API directly; it neither
duplicates an Original path nor supplies an empty leg to a pair API.

The card uses existing translated labels and records validated session/part
and source-clock metadata. The serial notes writer fingerprints the exact
finalized Original into its existing immutable `.okuflow-media.json` identity;
the browser transcript sidecar remains associated with that Original. Missing,
changed or replaced Original bytes retain the existing storage error behavior.
`NoteCapturedVideoPair`, `NoteCapturedPhotoPair` and the paired HTML renderer
remain available for legacy videos and current paired photos. Existing media
and notes files are untouched.

## Validation

`recording_manager_tests` now covers one-source queue admission and ownership,
malformed pixels, bounded overflow, exact bounded part-admission snapshots,
late startup success after abandonment, processed-preference deprecation,
preserved codec-failure files and the existing exact-session/retirement rules.
Start-acknowledgement rows cover encoder-start/first-frame failure, timeline
and audio readiness, repeated parts, reused session ids and queued delivery
after failure, abandonment, shutdown or destruction. These model-free rows
exercise the actual admission-publication and delivery boundaries; they do
not claim native codec success.
`notes_html_tests` verifies one current video card and immutable identity,
Original transcript targeting, legacy two-video cards and paired photos.
Those checks parse the appended media elements and decoded attributes, excluding
the document's CSS selectors and JavaScript. They compare each card's actual
video/image sources and transcript identity rather than counting label text.

Include `tests/original_recording.cmake` after the manager target to register
`recording_original_native`. This serial `codec` case enables only
`nativeOriginalRecordsOneVideoAndAudioTrack`; it uses synthetic CPU frames and
PCM, with no camera, microphone, browser or native window. It encodes through
the actual manager/writer, checks one successful Original output plus preserved
failed attempts, one video/audio stream, exactly 48,000 admitted PCM frames,
clean decoded pixels and source dimensions, archive identity and part receipt.
The synthetic Original advances red, green and blue while its legacy display
payload stays black; decoding must recover all three colours through end of
stream. This guards recorder admission against a frozen display payload; it
does not replace the separate CUDA surface/hold-path check.
`media_gpu_scene_cuda` supplies that separate window-free check with the real
`CudaInteropSurface`: it requires a sharp reference, exact held acquisition,
recovery crossfade and return to Live, while comparing every Original texture
with the current raw input byte for byte. Both tests passed in the 2026-10-19
integration run. They do not establish physical-camera or long-recording quality.
Enumerated outputs and failed attempts are matched through held Windows file
identities, including when Qt and the native fallback label paths differently.
The normal manager case leaves this native slot explicitly skipped.
The native row sets `QT_FORCE_STDERR_LOGGING=1` so Windows Qt reports its exact
assertions to the archived CTest output even when no console is attached.

Direct invocation after building is:

```powershell
$env:OKUFLOW_TEST_ORIGINAL_RECORDING_NATIVE = '1'
& '<cpu-build>/tests/recording_manager_tests.exe' nativeOriginalRecordsOneVideoAndAudioTrack
```

Native output is retained under
`%LOCALAPPDATA%/OkuFlow/test-evidence/original-recording/native-*`; set
`OKUFLOW_TEST_ORIGINAL_RECORDING_EVIDENCE_DIR` to another private local evidence
root. The native slot is not real-camera, physical-microphone, long-recording,
UI or release acceptance. No builds or tests were executed during this
subtask; validation is centrally coordinated by the parent implementation.
