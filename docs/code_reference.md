# OkuFlow Code Reference

## Optional embedded selected-video adapter (2026-10-21)

`src/common/video_ffmpeg_reader.{hpp,cpp}` defines private
`detail::{FfmpegPlaybackRequested,CreateFfmpegVideoReader,ArmFfmpegVideoReader,
FfmpegVideoReaderError}`. Without explicit SDK headers it compiles a native
fallback stub. `Api`/`Library` load one private dynamic runtime identity with
version/config/license checks. `FragmentedMp4` probes bounded held-stream boxes
to refuse the measured fragmented-file PTS mismatch, refuses exhaustion of its
top-level or child-box budget, and `Matrix` validates
supported color matrices. `Track::{Open,Seek,Decode,Sample}` own one held
AVIO/demuxer/decoder/packet/frame per track; `Read`, `SeekIo`, `Interrupt`,
`DenyExternal` and `Canceled` fence file access and secondary I/O. `Reader`
implements the synchronous IMFSourceReader subset and private
`EmbeddedControl::{Arm,Failure}` COM interface. QueryInterface identifies
system readers without RTTI. `Require`/`AvCheck` retain native/library failures.
RAII retires contexts/conversions on the existing worker; code remains loaded
for process lifetime.

`CreateFfmpegVideoReader` uses the successfully opened video demuxer's stream
inventory to decide whether an audio cursor is required. Failure of a known
audio track cannot be mistaken for a genuinely silent source.

`video_playback.cpp::ConfigureReader` accepts worker state and the exact
request generation for cancellation and same-held-file fallback; it never
adopts a successor generation while opening an older request.
`ReadVideo`, `QueueAudio` and `SeekReader` arm the
optional provider with generation/deadline. `Publish` fills snapshot
`decoderBackend` independently of its reason; private `Reader::Reset` resets
it. Existing `SealFrame`, pixel quota, PCM trimming/high-water, supersession,
archive proof, GPU quarantine and publication own all selected-frame authority.
Native CPU-fallback reason formatting is covered by the long-GOP regression.
No new app/controller or OCR authority exists. See [contract](video_ffmpeg_playback.md).

`cmake/FfmpegPlayback.cmake::okuflow_configure_ffmpeg_playback` explicitly adds
optional headers/definitions, without downloading, import linking or deployment.
`prepare_ffmpeg_playback.ps1` verifies pinned inputs/reused sources, builds private
shared SDKs and records runtime/config/notices. `compare_video_backends.ps1`
owns the global lease, finite child and evidence. `EmbeddedAdmissionRefusals`
covers exhausted container probes and a failed cursor for known audio.
`EmbeddedContract`,
`EmbeddedMissingRuntime` and `CompareEmbedded` exercise actual backend/fallback,
canceled I/O, issuance/retirement and same-source PTS; production PCM/4K/archive
fixtures include continuity and embedded-specific backend assertions.

## Optional UCT software-event timing (2026-10-21)

`uct::timing::{Channel,Event,Flag,Key,Values,PublicationTicket,Record}` describe
numeric-only software events and exact producer/source/acquisition/job/authority
correlation. `Recorder::{TryRecord,CloseInto}` owns a fixed 4096-record buffer.
Its private `ActiveAttempt` guard latches incomplete coverage when close overlaps
an unresolved producer/drop and bounds the window by copied event timestamps;
`Snapshot` and `ChainSummary` distinguish lost, incomplete, cancelled, faulted
and mismatched-ticket chains. `Summarize` covers worker/controller phases only.
`WriteJsonLines` emits a locale-stable bounded schema; `Enabled`,
`SteadyNanoseconds`, `NewProducerId`, `RecordEvent`, `CloseForDiagnostics` and
`FlushAtNormalTeardown` implement opt-in process lifetime, nonblocking recording
and one uniquely created LocalAppData output. No pixels, OCR text or arbitrary
strings enter records. `GpuBackend` describes recognizer status separately from
resident `DeviceInput`. See [timing contract](uct_timing_trace.md).

`uct::TimingKey` and `TraceTiming` adapt `SourceStamp` without mixing media PTS
with steady nanoseconds. Recognition and probe schedulers expose
`TimingProducerId` and record submit/dequeue/done/cancel/state/fault boundaries.
Controller `NoteTimingAdmission` captures the actual selected result plus its
corroborator relation; `UpdatePositionedTimingPublication` issues positioned
revisions only when content changes. `UpdatePaneTimingPublication` owns a
separate pane ticket and can share the revision for simultaneous changes.
`UpdateReadingText` cannot reticket unchanged positioned content.
`SetTimingTicket` on overlay, panel and screen-text owner copies that provenance.
The overlay records assignment and the first real backing-store paint per
revision; the pane records changed text; accessibility records scheduled and
published snapshots, including coalescence. Reprojection and export cannot
manufacture a new admission or first-content paint.

`uct_timing_trace_contract` tests recorder bounds, contention, concurrency,
closed-window accounting, metadata schema and chain classification.
`uct_timing_controller_contract` tests actual scripted scheduler/UI/admission
ticket correlation, and `uct_screen_text_timing_publication` tests immediate
replacement, delayed/coalesced additions and first-paint provenance. These are
desktop-free software contracts, not external UIA or photon timing acceptance.
`uct_timing_pane_publication_contract` exercises the real controller's pane-only
change and later pan, preventing a phantom content-paint sample. Its staged
80-ms fake clock awaits exact probe-frame validation and actual recognition
admission callbacks before advancing, so setup cannot expire otherwise current
350-ms proof while worker/UI receipts remain queued.

## Accessible video and immutable UCT (2026-10-18, development)

- `AccessibleVirtualChildren`, `MarkAccessibleVirtualChildHost` and
  `kAccessibleVirtualChildHostProperty` add virtual elements to `RenderWidget`'s
  normal accessibility tree. `UctScreenTextAccessibility::{SetEnabled,
  ContentChanged,RevokeAll,ProcessDueWork}` owns bounded per-line StaticText
  elements, passive one/three-second timing for new lines and immediate
  revocation. `Sync` replaces changed text under an existing line ID in the same
  update, preserving unchanged interfaces and the cadence for unrelated additions.
  `replacementDoesNotWaitBehindReadingHold` covers mixed replacement/addition
  under an active reading hold at all three offscreen scales.
  `UctScreenTextSnapshot` and `UctScreenTextTiming` define its source/timing DTOs.
  `uct::OrderLinesForReading`, `ScreenToken`, `ScreenLine`, `ScreenTextLimits`
  and `OverlayModel::ScreenLines` project only eligible glyph text and omission
  runs. The overlay's `PaintedWordGeometry`, `PaintedWord`, `PaintedSceneRect`,
  `VisiblePart`, `PaintedGeometryCurrent` and `PaintedGeneration` answer geometry
  from the actual paint; `SceneWord::wordIndex` binds that projection to glyphs.
  Controller `ScreenTextAccessibility`/`ScreenTextSnapshot` hooks reuse native
  invalidation without adding OCR/probe work. See
  [screen text accessibility](uct_screen_text_accessibility.md); offscreen
  model checks do not establish native Windows UIA or screen-reader acceptance.

- `VideoFileIdentity::SameFile`, `OpenStableVideoReadStream` and
  `CreateRecordingArchiveOutputStream` bind read-only inputs and exclusively
  created outputs to held Windows file IDs. `RecordingArchive` observes writes
  before mutation, validates complete fMP4 configuration/sample payload ranges,
  and exposes independent accepted/verified/finalized watermarks through
  `RecordingArchiveSnapshot`. Its parser and byte-stream ownership are described
  in [the native backend contract](video_archive_backend.md).
- `VideoRecorder::{Archive,LastAdmittedVideoTimestamp100ns,FailedAttemptPaths}`
  preserve the selected attempt and real admission clock separately from encoder
  completion. `RecordingManager::AddOriginalFrame` owns one clean source stream;
  its old paired submission wrappers ignore processed pixels/ink. `CurrentArchive`,
  `LastSavedSegment` and `CurrentTimeline` expose bounded immutable snapshots.
  `ActiveRecordingTimeline::FindAdmission` requires both source sequence and
  capture clock; absence never invents a recorded PTS. `SavedRecordingSegment`
  retains its archive, first/last capture clocks, successful frame count and
  failed-attempt paths. See [Original recording](original_only_recording.md).
- `RecordingManager::SessionStartedCallback` is the optional final constructor
  callback. `CompleteFrameSubmission`/`PublishFrameAdmission` publish the first
  accepted Original's timeline before queuing one exact-session acknowledgement.
  Stop, startup failure, replacement, abandonment, shutdown and destruction
  revoke stale delivery. The app switches saved playback to live only from this
  acknowledgement, so an encoder failure preserves the prior file/cursor.
- `AssistiveRuntime::NoteCapturedVideo` and the feature-manager forwarding API
  write one video card with its existing transcript identity and sidecar handoff.
  Its `AppendNoteHtmlSection` submission uses a single-media layout; legacy video
  pairs and photo pairs retain both views. `ParseSettingsRoot` ignores retired recording canvas values
  and serialization omits them. `SetRecordingCanvasMode` is a compatibility slot;
  `PrepareRecordingPreferencePatch` rejects deprecated canvas mutations atomically.
- `VideoPlayback` owns one native decoder worker, including retirement, and one
  newest pending request. `OpenFile`, `OpenArchive`, `Seek100ns` and `JumpToEnd`
  return request generations. `Snapshot` supplies owned `VideoPlaybackFrame`
  pixels, a genuinely distinct optional neighbour and actual PTS/track identity.
  `SetPlaying`, `SetMuted`, `SetAudioClock100ns`, `TakeAudio` and `Close` preserve
  held-position, EOF and cancellation semantics. `VideoPlaybackLayoutFits`
  enforces complete decoded resource admission; `PlanVideoPlaybackMemory`
  returns a checked `VideoPlaybackMemoryPlan` with staging/frame bytes, working
  and retained allowances, adaptive cap and admitted lookahead. It accounts for
  stride and square-pixel expansion under a 256 MiB absolute ceiling.
  `VideoPlaybackTimestampFits` bounds media and host-clock arithmetic. Snapshot
  `decodedBytes`/`peakDecodedBytes` account retained frames and staging;
  `decodedByteLimit`, `decodeWorkingBytes`, `readyFrames`, `lookaheadFrames`
  and `memoryPressure` expose current admission/backpressure. First-target
  `lastRequestLatencyUs`, `lastRequestReadCount` and `lastRequestCopiedFrames`
  exclude deferred optional corroboration and distinguish codec preroll from
  owned copies. `lastRequestReusedFrame` identifies a proven same-frame seek;
  pending/failed requests clear the previous completion counters. They measure
  worker publication, not screen latency. `VideoPlaybackFrame::bgra` is an
  immutable shared pixel allocation; `AllocatePixels` charges it until its last
  lease retires. Reticketing metadata shares those pixels without a copy or a
  second budget charge. A weak exact-seek interval retains no extra raw history;
  an End scan proves only its selected endpoint, never its search anchor range.
  The ready queue holds at most two future saved-file frames or one archive
  frame. `Submit` invalidates it immediately; native calls remain cooperative.
  Optional corroboration follows exact-target publication and a 50 ms quiet
  period, so immediate scrubs take priority. A reused native cursor repositions
  before corroboration/Resume; audible Resume also skips owned preroll copies.
  `Worker` consumes held-frame rewind intent and `restartAtCursor` together
  under the same state lock. End publication rechecks both flags, so a
  same-generation Pause/Resume cannot strand the decoder at EOS or publish
  Ended over a pending replay of an earlier issued frame.
  `VideoPlaybackState::Waiting` retains forward intent at a growing archive's
  verified tail; new complete fragments resume it without another user action.
  Archive `parserBoxAttempts` exposes bounded unsupported-generation work.
  No playback method finalizes a writer.
  Native codec fixture `NativeAvcAac` fences its exact active-wait clock check
  with a separate test-owned `RecordingArchive`. Its `appendVerifiedPrefix`
  lambda copies only an identity/generation-validated `OpenReadStream` extent
  through `Observe` (16 MiB total, 64 KiB scratch), requires identical parsed
  track/configuration/sample/endpoint metadata, and explicitly appends verified
  native suffixes. Stable mirror version/extents are prerequisites for exact
  held PTS; delayed native output cannot invalidate that fixture precondition.
- `VideoAudioOutput::{Configure,Push,Reset,Close,Snapshot}` provide one bounded
  WASAPI owner. `VideoAudioOutputSnapshot` reports device-consumed PTS rather
  than submitted PCM; generation changes flush obsolete sound. See
  [audio ownership and clock limits](video_audio_output.md).
- `OkuFlowApp::VideoDocumentState` owns the selected source, part manifest,
  held scene, transport intent and pending-finalization open. `InitializeVideoPlayback`,
  `OpenVideoFile`, `OpenPendingVideoFile`, `PollVideoPlayback` and
  `ShutdownVideoPlayback` manage it without UI-thread decoder/encoder joins.
  `OnVideoRecordingStarted`, `OnVideoRecordingStopping`, `OnVideoRecordingEnded`
  and `OnVideoSegmentSaved` preserve the same-session cursor while opening a
  foreign file waits for finalization. `BeforeRequestRecording` preserves the
  existing file on refused preflight. `SeekSelectedVideo`,
  `ToggleSelectedVideoPlayback`, `SetSelectedVideoPlaying`,
  `JumpSelectedVideoToEnd` and `ReturnVideoToCamera` execute native transport
  intents. `SelectedVideoState` supplies path-free frame/PTS/operation receipts.
  `MediaPlaybackPlaying` separates selected forward intent from actual decoder
  Playing, so tail waits never start stale audio. `RetirePendingMediaHoldReadback`
  retains canceled full-source request identities until their exact completion.
  `SaveSelectedVideoFrame` owns the exact selected viewport and admitted UCT/ink
  before asynchronous atomic PNG writing. See [document lifecycle](video_document_lifecycle.md).
- `PresentedOriginalCache::{Stage,Presented,Displayed,Reset}` keep at most the
  last successfully displayed Original and one pending successor within 96 MiB.
  They share recorder leases without collecting unrecorded camera history.
  `AdmitOriginalFrame` separates source admission from presentation.
  `CaptureLiveForMediaHold` uses an owned matching receipt;
  `D3D12Presenter::RequestReadback(GpuVideoFrame)` retains its resource/fence
  through asynchronous completion. `AcceptLiveMediaHold` accepts only that
  exact still and reports actual admission to the CPU caller. Refused or failed
  manual holds keep an active recording's live camera selected, while a
  finishing recording retains its separate failure receipt.
  `PresentMediaView`, `PresentSelectedMediaPixels` and the
  `PresentFitted` selected-media branch reuse owned pixels for pan/zoom without
  claiming camera liveness. `CameraOriginalRotationQuarterTurns` separates
  recording orientation from file-view rotation. See [presentation](media_presentation.md).
- `VideoPlaybackGpuImage` owns immutable D3D11 texture storage;
  `VideoGpuFormat` describes decoded/square-pixel geometry and orientation.
  `VideoGpuDecoder::{Create,CreateFromDevice,ConfigureReader,SetFormat,Convert,
  RetainedBytes,HasConversionCredit}` implement exact-adapter DXGI decode output,
  checked GPU color conversion and bounded texture admission/retirement.
  `CreateFromDevice` can force a private conversion target for compatibility
  validation. `VideoPlayback::{SetGpuAdapter,UseCpuFallback}` select the preferred
  reader and explicit compatibility transition; `gpuResident`, `gpuTextureBytes`
  and `backendReason` report actual output rather than inferred hardware use.
- `MediaGpuScene::{Prepare,Process,Texture,Width,Height,SceneReady,SceneEpoch,
  SceneFrameSequence,SceneAppearance,PresentationSync,RecordPresented,
  TakeUctAnalysis,Analysis,UctAnalysisFailed,Error,Invalidate}` own one private
  CUDA scene with worker initialization/retirement, exact input leases and
  independent producer/read-completion signals. `Status` distinguishes Pending,
  Ready and Failed without a UI join. Camera stateful effects are cleared.
  `FenceSyncParams::presentationFence` selects this independent timeline for
  presenter reads, leaving camera/external signal authority unchanged.
- `PresentSelectedMediaGpuFrame`, `MediaGpuViewSelected` and
  `RequestMediaCpuFallback` wire the scene, supported appearance, viewport
  admission, observations and current UCT. `PresentSelectedMediaFrameForCapture`
  temporarily selects the actual presented frame instead of decoder-ahead work.
  `BeginSelectedVideoSaveReceipt`, `QueueSelectedVideoFrameImage`,
  `PollSelectedVideoSave` and `ConsumeVideoGpuReadback` preserve the exact owned
  Save payload across asynchronous GPU readback/PNG writing and resize retry;
  retired save copies cannot enter unrelated image consumers. GPU annotation
  requests use the selected-media presentation and exact matching receipt.
- `UctImmutableFrame::device` and device/pixel validation helpers
  (`DeviceMatchesFrame`, `PixelsMatchFrame`, `FrameInputValid`, `EffectiveDevice`)
  admit exact owned GPU analysis or CPU fallback. `UctAdvancingMediaPresentation`,
  `OnAdvancingMediaPresentedWithoutAnalysis`, `HasAdvancingDisplayReceipt`,
  `AdvancingDisplayFrameSeq`, `AdvancingDisplayTimestampMs`, `ModelTime` and
  `AdvancingValidationFresh` separate the displayed media clock from analysis
  ownership. Missing input never restamps pixels or refreshes validation;
  `mediaValidationFreshnessMs` bounds wall retention as well as real media PTS.
  Consumed advancing device leases use weak identity after worker handoff.
- `VideoGpuPresenterFixture` is the private window-free initialization/upload
  seam used only by `video_gpu_snapshot_tests`. It exercises production snapshot
  shaders, fence signals and owned readbacks. `NativeGpuConversion` checks actual
  NV12/P010 pixels, pool credit and private-target conversion;
  `NativeGpuExternal` validates exact private codec files through Open/Seek/End.
  `media_gpu_scene_tests` checks real CUDA scene pixels, rotation, effects,
  device analysis identity, private reads, source leases and init cancellation.
- `VideoTransport::Snapshot`, `SetSnapshot`, `SetUiHidden`, `TimeText` and its
  semantic signals expose native keyboard/accessibility controls without owning
  media. `MainWindow::videoTransport`, `openVideoButton`, `openVideoRequested`
  and `videoFileDropped` host it below the render surface. Drag mappings stay
  fixed until release; the slider supplies numeric milliseconds and human time
  through its accessible value interface. See [transport UI](video_transport.md).
- `TokenDisposition`, confidence and protected geometry preserve rejected OCR
  spans through Paddle decoding, `PrepareLines`, reading projection and paint.
  `RecognitionResult::sourcePixelDigest` is computed on the worker without row
  padding. `OverlayModel::AdmitImmutable` requires distinct neighbouring frame
  and pixel evidence before glyphs; a lone immutable result remains text-only.
  `protectedSceneRects` keeps replacement plates away from uncertain source ink.
  See [uncertainty contracts](uct_uncertainty_repair.md).
- `UctImmutableFrame` and `UctImmutableScene` carry owned source pixels and
  explicit media authority. `UltraClearTextController::{SetImmutableScene,
  UpdateImmutableView,ClearImmutableScene,HasImmutableScene}` share the existing
  scheduler, submit at most a selected and optional corroborating pass, and use
  an independent job deadline. Unchanged completed stills schedule no periodic
  OCR/probes; view updates only reproject. See [immutable controller](uct_immutable_controller.md).
- `UctAdvancingMediaScene` and `OnAdvancingMediaPresented` bind successful
  playback receipts to real media PTS and independent wall-clock job deadlines.
  They pace residual probes at 80 ms and OCR at a 350 ms minimum/1,500 ms
  refresh on the same scheduler, preserving ordinary frame authority while
  seek/source changes revoke it. `OverlayModel::AdmitAdvancingMedia` changes
  only the live source-age admission rule; distinct-pixel temporal evidence
  and current residual checks remain required. Unchanged paused scenes return
  to one-shot immutable admission. `HasAdvancingMediaScene` distinguishes modes.
  `OnMediaPresentationFailed` immediately hides glyph/pane/screen text and
  latches late-result suppression; `OnMediaPresentationSucceeded` restores only
  after a truthful selected-frame receipt. View-only updates and hidden-camera
  notifications cannot renew that authority.
- `OkuFlowApp::MediaViewportAdmission` and its `Stamp` retain exact successful
  source, recipe, transform, extent and marker identity. Matching still polls
  reuse the viewport/receipt without resampling or uploading it; source/view
  dirtiness and presenter retry requests still admit work. Discarded full-source
  hold copies cannot be consumed as viewport observations.
- `ObservationService` accepts an optional deterministic ID factory, stores its
  latest 16 mappings in insertion order and clears the order on cancellation.
  Its immutable `ObservationFrame` carries document/generation/PTS metadata,
  reports no camera frame age, and rejects prior-frame coordinates after media
  advances even within the same document. `AuthorizedObservation` preserves
  that identity through `ObservationStillCurrent` checks at actual delivery.
- `Field::VideoSelection` and `ControlPlan::requiredFields` add revision
  preconditions without fabricated writes or undo entries. View inverses also
  retain source authority, so a prior seek cannot undo into a new document.
  `Inverse.videoSelection` preserves the exact media identity/generation across
  the same owner's temporary camera restore without allowing an older view
  inverse into a newly opened or sought video. Native field revisions still win.
  `AdmittedControlAuthority` and `CaptureOperationAuthority` retain cancellation
  for finite admitted work after normal turn completion. `OperationKind::Video`
  exposes bounded path-free durable receipts. `PrepareVideoControl`,
  `QueryVideoState` and `ExecuteVideoControl` share native actions through seven
  new full/V1 tools (43 total); Open remains native UI-only. The default camera
  catalog is not promoted by this change. See [agent control](video_agent_control.md).

The native fixture's `OKUFLOW_VIDEO_DOCUMENT_TESTS` seams are compiled only in
that opt-in fixture, including owned-frame seeding and held-frame inspection.
They are not a shipping app or externally callable API.

`SeedCommittedVideoSaveForTest` seeds only the fixture's prior save receipt;
real source/controller checks still reject a new unadmitted Save. Invalid native
save clicks leave that receipt unchanged; queued payloads receive fresh identity.

## Configurable UCT models (2026-10-18)

- `uct_model_settings.hpp`: `ModelTier`, `ModelSettings`, `ModelTierToken` and
  strict `ParseModelTier` define three finite tiers and independent GPU M/M,
  CPU T/S defaults. `settings::PersistentSettings::ultraClearTextModels` is an
  additive root setting. `ParseSettingsRoot` defaults each invalid/missing member
  independently; serialization stores only the four tokens, never activation.
- `uct_model_pins.hpp`: `PaddleModelPin`, `kPaddleModelPins` and
  `FindPaddleModelPin` bind each detector/recognizer tier to reviewed immutable
  graph/config hashes and upstream revision. `LoadPaddleAssets` selects those
  paths, validates both graph and dictionary and returns exact graph hashes in
  `PaddleAssets`. TensorRT `Network` consumes the selected compiled hash;
  cache keys remain model/device/driver/version-bound.
- `CreateDefaultRecognizer` and `CreateAutomaticRecognizer` capture immutable
  `ModelSettings`. `CreateTensorRtRecognizer` and `CreatePaddleCpuRecognizer`
  receive the selected pair separately. Preparation and inference remain worker
  owned; CPU serves while the selected GPU pair prepares.
- `UltraClearTextPanel::{ModelSettings,SetModelSettings}` read/set four combos;
  `gpuDetectorCombo`, `gpuRecognizerCombo`, `cpuDetectorCombo` and
  `cpuRecognizerCombo` expose their native controls. Setters block signals.
  The collapsed advanced foldout retains normal keyboard
  navigation and translated captions. `modelSettingsChanged` reaches
  `OkuFlowApp::OnUltraClearTextModelsChanged`, which saves current preferences and
  calls `UltraClearTextController::ReplaceRecognizerFactory`. This retires the old
  generation asynchronously, preserving UCT enabled/Show original preferences.
- `AppControlTarget::FinalizeUctModelParameters` coalesces a batch's model
  changes into one worker replacement. The four `image.uct.{gpu,cpu}.{detector,recognizer}`
  fields share native combos, saved-state projection, strict enum validation,
  user-revision checks and Stop and undo. Model preferences remain writable on
  CPU-only builds. Effective values are null when that backend is inactive.
- Regressions cover legacy/default/invalid/round-trip settings, all six pin
  identities, independent selectors, a held old worker during replacement,
  closed planner enums and actual-app offscreen persistence/undo/user-wins.
  The five new labels were reviewed across all 24 catalogs (120/120 entries)
  by three read-only Luna agents; all 24 flagged replacements were reconciled.
  This is model review, separate from native-speaker and rendered acceptance.

Validation on 2026-10-18: shipping CUDA Release compiled; the tracked background
CPU gate passed all 86 cases (one optional MCP Qt row skipped inside a passed
case), and all 10 CUDA delta cases passed. The initial matrix's CPU resume was
refused because a repaired coverage fixture had changed shared environment
inputs; its original failed evidence was preserved, then a fresh CPU gate
completed successfully. Native offscreen acceptance passed all 24 Qt rows,
including `NativeControlFixture::{UctModels,NativeUctModels}` persistence/undo/
user-wins coverage. Explicit real-model acceptance passed seven Qt rows:
default tiny/small plus tiny/tiny, small/small and medium/medium on CPU, and
medium/medium via TensorRT with host and owned-device inputs. This verifies
the pinned graph integrations, not broad OCR accuracy or a new benchmark.
All 24 catalogs passed exact parity at 812 source keys. Native foreground GUI,
physical cameras and installer acceptance were excluded; no installer published.

## PaddleOCR analysis and fallback (2026-10-18)

This section supersedes the older Windows-only/final-viewport UCT description
below. See [UCT architecture](ultra_clear_text.md) for transfer boundaries.

- `uct_device_image.hpp`: `DeviceImage` is shared immutable ownership of a
  pre-zoom BGRA allocation and exact device/source/frame/processing/time stamp.
  `Data`/`WaitReady` expose device input; worker-only `Readback` provides OCR
  or CPU fallback pixels. `ReadProbe` is a separate bounded
  validation copy. It never supplies neural input. TensorRT holds a shared
  image lease until the stream proves completion, including error cleanup.
- `uct_analysis_pool.hpp/.cpp`: `AnalysisPool::Capture` makes stream-ordered
  owned D2D snapshots in two slots, skipping busy/oversized inputs. Private
  `Storage` event ownership prevents reuse while leased; `Image` implements
  worker-only downloads through pinned `Download` state and private streams.
  Unknown completion retains both DMA endpoints through `AsyncCleanup`.
- `AnalysisKind`, `DeviceImage::{historyEpoch,seedFrameSequence,blendFrameCount,
  temporalAlpha}` disclose single-frame versus temporally smoothed OCR input.
  `ValidationData`/`ValidationPitch`/`WaitValidationReady` and worker-only
  `ReadValidation` expose the paired exact current frame; their defaults alias
  the OCR input for ordinary single-frame images. `ReadProbe`, resident
  references and fingerprints use validation pixels; OCR color/ink fitting
  uses the OCR pixels. `AnalysisPool::CapturePair`/`CaptureInternal` reserve
  and publish a complete pair or none, counting both halves against 128 MiB.
- `CudaInteropSurface::{EnsureUctTemporalBuffers,EnsureUctSnapshotBuffers,ReleaseUctTemporalBuffers,
  ResetUctTemporalHistory,ClearUctAllocationFailures,RegenerateUctSmoothedSnapshot}` own a separate neutral EMA after geometry and
  before appearance/zoom. History is advanced per processed frame, with
  explicit reseeding and no change to the display temporal stage order.
  Media UCT snapshots allocate lazily, with no UCT history copies while UCT or
  smoothing is off. First-use pitched allocation may synchronize. A true reset
  revokes saved pre-frame history/identity; `RefreshUctAnalysis` never replaces a
  wanted-but-invalid EMA with raw pixels. After a true reset only,
  `RegenerateUctSmoothedSnapshot` may seed a new one-frame pair from the held
  geometry without changing display, PTS or appearance. Actual failures cannot
  take that recovery branch. `uctAllocationFailedWidth_/Height_` latch EMA
  allocation failure per size; `uctSnapshotFailed_` independently latches replay
  failure. `UctAnalysisFailed` includes both. Explicit UCT/history resets clear
  allocation latches; ordinary frames never retry them. Pool completion becomes
  unknown only after work is queued; pre-enqueue failures retire safely, while
  queued allocation/copy without completion proof stays quarantined.
- `ResetKeystoneState` clears only keystone tracking for the per-frame disabled
  path. Public `ResetKeystone` additionally revokes UCT history. The Text Clarity
  master edge resets display temporal history without changing neutral UCT.
- `MainWindowTests::desktopVisualChat` waits for the queued splitter/viewport
  resize render before asserting zero horizontal overflow. Text snapshot arrival
  is synchronous and cannot prove that the native layout has settled; the
  wrapping assertion and both theme rows remain required in remote GUI checks.
- `CudaInteropSurface::{EnableUctAnalysis,TakeUctAnalysis}` controls the
  throttled geometry-correct snapshot before appearance/SuperRes/zoom.
  `TryCudaFrame` enables it only for UCT. `PresentLatestCudaScene` accepts only
  the successfully presented frame/processing generation. UCT uses neutral
  analysis provenance separately from annotation neural provenance.
  `PresentFitted` hands CPU UCT the pre-zoom scene and an identity transform.
- `Recognizer::{Status,Preparing,WantsDeviceInput,RecognizeDevice}` provides status,
  device routing and default CPU readback. `RecognitionResult::deviceInput`
  records the route; validation similarity is computed independently by probes.
  `RecognitionScheduler::SubmitDevice` validates/retains the device lease and
  coalesces it latest-only. Native exceptions produce failed outputs. GPU OCR
  precedes that job's full reference/color readback. Its `probeOnly` mode uses
  `ReadProbe`, never a recognizer, and constructs only a validation reference.
  `RecognitionOutput::{inputFault,backendPreparing}` distinguishes a dropped
  input/temporary preparation from a fault that retires an inference backend.
- `UltraClearTextController::OnDeviceScene` requires matching source/frame and
  recent capture time, stamps identity scene coordinates, and submits separate
  validation plus an exact pending candidate. `HandleProbeOutcome` consumes
  only the matching stable candidate under coverage/refresh gates and never
  queues a second device OCR lease while inference runs. `EnsureScheduler` owns a bounded device
  validation reader feeding CPU `ProbeWorker`; `Invalidate` cancels all three
  authorities. `HandleResult` compares against the newest independent probe;
  `HandleProbeOutcome` cannot move the reference backward. Display zoom does
  not alter recognition input or stop validation during held inference.
  `SetEnabled(false)` retires the recognition scheduler through `AsyncCleanup`
  after revoking callback generation, so reenable creates a fresh chain without
  joining a native call on the UI thread.
- `uct_paddle.hpp`: `PaddleAssets` and `LoadPaddleAssets` verify pinned graphs
  and dictionary. `DefaultPaddleRoot` resolves explicit, adjacent and local
  roots. `PaddleTensor`/`PaddleCpuSession` describe bounded float I/O.
  `PaddleDetectionSize`, `PaddleDetectionRegions`, `PaddleRecognitionWindows`,
  `PaddleMergeRecognition`, `PaddleNormalize`, `PaddleDetect`, `PaddleRectify`,
  `PaddleDecode`/`PaddleDecodeTokens`, `PaddleQuad` and `PaddleToken` implement
  first-party bilinear BGR normalization, connected-component/PCA regions,
  padded rectification and confidence-filtered CTC word spans. Spans are
  approximate, not exact glyph geometry. `gpu::{Normalize,Detect,RecognitionWindows,Rectify,
  Decode,Probe}` in `uct_kernels` implement device equivalents using
  `gpu::{Component,Quad,Token}` with finite-value checks and bounded outputs.
- `CreateDefaultRecognizer` delegates to `CreateAutomaticRecognizer`:
  TensorRT, Paddle CPU, Windows. The candidate-vector overload supports exact
  fallback tests. Private `AutomaticRecognizer` retires unavailable/failing
  candidates for its lifetime; input faults and cancellation never advance the
  chain. A preparing GPU candidate is retained while CPU/Windows serves, then
  promoted between jobs. CPU device input is copied once per call across fallback.
  `CreateWindowsRecognizer` retains the original Windows.Media.Ocr adapter.
- `CreatePaddleCpuRecognizer` uses `OpenPaddleCpuSession`/private
  `OnnxCpuSession`, runtime-loaded hash-pinned CPU ONNX Runtime, two intra-op
  threads and checked float tensor dimensions. `CreateTensorRtRecognizer`
  creates private `TensorRtRecognizer`: `BeginPreparation` launches an owned
  `Preparation` worker for exact-version SDK factories, low-priority stream and
  model/GPU/driver-bound cached `Network` engines. `Progress` cooperatively
  checks preparation cancellation; `Ensure` only adopts ready state. `Retire`
  and `RetireState` own asynchronous completion/cleanup. Private `State`,
  `DeviceBuffer`, `HostBuffer`, `Libraries`, `DllDirectory` and mutex-protected
  `Logger` keep GPU/host/code lifetime independent of the UI. Only counts and
  compact boxes/tokens cross between neural stages and final string assembly.
- `cmake/UctOcr.cmake::okuflow_configure_ocr` locates optional local SDK
  headers, compiles availability stubs when absent and never downloads.
  `prepare_uct_runtime.ps1::{Get-Verified,Expand-Verified}` checks exact archive,
  model and CPU DLL hashes, verifies extracted bytes before its completion marker
  and retains notices; TensorRT remains external. Strict adapter-required CMake
  options turn missing headers/CUDA into configure errors.
- `PaddleTests` covers algorithms, fallback/exception/cancellation, model
  refusal and CUDA snapshot parity/ownership; opt-in real CPU/TensorRT rows
  exercise actual graphs, including direct device input. UCT controller tests
  exercise held GPU input, cancellation, UI progress and independent probes
  at 16x. Fixture helpers (`FakeBackend`, `BackendCalls`, `GpuBuffer`,
  `FixtureStream`, `DeviceJobState`, `TestDeviceImage`, `DeviceRecognizer`,
  `ReleaseDeviceJob`, `CopyFaultImage`, `SceneDeviceImage`) are test-only and never deployed.

## Packaging provenance authority (audit RB-001/RB-002)

`windows-installer/wix_tool.ps1` supplies `Get-VerifiedWixTool`,
`Assert-VerifiedWixTree` and `Invoke-VerifiedWix`. They verify the pinned NuGet
archive, extract into a hash-named private generation, reject unsafe paths,
changed/missing files and unexpected file additions, and reverify the complete
tree before executing the archive's own apphost. Both MSI compilation and
payload extraction use it; an old installed `wix.exe` shim is never selected.
The MSI receipt includes package and executed-apphost SHA-256 values.

`cmake/ReleaseBuildFacts.cmake::okuflow_generate_release_build_facts` emits
`okuflow-build-facts/<configuration>.json` from the application target: actual
output, CUDA enablement, compiler/toolkit versions, compiler and linked static
runtime hashes, target architectures and exact toolkit-license identity. Its
`POST_BUILD` command runs `SealReleaseBuildFacts.cmake` to bind the facts hash
and successfully linked application hash in `<configuration>.seal.json`; facts
are also link dependencies so a provenance change cannot keep an old seal.
CPU facts contain no CUDA claims. `Get-OkuFlowReleaseBuildFacts` in
`windows-installer/release_build_facts.ps1` checks source/build/configuration and
cache agreement, matching successful-build seal, built output,
compiler/runtime/license bytes and toolkit paths;
when given a staged application it also checks byte equality. The command
`get_release_build_facts.ps1` exposes those facts or writes a fresh CRLF batch
environment file using `BUNDLE_EFFECTIVE_CUDA` and `BUNDLE_CUDA_LICENSE_PATH`.
`generate_release_metadata.ps1` requires the matching build directory, embeds
these facts in the manifest, and checks the staged toolkit notice before using
the effective toolkit version in SPDX. Metadata never creates or refreshes a
seal; the bundle refreshes it only after its controlled successful signing step.
Its optional `-CudaEnabled` is an agreement assertion only, never the source of
metadata.

`test_release_provenance.ps1` exercises these production helpers with private
synthetic packages and CMake imported targets, including actual metadata/SBOM
generation with guarded synthetic signature results. Optional `-WixPackage`
also runs the real pin's version check and compiles/extracts a tiny synthetic
MSI from a private copy; the fixture never installs that package.

## Immutable transcript publication (audit PW-003)

The compiled notes bridge's `saveTranscriptSnapshot` serializes sidecar writes
by stable recording UUID. Completed segments and metadata become an immutable
JSON payload before file-handle awaits. Each card's `transcribeJobGeneration`
fences delayed save success/failure updates, while prior durable results still
finish writing. Failed writes abort their stream and do not prevent successor
writes. `scripts/test_notes_transcribe_bridge.mjs` executes the actual C++ raw
script literals under Node with only DOM/file-handle primitives simulated. It
holds the first file open while a successor clears its UI or finishes, proving
snapshot preservation, write ordering and status ownership without a browser,
model, real media or desktop. Exact media contracts below complement these
publication checks; neither establishes codec timing or Chrome grant acceptance.

## Fresh test-input hashing (audit TB-001)

`run_test_gate.ps1::Initialize-HashCache` starts an empty pass-local memo rather
than trusting persisted length/mtime digests. `Add-FileDigest` hashes every
distinct input at least once per pass; duplicate references can reuse that
pass's digest. The gate clears the memo before its post-run runtime/test
fingerprints, so metadata-preserving edits invalidate evidence across runs and
during execution. `Save-HashCache` retains diagnostics only. The runner contract
checks identical-length replacement with restored timestamps and fresh-pass
hash calls, while preserving all existing failure blockers and resume rules.

## Observation authority at egress (audit AS-004)

`AuthorizedObservation` is internal control-result provenance: context id,
source/processing/view/geometry stamp, frame-grant generation and native frame
sequence/monotonic processing time. `ObservationService::Encode` supplies the
pixel identity; `AppControlService::Delivery` preserves it and adds the exact
context/grant authority. Reservations retain admission-time stamps rather than
sampling new identity at completion. `AuthorizeFrameDelivery` polls native edits
and checks context, grants, modal state and original stamp; its typed overload
also rejects a revoked/re-enabled grant generation. HTTP stores authority with
pending images and rechecks immediately before POST. Codex and the MCP pipe
fence use the same typed authority. Initial chat preview work and scoped-scene
requests carry the original record rather than sampling a later stamp. These
records are never serialized as provider credentials or model-writable grants.

## Protected credential generations (audit AS-002)

`SettingsController::CredentialBinding` records the runtime id/provider/endpoint/
secret tuple loaded by `LoadProtectedSecrets` or staged by
`PrepareProtectedSecrets`. An unchanged binding is reused without rewriting the
secret. Any change stages a fresh UUID-suffixed id, preserving earlier primary
and backup references. A failed JSON publication retries that exact staged
generation; a failed secret write leaves the old reference untouched. Older
generations are deliberately retained for recoverable settings rather than
automatically deleted during rotation. The fake-secret-store regressions cover
failed publication, corrupt-primary backup recovery, endpoint-only changes and
unchanged-save stability, without accessing Windows Credential Manager.

## Recording session retirement (audit NP-003)

`RecordingManager::FinishSession` captures the ending session, clears its video
and audio queues and stop flags, retires its worker-operation marker and queues
immutable completion callbacks before publishing `Completed` or `Failed`.
Those states admit a new session immediately, so worker video/audio error
epilogues must not clear shared queues afterward. The private immutable
`statePublicationObserver_` and friend `RecordingManagerTestAccess` expose this
boundary to the desktop-free fixture without starting a codec. Its
`terminalPublicationRetiresOldSession` cases hold publication for normal,
video-error and audio-error retirement while a replacement is admitted; old
cleanup cannot remove the replacement's queues, stop request or session id.

## CUDA toolkit selection and one-time comparison

`cmake/CudaTargets.cmake::okuflow_select_cuda_architectures` selects the shared
application/fixture/cubin inventory from the actual compiler version, adding
Pascal `sm_61` for CUDA 12.9 while retaining the modern targets in CUDA 13.
`OKUFLOW_CUDA_TOOLKIT_ROOT` optionally pins the compiler and FindCUDAToolkit
root before language initialization; switching toolkits requires a fresh cache.
Normal discovery refuses CUDA 13 rather than silently changing the CUDA 12.9
default. CUDA 13 remains available through an explicit comparison root.
`tests/cuda_toolchain_benchmark.cu::{Check,Buffer,Events,Percentile,Measure,RunSize}`
implements the opt-in, desktop-free one-time production-kernel comparison.
`Measure` excludes allocation/upload/readback from CUDA-event timing and emits
JSONL timing/identity/checksum records. It is not in default builds or CTest.
The bundle's `resolve_cuda_license` label prioritizes this explicit root and
refuses a missing selected-toolkit license instead of staging another version.

## Windows release orchestration

`scripts/publish_windows_release.ps1` invokes the tracked shipping/CPU/CUDA and
bundle gates, snapshots the clean resulting inventory, builds and validates MSI
and ZIP, optionally runs disposable installer acceptance, and publishes only
after uploaded draft assets match local hashes. `Assert-CleanSource` binds each
stage to the initial clean commit and refuses changes during the run. It does
not submit Store/winget entries or install an acceptance MSI on the host.

`LanguageManagerTests::liveStatusRetranslatesAccessibly` expects the reviewed
Turkish pipeline-status wording while retaining the accessible-name and text
change-event assertions.

The native idle-chrome tests align injected Qt mouse events with the observed
cursor position, allow at most one logical pixel of mixed-DPI rounding, and
detect interruptions using exact native pixels. Startup pointer/focus events
are drained before the unrelated-input fixture expires its timer.

## Localization resources and validator, 2026-10-14

The 24 `translations/okuflow_*.ts` catalogs contain 805 source keys each in
the local chat/control development work.
`scripts/check_translations.ps1::Read-Catalog` retains exact source, format,
markup and line-ending checks. Its French and Dutch identical-cognate tables
also permit genuine identical words such as French `Microphone`/`Photo` and
Dutch `Contrast`, without permitting untranslated English sentences.

`common/notes_document_template.hpp` and `common/notes_transcribe_bridge.hpp`
retain their 25 inline language maps. The corrected viewer labels distinguish
video recordings from audio, and bridge guidance matches the visible Transcribe
action. `launcher::kText` retains 25 rows of 14 preflight messages; corrections
change wording and agreement without changing runtime-preflight behavior.
The [review record](localization_review_2026-10-14.md) documents source inventories,
correction counts and validation. No translation keys or runtime dependencies
were added.

## Android CLI publishing

`scripts/configure_android_play.ps1` validates a local Google service-account
JSON and stores it as a Windows DPAPI credential outside Drive/Git. It verifies
the protected round trip and persists only the safe profile path.
`scripts/google-play/publisher_helpers.ps1` supplies
`Get-AndroidPlayProfilePath` (local/reparse boundaries),
`Test-AndroidPlayCredential` (service-account identity and fixed Google OAuth endpoint),
`Read-AndroidPlayCredential`, `ConvertTo-AndroidPlayBase64Url`,
`New-AndroidPlayAccessToken` and `Invoke-AndroidPlayRequest`.
The in-memory `OkuFlow.PlayPublisherCrypto.Sign` imports PKCS8 PEM and signs an
RS256 JWT through the system .NET cryptography APIs; no key or token is logged.
Requests target Google's fixed OAuth/Android Publisher endpoints and app edits only.

`scripts/publish_android_play.ps1` checks access or prepares a testing-track edit.
It binds local AAB SHA-256 to Google's version/digest, reuses identical uploaded
bundles, verifies the expected active version and validates before an explicit
`-Publish` commit. Uncommitted owned edits are discarded; uploads can still consume
version codes. Production is refused. Local receipts contain no authentication data.
`scripts/test_android_play.ps1` uses an isolated fake identity to verify DPAPI,
actual RSA signature/claims and private-path/production guards without networking
or reading live credentials. See [CLI publishing](android_cli_publishing.md).

## Installer and brand tooling (2026-10-13)

`scripts/windows-installer/common.ps1` supplies stable per-path IDs/GUIDs,
`New-VerifiedBundleSnapshot` (clean provenance, safe inventory, required files,
copy/hash checks), SDK discovery, Authenticode signing and read-only MSI table
queries. `build_msi.ps1` authors standard WiX components, branded advertised
shortcuts and transactional major upgrades from that snapshot. Its versioned
receipt records installer hash, source and unsigned-preview status.
Advertised shortcut icons use `OkuFlowIcon.exe` with the verified launcher PE
as Icon-table data, satisfying Windows Installer's target-extension/PE rule.
Bare ICO data with an extensionless name produced blank/non-resolving links.
The VM fixture checks the installed resource path and rendered star/ring pixels.
`test_msi_package.ps1` checks database policy and compares extracted payload
hash multiplicities to the verified source without installation or UAC.
`test_msi_sandbox.ps1` creates an owned, network-disabled disposable guest;
`windows-installer/sandbox_acceptance.ps1` requires its WDAG account and checks
real install/upgrade/downgrade/uninstall, payload bytes, shortcuts and retained
user data. Its logs/result stay in the guest's mapped local evidence directory.
The host waits for guest evidence rather than launcher exit because current
Windows Sandbox detaches its RemoteSession process. Existing remote/guest
sessions are refused. An idle Store Sandbox server is left running; its `wsb`
guest inventory must be queryable and empty before a new fixture starts.
The guest publishes its result atomically.
`TimeoutSeconds` bounds host waiting (900 seconds by default) without forcibly
ending a running guest. winget draft headers select the exact 1.9 schema.
`generate_winget_manifest.ps1` uses the receipt and exact GitHub URL to write
the three catalog drafts; it does not submit a PR or enable automatic updates.
`build_store_msix.ps1` requires `-ReviewOnly`, uses the public Partner Center
identity and SDK schema/round-trip validation, and records runtime/certification
blockers. It neither signs nor installs/submits the review package.

`scripts/icon_tools/render_icon.cpp::main` uses QImageReader and smooth Qt
scaling without a desktop window to export Store PNG logos and PNG-encoded ICO
entries from the original canonical star PNG. It also accepts SVG input for
archived vector artwork. The wrapper preserves the canonical PNG and compiles
against the selected Qt SDK and keeps developer output outside Drive.
The Windows RC source declares an explicit CMake `OBJECT_DEPENDS` on the ICO
so Ninja rebuilds both launcher and Qt executable resources after icon updates.
`test_brand_icons.ps1` reads resources without executing either binary and
compares all seven embedded PNG frames in both deployed executables to the ICO.
`build_release_bundle.bat::find_built_executable` selects the active generator's
layout, derives the launcher beside its Qt child and rejects missing outputs;
old output directories cannot override current Ninja binaries.
The browser fixture discovers static transcription caches by prefix and checks
the page's actual icon URL, so a new cache version does not reopen an old cache.

## Browser transcription notes handoff (2026-10-12)

`AssistiveRuntime::EnsureNotesFile` composes the append-safe local notes shell
from `common/notes_document_template.hpp` and the inline file-access/port bridge
in `common/notes_transcribe_bridge.hpp`. `AppendNoteMediaPair` adds one browser
Transcribe control for a recorded video pair, using the Original audio. The
desktop does no speech processing. The browser writes/restores versioned JSON
sidecars beside recordings and never rewrites the actively appended notes HTML.
The shell preserves native disclosure cards, conversation grouping, theme,
expand/collapse and escaped content, with larger controls and narrow layouts.

The 2026-10-13 shell redesign keeps every placeholder and entry class. Its
inline viewer script adds a remembered text scale (`okuflow-notes-scale`,
87.5-200%), a four-state colour scheme (`okuflow-notes-theme`: auto, light,
dark, contrast), SVG kind icons, kind filter chips, Read aloud/Copy on
`.note-text` answers (Web Speech and Clipboard APIs, local only) and
playback-synchronised transcript highlighting using the bridge's
`data-start`/`data-end` row attributes. Its toolbar labels are an inline
25-language table keyed on the full document language tag, then its primary
subtag. The inline bridge uses the same lookup and the preview renderer supports
all 25 languages. RTL locales use logical spacing and per-section `lang`/`dir`.
The template is
split into adjacent raw-string pieces to stay within MSVC literal limits and
must not contain `http://` or a literal `<details` (asserted by the notes
tests). `scripts/render_notes_preview.py` renders it with sample entries whose
markup mirrors `AssistiveRuntime`; update both when entry markup changes.

At narrow widths the toolbar returns to document flow, controls wrap within
the viewport, summary timestamps span their own row, and padding/icon sizing
leaves room for 200% text. `scripts/test_notes_site_browser.mjs` validates
these layouts against real generated EN/TR/DE/NL/AR fixture files in a private Chrome
profile, including loaded local images/video, filters, conversation grouping,
expand/collapse and persisted text size. It also checks the sibling website's
carousel, themes, reduced motion, Arabic navigation and narrow translations;
fixtures, profiles and screenshots stay in local app data.
Its scene-count assertion follows actual slides and requires matching navigation
dots; notes checks include localized toolbar text and Arabic computed direction.

Transcript timestamps retain 44 px targets. Playback follows only the transcript
panel's internal scroll position, without moving the document. The small-screen
toolbar stays in normal flow, and Stop reading resets its state immediately.
Conversation grouping uses a dictionary without inherited property names.

`AssistiveRuntime` revokes an active HTTP Explain answer's eligibility for history
when provider, URL, model, API key or the conversation is reset. A late response
cannot repopulate cleared history or carry it into a new configuration. Read
discards any supplied thread ID at the Codex client boundary and starts a fresh
ephemeral thread. New conversation is blocked throughout request/capture work.

`MainWindowTests::mouseFocusedChromeCanIdleFade` aligns synthetic Qt input
with the native cursor, verifies that stationary logical/physical positions
cannot reveal faded controls, reports outside pointer interference distinctly,
and restores the original cursor with a scope guard. Its production visibility
assertions and timeouts are preserved.

`MainWindow::eventFilter` compares rounded logical Qt mouse coordinates,
matching the `QCursor::pos()` baseline sampled when controls fade. Fractional
stationary notifications at scaled Windows DPI do not wake controls.
`nativeEventFilter` preserves physical-pixel movement precision. Regression
coverage includes fractional stationary input and a one-pixel native wake.

Authoritative code map for the current repository state as of 2026-10-13. Update this file whenever classes, public structs, or significant functions change.

The release-bundle `copy_required_file` subroutine verifies destination
existence and byte equality immediately after copying required notices,
so metadata generation cannot silently accept an incomplete copy.

## USB phone discovery and recovery

- `CameraDescriptor.phoneAppReady`, `phoneBusy` and `phoneProblem` describe the
  companion's non-claiming capability probe. Serial and camera ID identify a
  saved choice; adb transport IDs belong only to the current cable connection.
- `MediaCapture.EnumerateCameras(includePhones)` can list only Media Foundation
  cameras. App startup and webcam reconnect pass `false`; phone socket work
  runs on a background discovery worker. `MergePhoneCameraDiscovery` preserves
  physical order, known lens identities and a live phone across a transient
  listing. An unknown adb result keeps the previous snapshot untouched.
- `ResolveAutomaticPhoneIndex` preserves explicit webcam choices and requires
  phone readiness before replacing a working camera. With no working source,
  a saved attached phone can be retried before its listener starts.
- `OkuFlowApp.PollPhoneCameraDiscovery` consumes an asynchronous USB snapshot
  every five seconds, reconciles by stable identity, and rebuilds the picker
  only when visible names/identities change. A running phone is never probed;
  a starting phone is excluded inside the transport. Recording permits only
  recovery of the same source, without switching to a fallback.
- `StartCameraCapture` always dispatches phone startup off the UI thread.
  `StartFallbackCamera` restores a temporary webcam after phone startup/loss;
  it does not overwrite the phone preference. Phone errors are nonmodal.
  `PhoneRetryPolicy.Observe/Failed/Succeeded/CanAttempt` tracks per-transport
  readiness and 5/10/20/60-second failure backoff (at least 30 seconds for busy
  or hot errors). Replug, recovery, changed problem or server epoch resets that backoff.
- `PersistentSettings.cameraSelectionExplicit` distinguishes a user pick from
  an implicit first-run webcam. `OnCameraSelectionChanged` sets it and saves
  the stable link. `settings::LoadDetailed` infers an explicit preference for
  old files with a saved identity/index; serialization retains the flag.
- `CameraStartupJob.canceled` is shared independently of the app lifetime.
  `StopCameraCapture` revokes it; `MediaCapture.StartCapture`,
  `MediaCaptureSession.StartCapture` and `PhoneCameraTransport.Start` forward
  that cancellation to the first-frame wait. Socket reads/writes also check
  cancellation. `PhoneProblemCode` / `ProblemCode` expose optional wire errors.
- `ParseAdbServerEndpoint` accepts only local TCP adb overrides; remote,
  unsupported and malformed overrides disable phone support. `Endpoint`,
  `FindAdbExecutable` and `EnsureAdbServer` select an explicit path, bundled
  `platform-tools`, environment-owned SDK or PATH. Without an existing listener
  or endpoint override, a hidden `-L tcp:<port> server nodaemon` child binds a
  private ephemeral loopback port. `AdbServerEndpoint.ownedByThisProcess`
  distinguishes this exact child from borrowed default/overridden servers.
  `Endpoint` never redirects a failed owned session to the shared default port;
  an unavailable owned child yields port zero until its manager can retry.
  `EnsureAdbServer` returns readiness, serializes lifecycle changes, requires
  bootstrap intent, yields a dead child to a new default listener and retires
  idle ownership after explicit webcam intent. `PhoneServerIntent` separates
  effect-free `BorrowOnly`, guarded `PhoneUse` and idle `ReleaseForWebcam`.
  `RequireAdbServer` permits host traffic only after ready EnsureResult values.
  `ListenerIdentity`/`ObserveServerIdentity` track port/PID/creation time;
  `CameraDescriptor.phoneServerEpoch` renews launch/recovery/retry budgets even
  when a restarted server reuses a transport ID. No epoch or PID is persisted.
  Two default version failures spanning five seconds permit reviewed legacy
  recovery or an isolated shadow child. Empty raw private transports and two
  healthy default checks permit shadow handback; live reservations forbid it.
- `OwnedChildProcess` in `capture/adb_owned_server.hpp/.cpp` creates a
  non-inherited `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job and launches suspended,
  with only explicit NUL standard handles inherited. Assignment precedes
  resume; failure cleans up the exact fresh process. `Stop` and the destructor
  terminate only its held job/process handles; owner crash closes the job in
  the kernel. `LaunchOptions.assignOverride` is a private test seam.
  `environmentOverrides` builds a sorted Unicode child environment without
  modifying the parent; production sets `ADB_EMU=0` and `ADB_REJECT_KILL_SERVER=1`.
  `DuplicateObservationHandle` returns a non-inherited synchronize-only handle.
  `OwnsListeningPort` uses
  the native IPv4 TCP owner table to observe exact-child loopback ownership,
  never as process-termination authority.
- `OwnedAdbServer` serializes `EnsureRunning` and injects probe/launch/port/session
  functions for private tests. `OwnedAdbServerPolicy` defaults to two consecutive
  failed probes, a 5s readiness window, 50ms polling and 30s launch/restart
  cooldown, including failures and already-reaped child exits. Cancellation is
  checked before effects and after probes. Live/starting phone sessions suppress
  restart. `EnsureResult` reports launch/ready/deferred/throttled/canceled/stopped
  outcomes; `Port`, `OwnsRunningServer`, `LaunchCount` and `RestartCount` expose
  process-local observations. `RunningSnapshot` holds an atomic shared
  synchronize-only handle/port so `Port` never waits for a probe; it reports
  child liveness, not host readiness. `EnsureRunning(..., false)` checks exact
  listener health without lifecycle effects for ordinary enumeration.
  Manager and lifecycle timed-lock waits
  check cancellation every 20 ms. `ProbeAdbHostVersion` uses explicit ports, while
  `CreateOwnedAdbServer` wires exact hidden children, emulator suppression,
  native listener validation and runtime occupancy. `Stop` sets an atomic
  request before waiting for the manager lock, interrupting the next polling
  step. Restarts pick a fresh port rather than reusing accepted connections.
  `ShutdownOwnedAdbServer` stops only this manager's child after discovery and
  capture teardown; `OwnedAdbServerPort` returns its live private port. Nothing
  persists PID ownership or runs global adb client kill commands.
  `AdbRuntime.shutdownRequested` prevents later bootstrap by abandoned callers.
- `TryReclaimLegacyBundledAdbServer` in `capture/adb_legacy_recovery.hpp/.cpp`
  deliberately adopts only a confirmed-hung reviewed legacy bundle server.
  `LegacyAdbRecoveryRequest` supplies phone intent, no-session snapshot/live
  callback and cancel/shutdown latches. `LegacyAdbProvenanceExpectations` pins
  the manifest, actual adb entry hash, canonical flat/nested layout and launcher
  product metadata or reviewed launcher SHA. `LegacyAdbRecoveryPolicy` bounds
  mutex/connect/exit waits; test-only callbacks and sibling scan scope isolate
  private race fixtures. Classification and each effect revalidate the exact
  held process creation/image, native listener, provenance and sibling/session
  absence. A per-user/session mutex serializes reclaimers. Raw `host:kill` gets
  post-connect revalidation; a failed bounded exit wait can use only the exact
  held process handle. `LegacyAdbRecoveryResult` distinguishes recovered,
  unmanaged, busy, identity-changed, canceled and failed, with structured detail
  and observation diagnostics. Unknown/SDK/overridden servers are never stopped.
- `PhoneAdbTestHooks` / `SetPhoneAdbTestHooks` isolate fake-socket tests on a
  private random port and disable real bootstrap; switching hooks stops any
  previously owned child and resets test budgets. `OccupiedPhone`, `MayLaunch`
  and `AllowAppLaunch` prevent overlapping probes and repeated automatic
  activity launches for one USB transport. A manual camera pick renews the
  launch allowance. `ControlHello` accepts a token-free `probe:true` ACK,
  supports older tokened ACKs and reads coded ERROR packets. Real hello and
  video binding are retried together within a bounded connection window.
  `ownedServerExecutable` / `ownedServerArguments` opt into the private fake
  process fixture, with the hook port standing in for 5037. `OccupiedPhone`
  increments/decrements per-serial counts so overlapping sessions remain
  reserved; `Run` first repairs the idle server before registering its own slot.
- `AdbHostReply` bounds raw host-query replies; `ListUsbPhones` separates USB
  listing from companion probes and retains `transport_id` in legacy listings.
  `PhoneCameraTransport.RecoverStalledUsbTransport` requires a responsive adb
  version query, the same authorized USB serial/transport before and after a
  pair of timed-out independent shell echoes, plus a fresh non-claiming companion
  timeout, then sends `host-transport-id:ID:reconnect`. The echo checks require
  all response bytes, not just one early byte. Post-ack listing must observe the
  stale ID disappearing or changing within two seconds before success.
  `AdbRuntime.nextUsbRecovery` limits requests to once per phone/server per
  minute within the process across changing transport IDs, including unsupported
  replies. A separate 30-second health-probe cooldown follows definitive answers;
  neither throttle is persisted across restarts.
  `UsbResponseTimeout` distinguishes timeout from explicit adb rejection/EOF.
  `Run` refreshes its ID using bare USB listing rather than probing other phones,
  and invokes recovery after non-protocol failures and socket unwinding;
  coded camera errors and cancellation bypass it. `Enumerate` accepts an
  optional preferred idle recovery serial. `PollPhoneCameraDiscovery` supplies
  it only while automatic phone selection is allowed and no phone is active.
  Active/starting phone probes, ordinary browsing and explicit webcam selection
  retain their ownership rules. Acknowledgment is not a recovered video frame.
- `PhoneDiscoveryStatus` distinguishes a parsed listing from missing listener,
  unresponsive borrowed server, owned startup/recovery, unsupported endpoint
  and unknown failure. `Enumerate` defaults its server intent to `BorrowOnly`;
  background app discovery passes its automatic-phone intent. Its
  optional status output reaches the app's background discovery future; a
  stalled server preserves saved camera intent and shows translated automatic
  retry guidance instead of generic missing-phone help. Discovery never
  resets or terminates an unknown borrowed server; the reviewed-legacy rule
  above handles detached OkuFlow support. GPU-required details retain the concrete CUDA error,
  `OwnedServerRecovering` and `ServerUnresponsive` display localized automatic
  retry with polite live announcements, suppressing stale errors and manual
  process-management instructions. Device selection logs its compute capability
  for field diagnostics.
- `PollWithDeadline` / `BoundedPollOutcome` bound nonblocking shutdown queries
  by completion, expiry and session abandonment. Capture's D3D11 `GetData` poll
  uses a 1000ms deadline and throws on incomplete work, retaining the session.
  The facade keeps its shared 1500ms stop deadline and logs individual quiesce,
  consumer-release and final-release timings when stopping fails.
- Android `UsbSocketServer.capabilityJson` warms a capability snapshot;
  `readChannel` answers probes without claiming a session. `writeDirect`
  returns discovery/busy replies, while `peerAllowed` and `videoBindExpired`
  isolate a dead peer and retire control-only hosts after five seconds.
  `HostProblem.wireCode` and `SESSION_BUSY_CODE` identify recoverable errors.
  `CameraStreamService.isRunning` and the resumed activity's three-second
  retry restore the listener; `serviceStopDelayMs` debounces USB loss for two
  seconds, while permission/debugging loss stops immediately. `restart` is
  synchronized across the socket and UI callers.
- `HostLink.retryTransientProblem` uses `canRetryProblem` to clear disconnected
  busy/failed camera problems after 30 seconds. `SetupViewModel.onResumed`
  clears blocked camera problems after access returns; changed settings clear
  an incompatible-mode problem. Foreground refresh drives these retries.
- `scripts/build_adb_from_source.ps1` / `Get-VerifiedInput` verify pinned source,
  patches and development libraries, prepare retained source and compile adb
  with isolated MSYS2 UCRT64 plus the two USB DLLs with MSVC 2022.
- `scripts/package_adb_source_build.ps1` resolves the PE import closure, stages
  exact notices and complete source/relinking inputs, and creates `adb-build.json`.
  `scripts/stage_platform_tools.ps1` validates the reviewed manifest pin and
  every input/staged checksum. The metadata generator inventories adb and its
  distinct static/dynamic dependencies, including BoringSSL's extracted license.
- `scripts/test_adb_source_build.ps1` / `Read-Exact` / `Request-PrivateAdb` test
  protocol 41, device listing, loopback binding and hidden startup on a private
  port. Only its own child is stopped; the shared server remains untouched.
- `ActivateFixtureWindow` in `tests/gui_test_support.hpp` temporarily attaches the
  fixture thread's native input queue to obtain Windows foreground focus, then
  detaches immediately. Its bounded wait verifies both Win32 foreground and Qt
  activation before keyboard/idle assertions. GUI tests remain serial and need
  no human clicks; their original behavior assertions remain intact.
- `test::DesktopLease` in that header acquires a per-user `QLockFile` before
  QApplication through `OKUFLOW_GUI_TEST_MAIN`. All eight native GUI fixture
  executables use it, so separate CTest/build processes cannot own competing
  fixture windows. A refused lease exits 73 as an explicit interruption, not a
  skipped/passing test. Activation now uses a bounded Qt condition wait instead
  of an unconditional first 20 ms delay.
  `ActivationFailureMessage` marks an interruption only when a known foreign
  process owns native foreground; Qt/own-process or unknown activation failures
  remain ordinary failures, ineligible for retry credit.
- `okuflow_add_headless_cases` in `tests/headless_cases.cmake` registers the
  Assistant preparation fixture's inline private Qt slots individually, keeps
  all `_data` rows attached, applies a 30-second per-slot deadline and captured
  Windows logging, and exports the case list for runtime-input declarations.
  Source edits trigger reconfiguration; the fixture family remains selectable
  by its `assistive_preparation` label.
- `okuflow_add_gui_cases` in `tests/gui_cases.cmake` registers each main-window,
  annotation-overlay and AI-settings out-of-class Qt slot as a CTest selector,
  excluding lifecycle and `_data` helpers while retaining all data rows. It
  adds `gui` and an idle/input/layout/state label, a 30 s per-slot timeout,
  `RUN_SERIAL`, desktop resource lock and the Qt runtime/logging environment.
  The remaining GUI registrations also receive serial/resource-lock properties.
- `MainWindowTests::modesGridSearchAndGeometry` and
  `minimumViewportChromeFits` use bounded condition waits for focus and layout;
  diagnostics and assertions are retained. Native activation/pointer
  precondition assertions identify desktop interruptions explicitly.
  `desktopVisualLayouts` and `desktopVisualChat` exercise production widgets
  at sizes fitted by `FitVisualWindow` to the real logical work area minus
  native frame margins. Wide/tall geometry tests keep their original inputs.
  `PrepareVisualWindow` disables only the fixture HWND's DWM transition before
  showing it, avoiding translucent opening frames without changing OS settings.
  `CaptureWindows`/`CaptureBounds` include visible native peers;
  `CaptureUnobstructed` checks foreground ownership and physical native hit
  samples. `SaveScreenshot` refuses off-screen/obstructed/changing captures,
  checks raster dimensions and records `native-captures.jsonl`. Desktop visual
  cases require successful captures when requested and verify native chat
  footer reachability, owner following and Approval Details reading state.
  `NativeHitBelongsTo` verifies the primary chat footer's physical hit targets
  belong to its native window, including against other fixture peer windows,
  after the queued splitter resize settles within a bounded wait. The chat
  fixture sets/restores the application color-scheme hint and palette together
  to select matching native light/dark assets without changing the OS theme.
  Each layout row also asserts rejection of an intentional off-screen capture.
  `RenderWidgetTests::continuousResizingDoesNotStarvePresenter` waits for ten
  resize changes and a concurrent presenter update within the original 220 ms
  deadline, then verifies final dimensions and drains the presenter.
- `scripts/run_test_gate.ps1` discovers CTest commands with `--show-only=json-v1`
  and records runs with immutable per-attempt JUnit/console logs in a schema-2
  `ledger-v2.json` per build directory and configuration. `Get-RuntimeInputs`
  hashes shared Qt/system runtime DLLs, CTest, culture and relevant environment
  values once per run (desktop/DPI/boot only when a `gui` test is involved);
  `Get-TestFingerprint` adds the test executable, adjacent DLL/QM files, rooted
  arguments, CTest properties and `REQUIRED_FILES`. Fixture `--output`/`--trace`
  arguments name generated results and are excluded as inputs.
  `Initialize-HashCache`/`Save-HashCache` reuse SHA-256 values keyed by full
  path, length and LastWriteTimeUtc ticks. `Convert-LedgerInputs` stores each
  shared runtime map once per digest and `Limit-LedgerEvidence` keeps the current
  fingerprint plus three recent reusable results. Older Failed/Interrupted/
  Unfinished fingerprints retain compact blocker tombstones, including when
  the inputs are later reverted. A
  mid-run fingerprint change names the changed inputs without logging values.
  `-Test`/`-Label`/`-Select` produce a `SCOPED PASS/FAIL` verdict for exactly the
  selection; `-UncoveredByPreset` selects `gpu`, `cuda-variant` and names absent
  from another preset. Pending cases run in three partitions: parallel
  background, serial `gpu`/`codec` and serial `gui`.
  `Get-CaseResult` keeps assertions/mixed failures distinct from one explicitly
  marked desktop interruption, and reports optional CTest/Qt skips separately.
  JUnit retains up to 1 MiB per case; truncated passing output is Unfinished
  so a late Qt row skip cannot silently become reusable full coverage.
  `Save-Ledger` replaces the ledger atomically; attempt logs stay intact.
  Each CTest invocation also archives its detailed LastTest/LastTestsFailed logs
  before another selection can overwrite them.
  Failed/unfinished cases never auto-retry; `-Resume -RetryInterrupted` runs
  only an explicitly named recorded interruption once. Resumes rerun optional
  skips. If the latest unresolved result has different shared runtime inputs, resume refuses
  before executing another attempt; an explicitly fresh gate is required after
  diagnosis. Rebuilt per-test inputs can resume under the same runtime after
  repair. A missing/mismatched result cannot complete the gate.
- `cmake/PowerShell.cmake::okuflow_find_powershell` resolves a discovered or
  cached interpreter through its own process image for generated test commands.
  Its cache retains the launcher, resolves the current image each configure,
  and rediscovers stale versioned paths after Store updates.
  MCP script tests and `SharedSourceEquivalence.cmake` use it so Windows Store
  launch aliases cannot break executable hashing. Registered Qt fixtures force
  stderr logging; batch bookkeeping uses `AGENT_CUDA_*` to stay outside runtime
  environment fingerprints.
- `scripts/select_tests.ps1` maps changed files (`-Files`, or `-Base` plus
  working-tree and untracked changes) to CTest cases and build targets. It reads
  `ninja -t deps`/`-t query` transitively through objects and OBJECT/static
  libraries, adds `REQUIRED_FILES` and rooted-argument consumers, and applies
  `tests/test_impact.json` for full/none/extra-check rules. `Full` records the
  reason for a fail-safe full selection, including missing, stale or incomplete
  dependency data. `scripts/agent_check.ps1` initializes VS 2022, configures
  `msvc-cpu` when needed, builds the selected targets and calls the runner with
  `-Test`; GUI cases stay pending unless `-IncludeGui`. `scripts/test_select_tests.ps1`
  uses `Assert-Selection` against a private synthetic CMake/Ninja project.
- `tests/shared_sources.cmake`: `okuflow_share_test_sources` builds OBJECT
  libraries for consumers with identical compile contracts
  (`okuflow_test_compile_contract` checks them at configure time), and
  `okuflow_share_equivalent_sources` declares explicit canonical groups whose
  consumers were verified by preprocessing, keeping executable MOC headers local
  and recording the original consumers. In `cmake/SharedSourceEquivalence.cmake`,
  `okuflow_check_shared_source_overrides` rejects per-source settings and
  `okuflow_record_shared_source_environments` records checked target witnesses
  and declares shared source bytes and project headers as CTest REQUIRED_FILES.
  Thus impact selection includes the guard and its prior pass cannot survive a
  changed preprocessing input.
  `cmake/check_shared_source_equivalence.ps1.in` backs the headless
  `shared_source_equivalence` CTest: it replays each resolved MSVC environment,
  compares code-generation flags and preprocessing SHA-256 values, and reports
  the first mismatch. `tests/precompiled_headers.cmake` applies the optional
  `OKUFLOW_USE_PCH` header set to C++ only.
- `.github/workflows/windows-checks.yml` installs the pinned Qt/MSVC toolchain,
  using `scripts/initialize_windows_ci_toolchain.ps1` to discover only VS 2022
  with `vswhere`, invoke Microsoft's installed developer PowerShell for x64,
  verify the compiler belongs to that installation, and export changed environment
  values through the Actions environment file. `-EnvironmentFile` permits a
  private desktop-free validation file; no JavaScript action is required for MSVC.
  The Android host job pins `ubuntu-24.04` to avoid moving-image migrations.
  The workflow builds the CPU preset and runs background/native GUI checks on a disposable
  GitHub Windows VM. Pull requests run `select_tests.ps1` against the PR base:
  docs/site-only changes skip compilation, scoped changes build and test only
  the selected targets and cases, and full selections keep the complete run.
  Pushes to main and dispatch `all` stay complete. sccache (with embedded `/Z7`
  debug information) caches compilation; the incremental `build/msvc-cpu` cache
  is restored by every run but saved only by pushes to main. The Android host job
  runs only when Android files change. `scripts/run_windows_ci_gui.ps1` refuses local execution;
  `OkuFlowCiDesktop.InputName()` reads the native input desktop with an exact
  scoped handle, refusing session 0, noninteractive/locked desktops and Qt
  offscreen overrides. It discovers registered CTest cases by label/exact name.
  `all` fills the existing CPU gate's GUI entries; narrow diagnostics produce
  separate JUnit/logs without whole-gate credit. Both paths retain serial
  execution and every Qt data row. Workflow artifacts preserve the results.
  `-PrepareDesktop` calls `EnsureDisplaySize()` only after the remote ownership
  and interactive-session guards. Its explicit 220-byte `DisplayMode` preserves
  native DEVMODEW layout, requests width/height after `CDS_TEST` and changes only
  the disposable VM's current mode, without saving registry defaults. Both
  preparation and test execution verify at least 1920Ã—1080; an unsupported or
  unchanged mode stays a failed prerequisite. `desktop` scope skips SDK/builds.
  `MainWindowTests::inspectorContrastAppearance` retains its contrast assertions
  and saves the exact grabbed light/dark image before checking rendered text.
- `scripts/test_test_gate.ps1` uses `Assert-Contract`, `Invoke-Gate` and
  `Read-Ledger` with private command-line CTest fixtures to check resume/retry,
  partial coverage, failure/skip handling and inventory invalidation. It also
  checks cached-alias discovery, batch bookkeeping and refusal to retry an
  unresolved test under changed environment inputs. It creates
  no Qt windows and is run explicitly when the runner changes, not redundantly
  inside every CPU/CUDA suite. `agent_build.bat --retry-interrupted` restores the
  build's VS environment without compiling or publishing; the ordinary matrix
  proceeds to CUDA after a CPU CTest failure, retaining both inventories.
- `phone_camera_contract`, `phone_retry_policy` and settings tests cover
  delayed arrival, replacement devices, explicit selection, cached lenses,
  endpoint overrides, old/new probes, busy probes and cancellation against
  isolated sockets. `LinkPolicyTest` adds peer, bind deadline and wire-code
  and transient retry checks; Android now has 54 JVM tests.
  `FakePhoneAdb` optionally prebuffers a request before `Serve` for the
  `alreadyBufferedFakeAdbRequestIsNotDiscarded` regression; the handler checks
  `bytesAvailable()` before its bounded readiness wait, so queued request data
  does not require a second network event. This is a test-server repair only.
  `FakePhoneAdb::ArrivalMode` also gates a two-client listener backlog for
  `alreadyQueuedFakeAdbConnectionsAreHandled`; the worker drains Qt's pending
  queue before blocking for another arrival. Its optional `healthProbeReceived`
  callback fires after acknowledging the shell health request.
  `canceledUsbRecoveryStopsPromptly_data` supplies immediate/delayed startup;
  `canceledUsbRecoveryStopsPromptly` uses that callback to cancel and measure the
  unchanged 500 ms completion limit independently of worker-start scheduling.
  Process-scanning recovery and
  temporary OkuFlow-named launcher/MCP/native fixtures share the CTest
  `okuflow_process_identity` resource, retaining fail-closed provenance checks.

## GUI launch and explicit capture controls

- The Windows `oku_flow` target uses the GUI subsystem. `main` attaches only an
  existing parent console when `--console` is present, then restores its CRT
  output streams for diagnostics; ordinary launch never allocates a console.
- `CameraDescriptor.phoneAppReady` records a successful companion capability
  handshake. `ResolveSavedCameraIndex` still honors saved identity/legacy index;
  only an unconfigured first launch prefers a ready companion over index zero.
- `PhoneCameraTransport.SetTorch` coalesces a session-only Boolean command;
  its bounded Boolean-returning overload takes a nonnull shared atomic
  cancellation token and positive `steady_clock` milliseconds deadline. The
  native one-argument void entry point is preserved. `QueueTorch` stores desired
  state/token/deadline/generation under the transport mutex; superseding native
  or actor commands cancel the prior actor token. `SendPendingTorch` and
  `SendTorchCommand` guard the actual socket write against cancellation, expiry
  and stale generation, including a record already dequeued. `RetirePendingTorch`
  revokes pending tokens on cancellation, retry and worker exit.
  `ApplyTorchAcknowledgment` updates actual state only from phone status, never
  from admission or cancellation; bytes already emitted can still be acknowledged.
  `MediaCapture.SetPhoneTorch` has the corresponding bounded overload and rejects
  absent/non-running phone sessions and split-stop access without retaining app
  references. Deadlines use the steady-clock epoch, not wall-clock/Qt elapsed time.
  `TorchAvailable`/`TorchEnabled` expose atomics from capture metadata. A fresh
  connection clears pending light requests and sends explicit light-off before
  Start, including HEVC fallback. `MediaCapture.SetPhoneTorch`,
  `PhoneTorchAvailable` and `PhoneTorchEnabled` provide UI-facing no-op/false
  behavior for webcams. `MainWindow.phoneLightButton` is bound through
  `UIStateManager`; `OkuFlowApp.UpdatePhoneLightUi` shows it only for a selected
  phone, enables it from hardware availability, and reflects actual state.
- `CameraSettings.forNewHostSession` clears only torch. Store reads always start
  with light off, updates remove the legacy persisted torch key, and service
  startup/binding/Stop/disconnect reset it. The Android settings show a Windows
  control hint; only an explicit host command requests light on. `CameraEngine`
  emits optional `torchAvailable`/`torchEnabled` metadata from Camera2 results.
- `scripts/agent_build.bat` accepts `OKUFLOW_AGENT_RELEASE_BUILD_DIR` for the
  same release preset in an isolated directory. CPU/CUDA presets are unchanged.

## Audit-49 unused-code cleanup

- Removed 34 reviewed function definitions: the 26 census candidates, five
  exclusively unused CUDA helpers, and three obsolete recording/application
  pairing functions. Declarations, CPU/Maxine fallback variants, pairing state,
  and write-only CPU raw dimensions were removed together.
- Recording uses `AddSceneFrame` / `AddGpuSceneFrame` and `QueuedFrame`
  snapshots. Completed photo/assistant readbacks no longer enter an empty
  recording map. `RecordingDropCounts` no longer contains `pairingMiss`.
- Current linear-buffer effects, canonical viewport presentation, shared fence
  ordering, and the 17 test-only APIs remain. The removed gradient, surface
  black/white, surface zoom and blend kernels had no active launcher.
- `OKUFLOW_AGENT_EXCLUDE_GUI=1` selects explicitly partial desktop-free
  validation in `scripts/agent_build.bat`; the default still runs every test.
  See [review 49](../improvement_ideas/49-unused-code-census-2026-10-11.md)
  for the measured baseline, removal set and validation limits.

## Audit-50 alternating repairs

- `AnnotationOverlay::eventFilter` intercepts keyboard input only while Draw is
  active and visible. Scoped `filteringKey_` / `forwardingKey_` distinguish an original
  filtered delivery from an explicit owner forward. An unhandled filtered
  navigation key follows Qt's original route; an explicit forward reaches
  the owner once, even when ignored. Unhandled filtered keys retain their
  original acceptance for native child controls. Inline text entry owns
  bubbled navigation. `forwardedPanKeys_` pairs accepted pan presses with
  physical releases, ignores auto-repeat releases, and
  `ReleaseForwardedPanKeys` flushes them on disable/hide/focus loss or window
  deactivation. Local Draw shortcuts and selected-ink edits retain ownership.
  The inline editor installs this filter and retires pending text on actual
  FocusOut, including untouched empty editors that emit no editingFinished.
  CommitPendingText guards retire state before hiding; typed labels commit once.
- `annotation_key_routing_headless` exercises the production overlay using
  Qt's offscreen platform. A bounded redispatch fence reproduces the previous
  feedback loop without exhausting the stack. This test is desktop-free;
  native focus/layout validation remains in the separate GUI group.
- Author/reviewer evidence and remaining priorities are recorded in
  [review 50](../improvement_ideas/50-alternating-opus-repairs-2026-10-11.md).

## Audit-47 sequential repair contracts

- `RecordingAudioBacklog` bounds outstanding PCM by block count (128), bytes
  (4 MiB) and accumulated PCM duration (2 s), separately from video admission.
  The configured Microsoft raw LC AAC contract emits 1024 PCM frames per output
  packet. Confirmed packets retire cumulative PCM frames (not input blocks),
  retaining two packets of conservative credit. Retirement requires fresh
  output progress: unchanged padded/partial-packet credit cannot expand into
  later tiny PCM blocks. Failed/regressed/impossible statistics cannot earn
  credit. Capture-clock/encoded-clock gaps and drift do
  not influence admission; they remain a separate playback-quality issue.
  `RecordAccepted` follows successful submission; `Reset(bytesPerPcmFrame)` runs
  on CPU/native-GPU startup with the negotiated channel alignment.
- Worker-only `VideoRecorder::CanAcceptAudioFrame` validates PCM size,
  alignment, duration and arithmetic before querying encoder statistics.
  `RecordingManager::WriteAudioFrame` checks both budgets first and ends the
  session explicitly at exhausted/invalid admission, preserving the existing
  paired-finalization/watchdog contract. `AddAudioFrame` also checks admission
  for direct callers. Estimated input backlog is bounded; internal MFT/driver
  allocations and synchronous COM/finalization latency remain outside this
  contract. Statistics must confirm progress for sustained recording.
  `AudioFormat` remains validated by sink negotiation; alternative AAC MFTs
  with a different packet contract are outside this policy's verified scope.
- `RecordingDropCounts::audioQueueOverflow` / `audioEncoderBacklog` describe
  microphone blocks and remain excluded from video `Total()`. Session reset,
  `DropCounts` and `BuildDropSummary` preserve this distinction.

## Audit-46 follow-up contracts

- `InitializeLegacyCameraIdentity` records the actual opened index/link only
  after a successful automatic start of an index-only profile; an existing
  preferred link survives fallback. Explicit selection replaces the preference.
- `EnqueueCaptureFrame` retains bursts up to six pending arrivals, then retires
  the saturated queue and retains the newest arrival. Its exact retired count
  and caller-owned retired storage keep lease release outside the ingress lock.
  `NotifyCaptureFrames(received, overwritten)` counts arrivals and losses
  independently, including losses of earlier batches.
- Capture/recording D3D11 video processors explicitly disable driver automatic
  enhancement; requested color conversion remains active.
- `RecordingEncoderBacklog` admits AV1/H.264 video with a 12-sample / 128 MiB
  estimated input-payload budget per writer. `CanAcceptVideoFrame` queries
  encoded progress on the worker and includes pending/incoming samples; failed
  or inconsistent statistics grant no new completion credit. The manager checks
  both writers before writing either timeline and drops pressured pairs together.
  `MF_LOW_LATENCY` is requested with conditional driver/quality behavior. AAC
  input admission is now separately bounded by audit 47; internal AAC/driver
  allocation and blocking finalization time remain separate limits.
- `RecordingEncoderBacklog::CanStart` requires two slots and two input payloads
  to fit; `VideoRecorder::InputPayloadBytes` computes BGRA/NV12 sizes without
  overflow. `InputFitsEncoderBudget` applies the 64 MiB per-input ceiling
  before CPU/native-GPU startup and before compatibility fallback creates a
  sink. Oversized inputs fail explicitly; this is separate from pool/driver
  capability checks. Failed native output is preserved unless confirmed empty.
- Native GPU samples own a separate NV12 output, rather than retaining the
  source BGRA pool lease through encoder buffering. `HasPendingGpuReads` and
  worker-only `PollGpuInputReads` retain source/reader graphs until confirmed
  conversion completion; Pending/Unknown remain quarantined. The manager's
  `RetireGpuInputReads` also polls while no frame can arrive, waking an idle
  worker at 10 ms only when pending readers exist. Writer/worker stage tags keep
  those driver calls visible to the existing watchdog; the UI never polls COM.
  Ordinary admission uses `DONOTFLUSH`; when no video is queued, the worker
  allows asynchronous query-command flushing to avoid stalled reader progress.
  This adds no completion wait or sink-sample submission.
- `NotifyCaptureGpuUploadBusy` counts each definitively dropped arrival once.
  `RecordingDropCounts::captureGpuUploadBusy` and `encoderBacklog` join `Total`;
  `encoderStatisticsUnavailable` is a diagnostic subset excluded from `Total`.
  Full/unobservable queues produce visible notices.
- `SetLiveTextCoalesced` keeps its first deadline while replacing the pending
  value; immediate `SetLiveText` cancels pending publication. Percentile work
  moves to `UpdatePerformanceDiagnosticsLabel`, a widget-owned 400 ms timer
  that skips hidden diagnostics and refreshes after show even without new frames.
- `RetranslateWidget` visits only the receiving widget for Show/Polish; full-tree
  translation remains for explicit language changes. Exact keys skip formatted
  matching; untranslated templates skip regex while preserving other translators.
- `ConversationTranscriptLoadFailed` forwards as `AssistantTranscriptLoadFailed`.
  `pendingAssistantHistoryThreadId_` owns the loading placeholder; matching
  terminals retire it. `CancelAssistantHistoryLoad` clears it before prompt/New/
  delete while preserving the thread used for follow-up prompts.
- Photo recovery lists transaction artifacts only. Nominated finals' timestamps
  still participate in legacy staleness checks; ownership/locking rules remain.
- `LaunchOpaqueAlphaLinear` sets only alpha after a successful Maxine transfer,
  before cache publication. The camera-scene shader is opaque; annotation
  composition retains its texture alpha. Private allocations are not exposed
  before the successful transfer and alpha pass.
- Mask hysteresis retains soft coverage, holding prior values in the uncertainty
  band. Text composite permits exact equal-pitch src/dst aliasing; automatic mixed
  scenes in place return before pixel loads. Replacement consumes only the mask,
  and the existing mode-0 blend API remains.

Actual validation/limits: [review 46](../improvement_ideas/46-additional-opus-review-2026-10-11.md).

## Audit-45 implementation contracts

- `PersistentSettings::cameraSymbolicLink` serializes physical selection;
  `ResolveSavedCameraIndex(...)` resolves case-insensitive device identity,
  handles a missing preferred device, and migrates legacy index-only settings.
  startup prefers it over `cameraIndex`, retaining the legacy index when no
  link exists. `MediaCapture::LastSymbolicLink()` now snapshots the requested
  descriptor before open, including failure, and survives Stop for retry.
- `IsSupportedCaptureLayout(...)` rejects unsupported subtype/extent/stride
  before dynamic format adoption. `CopyMappedCaptureTexture(...)` uses the
  texture's allocation height for NV12 UV, validates visible bounds/pitch, and
  swaps channels only for explicit RGBA. `CopyGpuFrame` accounts for source
  mip/subresource extent; unsupported changed formats enter error/recovery.
- `CudaInteropSurface::LastFailureWasBusy()` reports an upload-admission
  outcome separately from faults. Three `PinnedUploadSlot`s are queried
  before work submission; `OkuFlowApp::cudaSubmissionBusy_` retains the prior
  scene and cancels only the unused fence reservation, without CPU recovery.
- `ComputeContainingSourceCrop(...)` requires a supported integer crop to
  contain the whole requested view. `CanonicalizeSuperResViewTransform(...)`
  gives inference/presentation one SR-only geometry, snapping pan by at most
  one source pixel and mapping advertised 1.33 to exact 4/3. Unsupported crops
  use existing fallback. Maxine `loadedStrength_` participates in load reuse.
  `IsSuperResInputHeightSupported(...)` in `superres_scale_policy.hpp` filters
  each candidate before SDK loading: 4x accepts input through 540p, 3x through
  720p, and 2x/1.5x/4/3x through 2160p. `UpdateSuperResCache` keeps scanning
  lower factors when a candidate exceeds its model limit, preserving desktop
  zoom. Other SDK resolution checks still apply. The spatial geometry fixture
  covers the 4000x2256 phone regression and model-height boundaries.
  `UpdateSuperResCache` prepares an exact CPU descriptor; it never calls the
  SDK or borrows the processing buffers for inference. Private completion timing
  contributes only to its matching source/output/strength configuration,
  including stale jobs. Unrelated text controls do not reload the effect.
- `VirtualTripodPairInsideRoi` subtracts recovery keyframe origin for anchor
  ROI selection. `VirtualTripodRelativeFallbackKernel` rejection holds both
  correction and relative motion. CLAHE clamps both independently derived
  neighbor tile indices. `LaunchTextLocalMean` uses squared-workspace-free
  box-kernel specializations when later processing replaces statistics;
  full statistics remain when consumed. Explicit Adaptive Text skips replaced
  Smart Sharpen output; automatic mixed mode retains it. NIS edge maps reuse
  the already loaded 6x6 luma neighborhood.
- `PipelineOrchestrator::MarkSceneDirty()` marks camera output without starting
  the user-motion tail; `MarkViewportDirty()` retains motion behavior.
  `InteractionController::ApplyZoom` explicitly invalidates keyboard zoom.
  `RunFrameTick` drains completed readbacks before idle early returns; GPU
  publication/rotation/stop clear stale CPU presentation buffers.
- `AnnotationModel::RotateClockwise`, `AnnotationOverlay::RotateClockwise`,
  and `MainWindow::rotateAnnotationsClockwise` rebase ink and undo/redo on
  image rotation, end active gestures, and retain upright text labels.
- `RecordingManager` launches its worker after full member/UI setup.
  `QueuedFrame` carries value snapshots of annotation strokes;
  `AddSceneFrame` accepts these snapshots on the live recording path, and
  `ComposeRecordingAnnotations` renders only processed output after resampling.
  Review 49 removed the unused pending-original map and its readback pairing
  callbacks; photo and assistant readbacks retain their independent handling.
  `activeCanvasMode_` captures selected mode at session start for both clone
  resolution and worker setup. `ConvertRecordingBgraToNv12` normalizes CPU
  fallback into the GPU writer's BT.601 limited-range NV12 contract, averaging
  2x2 chroma; `AddFrame` never labels BGRA bytes as NV12.
- `AssistiveRuntime::CancelVlmPreparation` retires request ownership before its
  terminal event; `CompleteAssistantPreparation` rejects stale generations and
  removes only their temporary files. `CancelAssistantConversationLoad` delegates
  to `CodexAppServerClient::CancelConversationLoad`; `conversationLoadGeneration_`
  rejects history replies after newer loads/turns. App callbacks also validate
  selected thread and pending answer/capture state.
- `SettingsController::StorageOperations` injects read/write/remove/save adapters
  for deterministic regressions. `PrepareProtectedSecrets` queues deletion ids;
  `AssistiveSettings::pendingCredentialRemovals` persists them before deletion
  and clears them only after successful removal and settings cleanup.
  A replacement key uses a fresh opaque id if the old id is still pending
  deletion, preventing a persisted old intent from deleting the new key.
- Assistant append cursors no longer replace selected editor cursors; scroll
  follows only an unselected tail. Draw tool/action rails use bounded scroll
  areas.
  `AiSettingsDialog::result` retains saved voice fields without a discovered voice.

Validation results and remaining native/hardware acceptance limits are recorded
in [audit 45](../improvement_ideas/45-codebase-audit-2026-10-11.md). Encoder-resource
pooling and per-device NIS coefficient initialization remain deferred; these
source changes make no measured throughput or latency claim.

## App Module

### Windows build and packaging helpers
- `scripts/build_and_run.bat`, `scripts/build_release_bundle.bat`, and
  `cmake/CMakePresets.json` default to Qt 6.12.0; translation validation and
  startup profiling use the same SDK default.
- `scripts/build_release_bundle.bat` copies and requires `START_HERE.txt` from
  `docs/portable_start_here.txt`, using the existing exact-byte copy helper.
  It explains the public `oku_flow.exe` launch path and supporting application
  file. Release metadata inventories and hashes the guide automatically.
- `scripts/build_release_bundle.bat` stages the five required Qt module SPDX
  documents plus PDF when installed. `validate_staging_bundle` also requires PDF
  metadata if `Qt6Pdf.dll` is deployed.
  `OKUFLOW_BUNDLE_BUILD_DIR` optionally redirects its build tree outside the
  synced checkout; the full build/test/staging gates still run.
  `resolve_cuda_license` accepts the selected toolkit's `EULA.txt` or newer
  `LICENSE` layout while retaining the deployed notice's existing filename.
- `scripts/generate_release_metadata.ps1` validates and links the staged
  documents using the unique module name and its actual SPDXID, including
  Qt 6.12's hash suffix. PDF joins the required document list when its runtime or notice
  is present; missing metadata for a deployed PDF runtime remains an error.

### `scripts/lecture_camera.ps1`
- Start/Stop/Status/Download operations implement the local OBS lecture camera.
  The `.bat` starters expose one-click camera/app launch and shutdown;
  `lecture_camera_sample.json` retains the reviewed default source/provenance.
- `Download-Lecture` checks the publisher download's byte count and SHA-1,
  or invokes external yt-dlp for an explicit replacement URL. It stages to a
  unique file before replacing the destination and writes source/attribution
  and SHA-256 beside the completed media.
- `Get-OwnedProcess` validates recorded PID, start time, and executable inside
  the scratch root. `Stop-OwnedObs` stops Virtual Camera over WebSocket,
  requests normal exit, and force-stops only the verified owned process after
  a timeout. Startup failures use the same cleanup.
- `Connect-Obs`, `Receive-ObsMessage`, `Send-ObsMessage`, `Get-Base64Sha256`, and
  `Invoke-ObsRequest` implement bounded authenticated obs-websocket v5 control.
  `Write-JsonFile` serializes private scenes, plugin config, and ownership.
  Start creates a separate portable OBS copy for each installed executable
  hash, fits a looping media source to a 1280x720/30 FPS output, checks decode
  and camera state, and persists the selected local file and start offset.
- `Install-MediaFoundationOutput` verifies the pinned DroidCam output plugin
  package, extracts/selects its x64 DLL, and installs it into private OBS only.
  The existing DroidCam Video driver exposes this scene to Media Foundation;
  built-in OBS Virtual Camera remains available for DirectShow clients.
  `Start-OkuFlow` opens the deployed bundle only when it is not already running.

### `include/okuflow/app/app.hpp`
`okuflow::OkuFlowApp`
- Composition root for Qt/COM/Media Foundation startup and the application
  services. It delegates scheduling, settings/presets, widget synchronization,
  assistive behavior, interaction, and paired recording to focused manager
  classes instead of implementing those policies directly.
- Public API:
  - `OkuFlowApp(int& argc, char** argv)`
  - `~OkuFlowApp()`
  - `bool Initialize()` â€” performs fallible platform, UI, presenter, camera,
    settings, and service initialization after construction
  - `int Run()`
- Implementation is separated by responsibility:
  - `src/app/app_bootstrap.cpp` â€” lifecycle, service construction, signal wiring
  - `src/app/app_startup.cpp` â€” independent initial-camera worker, completion
    adoption, first-presentation timing, and opt-in startup profiling
  - `src/app/app_pipeline_runtime.cpp` â€” camera-clock processing, persistent
    scene publication, viewport-only presentation, readback, photos, and
    annotated viewport snapshots
  - `src/app/app_interaction.cpp` â€” input routing, status, focus mapping
  - `src/app/app_controls.cpp` â€” processing-control slots
  - `src/app/app_settings.cpp` â€” presets and persistent settings. A clicked
    preset's ID remains authoritative through control normalization; unresolved
    presets repaint from the applied configuration. Startup restores the saved
    ID before applying its live configuration.
  - `src/app/app_assistant.cpp` â€” Assistant and assistive actions
  - `src/app/app.cpp` â€” intentionally empty compatibility translation unit
- The low-level CUDA/presentation entry points remain private `OkuFlowApp`
  methods; `PipelineOrchestrator` schedules them through callbacks. The
  presenter owns GPU fence ordering. Camera or rotation changes never reset
  that live fence timeline. Repeated CUDA
  failures drain both APIs before recovery; surface teardown disables interop
  only after the drain succeeds.
- `StartCameraCapture(..., bool backgroundStartup = false)` starts the initial
  device on a worker after settings/UI construction and before window show,
  overlapping device opening with native presenter initialization. Manual
  camera changes and reconnects retain the existing synchronous path.
  `QueueInitialCameraStart(...)` recreates the camera activation in its worker
  COM apartment using the camera's symbolic link for physical cameras. Phone
  serial, camera ID and transport ID are copied as values and bypass MF device
  activation, preserving the optional phone branch at initial startup. Native modes are obtained
  from that same streaming reader; startup no longer opens a preliminary reader.
- `CameraStartupJob` owns an independent `MediaCapture`; `CameraIngress` revokes
  completion delivery when a session ends. `CompleteCameraCaptureStart(...)`
  adopts the capture, restores camera controls, publishes negotiated modes and
  notices, and delivers any runtime failure received before adoption.
  `StartupWorkerTracker` retains active/abandoned state beyond app lifetime,
  preventing process-global Media Foundation teardown under a pending driver.
  The job owns its callback/ingress references and releases them before
  decrementing that tracker, including canceled or discarded completions.
- `ConfigureStartupProfiling()` enables a 20 ms Qt heartbeat and automatic
  report/normal exit only with `--startup-profile=<path>`. Profiling loads and
  saves `settings.json` beside that report, explicitly isolating Windows
  known-folder settings. `--startup-profile-ms=<ms>` defaults to 15000 and
  clamps to 1000â€“600000, allowing ten-minute manual cable-recovery sessions;
  `--startup-profile-legacy-camera` reproduces the former
  synchronous preliminary-reader sequence for comparison in the same build.
  `WriteStartupProfile()` atomically writes timings, actual camera arrivals,
  ingress drops, processed/presented counts, missed admission attempts, stage
  p95s, and negotiated camera details. `RecordStartupFirstPresent()` runs only
  after successful presentation. The timing origin excludes executable loading;
  presentation submission is not display scanout or sensor-to-photon latency.
- `ConfigureStartupProfiling()` also schedules one opt-in native UI capture
  when `OKUFLOW_UI_CAPTURE_PATH` is valid; `OKUFLOW_UI_CAPTURE_DELAY_MS` selects
  its delay. Private `CaptureNativeUi()` requires a settled camera presentation,
  an on-screen unchanged frame, and `UiCaptureRegionIsClear()` checks foreground
  ownership and foreign-window overlap before and after `QScreen::grabWindow`.
  Visible owned native peers repaint 100 ms before the grab to settle partial
  layered backing-store updates; failure diagnostics omit foreign window titles.
  The raw composed PNG includes native sibling chrome and is saved atomically;
  no cursor movement, cursor hiding, or pixel edits are performed.
  See [`ui_capture.md`](ui_capture.md) for the isolated-session workflow.
- `ApplyPersistentSettings(settings::PersistentSettings settings)` takes an
  immutable value snapshot before restoring controls. Startup handlers can
  request saves into the controller; those reentrant writes must not overwrite
  the saved mode, panel width, framing, or processing values still being read.
- `pendingSceneCaptureClock100ns_` retains the capture timestamp of the newest
  processed scene through a busy presentation slot. Only its first successful
  presentation records a latency sample; camera switches/stops clear it.
- `TryProcessRawFrameWithCuda(...)` reuses already-materialized BGRA pixels
  through the existing host-upload tail during startup validation or explicit
  CPU readback. It first polls the previous camera-copy lease; frames with no
  CPU data continue through direct D3D11/CUDA transfer. This avoids duplicate
  startup conversion without changing validation or steady GPU ownership.
  A negative native CPU stride requires normalization only for system-memory
  input; it must not reject a GPU texture whose layout is defined by its
  resource and subresource metadata.
- Test `SimulatedNativeMoveChat::event(...)` filters spontaneous mouse moves
  only inside the simulated native-drag fixture, so physical pointer movement
  cannot overwrite injected drag coordinates. It retains synthetic events and
  normal window-deactivation/ungrab handling; held-pull fixtures await native
  activation before injecting input.
- `CaptureGpuPending` in `app/capture_handoff_policy.hpp` distinguishes no
  pending work, CUDA copy ownership, and a D3D11 query. `CaptureHandoffPolicy`
  counts consecutive query deadlines, resets the streak on actual readiness,
  and latches session demotion after three. Camera startup/stop reset it.
  Demotion selects existing safe-copy input without changing the user's camera
  preference or releasing a pending query/lease. CUDA copy waits retain their
  frame until completion or the existing ownership fault; the 25 ms D3D11
  deadline cannot authorize reuse of an unfinished CUDA copy.
- `PresentLatestCudaScene()` preserves the full-scene `annotationTransform` for
  live annotations, annotation captures, and recording ink. Remapping into a
  cropped SuperRes cache changes texture sampling only; stored strokes remain
  normalized to the full processed scene and scale to the recording canvas.
- Photo-pair JPEG encoding/writes and annotation compositing/PNG writes run in
  a bounded two-thread application image-I/O pool. The UI thread deep-copies
  the source frame before dispatch, and worker completion returns through a
  queued callback for notes/status updates. Pool saturation is reported rather
  than queued without bound, and shutdown drains active jobs before dependent
  services are destroyed.

### `include/okuflow/app/ultra_clear_text_controller.hpp`
`okuflow::UctGeometryKey`, `okuflow::UctFrameContext`, `okuflow::UctTiming`,
`okuflow::UltraClearTextController`
- UI-thread Ultra Clear Text runtime. `OnPresented(...)` runs on every
  presentation: it forwards the transform to the overlay, starts a new
  geometry generation (and clears everything) when `UctGeometryKey` changes,
  pauses for unsupported composites, enforces freshness and returns whether one
  asynchronous viewport copy is wanted (80 ms probe interval, one outstanding).
- `OnReadbackRequested`/`OwnsReadback`/`OnReadbackCompleted` attach to the
  presenter readback ring; `OnCpuViewport` serves the CPU presentation path.
  Probes revalidate accepted lines and decide latest-only submissions; results
  return through queued calls and are admitted by `uct::OverlayModel`.
- Off by default with a shared saved opt-in, gated by Text Clarity.
  `OkuFlowApp::InitializeUltraClearText()`
  and `OkuFlowApp::UltraClearTextContext(...)` (in
  `src/app/app_ultra_clear_text.cpp`) provide production wiring; the
  controller member is declared last in `OkuFlowApp` so it retires first.
- `UctFrameContext::compositeFrames` remains display truth for annotations
  and observations. `unsupportedCompositeFrames`, `UctSceneSnapshot`'s matching
  flag and the optional provenance arguments distinguish UCT eligibility.
  CUDA Temporal Smooth has paired neutral analysis; Extra Stable uses the
  capture-owned hold record. Recovery crossfades and CPU debug composites
  cannot confirm text. `OnDeviceScene` and
  `ImmutableStamp` carry the paired image's blend provenance without restamping
  it from a later display receipt. Unsupported stabilized composites retain
  their Extra Stable pause reason; debug fallback names text recognition pausing
  in the debug composite view. `SetMediaOverlaySuppressed` carries a separate
  `extraStable` reason and refreshes the accessible status on reason changes,
  even when the suppression boolean stays the same.

- `uct::HoldProvenanceRecord` / `HoldProvenance` / `HoldDisplay` name the exact
  acquisition behind live, held or crossfaded pixels. `DecodeHoldProvenance`,
  `ApplyHoldProvenance` and `EvidenceAcquisition` reject inconsistent records.
  `LaunchRecordBumpHoldProvenance` mirrors the display kernel on its stream;
  the analysis pool owns a pinned 32-byte copy read only by workers. Held
  repeats never corroborate pending words. `OverlayModel::ApplyChecks(...,
  renew=false)` revokes display without advancing proof during a crossfade.
- `CudaInteropSurface::InjectUctAllocationFailureForTesting(UctAllocationFault)`
  and `uct::AnalysisPool::InjectFaultForTesting(InjectedFault)` are explicit,
  bounded fixture seams for EMA/replay allocation pressure and unknown copy
  completion. Normal operation never arms them; tests verify latch/reset,
  atomic image/record refusal and quarantine without exhausting real VRAM.
- Composite identity revokes display even when worker checks are missing or
  budget-limited. The controller retains the composite probe as an admission
  barrier so a late older recognition completion cannot revive text over it.
- Test-only `OkuFlowApp::SeedPendingLiveHoldForTest(bool)` stages the same
  completion boundary used by asynchronous clean-frame retention. Native
  fixture rows verify refusal returns an active recording to Live, including
  camera loss, and marks a finishing recording unavailable.

### `include/okuflow/app/pipeline_orchestrator.hpp`
`okuflow::TimingPercentiles`
- Rolling timing summary with nearest-rank `p50Ms`, `p95Ms`, `p99Ms`, and
  `sampleCount`. `IsValid()` distinguishes an empty window.

`okuflow::FrameTimingStage`
- Independent CPU-side timing windows for `CaptureHandoff`,
  `CpuPreparation`, `CudaSubmission`, `Presentation`, and `RecordingClone`.
  `CudaSubmission` measures the host call duration only; sampled CUDA events
  provide actual GPU execution boundaries.

`okuflow::PipelineOrchestrator`
- Owns the two-clock scheduler, active/idle viewport rate policy, elapsed-time
  tick timing at nanosecond precision, dirty/presentation generation state, display-rate clamp
  reporting, camera reconnect backoff, and repeated CUDA failure count. The
  D3D12 presenter owns the shared fence timeline.
- Camera-clock work runs only when `tick(...)` consumes a fresh capture frame.
  Viewport motion can present the last completed CPU or GPU scene at the
  effective display rate without advancing temporal effects, recording,
  or SuperRes.
- Public controls include `Start()`, `Stop()`, `UpdateTimerPolicy()`,
  `NotifyCameraFrameAvailable(int delayMs = 0)`,
  viewport rate/fit setters, dirty/motion/present notifications, measured rate
  and timing accessors, reconnect methods, and CUDA-failure methods.
  `FrameTickPercentiles()`, `CaptureToPresentPercentiles()`, and
  `StagePercentiles(...)` summarize independent 240-sample windows;
  `RecordCaptureToPresentSample(...)` and `RecordStageSample(...)` accept only
  finite non-negative samples. Processing-budget warnings compare p95 camera
  work with the negotiated camera frame period and include every available
  stage p95 so an aggregate delay is not mislabeled as CUDA work.
- `NotifyCameraFrameAvailable(...)` is thread-safe and coalesces Media
  Foundation callback wakeups onto the Qt event loop. Immediate wakeups drive
  camera-clock work from frame arrival instead of polling a single mailbox at
  the producer's nominal frame rate; a short delayed wakeup retries retained
  GPU work without blocking that event loop.
- `Callbacks` supplies fresh-frame processing, motion/presenter/camera state,
  active display refresh discovery, and a one-shot explicit-rate clamp notice.
  `presentPendingScene` runs after every `tick(...)`, including camera-handoff
  early returns. `OkuFlowApp::PresentPendingScene()` selects a dirty cached
  CPU/GPU scene or a presenter's resize redraw without advancing temporal
  processing, SuperRes, or paired recording. A fresh camera frame's successful
  presentation clears dirtiness, so the callback does not redraw it twice.
- `IsViewportInteractionActive()` reads keyboard/joystick motion, a held mouse
  pan, or a dedicated 150 ms navigation tail. Camera-only `MarkSceneDirty()`
  and generic redraw/readback `MarkViewportDirty()` calls do not extend it.
  Actual pan/zoom calls explicitly notify motion. The timer policy and per-camera-frame Maxine
  admission use this same predicate; no SDK call is begun while it is true.

### `include/okuflow/app/recording_session_info.hpp`
`okuflow::RecordingSessionInfo`
- Recording-owned Qt metatype carrying a stable QUuid `id` and filename
  `timestampToken`. RecordingManager creates it after successful start
  preflight; saved-segment and session-ended callbacks retain it by value.

### `include/okuflow/app/recording_manager.hpp`
`okuflow::RecordingTimingSnapshot`, `okuflow::CapturedFrame`,
`okuflow::RecordingManager`
- Owns the explicit recording state machine and a bounded 12-frame worker
  queue, keeping Media Foundation encoding and checked finalization off the UI
  thread.
- Receives camera work from an event-driven application queue: preview remains
  latest-frame-first, while an active recording retains up to six pending
  `MediaFrame` objects to absorb short callback/processing phase differences.
  `NotifyCaptureFrame(...)`, `NotifyCaptureGpuRetry()`, and
  `NotifyCaptureSafeCopyFallback()` keep capture overwrite, asynchronous GPU
  retry, and actual system-memory fallback accounting distinct.
- `AddOriginalFrame` accepts the full clean, orientation-normalized camera
  source independently of selected playback. GPU input retains its producer
  fence/lease; CPU input owns tightly packed BGRA. Compatibility paired entry
  points ignore processed payloads and ink. A source-format change creates
  another Original part, never a processed companion.
- Probes AV1 then H.264 for one selected output. Failed attempts retain their
  exact paths and bytes. Diagnostics report codec/input route, dimensions,
  samples and capture/pool/queue/encoder drops without claiming hardware
  completion. Ordinary packaged GUI launches remain silent.
- `Stop` releases queued video/audio and finalizes the worker-owned sample.
  Nonempty failed outputs remain preserved; only owned regular zero-byte
  outputs with no accepted video are removed. Exactly one successfully
  finalized Original per part is registered in notes.
- Stop is bounded: an independent UI-thread watchdog fires when
  Stopping/Finalizing has not reached a terminal state within 8 seconds,
  reports the exact blocked call via `DescribeWorkerStage()` (worker
  operation plus the `VideoRecorder`'s observable `WriterStage`), forces
  `Failed`, restores the UI, and disables recording for the rest of the
  process (`IsWorkerAbandoned()`).
- App close uses `ShutdownForProcessExit()`: a bounded handshake that joins a
  worker that exits and returns true when manager destruction is safe. On
  false the worker is still wedged
  inside a synchronous encoder/driver call â€” the recorder is abandoned
  (their COM teardown becomes an intentional leak), the worker is detached,
  and the caller must leak the manager (`unique_ptr::release`) so the
  detached worker can never touch freed queues or a dangling record button
  if the blocked call returns before process exit. All worker-side UI
  posting and `StartSegment` carry abandonment guards, every mutating
  `VideoRecorder` entry rejects an abandoned recorder (a resumed call chain
  cannot re-enter the sink writer), and `IsWorkerAbandoned()` remains sticky
  if the poisoned worker later recovers and joins. App teardown checks that
  state separately from the safe-to-destroy return value and skips
  `MFShutdown`/`CoUninitialize` because the abandoned recorder COM references
  remain intentionally leaked.
- Every session carries a stable `RecordingSessionInfo` (QUuid id plus the
  filename timestamp token), generated only after start preflight passes and
  readable through `CurrentSessionInfo()` while active. Both queued callbacks
  capture it by value, so a delayed callback can never be associated with a
  newer session's saved-media metadata or notes.
- Its constructor accepts a `SegmentSavedCallback`. The worker posts that
  callback to the Qt thread only after the selected Original finalizes,
  providing `SavedRecordingSegment` (session/index, Original path, archive,
  accepted capture clocks, frame count and failed attempts). `processedPath`
  is compatibility-only and empty for new recordings.
- Its optional `SessionEndedCallback` receives the ended session's identity
  and lets the application release the active
  microphone after asynchronous finalization. `SetAudioCaptureEnabled(...)`
  controls whether new segments contain sound, and `AddAudioFrame(...)`
  transfers normalized PCM into the same bounded recording worker. Each audio
  sample is written once relative to the segment's monotonic clock origin.
- `EncoderSubmitTiming()` exposes rolling p50/p95/p99 CPU time for submitting
  Original video samples on the recording worker. It intentionally does not
  claim to measure asynchronous hardware-encoder completion.
- Also owns UI button state, destination free-space preflight, dated
  `UserDataPaths` output, and asynchronous teardown.

### `include/okuflow/app/user_data_paths.hpp`
`okuflow::UserDataValidationResult`, `okuflow::PhotoPairRecoveryResult`, `okuflow::PhotoPairWriteResult`,
`okuflow::UserDataPaths`
- Owns every user-created artifact location independently of the executable.
  An empty configured root resolves through
  `QStandardPaths::DocumentsLocation/OkuFlow`; explicit roots are normalized,
  probed for writable create/delete access, and rejected inside the install
  directory. Containment resolves Windows junctions/symlinks first via
  `std::filesystem::canonical` (`QFileInfo::canonicalFilePath` does not
  resolve junctions), canonicalizing the deepest existing ancestor of a
  not-yet-created root, so a link pointing back into the install directory
  cannot pass a lexical comparison. The constructor applies the same
  containment refusal to persisted roots, and app startup revalidates the
  saved root with a user-visible fallback notice.
- Public category accessors lazily create the root plus `Photos`,
  `Recordings`, `Notes`, `Analysis`, and `Debug`; photo and recording
  accessors add ISO-date subfolders. `Debug()` is used only by
  console-attached diagnostic launches.
- `WritePhotoPair(directory, preferredStem, encodeOriginal, encodeProcessed)`
  reserves an unused stem with a `QLockFile`, tries UUID-suffixed stems on
  collisions, and creates temps with `NewOnly`. Encoder callbacks write to
  `QIODevice`; successful encodes/flushes precede a signed `.pair.pending`
  marker and the two-rename commit. Rollback removes only paths this writer
  created. `PhotoPairWriteResult` returns commit success, the selected final
  paths, and cleanup leftovers. `SaveCapturedPhotoPair` uses this helper on
  the existing image-I/O pool.
- `RecoverInterruptedPhotoPairs(staleBefore)` scans the Photos root and every
  dated subdirectory for stale `IMG_*` transactions. A valid, bounded-read
  `.pair.pending` marker and intact counterpart temp allow completion of an
  interrupted second rename. Otherwise recovery cleans disposable temps and
  preserves all final JPEGs, reporting incomplete marked pairs or invalid
  markers. Unmarked lone finals remain user-owned. Each active writer holds
  `IMG_*.pair.lock`, preventing recovery from racing a live writer.
  `PhotoPairRecoveryResult` reports completed pairs, removed files, and paths
  that could not be reconciled for a user-visible warning.
- The legacy install-relative `output` migration (copy-on-first-run prompt)
  was removed by owner decision on 2026-07-31; old files stay where they are
  and are never touched.

### `include/okuflow/app/settings_controller.hpp`
`okuflow::SettingsController`
- Owns the persisted settings document and settings path, built-in/user preset
  lookup, live-config decoration/matching, quick-option promotion, and saving.
  `StorageOperations::saveSettings` returns `settings::SaveStatus` and defaults
  to `SaveDetailed`; size refusals propagate without reserializing the payload.
  The public bool `settings::Save` wrapper remains available. Fresh on-disk
  validation before backup rotation and secret-commit ordering are unchanged.
- Rewrites a successfully loaded document when `LoadResult::migrationApplied`
  is true, preserving the pre-migration file through the normal backup path.
- Loads/saves the runtime VLM secret through `ProtectedSecretStore` while
  keeping only its credential id in the settings document. A credential read
  error prevents an unrelated save from silently erasing the protected
  secret.
- Distinguishes first run from invalid, unreadable, oversized, and
  future-version settings. It preserves the failed document (oversized files
  as `.too-large-<timestamp>`), restores a valid `.backup` when possible, and
  exposes a startup notice suitable for the visible status area. A save
  refused by the 16 MiB limit reports that cause in `LastError()`.
  A missing primary also recovers a valid bounded backup; absent/invalid
  backups keep first-run defaults. Late publication failure reports an I/O
  retry message without falsely promising an unchanged backup generation.

### `include/okuflow/app/protected_secret_store.hpp`
`okuflow::ProtectedSecretResult`, `okuflow::ProtectedSecretStore`
- Windows Credential Manager adapter for user secrets. Results distinguish
  found, missing, and API error states.
- `DefaultVlmCredentialId()` returns `OkuFlow/VLM API Key`;
  `Read(...)`, `Write(...)`, and `Remove(...)` operate on opaque target ids.
  Secret blobs are UTF-8 and are cleared from temporary/native buffers after
  use.

### `include/okuflow/app/ui_state_manager.hpp`
`okuflow::UIStateManager`
- Caches the `MainWindow` widget accessors in one UI-only object and translates
  between controls and `settings::AdvancedConfig`. It centralizes signal
  blocking during programmatic configuration changes.
- `ApplyConfigToUI(config, preservePresetSelection = false)` forwards the
  identity policy to the final persistence sync. Preset activation and startup
  preserve the selected ID; manual edits and Reset continue to re-match it.
  The final `SyncCurrentConfigToPersistence` owns the section-badge refresh;
  enablement-only `UpdateControlEnabledStates` no longer performs a duplicate.
  Session-only Extra Stable remains outside `AdvancedConfig` and mode badges.

### `include/okuflow/ui/theme_contrast.hpp`

- `RelativeLuminance`, `ContrastRatio`, `IsLightSurface` and `IsLightTheme`
  derive readable light/dark choices from the current window palette.
- Disabled/placeholder, warning and status helpers return scheme-specific ink;
  `PinnedDarkPalette` fills text/base/button roles for the deliberately dark
  colors popover and editor. `ThemedIcon` rasterizes the bundled SVGs with
  normal/disabled stroke colors while retaining colored fills and original
  assets. It adds no direct SVG-library dependency.
- MainWindow's `ApplyThemeContrast`, `UpdateSuperResStatusStyle`, themed-icon
  tracking and refresh helpers reapply contrast on palette/scheme changes.
  Expanded section headers use their button text role, and quick-mode labels
  use window text; expansion signals, keyboard order and translations remain.
  Carousel Previous from Custom wraps to the last available quick mode.

### `include/okuflow/app/assistive_feature_manager.hpp`
`okuflow::AssistiveFeatureManager`
- `ClearFocusWarning()` clears only the manager-owned Focus warning. Runtime
  answer updates relinquish that ownership, so turning Text Clarity off does
  not dismiss an explanation that replaced the warning.
- Owns `AssistiveRuntime` plus the floating `AssistiveOverlay`, periodic
  analysis cadence, vision mode state, focus warnings, TTS/result routing,
  persisted camera-relative overlay geometry, and lecture-note routing through
  `UserDataPaths`.
- Routes `AssistiveRuntime::PrivacyNotice` into visible status and a
  screen-reader announcement without invoking speech synthesis.

### `include/okuflow/app/suspend_guard.hpp`
`okuflow::SuspendGuard`
- Small non-copyable RAII latch used while applying settings/UI state. It
  restores the referenced suspension flag even when a scope exits early.

### `include/okuflow/app/interaction_controller.hpp`
`okuflow::InteractionController`
- Converts keyboard, wheel, mouse-drag, and virtual joystick input into zoom-center updates.
- Public API:
  - `explicit InteractionController(OkuFlowApp& app)`
  - `bool HandlePanKey(int key, bool pressed)`
  - `bool HandlePanScroll(const QWheelEvent* wheelEvent)`
  - `void HandleZoomWheel(const QWheelEvent* wheelEvent)` â€” consumes fractional
    pixel or angle deltas, applies optional bounded velocity acceleration, and
    preserves cursor-anchored focus
  - `void HandleKeyboardZoom(float notches)` â€” deterministic unaccelerated
    geometric keyboard zoom
  - `bool ApplyInputForces(double elapsedSeconds)` â€” integrates motion as
    normalized units per second, so 60 and 120 FPS move at the same speed
  - `bool HasContinuousMotion() const`
  - `void BeginMousePan(const QPointF& pos, const QSize& widgetSize)`
  - `bool UpdateMousePan(const QPointF& pos)`
  - `void EndMousePan()`
  - `bool IsMousePanActive() const`
  - `void ResetJoystick()`
  - `void SetJoystickAxes(float x, float y)`

### `include/okuflow/app/setup_assistant.hpp`
`okuflow::SetupAssistantDialog`
- Nonmodal, accessible dependency manager shown after first paint when
  Codex is missing and the user has not declined automatic
  prompting. Advanced `Setup & Downloads` can reopen it at any time.
- Startup `NeedsSetup` checks only a missing Codex executable; optional Maxine
  does not trigger startup prompting. The dialog fits its dependency rows up
  to 85% of the available screen height. The translated Codex row
  describes its Read, Explain, and Assistant functions; Install becomes Update
  when the executable is found. No separate recognition component is probed.
- Streams the pinned official Codex bootstrap and architecture-specific
  NVIDIA Video Effects installers with a 60-second inactivity timeout and
  SHA-256 verification. Failed Qt transfers retry with Windows `curl.exe`.
- NVIDIA's verified installer is launched with `ShellExecuteExW` and the
  `runas` verb so administrator-required manifests receive a real UAC consent
  flow instead of failing through `QProcess` with Windows error 740. A timer
  monitors the returned process handle and refreshes status on completion; the
  in-progress row directs the user to continue in the separate vendor
  installer window.
- NVIDIA installation/removal is delegated to the vendor installer or
  registered uninstaller.
- Codex installs or updates through OpenAI's verified per-user bootstrap.
  OkuFlow detects its standalone and WinGet paths, persists the resolved
  executable, and keeps ChatGPT authentication as a separate AI Settings
  action. Because the official installer exposes no uninstall action, the row
  opens the install folder or official setup guide instead.
- Dependency rows pair explicit text with large green check/red X status
  indicators.
- Public API:
  - `SetupAssistantDialog(const QString& configuredCodexPath, bool declined, QWidget* parent = nullptr)`
  - `~SetupAssistantDialog()`
  - `static bool NeedsSetup(const QString& configuredCodexPath)`
  - `static QString FindCodexExecutable(const QString& configuredPath = {})`
- Signals: `CodexPathChanged(...)`,
  `DeclinePreferenceChanged(...)`, and `DependenciesChanged()`.
- `closeEvent(...)` cancels an in-flight download, but keeps the dialog alive
  while a launched vendor installer is running so Qt cannot terminate it by
  destroying the child `QProcess`.

### `include/okuflow/app/color_schemes.hpp`
Namespace `okuflow::color_schemes`
- `enum class SchemeMode { Duotone, Posterize, Gradient }` selects two-stop,
  hard-banded, or smoothly interpolated luma mapping.
- `struct ColorScheme` is the single display-color model: stable id, visible and
  accessible names, mode, 2-8 `QColor` stops, stepped/text-polarity flags,
  optional legacy mode, and effect classification.
- `using ColorLut = std::array<std::uint32_t, 256>` stores packed BGRA results.
- `BuiltInColorSchemes()`, `LegacyColorScheme(int)`, and
  `FindBuiltInColorScheme(...)` expose the authoritative table and legacy
  migration.
- `NormalizeColorScheme(...)` and `BuildColorLut(...)` validate a scheme and
  generate its GPU-ready table.
- `TextForegroundBgra(...)` / `TextBackgroundBgra(...)` supply the same active
  scheme endpoints to Text Clarity compositing.
- `PackBgra(...)`, `UnpackBgra(...)`, `ModeToken(...)`, `ModeFromToken(...)`,
  and `SchemesEquivalent(...)` support persistence and comparison.

### `include/okuflow/app/settings_store.hpp`
Namespace `okuflow::settings`
- `enum class ViewportRateMode`
  - global viewport navigation policy: `AutoUpTo120`, fixed 60/90/120 FPS, or
    `MatchDisplay`; explicit choices clamp to the active monitor
- `enum class ViewportFitModeSetting`
  - global aspect policy: default crop-to-fill or show-all fit with black bars
- `struct AdvancedConfig`
  - full profile-owned tuning payload including image-processing and focus
    state, vision mode flags, `stabilizationEnabled`, structured `colorScheme` plus the
    migration-only `displayColorMode`, screen fix, and Text Clarity
    (`autoTextClarityEnabled`, background flatten, adaptive
    binarization/Sauvola/softness, polarity, stroke weight, smart sharpen,
    CLAHE, two-color output, mask hysteresis, selective sharpen, focus
    detection, glare suppression, `mlSuperResEnabled`, and
    `mlSuperResStrength`; optional, default-off `mlSuperResPrefer2x` raises the
    profile's minimum magnification to 2x for a smaller, faster, narrower-view
    AI source crop; mutually exclusive `mlSuperResUltra1440p` uses the complete
    processed camera frame and a separate cache capped at 1440p-class output)
  - legacy `mlTextSuperResolution*` JSON keys migrate to the current Maxine
    keys on load; new settings are written only under `mlSuperRes*`
  - `rotationQuarterTurns` is read from old profile JSON only for backward migration, is no longer written into profiles, and is ignored by current profile comparisons
- `struct AssistiveSettings`
  - AI/assistive configuration: `aiProvider`, `codexExecutablePath`,
    `codexModel`, `codexReasoningEffort`, `codexInternetEnabled`,
    `codexCodingEnabled`, `codexWorkspaceDirectory`,
    `assistantInstructions`, `vlmApiUrl`, runtime-only `vlmApiKey`,
    persisted `vlmCredentialId`, `vlmModel`, `vlmPrompt`,
    `ttsEngine`, `ttsVoiceName`, `ttsVoiceLocale`, `ttsRate`,
    and `lectureNotesEnabled`
  - defaults to Codex model `gpt-6-luna`; the former misspelled
    `gpt-5.6-tera` value is normalized during load
  - `vlmApiKey` is never serialized and a JSON field with that name is
    ignored. `SettingsController` resolves `vlmCredentialId` through Windows
    Credential Manager. Matching environment variables remain runtime
    fallbacks.
- `struct CodexConversation`
  - OkuFlow-owned thread index entry: `threadId`, `title`, `preview`, `createdAt`, `updatedAt`, and `explain` (serialized only when true; marks a thread opened by Explain, whose empty title displays as the translated "Explain conversation"); transcripts remain in the Codex thread store
- `struct PresetDefinition`
  - stage-1 quick-mode metadata: preset id, name, description, target config id, built-in flag
- `struct PersistentSettings`
  - persists global `language` as `system` or a canonical supported code from
    `app_language.hpp` (missing/unknown follows system preferences; explicit
    legacy `en`/`tr`/`de` remains selected), `cameraIndex`, stable
    `cameraFormatStableId`,
    crash-safe `cameraAccelerationAttempt`, per-symbolic-link
    `CameraAccelerationSetting` mode/fallback/reason/date records plus the last
    successful ladder rung (`zeroCopy`, `acceleratedCopy`, or
    `compatibility`),
    and a load-only bridge that migrates the removed global compatibility
    checkbox to the selected camera's `Compatibility mode`,
    configurable `userDataRoot` (empty selects the Documents default), and
    `rotationQuarterTurns`, UI state,

    `simpleUiMode`, resizable `advancedPanelWidth`, camera-relative
    `assistiveOverlayGeometry`, `assistiveOverlayDockPosition` (floating/left/right;
    missing or invalid values default to floating), global annotation color/width/dashed style,
    shape kind, text size, and capture-on-exit preferences (annotation strokes
    are deliberately not persisted),
    `viewportRateMode`, `viewportFitMode`, global `recordingCanvasMode`,
    stable `microphoneEndpointId`,
    `zoomWheelAcceleration`, and the
    `uiSectionStates` disclosure map,
    `setupAssistantDeclined`, selected preset id, current live advanced config,
    reusable `customColorScheme`,
    the `assistive` settings block,
    OkuFlow-created `codexConversations`, `explainThreadId` (the Codex thread
    every Explain continues; empty or absent means the next Explain opens
    one), and user-created configs/presets
- `kMaxSettingsFileBytes`
  - 16 MiB raw on-disk limit for `settings.json` and `.backup`, counting CRLF
    bytes; see `docs/hardcoded_paths.md` for the capacity rationale
- `enum class LoadStatus`, `struct LoadResult`
  - classify `Loaded`, `Missing`, `Unreadable`, `InvalidJson`,
    `UnsupportedVersion`, and `TooLarge`, with an optional settings payload,
    error text, and `migrationApplied` write-back flag
- `enum class SaveStatus`
  - `Saved`, `TooLarge` (refused before any file changed), or `Failed`
- Functions:
  - `QString ResolveSettingsPath()`
  - `void EnsureSettingsDirectory(const QString& path)`
  - `LoadResult LoadDetailed(const QString& path)`. It reads in binary mode,
    at most one byte past `kMaxSettingsFileBytes`, so a growing file cannot
    bypass the bound. It rejects versions newer than schema 16, preserves the
    reason, and reports the model-id migration.
  - `std::optional<PersistentSettings> Load(const QString& path)` â€”
    compatibility wrapper around `LoadDetailed`
  - `QByteArray Serialize(const PersistentSettings& settings)` â€” the exact
    indented CRLF bytes that `Save` writes, used for size accounting
  - `SaveStatus SaveDetailed(const QString& path, const PersistentSettings& settings)`.
    It refuses a document over the limit before inspecting or replacing
    anything. It uses `QSaveFile` in binary mode, checks commit success, and
    stages and flushes the replacement before rotating a valid prior document
    or setting an oversized document aside. `RenameWithoutReplacing` uses a
    same-directory Win32 rename with no copy/replacement fallback for the
    oversized sidecar and rollback. A commit failure attempts to restore that
    original without overwriting another writer. The ordinary valid-primary
    backup can already be rotated when a late commit fails; both file
    generations are not guaranteed unchanged. Runtime secrets are never
    serialized.
  - `bool Save(const QString& path, const PersistentSettings& settings)` â€”
    `SaveDetailed(...) == SaveStatus::Saved`
  - `const std::vector<AdvancedConfig>& BuiltInConfigs()`
  - `const std::vector<PresetDefinition>& BuiltInPresets()`
  - `QString DefaultPresetId()`
  - `const AdvancedConfig* FindAdvancedConfigById(...)`
  - `const PresetDefinition* FindPresetById(...)`
  - `std::optional<AdvancedConfig> ResolveConfigForPreset(...)`
  - `bool AreConfigsEquivalent(...)` â€” compares slider-backed values at their
    representable UI precision so applied presets remain selected after
    control quantization. Legacy `rotationQuarterTurns` is excluded because
    orientation is a shared camera setting, not part of a quick mode.

### `include/okuflow/app/app_language.hpp`

- `AppLanguage` is the 25-entry registry listed in [localization](localization.md).
  `SupportedAppLanguages`, `AppLanguageCode`, `AppLanguageNativeName`,
  `AppLanguageEnglishName`, `AppLanguageFlagResource`, `AppLanguageCatalogResource`,
  `AppLanguageLocale` and `AppLanguageLayoutDirection` separate persisted tags,
  model directives, picker text, embedded resources and locale/direction.
- `AppLanguageFromCode` offers strict output-pointer and English-fallback
  overloads. `AppLanguageFromLocaleTag`, `AppLanguageFromUiLanguages` and
  `AppLanguageFromSystemLocale` preserve ordered system preferences, regional
  variants, Chinese script precedence and legacy `iw`/`in` aliases.
- `kSystemLanguagePreference`, `NormalizeLanguagePreference`,
  `IsSystemLanguagePreference` and `ResolveLanguagePreference` implement the
  persistent system/manual choice; explicit supported codes win.

### `include/okuflow/app/language_manager.hpp`
`okuflow::LanguageManager`
- Owns the process `QTranslator`, supported application locale, and live
  widget-tree retranslation. It does not own persistence; `OkuFlowApp`
  reads/writes the stable language code through `SettingsController`.
- The constructor accepts an optional `CatalogResolver` for isolated failure
  fixtures; production resolves embedded resources. The destructor removes its
  owned translator. `SetLanguage` loads/installs the candidate before removing
  the active translator, retaining language/locale/direction on load failure.
  English removes the translator. A successful switch applies
  the matching `QLocale` and process layout direction, and re-translates every
  open top-level widget without reconstructing it or changing control
  selection.
- `--rtl-test` and `OKUFLOW_FORCE_RTL=1` force the English interface through
  the right-to-left layout path for development and regression testing. They
  do not add a language to the user-facing picker.
- Signals: `languageChanged(const QString&)` and
  `languageChangeFailed(const QString&)`.

### `include/okuflow/app/constants.hpp`
Namespace `okuflow::app_constants`
- UI scaling and step constants for zoom, panning, and blur controls
- `kZoomSliderMaxMultiplier` is 16; the slider spans 100..1600 at hundredth
  precision. Wheel/keyboard limits follow that same slider range.
- Helper functions:
  - `ClampZoomMultiplier(float)` validates finite magnification in 1x..16x,
    defaulting non-finite values to 1x. Built-in/modern/legacy config parsing,
    serialization and UI restoration share it; existing valid zoom settings
    and profile values are preserved without a schema change.
  - `SliderValueToSigma(int sliderValue)`
  - `SnapBlurRadius(int value)`

## Capture Module

### `include/okuflow/capture/audio_capture.hpp`
Types:
- `struct AudioDeviceDescriptor`
  - Windows endpoint `name`, stable `endpointId`, `isDefault`, and retained
    Media Foundation activation object
- `struct AudioFrame`
  - signed 16-bit PCM bytes plus sample rate, channel count, bit depth,
    QPC-derived `captureClock100ns`, and block duration
- `using AudioFrameCallback` / `using AudioErrorCallback`

`okuflow::AudioCapture`
- Enumerates Windows microphone capture endpoints, marks the current
  multimedia default, and normalizes the selected device to 48 kHz mono PCM.
- The source reader runs on its own thread only while recording. Audio block
  timestamps use the same monotonic QPC domain as camera-frame arrival so the
  recording worker can mux one synchronized AAC track into both MP4 files.
- `Stop()` is bounded and detach-safe: the reader `Flush` that wakes the
  blocking `ReadSample` runs on a helper thread (it would serialize behind a
  wedged reader call), both threads get 3 seconds to acknowledge, and a
  wedged thread is detached with its COM references leaked. The capture
  loop and flusher share an independently owned `CaptureSession`
  (shared_ptr) holding every flag they touch â€” never a pointer back to
  `AudioCapture` â€” so a detached thread survives the object's destruction,
  and each `Start()` creates a fresh session a stale thread cannot observe.
  Buffer conversion/locking rechecks the session after each potentially
  blocking operation and immediately before dispatch. At the app boundary,
  callbacks retain only an independently owned `MicrophoneCallbackTarget`:
  Stop cancels its generation before waiting, delivery holds its mutex, and
  app destruction clears its pointer under that same mutex. Thus even a
  reader detached between the final session check and callback invocation can
  reach neither freed app state nor a restarted session. `WasAbandoned()`
  tells the app to skip process-global MF teardown.
- Public API:
  - `std::vector<AudioDeviceDescriptor> EnumerateDevices()`
  - `bool Start(const AudioDeviceDescriptor&, AudioFrameCallback,
    AudioErrorCallback = {})`
  - `void Stop()`
  - `bool IsRunning() const`
  - `bool WasAbandoned() const`
  - `const std::string& LastError() const`
  - `const std::wstring& ActiveEndpointId() const`

### `include/okuflow/capture/media_capture.hpp`
Types:
- `struct MediaFrame`
  - optional packed CPU `data`; optional accelerated `gpuTexture`,
    `gpuSample`, `gpuSubresource`, and `gpuFormat`; `subtype`, `width`,
    `height`, signed `stride`, `dataSize`,
    Media Foundation `captureTimestamp100ns`, arrival
    `captureClock100ns`, monotonic `sequenceNumber`, and negotiated frame-rate
    numerator/denominator. The arrival clock is shared with microphone
    capture and capture-to-present diagnostics. A negative stride is preserved
    until the CPU conversion boundary instead of being reinterpreted as an
    unsigned allocation size.
  - `bool IsGpuResident() const` reports whether the frame retains a D3D11
    source texture. A frame may carry both GPU and CPU representations during
    startup validation or original-media capture.
  - Accelerated frames retain the originating `IMFSample` in `gpuSample`.
    Media Foundation's source-reader allocator may recycle a pooled texture
    after its sample is released even while another COM reference keeps the
    texture object alive. The sample lease therefore travels with the frame
    through the recording burst queue and deferred GPU-conversion retry, and
    releases with the frame on consumption or drop.
- `using FrameCallback = std::function<void(MediaFrame&& frame)>` â€” transfers
  ownership from the capture thread into the app's mutex-protected pending-frame
  queue; the Qt frame tick moves it back out, avoiding two full-frame copies
- `using CaptureErrorCallback = std::function<void(const std::string& message)>`
- `struct CameraDescriptor`
  - `name`, `symbolicLink`, `activation`
- `struct VideoFormat`
  - Media Foundation `subtype`, `width`, `height`, frame-rate `numerator` /
    `denominator`, and a driver-order-independent `stableId`
- `enum class CameraFailureKind`
  - `None`, `DeviceBusy`, `DeviceMissing`, `AccessDenied`, `Other` â€” plain-language classification of the most recent capture failure
- `enum class CaptureAccelerationMode`
  - `Accelerated` creates a D3D11/DXGI device manager for Media Foundation
    hardware transforms; `Compatibility` retains the conservative
    system-memory reader
- `enum class GpuFramePreparationResult`
  - `Ready` means the reusable NT-shareable BGRA conversion texture is safe
    for CUDA; `Retry` means its D3D11 completion query is still pending and
    the caller should use one safe readback without disabling the top rung;
    `Unsupported` means the session must move down the capture ladder

`okuflow::MediaCapture`
- Media Foundation camera enumeration and threaded source-reader capture.
- Public API:
  - `MediaCapture()`
  - `~MediaCapture()`
  - `bool Initialize()`
  - `void Shutdown()`
  - `std::vector<CameraDescriptor> EnumerateCameras()`
  - `std::vector<VideoFormat> EnumerateFormats(const CameraDescriptor& descriptor)`
  - `bool StartCapture(const CameraDescriptor& descriptor, const VideoFormat*
    requestedFormat, FrameCallback callback, GUID preferredSubtype =
    MFVideoFormat_NV12, CaptureErrorCallback errorCallback = {},
    CaptureAccelerationMode accelerationMode =
    CaptureAccelerationMode::Compatibility, const std::wstring&
    requestedStableId = {})` â€” requests the selected frame
    size/rate when provided. Otherwise resolves the stable ID against the
    active reader's native modes, falling back to driver choice if unmatched;
    prefers compact
    NV12 for CUDA conversion and falls back through YUY2/BGRA; retries
    transient busy/resource errors internally. Accelerated mode accepts
    `IMFDXGIBuffer` samples and, after startup validation, transfers retained
    D3D11 textures instead of steady-state CPU buffers.
  - `bool StopCapture(const std::function<void(bool quiesced)>&
    beforeAccelerationRelease = {})` revokes callback entry and coordinates
    worker stop/producer quiescence and final resource cleanup off the UI
    thread, using one 1500 ms worker deadline. The caller's callback has its
    own bounded GPU drain; true means producer work completed and its backing
    remains alive for consumer release. False requires retaining imports.
    A failed stop retains the entire session until process exit.
  - `void BeginStopCapture()` immediately revokes callback entry and facade
    GPU/phone operations, then starts at most one stop worker owning the exact
    shared session. `CaptureStopStatus PollStopCapture()` reads only control
    state and returns `WaitingForQuiescence`, `ReadyToRelease`,
    `WaitingForRelease`, `Completed` or sticky `Failed` without driver waits.
    `void ReleaseStoppedCapture(bool consumerSafe)` permits producer teardown
    only after quiescence and successful cleanup of that session's consumer
    imports; early/unsafe acknowledgment retains the session. The total split
    stop deadline is 1500 ms, including consumer cleanup and final release.
    Worker completion timestamps preserve success across delayed UI polling.
    An unfinished split stop cannot implicitly release consumer imports through
    `StartCapture`, `Shutdown` or destruction; the legacy wrapper remains bounded.
    `CaptureShutdown::Poll()` snapshots quiescence (including entered callbacks),
    abandonment, completion and its monotonic timestamp under a control-only lock.
  - `bool WasAbandoned() const` is sticky: further capture starts/enumeration
    are refused and app shutdown skips `MFShutdown` / `CoUninitialize`.
  - `const std::string& LastError() const`
  - `const std::string& FormatNotice() const` â€” reports a requested versus
    negotiated mismatch in plain language
  - `const VideoFormat& NegotiatedFormat() const`
  - `const std::vector<VideoFormat>& NativeFormats() const` â€” immutable facade
    snapshot populated by the streaming reader before `StartCapture` returns
  - `void Swap(MediaCapture&) noexcept` â€” transfers the session and every
    diagnostic/format/abandonment snapshot; callers must exclude concurrent use
  - `CameraFailureKind LastFailureKind() const`
  - `CaptureAccelerationMode AccelerationMode() const`
  - `bool ConsumeAccelerationValidated()` / `bool
    ConsumeAccelerationRejected()` â€” one-shot startup-validator results used
    by the application fallback ladder
  - `GpuFramePreparationResult PrepareGpuFrameForCuda(const MediaFrame&,
    ComPtr<ID3D11Texture2D>&, std::shared_ptr<void>& outLease)` â€” converts a retained accelerated sample into a
    reusable NT-shareable BGRA D3D11 texture without crossing system memory.
    The viewport thread polls the D3D11 completion query once without sleeping. A
    delayed query returns `Retry`; subsequent polls permit runtime flushing
    (`GetData` flags zero) to prevent pending-query starvation observed with
    shared hardware-decoder output. The caller must retain and resubmit that
    exact `MediaFrame`, allowing the same conversion to complete
    asynchronously without issuing a duplicate blit or crossing system
    memory. The application permits up to 25 ms of event-loop retries before
    using a safe copy for that frame. Unsupported interop separately disables
    direct transfer for the capture session. Retries return to Qt rather than
    spending a per-frame polling allowance on its UI thread.
    Ready returns a producer lease, which prevents another blit and keeps
    session resources alive until CUDA observes camera-copy completion.
    Matching BGRA input uses `CopyBgraCaptureTexture` instead of redundant
    VideoProcessor work. Other formats retain color-aware video conversion.
  - `bool ReadbackGpuFrame(MediaFrame&)` â€” explicitly populates packed CPU
    bytes for validation, original recording/photo capture, or fallback
  - `double CurrentFrameRate() const` â€” reports the negotiated capture rate;
    diagnostics do not claim viewport re-presentation as camera FPS
  - `bool ConsumeDeviceLost()` â€” returns/clears mid-stream loss and detects an
    overdue in-flight Media Foundation `ReadSample` without calling the driver.
    The deadline is 10 working seconds or four negotiated frame periods,
    whichever is larger, capped at 60 seconds. A successful atomic claim revokes
    new deliveries and signals the existing reconnect/split-stop path once.
    Explicitly stopped and phone sessions are excluded. An unquiescent driver
    still retains its exact session and refuses reuse; detection is not proof
    that a wedged device can recover in process.
  - `const std::wstring& LastSymbolicLink() const` â€” symbolic link of the most recently requested device, including failed opens; kept across loss/stop so reconnection retries that physical camera
- Internal helpers belong to independently shared `MediaCaptureSession`:
  - `ConfigureReader(...)`
    Optional capture diagnostics compare the media source's native mode with
    the transformed reader output.
  - `ReadCurrentFormat(IMFSourceReader* reader, FrameFormat& outFormat)` honors
    signed `MF_MT_DEFAULT_STRIDE`, using the calculated subtype pitch only
    when the media type has no explicit stride.
  - `CaptureLoop(FrameCallback callback, CaptureErrorCallback errorCallback)`
    stamps each actual `ReadSample` in `readStartedWorkingMs_`; every return,
    including STREAMTICK, disarms it. A watchdog-winning late return cannot
    deliver or start another read. CPU/GPU conversion work is not a pending read.
    `ReadWatchdogNowMs()` uses `QueryUnbiasedInterruptTime` so system sleep and
    hibernation do not consume the driver's allowance. Zero disables expiry
    when a clock observation is unavailable.
  - `CreateAccelerationDeviceManager()`, `AttachDxgiFrame(...)`,
    `CopyGpuFrame(...)`, `EnsureVideoProcessor(...)`, and
    `ValidateStartupFrame(...)`
    `EnsureVideoProcessor` prepares only a shared target/query for exact BGRA
    copies; it creates video-processing objects only for actual conversion.
  - `CopyBgraCaptureTexture(...)` in `capture/capture_texture.hpp` checks
    matching BGRA formats, single-sample textures, array/mip bounds, and visible
    extents before issuing `CopySubresourceRegion` into the NT-shared target.
    Successful submission still requires the completion query and CUDA lease.
  - `ExtractFormats(IMFSourceReader* reader)`
  - `HrToString(HRESULT hr)`
- Capture ownership:
  - `MediaCapture` owns UI diagnostic snapshots. Capture/stop threads own a
    shared session; `Quiesce` runs Flush/join/producer completion and
    `ReleaseResources` waits for consumer leases before final COM release.
  - `CaptureShutdown::TryEnterDelivery` grants counted `Delivery` tokens.
    Cancellation denies new tokens; quiescence waits for entered deliveries.
    `AllowRelease`, `Abandon`, and bounded completion waits coordinate teardown.
  - App `CameraIngress` owns queued frames independently, posts coalesced Qt
    context callbacks, and revokes delivery under a narrow mutex. Dropped MF
    samples are released outside that lock. Stale sessions cannot access app
    state, and timed-out ingress/sample ownership is retained for process exit.
  - `activeActivation_` retains the activation object for the live session and
    `StopCapture(...)` calls `ShutdownObject()` before releasing it
  - temporary mode enumeration uses the same balanced activation/shutdown contract
  - the capture callback moves each completed `MediaFrame`; it does not retain
    or access the moved buffer after invoking the consumer

## Common Module

### `include/okuflow/common/codex_json_rpc_process.hpp`
`okuflow::CodexJsonRpcProcess`
- Shared JSONL/JSON-RPC stdio transport for local Codex app-server children,
  extracted from `CodexAppServerClient` (plan 36 Phase 1). Owns the
  `QProcess` lifecycle, request ids and per-request reply deadlines (default
  60 s), newline framing with hard bounds (2 MiB per message, 4 MiB buffered),
  one payload-free stderr indication per child, and default denial (`-32601`) of unexpected
  server-to-client requests; a client-installed handler may answer known
  request methods instead.
- Diagnostics never contain raw payload bytes â€” malformed lines are reported
  by size and parse offset only, because protocol lines can carry conversation
  text, images, or account details.
- `SetRequestTimeoutMs(int)` adjusts the per-request deadline; the Assistant
  client exposes the same control while leaving the normal 60-second default.
- Framing or size violations emit `ProtocolFailed`, fail pending replies, and
  kill the child. Clients fail their own pending replies on `Finished` with a
  client-appropriate message.

### `include/okuflow/common/codex_app_server_client.hpp`
`okuflow::CodexAppServerClient`
- Native Qt JSON-RPC client for the local `codex app-server` stdio transport,
  built on the shared `CodexJsonRpcProcess`.
- The public surface covers server lifecycle, account/login state,
  model/rate-limit discovery, assistant turns, cancellation, and OkuFlow
  conversation load/rename/delete operations.
- Restricted mode uses read-only sandboxing, approval policy `never`, disabled
  network access, and vision-only developer instructions. Persistent Advanced
  Assistant turns can opt into web/network access and workspace-scoped command
  execution/file changes. Read and Explain never inherit those permissions.
  MCP, dynamic, and collaboration tool items always trigger interruption.
  Server-initiated command/file approvals are declined, and permission-profile
  requests receive an empty grant so no additional filesystem or network
  capability is accidentally approved.
- The stable app-server turn surface does not currently provide a complete
  per-turn tool allow-list. OkuFlow therefore combines the strongest exposed
  sandbox/network/approval policy with reactive interruption as defense in
  depth rather than claiming instruction text alone prevents a tool start.
- Turn liveness is bounded: 90-second activity timeout, 180-second vision-turn
  cap, 30-minute persistent-turn cap, and a five-second interrupt grace before
  forced local reset. Protocol buffers/messages, answers, and loaded
  transcripts have explicit size/count ceilings and mark truncation.
- An admitted turn carries a generation through account, thread opening, and
  turn start. Stop retains an in-flight start until its turn id can be
  interrupted, and a canceled opening cannot create or mutate a replacement.
  Stale notifications and approvals require matching thread/turn identity;
  recognized approval requests are still declined. Failed initialization or
  transport timeout retires the child before publishing one terminal result,
  so a later request can start a fresh process. The watchdog starts at
  admission, covering initialization as well as execution.
- Public API:
  - `Configure(const QString& executablePath, const QString& preferredModel, const QString& reasoningEffort, const QString& assistantInstructions, bool internetEnabled, bool codingEnabled, const QString& workspaceDirectory)`
  - `Start()` / `Shutdown()`
  - `SetRequestTimeoutMs(int)` changes the JSON-RPC request deadline for
    controlled testing or integration; the default remains 60 seconds
  - `IsReady()`, `IsTurnActive()`
  - `BuiltInAssistantInstructions()` returns the read-only OkuFlow identity
    prompt shown in AI Settings
  - `RefreshAccount()` / `StartChatGptLogin()`
  - `RequestVisionTurn(prompt, imagePath, threadId, bool persistent, bool retainThread = false, bool permitCoding = true)` / `InterruptTurn()`
    - `persistent` = Advanced Assistant turn (stored thread, opt-in
      permissions). Non-persistent with `retainThread` = Explain: a stored
      (`ephemeral: false`) thread under the restricted vision policy. If
      `thread/resume` of its id fails while Codex is ready and signed in, the
      same admitted turn opens a replacement thread instead of failing.
      Neither flag = Read: a fresh ephemeral thread.
  - `LoadConversation(...)`, `RenameConversation(...)`, `DeleteConversation(...)`
- Signals cover server/account/model/rate-limit state, login URL, conversation
  lifecycle/transcript loading, and streamed turn start/delta/completion.
  `ConversationCreated` reports new Assistant threads;
  `RetainedThreadStarted(const QJsonObject&)` reports a new or replacement
  Explain thread.
  `ModelCatalogChanged(const QJsonArray&, ...)` preserves app-server display
  names, defaults, modalities, and supported reasoning efforts while the
  compatibility `ModelsChanged(QStringList, ...)` signal remains available.
  Destruction suppresses outward lifecycle notifications while synchronously
  stopping the child process, so owners cannot receive callbacks from
  partially destroyed service state.
- `DeveloperInstructions` makes verbatim reading preserve source wording and
  language even when conversational response preferences request translation
  or summarization.

### `include/okuflow/common/assistive_runtime.hpp`
Supporting types:
- `struct AssistiveRuntimeConfig`
  - same provider/Codex/VLM/speech fields as `settings::AssistiveSettings`, including Codex internet/coding/workspace permissions, plus `QString notesDirectory` (absolute directory for lecture notes; empty disables notes)

`okuflow::AssistiveRuntime`
- Asynchronous assistive-analysis runtime owned by `OkuFlowApp`.
- Notes use an independent ordered worker queue (`NotesWorkState` and
  `QueueNotesWork`): up to 128 jobs / 256 MiB of retained data. Each job owns
  immutable content and its destination; runtime destruction disconnects
  completion delivery without waiting for notes storage. Accepted writes
  drain through Qt's global pool at application shutdown.
- `SaveAnalyzedImageForNotes` reserves a UUID path and shares the frame in
  memory (up to three images / 192 MiB); `RemoveNoteImage` cancels that
  reservation. `AppendNoteBlock` transfers referenced images to the worker,
  which resizes/encodes them before the checked HTML append and removes them
  on failure. Canceled analyses never create a notes image on disk.
- Owns a bounded two-thread image-preparation pool for vision resize, JPEG,
  base64, JSON, and temporary-frame writes. Generation tokens invalidate
  canceled work; queued completions start requests on the runtime thread.
- `StartVlm(..., bool readText, bool continueExplain)` chooses either a verbatim transcription prompt
  or the configured scene prompt. Read omits interface-language directives and
  HTTP conversational preferences to preserve source text. It keeps answers up to the
  existing 256 Ki-character cap and records them under `Text on screen`;
  Explain keeps its full shared-chat answer and full notes text. The HTTP
  `BuildVlmRequestBody` token limit is 4096 for Read, 180 for Explain, and
  2048 for a typed tool-free followup.
- Explain continuity (`continueExplain`, set by `SubmitFrameForced` for
  Explain only): with Codex, `StartCodexVlm(..., bool retainThread)` continues
  `explainThreadId_`, or opens a stored thread whose id arrives through
  `RetainedThreadStarted`. Read always uses a new ephemeral thread. A
  continuing request appends `ExplainContinuationPrompt` (describe this view,
  use earlier views only as context). With an OpenAI-compatible server,
  the shared store supplies the last 4 completed exchanges (each answer at
  most 1200 characters, text only, in memory); `BuildVlmRequestBody` resends
  role/content pairs before the current request. Read and periodic analyses
  neither send nor extend it; a provider, server URL, or model change clears it.
  Explain results reach lecture notes exactly as before (`FinishVlmSuccess`).
- There is one vision busy state. A second forced request reports status
  without clearing the active answer or its retained notes image. Codex
  requests remain user-initiated; enabling scene mode does not spend allowance
  periodically. `OkuFlowApp::SubmitOnDemandAnalysis(bool readText)` retains
  the requested action across asynchronous GPU readback. Explain and Advanced
  Assistant submissions exclude each other's pending captures and active work.
  `CancelPendingAssistantCaptures()` clears queued captures and readback ids
  on Stop, camera shutdown, or a presenter fault without interrupting an
  already-dispatched chat. `ArmAssistantCaptureDeadline()` bounds capture
  preparation to five seconds independently of presentation ticks;
  `assistantCaptureGeneration_` prevents an old deadline from cancelling a
  later request. Overlay-driven Explain/Stop updates are queued so they read
  busy state after the runtime finishes its transition.
- VLM path:
  - default: saves a temporary JPEG and submits it as `localImage` through `CodexAppServerClient`; Codex explanations are on-demand rather than periodic
  - fallback: JPEG-encodes the frame and posts an OpenAI-compatible `chat/completions` request; API keys are optional for local servers
  - streams/finalizes text into the overlay, notes, and Assistant signals; TTS is invoked separately by `ReadAloud(...)`
  - emits a deduplicated `PrivacyNotice` naming whether a camera frame is
    attached, the local/remote provider, persistence (saved Explain
    conversation, temporary per Read, or recent Explain answers kept in
    memory), and enabled internet/coding capabilities
- Temporary Codex files carry the owning process id, are removed on every
  normal completion/cancellation/shutdown path, and stale files belonging to
  dead processes are swept at startup.
- Public API:
  - `AssistiveRuntime(QObject* parent = nullptr)`
  - `~AssistiveRuntime()`
  - `void SetConfig(const AssistiveRuntimeConfig& config)`
  - `void SetModes(bool vlmEnabled)`
  - `bool WantsAnalysis() const`
  - `bool IsBusy() const`
  - `bool IsCodexTurnActive() const`
  - `void SubmitFrame(const uint8_t* bgraData, int width, int height)`
  - `void SubmitFrameForced(const uint8_t* bgraData, int width, int height, bool readText = false)` â€” reads text or explains the scene immediately regardless of enabled modes
  - `void ReadAloud(const QString& text)` â€” speaks a result only after an explicit user request, using the configured Windows voice and speed
  - `void SetResponseLanguage(const QString& languageCode)` â€” selects the
    response-language directive for subsequent AI requests and prefers a
    matching installed TTS locale without initiating speech
  - `void DismissOverlay()` â€” hides the current result panel until a new forced Read, Explain, or Assistant request begins
  - Codex/Assistant control: `StartCodexLogin()`, `StopAssistant()`, `SubmitAssistantPrompt(...)`, `LoadAssistantConversation(...)`, `RenameAssistantConversation(...)`, `DeleteAssistantConversation(...)`
  - Explain conversation: `SetExplainConversationId(const QString&)` restores
    the persisted id without signaling; `ExplainConversationId()`;
    `StartNewExplainConversation()` forgets the id and the OpenAI-compatible
    history. Signals: `ExplainConversationCreated(const QJsonObject& thread)`
    for a new/replacement thread, and `ExplainConversationChanged(const QString&)`
    whenever the id changes (empty after reset or after
    `AssistantConversationDeleted` for that thread). `OkuFlowApp` mirrors the
    id into `PersistentSettings::explainThreadId`, lists the thread with
    `CodexConversation::explain`, and on a completed Explain refreshes that
    entry's `updatedAt` and preview. `StartNewAssistantConversation()` also
    always resets the visible Explain chat and its private backend bindings.
  - `void NoteCapturedPhotoPair(const QString& originalPath, const QString& processedPath)` â€” appends synchronized original and processed images to the HTML lecture notes
  - `void NoteCapturedVideoPair(const QString& originalPath, const QString& processedPath)` â€” appends synchronized playable MP4 controls and direct links to the HTML lecture notes
  - `void NoteAnnotationSnapshot(const QString& filePath, const QString& heading)`
    â€” appends a marked viewport PNG with a caller-supplied accessible heading
    to the HTML lecture notes
  - `QString notesFilePath() const` â€” absolute reserved notes path (empty
    until creation is queued; the file may not exist yet).
  - `bool HasPendingNotesWrites() const` includes queued work and completions
    awaiting delivery on the runtime thread. `OkuFlowApp::OpenNotesFile`
    defers opening until this clears. `NotesWriteFinished(path, error)` reports
    completion of accepted storage work.
- Manual-only text-to-speech of results via Qt TextToSpeech when built with `OKUFLOW_HAS_TTS=1`; the runtime prefers the `winrt` engine and falls back to Qt's default engine.
- Lecture notes: a valid per-session HTML document in the configured notes
  directory collecting escaped timestamped text readings, scene explanations,
  annotations, paired original/processed photos, fully finalized paired
  video segments. The head is written once and
  self-contained (no external resources): design-token CSS with light/dark
  themes (a top-right toggle persisted in localStorage, defaulting to the
  system scheme), expand/collapse-all controls, native `<details>/<summary>`
  cards for every section with the heading kept inside the summary for
  screen-reader heading navigation, WCAG-checked contrast, visible focus
  rings, and reduced-motion respect. Media uses portable relative links and
  responsive grids; videos include native browser controls plus direct
  links. During a session, new blocks append without rereading and rewriting
  the prior document; the closing HTML tags are finalized when the session
  closes, and an abrupt exit still leaves a fully styled, readable page.
  The `notes_html` ctest guards head structure, escaping, dedupe, and the
  appended-block invariants.
- Signals:
  - `OverlayUpdated(const QString& title, const QString& body, bool visible)`
  - Codex server/account/model/rate-limit/login state
  - Assistant conversation lifecycle, transcript, and streamed turn events

### `include/okuflow/common/response_language.hpp`
`okuflow::AppendResponseLanguageDirective(...)`
- Keeps built-in and user-authored model instructions intact while appending
  `Respond in <registry English language name>.` for the selected UI language.
  English adds no directive. This helper is shared by the Codex and
  OpenAI-compatible VLM request paths.

### `include/okuflow/common/image_processing.hpp`
Namespace `okuflow::processing`
- Format conversion helpers:
  - `CopyArgbToBgra(...)`
  - `CopyRgbxToBgra(...)`
  - `ConvertNv12ToBgra(...)`
  - `ConvertYuy2ToBgra(...)`
  - CPU pipeline dispatch plus the NV12/YUY2 helpers reject dimensions above
    16384 per axis, undersized strides/source buffers, overflowed size
    calculations, and unsafe odd-width packed layouts before writing output
- CPU effect helpers:
  - `ApplyBlackWhite(...)`
  - `ApplyZoom(...)`
  - `ApplyGaussianBlur(...)`
  - `ApplyTemporalSmoothCpu(...)`

### `include/okuflow/common/frame_pipeline.hpp`
Namespace `okuflow::processing`
- `struct CpuPipelineConfig`
  - `enableBlackWhite`, `blackWhiteThreshold`
  - `enableZoom`, `zoomAmount`, `zoomCenterX`, `zoomCenterY`
  - `enableBlur`, `blurRadius`, `blurSigma`
  - `enableTemporalSmooth`, `temporalSmoothAlpha`
- `struct CpuPipelineOutput`
  - `data`, `width`, `height`, `isComposite`

`CpuFramePipeline`
- Owns the CPU fallback pipeline and the intermediate stage buffers.
- Public API:
  - `bool ConvertFrameToBgra(...)`
  - `bool RotateRawBuffer(int quarterTurns, UINT& width, UINT& height)`
    - Normalizes positive/negative quarter turns, preserves every BGRA byte,
      and swaps retained input/rotation vectors. Every destination pixel is
      overwritten; equal-size rotated frames reuse both allocations after
      warm-up. Resolution growth can allocate; buffers retain peak capacity.
  - `CpuPipelineOutput BuildStages(UINT width, UINT height, const CpuPipelineConfig& config, bool debugViewEnabled)`
  - `void ResetTemporalHistory()`
  - `const std::vector<uint8_t>& StageRaw() const`

### `include/okuflow/common/media_writer.hpp`
`okuflow::GpuVideoFrame`, `okuflow::VideoRecorder`
- `GpuVideoFrame` identifies one shareable BGRA D3D texture, the shared fence
  value that completes its producer write, its dimensions, and an opaque
  lifetime lease. The independently armed conversion-reader graph retains that
  lease until confirmed GPU completion. Media Foundation owns the separate
  NV12 encoder output and does not keep the BGRA pool slot for encoder latency.
- Media Foundation sink-writer wrapper for live AV1 or H.264 output with an
  optional AAC audio stream. The
  container is fragmented MP4 (fMP4): fragments flush to disk while recording,
  limiting potential loss after a process failure. A retained incomplete file
  is never described as playable without verification. Files keep `.mp4`.
- `enum class Codec` â€” `Av1`, `H264`
- `struct AudioFormat` â€” 48 kHz mono 16-bit PCM input description used to
  configure the AAC sink stream
- `enum class StopReason` â€” `None`, `Manual`, `DiskFull`, `WriteFailed`; why
  the recorder last transitioned from recording to stopped.
- `enum class FinalizeDisposition` and `struct FinalizeResult` â€” distinguish
  nothing-to-finalize, completed, and truncated outcomes; include the original
  failure HRESULT, estimated duration, accepted `videoSamplesWritten`, and
  `HasPlayableVideo()` predicate. Accepted samples do not verify playback.
  The terminal result is cached across automatic writer stop and repeated
  `Stop()` calls until the next `Start()`.
- Public API:
  - `VideoRecorder()`
  - `~VideoRecorder()`
  - `bool Start(const std::wstring& filePath, UINT width, UINT height, UINT
    frameRateNumerator, UINT frameRateDenominator, Codec codec, const
    AudioFormat* audioFormat = nullptr)` â€” starts the requested live video
    encoder and optional AAC stream with the exact negotiated rate; refuses to
    overwrite an existing output or start with under 500 MB free on the target
    volume (`LastError()` explains why)
  - `bool StartGpu(..., const GpuVideoFrame& probeFrame, ...)` â€” starts the
    same writer with an adapter-matched D3D11 device manager; unsupported
    hardware routes use the existing CPU writer
  - `bool AddGpuFrame(const GpuVideoFrame&, const
    RecordingFrameIdentity&)` â€” opens the pooled D3D12 allocation through its
    NT handle, queues a GPU fence wait, converts the BGRA recording canvas to
    encoder-native NV12 through a D3D11 VideoProcessor, and submits the NV12
    `IMFDXGIBuffer` without reading pixels into system memory. This avoids
    placing a D3D-unaware BGRA color converter ahead of the hardware encoder.
    A nonblocking D3D11 event query retains the source texture lease and
    conversion resource graph independently of Media Foundation sample
    ownership until the GPU read completes. Normal admission polls without
    flushing; at the 48-reader limit, the recording worker allows query
    progress for at most 100 ms before reporting a failure. Finalization
    uses the same bounded, flush-capable retirement poll; unresolved or
    unknown reads remain retained rather than authorizing premature reuse.
    `PollGpuReadLeases(bool allowFlush = false)` selects that polling mode;
    only an actual completed query releases a reader graph.
    Public worker-only `PollGpuInputReads(bool allowFlush = false)` allows the
    manager to retire readers
    without waiting for another video/audio arrival; `HasPendingGpuReads()`
    selects a timed idle wake while such queries remain. With no queued video,
    the manager permits query-command flushing: `DONOTFLUSH` alone failed to
    retire the first conversion on the tested device. Each call still performs
    one asynchronous query per lease, without a completion wait or retry loop.
    These methods never submit or finalize a sink sample.
    Before `WriteSample`, the D3D11 video context is flushed and the DXGI
    media buffer's current length is set to its maximum length. Media
    Foundation creates a DXGI buffer with zero valid bytes initially, and the
    NVIDIA encoder rejects that nominally empty sample with `E_INVALIDARG`.
    A worker-side compatibility readback remains the permanent fallback when
    the adapter cannot create the video-processor path.
  - `FinalizeResult Stop()` â€” returns the cached terminal result after an
    automatic stop, preserving the original write/finalization error. A failed
    pending sample is discarded rather than retried during teardown
  - `bool IsRecording() const`
  - `bool AddFrame(const uint8_t* bgraData, size_t strideBytes, const RecordingFrameIdentity& identity)` â€” writes a variable-frame-rate sample from the normalized camera timestamp; unknown timestamps advance using the exact negotiated ratio. Free space is re-checked every ~5 seconds; below 200 MB the recording is finalized cleanly and `AddFrame` returns false with `StopReason::DiskFull` (the file is already intact on disk)
  - `bool AddAudioFrame(const std::uint8_t* pcmData, std::size_t byteCount,
    std::int64_t sampleTime100ns, std::int64_t duration100ns)` â€” writes one
    normalized PCM block into the optional AAC stream using the recording
    manager's segment-relative monotonic timestamp
  - `double DurationSeconds() const`
  - `const std::string& LastError() const`
  - `static const char* CodecName(Codec codec)`
- Internal helpers:
  - `InitializeSink(...)`
  - `FinalizeAndStop(StopReason reason)`
  - `SetError(const std::string& err)`

### `include/okuflow/common/recording_preservation.hpp`
`okuflow::RecordingTerminalResult`, `okuflow::CanRemoveEmptyRecording`
- `RecordingTerminalResult<Result>` retains the first terminal writer result
  through teardown and repeated Stop calls; `Reset()` begins a new recording.
- `CanRemoveEmptyRecording(...)` permits cleanup only for a session-owned,
  regular, non-symlink zero-byte output with no accepted video samples. Small
  or header-only nonempty files and unknown-size files are preserved.

### `include/okuflow/common/gpu_read_retirement.hpp`
`okuflow::GpuReadRetirement`
- Tracks an independently armed GPU source-reader lease. Completed queries
  release it, pending queries retain it, and unknown completion latches
  retention. The writer uses this policy in addition to encoder-held sample
  leases so an early sample release cannot recycle a texture still being read.

### `include/okuflow/common/recording_contract.hpp`
`okuflow::RecordingFrameIdentity`, `okuflow::RecordingTimeline`,
`okuflow::RecordingCanvasMode`, `okuflow::RecordingCanvasSize`,
`okuflow::RecordingViewTransform`, `okuflow::RecordingDropCounts`,
`okuflow::RecordingState`, `okuflow::RecordingCompletionOutcome`
- Defines the hardware-independent contract shared by capture, recording, and
  tests. `RecordingTimeline` maps camera timestamps to a monotonic, zero-based
  Media Foundation timeline and falls back to exact fractional-rate timing.
- `ResolveRecordingCanvas(...)` and `ResampleRecordingCanvas(...)` preserve
  canonical Fill/Fit geometry on a fixed processed-video canvas from 360p
  through 2160p; landscape/portrait orientation follows the source.
- `IsValidRecordingStateTransition(...)` and
  `ClassifyRecordingCompletion(...)` centralize legal lifecycle transitions
  and terminal truthfulness. `RecordingDropCounts::Total()` excludes estimated
  timestamps while aggregating real frame losses.

### `include/okuflow/common/maxine_superres.hpp`
`okuflow::MaxineSuperRes`
- GPL-clean runtime-only adapter for NVIDIA Video Effects SuperRes. The
  implementation includes the staged MIT headers but resolves every
  proprietary entry point from `NVCVImage.dll` and `NVVideoEffects.dll` with
  `LoadLibraryExW`/`GetProcAddress`; it has no import-library dependency.
- Discovery checks an explicit override, `OKUFLOW_MAXINE_PATH`,
  `NV_VIDEO_EFFECTS_PATH`, the standard Program Files directory, and uninstall
  registry views. Models are loaded from the detected runtime's `models`
  subdirectory. A missing runtime is cached as unavailable without crashing.
- Converts BGRA8 device memory to/from planar BGR float using
  `NvCVImage_Transfer`, runs the effect on the caller's CUDA stream, and never
  reads a frame back to the host.
- Public API:
  - `MaxineSuperRes()` / `~MaxineSuperRes()`
  - `bool Ensure(...)` for explicit source/destination dimensions or integer scale
  - `bool Run(...)` validates pitched crop views and follows NVIDIA's
    synchronous inference path before copying the complete result into the
    device destination, preventing previous-frame output from appearing as a
    moving ghost layer
  - `void SetStrength(float strength)` / `void Teardown()`
  - `bool IsReady() const`
  - `const std::string& LastError() const`
  - `const std::wstring& RuntimeDirectory() const`
  - `static std::wstring FindRuntimeDirectory(...)`
  - `static bool IsRuntimeInstalled(...)`

### `include/okuflow/common/annotation_model.hpp`
`okuflow::AnnotationTool`, `okuflow::AnnotationItemKind`,
`okuflow::AnnotationStroke`,
`okuflow::AnnotationModel`
- Session-owned vector annotation state. Freehand strokes, two-point straight
  lines, rectangle/ellipse outlines, and text labels share one item model.
  Items carry Solid/Dashed style where applicable. Points and widths use
  normalized processed-scene coordinates rather than viewport pixels, so ink
  stays registered while the canonical viewport pans or zooms.
- Editing supports scene-space hit testing, one-item click selection and
  rectangle-based multi-selection through `SelectInRect(...)`.
  `selectedStrokeIds()` exposes the ordered selection set while the legacy
  `selectedStrokeId()`/`selectedStroke()` accessors identify its primary
  (last-selected) item. Selected groups move, nudge, delete, and undo as one
  transform gesture. Single-item selections retain bounding-box feedback with
  eight resize handles and pointer/keyboard scaling. Whole-item erase, clear,
  bounded undo/redo, and permanent session reset remain supported.
- `BuildStrokePath(...)` is the shared geometry source for paint, hit testing,
  selection, and scale bounds. `RenderAnnotationStrokes(...)` is shared by the
  live overlay and saved PNG path. It maps through `ViewTransform` and draws a
  matched dashed or solid dark halo under each high-contrast core stroke, so
  dashed gaps stay transparent.
- `AnnotationSceneTolerance(...)` converts viewport-pixel widths and hit
  tolerances into normalized scene distance.

### `include/okuflow/common/view_transform.hpp`
`okuflow::ViewportFitMode`, `okuflow::ViewTransform`,
`okuflow::NormalizedSourceRect`, `okuflow::PixelViewMapping`
- Define the single camera-to-viewport geometry contract used by the D3D12
  shader, CPU fallback, pointer mapping, focus marker, and cached SuperRes ROI.
  The mapping always uses one uniform scale.
- `ComputeViewTransform(...)` computes aspect-safe Fill/crop or Fit/letterbox
  source and destination rectangles from rotated scene size, native viewport
  pixels, zoom, and focus.
- `RemapViewTransformToSourceRect(...)` expresses the requested scene view
  inside a cached ROI and rejects views that fall outside it.
- `ComputePixelViewMapping(...)` derives integer destination bounds and source
  sampling increments for the CPU fallback from the same transform.

### `include/okuflow/common/ultra_clear_text.hpp`
`okuflow::uct::SourceStamp`, `ProbeFrame`, `Recognizer`, `BackendStatus`,
`RecognitionScheduler`, `RecognitionResult`, `ReferencePatch`, `PatchMatch`,
`OverlayModel`, `AcceptedLine`, `SceneWord`, `DisplayWord`, `AdmitVerdict`
- Ultra Clear Text core (Qt Core/Gui only). `SourceStamp` carries camera
  epoch, UCT geometry generation, frame sequence, request time and the exact
  full-scene `ViewTransform` of a viewport copy; results and accepted lines
  keep the stamp of their own pixels. `temporallySmoothed`, `historyEpoch`,
  `seedFrameSeq`, `blendFrameCount` and `temporalAlpha` preserve OCR blend
  provenance, while the reference/probe always describes its exact current
  contributing frame.
- `ViewPixelToScene`/`SceneToViewPixel`/`ViewRectToScene`/`SceneRectToView`
  map through one transform via normalized coordinates (DPI applied once).
- `MakeProbeFrame(...)` builds a decimated luma copy plus summed-area table.
  `BuildReferencePatch`/`MatchPatch` correlate box-filtered scene cells
  (zoom-consistent) with a bounded translation search; `SceneSimilarity`
  compares two probes over their common scene area.
- `RecognitionScheduler` owns one worker, one in-flight job and one
  latest-only pending slot; its QImage `Submit` copies caller-owned pixels and
  its SharedPixels overload transfers shared immutable ownership. `Cancel` revokes
  authority without releasing in-flight input, destruction is bounded and
  detaches a held worker that keeps its own state.
- `OverlayModel::Admit` rejects stale scene, late, failed and moved-content
  results, requires temporal agreement and drops `IsHighRiskToken` words;
  `Validate` revalidates lines against newer probes; `SceneWords`/
  `DisplayWords` apply the 350 ms freshness rule; `ReadingText` orders lines
  and marks omissions with an ellipsis.
- Smoothed live/advancing candidates in `AdmitImpl` start unvalidated even
  after OCR agreement. `ApplyChecks` refuses an older frame as proof for a
  new smoothed line. `smoothedResultsNeedCurrentFrameProbeProof` covers
  deferred visibility, older-probe refusal and removal on motion.
  `RecognitionResult::currentInputMatches` contains worker-computed smoothed
  versus current-frame registration per line: every candidate word must match
  in place. `kInputRegistrationTuning` requires 0.94 correlation, 0.95 visibility
  and zero search/refinement on CPU and GPU. `MatchTuning::searchRadiusPx` now
  admits zero as an explicit static-registration mode; motion defaults stay
  unchanged. `resident::Prepare` uses a transient OCR integral and batches of
  at most 1024 word descriptors, with at most 8192 words, returning booleans only;
  CPU fallback compares its two worker-owned images. `AdmitImpl` drops a
  missing/failed input match rather than inventing new evidence. Only an exact
  old line independently validated at/after that frame may survive, retaining
  its original reference, identity and proof clock. Neighbour glyph evidence
  excludes failed smoothed-input registration too.
  `smoothedProbeFallbackDownloadsOnlyTheCurrentProbe` guards the independent
  CPU probe path against an extra full-frame validation readback.
- `CreateDefaultRecognizer()` (in `src/common/uct_default_recognizer.cpp`)
  returns the Windows OCR adapter: grayscale, dark-background inversion and
  the engine's maximum dimension, mapping boxes back to copy pixels.

### `include/okuflow/common/uct_windows_ocr.hpp`
`okuflow::uct::windows_ocr::Engine`
- Qt-free C++/WinRT wrapper of `Windows.Media.Ocr` compiled without the Qt
  precompiled header. `Initialize()` joins the worker to the MTA and creates
  an engine from profile languages or the first installed OCR language.
  `Recognize(...)` copies BGRA rows into a `SoftwareBitmap`, waits through its
  own completion handler, requests `Cancel()` on cancellation and still waits
  (bounded) before returning.

## D3D12 Module

### `include/okuflow/d3d12/presenter.hpp`
`okuflow::ViewportPresentationOptions`
- Optional per-present focus marker and readback request used by the viewport
  shader pass.

`okuflow::D3D12Presenter`
- Manages the D3D12 device, native-client-sized swap chain, viewport shader,
  per-frame upload buffers, shared fence, readback rings, and a bounded pool of
  shareable recording textures.
- `PresentSceneTexture(...)` samples a persistent processed scene through
  `ViewTransform` using a full-screen triangle and bilinear sampler. The back
  buffer remains the render HWND's native pixel size; camera texture dimensions
  never resize it, so DXGI cannot stretch one camera axis independently.
- Presentation uses two frame slots plus the swap chain frame-latency waitable
  object. `TryAcquireFrameSlot()` first checks allocator completion, then polls
  the latency object with zero timeout. A busy slot counts a missed attempt
  and retains dirty state without resetting an allocator or submitting work.
  `NeedsScenePresent()`, viewport dimensions, and
  `MissedPresentCount()` expose scheduler/diagnostic state.
- The presenter alone owns the monotonic shared fence. CUDA reserves an
  external value through `ReserveExternalSignal()`, then commits it only after
  enqueue or cancels a failed reservation after a bounded drain. A canceled
  hole is never a wait target. Every graphics Execute/drain path queues the
  latest committed CUDA dependency before reserving a graphics value, even
  when a viewport present was skipped. `GetLastSignaledFenceValue()` remains
  the latest graphics submission, not the latest CUDA reservation.
- Native resize requests are coalesced by `RenderWidget`. `Resize(...)` drains
  in-flight back buffers before `ResizeBuffers`, while CPU upload buffers are
  recreated lazily only if the CPU path next presents at the new size.
- Public API:
  - `D3D12Presenter()`
  - `~D3D12Presenter()`
  - `void Initialize(HWND hwnd, UINT width, UINT height)`
  - `bool IsInitialized() const`
  - `void Resize(UINT width, UINT height)`
  - `void Present(const uint8_t* data, UINT width, UINT height)`
  - `bool PresentSceneTexture(ID3D12Resource*, UINT sourceWidth,
    UINT sourceHeight, const ViewTransform&, const FenceSyncParams*,
    const ViewportPresentationOptions*, UINT64* outReadbackRequestId,
    UINT64* outIndependentReadSignal)`
  - `bool ReadbackTexture(ID3D12Resource* texture, UINT width, UINT height,
    std::vector<uint8_t>& outBgra, UINT64 waitFenceValue = 0)` â€” optionally
    queues a shared-fence wait before the blocking GPU copy, used by on-demand
    Read and Assistant frame attachment to avoid reading active CUDA writes
  - `bool RequestReadback(ID3D12Resource* texture, UINT width, UINT height, UINT64* outRequestId = nullptr)` â€” enqueues an async copy into a four-slot ring, optionally returning its fence-backed request id; returns false when every slot is in flight
  - `bool HasReadbackRequest(UINT64)` - reports an exact active ring receipt,
    allowing an owned snapshot to retry after resize dropped its old copy.
  - `GpuVideoFrame RequestRecordingFrame(...)` â€” samples the canonical
    recording transform into a leased shareable BGRA texture. The lazily grown
    pool is capped at 384 MiB and 48 slots; a null result with
    `outPoolExhausted=true` lets recording count a drop rather than block or
    allocate without limit. The producer fence remains mandatory in addition
    to the source-reader lifetime lease. Optional BGRA annotation pixels are
    uploaded into a per-lease texture and alpha-composited after scene
    framing. Only the processed caller supplies this layer, so original
    recordings remain clean.
    Its optional `UINT64 waitFenceValue = 0` queues the CUDA producer
    dependency explicitly, so recording clones remain valid when viewport
    admission is busy. The presenter reserves and publishes the actual clone
    completion on its timeline. Optional `independentSceneSync` instead waits
    and signals the private media producer/read fence, returning the actual read
    signal even when a later operation fails. `viewportOptions` retains focus
    marker geometry in an exact selected-view clone. The no-semaphore fallback retains its bounded
    completion drain before the next CUDA overwrite. Pool reuse and
    obsolete-dimension eviction both require the producer fence to retire,
    as well as consumer-reader lease release; pending or device-lost completion
    cannot authorize reuse.
  - `bool TryGetCompletedReadback(std::vector<uint8_t>& outBgra, UINT& outWidth, UINT& outHeight, UINT64* outRequestId = nullptr)` â€” moves the oldest completed request's tightly packed BGRA8 pixels out and optionally returns the matching request id. Pending requests are silently dropped by `Resize`
  - `ID3D12Device* GetDevice() const`
  - `ID3D12Fence* GetFence() const`
  - `UINT64 GetLastSignaledFenceValue() const`
  - `ReserveExternalSignal()`, `CommitExternalSignal(UINT64)`,
    `CancelExternalSignal(UINT64)`, `LastGraphicsSignal()`, and
    `LastExternalSignal()` are the presenter-thread CUDA/graphics timeline
    contract. A failed or unknown CUDA drain leaves the pending reservation
    unavailable rather than allowing a conflicting graphics submission.
  - `bool WaitForIdle() noexcept` drains submitted GPU work with a 1000 ms
    deadline and device-removal probes. False latches `IsFaulted()` and never
    grants resource reuse/release. Presentation, resize, and readback catch
    errors and stop submission on terminal faults. Destruction quarantines
    the still-owned GPU resource graph instead of freeing it.
  - `WaitForFenceDeadline` in `d3d12/fence_wait.hpp` verifies actual fence
    completion after wakeups, rejects the device-removal sentinel, and bounds
    waits at explicit ownership boundaries. Normal frame admission does not
    use those blocking waits; `Present(1, 0)` retains synchronized presentation.
  - `FrameReadiness` / `PollFrameReadiness` in `d3d12/frame_readiness.hpp`
    distinguish Ready, Busy, DeviceLost, and WaitFailed without consuming a
    latency signal while the selected allocator is still in flight.
  - `SharedFenceTimeline` in `d3d12/shared_timeline.hpp` implements monotonic
    external reservation/commit/cancel and one queued dependency per committed
    value; `PollRecordingSlot` in `d3d12/recording_slot_policy.hpp` applies
    producer completion and device-loss checks to pool reuse and eviction.
  - `OkuFlowApp::HandlePresenterFault` also handles CUDA terminal faults:
    it stops frame processing and reports that restarting is required.

## CUDA Module

### `include/okuflow/cuda/cuda_interop.hpp`
Supporting types:
- `struct FenceSyncParams`
  - `enable`, `waitValue`, `signalValue`
- `struct KeystoneTrackingState`
  - `paused`, `canStepBack`, `canStepForward`, `stepPending`, `position`, and
    `count` describe the bounded accepted-correction history exposed to Qt
- `struct SuperResRoiMetadata`
  - identifies the generation, normalized source rectangle, output extent, and
    scale of the shared Maxine or NIS/FSR cache texture. Viewport
    presentation uses it only when the current `ViewTransform` is contained in
    that ROI; otherwise it immediately presents the registered conventional
    scene rather than blending mismatched images.
- `enum class SpatialUpscaler`
  - `kFsrEasuRcas`
  - `kNis` â€” the persisted and runtime default
- `enum class CudaBufferFormat`
  - `kRgba8`
  - `kRgba16F`
- `enum class DisplayColorTransform`
  - `kNone` preserves full color and keeps the identity fast path
  - `kInvert` preserves the legacy per-channel inversion path
  - `kLumaLut` maps byte luma through the active 256-entry table
- `struct ProcessingSettings`
  - `TextStageActivity` and `TextClarityStages()` select the nine child effects
    from the master and each individual preference. CUDA dispatch consumes this
    selection directly; enabling the master never re-enables an unchecked
    refinement. `pipeline` reports whether a text-processing entry stage runs.
  - `ApplyTextClarityMaster()` suppresses all nine child text-stage flags on
    an effective frame settings copy when Text Clarity is off. Saved options,
    independent Black & White, display colors, and Maxine remain unchanged.
    Both the application builder and direct `ProcessFrame` entry apply it.
    Master transitions reset text-mask and temporal color history; pending
    focus copies keep their owned slot until completion and discard stale
    results instead of republishing them after re-enabling.
  - toggles and parameters for BW, zoom, blur, focus marker, spatial sharpening, temporal smoothing, and staging format
  - `zoomAmount` describes viewing magnification independently of `enableZoom`,
    which controls only the legacy CUDA image-zoom stage.
    `EffectiveViewingMagnification()` returns a finite value of at least 1x,
    defaulting invalid inputs to 1x. Stabilization uses that magnification for
    visible-region matching and display-pixel deadband even when presentation
    owns image enlargement.
    `RunCudaPipeline` sends 1x in Fit mode because that geometry suppresses
    requested zoom. `spatialViewTransform` and `spatialViewportWidth/Height`
    supply canonical geometry to the spatial enlargement stage.
  - stabilization: `enableStabilization` selects the one full-strength CUDA
    fixed-reference path. Transient `enableBumpHold`, presented to users as `Extra Stable (hold on shake)`,
    retains the last sharp stabilized GPU frame through rejected/blurred
    impacts and crossfades back after a stable recovery window
  - display grading: `displayColorTransform`, host-owned `displayColorLut`, and
    monotonic `displayColorLutGeneration`; `textForegroundBgra` and
    `textBackgroundBgra` keep Text Clarity on the same scheme endpoints;
    `contrast` (0.25..4.0) and `brightness` (-1..1) remain independent
  - screen fix: `enableKeystone` (auto-detect the projected slide quad and warp it fronto-parallel), `enableAutoContrast` (percentile level stretch before contrast/brightness), `autoContrastStrength` (0..1 blend toward the full stretch)
  - text clarity: master/individual toggles and strength values for background
    flattening, adaptive Sauvola, soft edges, polarity, stroke weight, smart
    sharpening, CLAHE, two-color mapping, hysteresis, selective sharpening,
    focus detection and glare suppression
  - Maxine SuperRes: `enableMlSuperRes`, `mlSuperResStrength` (0..1), and
    `mlSuperResUltra1440p` for full-frame inference into the separate
    high-resolution cache
  - `deferMlSuperResForInteraction` is transient scheduling intent from the
    viewport scheduler, never a saved mode or user toggle. The SuperRes stage
    polls old timing but bypasses model loading and inference during navigation,
    invalidates the old SR ROI for that frame, and leaves the normal current-frame
    spatial/base path to present and capture. Model ownership, failure latches,
    performance override and the measured timing window remain intact.
- `struct ProcessingInput`
  - `hostPixels`, `hostStrideBytes`, `pixelSizeBytes`, `width`, `height` â€” `width`/`height` always describe the host pixel layout (pre-rotation)
  - optional `d3d11Texture` plus `d3d11Subresource` and a required
    `d3d11TextureLease` bypass host staging. The
    texture is BGRA8 and enters CUDA through the D3D11 â†’ D3D12 â†’
    `cudaImportExternalMemory` bridge.
  - `inputFormat` â€” 0=BGRA8 (existing CPU-converted path), 1=NV12 (`hostPixels` = Y plane, `hostPlane2` = interleaved UV plane with `hostPlane2StrideBytes`), 2=YUY2 (packed in `hostPixels`)
  - `rotationQuarterTurns` â€” 0..3 clockwise, applied on the GPU after conversion for raw formats only (ignored for BGRA with a one-shot warning). For odd turns the interop surface must be created at the post-rotation extent (height x width); `ProcessFrame` validates and returns false on mismatch

`okuflow::CudaInteropSurface`
- Imports a D3D12 texture into CUDA and runs the GPU effect chain.
- Teardown and buffer reallocation use bounded stream completion polling;
  failed drains latch a terminal fault and preserve allocations, owning CUDA
  state, and retained D3D resources/fence until process exit.
- After a proven drain, file-local `ReleaseMappedExternalImage(...)` destroys
  each surface object, clears its non-owning level-zero view, frees its mapped
  mipmapped array, then destroys external memory. Normal teardown and partial
  construction cleanup apply this to scene, SuperRes, and original imports.
- Host upload uses two `cudaMallocHost` staging slots shared by BGRA, NV12,
  and YUY2. Only the Qt tick writes/rotates the slots. A per-slot CUDA event,
  recorded immediately after the final H2D copy, guards the next host write;
  shared D3D/CUDA fence values cannot protect host-side memory reuse.
- The accelerated top rung exposes Media Foundation's reusable NT-shareable
  D3D11 VideoProcessor output to the existing D3D12 device, imports that
  allocation once with `cudaExternalMemoryHandleTypeD3D12Resource`, caches its
  level-zero array by texture identity, and issues one device-to-device copy
  into the normal pitched processing buffer per frame. A completion event
  protects the producer lease; no per-frame processing-stream synchronization
  is required. Teardown frees the
  mapped mip array before destroying external memory. Import/copy failures are
  classified separately before submission so the application can retain the
  CUDA effects pipeline and move down the camera ladder. Once a copy may be
  in flight, failure retains source/destination ownership and is terminal.
- Public API:
  - `explicit CudaInteropSurface(ID3D12Resource* texture, ID3D12Fence* sharedFence = nullptr)`
  - `~CudaInteropSurface()`
  - `bool IsValid() const`
  - `bool HasExternalSemaphore() const`
  - `void RunGradientDemoKernel(unsigned int width, unsigned int height, float timeSeconds)`
  - `bool ProcessFrame(const ProcessingInput& input, const ProcessingSettings& settings, const FenceSyncParams& fenceSync)`
  - `const std::string& LastError() const`
  - `bool LastFailureWasCaptureInterop() const`
  - `void ResetCaptureInterop(bool atProcessExit = false)` releases completed
    imports; pending or failed imports/leases are retained without blocking.
    `AbandonCaptureInterop() noexcept` preserves the complete import state
    without driver calls or destruction when producer quiescence is unknown.
  - `bool PollCaptureCopy()` nonblockingly releases a completed camera lease;
    false means pending unless `IsFaulted()` is true. `GpuCopyLease` permits
    one outstanding lease, and retains it on error or 1000 ms expiry.
  - `bool WaitForIdle() noexcept` uses stream queries with a 1000 ms deadline.
    `bool IsFaulted() const` exposes a sticky failure, distinct from ordinary
    initialization unavailability.
  - `void ResetTemporalHistory()`
  - `void ResetStabilization()` â€” clears previous luma/projections, fixed
    reference maps, absolute correction state, diagnostics, and timing; the app
    calls it on camera switch/stop, rotation, and profile changes
  - `const std::string& StabilizerStatus() const` / `float
    LastStabilizerMs() const` â€” accepted estimator, sampled inlier/correction
    summary, and non-blocking sampled GPU duration exposed to Advanced
    diagnostics
  - `void ResetKeystone()` â€” clears the keystone detection state (smoothed source-quad corners, pending luma snapshot); the app calls it alongside every `ResetStabilization()` and when the keystone toggle changes
  - `void SetKeystoneTrackingPaused(bool paused)` â€” freezes or resumes periodic
    projected-quad detection without changing the current warp
  - `bool StepKeystoneCorrection(int direction)` â€” freezes tracking, restores
    the previous/next accepted corner set, or requests one fresh detection when
    stepping forward from the newest entry
  - `KeystoneTrackingState GetKeystoneTrackingState() const` â€” returns the
    current history and pending-step state; history is capped at 32 entries
  - `void ResetTextClarityHistory()` â€” invalidates the previous binary mask
  - `bool HasFocusScore() const` / `float LatestFocusScore() const` /
    `bool IsFocusAcceptable(float threshold) const`
  - `const std::string& SuperResStatus() const`, `bool IsSuperResActive() const`,
    and `void ResetSuperRes()` expose/reset the optional runtime tier;
    `SuperResSourceWidth()/SuperResSourceHeight()/SuperResFactor()` report the
    crop and the fixed scale factor of the active AI stage
  - `IsSuperResPerformanceLimited()` reports a latency
    guard decision against the 24 ms target;
    `SetSuperResPerformanceOverride(bool)` explicitly enables or restores the
    guard
  - `float LastGpuFrameMs() const` â€” P8 GPU timing: duration of the last
    sampled ProcessFrame kernel chain (cudaEvent pair recorded every 30th
    frame, polled non-blockingly by `ConsumeProcessTiming()`); negative until
    the first sample completes
  - `GpuStageTimings LastGpuStageTimings() const` â€” non-blocking sampled CUDA
    event durations for input/upload, stabilization and geometry, image
    effects, and output/SuperRes-cache publication. The boundaries share the
    existing every-30th-frame sample and add no stream synchronization.
- SuperRes configuration changes retire prior work before reloading the SDK's
  load-time strength/mode selectors; unrelated Text Clarity controls retain
  the loaded model and its timing history.
  Enabling viewport-target mode restores the default 0.65 strength when zero
  and enforces its 1.33x minimum zoom before the next frame. The CUDA path
  snaps to the largest supported Maxine factor (4/3, 1.5, 2, 3 or 4) at or
  below the requested zoom whose source crop maps exactly onto the viewport,
  crops around the actual clamped zoom center, and applies residual
  magnification with the GPU sampler around the same mapped center. Ultra mode
  instead imports a second D3D12/CUDA texture whose extent may differ from the
  camera-sized primary scene, allocates a matching pitched CUDA output, runs
  the full source frame to at most 2560x1440 (or 1440x2560 after rotation), and
  lets the same presenter apply pan/zoom afterward. 720p uses 2x, 1080p uses
  4/3x, and 1440p remains native. Setup failures latch per
  (crop, factor) key and retry only when the key changes or the toggle is
  re-enabled. The model directory resolves to the runtime's `models`
  subdirectory, falling back to the DLL directory itself.
- Stage order: upload/convert/rotate â†’ stabilization (including optional
  post-warp Extra Stable hold) â†’ keystone â†’ Text Clarity
  analysis/flatten/CLAHE/mask/smart-sharpen â†’ legacy BW â†’ native-size spatial
  sharpening when no enlargement is needed â†’ blur â†’ temporal â†’ auto-contrast
  â†’ color grade â†’ focus marker â†’ full-scene interop copy â†’ Maxine or bounded
  spatial ROI cache. Maxine discards 10 warmup timings, averages the next
  60 effect runs with CUDA events, and disables that inference configuration above
  the 24 ms target unless the user explicitly overrides the guard.
- Stabilization uses a dedicated analysis surface capped at 640x360 (keystone
  remains capped at 320x180). The one on-stream path tracks a fixed reference
  from the previous accepted
  absolute-translation prediction and fits the expanded visible zoom region
  when at least 12 tracks are available there. Lock creation selects the
  sharpest of five frames, then caches a real three-level Gaussian pyramid,
  adaptive distributed subpixel corners, and inverse translation Hessians.
  Multi-frame reference accumulation is disabled because replaying a saved
  low-texture clamp recording showed that insufficiently registered samples
  degraded stabilization even though the synthetic accumulation checks
  passed. The steady-state inverse-compositional tracker rejects high-error
  patches and the deterministic tripod estimator fits translation only,
  avoiding false rotation/scale shimmer on a mounted camera.
  Its absolute measurement uses a zoom-scaled display-pixel deadband and
  crop-backed correction authority; invalid frames hold the correction
  indefinitely. Reference capture is automatic when stabilization starts or
  the camera/pipeline resets. A keyframe map, wide-range projection seed, and
  short-lived relative fallback recover the same absolute coordinate frame
  after a bump. The bilinear warp
  applies one uniform global correction. Three four-float diagnostic
  samples (fit metadata, correction, and measured frame motion) are copied
  asynchronously every 30 frames and emitted once to the temporary Release
  diagnostics terminal; there is no estimator frame readback or stream wait.
- The luma LUT is copied to CUDA constant memory asynchronously only when
  `displayColorLutGeneration` changes. Contrast and brightness edits reuse the
  resident table and do not trigger uploads.

### `include/okuflow/cuda/cuda_kernels.hpp`
`okuflow::StabilizationState`
- Device-resident x/y translation, rotation, and log-scale measurements;
  current/previous correction; last reliable frame motion; fixed-reference
  anchor correction; recovery state; and compact fixed-reference diagnostics.

`okuflow::TripodReferenceFeature`
- One prepared fixed-reference feature: subpixel level-0 position plus cached
  inverse 2x2 translation Hessians for the full, half, and quarter-resolution
  Gaussian pyramid levels.

`okuflow::BumpHoldState`
- Device-resident presentation state for the opt-in Extra Stable impact
  hold: live/held/recovery mode, focus ratio, transform step, previous absolute
  motion, stable-frame counter, crossfade progress, and whether a safe
  full-resolution frame has been captured.

Kernel launch wrappers:
- `LaunchBlackWhiteLinear(...)`
- `LaunchZoomLinear(...)`
- `LaunchGaussianBlurLinear(...)`
- `LaunchFocusMarkerLinear(...)`
- `LaunchFsrEasuRcasLinear(...)` â€” runs the pinned AMD FSR 1.0.2 FP32
  EASU reference equations into the caller-provided scratch surface, then
  applies RCAS into the destination. Source and destination dimensions may
  differ; the scratch surface must match the destination dimensions.
- `LaunchNisLinear(...)` â€” runs the pinned NVIDIA Image Scaling 1.0.3
  NVScaler coefficient banks, edge detector, directional filters, and
  adaptive sharpening for 1x through 2x enlargement.
- `LaunchTemporalSmoothLinear(...)`
- `LaunchStabilizationLumaDownsample(...)`
- `LaunchStabilizationProjections(...)` â€” accumulates 16x16 block row/column
  partials with shared-memory atomics, then merges one partial per bin into
  global memory for wide-range fixed-reference re-acquisition
- `LaunchStabilizationFeaturePairs(...)` â€” grid-spaced Harris selection and
  three-level sparse Lucas-Kanade tracking on small luma
- `LaunchVirtualTripodFeaturePairs(...)` â€” fixed-reference GPU tracking seeded
  from `StabilizationState::lastFrameMotion`, with full/source coordinate
  mapping, a forward/backward closure check, and an absolute-displacement gate
- `LaunchStabilizationLumaPyramid(...)` â€” builds real half- and
  quarter-resolution luma levels with a 5x5 binomial Gaussian before
  subsampling
- `LaunchPrepareVirtualTripodReference(...)` â€” selects one adaptive strongest
  corner per 16x16 reference cell, refines it to subpixel position, and caches
  inverse translation Hessians for all three pyramid levels
- `LaunchPreparedVirtualTripodFeaturePairs(...)` â€” tracks the cached reference
  features coarse-to-fine with inverse-compositional Lucas-Kanade, seeded by
  the last accepted absolute translation and rejected by final patch error
- `LaunchMeasureVirtualTripodFocus(...)` /
  `LaunchSelectSharperVirtualTripodReference(...)` â€” device-side Laplacian
  focus score and sharpest-candidate selection without a frame readback
- `LaunchResetBumpHoldState(...)`, `LaunchUpdateBumpHoldState(...)`, and
  `LaunchApplyBumpHold(...)` â€” capture only a sharp/model-valid settled frame,
  hold it through rejected, blurred, or discontinuous tripod measurements,
  keep registration advancing underneath, then crossfade back after five
  stable frames
- `LaunchInitializeVirtualTripodAccumulator(...)` /
  `LaunchAccumulateVirtualTripodReference(...)` /
  `LaunchFinalizeVirtualTripodReference(...)` â€” constructs the persistent
  reference from registered frames, rejecting low-focus, invalid-transform,
  out-of-bounds, and moving/photometric-outlier pixels
- `LaunchResetVirtualTripodState(...)` /
  `LaunchVirtualTripodSimilarityEstimate(...)` â€” continuity-preserving anchor
  capture plus deterministic translation-only absolute-reference RANSAC. The
  estimator accepts strength, focus center, and zoom so it can prefer the
  visible-region tracks and define its deadband in display pixels. Invalid
  models preserve the last correction.
- `LaunchStabilizationWarp(...)` â€” applies the current fixed-reference
  correction with center-relative bilinear inverse sampling
- `LaunchDisplayColorGradeLinear(...)` â€” takes the display transform plus
  optional device `autoContrastLevels` (float2 lo/hi) and
  `autoContrastStrength`; the LUT branch indexes constant memory by byte luma
- `LaunchNv12ToBgraLinear(...)` / `LaunchYuy2ToBgraLinear(...)` â€” BT.601 limited-range YUV to BGRA, integer math identical to the CPU converters
- `LaunchRotateQuarterLinear(...)` â€” rotate by 1/2/3 clockwise quarter turns (destination is srcHeight x srcWidth for odd turns)
- `LaunchKeystoneWarp(...)` â€” bilinear homography warp (output pixel to source pixel, row-major 3x3), black outside the source rect
- `LaunchAutoContrastHistogram(...)` / `LaunchAutoContrastAnalysis(...)` â€” 256-bin luma histogram plus 2nd/98th-percentile level derivation low-passed into device-resident levels
- `LaunchTextLocalStatistics(...)`, `LaunchTextSceneAnalysis(...)`,
  `LaunchBackgroundFlattenLinear(...)`, `LaunchClaheLinear(...)`,
  `LaunchSauvolaMask(...)`, `LaunchStrokeWeight(...)`,
  `LaunchTextMaskHysteresis(...)`, `LaunchTextMaskComposite(...)`,
  `LaunchSmartSharpenLinear(...)`, and `LaunchFocusMetric(...)`
- `LaunchSmartSharpenLinear(...)` accepts a trailing optional device
  `replacementAnalysis` pointer. Both bilateral and sharpening kernels leave
  destination and scratch untouched when its scene class is nonzero, matching
  the following mode-2 document composite's complete RGB replacement. The
  pipeline passes it only when that composite follows; mixed scenes retain
  ordinary sharpening. No host readback or synchronization is added.
- `SauvolaMaskKernel` / `LaunchSauvolaMask(...)` create an ink mask from local
  mean and variance. Light-on-dark text applies the ordinary dark-ink Sauvola
  formula to inverted luma and mean, retaining the original variance; reversing
  only the comparison would falsely mark uniform dark backgrounds as ink.
  Explicit polarity and the device analysis flag select the same branches.
  Algorithm reference: Sauvola and Pietikainen,
  [Adaptive document image binarization](https://www.sciencedirect.com/science/article/pii/S0031320399000552)
  (2000); intensity inversion is OkuFlow's light-text adaptation.
- `bool UploadGaussianKernel(int radius, float sigma, cudaStream_t stream)` â€”
  normalizes exact Gaussian weights and queues the live weight/radius symbols
  on `stream`; no unused size symbol or device-wide synchronization remains
- `bool UploadDisplayColorLut(const std::uint32_t* lut256, cudaStream_t)` queues
  one 256-entry BGRA table upload on the processing stream

## UI Module

### `include/okuflow/ui/ui_translation.hpp`
Namespace `okuflow`
- `TranslateUi(...)` translates complete English source strings through the
  shared `OkuFlow` catalog context. It supports `%1`-style formatted status
  templates without translating fragments.
- `SetLiveTranslationSource(...)` records source-language dynamic text and
  its semantic role so language changes can rebuild visible and accessible
  text rather than translating a prior translation.
- `SetLiveAccessibleDescription(...)` records a replaceable source description,
  including an empty value, for dynamic shortcut metadata and emits a
  DescriptionChanged accessibility event when the applied value changes.
- Internal `TranslateTrackedProperty(...)` tracks the last applied translation
  so application updates to labels/tooltips become the new source instead of
  being overwritten by construction-time text on Show/Polish.
- `RetranslateWidgetTree(...)` captures and reapplies hand-built widget text,
  titles, placeholders, tooltips, accessible metadata, tabs, and ordinary
  combo entries on `QEvent::LanguageChange`.
- `SetComboItemsAreData(...)` exempts camera names, voices, model ids, and
  other data-driven combo entries while still translating their surrounding
  UI metadata.

### `include/okuflow/ui/ultra_clear_text_overlay.hpp`
`okuflow::UltraClearTextOverlay`
- Click-through, non-activating tool window over the render target that
  paints accepted scene-space words as glyph plates through the latest
  `ViewTransform`. Measured colors are kept at â‰¥ 7:1 contrast, otherwise
  black/white in scene polarity; weight follows Text Clarity stroke weight.
  Paint is clipped away from floating panels, annotation chrome and the
  floating Assistant. `ProjectedWordRects()` exposes drawn geometry.
- `eventFilter` tracks application activation and owner window blocking;
  `UpdateVisibility` also checks owner enablement and the active modal widget.
  The native overlay stays hidden during file pickers, nested modal dialogs,
  and application deactivation, including late `SetWords` and geometry events.
  Accepted words are retained; unblocking/reactivation restores the current
  viewport placement without another OCR job. Hidden paints do not qualify as
  current positioned accessibility geometry.
- `CaptureSnapshot(transform, viewportSize)` records an independent vector
  `UctOverlaySnapshot` from eligible cached glyphs and plates at the capture's
  exact view transform. `ComputePaintPlan` shares clipping/protected-token and
  replacement eligibility with screen paint, while excluding UI chrome only on
  screen. `PaintWords` shares both drawing passes. Captures do not replace the
  last-painted accessibility plan or query widgets on the writer thread.
- `UctOverlaySnapshot::Paint` replays frozen drawing commands at the saved
  image's pixel size; later word/style/pan changes cannot alter them.
  `ViewOverlaySnapshot::Paint` composites that text before value-owned
  annotation strokes, with no editing handles or UI controls.
- `OkuFlowApp::CaptureViewOverlays` freezes the current eligible UCT commands
  and active ink for one `ViewTransform`. Pending photo and annotation receipts
  own these values until their exact readback completes. Original photo pixels
  never pass through this composition.
  `SaveCapturedPhotoPair` consumes `pendingPhotoOverlays_` before queueing the
  image writer; timeout/cancellation clears it. `QueueAnnotationSnapshot` freezes
  text alongside its strokes, and `SaveAnnotationSnapshot` receives that exact
  `UctOverlaySnapshot` instead of reading the live overlay later.
  CPU failed/deferred presentation clears its stale overlay receipt; capture
  also requires matching acquisition sequence and viewport extent before
  compositing, so newer CPU pixels never use an older view's ink transform.
  `uct_capture_test::FrozenExportMatchesGlyphsAndSurvivesLiveChanges` compares
  screen/export pixels and worker replay; `ExportProtectsOmissionsAndKeepsInkAboveText`
  covers protected tokens, chrome and layer order. Native fixture helpers
  `PhotoDirectory`, `SavePhotoFixture` and `SaveDrawingFixture` call the real
  writers for `processedPhotoAndDrawingIncludeUctAndInkWithCleanOriginal`.

### `include/okuflow/ui/ultra_clear_text_panel.hpp`
`okuflow::UltraClearTextPanel`
- Accessible opt-in checkbox, Show original checkbox (enabled while on), a
  live status label (polite for important states, silent otherwise) and a
  read-only selectable `Recognized text` pane that preserves
  scroll position. Hosted at the end of MainWindow's Text clarity section.
  `SetStatus` updates visible text and the accessible name immediately through
  `SetLiveText`, without diagnostic coalescing. Repeated source text is a no-op.
  `panelStatusUpdatesCachedAccessibleTextImmediately` retains the real Qt
  accessible interface through routine state changes without draining events,
  and checks that the reading pane's text interface returns refreshed content.
  This offscreen contract does not establish native Windows UIA client behavior.

### `include/okuflow/ui/live_status_text.hpp`
`okuflow::LivePoliteness`, `okuflow::SetLiveText(...)`, and
`okuflow::SetLiveTextCoalesced(...)`
- Centralize dynamic `QLabel` and `QAbstractButton` updates on the Qt UI
  thread. The helper changes the visible text, composes a role-qualified
  accessible name, emits accessibility `TextUpdated` and `NameChanged`
  invalidations for labels, and can issue a polite or assertive announcement.
- `LivePoliteness::kSilent` updates the accessibility tree without
  unsolicited speech. `kPolite` covers normal state changes and `kAssertive`
  is reserved for failures such as camera loss.
- Announcements with the same complete accessible text are suppressed for
  five seconds per widget. The coalesced form uses a trailing single-shot
  timer so rapid diagnostics publish only their latest value.
- Public API:
  - `enum class LivePoliteness { kSilent, kPolite, kAssertive }`
  - `void SetLiveText(QWidget* widget, const QString& text,
    LivePoliteness politeness, const QString& rolePrefix = {})`
  - `void SetLiveTextCoalesced(QWidget* widget, const QString& text,
    LivePoliteness politeness, const QString& rolePrefix = {},
    int delayMs = 400)`

### `include/okuflow/ui/render_widget.hpp`
`okuflow::RenderWidget`
- Native widget that hosts the D3D12 presenter. Show and resize events create
  or coalesce native-client-pixel presenter resizes so splitter/window motion
  cannot make camera-frame dimensions take over the swap chain.
- `resizeEvent()` schedules a 16 ms resize only when no update is already
  pending; `ApplyPendingPresenterResize()` reads the latest native client size.
  Continuous splitter movement therefore cannot postpone the resize indefinitely.
  `D3D12Presenter::CreateSwapChain()` uses `DXGI_SCALING_NONE`, keeping old pixels
  unscaled between a client resize and the newly rendered aspect-correct frame.
- Public API:
  - `explicit RenderWidget(QWidget* parent = nullptr)`
  - `QPaintEngine* paintEngine() const override`
  - `void setPresenter(D3D12Presenter* presenter)`
  - `bool isPresenterReady() const`

### `include/okuflow/ui/joystick_overlay.hpp`
`okuflow::JoystickOverlay`
- Circular on-canvas joystick overlay.
- Anchors to the render viewport's bottom-right corner, but detects the Simple
  action cluster's native window rectangle and moves one margin above it.
  Placement refreshes when either the viewport or action cluster changes
  geometry or visibility, so the joystick remains reachable in both UI modes.
- Public API:
  - `explicit JoystickOverlay(QWidget* parent = nullptr)`
  - `void ResetKnob()`
- Signal:
  - `JoystickChanged(float normX, float normY)`

### `include/okuflow/ui/annotation_overlay.hpp`
`okuflow::AnnotationOverlay`
- Transparent annotation surface implemented as a frameless tool window owned
  by `MainWindow`. A one-alpha backing surface keeps Windows mouse hit testing
  active above the native D3D HWND without visibly obscuring the camera. It
  follows
  the render viewport in native screen coordinates so ink and controls remain
  above the native D3D swap-chain HWND.
- Public API controls active state, canonical `ViewTransform`, global
  color/width/dashed/shape/text-size/save-on-exit preferences, ink access,
  undo/redo, pending-text commit through `CommitPendingText()`, and ordered
  accessible focus targets.
- `SetExcludedWidget(QWidget*)` tracks a guarded Assistant widget and excludes
  its current visible bounds from the native window region and paint clip.
  `UpdateInputRegion()` refreshes on movement, resize, visibility, and viewport
  ancestor changes; `IsExcluded()` also rejects drawing over that panel during
  captured pointer gestures. Masks include toolbar children and handle a
  completely covered viewport without accidentally clearing the mask.
  `PositionToolbar()` moves tool/action rails beside, above, or below the
  floating Assistant when there is enough space, restoring their edge positions
  when it moves away or hides. Any unavoidable overlap remains masked.
  The shared `RenderAnnotationStrokes()` intersects its scene destination clip
  with the caller's existing exclusion region, preserving the chat hole while
  painting ink and selection visuals.
- The flush-left vertical tool rail covers Move, Pen, Line, Shape, Text, and
  whole-item Erase. Pen/Line/Shape/Text open a transient flyout containing only
  relevant color, size, style, shape, or text controls. A separate flush-right
  rail owns Undo, Redo, annotated Save, capture-and-Clear, and Done.
- Keyboard commands remain active from tool controls without stealing ordinary
  editing shortcuts from the text field. Wheel and unconsumed navigation keys
  are remapped and synchronously forwarded to the render target/main window,
  crossing the top-level tool-window boundary without duplicating pan/zoom
  logic. Middle-button press/move/release events use the same explicit
  remapping. Move drags on empty camera content create a visible marquee;
  intersecting vector items become one group that can be dragged, nudged, or
  deleted together. Shift/Ctrl extends the current selection and screen-reader
  announcements report the resulting item count.
  Persistent corner controls are excluded from the annotation window's native
  hit-test surface using Win32 window rectangles, keeping physical
  `WM_NCHITTEST` coordinates correct at per-monitor DPI scales above 100%.
  The persistent Hide UI control is also excluded so Draw cannot intercept it.
  Explicit event forwarding keeps drag-pan available over the canvas. Text
  creates an inline editor at the clicked scene coordinate and commits on
  Enter; Escape cancels it. Slider wheel input is ignored.
- Signals separate snapshot, clear, exit, clean-photo, preferences, and
  accessible tool-change intent from application capture policy.

### `include/okuflow/ui/main_window.hpp`
`okuflow::MainWindow`
- Builds the UI shell and exposes widget accessors used by `OkuFlowApp`.
- Installs both Qt and Win32-native event filters for reliable activity
  detection across the native swap-chain surface and its owned control windows.
  Separate logical/physical pointer baselines ignore stationary hide-induced
  messages; native activity is restricted to existing magnifier surface HWNDs.
  `FadeSimpleChrome()` snapshots the pointer after pinning checks, and
  `SetChromeOpacity()` retains in-process activation before hiding active chrome.
- Two-speed UI around one persistent render widget: Simple uses three native
  corner-control windows plus contextual keystone controls. Advanced keeps
  the carousel and actions available beside a tabbed inspector (520 logical
  pixels by default, 360 minimum, saved preference capped at 1200).
  `Image` holds quick-mode tuning with nested Fine-tune text; `Settings`
  holds shared Camera, View and navigation, Recording, Notes and files,
  Language, AI and downloads, and Troubleshooting groups. `Assistant` keeps
  Chat/History. Existing widget getters
  remain valid after reparenting. Help and keyboard tab cycling stay available.
  - Constructor helper `makeDependentGroup` creates indented, left-ruled
    visual containers for Zoom position, Extra Stable, spatial sharpening,
    Super Resolution modes and blur parameters.
    It preserves application-owned child enabled states and saved preferences.
  - `setCameraPlaceholder(...)` / `UpdateCameraPlaceholderVisibility()` show
    source-tracked startup/reconnect/stopped-capture text only while the app is
    active. `OkuFlowApp::UpdateCameraPlaceholder()` derives it from capture
    state and `cameraFramePresented_`, reset at camera start/stop.
  - `OkuFlowApp::UpdateControlEnabledStates()` disables Focus X/Y along with
    the Zoom slider when Zoom is off; companion labels mirror that state.
  - `RaiseChromeAboveCanvas()` restores visible corner controls, the persistent
    Hide/Show button, and grid above the Draw canvas after Draw activation or
    UI restoration; `RaiseDialogsAboveChrome()` then keeps visible owned
    dialogs above those tool windows.
  - Constructor styles mute disabled slider fills and handles; the search
    fields' internal icon buttons retain Qt geometry without toolbar padding.
    Tab panes and the Image/Settings/Assistant pages explicitly paint
    `palette(window)`, pairing their WindowText labels with the actual surface
    even when a native tab style paints a light pane under a dark app palette.
    UI regressions compare actual enabled/disabled rendering and assert search
    icon bounds and vertical alignment.
  - `nativeEvent()` follows the existing owner's exact HWND on `WM_MOVE`, skips
    minimized native moves, and leaves the message available to Qt. It calls
    `MoveSimpleChrome()` before Qt's deferred geometry notification; that helper
    translates the eight chrome peers and the UCT glyph window by the change
    in render origin, preserving
    their actual positions after native pixel rounding rather than restoring
    pre-rounding offsets.
    The explicit-origin overload uses `ClientToScreen`, the matching native/Qt
    monitor origins and the screen DPR, avoiding stale Qt globals at 1Ã— scaling.
    It only repositions existing layouts/retained glyph pixels; later Qt events
    reconcile size/DPR. UCT therefore follows each native move without waiting
    for queued Qt movement, a repaint timer, OCR or semantic UIA updates.
    `NativeControlFixture::MoveViewportPeers` exercises that same private path;
    `uctOverlayFollowsImmediateViewportMovesWithoutQueuedEvents` verifies 120
    successive synthetic origin receipts with Qt's event queue withheld, stable
    accepted words/transform and no repaint. The native
    `chromeFollowsNativeOwnerMove` case checks the actual UCT HWND alongside
    all chrome peers, in LTR/RTL and the existing CI scale matrix.
    At non-unit DPR it uses Qt's live native render mapping, preserving child
    origin rounding. Unit-scale moves use the render-to-owner offset captured
    from completed global geometry, rather than recomputing logical parent offsets.
    Qt owner/ancestor/render Move events also reconcile placement.
    For pure moves, `WM_WINDOWPOSCHANGING` snapshots the actual render origin
    before Windows changes the owner. The first scaled RTL displacement therefore
    excludes any child-origin rounding that occurred after full fitting.
    `UpdateSimpleChromeGeometry()` records the render origin, viewport size and DPR
    after fitting; a size/DPR mismatch returns to that full fit. Pure moves
    neither enqueue a fit nor alter layout, focus, visibility or opacity.
    Independent visibility-control placement remains intact during translation.
    `MainWindowTests::chromeFollowsNativeOwnerMove` covers Simple/Advanced and
    LTR/RTL, forward/reverse native moves, immediate Qt and visible HWND
    displacement, no deferred layout requests, and real resize refitting.
  - `UpdateSimpleChromeGeometry()` retains independent bottom clusters.
    `chromeDock` is `corner` when both fit beside each other and `stacked`
    when the compact left group must sit above the actions. `modeRows` is 1
    or 2 on the left panel. It folds the mode name above Previous/grid/Next
    before compacting actions. `actionRows` is 1 or 2 on the right panel:
    further narrowing wraps three actions above two, with equal widths within
    each row and matching outer edges. Extremely narrow views stack the compact
    left cluster over the trailing action group, never full-width bands. Each
    pass releases pinned sizes and remeasures translated content. Only
    Simple's visible tracking controls reserve space; Advanced's hidden peer does not. A
    deferred language-change pass remeasures after the whole tree is
    translated. Geometry regressions cover English/Turkish/German, live
    language switching without resizing, hidden tracking, and a short
    narrow window at its effective layout minimum.
  - Internal `ChromeActionButton` retains the normal `QPushButton` API,
    checked/disabled/focus state, full text and accessibility metadata.
    `SetFace(Beside/Under/Icon)`, `sizeHint`, `minimumSizeHint` and `paintEvent`
    adapt only visual placement: beside-icon labels normally, below-icon labels
    at narrower widths, icon-only only when even two labeled rows exceed the viewport.
    Compact buttons remain at least 48 px wide and 58 px tall. No new timer,
    reduced font size or lost accessible label is added.
  - Internal `ModeCarouselButton` elides at paint time and draws a separate
    shortcut badge. `EnabledMirror` dims a slider's companion readout;
    `FormatPercent`, `FormatScaled`, and `FormatSigned` use the active locale.
    `refreshSliderReadouts()` runs refreshers after language changes and
    blocked-signal configuration restores. `FilterSettingsTab(SettingsScope,
    const QString&)` searches Image or shared Settings, excluding numeric
    readouts. `ActivateSearchResult(SettingsScope)` focuses the match or
    transfers the query to the other tab when only it has a result. Language
    changes reapply active queries after widget labels have been translated.
    `InspectorFocusOrder(QWidget*)` follows the visible layout order and
    `FocusRegion(bool)` bridges native control windows for F6 navigation.
  - `void setSimpleMode(bool simple)` / `bool isSimpleMode() const`
  - `void setExplainBusy(bool busy)` retains explicit cancellable-request
    state, independent of the localized or compact button label. App startup
    wiring synchronizes it across GPU capture, preparation, temporary AI
    requests, completion, and cancellation; persistent chat is separate.
  - `int advancedPanelWidth() const` / `void setAdvancedPanelWidth(int width)`
    expose the persisted splitter width while preserving minimum camera and
    inspector widths
  - `QAbstractButton* simpleModeButton() const` / `QAbstractButton* advancedModeButton() const` â€” checkable and mutually exclusive; state switching is wired internally, while the app connects only for persistence
- Public API includes getters for:
  - camera selection, orientation, capture resolution/frame rate and negotiated
    mode notice under Settings > Camera; processed-video resolution and
    microphone under Recording; acceleration, status and camera test under
    Troubleshooting
  - quick-mode preset list, preset description label, quick-option promotion,
    and Reset Mode; reset emits `resetCurrentProfileRequested()` so the app
    can restore profile-owned defaults without altering global controls
  - BW, zoom, blur, temporal smoothing, and spatial sharpening controls
  - visible `stabilizationCheckbox()`, `bumpHoldCheckbox()` (the internal
    getter for the user-facing `Extra Stable (hold on shake)` control)
  - screen-fix controls: `keystoneCheckbox()` ("Straighten Screen (Keystone)"), `autoContrastCheckbox()` ("Auto Contrast"), and `autoContrastStrengthSlider()` (0â€“100, default 70, enabled with its checkbox); `setKeystoneTrackingControls(...)` updates the shared Simple/Advanced Previous, Stop/Continue, and Next controls and their accessible state
  - Simple and Advanced Text Clarity master controls plus component checkboxes,
    sliders, polarity selector, and profile-owned NVIDIA Super Resolution
    enable/strength, Ultra full-frame cache, and Faster 2x choices; Ultra and
    Faster 2x are mutually exclusive and all SuperRes controls stay disabled
    when its build option is off
  - display color picker (`displayColorPicker()`) backed by structured
    `ColorScheme` values and accessible swatches, contrast slider (25-400 =
    contrast x100), brightness slider (-100..100)
  - Scene Explain checkbox plus assistive overlay toggle
  - on-demand analysis buttons (`explainNowButton()`, `readTextButton()`) and
    assistive buttons (`aiSettingsButton()`, `openNotesButton()`,
    `setupAssistantButton()`)
  - Assistant status, sign-in, transcript, prompt, camera attachment, send/stop/new, and history resume/rename/export/delete widgets
  - focus sliders, rotation combo, debug toggle, focus marker, and global
    viewport preferences under Settings > View and navigation
  - capture and recording buttons
  - annotation mode button/overlay plus preference and viewport-transform
    setters; activation raises the persistent Photo/Record/Explain/Read/Draw
    tool window above the transparent annotation surface, while the overlay's
    native hit test returns all persistent corner-control rectangles to their
    underlying Qt tool windows. Those actions therefore remain clickable after
    further canvas interaction, and the checked Draw action can exit
    annotation mode
  - processing status label and `performanceDiagnosticsLabel()` for rolling
    p50/p95/p99 camera-processing and capture-to-present timings
  - the static `SuperRes powered by NVIDIA Maxineâ„¢` attribution at the bottom
    of the Advanced inspector; `setMaxineRuntimeInstalled(...)` enables or
    disables the compiled control from actual runtime detection and keeps its
    tooltip and accessible description current after Setup changes;
    `isMaxineRuntimeInstalled()` exposes the cached state to dependent controls
  - `setSuperResStatus(...)` updates the dedicated status row with runtime or
    fallback state plus compact wrapping source-crop, viewport-target,
    final-zoom, and measured-latency details; a latency-only failure exposes a
    compact `Ignore 24 ms performance limit` checkbox
- `quickModeActivated(int row)` represents deliberate mode application.
  App wiring uses it instead of raw list highlight changes, so browsing does
  not repeatedly change the camera and applying the current row still works.
  `ActivateRelativePreset(int)` advances from the applied mode even while the
  grid highlights an uncommitted candidate.
- Signals `annotationSnapshotRequested(int)`,
  `annotationPreferencesChanged(...)`, `keystoneStepBackRequested()`,
  `keystonePauseResumeRequested()`,
  `keystoneStepForwardRequested()`, `resetCurrentProfileRequested()`, and
  `superResPerformanceOverrideChanged(bool)` bridge UI commands to
  `OkuFlowApp`.
- Event handling:
  - private `CheckBoxLabel` companions wrap long Image-option captions while
    preserving the checkbox's accessible name, focus target, and label clicks;
    the backend selector keeps a bounded minimum width for translated values
  - private `TrackingButtonRow` measures translated Back/Stop/Next buttons and
    reflows them into three, two, or one column at narrow inspector widths
  - arrow-key routing for panning
  - global Qt activity detection plus native render-window mouse
    detection that reveals chrome, restarts the five-second idle timer, and
    preserves keyboard-focused controls; mouse-click focus does not pin chrome.
    While chrome is already visible this is a
    deadline-only fast path and never recomputes or raises tool-window geometry
  - number keys `1`-`9` for the first nine quick modes (grid tiles show matching number badges)
  - `Esc` closes the quick-mode grid and `Ctrl+H` toggles explicit UI hiding.
    `setUiHidden(bool)` preserves mode, tab, inspector width, drawing, and
    assistant content; `isUiHidden()` reports the state. `uiVisibilityButton()`
    exposes the persistent top-right restore control. Hidden UI stays hidden
    during mouse activity and incoming answers; Ctrl+F/F6 restore it.
  - explicit `Tab` / `Shift+Tab` traversal across separate corner windows,
    `F6` / `Shift+F6` region navigation, and modal/popup keyboard isolation
  - previous/next wrapping profile activation and a temporary numbered tile grid
  - large centered mode toast plus assertive `QAccessibleAnnouncementEvent`
  - event filter on the render widget for Ctrl+wheel zoom, plain-wheel pan,
    middle-button drag pan, and corner-window repositioning on resize/move.
    Default assistant safe-area updates use a recursion guard because a
    resulting overlay resize can itself trigger native layout events

### `include/okuflow/ui/collapsible_section.hpp`
`okuflow::CollapsibleSection`
- Reusable two-level Advanced inspector disclosure group with a focusable
  `QToolButton` heading, visible/accessibility changed count, persisted key,
  and temporary search expansion that restores the prior state.
- Public API:
  - `QWidget* contentWidget() const`
  - `QToolButton* headerWidget() const` exposes the focusable search destination
  - `void setExpanded(bool)` / `bool isExpanded() const`
  - `bool persistedExpanded() const` returns the user's disclosure state,
    excluding temporary expansion caused by a search
  - `void setChangedCount(int)` updates the disclosure header
  - `void setPersistKey(const QString&)` / `const QString& persistKey() const`
  - `void setSearchExpanded(bool)`
- Signal: `expandedChanged(bool)`.

### `include/okuflow/ui/color_scheme_picker.hpp`
`okuflow::ColorSchemePicker`
- Offers labeled quick buttons for Posterize 6, Yellow on black, Normal colors,
  and Black on yellow above a large `More colors` trigger showing the current
  choice. `ArrangeQuickChoices()` and `resizeEvent()` use two columns below
  720 logical pixels and four above. Quick buttons show the active selection;
  programmatic changes refresh their checks without emitting `schemeChanged()`.
- The owned tool popover retains all built-in choices in six-column reading-
  color/effect grids and a Custom editor. Yellow/black pairs and Posterize 6
  lead their grids without duplicated choices. `QuickSchemeButton` paints a
  sample and wrapping label with keyboard focus and accessible scheme names.
  Its `paintEvent()` keeps the normal font when labels fit, reduces it once by
  at most one point with an 11-point floor, and draws at most two lines with
  overflow ellipsis; tooltips and accessible names retain the complete name.
- The formatted trigger translates both `More colors` and the active scheme
  name on selection and language changes. Its live formatted argument stores
  the translated built-in name so later generic Show/Polish retranslation
  preserves the subtitle; custom names remain raw. The focused picker tests
  cover actual German/Turkish/English `LanguageManager` roundtrips and optionally
  render narrow color shortcuts with the production base font without desktop focus.
- The frameless native popover uses an opaque backing store and solid dark
  palette rather than translucent composition over the D3D/inspector surface.
- Custom schemes support 2-8 stops, duotone/posterize/gradient modes, stepped
  output, per-stop `QColorDialog` wells, a live tile preview, and a persistent
  pencil-badged reusable tile.
- Selection is explicit: hover only updates normal button feedback and never
  changes the camera. `schemeChanged()` is emitted after a chosen built-in or
  custom scheme is applied.
- Every tile/well is a real focusable control with accessible names/tooltips.
  Arrow/Home/End navigate the grid; Esc closes and restores trigger focus;
  selection emits an assertive accessibility announcement.
- Public API: `currentScheme()`, `customScheme()`, `setCurrentScheme(...)`,
  `setCustomScheme(...)`, and `hasCustomScheme()`.

### `include/okuflow/ui/wheel_safe_combo_box.hpp`
`okuflow::WheelSafeComboBox` / `okuflow::WheelSafeSlider`
- Noneditable combos paint long entries with middle elision and show their
  full current text on hover when truncated. Model/current text stays intact
  for Qt accessibility; designed tooltips remain available for fitting text.
- Ignore wheel edits so events continue to the surrounding scroll area or
  camera navigation. Click, drag (sliders), and keyboard editing remain
  unchanged. All settings combos and sliders use these subclasses.

### `include/okuflow/ui/ai_settings_dialog.hpp`
`okuflow::AiSettingsDialog`
- Modal editor for AI provider selection and assistive configuration. Supports
  Codex subscription mode with executable/model overrides, explicit internet
  and workspace-scoped coding permissions, and an
  OpenAI-compatible server mode, including local servers without an API key.
  Its fixed action row surrounds a vertically scrollable content area with
  AI service, provider-specific connection, shared Assistant instructions and
  Explain prompt, Advanced Assistant permissions, Read Aloud, and notes groups.
  Only the selected provider's connection and permissions are shown.
  `UpdateFormWrapping()` stacks labels above fields at narrow widths; tab order
  follows the visible groups. The built-in Codex prompt is read-only and
  initially collapsed. User instructions and the Explain prompt remain editable
  for both providers. `result()` preserves initial fields not edited by the
  dialog, including the saved credential id needed to replace or delete a key.
  `SetCodexModelCatalog`
  fills the model selector and model-specific reasoning selector from the live
  app-server catalog while keeping an unavailable saved selection visible.
  It also enumerates all voices and locales exposed by Qt's selected Windows
  speech backend, persists voice/locale/rate selection, and previews speech
  only on request. It falls back from WinRT to SAPI only after an actual engine
  error, not while asynchronous WinRT initialization is still in progress.
- Public API:
  - `explicit AiSettingsDialog(const settings::AssistiveSettings& initial, QWidget* parent = nullptr)`
  - `SetCodexModelCatalog(const QJsonArray& models, const QString& selectedModel)`
  - `settings::AssistiveSettings result() const` â€” the edited settings after the dialog is accepted

### `include/okuflow/ui/assistive_overlay.hpp`
`okuflow::AssistiveOverlay`
- Solid Assistant `QDockWidget`, floating on top of the render
  surface. Its header and edges use the native window-system move/resize path;
  streamed result updates never reapply window geometry. A
  read-only 20-point `QTextBrowser` with 135% line spacing exposes incrementally streamed text to screen
  readers and keyboard selection; the question field stays editable while a
  response streams, while `SetBusy(true)` blocks Ask and Enter submission
  without discarding the draft. When ready, it sends the current view to the
  shared persistent Assistant conversation. Read Aloud remains manual
  and strips the visible `Read Text` / `Scene Explain` section labels from its speech
  payload. Explain uses `AssistantChatView` with New Conversation in its header
  and a question above the Read Aloud / attachment / Send footer. The separate
  Read result pane retains its own controls. Disabled submission is visually dimmed.
  The question field handles unmodified Escape directly: its first
  press clears a nonempty draft, and an empty-field press returns focus to the
  camera without dismissing the answer. An active header drag still cancels
  before either action; open placement menus retain their own Escape handling.
  The Close control
  uses a high-contrast white icon and border.
- The compact Panel position `QToolButton` menu docks left/right through the owning `QMainWindow`,
  which resizes the central camera layout, or returns to floating mode. Floating
  geometry is preserved separately from dock placement. Header/edge movement
  stays native while floating; dock separators still use Qt layout sizing.
  Placement actions retain their English source labels and use `TranslateUi`
  at construction, on `LanguageChange`, and before each menu opening; the
  persisted `floating`/`left`/`right` tokens remain unchanged.
  Default sizing includes the content layout's minimum before RTL anchoring,
  so font-dependent minimum sizes remain inside a safe area that can fit them.
- `BeginDrag()` tracks the native Windows move session. `moveEvent()` and
  `UpdateDockPreview()` calculate left/right targets in Qt logical coordinates
  with a 36-pixel edge zone, retaining the highlight while the grabbed title
  point crosses the edge. The owned translucent label is input-transparent and
  non-activating, with localized release instructions. `nativeEvent()` handles
  move completion/cancellation; `FinishDrag()` defers docking until the native
  loop returns and invalidates stale drops on another action or hide. Header
  clicks, edge resizing, ordinary geometry updates, moving away, and Escape do
  not accidentally dock. The existing QWidget drag fallback shares this preview
  and completion path.
- Qt's `DockWidgetMovable` behavior is disabled to keep one owner of header
  gestures. `BeginDockedDrag()`/`ContinueDockedDrag()` arm a 350 ms timer after
  a 32-pixel pull. `ReleaseDockLatch()` floats the panel under the grabbed title
  point and starts the existing native move path. `CancelDockedDrag()` cancels
  on release, Escape, hide, lost capture, or another placement action. The same
  edge cannot re-arm until the pointer/window leaves its wider release zone.
  `UpdateDockPreview()` acquires within 36 pixels after 180 ms of stable entry,
  retains within 96 pixels, and debounces release/switching for 250 ms. Its
  single-shot timer rechecks current geometry even when motion stops; movement
  within the same target does not restart the pending transition. Finishing or
  canceling a drag clears pending timers so they cannot re-arm a later gesture.
  `UpdatePlacement()` never fights the geometry of an active floating drag.
- Internal `DockPreviewLabel::paintEvent()` paints the translucent purple target,
  six-pixel high-contrast border, and centered instruction directly; it does not
  depend on stylesheet background propagation in a native translucent window.
- Public API:
  - `explicit AssistiveOverlay(QWidget* parent = nullptr)`
  - `void SetContent(const QString& title, const QString& body, bool visible)`
  - `void SetUiSuppressed(bool)` temporarily hides the panel without dismissal
    or content loss. Incoming content retains its requested visibility until
    restoration; docked panels release camera space and preserve dock width.
    Unsaved floating placement uses 43% viewport width and 75% height, clamped
    to available space below top controls; saved or resized geometry wins.
  - `void SetSafeArea(const QRect& relativeSafeArea)` provides camera-relative
    space clear of Simple chrome for untouched default placement. MainWindow
    updates it from current native panel geometry; user-restored or moved
    geometry is never repositioned by this hint.
  - `void SetBusy(bool busy)`
  - `void RestoreRelativeGeometry(const QRect& geometry)`
  - `QRect RelativeGeometry() const`
  - `void SetDockPosition(const QString& position)` â€” floating/left/right layout;
    `QString DockPosition() const` returns the stable setting token
  - `std::vector<QWidget*> FocusTargets() const` â€” the Simple-mode outer focus
    bridge starts at the transcript, then New, question, Read Aloud, attachment,
    Send, panel position and Close for Explain chat. The legacy separate result
    list retains its seven controls. The footer order matches native traversal.
- Signals:
  - `Dismissed()`
  - `ReadAloudRequested(const QString& text)`
  - `QuestionSubmitted(const QString& question)`
  - `NewChatRequested()`

### `include/okuflow/ui/responsive_slider_row.hpp`
`okuflow::ResponsiveSliderRow`
- Reusable settings row that moves its slider to a full-width second line when
  the inspector is too narrow. This keeps labels wrapped and every point of
  the slider track reachable during live splitter resizing.
- `minimumSizeHint()` advertises the stacked row's minimum width instead of
  forcing the inline layout to overflow a narrow scroll viewport. Layout
  selection accounts for the minimum width of the numeric readout.

### `include/okuflow/capture/capture_buffer.hpp`
- `CopyCaptureBuffer(IMFMediaBuffer*, subtype, width, height,
  negotiatedStride, output, outputStride)` reads native two-dimensional pitch
  before the negotiated media-type fallback. It validates row extents and
  available buffer bounds, normalizes RGB/YUY2/NV12 into tight top-down bytes,
  and leaves output unchanged on invalid/truncated layouts. Single-buffer MF
  samples retain their native 2D interface instead of being flattened first.

### `include/okuflow/app/cuda_surface_retry.hpp`
- `CudaSurfaceConfiguration` keys device/fence identity, camera session,
  scene dimensions, and upscale-cache dimensions.
- `CudaSurfaceRetry::ShouldAttempt(configuration, now)` allows immediate
  initialization after configuration changes. `RecordFailure(now)` applies
  1/2/4/8/16/30-second capped delays measured from failed attempt completion;
  `RecordSuccess()` resets failure history. `OkuFlowApp::EnsureCudaSurface`
  checks this policy before graphics draining or shared resource allocation,
  so raw and converted fallback calls share one failed-attempt cache.

### `include/okuflow/common/yuv_color.hpp` and `capture/capture_color.hpp`
- `YuvColorInfo` carries `YuvMatrix::{Bt601,Bt709}` and
  `YuvRange::{Limited,Full}` with BT.601 limited defaults. It travels through
  capture `FrameFormat` / `MediaFrame`, CPU frame preparation and original
  capture conversion, and CUDA `ProcessingInput`.
- `GetYuvCoefficients(color)` selects fixed-point coefficients once per
  frame; `ConvertYuvPixel` and `YuvChannelToByte` share CPU/CUDA rounding and
  clamp after the full color equation. NV12/YUY2 converter and kernel-launch
  APIs accept a final optional `YuvColorInfo` argument.
- `ReadCaptureYuvColor(IMFAttributes*, output)` resolves MF matrix and nominal
  range, defaults missing/unknown values, and rejects unsupported explicit
  values. `CaptureVideoColorSpace(color)` maps the same tags into D3D11 video
  processor input. Output is full-range BGRA; unsupported full-range input
  capability moves capture to raw conversion.

### `include/okuflow/common/spatial_cache_geometry.hpp`
- `SpatialCacheGeometry` / `ComputeSpatialCacheGeometry` enclose the canonical
  visible ROI in integer source pixels and select differing output dimensions,
  bounded by existing scene/cache buffers, 1440p, and 2x per pass.
  `CudaInteropSurface::UpdateSpatialCache` runs selected NIS/FSR enlargement
  after stateful full-scene processing when Maxine has not published output.
- `PadSpatialCacheBorder` in `spatial_upscalers.cu` copies a one-texel edge
  guard into unused texture space, preventing bilinear sampling of stale
  pixels at partial-cache borders without additional allocation.
- Presentation normalizes partial-cache UVs against physical texture extent.
  Annotations retain scene coordinates. Viewport-only motion reuses the cache
  or presents the base scene when outside its ROI; `IsSuperResActive` continues
  to mean the ML backend only.

### `tests/render_widget_tests.cpp`
- `RenderWidgetTests::continuousResizingDoesNotStarvePresenter()` verifies
  that native back-buffer dimensions update during continuous resize input.
- `RenderWidgetTests::circleKeepsItsAspectAcrossViewportResizes()` checks
  circular-target GPU readback across Fill/Fit viewport sizes. It retries
  nonblocking presentation for at most two seconds, remembers the first
  successful submission, and matches its readback request before measuring.

### `tests/text_clarity_cuda_tests.cu`
- `MaskRunner::Run()` launches the production Sauvola kernel with padded luma,
  independently accumulated clamped 9x9 moments, and device polarity analysis.
  Output row padding and the extra bottom row remain guarded.
- `UniformBackgroundIsNotInk()`, `StrokesAndAutoPolarity()`, and
  `ComplementaryInputsHaveTheSameInkMask()` verify blank/noisy dark boards,
  bright/dark strokes, explicit and automatic polarity selection, missing-analysis
  fallback, and complementary-image symmetry within one byte, including soft
  edges. `MakeLightStrokes()`, `Complement()`, and `RequireEqual()` construct
  image fixtures and report pixel mismatches without reproducing the threshold
  formula. The `text_clarity_sauvola_cuda` target runs in CUDA-enabled Windows
  tests and skips with code 77 when no CUDA device is available.

### `tests/ai_settings_dialog_tests.cpp`
- `AiSettingsDialogTests` exercises the production dialog through accessible
  controls and its save button: protected credential-id retention after key
  clearing, editable Codex Explain prompts, and provider switching with hidden
  coding preferences preserved. Workspace cases reject an existing relative
  folder or nonexistent absolute folder, then accept a temporary absolute folder
  in the same dialog. The fixture dismisses only its own validation warnings.
- Keyboard cases verify Tab/Shift+Tab moves between the shared text editors
  without inserting tabs; narrow-layout cases check scroll reachability and
  horizontal containment for both providers. `ShowDialog`, `Accessible`, and
  `ClickSave` provide focused widget lookup, desktop activation, and bounded
  warning handling. The `ai_settings_dialog_interactions` CTest target disables
  TTS and substitutes only static built-in Codex reference text, without loading
  protected credentials or starting app-server, network, or camera services.
- `SaveDialogScreenshot()` optionally renders fixture dialog service, shared
  prompt, and bottom sections into `OKUFLOW_UI_TEST_SCREENSHOTS`, without
  compositor captures of unrelated desktop windows or saved account data.

### `tests/main_window_tests.cpp`
- `SyncScreenshotState()` mirrors initial app-owned language selection and
  Zoom enabled states in the camera-free screenshot fixture.
- `disabledSlidersHaveDistinctAppearance()` compares actual slider fill
  rendering when disabled directly or through the Text Clarity master.
  `searchIconsStayInsideFields()` verifies search/clear icon containment and
  vertical alignment. Minimum-viewport cases also capture translated color
  shortcuts after scrolling them into view.
- `GlobalRect()` and `ActionRowsProblem()` inspect the buttons' actual screen
  geometry. `bottomChromeFormsRectangularGroups()` covers translated corner,
  compact Advanced and minimum layouts, returning to a wide layout after
  folding the left cluster, live language changes, and tracking controls hidden
  in Advanced. Every layout requires one action row and disjoint clusters.

## Android Companion

This separate Kotlin/Compose project sends encoded camera frames over USB.
Native Windows consumes AVC and optional HEVC through the phone source below. See
`android/README.md`, `android/PROTOCOL.md` and plan 51.
The `connect_usb_hint` resource explains that the companion requires OkuFlow
for Windows on the PC, directs users to okuflow.com, and asks them to open the
desktop app and use a data-capable USB cable. All 25 catalogs and the EN/TR/DE
illustrative draft carry the instruction; setup-state resolution is unchanged.

| Source under `android/app/src/main/java/com/okuflow/camera/` | Contract |
| --- | --- |
| `MainActivity.kt` | `MainActivity.onCreate` sets dark edge-to-edge system bars, applies `FLAG_KEEP_SCREEN_ON` to every visible app panel, retains the normal lock-screen boundary, and installs `OkuFlowCameraTheme` / `OkuFlowCameraApp`. Launcher and adb starts reuse the `singleTask` activity. |
| `settings/AppLanguage.kt` | `AppLanguage` has 25 canonical tags/native names. `fromTag` handles explicit codes; `forLocale` preserves regional/script and legacy tags. `AppLanguageStore.selected/select` use framework per-app locales on Android 13+ or private `language/tag` preferences on Android 11â€“12. `wrap` applies older-version Activity/Service context configuration; `systemLanguage` labels the phone-default choice. MainActivity and CameraStreamService wrap their base contexts. |
| `setup/SetupStep.kt` | `DeviceSetupSnapshot` carries nullable developer/debugging settings, permission/USB flags and current `usbAdbActive` evidence. `canListenForHost` keeps permission/USB gates while allowing unknown debugging status. `debuggingSettingEnabled` maps Android 17 redacted zeros to unknown and preserves older disabled values. `SetupStep` includes optional `CheckUsbSetup` help, `HostConnected` (session open, camera starting/paused) and `Problem`. `resolveSetupStep(snapshot, link, problem)` accepts active USB adb evidence and never labels unknown settings disabled; a live Connected/Streaming session outranks the coarse USB reading, and a problem outranks an open session but never streaming, a missing cable or a setup prerequisite. |
| `setup/DeviceSetupMonitor.kt` | `DeviceSetupMonitor.start/stop` register/unregister global-setting observers and USB/battery receivers; `start` reads both sticky values synchronously to avoid a false disconnected snapshot during service startup. `refresh` rereads settings/permission; `readDebuggingSetting` handles Android 17 redaction/read failure without a false disabled state. The protected USB broadcast adb flag is cleared on disconnect. `snapshot` is a read-only StateFlow. USB-state broadcasts are an OEM-dependent heuristic, not proof of an authorized OkuFlow session. |
| `setup/SetupViewModel.kt` | Combines the monitor with `HostLink.state` and `HostLink.problem` (also exposed as `problem`). `onResumed`/`onPaused` set the service's visible-start gate; resuming clears a blocked-camera problem. `refresh` retries expired transient failures, refreshes prerequisites, clears saved denial and calls `startServiceIfReady`; a permission result while paused cannot start the service. `onCameraPermissionResult` retains denial across restart. `settings`, `diagnostics`, `updateSettings` bridge preferences/service. `onCleared` unregisters the monitor. |
| `setup/SystemSettingsLauncher.kt` | `openSetupSettings` uses `primarySetupSettingsTarget`: disabled developer setup and unknown-state `CheckUsbSetup` open About phone; the known USB-debugging prerequisite opens Developer options. `openAboutPhone`, `openDeveloperOptions`, `openPrivacySettings` (camera access switch) and `openAppDetails` try public Settings intents, then broader missing/restricted OEM fallbacks. `launchFirst` copies intents with `FLAG_ACTIVITY_NEW_TASK` so localized/application contexts also work. A successful start does not prove a hidden OEM activity stayed open. |
| `setup/OemFamily.kt` | `OemFamily.fromManufacturer` chooses Samsung, Xiaomi/Redmi/POCO, OnePlus/OPPO/realme, or generic Build-number wording without changing the intent target. |
| `link/HostLink.kt` | `HostLinkState` has Disconnected/Connected/Streaming. `HostProblem` names user-fixable failures (CameraBusy, CameraBlocked, NoCompatibleMode, CameraFailed, TooHot, ServiceFailed). `state`, `problem` and `details` (negotiated mode while connected) are published by the camera service through internal `publish`/`report`. A problem survives the host's disconnect after its ERROR packet and is cleared by a new session, the first streamed frame, cooling below CRITICAL (TooHot) or unplugging. |
| `ui/HostProblemText.kt` | `problemTitle` / `problemHint` map each `HostProblem` to localized title and remedy strings. |
| `ui/OkuFlowCameraApp.kt` | `OkuFlowCameraApp` collects state with lifecycle, refreshes on resume, auto-requests camera once per saved activity state, and builds one `StepContent` (title/hint/optional action) per step. The same title, plus the negotiated mode while connected, becomes the Advanced settings status. The root applies safe-drawing insets once, above an outlined Advanced settings entry. Problem actions open Advanced settings (NoCompatibleMode) or privacy settings (CameraBlocked). `developerHintFor` resolves localized OEM wording; `StepChangeHaptics` confirms changes after initial composition, skips the transient HostConnected state and uses the rejection pattern for problems. |
| `ui/SetupPanels.kt` | `ActionPanel` provides one large clickable panel; `WaitingPanel` has no action; `StreamingPanel` renders stream status; `MainActivity` owns keep-awake behavior for all panels. `StepHeader` shows the decorative amber `WarningBadge` for problems. `PanelScaffold` applies merged semantics, pane titles and scroll/minimum-height layout. `PanelText` centers full-width headings and justifies instructions; `StepHeader` centers step text and progress. `StreamingPanel` centers its status row and justifies its hint. `ProgressDots` and `ActionPill` provide shared progress/action rendering. Compose previews cover all states, denied permission, narrow German at 200% and landscape. |
| `ui/theme/Theme.kt` | `OkuFlowCameraTheme` installs a black/white palette, scalable type and an explicit light `LocalContentColor` for text outside Material surfaces. Magenta/dim gray are decorative; hint text uses high-contrast gray. `OkuFlowWarningAmber` marks problems only. |
| `settings/CameraSettings.kt` | `DEFAULT_CODEC` is `hevc`, the preferred codec for new installations and Reset; capture still falls back to AVC. `storedCodec` gives a missing stored codec that default while keeping an explicit saved `avc`; unknown values sanitize to AVC. `CameraSettings.hasExplicitMode` identifies a saved size/rate pair; `sanitized` validates paired dimensions, locally reported rates up to 60, legacy resolution ceilings, enums and exposure. Shared `CameraSettingsStore.get`, `update`, `read` persist/publish installation-local preferences. |
| `ui/AdvancedSettingsPanel.kt` | Dark `Surface` supplies explicit content color. A persistent back/title area sits above bounded-width scrolling cards. `SettingsSection`, `SettingsDivider`, and `SettingsHint` group camera/video and image controls. The back control and section headings are centered; the back control uses the same bright 2-dp outline as other controls. A `Stream status` card first on both levels shows the caller's `status` (current state title, plus the negotiated mode while connected). The main level holds camera, size/rate and preferred codec; a separate More options screen, with its own scroll position and one-level Back handling, contains all modes, bitrate and reset. Shared `ModeSetting` displays the exact saved pair even when the filtered main list omits it; `ChoiceSetting` keeps labels/values together inside bright 2-dp outlines, sizes the menu to its control, enlarges item padding without enlarging text, and scrolls to the selected item using measured row heights. Camera/codec-keyed background discovery populates the single size/rate picker and reconciles legacy or unavailable selections without overwriting concurrent image edits; `ToggleSetting` pairs visible labels with one accessible row switch. Exposure includes its value, brightness guidance and slider semantics. `AdvancedSettingsPreview` covers standard and 200% German layouts. The app handles Back to restore the setup/streaming pane. |
| `camera/ModeSelection.kt` | `StreamMode` holds size/FPS/bitrate and a size/rate `key`; `everydayModes` filters only the main picker to short edge â‰¥720 and FPS â‰¥24 without modifying the capture catalog; `fits` applies legacy, exact-dimension and rate bounds. `chooseMode` selects maximum detail then FPS, rejecting an empty intersection. `CameraSettings.withAvailableModes` preserves Automatic, migrates legacy ceilings and reconciles unavailable pairs while retaining image controls. |
| `camera/CameraCatalog.kt` | `CameraChoice`, `EncoderChoice`, `CameraConfiguration` describe device/encoder/output/range. `cameras` orders rear first; `encoders` requires hardware/surface support; `recordingConfigurations` intersects Camera2 sizes/minimum durations/AE upper rates with hardware encoder limits; `modes` exposes unique locally supported pairs for the picker; `configurations` applies saved dimensions, host decoder and thermal bounds to order runtime candidates; `capabilities` reports camera controls and codecs. |
| `camera/AnnexB.kt` | `annexB` preserves start-code NALs and validates/converts four-byte length-prefixed compressed output. |
| `camera/CameraFailures.kt` | `problemForDeviceError`, `problemForAccessReason` and `problemFor` classify Camera2 device errors, `CameraAccessException` reasons and `SecurityException` as `HostProblem` busy/blocked/failed. |
| `camera/CameraEngine.kt` | `start` uses locally discovered fallback candidates and one `CameraStartupPlan`, reporting an empty intersection as `NoCompatibleMode`; `onError` receives a `HostProblem` and the raw candidate diagnostic for the host. `begin` creates a generation within its assigned absolute deadline; `candidateState` publishes bounded attempt progress and `startupCurrent` rejects late results. `createEncoder` publishes CSD/frames; `openCamera` creates the recording session and SENSOR timestamps on API 33+. `repeatRequest` applies zoom/focus/exposure/light/OIS/EIS and capture-result metadata. `setZoom`, `requestKeyFrame`, `stop`, `fail`, `stopInternal`, `close` serialize hardware ownership; atomic tokens invalidate old callbacks before queued teardown. |
| `camera/CameraStreamService.kt` | Non-exported camera foreground service. `onCreate` owns notification/monitor/server/engine/preferences and thermal registration, and publishes the actual stream codec in status; `command` handles the control allowlist with separate camera selection/request IDs; `restart` avoids unchanged settings and respects host/thermal state. `publishIdle` clears stale mode details and reports Connected while the host session remains, otherwise Disconnected. An existing foreground listener survives unplug/replug; a two-second USB debounce invokes `disconnectHost` without recreating a camera service in the background. `listenerMustStop` still revokes the service on permission loss or debugging explicitly off. Engine/thermal/startup failures report a `HostProblem`; raw diagnostics reach the host in ERROR packets. `onDestroy` removes only an owned thermal registration and uses independent `cleanup` calls for partial startup; destroyed services cannot republish notifications. `onActivityResumed`/`onActivityPaused` control `startVisible`, and foreground refusal waits for a fresh resume. |
| `camera/ServiceLifecycle.kt` | `ListenerRegistration.register/close` acquire ownership only after a successful add and remove it at most once, so a failed USB or thermal startup cannot remove an unregistered listener. `VisibleServiceStartGate.onResumed/onPaused/onRefused` requires a resumed activity for new foreground starts and suppresses refused starts until another resume. Both contracts have JVM regressions in `ServiceLifecycleTest`. |
| `link/WireProtocol.kt` | `Hello`/`Packet`, `readHello`, `readPacket`, `writePacket`, `framePayload`, `readExact` implement bounded little-endian v1 framing: control 64 KiB, video 16 MiB. |
| `link/UsbSocketServer.kt` | Authenticates shell/root peers and binds one control/video pair with a random token; `onConnect` runs after the video channel is bound and before its ACK permits the first start request. `Channel.send/close` own bounded writer queues; `readChannel`, `control`, `state`, `metadata`, `frame`, `disconnect`, `close` handle generation gating, ping, EOF/timeouts and pressure without blocking the camera on network writes. `disconnectHost` retires both channels and capture while retaining the abstract listener for cable recovery. The watchdog reads its volatile write-start timestamp once per check and logs transport/queue failure categories without session tokens. `HostLimits` holds decoder ceilings. |
| `link/OutboundQueue.kt` | `OutboundQueue.offer/take/complete/close` implement one bounded FIFO writer with packet admission and queued-plus-in-flight byte ownership. `controlQueue()` reserves 32 of 128 slots and 128 KiB of 512 KiB for ACK/STATE/ERROR; advisory metadata drops above 96 slots/384 KiB. `videoQueue()` retains the 32-packet/32 MiB reliable limit. Close wakes waiting writers; reliable/video overflow and the socket watchdog retain session-failure policy. |

All 25 `app/src/main/res/values*/strings.xml` catalogs have exact source-key
parity (98 keys). AGP generates the per-app locale configuration;
`resources.properties` names the unqualified source locale `en-US`.
The outlined Advanced entry uses `ic_settings_gear.xml` with one localized
accessible name. `AdvancedSettingsPanel` includes a native-name language picker,
selected-state choice semantics and exposure-slider value semantics. Navigation
arrows follow `LocalLayoutDirection`; centered/justified setup text is preserved.
`UsbSocketServer` serializes Connected/Disconnected callbacks with session
binding/retirement, so old-host cleanup invalidates capture before a new host
can claim the slot. Cleanup callback failures cannot escape the socket worker.
`CameraStreamService` ignores queued thermal callbacks after destruction.
The manifest declares CAMERA and camera foreground-service permissions without INTERNET, disables backup, and uses
`res/xml/data_extraction_rules.xml` to exclude device-specific setup history.

`SetupSettingsTarget` / `primarySetupSettingsTarget` are pure onboarding
navigation policy, tested against resolved older disabled and Android 17
redacted snapshots. Connection help keeps unknown distinct from off and offers
separate Developer-mode (About phone) and USB-debugging (Developer options)
actions. `StepContent` carries optional secondary action label/callback;
`ActionPanel` exposes two independent footer controls instead of a nested
whole-panel button when both are present. `ActionPill.modifier` supplies
the primary button semantics; other single-action panels remain unchanged.
`UsbSetupHelpPanelPreview` covers regular, German 200% and Arabic layouts.
`SetupStepTest`, `OemFamilyTest`, `WireProtocolTest`, `StreamPolicyTest`,
`CameraSettingsTest`, `CameraFailuresTest`, `LinkPolicyTest`, `StreamingIntentTest`, `ServiceLifecycleTest` and `AppLanguageTest` provide 67 JVM checks, including locale/script matching, partial-start listener ownership, visible/refused foreground-start policy, unplug prerequisite retention, older-setting behavior,
Android 17 redaction, USB evidence, connected/problem step priority, camera
failure classification, the HEVC default with explicit-AVC preservation, exact dimension/rate bounds, legacy preference
migration and camera-mode reconciliation. `CameraEngine.start` uses actual
camera/encoder modes instead of a hardcoded resolution fallback ladder. Real-device results and hardware limits
are recorded in `android/DEVICE_VALIDATION.md`.

`scripts/build_android.ps1` is the tracked Android validation entry point:
`Assert-AndroidTranslations` rejects empty/duplicate/missing/extra locale keys;
`Read-AndroidStrings` uses XML `LocalName` to avoid PowerShell's attribute-name
adapter collision and checks format tokens across every locale. The Qt validator
likewise requires all 24 non-English catalogs and exact source/format parity.
it then runs `assembleDebug` / `testDebugUnitTest`, plus optional `lintDebug`.
`Read-AdbAscii` reads bounded responses and `Assert-AdbServerCompatible`
inspects the running server without killing it. `-Install` installs/launches
only with one authorized device. Environment values are restored on exit;
outputs/caches remain outside Drive. The Gradle files pin dependencies,
JVM 17 output, API levels and local output paths, with wrapper checksum
verification. `android/design/setup-panels.html` illustrates the panels with
translated app strings; it is an HTML draft, not a native-device screenshot.

### Closed-test state, notices and signed artifacts

- `settings/StreamingIntent.kt`: `StreamingPausePersistence` abstracts the saved
  pause flag; `StreamingIntent.canStart`, `pause` and `resume` preserve explicit
  intent across reopening/retries. Failed storage writes remain paused.
  `StreamingIntentStore.get` provides the installation-local singleton;
  `pause`/`resume` synchronously commit only user actions and publish `paused`
  and `saveFailed` StateFlows, independently of camera-settings reset.
- `camera/StreamingPauseReceiver.kt`: the non-exported receiver's `onReceive`
  calls `pauseStreaming`, saving Pause before stopping the foreground service.
  `SetupViewModel` and `CameraStreamService` recheck that intent at startup,
  host commands, retries and restarts. `SetupStep.Paused` and
  `ui/StreamingIntentControl.kt` expose deliberate Resume. Notification and
  Advanced settings use the same intent, without adding a frame-path write.
- `ui/PrivacyPanel.kt` and `ui/LicensesPanel.kt` present translated privacy
  information and retained original notices offline. External public/contact
  links delegate to the owner's browser. Notification permission is optional;
  the monochrome notification icon and text reflect actual service state.
- The immutable application id is `com.okuflow`; explicit manifest component
  names retain Kotlin namespace `com.okuflow.camera`. The verified VIEW intent
  handles only HTTPS `okuflow.com/camera/open`. Google's public app-signing
  certificate in the website association differs from the private upload key.
- `app/build.gradle.kts` enforces all-or-none upload-signing inputs and explicit
  unsigned review mode. Release R8 runs with `app/diagnostic-r8.pro` disabling
  obfuscation/shrinking/optimization and preserving attributes, to generate an
  actual embedded diagnostic mapping; native FULL extraction is requested.
  Resource shrinking is explicitly off, with only the corresponding lint
  optimization advisory exempted for the diagnostic-only release policy.
  Release dependency inventory checks actual artifact
  hashes against reviewed runtime/native evidence before packaging. The
  `android/tools/` review scripts retain original POM/licenses, native source
  blob/build evidence and ELF dependencies under `android/licenses/`.
- `scripts/build_android_signed.ps1` initializes an owner-controlled local key
  with masked password input or loads a Windows DPAPI credential profile.
  Persistent signing variables are path/alias pointers; four temporary
  `UPLOAD_*` process inputs and Gradle daemon settings are restored on exit.
  Keytool receives passwords through environment references, never argv.
- `scripts/build_android.ps1 -Release` runs release assembly, JVM tests, lint,
  AAB and inventory before `scripts/validate_android_release.ps1`.
  `Assert-ApplicationIdentity`, `Assert-CameraAppLink`, `Assert-Elf16K`,
  `Assert-ArchiveLibraries` and `Assert-PackagedNotices` check manifest identity,
  the narrow verified URL, alignment and actual APK/AAB retained assets.
  `Get-ApkUploadCertificateDigest` handles old/current SDK signer prefixes,
  repeated scheme reports of one certificate and rejects different identities.
  `Assert-R8DiagnosticMapping` checks compiler/source/line metadata, unchanged
  runtime class names (including AGP's explicit synthetic Compose trace alias)
  and readable app lifecycle methods. `Assert-BundleDiagnosticMapping` binds the
  final generated map to the exact AAB entry by length and SHA-256; effective
  R8 flags/configuration hash and native metadata presence enter the receipt.
  `-SelfTest` supplies 34 fixtures (4 ELF, 4 notices, 4 identities, 4 links,
  6 signer outputs, 9 mapping-policy, 3 mapping-binding). Signed artifacts require matching APK/AAB upload identities;
  unsigned review artifacts cannot become Play upload candidates.
- `android/tools/export_play_art.ps1`: `Draw-OkuFlowMark` rasterizes the owned
  Android launcher vector geometry. The exporter writes the required store icon
  and feature graphic outside the checkout, disposes graphics resources and
  rejects checkout output paths. It does not fabricate screenshots or performance
  evidence; real app captures require the active physical display on foldables.

### Optional Windows phone source and host probe

- `CameraDescriptor.phoneSerial/phoneCameraId/phoneTransportId` distinguish the
  companion without changing MF activation. `MediaCapture.EnumerateCameras`
  appends phone entries; `ResolveSavedCameraIndex` retains saved webcam/phone
  identity and falls back from a busy/unavailable lens to that phone's default
  entry. `EnumerateFormats` leaves phone negotiation automatic. `SetPhoneZoom`
  and `SetPhoneCamera` enqueue controls and are no-ops for webcams.
- `MediaCaptureSession.StartCapture` isolates phone deliveries with
  `CaptureShutdown` and shares its DXGI manager. `Cancel`, `Quiesce`,
  `ReleaseResources` cancel/join/reset the transport through the existing
  bounded stop coordinator; a hung decoder retains its session. Frame-rate
  atomics follow decoded generation metadata.
- `phone_camera.hpp` / `phone_camera.cpp`: `ParseAdbUsbDevices` reads bounded
  public protobuf fields without a runtime dependency. `PhoneCameraTransport`
  `Enumerate` uses USB connection type, with explicit-USB legacy fallback, and
  optionally lists camera IDs. `Start` waits for first decoded output; `Run`
  revalidates USB transport ID, launches the installed activity, binds channels,
  negotiates AVC plus probed HEVC, polls controls/video, checks generation/sequence/PTS
  and maintains keepalive. Its session lambda can reconnect once offering only AVC
  after `HevcDecoderError` (creation/processing failure or four seconds without a
  first decoded frame), retaining monotonic output timing/sequence and saved preferences. `SetZoom`/`SetCamera` coalesce writes; `Cancel`/`Stop`
  revoke/join; `Format`/`Error` return synchronized snapshots. Helpers `Read`,
  `Write`, `Connect`, `AdbRequest`, `Transport`, `ReadPacket`, `Json`, `Hello`,
  `Command`, `ControlHello` bound off-UI network/parser work.
  Missing-listener discovery launches `com.okuflow/com.okuflow.camera.MainActivity`
  once per USB attachment; only Android's missing-activity Error type 3 permits
  the older `com.okuflow.camera/.MainActivity` fallback. A denied canonical launch
  never bypasses Pause through the legacy app. Launch output is bounded and
  drained after disconnect before classification.
- `PhoneVideoCodec` identifies AVC/HEVC. `PhoneVideoDecoder` owns the system
  transform through `Impl`; `IsSupported` probes activation and 1080p/60 input/NV12
  configuration with the optional DXGI manager; `Decode` submits bounded Annex B.
  `CreatePhoneDecoder` uses the built-in H.264 CLSID or enumerates/activates a
  synchronous HEVC MFT, releasing activation arrays and detaching the owned transform. Internal `Decoder.SelectOutput`, `Input`, `Drain`
  negotiate NV12, handle stream changes, pin GPU samples or normalize CPU
  buffers. Sequence/time remain continuous across encoder generations;
  `captureClock100ns` explicitly measures host arrival.
- `scripts/android_camera_probe.py`: `AdbServer` reads a structured snapshot
  and opens verified USB transport IDs without owning adb's lifecycle.
  `protobuf_fields`, `protobuf_usb_devices`, `usb_devices`, `read_exact`,
  `read_packet`, `adb_request`, `send_hello`, `json_bytes` validate framing.
  `PhoneCameraClient.probe/connect/command/close` own the two channels. `capture`
  records per-generation video/metadata, reports arrival cadence and runs
  keepalive/optional JSON stdin controls; `main` selects USB serial/local output.
- `tests/phone_camera_tests.cpp` checks USB identity without Windows bus paths,
  malformed protobuf, actual AVC/HEVC system-decoder NV12 and saved source selection.
  The HEVC data row skips explicitly when the optional decoder is unavailable.
  `OwnedLaunchState::Launch` and owned-child assertions retain the native
  launch diagnostic, including Application Control errors, rather than
  reporting only a null fixture child. They do not skip or retry blocked cases.
  `android/tests/test_host_probe.py` supplies eleven fake-server/parser/control
  checks with no live adb changes. Viewport-to-phone zoom handoff remains pending.
  Canonical cold launch, missing-only legacy fallback and blocked-launch cases
  use isolated fake endpoints; `PhoneCameraClient` mirrors the same launch rules.
  `FakeAdb.peer` emits ordered video packets until the fixture closes, instead of
  ending after ten frames. The capture test keeps its packet/control assertions
  with a one-second duration rather than requiring three frames in 50 ms;
  this avoids coupling protocol correctness to host scheduler/TCP timing.

## Entry Point

### `src/app/main.cpp`
- On Windows, constructs `OkuFlowApp`, calls fallible `Initialize()`, then
  enters `Run()` inside a `try`/`catch`.
- On non-Windows platforms, exits with an unsupported-platform message.

`MainWindow::refreshTextClarityUi()` updates the master-dependent Fine-tune text enabled state and explanatory accessibility text. Constructor/toggle/language refresh calls keep standalone widgets consistent; application settings restoration must explicitly call it after blocked master updates. The nested `textClarityRefinements` wrapper provides a visual guide and indentation while leaving the disclosure header available.

## Windows runtime startup and installer

- `app/windows_runtime_contract.hpp/.cpp` is a Qt-free Win32 contract shared
  by the /MT launcher and Setup Assistant. `WindowsRuntimeVersion` is a
  four-component version; `SupportsWindowsRuntimeVersion` admits supported
  newer versions of the same major. `IsSystemWindowsRuntimeReady` checks
  registry versions and required system DLLs. `WindowsExecutableDirectory`
  locates the current executable. `VerifyWindowsRuntimeInstallerFile` hashes
  a bounded installer using BCrypt and the generated pin.
  `QuoteWindowsArgument` preserves Windows argv quoting;
  `ClassifyWindowsRuntimeInstallResult` handles success, newer-installed,
  restart, raw/HRESULT cancellation and failed detection.
- `WindowsRuntimePayloadDirectory` selects the `app/` directory when present,
  otherwise the legacy flat directory. It does not require the nested child
  to exist before choosing that directory, preventing stale flat fallback.
- `UserDataPaths`' internal `BundleInstallDirectory` recognizes a nested `app`
  directory with a parent launcher. Constructor and `ValidateRoot` protect the
  whole bundle before canonical/junction containment checks.
- `scripts/windows-installer/bundle_layout.ps1::Get-OkuFlowBundleLayout` reports
  root, payload root, prefix and child EXE path for legacy/nested snapshots.
  Metadata uses root-relative file records and payload-relative reviewed adb
  records. MSI signing/icon checks and guest downgrade checks follow this path.
- `scripts/organize_release_bundle.ps1` relocates a fresh flat staging payload
  into `app/` before metadata generation. It retains the root launcher/docs and
  rejects existing payloads, metadata, captures and reparse points before moving.
  `test_bundle_layout.ps1` checks synthetic byte/integrity and refusal cases.
- `app/windows_runtime.hpp/.cpp`: `WindowsRuntimeStatus::Ready`,
  `CheckWindowsRuntime` and `VerifyWindowsRuntimeInstaller` expose Qt wrappers.
- `src/launcher/launcher_text.hpp`: native `Language` / `Message` enums and
  `kText` provide 25 languages Ã— 14 preflight messages without Qt dependencies.
  `LanguageFromWindows` maps the UI LANGID (Traditional Chinese falls back to
  English); `Translate` looks up text, `IsRightToLeft` selects dialog direction.
- `src/launcher/main.cpp`: `Text` formats `%1` with a supplied error value using
  Windows UI language before the Qt application starts. `Inform` shows a native
  Common Controls v6 dialog with matching RTL flags.
  `PrepareWindowsRuntime` checks or offers the pinned vendor installer using
  an owned temporary copy. `wWinMain` forwards arguments, working directory,
  environment, redirected streams and child exit code to `OkuFlowApp.exe`.
  New bundles launch `app/OkuFlowApp.exe`; the installer preflight uses that
  same runtime directory. Runtime contract fixtures exercise both layouts.
- `SetupAssistantDialog` accepts an optional injected runtime-check function
  for fixtures, adds the Windows dependency row and includes it in
  `NeedsSetup`. `BeginWindowsRuntimeInstall` prepares/verifies its private
  copy; `StartElevatedInstaller(Dependency)` shares owned process polling
  with NVIDIA installation, preserving runtime cancellation/reboot outcomes.
- `cmake/WindowsRuntime.cmake` generates `windows_runtime_pins.hpp` from
  the tracked pin. `stage_windows_runtime.ps1` verifies SHA-256, Microsoft
  signature and exact license. Release metadata records both executable
  signatures and the Microsoft license/provenance package. Signing covers
  both binaries; windeployqt examines the Qt child.
- `windows_runtime_contract` exercises the actual launcher with the private
  `launcher_fixture` (argv, Unicode, cwd, environment, streams, exit status),
  version admission and tampered installer rejection.
  `setup_assistant_interactions` checks supported/missing runtime presentation
  and incomplete-bundle behavior without executing installers.

## Windows phone/GPU progress safeguards (2026-10-13)

- `PhoneStartupBudget` in `capture/phone_startup_budget.hpp` owns `Start`'s
  original 12-second monotonic `Deadline`; `RemainingCameraMs` subtracts USB
  setup time and a one-second decode reserve. `PhoneCameraTransport::Run` sends
  that value as `start.startupBudgetMs`; an initial HEVC retry shares the same
  budget, and cancellation still wakes `Start` on its 20 ms wait slices.
- Android `CameraStartupPlan` stores the command-arrival epoch, clamps the
  budget to 1â€“12000 ms and retains at most six modes with at least 500 ms each.
  `attempt` divides remaining time after future 200 ms retry gaps, capped at
  four seconds; `remainingMs` and `retryDelayMs` cannot renew the absolute
  deadline. The live start-token predicate rejects retired queued attempts.
  `CameraEngine::begin`, `candidateState` and `startupCurrent` enforce and report
  the assigned attempt deadline around synchronous setup and callbacks.
  `CameraStreamService::command` captures arrival before the owner queue;
  `StartupWindow` preserves it through pre-video settings/thermal restarts and
  retires on first video or session stop. `restart` passes that immutable epoch
  to the engine. JVM fake-clock regressions cover stalled-first/fallback-success,
  1/2/6 candidates, depleted budgets, consumed queue/setup time and cancellation;
  these are separate from physical camera/driver acceptance.
- `PhoneFrameProgress` in `capture/phone_frame_progress.hpp` tracks packet and
  decoded-frame clocks separately. `BeginGeneration` clears HEVC generation
  progress; `EncodedFrameReceived` starts its four-second first-output check
  only with real video rather than CSD. `FrameDecoded` is the only
  decoded-progress renewal.
  `PhoneCameraTransport` uses `Check` on packet and idle iterations, preserving
  the existing one-time HEVC fallback and reconnect policy.
- `CaptureAperture` and `ResolveNv12DisplayAperture` in `capture/capture_buffer.hpp`
  validate integral, even, in-bounds minimum/geometric display apertures and
  use the negotiated phone dimensions when metadata is absent. The decoder
  keeps allocation dimensions for buffers while publishing visible dimensions.
  `MediaFrame::gpuVisibleX/Y` carries its GPU crop; `CopyMappedCaptureTexture`
  crops both planes while keeping the UV origin at allocation height.
- `GpuFrameReadbackResult` distinguishes `Ready`, `Pending` and `Failed`.
  `MediaCaptureSession::ReadbackGpuFrame` queues one copy, retains the original
  sample and texture, polls its query, and maps with `DO_NOT_WAIT`. It uses
  `try_lock` and a one-second deadline. Failure requests reconnection while
  retaining pending resources until the stop coordinator proves completion.
  `HasPendingGpuReadback` and `CaptureGpuPending::Readback` keep the same frame
  through `RunFrameTick`; retries do not re-copy or re-run temporal processing.
- `FenceProgressWatchdog::Observe` observes submitted and completed values,
  faults on one second without forward completion, and treats device-removal
  sentinels as failure. New submissions do not renew the deadline. Idle or
  occluded presentation with no outstanding GPU work remains healthy.
  `D3D12Presenter::PollGpuProgress` runs at submissions and idle app ticks.
- `HandlePresenterFault` logs CUDA causes and latches a persistent camera
  placeholder and assertive processing status. Transient messages cannot hide
  it. `OnTextClarityControlsChanged` marks viewport changes when SuperRes forces
  zoom, including when no camera frames arrive.
- `DeadlineWorker` runs owned serial host tasks on one independent thread.
  `Execute` publishes completed results, distinguishes exceptions/deadlines,
  and refuses later jobs after a timeout. Destruction requests shutdown without
  joining a blocked vendor call. Borrowed resources must be retained by the
  caller; the worker never treats a timeout as completion.
- `MaxineSuperRes::Worker` selects the CUDA device and runs load, synchronous
  inference and teardown within a one-second host deadline. `IsFaulted` forbids
  further adapter access to a still-running SDK call. The production caller
  is now `MaxineAsyncProducer`'s independent worker, which retains its entire
  private closure on unknown completion. `CudaInteropSurface` does not wait on
  that adapter or quarantine healthy base storage for a private failure. Strength
  changes settle for 200 ms before a model reload. Performance override never
  bypasses safety bounds.

Regression coverage: `deadline_worker` exercises completion publication, serial
ownership, exceptions, timeout, rejected retries and destruction with a retained
job. `capture_shutdown` adds WARP readback checks for single-copy pixel identity,
mutex contention, deadline retention and mismatched frames. `capture_buffer_layout`
covers padded NV12 and aperture crop offsets; phone/fence fixtures use fake clocks
for silent decoder and completion stalls. These are local fixtures, not evidence
of real-phone disconnect, Maxine visual quality or long-soak acceptance.

### Dedicated Codex agent launch profile

`CodexAgentProfile` and `CodexAgentLaunchProfile` in
`common/codex_agent_profile.hpp` validate the offline CLI version and the exact
feature name/stability inventory before constructing a restricted agent-only
launch. They disable provider/execution features and only produce bare-key
MCP overrides after validating every inventory name. Initialization explicitly
opts into the experimental API. This does not establish runtime tool acceptance.
The first-party profile under `assets/codex-agent/` records local offline facts;
`codex_agent_profile_contract` checks malformed/version-drift/name-injection
boundaries without a real Codex process or credentials.

`AssistiveRuntime::SetPeriodicAnalysisEnabled` preserves explicit user
Read/Explain/Assistant work when profile/overlay configuration disables periodic
analysis. `StartVlm` stamps request origin; only periodic-owned preparation/reply
may be canceled by that setting. The legacy explicit `SetModes` stop behavior
remains available; manager profile transitions use the scoped method.

## Agent-control development core

`include/okuflow/app/agent/` mirrors `src/app/agent/`:

- `control_registry.hpp/.cpp`: `CatalogEdition`, `Tier`, `ToolId` and
  `ToolDescriptor` define the finite catalog. `CatalogEditionName`,
  `ControlRegistry` and `FindControlTool` return immutable, Qt-free descriptors
  and draft-07 schemas for development editions and gated `v1`.
- `app_control_service.hpp/.cpp`: `Field`, `FieldWrite`, `ControlPlan`,
  `ControlResult`, `ControlRequest`, `ContextKind`, `ControlGrants` and
  `ContextSpec` form the typed boundary. `IAppControlTarget` exposes safe state,
  field plans/transitions, queries, async execution and exact recording cleanup.
  `AppControlService` provides `SetEnabled`, `Begin`, `End`, `SetGrants`,
  `SourceChanged`, `NativeFieldsChanged`, queued `Submit`, `ResolveApproval`,
  `PendingApproval`, `StopAndUndo`, `SetEventSink` and `Edition`. Private
  `Normalize`, `Dispatch`, `Run`, `Authorized`, `Stale`, `Ask`, `Finish`,
  `ValidContext`, `Event`, `Expire` and monotonic `Now` enforce admission,
  revisions, budgets, delivery and single-use approval lifetime. `FieldName`
  fixes DTO field names; `ControlResult::Failure` allowlists failure codes.
  `Continue` refuses an external lease's observe, camera-source, photo and
  recording tools with `denied_by_policy` when the lease bundle lacks that
  toggle, before `Prepare` can report availability.
  `NativeFieldsChanged` ignores empty polls without changing versions or
  emitting a user-edit event. `StopAndUndo` retains eligible failed inverses
  for a later revision-checked retry within the retained context; its `retained`
  list is separate from
  restored/skipped results. `sourceRestoreFields` identifies requested temporary
  source restoration, not completed hardware reconnection.
  `Begin` synchronizes native state before capturing the new baseline; `Continue`
  rechecks it after deferred metrics. `End`/`RetireOwner` consume source-journal
  entries when requesting the exact owner's restoration, so a later Undo does
  not label an already-requested restore as skipped. Undo events carry both
  `retained` and `sourceRestoreFields` alongside restored/skipped fields.
- `receipt_labels.hpp`: pure `ReceiptLabelSource(std::string_view)` maps the
  finite canonical/legacy receipt vocabulary to existing translated control
  captions. Unknown keys use `kReceiptFallbackLabel` (`Settings`) and are never
  echoed as UI labels. `FieldLabel` translates the returned source caption.
- `app_control_target.hpp/.cpp`: `AppControlTarget` implements state/fields,
  `Prepare`, `Apply`, `Save`, `Query`, `Execute`, modal/user/recording predicates,
  `StopOwnedRecording` and `NoticeNativeChanges` against `OkuFlowApp`.
  `CancelOwnedSource` observes genuine preceding edits and then refreshes its
  baseline after its own source restoration without attributing it to the user.
  `ProfileFields` is the finite profile inventory; `CreatePhoto`, `PhotoState`,
  `CancelPhotoReadback` and `RecordingEnded` retain durable operation identity.
- `operation_tracker.hpp/.cpp`: `OperationKind`, `OperationState`,
  `OperationRecord`, `OperationStateName` and `OperationTracker` define legal
  progress. `Create`, `Advance`, `SetSession`, `Get`, `SafeStatus`, `Prune` and
  monotonic `Now` reject stale/illegal terminal changes, bound retained records,
  and sanitize outcomes.

`OkuFlowApp::ControlService` exposes the adapter entry. Initialization owns the
service/target; destruction retires them before capture resources. Native
configuration synchronization reports changed field revisions. In
`app_agent_capture.cpp`, `RequestPhotoCapture`, `RequestRecording` and
`StopRecording` replace button lambdas with named production entry points.
Photo readback/writer callbacks retain an operation id through paired commit.
`RecordingManager::SetRequested` accepts an optional app-owned reserved session;
`StopSession` checks its expected id and `SetSessionAudioResult` changes only
that session. `CurrentSessionInfo` retains identity through stopping/finalizing.
`RecordingSessionInfo` adds `RecordingOwnerKind`, `RecordingAudioRequest`,
`RecordingSessionCompletion`, owner id and actual audio; queued completions
report the ended session rather than querying a replacement.

Settings version 17 records `agentControlEnabled` (default true) and
`externalMcpEnabled` (default false),
conversation `kind` and `toolsetVersion`. `EffectiveConversationKind` infers
legacy Explain only for absent/unassigned kind and maps unknown values to
tool-free Assistant. `AssistiveSettings` and `AssistiveFromJson` use these
defaults for missing control keys. Pre-version-17 loading ignores unsupported
control keys and uses the same defaults; version-17 explicit true/false values
survive loading and saving. Camera/capture grants remain transient and off.
`agent_control_contract`, `settings_store_round_trip` and `recording_manager`
exercise malformed arguments, deduplication, approval replacement, cancellation,
user edits, safe durable outcomes and exact-session stop without windows.
These fixtures do not establish completed provider/MCP or native acceptance.

### Bounded HTTP agent transport

`common/http_agent_client.hpp` defines `HttpAgentToolMode` (Off, Tools,
LegacyFunctions), `HttpAgentConfig` (exact endpoint/model/header-only API key),
`HttpAgentTurn` (app-issued context/origin and authorized initial viewport),
`HttpAgentOutcome` (text/fixed error code/interruption) and
`HttpAgentCapability` (session verdict and optional parallel-flag acceptance).
`HttpAgentClient::Configure` and `SetControlService` cancel obsolete work without
performing inference. Independent configuration/service epochs retain the newest
settings when cancellation callbacks change them again. `TestCapability` is the explicit one-request, frame-free
capability action; its optional boolean tests `parallel_tool_calls:false`.
`Capability` is session-only and keyed by endpoint/model/wire mode; credentials
invalidate cached verdicts. `Start` requires an active app-issued embedded context,
validated configuration and successful capability test; `Busy` reports ownership.
`Cancel` aborts the exact reply/pending core call; generation and context checks
retire late callbacks. The adapter never issues a context, consent or grant.

The implementation's `Descriptors` converts the authoritative registry to provider
function schemas; `ParseCalls` validates the entire round, including exact service
argument validation, before `RunNextCall` submits calls sequentially. Original
assistant objects preserve argument strings/objects; tool replies retain call order,
then authorized observations form a user image message. `StripImages` removes prior
images before another round. `Post` bounds serialized requests and refuses redirects;
`ReplyFinished` bounds response bytes and maps failures to fixed codes. `Finish`
clears image-bearing memory and emits one completion. Private `IsCurrent`,
`ConfigurationValid` and `ProfileKey` enforce origin/configuration/capability rules.

## Assistant chat and consent widgets

- `AssistantChatView` (`ui/assistant_chat_view.hpp/.cpp`) renders trusted
  `SetChatSnapshot` DTOs into escaped native chat bubbles. `RenderMessages` uses
  root-level text paragraphs rather than table frames so Qt's accessible character
  rectangles match the painted viewport after movement, wrapping and scrolling.
  Its viewport resize filter coalesces a render after child layout settles,
  keeping paragraph margins and thumbnails within the actual available width.
  Bounded JPEG previews are decoded only as in-memory document resources with
  preserved aspect ratio; provider text cannot introduce image URLs. Draft/attachment/busy
  belong to the app model; selection/scroll remain per view. `SetSettingsWidget`,
  `SetApprovalWidget` and `SetRoutingWidget` place native consent content.
  `ShowSettings`, `ShowHistory` and `CloseDrawer` provide overlays with Escape
  and focus restoration. `FocusApproval` deliberately focuses Deny.
  `DrawerContainsFocus` lets the main window defer its outer Tab bridge to
  native traversal inside an open overlay drawer.
  `RefreshTabOrder` inserts native approval/routing controls between the
  transcript and composer actions. MainWindow defers its outer cross-window
  bridge whenever focus is within the visible floating Assistant, preserving
  native toolbar, drawer, decision and busy-action navigation.
  `RefreshIcons` follows palette changes, including the moved floating header.
  Finite Explain-origin metadata adds a trusted camera tag.
  `SetConnectionState` and `SetUndoAvailable` mirror app state without authority.
  Accessors expose header, transcript, composer and history actions for binding.
  The composer footer orders manual Read Aloud, flexible space, attachment and
  Send; `RefreshActions` keeps Read Aloud visible but disabled without a completed
  idle answer and reserves the transient action strip for Stop/Undo.
  The private `ChatAttachmentSwitch` is a `QCheckBox` with fixed 80x44 size hints,
  a whole-widget `hitButton` and palette-aware `paintEvent` for the bundled camera
  icon, track, position-changing thumb and focus outline. Native checkable state,
  Space handling, translated name/tooltip and model bindings are preserved.
  `RefreshIcons` also refreshes its camera icon. Tab order follows the footer and
  all geometry mirrors with the layout direction.
  `ChatDraftEdited`, `ChatAttachmentChanged`, `ChatSubmitted`, `ChatNewRequested`,
  `ChatHistoryRequested`, `ChatSelected`, `ChatReadAloudRequested`, Stop/Undo and
  AISettings signals represent explicit UI actions. Enter sends once, Shift+Enter
  inserts a newline. Incoming text/receipts never execute markup or consent.
- `AgentControlPanel` (`ui/agent_control_panel.hpp/.cpp`) supplies settings-sheet
  content and detached `approvalWidget`/`routingNoticeWidget`. `SetEnabled` mirrors
  persisted masters but provides no duplicate switches; `SetSessionGrants`
  restores turn/session state without authorization signals. `SetPendingApproval`
  displays a human question from a finite tool map, immutable target display and
  expiry; collapsed Details retains exact canonical identity/normalized JSON.
  `ClearPendingApproval`, `hasPendingApproval`, `approvalDenyButton` and
  `FocusPendingApproval` support explicit navigation and single-use decisions.
  Orientation uses canonical `degreesClockwise`; recording time is localized.
  `NativeChangeSummary` maps a finite set of canonical normalized scalar image
  fields to existing translated labels and locale-formatted values; it never
  copies raw model strings or changes approval authority.
- `SetExternalConnection` never opens a section. `SetExternalLease` mirrors
  `ContextStatus` with read-only active grants, times and budgets. Optional grants
  start off. `AppendAction` bounds plain activity to 128 blocks/32 KiB and emits
  `actionReceiptAdded`; app-owned receipts attach only to their original row.
  `SetStatus` routes persistent accessibility; `SetCapabilityStatus` is display
  only and mirrors the pending test state into a shown routing notice.
  `SetRoutingNotice` chooses an explicit capability test or ready conversation
  action; `RefreshRoutingNotice` changes a shown row without opening hidden UI.
  `routingNoticeShown` exposes local visibility without implying authorization.
  The existing disabled-control status remains visible in the settings drawer.
  `RequestStopAndUndo` is shared by visible and shortcut paths.
  Trusted activation signals are session grants, approval/lease/revoke, stop/undo,
  capability testing and routing choices. The obsolete `enabledChanged` signal
  remains source-compatible but is never emitted by this panel.
- `MainWindow::assistantChatView`, `agentControlPanel`, `agentLiveRegionLabel`
  expose presentation and the persistent live region outside hideable chrome.
  `focusAgentApproval` reveals Advanced Assistant and focuses Deny only for a
  pending record. Application Ctrl+Shift+A/U work with ordinary chrome hidden.
  `AiSettingsDialog::result` alone owns persisted embedded/external masters;
  `UpdateProviderFields` unchecks/disables coding/internet when control is on,
  and result guards against conflicting enabled permissions;
  it never saves session grants, approvals or leases.
- `AssistiveOverlay::SetChatSnapshot` updates the same pure chat view without
  raising/showing it; `ShowChat` is reserved for explicit admitted Explain.
  `SetReadContent` owns a separate result pane and never mutates chat; legacy
  `SetContent` delegates to that Read path. `ReadResultPane`/`ReadBody` expose its separate result for semantic
  fixtures; the destructor releases that pane. `SetControlRoutingNotice`, `QuestionDraft`, `ClearQuestionDraft` retain
  refused shared questions until app acceptance. Existing manager forwarding
  emits QuestionSubmitted/NewChatRequested once; `FocusTargets` exposes the
  chat-entry/footer/placement controls of the current view; native traversal
  inside it also includes dynamically visible routing and approval controls.
  An explicit `EscapeRequested` activates the camera owner before returning
  focus to its render target; passive snapshots never activate either window.
- `MainWindow::RefreshControlReadouts` executes registered native readout
  refreshers after a signal-blocked typed control batch, without synthetic
  slider events. `ColorSchemePicker::clearCustomScheme` silently restores the
  original absent custom recipe and editor defaults for internal atomic undo.
- Native chat regression fixtures bind `VisibleChatSession::SnapshotChanged`
  to both views, inspect the independent `ReadBody`, and retain each view's
  selection and scrolling during deltas. Docking fixtures press a verified bare
  margin on `chatView()->headerWidget()`, preserving native move/latch/cancel
  assertions rather than targeting the hidden legacy toolbar.

`MainWindow::setAnnotationMode`, `AnnotationOverlay::SetActive` and `SetTool`
accept an explicit `preserveFocus` flag, defaulting off for manual controls.
App-owned controlled transitions pass it to avoid focusing the canvas, Draw
button or selected tool. Controlled canvas admission uses
`WA_ShowWithoutActivating`; `ShowToolOptions` retains announcements and normal
manual focus behavior while suppressing controlled focus transfers. The native
controlled-toggle fixture verifies owner focus through off/on and tool changes
while preserving ink, undo/redo and the manual-exit clearing contract.

## Dedicated agent provider and answer ownership (0.3.1 development)

`CodexAgentClient` is a separate, restricted JSONL child. `Configure` retires its
own process/session when provider identity changes; `TestCapability` explicitly
runs bounded offline preflight and a tiny no-frame dynamic-tool round trip.
`SubmitTurn` requires an active app-issued embedded context, matched catalog,
confirmed capability and initial-frame grant. `StartThread`/`StartTurn` declare
only the shared registry, use a controlled read-only working directory and bind
the turn exclusively from its correlated start reply. `HandleCall` buffers at
most eight pre-ack calls without effects; `DispatchCall` denies wrong/retired
turns and undeclared tools. `Answer` returns only common safe DTOs and bounded
view observations. `Tick` uses monotonic deadlines and cancels exact queued
control calls before timeout delivery. `Cancel`, `ResetConversation` and
`Shutdown` retire only this child, never the ordinary Assistant or external adb.
Notification policy fails closed on forbidden items or unexpected MCP startup.
No history/resume operation or account-management API is exposed by this client.
`SubmitTurn` returns false without a terminal signal when it owns no admitted
turn. Its optional completed-text bridge accepts only bounded user/assistant
strings, never backend IDs, images, tool results or permission objects. Quoted
bridge data remains ordinary input. Retired identities are scoped to their
thread and reset only for a new child. Synchronous UI callbacks are fenced by
session/generation epochs before preflight, turn start or buffered dispatch;
a replacement request cannot be started twice by the superseded callback.

`AssistiveRuntime::BeginControlAnswer`, `ControlAnswerStarted`,
`ControlAnswerDelta` and `CompleteControlAnswer` share busy state, accessible
answer display, manual speech and local lecture-note exchange storage with the
existing Assistant. They confer no grants. Completion is single-use and clears
the stop callback before emitting; periodic-mode changes cannot cancel it.
`StopAssistant` dispatches only the active answer's exact cancellation callback.

`OkuFlowApp::InitializeAgentControl` wires the permission panel and separate IPC
endpoint to the shared service; `RefreshAgentControlSettings` retires the
app-created recording owner on provider/master changes. `TestAgentCapability`
requires explicit UI activation. `StartAgentConversation` preserves the draft
and never submits it; `SubmitAgentPrompt` obtains the app-owned context and uses
only a current authorized viewport observation for an initial image.
Readiness changes refresh shown routing choices in both views. A control-enabled
but untested submission offers the explicit capability test; configuration
changes retire old readiness. `SubmitAssistantPromptText` consumes the tool-free
override at entry even if admission is refused; the native fallback moves a
restored Agent-kind chat to a new Explain conversation while preserving its draft.
`StartAgentConversation` clears any old one-shot override. `Initialize` binds
both Connect ChatGPT buttons to the same runtime login action and mirrors
server/account state, connection dots and model tooltips to both chat headers.
`SetAssistantBusy` gates both login buttons together.
`CancelAgentTurn` cancels provider work and exact context; its optional undo
path calls `StopAndUndo` before provider cancellation and lease revocation, so
temporary camera ownership is still available when requesting restoration.
It preserves later native user edits. Both editors refuse incompatible
conversation routing before clearing a prompt or capturing pixels. History
kinds remain immutable; agent ids never enter the ordinary history loader.

`codex_agent_client_contract` uses a separate fake process, pinned preflight
facts, real JSONL framing and a private core target. It exercises pre-ack calls,
stale-first and retired-first calls, forbidden tools/items and unavailable
admission. It starts no account, inference provider, camera or native windows;
live dynamic-tool compatibility remains a separate acceptance gate.

## First-party local MCP implementation (0.3.1 development)

`mcp::Json` parses bounded strict UTF-8 JSON with depth/node limits;
`MatchesSchema` validates the shared finite registry schema subset.
`Protocol` owns pinned MCP lifecycle, ids, tool descriptors/results, cancellation
and framing strikes. `AppChannel` abstracts app forwarding; `PipeChannel`
verifies the owned app peer, pairs/reconnects and forwards bounded requests.
`Handle`, `PeerIdentity` and `InstanceDescriptor` retain exact native process,
file, token/session and pipe identities; persistent descriptors contain no
secret or grant. `AppIpcEndpoint` stays inactive until the owner's external
master switch. `PendingPeerId`/`PendingClientName` expose the verified pending
row, `ApprovePending(expectedPeerId, grants)` issues a GUI-owned lease only
when the displayed peer still matches, and `Revoke` retires exact authority.
`SetPendingChanged` never opens a window; `SetEnabled(false)` cancels the owned
connection before bounded worker cleanup. The final image fence rechecks
native identity, lease, cancellation, source/processing/view stamp and grant.
Codec/base64 work is performed by the writer, not the GUI thread.

`Get-PublicationUtcTimestamp` in `scripts/windows-installer/publication_time.ps1`
preserves explicit UTC/offset semantics for strings and PowerShell's typed
JSON dates. The publisher uses it for fresh validated manifests; ambiguous
local timestamps are refused. Its private regression accepts six explicit
representations and rejects five ambiguous ones without builds or desktop use.

See [MCP integration and limitations](mcp_control.md). The deployed-image
CMake policy only selects the finite expected executable identity; it neither
grants control nor promotes a development catalog or bypasses release gates.

## Shared visible chat (plan 52 Draft 6, development)

`VisibleChatSession` owns one logical transcript, bounded memory-only frame previews, shared draft,
turn attachment and busy state for Advanced Assistant and floating Explain.
`ChatTurnTicket{conversationId,epoch,requestGeneration}` fences admission,
streaming and terminal replacement. `Admit` creates one question and one
assistant placeholder; `Complete` replaces the authoritative answer once.
`New`, `Restore` and `RetireBackends` invalidate old ownership.
`ChatBackendKind` is immutable for the visible conversation; private Explain,
Agent, Assistant and Coding backend bindings stay outside presentation DTOs.
`TextContext` and `BridgeContext` expose at most four completed exchanges /
32 KiB of role/content strings, with no images, IDs, grants or action records.
`ActiveTicket` exposes app-owned presentation ownership. `AppendActionReceipt`
stores bounded first-party presentation text separately; its ticket overload
admits a late receipt only into the original row in the same conversation/epoch.

`AssistiveRuntime::PrepareChatTurn`, `SubmitExplainFollowup`,
`ResetVisibleChat`, `FailPreparedChat` and `HasChatTurn` integrate that store
with provider/notes ownership. Explain followups retain a restricted Explain
thread; Read remains an isolated request and never mutates chat.
`CancelPeriodicForUserTurn`, `ReserveForegroundCapture` and
`IsForegroundBusy` separate explicit user work from periodic analysis.
`PublishChatFinished` preserves compatibility terminal signals without double
rendering. Notes are still written once by the runtime after accepted success.

Ordinary `CodexAppServerClient::RequestVisionTurn` adds a trusted
`permitCoding` argument. Pending turns freeze coding/internet/workspace
permissions at admission; saved Assistant histories force read-only while
Coding histories require the current explicit workspace grant. Explain never
borrows persistent/coding authority. `HttpAgentTurn::textHistory` accepts only
bounded completed user/assistant role pairs and never carries historical images.
Tool-free HTTP followups use the runtime path independently of agent probes.

`OkuFlowApp::LoadAssistantConversationById` preserves immutable saved kinds.
Agent histories restore presentation only and never enter the ordinary loader.
One control event sink and trusted panel binding feed both native control panels.
New/history transitions stop manual speech and retire the old control owner.
`NewConversationKind` applies the trusted native New policy: Codex with control
off starts Coding only with the explicit coding setting, otherwise Assistant;
HTTP or embedded-control New starts Explain. `StartNewAssistantConversationAs`
keeps the Qt New slot unchanged while Explain explicitly resets to Explain.
`ResetVisibleChat(kind)` accepts that immutable initial kind. Explain itself
always uses the restricted retained backend even after a Coding New.
## MCP release contract helpers (0.3.1 development)

`mcp::main --build-info` emits a single bounded fact object before opening
stdio/IPC. `Assert-McpReleaseBuildInfo` requires exact V1/deployed-policy facts;
`Assert-McpReleaseCache` refuses all development editions before bundle builds
and tests. `Get-McpBuildInfo` runs only an owned diagnostic child with a timeout.
`Assert-McpNativeImports` uses VS2022's read-only `dumpbin` and permits only the
five reviewed Windows system libraries, excluding Qt and dynamic VC runtime.
`Get-McpReleaseBuild` combines cache, actual executable and native import checks.
`Assert-McpBundleInventory` verifies declared path/hash parity in legacy or
nested snapshots, while retaining compatibility with published non-MCP bundles.
The release script signs/copies/compares the bridge; metadata records it and MSI
0.3.1+ requires it. These checks do not themselves establish owner/provider or
camera acceptance, and no V1 release has been produced.

ObservationService accepts an optional monotonic clock for deterministic fixtures, retains the actual processed-frame timestamp, and recomputes frameAgeMs on completion and every Metrics read. Missing/future timestamps, held output and frames older than MaximumFreshFrameAgeMs (1000 ms) are stale. AppControlTarget AutoAdjust requires known false held/converging flags; unknown GPU state reports unavailable. Resolve continues to use the immutable observation transform across view/processing changes, rejecting source or geometry changes.

CameraSelect now prepares exact opaque camera identity with recording/photo/start-retirement guards, records trusted OwnedSourceRequested revision ownership, and dispatches background StartCameraCapture with AgentTemporary origin and an explicit default-format override. A bounded 50 ms GUI metadata poll advances stopping/starting/first_frame durable status, with a 30-second first-frame deadline. CameraTransitionFailed records capture_failed and requests previous-actual/saved restoration without changing saved intent. CancelCameraTransition accepts failedFallback to mark actual restored identity as fallback; cancellation or a native camera/format choice retires the request generation. OperationTracker exposes only boolean camera fallbackActive/fallbackRequested metadata. No driver/media work runs in the poll.

The shared ChatAttachmentChanged bootstrap binding immediately revokes an active embedded frame grant and fans that state to both native panels when unchecked. Checking during a foreground turn only refreshes the authoritative model; it cannot widen that turn's grant.

RevokeFrameGrant(owner) is trusted GUI-only revocation of both an exact owner's embedded image permission and its queued source-bound next-turn grant. ContextStatus.nextTurnFrames exposes pending native consent separately from an ended context's inactive frames grant. Unchecking chat attachment calls this even after the provider turn ended; another owner cannot revoke the queued grant.
## Finite grouped-control input validation

`MatchesControlSchema` is the first-party inline GUI-side draft-07 subset used
by `AppControlService::Normalize` through its local matcher. It rejects unknown
types without coercion, enforces finite object properties and property-count
bounds, bounded arrays and unique/items constraints, exact nullable/oneOf/not
semantics, number bounds and Unicode-codepoint string lengths. Recursion stops
after 32 levels. Fixed registry schemas are trusted code; caller values are not.
`control_schema_contract` covers wrong shapes, types, empty/broad objects,
out-of-range values, duplicated arrays, nullable branches and excessive nesting
without any desktop, account or device. The expanded native parameter inventory
and its current integration limits are in [control_inventory.md](control_inventory.md).

FrameProcessed only refreshes the observation input-age timestamp on a new captured sequence or source epoch, so changing effects on a cached input cannot make an old camera frame fresh. The bounded 120-entry/two-second timestamp window reports processed unique-frame rate (fpsBasis processed_unique_frames), rather than negotiated camera FPS; stalled output reports zero. Image State adds requested/effective/inactiveReason for reading-color stages, and raw-passthrough preparation rejects actual image-effect changes.
## Expanded parameter catalog (0.3.1 development)

`ParameterRegistry`/`FindParameter` provide 94 immutable adjustable descriptors;
`ProtectedControlRegistry` reports 17 protected metadata entries without values,
and `ControlActionRegistry` distinguishes 34 workflows from settings. The new
`DevFull` edition adds 17 typed discovery/get/set tools, while the earlier
14/17/19-tool development editions remain unchanged. V1 remains unpublished.

`ParameterGroupForTool` maps finite ToolIds to the eight groups.
`ParameterGetSchema`/`ParameterSetSchema` produce closed, bounded input schemas;
setters reject empty/unknown fields and never accept raw settings paths or
permission keys. `ParameterMetadataPage` uses stable after-field pagination,
at most 16 entries and 32 KiB per page. Runtime targets must separately supply
honest availability and current/effective values: metadata/schema presence
does not claim that a native setter is integrated or active on this hardware.
Output destinations are opaque owner-approved directory handles, never model
filesystem paths. See [the inventory](control_inventory.md) for exact native
handlers, side effects, persistence and outstanding integration gates.

### Durable logical chat history

`StoredVisibleChat` contains an opaque logical id, title/draft, timestamps and
bounded terminal text rows. `VisibleChatSessionStore` loads only owned
opaque-id JSON children, strictly validates its versioned schema, rejects
symlinks/traversal and known credential echoes, and writes with `QSaveFile`
atomic replacement with direct-write fallback disabled. `List`, `Load`,
`Save`, `Rename`, `Delete`, `Revision` and `ErrorCode` expose text storage
only, never backend references or authority. Limits are 128 rows, 1 MiB per
record, 64 conversations and 32 MiB retained total, evicting oldest owned
records. Reload work has finite file/byte budgets.

`VisibleChatSession::RestoreLogical` restores full mixed text as Explain,
clearing backend bindings/attachments; `IsLocalLogical` distinguishes these
records from legacy provider history even when a provider id resembles a UUID.
`OwnsTicket` admits a late first-party receipt/grant presentation only in its
original visible epoch. Explain-button user and assistant rows retain the
finite `origin:"explain"` presentation tag; other origins omit it.
`SetImagePreview` accepts only the active turn's bounded JPEG payload and keeps
at most eight thumbnails of 64 KiB each in memory. Restore, text/bridge context
and the history store's explicit serialization omit `imagePreviewJpeg`.
`AssistiveRuntime::PrepareControlImagePreview(jpeg, ready)` prepares the
authorized initial control frame on the existing bounded worker pool, publishes
only to the still-current control ticket, then continues provider submission.
Cancellation/reset rejects both the late thumbnail and continuation; a busy
worker pool omits this optional preview without blocking the request.
`SubmitAgentPrompt` rechecks frame permission and source epoch after this wait.
`ChatImagePreview` derives a maximum-320-pixel JPEG from the exact preparation
worker frame; both HTTP and Codex publish it only after existing generation
checks and successful image preparation. `CompleteAssistantPreparation` accepts
an optional prepared preview, publishing it only after its existing saved-image
and generation guards.

`OkuFlowApp::PersistVisibleChat` debounces local history writes and flushes
before New/history transitions and shutdown. `RenameStoredConversation` and
`DeleteStoredConversation` operate on the selected owned logical record.
New mixed chats expose one logical drawer row; backend conversation-created
events do not create duplicate legacy entries. Legacy Assistant/Coding/Explain
entries retain their existing kind-safe provider loader and native dialogs.
Restoration writes no notes and starts fresh restricted backend segments.

Shared logical chat persistence is constructed under AppLocalDataLocation/visible-chat and debounced for 350 ms on SnapshotChanged, then flushed after turn cancellation during normal app teardown. New local logical UUIDs do not restore persisted private Explain anchors or create duplicate backend index entries; explicit legacy history selection still restores its immutable kind. Native logical rename/delete retain the shared dialogs and route to the bounded local store. Export reads the exact selected logical record and formats only validated role/text/action strings and fixed failure/interruption status, without changing the displayed chat or calling a provider.

ControlPlan.observedFields names only returned typed getter values; the service refreshes those revision baselines and emits fieldRevisions without widening other fields. requiresApproval forces exact native consent for sensitive preference changes. AppControlTarget exposes IsApplyingControlledTransition for persistence batching: native SavePersistentSettings still captures live UI/annotation/source settings during a controlled field delta but skips disk commit; target Save collects final live state and performs one settings commit. Driver safety-marker transactions remain separate from this ordinary UI batching.

`common/control_diagnostics.hpp` defines Qt-free `ControlFrameIdentity` and
`ControlDiagnosticSnapshot`. Identity retains the exact camera sequence,
source/processing/view/geometry epochs and monotonic submission time.
`DecodeControlDiagnosticValues` maps completed similarity-model, tripod state,
bump mode and readiness values to optional held/converging values; unready,
nonfinite and unknown tags never become settled/live assertions. Recovery blends
remain held. Decoder calls preserve provenance and the separately captured
keystone validity.

`ProcessingSettings::controlFrame` carries this caller-issued identity into
CUDA. `CudaInteropSurface::ControlDiagnostics` polls the existing asynchronous
stabilizer timing event without waiting or adding GPU copies. It publishes only
completed samples labeled after a successful frame submission, retains their
original time, and prevents an older pending sample replacing a newer output.
Keystone reference validity derives from successful quad detection, reset,
validated correction history and staleness, not history length alone. These
sampled diagnostics are not assertions about a newer presented frame; consumers
must verify identity and age. CPU-only surfaces return an unknown snapshot.

`SettingsController::LastCommittedSettings` returns an optional immutable
snapshot of the last successfully loaded or committed persistent configuration.
Defaults without a settings file remain unknown; live `MutableSettings` edits
do not change it. `RememberCommittedSettings` omits runtime plaintext API keys
and mirrors the serializer's canonical language token. Each successful storage
commit advances the snapshot, including migration/recovery and the first phase
of credential deletion. A later credential-cleanup failure may therefore return
false from `Save` while the snapshot accurately reflects the settings bytes
already committed. Failed writes retain the preceding snapshot. A recovered
valid backup remains last known persisted state even if primary writeback fails;
the snapshot does not assert that a primary file exists or that credentials were
successfully deleted. MCP getters must project only allowlisted knob fields from
this object and never return the full persistent settings object.

### Scoped scene analysis and finite assistant preferences (Plan 52)

`PrepareAssistantPreferencePatch` (`app/agent/assistant_parameter_policy.hpp`) is
pure whole-patch admission for the ten assistant preference descriptors and
`notes_output.lectureNotes`. It rejects unknown/protected/cross-group fields,
secret echoes, invalid bounds, unavailable model/effort combinations and voice
handles outside the supplied installed inventory. A model change may add its
coupled reasoning write. Sensitive provider/model/instruction/prompt writes
carry the explicit `ApprovalDomain::Configuration`; this grants no frame or
external-lease authority.

`AppControlTarget::{Prepare,Query,Apply}AssistantParameters`,
`AssistantParameterFields`, `AssistantParameterField` and
`FinalizeAssistantParameters` connect those finite descriptors to native
settings. Getters return bounded requested/effective/saved values, availability,
persistence and only the fields actually returned for revision baselines. Voice
inventory is lazy, never speaks, and exposes opaque handles rather than native
voice identities. Protected or oversized preferences have null public values.
Settings application is deferred until the core's mutation/save loop finishes.

`OkuFlowApp::{SetAgentSceneEnabled,AgentSceneState,CancelAgentScene,TickAgentScene}`
own a context-only scene intent. `sceneExplain` reports that intent separately
from `nativeRequested`; it never changes the saved native periodic checkbox.
Every image comes from an ordinary `view_observe` reservation and its existing
frame/byte budget. Exact context, source epoch and full frame stamp are checked
before delivery. Scope cancellation affects only its independent request lane.

`AssistiveRuntime::{SubmitScopedScene,CancelScopedScene,FinishScopedScene}` run
independent tool-free HTTP or temporary restricted Codex requests. They add no
chat rows or automatic speech, and write each accepted answer to runtime-owned
notes once. Permission/configuration retirement suppresses late presentation
and notes. `CodexAppServerClient::RequestVisionTurn` accepts an optional
app-issued `frameAuthorization` callback, checked at actual `turn/start` after
asynchronous initialization; ordinary Read/Explain callers retain their API.

Assistant preference reads accept assistant-only finite `valueKind` projections: `all` (default), `requested` and `saved`. `BoundAssistantPreferenceRead` packs actual serialized DTOs to 29 KiB, reserving core metadata within the fixed 32 KiB result cap. Over-budget equal strings may use the typed object `{sameAs: requested}` for effective/saved; distinct saved values are never substituted. Rows that still overflow use null values plus `filterRequired:true` and `unsupportedReason:response_budget`; clients select fields and a projection to retrieve the complete finite value without truncation. Saved-only reads do not observe/rebaseline current values. String admission counts Unicode codepoints, independently of the unchanged transport byte budget. Scoped scene notes are text-only under frame authority; saved photos remain the separately authorized capture path.

### Native grouped control admission and atomic batches

`ControlPlan::requiresFrames` independently requires the active context frame grant, including after exact configuration approval. `IAppControlTarget::BeginFieldPlan` validates without applying effects; `CommitFieldPlan` starts native mutation only after the service captures the complete original field snapshot. `EndFieldPlan` completes the batch and refreshes native readouts. The anticipated applied/effective receipt is bounded before any native effect. Approval resolution compares the complete normalized write sequence and its permission/history/recording semantics against the immutable approved plan. Pending approval DTOs expose finite native `approvalDomain` and `sideEffects` values for accessible headings, independently of model arguments.

`AppControlTarget::ImageParameterField`, `ImageProfileFields` and
`ImageParameterFields` map the finite 53 image and nine view preferences plus
the internal complete custom-color recipe. `PrepareImageParameters` uses the
pure `PlanImageParameterDependencies` to finish closed-schema validation,
native-step normalization and changed-value dependencies before publishing
writes. `QueryImageParameters` separates requested, effective and allowlisted
last-committed saved values; session-only values have no saved value.
`ImageParameterEffectiveIntent` follows master/child and consumed-mask
dependencies: selective sharpening needs active smart sharpening without
adaptive binarization; hysteresis and shared mask parameters depend on an
actually consumed adaptive or selective-sharpening mask.
`ValidateImageParameterBatch` is read-only and checks changed-field hardware
availability, every native control's type/range, complete recipe built-in
selection and component consistency, and pause/hold/override prerequisites.
`ApplyImageParameterBatch` validates again before staging only changed Text
Clarity/Super Resolution controls and invoking their native handler once;
custom recipes apply as one complete object. `ApplyImageParameter` records
independent component writes without partial recipe or refinement mutations.
Recipe validation includes its selected built-in ID before any refinement
handler runs. Commit failure uses the service's original complete snapshot for
rollback, including effects applied during commit rather than only the later
per-field loop.

`AppControlTarget` routes finite image/view, Assistant/notes, UI/drawing, and capture-configuration parameters through typed helpers. `PrepareCaptureParameters`, `QueryCaptureParameters`, `CaptureParameterFields`, and `ApplyCaptureParameter` own the five camera controls. Separate `PrepareRecordingParameters`, `QueryRecordingParameters`, `RecordingParameterFields`, `ApplyRecordingParameter`, `ExecuteRecordingParameters` and `RecordingParameterOperationStatus` own recording canvas and installed microphone handles. A live microphone change requires exact recording-session approval and a recording grant; it stops only that session and applies preferences only after terminal completion, with deadline, source/context, field revision and native-value fences. Native combo availability is checked before the irreversible stop. A canceled or superseded operation never applies preferences later or restarts a recording. Output destinations use private `dir-` handles for Documents/current roots and at most eight roots selected through the native owner picker. `RememberOutputRoot` never accepts model paths. `OkuFlowApp::SetUserDataRoot` and `SetRecordingCanvasMode` share the actual native transitions; root runtime finalization is deferred until the immutable mutation receipt is secured. Changed output roots refuse native periodic-scene mode to prevent implicit egress rearming. Grouped camera selection, format and acceleration use asynchronous native transition operations; requested and acknowledged torch state remain distinct. Hardware acceptance is separate from this development routing.

`ProcessingSettings.controlFrame` receives the exact submission identity in the production pipeline. `PresentedFrame` accepts completed GPU temporal diagnostics only when source, processing, view, geometry, sequence, and monotonic freshness match the presented frame. Its bounded diagnostic provenance identifies unavailable or mismatched samples; unknown held/converging/keystone values remain unknown. CPU output reports its own known temporal state.

Failed synchronous field application rolls the normalized field set back before any settings save, restoring the prior revision/baseline/journal accounting. Undo uses the same native batch seam; custom recipes and their independent component revisions form an all-or-skip inverse cluster when newer native edits exist. Ordinary native batches preserve the selected preset identity unless the plan explicitly changes it.

The preference planner normalizes speech rate to the native slider's 0.01 steps
and classifies complete Unicode codepoints, including supplementary invisible
format controls. It rejects invisible/bidi format controls in single-line identifiers; multi-line
instructions preserve LRM/RLM. `AssistantPreferenceValueInContract` identifies
legacy native values outside finite bounds. `BoundAssistantPreferenceRead`
marks an oversized legacy value that cannot fit even one projection as
`valueOutOfContract:true`, `requiresUi:true`, `unsupportedReason:value_out_of_range`
and `filterRequired:false`, without truncating or modifying stored editor text.
Aggregate response packing preserves this UI requirement even when other finite
values must also be omitted for the response budget.

Native periodic scene intent is preserved by conservative whole-patch guards:
while it is enabled, actor overlay false-to-true and changed runtime preferences
(including notes destination) are denied. Scoped Scene remains independently
supported under core frame budgets. Preference finalization uses overlay-only
presentation refresh, never `SetModes`. These guarded native-scene combinations
are not claimed to support all settings changes.

Scoped Codex vision opts into `SetRestrictedLaunchMode`: asynchronous pinned
CLI version/features/MCP-inventory preflight delegates to existing
`CodexAgentProfile::Validate`. Every inventoried MCP server and reviewed
apps/plugins/shell/web feature is disabled before the additional child starts.
Inventory/diagnostic secrets are discarded; initialization remains ordinary
and non-experimental. Ordinary Read/Explain/Assistant launch is unchanged.

The development control service exposes `PendingApproval.normalizedChanges` using finite registry parameter IDs, including coupled writes and excluding the internal custom-recipe bookkeeping field. Native display labels and the recording-stop side effect remain separate from authorization. Camera source activation validates the pending intent generation and retires earlier frame authority; a native camera selection supersedes pending actor startup and waits for an exact recording stop. Camera format handles belong to their enumerated device, and torch writes bind the actual active phone source rather than a requested fallback identity. These development paths require fresh compilation and separate hardware acceptance before release promotion.

`UserDataPaths::ValidateRoot(..., verifyWritable=false)` checks canonical containment without creating directories or probing writes during agent approval preparation. Native application uses the default writable validation only after authority is established. A private temporary-directory regression checks both the side-effect-free preparation and the accepted writable path.

`FieldWrite::retryUnacknowledged` is a trusted native-plan marker, never a model argument. It permits retrying a phone torch request whose requested value already matches but whose device acknowledgment differs. It participates in exact approval revalidation and bypasses requested-value no-op suppression; the bounded token/deadline and ACK-only effective state remain authoritative. A core regression verifies execution and rejection when the approved retry semantics change.

Grouped mutation receipts and approval summaries convert native upscaler/polarity indices to the registry's finite string tokens; legacy control receipts retain their established representation. Pre-effect result-budget validation uses canonical registry names and values, preventing a short internal field name from underestimating the final grouped response. The core fixture checks wire enum receipts against actual integer native state.

Failed mutation rollback reports `rolledBack=false` if a native inverse cannot be applied. The service then invalidates the affected field revisions/baselines and removes their prior undo authority instead of pretending the old state was restored. No partial settings save occurs; further deliberate mutation requires a fresh current-field read.

### Native UI and annotation control helpers

`AppControlTarget::PlanUiParameterDependencies`, `PrepareUiParameters`,
`QueryUiParameters`, `UiParameterFields`, `ApplyUiParameter` and `UiParameterField`
cover nine UI and eight annotation fields through the actual native transitions.
They validate the complete finite group before effects and retain unrelated
state. `MainWindow::preferredAdvancedPanelWidth`, `advancedActiveTab` and
`setAdvancedActiveTab` expose the native inspector state without opening it.
`AnnotationOverlay::SelectedTool` and public `SetTool` preserve selected tools.
The optional tool argument on `MainWindow::setAnnotationMode` and `retainInk`
on `AnnotationOverlay::SetActive` preserve ink for controlled toggles; ordinary
manual exit retains its existing reset behavior. Capture compositors include
ink only when annotation mode is active.

`ProcessingSettings::TextStageActivity` and `TextClarityStages` make each text
refinement depend on both the Text Clarity master and its stored child flag.
The CUDA dispatcher consumes that result rather than enabling children merely
because the master is on. A 1,024-combination settings contract covers all flags.

`AgentControlPanel::SetPendingApproval` retains the Details document, caret,
selection and scroll position across countdown/event refreshes when the exact
review text is unchanged. Changed contents replace the document; a new approval
identity also resets and collapses the review. It displays canonical `normalizedChanges`
and the native `targetDisplay` as plain text, including implicit dependencies,
while preserving the original service argument binding. Oversized review details
fail closed. No provider text or chat reply grants authority.

`ControlToolDestructiveHint(ToolId) noexcept` supplies conservative advisory MCP
metadata: reads, Observe and additive photo/recording starts are non-destructive;
replacements, resets, recording stops and grouped setters are marked destructive.
It does not change schemas, grants, dispatch or native authority.

### Native control decision and commit boundaries

`IAppControlTarget::SynchronizeNativeState` polls production fields before dispatch,
approval resolution, delayed preference commit and Stop/Undo. The native target
delegates to `NoticeNativeChanges`; a qualified acceleration notification updates
only its own observed field. Unrelated manual changes remain visible to the next
decision. `ReadField` resolves device-qualified preferences, and `EffectiveValues`
uses native group getters rather than equating requested preferences with output.

`BeginFieldPlan` performs validation only; `CommitFieldPlan` stages coupled native
writes after the service captures the inverse ledger. Failed commit/apply restores
the prior values without saving a partial plan, or invalidates inverse authority
when restoration fails. Processing/geometry provenance is retired after rollback
because restored preferences cannot recover consumed processing history.

`OwnedSourceRequested` advances both source and format revisions and retains their
owner-bound restoration journal. Stop/Undo reports asynchronous source restoration
as requested, rather than already presented. Camera operation status preserves
bounded opaque requested source/format, applied acceleration and actual first-frame
effective state. Immediate start refusal rolls back synchronous preferences while
the context/revisions still match; an accepted later hardware failure retains the
reported preference writes and undo journal. Saved startup source remains separate.

`NativeControlFixture` is an opt-in actual-app CPU/dev-full seam. It constructs real
native controls and settings in a temporary directory without platform/device or
provider bootstrap, and uses Qt offscreen. Its separate SDK evidence validates only
the executed CPU-native subset; unavailable CUDA/image writes and physical source
acceptance remain distinct. `native_control.cmake` clones the production target
sources under `OKUFLOW_ENABLE_NATIVE_CONTROL_FIXTURE`, disabled by default.
`FailingNativeCommit` forwards the fixture's reads and effects to the real target
but injects a failed commit after an actual native CPU zoom write. The production
service must restore the real fields and leave saved values and undo authority
unchanged; it does not replace native controls with a synthetic state map.
`NativeControlFixture::RequestTemporarySource` installs only private temporary
ownership bookkeeping, with no device enumeration/open/start. Its
`CancelAgentTurn`/`AgentContext` helpers exercise and inspect the production
trusted cancellation path. The regression checks source-restore intent, native
zoom restoration and the absence of a spurious user-edit event after cleanup.

### Android stream event ownership (PW-001)

- `camera/StreamSessionOwner.kt`: thread-confined `bind`, `accepts`, `streaming`,
  `start`, `stop`, `retire`, `restart` and terminal `close` own exact-host intent.
  Restart eligibility and `CameraEngine.start` admission share one serial event.
- `CameraStreamService.onOwner` posts socket and engine state/error callbacks to
  Android's main executor. `commandOnOwner` revalidates the command's immutable
  host token; settings, thermal changes and destruction use the same owner.
- `CameraEngine.start` and each `Run` carry `sessionId`; all state, metadata,
  encoded frame and error callbacks return that identity, including fallback.
- `UsbSocketServer` connect/command/disconnect callbacks include the original
  token and run outside its socket lock. `state`, `metadata`, `frame`, `error`,
  `hostCodecs`, `hostLimits` and `connected(id)` select only that host;
  `invalidateStream(id)` revokes its old frame generation at Stop.
- `StreamSessionOwnerTest` exercises held restart delivery after Stop, successor
  ownership, stale callbacks, immediate transport loss, shutdown and owner-thread
  enforcement. It is JVM lifecycle evidence, not phone/hardware acceptance.

### Required-capability test receipts (TB-002)

`scripts/run_test_gate.ps1` exposes `-CapabilityProfile` with development,
windows-cpu, windows-cuda and windows-cuda-kernels profiles.
`Get-CapabilityRequirements` declares the registered mandatory test names,
required labels and configured CUDA variant. `Get-CapabilityVerdict` evaluates
only freshly selected/fingerprinted records: only `Passed` satisfies a required
case; absence is `NotRun`, skip/partial/unresolved is `Blocked`, and assertions
remain `Failed`. It leaves original CTest results unchanged. Strict shipping
profiles reject filtered inventories and missing GUI registration. The ledger's
bounded `capabilityChecks` history records the separate profile verdict, shared
runtime and per-case fingerprints. `build_release_bundle.bat` selects the strict
profile before staging. `test_test_gate.ps1` uses pure policy contracts plus
actual command-line runner fixtures for whole-test and Qt-row skip failures.

`MainWindowTests::ultraClearTextControlsHaveNativeGeometry` is a serial native
fixture for the production UCT panel: independent opt-in while optical Text
Clarity is off, keyboard toggle/Tab order, read-only pane and Qt accessibility
screen bounds, foreground hit ownership and an unobstructed native capture.
It uses no camera/provider and does not establish real OCR/UIA word ranges.

`ProcessingSettings::ApplyTextClarityMaster` also neutralizes the effective
background-flatten strength whenever the master or flatten child is off. The
combined background/glare kernel then leaves ordinary pixels unchanged for
glare-only selection. Saved preferences retain their dormant strength.
`SettingsStoreTests::textClarityStageSelectionHonorsEveryRefinement` verifies
all 1024 master/child combinations; `GlareOnlyLeavesOrdinaryPixelsUnchanged`
checks ordinary/highlight GPU pixels, alpha and pitched guards for all four
flatten/glare combinations.

### UCT immutable handoff and processed-scene provenance

- `UctSceneSettings`, `UctSceneSnapshot` and `UctSceneProvenance` retain exact
  applied source/rotation/warp/LUT identity and frame on successful processing.
  `OnCudaSceneProcessed`, `OnCpuSceneProcessed`, `RequestFor`, `Context`,
  `InvalidateCuda` and `Reset` distinguish cached scenes from pending requests;
  CPU fallback claims no CUDA warps/colors. `UltraClearTextRequest` and
  `UltraClearTextContext` use this seam; `uctWarpRevision_` tracks discrete resets.
- `PixelBuffer`, `SharedPixels`, `PixelPool::{Create,Adopt,Copy,Take,Give,Wrap}`
  retain immutable readback storage across probe/OCR workers. GPU completion
  transfers the reusable vector after other consumers; CPU presentation copies.
  `WrapPixels` gives a read-only QImage view with shared lifetime.
- `PreparedWord`, `PreparedLine` and `PrepareLines` prepare scene boxes/reference
  patches on the recognition worker; the scheduler accepts owned shared pixels.
- `IsHighRiskToken` permits underscores and Windows path separators in file
  tokens with a one-to-sixteen-letter ASCII extension, while preserving the
  ordinary confusable and math rejection rules. `OverlayModel::AdmitImpl`
  retains current-validated geometry/identity through small exclusion-box
  jitter; it unions exclusions and preserves the original reference and clocks.
  Live/advancing detector omissions can also retain an existing shown object
  validated at/after the OCR job, provided neither prepared observations
  (including rejected spans) nor admitted replacements intersect it. This
  preserves the exact identity, reference and deadline; OCR never renews proof
  by omission. `ApplyChecks` still revokes genuinely changed regional pixels.
- `LineReference`, `LineCheck`, `CheckLines`, `ProbeRequest`, `ProbeOutcome`,
  `ProbeWorkerStats` and `ProbeWorker` implement budgeted off-thread probe
  construction, similarity and residual matching with latest-only admission.
- `UltraClearTextController::{SubmitProbe,HandleProbeOutcome,ProbeStatistics}`
  apply current-generation outcomes only. `OnSourceRetired` immediately revokes
  work and text; `OnPresentationFailed` hides words for rejected presentation.
  `LastOutcomeHandlingMs`, `LastResultHandlingMs` and `PixelAllocations` expose
  bounded fixture diagnostics. Full LUT generations are retained without hashes.
- `OverlayModel::{LineReferences,ApplyChecks,IsShown}` merge per-line outcomes
  without overwriting newer validation. `Admit` has a bounded residual budget,
  leaving unchecked lines hidden. `ReadingText(now,frame)` excludes stale/invalid
  lines and clusters rows before strict sorting. `RefreshOverlay` compares colors
  as well as content/geometry; `UltraClearTextOverlay::Words` exposes fixture data.
- Added UCT regressions exercise these production seams, 300 actual cached-glyph
  repaint frames during held OCR, source retirement without another present,
  mixed-height reading order across permutations and unchanged-word recoloring.
  They remain separate from native camera, UIA word ranges and frame latency.

`VisibleChatSessionStore::Write` commits its QSaveFile replacement before
retention pruning. A failed open/write/rename cannot delete any prior retained
conversation. `StoredBytes` measures committed storage; at most one additional
record (1 MiB, 65 total entries / 33 MiB) can remain committed if pruning fails.
`ErrorCode=retention_pending` distinguishes that successful commit from cleanup.
A later save retries due cleanup and refuses further growth while blocked;
`Reload` can read the bounded overflow. Successful pruning advances `Revision`
so a later failed write cannot hide committed history cleanup from observers.
Tests use exact Windows file locks to
force commit failure and cleanup failure without changing owner data/permissions.

`CodexAppServerClient::EnsureLaunchPermissions` chooses immutable per-child
coding/internet grants before admitting a turn. It retires any differently
privileged child first. Read/Explain always choose constrained offline launch,
including after coding-enabled persistent turns. Idle/account children start
constrained. Configure retires changed executable/security/workspace profiles,
retaining the old profile identity if retirement is incomplete so no new turn
can silently reuse wider authority. `restrictedStageFinished_` tracks only the
current preflight stage; the persistent finished handler resumes current work
after a canceled earlier preflight exits. `CodexAgentProfile::Validate` accepts
an explicit web-search grant for ordinary non-coding Assistant turns; it still
disables ambient MCP, shell/code/computer/plugin/hooks and local `view_image`
tools before app-server starts. Dedicated control/scoped vision callers retain
the default offline profile. Attached input images are independent of the local
file-image tool. Tests log child argv and exercise profile transitions.

`OkuFlowApp::StartCameraCapture` routes ordinary initial/reopen/format/reconnect
starts through `QueueInitialCameraStart`, retaining its revocable ingress,
worker tracker and generation checks. Only explicit legacy startup profiling
may use the old synchronous path. The private `cameraStartForTest_` seam replaces
driver activation in the native fixture, never in normal launches.
`DriveCameraReconnect` recreates the saved requested symbolic link on that worker
without UI-side device enumeration or premature mode reads. Completion owns
success/retry; acceptance does not announce a reconnect. Native fixture barriers
hold Open/Start while the UI pumps events and Stop revokes the exact worker.

`UltraClearTextTests::modelAdmissionBudgetNeverDisplaysUncheckedText` forces a
zero admission-matching budget and proves glyphs/reading text remain hidden
until independent probe checks validate the lines. The content-moved algorithm
case explicitly permits complete matching, so CPU scheduling cannot turn a
partial deferred result into an incorrect test failure. Production stays at
its 3 ms UI admission budget.

`MicrophoneWorkerState` retains cancellation, startup/retirement completion,
abandonment and error state independently of the app. The worker constructs the
activation from the endpoint ID in its own MTA and owns Open/reader/Flush/
Shutdown/final release. `OkuFlowApp::StartSelectedMicrophone` now returns
`MicrophoneStartResult` (Disabled/Pending/Failed); `CompleteMicrophoneStart`
checks exact worker, recording ID and callback generation. `StopMicrophoneCapture`
revokes callback authority and signals that worker without a native wait.
`MicrophoneTeardownUnsafe` gates global MF teardown through final release, not
only reader-loop exit. An unretired or abandoned worker prevents new microphone
acquisition. Private Start/Stop seams and a shortened fixture-only deadline
exercise held driver work and late success without touching a device.

`RecordingManager::SetSessionAudioPending` collects bounded audio while holding
segment admission; pending pre-start video frames are discarded. Its exact-ID
`SetSessionAudioResult` resolves readiness, wakes the worker and updates actual
audio only for an active matching session. Stop/retirement clears pending state;
late or foreign decisions cannot enable audio in a successor. The codec-free
fixture checks this production admission boundary and the native app fixture
checks UI events, retirement retention, None, timeout and successor identity.

`SettingsController::PromoteCurrentConfig` now returns an optional committed
preset. It assigns unique UUID-based config/preset identities, saves the staged
mode immediately and restores only its affected live vectors/selection/config
if publication fails. A post-commit credential-cleanup failure retains the
committed mode with LastError as a warning. `PromoteCurrentConfigToPreset`
refreshes the native list only after durable publication and displays storage
errors. Tests reopen immediately without shutdown save and force a failed
commit followed by retry while preserving prior mode/live settings.

Android host CI and `tests/test_impact.json` now include the imported
`scripts/android_camera_probe.py` dependency. `test_select_tests.ps1` exercises
a probe-only change: host protocol tests are selected without Windows targets.
Workflow/impact-manifest changes also select the CI protocol job.

The root preset file, included `cmake/CMakePresets.json` and root minimum
declare CMake 3.25, matching schema version 6. Portable 3.25.3 configures the
root project and parses both preset manifests. `cmake/` is an included CMake
implementation directory, not a standalone source root. A diagnostic attempt
to configure it directly was retained separately and is not passing evidence.

`lecture_camera.ps1::Wait-ObsCameraReady` polls the exact owned startup until its
output API is ready or the deadline/owner exit refuses it. `Invoke-ObsRequest`
retains typed ObsStatusCode; only NotReady (207) retries on a connected socket.
`test_lecture_camera.ps1` evaluates those actual functions with fake protocol
replies, testing convergence, permanent failure, exit and expiry without OBS.

`UltraClearTextOverlay::RebuildGlyphLayout` caches ink metrics and a robust
shared type size/baseline per accepted OCR line. `SceneWord::lineId` is carried
from the model through the controller; unlabeled words remain independent.
`ProjectedGlyphs` exposes the same cached layout reprojected for diagnostics.
Paint draws every original/replacement union plate before any sharp letters,
so adjacent plates cannot erase previously drawn overhangs. The offscreen
fixture checks capitals, ascenders and descenders across two independent lines
and pan/zoom, rather than matching UI wording.

`AppControlTarget::Execute(Wait)` snapshots the same enriched Operation Query
projection that subsequent polls compare. Deferred recording preferences no
longer satisfy operation_state merely because Query adds preference metadata.
The real-target offscreen fixture seeds a deferred tracker entry, verifies an
unchanged wait times out unmet, then verifies an actual completion satisfies it.
`NativeControlFixture` is the target's test-only friend for controlled ledger
setup; the wait and projection are unchanged production paths.

`AssistantChatView::RenderMessages` tags root heading/body blocks with stable
message identity (`ChatBlockIdentity`, `TagChatBlocks`). `CaptureChatAnchor`
and `RestoreChatAnchor` retain each selection endpoint's message, part and UTF-16
offset separately, preserving reverse selections and duplicate occurrences.
The viewport restores a semantic top anchor plus its pixel offset; it does not
search for the first matching selected string. Reflow remains root-level, so
existing Qt/UIA character geometry is preserved. A synthetic duplicate-message
regression changes earlier content and streams later content, checking exact
selection endpoints and direction rather than interface wording.

`AnnotationWriteResult` and `UserDataPaths::WriteAnnotationImage` give single
annotation PNGs collision-safe atomic publication. Encoding/flush completes in
an exclusive same-directory QTemporaryFile before atomic-only rename; existing
names are never replaced, including a racing destination. Collision retries
choose UUID suffixes. Failed/empty encodes clean up only that owned temporary,
reporting any cleanup leftovers. `SaveAnnotationSnapshot` records notes only
for the committed returned name. File fixtures preserve an open prior capture,
repeat a timestamp, race publication and simulate a failed partial encoder;
they do not claim a physical disk-full run or power-loss durability.

`SessionZoomIntent` owns desired camera zoom for an exact host/start token.
`CameraEngine::start` resets it once per start, while fallback candidates share
it. `setZoom(sessionId,ratio)` captures the admission token and rejects retired
or foreign runs on the camera handler. It stores early intent without creating
a repeating request until CameraDevice and capture session are ready; the
configured callback reads that intent. Pure JVM regressions verify readiness,
fallback retention and stale/foreign commands. This changes camera crop only;
no Android display size, density, aspect ratio, font, rotation or scaling is set.

`CameraSettingsState::update` is the serialized read/patch/persist/publish owner
with monotonically increasing `revision` for actual accepted changes. UI actions
and host commands now submit field transformations, evaluated against the latest
state inside that boundary; stale Compose snapshots cannot replace independent
host fields. `CameraSettingsStore` retains the existing preferences format and
session-only, never-persisted torch. `SetupViewModel::updateSettings` clears a
transient problem only after an actual accepted patch. Advanced/ModeSetting
callbacks carry just chosen values; Reset remains an explicit whole-state reset.
Same-field conflicts use last accepted patch; no-op patches do not restart the
stream. JVM fixtures cover both orders, a barrier-held concurrent persist, an
explicit same-field policy, failed publication and new-host torch reset.

`AppControlService::ApprovedOperationOutcome` publishes admission as pending,
reserving a bounded watcher before Execute. `PollOperationReceipts` uses public
Operation Query until committed/completed/first_frame, recording-start success
or failure; missing status becomes explicitly unavailable, never inferred success
or continued progress. Each terminal receipt is emitted once. Watchers outlive
turn/context retirement without granting authority. `PublishOperationReceipt`
keeps the original context/approval/operation identity in outcome-only human
notifications; it appends no event to a replacement context's tool-visible ring.
`OkuFlowApp::agentReceiptTickets_` retains only still-owned visible-chat tickets,
so a later outcome appends to its original retained turn rather than a new turn.
Switching/deleting the conversation invalidates its tickets and does not transfer
a receipt to another conversation. Service fixtures cover pendingâ†’commit/failure/
unknown and context replacement; the old coverage fixture's drop-all-human-
receipts expectation was reconciled while preserving its no-cross-context assertion.

Two new receipt strings have complete read-only Luna review of all 48 changed
entries, separate from native-speaker and rendered accessibility acceptance; see
`localization_review_2026_10_17.md`.


### Displayed-frame annotation leases (2026-10-17)

`OkuFlowApp::PendingAnnotationCapture` retains the displayed scene source/frame,
processing revision, viewport geometry and transform with its strokes.
`AnnotationFrameMatches` rejects a later source, frame, reprocessed scene, request,
viewport or pan/zoom transform. The GPU save requests a copy of the cached
already-displayed scene immediately in the same UI turn; a full ring or expired
readback fails closed instead of retrying on another frame. CPU saves copy the
last successfully presented viewport directly. `CancelAnnotationCaptures` revokes
queued/active IDs and the cached view before every camera-retirement early return.
`lastAnnotationPresentation_` is invalidated before mutating CPU viewport pixels
and is published only after presentation succeeds. Recognition permissions and OCR
results do not participate: the existing applied-scene provenance is reused only
as a spatial/source identity. The native offscreen fixture verifies changed frame,
source, effects revision, dimensions, transform and retirement rejection without a
camera, presenter, phone or provider.

## Recording fallback and timeline audit (NP-006 / NP-007)

`NewRecordingAttemptPath(const std::wstring&)` in `common/media_writer.hpp`
allocates a GUID sibling for another codec/input attempt. `VideoRecorder`
exclusively creates each sink byte stream, retains failed outputs, and exposes
`OutputPath()` for its chosen attempt, retained through `Stop()`.
`StartGpu` uses a fresh sibling when native sink initialization fails before
trying worker readback. `RecordingManager::StartSegment` similarly gives
H.264 fresh paths after either AV1 leg fails; `segmentProcessedPath_` and
`segmentOriginalPath_` carry the chosen paths to `FinalizeSegment` and notes.
No fallback deletes or reopens failed output. Private fixture callbacks permit
metadata-failure injection without changing production policy.

`ValidateDecodedAudio` and `ValidateDecodedVideoMarker` in the codec fixture
check distinguishable frequency/color markers against media PTS, including the
opt-in three-second-gap diagnostic. See
[recording attempt ownership and timeline contract](recording_attempts_and_timeline.md)
for the silence policy and the distinction between fake policy evidence and
native codec verification.

## Bundle publication and preflight helpers (RB-003 / RB-004)

`scripts/publish_release_bundle.ps1` validates a uniquely named sibling stage
and uses `Open-VerifiedBundle` to acquire temporary read leases, hash the exact
manifest inventory, reject owner/unknown content and exclude running/locked
runtime generations. `Switch-Bundle` closes verification leases (required by
Windows directory renaming), checks the inventory again, renames the prior
complete generation to a unique recovery path, publishes by same-parent rename,
and rehashes all published files. Rename/verification failure rolls names back;
recovery failure retains all remaining generations. Old generations are never
recursively deleted. `Assert-PlainPath`/`Get-PlainTree` reject reparse traversal,
and `Assert-NoRunningImage` observes process images without terminating any.
The `CreateStage` action checks the parent for reparse traversal and exclusively
creates a fresh GUID stage. The `DiscardStage` action accepts only that stage shape and
refuses owner data, reparse paths or running images. A per-dist native mutex
serializes publication; there is no copy/merge fallback.

`scripts/check_bundle_toolchain.ps1` checks an initialized VS2022 x64 tool
surface and compatible Ninja/Release cache without writes. The batch
`--check-toolchain` path also checks Qt deployment prerequisites and exits
before configure/build/staging. `test_bundle_publication.ps1` exercises exact
old/new inventories, DLL locks, a normally exiting hidden private console child,
owner data/path/junction refusals, and injected rename/verification rollback.
`test_bundle_toolchain.ps1` drives the real batch's positive and negative
preflights with private cache fixtures. These are scoped regressions rather
than full release evidence.

`test_bundle_publication.ps1 -CaseSet All|Exchange|Refusals|Rollback` retains
the same 13 synthetic scenarios. `Complete-Case` and `New-Case` emit named
BEGIN/PASS timing without contaminating returned fixture objects; each selected
set asserts its completed scenario count (13/3/7/3). CTest registers Exchange as
`bundle_publication_contract`, plus `bundle_publication_refusals` and
`bundle_publication_rollback`. All three retain the 60-second per-case limit,
process-identity resource lock and actual publisher/helper runtime inputs.
This splits a cumulative fixture timeout into focused results without changing
production transaction checks, failure injection or owner-data assertions.

`build_release_bundle.bat::resolve_qt` selects `QT_PREFIX`, `Qt6_DIR`,
`QT_ROOT_DIR`, then the local SDK default, preserving explicit-override failure.
It prints the selected path before checking real deployment tools, licenses and
module SPDX files. CTest's `bundle_publication.cmake` supplies its configured
SDK/source root and declares those actual notice inputs in `REQUIRED_FILES`.
The positive `Invoke-Preflight` case uses a private junction to that real SDK
with spaces in its path and asserts the selected prefix, so an installed local
default cannot conceal CI's relocated-SDK regression. It restores all changed
process environment variables. The workflow's existing pinned Qt action also
installs the matching `qtmultimedia` source archive; its source-aware cache key
does not restore the older runtime-only SDK inventory.

`resolve_qt` normalizes the matching source root before resolving license
inputs. After the SDK and online-installer license locations, it accepts the
module source's unchanged `qtmultimedia/LICENSES/LGPL-3.0-only.txt`, logs the
selected file, and fails when all candidates are absent. CTest mirrors that
selection and fingerprints the source LGPL notice as well as the FFmpeg notice.
The regression's relocated SDK exposes the actual SDK entries through private
junctions (root files are hash-verified copies), excluding only `LICENSES`.
Its positive case requires the selected license to be the matching source file,
so the owner's installed notices cannot mask the archive-layout failure. It
still performs exactly one real VS initialization within the 60-second limit.

## Exact recording identity (audit PW-004)

`NotesRecordingIdentity` runs only inside `AssistiveRuntime::AppendNoteBlock`'s
serial notes-storage closure. `NoteSectionMetadata::recordingPath` carries the
finalized Original path from `AppendNoteMediaPair` through
`AppendNoteHtmlSection`; no filename counterpart is inferred. The worker reads
at most 1 MiB at a time and verifies length/modification stability. It assigns a
UUID and publishes `<actual-original-name>.okuflow-media.json` by a complete
same-directory temporary file and a no-replace rename. A valid existing marker
retains its UUID; invalid markers or changed bytes are refused, never rewritten.
The immutable identity is escaped into `data-recording-identity` before the HTML
block is appended. Hashing and filesystem publication never run on the UI thread.

The marker contains version 1, `mediaId`, actual `name`, exact byte `size`,
`algorithm: sha256-chain-1m-v1`, and a lowercase 64-hex `fingerprint`. The chain
starts at 32 zero bytes; for each consecutive 1 MiB chunk (the last may be
shorter), its next state is SHA-256(previous state || chunk). This bounded-memory
fingerprint deliberately has its own algorithm name; it is not a whole-file
SHA-256 digest. Empty recordings have no identity. Native/Node/Python fixtures
share the fixed `fixture recording` vector
`2e0fa70301bb1ee4a5875308ff4df614d95578a0d19cf1788dce6a0c11aa64ba`.

The compiled bridge's `validIdentity`, `sameIdentity`, `fingerprint`,
`verifyRecording` and `recording` require the exact marker plus a fresh byte
fingerprint for the granted directory's resolved file. Native UUID, filename,
size, algorithm and fingerprint bind the File handoff and every returned message.
`saveTranscriptSnapshot` revalidates the current file before its serialized write;
`transcriptName` addresses the target as `.okuflow-transcript-<mediaId>.json`
(61 ASCII characters, independent of video basename). An older writer held at
`createWritable` can therefore only publish to its old UUID's file after the
recording path acquires new bytes, marker and transcript. Version-2 sidecars
restore only against the complete expected identity. Legacy basename-only
`<video>.okuflow-transcript.json`, size/mtime-only version-1 sidecars and old
notes without identity are refused; legacy files are not migrated or modified.
The browser can still play existing media and choose a file directly in Transcribe.

The sibling website validates declaration shape in `recordingIdentity`, then
`verifyRecordingFile` checks bounded slices before result reuse, cache/model work
or decoding. Those awaits use the existing job-cancellation fence. `notify`
echoes the immutable identity; `resultKey` binds notes results to UUID/fingerprint
and isolates standalone selections by job. `render_notes_preview.py` mirrors the
identity with `recording_identity`, including atomic no-replace marker publication;
changed preview bytes require a fresh output folder. No cryptographic library or
runtime dependency is added: Qt, WebCrypto and Python's standard library are used.

## Performance measurement helpers (NP-010 / PF-001)

- `MaxineSuperRes::Worker::Execute` retains its existing synchronous one-second
  deadline. With `OKUFLOW_MAXINE_PROFILE=1`, optional `CallTiming` atomics survive
  a late task and `Profile::Record/Report` summarize the last 16,384 stage/outcome
  samples on destruction. Adapter-caller wait, worker dispatch and completed
  SDK host duration are separate; timeout/throw samples do not certify GPU
  completion. In production that caller is the independent producer worker,
  not the Qt/presentation thread.
- `tests/assistant_chat_benchmark.cpp` is an excluded, non-CTest diagnostic.
  `Snapshot/Delta` generate bounded pixel-free chat state and optional original
  JPEGs; `Distribution` uses nearest rank; `ControlledWaits` exercises the
  production deadline helper only. `main` forces Qt offscreen and measures
  production `AssistantChatView::SetChatSnapshot`, CPU paint, queued callback
  delay, selection identity/direction and viewport-anchor displacement.
- `DeadlineWorkerTests::busyWorkerRejectsRatherThanQueueing` uses owned promises
  and a private worker to prove that an overlapping task is rejected and never
  runs. It creates no GUI, CUDA context or third-party SDK call.

See [performance diagnostic scope and commands](performance_diagnostics.md).

`AssistantChatView::TryRenderStreamingAppend` handles only append-only,
nonempty streaming assistant text in an already maximum-width final bubble.
`RenderContext` compares palette/font/locale/layout/viewport width and translated
headings; identical prefix and all other snapshot/message fields are required.
The helper replaces only the escaped final root body, preserving its format and
`ChatBlockIdentity`; it clears stale `renderedHtml_`. All other edits call
`RenderMessages`. Both paths use `CaptureChatReadingState` and
`RestoreChatReadingState` for message-scoped endpoints and viewport anchors.
`AgentControlPanelTests::streamingTailMatchesFullRenderAndFallback` compares
incremental text/paragraph geometry against fresh full renders and retains
selection, immediate/deferred prefix-mutation, fallbacks and accessible-character
assertions. Tail replacement temporarily disables layout with a scope-guarded
restore; queued viewport resize callbacks rebuild only for a different final
render context. The excluded benchmark's `DiagnosticApplication::notify` can
record bounded inclusive slow-event timing with `--trace-events`, separately
from ordinary A/B evidence.

`AnnotationGpuPresentationMatches` additionally compares the actual selected GPU
texture, SR route, source extents, remapped presentation transform and focus-marker
options. Disabling SR can select different pixels without advancing the ordinary
scene revision; these output signatures prevent that same-frame substitution.


### PCM timeline continuity after native AAC verification
`RecordingPcmSpan` and `RecordingPcmTimeline` in `recording_contract.hpp` plan
integer silence/overlap/data spans without mutation; `Advance` commits only
accepted PCM frames and `TimestampAfter` converts the absolute cursor without
rounded-duration drift. Ten-second maximum holes and checked conversions reject
huge/overflowing work. `VideoRecorder::ValidAudioInput`, `CanAcceptAudioBlock`
and `WriteAudioBlock` separate input validation, per-block AAC admission and
submission. `AddAudioFrame` streams fixed-size silence blocks under one 250 ms
repair-admission deadline, trims covered overlaps and advances only after
successful submission. Start/StartGpu reset the cursor per segment.

The native marked post-repair diagnostic submitted 288,000 PCM frames for the
three-second-gap case and placed decoded audio/video markers at 3.999333 /
3.999967 seconds, respectively. Normal submissions remained 144,000 frames.
This verifies the synthetic codec timeline seam; physical microphone and full
release acceptance remain separate.

### Maxine explicit runtime retirement and private lifecycle fixture (2026-10-17)

`MaxineSuperRes::Impl::Teardown` re-arms its runtime-probe latch after unloading
the effect and DLLs, allowing `Ensure â†’ Teardown â†’ Ensure` on the same adapter.
`MaxineSuperRes::retired_` starts true, becomes false before an actual model-load
dispatch, and becomes true only after a completed worker teardown. Repeated
explicit teardown and destruction of a known retired adapter dispatch no further
SDK task. Worker faults still prevent teardown and preserve the existing full
`Impl` quarantine; retirement never cancels or releases a timed-out call. Every
dispatched lifecycle operation retains the adapter's originally selected device.

`maxine_lifecycle_tests` compiles the production adapter and loads privately built
`maxine_fake_image` / `maxine_fake_effects` DLLs through the existing explicit
runtime-directory override. `MaxineFakeSnapshot` and `OkuFlowFake*` exports exist
only in those test DLLs. They control a failed model load and a gated late frame,
and count effect/image ownership, thread/device identity and dispatch profiles.
`maxine_lifecycle_cuda` covers reload, idempotent retirement, partial-failure
recovery and timeout/destructor/late-completion retention using a real CUDA device
without a proprietary model, camera or native window. Fake image storage and
transfers deliberately validate lifecycle/ABI behavior, not GPU image quality or
actual Maxine performance. See [fixture commands](../tests/README.md).

`EnsureTimingEventPair` in `cuda/timing_event_pair.hpp` constructs both timing
events before replacing either caller-owned handle. Failed creation retires new
partial resources and preserves previous ownership; successful adoption retires
any previous partial pair exactly once. Complete pairs are reused. Production
callbacks retain the exact CUDA allocation error, while `cuda_surface_retry`
injects second-allocation failure and recovery without a GPU.

`MaxineSuperRes::Impl::Run` checks input transfer, synchronous effect execution
and output transfer in order. A failure preserves its exact stage and numeric
`NvCV_Status` in `LastError`, then returns before any later stage; an ordinary SDK
status retains the existing optional-failure policy rather than becoming a worker
fault. The private fixture fails each stage once, verifies downstream calls were
skipped, then processes successfully with the same loaded effect and image owner.

`maxine_navigation_tests` drives the real `CudaInteropSurface` using a private
matching-LUID D3D12 device/shared textures, compiled production CUDA/NIS kernels
and CPU input frames. Its harness copies completed private textures to a bounded
readback buffer without a native window/presenter. The private fake-image
`OkuFlowFakeFillOutput` control is off by default; it can enqueue one known
32-bit pattern on the supplied CUDA stream, allowing exact neural-versus-current
fallback checks. The fixture checks deferral before model load, fallback after a
new input, disabled SR, idle model reuse and balanced teardown. Original driver,
SDK-model quality, native navigation performance and external-presenter fence
acceptance remain separate from this scoped fixture.

### Bounded bundle toolchain preflight (CI follow-up, 2026-10-17)

`check_bundle_toolchain.ps1 -ConfigurationOnly` validates Ninja selection,
generator arguments and existing cache identity before any VS shell startup.
Its success explicitly leaves toolchain/Qt readiness unchecked. The batch still
runs the real installed VS2022 x64 initializer on every valid invocation and
then performs the complete helper/Qt preflight; it never trusts an inherited
`VSCMD_VER` or architecture claim as a shortcut. Before initialization it clears
`VSINSTALLDIR`, which Microsoft's script otherwise preserves. The complete
helper independently queries the installed VS2022 C++ roots with Installer
`vswhere.exe`, verifies the selected root, and requires the active compiler's
exact `VC/Tools/MSVC/<version>/bin/Hostx64/x64/cl.exe` layout under that root.
An arbitrary ancestor path or a different PATH compiler cannot certify it.

`build_release_bundle.bat::initialize_toolchain` selects the registered root
with `vswhere -property installationPath`, then uses its fixed
`Common7/Tools/VsDevCmd.bat` location instead of recursively enumerating the
installation. Within the batch's `setlocal` environment it disables only the
initializer's auxiliary telemetry process with `VSCMD_SKIP_SENDTELEMETRY=1`;
the real initializer, registered compiler check and Qt checks still execute.
`test_bundle_toolchain.ps1::Invoke-Preflight` tees child output live into CTest
and an individual case log, preserving the last setup stage if CTest times out.

The fixture preserves all generator/cache and no-mutation assertions while
performing exactly one real VS initialization. Configuration refusals assert
that the initializer was never reached. In-process negative checks use forged
environment declarations with the real installed discovery tool, without fake
VS scripts, executable replacements, compilation or GUI. Named case logs and
elapsed progress expose the stage reached if a deadline is hit. The existing
60-second CTest limit is unchanged. This removes redundant setup identified
after the remote 60.15-second timeout; runtime/CI verification remains separate
from static review of the repair.

### Argon 54 private regression seams

`CaptureReadbackTestAccess::StartReader`, `SetPendingReadAge`, `PendingRead`
and `SetFrameRate` drive `CaptureLoop` through `TickThenBlockedReader` without
a camera. Its first STREAMTICK returns; the next read blocks on an owned gate.
`CaptureShutdownTests` checks low-rate grace, one timeout signal, nonblocking
poll/stop, explicit cancellation, phone exclusion and late-read suppression.
No fixture mutates the owner's camera or clock. `OutboundQueueTest` exercises
the same production Kotlin queue and control/video factories under saturation,
reserved-capacity, FIFO, concurrent producers and close/in-flight transitions.

### Bounded camera-read recovery follow-up

`CompleteCameraCaptureStart` adopts the reader but does not complete physical
camera recovery. `RecordStartupFirstPresent` retires pending recovery on the successful
CPU/CUDA presentation hook, before its one-time startup logging early return.
It announces reconnection only for Reconnect-origin video; working manual or
fallback video silently retires the superseded recovery owner. `BeginCameraReconnect` retires an active
zero-frame failed attempt without resetting the original window;
`DriveCameraReconnect` checks the physical source's 30-second expiry before due,
startup or active-reader guards and never reopens an adopted reader every tick.
Expiry cancels reconnect-owned pending startup and retires an adopted attempt.
Phone retry ownership remains separate. `RunFrameTick` routes every consumed
loss through this state machine, including failures during recovery.

`NativeControlFixture::CameraActive`, `DriveReconnect`, `ExpireReconnect` and
`PresentedCameraFrame`, `ReconnectDue` and `LoseActiveReconnect` exercise adoption, successful presentation acknowledgment
and expiry without a camera or native presenter. The orchestrator regression
separately checks that retries cannot renew the original deadline. These hooks
validate control flow, not actual hardware or optical delivery.

### Independent Maxine producer and presentation polling (2026-10-17)

`common/maxine_async_producer.hpp/.cpp` defines `MaxineAsyncProducer`,
`Ticket`, `Config`, `Output`, `Statistics` and its private `State` phase machine.
`Request` overwrites a CPU descriptor; `TrySubmit` snapshots a full-scene CUDA
array ROI onto private pitched input and records input-ready without waiting.
`Run` owns allocation, exact configuration/strength setup, SDK calls, private
completion timing and cleanup on one independent worker. `TryTake` exposes an
exclusive output lease only for the full current ticket; stale output exposes
only timing/config and retires without pixel publication. `ReleaseOutput`
records a consumer event; reconfiguration/free wait on that event off UI.
`Invalidate(false)` revokes authority without defeating ordinary failure
latches. Default strong invalidation also permits a failed configuration to
retry, including a failure completing after the reset; a worker/unknown-completion
fault remains sticky. Destruction requests
stop without joining; `State::quarantined` retains the entire unsafe closure
without allocating. `HasWork`, `IsFaulted`, `LastError` and `Stats` return CPU
state only. No app, source texture or presenter pointer is retained by the SDK.

`ProcessingInput` carries unconditional `cameraEpoch`, `frameSequence` and
`settingsGeneration`. `CudaInteropSurface::AsyncSuperResState` binds those to a
processing serial, request generation, exact scale/strength and ROI.
`UpdateSuperResCache` only requests private preparation; current-frame spatial
fallback remains available. `RefreshSuperRes` polls readiness, snapshots input
or publishes an exact result under the existing external fence reservation.
`SuperResRefreshResult` distinguishes no work, submission, publication and
uncertain shared work. `HasSuperResWork` selects actual work; `NeedsSuperResPolling`
also covers model preparation and in-flight inference. `SuperResStatistics`
returns bounded job/replacement/stale counters and accepted-result age.
`AccumulateSuperResTiming` includes completed stale jobs for the matching
configuration in the existing guard. Reset no longer drains a private SDK call.
The former shared output scratch/timing-event ownership has been removed.
`InvalidateSuperResRequest` revokes pending/active neural authority without
model teardown or performance-guard reset. Native
`SyncCurrentConfigToPersistence` advances `OkuFlowApp::cudaProcessingGeneration_`
and calls this revocation before optional agent synchronization or suspended
persistence tracking. `RunCudaPipeline` stamps that generation unconditionally.

`OkuFlowApp::RefreshAsyncSuperRes` runs on the existing presentation callback,
reserving/committing/canceling exact shared-fence values. Without a semaphore,
it polls graphics before writing and polls CUDA before graphics reads the new
cache. `asyncSuperResPresentPending_` retains that completion requirement.
Publication increments annotation authority and marks scene output dirty without
starting a user-navigation tail. `PipelineOrchestrator::Callbacks::hasAsyncSceneWork`
keeps readiness polling at the effective display rate between slow camera frames,
without reprocessing the camera or manufacturing navigation input.
`UctGeometryKey::neuralEnhancement` revokes glyphs/held recognition across a
base/spatial-to-neural transition of the same frame. Presented observation
metadata names the selected enhancement, source/frame and cache generation.
Immediate photos/recordings keep exact current-frame pairing and do not wait
for neural output.

`maxine_async_tests` drives private real-CUDA/fake-SDK snapshot, all-ticket-field
rejection, one-slot/coalescing, consumer lease, invalidated shutdown and failed
configuration retry cases. Fake `OkuFlowFakeEchoInput`/`inputFirstPixel` test
pixel identity rather than recognition quality. The surface fixture imports a
real shared D3D12 fence and verifies exact fallback/neural output. The scheduler
fixture checks 120-Hz polling between 5-FPS camera frames, restoring idle policy
without a motion tail; the UCT fixture rejects held recognition after enhancement
changes. These are background/offscreen fixtures, not native optical/pan evidence.
The async fixture's `Gate::State` is heap-owned and retained with its handles
if callback completion is unknown, including a callback that has not entered.
`NativeControlFixture::CurrentImageConfig`, `RestoreImageConfig`,
`ProcessingGeneration` and `NativeContrastWithoutAgent` verify generation
advancement and restore isolated settings with the agent target absent.

### UCT confirmed presentation and ordered omissions

`UltraClearTextController::OnPresented` is submission preflight;
`OnPresentationSucceeded` is the explicit presenter receipt. The latter owns
the watchdog clock and restores cached unchanged-scene text after a failure or
600 ms presentation timeout. `PresentationAvailable` gates both glyphs and the
reading pane, including control-toggle and worker-completion paths. Show original
alone leaves verified reading text available. Production CUDA/CPU presenters and
the UCT fixture report success separately from preflight.

`OnPresentationDeferred` distinguishes a non-faulted busy frame slot from a
failed presentation. `attemptedTransform_` is staged by `OnPresented`;
`presentedTransform_` and `presentedGeometryGeneration_` commit only on success.
`attemptedFrameSeq_` commits to `currentFrameSeq_` only on success as well.
`attemptedViewport_`/`presentedViewport_` refuse deferred reuse across a resize
or invalid transform, even if the presenter's retry flag remains set.
Deferral reverts to that exact same-generation projection without renewing the
watchdog; actual faults, source/geometry changes and expiry keep their guards.

`uct::AcceptedLine::readingTokens` retains source token order separately from
its drawable accepted words; empty tokens represent omitted original words.
`OverlayModel::ReadingText` emits those gaps in place and collapses consecutive
omissions. Controller timeout/failure fixtures and the data-driven omission
fixture cover these boundaries.

### UCT replacement eligibility and reading-position refresh

`UltraClearTextOverlay::SceneGlyph` caches reference ink, the reference font's
fitted geometry and readable colors at content/style changes. `referenceFont_`
is reused during paint. `ReplacementIsDrawable` makes one projected-size,
validity and clipping decision consumed by both background and glyph passes;
an ineligible glyph cannot leave an opaque plate over the original camera word.
The existing per-line baseline and coordinate contract are preserved.

`UltraClearTextPanel::SetReadingText` remaps both selection endpoints through
unchanged prefix/suffix text via `RemapReadingOffset`, clamps replaced-middle
positions and keeps them outside UTF-16 surrogate pairs. It restores vertical
and horizontal scroll after the cursor. Recognition never waits for pane focus.
Data-driven fixtures cover missing glyph/plate pairs, forward/reversed selection,
caret, prepended/deleted content, shorter replacements and supplementary text.

`MainWindowTests::ultraClearTextOverlayFollowsNativeViewport` exercises the real
native overlay with known synthetic words, Fill-mode pan/zoom, resize, geometry,
paint receipts and complete composed captures. It does not open a camera or
verify OCR accuracy. The Windows OCR fixture additionally checks both text
polarities against an opposite-brightness surround through the real backend.

### UCT independent-review matching and lifecycle repairs

`MatchPatch` separates coarse stride-3 ranking from dense acceptance, keeps
the measured zero-displacement incumbent, and checks visibility/score again
after combined subpixel refinement. Coarse proposals still seed their own local
neighborhood search. Invalid border samples cannot determine interpolation.
No NCC threshold or search radius is relaxed.

`OverlayModel::Admit` retires cached outside-coverage lines whose words are
spatially replaced by a cropped recognition, preventing duplicate resurrection
after returning to the wider view. Near-identical refreshes retain the entire old
line only when its text/reading tokens agree, boxes differ by at most 10% of
word height (IoU >= 0.8), and its existing proof is still fresh in the same scene;
a fresher validated candidate wins. The old reference, boxes, id and validation
timestamp remain intact and each retained id is consumed once; the new reference
never inherits that proof. Different text stays hidden for worker validation.
`LineReferences` orders unverified lines first, then oldest validation, to avoid
fixed-order starvation. `ApplyChecks` leaves existing proof unchanged on a
missed check budget; `IsShown` still applies the original 350 ms freshness limit.

`MeasureLineColors` uses the bounded surrounding margin to distinguish paper
from dense foreground and takes the background-class median of that ring;
minority-ink inference remains the no-margin fallback. `StableLineColors` snaps
background brightness to 64 fixed levels while retaining hue, and keeps previous
colors inside an 8-channel-level same-polarity hysteresis band.
Both polarities are checked through the actual scheduler with controlled dense
strokes, independently of recognition quality.

`RecognizedWord::paddedBounds` marks Paddle detector/CTC regions; `inkSearchPx`
allows a one-CTC-step extension inside the line and neighbouring-word midpoints.
The scheduler
uses worker-only `RefineWordInkBounds` to fit bounded (about 16K samples) source
ink before `PrepareLines`. Row runs isolate the dominant word body; detached
marks are retained for matching dotted/accented tokens. Low-contrast/tiny or
ambiguous multi-band fits retain their detector box. Native Windows OCR boxes
are unmarked. Overlay `ReadableColors` preserves source background and polarity,
snaps neutral ink to black/white (readable chromatic ink keeps its hue),
and binary-searches only the minimum background adjustment still needed for 7:1.
Both operations occur outside the pan/zoom paint path.

`UltraClearTextController::SetEnabled` retries terminal initialization on an
explicit off/on using a fresh recognizer. `schedulerGeneration_` rejects queued
status/result callbacks from a predecessor scheduler; ordinary slow recognition
does not cause a restart. Runtime-error and initialization-error retry remain
separate paths. Two initialization-state rows exercise actual recreation.

`slowOcrNeverBlocksPresentationClock` establishes a held OCR job before the
300-presentation loop. Asynchronous worker startup under parallel test load
cannot shorten that loop; the original progress/time assertions remain.

### OCR device recovery and deterministic region selection

`AnalysisCaptureHealth` distinguishes clean retryable resource pressure from
unknown-completion quarantine; `CanAttempt` implements the 250 ms backoff and
`Failure`/`Success` retain visible health until recovery. `AnalysisPool::HasFailure`
and `CudaInteropSurface::UctAnalysisFailed` project this owner-thread state to
`UltraClearTextController::OnAnalysisHealth`. Probe and recognition input fault
counters remain separate from backend failure, so a good probe cannot mask a
repeated OCR input fault. Candidate admission serializes both device worker hops
and times out without joining. `HandleResult` uses `OverlayModel::AdmitLiveCamera`
for live captures and `AdmitAdvancingMedia` for playing files, with the newest
independent probe; no same-frame stability check claims to detect motion.

`AutomaticRecognizer` records Uninitialized/Ready/Retired per candidate, preserving
ready fallback sessions and lifetime retirement across asynchronous GPU promotion.
`TensorRtRecognizer::RecognizeDevice` preserves engines for pre-enqueue input
faults. `Network` validates the actual bytes before SDK parsing. GPU `boxes`,
`countBoxes`, `boxOffsets`, `emitBoxes` compact valid component roots in raster
order, using label scratch for block offsets after connectivity is consumed.
`eligibleRank` computes deterministic intra-block ranks without CPU map readback.

`scripts/run_test_gate.ps1 -RecheckAfterRepair '<diagnosed repair>'` explicitly
rechecks blockers once, reuses matching passes, and archives the repair reason
and original blocker identity on each new attempt. It refuses Resume and
RetryInterrupted combinations; this is never an automatic failed-test retry.
The private test-gate fixture covers unchanged test bytes after a repaired
external prerequisite, ordinary retry refusal, exact one-time recheck, unaffected
pass reuse and retention of the original logs.

`UltraClearTextController::AnalysisHealthy` gates both painted glyphs and reading
text, so recoverable input failure status always corresponds to original pixels.
Health recovery preserves model state but still requires normal age/proof checks.

`OnCpuViewport` clears CUDA pool failure only after acquiring valid current CPU
pixels, so acceleration fallback cannot leave a healthy CPU path latched off.

`AnalysisAllocation::{Automatic,Legacy}` selects capability-tested stream-ordered
or legacy snapshot/reduced-buffer allocation. The choice is cached per device;
reader caches also bind allocation kind. Legacy cleanup stays on retirement
workers. `gpu::Component` uses integer count and signed 64-bit moment sums;
`axes` evaluates covariance in double. Both `boxes` and `emitBoxes` use the same
PCA orientation rule. The oriented fixture compares exact corner order on
horizontal, vertical and positive/negative tilted large components.

`RecognitionScheduler` snapshots `WantsDeviceInput` once for each job and no
longer accepts a redundant previous-reference argument on `SubmitDevice` or
publishes unused `RecognitionResult::stability`. Input provenance remains.
`State::uploadPool` caches CPU-presentation snapshots for TensorRT; proven clean
upload resource faults do not retire the engines.

Repair rechecks require exact named matching Failed/Unfinished cases; a failed
recheck cannot repeat unchanged. Results retain `repairRecheckOf` and `repairReason`;
capability records retain `repairRechecks`, and summaries identify repaired passes.

### GPU playback lifetime and exact selected-frame observations

`VideoGpuIncompatible` identifies native GPU device/format/conversion failures;
`VideoGpuBackpressure` identifies temporary consumer-held allocation credit.
Only the former may select the same-document CPU compatibility retry.
`VideoGpuDecoder::AttachSourceReader`, `DetachSourceReader`, `HasUnsafePending`,
`PollPending`, `DrainPending`, `ClearPending` and `MarkUnsafe` preserve the complete
reader/sample/view/output/query owner before GPU submission. A never-ended
query is not polled; unproven completion retains the entire converter until
process exit. Native calls remain on the one playback worker.

`VideoReadStreamClone`, returned optionally by `OpenStableVideoReadStream` and
`RecordingArchive::OpenReadStream`, creates independent stream cursors over the
same held file backing and archive validation policy. GPU-to-CPU replacement
cannot share a still-prefetching reader's cursor or reopen an unrelated path.
`VideoPlaybackSnapshot::gpuDecodedBitsPerComponent` records the accepted decoded
subtype's precision before display conversion.

`D3D12Presenter::KeepSubmittedFence` retains a producer fence before queuing its
Wait. `SubmittedSource<T>::lastRead` is assigned only by a real global graphics
submission. `RetireCompletedSourceReferences` transfers proven-completed source
textures and fences to the single bounded `sourceRetirement_` worker. Unknown
completion follows presenter quarantine, preserving both native object types.

`AppControlTarget::PresentedMediaFrame` constructs immutable observation authority
from the submitted candidate's exact frame sequence and media PTS. The ordinary
selected-document identity and control stamp remain source-scoped; the previous
`doc.presented` cursor is never attached to newly submitted pixels. CPU media
calls this only after a successful viewport receipt. `QueueAnnotationSnapshot`
regenerates the actual selected frame before reading its annotation authority;
Read/Explain and non-agent image attachments do the same in their click turn.
`QueryVideoState` and `State` report `gpu_selected_media` or `cpu_selected_media`
from the displayed path. `AppControlTarget::SelectedMediaImageFieldAvailable`
separates the basic CPU-rendering whitelist from all supported GPU image effects;
Debug remains camera-only. Writable capability comes from
`SelectedMediaGpuProcessingAvailable`, independently of the actual decoder, while
effective values require an actual processed media receipt. Preparing effects use
`no_processed_frame`; unsupported effects use `selected_media_unsupported_effect`.

### Complete selected-video processing and transport repairs

`OkuFlowApp::BuildProcessingSettings` is shared by live-camera and selected-video
submission. It maps the same effective Text Clarity, blur/sharpen, color,
auto-contrast, tracking, temporal and neural preferences; the hidden recording
camera still requests only clean Original conversion. `ProcessingCacheExtent`
bounds the media spatial/neural cache while preserving aspect ratio.
`MediaAppearanceSignature` hashes explicit effect members without padding,
host pointers, frame clocks or view-only geometry. `mediaGpuViewRecipe_` tracks
ROI/navigation separately, preserving UCT's full-scene appearance authority.

`SetMediaOverlaySuppressed` revokes media OCR jobs, glyph/pane text and UIA
exposure when processed-view correspondence is unprovable, while retaining
the user's enable preference. Job admission and late callbacks honor the gate;
clearing media removes it even when no immutable input remains owned.

`VideoGpuDecoder::DecoderPreferredOutputs` probes a bounded merit-ordered native
decoder's offered types for the exact coded input and device without streaming.
`CodedBitsPerComponent` uses bounded container records/profile evidence before
opportunistic negotiation. GPU incompatibility can start one fresh CPU-attempt
budget; it cannot extend or restart the failed GPU attempt indefinitely.

`CudaInteropSurface::D3D11SourceStatus`, `D3D11SourceStatistics`,
`EnableAsyncD3D11SourcePreparation`, `PrepareD3D11Source`, `SourceStatistics`,
`D3D11PrepareSlot`, `BuildD3D11Import`, `AdoptPreparedImport`, `PollImportCopy`,
`PollAllImportCopies` and `DestroyD3D11Import` separate bounded source preparation,
eight-entry reuse, exact copy leases and worker-only native import retirement.
`MediaGpuScene::SourceStatistics` exposes evidence of worker imports and reuse.

`VideoTransport::IsActionReady`, `SetFocusReturnTarget` and its focus/readiness
helpers preserve semantic focus while a transiently unavailable action is inert.
`SeekBaseline` and `CommitSeek` use current/pending document time for relative
keys, wheel and accessible increments, independently of the frozen inspection
thumb. `AnnounceSettledState` coalesces only settled states and their current time.
`MainWindow::eventFilter` checks action readiness for Space on the render host.
Icon faces retain translated tooltips and accessible names; recording hides duplicate Return/Mute,
and file Return remains last in the tab order.

`VideoDocumentState::controlsMediaGpuAvailable` refreshes native enabled states
when media processing capability changes without changing documents. New file or
own-recording rebindings clear the prior document's display receipt and completed
request; offscreen decode alone cannot authorize image actions for the new file.
`openingDifferentVideoCannotInheritPreviousDisplayReceipt` covers this boundary.

`PresentedMediaFrame` also accepts the candidate's `gpuProcessed` flag. A first
GPU frame cannot inherit CPU diagnostic certainty from the previous display;
missing/mismatched private-scene diagnostics remain unknown until proven.

`MediaGpuScene::Prepare` accepts optional bounded cache extents. `BaseTexture`,
`SuperResTexture`, `SuperResWidth/Height`, `SuperResRoi`, `IsSuperResActive`,
`IsSuperResPerformanceLimited`, `SuperResStatus`, `SetSuperResPerformanceOverride`,
`NeedsSuperResPolling`, `RefreshSuperRes` and `SuperResStatistics` expose the private
media enhancement lifecycle. Publication advances `SceneGeneration` and the
private producer fence; a paused scene polls this independently of decode.
`GetKeystoneTrackingState`, `SetKeystoneTrackingPaused`, `StepKeystoneCorrection`
and `ControlDiagnostics` route controls to the selected scene rather than the
hidden camera. Successful steps force exact-frame replay. `HistoryResetCount`
and `UctCoordinatesReliable` describe processing discontinuity and single-frame
OCR correspondence, without promising real model quality.

`ProcessingInput::reprocessFrame`, `CudaInteropSurface::EnableFrameReprocessing`,
`FrameReprocessingState`, `ResetFrameHistory` and `RefreshUctAnalysis` preserve
post-geometry pixels and histories from before the current immutable sample.
Forward samples advance tracking/history; same-sample appearance/ROI updates
replay without registering or blending that sample again. Analysis-only retry
uses the owned geometry snapshot and closes its private-fence submission even
when the analysis pool is busy. `EffectAndHistoryTests` and
`SpatialAndNeuralTests` inspect actual GPU pixels, replay/reset, paused keystone
step, private fake-SDK publication, retained models and obsolete tickets.
`AwaitOwnedWorkers` is console-fixture shutdown only: it keeps the static CUDA
runtime loaded until the fixture's own detached host workers finish.

The reprocessing snapshot also retains the pre-frame UCT EMA metadata/history
and the completed neutral smoothed pixels. Replay reconstructs the same blend;
`RefreshUctAnalysis` publishes the saved smoothed/current pair without advancing
history. A paused UCT re-enable instead reseeds a one-frame pair under the reset
epoch and starts the next forward blend from that held frame. Text Clarity edits
preserve neutral UCT history. `MediaGpuScene::TakeUctAnalysis` checks pair provenance and exact scene
identity, and `UctCoordinatesReliable` permits Temporal Smooth and exact held
Extra Stable images while refusing neural-coordinate mismatches. The
media CUDA fixture checks actual paired pixels, replay and seek reseeding;
`gpuPairedCaptureKeepsSmoothedOcrAndCurrentValidationApart` checks exact halves,
  raw fingerprints and two-slot pressure.
  It also rejects mismatching OCR/current pixels even when the raw patch remains
  trackable, and accepts exact paired pixels. `optionalRealGpu` exercises normal
  upload, owned single-frame and paired input through real pinned TensorRT graphs,
  including resident input-registration booleans; it remains explicit opt-in.

`SelectedMediaDocumentPts100ns` adds the current recording part's document offset
before producing observation authority; UCT and decoder internals keep local PTS.
`SelectedMediaGpuSceneProcessed` preserves a matching analysis lease across a
view replay, including new allocations for the same sample. It rejects foreign epoch/frame/appearance leases before
storing them, and spends retries only when their deadline is due.
Correspondence recovery resets the bounded analysis budget. Media keystone and
processing/SR status refresh on a 100-ms cadence, including paused polling.
`ExpireAnnotationReadback` is shared by live processing and media polling; a
missing ring receipt or one-second expiry retires that exact annotation intent
without substituting a later frame. Save uses the displayed processing backend,
independently of whether decode supplied CPU bytes or a resident GPU texture.

`ProcessingInput::rotateBgraOnGpu` is opt-in for unrotated host video BGRA;
camera BGRA keeps its existing pre-rotated contract. `PrepareMediaBgraInput`
checks post-rotation extent and pinned-ring/stream credit before allocating or
queuing work. CPU input validates actual row pitch and last-row minimum bytes;
weak exact storage identity plus layout/type prevents false same-frame reuse
across CPU/GPU transitions. `MediaGpuScene::Process` admits only one in-flight
processing pass, leaving newer ROI/source requests pending rather than queuing
obsolete work. `ResetSuperRes` and `ResetTextClarityHistory` revoke pending neural
output on explicit native edits without joining workers. The latter also clears
the saved pre-frame text-mask validity so replay cannot resurrect old settings.


`validUctLeaseSurvivesViewReplaysAndRetryStaysBounded` checks 125 replay polls,
exact backoff boundaries, immediate rejection of mismatched incoming OCR leases
and recovery in the real app wiring. `selectedMediaPollingExpiresPendingAnnotationReadbackWithoutRetargeting`
uses the document timer to expire a genuinely active annotation intent without
requiring a camera or retargeting later pixels. `ActionRowsProblem` inventories
the production corner actions (now five, with Select video in Camera settings), while preserving its
rectangle, equal-row-width and minimum-height assertions.

`MediaGpuScene(unsigned outputBuffers = 1)` accepts one or two outputs. The
experimental two-output `OutputSlot` rotates base/cache textures and a private
fence timeline while preserving one `CudaInteropSurface`, processing history,
source-import cache and inference worker. `Bundle::SelectOutput` swaps only
prepared output bindings. Both reader completions are proven during retirement;
Pending admission never advances history or publishes another texture's stamp.
`Release()` gives a nonblocking exact-owner completion receipt for explicit
resource release, unlike cache-preserving `Invalidate()`.

`CudaInteropSurface::AlternateOutput`, `ConfigureMediaOutputBuffer`,
`SwapMediaOutputBindings` and `SelectMediaOutputBuffer` prepare matching native
resources on the construction worker and swap their imported surfaces/semaphore
only with an idle producer. Quarantine retains the entire alternate owner too.
Ordinary camera/app construction does not allocate or select an alternate.

`media_gpu_buffer_benchmark.hpp` supplies `DoubleOutputTests` and the explicit
`BufferBenchmark` entry point. `VideoGpuPresenterFixture::Initialize` uses the
existing friend seam to create production viewport commands/shaders without an
HWND. `AwaitProducer` verifies independent output completion; `Percentile`
reduces local timing samples. Existing effect/history and spatial/neural cases
accept an output count and validate both configurations. Timed comparisons stay
outside recurring CTest and do not claim end-to-end player/monitor latency.


### Camera settings video source (2026-10-19)

`MainWindow::setCameraDevices(names, selectedCamera)` rebuilds device labels and
appends the tagged, translated View video option without emitting a source change.
It preserves a selected video across discovery refreshes. `videoSourceSelected()`
checks the item's data tag rather than a translated label or a camera index.
`setVideoSourceSelected(selected, cameraIndex)` silently synchronizes the source
with playback adoption, opening, failure, cancellation and return. The private
`UpdateCameraSourceUi()` shows Select video below the dropdown and hides camera
format/torch controls while leaving Orientation available. Keyboard order follows
the inspector layout; only the five capture/assistive buttons enter corner geometry.

`PopulateCameraCombo()` delegates to that device-list seam; automatic phone and
fallback index updates preserve a video choice. `OnCameraSelectionChanged()` routes
the video entry to the existing local picker and a manual camera entry back to
live capture. `InitializeVideoPlayback()` re-synchronizes the source on picker
cancel, and `PollVideoPlayback()` reflects the current document without opening a
dialog. `SetRotationQuarterTurns()` and `SavePersistentSettings()` retain
`CameraOriginalRotationQuarterTurns()` as the saved camera preference during file
review, while selected media continues using the current orientation recipe.

The offscreen native fixture adds `videoSourceControlsBelongToCameraSettings`
(placement, five-action separation, source visibility, keyboard order and silent
refresh/reselection) and `selectedVideoOrientationPreservesCameraPreferenceAndIdentity`
(actual native file adoption, quarter/half/three-quarter rotation through the
native control handler, paused cursor/document identity and camera restoration).
`NativeControlFixture::Window`, `VideoOrientation`, `VideoRecipeOrientation`,
`CameraOrientation`, `SavedOrientation` and `ReturnVideo` expose only these
hardware-free production seams; they do not claim actual GPU display or UIA.


### Steady-camera UCT acquisition evidence (2026-10-19)

`SourceStamp::liveCameraCapture` is a trailing, default-false provenance flag.
`UltraClearTextController::MakeStamp` sets it only from live camera contexts;
`ImmutableStamp` and selected/advancing media keep it false. The scheduler retains
the flag through owned input, probe and completion metadata. Recognition jobs
from live capture and all validation-only jobs skip `SourcePixelDigest`; saved
and unknown CPU OCR inputs still hash exact active pixel rows, excluding padding.
GPU saved-input fingerprints use the resident tree algorithm documented below.
This removes live-camera hashing; device color/reference and residual preparation
also remain on the GPU, with no image download in the native GPU path.

`OverlayModel::AdmitLiveCamera` enters the capture-specific `AdmitImpl` mode.
New pending words need the same source/geometry and scheduler authority, two
matching recognitions, and a strictly later capture-owned sample sequence with
both stamps marked live. Byte-identical acquisitions may corroborate; repeated
jobs/timestamps on the same sample may not. `Reset` and admission-mode changes
clear pending/accepted evidence, preventing a saved frame from borrowing camera
proof. Saved/unknown `Admit`/`AdmitAdvancingMedia` and immutable-neighbor checks
retain full-hash duplicate protection. Freshness, risky-token filtering and
pixel-residual registration still apply to both paths.

`liveCameraIdenticalPixelsAcquireText` uses five offscreen controller rows:
CPU camera, device camera before/after video, and resident camera before/after video. They
first replay one unchanged sample (no glyphs), then deliver new acquisition
identities with exactly the same pixels (glyphs/reading text). The original
three CPU/device rows failed against the pre-fix controller; the two additional
resident-oracle rows cover the new GPU-analysis protocol. The model's
`liveCaptureAcceptsIdenticalPixelsOnlyFromNewAcquisitions` checks capture mode,
replays, older samples, source/geometry/authority changes and media-mode fencing.
`schedulerPixelEvidenceIgnoresAllocationPadding` additionally proves live input
produces no digest while reference pixels remain available, and default inputs
still produce the exact SHA-256 independent of allocation padding.


### GPU-resident UCT analysis (2026-10-19)

`DeviceImage::HasResidentAnalysis` / `PrepareResidentAnalysis` distinguish native
CUDA metadata preparation from an explicit CPU image implementation. The scheduler
uses them for device OCR and independent validation, failing input preparation
rather than downloading after a resident error. CPU inference retains its one
worker-side `Readback`. `RecognitionResult::DigestKind` distinguishes active-row
SHA-256 from the versioned GPU SHA-256 tree; pending/immutable evidence requires
matching algorithms. `CorroborationRequest` snapshots the selected job ID,
reference descriptors and `MatchTuning` in `SubmitDevice`; `SubmitImmutableIfNeeded`
supplies them. `corroborationMatches`, `corroboratedJobId` and `corroborationTuning`
carry the worker's result. Resident admission requires that exact selected job,
line count and model policy, alongside neighbour provenance/content checks.

`ProbeFrame::resident`, `Width` and `Height` support a CPU image/integral or an
independent `ResidentProbe`. `ReferencePatch::resident` holds an opaque `ResidentPatch`
without host feature samples. `ResidentProbe::{Reference,Match,Similarity}` are worker
operations. Batch `References`/`Matches` keep compatibility defaults for test
oracles; native CUDA overrides use one launch/fence per bounded group (at most
1024 lines). `PrepareLines`, `CheckLines` and neighbour corroboration use these
batches on OCR/probe workers. `CheckLines` uses 64-line GPU chunks and checks
budget/cancellation between them, preserving completed results and leaving the
remaining lines unchecked; CPU checks retain their per-line time budget.
CPU/GPU search radius is clamped to 1â€“10 probe pixels. UI `MatchPatch` never
executes a resident operation. `AdmitImpl` defers
newer resident checks, preserves unchanged validated objects, and uses prepared
neighbour lines/results rather than building references on the UI thread.
`ProbeWorker::State` signals exit; destruction revokes callbacks and joins only
within a bounded wait, leaving held work with its state and inputs.

`src/cuda/uct_device_analysis.hpp` is private to `uct_analysis_pool.cpp`:
`resident::Prepare` builds independent GPU integral probes, batches line colors and
word ink fitting, and computes complete saved-input fingerprints. Normal consumer
cancellation quarantines unfinished work but does not permanently retire its
immutable probe; CUDA failures still fail closed. `Probe` implements
the resident interface; `Patch` owns a normalized GPU descriptor with an offset
into a shared batch allocation. `Mapping`, `ViewArea`
and `Project` preserve the CPU coordinate/filter contract. `Worker`, `Upload`,
`CopyResult` and `Finish` reuse private streams and pinned metadata staging with
completion fences. `Context::Pause` uses a private Windows high-resolution
one-shot timer after a few cooperative polls, avoiding a coarse sleep per line
without changing process/system timer resolution. `Quarantine` retains unknown work. `Allocate` uses stream-ordered
allocation on capable devices; `RetirementQueue::{Retire,Run}` release known-complete
allocations on one private worker, with uncertain failures retained.

`src/cuda/uct_validation_kernels.cuh`, compiled by `uct_kernels.cu`, implements
`ValidationIntegral`, `Reference`/`References`, `MatchReference`/`MatchReferences`,
`Similarity`, `LineColors`,
`InkBounds`, `FingerprintLeaves` and `FingerprintParents`. Color polarity/surround
medians, dominant ink bands/detached marks, dense/coarse registration and subpixel
refinement follow CPU policy. `referenceBody` and `matchBody` are shared by the
single-line and batched kernel entry points. All GPU-to-host transfers in this module contain
only output records (flags, colors, fitted boxes, scores, digests), never images,
integrals, patches or model feature tensors.

The `uct_paddle_cuda` fixture adds guarded resident scheduling, CPU metadata and
registration parity, complete fingerprint and capture-slot lifetime cases. Its
opt-in `optionalRealGpu` row additionally runs final resident metadata/validation
on real TensorRT OCR output. Controller resident-oracle rows and
`residentSelectedNeighbourUsesWorkerRegistration` check asynchronous integration;
these CPU oracles do not stand in for the separate production CUDA kernel checks.
`ReferenceRequest`/`MatchRequest` contain only small geometry/device-pointer records;
output compaction retains original line indices, including invalid middle entries.
`residentDensePageLatencyExperiment` is an explicit opt-in 60-line comparison of
worker preparation and validation; ordinary CTest does not run a timing benchmark.


### Video-picker and CI review closure (2026-10-19)

`PollVideoPlayback` compares the source-combo intent with the actual viewed/opening
source before reconciling it. A changed combo now refreshes `UpdatePhoneLightUi`
even when the selected media/backend did not change, restoring the phone-only
control after cancelled or failed opens without showing it for a webcam.
`NativeControlFixture::CancelVideoPickerRestoresPhoneLight` and
`cancelledVideoPickerRestoresOnlyPhoneLight` exercise this with fixture camera
metadata only, without opening a picker or connecting to a device.

`MainWindow::UpdateSimpleChromeGeometry` decides the complete row/face layout
before resizing any native peer. `ChromeGridCell`/`ChromeGridSize` measure grid
spans with spacer items and the actual layout margins, spacing and widget
borders. `ChromeActionButton::SizeHintForFace` caches style-equivalent hidden
measurement-button hints for Beside/Under/Icon faces; text, font, icon presence/dimensions,
DPR and style/font changes invalidate the relevant measurements. Its
`changeEvent` preserves normal QPushButton event handling while clearing stale
measurements. Each action row retains its `QHBoxLayout`, equal translated
minimums and equal stretch while its row count stays unchanged. The outer grid
owns one or two rows; replacing layout items preserves button/focus identity.
The left carousel folds first, followed by a three-plus-two action group at
tighter widths. Offscreen `translatedActionRowsHaveUniformCellMinimums`
and its three language data rows use the real Segoe UI font and exercise
corner-to-compact, narrow and return transitions, group bounds/disjointness,
button targets, tooltips and retained accessible names. The native
`bottomChromeFormsRectangularGroups` case requires responsive one/two action rows and
independent groups instead of full-width stacked bands. The opt-in
`agent_control_capture --chrome-output` branch composes the actual offscreen
MainWindow peers at 1440, 1100, 960 and 720 px, with no device/provider bootstrap;
it is development layout evidence, not native focus/UIA or camera acceptance.

`bottomChromeRetainsNativeSizeDuringRefresh_data` supplies wide, folded-mode
and folded-both native layouts in LTR/RTL. `bottomChromeRetainsNativeSizeDuringRefresh`
observes all synchronous peer Resize events across repeated activation,
small resizes away from wrapping thresholds and immediate native owner movement.
It rejects even temporary expand/contract geometry, rather than checking only
the final rectangle after the flicker. The original implementation fails all
four folded rows and passes both wide rows; original evidence is retained.

`residentBatchesRespectBudgetAndPreserveCompletedChecks` covers 1100 references,
partial budget progress and cancellation. `gpuResidentBatchMappingMatchesDistinctCpuLines`
compares distinct unchanged, occluded and moved lines, an invalid middle entry
and a CPU descriptor against sequential CPU results. Kernel coarse search uses
the CPU's zero-centred fallback when no coarse candidate qualifies.

`Harness::MakeSharedTexture`, `MakeSharedFence` and `ReadTexture(TextureSync)`
in `media_gpu_scene_tests.cpp` provide private production-compatible targets
and an explicitly fenced readback. `ReadScene` wraps that helper while preserving
the scene's presentation reservation/consumption bookkeeping.
`OriginalTextureAdvancesDuringExtraStableHoldTests` drives the actual camera
surface through sharp-reference acquisition, blur hold, recovery crossfade and
Live. It compares each frame's Original texture to current raw input exactly,
and the held display to the stored pixels of its named acquisition. Its private
bundle proves idle before destruction and retains unknown GPU work on failure.

The complete `immutable_uct_contract` Qt suite has a 120-second aggregate CTest
deadline after archived CI evidence showed 31 completed cases before its old
60-second limit. Individual QTRY/assertion limits and all rows remain intact.

### Rejected held-acquisition retry (2026-10-20)

`UltraClearTextController::HandleProbeOutcome` binds `lastOcrJob_` to the exact
submission that set the acquisition marker. `HandleResult`'s local
`retryUnreadAcquisition` clears that marker only for the same job when an OCR
result fails, expires, carries unusable composite pixels or loses admission.
Accepted, pending and empty results retain held-image deduplication; old
completions cannot reset the marker of a newer submission.
`heldAcquisitionRetriesRejectedOcr` uses actual device-probe/controller/scheduler
wiring with synthetic `SceneDeviceImage` hold records. It drains warm-up jobs
and checks the blocked image's checksum before testing three rejection paths,
then requires additional held text and no further OCR for that acquisition.
See [the independent review](uct_opus_review_2026_10_20.md) for evidence and limits.

### Compact transport and detail OCR (2026-10-20)

`VideoTransport::ApplyFace`, `ApplyButtonFaces`, `SecondaryButtons` and
`UpdateLayout` keep 44-DIP icon actions beside the scrubber, with secondary-row
fallback. `FocusTargets` interleaves the timeline in visual order; `ApplyFace`
caches resource/palette/state identity instead of rasterizing on every snapshot.
`RefreshTimeline` reserves the actual duration format width. Tooltip and native
accessible names keep complete translated actions.

`PaddleDetectionRegions` returns complete-scene context plus at most 16 detail
rectangles. `PaddleRecognitionWindows` and `gpu::RecognitionWindows` retain a
whole detected line and add up to eight overlapping recognition windows.
`PaddleQuad::internalEdges` and `gpu::Quad::internalEdges` bind left/right cut
provenance to CTC decoding; cut-edge fragments cannot become complete words.
`PaddleMergeRecognition` maps bounds/search/protected rectangles to full-scene
coordinates, reconciles duplicates and retains stronger evidence. GPU images
remain worker-owned strided subviews; detector profiles/models are unchanged.
Row association now compares a nearby individual word's type size, vertical
position and gap; preferred rows must qualify too. A contained literal
prefix/suffix fragment can overlap at 65% (ordinary duplicates retain 70%),
with the original confidence/candidate precedence and 1024-line cap. See
[coverage and stable placement](uct_coverage_stability_repair.md).
`VideoPlayback::kSeekSampleLimit` is a 4096-read malformed-stream guard.
The private `detail::VideoPlaybackSeekBudget::{Deadline,Expired,Observe}`
maintains a two-second no-progress window under an absolute sixty-second
request cap. Only strictly increasing decoded video PTS renews it; late,
absent, repeated or backward samples cannot. Open/seek and audio-aligned Resume
use the same guard and per-read generation checks. `MarkNativeOperation`
refreshes watchdog timing on real progress without publishing a frame.
`Reader::resumeClockPending` rebases host playback after successful preroll and
invalidates the old audio clock. `SeekReader` resets this state and PCM floors;
`QueueAudio` keeps `audioEnd100ns` as a handoff-independent high-water, so
`TakeAudio` cannot trigger oversupply while video waits. Output hard caps stay
unchanged. See [video/audio repair](video_audio_preroll_repair.md).

`RecordingArchive::State::provenDurationHighWater` advances only with parsed,
producer-fenced fragment proof. `Finalized` restores that monotonic mapping
metadata after finalizer backwrites/SetLength, while revoking generation, sample
counts and readable/seekable ranges as before. It cannot authorize old bytes.
`FinalizationRetainsRevokedMapping` checks this independently of parser timing.

### UCT per-word colors and backend readiness (2026-10-20)

`RecognizedWord::colors`, `PreparedWord::colors` and `AcceptedWord::colors`
carry optional local `LineColors` without splitting semantic lines or changing
validation references. The CPU scheduler and resident metadata worker measure
each word before ink fitting. `OverlayModel::SceneWords` uses the local pair
when present, preserving legacy line fallback and stable-color hysteresis.
`BackendStatus::usesGpu` describes the selected recognizer. The panel's
`SetBackendStatus` keeps its visible backend label and accessible name current.
`WindowsOcrRecognizer::Status` retains its initialized CPU identity for runtime
publication, so a preparation bridge cannot leave a stale GPU label.

`AutomaticRecognizer` accepts an optional preparation-bridge candidate index.
Production uses Windows OCR while TensorRT prepares, deferring configured CPU
sessions until necessary. A complete bridge result can set `backendPreparing`
to request preferred-backend refinement without bypassing evidence admission.
`Libraries::PrepareBuilder` lazily loads compilation-only TensorRT resources;
`TensorRtRecognizer` groups eight batch-one submissions per worker stream wait.

`UltraClearTextController::ScheduleImmutableRetry` retries unsuccessful selected
or neighbour work under the same ticket with bounded timers. New geometry revokes
old callbacks; successful final one-shot analysis is not periodically repeated.
See [color/readiness repair](uct_color_startup_repair.md) for limits and tests.

### Text Clarity master and saved UCT intent (2026-10-20)

`PersistentSettings::ultraClearTextEnabled` is the shared saved opt-in (missing or
invalid JSON defaults off). `UltraClearTextController::SetEnabled` records user
intent; `SetTextClarityEnabled` gates it and `ApplyEnabled` owns effective worker,
glyph and UIA lifecycle. `IsEnabledByUser` is saved, while `IsEnabled` reports
runtime use. The panel mirrors retained intent while its master is off, disabling
the opt-in/comparison controls. Its setters update dependent state even under
signal blocking. App initialization/settings restoration and Text Clarity handlers
keep this gate current; quick-mode changes do not overwrite shared UCT intent.
The corner switch uses first-party on/off SVG indicators on the existing native
QCheckBox, preserving focus, Space activation and checked accessibility state.

### Bounded TensorRT recognition microbatches (2026-10-20)

`Network` caches its current shape and tensor bindings, accepts recognizer
batch dimensions up to eight/sixteen and bounds checked output products before
allocation. Exact wider-profile shapes extend recognizer cache identity while
detector and legacy plans stay compatible. `State::recognizerBatch` controls
input rectification, inference output offsets and distinct compact metadata
slots; preparation can fall back through sixteen/eight/one. `TensorRtProfile`
owns bounded opt-in CUDA events with the inference state and emits completed
stage/call/allocation diagnostics. `uct_batch_test::CompareTensorRtBatches`
retains finite same-input full-output comparisons separately from normal CTest.
The automatic capacity remains one pending wider-profile acceptance. A profiling
exception clears successful output before backend retirement, keeping a failed
diagnostic from publishing successful text.
`CreateTensorRtRecognizer` accepts an optional `TensorRtRetirementObserver` that
receives the exact `AsyncCleanup` receipt without changing cleanup or quarantine.
The callback is absent in production. The cached `optionalTensorRtRetirement`
fixture waits in its isolated offscreen process for those closures before exit;
nested analysis storage and vendor workers remain outside that receipt's scope.
`uct_batch_test::RetirementReceipts` retains/polls these exact receipts after
scheduler destruction, including early fixture assertion exits. The batch
diagnostic defaults to medium/medium, supports the existing small/small selector
and independent detector/recognizer tier overrides, and records both immutable
model pins before comparing each pair with its own batch one.
See [GPU batching](uct_gpu_batching.md) for ceilings and evidence boundaries.

### Paused scrolling text corroboration (2026-10-20)

Both `RecognitionScheduler::Submit` CPU overloads accept `CorroborationRequest`
like the device overload. The immutable controller shares the selected result's
prepared lines and decimated reference with its neighbour job. Worker-local
`MatchImmutableWords` relocates bounded word reference descriptors to recognized
neighbour positions and returns `RecognitionResult::corroborationWordMatches`;
resident samples never leave GPU memory. It does not move selected-frame glyphs.
`OverlayModel::AdmitImpl` checks the exact receipt job/policy/shape and consumes
completed line/word checks independently of its inline CPU match budget. Empty,
foreign, changed, low-confidence and missing neighbour evidence remain refused.
See [immutable UCT](uct_immutable_controller.md) for bounds and ownership.

`TensorRtProfile::CropWidth` records bounded useful-width bins from existing
quad metadata; `Wait` attributes the existing detector-count and recognizer-group
host waits. They add no synchronization or transfer. Reports include width sums,
padding fraction and separate host-wait intervals only when profiling is enabled.
`CropDescriptor` hashes the already-copied ordered crop descriptors using an
explicit versioned little-endian encoding, excluding C++ padding and image bytes.
This diagnostic-only fingerprint can establish equal detector crop work across
recognizer/batch comparisons; it does not hash pixels on the runtime path.
See [performance stages](uct_performance_roadmap_2026_10_20.md).

`uct_character_test::Run` is an unregistered finite CPU/resident diagnostic for
the existing matcher, covering changed/unchanged tokens at three sizes and three
decimations with synthetic translation controls. `optionalCharacterChangeCpu`
and `optionalCharacterChangeGpu` supply actual production probe implementations.
Full per-case output and false-accept/false-hide totals are retained without
converting diagnostic completion into a passing reuse-quality gate. See
[character verification](uct_character_verification.md).

`uct_resident_test::ResidentBaselineDiagnostic` enters the actual
`RecognitionScheduler::SubmitDevice` path through a metered, owned AnalysisPool
image, with upload/capture setup outside steady timing. Bounded seeded model/batch
rounds preserve complete outputs, independent resident-analysis timing, cached
plan identities and exact TensorRT State receipts. The model-free
`SelectionAndComparisonSelfTest` is part of `uct_paddle_processing`; the actual
GPU experiment is unregistered and explicit. `CacheInventory` hashes every file
afresh and tests metadata-preserving mutation. `TimedOutput` retains an ordered
crop fingerprint; counts alone cannot certify equal detector work. See
[resident baseline](uct_resident_baseline.md).

Within that private test module, `Selection` / `PlanBlocks` bound and seed the
experiment. `ProfileCapture` forwards Qt logs while retaining completed profile
JSON; `MeteredImage` forwards the real owned device input and counts any image
egress. `Validate` refuses fallback, nonresident, missing-fingerprint or truncated
trace results. `Run::Capture` completes each owned analysis copy before steady
timing. `StrictComparisons` preserves the preselected output gate and adds known
crop-identity equality; `MatchByOccurrence` and `TruthJson` are diagnostic only.
`Stats` retains run order and withholds p95 below 20 samples. `ResultJson` records
source/job/geometry/temporal authority with the full neural output.


### Batch-eight development policy and frozen diagnostic (2026-10-20)

`ModelSettings::gpuRecognizer` retains Medium, with medium detection.
`DefaultTensorRtBatchCapacity` chooses eight for Medium/Small and one for Tiny;
`TensorRtRecognizer` preserves explicit 1/8/16 overrides and preparation fallback.
Saved model selections retain authority. `TensorRtCropDiagnostic`, optionally
passed to `CreateTensorRtRecognizer`, retains at most four exact owned image
leases and the compact regions/quads already returned by detection. Its private
`TensorRtCropDiagnosticAccess` binds replay to device pointer, extent, stride,
source/frame/processing/history identity and detector hash. Replay uploads only
compact descriptors and uses existing group completion waits. No automatic
backend constructs it, and it cannot cache/reuse text or authorize admission.
The private `detail::FrozenUploadRetirement` (`src/common/uct_frozen_upload.hpp`)
tracks a successfully queued descriptor H2D until `Completed()` follows the
existing successful group wait. `Cancel()` transfers the exact entire State
through `RetireState`/`AsyncCleanup` while the upload is pending: its owned input
lease, pinned descriptor/token storage and engines survive until completion or
the existing failed-cleanup quarantine. The caller retains State until the
retirement handoff succeeds. A zero-crop/no-upload region or cancellation after
the completed first group releases only `inFlight`, keeping healthy engines.
Later regions re-arm the guard only after their own successful descriptor upload.
The standalone headless `uct_frozen_upload_contract` exercises that actual
ownership decision with a gated native-completion substitute, covering all
three lifetimes, subsequent-job isolation and failed handoff. It executes no
CUDA/TensorRT/cache/model operations and does not certify real driver timing.
`detail::RetireOwnedState` is the shared final, nonthrowing retirement policy
used by `TensorRtRecognizer::Retire()` and `Preparation::~Preparation()`.
It keeps the reusable State owner until `RetireState` accepts an actual async
cleanup receipt. If allocation/handoff throws or refuses ownership, it assigns
the existing owner to State's pre-existing `retirementQuarantine` shared pointer
before clearing reusable ownership. That noexcept shared-pointer copy allocates
no new control block and retains the complete State/input/pinned-storage closure
until process exit, matching `AsyncCleanup`'s unknown-completion failure policy.
No receipt is fabricated for self-quarantine, and observer exceptions remain
contained. `RecognizeDevice` seals this retirement decision before diagnostic
error-string formatting, which may also fail under allocation pressure.
The headless fixture covers cancellation handoff failure followed by final
allocation failure or refusal, plus successful gated final release through
this exact helper; the two final-retirement call sites use the same policy.
`Selection::frozenCrops` in `uct_resident_diagnostic.hpp` holds separate bounded
per-source pools and recreates only metering decorators; every timed frozen job
asserts zero detector calls. Original complete-path timing remains the default.
`UserTolerancePass` separately records exact text/order/disposition/padding and
<=10-pixel drift of every box edge while allowing finite confidence variation.
`StrictComparisons` retains its original strict verdict and assertion. See
`uct_batch8_quality_acceptance.md` and `uct_resident_baseline.md`.

`TensorRtCropDiagnostic::TryCommit` is private, bounded to four leases and
nonthrowing. Saturation or allocation failure in the optional compact recorder
disables that capture without clearing finished OCR or retiring healthy engines.
The narrow friend fixture exercises the same commit path without TensorRT,
including retained input ownership and unchanged first-four descriptors after a
fifth-source refusal. CUDA failures still follow the normal quarantine policy.


### Bounded UCT shadow planner integration (2026-10-20)

`RegionShadowPlanner::Observe` consumes const, complete, prepared full OCR and
emits `RegionShadowPlan` metadata. Region identities are source-space spatial
occurrences; equal strings at different locations stay separate. Source,
geometry, scheduler authority, job and acquisition tickets reject stale/held
work. The bounded coverage grid includes unseen scene areas, and reserved
oldest-debt selections coexist with requested-focus priority. A fixed eight-pair
overlap array retains the exact previous sort prefix, and a ranked missing-id
queue preserves last-seen/id eviction order without repeated empty scans.
All plans carry
`RegionAuthoritativeAction::kRunFullOcr`; neither selections nor repetition
counters authorize skipping inference or publishing text.
`PreparedWordMatchesSource` checks same-job text/confidence and stamp-mapped
scene/protected geometry before scene clamping, preventing stale same-count
prepared arrays from manufacturing stability. Positive frame geometry and
hard-clamped `maxWordUtf16Units`/`maxTotalUtf16Units` reject malformed or oversized
metadata before content processing or evidence-clock changes. These observer
limits never truncate authoritative OCR.

`RecognitionScheduler`'s optional trailing `shadowPlanner` constructor flag
defaults false and is ignored for probe-only workers. Its existing worker owns
one planner. Complete current jobs are observed after ordinary OCR/preparation;
`RecognitionResult::shadowPlan`, `shadowPlannerMs` and `shadowPlannerError` carry
separate diagnostics. Ordinary `durationMs` excludes observer time. Each
scheduler authority change explicitly resets planner tickets before observing,
including backward seeks and source epochs restarted at a smaller number. An
observer exception retires the observer while preserving full recognition.
`SetShadowFocus` overwrites a single descriptor copied into the next job,
without submitting work or renewing evidence. The explicit
`FailNextShadowObservationForTest` seam covers observer failure isolation.

The controller enables observation only for
`OKUFLOW_UCT_SHADOW_PLANNER=1`. View updates supply the normalized visible region
as a priority hint; glyph/reading/UIA paths never consume shadow plans.
`uct_region_planner_tests` covers metadata policy, and
`uct_region_scheduler_tests` covers the real scheduler boundary, opt-out, focus,
capture-owned held identity, rejection of an unowned hold claim, seek reset and
injected failure. `OwnedHeldInput` models that metadata protocol through the
explicit CPU fallback fixture without a native device. Both use independently
registered 30-second headless slots. `optionalResidentBaseline` additionally
accepts `OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER=1` only with frozen source identities,
records observer time/tickets/debt and retains full OCR/zero-egress validity.
See `uct_region_planner.md` for acceptance boundaries and hard limits.

`uct_region_planner_benchmark` is an excluded, explicit QCoreApplication
diagnostic. `MakeResult` builds bounded synthetic full/prepared metadata with
distinct acquisition tickets; `RunScenario` times only `Observe`, while
`ValidatePlan` checks tickets, occurrence accounting, caps and coverage.
`Distribution` preserves raw samples and nearest-rank median/p95/max. Its stable,
shifted, repeated-label, continual-churn and overlapping scenarios make no OCR
latency or accuracy claim. `--run` is required; output is bounded JSON and no
ordinary CTest case runs the benchmark.

### Exact paused-source UCT continuity (2026-10-21)

`VideoPlaybackFrame::decoderLease` is an opaque `VideoPlaybackFrameLease`
containing weak player/object ownership and immutable file, request, archive
and track provenance. `SealFrame` attaches it to each real native decode or
same-pixel seek reticket; `IssuedFrame` requires the identical shared ownership
and object as well as current document provenance. `FollowingFrame` also
requires a distinct sample, matching geometry and a later PTS within 250 ms.
`VideoPlayback::PauseAtFrame` / `PauseAtFrameLocked` retain that exact displayed
frame, select the nearest already decoded future lease or request one bounded
read on the existing worker, and clear audio without a Pause seek. The private
`repositionOnResume` flag preserves decoder realignment when display lags;
`neighborMayReposition` authorizes deferred seek work only for an explicit
Open/Seek. The application clears its predecessor on Pause and adopts a later
neighbour-only snapshot only if its selected pointer still equals the hold and
the independent sample's PTS is later by at most 250 ms. Optional preparation
stops before native reads when queued future images already exceed that window
or the ready queue is full. Resume consumes rewound-cursor intent before the
EOS/empty branch, preserving the real final PTS; live-tail Waiting publication
also requires playback to remain active under the mutex.

`CorroborationInputValid` validates an optional independent neighbouring input
before it consumes the immutable controller's bounded evidence slot.
`UltraClearTextController::SetImmutableScene` identifies the selected acquisition
independently of that ancillary descriptor: its first usable late arrival does
not cancel the selected job, reset its deadline/generation or clear the model.
`HandleImmutableResult` preserves refused refinements while allowing an explicit
`kContentMoved` contradiction to revoke old proof without rollback.

`HasProvenImmutableArtifact` checks registered glyph proof against the current
owned selected input, geometry generation, frame and real PTS. It grants no
analysis/admission authority. `StopImmutableRefinement` exhausts optional work,
cancels scheduler authority and clears pending handles while retaining the
exact old model and its clocks. This presentation-only exception applies to
terminal refinement errors; source/seek/settings changes and unsuccessful
presentation still revoke or hide the artifact. Reading-only candidates do
not qualify. The backend panel keeps the actual recognizer status.

`RecognitionScheduler::State::Run` selects preparation from the device's
resident-analysis capability independently of whether OCR itself ran on CPU.
A temporary Windows bridge therefore uses the same resident reference/color
and GPU-tree digest protocol as the later GPU result. It retains only the CPU
OCR backend's explicit readback; resident errors cannot trigger CPU analysis.

`OverlayModel::AdmitImmutable` compares complete selected-source provenance,
authority and full digest before retaining earlier same-frame coverage.
`MergeImmutableCoverage` keeps old proved word identities/geometry/reference/
validation clocks and adds only independently proved new glyphs. The union caps
are 1024 lines, 8192 word slots and 32768 reading tokens/protected rectangles;
retired slots count toward the bound. A fixed 64-row spatial index limits
ordinary matching to nearby rows. Contradictory independently proved overlap
falls back to original ink, and source/seek changes cannot inherit old proof.
`ApplyImmutableConflictBlocks` applies persistent word-local conflict rectangles
through the same bounded index, including when no old glyph survives. The 8192
rectangle cap fails closed on saturation. Only new complete selected-source
authority/digest or `Reset` clears these blocks; repeated refinements cannot
resurrect disputed text. Independently proved matching words may refresh only
their stabilized colors without renewing IDs, slots, references, geometry or
validation clocks.

`MeasureLineColors` and the CUDA descriptor kernel share private integer
`color_policy::{Rgb,Median,Compatible,Endpoint,AdjacentBackground,Background}` decisions. Three
2-pixel body passes find robust ink/paper medians and the ink's opaque quartile;
four bounded side strips require RGB agreement with local body clusters and
in-box corner support before exterior color may override polarity. Scratch is
  fixed (about 8.3 KiB CPU stack and 10.2 KiB CUDA shared memory per block), with
no per-word allocation or image download. `LocalColorFixture`,
`LocalColorFixtures` and `ColorDistance` supply shared controlled surfaces and
tiny/clipped bounds to CPU and exact CUDA descriptor tests. These remain solid
neutral-source colors, separate from display grading and 7:1 renderer contrast.
All four immediately adjacent one-pixel sides can establish a compatible local
surface without requiring paper in glyph-filled corners; that branch skips the
farther ring. Missing or mixed adjacent sides retain the conservative fallback.

`HandleProbe` records noncomposite residual invalidation in
`probeInvalidatedSinceSubmit_`; it requests replacement OCR even while surviving
lines cover the viewport. Successful submission or authority invalidation clears
this demand. Stable-probe checks, 80 ms probe cadence, 350 ms advancing-media
pacing, held-acquisition exclusion and latest-only pool ownership remain intact.

`AcceptedLine::readingWordIndices` is optional. Empty retains the sequential
token/word contract. Otherwise each reading token explicitly names its stable
word slot, with -1 for omissions, so an immutable refinement can append newly
proved words without renumbering old UIA identities. `ScreenLines` validates
that mapping and exposes only eligible named glyphs; `AcceptedWordCount` counts
active named slots rather than retired duplicate reading-only slots.

The explicit `uct_paused_video_test::Run` / `optionalPausedVideoProof` diagnostic
uses existing pinned M/M N8 plans and an inference-only SDK, actual owned CUDA
inputs, production worker corroboration and immutable admission. It records
per-word proof alongside raw candidate output, verifies zero image egress in
GPU-only mode and unchanged cached-plan bytes, and waits for exact TensorRT
State retirement. Its explicit `CpuBridgeThenResident` diagnostic wrapper uses
two real Windows OCR calls before real device recognition, checks the identical
canonical fingerprint, and records old-word retention/revocation. That bridge
mode counts exactly two explicit CPU OCR image readbacks. Per-word records
require exact text and selected geometry rather than overlap matching.
The direct `optionalDenseImage` diagnostic also waits for that receipt after
recognizer destruction, including assertion exits. See
`uct_paused_continuity_repair.md` for selectors, the 60 FPS source-specific frame
mapping and the distinction from decoder/native-display acceptance.
## Opt-in recognizer width policy

`src/common/uct_recognizer_width.hpp` supplies bounded 128/256/512/1024
canvas selection, contiguous ordered-group maxima and exact profile extent
validation. `gpu::RecognitionGroupWidths` initializes the complete bounded
metadata array without sorting or transferring pixels. TensorRT `Network`
checks dynamic-width profile identity and actual output extents; `State`
owns device/pinned group widths through the existing stream waits. Explicit
width preparation is separate from cache-only inference and retains the
frozen-upload / `RetireOwnedState` ownership contracts.
`TensorRtProfile` records width policy, canvas counts and useful padding.
The five width fixtures and resident selector are described in
[narrower recognition inputs](uct_recognizer_width.md); defaults remain fixed.

## Oriented local UCT color preparation

`MeasureLineColors` accepts an optional detected `TextQuad`. Its body, corner
and immediate/expanded exterior samples use that quad's bilinear local frame;
the existing `color_policy` medians, compatibility votes and opaque-ink endpoint
decisions are unchanged. The scheduler selects this frame for oriented words
before `FitOrientedWordInk` scans the separately bounded `inkSearchQuadPx`.
Rectangle line colors and non-oriented words retain the legacy sampling grid.

`gpu::LineColors` accepts an optional parallel `OrientedInk` frame array (zero
width selects the corresponding rectangle). The resident preparation worker
uploads detected color frames alongside the existing search-frame ink
descriptors within its bounded scratch. `validation::orientedPixel` provides
the shared floor/bilinear addressing for CUDA colors and `orientedInkAt`, using
explicit non-fused double operations to match the CPU. Out-of-image samples
contribute neither sums nor votes; valid sample counts normalize clipped body
means. Stream completion, input ownership and compact-only readback remain
unchanged.

`FitOrientedWordInk` and `validation::orientedInk` remove eight-connected ink
components spanning opposite padded search edges before dominant-row fitting.
This separates glyphs from an identically colored panel and nearby edges. CPU
uses a queue of at most 32768 cells; CUDA packs one ink bit plus six
component reachability bits per cell and 4096 row masses (about 46 KiB shared
scratch). Monotone whole-run propagation has the CPU queue's fixed point when
complete. `gpu::kOrientedInkMaxPropagationPasses` caps it at 64 scans; an
incomplete result keeps detector geometry. `OrientedFit::propagationPasses` and
`propagationComplete` retain compact diagnostic evidence. Original fit-size/contrast and
detached-mark rules remain; no token disposition or confidence is changed.
`OrientedFitReason`/`OrientedFit::reason` distinguish invalid input, propagation
cap, unanchored ink, too few samples, ambiguous row bands, the shrink guard and
a completed fit. This uses the result struct's existing alignment space.
`color_policy::Compatible` anchors entire components to measured ink; all raw
luma-ink pixels of anchored glyphs remain, preserving darker/lighter cores and
AA. Paper-enclosed interior components remain regardless of RGB variation.
Unanchored edge-only fragments or unanchored components spanning two edges
are excluded. An unanchored boundary component extending inside causes whole-word detector
fallback, avoiding silent letter truncation from a gray opaque-quartile median.
The ambiguity guard uses the existing eight-sample minimum;
`validation::substantialInkComponent` checks at most eight cells to match the
CPU queue's component-size decision without additional shared image storage.

`uct_geometry_test::TiltedButtonColors` runs 48 real-glyph tilted/perspective
button fixtures through CPU and resident schedulers. It checks source polarity,
glyph rather than button bounds, all four corners, spacing/protection and exact
GPU parity with guarded zero image egress, including dotted/descending glyphs
touching search edges. `GpuWindingInkWorkCap` requires bounded incomplete GPU
fallback rather than a partially pruned fit. Existing `CpuGeometry`/`GpuGeometry`
slots include it in `uct_paddle_processing`/`uct_paddle_cuda`. Diagnostic
`uct_batch_test::Output` includes raw per-word foreground/background colors
alongside fitted quads in private image-proof JSON. See
[local color evidence and limitations](uct_color_startup_repair.md).
The opt-in paused proof's `fitReplay` lambda reuses resident input/search quads
and raw colors after actual recognition. `selectedFitReplay` and
`neighbourFitReplay` record per-word passes/completion, a pass histogram and
three CUDA-event kernel durations after one warmup. Replay fitted flags are
labelled separately from production fits; these measurements are excluded
from actual worker durations and transfer only descriptor results.
