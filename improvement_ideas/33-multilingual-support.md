# 33 — Multilingual Support (English / Türkçe / Deutsch, live-switchable)

**Status: IMPLEMENTED — automated gates green; owner language and screen-reader
review remains.**
**Owner directive (2026-07-31):** full multilingual support — not just UI
labels but AI prompts and the accessibility layer (accessible names,
announcements, live status). Live-switchable at runtime, no restart. Launch
languages: **English, Turkish, German**. The language picker shows **flags**
alongside the language names.

**Implementation result (2026-07-31):** the app now ships embedded Turkish and
German Qt catalogs with 616/616 completed entries each, a persisted
live-switchable language manager, translated dynamic/accessibility text,
locale-aware display values, AI response-language directives, matching TTS
voice preference, and a catalog-integrity gate. The tracked Windows gate passes
release compilation, CPU 11/11, and CUDA 13/13. Remaining owner work is a
Turkish wording review, German native-speaker review, and live NVDA/Narrator
verification.

**Sequencing:** builds on **plan 32** (live accessible status), which is
already implemented (VERIFY — only the owner NVDA/Narrator pass remains).
Plan 32 refactored dynamic text so it derives from state through one choke
point (`SetLiveText`). That refactor is exactly what makes live language
switching of dynamic text cheap: re-run the derivation, get the new
language. P2 of this plan plugs into those derivation owners directly.

---

## 1. Current state (audited 2026-07-31, not assumed)

- **Zero i18n infrastructure.** No `tr()` call anywhere in `src/` or
  `include/` (verified by grep). No `QTranslator`, no `.ts` files, no
  `lupdate`/`lrelease` in any CMake file. Every user-visible string is a raw
  `QStringLiteral` in English.
- **Qt 6.9.3** (verified against the installed `Qt6Core.dll`). Modern
  `qt_add_translations()` CMake integration is fully available, including
  auto-generated `update_translations` / `release_translations` targets and
  resource embedding.
- **String volume** (user-visible literals, rough scale):
  `src/ui/main_window.cpp` alone is 3,106 lines with ~200
  `QStringLiteral`/`QString` sites; add `app_controls.cpp` (1,129 lines),
  `ui_state_manager.cpp` (446 lines), `recording_manager.cpp`,
  `app_assistant.cpp`, `setup_assistant.cpp`, `ai_settings_dialog.cpp`,
  `assistive_runtime.cpp`, and the overlay widgets. Total is in the
  high hundreds of strings.
- **84 accessibility call sites** (`setAccessibleName`,
  `setAccessibleDescription`, `QAccessibleAnnouncementEvent`) across `src/`.
  All English, all in scope.
- **AI prompts are hardcoded English** in
  `src/common/assistive_runtime.cpp:1121` and `:1148`
  ("Describe the visible scene briefly for a low-vision user…"). The
  user-editable `assistantInstructions` rides through
  `settings_store.cpp:467/:499` and is injected as a system message at
  `assistive_runtime.cpp:274-278`.
- **Settings are a custom JSON store** (`settings_store.cpp`,
  `settings.json` under the user data dir) — *not* QSettings. The language
  choice persists there.
- **TTS voice enumeration is already locale-aware**:
  `ai_settings_dialog.cpp:47-50` reads `voice.locale()` and formats
  language/territory names. That is the hook for voice-follows-language.
- **One live Turkish-casing hazard**: `ai_settings_dialog.cpp:73`
  capitalizes a label with `label[0].toUpper()` (QChar Unicode default).
  Turkish `i` must uppercase to `İ` (dotted), which QChar's default does not
  do. Programmatic capitalization of translated text is banned by this plan
  (D10).
- **Color scheme display names** (`color_schemes.cpp:137` "Custom colors",
  and friends) are user-visible; scheme **ids** (`"posterize"`, `"duotone"`,
  `color_schemes.cpp:212-216`) are serialization tokens and must never be
  localized.

## 2. Architecture decisions

### D1 — Qt Linguist stack, not a custom string table
Use the standard `tr()` → `.ts` (XML, UTF-8, LF) → `.qm` pipeline with
`qt_add_translations()`:
- `lupdate` extraction keeps the catalog honest mechanically — a custom table
  rots the first time someone adds a label and forgets the table.
- Free plural handling (`tr("%n frame(s)", nullptr, n)`) — German plural
  forms differ from English; Turkish has no plural agreement after numerals.
