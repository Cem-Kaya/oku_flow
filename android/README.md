# OkuFlow Camera (Android)

Companion app that turns an Android phone into an OkuFlow camera over the
USB cable. Design, protocol and roadmap:
[`improvement_ideas/51-okuflow-camera-android.md`](../improvement_ideas/51-okuflow-camera-android.md).
Toolchain and build commands: [`BUILDING.md`](BUILDING.md).

## Status

The app includes onboarding, Camera2 hardware encoding over USB debugging,
remote camera controls and a saved Advanced settings menu. It walks through
Developer mode → USB debugging → camera permission → plugging in, one
instruction per screen. Android 17 hides the first two setting values, so the
app uses the current USB adb function when available and treats redacted
values as unknown. It offers connection help instead of falsely claiming
Developer mode is off; older Android versions retain their setting checks. The native Windows receiver accepts H.264 and,
with an installed system decoder, HEVC into the existing MediaFrame pipeline.
The Python probe also receives HEVC.
The implemented wire contract is in [PROTOCOL.md](PROTOCOL.md).

After setup the phone always states what is happening and what to do next.
It shows **Open OkuFlow on your computer**, then **Connected to OkuFlow** while
waiting for camera video, then **Streaming to OkuFlow** with the actual mode.
Failures replace the raw English diagnostic with a named problem and its remedy,
marked by an amber warning ring: camera busy, camera access blocked (button
opens privacy settings), video mode unavailable (button opens Advanced
settings), camera stopped, phone too hot and USB camera could not start.
Hints tell users that OkuFlow on Windows retries for a short time and, if the
picture does not return, to choose the phone again in its camera list. A
problem remains visible after Windows disconnects. It clears on the next
session or stream, after cooling (heat) or after unplugging. The raw diagnostic
still reaches Windows. Problems use Android's rejection vibration; other step
changes use the confirmation vibration, without one for the brief connected state.

Unlock the phone and leave OkuFlow Camera visible when connecting. The app
does not show over the lock screen or wake a manually locked phone.

This is an optional extra camera source. Normal USB cameras continue through
OkuFlow's existing Media Foundation path and must require no phone app, adb
or Android tools. Selecting a webcam prevents phone hot-plug from taking
over. Plan 51 includes a normal-camera regression gate for the later Windows
integration. Phone enumeration is appended to the physical camera list;
normal cameras continue through their existing activation/capture branch.

The Android toolchain is installed and the debug APK builds with 49 passing
JVM tests. Run `pwsh -NoProfile -File scripts/build_android.ps1 -Lint` from
the repository root. This also checks English/Turkish/German resource key
parity. Outputs and caches stay under local app data, outside Google Drive.

Review the [interactive screen draft](design/setup-panels.html) in a browser.
It uses the app's translated strings and illustrates nine states,
including connected, streaming, a camera problem and hidden-setting connection help. Native Compose previews also cover
every state, permission denial, landscape, and German at 200% font scale.
Real Pixel 9 Pro Fold validation and remaining acceptance checks are recorded
in [DEVICE_VALIDATION.md](DEVICE_VALIDATION.md). Neither preview replaces hardware testing.
When app wording changes, update the draft's embedded `translations` data
from the three Android resource catalogs.

The Settings button opens About phone or Developer options, with OEM
fallbacks. Android still requires manually enabling Developer mode and USB
debugging. The app rechecks them when you return. Camera-permission denial
survives app restart so the one-tap app-settings shortcut remains available;
granting access clears it. This installation-local history is excluded from
backup and device transfer.

## Layout

```
android/
  app/src/main/java/com/okuflow/camera/
    MainActivity.kt              entry point (Compose)
    camera/                     Camera2 catalog, mode selection, encoder and foreground service
    link/                       bounded USB protocol/socket server and host connection state
    settings/                   saved camera/quality/stabilization preferences
    setup/                       onboarding: state resolution, settings observers, system-screen intents
    ui/                          single-screen panels and theme
  app/src/main/res/              strings (en, tr, de), theme, launcher icon
  app/src/test/java/             JVM unit tests
```

## Try the stream

Install/open the debug APK once using `scripts/build_android.ps1 -Lint -Install`
with one authorized USB phone, or install the built APK manually. Complete the
three setup steps and allow the computer's USB debugging prompt. This prototype
requires a running local adb server; the prepared SDK or an existing compatible
tool such as scrcpy can provide it. Windows bundles do not yet install or ship adb.

Start the newly built Windows executable and select **OkuFlow Camera · device**
under Advanced → Settings → Camera. With the phone app open during discovery,
additional entries identify its camera IDs. The default phone entry retains
the phone's saved camera; selecting an ID sends the camera command. The saved
Windows phone choice persists. New installations and **Restore automatic
settings** prefer **HEVC / H.265**; choose **H.264** under **Advanced settings →
Preferred codec** to keep AVC. An existing saved H.264 choice is not changed.
Windows advertises HEVC only when its installed Media Foundation decoder
activates and accepts NV12 output; otherwise the phone uses H.264, as it also
does without a local hardware HEVC encoder. A decoder failure reconnects once with H.264
without changing phone preferences. No codec download is required for H.264.

For independent transport validation:

```powershell
python -B scripts/android_camera_probe.py --list
python -B scripts/android_camera_probe.py --launch --seconds 30 --interactive
python -B -m unittest discover -s android/tests -v
```

