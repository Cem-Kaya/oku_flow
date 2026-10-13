# Opus review: Windows signing for an individual in Türkiye

Review date: **2026-10-13**. This reviews
[`windows-signing-turkiye-research-2026-10-13.md`](windows-signing-turkiye-research-2026-10-13.md)
(the "prior report"); it does not replace it. This was research only. No certificate
was ordered, no account was changed, nothing was signed or published, and no
vendor was contacted. The prior report was not edited. Its corrections are
listed below for the owner/parent to apply.

## Bottom line: the simplest practical route

1. **Make the Microsoft Store MSIX the main trusted install.** It's free.
   Microsoft re-signs the package after certification, Store installs show no
   SmartScreen warning, and the Store delivers updates. Store installs carry
   Microsoft's signature, so the 60 unsigned third-party PE files described
   below stop being the owner's problem on this route. The only gate is the
   repository's own packaged-runtime and acceptance work. A certificate
   purchase doesn't help with that gate.
2. **Get winget from the Store too.** `msstore` (the Microsoft Store catalog)
   is a default winget source. Once the Store listing is live, check that
   `winget install --source msstore 9PLDP5TMG8M3` works. That gives a winget
   route with no purchased certificate. The prior report missed this.
3. **Keep the GitHub ZIP/MSI as an explicitly unsigned preview** until a
   certificate is truly needed. Today it's published that way, and the
   community winget PR has already passed automated validation unsigned.
4. **Buy an individual (IV) certificate only if** signed GitHub/winget-community
   downloads still matter once the Store listing exists. Examples: portable
   ZIP users, machines without the Store, enterprise users. Before paying,
   get written confirmation that the CA accepts this applicant's Turkish ID
   and address proof. Size the purchase for **about 64 signatures per
   release**, not 4 (see C1/C2).

Skip the Store **MSI/EXE** listing. It needs the same paid certificate plus
signing of every PE file, and it gets no Store-managed updates. With MSIX
available, it adds cost without adding trust.

## What the prior report got right (independently confirmed)

