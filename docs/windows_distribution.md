# Windows installers, signing and updates

The portable ZIP stays supported. The MSI adds a machine-wide installation
under `Program Files\OkuFlow`, with branded public desktop and Start menu
shortcuts, Add/Remove Programs registration and transactional major upgrades.
It includes the entire verified release inventory: Qt, source-built adb,
notices, adb source/relinking ZIP and Microsoft's pinned runtime installer.
Settings and `Documents\OkuFlow` captures are outside the installation and
are preserved by uninstall. The MSI never launches another installer as a
custom action; first launch uses the existing runtime preflight when needed.
Its Restart Manager policy detects locked files without automatically closing
a running OkuFlow. Finish a recording and close the app before upgrading.

## Build and validate an MSI

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

`assets/okuflow-icon.svg` is the owner's cyan website magnifier mark. Export
it with `scripts/generate_brand_icons.ps1` in a Visual Studio 2022 x64 shell.
Qt SVG produces transparent PNGs and a seven-size 16–256px ICO. Native
executables, MSI shortcuts/ARP and Store review logos use these generated
assets. The website copies the exact ICO bytes into its content-hashed favicon
and fallback path; its separate static cache version changes with the icon.
The wordmark remains text and is not squeezed into a small favicon.

Sources: [Microsoft signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options),
[distribution and updates](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/choose-distribution-path),
[desktop packaging prerequisites](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-prepare),
[winget contribution workflow](https://learn.microsoft.com/en-us/windows/package-manager/package/repository).
