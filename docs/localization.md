# Application languages

The unreleased Windows and Android companions support 25 UI languages. This
includes the website's 14 homepage languages, Dutch and ten additional widely
used languages. The browser transcription tool and newly generated local notes
use the same 25-language set. Marketing homepages remain at 14 languages.

Published Windows v0.2.3 and Play Android 0.1.2 (code 3) predate this work.
The prepared Android version is 0.1.3 (code 4); building it locally does not
install it, upload it or release it to testers.

| Language | Stable code | Android resource qualifier |
| --- | --- | --- |
| English | `en` | unqualified `values` |
| Turkish | `tr` | `values-tr` |
| German | `de` | `values-de` |
| French | `fr` | `values-fr` |
| Spanish | `es` | `values-es` |
| Italian | `it` | `values-it` |
| Portuguese (Brazil) | `pt-BR` | `values-pt-rBR` |
| Russian | `ru` | `values-ru` |
| Ukrainian | `uk` | `values-uk` |
| Arabic | `ar` | `values-ar` |
| Hindi | `hi` | `values-hi` |
| Japanese | `ja` | `values-ja` |
| Korean | `ko` | `values-ko` |
| Chinese (Simplified) | `zh-Hans` | `values-b+zh+Hans` |
| Dutch | `nl` | `values-nl` |
| Polish | `pl` | `values-pl` |
| Czech | `cs` | `values-cs` |
| Swedish | `sv` | `values-sv` |
| Indonesian | `id` | `values-in` |
| Vietnamese | `vi` | `values-vi` |
| Thai | `th` | `values-th` |
| Bengali | `bn` | `values-bn` |
| Persian | `fa` | `values-fa` |
| Hebrew | `he` | `values-iw` |
| Urdu | `ur` | `values-ur` |

## Defaults and controls

Windows offers **System default** followed by native language names and
decorative flags in **Advanced → Settings → Language**. A supported saved
choice wins over Windows display preferences; missing/unknown settings follow
the system, falling back to English. The `system` token keeps following the
system on later launches. Existing explicit `en`, `tr` and `de` choices survive.
Labels, accessibility names, numeric formatting and subsequent AI response
directives change in place. A running AI request retains its original language.
Read still preserves the source text's language. Installed Windows speech voices
remain a separate system capability; translations do not install new voices.

Android's outlined **Advanced settings** entry includes a decorative gear and
a localized accessible name. Its first section offers **System default** and
the native language names. Android 13+ persists the selection through
`LocaleManager` and exposes it in system per-app language settings; changing it
recreates the activity. Android 11–12 use a private preference and wrapped
Activity/Service contexts. The default follows the phone's language. AGP
generates the locale configuration from the packaged resources. Device behavior
on those older versions still needs acceptance testing.

Locale matching accepts regional variants and legacy `iw`/`in` tags. Portuguese
variants use the Brazilian catalog. An explicit Chinese script wins over its
territory: `zh-Hans-TW` selects Simplified, while `zh-Hant`, `zh-TW`, `zh-HK`
and `zh-MO` continue to the next supported preference. Traditional Chinese is
not offered as Simplified Chinese. Canonical saved codes retain `pt-BR` and
`zh-Hans`, rather than truncating them to primary subtags.

Arabic, Persian, Hebrew and Urdu select right-to-left UI direction. Camera
pixels and physical viewport coordinates are unchanged. Notes use logical
spacing and per-entry `lang`/`dir`, and Android navigation arrows follow layout
direction. The owner's centered headings and justified instructions remain.

The small native runtime launcher has its own compiled 25-language table and
uses Windows UI language before Qt starts. It cannot read the Qt app's saved
language preference at that point. Newly written notes contain all viewer and
handoff labels inline; existing notes are not rewritten. The browser tool uses
the notes' requested language or supported browser preferences.

## Coverage and provenance

Each of the 24 non-English Qt catalogs contains 728 complete source entries;
each Android locale contains the same 98 keys as English. The Transcribe UI
contains 57 keys per language. Qt catalogs, Android resources, launcher strings
and inline notes labels are packaged locally. No runtime translation API or new
third-party runtime dependency is introduced.

The 22 newly added translations are machine-assisted drafts of first-party
public UI text, generated during development with Google's translation service.
No credentials, recordings or private user text were submitted. Source-key,
placeholder and markup checks plus focused wording corrections establish
structural coverage, not fluent translation quality. Native-speaker review,
font coverage, screen readers and narrow/large-type layouts remain required.

### Multi-model naturalness review and correction pass, 2026-10-14

53 read-only reviewer agents (25 GPT-6-Luna and 25 Claude Haiku 5.5, one pair
per language, plus 3 Gemini 3.8 Flash cross-checks for Android, the website
and the Windows executable) audited every surface above for native
naturalness. The reviewers did not edit files; the corrections were applied
by the coordinating developer agent from their reports. The review found
systematic meaning-changing machine-translation errors in the new drafts
(typical surface scores 4–8/10), driven by ambiguous English source words.
Corrections (roughly 800 strings) cover: Android setup strings whose meaning
was inverted or lost (“Starts off” rendered as “Begins”, declining a
permission rendered as a physical fall in 20 locales, “Build number”
rendered as construction, stabilization rendered as political stability or
an eBook edition); Qt polysemy (Draw as a sports tie/lottery, Record as a
log/inscription, Export as cargo, crop as a farm harvest, speech as an
oration, transcript as a school report card, the collapsed accessibility
state as a building collapse, the image writer as a human author, the
Ukrainian runtime error reading “Wednesday of execution”); register errors
(rude or informal imperatives); notes/Transcribe label errors of the same
families; and website calques, applying the owner's rule: where a literal
translation sounds unnatural, use a natural local phrasing or the
established English term, never a word-for-word calque (for example Turkish
“yapay zekâ destekli erişilebilirlik” replaces “etmen tabanlı
erişilebilirlik” on the homepage).

