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
`ConnectionType.USB` and authorized `ConnectionState.DEVICE` for the camera
list. This works when native Windows adb supplies no bus address. Connections
select that `host:transport-id:<id>` and then `localabstract:okuflow_camera`.
Older servers fall back to `host:devices-l`, accepting only entries with
explicit `usb:` evidence. Wi-Fi, emulators and unknown transports are excluded
from the camera list. The recovery logic additionally keeps every listed USB
transport with its raw state (connecting 1, authorizing 2, unauthorized 3, no
permission 4, offline 6, device 8 in AOSP `devices.proto` numbering) so it can
name and act on states the camera list hides. The Python probe never owns the
server lifecycle.

Windows discovers in the background. Its pace comes from the link doctor
(`phone_link_doctor`): at once after an event (application start, ADB interface
arrival or removal, resume from sleep or the display turning on again, a manual
pick, the window activated after two idle minutes), then 1 s for five failed
cycles, then 2, 3, 5 s and at most 10 s. States only the user can resolve
(unauthorized, locked phone after two launch attempts, paused, busy with
another host, hot, blocked, no ADB interface) are polled every 2 s without
back-off. Host budgets: 500 ms connect (cancellation checked every 20 ms),
1000 ms per host reply field, 1000 ms companion probe transport.

Without an existing listener or endpoint override, Windows may directly start a
hidden local `adb -L tcp:<port> server nodaemon`. It never runs client
start-server/kill-server commands and adds no adb forwards. Local TCP endpoint
overrides are honored; remote or unsupported endpoints are refused. Owned
bootstrap and crash relaunch wait 5 s, doubling to at most 30 s without a new
event; every event returns to 5 s. The exact owned child is assigned to a
non-inherited kill-on-close Windows job before it resumes; normal OkuFlow
shutdown and process crash both terminate it. Owned bootstrap requires
phone-use intent; an explicit webcam choice prevents it and relinquishes an
idle owned server. Emulator scanning is disabled with `ADB_EMU=0`; owned
support rejects external host-kill requests with `ADB_REJECT_KILL_SERVER=1`.
WinUSB ownership can hide these phones from a second server; cooperating tools
can use the logged private port or start SDK adb first. Readiness is polled for
five seconds in 50ms slices. Two consecutive unresponsive host-version probes
allow restarting only that owned child, with cancellation and live/starting
camera sessions taking precedence. The host-version read deadline is one second
per bounded field. Listener ownership is checked against the exact held child
process; a foreign listener on a raced port is not accepted as owned. Dead
children can yield to a newly responsive default server after its sessions
finish. Startup/recovery reports automatic retry without manual
process-management instructions. Shutdown latches out new owned launches.

### Recovery ladder (Windows)

Each discovery cycle with phone-use intent observes the whole chain (Windows
ADB interface through SetupAPI, server kind and health, the target phone's raw
state, the companion probe and, when adbd answers but the companion does not,
one read-only shell query: `dumpsys power` wakefulness, `dumpsys window`
keyguard lines, `pm path` and `pidof` for both packages, closed by the line
`OKUFLOW_FACTS_END`; Windows reads to that whole line, so output that contains
"END" elsewhere does not cut it short). The doctor names one
diagnosis and one next action, least disturbing first:

