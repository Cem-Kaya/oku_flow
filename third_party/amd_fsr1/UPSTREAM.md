# AMD FidelityFX Super Resolution 1.0

- Upstream: <https://github.com/GPUOpen-Effects/FidelityFX-FSR>
- Version: 1.0.2
- Commit: `a21ffb8f6c13233ba336352bdff293894c706575`
- License: MIT; see `LICENSE.txt`
- Imported files: `ffx_a.h`, `ffx_fsr1.h`, and the upstream license

OpenZoom's CUDA adaptation is implemented in
`src/cuda/spatial_upscalers.cu`. It preserves the reference FP32 EASU and
RCAS equations while replacing shader-language callbacks and resource access
with pitched CUDA BGRA8 loads and stores.
