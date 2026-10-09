# Plan 15 - Aspect-safe, high-refresh viewport (2026-07-23)

Status: **Implemented and basic Windows-smoke verified on 2026-07-23;
multi-monitor/DPI acceptance and the 2560x1440 timing gate remain pending.**
The CUDA and CPU presentation paths now share one aspect-safe transform, and
camera processing is separated from cached-scene viewport presentation.

Implementation notes:

- `ViewTransform` is the canonical Fill/Fit, zoom, focus, CPU sampling, D3D12
  shader, focus-marker, capture, and SuperRes-ROI geometry model.
- The swap chain follows native HWND client pixels and camera-sized texture
  presentation no longer resizes it. Splitter changes are coalesced before a
  resize.
- `PipelineOrchestrator` owns the active/idle viewport clock, elapsed-time
  motion with nanosecond tick precision, monitor-rate clamping, one-shot clamp
  notice, and diagnostics. The blocking no-fence fallback is deliberately
  clamped to camera rate.
- Camera-clock work publishes a persistent CPU or CUDA scene. Viewport-only
  ticks re-present that scene without advancing temporal effects, SuperRes,
  recording, OCR, or assistive analysis.
- A deterministic checkerboard/circle regression test covers 16:9, 4:3, 1:1,
  9:16, and 21:9, with a separate odd-rotation dimension check.
- The CUDA application completed a 45-second live-camera smoke run, remained
  responsive, and exited normally through the window close path with code 0.
- SuperRes output is a generation-tagged cached ROI. It is sampled only when
  it contains the requested view; otherwise the registered conventional scene
  is shown without cross-fading different geometry.
- The CUDA/D3D12 shared fence is strictly monotonic across both clocks:
  graphics reservations start above the presenter's internal frame-slot value,
  and the actual post-present value is adopted after every draw. Blocking
  on-demand OCR/Assistant readback queues a wait for the latest CUDA signal
  before copying.
- Simple-mode activity uses a deadline-only fast path after chrome is visible;
  it no longer moves and raises four native tool windows for every Qt and
  Win32 mouse notification.
- Deliberate limits: SuperRes ROI overscan remains the plan's later
  optimization; exact camera-mode selection remains a separate follow-on.
  Swap-chain resize events are coalesced, but the final resize still drains
  resources because deferred back-buffer retirement was not safe to introduce
  without a dedicated resize queue.

This plan fixes those problems together because both require the same missing
boundary: OkuFlow needs a persistent processed scene texture and a separate,
viewport-sized presentation pass.

## User-visible goals

1. A circle in the camera image always remains a circle. Resizing the window,
   moving the Advanced splitter, rotating the camera, or changing monitors must
   never stretch or squeeze either axis.
2. The default viewport policy is **Fill (crop)**: fill every pixel of the
   camera area while preserving aspect ratio, cropping excess content equally
   around the current focus point. An optional **Fit (show all)** policy may
   letterbox, but it must also preserve aspect ratio.
3. Pan, joystick motion, mouse drag, and animated zoom can update at a selected
   viewport rate up to 120 FPS or the display's supported refresh rate, even
   when the camera supplies only 30 FPS.
4. Camera content remains truthful. A 30 FPS camera is still 30 FPS; this plan
   makes navigation over the latest processed frame smoother and does not
   synthesize intermediate camera frames.
5. The setting is dynamic and global, not stored in individual image presets.

## Confirmed current behavior and root cause

### Aspect-ratio regression

- `ComputeViewMapping` in `src/app/app.cpp` already contains correct uniform
  crop-to-fill / fit math. `PresentFitted` uses it to create a buffer matching
  the render widget dimensions, so the CPU/passthrough path preserves aspect.
- The main raw NV12/YUY2 CUDA path does not use that mapping. In
  `TryProcessRawFrameWithCuda`, the CUDA surface is allocated at the
  post-rotation camera extent (for example 1280x720), and `RunCudaPipeline`
  passes that same camera extent to `D3D12Presenter::PresentFromTexture`.
- `PresentFromTexture` resizes the swap-chain buffers to the texture extent,
  copies with `CopyResource`, and the swap chain is configured with
  `DXGI_SCALING_STRETCH`. Meanwhile `RenderWidget::resizeEvent` tries to resize
  the same swap chain to the actual widget extent. The next camera frame can
  resize it back to the camera extent. DXGI then stretches that back buffer to
  the HWND client rectangle, producing the visible horizontal or vertical
  distortion in narrow, wide, and rotated layouts.
