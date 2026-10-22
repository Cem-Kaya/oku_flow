# Live RAM recap (development)

Camera view now keeps a silent rolling recap independently of Record. The same
bottom transport provides Pause, Resume, Rewind 10 seconds, timeline seeking,
Save frame and Jump to live. The source is **Live recap**, distinct from a
saved video and the current recording's on-disk Original archive. Opening a
saved video and recording preflight retain their existing ownership rules.
This feature is an unreleased implementation, not a published download.

The timeline shows its actual first/end times and selected position. Startup
first says **Buffering live video…**; the available span grows toward five
minutes. Whole independently coded segments are evicted as new video arrives.
Settings → View and navigation offers **Live recap duration (minutes)** from
one to five, default five. Its decimal-MB estimate follows the camera format:
60 MB per minute through 1080p, 120 MB above it; before a camera format is known
it shows the range. The estimate covers compressed video history only, with
additional working memory stated separately. Reducing duration immediately
trims history; increasing it fills from retained/new video without inventing
evicted frames. The preference persists independently of camera and zoom intent.
If shrinking evicts the selected scene's time, that bounded pin remains a
paused hold at its true old position outside the new seek window. The numeric
readout and accessibility description retain that position; the scrubber hides a
misleading clamped playhead. Resume or a seek enters actual retained history.
The old pin still counts toward the two-pin limit. Numeric RangeValue is absent
while the held/live position is outside the seek window; ordinary timeline
actions still seek within actual bounds and Jump to live remains available.
Encoder pressure, real camera stalls and codec unavailability are not replaced
by duplicate frames or invented history. A byte-limited shorter span remains
visible as its actual bounds. Jump to live waits for a new camera presentation.

## Encoding and memory ownership

The clean Original producer supplies capture-owned pixels before stabilization,
temporal hold, UCT, appearance and viewport zoom. The camera keeps producing
Originals while another scene is paused or replaying. No microphone starts for
recap, and no playback audio endpoint is opened. Normal recording still uses
its independent writer, microphone consent, paths, archives and finalization;
recap never replaces or edits those files.

`RamVideoRecap` owns one native worker and a latest bounded input queue. The
worker reuses `VideoRecorder::StartMemory`, with H.264 video-only output into
first-party seekable heap byte streams. Each approximately two-second segment
starts a new encoder/access point and becomes readable only after successful
Finalize, release of every mutable cursor, and immutable Seal. Encoder setup,
encoding, native release and finalization run on this worker. The app never
joins it to pause, seek, stop a camera or shut down. A stuck native call retains
its complete owned closure and reports unavailable; capture continues.

The reviewed policy preserves native resolution through 3840×2160 (including
portrait): at most 30 FPS, with 8 Mbps through 1080p-equivalent pixel count
and 16 Mbps above it. The owner's later instruction superseded the initial
15 FPS high-resolution proposal. Sustained 4K30 requires measurement;
unsupported/odd formats do not
silently downscale. Admission decimates actual captures; delivery-clock PTS
preserves fractional timing and gaps, with no synthetic repeated frames.
RAM CPU input uses the existing first-party BGRA-to-NV12 conversion on its
worker and disables automatic native converters only on the CPU route, where
hardware transforms are disabled. The GPU route already supplies native NV12
and keeps its hardware-enabled attributes without DISABLE_CONVERTERS. A native VideoProcessor's
default frame-rate conversion otherwise duplicates pictures across holes even
with low-latency encoding. Sample durations retain the real successive PTS
differences required by fMP4 timing, including fractional ticks and holes;
holding one real picture across a hole never adds a new acquisition.
See [Windows frame-rate conversion](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-xvp-disable-frc).
Capture admissions retain exact 100-ns delivery timestamps. Native fMP4 uses
its own measured track timebase (30 kHz in the local codec fixture), so native
PTS can differ by one container tick plus integer conversion rounding. The
player exposes the real decoded PTS; it does not claim an arbitrary 100-ns
capture value is exactly representable in that container. Native fixtures read
mdhd directly from RAM and check every real picture, monotonic PTS, capture
holes and total drift against that rational timebase.

