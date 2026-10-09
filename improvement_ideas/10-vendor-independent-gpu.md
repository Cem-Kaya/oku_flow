# Vendor-Independent GPU Backend Plan (2026-07-22)

Goal: run the full processing pipeline on ANY DX12-capable GPU (AMD/Intel/
NVIDIA), retiring both the NVIDIA-only constraint and the pure-CPU build.

## Why D3D12 compute (not Vulkan/OpenCL)
- Presentation is already D3D12: a compute backend writes the SAME output
  texture with native UAV access and native fences — the entire CUDA external
  memory/semaphore interop layer vanishes for this backend.
- NIS (NVIDIA) and FSR 1.0 (AMD) are published as HLSL originally — our CUDA
  versions were ports; back-porting is nearly free.
- Vulkan would add a second windowing/interop story on Windows; only worth it
  if the Linux port (separate plan) lands first.

## Architecture
1. **`IGpuBackend` interface** extracted from the current CudaInteropSurface
   contract — `ProcessingInput`/`ProcessingSettings` are already backend-
   neutral by design. Methods: Ensure(size/format), ProcessFrame(input,
   settings, fence), Reset*(stabilization/keystone/temporal), teardown.
2. **`CudaBackend`** = today's code, unchanged (keeps Maxine premium tier and
   any TensorRT ML tier — those stay NVIDIA-only features).
3. **`D3d12ComputeBackend`** (new): HLSL compute shader per stage, compiled
   offline (DXC → .cso baked into the exe or qrc). Ping-pong UAV buffers
   mirror the CUDA design; small stats buffers (histograms, projections,
   motion state) are UAVs with shader atomics; the "no per-frame readback"
   discipline carries over identically (focus/keystone stats via tiny
   copy-to-readback-buffer polled by fence, like the async readback ring).
4. **Backend selection** at startup: NVIDIA+CUDA OK → CudaBackend (feature
   superset); else D3d12ComputeBackend; `OKUFLOW_FORCE_BACKEND` override for
   testing. Status label shows active backend.
5. **CPU build deprecation**: once the D3D12 backend reaches parity, remove
   the `msvc-cpu` preset and CPU effect stages entirely (CPU keeps only
   capture-format conversion fallback + debug composite, or drop debug view).
   Every Windows machine with any DX12 GPU (incl. Intel iGPUs) gets
   acceleration; SSE/AVX work is explicitly NOT worth it.

## Port order (stage difficulty)
- Wave A (scaffold + basics): conversion NV12/YUY2→BGRA, rotation, zoom,
  color grade, brightness/contrast, B&W → a usable magnifier on any GPU.
- Wave B: NIS + FSR (drop-in vendor HLSL), Gaussian blur, temporal smooth.
- Wave C: stabilization (projections via atomics + tiny reduce), keystone
  warp, auto-contrast histogram.
- Wave D: text clarity stack (box-stats Sauvola, CLAHE, stroke, hysteresis).
- Parity gate: pixel-compare CUDA vs D3D12 outputs on golden frames (tolerance
  ±2/255); perf gate: 1080p full stack ≤8 ms on an Intel Iris Xe class iGPU
  for the basic stack, discrete AMD for the full stack.
- ML tier on non-NVIDIA: ONNX Runtime + DirectML EP for RLFN later (08-plan).

## Effort estimate
Wave A ≈ one agent-day (the plumbing is the work); B–D ≈ one agent-day each
given the CUDA kernels as reference. The `dx12_cuda_minimal` harness idea from
run_minimal_test.bat becomes real here: make it a `d3d12_compute_selftest`
target that runs each shader on a test image headlessly — first actual
automated GPU test in the repo.