- The result also causes resize churn: the Qt resize path and per-frame CUDA
  present path can fight over swap-chain dimensions.

The fix is not to alter `DXGI_SCALING_STRETCH` alone. The back buffer must always
match the render widget's native-pixel client size, and presentation must render
the camera texture into that back buffer with a single uniform mapping. Never
use the window compositor as the camera-image scaler.

### Camera FPS currently gates viewport motion

- `frameTimer_->start(16)` requests an approximately 62.5 Hz Qt tick.
- `RunFrameTick` applies input forces, consumes `latestFrame_`, and returns
  before processing or presenting when no new camera frame exists.
- Therefore holding a pan key or joystick changes the visible crop only when a
  new camera frame is available. A 30 FPS camera limits pan/zoom motion to about
  30 visible steps per second regardless of timer frequency.
- `D3D12Presenter` calls `Present(1, 0)`, so final presentation is also paced by
  the active monitor's refresh rate. Window dragging itself is handled by the
  Windows compositor and is independent of the camera pipeline.
- `MediaCapture::ConfigureReader` currently requests only a pixel subtype. It
  does not request an exact `MF_MT_FRAME_SIZE` or `MF_MT_FRAME_RATE`, and its
  private `FrameFormat` does not retain the active frame-rate numerator and
  denominator. The "Available modes" list is therefore informational, not the
  selected active capture mode.

## Required architecture: two clocks, one geometry model

### Clock A - camera and processing

Run only when a fresh camera frame arrives:

1. Capture and format conversion.
2. Stabilization, temporal smoothing, keystone tracking, text hysteresis,
   auto-contrast histories, and all other stateful effects.
3. Expensive processing such as NVIDIA Video Effects Super Resolution.
4. Recording, photo capture, OCR, and assistive-analysis scheduling.
5. Publish the completed GPU texture as the latest processed **scene texture**.

No stateful effect may advance again merely because the same camera frame is
presented a second time.

### Clock B - viewport presentation and interaction

Run at the configured viewport rate while visible:

1. Integrate pan/zoom input using elapsed time, not a fixed amount per tick.
2. Recompute the aspect-safe crop rectangle from the latest viewport size,
   rotation-adjusted scene size, zoom, and focus center.
3. Draw the persistent scene texture into the current viewport-sized swap-chain
   back buffer using a lightweight D3D12 full-screen triangle and bilinear
   sampler.
4. Present using the swap chain's frame-latency waitable object / VSync pacing.

When no new camera frame arrives, Clock B reuses the latest complete scene
texture. Moving objects remain at camera FPS, while navigation across that
texture remains responsive at the display rate.

## Canonical aspect and crop math

Let source dimensions after rotation be `Ws x Hs`, viewport dimensions in
native device pixels be `Wd x Hd`, source aspect `As = Ws/Hs`, destination
aspect `Ad = Wd/Hd`, focus center `(cx, cy)` in normalized source coordinates,
and user magnification `z >= 1`.

For **Fill (crop)**, first find the largest source rectangle matching `Ad`:

```text
if Ad > As:             # destination is relatively wider
    cropW = Ws
    cropH = Ws / Ad
else:                   # destination is relatively taller/narrower
    cropW = Hs * Ad
    cropH = Hs

sampleW = cropW / z
sampleH = cropH / z
```

Center `sampleW x sampleH` around `(cx, cy)`, then clamp the rectangle inside
the source. Compute one UV rectangle and sample it uniformly over the whole
viewport. Do not derive independent X and Y scales after this point.

For **Fit (show all)**, use the whole source and compute one uniform scale
`min(Wd/Ws, Hd/Hs)`, centering the active image and clearing the remaining
back-buffer area to black. This is optional; Fill remains the default magnifier
behavior.

Odd rotations swap the effective source dimensions before this math. The same
mapping object must drive rendering, pointer-to-source mapping, the focus
marker, assistive capture, and any overlay anchored to camera coordinates.

## D3D12 presentation design

1. Keep the swap-chain buffers exactly equal to the render HWND's **native
   client-pixel dimensions**. Qt logical sizes must be converted using the
   window's effective device-pixel ratio, or the native client rect must be
   queried directly. Recreate/resize on `WM_SIZE`, DPI transition, and Advanced
   splitter movement, not from camera-frame dimensions.
2. Add an aspect-aware presenter entry point such as
   `PresentSceneTexture(texture, sourceSize, ViewTransform, fenceSync)`. It
   transitions the scene texture to a shader-resource state, the back buffer to
   render-target state, draws one full-screen triangle, and transitions both
   resources back to their required states.