1. List again.
2. Targeted `host-transport-id:<id>:reconnect`, also for a transport listed
   `offline`, `connecting`, `no permissions` or in a state OkuFlow does not
   know (`host:reconnect-offline` when no id is known, and only on OkuFlow's
   own private server, since it reaches every offline device of that server):
   at once, 3 s later, then every 30 s until the next event. `authorizing` is
   the Allow handshake and is never reset; it reads "Restoring USB
   connection…" for 2 s and then asks the user to tap Allow. A phone listed
   `unauthorized` for 20 s without a change gets one targeted reconnect per
   event, which makes the phone ask the Allow question again. A
   transport in state DEVICE keeps the earlier proof steps before a reset
   (responsive version and listing, two consecutive 2-second full-token echo
   timeouts, a fresh non-claiming companion timeout, the same transport still
   listed, 2-second post-ack disappearance check); the per-phone/server cadence
   is at once, 3 s, then 30 s, renewed by events. Healthy or definitive answers
   suppress health probes for 10 s. On a server OkuFlow does not own (a
   borrowed default-port server or the user's own endpoint) a phone that stays
   `offline`, `connecting`, stalled or with `no permissions` gets two targeted
   reconnects (counted also when one cannot be sent, for example without a
   transport id), then an end state 6 s later: a default-port server that keeps
   it stuck is holding the USB interface without serving it and follows rung 4
   (unless it serves another device in state DEVICE: the other-program
   sentence); the user's endpoint is never taken over and Windows asks for a
   re-plug of the cable. The same sentence ends the user's endpoint's other
   states: responsive but not listing a phone Windows shows (after the 5 s
   blind grace) and unresponsive for 30 s.
3. Restart OkuFlow's own server when it answers `host:version` but lists no USB
   entry at all although Windows shows the ADB interface (another listed phone
   is not blindness), 5 s after the last event or first such observation, or
   when a transport stays offline or connecting 6 s after two reconnects; then
   15 s, then 60 s. At most three restarts per episode (until the next event);
   after that Windows asks the user to unplug the phone's cable and plug it
   back in, and keeps probing. Live sessions that deliver frames defer it.
   When Windows cannot say whether the ADB interface is there (only when the
   SetupAPI query itself fails; a phone that does not publish the standard ADB
   interface GUID reads as absent and gets the connect sentence), OkuFlow's
   own server is treated as if it were there after 30 s blind: the same
   restarts, cap and re-plug sentence, and a private server may be started
   beside a blind default. This is never a reason to ask a server OkuFlow did
   not start to exit or to stop it: such a takeover ends in the re-plug
   sentence after the takeover grace, and a hung default after 30 s
   unresponsive. A first automatic start with no saved phone wants the phone
   under "unknown" once the server lists a USB phone.
   While OkuFlow's own server is blind, Windows also looks at the default port:
   another adb there (Android Studio, scrcpy, a vendor suite, or one restarted
   after `host:kill`) is handed back to when it lists the phone ready,
   unauthorized or authorizing (offline, connecting or no permissions there is
   not serving it, and neither is "ready" for a phone whose adbd timed out on
   that same listener earlier in the episode), and otherwise follows rung 4.
4. A borrowed default-port server that is responsive but blind for 5 s, or
   that keeps the phone stuck after rung 2, gets a private owned server beside
   it; whichever lists the phone is used, and a default that lists the phone
   ready, unauthorized or authorizing is handed back to at once. A phone that
   default listed ready while its adbd did not answer (a stalled transport)
   stays "not served" there until an event, adbd answering there, or another
   listener: while its shell stays silent, that listing never stops `host:kill`
   or termination from going on. Right before either, Windows asks the phone's
   shell once more through that listener; if it answers, nothing is stopped and
   USB is handed back on the next cycle. Starts of the
   private server beside the default and hand-backs (also the hand-back of a
   private server started for a hung default, which needs the same usable
   listing, never just an answer) are counted together, also the private
   server started at once beside a confirmed hung default. At four per
   episode no more private servers are started beside the default, while a
   default that lists the phone is still handed back to (the count ends where
   the phone is listed); then Windows says another program is using the
   phone's USB connection.
   If neither lists it 6 s later and that default lists no other device in
   state DEVICE, Windows sends it `host:kill` (the ordinary request every adb
   client sends on a version mismatch). If its listener remains 2 s later, or
   if the default is confirmed hung (two failed version probes of the same
   listener spanning 5 s, also after a resume or start, confirmed by the
   doctor's own probe while OkuFlow's server runs beside it), Windows
   terminates exactly that process (see "Default-port server stop" below).
   Termination needs the Windows ADB interface to be present, checked again
   when the stop runs (also before `host:kill`). A hang confirmed only by
   version probes first gets one short listing attempt: if it is answered at
   all (another device, the phone, just an answer, or an adb FAIL reply: a
   slow but healthy server, or one that recovered) nothing is stopped and the
   hang must be confirmed again from scratch. When that listing showed
   another device in state DEVICE, the default serves someone: Windows says
   another program is using the phone's USB connection instead of trying
   again. A hung
   listener with the phone unplugged reads as "connect the phone", not as
   recovery. A stop outcome noted after a newer event belongs to the old
   episode and is dropped. A listener that
   does not accept a connection (a timed-out connect, or a refused one while
   the listener table still shows it, as for a frozen adb with a full accept
   queue) counts as hung, never as absent. Per episode at most two
   `host:kill` requests and three terminations (a termination not carried out
   because the default answered its listing does not count); a different
   process that appears on the port is a new takeover and is asked first.
   Starts beside the default and hand-backs count only once they happened.
   After the caps,
   or when a stop is refused, Windows says another program is using the
   phone's USB connection instead of fighting.
5. Phone side: when the companion socket is refused or times out while adbd
   answers, Windows sends `input keyevent KEYCODE_WAKEUP` and
   `am start -n com.okuflow/com.okuflow.camera.MainActivity --ez
   com.okuflow.camera.extra.HOST_LAUNCH true` (the legacy debug package only
   when the Play activity is absent): at once after an event, 3 s and 6 s later,
   then every 20 s while the user wants the phone. Camera starts take at most
   one wake + launch per start attempt from that same per-event budget (no
   timer of their own). The app is never force-stopped. Two exceptions end
   the loop: when the app process runs but its listener refuses (the companion
   installed before the paused-status answer, while paused), whatever the
   keyguard shows, discovery and camera starts together launch once per event
   and Windows asks the user to open OkuFlow Camera and, if it shows that
   streaming is paused, to tap Resume streaming (also right when its camera
   service did not start). Within 5 s after OkuFlow's own launch such a
   refusal is a launch still starting and holds nothing (the ordinary spacing
   applies), and a hold ends as soon as the app process is no longer running,
   which is relaunched at once the first time after an event and otherwise 20 s
   after the last launch (Pause survives the process, so an app that keeps
   being killed is not brought forward every time);
   when the listener accepts but never answers after two launches (or after
   that single launch), Windows asks the user to close OkuFlow Camera on the
   phone and open it again (relaunching would only bring the hanging activity
   forward). A companion problem other than paused, another host, blocked or
   hot (the camera busy in another phone app, failed, no mode) reads
   "Restoring" for 20 s, then Windows asks the user to close other camera apps
   on the phone and open OkuFlow Camera again.
6. When only the user can act, Windows shows one sentence (authorize, unlock,
   open the app, tap Resume streaming, re-plug the cable, close and reopen the
   app, close another program, reinstall when OkuFlow's own adb cannot be
   started twice in a row, which a missing `adb.exe` reaches at once and an
   adb that is killed before it answers reaches after the private server's
   launch back-off; connect the chosen phone or choose the connected one when
   another phone is listed) and keeps probing. The status line speaks the
   sentence itself, after every discovery result, also while a fallback webcam
   shows the picture, and a passing notification does not replace it.

`PhoneCameraTransport::Start` waits up to 4 s (inside its 12-second budget) for
a selected phone that is listed but not yet in state DEVICE. Every diagnosis
change and every ladder action is one line in the debug log and in the always-on
`phone-link.log` (see docs/hardcoded_paths.md); phone serials appear only as a
short hash.

### Default-port server stop (owner decision, 2026-11-17)

The owner decided on 2026-11-17 that a default-port adb server that hangs is to
be killed, not worked around. This replaces the earlier rule that an unknown
listener is never terminated. Windows stops a default-port server only with
phone-use intent, when no adb endpoint environment variable is set, no other
device is in state DEVICE on that server (when it can still answer), and no
live phone session needs it. It first sends `host:kill` where the server can
still answer. It terminates only the exact process behind the IPv4 loopback
listener, through a handle opened for that listener's owning process and
re-verified (same PID and creation time as the identity observed when the
decision was made, image file name `adb.exe`, not OkuFlow itself, still owning
the listener), never by name, never a process tree. An endpoint set through
`ADB_SERVER_SOCKET`, `ANDROID_ADB_SERVER_PORT` or `ANDROID_ADB_SERVER_ADDRESS`
stays the user's: it is never shadowed, asked to exit or stopped. A responsive
server serving other devices keeps running beside OkuFlow's private server, and
Windows then says another program is using the phone's USB connection. If
Windows denies termination (elevated or another user's server), the trace says
so and the same sentence is shown. So it is for every other stop that does not
end the server: a listener whose process cannot be opened (another user), an
image other than `adb.exe` (vendor-renamed servers are left alone and never
receive `host:kill`), and a changed identity or failure twice in a row. The
refusal ends when the listener is gone, another process listens, or the
phone is listed. `host:kill` itself goes out only after a fresh listing that
succeeded, serves no other device and still does not list the phone, and
after the same PID, creation time and `adb.exe` image checks (rechecked after
connecting); termination does not repeat a `host:kill` already sent to that
listener. The reviewed legacy OkuFlow bundle
reclamation in docs/adb_distribution.md still runs first for a hung default.

### Leftover OkuFlow instance (2026-11-17, iteration 2)

This replaces the earlier rule that a windowless sibling instance is only
reported. A real OkuFlow instance (a main window on the Windows platform;
offscreen and test instances never register) creates the named events
`Local\OkuFlow.Instance.<tag>.<pid>` and `Local\OkuFlow.Exiting.<tag>.<pid>`
(the tag is a hash of its image path) and sets the second once its critical
teardown is done (recording finalized, image writes, capture stopped, settings
saved, its own adb stopped), not when it begins to exit. Another instance of
the same image, user and session is a leftover when its exit signal has been
set for 5 s and it still lives, or when it is registered, shows no window on
two observations 20 s apart (above the longest bounded critical teardown,
about 17 s) and the windows of its UI thread (the thread that owns its main
window, recorded at registration; every top-level window when no thread is
recorded) do not answer a window message within 1 s (a sibling with a visible
window is not sent one). A hidden window of another thread decides nothing.
Windows terminates that exact process once, and only while it plausibly
holds the phone (the phone is listed by no server, the Windows ADB interface
is present, the blind grace has passed), through a held handle (same PID,
creation time and image path as this installation, no visible window at that
moment), which ends its owned adb child through the job object. If the
process cannot even be opened, nothing is stopped. If Windows refuses the
termination, only that instance's own adb child is stopped while the instance
is held open and still matches, when the child is proven (child of that PID,
created after it, image this installation's `platform-tools\adb.exe`);
otherwise the user is told another program is using the phone. Every wait
here observes application shutdown. A sibling with a visible window is never
terminated: after the 5 s blind grace the user is asked to close the other
OkuFlow window. An unregistered windowless sibling (an older version or a
test run) is named in the trace once, never terminated, and does not stop
the blind ladder.
Normal discovery and a fresh camera session follow every rung; recovered
transport is not successful video delivery.
Schema reference: [AOSP devices.proto](https://android.googlesource.com/platform/packages/modules/adb/+/7806e425e18a48a0f9e580e381cdc86ed5478a4c/proto/devices.proto).

One control connection and one video connection form a session. A second
control host receives ERROR `{"error":…,"code":"sessionBusy"}` and is closed.
The video connection must bind within five seconds of the control ACK, or the
phone closes the control connection and frees the slot. The service generates a random session token for
binding the video channel. The token is installation/session-local, never
included in probe logs. Closing either connection retires both and releases
the camera. An authorized computer has the normal privileges of USB debugging.

Service events retain the host token internally through command dispatch and
camera callbacks. The service serializes Stop, restart and retirement on its
main event owner, rechecking the exact connected host at execution. Socket
callbacks run outside the session lock. Retired-host commands/results cannot
be forwarded to a successor. The v1 wire layout and command names are unchanged.

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

While the user has saved Pause, the camera service and its listener are stopped.
Newer companions then bind a status-only listener on the same socket while the
app process lives: a probe receives `{"protocol":1,"probe":true,"busy":false,
"problem":"paused","cameras":[]}` and a control hello receives ERROR
`{"code":"paused"}`. It never opens the camera or clears Pause. The listener
belongs to the process, not to a screen: a supervisor started by the first
component of the process keeps it bound every 500 ms while Pause is saved
(retrying without a limit while the camera service still releases the socket)
and closes it otherwise. Resume closes it synchronously before the camera
service can start and bind the socket; if Resume cannot be saved, it is bound
again. Compatibility: older hosts treat the
unknown `paused` code as "not ready" and never start; newer hosts with an older
companion see an absent socket and use the read-only shell facts instead (a
running app process with a refused socket is launched once per event and
the user is asked to tap Resume streaming, never relaunched in a loop).

A launch carrying the boolean extra `com.okuflow.camera.extra.HOST_LAUNCH`
shows the activity over the lock screen and turns the screen on, so a locked
phone on a stand can start the camera service from a resumed activity. A saved
Pause disables this; leaving the activity clears both flags. A host launch also
clears an earlier foreground-service refusal (it may arrive through
`onNewIntent` while the activity is already resumed, which produces no
resume). A recreated activity (configuration change, or restore after the
process was killed) is not a host launch: the extra of the original intent is
dropped there. The read-only keyguard facts read `mShowingLockscreen` (Android
9 and older), the KeyguardController's `mKeyguardShowing`/`KeyguardShowing`
(Android 10 and later) and DeviceIdleController's `mScreenLocked` as a
fallback; any one reporting true counts as locked.
Capabilities are built once per listener (warmed in the background) and copied
per reply.

While the activity is resumed, it refreshes host/service state every three
seconds. A disconnected `busy` or `failed` camera problem remains visible for
at least 10 seconds (was 30), then clears to permit a fresh host attempt. A blocked
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
`hot` thermal stop, `sessionBusy` another host holds the session, `paused` the
user saved Pause (status-only listener). `error` stays
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
| `start` | Optional `startupBudgetMs` positive integer | Negotiate and start with saved phone settings within the host's remaining startup budget |
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
32 packets / 32 MiB; control holds 128 packets / 512 KiB. Byte limits include
the packet being written. Per-frame METADATA is advisory: once control holds
96 packets or 384 KiB, new METADATA is dropped and the session stays up. The
remaining 32 packets / 128 KiB are kept for ACK, STATE and ERROR, which keep
their order and are never dropped. Overflow of that full control bound still
disconnects the session, without a retry. Hosts must tolerate gaps in
METADATA `frameNumber`.

`configured` precedes video for a generation. `streaming` follows the first
non-configuration encoded frame. Both report `generation`, `camera`, `width`,
`height`, `fps` (negotiated upper bound), `bitrate`, `mime`, `orientation`,
`facing`. `streaming` also reports the output `timestampBase`: `sensor` on
API 33+, otherwise `device-default`. Startup tries up to six capability-probed
candidates from the local camera/encoder intersection, bounded by the saved
size/rate pair and host limits. It reduces size/FPS or falls back to AVC after rejection
or the candidate's first-video deadline. Thermal status SEVERE requests at most 30 FPS;
CRITICAL stops capture and requires a new start after cooling.

### Startup budget and candidate progress

Native Windows keeps one 12-second deadline from `PhoneCameraTransport::Start`,
including USB/listener negotiation and any initial HEVC-to-AVC reconnect. Just
before `start`, it sends the remaining milliseconds minus a one-second decoder
reserve as `startupBudgetMs`; an exhausted budget fails without starting camera
work. This field is optional for compatibility: an older host gets a 12-second
phone cap, and an older companion may ignore the field while the host retains
its own deadline.

The phone clamps this field to 1–12000 ms. Time starts when the command arrives,
so the main-owner queue, camera-handler queue, catalog discovery, synchronous
encoder setup, teardown and 200 ms retry gaps consume the same absolute budget.
It retains at most six candidates, reducing that count if necessary to reserve
at least 500 ms per candidate. Each attempt receives an equal share of the
remaining time after future retry gaps, capped at four seconds. Fast rejection
returns unused time to later candidates. Fewer candidates keep up to four seconds
each; a single candidate still fails after four seconds without encoded video.
Settings/thermal restarts before the first video retain the original budget.
Stop, disconnect, replacement start and close retire the start token, preventing
queued retries and old camera callbacks from reopening or publishing a stream.
Android platform calls cannot be preempted inside a stalled camera/codec driver;
late results are rejected and resources released when the handler resumes.

`configured` adds `attempt` (one-based), `candidates`, `startupBudgetMs`,
`remainingMs` and `attemptTimeoutMs`. On a failed candidate with another attempt
available, `retrying` carries the same fields plus `reason`, `retryInMs` and
`failedGeneration`; it omits `generation` because encoder creation can fail
before that generation was configured. These fields are diagnostic data, not
localized UI copy. Terminal ERROR includes the failed candidate's mode and
reason, or states that insufficient startup time remains. Hosts must not parse
free-text reasons for behavior.

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
If HEVC decoder creation/processing fails or produces no initial decoded frame
for four seconds after its first non-configuration encoded frame, Windows closes
both channels and reconnects once offering AVC only. Codec configuration alone
does not start this four-second clock while the phone is still trying modes;
the overall startup and 12-second decoded-progress deadlines remain bounded.
This changes negotiation for that session, not the saved phone preference.
The phone status includes the actual codec, so AVC fallback is visible.
Samples pin decoder textures until consumers release them; CPU NV12 is the
fallback. Native capture timestamps are continuous per-session presentation
times, rebased across encoder generations; `captureClock100ns` is host arrival
time. Neither path claims measured sensor-to-display latency.

The desktop source exposes queued zoom/camera APIs, and discovered camera IDs
appear as camera-list entries. New companions add `automaticCameraId: "auto"`
and `automaticCamera` (the currently resolved ID) to the capability response.
The `camera` command accepts that automatic selector and persists empty camera
intent; explicit IDs remain explicit. Each camera may add `role` (`auto`,
`ultrawide`, `main`, `telephoto`, `front`, `back`), `logical` and
`equivalentFocalMm`. Unknown/missing roles retain generic labels. Physical
selection IDs are opaque and can contain `/`; hosts must not parse them.
Their fixed outputs expose zoom range 1–1; logical outputs advertise their
actual ratio range. Windows uses a separate camera-zoom control and saves intent
per phone/lens. Automatic handoff of OkuFlow's viewport zoom,
per-frame metadata joins, clock synchronization and high-speed capture above
60 FPS remain separate plan-51 work.

Zoom accepted during Camera2 startup is retained for that host/start generation
and applied when the capture session is configured. It does not consume a mode
fallback. A new start resets zoom; retired host/start commands cannot affect it.
