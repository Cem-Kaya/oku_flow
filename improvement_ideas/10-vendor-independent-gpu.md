# GPU effects on AMD and Intel

State: **not implemented; separate strategic project**.
Baseline: Windows 0.3.1; reviewed 2026-10-24.

D3D12 presentation already works. The image-enhancement pipeline still uses
CUDA; CPU OCR does not make Text Clarity, stabilization or other CUDA effects
run on AMD/Intel GPUs.

## Proposed direction

- Extract a narrow processing-backend interface while retaining existing frame,
  transform, fence and retirement ownership.
- Add a D3D12 compute backend for basic conversion, rotation, view transforms
  and colors before porting the expensive enhancement stages.
- Retain the existing CUDA/TensorRT/Maxine route for supported NVIDIA hardware.
- Evaluate a non-NVIDIA OCR acceleration runtime separately; ORT CPU fallback
  already exists and should remain usable.

Do not remove CPU compatibility or promise parity before implementation and
real-device measurements. Old claims that the port is nearly free or fits a
few agent-days were unmeasured estimates and have been removed.

A first useful milestone is a readable, responsive basic magnifier on an Intel
laptop; full stabilization/UCT/Maxine-equivalent coverage is a different scope.
New runtime/model dependencies require their own license and payload review.
