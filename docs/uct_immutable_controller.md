# Selected-media UCT controller

`UltraClearTextController` owns a selected-media mode independently of camera
liveness. `SetImmutableScene(UctImmutableScene)` takes shared ownership of a
complete processed-scene BGRA image, its real frame sequence and timestamp,
the scene geometry, and the selected source authority (for example a seek
generation). The optional corroborating image has its own real frame identity.
Neither input is represented as a camera presentation or assigned a fresh
capture timestamp.

A selection revokes earlier scheduler and probe authority, clears the accepted
artifact, and submits the selected image once on the existing recognition
scheduler when that backend reports readiness. There is no second media OCR
worker. The existing camera probe machinery stays dormant while media is
selected. Camera presentation receipts, CPU/device input, readback requests,
source-retirement notifications and camera input-health notifications cannot
change the selected media's identity or projection.

`OverlayModel::AdmitImmutable` receives the selected result's unchanged ticket.
The first result supplies selectable text with an omission marker and leaves
original pixels visible. One optional distinct neighboring image can be
recognized after the selected result. Admission additionally requires a
different frame sequence, a different full-source SHA-256 digest, matching
text and geometry, and a matching line residual reference before glyph
replacement is eligible. Reused leases are refused before scheduling;
different leases containing identical pixels do not provide corroboration.
Failure or unavailable evidence ends that bounded attempt.

Source age is separate from the current analysis deadline. The default
`UctTiming::immutableJobTimeoutMs` is 30,000 ms measured from job submission;
late completion is rejected without altering the source timestamp. An
unchanged completed selection does not enqueue periodic OCR or motion probes.
The accepted artifact remains tied to the selected frame, including when the
source is older than the live four-second result gate or when no live
presentation has occurred within the 600 ms camera watchdog.

`UpdateImmutableView` changes only the projection and stroke weight. Because
the input analysis covers the complete processed scene, pan, zoom, resize and
Show original reuse the same artifact and never renew evidence. Repeating the
same selection lease and identity also changes only its projection. Supplying
new selected pixels, frame identity, geometry or source authority cancels old
work immediately. Recognizer replacement fences the old backend and analyzes
the retained selected image once with the replacement.

`ClearImmutableScene` revokes selected work and clears its reading text before
returning to live mode. Live analysis waits for a truthful new camera
presentation. Invalid selected input still suspends hidden live analysis until
an explicit clear or replacement selection; it cannot accidentally reveal
camera text over a media view.

`OnAdvancingMediaPresented(UctAdvancingMediaScene)` accepts only successful
native playback receipts. Its DTO carries the same geometry and source
authority plus one owned current frame, real media PTS in milliseconds, and
the committed view transform. Ordinary advancing frames retain one authority
and one newest scene lease; they do not cancel in-flight OCR or prepare a
neighbor. Geometry, seek/source authority, backward frame/PTS progression or
mutation of an existing frame identity revokes earlier work synchronously.
The caller supplies a new authority for a seek or source transition. Camera
callbacks are ignored throughout both selected-media modes.

Selected-media presenter faults have their own signal:
`OnMediaPresentationFailed` immediately clears glyphs, the reading pane and
published screen-reader elements. It retains the cached scene artifact, but
latches presentation unavailability across late backend/OCR callbacks, Show
original changes, feature toggles and `UpdateImmutableView`. Camera failure
and success notifications remain isolated from media. Only a real successful
media receipt reopens presentation: `SetImmutableScene` (including its exact
same-frame path), `OnAdvancingMediaPresented`, or the explicit
`OnMediaPresentationSucceeded` signal after a successful view-only media draw.
The latter restores a still-valid cached artifact without new OCR, probes,
source timestamps or frame authority. A view update alone cannot certify
successful output. Both media-only signals are ignored after returning live.

Advancing analysis shares the existing scheduler and probe worker. A distinct
presented frame can enter the decimated full-scene probe path at most once
per `probeIntervalMs` (80 ms by default); one in-flight probe plus one latest
pending probe bounds validation. Full-scene pixels remain shared leases.
OCR has no queued replacement while inference is busy and is paced by
`mediaRecognitionIntervalMs` (350 ms by default), with the existing 1,500 ms
refresh interval once accepted coverage is complete. Pending words still need
genuinely distinct frame/pixel evidence. Per-frame view receipts reproject
without OCR/probe work when the actual frame is unchanged.

`OverlayModel::AdmitAdvancingMedia` preserves live temporal agreement and
residual checks while separating media age from job age. Every source stamp
retains its original media PTS; the controller enforces the same 30-second
wall-clock job deadline independently. Scene glyphs, reading text and screen
text all evaluate freshness against the current media PTS. Current independent
probes can therefore keep stable lecture text validated during slow OCR; a
failed residual removes affected old text and accessibility exposure as soon
as that probe completes. Detection latency is bounded by probe admission and
worker completion, rather than a full-image UI-thread comparison per frame.
A held VFR frame remains tied to the last successful receipt and its unchanged
media clock. Failed native submissions do not call this receipt API.

Advancing-to-paused `SetImmutableScene` cancels paced work and analyzes the
selected image plus its optional real neighbor once. Entering advancement
revokes the paused artifact on its first successful receipt; clearing either
mode revokes media work before returning live. `HasImmutableScene` reports
selected-media ownership for both modes so existing camera guards remain
effective; `HasAdvancingMediaScene` distinguishes playback for diagnostics.

`tests/ultra_clear_text_controller_tests.cpp` is an offscreen scripted-backend
fixture covering old-frame admission, zero idle work, backend-readiness
submission, projection-only zoom, identical-pixel rejection, late-result
fencing, model replacement, independent deadlines and invalid-input isolation.
Advancing cases additionally cover two seconds of bounded OCR/probe work,
latest-frame validation, matching reading/scene/screen projections, a held
recognizer, changed scene pixels, authority revocation, hidden-camera isolation,
one-shot pause and wall-clock deadline expiry with fixed media PTS.
Presentation-fault cases cover both media modes, immediate pane/glyph/active
accessibility revocation, blocked late readiness and recognition, view/toggle
isolation, and exact successful receipt recovery without additional analysis.
It requires no camera, desktop focus, external OCR runtime or media sample.
The zoom regression uses the cropped Fill transform, verifies its half-size
source extent and checks the overlay's exact scene-to-view projection; Fit
deliberately preserves the complete source and ignores magnification.
Each invocation preserves an explicit QtTest text report under
`%LOCALAPPDATA%/OkuFlow/test-evidence/immutable-uct/` and replays it to stdout
with the numeric result, keeping test diagnostics inspectable even when the
implicit QtTest logger produces no captured output.
