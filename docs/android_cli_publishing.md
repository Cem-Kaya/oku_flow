# Google Play releases from the command line

The owner-controlled upload key signs an AAB. A separate Google service account
authenticates publishing; an upload keystore is not a Play API credential.
Use the Google project **OkuFlow Play Publishing** (`okuflow-play-publishing`)
and the **OkuFlow Test Publisher** account. Keep this project's Cloud billing
disabled. Google Play review and tester eligibility still apply to API releases.

Enable the Google Play Android Developer API after accepting its terms, then
grant the service-account email app-specific access in Play Users and permissions:
view OkuFlow app information and release to testing tracks. No production,
financial, user-management or account-wide rights are needed. Google Cloud
project roles are not a substitute for Play permissions; this publisher needs
no Cloud Editor/Owner role. Initial account/legal forms remain Console work.

Download its JSON key privately, move it to local storage outside Drive/Git,
then run:

```powershell
pwsh -NoProfile -File scripts/configure_android_play.ps1 `
  -ServiceAccountJson "$env:LOCALAPPDATA\OkuFlow\google-play\private-key.json"
pwsh -NoProfile -File scripts/publish_android_play.ps1 -CheckAccess
```

The helper stores the complete key JSON as a Windows DPAPI-protected credential
under `%LOCALAPPDATA%\OkuFlow\google-play\okuflow-test-publisher.credentials.xml`.
Only the safe `OKUFLOW_ANDROID_PLAY_PROFILE` path persists in the user environment.
Future shells inherit it; the default path also works in the current shell.
Neither keys, refresh/access tokens, assertions nor encrypted profiles belong in
Git or release ZIPs. The owner can rotate/revoke the Google key in IAM; an existing
local profile is never silently replaced. Keep any original key backup private.

First build with `scripts/build_android_signed.ps1`, inspect the tested release
and record its SHA-256. Put localized notes in a local JSON array, for example:
`[{"language":"en-GB","text":"Fixes USB reconnect crashes."}]`.
Use the exact bundle version/hash and the current active track version:

```powershell
$aab = "$env:LOCALAPPDATA\OkuFlow\android-build\app\outputs\bundle\release\app-release.aab"
$sha = (Get-FileHash -LiteralPath $aab).Hash
pwsh -NoProfile -File scripts/publish_android_play.ps1 `
  -BundlePath $aab -VersionCode 3 -ExpectedSha256 $sha `
  -ExpectedActiveVersionCode 1 -Track alpha `
  -ReleaseName '0.1.2 - USB reconnect fix' `
  -ReleaseNotesPath "$env:LOCALAPPDATA\OkuFlow\google-play\release-notes.json"
# Add -Publish only for the reviewed release that the owner wants submitted.
```

Without `-Publish`, the helper validates a disposable edit and discards it.
An uploaded bundle may remain in Play and consume its version code even when
the edit is discarded; validation is not a purely read-only upload. `-CheckAccess`
creates/discards an edit without uploading or changing a track. Existing bundles
are reused only when both version code and Google-reported SHA-256 match.
Do not rebuild/reuse an already uploaded version code for changed binaries.

Publishing compares the current active version before replacing the selected
testing-track release, validates the complete edit, then commits only with
`-Publish`. It changes no listing, countries, testers, permissions or other tracks.
Production is refused. Receipts are written locally with version, hash, previous
codes, timestamp and commit status; no authentication material is recorded.
Never edit the same app in Console while an API edit is active: Console changes
invalidate API edits. A committed API edit is not proof of review approval or
device availability; verify Publishing overview and the resulting track afterward.

Sources: [API setup](https://developers.google.com/android-publisher/getting_started),
[transactional edits](https://developers.google.com/android-publisher/edits),
[track update](https://developers.google.com/android-publisher/api-ref/rest/v3/edits.tracks/update),
[billing isolation](https://docs.cloud.google.com/billing/docs/how-to/modify-project).