| Claim | Status | Primary source |
| --- | --- | --- |
| Store MSIX is re-signed free by Microsoft; account gives no transferable certificate | Confirmed (page dated 2026-08-29) | [Code signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options) |
| Store MSI/EXE: "The binary and all of its Portable Executable (PE) files must be digitally signed" chaining to a Trusted Root Program CA; versioned immutable HTTPS URL; silent install; UAC allowed; no Store auto-update | Confirmed | [MSI/EXE package requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msi/app-package-requirements), [distribution path](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/choose-distribution-path) (2026-10-07) |
| Artifact Signing Public Trust individuals: US/Canada only; ~US$9.99/month; paid subscription required | Confirmed (quickstart updated 2026-10-08; FAQ 2026-10-06) | [Quickstart](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart), [FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq) |
| EV no longer bypasses SmartScreen | Confirmed verbatim: "EV certificates no longer bypass SmartScreen" | [SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation) |
| SSL.com IV: individuals, "no business registration required", US$129/yr; YubiKey add-on +US$379 | Confirmed on live product page | [SSL.com IV](https://www.ssl.com/products/software-integrity/code-signing/iv/) |
| SSL.com manual validation: ID front/back plus selfie; ID must show address | Confirmed (page dated 2025-09-02) | [SSL.com validation](https://www.ssl.com/how-to/validation-process-for-document-signing-code-signing-and-ev-code-signing-certificates/) |
| Certum Standard cloud from €209; individuals need ID plus a utility bill in their name; stock message contradictory | Confirmed | [Certum product](https://shop.certum.eu/standard-code-signing-in-the-cloud.html), [Certum documents](https://support.certum.eu/en/code-signing-required-documents/) |
| Certum Open Source from €49; revoked if used for "software distributed commercially" | Confirmed | [Certum shop](https://shop.certum.eu/buy-a-code-signing-certyficate), [Certum documents](https://support.certum.eu/en/code-signing-required-documents/) |
| Sectigo OV code signing may be issued to individuals (Video ID) | Confirmed ("Updated on June 2, 2026") | [Sectigo FAQ](https://www.sectigo.com/faqs/detail/OV-Code-Signing-Validation-for-Organizations-and-Individuals) |
| SignPath Foundation excludes "commercial dual-licensing" | Confirmed (page still marked draft) | [SignPath terms](https://signpath.org/terms.html) |
| CSC-31: maximum 460 days from 2026-03-01 | Confirmed (ballot passed) | [CA/B Forum](https://cabforum.org/2025/11/17/ballot-csc-31-maximum-validity-reduction/) |
| v0.2.3 unsigned; winget PR 451412 open; Store ID 9PLDP5TMG8M3 isn't approval | Confirmed via `gh` and the repository | Release body, PR state, `scripts/windows-installer/store-identity.json` |
| Bundle signs `oku_flow.exe`, `OkuFlowApp.exe` and the `mf_dxva_minimal.exe` probe; MSI signs the installer | Confirmed in `scripts/build_release_bundle.bat`, `scripts/windows-installer/common.ps1` | Repository |

The prior report's core recommendation (Store MSIX first, then compare
Certum and SSL.com IV quotes) still holds. The corrections below are about
scope, volume and cost. The direction is unchanged.

## Corrections and omissions, by priority

**C1 — High: GitHub/winget builds need every PE signed, not just the
first-party files (Smart App Control).** The prior report treats a full-PE
signing inventory as a Store MSI/EXE requirement only. Microsoft's SmartScreen
page states: "Smart App Control will block execution of unsigned files unless
the file has a positive reputation. Smart App Control signature checks apply to
all executable files, not just those downloaded from the Internet." Microsoft
Support says that when Smart App Control blocks an improperly signed file an app
loads, "The functionality that depends on the file doesn't work."
([SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation),
[Smart App Control blocked part of this app](https://support.microsoft.com/en-au/topic/smart-app-control-has-blocked-part-of-this-app-0729fff1-48bf-4b25-aa97-632fe55ccca2))
A read-only scan of the local v0.2.3 bundle (`dist\OkuFlow2`, 151 files)
found 100 PE files:

- 33 signed by The Qt Company Oy and 4 by Microsoft. All are timestamped and
  valid. Leave these unchanged.
- **63 unsigned.** Three are first-party files covered by the existing hook
  once a certificate exists: `oku_flow.exe`, `OkuFlowApp.exe` and
  `mf_dxva_minimal.exe`. The other **60** are the source-built
  `platform-tools\adb.exe` plus 59 DLLs, including the `AdbWin*`, abseil,
  protobuf, MinGW runtime and compression DLLs. No hook covers them.

`scripts/build_msi.ps1` currently requires valid signatures only on
`oku_flow.exe` and `OkuFlowApp.exe`. A "signed" MSI built today would
therefore still ship 60 unsigned PEs. On a Smart App Control-enabled PC, the
phone-camera (adb) feature could fail unless those files have cloud
reputation. Smart App Control behavior wasn't tested here. Validate it in a
clean VM with Smart App Control enabled. The prior report's "GitHub MSI / ZIP:
sign the executable payloads" row is right in spirit, but its step 4
understates this.

**C2 — High: the SSL.com eSigner budget is too small for this signature
volume, and its price source is stale.** Signing every unsigned PE plus the
MSI takes **about 64 signing operations per release**. Signing the ZIP's
loose files separately doesn't add more, because the same signed files feed
both artifacts. Rebuilt candidates consume more. eSigner IV Tier 1 provides
240 signings per year for US$180, with US$1 per extra signing. That covers
about three full releases a year. Also, that table comes from an SSL.com page
**dated 2022-05-12**
([eSigner pricing](https://www.ssl.com/guide/esigner-pricing/)), and the live
IV product page lists eSigner as "+ Subscription" with **no price shown**. So
US$309/yr is indicative, not current. Volume-friendlier options:

- **Certum cloud.** Certum publishes a **5,000 signatures/month** limit for
  cloud products. The prior report said limits needed a quote.
- **SSL.com IV on the YubiKey.** No per-signature metering was shown:
  US$379 for the key plus US$129/yr for the certificate. Confirm token reuse
  at renewal.
- **Fewer PE files.** Reduce the number of adb DLLs, for example with a static
  adb build, after license and relinking review per
  `docs/adb_distribution.md`.

Whether eSigner meters per file isn't stated on the pages reviewed. Ask SSL.com.

**C3 — Medium: the winget `msstore` source was missed.** The prior report says
a purchased certificate is needed for winget-style distribution with trust.
winget ships three default sources, including **msstore — "The Microsoft Store
catalog"** ([winget source](https://learn.microsoft.com/en-us/windows/package-manager/winget/source)).
Once the Store MSIX is live, `winget install` from msstore gives a
Microsoft-signed, Store-updated install with no certificate purchase. Confirm
OkuFlow is resolvable there after publication; that was not tested.

**C4 — Medium: the Turkish address proof is the most likely blocker at
SSL.com.** The SSL.com manual flow says "we must be able to see your name,
address, year of birth, and photograph" on the ID and lists no separate
address document. Turkish chip ID cards and passports may not print a
residential address; check the actual document. Ask SSL.com before ordering
whether it accepts a separate address proof (a utility bill, a bank statement,
or the e-Devlet *Yerleşim Yeri Belgesi*) and in which language. The SSL.com
product page also says validation completes after "a successful callback to a
listed phone number", so ask what "listed" means for a Turkish individual.
Certum explicitly asks individuals for a utility bill "issued for the
subscriber". If the household bills aren't in the owner's name, ask which
substitutes Certum accepts.

**C5 — Medium: the PR status is now more advanced than "open".** As of the
`gh` read on 2026-10-13, PR 451412 is open with the **`Validation-Completed`**
and **`Azure-Pipeline-Passed`** labels, and it is not merged. The unsigned MSI
passed winget's automated validation, so the remaining step is moderator
merge. `docs/windows_distribution.md` still says "awaiting checks/review";
consider updating it.

**C6 — Low: Artifact Signing is unavailable to organizations in Türkiye
too.** The prior report notes that the expanded organization list doesn't
help an individual. Türkiye also isn't on the expanded organization list (US,
Canada, EU, UK, Australia, New Zealand, Japan, South Korea, Singapore,
Switzerland, Norway, Israel). Forming a Turkish company wouldn't unlock it.
Individual validation also requires an Azure billing account of type
*Individual* whose name and sold-to address match the ID. That point is moot
for this owner.

**C7 — Low: SignPath Foundation has two more disqualifiers.** Besides the
dual-licensing rule, the terms say "The project may not contain any
proprietary, non open-source component" (the bundle ships Microsoft's
`vc_redist.x64.exe`) and "The code signing certificate is issued to SignPath
Foundation". Windows would show SignPath Foundation as the publisher, not the
owner.

**C8 — Low: tooling details.**

- **Timestamp authority.** The scripts hard-code
  `/tr http://timestamp.digicert.com`. Any RFC 3161 time-stamp authority works
  with any CA's certificate, so "use the provider TSA" is optional.
- **Verification.** The scripts verify with `signtool verify /pa`. Adding
  `/tw` (warn on a missing timestamp) and failing on that warning matches the
  prior report's step 5.
- **Validity.** Certum states a **459-day** per-certificate cap from
  2026-02-27, with free reissues inside 2/3-year products. That's consistent
  with CSC-31.

**C9 — Low: what the certificate shows publicly.** An IV certificate's
subject is the validated personal name, usually with locality and country.
Artifact Signing, for example, displays city, state and country. The name
appears in UAC and SmartScreen prompts and in every signed file. "OkuFlow"
appears as the publisher only in the Store listing (`publisherDisplayName`).

**C10 — Accessibility note for the owner.** The identity flows rely on visual
steps: document photos at specified resolution, a selfie holding the ID, QR
scans, and Video ID. Ask each CA for an assisted alternative before paying.
Certum lists Registration Points and notarial confirmation as options. Sighted
help with the capture steps is also reasonable.

## Refined action plan (ordered; no purchases implied)

1. **Unblock the Store MSIX.** The repository already identifies the blocker:
   the system-runtime UAC bootstrap. Microsoft's packaging guidance says Store
   apps that need elevation for any part of their functionality aren't
   accepted, and it documents the VCLibs framework dependency as the packaged
   C++ runtime route
   ([prepare to package](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-prepare)).
   The repository already notes that the VCLibs framework may not meet the
   14.44 minimum. Evaluate in-package runtime DLLs instead, with license
   review, then complete the packaged camera/USB/adb/AI/notes/update acceptance
   already listed in `docs/windows_distribution.md`.
2. **Submit the MSIX through Partner Center** using the tracked identity.
   After certification, make the Store link the primary download on
   okuflow.com and in the README for typical users, and verify the `msstore`
   winget path.
3. **Keep GitHub/winget-community releases explicitly unsigned** until step 4
   is justified. Never replace published bytes.
4. **If signed direct downloads are still wanted:** the owner (not an agent)
   sends Certum and SSL.com the questions below, without attaching ID. Choose
   the provider that confirms applicant acceptance in writing at the lowest
   cost for about 64 signatures per release.
5. **Engineering before the first signed release:**
   - Extend the bundle signing hook to every unsigned PE, including the 60
     adb files.
   - Make `build_msi.ps1` and the bundle gate fail on **any** unsigned or
     untimestamped PE in the payload, keeping vendor signatures unchanged.
   - Regenerate inventories and notices after signing.
   - Run the clean-VM test with Smart App Control enabled.
   Optionally reduce the adb DLL count first, after license review.
6. **Store MSI/EXE listing:** don't pursue it while the MSIX route is viable.

### Questions for each CA (owner to send)

- Do you issue IV/individual code-signing certificates to a resident of
  Türkiye with no registered business?
- Which documents prove identity and address if the national ID/passport
  shows no address? Do you accept the e-Devlet residence document, and must
  documents be translated or notarized?
- What is the exact subject (name, locality, country) shown on the certificate?
- Total price in EUR/USD including VAT, any validation fees, and the price per
  validity period?
- Signing limits (per file? per month?), overage pricing, and SignTool
  compatibility with a certificate-store thumbprint (`/sha1`)?
- Renewal, reissue and token-reuse fees, and the refund if validation fails?
- Do you allow signing redistributed third-party open-source binaries (adb,
  MinGW runtime DLLs) built or shipped by the subscriber?
- Is there an assisted or in-person validation option for a low-vision
  applicant?
- Certum only: does the GPL-3.0 plus commercial dual license count as
  "distributed commercially" for the Open Source product? Treat it as
  ineligible unless the answer is a written yes.

## Not verified / not accessed

- Checkout totals in TRY, Turkish VAT treatment, and card FX fees. No
  checkout pages were used.
- Certum per-duration prices and real stock.
- The current eSigner price. The live page shows no price; the table is from
  2022.
- Whether either CA accepts this specific applicant. No vendor was contacted.
- Smart App Control behavior on OkuFlow's actual files, and `msstore`
  resolvability. Neither was tested.
- Whether any CA's subscriber agreement restricts signing third-party binaries.
  Not found on the pages reviewed.
- The Store MSIX path's certification outcome for this app.

Neither signing report is listed in `improvement_ideas/README.md`.
