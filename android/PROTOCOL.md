# OkuFlow Camera protocol v1

Implemented in `link/WireProtocol.kt`, `link/UsbSocketServer.kt`,
`scripts/android_camera_probe.py` and `src/capture/phone_camera.cpp`.
This document describes the current wire contract. Plan 51 describes later work.

## Transport and lifecycle

The phone listens on the abstract Unix socket `okuflow_camera`. It accepts only
Android shell (uid 2000) or root (uid 0) peers. The camera foreground service
starts from the resumed activity after camera permission, USB debugging and
the USB connection heuristic are satisfied; the resumed activity retries every
three seconds while the service is not running, including after a listener
failure, unless the user saved Pause. Pause stops the listener/camera and persists
across app reopening or automatic host launch; only explicit Resume clears it.
The canonical install package is `com.okuflow`, launched through
`com.okuflow/com.okuflow.camera.MainActivity`; legacy development packages use
`com.okuflow.camera/.MainActivity`. The abstract socket and protocol version are
unchanged. Turning debugging off or removing permission stops the service at
once. A listener already started with foreground camera authorization survives
USB unplug/replug: EOF retires the camera/session, and a USB-disconnected
broadcast also retires any remaining host session after two seconds. The
abstract endpoint stays bound for a fresh host session. This does not create
a new camera foreground service from a background activity. Rejected foreground
starts wait for the next activity resume instead of retrying every three seconds.
A peer whose credentials cannot be read is closed without affecting the
listener. No INTERNET permission is
declared. The peer uid alone does not prove that a connection uses a cable.

The Python host connects directly to the running adb server at `127.0.0.1:5037`.
Windows borrows a listener there or an explicit local endpoint override. When
neither exists, Windows launches an owned server on a private ephemeral
loopback port and uses that endpoint for all host/transport requests.
The initial `host:track-devices-proto-binary` snapshot identifies
`ConnectionType.USB` and authorized `ConnectionState.DEVICE`. This works when
native Windows adb supplies no bus address. Connections select that
`host:transport-id:<id>` and then `localabstract:okuflow_camera`. Older servers
fall back to `host:devices-l`, accepting only entries with explicit `usb:`
evidence. Wi-Fi, emulators and unknown transports are excluded. The Python probe never owns the server lifecycle. Windows polls in the
background every five seconds and, without an existing listener or endpoint
override, may directly start a hidden local `adb -L tcp:<port> server nodaemon`. It never
runs client start-server/kill-server commands, upgrades or replaces another
tool's server, or adds adb forwards. Local TCP endpoint overrides are honored;
remote or unsupported endpoints are refused. Bootstrap retries wait at least
30 seconds, including failed launches and unexpected child exits. The exact
owned child is assigned to a non-inherited kill-on-close Windows job before
it resumes; normal OkuFlow shutdown and process crash both terminate it.
Owned bootstrap requires phone-use intent; an explicit webcam choice prevents
it and relinquishes an idle owned server. Emulator scanning is disabled with
`ADB_EMU=0`; owned support rejects external host-kill requests with
`ADB_REJECT_KILL_SERVER=1`. WinUSB ownership can hide these phones from a second server;
cooperating tools can use the logged private port or start SDK adb first.
Readiness is polled for five seconds in 50ms slices. Two consecutive
unresponsive host-version probes allow restarting only that owned child, with
cancellation and live/starting camera sessions taking precedence. Pre-existing
and overridden servers remain borrowed. Only confirmed-hung, reviewed legacy
OkuFlow bundles qualify for the separately provenanced reclamation in
docs/adb_distribution.md; unknown/SDK/overridden servers remain untouched.
The host-version read deadline is one second per bounded field. Listener
ownership is checked against the exact held child process; a foreign listener
on a raced port is not accepted as owned. Dead children can yield to a newly
responsive default server after its sessions finish. Startup/recovery reports automatic retry without
manual process-management instructions. Shutdown latches out new owned launches.
After a failed stream or idle preferred-phone probe, Windows may issue
`host-transport-id:<id>:reconnect` only after a responsive adb version/listing
and two consecutive 2-second full-token echo timeouts, followed by a fresh
non-claiming companion timeout. The same authorized USB transport must remain
listed after these checks. A per-process, per-phone/server one-minute limit
survives transport-ID changes, but not process restarts. Healthy or definitive
phone answers suppress health probes for 30 seconds. Busy/paused/error states and other discovered phones do
not trigger recovery; other adb clients of the selected phone briefly lose
that transport. Global hangs use the guarded owned/legacy lifecycle above,
or a private shadow while an unknown listener remains untouched. An unused
shadow hands back only after no raw transports and two healthy default probes.
Server epochs include process creation time to distinguish reused transport IDs.
After acknowledgment, a 2-second window must observe disappearance or
a new transport ID before reporting transport recovery. Normal discovery and
a fresh camera session follow; recovered transport is not successful video delivery.
Schema reference: [AOSP devices.proto](https://android.googlesource.com/platform/packages/modules/adb/+/7806e425e18a48a0f9e580e381cdc86ed5478a4c/proto/devices.proto).

One control connection and one video connection form a session. A second
control host receives ERROR `{"error":…,"code":"sessionBusy"}` and is closed.
The video connection must bind within five seconds of the control ACK, or the
phone closes the control connection and frees the slot. The service generates a random session token for
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

Probe hello (role 1): `{"probe":true}`. It never claims the session, allocates
a token or touches an active session. The phone replies with one ACK and closes:
the capability fields below plus `protocol`, `probe: true`, `busy` (another
control session exists) and `problem` (current problem code or `null`). Hosts
use it for discovery and back off while `busy` or `problem` is `hot`/`busy`/`blocked`.
Capabilities are built once per listener (warmed in the background) and copied
per reply.

While the activity is resumed, it refreshes host/service state every three
seconds. A disconnected `busy` or `failed` camera problem remains visible for
at least 30 seconds, then clears to permit a fresh host attempt. A blocked
camera clears after access returns; `noMode` clears after camera settings change.
Thermal recovery remains tied to the actual thermal state. These transitions
avoid a stale probe problem permanently preventing Windows from reconnecting.

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
| ERROR | 5 | `{"error":"diagnostic","code":"…"}` |
| FRAME | 6 | Binary encoded video |

Control payloads are at most 64 KiB; a FRAME payload is at most 16 MiB,
including its prefix. JSON packets require flags zero. FRAME flags are bit 0
key frame, bit 1 codec configuration; other bits are invalid. Parsers reject
truncation, invalid versions and lengths before allocating the payload.

ERROR `code` is optional (older phones omit it; hosts treat absence as
unclassified): `busy` camera held elsewhere, `blocked` policy/privacy/permission,
`noMode` no compatible mode, `failed` other camera/encoder/service failure,
`hot` thermal stop, `sessionBusy` another host holds the session. `error` stays
free text for logs only; hosts must not parse it.

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
