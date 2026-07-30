# Plan 30 — The External-Memory Capture Bridge (CUDA External Resource Interoperability)

Status: **IMPLEMENTED 2026-07-29 — EXTERNAL-MEMORY ×100 GATE PASSED;
long hardware validation and the async fence upgrade remain.** This plan
documents the replacement of the legacy
CUDA↔D3D11 graphics-interop API in the capture path (the plan 28 stage 4
shutdown-crash blocker), names the APIs involved, records the as-built
recipe with code anchors, and tracks what is still open. It supersedes the
"Stage 4 blocker" directive in plan 28, which led to this implementation.

## The API names (the question this plan answers first)

**Gen 1 — what crashed — "CUDA graphics interop"** (CUDA 3.x era, ~2009,
header `cuda_d3d11_interop.h`, maintenance-only in modern drivers):
`cudaGraphicsD3D11RegisterResource`, `cudaGraphicsMapResources`,
`cudaGraphicsSubResourceGetMappedArray`, `cudaGraphicsUnmapResources`,
`cudaGraphicsUnregisterResource`. The last one faults inside
`nvwgf2umx.dll` on the installed driver after a VideoProcessor-written
texture was used (analysis in plan 28: every documented precondition was
verified satisfied).

**Gen 2 — what we use now — "CUDA External Resource Interoperability"**
(CUDA 10.0, 2018, part of the runtime API; the same explicit-sharing model
as Vulkan's `VK_KHR_external_memory_win32`):

- Memory: `cudaImportExternalMemory` (+ `cudaExternalMemoryHandleDesc`,
  handle types incl. `cudaExternalMemoryHandleTypeD3D12Resource`,
  `...D3D12Heap`, `...D3D11Resource`, `...OpaqueWin32`; flag
  `cudaExternalMemoryDedicated`),
  `cudaExternalMemoryGetMappedMipmappedArray`,
  `cudaGetMipmappedArrayLevel`, `cudaFreeMipmappedArray`,
  `cudaDestroyExternalMemory`.
- Synchronization (the phase-2 upgrade): `cudaImportExternalSemaphore`
  (`cudaExternalSemaphoreHandleTypeD3D12Fence` — shared `ID3D11Fence`
  objects interoperate through this type; same kernel object),
  `cudaWaitExternalSemaphoresAsync`, `cudaSignalExternalSemaphoresAsync`,
  `cudaDestroyExternalSemaphore`.

**Windows side:** `D3D11_RESOURCE_MISC_SHARED |
D3D11_RESOURCE_MISC_SHARED_NTHANDLE` at texture creation,
`IDXGIResource1::CreateSharedHandle` (NT handle),
`ID3D12Device::OpenSharedHandle`, `ID3D12Device::CreateSharedHandle`,
`D3D12_RESOURCE_ALLOCATION_INFO` via `GetResourceAllocationInfo`; later
`ID3D11Device5::CreateFence(D3D11_FENCE_FLAG_SHARED)`.

This codebase already ran Gen 2 in production for the presenter surface and
the SuperRes cache (`cuda_interop.cpp:647-691`, `:733-771`) — the capture
bridge joins that same pattern; Gen 1 survives only in an isolated
diagnostic path.

## The as-built recipe (verified in-tree 2026-07-29)

Why not the "obvious" direct D3D11 import: the
`cudaExternalMemoryHandleTypeD3D11Resource` description requires the
**allocation size**, and D3D11 has no public allocation-size query —
guessing invites exactly the undefined behavior that produced the original
crash. The implemented route measures the allocation with D3D12 and imports
with strictly documented handle typing:

1. The VideoProcessor's BGRA conversion texture is created shareable:
   `MISC_SHARED | MISC_SHARED_NTHANDLE`, `BIND_RENDER_TARGET |
   SHADER_RESOURCE` (`src/capture/media_capture.cpp:1206-1221`). The VP
   output view is created directly on the shared texture (accepted by the
   current driver; the CopyResource fallback from plan 28 remains the
   documented plan-B if another driver refuses).
2. Import, once per texture identity
   (`src/cuda/cuda_interop.cpp:2589-2686`):
   D3D11 texture → `IDXGIResource1::CreateSharedHandle` →
   `d3d12Device_->OpenSharedHandle` → **desc validation** (dimension,
   width/height, format must match the D3D11 allocation) →
   `GetResourceAllocationInfo` for the true size → re-share from D3D12 →
   `cudaImportExternalMemory` (`D3D12Resource` type, `Dedicated` flag) →
   `cudaExternalMemoryGetMappedMipmappedArray`
   (`cudaArraySurfaceLoadStore | cudaArrayColorAttachment`) →
   `cudaGetMipmappedArrayLevel` level 0 cached. Both NT handles are closed
   immediately after import.
3. Per frame: exactly one `cudaMemcpy2DFromArrayAsync` from the cached
   level-0 array into the existing pitched processing buffer, then a stream
   drain before MediaCapture's next `VideoProcessorBlt` reuses the texture.
   **No map/unmap driver calls per frame** — the Gen-1 pair is gone.
4. Teardown (`ResetCaptureInterop(bool atProcessExit)`,
   `cuda_interop.cpp:543-604`): stream + device sync →
   `cudaFreeMipmappedArray` → `cudaDestroyExternalMemory` → ComPtr
   releases. Deterministic, order-safe, and decoupled from MediaCapture's
   D3D11 teardown — the imported allocation is held by its own kernel
   reference.
5. The plan-28 driver-workaround also landed: if a *legacy* registration
   exists at process exit (diagnostic path only), it is deliberately
   retained — registration **and** texture ComPtr together, honoring
   CUDA's registered-resource lifetime contract — and reclaimed by Windows
   at process teardown (`:561-571`). The production camera path never
   creates a legacy registration.

## What this bought (from the plan-28 analysis)

Crash API never called again; two driver calls removed from the per-frame
hot path; lifetime decoupling that makes the teardown-ordering bug class
structurally hard to write; one interop model across capture, presenter,
and SuperRes; driver-risk moved from the untested legacy path to the path
every modern engine exercises; and the prerequisite for GPU-side
synchronization, which Gen 1 could never do. Explicitly not bought:
frame-rate (camera-bound) or visual change.

## Remaining work

### R1 — Hardware validation (⛔ owner, closes the plan 28 blocker)

- **Passed 2026-07-29:** 100 consecutive app
  launch -> external-memory stream -> normal close cycles on the physical USB
  webcam. All 100 imported the external texture, all 100 exited with code 0,
  seven exercised the delayed-query retry, none disabled the top rung, and
  Application Error / WER contained zero `open_zoom.exe` faults.
- **Passed 2026-07-29:** long production moving-picture inspection on the
  physical camera. The owner reported flawless output with no visible tearing
  or stale frames.
- Camera switching between DroidCam / USB webcam / Pixel (black-frame
  fallback camera) under the new import; force-fail via
  `OPENZOOM_FORCE_CAPTURE_COPY_RUNG=1` degrades with one announcement.
- Camera-switch and device-removal checks remain. The 1000+ moving-frame
  tearing/staleness portion of plan 28 gate 3 passed on 2026-07-29.

### R2 — Legacy repro artifact (agent, half a day)

**Implemented 2026-07-29.** `sandbox/mf_dxva_minimal` exposes two explicit,
watchdog-isolated modes:

- `--interop external --iterations 100` exercises the production
  D3D11 -> D3D12 -> CUDA import, device copy, and ordered destruction.
- `--interop legacy --iterations 100` exercises the Gen-1
  register→map→unmap→unregister sequence as an opt-in NVIDIA reproduction;
  it is never invoked by normal probes or automated build gates.

The USB webcam external route passed 100/100 iterations at 0.684 ms average;
the deployed release-bundle probe repeated 100/100 at 0.747 ms average.
The dangerous legacy mode was built but deliberately not run during routine
validation; a crash/no-verdict is classified by the parent watchdog rather
than taking down OpenZoom.

### R3 — Phase 2: fence-based async handoff (later, do not rush)

Replace the two CPU waits (D3D11 event query before the CUDA read; stream
drain before VP reuse) with a shared fence:
`ID3D11Device5::CreateFence(D3D11_FENCE_FLAG_SHARED)` →
`cudaImportExternalSemaphore(cudaExternalSemaphoreHandleTypeD3D12Fence)` →
VP blit signals N, CUDA stream waits N / signals N+1, D3D11 waits N+1
before reuse. Zero CPU stalls in the handoff; lower latency jitter; kinder
to battery (idea 29-E). Sequencing rule: only after R1 passes and one week
of real-lecture soak on the synchronous version — one capture-path change
at a time, per the lesson this very plan exists to record.

### R4 — Bookkeeping

Fold the plan 28 stage-4 status and failure-mode table onto this recipe;
close 28 once R1 passes and gate 2 (lecture-length session) is done. Plan
16's fence postmortems remain required reading before R3.

## Acceptance

R1 numbers recorded here; legacy repro loop crashes and external-memory
loop is clean (R2); after R3, capture→present jitter measurably narrows
under `OPENZOOM_CAPTURE_DIAGNOSTICS=1` with no correctness regressions on
the R1 matrix.
