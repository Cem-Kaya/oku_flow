# Source-built Android Debug Bridge distribution

OkuFlow's Windows ZIP includes an independent adb executable, both Windows USB
support DLLs, their transitive dependency DLLs, exact notices and corresponding
source/relinking materials. No Android Studio or SDK installation is required
for users. The phone still needs OkuFlow Camera, USB debugging and authorization
for the computer; some vendors also need their Windows USB driver installed.
The Android APK is built and installed separately.

## Build and stage

Install Visual Studio 2022's C++ workload and Windows SDK, then use PowerShell 7:

```powershell
pwsh -NoProfile -File scripts/build_adb_from_source.ps1
pwsh -NoProfile -File scripts/test_adb_source_build.ps1
scripts/build_release_bundle.bat
```

The build creates an isolated MSYS2 UCRT64 installation under
`%LOCALAPPDATA%/OkuFlow/adb-source-build`; it does not modify a user's MSYS2.
Initial toolchain installation needs internet. `-BuildRoot` selects another
local directory and `-MsysRoot` accepts an existing compatible toolchain.
Validated GCC is 16.2.0; the script rejects a different compiler for review.
Prepared source is retained, never silently discarded.

`scripts/adb-source/pins.json` records SHA-256 values and immutable source/patch
URLs: android-tools 37.0.0, AOSP Windows USB API sources, five MSYS2 Windows
patches and the protobuf 36 compatibility patch, plus exact UCRT64 dependency
package archives. The script builds only the adb target, with bundled libusb,
external dependency libraries and wireless mDNS disabled. MSVC 2022 builds
AdbWinApi.dll and AdbWinUsbApi.dll from source. PE import traversal copies the
complete runtime closure; this source build needs more than three binaries.

The output root contains:

- `platform-tools/`: adb, USB/dependency DLLs, notices, revision and build manifest.
- `licenses/adb/`: original component notices, LGPL text and GCC exception terms.
- `sources/adb-37.0.0-okuflow-source.zip`: complete adb/libusb/BoringSSL and USB
  source archives, applied patches, exact headers/import-library/runtime packages
  and rebuild instructions/scripts. Toolchain binaries are separately installed.

`adb-build.json` records compiler/version, source hashes, component linkage and
every output checksum. Review a newly built output and update the manifest pin
in `scripts/adb-source/distribution.json` before releasing it. Timestamps or
toolchain changes can alter hashes; this is a verified-input build recipe, not
a claim of byte-identical reproducible executables.

`scripts/stage_platform_tools.ps1 -BundlePath <fresh-stage>` accepts `-SourcePath`,
then `OKUFLOW_PLATFORM_TOOLS_DIR`, then the default `build-output` root. That
root must contain all three directories above; an SDK `platform-tools` directory
is not accepted. Staging checks the reviewed manifest and every file, rejects
unexpected files and checks the copied hashes again. Release metadata lists the
component licenses separately and includes the source ZIP checksum.

## License terms

This is an unsigned OkuFlow build from open-source inputs, not Google's SDK
binary. The AOSP adb and USB API code is Apache-2.0. adb also statically links
libusb (LGPL-2.1-or-later) and BoringSSL (its full composite upstream license).
Dependency libraries retain their individual Apache/MIT/BSD/Zlib terms; GCC
runtimes retain GPL terms with the GCC Runtime Library Exception. The exact
inventory is in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) and the SBOM.

adb runs separately from OkuFlow. Bundling it does not replace its own licenses
with OkuFlow's GPL/commercial terms. For the static libusb component, the release
includes the corresponding library source and the source/relinking materials
for the work that uses it, following LGPL 2.1 section 6(a). Users may modify
libusb and reverse engineer adb to debug those modifications. OkuFlow's
commercial terms do not restrict those rights. Retain the notices and complete
source/relinking ZIP when redistributing; do not publish just the three main
adb binaries and omit their dependencies or source.

To rebuild or modify libusb, extract the source ZIP and follow its README:
build once with `-InputDirectory ./inputs`, edit the retained libusb source and
rerun with `-SkipToolchainSetup`. Exact dependency packages permit relinking
without relying on a later library ABI. Releasing that modified build requires
reviewing its provenance and updating the distribution pin.

## Server ownership and validation