- Context + disambiguation comments are built in
  (`//: translator comment`, `tr("Stop", "recording button")`).
- `.qm` files embed into the executable via the resource system —
  **zero new deployment files**, `windeployqt` flow unchanged.

Files: `translations/openzoom_tr.ts`, `translations/openzoom_de.ts`
(English is the source language; no `openzoom_en.ts`). Both LF, UTF-8.

### D2 — Translation contexts
Strings inside `Q_OBJECT` classes use plain `tr()` (class-name context).
Free functions and non-QObject helpers use
`QCoreApplication::translate("OpenZoom", …)` with a small set of stable
context names (`"OpenZoom"`, `"Recording"`, `"Assistant"`). Never build a
sentence by concatenating translated fragments — word order differs in
German and Turkish; always use `%1`-style placeholders on a full sentence.

### D3 — LanguageManager and the live-switch mechanism
A small app-owned object (not a singleton), e.g.
`include/openzoom/app/language_manager.hpp`:

- `enum class AppLanguage { English, Turkish, German };`
- `SetLanguage(AppLanguage)`:
  1. `qApp->removeTranslator()` on the previous `QTranslator` (if any),
  2. `install` the new one loaded from `:/i18n/openzoom_<code>.qm`
     (English = no translator installed; source strings are the catalog),
  3. persist `"language": "tr"` to the settings store,
  4. Qt then delivers `QEvent::LanguageChange` to every widget — that event
     is the *only* fan-out mechanism; no custom signal spaghetti.
- First run: map `QLocale::system().language()` into the supported set
  (`Turkish → tr`, `German → de`, everything else → `en`).
- Also install the matching `QLocale` as default
  (`QLocale::setDefault`) so `%L1` number formatting follows
  (`1,5×` in German/Turkish vs `1.5×` in English) — **display only**; see D5.

Every hand-built widget/panel gains the standard pattern:

```cpp
void Retranslate();                 // sets every text/accessibleName it owns
void changeEvent(QEvent* e) override {
    if (e->type() == QEvent::LanguageChange) { Retranslate(); }
    QWidget::changeEvent(e);
}
```

The constructor builds structure, then calls `Retranslate()` once — so there
is exactly **one** place where each widget's texts are assigned, forever.
This is the bulk of the mechanical work in P1.

### D4 — Dynamic text re-derives from state (plan 32 synergy)
Plan 32 makes live status text a pure function of state pushed through
`SetLiveText`. On `LanguageChange`, the owning component re-runs the
state→text derivation and pushes again. Concretely: the GPU-pipeline status,
recording status line, camera status, assistant status re-emit their current
state's text in the new language. Historic/transient toasts that already
expired are *not* retroactively re-rendered (nothing to re-render into).
The most recent still-visible status line **is** re-derived if its state is
still known; otherwise it is left as-is — never show a stale-language string
as if it were fresh.

### D5 — The translation boundary (what is NEVER localized)
- **Debug/diagnostic logs** (`DebugLog`, `qCritical`, writer stage names,
  `DescribeWorkerStage` internals): English forever. The crash-forensics
  workflow and greppability depend on it.
- **User-visible failure wrappers** are translated, their technical payload
  is not: `tr("Recording failed: %1").arg(englishDetail)`. The owner reads
  the sentence; the agent greps the detail.
- **Serialization tokens**: settings.json keys and values (scheme ids,
  camera stable ids, effort tokens like `"low"`), recording folder/file
  names (`VID_*.mp4` — must stay ASCII and locale-independent), JSON numbers
  (QJsonDocument always uses `.` — do not "fix" that).
- Case-normalization of internal tokens (`.toLower()` on ids, hosts,
  efforts at `color_schemes.cpp:221`, `settings_store.cpp:745`, etc.) is
  correct as-is — those are tokens, not text. Leave them.

### D6 — AI prompt policy
- **Built-in prompts stay English** — instruction-following is most reliable
  in English — and gain one appended directive line selected by language:
  `"Respond in Turkish."` / `"Respond in German."` (nothing appended for
  English). One helper owns this:
  `AssistiveRuntime::AppendResponseLanguageDirective(QString prompt)`;
  both hardcoded prompt sites (`assistive_runtime.cpp:1121`, `:1148`) and
  the user-typed assistant prompt path (`SubmitAssistantPrompt`) route
  through it.
- **User-authored `assistantInstructions` are never modified or
  translated** — they are the user's own words. The response-language
  directive is appended as a *separate, final* system line so a user who
  explicitly writes "always answer in English" in their instructions wins
  (later user instruction beats our directive is acceptable; note this in
  the settings dialog help text).
