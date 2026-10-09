# 37 — Code Review Notes (2026-09-10)

Status: **IMPLEMENTED AND VERIFIED — all ten findings addressed, 2026-09-10.**

These notes record the functionality, correctness, performance, and recovery
findings from the source review of the working tree based on `bd52975`, including
its uncommitted changes. The owner initially requested notes only, then authorized
Astra sub-agent implementation of all ten issues followed by root review,
compilation, and testing. This document retains each original finding and records
its implementation status; executable coverage belongs in the repository tests.
The problem/evidence/correction paragraphs below preserve the original review;
each implementation paragraph describes the resulting code.

**Evidence terminology:** `Confirmed in code` means the control flow or data
handling was inspected. It does not mean the visible failure was reproduced.
`Conditional risk` means a missing recovery boundary is visible in code but its
failure requires a stalled driver or other external condition. `Performance
opportunity` means the synchronization or work is present, but its cost has not
been measured in this review. Line numbers refer to the reviewed working tree;
use the named symbols to relocate the evidence after edits.

## Verification result

Root reviewed and integrated the Astra implementations and ran the tracked
`scripts/agent_build.bat` matrix after the final teardown correction:

| Gate | Final result |
| --- | --- |
| Release application compilation | PASS |
| CPU preset | 23/23 tests passed |
| CUDA preset | 27/27 tests passed, including all four GPU-labeled tests |
| Turkish/German source parity | 687 complete entries in each catalog; PASS |
| Diff whitespace check (Windows CRLF-aware) | PASS |

Final log: [review37-build-matrix.log](../build/review37-build-matrix.log).
The follow-up `scripts/build_release_bundle.bat` run also passed all 27
release tests and published the complete CUDA-enabled bundle to
`dist/OkuFlow`. Its executable SHA-256 was checked against
`build/release-bundle/cmake/Release/oku_flow.exe` and matched exactly.
Packaging log: [review37-bundle.log](../build/review37-bundle.log).
The CUDA preset includes the shared CPU coverage plus its GPU targets; the
counts are suite executions, not 50 different tests. No targets were skipped.
The first integration build exposed a missing `NOMINMAX` definition in the new
standalone viewing-zoom test target; its configuration was corrected before
the passing matrix. The final pass includes explicit allocation-free camera
import abandonment when producer quiescence is unknown.

These results verify compilation and automated coverage. Physical camera
switching, actual wedged-driver recovery, live Maxine behavior, and end-to-end
camera throughput were not measured in this task. Controlled lifecycle tests
validate timeout and ownership decisions without forcing a real driver hang.

## 1. Stabilization receives 1x magnification even when the viewport is zoomed

**Status:** Implemented. Original evidence: confirmed in code. **Priority:** P1.

**Implementation:** `ProcessingSettings::EffectiveViewingMagnification()` feeds
stabilization independently of legacy image zoom. App configuration supplies
effective viewing zoom, including 1x when Fit suppresses requested magnification.
Coverage: `stabilization_viewing_zoom`, existing canonical-view geometry and
CUDA stabilization/replay targets.

**Problem:** `OkuFlowApp::RunCudaPipeline()` sets `settings.enableZoom = false`
because image enlargement now belongs to presentation. It separately supplies
the actual viewing magnification in `settings.zoomAmount`.
`CudaInteropSurface::ProcessFrame()` nevertheless uses `enableZoom` to decide
whether stabilization should use that magnification, forcing its local
`zoomAmount` to 1.0.

**Impact:** The stabilizer's visible-region preference, matching tolerance, and
display-pixel deadband use unzoomed assumptions while the user may be reading at
4x or higher magnification. Source motion can therefore be treated as acceptable
even when magnification makes it conspicuous, and motion outside the reading
region can receive the wrong weight.

**Evidence:**

- [app_pipeline_runtime.cpp](../src/app/app_pipeline_runtime.cpp),
  `OkuFlowApp::RunCudaPipeline`, lines 728–734.
