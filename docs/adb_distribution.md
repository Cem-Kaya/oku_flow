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
throttled to 30 seconds. The shared server stays alive after OkuFlow exits;
owner-managed endpoint overrides disable bootstrap. The Python probe does not
own server startup or shutdown.

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