Enter JSON such as `{"op":"zoom","ratio":2}` or `{"op":"camera","cameraId":"1"}`
using a camera ID from the printed capabilities. The probe saves elementary
video, metadata and arrival-cadence summaries under local app data; use
`--output` to choose a new directory. Play a captured generation with
`ffplay camera-g1.h264` or inspect it with `ffprobe`. Those tools are optional,
separately installed, and not part of the APK or Windows runtime.

The screen stays awake while any app screen is visible, including setup and
Advanced settings. Leaving the app restores normal screen timeout. It does not
override a manual lock or unlock the phone.

## Advanced settings and limits

Settings use high-contrast labeled rows in Camera and video / Image controls
groups. Dropdowns use bright 2-dp outlines, wider menus and taller padded choices,
without increasing text size. Each menu opens at its selected value. Labels stay
with their values; autofocus and light switches
keep visible adjacent labels. Exposure shows its current value and brightness
guidance. The outlined back control stays above the scrolling content. Section
headings are centered. A **Stream status** card comes first on both levels.
It shows the main screen's current state and, while connected, the actual
size, FPS, codec and bitrate, so changes and fallbacks are visible. The main
screen's **Advanced settings** entry uses the same 2-dp outline. Content
width is bounded on wide/foldable screens, and rows grow with system font size.

The menu saves camera, a combined resolution/FPS mode, bitrate, AVC/HEVC preference,
Off/Optical/Electronic stabilization, continuous autofocus and exposure
compensation. Changes apply after a short debounce and restart capture when
needed. Unsupported light/optical modes stay off; actual capture metadata
reports the result. Autofocus off requests infinity focus. Reset restores
automatic resolution/bitrate, a 60-FPS ceiling, preferred HEVC (with AVC
fallback) and optical stabilization when available. Default mode selection prioritizes detail, then FPS.

Camera light is session-only and always off after app startup, host binding,
Stop, or disconnect. Old saved light-on values are removed. Turn it on explicitly
with **Settings → Camera → Turn on phone light** in Windows OkuFlow; the phone
settings show where to find that control. Capture metadata reports the actual
light availability and state. No light-on preference is restored on reconnect.

The main **Resolution and frame rate** dropdown lists actual pairs such as
**1920 × 1080 @ 60 FPS** and **1920 × 1080 @ 30 FPS**, plus Automatic. It shows
only modes whose shorter edge is at least 720 pixels and whose rate is at least
24 FPS. **More options** opens a separate screen one menu level down: **All
camera modes** contains the entire detected list together, including the main
choices and lower resolutions/rates. Bitrate and reset also live on that
screen; preferred codec is on the main settings screen. The back button or system Back returns one level.
A low mode selected there remains saved and clearly displayed on the main screen.
The filter only simplifies the picker; it does not remove older-phone or thermal
fallbacks. Both screens have independent scroll positions. The phone
queries its selected Camera2 recording outputs, minimum frame durations and AE
rate ranges, then intersects these with hardware surface-encoder capabilities.
The list refreshes locally when camera or preferred codec changes; it needs no
PC connection or network. HEVC preference retains the AVC fallback. Dimensions
are the native recording buffer size, before display rotation. No resize stage
invents a missing size or rate.

The selected dimensions and rate persist together. Older ceiling preferences
migrate to an available pair when settings opens. Switching to a camera/codec
without that pair selects a supported smaller/slower pair, or Automatic when
none fits. Capture also respects host decoder and thermal limits; actual stream
status reports any fallback. Advertised hardware support is not a measured FPS
guarantee. Automatic prioritizes detail, then frame rate (up to 60 FPS).
See [device validation](DEVICE_VALIDATION.md) for the tested phone's actual modes.

API contracts: [Camera2 output sizes/durations](https://developer.android.com/reference/android/hardware/camera2/params/StreamConfigurationMap)
and [encoder size/rate support](https://developer.android.com/reference/android/media/MediaCodecInfo.VideoCapabilities).

Regular recording sessions are limited to 60 FPS; custom GPU/gyro processing,
high-speed sessions, measured thermal/link adaptation, clock synchronization,
automatic viewport-to-phone zoom handoff and production packaging remain
plan-51 work. OkuFlow's normal zoom control still applies desktop magnification;
the protocol/probe and queued capture APIs can control phone zoom directly.

The tests cover framing, NAL conversion, mode/preference policy, a fake adb
server with separate channels and remote commands, USB identification without
a Windows bus path, the Windows system decoder, and saved source selection.
See [the device validation record](DEVICE_VALIDATION.md) for real-stream
results. TalkBack, older-phone hardware coverage and a long thermal soak
remain acceptance work. The Windows GUI gate needs an uncontested desktop.

## Rules

- Product name **OkuFlow Camera**; package `com.okuflow.camera`.
- No INTERNET permission: frames leave the phone only through the adb
  tunnel to OkuFlow.
- Camera2 and MediaCodec directly — no CameraX.
- Keep `values/`, `values-tr/` and `values-de/` strings in exact key parity.
- Low-vision first: white-on-black text, one large action per screen,
  centered headings/step counters/progress, justified instruction paragraphs,
  merged panel semantics and scrolling layouts for large system fonts.
  TalkBack behavior and 200% font-scale rendering still need device acceptance.