Limits are 768 MiB compressed ring storage, 8 MiB per active segment, two pinned
segments and at most 160 ring segments (normally about 150 for five minutes).
Empty/oversized sources are refused, including through the public ring API.
Input limits are
two pending raw inputs / 96 MiB, and one worker input. Decoded media
retains the existing independent 96–256 MiB CPU / 256 MiB GPU quota. Manual
Freeze retains one bounded scene plus its one in-flight readback; live clean
Original retention keeps its existing 96 MiB limit. Codec/driver working
allocations and existing CUDA scene/model allocations are separate limits.
At the target rates, 300 seconds of coded payload is approximately 300/600 MB;
container/allocation overhead and encoder bursts still count toward the cap.
Actual memory and sustainable cadence require measurement, not just bitrate
arithmetic. New encoders/finalization can cause counted gaps; this design makes
no gap-free or latency promise.
Seal compacts each finalized segment to its exact retained allocation before
ring admission. Its bounded transient old-plus-new backing is at most 16 MiB;
charging an unused 8 MiB reservation for every segment would shorten history.

There are no application-created temporary video, index or cache files,
disk-spill path, memory-mapped file or RAM disk. Compressed backing is ordinary
owned heap storage; read cursors share those exact bytes. Ring eviction removes
whole independently playable segments, while at most two consumer pins keep
evicted bytes alive. Copying a source lease retains its pin too.
The private lease wrapper passes that same counted holder into every read
cursor and clone factory. Dropping source/segment metadata cannot return pin
credit while a native cursor still retains the evicted compressed bytes.

## Playback and exact Freeze

`VideoPlayback::OpenMemory` uses the existing Media Foundation worker, generation
cancellation, native timestamps, decoded quotas, GPU/CPU fallback, exact
decoder-issued frame leases and real following-neighbour policy (distinct PTS,
at most 250 ms). Opaque memory backing proves document identity; no Windows
file ID is manufactured. The app translates segment-local PTS to the recap's
capture timeline and switches to the next available segment at playback end.
An obsolete seek cannot publish into a new request/source.

Manual live Freeze works before the first coded segment exists. A successful
camera presentation retains the actual full scene texture/pixels and committed
view transform in a bounded private lease. This is the scene before native
chrome, Draw and UCT glyph windows. A newer deferred camera frame cannot replace
it. The frozen scene is already processed, so replaying it applies no second
stabilization, temporal blend, neural enhancement or appearance treatment.
Pan/zoom works over those retained scene pixels. Failed retention is explicit;
it never quietly substitutes an older image.
An oversized newer scene also revokes the previous Freeze lease. Hidden live
camera processing cannot make an unchanged held scene resample continuously.
The display-only recipe retains committed focus-marker coordinates and CPU/GPU
sampling mode. Rotation/pan/zoom sample those same held pixels without another
appearance pass. A neural ROI cache is retained at its actual scene extent and
ROI-relative transform; this exact scene hold cannot recreate camera regions
outside that cache. Coded clean Original recap retains the full camera extent.

An exact scene hold and a coded Original are separate authorities. A composited
frozen scene has no invented independent acquisition, encoded PTS or neighbour;
UCT replacement is suppressed while that scene is held. Seeking into coded
recap restores the existing full-scene immutable/advancing media UCT policy,
using actual decoded pixels and genuine neighbouring samples. Resume after a
manual live scene hold advances to a genuine coded sample at/after its capture
time once that segment is readable, without relabelling the exact held pixels
as a lossy decoder sample. Jump to live and Return restore saved camera
orientation, framing, zoom and centre/control intent.

Camera stop, source/format/FPS/orientation changes or backward delivery time
clear the old rolling authority and cancel its pending generation. Old pinned
pixels can remain owned until their bounded consumers retire, but cannot become
the new source's history. Only the duration preference persists, never media;
stopping/restarting the camera starts filling a new window.

## Validation boundary

