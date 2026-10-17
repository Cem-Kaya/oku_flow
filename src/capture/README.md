# capture module

Actual camera arrival cadence and presentation throughput are measured
separately from the media source's nominal format and reader output rate.

This module owns Media Foundation camera discovery and frame acquisition.

It also contains the optional `PhoneCameraTransport` / `PhoneVideoDecoder`.
Phone entries are appended after physical cameras and retain stable USB serial
and optional camera-ID identities. Structured adb connection types distinguish
USB from Wi-Fi even when Windows supplies no bus address. The existing local
adb server is borrowed without replacing or killing it. If no default listener
or endpoint override exists, OkuFlow can start its own private server. Failure
of that optional server leaves normal camera discovery intact.

On first launch, an authorized phone with a successful companion handshake is
preferred. Saved webcam/phone identities and valid legacy indices take precedence.

Selecting a phone opens independent video/control channels on a private worker,
launches the installed companion, and feeds a system AVC or HEVC decoder.
`PhoneVideoCodec` identifies the compressed input. AVC uses the built-in decoder;
HEVC enumerates synchronous MFTs accepting HEVC/NV12, including the installed
Microsoft HEVC extension. This synchronous transform may internally use DXVA.
HEVC is advertised only after activation and an input/output configuration probe.
A failed HEVC decoder or four seconds without its first decoded frame closes the
session and reconnects once advertising only AVC. Phone preferences survive,
and frame sequence/timestamps remain continuous through the fallback. Hardware
output can share the capture DXGI manager and pins its samples; CPU NV12 uses
the normal buffer normalization path. Phone stop follows the same bounded
shutdown coordinator. `SetPhoneZoom` / `SetPhoneCamera` coalesce commands and
are no-ops for webcams; automatic viewport-to-phone zoom is later work.
`SetPhoneTorch` also coalesces requests. Its bounded actor overload returns
admission only for this live phone session, before split stop begins, with a
nonnull shared cancellation token and positive steady-clock millisecond deadline.
The transport stores the desired state, token, deadline and generation together;
new native/actor commands retire previous queued or dequeued actor requests.
Cancellation/expiry/generation are checked immediately before the control socket
write. Cancel, terminal failure and HEVC retry retire pending requests rather
than replaying them. Already emitted bytes may still be acknowledged after
cancellation, so queue admission never changes the reported light state; only
the phone's status acknowledgment does. Every stream connection explicitly
requests the light off; capture metadata reports actual availability and state.
The Camera settings button is enabled only when that live phone supports a light.
See [`android/PROTOCOL.md`](../../android/PROTOCOL.md) for limits and hardware gates.

Pending D3D11 conversion-query polls allow the runtime to flush submission
work, after the initial explicit End/Flush. This prevents the repeated 25 ms
handoff timeouts observed with phone hardware decoding on the tested NVIDIA
adapter. Each attempt still polls once and returns to Qt on `S_FALSE`; the
retained-frame, producer-lease and bounded fallback rules remain in force.
See [`android/DEVICE_VALIDATION.md`](../../android/DEVICE_VALIDATION.md) for
the measured comparison and its limits.

Current responsibilities:
- enumerate available video devices
- list native capture modes for the selected device
- snapshot the streaming reader's native modes during startup and resolve a
  persisted mode id on that reader, avoiding a separate activate/probe/shutdown
  cycle before the live camera starts. `NativeFormats()` belongs to the facade
  and is read only after `StartCapture` returns; an explicit `VideoFormat`
  pointer takes precedence over the optional persisted id, and a missing id
  retains automatic negotiation.
- stream frames on a dedicated capture thread via `IMFSourceReader`
- create an optional D3D11/DXGI device manager for Media Foundation hardware
  transforms, retain validated DXGI-backed samples as GPU-resident
  `MediaFrame` objects, and use a reusable BGRA GPU target for
  D3D11 → D3D12 → CUDA external-memory import. Each queued frame pins its
  originating `IMFSample`; retaining only the D3D11 texture object would allow
  the source-reader allocator to recycle and overwrite that surface.
- read a retained GPU sample back through one staging texture only for startup
  validation, original recording/photo capture, CPU fallback, or a failed
  D3D11/CUDA interop attempt
  - when startup validation has already materialized BGRA pixels, preview
    reuses those pixels through the existing CPU normalization/rotation and
    CUDA upload path instead of repeating the D3D11 texture conversion. The
    prior CUDA-copy lease still completes before reuse; validation and the
    GPU-only path after startup are unchanged.
- retain a conservative system-memory compatibility mode; the application
  persists per-camera acceleration decisions and reopens this path after
  negotiation, blank-frame, or crash-marker failures
- own each `IMFActivate` session through `ShutdownObject()` so mode probes and capture restarts do not reuse a shut-down media source
- report capture-thread failures and adapt frame metadata when a device changes its current media type
- preserve native `IMF2DBuffer2` / `IMF2DBuffer` pitch when reading CPU frames;
  `CopyCaptureBuffer` normalizes padded RGB/YUY2/NV12 and bottom-up RGB to
  tightly packed top-down frames, validates accessible bytes when available,
  and keeps NV12 luma/chroma planes aligned to the normalized stride
- distinguish CPU pitch from DXGI surface orientation: a negative negotiated
  CPU stride requires CPU normalization only for system-memory frames. It
  does not reject a GPU-resident frame or force a steady-state readback.

