# NVIDIA Image Scaling SDK

- Upstream: <https://github.com/NVIDIAGameWorks/NVIDIAImageScaling>
- Version: 1.0.3
- Commit: `35e13ba316c98eeecf16f37eae70ce88019911f6`
- License: MIT; see `LICENSE.txt`
- Imported files: `NIS_Config.h`, `NIS_Scaler.h`, and the upstream license

OpenZoom's CUDA adaptation is implemented in
`src/cuda/spatial_upscalers.cu`. It uses the upstream host configuration,
64-phase scaler and USM coefficient banks, edge detector, four directional
filters, and adaptive sharpening equations. Shader texture/group primitives
are replaced with pitched CUDA BGRA8 loads and stores.
