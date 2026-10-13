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
If no listener or endpoint override exists, it starts adb hidden with
`-L tcp:<port> server nodaemon`, which binds loopback without `-a`. Bootstrap is
throttled to 30 seconds. New owned servers use a private ephemeral port and a
non-inherited Windows kill-on-close job; normal app exit or crash stops only
that exact child. Existing/default and overridden servers remain borrowed;
only the reviewed legacy OkuFlow bundle qualifies for the separate reclamation
rule below. A dead/failed owned child can yield to a newly available
responsive default server after its sessions finish. An explicit webcam choice prevents owned bootstrap and
relinquishes an idle owned server. The Python probe does not own server startup
or shutdown.

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
idle restart, with a fresh port and a 30-second retry budget. The IPv4 loopback
listener's PID must match the exact held process handle before readiness is
accepted; PID lookup is observation only, never termination authority. Shutdown
latches out future bootstrap, and stopping a manager interrupts its next probe
or polling step. Owned startup/recovery uses localized automatic-retry guidance;
The product never sends users to Task Manager; it keeps checking and retrying
the connection automatically. Recovery of a genuinely foreign server that
retains the exclusive USB handle is an external ownership limitation, not
permission to terminate that foreign process. Lifecycle and manager lock waits
check cancellation every 20 ms; live-port observation does not wait for a host
probe. Camera startup accepts only confirmed-ready results. Server identity
includes port, PID and process creation time, so a restarted server reusing
transport ID 1 still renews launch/recovery budgets and camera retry state.

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

If a hung default listener cannot be reclaimed, it remains untouched and
OkuFlow tries a private owned server. WinUSB may remain unavailable while that
other server retains the phone. An unused shadow server hands back only after
its raw transport list is empty and the default answers two consecutive version
checks. Unauthorized transports also prevent handback. Ordinary enumeration
(`BorrowOnly`) performs no lifecycle effects; `ReleaseForWebcam` only releases
idle owned support, while `PhoneUse` enables guarded bootstrap/recovery.

After a camera failure, Windows can request a targeted reset of the selected
USB transport without stopping adb. A responsive `host:version`, current
authorized USB listing and a separate timed-out shell echo are required;
the serial/transport is rechecked after the timeout. It sends only
`host-transport-id:ID:reconnect`, at most once per phone/server per minute,
including across new transport IDs or unsupported replies. An idle preferred
phone can receive the same recovery during background discovery while a
fallback webcam is active. Ordinary browsing, explicit webcam intent, coded
phone problems, busy sessions and live/starting phone ownership do not trigger
it. A successful request still needs subsequent discovery and decoded frames.
Other adb clients using that same phone lose their transport during this reset;
other phones and the server process are retained.

A globally hung server cannot acknowledge this transport request. The automatic
server recovery above handles OkuFlow-owned or proven legacy support; unknown
shared servers remain untouched. OkuFlow does not reset every offline device,
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