Production seams are registered as `video_memory_stream_contract`,
`video_memory_native_h264`, `video_recap_contract`, `video_recap_native_codec`,
`video_recap_native_4k30`,
and additions to `video_transport_contract` / the opt-in actual-app offscreen
fixture. They cover stream bounds/sealing/cursors, native silent H.264 and
keyframe/PTS evidence, whole-segment time/byte eviction and pinned leases,
generation cancellation, original capture gaps, actual Freeze during warmup,
return-live intent and continued buffering during a held consumer lease.
The finite 65-frame 4K30 native fixture prints process working/private committed
memory and peak working/committed memory around encode/decode. These process
measurements include fixture/runtime/codec allocations and are separate from
ring capacity; they do not measure a filled five-minute history.
It verifies actual compressed sample count and capture-gap PTS after more than
one nominal segment of admissions, without claiming real-time camera throughput.

On 2026-10-22 the isolated CPU app and seven necessary fixture targets built
with at most two compiler jobs. The tracked gate passed **11 of 230** cases,
Parallel 1 / GUI excluded: the six headless settings/app/ring/stream/transport/
shared-source cases and five serial native codec/playback/audio cases. This is
a scoped pass, not the full release gate. `agent_check.ps1` correctly requires
the full matrix because CMake/test-impact infrastructure changed; its selection
exit 2 is not a test failure or scoped acceptance. PCH ON was used; PCH OFF,
shipping/CUDA integration and the complete matrix remain parent-owned.

The 65-frame 3840×2160 / 30 FPS synthetic native check completed encode/decode
in 2367 ms, retaining 4,387,854 compressed bytes with exact matching capacity.
All 65 captured pictures produced exactly 65 compressed pictures. The actual RAM
mdhd timebase was 30,000 Hz; native signed PTS error stayed between -334 and 0
100-ns units, within one measured container tick plus integer conversion
rounding. Every raw capture/native timestamp is retained in the gate receipt.
This finite burst does not establish sustained physical camera FPS.

That separate 4K fixture process peaked at 562,413,568 bytes working set and
590,290,944 bytes private commit. After the first decoded-frame receipt it used
535,285,760 bytes working set / 557,035,520 bytes private commit. Its source was
4,387,854 bytes; the player snapshot reported 33,177,600 retained decoded bytes,
66,355,200 peak decoded bytes and 66,355,200 decode-working bytes. Snapshot
categories overlap process totals; remaining codec/runtime allocations were
not individually attributed. No filled 768 MiB ring or paused app scene was
part of this process. These are measured short-4K-fixture values, not a measured
five-minute 1440p footprint. The latter's compressed estimate remains exactly
16 Mbps × 300 s / 8 = **600 decimal MB**, plus separate working allocations.

The asynchronous small-frame producer fixture completed two segments with zero
counted drops and a maximum native call of 1802 microseconds while a consumer
retained an older lease. This is not a camera-load or long-soak measurement.
`media_writer_codecs_aac` also passed with synthetic PCM and actual H.264/AAC
and AV1/AAC. Its opt-in three-second gap retained all 90 video frames; the
decoded audio/video marker difference was -6333 100-ns units (0.6333 ms).
Marked audio fixtures were retained privately, without opening endpoints.

First failures remain archived: stale AUTOMOC output and a fixture declaration
compile error; app-fixture display readiness; MF adapter cached cursor bounds;
recap/file timeline refresh separation; automatic native frame-rate conversion;
and strict-100-ns container assertions before measuring mdhd quantization.
Only changed inputs were rerun. Final pre/post gate hashes match for source,
executables and compiled catalogs. Evidence is under the parent's approved
`local-work-data/live-ram-recap-t3-3a432869` directory; no evidence is shipped.
These synthetic/offscreen checks do not establish native GUI focus/layout,
screen-reader, physical camera quality, five-minute real-camera soak, hardware
RAM encoder/decoder behavior, installer or release acceptance. Desktop acceptance remains
parent-owned. New source keys have a separate complete model
review record in [localization review](live_ram_recap_language_review.md).
