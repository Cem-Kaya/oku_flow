# Windows runtime setup

The release ZIP uses `oku_flow.exe` as a small native launcher. It is built
with MSVC's static CRT (`/MT`) and has no Qt or Visual C++ runtime DLL imports,
so it can detect missing dependencies before the main application starts.
New bundles deploy the main Qt application as `app/OkuFlowApp.exe`, with Qt,
plugins, the runtime installer and USB tools beside it inside `app/`. The ZIP
root has the public `oku_flow.exe` launcher and documentation. Keep the complete
folder tree together. Legacy flat bundles remain supported. Development builds retain the existing
Qt `oku_flow.exe`; the separate build target is `okuflow_launcher`.

Users start the portable app with **oku_flow.exe**. `OkuFlowApp.exe` is started
automatically and remains a required supporting component. The next bundle
includes `START_HERE.txt`, copied byte-for-byte from `portable_start_here.txt`,
with English/Turkish/German instructions. Staging requires that guide and the
release inventory hashes it like the other bundled files. MSI desktop/Start
menu shortcuts already target `oku_flow.exe`, so users do not choose a binary.
The nested payload is authoritative when `app/` exists; an incomplete nested
update does not fall back to a stale root-level application. Arguments, working
directory and stdout/stderr forwarding keep their existing contract. The
application's data-folder validation protects the whole install tree, including
the parent of the nested runtime directory.

The launcher checks the x64 system runtime version and required system DLLs.
If the installed version is supported, it opens OkuFlow without installation
or administrator approval. Otherwise, it offers the included Microsoft
`vc_redist.x64.exe`, verifies a private temporary copy against the reviewed
SHA-256 pin, and invokes the vendor installer with `/install /norestart`.
The user accepts Microsoft's license and any Windows UAC prompt. Declining
stops setup; no forced reboot or silent system change occurs. A required
restart is reported. Starting OkuFlow later retries detection.

Advanced → AI and downloads → Setup Assistant includes the same runtime
status and installation path. Supported newer versions are retained.
Incomplete ZIPs and unreviewed installers fail verification. Installers are
copied to owned temporary directories; the bundled original remains intact.
The existing Codex and NVIDIA installers keep their separate behavior.

`scripts/vc-runtime/pins.json` drives the minimum version, installer hash,
required DLL names and license hash. `scripts/vc-runtime/LICENSE.rtf` is the
exact English license from that installer. Packaging validates Microsoft's
Authenticode signature, retains the unmodified installer and license, and
records both executables and runtime provenance in the manifest/SPDX document.
No app-local Visual C++ runtime DLLs are copied. A pin change requires review
of the new installer, license, system versions and these tests.

The launcher inherits the working directory and environment, preserves all
arguments and redirected output, waits for its application child and returns
the child's exit code. Normal launching creates no terminal. `--console`
attaches an existing parent terminal. Profiling and lecture helpers recognize
both process names and never replace an owner's running instance. Release
signing covers both executables; private unsigned bundles remain explicitly
unsigned.

## Validation

Run the tracked `scripts/agent_build.bat` matrix and the release bundle script.
`windows_runtime_contract` tests version admission, installer exit/cancel
handling, tampered installer rejection and the actual native launcher with an
owned fixture child (arguments, Unicode, current directory, environment,
stdout/stderr and exit code). That child test skips on machines without the
runtime to avoid initiating installation. `setup_assistant_interactions`
checks installed/missing dependency UI without running installers or changing
the machine. GUI tests run serially and need no owner interaction.

A clean Windows 10/11 VM acceptance run must still verify missing-runtime
installation, UAC cancellation, vendor cancellation, restart behavior and a
successful subsequent launch. The development PC already has the runtime;
unit tests and import inspection do not substitute for that VM evidence.

`scripts/test_bundle_layout.ps1` uses private synthetic files to check relocation,
one root EXE, unchanged bytes, legacy/nested snapshot integrity, tamper rejection
and refusals for captures, metadata or an existing app directory. It does not
install software, execute fixture binaries or validate a real release's provenance.

### Nested layout validation, 2026-10-13

The tracked release build and background CPU (25) / CUDA (30) CTest entries
passed with GUI excluded. The runtime contract passed both legacy-flat and
nested-runtime child forwarding, and the capture-folder fixture rejected the
parent install directory. Synthetic packaging fixtures passed.

A private review ZIP was assembled from the verified v0.2.3 runtime/notices
inventory and the newly compiled launcher/application, without modifying the
published bundle or starting a camera. Metadata validated the original runtime
installer pin, adb source/relinking records and Qt SPDX documents at their nested
paths. Both binaries retained all seven exact branded icon frames. All 152
archived payload/metadata files matched the staged hashes; only `oku_flow.exe`
is a top-level EXE. Its ZIP SHA-256 is
`A7E141BA205F263406435A7C0736A71B3C252763DDD4F89BA4E7A335D79011A5`.

Review output stays under `%LOCALAPPDATA%\OkuFlow\bundle-layout-preview\`,
marked `sourceTreeDirty=true`, `nativeGuiVerified=false`, `published=false`.
Fresh native GUI and clean-VM MSI install/upgrade acceptance remain pending;
the preview is not a replacement for those publication gates.

Microsoft recommends central installation for servicing. Future OkuFlow
updates can refresh the pinned installer, while the system installation can
receive Microsoft servicing independently. See [Microsoft redistribution
guidance](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files).
