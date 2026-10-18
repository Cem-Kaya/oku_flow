# Accessible video and UCT translation review (2026-10-18)

The development change adds 60 English source keys across 24 non-English Qt
catalogs: 1,440 translated entries. Small language-capable Luna agents authored
the drafts; independent Luna agents reviewed every assigned entry with an
OK/flagged/unread verdict. No entry remains unread. Review is machine-assisted;
it does not establish native-speaker fluency, RTL rendering or screen-reader
acceptance. Android and website catalogs are unchanged.

| Catalog | First packet reviewed | Corrections applied | Follow-up reviewed | Corrections applied |
| --- | ---: | ---: | ---: | ---: |
| `ar` | 54 | 6 | 6 | 0 |
| `fa` | 54 | 15 | 6 | 0 |
| `he` | 54 | 4 | 6 | 0 |
| `cs` | 54 | 4 | 6 | 0 |
| `nl` | 54 | 3 | 6 | 0 |
| `sv` | 54 | 5 | 6 | 0 |
| `es` | 54 | 5 | 6 | 0 |
| `it` | 54 | 3 | 6 | 0 |
| `pt_BR` | 54 | 2 | 6 | 0 |
| `hi` | 54 | 11 | 6 | 1 |
| `bn` | 54 | 9 | 6 | 0 |
| `ur` | 54 | 3 | 6 | 0 |
| `id` | 54 | 3 | 6 | 1 |
| `vi` | 54 | 3 | 6 | 0 |
| `th` | 54 | 8 | 6 | 0 |
| `ja` | 54 | 7 | 6 | 0 |
| `ko` | 54 | 9 | 6 | 0 |
| `zh_Hans` | 54 | 6 | 6 | 0 |
| `ru` | 54 | 5 | 6 | 0 |
| `uk` | 54 | 6 | 6 | 0 |
| `pl` | 54 | 2 | 6 | 0 |
| `tr` | 54 | 2 | 6 | 1 |
| `de` | 54 | 11 | 6 | 0 |
| `fr` | 54 | 7 | 6 | 0 |

The first packet contains the source-specific transport, file dialogs, explicit
states/errors and Original-only help text. The follow-up covers positioned UCT
accessibility and exact-Pause/processing status. All 142 flagged replacements
were reconciled against source context, placeholders and markup. Four extra
bold-tag pairs were removed from the Arabic/Persian/Hebrew help replacements
after an independent follow-up; the revised French Save-frame sentence was
also independently approved.

Review records stay under `%LOCALAPPDATA%/OkuFlow/recap-localization-20261018/`.
Their per-language entry/key coverage and SHA-256 identities are retained in
`reconciliation.json` and `reconciliation-extra.json`. The follow-up review hash is
`fd17590dcfe03ea154bd72988e8106055a95cc50d132b7174e6f43f265a4fe3f`.

The reviewed REC suffix is naturally identical to English in French, Spanish,
Brazilian Portuguese and Swedish; the validator permits exactly that key for
those languages. Other identity exceptions are limited to reviewed shared video
labels. Complete catalog/source parity is 872 entries per language.

`scripts/check_translations.ps1 -UpdateManifest` compiled all 24 catalogs with
872 finished entries and zero unfinished entries. This is structural/compiler
evidence; native layouts and real assistive-technology behaviour remain separate.