- [cuda_interop.cpp](../src/cuda/cuda_interop.cpp),
  `CudaInteropSurface::ProcessFrame`, lines 3220–3224 and the subsequent
  `LaunchVirtualTripodMatchCandidate` / `LaunchSelectVirtualTripodMatch` calls.

**Suggested correction:** Represent viewing magnification independently of
whether the processing stage applies zoom. Feed the effective viewing
magnification to stabilization while retaining full-scene processing.

Related: [14 — Stabilization v2](14-stabilization-v2.md) and
[15 — Aspect-safe viewport](15-aspect-safe-high-refresh-viewport.md).

## 2. Recorded annotations use SuperRes texture coordinates for scene-based ink

**Status:** Implemented. Original evidence: confirmed in code. **Priority:** P2.

**Implementation:** Recorded ink uses the preserved `annotationTransform` in
full-scene coordinates while texture sampling uses the remapped cache transform.
`annotation_model_geometry` now covers cropped-cache recording alignment on
multiple canvas sizes and the full-frame path.

**Problem:** `PresentLatestCudaScene()` preserves the full-scene transform as
`annotationTransform`, then remaps `transform` into the SuperRes crop. The live
overlay receives the full-scene transform, but the recording annotation render
receives the remapped `transform`. Stored stroke positions are still relative to
the full scene.

**Impact:** When a cropped SuperRes cache is presented, ink burned into the
processed recording can shift, scale incorrectly, or leave the visible area even
though the live overlay remains aligned. A full-frame cache with an identity
source rectangle does not expose the same mismatch.

**Evidence:** [app_pipeline_runtime.cpp](../src/app/app_pipeline_runtime.cpp),
`OkuFlowApp::PresentLatestCudaScene`: crop remapping at lines 892–904 and
`RenderAnnotationStrokes(..., transform, ...)` at lines 1057–1061.

**Suggested correction:** Render the annotation layer using `annotationTransform`.
Retain the remapped transform for sampling the SuperRes texture.

Related: [18 — Annotation mode](18-annotation-mode-plan.md).

## 3. System-memory capture calculates stride instead of preserving buffer pitch

**Status:** Implemented. Original evidence: confirmed layout assumption; affected devices not identified.

**Implementation:** `CopyCaptureBuffer()` prefers native 2D buffer pitch, validates
available bounds, and normalizes padded/bottom-up RGB, YUY2, and NV12. Negotiation
honors signed `MF_MT_DEFAULT_STRIDE`; single-buffer samples preserve their 2D
interface. `capture_buffer_layout` covers real MF 2D buffers, padded layouts,
truncation rejection, and unchanged output on invalid input.
**Priority:** P2.

**Problem:** `MediaCapture::ReadCurrentFormat()` derives row stride from subtype
and width using `MFGetStrideForBitmapInfoHeader`. The capture path does not read
`MF_MT_DEFAULT_STRIDE` or obtain actual pitch through a 2D buffer interface. The
system-memory path copies a locked buffer and assigns this calculated stride to
the resulting `MediaFrame`.

**Impact:** If a source supplies padded rows or a different orientation, the
conversion routines can interpret the buffer incorrectly despite passing the
minimum-size checks. For NV12, an incorrect Y stride also changes the calculated
UV-plane offset. Existing support for a stride argument in conversion does not
help when capture supplies the wrong value.

**Evidence:** [media_capture.cpp](../src/capture/media_capture.cpp),
`MediaCapture::ReadCurrentFormat`, lines 877–893, and
`MediaCapture::CaptureLoop`, system-memory `Lock` / `frame.data.assign` path around
lines 1616–1649.

**Suggested correction:** Preserve the negotiated and actual buffer layout,
including pitch and orientation, and normalize the layout explicitly when
necessary. Keep each plane's offsets and extents consistent with that layout.

Related: [20 — Capture and recording integrity](20-capture-recording-integrity.md).

## 4. YUV conversion hard-codes BT.601 limited range

**Status:** Implemented. Original evidence: confirmed; impact depends on source color format.

