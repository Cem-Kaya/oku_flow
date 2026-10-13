# OkuFlow Camera closed-test readiness

## Verified Console and update status (2026-10-13)

Google Play Console shows the Alpha track active with version code 1 / 0.1.0.
The existing selected tester list now has 10 members. The signed USB reconnect
fix, version code 2 / 0.1.1, passed release tests, lint and APK/AAB artifact
validation and was accepted into a new Console release draft. Submission and
review are separate from upload; fixed Play-signed unplug/replug acceptance is
still pending. The signed code-3 / 0.1.2 candidate supersedes that draft with
an actual generated R8 mapping; native symbols remain unavailable from the
stripped dependency. The dedicated OkuFlow Play Publishing Cloud project has
billing disabled and the Play API enabled. OkuFlow Test Publisher has app-specific
view/testing-release access; its private local credential authenticated and read
the active code-1 track through the CLI. Commit, review and tester availability
remain separate gates. [CLI workflow](../docs/android_cli_publishing.md).

The immediate scope is the owner's **first Google Play closed test for 12 invited
testers**, not production publication. This document records preparation and
remaining gates, not a submission, policy exception or approval. The app targets
stable API 37 with minimum API 30; do not downgrade to API 35 based on an outdated
preview claim. API 37 exceeds the current API 36 submission floor.
[Target API policy](https://support.google.com/googleplay/android-developer/answer/11926878).

## Implemented technical preparation

- The tracked release path runs release JVM tests, lint, APK/AAB assembly and
  artifact validation. It checks non-debuggable/non-test-only identity,
  permissions, optional rear camera/autofocus, camera FGS and non-exported Pause
  receiver, signatures or explicitly unsigned review mode, and 16 KB APK/ELF
  alignment. The prepared code-3 / 0.1.2 release now runs R8 with renaming,
  shrinking and optimization disabled to embed a genuine generated mapping.
  Its local/AAB bytes and readable app names are validated. Native FULL debug
  metadata is requested, but the stripped graphics-path dependency provides no
  extractable symbols; native-warning removal and Console acceptance remain
  unverified. [Diagnostic evidence](../improvement_ideas/android-crash-symbols-and-mapping.md).
- The actual 65-entry / 64-unique-artifact runtime graph is reviewed against
  official POM/source and original license material. Native graphics-path
  source/build inputs and binary dependencies are reviewed separately. Original
  notices are accessible offline and verified in the APK/AAB, with hashes tying
  them to the resolved graph. [Review evidence](licenses/README.md).
- **Pause streaming** on the main/settings screens and foreground notification
  saves intent before stopping the camera/listener. App reopening, USB
  reconnects, automatic Windows launch and retry loops cannot clear it. Only
  **Resume streaming** does, and camera/cable/debugging prerequisites still apply.
  Notification permission remains optional; denial leaves the phone action usable.
- Normal setup text explains the USB/Windows AI boundary and authorizing only
  your own trusted private computer. **Always allow** is optional for that
  computer. No PIN, new pairing step, TLS or application-encryption mechanism
  has been added; the existing USB streaming and encoding transport is unchanged.

## Owner and closed-track gates still required

1. The owner-controlled upload key is now protected outside Drive/Git, the
   signed APK/AAB passed matching-certificate validation, and Google Play
   accepted build 1 / version 0.1.0 into the Alpha closed-test draft on
   2026-10-12. Play App Signing is configured. This is not a rollout or approval.
   The unsigned review artifact and debug APK are not Play upload candidates.
   Run `pwsh -NoProfile -File scripts/build_android_signed.ps1 -InitializeKey`
   in the owner's terminal for masked local input, private key/protected profile,
   public certificate export and a signed Release build. Future runs omit
   `-InitializeKey`; passwords are process-only inputs, not permanent environment
   values. [Helper details](BUILDING.md#release-artifacts-and-upload-signing).
   [Signing](https://developer.android.com/studio/publish/app-signing).
   The manifest handles only verified HTTPS `okuflow.com/camera/open`; the
   website's live Digital Asset Links uses Google's public app-signing
   certificate. The local upload key is separate and is never published.
2. Verify developer/account access and complete the closed-track Console
   requirements: tester invitations/access, store listing, content rating,
   audience/ads declarations, camera FGS explanation/demo and app-access
   instructions. State the Windows/data-cable/debugging requirement before
   download. Camera viewing needs no AI login; selected AI features are optional.
   [Review preparation](https://support.google.com/googleplay/android-developer/answer/9859455),
   [FGS declarations](https://support.google.com/googleplay/android-developer/answer/13392821).
   The current Console draft uses the owner's name **OkuFlow** with the built
   immutable package `com.okuflow`, as chosen when the owner created the Play
   draft. The built Kotlin class remains `com.okuflow.camera.MainActivity`, so
   the adb component is `com.okuflow/com.okuflow.camera.MainActivity`. A created
   app draft does not clear signing, closed-track forms or policy review.
3. Publish [PRIVACY.md](PRIVACY.md) at
   <https://github.com/Cem-Kaya/oku_flow/blob/main/android/PRIVACY.md>, verify the
   public URL, and enter it in Console. Offline English/Turkish/German policy
   content exists, but this new public source must actually be published.
4. Complete Data Safety from the whole data flow: camera video/capabilities,
   capture/error metadata, USB transfer, Windows saving and optional selected AI
   provider. Closed tracks also require this form; the internal-only exemption
   must not be applied to a closed test. No Internet permission does not prove
   no off-device collection, encryption or ephemeral retention. Evaluate
   collection exceptions separately from user-initiated sharing exceptions.
   [Data Safety](https://support.google.com/googleplay/android-developer/answer/10787469).
5. Resolve the **unchanged USB transport's Play-policy applicability**. Google's
   direct User Data policy includes camera data as sensitive and requires secure
   transmission using modern cryptography. We found no explicit private-USB
   exemption in that policy. The owner has chosen no application encryption;
   trusted-computer authorization is described accurately and does not establish
   encryption. Treat this as an unresolved policy applicability risk, not a
   confirmed rejection and not an independently granted exemption. It is not
   cleared by successful offline tests, user consent or closed-test status.
   [Direct User Data policy](https://support.google.com/googleplay/android-developer/answer/10144311).
6. Validate generated Play/bundletool installation artifacts and 16 KB runtime
   behavior. Current checks inspect the assembled APK ZIP and both archives'
   native ELF segments; they do not verify generated splits running on a device.
   [16 KB support](https://developer.android.com/guide/practices/page-sizes).
7. Inspect the actual signed tester build on devices: Pause via phone and
   notification, reopen/reconnect/permission denial, explicit Resume, notification
   denial, offline privacy/notices, translated layouts and accessibility. Pure
   JVM intent tests verify retry/reopen persistence boundaries; they do not
   establish native camera release timing or screen pixels. Provide exact
   corresponding application source to GPL recipients along with the applicable
   notices. Do not refer to an older source revision as this build's exact source.

## Tester/reviewer instructions draft

1. Install the authorized Play closed-track release on an Android 11+ phone with
   a camera. Obtain the matching published OkuFlow Windows build. Some phone
   vendors require their own Windows USB debugging driver; disclose this hardware
   requirement rather than claiming a standalone phone magnifier.
2. Follow Developer options, USB debugging and camera permission setup. Use a
   data-capable cable with your own trusted private Windows computer, unlock the
   phone and authorize that computer. Persistent **Always allow** is optional;
   avoid it on shared computers. No supplied account, PIN or added pairing is needed.
3. Open Windows OkuFlow. A ready authorized phone is discovered; choose it in
   Camera settings if another camera was explicitly saved. The phone shows
   Connected then Streaming with actual selected mode. Normal camera viewing
   needs no AI sign-in.
4. Choose **Pause streaming** on the phone or its notification: camera/listener
   stop. Disconnect/reconnect and reopen the app; it must remain paused. Select
   **Resume streaming** to reconnect deliberately. Camera-settings reset must
   not clear Pause. Notification permission can be declined without blocking
   streaming or the phone UI's Pause action.
5. Read Advanced settings → Privacy policy and Open-source licenses offline.
   Confirm visible labels/controls at larger fonts in the phone's language. Report
   device/build/mode and observed problems without sharing private camera material.

## Separate later production acceptance

The owner-selected 12-person group is the immediate closed-test scope. For
relevant personal accounts created after 13 November 2023, at least **12 testers
opted in continuously for 14 days** are required before applying for production
access. This is not a blanket requirement for every account or an old 20-tester
rule, and closed-track creation is not itself production approval.
[Testing requirements](https://support.google.com/googleplay/android-developer/answer/14151465).

Broader phone/lens coverage, front-only/fixed-focus hardware, native TalkBack and
200% fonts in all three languages, cable/recovery/decoder fallback and long
thermal/charging acceptance remain separate expanded-release work. Historical
short Pixel/Samsung debug captures do not establish all-device performance,
lossless transfer or sensor-to-photon latency. No change here claims a measured
streaming speed improvement or a performance acceptance result.
