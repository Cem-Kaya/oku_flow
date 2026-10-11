# OkuFlow Camera protocol v1

Implemented in `link/WireProtocol.kt`, `link/UsbSocketServer.kt`,
`scripts/android_camera_probe.py` and `src/capture/phone_camera.cpp`.
This document describes the current wire contract. Plan 51 describes later work.

## Transport and lifecycle

The phone listens on the abstract Unix socket `okuflow_camera`. It accepts only
Android shell (uid 2000) or root (uid 0) peers. The camera foreground service
starts from the resumed activity after camera permission, USB debugging and
the USB connection heuristic are satisfied. Turning debugging off, removing
permission or disconnecting USB stops the service. No INTERNET permission is
declared. The peer uid alone does not prove that a connection uses a cable.

Both hosts connect directly to the running adb server at `127.0.0.1:5037`.
The initial `host:track-devices-proto-binary` snapshot identifies
`ConnectionType.USB` and authorized `ConnectionState.DEVICE`. This works when
native Windows adb supplies no bus address. Connections select that
`host:transport-id:<id>` and then `localabstract:okuflow_camera`. Older servers
fall back to `host:devices-l`, accepting only entries with explicit `usb:`
evidence. Wi-Fi, emulators and unknown transports are excluded. Neither host
starts, upgrades, kills or reconfigures the server or adds adb forwards.
Schema reference: [AOSP devices.proto](https://android.googlesource.com/platform/packages/modules/adb/+/7806e425e18a48a0f9e580e381cdc86ed5478a4c/proto/devices.proto).

One control connection and one video connection form a session. A second
control host is rejected. The service generates a random session token for
binding the video channel. The token is installation/session-local, never
included in probe logs. Closing either connection retires both and releases
the camera. An authorized computer has the normal privileges of USB debugging.

## Hello and packet framing

All integers are little-endian. Each connection starts with:

| Field | Size | Value |
| --- | --- | --- |
| Magic | 4 bytes | ASCII `OKUF` |
| Version | u16 | `1` |
| Role | u16 | `1` control, `2` video |
| JSON length | u32 | 2–65536 bytes |
| Body | UTF-8 JSON | Object |

Control hello: `{"codecs":["avc","hevc"]}`. Optional decoder ceilings are
`maxWidth`, `maxHeight`, `maxPixelsPerSecond`. The native Windows receiver
offers AVC, plus HEVC when a local activation/input/NV12-output probe succeeds,
with 4096×2304 / 251658240 pixels per second limits. The phone
intersects those limits with the selected camera, hardware encoder and saved
settings. HEVC is the preferred codec for new installations and Reset; it is
used only when the saved preference, a local hardware encoder and the host all
support it. Otherwise the phone sends AVC. An explicitly saved AVC preference
is kept. The control ACK contains `protocol`, `token`, `cameras`, `codecs`,
`hostCodecs`. Each camera reports `id`, `facing`, `orientation`, `zoomMin`,
`zoomMax`, `torch`, `timestampSource`. Numeric facing values follow Camera2.

Video hello: `{"token":"<control ACK token>"}`. Its ACK is
`{"channel":"video"}`. Bind the video connection before sending commands.

Subsequent packets have an eight-byte header:
`u16 type, u16 flags, u32 payloadLength`, followed by payload bytes.

| Type | Value | Body |
| --- | --- | --- |
| ACK | 1 | JSON handshake response |
| COMMAND | 2 | JSON host command |
| STATE | 3 | JSON phone event |
| METADATA | 4 | JSON capture result |
| ERROR | 5 | `{"error":"diagnostic"}` |
| FRAME | 6 | Binary encoded video |

Control payloads are at most 64 KiB; a FRAME payload is at most 16 MiB,
including its prefix. JSON packets require flags zero. FRAME flags are bit 0
key frame, bit 1 codec configuration; other bits are invalid. Parsers reject
truncation, invalid versions and lengths before allocating the payload.

Golden COMMAND vector for `{}`:
`02 00 00 00 02 00 00 00 7B 7D`.

## Commands and events

Commands use an `op` string and an optional numeric `id` echoed by
`commandAccepted` or `pong`. Acceptance means the request was queued;
capture metadata reports the actual hardware result. Camera selection uses
`cameraId` independently of numeric request `id`. The phone and Python client
also accept the initial prototype's `camera` + string `id` spelling for compatibility.

| op | Additional fields | Behavior |
| --- | --- | --- |
| `start` | None | Negotiate and start with saved phone settings |
| `stop` | None | Release capture; retain the socket session |
| `zoom` | `ratio` positive finite number | Clamp to the exposed zoom ratio range |
| `camera` | `cameraId` exposed camera id | Persist and restart using that camera |
| `torch` | `enabled` boolean | Session-only light request; apply when available. Starts off on every binding; Stop/disconnect clears it. |
| `exposure` | `steps` integer | Persist compensation; clamp to camera range |
| `autofocus` | `enabled` boolean | Continuous video AF; off requests infinity |
| `keyframe` | None | Request a sync frame from MediaCodec |
| `ping` | None | Keepalive; respond with `pong` |

Hosts send `ping` every five seconds. Control inactivity times out after
30 seconds; hello has a five-second deadline. A blocked writer is closed
after three seconds, using one snapshot of the volatile write-start timestamp
to avoid mistaking a completed write for a stall. No arbitrary inter-frame packets are dropped: video
queue pressure disconnects the session so the next start begins with a
fresh codec configuration and key frame. The video queue holds at most
32 packets / 32 MiB; control holds 128 packets / 512 KiB.

`configured` precedes video for a generation. `streaming` follows the first
non-configuration encoded frame. Both report `generation`, `camera`, `width`,
`height`, `fps` (negotiated upper bound), `bitrate`, `mime`, `orientation`,
`facing`. `streaming` also reports the output `timestampBase`: `sensor` on
API 33+, otherwise `device-default`. Startup tries up to six capability-probed
candidates from the local camera/encoder intersection, bounded by the saved
size/rate pair and host limits. It reduces size/FPS or falls back to AVC after rejection
or four seconds without frames. Thermal status SEVERE requests at most 30 FPS;
CRITICAL stops capture and requires a new start after cooling.

## Video and capture metadata

FRAME begins with `i64 ptsUs, i64 sequence, u32 generation` (20 bytes), then
Annex B H.264/HEVC bytes. Configuration packets contain start-code SPS/PPS
(and VPS for HEVC). The sequence starts at zero for each encoder generation,
including configuration packets. Non-configuration PTS must advance within
the generation. Encoders request no B-frames and a one-second key interval.

METADATA includes `generation`, Camera2 `frameNumber`, `sensorTimestampNs`,
applied `zoom`, `crop` `[left,top,right,bottom]`, `physicalCamera`,
`exposureNs`, `exposureCompensationSteps`, `iso`, `afState`, `aeState`,
`focusDistance`, `ois`, `eis`, `torchAvailable`, `torchEnabled`. The two optional
torch Booleans report flash hardware availability and the applied Camera2 flash
mode; older companions omit them, so Windows keeps its light button disabled.
Metadata and video have independent sequences.
API 33+ explicitly selects SENSOR output timestamps; older devices need
timestamp mapping validation before metadata-driven residual zoom is enabled.

The probe records generations as separate elementary streams and metadata as
JSONL. Native Windows decodes AVC and optional HEVC through system Media Foundation
MFTs, requesting low latency and sharing its capture DXGI manager when supported.
HEVC uses Main 8-bit / Annex B input and NV12 output. Its codec is not bundled:
the installed Windows decoder must activate and pass an input/output probe.
If HEVC decoder creation/processing fails or produces no initial frame for four
seconds, Windows closes both channels and reconnects once offering AVC only.
This changes negotiation for that session, not the saved phone preference.
The phone status includes the actual codec, so AVC fallback is visible.
Samples pin decoder textures until consumers release them; CPU NV12 is the
fallback. Native capture timestamps are continuous per-session presentation
times, rebased across encoder generations; `captureClock100ns` is host arrival
time. Neither path claims measured sensor-to-display latency.

The desktop source exposes queued zoom/camera APIs, and discovered camera IDs
appear as camera-list entries. Automatic handoff of OkuFlow's viewport zoom,
per-frame metadata joins, clock synchronization and high-speed capture above
60 FPS remain separate plan-51 work.
