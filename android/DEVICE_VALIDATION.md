# Real-device validation — 2026-10-11

Debug companion and native Windows development build, tested on a Pixel 9 Pro
Fold running Android 17/API 37, using an authorized USB adb transport. The PC
uses an RTX 4090 and reports approximately 120 Hz at 5120 × 1440. G-SYNC
activation for the application was not measured. Samsung S21 FE Exynos and
Galaxy A35 were not attached; passing setup-policy tests do not substitute
for testing those phones.

Evidence is local under `%LOCALAPPDATA%\OkuFlow\phone-integration-20261011`;
earlier probes are under `%LOCALAPPDATA%\OkuFlow\camera-probes`. Video, real
camera images, logs and settings fixtures are not committed or bundled.
The existing adb server and ordinary user settings were preserved. Profiling
uses `scripts/profile_startup.ps1` with isolated settings and normally exiting
children; no running user OkuFlow instance was terminated.

## Setup and controls

- Android 17 returned redacted zero values for both developer settings while
  shell settings were enabled. Current USB `adb=true` evidence now permits
  setup to advance. Unknown settings show connection help. Older Android
  retains its explicit enabled/disabled checks; camera permission is still
  mandatory. Minimum supported Android version remains 11/API 30.
- `FLAG_KEEP_SCREEN_ON` was verified on the app window and the device remained
  awake beyond its 120-second timeout while Advanced settings was visible.
  Manual locking still blocks the camera until the user unlocks it.
- A 75-second probe exercised zoom 2×/0.6×/1×, front/rear switching, exposure
  +2/0, autofocus off/on and a keyframe request. Capture metadata confirmed
  the controls. All seven encoded generations passed sequence/timestamp checks;
  the USB transport identity remained stable throughout.
- Two abrupt socket disconnects were followed by successful fresh sessions
  at 1920 × 1080, approximately 59.5 and 59.3 FPS. This was transport-session
  recovery, not a controlled physical unplug/replug test.
- Earlier interrupted probes coincided with changing adb transport identities.
  Their cause was not established. The watchdog timestamp race was fixed,
  but it is not claimed to explain every interruption.

## Earlier mode-ceiling checks

These checks used the former resolution/FPS ceilings. Selection favored detail
before frame rate and respects Camera2 durations, hardware encoder support,
host limits and thermal state. It does not synthesize unsupported sizes.

| Test | Actual encoded mode | Measured arrival cadence |
|---|---|---|
| AVC probe, automatic | 4000 × 3000 / 30 | 29.67 FPS |
| HEVC probe, automatic | 4000 × 3000 / 30 | 29.94 FPS |
| Native Windows, automatic | 4000 × 2256 / 24 | 23.99 FPS |
| Native Windows, 1080p/60 ceiling | 1920 × 1080 / 60 | about 59.5 FPS |
| Probe, 1440p/60 ceiling | 2560 × 1280 / 30 | 29.95 FPS |
| Native Windows, 1440p/60 ceiling | 2560 × 1280 / 30 | 29.99 FPS |

Thermal status was zero during the 1440p check. This did not validate
2560 × 1440 at 60 FPS; that mode was not selected by the current intersection.
After the streaming comparison, the phone was restored to 1080p, 60-FPS ceiling, H.264, automatic bitrate,
rear camera, optical stabilization, autofocus on, exposure zero and light off.
The AVC and HEVC probe generations were fully decoded with ffmpeg. A real
room image and native viewport were visually inspected; this is not a
calibrated text-legibility or optical-quality test.

## Consumer performance investigation

Comparable 20-second native runs used the same isolated phone settings at
1080p/60. These are short observations on this PC, not universal benchmarks.