**Implementation:** `YuvColorInfo` follows frames through CPU, CUDA, D3D11, and
original-recording conversion. BT.601/709 and limited/full range use shared
pixel equations; missing/unknown tags default explicitly to BT.601 limited.
Unsupported explicit tags are rejected. CPU and CUDA color targets compare
both layouts against independent floating-point reference values.
**Priority:** P2.

**Problem:** CPU and CUDA NV12/YUY2 conversion use fixed BT.601 limited-range
coefficients. Negotiated matrix and nominal-range metadata are not carried into
the conversion settings.

**Impact:** BT.709 input is converted with the wrong matrix, and full-range input
is expanded as though its luma used limited range. Colors and black/white levels
can be incorrect, affecting later contrast and text-threshold processing. CPU/GPU
agreement alone does not establish agreement with the source.

**Evidence:**

- [cuda_kernels.cu](../src/cuda/cuda_kernels.cu), `Bt601ToBgra` and its NV12/YUY2
  callers, starting at line 2289.
- [image_processing.cpp](../src/common/image_processing.cpp),
  `ConvertNv12ToBgra`, fixed conversion equations around line 129, and
  `ConvertYuy2ToBgra`.
- [media_capture.cpp](../src/capture/media_capture.cpp),
  `MediaCapture::ReadCurrentFormat`.

**Suggested correction:** Carry color matrix and range with the frame format,
apply them consistently in CPU/GPU conversion, and define explicit defaults for
sources that omit the metadata.

## 5. Failed CUDA initialization is retried in the per-frame path without backoff

**Status:** Implemented. Original evidence: confirmed in code. **Priority:** P2.

**Implementation:** `CudaSurfaceRetry` gates synchronization and allocation using
a device/fence/session/extent key and 1/2/4/8/16/30-second capped backoff. Raw and
converted fallback share that state. Configuration changes allow immediate
retry. `cuda_surface_retry` checks deadline boundaries, repeated frame calls,
configuration invalidation, and successful recovery without sleeping.

**Problem:** `EnsureCudaSurface()` caches a valid surface but does not remember a
failed initialization or impose a retry delay. A later frame again drains
graphics work, creates shared textures, attempts CUDA initialization, and releases
the failed allocations. A raw NV12/YUY2 frame can reach initialization first through
the raw path and again through the CPU-converted fallback.

**Impact:** When CUDA interop is persistently unavailable, passthrough preview can
repeatedly pay allocation, synchronization, initialization, and logging costs.
The fallback therefore continues doing work that is already known to fail.

**Evidence:** [app_pipeline_runtime.cpp](../src/app/app_pipeline_runtime.cpp),
`OkuFlowApp::EnsureCudaSurface`, successful-cache check at lines 346–350,
resource reset at lines 353–365, and initialization failure at lines 424–436;
callers `TryProcessRawFrameWithCuda` and `ProcessFrameWithCuda`.

**Suggested correction:** Introduce an unavailable/retry state keyed to relevant
device and configuration changes. Use explicit retry or bounded backoff, and
coalesce repeated diagnostics while passthrough remains active.

Related: [22 — Threading and performance](22-threading-performance.md).

## 6. Notes image encoding and notes writes still execute on the UI thread

**Status:** Implemented. Original evidence: confirmed; pause duration unmeasured.

**Implementation:** A serial queue performs image encoding, file creation,
append/flush/rollback, and finalization with captured paths/data. Limits are
128 jobs / 256 MiB plus three pending images / 192 MiB. Canceled images stay
off disk. Asynchronous errors reach the UI; opening notes and announcing saved
transcripts wait for completion. `notes_html` covers ordering, destination
changes, failure/retry, queue limits, destruction, and image cleanup.
**Priority:** P2.

**Problem:** OCR, VLM, and Assistant frame preparation call
`SaveAnalyzedImageForNotes()` before dispatching their background preparation
jobs. This helper creates directories, potentially resizes the image, encodes a
JPEG, and writes it synchronously. Final transcript callbacks also append and
flush notes synchronously from the UI path.

**Impact:** Large frames or a slow notes destination can interrupt preview and
input while an analysis request or transcript save is handled. This leaves a
synchronous path underneath the documented asynchronous preparation behavior.

