# capture module

This module owns Media Foundation camera discovery and frame acquisition.

Current responsibilities:
- enumerate available video devices
- list native capture modes for the selected device
- stream frames on a dedicated capture thread via `IMFSourceReader`
- create an optional D3D11/DXGI device manager for Media Foundation hardware
  transforms, retain validated DXGI-backed samples as GPU-resident
  `MediaFrame` objects, and use a reusable D3D11 VideoProcessor BGRA target for
  D3D11 → D3D12 → CUDA external-memory import. Each queued frame pins its
  originating `IMFSample`; retaining only the D3D11 texture object would allow
  the source-reader allocator to recycle and overwrite that surface.
- read a retained GPU sample back through one staging texture only for startup
  validation, original recording/photo capture, CPU fallback, or a failed
  D3D11/CUDA interop attempt
- retain a conservative system-memory compatibility mode; the application
  persists per-camera acceleration decisions and reopens this path after
  negotiation, blank-frame, or crash-marker failures
- own each `IMFActivate` session through `ShutdownObject()` so mode probes and capture restarts do not reuse a shut-down media source
- report capture-thread failures and adapt frame metadata when a device changes its current media type

CPU frame conversion and fallback handling live in `src/common/`. On the
steady-state accelerated preview, D3D11 performs NV12/YUY2-to-BGRA conversion
without crossing system memory. The target is NT-shareable; the app opens the
same allocation on D3D12 so CUDA can import it with a driver-reported allocation
size. CUDA then performs one device-to-device copy into the existing linear
working surface; "direct GPU" therefore means zero CPU-copy, not zero GPU-copy.
If the D3D11 completion query is unusually delayed, the application retains
that exact GPU-backed frame and retries it asynchronously for up to 25 ms. The
safe readback rung is used only if that bounded retry window expires. Media
Foundation frame arrival wakes the application pipeline directly; while
recording, a six-frame burst queue absorbs short processing stalls instead of
letting a same-rate polling timer overwrite pending camera frames.
