# Owned BF16 corpus source review (2026-10-23)

Independent read-only review completed with **8/8 crop entries OK, zero flagged
crop entries and zero unread entries**. This is source, exact-byte, contract,
provenance and existing-image review, not model attestation or public approval.

| ID | Source truth reviewed | Width × height | Raw bytes | Verdict |
| --- | --- | --- | --- | --- |
| words-regular | `OkuFlow` | 170 × 48 | 8,160 | OK |
| phrase-bold | `Please read this line` | 386 × 48 | 18,528 | OK |
| date-regular | `2026-10-23` | 200 × 48 | 9,600 | OK |
| decimal-bold | `+12.50 -0.75` | 245 × 48 | 11,760 | OK |
| o-zero-regular | `O0 OO 00` | 191 × 48 | 9,168 | OK |
| i-l-one-inverse | `Il1 I l 1` | 155 × 48 | 7,440 | OK |
| negation-bold | `Do not enter` | 257 × 48 | 12,336 | OK |
| accents-inverse | `café naïve` | 193 × 48 | 9,264 | OK |

The reviewer independently recomputed all raw/text/font/notice/generator/
manifest/provenance/sheet/proposal hashes. Source literals, manifest and
provenance agree for every entry, including capital I U+0049, lowercase l U+006C,
digit1 U+0031 and precomposed é U+00E9/ï U+00EF. All raw lengths are width×48.
The existing contact sheet showed upright, unclipped crops and stated polarities.
Tags represent actual source glyph coverage, not recognition success.

The strict manifest has exactly seven root keys and five keys per sample.
All 25 ModelIdentity keys/types/values match the exact independent public boundary
commit `faa7fbfe7b87983802c881411a856328bc873dfd`, including model/dictionary hashes,
FLOAT DEVICE LINEAR ABI, fixed1024 N1/N8 profile, preprocessing and normalized-zero
future padding. Source authorship is synthetic; no user capture or candidate
output supplies expected text. License facts and the absence of third-party
code/font/tool binaries or new clearance claims were reviewed.

One documentation finding identified the not-yet-created repository asset path.
At review time the bytes intentionally existed only in owned work-data while
review was pending. After this review completed, the implementation agent copied
all 12 reviewed files byte-for-byte into the documented repository source path,
independently comparing hashes of each source and copy. The crop/path/license
docs now distinguish original work-data from the later source copies. No
rerender or generated-file modification was used to resolve the finding.
The original finding remains preserved in `review-original.md` alongside the
first-render process records under the owned work-data directory documented in
[the crop proposal](uct_owned_bf16_crops.md).

Only one nested task was created. It used Codex gpt-6.1-sol, reasoning xhigh,
service tier default, shared checkout, read-only instructions; no children remain.
All nested orchestration IDs are retained verbatim:

- Client request ID: `owned-bf16-crops-readonly-review-20261023-r1`
- Task ID: `node:delegated-task:command%3Amcp%3Aa7267bed-787d-4da3-9a82-31cb240cfc79%3Adelegate-task%3Aowned-bf16-crops-readonly-review-20261023-r1`
- Child thread ID: `thread:delegated-task:command%3Amcp%3Aa7267bed-787d-4da3-9a82-31cb240cfc79%3Adelegate-task%3Aowned-bf16-crops-readonly-review-20261023-r1`
- Child run ID: `run:thread:thread%3Adelegated-task%3Acommand%253Amcp%253Aa7267bed-787d-4da3-9a82-31cb240cfc79%253Adelegate-task%253Aowned-bf16-crops-readonly-review-20261023-r1:ordinal:1`
- Child node ID: `node:delegated-task:command%3Amcp%3Aa7267bed-787d-4da3-9a82-31cb240cfc79%3Adelegate-task%3Aowned-bf16-crops-readonly-review-20261023-r1`

The implementation agent and independent reviewer inspected the existing PNG
through `view_image`; no desktop was used. Parent visual review, actual runtime
quality, strict N1 plus full N8 exact-text attestation and explicit separate
catalog approval remain outstanding. ProductionCorpusCatalog is unchanged and
unapproved. No golden pass, ready receipt, accuracy, speed or release acceptance
is claimed. No tests/build/model/GPU work ran in this review.