| Run directory | Direct GPU | Expired handoffs | Completed-frame tick p95 | Host arrival-to-present p95 | Received / processed / presented |
|---|---|---|---|---|---|
| `native-1080-clean` | no, demoted to safe copy | 3 | 7.36 ms | 8.15 ms | 1093 / 1080 / 1068 |
| `native-compatibility` | no, CPU decode then CUDA upload | 0 | 1.80 ms | 3.10 ms | 1098 / 1090 / 1088 |
| `native-query-flush` | yes | 0 | 0.99 ms | 6.86 ms | 1091 / 1083 / 1061 |
| `native-query-flush-repeat` | yes | 0 | 0.94 ms | 6.77 ms | 1092 / 1084 / 1068 |

The retained D3D11 conversion query was repeatedly reaching its 25 ms deadline
with `D3D11_ASYNC_GETDATA_DONOTFLUSH`, even after initial End/Flush. Allowing
runtime flushing on subsequent single polls (`GetData` flags zero) kept direct
GPU transfer active in both verification runs. Their maximum retry spans were
18 and 14 ms; no CPU preparation samples were needed. No busy-wait loop,
resource-lifetime bypass or larger frame queue was introduced. An earlier
`D3D11_VIDEO_USAGE_OPTIMAL_SPEED` experiment did not help and was reverted.
The driver-internal reason for submission starvation is not established.

This reduces processing cost and handoff failures. It does **not** establish
zero frame loss or a higher sustained display rate: the final rolling viewport
rates were 57.48 and 58.24 FPS. The compatibility run was smoother on this PC;
it remains an existing per-camera option, not a new default for all phones.

Camera arrival FPS measures host decoded-frame callbacks; sensor metadata in
the reconnect probes measured about 59.57 FPS. The viewport rate uses a separate
rolling window and counts successful presentation submissions, which may include
cached-scene redraws. Do not subtract rounded FPS values as an exact loss count.
Ingress drops count older pending frames replaced by newer arrivals. The
`missed_present_attempts` counter combines busy frame fences and swap-chain
admission; repeated retries are not distinct lost frames. It does not identify
the monitor as the cause.

OkuFlow requests VSync through `Present(1, 0)` and sets maximum queued
presentation latency to one. A 120 Hz display does not imply a 60 FPS cap.
Neither the current counter nor the display refresh query establishes whether
NVIDIA G-SYNC is active for the window. Host arrival-to-present timing starts
after decoding and ends at submission; it is not sensor-to-photon latency.
Tick p95 excludes time yielded to Qt between GPU-query retries. CPU preparation
percentiles in the old path include fallback samples, not a CPU-conversion cost
paid on every steady-state frame.

References: [Microsoft GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata),
[Microsoft Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present),
[NVIDIA G-SYNC setup](https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-gb/mergedProjects/nvdspENG/To_use_variable_refresh_rates.htm).

## Combined mode picker and dropdown verification

The updated APK was installed on the same Pixel. At its existing 150% system
font scale, actual screenshots confirm visible labels, bright 2-dp outlines,
full-control-width menus, larger padded choices and a black-on-white selected
row. Text sizes were preserved. Menus scroll to the selected item on opening.
Evidence is under `settings-ui/combined-mode-top.png`,
`settings-ui/combined-mode-menu-30.png` and `settings-ui/dropdown-display-check.mp4`
(with an unmodified decoded frame in `dropdown-recorded-frame.png`). These are
native captures, not HTML or Compose previews.

Some ordinary `screencap` captures intermittently omit parts of static text,
including system clock/icons and popup backgrounds. A separate `screenrecord`
capture of the same unchanged screen shows the complete UI and system chrome;
this points to a screenshot-path issue rather than missing app content.
The optional DPU-forced capture failed on this device. Physical-display
confirmation was requested; do not use the incomplete PNGs as layout evidence
or claim the OS capture issue has been repaired.

The former 1080p/60 ceiling migrated to the stored **1920 × 1080 @ 60 FPS** pair.
Both dimensions and FPS were inspected in app preferences. Camera 0 → 1 → 0
and AVC → HEVC → AVC selections completed with that pair retained and the mode
picker available. The list is populated locally even without an active host.
The earlier 1440p-ceiling results above describe the previous selector, now
replaced by actual size/rate pairs.