3. Replace the CUDA path's `CopyResource(backBuffer, cameraSizedTexture)` with
   this draw. `CopyResource` remains valid only for genuinely equal-sized debug
   or copy operations; it must not define camera-to-viewport geometry.
4. Create the root signature, pipeline-state object, sampler, SRV descriptor,
   and constant-buffer ring once. Per presentation, update only crop origin,
   crop scale, and optional Fit offsets.
5. Avoid `WaitForGpu()` during interactive splitter/window resizing. Retire old
   back buffers safely and coalesce resize events so resizing the Advanced panel
   does not stall the UI for every mouse pixel.
6. Treat `DXGI_SCALING_STRETCH` as an emergency compositor behavior only. Exact
   back-buffer sizing plus the shader mapping is the geometry authority.

Preferred implementation is a D3D12 graphics pass because it can re-present an
unchanged CUDA result without invoking CUDA again. A second CUDA surface sized
to the viewport is an acceptable fallback prototype, but it adds interop and
allocation pressure and is not the intended final design.

## NVIDIA SuperRes and other zoom-dependent stages

The viewport and SuperRes must depict the same geometry, but SuperRes must not
run 120 times per second.

- Clock A owns SuperRes inference and publishes an ROI texture plus metadata:
  source-frame generation, source UV rectangle, output dimensions, and zoom
  factor. Failure remains latched by configuration as in Plan 09.
- Clock B may display the cached SuperRes ROI only while its metadata covers the
  requested viewport crop. If a pan moves outside that ROI, immediately display
  the full-scene conventional path with identical geometry while Clock A builds
  a new ROI from the next camera frame. Never blend differently registered
  images; that was the cause of the earlier ghost/double-exposure defect.
- A later optimization can overscan the SuperRes ROI to tolerate small
  high-refresh pans between camera frames. Correct registration is mandatory;
  seamless quality switching is secondary.

## Viewport refresh setting

Add a global Advanced setting under **Global device / Interaction**:

```text
Viewport motion rate
  Auto (up to 120 FPS)   [recommended default]
  60 FPS
  90 FPS
  120 FPS
  Match display
```

Advanced diagnostics may expose 144, 165, and 240 FPS when the active monitor
supports them. The normal list should remain short and plain-language.

- `Auto (up to 120 FPS)` targets `min(activeDisplayHz, 120)` and reduces to the
  camera/presentation rate when the viewport is idle. This provides smooth
  motion without continuously consuming 120-Hz power.
- An explicit rate is clamped to the active monitor refresh. Moving the window
  between monitors updates the effective rate without restarting capture.
- `Match display` targets the active monitor's refresh, including 144/165/240
  Hz where supported, subject to measured presentation capacity.
- Run the high-rate loop only while pan, zoom, focus animation, resize, or a
  short approximately 150 ms settling tail is active. A new camera frame still
  triggers a present while idle.
- Use elapsed seconds for input integration. Existing constants that represent
  "movement per tick" must become rates such as normalized units per second, so
  motion speed is identical at 60 and 120 FPS.
- Prefer DXGI frame-latency pacing over relying solely on a `QTimer`. A precise
  timer may schedule input updates, but the swap chain is the authority for
  back-pressure and VSync. Track missed deadlines rather than queueing presents.

This setting controls only viewport navigation. A separate future camera-mode
selector should make `Available modes` actionable by requesting exact
`MF_MT_FRAME_SIZE`, `MF_MT_FRAME_RATE`, and subtype values and persisting the
choice per camera symbolic link.

## Diagnostics and accessibility

- Extend `FrameFormat` to retain the active frame-rate numerator/denominator.
- Add nonintrusive status/tooltip data:
  `Camera 30 FPS | Viewport 120/120 FPS | Display 144 Hz`.
- Report target rate, measured presentation rate, missed presents, current
  viewport pixel size, scene texture size, and Fill/Fit mode in the diagnostic
  area. Do not put this debug text in the full-screen Simple view.
- The rate selector, Fill/Fit selector, and their current/effective values need
  accessible names and descriptions. Wheel events over either selector must
  scroll the containing panel rather than changing the selected value, matching
  the existing wheel-safe control policy.
- Announce a forced clamp once through the status system, for example:
  "Viewport motion limited to 60 FPS by this display." Do not announce routine
  frame-rate fluctuations.

## Ownership and likely code touchpoints

- `src/d3d12/presenter.cpp`, `include/okuflow/d3d12/presenter.hpp`: viewport
  render pass, exact client-size swap chain, descriptor/constant rings, pacing,
  presentation counters.