**Evidence:**

- [assistive_runtime.cpp](../src/common/assistive_runtime.cpp),
  `StartOcr`, notes-image save at line 1242; corresponding calls in
  `SubmitAssistantPrompt`, `StartVlm`, and `StartCodexVlm`.
- Same file, `SaveAnalyzedImageForNotes`, starting at line 2246, and
  `AppendNoteBlock`, starting at line 2369.
- [app_bootstrap.cpp](../src/app/app_bootstrap.cpp), `SegmentFinalized` callback
  calling `NoteTranscriptSegment`, starting at line 1125.

**Suggested correction:** Move notes encoding and storage to bounded background
work. Preserve write ordering, cancellation, error reporting, and ownership of
images so canceled requests do not leave orphan files or broken references.

Related: [22 — Threading and performance](22-threading-performance.md),
[34 — Searchable captured notes](34-searchable-captured-notes.md), and
[36 — Recording transcription](36-recording-transcription-to-notes.md).

## 7. NIS/FSR are invoked at 1:1 dimensions rather than performing magnification

**Status:** Implemented. Original evidence: confirmed integration limitation. **Priority:** P2.

**Implementation:** `ComputeSpatialCacheGeometry` selects the visible integer
crop and differing output extent; `UpdateSpatialCache` runs NIS/FSR enlargement
once per camera frame after stateful processing. It reuses existing buffers,
with 2x per-pass and 1440p limits; further zoom remains display scaling. A
one-texel guard prevents stale-pixel interpolation at partial-cache borders.
Geometry and real CUDA targets cover crop bounds, pitched offset inputs, and
guard pixels. Viewport-only motion reuses the cache or the registered base scene.

**Problem:** `CudaInteropSurface::ProcessFrame()` passes identical input and output
dimensions to both `LaunchNisLinear` and `LaunchFsrEasuRcasLinear`. Subsequent
viewport enlargement uses the D3D12 linear sampler.

**Impact:** These backends can affect sharpening, but their enlargement algorithms
do not perform the displayed magnification. This differs from the UI description
that the feature sharpens and upscales. The cost of an EASU pass at unchanged
dimensions is also an optimization question, not a measured speed regression.

**Evidence:** [cuda_interop.cpp](../src/cuda/cuda_interop.cpp), spatial sharpening
branch starting at line 3856;
[presenter.cpp](../src/d3d12/presenter.cpp), linear sampler at line 896;
[main_window.cpp](../src/ui/main_window.cpp), Spatial Sharpen accessibility
description around line 1725.

**Suggested correction:** Decide whether this feature provides sharpening only
or actual enlargement. For enlargement, connect the backend to the appropriate
crop/output geometry while keeping stateful effects on the camera clock. For
sharpening only, align the implementation and descriptions with that scope.

## 8. Camera stop has no deadline around reader shutdown or thread join

**Status:** Implemented. Original evidence: conditional recovery risk. **Priority:** P1 if a driver stalls.

**Implementation:** `MediaCapture` is a UI facade over a shared
`MediaCaptureSession`. Capture/stop workers own their session; `CaptureShutdown`
revokes callback entry and coordinates quiescence/cleanup with a shared 1500 ms
worker deadline plus bounded consumer GPU teardown. App `CameraIngress` isolates
callbacks and queued samples from app lifetime. Timeout retains resources,
preserves the crash marker, blocks further camera starts, and skips MF teardown
at exit. `capture_shutdown` exercises controlled stalled workers and release
ordering without wedging a real driver.

**Problem:** `MediaCapture::StopCapture()` calls synchronous `Flush()` and then
joins the capture thread without a deadline. The application calls this path for
camera changes and shutdown.

**Impact:** If a driver fails to return from reader operations, camera switching
or application close can block the UI indefinitely and prevent recovery logic
from running. This is not a claim that ordinary `Flush()` fails to unblock
`ReadSample`, nor a race on `sourceReader_`: the existing join-before-reset
ordering correctly protects normal resource lifetime.