| New picker selection | Capture | Frames | Measured arrival FPS |
| --- | --- | ---: | ---: |
| 1920 × 1080 @ 30 FPS | AVC probe, 10 seconds | 285 | 29.895 |
| 1920 × 1080 @ 60 FPS | AVC probe, 10 seconds | 574 | 59.434 |
| 1920 × 1080 @ 60 FPS | HEVC probe, 8 seconds | 453 | 59.403 |

All three elementary recordings fully decoded with FFmpeg `-xerror` and exit
code zero. Evidence directories are `paired-mode-30`, `paired-mode-60` and
`paired-mode-hevc`. These are short functional checks, not thermal-soak results.

An earlier 20-second native Windows run (`paired-mode-native`) retained the phone's
HEVC preference while the then AVC-only host negotiated AVC. It received
1920 × 1080 at 59.513 FPS: 1,088 frames received, 9 pending frames replaced,
1,079 processed, 1,073 presentation submissions. Direct GPU remained active,
with zero expired handoffs and no last-camera error. The final rolling viewport
sample was 59.821 FPS; its sampling window differs from the whole capture and
it is not a sustained-throughput or latency guarantee. The profiling child
exited normally and used isolated desktop settings.

At that stage the phone was left on rear camera 0, AVC, **1920 × 1080 @ 60 FPS**, automatic
bitrate, optical stabilization and autofocus. Its user-enabled camera light
was preserved. Native TalkBack, German at 200%, and other phones remain untested.

## Simplified picker and separate More options screen

The latest APK was built, installed and checked at the preserved 150% system
font scale. The main picker now shows only locally detected pairs with short
edge at least 720 pixels and frame rate at least 24 FPS. Its bottom entries
included 1280 × 720 at 60/30/24 FPS; the 1080p choices excluded 15 FPS.
**More options** opens a separate screen containing the entire detected mode
catalog together, including the same 1080p pairs plus 15 FPS, along with bitrate,
codec, diagnostics and reset. Back dismisses an open popup before returning to
Advanced settings; both screens retain their own scroll position.

Native recordings `settings-ui/main-modes-filtered.mp4` and
`settings-ui/more-options.mp4`, and their unmodified decoded PNG frames, show
the complete controls and text. The wider outlined controls retain their font
sizes. UI hierarchy checks also confirmed the bottom of the filtered list.
The filter affects the menu only: lower modes remain selectable in More
options and available for automatic compatibility/thermal fallback.

## Native Windows HEVC receiver and compression checks

The new Windows receiver probes the installed Media Foundation HEVC decoder
before offering HEVC to the phone. This PC has a working system HEVC decoder:
both original H.264 and HEVC fixture rows decoded successfully (7 QtTest checks
passed, none skipped). No decoder extension or codec library was downloaded or
bundled by this change. A missing decoder omits HEVC from negotiation; HEVC
initialization/decode failure or no first output within four seconds reconnects
once offering AVC only. The missing-decoder and fault-injected retry paths have
not been exercised on real hardware.

Latest native runs used isolated settings and normally exiting profiling
children. Logs explicitly confirmed the negotiated MIME type:

| Evidence directory | Actual codec and mode | Arrival FPS | Received / pending replaced / processed / presented | Direct GPU / expired handoffs |
| --- | --- | ---: | --- | --- |
| `native-hevc-60` | HEVC, 1920 × 1080 @ 60 | 60.09 | 1093 / 10 / 1083 / 1077 | yes / 0 |
| `native-avc-regression` | AVC, 1920 × 1080 @ 60 | 60.21 | 795 / 10 / 785 / 780 | yes / 0 |