- **Presentation labels around AI output** ("Text on screen", status lines
  like "Preparing the current view…", `assistive_runtime.cpp:753-765`,
  `:1158`, `:1168`, `:1316`, `:1335`) are UI strings — translate normally.
- No transcription/speech-to-text surface was added by this plan. Plan 36 now
  owns that feature and must extend the shipped language contract.

### D7 — TTS voice follows the app language
On language switch, if the active `QTextToSpeech` voice's
`voice.locale().language()` does not match the new app language, pick the
best installed voice that does (prefer exact territory, else any territory);
if none exists, **keep the current voice** and show a translated one-time
status notice: "No Turkish voice is installed — using the current voice.
Install one under Windows Settings → Time & Language → Speech." The AI
settings dialog voice combo (already locale-aware at
`ai_settings_dialog.cpp:47-50`) groups/sorts matching-language voices first.
TTS remains strictly user-triggered (standing rule) — the notice is a status
line, not speech. Screen-reader synthesizer language is NVDA/Narrator's own
setting and out of our control; our job ends at delivering correctly
translated accessible text.

### D8 — Language picker with flags
- **Windows ships no emoji flag glyphs** (Segoe UI Emoji deliberately
  renders flag emoji as letter pairs like "TR"). Emoji flags are therefore
  unusable — ship three small **SVG flag assets** in the Qt resource file
  (`:/flags/us.svg`, `:/flags/tr.svg`, `:/flags/de.svg`), rendered as
  `QIcon` so they stay crisp at any DPI/scale.
- Picker widget: a combo (reuse `WheelSafeComboBox`) in the **Advanced
  panel's settings/general section**, entries = flag icon + the language's
  **native name**: `English`, `Türkçe`, `Deutsch` (native names are the
  universal convention — a Turkish speaker lost in an English UI must be
  able to find their language).
- English uses the **USA flag** (owner decision 2026-07-31).
- Accessibility: the combo's `accessibleName` is
  `tr("Application language")`; each entry's accessible text is the native
  name (flags are decorative and carry no accessible text of their own).
  Selection applies **immediately** (live switch) — no OK/apply step — and
  the switch itself fires the plan-32 announcement path with the new
  language's "Language set to Türkçe" text, so a screen-reader user gets
  immediate confirmation the switch happened.
- Data model: entries carry the language **code** (`en`/`tr`/`de`) as
  `Qt::UserRole`; selection is restored by code, never by row index.

### D9 — Combo/list repopulation on switch
Any combo whose entries are translated (color scheme names, capture modes,
reasoning-effort labels, voice descriptions) is repopulated inside its
owner's `Retranslate()`, **preserving selection by stable id** (UserRole),
never by index. Combos whose entries are device data (camera names, format
lists) are not touched by translation — only their labels are.

### D10 — Casing and text-shaping rules
- **Never programmatically capitalize translated text.** Fix
  `ai_settings_dialog.cpp:73`: the effort labels become three translated
  strings (`tr("Low")`, `tr("Medium")`, `tr("High")`) selected by token,
  instead of capitalizing the token. Turkish dotted/dotless *i* makes
  `QChar::toUpper()` wrong (`istekli` → `Istekli` instead of `İstekli`),
  and QLocale-aware casing per string is more fragile than just writing the
  label correctly in the catalog.
- **German strings are long** (compounds; typical 30-40% growth). No label
  may be assigned a fixed pixel width to "fit English". The plan-18 R2
  squished-rails layout and the Advanced panel must be audited with the
  German catalog loaded (P5 gate) — eliding is acceptable for status lines,
  never for button/control labels.
- Ellipsis is `…` (single char) in all three catalogs, matching existing
  style ("Preparing the current view…").

## 3. Scope inventory (what gets wrapped where)

