# OkuFlow Camera privacy policy

Provider: **Cem Kaya**. Updated: **5 November 2026**.

OkuFlow Camera turns your Android phone into a USB camera for OkuFlow on
Windows. It is designed to help people with low vision use their phone camera
for live magnification and accessible reading on their computer.

## Privacy at a glance

- Your camera stream goes through the USB connection to the computer you
  authorize. Normal camera viewing does not upload your images to the developer
  or to an AI service.
- The Android app buffers video in memory for streaming; it does not save camera
  recordings on the phone or upload your stream to developer-operated storage.
- The Windows app can save photos, recordings, notes and diagnostic logs on your
  computer. You control those files and can delete them through Windows.
- AI features are optional. When you choose them on Windows, images and questions
  can go to the provider you selected. That provider's own retention and deletion
  rules apply. A local vision endpoint is also supported.
- No account with OkuFlow is needed to use the camera companion.

This policy covers `com.okuflow`, the Android companion, and explains the related
desktop data flows. Older development installations used
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
While paused, the app answers the computer's status check with "paused" only;
the camera stays closed.

When the computer opens OkuFlow Camera over USB, the app can appear over the
lock screen and turn the screen on, so a locked phone on a stand reconnects
without being touched. This shows only OkuFlow Camera's own screen; it does not
unlock the phone or reveal other apps. Anyone holding the locked phone could
then see that screen and use its controls (for example Pause) until the app is
left. A saved Pause is respected: a paused app never appears over the lock
screen, so streaming cannot be resumed there without unlocking. Tapping Pause
while the app is shown over the lock screen takes it off the lock screen at
once; resuming then needs an unlock.
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

## Understanding the Google Play Data safety labels

Google Play uses "collected" to include data transmitted off the phone, even
when the destination is your own computer rather than the developer's servers.
For this app, the USB stream includes camera video, camera capabilities and the
diagnostic information described above. This label does not mean the developer
keeps a copy of your camera images.

Your local photos, recordings, notes and logs can be deleted from your computer.
The developer does not operate storage for your camera stream and cannot delete
files on your devices or data held by your selected AI provider. Use Windows or
Android storage controls for local data and the provider's own controls for
provider-held data. A Play label about the absence of a deletion request
mechanism should not be read as saying that your local files cannot be deleted.

The USB stream has no additional application-level encryption. Computer
authorization controls access but is not a promise of encrypted transmission.
Optional AI services have their own connection and security practices.

## Public links and contact

An offline privacy explanation is available in Advanced settings → Privacy
policy. Its wording reflects the installed app version; this public page contains
the latest clarification. Opening the public policy or contact link delegates to
the user's browser; GitHub and the browser handle that connection under their own
terms.

Contact Cem Kaya at [info@okuflow.com](mailto:info@okuflow.com), or through
[OkuFlow repository issues](https://github.com/Cem-Kaya/oku_flow/issues).
Issues are public: do not include camera images, credentials or private documents.
Material changes to these flows will update this policy and the app's disclosures.