The 20-second HEVC run reported no camera error, a 1.17 ms completed-frame
tick p95 and 5.58 ms host arrival-to-present p95. Its final rolling viewport
sample was 59.46 FPS and first presentation was about 1.96 seconds after
startup. The 15-second AVC regression also reported no camera error. Slightly
over-60 arrival estimates reflect sampling boundaries, not sensor rates above
the requested mode. These are short checks with different sampling windows;
they do not establish zero frame loss or sensor-to-photon latency.

A separate 10-second HEVC probe (`hevc-10mbps`) requested **10 Mbps** instead
of the automatic target of 17,418,240 bps, about 43% lower target bitrate.
It received 580 frames at 60.06 FPS, totaling 11,998,077 encoded bytes. The
complete stream decoded with FFmpeg `-xerror`, exit zero. Equal small-text
legibility at this lower bitrate has not been established, so automatic
bitrate was restored. The phone's streaming status now reports the actual
negotiated codec alongside size, FPS and bitrate.

Final app preferences were verified: rear camera 0, **HEVC**, exact
**1920 × 1080 @ 60 FPS**, automatic bitrate, optical stabilization, autofocus
enabled and exposure zero. The user's enabled camera light and 150% system
font scale were preserved. The More options screen was left visible.

## Latest regression and remaining acceptance

- Android: assembly/install/launch, 41 JVM tests, 57 translated resource keys,
  lint with no issues; seven Python protocol/probe tests pass.
- Windows: release compilation passed. The tracked aggregate run passed 30/31
  CPU suites and stopped before CUDA because `main_window_interactions` failed.
  A subsequent build and test through the tracked CUDA presets passed 35/36
  suites, with the same interaction suite failing. Both runs failed four focus
  cases while a foreground game (`UnityWndClass`) retained focus. The game was
  not terminated or interrupted to force a pass. Decoder and other capture
  suites passed. Logs are `windows-hevc-gate.log`, `windows-hevc-cuda.log` and
  `hevc-decoder-fixtures.txt`. The full gate is **not green**; no release bundle
  was published. Earlier complete/partial gate results remain historical only.
- Physical Logi C310 HD Webcam was retested with the HEVC-capable build
  (`webcam-hevc-regression`): 640 × 480 at 30.03 FPS arrival, about 11.79 seconds
  to first presentation, 252 received / 113 pending replaced / 138 processed /
  137 presented during the 20-second run. Direct GPU remained active with zero
  expired handoffs and no camera error. The final rolling viewport sample was
  14.90 FPS, consistent with the earlier paired/bursty callback behavior. This
  verifies ordinary-camera function alongside the optional phone path, not
  ideal webcam cadence. The driver/source cadence needs separate investigation.
  The index-zero virtual source is not counted as physical-webcam evidence.
- Still pending: controlled cable unplug/replug, long thermal/charging soak,
  Samsung hardware checks, TalkBack/200%-font native acceptance, viewport zoom
  handoff to phone zoom, production adb/APK packaging and release signing.
  Also pending: native missing-HEVC-decoder/failure-retry acceptance and the
  full Windows GUI gate with uncontested foreground focus. VP9/AV1 streaming
  is not implemented.

## Same-bitrate comparison and development binary — later 2026-10-11

Evidence for this continuation is under
`%LOCALAPPDATA%\OkuFlow\phone-codec-comparison-20261011`. The same Pixel 9 Pro
Fold remained connected, now unfolded. The user supplied the best available
stationary room scene. No calibrated text chart or motion target was available.

Four eight-second live captures used rear camera 0, 1920 × 1080 @ 60 FPS,
10,000,000 bps target, autofocus, optical stabilization and the preserved camera
light. Order was HEVC, AVC, AVC, HEVC to reduce simple order bias. All four
elementary streams decoded completely with FFmpeg `-xerror` and no error.

| Codec | Live arrival FPS, runs 1 / 2 | Measured encoded Mbps, runs 1 / 2 |
| --- | --- | --- |
| H.264 | 59.431 / 59.376 | 9.903 / 9.884 |
| HEVC | 59.133 / 59.293 | 9.857 / 9.863 |

