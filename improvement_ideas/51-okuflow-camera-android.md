# Phone and camera follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24.
The companion, USB protocol/decoder, source discovery, owned adb lifecycle,
phone light, source-built adb payload and Windows receiver exist. Read
[the current Android guide](../android/README.md),
[protocol](../android/PROTOCOL.md) and
[device evidence](../android/DEVICE_VALIDATION.md) for implementation details.

## Remaining integration

- **Viewport-to-phone zoom:** the transport/facade have `SetZoom` and
  `SetPhoneZoom`, but the app does not call that facade from viewport zoom.
  Design the optical/digital handoff, capability/range clamping and return to
  ordinary camera-view magnification. Do not confuse the existing MCP view
  zoom with phone optical zoom.
- **Camera working sets:** capability-probed focus/exposure/white-balance
  controls and per-device mode, rotation, profile and view restoration.
  Saved camera selection and per-camera acceleration policy already exist;
  they are not a complete hardware-control working set.
- **Adaptive phone delivery:** investigate bitrate adaptation to encoder/link
  pressure. Preserve truthful actual cadence, backpressure and source epochs.
- **Recording experiments:** assess encoded Original-stream passthrough or
  hardware HEVC only with actual source/codec support. Current recording works;
  these alternatives are not assumed improvements or new default commitments.

## Validation is separate

Keep real replug, older-phone, decoder and long-soak observations in the device
record. Do not call the entire companion unimplemented because a hardware
matrix is incomplete, and do not claim a Play submission is approval.

Never stop a foreign adb server or silently replace an explicit webcam choice.