This pass fixes the reviewers' highest-severity findings and the shared
polysemy patterns; it is not exhaustive line-by-line proofreading, and the
translations remain machine-assisted drafts pending native-speaker review.
Known structural leftovers recorded by the reviewers: Qt strings have no
plural forms (`%n`) for inflected count grammars, some guidance strings
embed button names as literal text, `Zoom %L1 times`-style English source
wording limits natural translation, and the website's “legally blind”
positioning is kept as the owner's wording. Local validation after the
pass: `scripts/check_translations.ps1` passed (24 catalogs × 728 entries;
French “Application” added to its identical-cognate allowlist),
`scripts/build_android.ps1 -Lint` passed (resource parity, unit tests and
lint), and website key/placeholder parity was verified locally; the
website's `npm run check` was not run because Node.js is not installed on
the current PATH. No build of the Windows application was run for these
string-only changes.

## Validation

Run `scripts/check_translations.ps1` for exact source keys, complete translations,
format tokens, line breaks and compiled catalogs. Use `-UpdateManifest` only
after deliberate source-key changes. `scripts/build_android.ps1 -Lint` checks
all Android keys and assembles/tests/lints the APK.

`language_catalog_contract` loads every real compiled catalog without creating
a window, checks locale/direction and saved/system precedence, and verifies that
a missing replacement catalog preserves the active translator, locale and
direction. Android `AppLanguageTest` checks tags and regional/script matching.
The website's `npm run check` includes `test-transcribe-languages.mjs` for all
UI keys, formatting tokens and locale selection.

Use `scripts/agent_build.bat` for normal Windows validation. When the desktop is
reserved, `OKUFLOW_AGENT_EXCLUDE_GUI=1` compiles the shipping release and runs
CPU/CUDA tests without native GUI fixtures. That result is partial validation;
it does not establish the complete publication gate or actual device rendering.

### Local results, 2026-10-13

- Shipping Windows release compilation passed. The background matrix passed
  25 CPU and 30 CUDA CTest entries, with zero skipped Qt rows. Native `gui`
  entries were excluded to respect the reserved desktop; this is partial
  validation, not a publication gate.
- Android debug assembly, 67 JVM tests and lint passed. The host protocol's
  11 standard-library tests passed without claiming the live adb server.
- Qt validation compiled all 24 non-English catalogs, each with 726 complete
  entries. Website checks passed 14 marketing pages and 25 Transcribe UI tables.
- The tracked headless Chrome fixture passed notes media, filtering,
  conversation grouping, theme/text-size persistence, EN/TR/DE/NL/AR controls,
  Arabic direction and 200% text at 320px. It also passed production CSP,
  variable-count carousel navigation, reduced motion and static icon caching.
  This used synthetic fixture media, not real recordings or model inference.

Logs and private profiles are under `%LOCALAPPDATA%\OkuFlow\language-work\`.
The APK is under `android-multilingual-build\app\outputs\apk\debug\app-debug.apk`
and the compiled Qt executable under `build-multilingual-release\cmake\oku_flow.exe`,
both relative to `%LOCALAPPDATA%\OkuFlow\`. These are development outputs, not
a staged portable bundle or Play upload. Their SHA-256 identities are:

| Artifact | SHA-256 |
| --- | --- |
| Debug APK | `B6B9BC78901BCA22917EF22D3BE970A30BF9BCF0A29C5A138058BA3F0B5F9358` |
| Compiled Qt EXE | `A1DECA56594058A3EAA0D67A7829EE615625824CEBDE5A71708F68AD02B557CB` |

The first compiled-catalog run caught the Chinese-script lookup bug; it was
repaired before the passing runs. A notes attribute-order check was preserved
when adding per-entry direction. One background attempt was invalidated by a
concurrent edit to test documentation; subsequent runs used settled inputs.
The Chrome fixture's obsolete four-slide expectations were replaced with
one-dot-per-slide assertions in both the default and localized-page checks.
Initial failed evidence remains retained separately from passing attempts.

The subsequent cable-screen wording update explicitly names the required
Windows app, its download website and the data-capable USB cable. All 25
Android catalogs and the three-language illustrative draft were updated;
debug assembly, JVM tests and lint passed again. The APK hash above includes
that wording. Native phone rendering and Play publication remain pending.
The Qt EXE hash reflects the subsequent nested-bundle data-folder protection.

The later Settings-navigation fix reuses the translated Developer-mode and
USB-debugging labels in two independent connection-help controls; no new
catalog keys were needed. Debug assembly, 68 JVM tests and lint passed. That
newer debug APK is `6981E0D3A186AB9E97A79317DFE6B920DBAF837839A2D7FB5913C426FCE4BA09`;
the earlier table remains the preceding wording/localization artifact. No
device was listed at that earlier build checkpoint. A later isolated
production-component fixture verified both Settings destinations on the Pixel
without replacing its Play installation; see `android/DEVICE_VALIDATION.md`.
Two new desktop USB recovery messages bring current Qt coverage to 728 keys.
The debug AAB's actual BundleConfig metadata also disables LANGUAGE splitting,
so manual language choices remain installed offline. This is debug-bundle
evidence, not a Google Play rollout or native-speaker acceptance.