These short observations show similar live throughput, not a meaningful speed
winner. They do not measure encoder-only latency or sensor-to-photon latency.

At the user's request, a temporary helper then encoded **one identical picture**
with both of the phone's hardware encoders. The input was frame 180 from the
initial automatic-bitrate HEVC camera recording, decoded to BT.709 limited-range
planar YUV420. This input already contains the original capture's compression;
the comparison measures additional compression of that exact picture, not
fidelity to an uncompressed camera sensor frame. Input SHA-256:
`a3a29d23b66e70a30b8ab948c5c4aef2c448f9c87210c7ffaed03908e63ba412`.

The helper submitted the same 1920 × 1080 image 180 times, timestamps at 60 FPS,
10 Mbps target, one-second keyframe interval and zero B frames. It used
MediaCodec flexible YUV input images, respecting plane row/pixel strides. The
encoders identified as `c2.exynos.h264.encoder` and `c2.exynos.hevc.encoder`, both
hardware accelerated on this Pixel. Those component names do not mean a Samsung
phone was tested. The reported profiles were AVC High and HEVC Main; both used
VBR and BT.709 limited-range SDR. The helper is separate from the app's Camera2
surface input and is not a measurement of that path's processing speed.

| Identical-picture result | H.264 | HEVC |
| --- | ---: | ---: |
| Decoded frames scored | 180 | 180 |
| Encoded bytes, including headers | 3,737,508 | 3,736,546 |
| Actual encoded bitrate | 9.966688 Mbps | 9.964123 Mbps |
| Mean-frame YUV PSNR, FFmpeg aggregate | 44.223763 dB | 44.471432 dB |
| Mean-frame YUV SSIM | 0.995091 | 0.995976 |
| Helper wall time, including input copying | 7.854674 s | 7.669475 s |

Both metrics compare the decoded output against the exact repeated YUV input;
180 score records were checked for each codec/metric. Higher scores mean less
distortion to this reference. HEVC's advantage was small: about 0.25 dB PSNR and
0.000885 SSIM at virtually identical actual bitrate. This is not a percentage
improvement in perceived quality, a moving-scene benchmark, or a general result
for every phone. The helper's copying overhead also prevents interpreting its
wall time as hardware encoder throughput. Results and per-frame metrics are in
`comparison-results.json` and `same-picture/`.

The temporary Java source, class, dex archive and all phone-side helper files
were removed after collecting the results. No benchmark code entered the app
or repository. Original reference/output evidence remains local. Phone settings
were restored to HEVC, exact 1080p/60 and Automatic bitrate; torch, autofocus and
system accessibility settings were preserved.

The codec selector moved directly onto **Advanced settings**, while the full
mode list and bitrate remain one level down in **More options**. The APK build,
41 JVM tests, 57-key translation check, lint and installation passed. The native
`codec-advanced.mp4` recording and decoded frame confirmed the visible codec
control and neighboring labeled image controls on the unfolded phone at 150%.
HEVC failure downgrade behavior was not changed by this placement edit.

With the owner using the desktop, `OKUFLOW_AGENT_EXCLUDE_GUI=1` and the tracked
`scripts/agent_build.bat` passed release compilation, 24/24 CPU suites and 29/29
CUDA suites. This is explicitly partial validation: GUI suites remain pending.
Qt 6.12 runtime deployment succeeded beside the ready-to-run development binary
at `build/msvc-release/cmake/oku_flow.exe`; the application was not launched to
take desktop focus and no existing release bundle was replaced. Executable
SHA-256: `ecc9f47e5a68c3128a4a673cda699024f90290b48ca8842bda504911f92d2ce9`.

## Phone UX review and HEVC default — final 2026-10-11 continuation

The Opus review changes were inspected, corrected, built and installed with
`scripts/build_android.ps1 -Lint -Install`. Final results: **48 JVM tests**, zero
failures/errors, **70 English/Turkish/German resource keys**, and zero lint
issues. The seven Python protocol tests also passed. The screen draft rendered
all nine states in all three languages (27 combinations); this is browser
draft evidence, separate from native phone evidence below.

