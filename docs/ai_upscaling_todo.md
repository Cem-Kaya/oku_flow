# AI Upscaling Implementation Plan

Refreshed on 2026-03-31 after auditing the current repository state.

## What Already Exists
- [x] Audit current GPU entry points: `OpenZoomApp::ProcessFrameWithCuda` and `CudaInteropSurface::ProcessFrame`.
- [x] Add a persistent UI toggle for spatial sharpening plus backend selection.
- [x] Introduce a staging format switch (`rgba8` or `fp16`) for future expansion.
- [x] Keep Gaussian blur available as a non-AI baseline path.
- [x] Document current build and licensing constraints for FSR and NIS.

## Near-Term GPU Work
- [ ] Measure end-to-end GPU latency for the current NIS and FSR-style paths at common camera resolutions.
- [ ] Add better timing and failure telemetry around CUDA interop setup and per-frame processing.
- [ ] Decide whether the shipping UI should continue exposing both NIS and FSR-style modes or narrow to a single default backend.
- [ ] Move more debug-stage visibility onto GPU-backed inspection surfaces so CUDA runs can be debugged without dropping fully to the CPU path.

## Mode 0: Baseline Filters
- [ ] Keep Gaussian blur as the simple readability baseline.
- [ ] Clarify how blur, temporal smoothing, and spatial sharpening are composed when multiple stages are enabled.
- [ ] Add regression captures for representative text-heavy scenes.

## Mode 1: Spatial Sharpen / Classic Upscaling
- [x] Wire NIS-style and FSR-style kernels into the CUDA path.
- [ ] Tune sharpness defaults against real magnification tasks instead of synthetic test images.
- [ ] Capture performance numbers for 720p, 1080p, and 1440p source sizes.

## Mode 2: Lightweight DL Super Resolution
- [ ] Decide whether OpenCV DNN is still the right first deep-learning integration point.
- [ ] Define model loading, caching, and fallback behavior before introducing new runtime dependencies.
- [ ] Document deployment impact early: DLLs, model weights, and bundle size.

## Mode 3: TensorRT-Class Backends
- [ ] Evaluate whether TensorRT is worth the added setup complexity for the intended user base.
- [ ] Specify a plan for engine caching, versioning, and GPU compatibility.
- [ ] Design watchdog and fallback behavior so failed engine initialization drops cleanly to the current CUDA or CPU path.

## Cross-Cutting Work
- [ ] Add build and test infrastructure for golden-image validation.
- [ ] Expand docs when new inference dependencies, model downloads, or licensing obligations are introduced.
- [ ] Keep the CUDA buffer-format story coherent if true FP16 processing is implemented later.
- [ ] Ensure accessibility and low-vision use cases, not benchmark scores alone, drive quality decisions.
