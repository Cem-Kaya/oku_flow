# Translation review continuation, 14 October 2026

This unreleased correction round follows the earlier 53-agent sampled review.
The owner requested smaller, complete catalog scopes and cancelled the Spark
fixer. Read-only GPT-6-Luna reviewers supplied findings; the coordinating
developer reviewed the proposals and applied the corrections.

## Review coverage and failed attempts

All 24 translated Qt catalogs were divided into four 182-entry scopes each:
96 scopes covering 17,472 translated entries. Another 24 scopes covered each
language's Android resources, launcher, notes viewer, transcription bridge,
website transcription UI and marketing homepage where one exists. English
source wording was retained. The website has 14 marketing locales, rather
than a homepage for every application language.

There were 131 delegation attempts for those 120 scopes. Failed Turkish
Unicode dumps, the failed German second chunk and unavailable-file-tool
attempts were superseded by UTF-8 shell-readable inputs. The Ukrainian first
chunk's refused attempt was also replaced. Terminal status alone was not
accepted as proof of a review: the report body was inspected for unread scopes.
The Russian other-surface report left four tables unresolved, so a further
Luna task reviewed all 116 exported rows: 14 launcher, 21 notes, 24 bridge and
57 Transcribe entries. That report's per-surface header counts were inconsistent
(13 launcher and 25 bridge); the actual exported table inventory is authoritative.

Reviewers report full chunk coverage and list flagged rows without the earlier
worst-40 limit. Their self-reported counts sometimes differ from their tables,
and some proposals refer to stale text or the wrong key. This records complete
assigned review coverage, not independent proof of native fluency or approval
of every sentence. The resources remain machine-assisted drafts.

## Distinct corrected values

These counts compare the final working files with app commit `d920ac0` and
website commit `64061e8`. They count each changed
locale/key once, including corrections recovered from the interrupted round.
They do not add the preceding roughly 800-string pass or count regenerated
HTML a second time. The combined total is 4,383 values.

| Locale | Qt | Android | Launcher | Notes | Bridge | Homepage | Transcribe | Total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| tr | 64 | 0 | 0 | 0 | 6 | 8 | 8 | 86 |
| de | 40 | 0 | 0 | 0 | 1 | 12 | 5 | 58 |
| fr | 159 | 15 | 1 | 1 | 6 | 14 | 14 | 210 |
| es | 175 | 18 | 3 | 1 | 2 | 12 | 13 | 224 |
| it | 152 | 11 | 3 | 2 | 6 | 7 | 7 | 188 |
| pt-BR | 152 | 14 | 2 | 1 | 3 | 22 | 13 | 207 |
| ru | 213 | 10 | 1 | 3 | 9 | 14 | 22 | 272 |
| uk | 193 | 10 | 5 | 5 | 6 | 16 | 1 | 236 |
| ar | 197 | 26 | 5 | 4 | 13 | 28 | 22 | 295 |
| hi | 110 | 1 | 2 | 1 | 6 | 3 | 9 | 132 |
| ja | 156 | 0 | 0 | 1 | 6 | 4 | 7 | 174 |
| ko | 61 | 2 | 0 | 0 | 5 | 6 | 7 | 81 |
| zh-Hans | 120 | 1 | 0 | 1 | 2 | 0 | 7 | 131 |
| nl | 104 | 5 | 0 | 2 | 0 | n/a | 4 | 115 |
| pl | 228 | 3 | 0 | 1 | 0 | n/a | 3 | 235 |
| cs | 229 | 2 | 0 | 1 | 3 | n/a | 6 | 241 |
| sv | 180 | 1 | 0 | 1 | 0 | n/a | 3 | 185 |
| id | 117 | 0 | 1 | 1 | 2 | n/a | 3 | 124 |
| vi | 170 | 3 | 0 | 2 | 4 | n/a | 7 | 186 |
| th | 148 | 2 | 0 | 0 | 3 | n/a | 6 | 159 |
| bn | 151 | 0 | 0 | 0 | 2 | n/a | 7 | 160 |
| fa | 248 | 7 | 0 | 1 | 1 | n/a | 6 | 263 |
| he | 202 | 4 | 3 | 0 | 0 | n/a | 8 | 217 |
| ur | 194 | 3 | 0 | 0 | 0 | n/a | 7 | 204 |
| **Total** | **3763** | **138** | **26** | **29** | **86** | **146** | **195** | **4383** |

## Decisions and examples

- Preserved source keys, positional/brace placeholders, markup and Qt line
  breaks. Repaired lost escaped ampersands in Spanish, Polish, Hebrew,
  Ukrainian and Thai proposals before accepting the catalogs.
- Kept the owner's rule: natural local wording, or an established English term
  when a forced calque would be worse. Japanese and Korean homepage AI wording,
  Turkish camera magnification and French speech-model wording were clarified.
- Fixed Android privacy statements about stored preferences and USB transfer,
  including Ukrainian instructions that wrongly left “Always allow” enabled on
  shared computers. Arabic and Vietnamese now distinguish codec encoding from
  encryption. Clearing phone-app data still does not delete Windows files or
  provider-held data.
- Corrected speech-transcript versus academic-transcript/copy terminology,
  video versus audio recording filters, CPU queue/status text and the Persian
  “faster, less accurate” quality option that had claimed greater accuracy.
- Matched embedded instructions to their visible action labels, including
  Russian, Arabic, Japanese, Korean, Czech and Italian bridge controls. Turkish
  retains the plain action “Yazıya dök” and names its result “Kayıt metni”.
- Rejected proposals that changed the “legally blind” audience claim to total
  blindness or a broader audience, asserted that local data is never sent
  anywhere, added undocumented units, changed fixed-reference stabilization to
  fixed-camera stabilization, or altered correct technical abbreviations.
- Retained understandable low-severity stylistic alternatives where a proposed
  change added no clarity or conflicted with the surrounding labels. Qt plural
  forms and English source redesign remain separate structural work.

## Validation and evidence

`scripts/check_translations.ps1` passes all 24 catalogs with 728 complete
translations each and compiles them with Qt 6.12.0. Its narrowly scoped French
and Dutch identical-word exceptions accept genuine cognates, rather than
allowlisting untranslated sentences.

`scripts/build_android.ps1 -Lint` passed debug assembly, all 68 JVM tests
(zero failures/errors/skips) and lint using the isolated local build at
`%LOCALAPPDATA%\OkuFlow\android-localization-review-build`. No phone was
installed, no screenshots were taken and no signed release was produced.

The sibling website's `npm run i18n` regenerated its 14 homepages.
`npm run check` passed site/CSP/asset checks, pinned vendor notices, the Android
domain association and all 25 Transcribe tables with 57 keys each. The initial
attempt caught forbidden em dashes in first-party copy; the corrected run is
retained separately. Node 24.21.0 came from the existing portable local tools.
Both inline notes maps parse as JSON with identical key inventories.

The Windows background matrix is run through `scripts/agent_build.bat` with
`OKUFLOW_AGENT_EXCLUDE_GUI=1` and an isolated local release build. Its result
is retained in `windows-localization-review-20261014.log` alongside the other
logs below. Native desktop availability was not granted for this run; excluded
GUI fixtures do not establish a complete release gate or rendered layout.

Private reports, exported tables, per-key edit evidence and validation logs
remain under `%LOCALAPPDATA%\OkuFlow\language-work`. No recordings, private
credentials or new translation-service dependencies were introduced. Fonts,
RTL layout, screen readers, native-speaker approval and real-phone rendering
remain separate acceptance work. Nothing in this round establishes publication.