New installs and Restore automatic settings now prefer HEVC. An explicit saved
H.264 choice is preserved, and hardware/host compatibility still permits AVC
fallback. The codec control remains directly in Advanced settings. Both settings
levels show a current status card; outlined navigation and centered section
headings retain the existing text sizes. Connected and Streaming are distinct,
and named problems with actionable hints remain visible after host disconnect.

Parent review retained the normal manual-lock boundary: the activity does not
show over the lock screen or wake a locked phone. Keep-awake still applies while
the app is visible. Connected wording now covers an idle/paused host as well as
startup, stale mode details are cleared when capture stops, and the server
publishes its connected callback before acknowledging the video binding so a
fast start cannot be overwritten by a late Connected update.

Evidence is under
`%LOCALAPPDATA%\OkuFlow\phone-codec-comparison-20261011\ux-review`. The Pixel 9
Pro Fold was on its folded cover display in landscape at 150% system font size
for the final native captures. Original screen recordings and decoded PNGs were
inspected; intermittent incomplete `screencap` text was not treated as a native
layout result.

| Check | Observed result |
| --- | --- |
| Bound host before capture | UI hierarchy reported Connected to OkuFlow and the waiting-for-video hint. The recording targeted an inactive display, so this check has no valid pixel evidence. |
| `connected-streaming`, 30 seconds | HEVC, 1920 × 1080 @ 60, automatic 17,418,240 bps target; 1,772 frames at 59.908 arrival FPS. `streaming.mp4` shows the complete live state and mode. |
| Real camera conflict | Opening the phone's normal camera interrupted capture. The host received `Camera disconnected`; the app showed The camera is busy with an amber ring and recovery instructions, including after host disconnect. `busy.mp4` verifies the native panel. |
| `avc-only-recovery`, 30 seconds | A fresh host session offering only AVC cleared the problem and received H.264, 1920 × 1080 @ 60; 1,776 frames at 59.916 arrival FPS. The saved phone preference remained HEVC. |
| `status-native`, 40 seconds | HEVC resumed at 59.886 arrival FPS (2,374 frames). `live-status.mp4` shows the Advanced settings status card with Streaming, 1920 × 1080, 60 FPS, HEVC and 17 Mbps, plus outlined navigation and centered headings. |

The complete HEVC `connected-streaming` and AVC `avc-only-recovery` elementary
streams decoded with FFmpeg `-xerror`, exit zero. These are short transport and
recovery checks, not latency or thermal-soak measurements. The deliberate camera
conflict also exposed a probe cleanup error (`WinError 10038`) after the socket
closed; its recorded ERROR packet and the native problem panel verified the app
behavior, but the probe's exception reporting remains imperfect.

Rear camera 0, exact 1920 × 1080 @ 60, HEVC preference, automatic bitrate, optical
stabilization, autofocus, exposure zero, the user's enabled light and 150% font
scale were preserved. The app was left in Advanced settings after host closure.

No Windows code changed in this UX review. The development executable and its
partial non-GUI validation above remain current; no desktop app was launched or
release bundle published. The AVC-only host check verifies negotiation fallback,
not the native Windows missing-decoder or decoder-failure retry paths. Other
problem panels, physical cable removal, long soak, Samsung phones, native
TalkBack/200%/translated layouts and the full Windows GUI gate remain pending.

## New Samsung installation and explicit capture controls — 2026-10-11

Installed and launched package `com.okuflow.camera` version `0.1.0` on the
newly authorized Samsung SM-G780F (Android 13/API 33). Camera permission was
granted, and both Developer options and USB debugging reported enabled.
The five-second local USB protocol probe negotiated HEVC at 4032×3024/30 FPS
and received 127 encoded frames at approximately 29.86 arrival FPS. This probe
advertises broader limits than the Windows receiver; it does not validate that
size in the Windows viewport or establish image quality or sensor-to-display
latency. Capture metadata reported the rear light available and disabled.
Probe media and metadata remain outside the repository and release bundle.