Windows uses raw host-protocol requests against an existing local server. It
never invokes a version-changing client command to stop/replace that server.
An explicit `OKUFLOW_ADB_PATH` and the documented SDK/PATH development
fallbacks are trusted local executable inputs, not remote inputs. Normal
bundles select their adjacent runtime ahead of implicit SDK/PATH candidates;
the explicit override has priority. Every owned launch logs its resolved
absolute path. Runtime lookup does not establish manifest integrity: staged
bundle validation and legacy-process reclamation have separate pinning gates.
Do not infer permission to terminate an unknown process from a path override.

If no listener or endpoint override exists, it starts adb hidden with
`-L tcp:<port> server nodaemon`, which binds loopback without `-a`. Bootstrap
waits 5 s, doubling to 30 s without a new event (arrival, removal, resume,
start, manual pick); every event returns to 5 s. New owned servers use a
private ephemeral port and a non-inherited Windows kill-on-close job; normal app
exit or crash stops only that exact child. Existing/default servers are
borrowed while they serve the phone; a default-port server that hangs or is
blind to a phone Windows shows is stopped under the owner's 2026-11-17 rule
below. Overridden servers always remain the user's. A dead/failed owned child can yield to a newly available
responsive default server after its sessions finish. An explicit webcam choice prevents owned bootstrap and
relinquishes an idle owned server. The Python probe does not own server startup
or shutdown.

The always-available automatic phone picker entry supplies explicit phone-use
intent even when no server or discovered phone entry exists. A new native USB/ADB
interface arrival supplies that intent too, following the owner's reconnect
policy. Repeated discovery on an unchanged connection preserves a manual webcam
choice. Neither event authorizes stopping a borrowed server or bypassing USB
authorization and companion readiness.

An owned server's WinUSB connection is exclusive: a second adb server may not
see its phones until that owned server releases them. Start SDK adb first to
have OkuFlow borrow it, or point cooperating tools at the private port printed
in the app log (`ANDROID_ADB_SERVER_PORT`). Those tools then share OkuFlow's
server lifetime. The child disables emulator scanning with `ADB_EMU=0` and
rejects unsolicited `host:kill` with `ADB_REJECT_KILL_SERVER=1` (checked
against the pinned 37.0.0 source). An automatically selected phone is phone-use
intent; an explicit webcam selection is not. A server is never stopped while
another live/starting phone session holds a counted occupancy reservation.

Owned readiness polling permits five seconds plus the current bounded probe.
Two failed host-version probes use one-second per-read deadlines before an
idle restart, with a fresh port and the 5 s (doubling to 30 s) retry budget.
The two probes are 1 s apart, and a child that is alive but has never answered
yet (antivirus scanning the executable, a loaded laptop) gets a 30 s start
grace before it can be judged hung. A
responsive owned server that lists no USB entry while Windows shows the ADB
interface is restarted by the link doctor 5 s after the last event, then after
15 s, then 60 s, at most three times per episode; the user is then asked to
re-plug the cable. Two consecutive launch failures of the bundled adb (a
missing or quarantined file) are named in one sentence. So are starts that
never answer: a child that stays alive without ever answering counts once per
30 s start grace (also one that is replaced, and one whose listener ownership
cannot be verified), and one that dies before it ever answered counts once.
After readiness, a responsive child whose listener ownership remains
unverifiable also counts once per start grace. Discovery waits for verified
readiness before listing the private server. The idle shadow's earlier
handback check also requires the exact private listener before listing and
rechecks that identity before accepting the empty listing as evidence.
A private server that cannot start beside another program's default counts
the same way. A verified response from OkuFlow's own server resets the count.
An owned server that answers its version probes while its device listing fails (time-out, closed socket, invalid reply, `FAIL` to
both listing forms) gets the same restart rung and cap after the 5 s grace and
ends in the re-plug sentence: upstream adb answers `host:version` without the
transport lock its listings take, so an adb stuck in a USB transport after
sleep or a cable pull answers the one and not the other. The IPv4 loopback
listener's PID must match the exact held process handle before readiness is
accepted; PID lookup is observation only, never termination authority. Shutdown
latches out future bootstrap, and stopping a manager interrupts its next probe
or polling step. Owned startup/recovery uses localized automatic-retry guidance;
The product never sends users to Task Manager; it keeps checking and retrying
the connection automatically.

