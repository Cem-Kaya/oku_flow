# Phone camera zoom and saved camera settings

Development changes after Windows 0.3.1; not included in that published installer.

The Windows Camera picker keeps a separate **Auto** entry for each phone. New
automatic phone connections select that entry; an explicitly saved webcam or
phone lens still wins. With the updated companion, Auto explicitly selects its
automatic rear camera instead of inheriting a previous manual phone lens.
Android prefers a logical rear camera, then the rear camera whose reported
field of view is nearest a conventional main camera. The phone's Camera2
implementation controls any automatic lens switching; optical switching is not
guaranteed on every device or at every zoom ratio.

Both pickers label known rear lenses **Ultrawide camera**, **Main camera** or
**Telephoto camera**. Labels use approximate 35-mm-equivalent focal length from
the reported focal length and sensor diagonal: below 20 mm, 20–45 mm, and 45 mm
or above. Camera IDs remain visible to distinguish multiple lenses of one kind.
Missing metadata retains a generic camera label; numeric IDs are not interpreted
as lens roles. These are field-of-view descriptions, not manufacturer model names.

The companion also lists physical cameras hidden behind a logical camera when
Android exposes their metadata. It opens their logical parent and attaches the
selected physical ID to the recording output. A device can reject that encoded
physical-camera configuration: ordinary bounded mode attempts then fail visibly;
the app does not silently substitute another lens. A public logical camera
selected by ID can still use the manufacturer's automatic lens behavior.

**Settings → Camera → Phone camera zoom** changes the phone's own camera zoom.
It is separate from viewport magnification, so the two factors are never silently
combined or compensated. Auto exposes the actual logical camera's advertised
zoom-ratio range, including ratios below 1 where available. Unknown ranges and
fixed physical outputs expose no adjustable phone zoom. The displayed value is
requested intent, not a per-frame applied-zoom measurement. Ordinary viewport
zoom still works on webcams, selected videos and frozen images.
Existing Assistant/MCP zoom commands continue to change viewport magnification.
Wheel scrolling over the phone-zoom field does not edit it; use its buttons or
keyboard to change the ratio.

The last requested phone zoom is saved per stable phone/lens identity and
clamped to the current handshake's range on reconnect. The transport retains it
across its own AVC fallback. New native zoom requests retire existing UCT proof;
the new image still needs ordinary current-frame recognition/probing. Zoom uses
the existing latest-only control queue, never a blocking UI socket call.

The initial saved camera settings cover **capture format and orientation** per
device/lens, plus the separate phone zoom preference. The first successful legacy
source adopts its current settings. Switching to a known source restores its
settings; a new source does not inherit another device's explicit capture format.
Temporary Assistant camera ownership does not write these format/orientation
records. Quick modes and media-file orientation keep their existing ownership.
This is not a generic UVC focus/exposure/white-balance implementation.

Storage adds bounded `cameraWorkingSets` and `phoneCameraZoom` objects to the
existing settings document; it stores no transient transport IDs or native PIDs.
Phone light remains session-only and starts off. Old companions remain compatible;
without the new automatic selector, their existing default-camera behavior remains.

## Validation scope

Development validation on 2026-10-24:

- Android assembly, 102 JVM tests across 16 suites (including lens identity),
  and lint passed, with no failed or skipped unit rows. Host protocol: 11 PASS.
- Windows CPU compilation and the tracked **SCOPED 5/246 PASS** gate covered
  phone transport, settings round trip, actual-app offscreen zoom/settings,
  language catalogs and shared-source equivalence. No skipped or unresolved
  selections; initial test time 76.39 seconds. The final wheel-scroll regression
  rechecked the actual-app fixture and source guard in 16.08 seconds, retaining
  three matching fingerprinted passes. This is not a full release gate.
- The production CUDA `oku_flow` target also compiled successfully. This is a
  compilation check, not GPU/phone runtime or full CUDA-suite acceptance.
- All 24 catalogs contain 890 finished translations. All 96 new label entries
  received language review; 12 replacements were applied, zero entries remained
  unread. The Android lens labels reuse the same reviewed wording.
- The initial Windows selection lacked its opt-in fixture. A later link failure
  exposed a direct application dependency in language refresh; replacing it with
  a Qt signal fixed the link. Both original logs remain preserved. Tests were
  run after the repair, without dropping an assertion or changing a deadline.

Logs, reviews and source receipts live outside Git in local-work-data under
`phone-zoom-ocr-design-20261024`.

Android assembly, JVM tests and lint cover compilation, role/default selection
and existing stream policies. Host protocol fixtures and Windows phone/settings
contracts use private fixtures. The 24 translated catalogs have model-reviewed
labels, with structural parity checked separately. Neither those checks nor a
debug APK establish physical lens availability, actual optical quality, native
screen-reader behavior or a new Play/Windows release.

Real-device follow-up should confirm Auto and each exposed lens, zoom endpoints,
USB reconnect/AVC fallback, mode rejection and saved source restoration. Keep
observations in [DEVICE_VALIDATION](../android/DEVICE_VALIDATION.md).

Platform contracts: [Camera2 multi-camera](https://developer.android.com/media/camera/camera2/multi-camera),
[logical-camera zoom](https://source.android.com/docs/core/camera/multi-camera),
[physical output selection](https://developer.android.com/reference/android/hardware/camera2/params/OutputConfiguration#setPhysicalCameraId(java.lang.String)).