The current Android build passes assembly, 49 JVM tests, lint, 71-key
English/Turkish/German resource parity, and seven Python host-protocol tests.
Windows release compilation and the desktop-restricted tracked matrix pass:
24 CPU tests and 29 CUDA tests. The executable's PE subsystem is Windows GUI.
The owner reports completing Windows GUI testing and explicitly approved
publication as tested. The automated GUI suite was not run for this publication;
`OKUFLOW_SKIP_BUNDLE_TESTS=1` explicitly bypasses the bundle's full CTest rerun.

## Automatic connection and source-built transport — 2026-10-12

The updated debug APK was assembled, linted, installed and launched on the
authorized Pixel 9 Pro Fold. Validation passed 54 JVM tests, 71-key
English/Turkish/German resource parity and eight Python protocol tests.
Discovery twice returned capabilities without a stream token, session claim
or busy state. Transient camera errors now permit a later retry without replug.

The tracked Windows startup profile used isolated settings and a normally
exiting child. Windows automatically selected the ready phone; it negotiated
HEVC 1920×1080/60. Over 20 seconds, measured arrival was 59.511 FPS and viewport
presentation was 59.055 FPS, with 1075 received and 1066 presented frames.
Ingress reported six drops and 43 presentation attempts encountered a busy
queue; these are separate counters, not a claim of zero dropped frames.
Capture-arrival-to-presentation submission p95 was 6.831 ms, not measured
sensor-to-photon latency. First phone presentation was submitted at 2071 ms.
The existing shared adb server remained running and unchanged.

The full unattended Windows matrix passed: release compilation, 32 CPU and
37 CUDA tests including seven GUI tests in each suite. The test harness now
verifies native Windows foreground focus before keyboard/idle assertions;
no owner clicks were required.

The bundled adb and USB DLLs were compiled from pinned source. Dependency
closure, source/notice staging and a hidden private-port server smoke test
passed. That test verified loopback binding, protocol 41 and device listing
while preserving the shared server. It does not establish clean-PC USB driver
installation or a cold source-built server taking over the already attached
phone. Exact rebuild/source materials are included in the release; see
[adb_distribution.md](../docs/adb_distribution.md).

## Play-installed unplug/reconnect crash diagnosis — 2026-10-13

Read-only diagnostics on the authorized Pixel 9 Pro Fold recovered two buffered
`com.okuflow` main-thread crash traces at 00:50:43 and 00:52:34. Both report
`Unable to stop service` caused by `IllegalArgumentException: Thermal status
listener was not added`, from `CameraStreamService.onDestroy`. The service had
acquired `PowerManager`, but partial startup had not registered its listener;
shutdown incorrectly treated an initialized manager as listener ownership.
The installed Google Play app is version code 1 / `0.1.0`. The phone also retains
an input-focus ANR at 00:52:39; its thread stack has not been recovered, so that
ANR is not attributed to the thermal exception.

The fix tracks successful thermal registration and tolerates partial cleanup.
It retains an existing authorized foreground listener across cable removal,
releasing the host/camera on EOF or the two-second USB debounce. New foreground
starts require a resumed activity; a rejected start waits for a fresh resume.
Pause, revoked camera permission and debugging explicitly off still stop the
service. Android 17 unknown settings and older-version checks are retained.
Version code 2 / `0.1.1` is prepared for the owner's later Play update.

Crash/lifecycle logs remain under
`%LOCALAPPDATA%\OkuFlow\android-usb-crash-20261013`, outside Git. Structured
adb discovery identified the USB transport before querying by transport ID;
the existing shared adb server and logcat buffers were preserved. The Play app
was not replaced, uninstalled, wiped or updated. Physical unplug/replug and
background reconnect acceptance of the fixed Play-signed build remain pending;
local JVM tests cannot establish those hardware results.
