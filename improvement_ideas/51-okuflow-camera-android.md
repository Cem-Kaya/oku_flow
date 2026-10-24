# Phone and camera follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24.
The companion, USB protocol/decoder, source discovery, owned adb lifecycle,
phone light, source-built adb payload and Windows receiver exist. Read
[the current Android guide](../android/README.md),
[protocol](../android/PROTOCOL.md) and
[device evidence](../android/DEVICE_VALIDATION.md) for implementation details.

## Remaining integration

- **Phone zoom and lenses:** implemented after 0.3.1 as a separate hardware
  camera-zoom control, Auto/default logical camera, named lens choices and saved
  zoom per phone/lens. Capture format and orientation also have per-camera
  records. See [the implementation contract](../docs/phone_camera_zoom.md);
  physical multi-lens acceptance remains separate. An automatic optical/digital
  handoff from viewport zoom is not implemented or needed for the separate control.
- **Further saved camera settings:** capability-probed focus/exposure/white-balance
  controls and per-device profile/view restoration. Saved selection, capture
  format, orientation, phone zoom and acceleration are not a complete hardware
  control set.
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
