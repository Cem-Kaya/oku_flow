# Camera recording and selected-media presentation

Camera ingestion and the selected view have separate ownership. New recording
admission takes the full clean camera source after format conversion and camera
orientation, before viewing effects. It does not depend on a successful viewport
present, a valid crop, or which document is selected. Imported video pixels never
enter the recorder, camera startup counters, recovery acknowledgments or camera
intent.

`OkuFlowApp::AdmitOriginalFrame` submits one owned Original CPU buffer or fenced
GPU clone to `RecordingManager::AddOriginalFrame`. The native CUDA camera path
admits it after source conversion; the CPU path admits it before preparing the
display scene. There is no processed-video recording canvas, ink composition or
second recording clone in either presentation path. Photos retain their separate
paired-image behavior.

When a recording continues behind an opened video or held recording frame, live
camera ingestion and device-loss handling continue. Native GPU input still uses
its conversion/orientation route and fenced Original clone, with live effects and
live UCT disabled. CPU fallback performs only clean Original conversion. With no
recording or photo consumer, hidden camera arrivals need no image processing.
Recording uses the saved camera-orientation snapshot while media rotation changes
only the selected view. Existing bounded capture, encoder and GPU-pool admission
policies still apply; this split does not promise lossless admission under pressure.

`PresentSelectedMediaPixels` builds a separate immutable CPU scene from owned
decoded pixels. Its cache key includes source/seek authority, exact frame,
dimensions, stride, orientation and appearance. Rotation, black-and-white,
reading-color LUT, contrast and brightness run once per frame or appearance
change. Pan, zoom, Fit/Fill, focus-marker movement and resize reuse that scene
through `PresentFitted`; they do not rerun decoding, camera effects or OCR.
Selected pixels never borrow the mutable live CUDA texture.

A successful viewport receipt retains its exact source/appearance recipe,
transform, native viewport extent and focus-marker state. Unchanged paused
polls reuse that receipt and the retained viewport pixels; they do not clear,
resample or upload the same image at the polling rate. Source/frame/appearance,
pan/zoom/resize, explicit viewport dirtiness and presenter retry demand require
another submission. Failed/deferred submissions never replace the receipt.
Hidden Original camera processing does not mark the selected viewport dirty.
Save frame can reuse the matching owned viewport without forcing another upload.

The current CPU playback path does not apply CUDA keystone, stabilization,
Extra Stable, temporal smoothing, blur, spatial sharpening, Text Clarity or
NVIDIA Super Resolution. Their live-camera settings must not be reported as
effective playback processing. Playback color/orientation support does not
establish equivalent processing quality or GPU acceleration. Native responsiveness,
recording continuity and memory plateau still need measured acceptance.

Immutable UCT receives the complete processed selected scene and, when available,
one real distinct decoded neighbor prepared with the same recipe. Display and OCR
share its immutable pixel ownership. Pan/zoom updates only its projection; media
PTS is preserved rather than refreshed to imitate a live capture. A seek or source
change replaces authority and revokes earlier work. Recognition and validation
remain asynchronous, and a held frame is never duplicated as corroboration.
During continuous playback, successful media receipts instead feed the shared
controller's paced advancing-media policy. It keeps the real media PTS and seek
authority, samples recognition and validates current scenes asynchronously;
ordinary advancement does not restart one-shot immutable recognition for every
frame. No full processed neighbor is prepared during advancement. Pause selects
one owned scene and optional real neighbor once. Both media modes publish OCR
authority only after successful native presentation, or against an unchanged
matching retained receipt, and do not fabricate camera liveness.

The native decoder's decoded-frame byte limit is not a total application-memory
limit. A processed displayed scene and at most one pending processed successor,
an optional processed UCT neighbor, the viewport buffer and bounded in-flight OCR
ownership are additional allocations.
The neighbor is prepared only while UCT is enabled. Include all of these in
resource-plateau measurements and admission budgeting.

Successful selected-media presentation publishes the same current viewport to
native observations and retains it in `presentationBuffer_` for explicit image
actions. Annotation snapshots retain selected-frame identity without setting the
camera-active flag. Read/Explain, Save frame and agent delivery must bind to the
selected document and successful presentation receipt; source/seek transitions
must retire stale requests before another document can consume them.

`PresentedOriginalCache` retains the last successfully displayed recording frame
and at most one processed successor awaiting a presentation receipt. Both slots
share Original recorder GPU leases, or own clean CPU buffers, with a combined
96 MiB payload limit. A successful receipt promotes its exact source sequence and
view transform. A deferred successor does not replace the displayed frame. A
successful frame whose source could not be retained revokes the older hold;
Pause cannot silently choose an earlier moment. No cache is populated by ordinary
camera use outside recording, and selected-media viewing releases the cache.

`CaptureLiveForMediaHold` reads that owned displayed Original, even after a newer
processed source has overwritten the mutable pipeline buffers. CUDA uses an
asynchronous full-Original readback with an exact request identifier and a
producer-fence dependency; the readback ring retains its Original lease until
copy completion. Cancellation cannot recycle the source early. Successful resize
drain releases dropped copies; unknown completion quarantines the bounded owner.
CPU holds copy the matching retained clean buffer. Temporal/debug composites have
no exact single-Original identity and remain unavailable for this hold seam.
The retained scene also carries the successfully applied appearance recipe.
Only source rotation, black-and-white, reading colors, contrast and brightness
can be reproduced by the current CPU view. Actual blur, spatial/neural output,
stabilization, keystone, temporal composition, auto contrast and Text Clarity
pixel effects make exact clean-Original Pause unavailable. A drawn focus marker
also uses different GPU/CPU presentation primitives and is rejected. A pending
control edit cannot rewrite the stored recipe: supported appearance must still
match the selected CPU recipe, or Pause fails without substituting another image.
The owner restores the retained successful crop and verifies its geometry before
accepting the hold; camera orientation is already baked into Original.
The owner acknowledges Pause only after
`AcceptLiveMediaHold` receives the owned pixels. A lost readback expires after
one second without a successful hold acknowledgment. Retained Original pixels
already contain camera orientation; media-view rotation is relative to that
normalized source.

Cancellation and timeout retire the exact Original readback identifier rather
than forgetting its provenance. A bounded discard slot consumes a late result
before observations, photos, periodic analysis or UCT viewport consumers. The
presenter permits only one owned Original copy outstanding, including canceled
copies; a second discard-only slot preserves provenance if a resize dropped an
earlier receipt before its discard was observed.
Retiring UI authority does not release unknown GPU work or recycle its source.

The actual-app offscreen regression stages Original N, acknowledges its display,
then stages N+1 without a display receipt while overwriting current pipeline
buffers. Pause must retain N's exact owned pixels, sequence and capture clock.
Separate cache tests cover lease lifetime, byte admission and revocation after a
successful unretained successor. These desktop-free checks do not validate a
native GPU copy or visible Pause latency.
Additional admission regressions count one viewport upload for 120 unchanged
polls, require new submissions for frame/appearance/geometry or explicit retry,
and preserve exact discard identity through cancellation/timeout routing. These
are policy checks, not measured native upload or display FPS.

This document describes implementation ownership, not completed native-camera,
GUI, codec, long-recording, NVDA or performance acceptance. Use the tracked scoped
and full Windows validation gates and report their actual scope separately.
