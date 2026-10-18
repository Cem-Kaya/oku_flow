# Windows installers, signing and updates

## Exact package and CUDA provenance (audit RB-001/RB-002)

MSI authoring and extraction execute WiX's own apphost from the SHA-256-pinned
4.0.6 NuGet archive. The shared resolver checks every extracted file against
that archive before invocation, rejecting changed files and added dependencies.
It does not resolve a second copy through NuGet feeds or reuse an installed
shim. Package/version/license pins and the developer-only distribution scope
remain unchanged; the MSI receipt records package and apphost hashes.

Release metadata requires a configured build directory containing
`okuflow-build-facts/Release.json`. CMake emits this from the actual application
target, compiler, linked static CUDA runtime and architecture list. Packaging
checks cache/configuration, the successful post-build facts/executable seal and
target-byte agreement, and stages the exact toolkit notice identified there.
Reconfiguring without rebuilding cannot relabel an existing executable. The
controlled signing step refreshes the seal after signing succeeds; standalone
metadata never refreshes it. CPU-only metadata omits CUDA packages even
when the caller has CUDA environment variables; an inconsistent `-CudaEnabled`
assertion is refused. Older build trees need reconfiguration and a successful
application build. These checks do
not establish GPU, driver, installer-guest or public-release acceptance.

## Published channels (2026-10-14)

