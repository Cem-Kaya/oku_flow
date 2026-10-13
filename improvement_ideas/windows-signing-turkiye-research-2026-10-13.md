# Windows signing for an individual in Türkiye

Research date: **2026-10-13**. Research only: no certificate ordered, identity
documents submitted, account changed, artifact signed or release published.
Prices below are observed public USD/EUR prices, not a Turkish checkout quote;
taxes, currency conversion, shipping and any separate verification costs need
confirmation.

**Recommendation:** keep Microsoft Store **MSIX** as the route to free Store
signing and Store updates, after the existing packaged-runtime acceptance work.
For independently signed GitHub/winget releases, seek a normal **individual**
code-signing certificate. SSL.com IV with eSigner has the clearest documented
automation path and a small-volume budget of **US$309/year before tax**.
Certum Standard cloud, advertised **from €209**, is a useful potentially cheaper
quote to compare. A company is not inherently required for these individual
products. Neither provider's public material reviewed here establishes final
acceptance of this particular Turkish applicant; confirm country, ID and address
verification before paying.

## What each distribution channel provides

| Channel | Signing responsibility and consequence |
| --- | --- |
| Microsoft Store MSIX | Microsoft signs the submitted package after certification, without a separately purchased certificate. This is Store package signing; the developer account does not supply a transferable private key/certificate for arbitrary GitHub EXE/MSI files. [Microsoft signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options) |
| Microsoft Store MSI/EXE listing | The publisher supplies an immutable HTTPS installer URL. The installer **and all PE files** must already have valid signatures chaining to a Microsoft Trusted Root Program CA. The installer must support silent, offline installation; UAC is permitted. Microsoft does not re-sign this installer. [Package requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msi/app-package-requirements) |
| GitHub MSI / portable ZIP | Obtain the signing identity independently. Sign the executable payloads and final MSI; packaging signed files in a ZIP does not itself create an Authenticode signature for the ZIP. Keep the published checksums for the completed artifacts. [Executable signatures](https://learn.microsoft.com/en-us/windows/win32/secbp/understanding-pe-signatures) |
| winget community catalog | A manifest adds discovery and installation of the publisher's installer. It does not confer a Microsoft signature. Microsoft describes signing as recommended for this route. Use the final installer's URL and SHA-256; catalog acceptance and trusted signing are different properties. [Distribution comparison](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/choose-distribution-path), [manifest submission](https://learn.microsoft.com/en-us/windows/package-manager/package/repository) |

The public [v0.2.3 release](https://github.com/Cem-Kaya/oku_flow/releases/tag/v0.2.3)
is explicitly unsigned. [winget PR 451412](https://github.com/microsoft/winget-pkgs/pull/451412)
was still **open** when checked. The tracked Store identity is `9PLDP5TMG8M3`;
its existence is not Store approval. The current
[distribution documentation](../docs/windows_distribution.md) and
[review-only packaging script](../scripts/build_store_msix.ps1) still require
packaged runtime/bootstrap, hardware, setup and update acceptance before Store
submission. This research does not remove that gate. Store-managed app updates
are available for MSIX, not the traditional Store MSI/EXE path.
[Microsoft distribution guidance](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/choose-distribution-path)

## Practical signing choices

| Route | Eligibility evidence | Public cost / operating model | Assessment |
| --- | --- | --- | --- |
| Azure Artifact Signing, formerly Trusted Signing | Public Trust individual developers must be in the **US or Canada**. Türkiye is not supported for this owner; changing the Azure resource region does not change identity eligibility. | Microsoft quotes a starting price around **US$9.99/month**; a paid Azure subscription is required. | Not an available public-signing route now. Private Trust is not trusted by public Windows installations. [Current quickstart](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart), [FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq), [trust models](https://learn.microsoft.com/en-us/azure/artifact-signing/concept-trust-models), [price summary](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options) |
| SSL.com **IV** + eSigner cloud | Explicit individual product: verified personal name, **no business registration required**. Confirm Türkiye ID/address support with validation staff. | Certificate **US$129/one year**, plus IV cloud Tier 1 **US$180/year for 240 signings**, or **US$20/month for 20 signings**. Annual total **US$309**; twelve monthly cloud payments plus annual certificate total **US$369**, before any introductory credit/tax. Additional Tier 1 signings **US$1 each**. | Strong fit for Windows SignTool and future CI. Count signed files, not releases. [IV product](https://www.ssl.com/products/software-integrity/code-signing/iv/), [eSigner pricing](https://www.ssl.com/guide/esigner-pricing/) |
| SSL.com IV + physical token | Same identity validation; private key stays on approved hardware. | Current product lists **US$379 YubiKey add-on**, plus US$129 annual certificate: **US$508 initial subtotal**, before shipping/tax. Certificate renewal continues; confirm token reuse/re-provisioning fees. | Useful for owner-controlled local signing without a cloud subscription; token availability constrains hosted CI. [IV product and hardware options](https://www.ssl.com/products/software-integrity/code-signing/iv/) |
| Certum **Standard Code Signing in the Cloud** | Official verification instructions explicitly accept individuals, with identity verification and a utility bill in the subscriber's name. | Product advertises **from €209**, using SimplySign rather than a physical card. Duration-specific checkout, taxes, signing limits, renewal and verification fees need a written quote; parsed page contains both stock messages. | Compare for occasional owner-operated releases. Do not assume unattended CI is included or supported without clarification. [Product](https://shop.certum.eu/standard-code-signing-in-the-cloud.html), [individual requirements](https://support.certum.eu/en/code-signing-required-documents/) |
| Sectigo non-EV code signing | Current June 2026 guidance explicitly allows individuals and uses photo ID/video verification, address/contact checks and request authorization. | Obtain a Türkiye quote including certificate, token/cloud storage, shipping and renewal; no reliable complete quote established here. | Legitimate alternative if the first two providers cannot validate the applicant. [Individual eligibility and validation](https://www.sectigo.com/faqs/detail/OV-Code-Signing-Validation-for-Organizations-and-Individuals) |

The current Azure quickstart lists more eligible **organization** countries than
Microsoft's general comparison page. Both still restrict **individuals** to the
US and Canada, so that documentation difference does not change this decision.
Do not buy EV solely to remove SmartScreen: Microsoft says EV no longer provides
an immediate reputation bypass. Fresh signed releases can still show warnings;
there is no guaranteed download threshold or date for reputation. A signature
establishes publisher identity/integrity, not Store approval or universal policy
acceptance. [SmartScreen guidance](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)

## Free and discounted open-source programs

**SignPath Foundation is incompatible with OkuFlow's current commercial dual
licensing.** Its published eligibility rule explicitly requires an OSI-approved
license without commercial dual licensing. The page labels its code of conduct
as a draft, but this is the published application condition; do not assume an
exception or change OkuFlow's licensing merely to qualify. Acceptance would
also involve controlled build/signing policies, rather than receiving an
unrestricted private key. [Foundation terms](https://signpath.org/terms.html)

Certum advertises paid Open Source cloud signing from **€49**. Its rules say
certificates used to sign commercially distributed software will be revoked.
Public material does not resolve whether the same GPL/commercial dual-licensed
OkuFlow binary qualifies. Obtain explicit written approval of that exact model
before considering it; ordinary Standard signing avoids that program-specific
ambiguity. [Certum products](https://shop.certum.eu/buy-a-code-signing-certyficate),
[Open Source restrictions](https://support.certum.eu/en/code-signing-required-documents/)

## Application and implementation steps

1. **Request eligibility and a complete quote, without sending ID initially.**
   State: individual resident in Türkiye, no registered company, Windows user-mode
   EXE/DLL/MSI signing for OkuFlow, GPL-3.0-only plus commercial licensing, GitHub
   and winget distribution, optional future Store MSI listing. Ask about Turkish
   passport/ID acceptance, address evidence, translations/notarization, verified
   publisher spelling, cloud signing limits, renewal/reissue fees and refunds
   if validation fails. SSL.com and Certum establish individual products, but a
   Turkish-language web page or a global reseller map is not applicant approval.

2. **Prepare identity information privately.** Expect exact legal name, physical
   address, email, telephone, valid government photo ID and identity/liveness
   verification. SSL.com documents ID front/back and a selfie holding it; its
   manual instructions require visible address details. Confirm its alternative
   address proof if the ID does not show the address. Certum explicitly requests
   a utility bill for an individual. Use only the CA's private submission portal.
   The signed publisher will normally be the validated personal name, not an
   unregistered `OkuFlow` company name. [SSL validation requirements](https://www.ssl.com/how-to/validation-process-for-document-signing-code-signing-and-ev-code-signing-certificates/),
   [Certum requirements](https://support.certum.eu/en/code-signing-required-documents/)

3. **Choose key custody and test the provider integration.** Public signing keys
   belong in approved hardware or a cloud HSM, not an exportable PFX in Drive or
   Git. SSL eSigner CKA exposes the cloud certificate through the Windows
   certificate store for SignTool; its CodeSignTool also supports CI. Start with
   owner-operated signing. For later CI, protect release authorization and
   credentials, isolate the signing job from untrusted PR execution, and retain
   signing audit records. eSigner automation uses account credentials and a TOTP
   secret; HSM custody does not make those credentials harmless. Keep generated
   credential configuration and secrets out of release artifacts and logs.
   [SSL CI integration](https://www.ssl.com/how-to/integrating-esigner-with-ci-cd-pipelines-a-complete-setup-and-configuration-guide/)

4. **Integrate with the existing tracked release gates.**
   `OKUFLOW_SIGN_CERT_SHA1` already selects a current-user certificate;
   `OKUFLOW_PUBLIC_RELEASE=1` rejects missing signing. The bundle script signs
   `oku_flow.exe`, `OkuFlowApp.exe` and its runtime probe; the MSI script signs
   the final installer. Provider middleware needs a real end-to-end test before
   claiming compatibility. A Store **MSI/EXE** submission additionally needs a
   complete PE inventory, including unsigned third-party DLLs/helpers; current
   first-party hooks alone do not establish that requirement. Preserve existing
   vendor signatures and reviewed notices, and regenerate affected inventories
   after signing. [Bundle script](../scripts/build_release_bundle.bat),
   [MSI script](../scripts/build_msi.ps1)

5. **Timestamp, verify, then freeze the release.** Use SHA-256 and RFC 3161
   timestamping (`/fd SHA256 /tr <provider TSA> /td SHA256`). Verify signatures
   and timestamps, for example `signtool verify /pa /all /v /tw <file>`, and treat
   timestamp warnings as a release failure. A valid timestamp can preserve an
   existing signature after certificate expiry; new builds still need a current
   signing certificate and service. Multi-year purchases can require certificate
   reissuance: the industry maximum became 460 days in March 2026, with some CAs
   using shorter caps. [Timestamping](https://learn.microsoft.com/en-us/windows/win32/seccrypto/time-stamping-authenticode-signatures),
   [SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool),
   [CA/Browser Forum change](https://cabforum.org/2025/11/17/ballot-csc-31-maximum-validity-reduction/)

6. **Publish a new immutable version after acceptance.** Complete the tracked
   build/test matrix and clean-VM installer acceptance. Generate hashes/SBOM and
   winget manifests from the final signed bytes. Do not replace v0.2.3's already
   published assets or change bytes at an existing manifest URL. For Store MSIX,
   finish the separate packaged acceptance work and submit using the tracked
   Partner Center identity; do not replace it with the CA certificate subject.
   [Repository distribution workflow](../docs/windows_distribution.md)

Remaining evidence gaps: applicant-specific Turkish identity acceptance; final
tax-inclusive invoices and Certum duration/limits/availability; actual provider
middleware compatibility; full PE signing inventory for a Store MSI route;
packaged MSIX runtime/hardware/update acceptance. No signing or publication test
was performed. This documentation-only research requires no application rebuild.
