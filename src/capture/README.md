# capture module

Actual camera arrival cadence and presentation throughput are measured
separately from the media source's nominal format and reader output rate.

This module owns Media Foundation camera discovery and frame acquisition.

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
Stop revokes callbacks and uses one 1500 ms worker deadline plus the consumer's
bounded CUDA drain. A stalled driver keeps its session/backing alive until
process exit and requires restart. A producer lease prevents reuse or release
of the converted texture until CUDA observes its asynchronous copy completion.
