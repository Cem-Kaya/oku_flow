# OkuFlow Camera privacy policy

Provider: **Cem Kaya**. Updated: **12 October 2026**.

This policy covers `com.okuflow`, the Android companion to OkuFlow for Windows,
and explains the related desktop data flows. Older development installations used
`com.okuflow.camera`; their camera data flows are the same. The Kotlin namespace
does not identify the Play/install package.

## Camera and USB transfer

After camera permission is granted and a host requests capture, OkuFlow Camera
encodes live camera video and transfers it through an authorized USB debugging
(adb) connection to the user's computer. It also transfers camera capabilities
(camera/lens identifiers, orientation, zoom range and light availability), codec
and capture settings, frame timing and error information needed to operate the
stream. The phone app does not request Internet, microphone, location or contacts
permissions. Camera access is limited to the camera-streaming function.

The app accepts Android shell/root peers on its local socket; Android's debugging
authorization controls which computers have adb access. Authorize only a computer
you trust. Debugging grants that computer broad access to the phone; it is not
limited to this camera app. Authorization does not mean the video is encrypted.
Use your own trusted private computer. Selecting "Always allow" is optional for
that computer; avoid persistent debugging authorization on shared computers.
Disconnect when finished, disable USB debugging or revoke debugging authorizations
under Developer options when appropriate.

## Storage and deletion

On the phone, encoded video is buffered in memory for streaming rather than saved
as recording files. Camera and setup preferences are stored in installation-local
Android app storage. Camera light state is session-only and resets off on a new
host connection. **Pause streaming** saves a separate pause preference and closes
the camera/listener service. Reconnecting, reopening or automatic computer launch
does not clear it; only **Resume streaming** does. Camera reset does not clear Pause.
If app storage cannot save the choice, the screen reports that limitation.
Restore automatic settings resets camera preferences. Android's
app-storage controls or uninstalling remove stored preferences. App backup and
device transfer are disabled. No in-app user account is created by the companion.

OkuFlow for Windows can save photos, recordings, notes and diagnostic logs to the
user's chosen computer folder (by default Documents/OkuFlow). Logs may contain
device identifiers/names and timing/error details. Users control these files and
delete them through Windows. Clearing or uninstalling the Android app does not
delete desktop files.

## Optional AI processing on Windows

The phone app streams locally by USB; that does not prevent the Windows app from
sending data elsewhere. If the user chooses Windows AI features, camera images
and questions can be sent through Codex or the configured vision service. A local
vision endpoint is also possible. The selected provider has its own privacy and
retention terms. Clearing phone storage does not delete data held by a provider;
use that provider's deletion/account controls. Normal USB camera viewing does not
require an AI request.

## Public links and contact

This policy is also readable offline in Advanced settings → Privacy policy in
English, Turkish and German. Opening its public link or the contact link delegates
to the user's browser; GitHub and the browser handle that connection under their
own terms.

Contact Cem Kaya at [info@okuflow.com](mailto:info@okuflow.com), or through
[OkuFlow repository issues](https://github.com/Cem-Kaya/oku_flow/issues).
Issues are public: do not include camera images, credentials or private documents.
Material changes to these flows will update this policy and the app's disclosures.
