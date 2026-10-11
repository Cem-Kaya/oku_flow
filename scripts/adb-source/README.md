# OkuFlow adb rebuild materials

The Windows release includes `sources/adb-37.0.0-okuflow-source.zip`. Extract
it and run, from PowerShell 7:

```
pwsh -NoProfile -File scripts/build_adb_from_source.ps1 -InputDirectory ./inputs
```

Install Visual Studio 2022 with C++/Windows SDK support first. The script creates
an isolated MSYS2 UCRT64 installation under `%LOCALAPPDATA%/OkuFlow/adb-source-build`.
Initial toolchain setup needs internet. Every source/patch/library input is
included in `inputs/` and checked against `scripts/adb-source/pins.json`.
GCC 16.2.0 is the validated compiler. A later compiler is rejected until its
build is reviewed. The exact dependency package versions are included locally.

`build-output/` contains adb.exe, the two source-built USB DLLs, their dependency
DLLs, notices, source archive and build provenance. It does not start an adb
server. Timestamps/toolchain packaging can change executable hashes; releasing
a rebuild requires reviewing and updating the distribution pin.

To modify libusb and relink adb, build once, edit
`prepared/android-tools-37.0.0/vendor/libusb/`, then rerun with
`-SkipToolchainSetup`. The script retains prepared source and rebuilds the
executable. The complete libusb and adb source is in the included original
source archive, with all applied patches supplied separately. Headers/import
libraries and runtime DLLs for the exact dependency versions are included as
MSYS2 package archives; the complete adb, libusb and BoringSSL sources are
included. GCC runtime components retain their Runtime Library Exception notices.

Run `scripts/test_adb_source_build.ps1 -RuntimePath <build-output/platform-tools>`
to test a hidden server on a private loopback port. This never stops or replaces
the existing shared adb server. The script cleans up only its own private child.

adb is a separate program. Its libusb component is LGPL-2.1-or-later. You may
modify this library and reverse engineer adb to debug your modifications under
that license; OkuFlow's commercial terms do not restrict those rights. Retain
the corresponding source/relinking materials and all notices when redistributing.

The source archives contain additional Android tools and their notices. Only
the `adb` build target and the two Windows USB DLL targets are redistributed
as executables. Wireless mDNS discovery is disabled; USB transport is enabled.