CPU frame conversion and fallback handling live in `src/common/`. On the
steady-state accelerated preview, D3D11 performs NV12/YUY2-to-BGRA conversion
without crossing system memory. Samples already in exact BGRA texture format
copy their visible rectangle directly from the retained array/mip subresource,
preserving channel bytes and avoiding redundant video processing. Other formats
still use the VideoProcessor conversion. The target is NT-shareable; the app opens the
same allocation on D3D12 so CUDA can import it with a driver-reported allocation
size. CUDA then performs one device-to-device copy into the existing linear
working surface; "direct GPU" therefore means zero CPU-copy, not zero GPU-copy.
Media Foundation YUV matrix/range metadata travels with each frame. Both
BT.601 and BT.709, in limited or full range, are honored in D3D11 and raw
CPU/CUDA conversions. Untagged formats default to BT.601 limited; explicitly
unsupported metadata is rejected. A D3D11 processor without full-range YUV
support falls back to the raw conversion path for that format.
Each D3D11 completion query polls once without sleeping on the viewport thread.
While GPU work is pending, the application retains that exact GPU-backed frame
and retries it through the event loop after 1 ms for up to 25 ms. The
safe readback rung is used only if that bounded retry window expires. Media
Foundation frame arrival wakes the application pipeline directly; while
recording, a six-frame burst queue absorbs short processing stalls instead of
letting a same-rate polling timer overwrite pending camera frames.

Each capture/stop worker owns a shared session independently of the UI facade.
Startup may use a separate worker-owned facade, then `Swap` it into the UI only
after startup finishes and neither facade has concurrent callers. Swapping
transfers the complete session, startup snapshots, and shutdown/abandon state;
it does not stop the camera or release producer backing.
`BeginStopCapture` revokes callbacks and denies further facade GPU/phone access
before starting one independently owned stop worker. `PollStopCapture` returns
without waiting for a driver: WaitingForQuiescence, ReadyToRelease,
WaitingForRelease, Completed or Failed. ReadyToRelease retains producer backing
until `ReleaseStoppedCapture(true)` acknowledges cleanup of the exact consumer
imports. A premature or unsafe acknowledgment fails closed. All phases share
1500 ms from Begin; worker completion timestamps prevent a delayed GUI poll
from turning an already successful stop into a timeout. The synchronous
`StopCapture` wrapper remains for legacy startup and exit paths, with its caller's
bounded CUDA drain. It cannot silently acknowledge an already pending split stop.
A stalled driver keeps its exact session/backing alive once until process exit,
requires restart, and prevents another source from starting on that facade.
A producer lease prevents reuse or release of the converted texture until CUDA
observes its asynchronous copy completion. Split-stop fixtures use scripted
workers and leases; they establish neither physical-camera nor phone acceptance.

Phone discovery runs on a separate app worker every five seconds. Startup and
webcam reconnect list only Media Foundation devices and use the cached phone
snapshot, so adb/socket delays never block camera enumeration on the UI thread.
Unknown adb results retain the prior cache. Probes report capabilities, busy and
problem without claiming the stream; active/starting phones are excluded.
`PhoneRetryPolicy` backs off failed starts, resetting on a new transport or
readiness transition. A working webcam is replaced only by a ready phone.

The raw host connection honors local TCP adb endpoint overrides. Unknown/SDK
borrowed servers remain untouched. Without an override/listener, a hidden bundled adb
server is launched directly on a private ephemeral loopback port. Its exact
process belongs to a non-inherited Windows kill-on-close job, so normal exit
and owner crash both stop it. Two consecutive unresponsive host-version probes
permit restarting only that child while no phone session is live or starting;
failed launches and crashes retain the 30-second cooldown. `PhoneAdbTestHooks` keeps contract tests off the
real server. The release bundles source-built adb and its complete dependency
runtime with notices, source and relinking inputs. See
[adb_distribution.md](../../docs/adb_distribution.md) for the pinned recipe.

Owned bootstrap requires phone-use intent; explicit webcam selection prevents
it and releases an idle owned child. WinUSB access is exclusive, so tools must
borrow the same server (its private port is logged) or start SDK adb first.
The child disables emulator scanning. Native IPv4 listener ownership is checked
against its exact process before readiness. A dead child can yield to a newly
available default server. Counted occupancy protects overlapping sessions, and
a new `Run` repairs an idle server before registering its own reservation.
Owned startup/recovery uses automatic-retry guidance in all 25 UI languages.
Shutdown prevents later bootstrap and interrupts manager polling. Lifecycle
lock waits are cancellable; live-child port snapshots are nonblocking and do
not substitute for readiness. Server epochs combine port/PID/creation time so
reused transport IDs cannot suppress renewed launch or recovery attempts.

Two default-host failures spanning five seconds permit narrowly proven legacy
OkuFlow bundle reclamation (manifest, adb image and launcher identity, exact
held process, no sibling/session, serialized and revalidated before effects).
Unknown listeners instead get an isolated shadow server; empty raw transport
listing plus two healthy default probes permit handback. BorrowOnly discovery
never retires or restarts support; explicit webcam intent only retires idle support.

Non-protocol phone failures and preferred idle discovery may perform a guarded
USB transport reset. A responsive host version/listing and a timed-out separate
device echo distinguish transport stalls from camera/decoder problems. The
same authorized USB attachment must remain listed afterward. Only that transport
is reconnected, at most once per phone/server per minute; a new transport ID
does not bypass this limit. Pause/busy/error codes, cancellation, unrelated
phones and unknown shared server ownership remain intact. See the deliberate
reviewed-legacy exception and WinUSB ownership limits in adb_distribution.md.

`AudioCapture::Start` and `Stop` are synchronous worker-only primitives. Stop's
three-second reader/Flush wait does not bound subsequent driver Shutdown or COM
release. The application now owns their complete lifetime on a microphone worker
and preserves it after a UI deadline. The Flush helper initializes its own MTA
and retires its reader reference before publishing completion.