[GitHub v0.2.5](https://github.com/Cem-Kaya/oku_flow/releases/tag/v0.2.5)
contains the immutable unsigned-preview MSI and portable ZIP from clean source
`142587eced22e5693a00c764b8772748a8b22e75`. Uploaded assets were downloaded
and rehashed before the draft became public.

| Artifact | SHA-256 |
| --- | --- |
| `OkuFlow-0.2.5-x64.msi` | `1029acabdd8eaa3e4b90f8bc388ab8ba912f6d7efcdcf901bce22fe2a3b4798f` |
| `OkuFlow-0.2.5-windows-x64.zip` | `e968b8d0685c6c8b2ef6a6179d434122d4ab4b6b70f794a6cdf6579c4fa968bf` |

Shipping compilation, 83 CPU tests, 89 CUDA tests and 89 shipping-bundle
checks passed without a bypass. Native GUI fixtures ran serially on the
owner-authorized free desktop. The separate GitHub VM also passed standard
CPU/GUI checks and the panel-movement diagnostic at 125%, 150% and 200%.
MSI extraction verified 152 payload files; the disposable Windows guest
passed all 167 install/upgrade/downgrade/shortcut/uninstall/user-data checks.
The website and public README point to 0.2.5. No new winget PR or Store
submission was made. The MCP/agent-control API is under development and is
not part of this release.

### Earlier 0.2.4 preview

[GitHub v0.2.4](https://github.com/Cem-Kaya/oku_flow/releases/tag/v0.2.4)
contains the immutable unsigned-preview MSI and portable ZIP from clean source
`c8c4cf1255084cbb2bb2fa969a3a59f7396bedb0`. Both public downloads were
downloaded again and matched their reviewed SHA-256 hashes:

| Artifact | SHA-256 |
| --- | --- |
| `OkuFlow-0.2.4-x64.msi` | `997ca100b1965952f9fbe646d38bc4c405fc413acf20ac4d514134c7a27557d7` |
| `OkuFlow-0.2.4-windows-x64.zip` | `cf07dd470288d6f7167a06287c4356fad9e265d1f9faacbcf3d5419f70d42e18` |

The release compiled successfully. Background CPU/CUDA tests passed; local
native GUI evidence remains partial following the owner's limit on repeated
desktop runs. The separate bundle CTest invocation used the explicit documented
test bypass. This preview is not a claim of a complete shipping test gate.
MSI structural checks verified all 152 payload files, installer actions and
branded shortcuts. The disposable guest installed the previous MSI and upgraded
to 0.2.4 successfully; shortcut images were captured there.

The portable/MSI build keeps supporting application/runtime files under
`app/`, leaving `oku_flow.exe` as the only root-level EXE. `START_HERE.txt`
explains launch and MSI shortcuts. Metadata and installer readers support both
legacy flat and nested inventories; source-built adb notices/relinking files
stay intact inside the runtime folder. The website links to 0.2.4. Older release
assets remain immutable; no new winget PR was submitted for 0.2.4.

### Earlier 0.2.3 release

[GitHub v0.2.3](https://github.com/Cem-Kaya/oku_flow/releases/tag/v0.2.3)
contains the immutable unsigned-preview MSI and portable ZIP. The exact public
MSI passed the clean-VM install/upgrade/downgrade/uninstall acceptance checks.
[winget submission 451412](https://github.com/microsoft/winget-pkgs/pull/451412)
requests `OkuFlow.OkuFlow` version 0.2.3; automated validation is complete and
moderator merge is pending as of 2026-10-14, so catalog
installation is not yet established. Microsoft Store has only the preparation
entry and structural review package described below; no Store submission is
claimed. Neither the GitHub MSI nor ZIP enables automatic app updates.

The v0.2.3 bundle comes from clean source `6de0a1e256ddbbd6af58dee761cd5fe883070e59`.
Release compilation, 31 CPU, 36 CUDA and 36 bundle CTest checks passed. The
MSI and ZIP each preserve all 151 inventory files; a disposable VM passed 166
install/upgrade/downgrade/icon/uninstall/user-data checks against v0.2.2.
The exact public MSI was downloaded and rehashed. v0.2.2 assets remain immutable.
The local primary bundle is locked/older; the tested v0.2.3 bundle is
`dist\OkuFlow2`, with both executable hashes verified against the active Ninja
output. No user process was terminated to publish it.

The portable ZIP stays supported. The MSI adds a machine-wide installation
under `Program Files\OkuFlow`, with branded public desktop and Start menu
shortcuts, Add/Remove Programs registration and transactional major upgrades.
Advertised shortcut icon data is a separate cached first-party launcher PE
named `OkuFlowIcon.exe`, matching the `.exe` target as Windows Installer requires.
The VM verifies that Windows can render its star magnifier, beyond file presence.
It includes the entire verified release inventory: Qt, source-built adb,
notices, adb source/relinking ZIP and Microsoft's pinned runtime installer.
Settings and `Documents\OkuFlow` captures are outside the installation and
are preserved by uninstall. The MSI never launches another installer as a
custom action; first launch uses the existing runtime preflight when needed.
Its Restart Manager policy detects locked files without automatically closing
a running OkuFlow. Finish a recording and close the app before upgrading.

## Build and validate an MSI

### Automated build, package and publication

`scripts/publish_windows_release.ps1` joins the existing release steps into one
command on a suitably equipped Windows/NVIDIA build machine. It runs the full
tracked shipping/CPU/CUDA matrix and the separate shipping-bundle CTest gate,
then builds the MSI and verifies every ZIP entry against the same bundle.
It refuses a dirty/changing checkout, excluded GUI checks, bypassed bundle tests,
existing version tags and existing output directories. It never closes OkuFlow.

```powershell
pwsh -NoProfile -File scripts/publish_windows_release.ps1 -Version 0.2.5 `
  -NotesFile C:\release-notes\0.2.5.md -AllowUnsignedPreview `
  -PreviousInstallerPath C:\release-fixtures\OkuFlow-0.2.4-x64.msi -Publish
```

`-Publish` also requires the tracked disposable Windows VM installer acceptance.
The command uploads MSI, ZIP and checksums to a draft, downloads and rehashes all
three, then publishes it as latest. Failed uploads/verification leave a draft;
inspect it before retrying rather than replacing versioned bytes. Omitting
`-Publish` prepares local artifacts only. Unsigned publication is explicitly
labelled an unsigned preview; without that switch a trusted signing identity is
required. Private logs and receipts stay in LocalAppData. No winget PR or Store
submission is made.

The routine GitHub CPU/GUI workflow still does not build shipping downloads:
its hosted VM has no NVIDIA hardware for the CUDA shipping gate. This command
automates release work without installing a runner on the owner's PC or treating
hosted CPU tests as hardware acceptance. Native tests need an explicitly free
desktop for that run; background operation does not waive that requirement.

First run `scripts/agent_build.bat` and `scripts/build_release_bundle.bat`.
Use the normal complete test gates, serially on a free desktop. The bundle
script selects executables from the active CMake generator's output layout;
stale `Release` directories from another generator cannot override Ninja's
current executable. It compares both deployed executable hashes to those
actual outputs. `dist\OkuFlow2` remains the locked-bundle fallback.

Run in PowerShell 7:

```powershell
pwsh -NoProfile -File scripts/build_msi.ps1 -Version 0.2.2
# Review/testing without a public signing identity:
pwsh -NoProfile -File scripts/build_msi.ps1 -Version 0.2.2 -AllowUnsignedPreview
```

Use `-BundlePath dist\OkuFlow2` when that is the new bundle. Default output is
`dist\Installers\OkuFlow-<version>-x64.msi`. The script fails on a dirty or
changed bundle, snapshots and rehashes its inventory in LocalAppData, pins
WiX 4.0.6's NuGet package and uses only standard Windows Installer actions.
No WiX extensions/custom-action binaries or .NET runtime ship in the MSI.
The developer needs a supported .NET SDK/runtime for WiX; end users do not.
Versions are three numeric MSI fields; published versioned artifacts must
never be replaced. Component identities are stable per relative path/x64;
product identity changes with the version, and the upgrade family stays fixed.

`test_msi_package.ps1` reads the MSI database without installation, checks
scope, shortcuts, rollback sequencing and absence of custom actions, then
extracts/rechecks every payload file. It does not validate installation.
Before a stable release, use a clean Windows VM to install, upgrade, block a
downgrade, uninstall and verify shortcuts and preserved user data. Missing
runtime/UAC and camera/AI acceptance remain separate checks. Never install or
uninstall a test MSI on the owner's machine to simulate a clean VM.

`scripts/test_msi_sandbox.ps1 -InstallerPath <candidate> -PreviousInstallerPath
<older-fixture> -BundlePath <verified-bundle>` automates that acceptance when
Windows Sandbox is enabled. It owns a new guest, disables network/clipboard/
camera/microphone, maps input read-only and maps only a private evidence folder
for writes. The guest tests silent install, upgrade, downgrade rejection,
every file hash, both shortcuts and uninstall preserving sentinel settings/
captures, then shuts down itself. It refuses an existing Sandbox session and
does not enable Windows features or request elevation on the owner PC.
Detached Sandbox launcher exit is not treated as completion; the host waits
for the guest result. An existing guest or remote session is never reused.
When the Store Sandbox server remains idle after shutdown, the host queries
its guest inventory and proceeds only when it is empty; it never stops the server.
Cold guest installer startup can be slow; `-TimeoutSeconds` controls the host
evidence wait (default 900 seconds). It never terminates a guest to force a pass.

## Signing and SmartScreen

The Microsoft developer account is not an Authenticode certificate. The
current account is individual/Türkiye. Microsoft's published eligibility
currently excludes that combination from Azure Artifact Signing. A trusted
CA certificate or an eligible external signing program is needed for GitHub
MSI/EXE distribution. A new trusted signature can still need SmartScreen
reputation; there is no guarantee that a first release has no warning.

`OKUFLOW_SIGN_CERT_SHA1` selects a certificate already held privately in the
Windows certificate store. Existing bundle signing covers both launcher and
Qt application; MSI signing uses the same pointer and SHA-256 RFC3161
timestamping. Default MSI creation requires signed application executables
and a verified installer signature. `-AllowUnsignedPreview` is explicit and
records its status in the adjacent build receipt. It establishes no public
trust. No self-signed root is installed, no warning is suppressed, and no key
or password belongs in Git, Drive or a release. The build receipt's private
build directory is local evidence; publish only the intended artifacts.

## Microsoft Store and automatic updates

The existing Partner Center product is a **draft MSIX/PWA** entry. Its public
identity is tracked in `scripts/windows-installer/store-identity.json`, read
from the account on 2026-10-13. It is not a credential or proof of approval.
Store MSIX certification provides Microsoft signing and automatic Store
updates without purchasing a public certificate. The Store MSI/EXE route
requires publisher signatures and does not supply automatic app updates.

```powershell
pwsh -NoProfile -File scripts/build_store_msix.ps1 -Version 0.2.2.0 -ReviewOnly
```

This emits an **unsigned structural review package**, checks SDK manifest
validation and round-trips all bundle bytes. It is not submitted or installable
as a trusted public download. Store release is deliberately still blocked:
the current system-runtime launcher may request an elevated installer on a
clean PC. A packaged runtime/framework strategy and packaged Setup Assistant
must be implemented and tested without that UAC bootstrap before submission.
The installed desktop VCLibs framework must not be assumed to satisfy the
reviewed 14.44 minimum. Packaged USB/adb, camera, microphone, GPU, external AI
setup, notes access, settings continuity and update behavior also need real
packaged acceptance. Listing, capability justification and certification are
unfinished. No automatic updater is enabled by the MSI or review package.

## winget

`generate_winget_manifest.ps1` derives version, ProductCode and SHA-256 from
the actual MSI receipt and accepts only an immutable GitHub release URL:

```powershell
pwsh -NoProfile -File scripts/generate_winget_manifest.ps1 `
  -InstallerPath dist/Installers/OkuFlow-0.2.2-x64.msi `
  -InstallerUrl https://github.com/Cem-Kaya/oku_flow/releases/download/v0.2.2/OkuFlow-0.2.2-x64.msi `
  -OutputDirectory "$env:LOCALAPPDATA/OkuFlow/winget-drafts"
```

Validate with `winget validate --manifest <version-directory>`. After VM
acceptance and publication of the exact installer, submit the three manifests
under `manifests/o/OkuFlow/OkuFlow/<version>` to `microsoft/winget-pkgs`.
Generation is not catalog acceptance. Once accepted, users can install with
`winget install --id OkuFlow.OkuFlow --exact` and request updates using
`winget upgrade --id OkuFlow.OkuFlow --exact`. This is an explicit update
command, not a background auto-update service.

## Brand assets

`assets/okuflow-icon.png` is the owner's selected original star magnifier:
a white magnifier around a magenta four-point star in a dark lens. The canonical
1254px PNG artwork stays unchanged. Export the native sizes with
`scripts/generate_brand_icons.ps1` in a Visual Studio 2022 x64 shell. Qt image
scaling produces Store PNGs and a seven-size 16–256px ICO. Native
executables, MSI shortcuts/ARP and Store review logos use these generated
assets. The website copies the exact ICO bytes into its content-hashed favicon
and fallback path and uses the exact canonical PNG in its header/footer.
Its separate static cache version changes with the icon. The superseded cyan
SVG remains a source archive under `assets/legacy`, outside active resources.
The wordmark remains text and is not squeezed into a small favicon.
Run `scripts/test_brand_icons.ps1 -BundlePath <new-bundle>` to verify that both
deployed PE files embed all seven exact icon frames, including incremental builds.

Sources: [Microsoft signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options),
[distribution and updates](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/choose-distribution-path),
[desktop packaging prerequisites](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-prepare),
[winget contribution workflow](https://learn.microsoft.com/en-us/windows/package-manager/package/repository).