**Evidence:** [media_capture.cpp](../src/capture/media_capture.cpp),
`MediaCapture::StopCapture`, lines 697–707;
[app_pipeline_runtime.cpp](../src/app/app_pipeline_runtime.cpp),
`OkuFlowApp::StopCameraCapture`, starting at line 1465.

**Suggested correction:** Design a bounded stop policy with independently owned
capture state. If a worker cannot stop, preserve its resources and callback
isolation until safe cleanup; detaching a thread that still uses the app object
would introduce a lifetime defect.

Related: [20 — Capture and recording integrity](20-capture-recording-integrity.md),
[22 — Threading and performance](22-threading-performance.md), and the refuted
reader race in [verified-non-issues.md](verified-non-issues.md).

## 9. Presenter fence waits can prevent UI recovery indefinitely

**Status:** Implemented. Original evidence: conditional recovery risk. **Priority:** P1 if a fence stalls.

**Implementation:** Presenter fence waits have a 1000 ms deadline with removal
checks and verified completion after wakeups. Frame-latency waits cap at 100 ms.
CUDA drains also use bounded completion queries. Terminal faults stop submission
and preserve the full in-flight resource graph until process exit; app boundaries
report restart requirements. `fence_wait` covers completion, spurious wakeups,
deadline expiry, removal sentinels, and wait failure. Driver calls themselves
remain subject to Windows/vendor behavior; tests do not force real device hangs.

**Problem:** `D3D12Presenter::WaitForGpu()` and `WaitForFenceValue()` use
`WaitForSingleObject(..., INFINITE)`. These helpers are reached from presentation,
resource changes, and idle/shutdown operations. The separate frame-latency wait
has a timeout, but the following fence wait does not.

**Impact:** A fence that never reaches its requested value can block the UI and
its recovery paths. This review did not reproduce such a fence failure.

**Evidence:** [presenter.cpp](../src/d3d12/presenter.cpp), `WaitForGpu` and
`WaitForFenceValue`, starting at lines 1663 and 1674; `WaitForFrameSlot` follows.

**Suggested correction:** Add a deadline and a device-failure/recovery decision.
Do not treat timeout as successful completion or reuse resources still owned by
in-flight work. Resource teardown and shared CUDA/D3D12 fence recovery must follow
the same ownership policy.

## 10. Direct GPU camera transfer synchronizes the processing stream every frame

**Status:** Implemented. Original evidence: performance opportunity; the previous
synchronization protected correctness. **Priority:** P2.

**Implementation:** An event-backed `GpuCopyLease` holds the producer session
until the asynchronous CUDA copy completes. Pending work prevents producer reuse;
query/record failure or 1000 ms expiry retains ownership and stops GPU processing.
Nonblocking polling replaces per-frame stream synchronization. Shutdown releases
only after producer/consumer completion; unknown quiescence uses explicit
allocation-free import abandonment. `gpu_copy_lease` verifies retention/reuse
decisions. No camera throughput or latency improvement is claimed without a
separate live measurement.

**Problem:** The direct camera import path copies the converted camera texture
device-to-device, then calls `cudaStreamSynchronize(stream_)`. That stream also
carries earlier processing and the graphics-fence wait, so the UI can wait for
more than the camera copy itself.

**Impact:** This restricts CPU/GPU overlap and can make camera processing sensitive
to preceding GPU work. Its actual cost has not been measured in this review.
The wait is currently required because MediaCapture reuses one BGRA conversion
texture; simply deleting it would allow the producer to overwrite pixels while
CUDA is reading them.

**Evidence:** [cuda_interop.cpp](../src/cuda/cuda_interop.cpp), camera
external-memory copy and synchronization at lines 2805–2822; the shared-fence wait
at the start of `CudaInteropSurface::ProcessFrame`.

**Suggested correction:** Consider a bounded conversion-texture ring with
explicit per-slot completion or shared-fence ownership. Reuse a slot only after
CUDA has finished reading it, and keep queue pressure bounded.

Related: [30 — External-memory capture bridge](30-external-memory-capture-bridge.md).
Its existing async-fence follow-up already tracks this direction; use that plan
for implementation rather than creating a competing synchronization design.