| Area | Files | Notes |
|---|---|---|
| Main window chrome, panels, buttons | `src/ui/main_window.cpp` | Largest single sweep (~200 literal sites). Split `Retranslate()` per panel to keep functions reviewable. |
| Control labels, slider rows, tooltips | `src/app/app_controls.cpp`, `src/ui/responsive_slider_row.cpp`, `src/ui/collapsible_section.cpp` | Slider value suffixes (`×`, `%`, `px`) are format strings with placeholders, not concatenation. |
| State/status text | `src/app/ui_state_manager.cpp`, plan-32 `SetLiveText` call sites | Re-derive on switch (D4). |
| Recording UX | `src/app/recording_manager.cpp` | Button states, "Stopping recording…", watchdog/abandon wrapper messages (D5 split: wrapper translated, stage detail English). |
| Assistant / AI | `src/app/app_assistant.cpp`, `src/common/assistive_runtime.cpp`, `src/app/assistive_feature_manager.cpp` | Status lines translated; prompts per D6. |
| Setup assistant | `src/app/setup_assistant.cpp` | Full walkthrough text. |
| AI settings dialog | `src/ui/ai_settings_dialog.cpp` | Includes D7 voice grouping + D10 casing fix. |
| Overlays | `src/ui/annotation_overlay.cpp`, `assistive_overlay.cpp`, `joystick_overlay.cpp` | Annotation tool names, hints, action labels (plan 18 R2 vocabulary). |
| Color schemes | `src/app/color_schemes.cpp`, `src/ui/color_scheme_picker.cpp` | Display names translated at presentation; ids untouched (D5). |
| Accessibility layer | all 84 `setAccessibleName`/`Description`/announcement sites | Every one moves inside a `Retranslate()` or a state-derivation so switch updates it; plan-32 `NameChanged`/announcement events then fire naturally. |
| Language picker itself | new widget + `LanguageManager` | D8. |

Out of scope for wrapping: `media_writer.cpp`, `media_capture.cpp`,
`cuda/`, `d3d12/` — core layers keep English strings (they surface only
through D5 wrappers or logs).

## 4. Phases

### P0 — Foundation (mechanism proves itself on one dialog)
1. `LanguageManager` (D3) + `"language"` key in `settings_store.cpp`
   (default from system locale, D3) + startup install before main window
   construction.