**Default-port server stop (owner decision, 2026-11-17).** The owner decided
that a default-port adb server that hangs is to be killed, not worked around.
This replaces the earlier rule that an unknown listener is never terminated.
With phone-use intent and no adb endpoint environment variable, a default-port
server that is confirmed hung (two failed version probes of the same listener
spanning 5 s; events restart the confirmation, they never shorten it; a
listener that does not accept counts as hung; so does a server whose version
answers while its device listing fails on two listings of the same listener
spanning 5 s, where a version answer never clears that confirmation and a
successful listing does), or that is responsive but blind
to a phone Windows shows while OkuFlow's private server beside it is blind too,
first receives one raw `host:kill`, only after a fresh listing that succeeded,
serves no other device and does not list the phone ready, unauthorized or
authorizing, and after the PID, creation-time and `adb.exe` image checks. A
borrowed default that keeps the phone offline, connecting, stalled or without
permissions after two targeted reconnects is treated as blind (it holds the
interface without serving it). A hang confirmed only by version probes gets
one short listing attempt before anything else: any answer keeps the server
and restarts the confirmation. If its listener remains after 2 s, or it
cannot answer at all, `TryStopDefaultAdbServer` terminates exactly the process
behind that listener (the IPv4 `127.0.0.1` row of the port; when there is none,
its any-address row, `0.0.0.0` or a dual-stack `::`, as an adb started with
`-a` binds: it answers OkuFlow's loopback probes too; `::1` alone is never
reached and is ignored; every lookup and the legacy reclamation share this
rule and the identity proof below is unchanged). The ask and both termination
paths require one unique owner before their pre-stop listing and capture its
PID and creation time. They require the same listener after the listing, after
any stall re-check and at the stop helper call. The helper opens the process and requires
that original PID and creation time, the
image file name `adb.exe`, a process other than OkuFlow, and the same listener
again immediately before `TerminateProcess` on the held handle. Never by name,
never a process tree, never a stored PID. A responsive server that lists
another device in state DEVICE is serving someone and keeps running; OkuFlow's
private server runs beside it and the app says another program is using the
phone's USB connection. An access-denied termination (elevated server or
another user's), a listener process that cannot be opened, an image other than
`adb.exe`, and a changed identity or failure twice in a row are traced and
shown with the same sentence, until the listener is gone or changes or the
phone is listed. Termination requires the Windows ADB interface to be present,
checked again when the stop runs; "unknown" never counts for a stop.
Per episode at most two `host:kill` requests, three terminations and four
private-server starts beside the default plus hand-backs (the start beside a
hung default counts once it actually starts, a hand-back once the private server
is retired, and a termination aborted because the listing answered does not
count toward the three terminations, nor does a `host:kill` request that was
never sent count toward the two (no termination follows such a request); three
such aborted stops (the first ask of an episode that finds the server already gone is not one; further ones are) in an episode, counted apart for a hung default and for a
takeover beside a blind one, end in the same other-program sentence, which
a server that recovers or goes away ends; at the cap no more starts, but a hand-back to a default
that lists the phone is still made); another adb that
appears beside OkuFlow's blind server is handed back to when it lists the phone
in a usable state and otherwise taken over under the same caps. A phone that
default listed ready while its adbd timed out there is not served by it, unless one
more shell request through that listener, made right before `host:kill` or a
termination, is answered: then nothing is stopped and USB is handed back. Overridden
endpoints are never asked to exit or stopped. Lifecycle and manager lock waits
check cancellation every 20 ms; live-port observation does not wait for a host
probe. Camera startup accepts only confirmed-ready results. Server identity
includes port, PID and process creation time, so a restarted server reusing
transport ID 1 still renews launch/recovery budgets and camera retry state.

