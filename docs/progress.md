# OpenZoom Project Tracker

## Current Snapshot
Updated after repository audit on 2026-03-31.

## Completed
- [x] Split the codebase into `app`, `capture`, `common`, `d3d12`, `cuda`, and `ui` modules with mirrored public headers.
- [x] Implement Media Foundation camera enumeration and per-camera mode discovery.
- [x] Build the CPU frame pipeline for conversion, rotation, zoom, blur, temporal smoothing, and debug compositing.
- [x] Bring up the Direct3D 12 presenter, including GPU texture readback.
- [x] Enable the CUDA interop processing path with CPU fallback.
- [x] Add persistent settings storage in `%APPDATA%\OpenZoom\OpenZoom\settings.json`.
- [x] Add processed photo capture and processed H.264 MP4 recording.
- [x] Add rotation-aware focus controls, joystick navigation, mouse pan, and wheel zoom/pan.
- [x] Add release-bundle scripting and a minimal validation harness entry point.
- [x] Introduce a two-stage quick-mode plus advanced-tuning UI model with promotable custom presets.
- [x] Add OCR/VLM assistive-mode scaffolding to the config model and UI.
- [x] Add a working assistive overlay plus OCR/VLM runtime hooks for live analysis.

## In Progress
- [ ] Broaden CUDA interop validation across more GPUs, drivers, and toolkit versions.
- [ ] Add real automated tests behind `OPENZOOM_ENABLE_TESTS`.
- [ ] Tighten documentation and release hygiene as the feature set expands.
- [ ] Improve OCR quality, add ROI selection, and harden VLM request/response handling across more providers.

## Upcoming
- [ ] Add richer overlays for diagnostics, accessibility, and future AI assistance.
- [ ] Expand recording and capture validation with regression fixtures.
- [ ] Continue the AI upscaling work described in `docs/ai_upscaling_todo.md`.