2. CMake: `find_package(Qt6 … LinguistTools)`, `qt_add_translations()` with
   the two `.ts` files, `.qm` embedded under `:/i18n/`. Verify
   `update_translations` target runs from the pwsh build bridge and that a
   no-op run leaves `.ts` files byte-identical (LF, stable ordering — add
   `-locations none` so line-number churn doesn't dirty the diff).
3. Flag SVGs into resources; language picker combo (D8) wired to
   `LanguageManager`.
4. Pilot conversion: `ai_settings_dialog.cpp` fully converted to the
   `Retranslate()` pattern (includes the D10 casing fix) with a handful of
   real Turkish/German strings in the catalogs.
   **Gate:** switching language live retitles the pilot dialog with no
   restart, no focus loss, and NVDA reads the new combo `accessibleName`.

### P1 — Static UI sweep
Convert every file in the §3 inventory to `tr()` + `Retranslate()`;
combos repopulate per D9; all 84 accessibility sites included. lupdate runs
clean (no "cannot invoke tr() like this" warnings — those indicate
concatenation or non-literal usage that must be restructured).
**Gate:** with the German catalog loaded, a full manual pass of every
panel shows no truncated control labels (D10).

### P2 — Dynamic/state text (after plan 32 is merged)
Wire `LanguageChange` into the plan-32 derivation owners so live status,
GPU-ready text, camera state, recording state re-emit in the new language
(D4). Recording wrapper/detail split per D5.
**Gate:** offscreen test — set state "GPU pipeline ready", switch to
Turkish, assert the live text widget's text AND `accessibleName` changed
and a `NameChanged` accessibility event fired
(`QAccessible::installUpdateHandler`, same harness as plan 32 tests).

### P3 — AI prompts + TTS
`AppendResponseLanguageDirective` (D6) on all three prompt paths; settings
dialog help text notes the interaction with custom instructions; TTS voice
follows language with the missing-voice notice (D7).
**Gate:** unit test asserts the composed request body ends with the
directive for tr/de and not for en; manual Codex round-trip in Turkish
returns Turkish prose.

### P4 — Catalog authoring and review
Agent authors complete `openzoom_tr.ts` and `openzoom_de.ts`. Before bulk
translation, fix a short **glossary** for consistency and put it at the top
of this plan when decided — proposed starting points, owner reviews the
Turkish column personally:

| English | Türkçe (proposed) | Deutsch (proposed) |
|---|---|---|
| Zoom | Yakınlaştırma | Zoom |
| Recording | Kayıt | Aufnahme |
| Stabilization | Sabitleme | Stabilisierung |
| Camera | Kamera | Kamera |
| Screen reader | Ekran okuyucu | Screenreader |
| Advanced panel | Gelişmiş panel | Erweitertes Panel |
| Snapshot/photo | Fotoğraf | Foto |

German ships marked as machine-authored pending a native-speaker pass
(track as an open item, not a blocker — a good German catalog with three
awkward phrasings still beats an English-only UI for a German student).
**Gate:** `lrelease` reports 0 unfinished/0 untranslated for both catalogs.

### P5 — Tests, gates, docs
1. **Catalog smoke test** (CPU suite, offscreen QPA): load each embedded
   `.qm`, assert success, spot-assert 3-4 sentinel strings per language.
2. **Live-switch test**: P2's gate test, registered in ctest
   (LABELS gui), SKIP_RETURN_CODE 77 where a QPA is unavailable.
3. **Catalog-drift gate**: CI/red-gate step runs `lupdate`; fails if it
   introduces new entries (a developer added a string without updating
   catalogs). This is the mechanical guard that keeps the catalogs honest
   after this plan closes.
4. **Casing sweep**: grep gate asserting no `.toUpper()`/`.toLower()` on a
   `tr(`-derived value (tokens per D5 are exempt by construction since they
   never pass through tr()).
5. Docs: `docs/code_reference.md` entries for `LanguageManager` and the
   `Retranslate()` convention; CHANGELOG; this plan's status updates.
6. Line-endings check on every touched file (`grep -c $'\r'` = 0); `.ts`
   files LF.

## 5. Hazards and edge cases (write tests or notes, don't discover in prod)

- **Concatenated sentences hiding in the sweep** — any `+` joining two
  translated pieces or a literal to a value is a bug (D2); lupdate warnings
  are the tripwire.
- **Placeholder collisions**: `%1` inside a string that also prints a
  literal percent — use `%%`-free Qt style (`arg()` only replaces `%N`);
  audit strings containing `%` during the sweep (slider percents).
- **Index-based combo restore after repopulation** (D9) — restoring by
  index silently selects the wrong scheme when order differs per language
  (alphabetical resorting is forbidden for exactly this reason: keep
  catalog order identical across languages).
- **Language switch mid-recording / mid-AI-turn**: switch must not touch
  the worker threads — all translation happens on the UI thread at
  presentation. In-flight AI turns complete in the old response language;
  the *next* turn follows the new setting. State this in the picker's
  tooltip; do not try to cancel/retarget in-flight turns.
- **`QLocale::setDefault` side effects**: audit for any parsing (not just
  formatting) of user-visible numbers — parsing of internal data must use
  `QLocale::c()` explicitly. Known-safe: settings JSON (QJsonDocument),
  ids, filenames (D5).
- **Turkish `İ/ı` in comparisons**: any case-insensitive compare on
  *user-visible* text must be `QString::compare(…, Qt::CaseInsensitive)`
  on tokens only, never on translated text (searching/filtering voice
  names in the AI dialog is the one current site — verify).
- **Flag assets**: SVGs must be self-made or public-domain
  (license-clean, GPL-compatible — flag designs themselves are public
  domain; use simple geometric renderings, no third-party icon packs with
  attribution requirements).
- **Catalog file endings**: `.ts` is XML — some editors normalize to CRLF;
  the P5 line-endings gate covers them explicitly (standing repo rule:
  never blanket-normalize).

## 6. Explicit non-goals

- No right-to-left language catalog ships in this plan. The shared descriptor
  registry now carries locale and layout direction; `LanguageManager` applies
  it globally; manually positioned Simple chrome and annotation rails mirror
  through logical leading/trailing anchors; directional history/navigation
  icons refresh; and `--rtl-test` / `OPENZOOM_FORCE_RTL=1` exercise the path
  with English text. Adding a fourth LTR language later remains one `.ts`
  file, flag, descriptor/enum entry, and response-language directive. Qt's
  Unicode text and Linguist catalogs support CJK without a new translation
  architecture. Arabic and other RTL languages can use the same plumbing, but
  native wording, typography/line breaking, and NVDA/Narrator review remain
  release gates for every added language.
- No locale-dependent file naming, folder naming, or settings encoding.
- No translation of debug logs, crash forensics output, or stage telemetry
  (D5).
- No speech-to-text implementation inside this multilingual plan. Plan 36 owns
  live recording transcription and its English/Türkçe/Deutsch additions.
- No per-string language tagging for screen-reader synth switching (UIA
  culture plumbing is not reliably consumed by NVDA per-property; out of
  scope).