**Iteration 8 additions.** Before every side effect of the ladder (ask,
termination, start beside the default, restart, reconnect, hand-back, wake and
launch, leftover-instance cleanup), right after each blocking pre-check, and
inside the stop (before `host:kill` and before `TerminateProcess`), the
companion launch (before each shell command) and both transport reconnect
requests (after the connect, right before the request is written), the
discovery cycle's
phone-intent generation is compared with the application's live one, which
advances on every source or target change (a webcam, a file, another phone,
no phone, an agent's temporary source): a stale cycle does nothing and traces
"dropped: the source changed"; an ask or termination dropped this way is
given back uncounted. A live-session veto inside the stop counts as an
aborted termination even if that session ends before the helper returns. A termination beside
OkuFlow's server whose pre-stop
listing is silent is an aborted stop until the listing hang is confirmed, then
the stop goes on; a `FAIL` there is an answer and keeps the server. The
ladder's hand-back holds the lifecycle lock around its live-session check and
the stop, as the transport's own hand-back does. Every listing (also of the
default beside OkuFlow's server and in both hand-back checks) tries the
protobuf form and then the text form, so an older adb that answers only
`host:devices-l` is asked, stopped and handed back to like any other. The
spacing of a rung grows with its reservations, not with the attempts a
give-back lowered, so given-back attempts still reach the ceiling. Resume
signals within 5 s of the last applied resume are one event; four device
removals within 60 s name the cable (re-plug sentence) until the link holds,
30 s pass without a removal, or a manual pick or resume starts over; a phone
that is listed ready or unauthorized at that moment keeps its own ladder and
sentence (tap Allow, resume streaming), so repeated re-plugs by the user never
hide what the phone waits for. While the sentence for OkuFlow's own adb that
cannot start stands beside an answering default, the start beside it is tried
again at the restart ceiling, uncharged: nothing else starts a private server
there, and the sentence ends once adb.exe can start. Once the
terminations of a hung default are spent, the other-program sentence stands
also while a respawned default is not confirmed yet. A configured loopback
endpoint that is absent or unresponsive reaches the existing re-plug sentence
after the same grace. OkuFlow keeps probing it so a responding server clears
the sentence.

**Iteration 9 additions.** The discovery cycle's and the ladder's authority
also reaches the server request itself (`EnsureAdbServer`): it is asked once
the lifecycle lock is taken, after each blocking probe or listing, and right
before every lifecycle effect (the legacy reclamation, a start beside the
default, a launch, restart, replacement or retirement of OkuFlow's own child),
inside the owned manager with every cancellation check, inside the legacy
reclamation with every cancellation check, and in the raw `host:kill` once
more right before its first byte. A request that lost its authority does
nothing further and lists nothing in that cycle. A cycle whose source changed
does not reach the doctor at all, so it reserves nothing of the episode; the
application advances the intent generation only when the intent changed
(computed after the change, before a pick's event), so a pick of the phone
already chosen revokes nothing. A private child that never answered (one that
died before answering, or that is retired alive after its readiness wait ran
to its deadline or a probe found it silent past it; a start cut short by a
source change counts nothing, whatever its age) is counted with the launch
failures when its manager is dropped (the yield to an answering default, both
hand-backs): starts beside an answering default end in the "cannot start"
sentence, which ends once OkuFlow's own server answers. The release for a
webcam adds nothing to them (the user chose another source) and clears the
private-server flags as the other retirements do. A legacy reclamation
entered on a listing hang lists the default once more first: an answer ends
the hang and nothing is reclaimed, an adb `FAIL` keeps the hang and forbids
the reclamation in that call. At the hand-over cap a listing-hung default is
borrowed and listed again, so a listing that succeeds ends the hang. Listing
failures belong to the listener they were seen on: a different listener
clears them, and both hang confirmations are read for the listener that is
there now. OkuFlow's private server beside a default that lists the phone is
handed back also when it does not answer or its listing fails. A session's
own server request uses a responsive default whatever start beside it is
requested. The ask and the termination refuse, as aborted stops, while a
phone session is live, that is registered and a frame was delivered within
2 s (which listener a session's sockets reach is not recorded, so any live
session counts; a session that is only starting or reconnecting does not, or a
manual pick retrying against a hung default would keep that default from being
stopped; the frame time is forgotten when the last session ends), and a live
session counts as
streaming by the frames the capture delivered. A restart deferred for a live
session is given back. A refused IPv4 connect with only an IPv6 (`::`) row
for the port is no listener.

Documented, not changed. A default whose listing hang is confirmed by
discovery's own listing (after `EnsureAdbServer`) is terminated in that same
cycle, before a private server is tried beside it; the next cycle's server
request starts one beside it if it is still there. Aborted stops keep the
spacing of their reservations: terminations at 0, +15 s and +75 s, asks at 0,
+6 s and +66 s, so the other-program sentence that three aborted stops lead to
comes after about 75 s of "Restoring" (66 s for unsent asks).

After at least two failed default host-version probes spanning five seconds,
phone-use intent may reclaim a detached adb from the reviewed legacy OkuFlow
release. This is deliberate adoption based on current provenance, not a claim
about which process originally spawned it. The canonical running image must
be `platform-tools/adb.exe` inside a recognizable complete flat/nested bundle,
with the pinned source-build manifest and matching executable hash. The launcher
must have the expected product metadata or the separately pinned public v0.2.3
launcher hash; that release lacks product metadata. The v0.2.3 ZIP was downloaded
and matched GitHub's digest before extracting the launcher for that pin.
Unknown files, SDK layouts, overrides, another same-user/session OkuFlow instance,
active/starting sessions and cancellation refuse reclamation. A named mutex
serializes attempts. The exact held process/listener and file provenance are
rechecked before sending one bounded raw `host:kill`, including after TCP
connection, and before any exact-handle forced termination. No process-name
kill or saved PID grants authority. Recovery still requires new decoded frames.

If a hung default listener is not a reviewed legacy bundle, OkuFlow starts a
private owned server at once and the link doctor applies the 2026-11-17 stop
rule above. WinUSB may remain unavailable while that
other server retains the phone. The idle shadow handback in the server
manager's caller requires two consecutive checks in which the verified
private listener lists no raw transports and the default lists the wanted
phone in a usable state. Unauthorized transports on the private server also
prevent this handback. Ordinary enumeration
(`BorrowOnly`) performs no lifecycle effects; `ReleaseForWebcam` only releases
idle owned support, while `PhoneUse` enables guarded bootstrap/recovery.

After a camera failure, Windows can request a targeted reset of the selected
USB transport without stopping adb. A responsive `host:version`, current
authorized USB listing and a separate timed-out shell echo are required;
the serial/transport is rechecked after the timeout. It sends only
`host-transport-id:ID:reconnect`: at once, 3 s later, then at most every 30 s
per phone/server until the next event, including across new transport IDs or
unsupported replies. Both recovery paths open the reconnect socket on the
observed port, verify its listener, then re-list on that port before the write.
The fresh listing must match the USB serial, transport ID and state that
authorized the request. A private listener must belong unambiguously to the
same live child before the listing and at the write. A transport-ID reconnect
can use a unique default listener's PID when Windows cannot open that process.
The ID-less `host:reconnect-offline` fallback requires a verified private server. An idle
preferred phone can receive the same recovery during background discovery while a
fallback webcam is active. Ordinary browsing, explicit webcam intent, coded
phone problems, busy sessions and live/starting phone ownership do not trigger
it. A successful request still needs subsequent discovery and decoded frames.
Other adb clients using that same phone lose their transport during a targeted
reset. The phone-link trace uses fixed tokens for known companion problem codes
and records unfamiliar codes as `unknown`. Discovery and session startup bind
wake and activity launch to the server that listed the selected phone. One
fresh listing verifies the USB serial and current transport ID for the whole
launch, including the legacy activity fallback. Both listing forms share a
deadline, limited further by the time remaining for session negotiation.
Each shell command selects by serial and rechecks listener and source
authority before writing. Launch can use a unique borrowed default's PID
when Windows cannot open the process. Stop authority still requires the
existing process and creation-time evidence. A reservation with no shell
bytes queued is returned to its launch budget with the retry spacing intact.
Session startup reserves time in its overall deadline for a control connection
after an activity write, even if the short connection window expired.

A globally hung server cannot acknowledge this transport request. The automatic
server recovery above handles OkuFlow-owned or proven legacy support and, under
the 2026-11-17 rule, a hung default-port `adb.exe`; overridden endpoints remain
the user's. OkuFlow does not reset every offline device,
revoke authorization or restart the phone daemon. Private fake-server fixtures verify
the guarded recovery; a naturally occurring live-server hang needs separate
device evidence. The upstream host service is described by the
[AOSP implementation](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/adb.cpp).

The tracked private-port test starts only its own hidden child, verifies
protocol 41, device listing, loopback addresses and preservation of the default
server's owner, then cleans up that exact child. It does not stop a shared
server or require the owner to change their tools.

On 2026-10-12 the source build, USB DLL build, dependency closure verification,
staging and private-port smoke test passed. Updated Android assembly, 54 JVM
tests and lint passed. The native Windows automatic phone connection was tested
with the authorized Pixel using the already running shared server: HEVC
1920×1080/60, 59.51 arrival FPS and 59.06 viewport FPS over a 20-second profile.
This is separate evidence from the private source-built-server startup test;
it does not claim a cold USB-driver installation test on a clean PC. The full
tracked Windows CPU/CUDA matrix also passed unattended, including GUI tests.