- `src/app/app.cpp`, `include/okuflow/app/app.hpp`: split camera processing from
  viewport presentation, scene generation ownership, two-clock scheduling,
  diagnostic aggregation.
- `src/app/interaction_controller.cpp`: elapsed-time pan/zoom integration.
- `src/ui/main_window.cpp`, `include/okuflow/ui/main_window.hpp`: reliable
  native-pixel viewport resize notification and global setting controls.
- `src/app/settings_store.cpp`, `include/okuflow/app/settings_store.hpp`: global
  viewport rate and Fill/Fit persistence with migration defaults.
- `src/capture/media_capture.cpp`, `include/okuflow/capture/media_capture.hpp`:
  report active FPS; exact camera-mode selection is a follow-on, not required
  for the first presentation fix.
- `src/cuda/cuda_interop.cpp`: publish a stable completed scene texture and
  SuperRes ROI metadata without rerunning stateful processing on Clock B.

New or changed public classes, functions, and structs require matching updates
to `docs/code_reference.md`. The user-visible setting also requires README,
settings documentation, and CHANGELOG entries in the implementation commit.

## Implementation sequence and gates

### Phase 1 - stop distortion first

1. Add a deterministic checkerboard/circle test texture.
2. Implement the viewport-sized D3D12 render pass and shared `ViewTransform`.
3. Switch the CUDA path from camera-sized `CopyResource` presentation to the
   aspect-aware draw.
4. Keep the existing camera-frame-driven tick temporarily.

Gate: all aspect acceptance cases below pass before high-refresh work begins.

### Phase 2 - split the clocks

1. Publish completed scene texture generations from Clock A.
2. Add Clock B and elapsed-time input integration.
3. Ensure temporal/stateful GPU stages and recording remain camera-clock only.
4. Add active/idle rate policy and missed-frame protection.

### Phase 3 - settings and instrumentation

1. Add global persisted rate and Fill/Fit controls.
2. Add active camera FPS, viewport FPS, display Hz, and missed-present counters.
3. Handle monitor/DPI changes and validate rate clamping.

### Phase 4 - SuperRes ROI integration

1. Attach geometry metadata to the cached SuperRes result.
2. Use it only when registered with the requested viewport.
3. Add overscan only after correctness tests pass.

## Acceptance checklist

### Aspect correctness

- [x] A circle/checkerboard remains geometrically correct at viewport ratios
      16:9, 4:3, 1:1, 9:16, and 21:9.
- [ ] Moving the Advanced splitter from minimum to maximum width changes crop
      amount only; it never changes X:Y scale.
- [x] Rotations 0, 90, 180, and 270 degrees preserve aspect and use the correct
      post-rotation dimensions.
- [ ] Repeated resize, maximize/restore, Alt-Tab, and monitor/DPI transitions
      never leave a stale, black, stretched, or partially copied edge.
- [x] Fill has no letterbox; Fit shows the entire frame with symmetric bars.
- [ ] Pointer pan, focus marker, OCR/assistive captures, processed photos, and
      recorded processed video use the same visible crop mapping.
- [ ] The swap chain is never resized back to camera dimensions during a
      camera frame present.

### High-refresh motion

- [ ] With a 30 FPS camera and 120 FPS viewport target, a held pan produces
      approximately 120 distinct viewport positions per second while moving
      camera content still changes only 30 times per second.
- [ ] Pan distance per second is the same at 60 and 120 FPS.
- [ ] Temporal smoothing, stabilization, text hysteresis, recording, OCR, and
      notes advance once per fresh camera frame, never once per presentation.
- [ ] A 60 Hz display clamps cleanly to 60; 120 on a 144 Hz display targets 120;
      moving between displays updates the effective value.
- [ ] Idle mode does not continuously render at 120 FPS without a new camera
      frame or viewport motion.
- [ ] The lightweight presentation pass stays below 1 ms at 2560x1440 on the
      reference RTX 4090 Laptop GPU, with missed presents reported rather than
      queued.
- [ ] SuperRes never shows a fixed-center ghost or blends two different crop
      geometries during high-refresh pan.

## Explicit non-goals

- Frame interpolation, optical-flow-generated camera frames, or claiming a
  higher camera FPS than the hardware supplies.
- Running CUDA image enhancement or SuperRes at the monitor refresh rate.
- Making the camera mode list selectable in the first aspect/presentation
  patch; that is useful but independently testable follow-on work.
- Postponing the aspect fix until the full 120 FPS architecture is complete.
  Phase 1 is independently shippable and should land first.
