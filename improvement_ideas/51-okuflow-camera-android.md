# Plan 51 — OkuFlow Camera: native Android camera over USB debugging

Status: **2026-10-11 — Phone streaming/Advanced settings and initial native
Windows receiver implemented; hardware acceptance pending.** Phase 0 is
complete. Phases 1–3 have local implementation/contract/decoder validation;
their real-phone gates remain open. Viewport-to-phone zoom handoff,
quality/motion experiments and packaging remain pending.

The owner subsequently requested an optional Advanced settings menu. Current
defaults prioritize supported detail then FPS, AVC, automatic bitrate and
optical stabilization where available. The implemented wire contract is
[`android/PROTOCOL.md`](../android/PROTOCOL.md); architectural descriptions
below retain later-phase design targets.

The debug APK, local toolchain paths, exact pinned versions, and validation
commands are recorded in [`android/BUILDING.md`](../android/BUILDING.md).
[`android/design/setup-panels.html`](../android/design/setup-panels.html) is
an interactive screen draft; it is not a capture of a running Android device.

## High-priority follow-up (2026-10-12)

**Priority: P1 / high priority, explicitly requested by the owner.** Root
completed these source and binary reviews after the requested Opus reviews
hit their rate limit. No rebuilt Windows bundle, end-to-end acceptance or
recording-path benchmark is claimed by this review.

1. **Ship the corrected Android launch path in the Windows bundle.**
   The inspected `dist/OkuFlow/OkuFlowApp.exe` still launches the old
   `com.okuflow.camera/.MainActivity` component. The source already attempts
   `com.okuflow/com.okuflow.camera.MainActivity` first in
   [`phone_camera.cpp`](../src/capture/phone_camera.cpp), but the deployed
   binary is stale. Rebuild, run the tracked release gates and deploy a
   complete bundle. Acceptance: a cold Windows launch discovers and opens
   the Play-package phone app, receives a real first frame, and reconnects
   after cable removal. Test canonical-only and canonical-plus-legacy app
   installations, saved phone preferences, and an explicit ordinary webcam
   preference. If the owner is running the primary bundle, follow the
   documented `dist/OkuFlow2` fallback without terminating that instance.

2. **Explain blocked app launches accurately.**
   `PhoneCameraTransport::Start` currently treats a non-missing Android
   launch error as an instruction to install the phone app, even when it is
   installed. Distinguish a missing app from a blocked launch or access
   failure and give the appropriate next action. Preserve the rule that a
   blocked canonical app must not be bypassed by launching the legacy app.
   Acceptance: targeted error-message checks and real installed-app launch
   failure/recovery checks; existing command-selection tests alone do not
   establish first-frame success.

3. **Connect Windows zoom controls to the phone camera API.**
   `OkuFlowApp::OnZoomAmountChanged` in
   [`app_controls.cpp`](../src/app/app_controls.cpp) currently changes only
   the desktop viewport. `MediaCapture::SetPhoneZoom` exists in
   [`media_capture_facade.cpp`](../src/capture/media_capture_facade.cpp) but
   has no application/UI caller. Implement the phone sensor/lens zoom plus
   residual viewport zoom handoff, using acknowledged frame metadata so
   one gesture cannot apply the same zoom twice. Acceptance: rapid zoom
   changes, supported lens transitions, camera switches and reconnection
   converge to the requested view; ordinary webcams retain desktop zoom.

4. **Make recording bitrate an explicit quality/file-size policy.**
   [`media_writer.cpp`](../src/common/media_writer.cpp) currently requests
   `width * height * 5` bits/s for both AV1 and H.264, independently of FPS
   and incoming bitrate. At 4000 x 2256 this requests 45.12 Mbps, so
   re-encoding a 30 Mbps phone stream can request a larger Original file.
   Define sensible codec/FPS-aware quality and smaller-file targets, then
   measure actual output bitrate, text legibility and recording cost. For
   planning, video alone at 30/50/10 Mbps is approximately 13.5/22.5/4.5
   decimal GB/hour; these are bitrate calculations, not measured file sizes.

5. **Evaluate recording optimizations while preserving both files.**
   The owner confirmed that **Original and Processed recordings are both
   intentional**. Keep the two-file output contract. Investigate these
   paths before choosing defaults:

   - **Original, preserve quality:** optionally mux the incoming H.265 or
     H.264 stream directly when it satisfies the recording contract. This
     avoids the Original-file encoder; live viewing still needs decoding.
   - **Original, smaller file:** re-encode at an explicit lower bitrate.
   - **Processed:** encode the processed frames. Benchmark hardware HEVC
     against the existing AV1-first path at comparable quality/bitrate,
     with H.264 fallback and runtime hardware capability checks. An HEVC
     input does not make processed HEVC re-encoding a passthrough operation.

   Direct muxing needs an encoded-packet side channel before the current
   receiver discards packets after decoder submission. Specify keyframe
   startup, codec-header/container conversion, phone-to-host timestamps and
   microphone sync, rotation, codec/mode/camera changes, segment boundaries,
   reconnects, disk pressure and finalization. The present Original file is
   rotation-correct and frame-matched to Processed. A compressed-packet queue
   cannot reuse the decoded-frame drop policy without breaking inter-frame
   dependencies; preserve that contract or make any different archive
   semantics an explicit option. Acceptance: bounded queues, playable files
   across those transitions, synchronized audio and measured CPU/GPU/frame
   cost on supported PCs. No codec default change is approved by these notes.

## Goal

A companion Android app, **OkuFlow Camera**, that turns the student's phone
into the best possible OkuFlow camera with *zero configuration*:

1. Install the app once. It shows **one instruction at a time**: turn on
   Developer mode → turn on USB debugging → allow the camera. Each
   instruction opens the nearest public system screen in one tap, with a
   broader Settings fallback for OEMs. Android still requires the user to
   tap Build number and change the USB debugging switch; the app cannot
   enable these protected settings itself. See [Android's developer-options
   guide](https://developer.android.com/studio/debug/dev-options).
2. Plug the phone into the laptop. OkuFlow finds it, starts the phone app if
   needed, and streams the camera over the cable at the highest quality and
   frame rate the phone, cable and PC sustain.
3. Zooming in OkuFlow zooms **the phone's camera**: the sensor crop and lens
   switching (wide → telephoto), where exposed by the phone, happen on the
   phone. Text quality must be measured per lens and mode. Camera switching, focus
   and exposure locks and the torch are controlled from OkuFlow.
4. The phone camera becomes OkuFlow's default camera, unless the user has
   explicitly picked a different one.

5. **The phone is an optional extra feature.** Ordinary USB/MF cameras must
   work independently of the Android app, SDK, adb, phone connection and
   phone-source state. Selecting a webcam must preserve its saved identity,
   formats, GPU processing, photos, recording and device-loss recovery.

## What exists today (analysis)

- **Original capture was Media Foundation only.** `MediaCapture` (`include/okuflow/capture/media_capture.hpp`)
  enumerates MF cameras, delivers `MediaFrame`s and prepares GPU-resident
  frames for CUDA (`PrepareGpuFrameForCuda`). The application is tightly
  coupled to that facade (`src/app/app_pipeline_runtime.cpp`: enumeration,
  start/stop, acceleration ladder, device-loss recovery, readback).
  The phone branch now enters that facade through `PhoneCameraTransport`
  and `PhoneVideoDecoder`, sharing the frame/sample ownership contract.
- **The `MediaFrame` GPU contract** — an NV12/BGRA `ID3D11Texture2D` plus the
  pinned `IMFSample`, `gpuSubresource`, `YuvColorInfo`, capture timestamps —
  is exactly what a hardware video *decoder* also produces. A phone stream
  decoded by a DXVA Media Foundation decoder can enter the existing
  D3D11 → D3D12 → CUDA path without new GPU plumbing.
- **Camera identity** is the MF symbolic link (`settings.cameraSymbolicLink`,
  `ResolveSavedCameraIndex`), which also keys the per-camera acceleration map.
- **Zoom** lives in `OkuFlowApp` (`zoomEnabled_`, `zoomAmount_`,
  `zoomCenterX_/Y_`) and becomes a `ViewTransform`
  (`include/okuflow/common/view_transform.hpp`). All zoom today is digital,
  on the PC, after capture.
- **Phones today** reach OkuFlow through Android's native USB webcam mode
  (UVC, Android 14+) or vendor virtual cameras (DroidCam/Iriun/Camo; the
  lecture-camera harness even rides the DroidCam driver). Plan 29 B names the
  Android UVC webcam the primary hardware target and records the limits:
  the phone chooses the mode (often 720p/1080p, MJPEG/YUY2-bandwidth bound),
  no lens or sensor-zoom control, focus/exposure locks usually absent.
- **Persona** (plan 29): legally blind student, phone clamped to the laptop,
  NVDA running, a 90-minute lecture, a rushed setup while the lecture starts.
  The phone UI must be operable with low vision and TalkBack.

UVC and ordinary USB webcams remain independently supported through the
existing MF path. OkuFlow Camera is preferred only within the phone-source
policy, never over an explicit normal-camera choice.

## Requirements (owner's words → requirement)

| Owner request | Requirement |
|---|---|
| "If developer mode is off it should only say turn developer mode on" | One screen, one instruction, one action; the subsequently requested Advanced menu is optional. |
| "A single click should take you to the screen to turn it on" | The whole screen is one button opening About phone (Developer mode) or Developer options (USB debugging). The screen advances by itself when the user returns. |
| "If both are on it should stream the video over the cable with no configuration" | Plug-and-play via adb; OkuFlow discovers the phone, launches the app, negotiates the mode automatically. |
| "Highest possible quality / frame rate" | Capability-driven mode selection across camera, encoder, PC decoder and link; hardware encode at high bitrate; adaptive step-down only under thermal or link pressure. |
| "Motion compensation etc. on the phone" | ISP OIS/EIS where it helps, gyro metadata to the PC, optional on-phone GPU stage later. |
| "Zooming in OkuFlow should talk back to the phone to zoom in and out, to switch cameras" | Sensor/lens zoom on the phone with seamless residual zoom on the PC; lens and facing switch commands. |
| "Best / lowest-level APIs available" | Camera2 + MediaCodec (surface input) + `LocalServerSocket` on Android; adb server protocol + Media Foundation DXVA decode on Windows. |
| "Very simple UX, plug and play, minimum config" | Automatic defaults, with a separate Advanced menu at the owner's later request. |
| "If the OkuFlow camera app is selected it should be the default unless the user switched it" | Default-camera policy below (D7). |
| "Extra feature; OkuFlow should still work with no problems if a normal USB camera is used" | Optional source boundary and normal-camera regression gate (D10); no phone setup or dependencies on the webcam path. |

## Architecture

```
 Phone (OkuFlow Camera)                               Laptop (OkuFlow)
 ┌───────────────────────────────────────┐           ┌──────────────────────────────────────────┐
 │ Camera2 logical camera ──Surface──┐   │           │ AdbHostClient ── TCP 127.0.0.1:5037 ──┐  │
 │  (ISP: zoom ratio, OIS/EIS, AF/AE)│   │           │  track-devices / transport / exec     │  │
 │                                   ▼   │   USB     │                                       │  │
 │ MediaCodec HW encoder (HEVC|H.264)    │  (adb)    │ PhoneCaptureSession                   │  │
 │  zero-copy surface input              │◄─────────►│  control channel ◄─► zoom/lens/focus  │  │
 │        │ access units + metadata      │           │  video channel ──► MF DXVA decoder    │  │
 │        ▼                              │           │        NV12 ID3D11Texture2D + IMFSample  │
 │ HostLinkServer                        │           │        ▼                                 │
 │  LocalServerSocket "okuflow_camera"   │           │ MediaFrame ─► existing GPU path:         │
 │  peer uid must be shell/root (adbd)   │           │  D3D11 VideoProcessor → D3D12 → CUDA     │
 │ Gyro (SensorManager) ── metadata ─────┤           │  (stabilize, Text Clarity, zoom, NIS/FSR)│
 │ ThermalGovernor, StreamingService     │           │ PhoneZoomBridge: target zoom → phone     │
 └───────────────────────────────────────┘           │  zoom + per-frame residual on the PC     │
                                                     └──────────────────────────────────────────┘
```

## Key decisions

### D1 — Transport: adb, app-owned abstract socket, direct adb-server protocol

- The phone app listens on an **abstract-namespace `LocalServerSocket`
  named `okuflow_camera`**. There is no TCP listener and no INTERNET
  permission.
- Every accepted connection is checked with
  `LocalSocket.getPeerCredentials()`: only uid 2000 (`AID_SHELL`, which adbd
  runs as on user builds) or 0 (adbd after `adb root`) is accepted. Other
  apps on the phone cannot read the camera. This is the same pattern Chrome
  uses for `localabstract:chrome_devtools_remote`.
- OkuFlow talks to the **adb server protocol directly** over
  `127.0.0.1:5037` (no `adb.exe` child processes for streaming):
  `host:track-devices-l` gives push notifications of plug/unplug/
  authorization state; `host:transport:<serial>` followed by
  `localabstract:okuflow_camera` turns the TCP connection into a raw stream
  to the phone socket — no `adb forward` ports to allocate, collide or
  clean up. `exec:` runs `am start`/`pm`/`cmd package install`.
- Host discovery accepts USB transports only. A shell/root peer credential
  proves access through an authorized privileged process, not whether adb
  used a cable or wireless debugging. The phone also stops accepting sessions
  when USB debugging is disabled; transport filtering belongs to the host.
- OkuFlow starts an adb server only when none is running (bundled
  `adb.exe start-server`) and the user enters phone setup/selects a phone
  source. Normal-camera startup does not provision adb. It **never** restarts or kills an existing server
  of a different version (scrcpy and Android Studio ship their own adb;
  mismatched clients kill each other's servers). The protocol is stable
  across versions, so OkuFlow simply uses whatever server is there.
- Rejected: Wi-Fi/wireless debugging (owner wants the cable; latency and
  privacy), `adb reverse`/`forward` TCP ports (extra state, port collisions),
  raw WinUSB ADB (conflicts with a running adb server claiming the interface).

### D2 — Lowest-level public APIs

- **Android:** Camera2 (`android.hardware.camera2`), *not* CameraX (a
  wrapper that hides zoom-ratio/physical-camera/metadata details we need).
  Camera output goes straight into `MediaCodec`'s input `Surface`
  (`createPersistentInputSurface`), so frames never touch the CPU — the same
  zero-copy path the system camera uses to record video. Kotlin is used for
  control code: the pixel path is surface-to-surface, so the NDK buys no
  throughput there. The NDK (C++) is reserved for the optional GPU
  processing stage (D5). The camera HAL itself is not accessible to apps;
  Camera2 is the lowest supported layer.
- **Windows:** adb server protocol (D1) and a Media Foundation hardware
  decoder MFT bound to the capture D3D11 device through
  `IMFDXGIDeviceManager` (`MF_SA_D3D11_AWARE`), `CODECAPI_AVLowLatencyMode`
  on, one access unit per input sample. Output is an NV12 D3D11 texture
  sample — the existing `MediaFrame` GPU contract. NVDEC (decoding straight
  into CUDA memory) is an optional later optimization, not the baseline:
  the MF path also serves the GPU-unavailable passthrough mode.

### D3 — Codec

Negotiated per session from the phone's hardware encoders and the PC's
hardware decoders:

1. **HEVC Main** when the phone has a hardware HEVC encoder for the chosen
   mode *and* the PC has an HEVC decoder MFT. HEVC on Windows needs the
   "HEVC Video Extensions" package (often OEM-preinstalled, not always), so
   OkuFlow probes `MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, …HEVC…)` instead of
   assuming.
2. **H.264** otherwise, using High only where both sides advertise it,
   then Main/Baseline as supported.
3. AV1 later, when both sides advertise hardware support.

Bitrate favors text quality while respecting measured link throughput and
encoder limits: high VBR targets per mode, no B-frames where supported,
long GOP with on-demand sync frames, headers prepended to sync frames,
BT.709 with explicit range signalled in the VUI so `YuvColorInfo` is exact.

### D4 — Zoom handoff: optical/sensor zoom on the phone, residual on the PC

- Phone zoom uses `CONTROL_ZOOM_RATIO` (API 30) on the **logical** back
  camera for combined optical/digital zoom when supported. Physical lens
  availability and switching remain device-dependent. `CONTROL_SETTINGS_OVERRIDE_ZOOM`
  (API 34) is used where available to cut zoom latency.
- OkuFlow's zoom amount `U` is split into phone zoom `Zp` and PC residual
  `U / Zp`. An active-array/stream-size calculation is a candidate limit,
  not proof of lossless detail: binning and ISP scaling matter. Phase 5
  compares actual text quality and latency with PC NIS/FSR/Maxine before
  choosing the phone zoom cap.
- **Every encoded frame carries the zoom ratio and crop region actually
  applied** (from its `CaptureResult`, matched by sensor timestamp). The PC
  computes the residual transform *per frame* from the applied values, so a
  zoom gesture is instant on screen and never jumps when the phone catches
  up a few frames later.
- Pan: with non-unit `CONTROL_ZOOM_RATIO`, Android restricts crop-region
  use to aspect-ratio cropping. Keep phone zoom centered, lower `Zp` until
  the requested view fits, and pan on the PC. A separate crop-only mode
  may pan if FREEFORM cropping is supported. See [CaptureRequest zoom
  semantics](https://developer.android.com/reference/android/hardware/camera2/CaptureRequest#CONTROL_ZOOM_RATIO).
- A lens switch reports `LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID` in the
  frame metadata; OkuFlow resets the stabilization reference and temporal
  Text Clarity state on that frame instead of treating the jump as shake.

### D5 — Motion compensation

1. **OIS on** (`LENS_OPTICAL_STABILIZATION_MODE`) is the initial candidate
   when supported; measure mounted-camera stability rather than assuming
   it is always beneficial.
2. **EIS** (`CONTROL_VIDEO_STABILIZATION_MODE`, `PREVIEW_STABILIZATION`
   on API 33+) is *off by default while OkuFlow's Stabilize Image is on*:
   EIS crops and may affect latency and the fixed-reference tripod lock.
   Test it when OkuFlow stabilization is off. Do not force conventional OIS
   and EIS on together; preview stabilization can override OIS selection.
   Phase 6 measures the combinations. See [Android stabilization controls](https://developer.android.com/reference/android/hardware/camera2/CaptureRequest#CONTROL_VIDEO_STABILIZATION_MODE).
3. **Gyro metadata**: gyroscope samples (on the `elapsedRealtimeNanos` clock,
   aligned with `SENSOR_TIMESTAMP` when `SENSOR_INFO_TIMESTAMP_SOURCE` is
   `REALTIME`) plus rolling-shutter skew stream to the PC as a stabilization
   prior for bumps and focus hunts.
4. **Optional on-phone GPU stage** (Camera → `SurfaceTexture` → GLES/Vulkan
   warp → encoder surface) is a research gate, not v1: it adds a GPU pass
   and latency, and OkuFlow's CUDA stabilizer is already strong.

### D6 — Integration behind the existing `MediaCapture` facade

The phone appears as one more `CameraDescriptor` with a new source kind and
a stable identity `okuflow-phone:<adb serial>` in the `symbolicLink` slot.
`StartCapture` on such a descriptor runs a `PhoneCaptureSession` that emits
the same `MediaFrame`s (GPU-resident decoder samples, pinned exactly like MF
camera samples) and the same error/device-lost signals. Settings identity,
the per-camera acceleration map, device-loss recovery, recording and photos
keep working unchanged. Rejected: a Windows virtual camera
(`MFCreateVirtualCamera`) as the primary path — it adds a frame-server hop
and a system-wide component and still needs a side channel for zoom. It may
come later to expose the phone to other apps.

### D7 — Default camera policy

- New setting `cameraChoiceExplicit` (true only when the user picks a camera
  in the UI). An existing nonempty saved camera identity is treated as an
  explicit choice, preserving the owner's selected webcam.
- On startup and on hot-plug: if an OkuFlow phone is ready and either the
  user never chose explicitly or their saved choice *is* a phone, use the
  phone. Otherwise keep the saved camera and announce once:
  "Phone camera available — press <hotkey> to switch."
- Auto-selection applies after phone discovery is enabled or a phone choice
  is saved. Ordinary webcam startup does not enable/provision phone support.
- Several phones: the saved one, else the first authorized.
- Unplugging the phone uses the existing device-loss path (quiet retry), and
  replugging resumes it automatically.

### D8 — Privacy

No INTERNET permission, abstract socket restricted to adbd's uid, Android's
camera privacy indicator plus a foreground-service notification while
streaming, no persistent storage of frames on the phone.

### D9 — Phone UI

Kotlin + Jetpack Compose (Material 3), one activity, one screen, one step
at a time. Black background, white text (21:1), white action button with
black text, magenta brand accent for decoration only (≈4.5:1, never text).
Panels use merged semantics and a pane title for step announcements, plus
a confirmation haptic. Layouts scroll for large system fonts. Real-device
TalkBack and large-font acceptance are still pending. English, Turkish and
German match the Windows app's supported languages.

### D10 — Optional source boundary and normal-camera regression gate

- Keep the existing MF enumeration/capture/session implementation for normal
  cameras. Dispatch to the phone session only for a phone descriptor; phone
  discovery, handshake and control work stay off the camera and Qt UI threads.
- Opening or using a normal camera must not start/install adb, launch the
  phone app, request USB-debugging setup, or wait for a phone/service. Missing
  phone components disable only the optional phone source. Windows builds
  and normal camera tests require no Android SDK, Gradle or phone.
- Phone zoom/lens commands apply only while the selected source is a phone.
  A webcam retains its current PC zoom and effect behavior. Switching back
  invalidates pending phone callbacks/frames without affecting the webcam.
- Phone faults use source-specific recovery. A selected physical camera is
  never displaced by phone hot-plug, authorization, installation or reconnect.
- Before shipping Windows integration, run the full tracked
  `scripts/agent_build.bat` matrix plus a real USB-camera pass: start/change
  modes, zoom/effects, photos/recording, saved selection, unplug/replug. Repeat
  with no adb/Android app, and with phone discovery/connection failing in the
  background. Fake-phone/fake-adb tests verify that failures and stale callbacks
  cannot interfere with an active physical-camera session.

## Phone app design

### Onboarding state machine (drafted)

Resolved by `resolveSetupStep` (`android/app/src/main/java/com/okuflow/camera/setup/SetupStep.kt`),
a pure function with JVM tests:

| Step | Detected by | One tap opens |
|---|---|---|
| 1. Turn on Developer mode | `Settings.Global.DEVELOPMENT_SETTINGS_ENABLED == 0` and USB debugging off | `ACTION_DEVICE_INFO_SETTINGS` (About phone), OEM-specific hint for where "Build number" is (Samsung, Xiaomi/Redmi/POCO, OnePlus/OPPO/realme, generic) |
| 2. Turn on USB debugging | `Settings.Global.ADB_ENABLED == 0` | `ACTION_APPLICATION_DEVELOPMENT_SETTINGS` |
| 3. Allow the camera | `CAMERA` not granted | the permission dialog (auto-shown once on arrival); after a permanent denial, the app's settings page |
| Plug into your computer | `android.hardware.usb.action.USB_STATE` `connected == false` (battery plug type as fallback) | — |
| Open OkuFlow on your computer | USB connected, no OkuFlow session; hint about the "Allow USB debugging?" prompt | — |
| Streaming to OkuFlow | `HostLink.state == Streaming` | — (screen kept on) |

Settings are observed via `ContentObserver`s on their `Settings.Global`
URIs and re-read on resume, so the screen advances the moment the user
returns. Camera denial is stored in installation-local `setup` preferences
so restarting preserves the app-settings shortcut; granting permission clears
it. Backups/transfers exclude that installation-specific history.
An enabled `adb_enabled` counts as developer options on, because
some OEM builds report the developer flag inconsistently.

### Streaming engine (Phase 2)

| Component | Responsibility |
|---|---|
| `StreamingService` | Foreground service, type `camera` (`FOREGROUND_SERVICE_CAMERA`, Android 14+ rules: started while the activity is visible). Owns the session so a brief app switch does not drop the stream. |
| `HostLinkServer` | Abstract socket, peer-uid check, one active session (a new authorized connection replaces the old one), keepalive/watchdog, publishes `HostLink.state`. |
| `CameraController` | Camera2 logical camera, capture session with the encoder surface (+ optional preview surface), repeating request updates for zoom/crop/AF/AE/torch, `CaptureResult` metadata extraction. |
| `EncoderPipeline` | `MediaCodec` surface encoder; sync frames on request; dynamic bitrate (`PARAMETER_KEY_VIDEO_BITRATE`); output drained on a dedicated thread straight into the video channel. |
| `MetadataMux` | Matches capture frame numbers and sensor timestamps to encoder PTS using a verified output-surface timestamp base. Do not assume PTS always equals sensor time: configure a sensor base where available, handle clock offsets/readout timestamps, and bound the metadata join. Attach applied zoom/crop/lens/AF/AE/ISO and gyro samples; missing metadata must not reuse another frame's zoom. |
| `ThermalGovernor` | `PowerManager` thermal status listener and headroom; steps down frame rate, then resolution, and tells OkuFlow why. |

Screen while streaming: kept on (camera access needs the visible activity
or the foreground service started from it), dimmed to near-black after a
few seconds without touch, restored by a tap.

### Mode selection (Phase 5 refines)

Candidate sizes come from `StreamConfigurationMap.getOutputSizes(MediaCodec::class.java)`
with `getOutputMinFrameDuration`, `CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES`,
encoder `VideoCapabilities` (`getSupportedPerformancePoints`,
`areSizeAndRateSupported`) and, on API 35+, `CameraDeviceSetup.isSessionConfigurationSupported`
for exact combinations (e.g. 4K60 + stabilization). The PC contributes its
decoder limits. Default ladder (owner question 1): **4K60 → 1440p60 →
1080p60 → 4K30 → 1080p30 → 720p30**. Sixty frames per second comes first
because magnified panning and latency benefit most, and D4 recovers detail
through sensor zoom. Fixed AE target FPS range for steady cadence.

## Protocol design targets (implemented v1 supersedes this draft)

Exact v1 framing, commands, 16-MiB video limits and timeouts are frozen in
[PROTOCOL.md](../android/PROTOCOL.md). The original draft below records later
targets; proposed clock sync/events and command extensions are not implemented claims.

- **Two connections** to the same abstract socket: the first message on
  each declares its role (`CONTROL` or `VIDEO`) and the session id from the
  control handshake. Separate adb streams keep small control messages from
  queueing behind large video packets.
- **Framing** (little-endian): `u32 magic 'OKUF'` only in the hello; then
  `u16 type, u16 flags, u32 length, payload`. Lengths are bounded (control
  64 KiB, video 64 MiB) and every parser rejects oversize or truncated input.
- **Handshake:** host `HELLO{protocol, app version, decoders[], max size/fps}`
  → phone `HELLO_ACK{protocol, app versionCode, device model, Android API,
  cameras[], zoom ranges, lossless zoom limits, encoders[], timestamp source}`
  → host `START{camera, mode, codec, bitrate}` → phone `STARTED{config}`.
  A major-version mismatch makes OkuFlow offer the bundled APK upgrade.
- **Video packet:** `flags{config, sync}`, `pts_us`, `sensor_ts_ns`, then
  Annex-B access unit.
- **Frame metadata** (control channel, keyed by `sensor_ts_ns`): applied
  zoom ratio, crop region, active physical camera id, focus distance, AF/AE
  state, exposure time, ISO.
- Video and control include a session generation and capture frame id;
  camera switches invalidate old messages. Bounded queues replace pending
  zoom updates with the latest target. Under video pressure, drop a whole
  undecodable GOP and request a sync frame rather than drop arbitrary
  interdependent access units. Metadata joins expire without blocking the
  Qt event loop or stalling camera capture.
- **Host → phone commands:** `SET_ZOOM{ratio, center}`, `SET_CAMERA{facing |
  physical id}`, `SET_FOCUS{auto | lock | point}`, `SET_EXPOSURE{auto |
  lock | point}`, `SET_TORCH`, `SET_STABILIZATION{ois, eis}`,
  `REQUEST_SYNC_FRAME`, `SET_BITRATE`, `SET_MODE`, `PING`.
- **Phone → host events:** `STATS{fps, bitrate, thermal, battery, dropped}`,
  `GYRO_BATCH`, `CAMERA_ERROR`, `STATE`, `PONG` (clock sync: NTP-style
  min-RTT offset between `elapsedRealtimeNanos` and the PC's QPC). Populate
  sensor-based `captureClock100ns` only with a validated timestamp source;
  otherwise retain arrival timing and mark sensor latency unavailable.

## Windows integration design (Phase 3)

| Component | Responsibility |
|---|---|
| `AdbHostClient` | Minimal adb server protocol client (hex length prefix, `OKAY`/`FAIL`); `host:version`, `host:track-devices-l`, `host:transport:<serial>`, `localabstract:`, `exec:`; starts a bundled server only when none is running. Off the UI thread. |
| `PhoneDeviceWatcher` | Turns device events into states with plain-language status: *Tap Allow on your phone* (`unauthorized`), *Install OkuFlow Camera* (package missing → one-click install of the bundled APK via `exec:cmd package install -S`), *Updating OkuFlow Camera*, *Unlock your phone*, *Ready*. Launches the app with `am start -n com.okuflow.camera/.MainActivity`. |
| `PhoneCaptureSession` | Control/video channels, handshake, keepalive, reconnect with the same quiet backoff as camera device loss. |
| `PhoneVideoDecoder` | MF DXVA decoder on the capture D3D11 device; low-latency mode; stream-change handling for mid-session SPS changes; decode errors request a sync frame. |
| `MediaCapture` source kind | Phone descriptors in `EnumerateCameras`; `StartCapture`/`StopCapture`/`ConsumeDeviceLost`/`PrepareGpuFrameForCuda` route to the phone session. |
| `PhoneZoomBridge` | Maps `zoomAmount_`/zoom centre to phone commands (D4), applies the per-frame residual to the `ViewTransform`, resets stabilization on lens change. |
| UI | Camera list entry "Phone — <model>"; phone controls in `Device` (lens/front-back, torch, Focus and Exposure Auto/Locked — the plan 29 B anti-hunt controls, provided by the phone protocol instead of UVC); status line "Phone camera 3840×2160 at 60 fps (HEVC)"; NVDA announcements; EN/TR/DE parity via `scripts/check_translations.ps1`. |

Later: record the phone's original bitstream without re-encoding (remux into
the existing fragmented MP4 writer) for a full-quality, zero-GPU-cost
"original" track.

## Quality and latency targets

- Glass-to-glass latency target: under 100 ms at 60 fps (measured with a
  screen-flash/LED test and the clock-synced timestamps).
- No steady-state frame drops over a 90-minute soak at the selected mode
  with the phone charging on the cable; thermal step-downs are reported,
  never silent.
- Text legibility judged on the plan 07 fixtures: phone sensor zoom +
  OkuFlow pipeline versus UVC webcam mode at the same magnification.

## Security and privacy

- Peer-uid check on every connection; the socket exists only while USB
  debugging is on and the app is in the foreground or its streaming service
  is running.
- No INTERNET permission; the stream exists only inside the adb tunnel.
- Protocol parsers on both sides are bounds-checked and fuzzed (Phase 3).
- User-facing note (docs): USB debugging grants the paired computer full
  device access; pair only your own computer. Android drops unused adb
  authorizations after a few days by default, so the "Allow USB debugging?"
  hint stays on the waiting screen.

## Phases

No time estimates by project rule. Each phase lists tasks and its
acceptance gate.

### Phase 0 — Toolchain and scaffold (completed by GPT-6.1-Sol)
- User-scope JDK, fresh user-owned Android SDK (`%LOCALAPPDATA%\Android\Sdk`)
  with platform-tools, latest stable platform/build-tools, NDK, CMake;
  `ANDROID_HOME` set; global `JAVA_HOME` untouched.
- Gradle wrapper + version catalog in `android/`; build outputs and Gradle/
  Kotlin caches **off the Google Drive mount**; `scripts/build_android.ps1`.
- **Gate:** `pwsh -File scripts/build_android.ps1` assembles the debug APK and
  passes JVM unit tests; `android/BUILDING.md` records exact versions.

### Phase 1 — Onboarding screens (implemented; device acceptance pending)
- Implemented: state machine, settings observers, one-tap intents with
  fallbacks, OEM hints, panels, theme, launcher icon, EN/TR/DE strings,
  resolver/OEM unit tests, persistent permission-denial shortcut, explicit
  backup exclusions, all-state Compose previews and an interactive HTML draft.
- Local validation: APK assembly, 12 JVM tests and Android lint pass.
  No connected phone was available; Settings navigation, TalkBack, and
  actual large-font Compose rendering have not been verified on hardware.
- Remaining: instrumented Compose UI tests (one per state, font scale 2.0,
  TalkBack semantics: one merged node per panel); a "still waiting" hint
  when USB is connected but no session arrives (unaccepted RSA prompt,
  Samsung Auto Blocker, charge-only cable); verify the Settings reads on the
  owner's phone(s).
- **Gate:** on a real phone from a fresh state, each state shows exactly one
  instruction; one tap reaches the right system screen; returning advances
  automatically; TalkBack and 200% font scale verified.

### Phase 2 — Phone streaming engine + host probe (implemented; hardware gate open)
- Implemented: `CameraCatalog`, `CameraEngine`, `CameraStreamService`, bounded
  `UsbSocketServer`/`WireProtocol`, Advanced settings, AVC/HEVC negotiation,
  metadata, startup fallback, generation invalidation and thermal handling. The
  combined size/rate picker discovers Camera2/hardware-encoder pairs locally,
  persists exact dimensions/FPS and refreshes on camera/codec changes.
- Probe: `scripts/android_camera_probe.py` (standard library), seven fake-server
  tests, per-generation video and metadata. Android has 41 passing JVM tests
  and zero lint issues. Pixel 9 Pro Fold AVC/HEVC capture, remote controls,
  connection recovery and native Windows reception have been exercised;
  [device validation](../android/DEVICE_VALIDATION.md) records actual modes,
  cadence and the remaining physical-replug/soak/older-phone gates. The
  original phase tasks below describe the acceptance target.
- `StreamingService`, `HostLinkServer`, `CameraController`,
  `EncoderPipeline`, `MetadataMux`, `ThermalGovernor` (table above).
- Protocol v1 Kotlin codec with golden byte vectors checked into the repo
  (shared with the C++ implementation in Phase 3).
- Host probe `scripts/android_camera_probe.py` (stdlib only): speaks
  the adb server protocol and OkuFlow protocol, writes the elementary stream
  plus `metadata.jsonl`, prints negotiated mode and measured arrival cadence.
  Use `ffprobe`/`ffplay` for hardware acceptance; sensor latency needs later clock sync.
- **Gate:** probe receives a decodable stream at the selected mode; a
  90-minute soak shows steady cadence, no leaks, reported thermal state;
  a non-shell local client is rejected.

### Phase 3 — OkuFlow Windows integration (initial receiver implemented)
- Implemented: optional facade/session branch, structured USB transport discovery,
  stable default/lens identities, independent channels/keepalive, system AVC
  and optional HEVC decoding with DXGI sharing where supported and CPU NV12
  fallback. HEVC is offered only after decoder activation/type probing and
  retries once as AVC on decoder failure or absent first output. Original
  AVC/HEVC fixtures validate decoded pixels; USB/parser/source-policy tests
  pass. Normal webcams retain their existing activation/capture branch.
- The latest Pixel native HEVC run delivered 1920 × 1080 @ 60 at about 60 FPS
  arrival with direct GPU transfer, no expired handoffs and no camera error.
  AVC and physical Logitech webcam regression checks also completed. Release
  compilation passed; 30/31 CPU and 35/36 CUDA suites passed, with the same
  foreground-focus interaction suite failing while a game held focus. The
  full gate is not green and no release bundle was published. Controlled
  physical USB hot-plug, missing-decoder/failure retry and long-soak acceptance
  remain open; see the device record for exact counts and evidence.
- Remaining: persistent device watcher, bundled adb/APK setup, automatic viewport
  zoom bridge, deeper fake-session/fuzz coverage and real-device acceptance.
- `AdbHostClient`, `PhoneDeviceWatcher`, `PhoneCaptureSession`,
  `PhoneVideoDecoder`, `MediaCapture` source kind, D7 policy, UI/status,
  translations; docs per `agents.md` (`docs/code_reference.md`, capture
  README, `CHANGELOG.md`).
- Tests: protocol golden vectors; adb client against a fake adb server;
  decoder fixture stream; facade with a fake phone session; default-policy
  matrix in `settings_store_tests`/a new suite; parser fuzzing.
- **Gate:** plug in → OkuFlow shows the phone camera with no configuration;
  unplug → quiet recovery; replug → resumes; explicit webcam choice is not
  overridden; D10's normal-camera regression gate passes with phone components
  absent or failing; `scripts/agent_build.bat` green.

### Phase 4 — Remote control from OkuFlow
- D4 zoom handoff with per-frame residual; pan via crop region or centre
  fallback; lens/facing switch; Focus and Exposure Auto/Locked and
  tap-to-point (`CONTROL_AF_REGIONS`/`AE_REGIONS` mapped through the current
  crop); torch; stabilization reset on lens change.
- **Gate:** continuous zoom 1× → max shows no visible jump at lens switches
  or when the phone catches up; locks report the real result; NVDA
  announces control state.

### Phase 5 — Quality and latency maximization
- Implemented: capability-driven mode pairs, a main menu limited to at least
  720p/24 FPS, and the full locally detected catalog together one level down
  in More options. HEVC/H.264 negotiation now checks Windows decoder presence.
  A 10 Mbps HEVC probe held about 60 FPS, but small-text quality equivalence is
  unproven and automatic bitrate remains unchanged.
- Remaining: bitrate adaptation from encoder/link pressure; ISP
  tuning experiments (`EDGE_MODE`, `NOISE_REDUCTION_MODE` versus Text
  Clarity's own sharpening); clock sync and latency measurement; optional
  original-bitstream recording.
- **Gate:** documented per-phone results (mode, codec, latency, legibility)
  on the owner's devices; defaults chosen from data.

### Phase 6 — Motion compensation
- OIS/EIS matrix versus OkuFlow fixed-reference stabilization (plan 25/26
  metrics); gyro prior research in the CUDA stabilizer; GLES/Vulkan
  on-phone stage behind a research gate.
- **Gate:** a measured recommendation per mode; no default that worsens
  the tripod lock.

### Phase 7 — Packaging and distribution
- Release signing key decided before any public install (debug-signed and
  release-signed APKs cannot upgrade each other); APK bundled in the Windows
  release with adb install/upgrade; adb redistribution licence verified and
  recorded in `docs/THIRD_PARTY_LICENSES.md` (or build adb from Apache-2.0
  AOSP sources); distribution channel (owner question 3), including
  Android's developer-verification rules for sideloaded apps; user docs.
- **Gate:** clean machine + clean phone reach streaming with only the three
  phone steps and one cable.

## Testing strategy

- Android: JVM unit tests for pure logic (state resolution, protocol codec,
  mode selection, zoom split); instrumented tests for Compose panels and
  Camera2/MediaCodec smoke on a device; the probe for end-to-end.
- Windows: CTest suites in the existing CPU/CUDA presets (non-GUI where
  possible), fake adb server, recorded decoder fixtures; GUI suites only
  through the tracked scripts.
- Hardware matrix: the owner's phones first (plan 29 B rule — no vendor
  rescue work), one Samsung and one Pixel class device when available.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Windows needs an OEM ADB driver for some phones (notably Samsung) | `PhoneDeviceWatcher` detects "USB device present, no adb" and links the vendor driver page; most current phones bind WinUSB automatically. |
| adb version conflicts with scrcpy/Android Studio | Protocol-level client, never restart a foreign server (D1). |
| Samsung Auto Blocker / Android Advanced Protection block USB debugging | Waiting-screen hint and OkuFlow status text; documented. |
| OEMs hide telephoto/ultra-wide from third-party apps or lack `CONTROL_ZOOM_RATIO` | Capability probe; fall back to digital crop and PC zoom; report what the phone exposes. |
| Tele lens minimum focus distance too long for close reading | Logical camera switching handles it on most phones; the bridge caps `Zp` when focus fails at close range. |
| Phone heat on long lectures while charging | `ThermalGovernor` step-down, dimmed screen, OkuFlow announcement. |
| HEVC decoder missing on the PC | Probe and fall back to H.264 (D3). |
| Another app takes the camera | Camera2 disconnect → `CAMERA_ERROR` → OkuFlow status; reacquire when free. |
| Coupling in `app_pipeline_runtime.cpp` makes the source-kind change risky | Keep the change inside the facade; reuse the device-loss/recovery contract; facade tests with a fake session first. |

## Open questions for the owner

These are optional preferences for later phases, not prerequisites for this
scaffold. Proceed with 60 fps first among sustained supported modes, a dark
phone status screen, first install from the project APK, and preserving any
explicit webcam choice. Do not show the camera over a locked screen by
default. Release signing and adb redistribution remain Phase 7 gates.

1. **Default mode ladder:** 60 fps first (proposed) or maximum resolution first?
2. **Lock screen:** may the streaming screen show over the lock screen
   (`showWhenLocked`) so OkuFlow can start the camera without unlocking?
3. **Distribution:** Play Store, okuflow.com APK, or install only through
   OkuFlow over adb (still needs a first install path for the onboarding UI)?
4. **adb:** bundle `adb.exe` with OkuFlow (proposed, after the licence check)
   or require an existing platform-tools install?
5. **Phone preview:** show the camera on the phone screen for a sighted
   helper (costs power/heat), or keep the dark status screen (proposed)?
6. **Hot-plug with an explicit webcam choice:** announce only (proposed) or
   switch automatically?

## References

- Camera2: <https://developer.android.com/reference/android/hardware/camera2/package-summary>
  (`CONTROL_ZOOM_RATIO`, `CONTROL_SETTINGS_OVERRIDE_ZOOM`,
  `LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID`, `CameraDeviceSetup`)
- MediaCodec: <https://developer.android.com/reference/android/media/MediaCodec>
- Output-surface timestamp bases: <https://developer.android.com/reference/android/hardware/camera2/params/OutputConfiguration#setTimestampBase(int)>
- `LocalServerSocket` / `LocalSocket.getPeerCredentials`: <https://developer.android.com/reference/android/net/LocalServerSocket>
- Foreground service types (camera): <https://developer.android.com/develop/background-work/services/fgs/service-types>
- adb server protocol: AOSP `packages/modules/adb/OVERVIEW.TXT`, `SERVICES.TXT`
- Media Foundation DXVA decoding: <https://learn.microsoft.com/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation>
- Prior art: scrcpy (camera mirroring over adb), Chrome DevTools remote socket.
- Related plans: 20 (capture integrity), 25/26 (stabilization), 28/30
  (GPU capture bridge), 29 B (camera controls, Android target), 29 L
  (framing assistant).
