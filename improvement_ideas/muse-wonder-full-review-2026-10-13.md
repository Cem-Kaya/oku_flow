# Muse Wonder Full review — OkuFlow (2026-10-13)

Status: **READ-ONLY REVIEW COMPLETE — recommendations only, nothing implemented.**
Reviewer: Muse Code powered by muse-wonder-full-1.0, acting as review sub-agent.
This is the only file this review created. No production code, translations,
website assets, build scripts, tests, or dependencies were changed; no index
was updated; nothing was committed or published.

## Snapshot and method

- Windows/Android repo `oku_flow`, branch `main`, HEAD `d66a60f`, plus a large
  uncommitted working tree (109 dirty/untracked entries at snapshot time
  2026-10-13 18:52 +03:00, still changing during the review). Findings are
  tagged **[shipped]** (in published v0.2.3 / committed code), **[working
  tree]** (uncommitted safeguards), or **[in-progress]** (the concurrent
  25-language / gear work, which may have moved on by the time this is read).
- Website repo `okuflow-website`, clean tree, HEAD `1bc95fd` (v0.2.3 downloads).
- Read first, per each repo's rules: `AGENTS.md`, `README.md`,
  `docs/developer_guide.md`, `docs/README.md`, `docs/hardcoded_paths.md`,
  `docs/ui_modes_design.md`, `android/README.md`, `android/PROTOCOL.md`,
  `android/DEVICE_VALIDATION.md`, website `README.md` and
  `docs/browser-transcription.md`, plus `docs/windows_crash_validation_2026-10-13.md`.
- Evidence labels: **Observed** = read in source, seen in an existing capture,
  or fetched live this session. **Inference** = control-flow consequence that
  was not executed. Per the owner's constraint, nothing was launched, no GUI
  fixtures were run, no APK was installed, and the phone and desktop were not
  operated. Visual claims use only the website's existing published captures,
  which are labeled **stale** where they predate a change. There is **no
  current visual verification** of the Windows app, Android app, or live
  website layout in this review.
- One live check was made without touching the desktop: the public GitHub API
  for release `v0.2.3`, fetched 2026-10-13, used only to resolve which
  download asset names exist (finding DOC-1).
- Line numbers are navigation hints for this snapshot; re-locate by symbol
  after subsequent edits.

## What already works (preserve these contracts)

- **Ownership/deadline discipline is unusually thorough.** Shared D3D12/CUDA
  fence timeline with reserved/committed/cancelled signals, retained-frame
  GPU readback with identity checks (`media_capture.cpp:1136-1224`), sticky
  recorder abandonment that correctly suppresses `MFShutdown`
  (`app_bootstrap.cpp:1223-1243`), bounded recording admission, periodic
  disk-space re-checks during recording (`media_writer.cpp:1282-1297`), and
  paired-photo transactions with startup recovery. The new
  `PhoneFrameProgress` correctly separates packet arrival from decoder
  progress, and reconfigurations do not renew its deadlines.
- **Intentional behaviors confirmed in code, not findings:** two encoded
  recordings (original + processed), AV1-then-H.264 together; HEVC-preferred
  phone streaming with one AVC reconnect (session-only, preference kept);
  torch forced off on every phone bind (`phone_camera.cpp:744`,
  Android `forNewHostSession`); saved explicit webcam choice survives
  discovery (`cameraSelectionExplicit`); phone discovery derives from USB
  transport/capability with no serial/model constants; transcription has no
  silent CPU/smaller-model fallback and locks settings during a job;
  first-launch phone preference and fallback-webcam-then-phone recovery
  (but see WIN-UX-1 for its startup side effect).
- **Accessibility groundwork is real:** large labeled Simple controls,
  three-tab Advanced ownership, live status announcements, Android one-action
  panels with pane titles and haptics, 44 px+ targets in notes/website,
  forced-colors support in notes/website/transcribe CSS, reduced-motion
  handling in the carousel and notes.
- **Validation recorded by the project (not re-run here):** complete
  CPU 79/79 + CUDA 84/84 after the SuperRes scale fix; then, after the
  diagnostic-duration change, CPU 79/79 and CUDA 78/84 with three explicit
  desktop interruptions and three unresolved assertions while the owner used
  the PC (`docs/windows_crash_validation_2026-10-13.md:178-187`). Those six
  remain **not passed** until a serial rerun. One live Pixel cable cycle
  passed (same doc, 18:30 follow-up); that is one device/cycle, not a soak.

## Top five next improvements

1. Fix the broken README download links (DOC-1) and add a release-time
   cross-repo link/asset check so README, website, and GitHub assets cannot
   drift again.
2. Make language switching fail safe (I18N-2) before the 25-language work
   ships: load-then-swap translators, and never show a selected language the
   UI is not actually using. Gate the feature on catalogs actually existing
   (I18N-1).
3. Replace the stale Advanced screenshot (WEB-1) and re-capture the set from
   the current build under `docs/ui_capture.md` rules.
4. Drop stale-generation phone packets instead of parking them (CAP-1), and
   add the missing Media Foundation post-first-frame stall watchdog (CAP-2)
   so every camera class has a progress deadline.
5. Let interrupted transcriptions keep their partial text (NOTE-1): enable
   Save-as-text and write an explicitly partial sidecar on cancel/close.

## Ranked findings

### DOC-1 — Published README download links point to non-existent assets — High [shipped]

- **Evidence (Observed):** `README.md:13` and `README.md:69` link to
  `releases/download/v0.2.3/OkuFlow-0.2.2-x64.msi` and
  `OkuFlow-0.2.2-windows-x64.zip`. The website links the same tag with
  `0.2.3` asset names (`okuflow-website/src/index.html:350-351`). The live
  GitHub API for tag `v0.2.3` (fetched this session) lists these two downloads:
  `OkuFlow-0.2.3-x64.msi` and `OkuFlow-0.2.3-windows-x64.zip`, and the ZIP
  digest matches the website README's published SHA-256. So the website is
  right and the app README's two primary download links 404.

  Parent rechecked the API and all four URLs: the two old filenames return
  HTTP 404; the two v0.2.3 filenames return HTTP 200. The API also lists
  `SHA256SUMS.txt`. Both README link locations are corrected in the local
  working tree with a changelog entry; this documentation fix is not pushed.
- **Trigger/impact:** A GitHub visitor clicks "Download MSI" at the top of
  the README — the first call to action — and gets a 404. For a low-vision
  user arriving from search, this reads as "the download is broken."
- **Minimal fix:** Change both README asset names to the `0.2.3` forms.
  Add a release checklist step (or script assertion) that README, website
  `src/index.html`, and the GitHub release asset list share version and
  asset names; the website already validates its own consistency, the app
  README has no equivalent gate.
- **Validation:** `curl -I` both corrected URLs (expect 200/302 to a signed
  asset URL) and keep the website `npm run check` version-parity rule.

### I18N-1 — The 25-language build currently references files that do not exist — High [in-progress]

- **Evidence (Observed at snapshot):** `cmake/CMakeLists.txt:171-196` lists
  24 `.ts` catalogs, but `translations/` contains only `okuflow_tr.ts` and
  `okuflow_de.ts`. `src/launcher/main.cpp:3` includes `launcher_text.hpp`,
  which exists nowhere in the tree (searched). Android has only `values/`,
  `values-de/`, `values-tr/` while `AppLanguage.kt` already offers 25 picker
  entries. `scripts/check_translations.ps1` now requires all 24 catalogs, so
  it must currently fail at the first missing file.
- **Impact:** None yet for users — this is the known concurrent work — but
  the tree cannot configure/build or pass the translation gate in this
  state, so any unrelated validation run against this snapshot will fail for
  non-obvious reasons. The crash-validation doc already notes the
  localization work "requires its own gate."
- **Minimal fix:** None for this review to apply; for the implementing
  agent: land catalogs/header before (or atomically with) their references,
  and treat "picker offers a language ⇒ catalog embedded and sentinel-tested"
  as the merge gate.
- **Validation:** `scripts/check_translations.ps1`, Android parity check in
  `build_android.ps1`, and a configure of the release preset.

### I18N-2 — A failed language load leaves the app with no translator but the old language selected — Medium-High [shipped path, amplified by in-progress work]

- **Evidence (Observed in source):** `LanguageManager::SetLanguage`
  (`src/app/language_manager.cpp:55-68`) removes the installed translator
  **before** loading the new catalog. On load failure it returns `false`
  with `translatorInstalled_ = false` but `language_` unchanged. The caller
  (`app_bootstrap.cpp:160-172`) reverts the combo to the previous setting
  and shows "could not be loaded" — but the strings are now retranslated
  with **no** translator (English source text), `languageCode()` still
  reports the old language to the assistant/TTS, and re-selecting the same
  combo entry will not fire a change to repair it. At startup
  (`app_bootstrap.cpp:66-70, 140-145`) a failed initial load falls back to
  English while the combo still displays the saved language ("system" or an
  explicit code), with no notice.
- **Trigger:** Any missing/corrupt `.qm` — exactly the state of 22 languages
  in I18N-1 — or a future partial bundle. A Turkish user tries French, gets
  an English UI that claims to be Turkish, and AI answers keep the old
  response language while the visible UI disagrees.
- **Minimal fix:** Load the candidate into a temporary `QTranslator` first;
  only remove/install on success. On failure keep the old translator and
  locale untouched. At startup, if the saved/system language fails to load,
  select the fallback in the combo too and queue a status notice.
- **Validation:** Extend `language_manager_tests.cpp` with a
  failure-path case (request a language whose catalog is absent in the test
  build, assert prior strings/locale/`languageCode()` survive) — currently
  the suite only exercises successful en/tr/de switches, and the new
  mapping helpers (`AppLanguageFromLocaleTag`, `ResolveLanguagePreference`)
  have no tests yet. Add table-driven cases: `zh-Hant` rejected (not mapped
  to Simplified), `zh-CN` accepted, legacy `iw`/`in`, `pt-PT → pt-BR`,
  unknown → English/system, and `QLocale("zh-CN").script()` behavior, which
  this review could not execute.

### WEB-1 — The published Advanced screenshot shows the removed Transcript tab — Medium-High [shipped]

- **Evidence (Observed):** `public/media/okuflow-advanced-13b63c54fa43.webp`
  (used on the homepage carousel, the tour, and mirrored in the app README)
  shows four inspector tabs: Image, Assistant, **Transcript**, Settings.
  Both current source and the v0.2.3 tag commit create exactly three
  (`main_window.cpp:1622-1624` at HEAD and at `6de0a1e`). The captures date
  from 10 October (`docs/media-provenance.md`), before the 2026-10-12
  transcription removal, so this is a **stale capture**, not a mockup.
- **Impact:** The site's own copy says speech processing happens in the
  browser, while its main product image advertises an in-app Transcript tab
  that no download contains. First-time users will hunt for a tab that does
  not exist; reviewers may read it as a feature regression.
- **Minimal fix:** Re-capture the Advanced view (and re-check the other
  four) from the current build via the tracked capture workflow, rename by
  content hash, and update references in both repos. Until then, the
  provenance doc should note the tab discrepancy.
- **Validation:** Visual check of the new capture's tab bar against
  `docs/ui_modes_design.md`'s three-tab contract; `npm run check` for hashes.

### CAP-1 — A stale-generation phone packet is parked until the 12-second stall deadline — Medium [working tree + shipped protocol]

- **Evidence (Observed control flow; consequence is Inference):** Windows
  keeps at most four `configured` generations (`phone_camera.cpp:782-784`,
  evicting the smallest). A video packet whose generation is not in that map
  is stored in `pending` and retried forever (`phone_camera.cpp:816`);
  `PacketReceived` is only called after a generation match (line 846), so
  the parked packet ages `lastPacket_` until `PhoneFrameProgress` declares
  `TransportStalled` at 12 s (`phone_frame_progress.hpp:32-33`) and the
  whole stream reconnects. Android legitimately produces this pattern: up
  to six startup candidates each emit `configured` and can emit CONFIG
  packets (`CameraEngine.kt:76-92, 125-131`) before failing, and control and
  video travel on independent sockets, so a CONFIG packet from generation
  *n* can be read after generations *n+1…n+5* have already evicted it.
- **Trigger/impact:** Encoder-candidate fallback, rapid resolution/codec
  changes, or a busy Windows reader during reconfiguration. Instead of
  dropping one obsolete packet, the user sees up to ~12 s of frozen picture
  followed by a full reconnect (with webcam fallback churn), even though a
  healthy newer generation is already available.
- **Minimal fix:** Only park a packet when its generation is **newer** than
  every known generation (control channel still catching up). If a newer
  generation is already known, drop the stale packet, reset sequence
  expectation for the incoming generation, and count the drop in
  diagnostics. Keep the 12 s deadlines for genuine stalls.
- **Validation:** Extend `phone_camera_tests.cpp` (fake-adb fixture) with
  out-of-order configured/CONFIG delivery across ≥5 generations; assert the
  newest generation decodes without a stall error or reconnect.

### CAP-2 — Media Foundation webcams have no post-first-frame stall watchdog — Medium [shipped]

- **Evidence (Observed):** The phone path now has independent 12 s
  packet/decoder deadlines and the presenter has a 1 s fence-progress
  watchdog, but no frame-arrival deadline exists for ordinary MF capture
  (searched `src/app`, `src/capture` for stall/watchdog logic; device loss
  is event-driven via `deviceLost_`). The design doc already admits the UI
  side: the camera-state placeholder "does not yet detect a camera that
  stalls after its first frame" (`docs/ui_modes_design.md`, placeholder
  paragraph).
- **Trigger/impact:** A wedged USB camera/driver that stops delivering
  samples without a device-lost event leaves the last frame on screen
  indefinitely — indistinguishable from a still board — and a recording
  keeps running with no new video. This is the one camera class without the
  "eventual convergence" guarantee the other paths now have.
- **Minimal fix:** Track ingress arrival time per session (capture thread,
  not presentation, so occlusion and Extra Stable retention do not count);
  after a generous bound (e.g. max(3 s, several frame periods at the
  negotiated rate)), raise the existing device-lost/reconnect path and show
  the existing reconnecting status. Exempt intentional stops and format
  changes already in recovery.
- **Validation:** Fake/session fixture that delivers frames then silence;
  assert one reconnect begins and no modal appears. Real-camera check by
  pausing a virtual-camera feed (existing lecture harness) without
  unplugging.

### WIN-UX-1 — A saved phone preference still opens the fallback webcam first at every startup — Medium-Low [shipped]

- **Evidence (Observed + owner's own test):** At bootstrap, cameras come
  from MF enumeration only; a saved `okuflow-phone:` link is absent, so
  `ResolveSavedCameraIndex` returns 0 (`media_capture_facade.cpp:102-104`)
  and `app_bootstrap.cpp:1157-1158` immediately starts that webcam. Phone
  discovery (5 s cadence) later switches to the phone
  (`app_pipeline_runtime.cpp:209-230`). The validation doc records exactly
  this behavior live: temporary 1280×720 Windows-camera capture, then the
  phone. The "Waiting for phone…" placeholder already exists for the
  no-camera case.
- **Impact:** The room webcam opens (indicator light, a few seconds of the
  wrong view) for a user who explicitly chose their phone; a fast Record
  press can start a recording on the wrong source; stabilization/SuperRes
  state initializes twice. It also makes phone startup look flaky.
- **Minimal fix:** When the saved link starts with `okuflow-phone:`, do not
  auto-start the fallback webcam at bootstrap. Show the existing waiting
  placeholder, run the first discovery immediately (not after 5 s), and
  start the fallback only after a bounded wait (e.g. first discovery
  snapshot reports the phone absent) or on explicit user pick — preserving
  today's recovery when the phone truly is gone.
- **Validation:** Startup-profile fixture with saved phone link and the
  phone present: assert first presented frame is the phone's. Repeat with
  phone absent: assert fallback starts once, after the bound, and the
  placeholder text was shown.

### GPU-1 — The new fence-progress watchdog can latch a terminal fault across sleep/resume — Medium-Low, conditional [working tree]

- **Evidence (Observed + Inference):** `FenceProgressWatchdog::Observe`
  (`fence_wait.hpp:16-41`) latches `TimedOut` permanently after 1000 ms
  without completed-value progress, using `GetTickCount64()`
  (`presenter.cpp` `PollGpuProgress`), which counts suspended time. It is
  polled on submissions and idle app ticks
  (`app_pipeline_runtime.cpp:442`). Inference: if the machine sleeps with a
  fence value outstanding, the first poll after resume can observe
  `completed < submitted` with a multi-second `nowMs` jump and latch
  "restart OkuFlow" before the GPU drains work that was merely suspended.
  The completed-first check narrows the window but does not close it,
  because the poll races the post-resume drain.
- **Impact:** Rare but severe-feeling: a healthy laptop resume in a lecture
  could require an app restart. Severity is bounded by how often a fence is
  outstanding across suspend.
- **Minimal fix:** Use a suspend-excluding clock for this watchdog
  (`QueryUnbiasedInterruptTimePrecise`), or treat the first observation
  after a large `nowMs` discontinuity as a fresh progress window and only
  latch if the *next* poll still shows no progress. Keep stickiness for
  genuine stalls.
- **Validation:** Unit: `Observe(10, 9, 0)` then `Observe(10, 9, 60000)`
  then `Observe(10, 10, 60050)` should stay healthy under the new rule,
  while the existing no-progress cases in `fence_wait_tests.cpp` still
  time out. Manual: sleep/resume during an active session on a test laptop.

### NOTE-1 — An interrupted transcription cannot be saved from the notes page — Medium [shipped]

- **Evidence (Observed):** The notes bridge writes the transcript sidecar
  only on the tool's `done` message
  (`notes_transcribe_bridge.hpp:148-160`) and unhides **Save as text** only
  on `done` or a restored sidecar (lines 104-105, 151). Streamed segments
  are shown live (line 141) but, if the tool tab is closed, stopped, or
  errors at 80% of a long lecture, `finish()` leaves them visible yet
  unexportable and unpersisted; reloading the notes loses them entirely.
  The tool side has the same shape: results are cached in-page only.
- **Impact:** The longest, most valuable transcriptions are the most likely
  to be interrupted (tab close, sleep, crash), and today that loses all
  completed work even though both pages already hold the segments.
- **Minimal fix:** In the bridge, enable Save-as-text whenever
  `card.segments.length > 0`, and on cancel/error/tab-closed write the
  sidecar with an explicit `complete: false` (restore accepts it and labels
  it partial, offering Transcribe again to replace it). No protocol change
  needed; segments already stream.
- **Validation:** Extend the existing Chrome fixture path: stop mid-job,
  assert the partial sidecar exists, reload, assert partial text restores
  with its label and a full re-run overwrites it.

### NOTE-2 — Notes folder resolution depends on the folder's name appearing in the path — Medium-Low [shipped]

- **Evidence (Observed):** `recording()` in
  `notes_transcribe_bridge.hpp:52-66` locates the granted root inside the
  recording URL via `parts.lastIndexOf(root.name)`, and any non-abort error
  in `begin()` clears the remembered root for the session (line 176). If
  the data-root name repeats deeper in the path, or the user grants a
  parent/nearby folder whose name also occurs elsewhere in the path, the
  computed relative path is wrong, the lookup throws, the root is dropped,
  and the user is asked to grant again — a loop with no explanation. (Two
  label strings, `folder` and `restore`, are also defined but never used.)
- **Minimal fix:** Resolve relative to the notes file's own directory
  structure: try the suffix after the **first** plausible anchor and verify
  by successfully opening the file; on failure, fall back to asking for the
  exact Recordings date-folder. Do not clear a granted root on a
  file-not-found error — only on permission loss — and say which file was
  not found.
- **Validation:** Fixture with a nested repeated folder name
  (`OkuFlow\Recordings\OkuFlow\…`-style) and one with a renamed root;
  assert transcription starts or the message names the missing file.

### I18N-3 — Localization stops at the app boundary: notes, bridge, and Transcribe stay en/tr/de — Medium [in-progress scope gap]

- **Evidence (Observed):** The notes viewer's UI strings
  (`notes_document_template.hpp:191-196`), the bridge labels
  (`notes_transcribe_bridge.hpp:11-17`), and the Transcribe tool
  (`public/transcribe/i18n.mjs`, three locales) cover en/tr/de only, while
  notes `lang`/`dir` now follow all 25 app languages. A French user's notes
  document will be `lang="fr"` with English controls, and the tool opens
  with `?ui=fr` and falls back to English. `scripts/render_notes_preview.py`
  accepts only `--lang en|tr|de` (`docs/hardcoded_paths.md:6`), so RTL notes
  cannot even be previewed. Related RTL risks are currently unverifiable:
  the notes CSS uses physical sides for the kind bar, chevron, ask-bubble,
  and back-to-top (template lines 71, 94, 100-103, 119, 155), and Android
  uses literal `‹`/`›` glyphs that do not mirror
  (`AdvancedSettingsPanel.kt:126,184`, `OkuFlowCameraApp.kt:283`) — all
  Inference until an ar/fa/he/ur fixture is rendered.
- **Minimal fix:** Decide explicitly whether notes/transcribe are in the
  25-language scope; if yes, generate their string tables from the same
  translation pass and extend the preview script's `--lang`. If not, say
  so in the release notes so the mixed-language notes page is a known
  limit, not a surprise. Either way, render one RTL notes fixture and one
  RTL Android settings preview before shipping RTL languages, and switch
  directional glyphs/CSS to logical/auto-mirrored forms where the fixture
  shows breakage.
- **Validation:** The existing notes-site browser check at an RTL language
  plus screenshot inspection; Android Compose preview with `locale = "ar"`.

### WEB-2 — Homepage root font size overrides the visitor's browser preference — Low-Medium [shipped]

- **Evidence (Observed):** `site-395fb02f0dc3.css:1` sets
  `html{font-size:18px}`. A px root size discards the browser's default
  font-size setting that low-vision visitors are the most likely to have
  raised; every `rem` on the page then scales from 18 px instead of their
  preference. The project's own Transcribe CSS does this correctly:
  `html{font:112.5%/1.6 …}` (`transcribe.css`), which is the same 18 px at
  defaults but respects user settings.
- **Minimal fix:** Change the homepage root to `font-size:112.5%` and check
  the few px-based layout constants (header height fallback, breakpoints
  are in px and unaffected by root size in media queries — verify the
  sticky-header offset still measures correctly, it is JS-measured).
- **Validation:** Load the page with a 24 px browser default font size and
  confirm body text scales up; `npm run check` for asset rehashing.

### WEB-3 — Website language redirect sends Traditional Chinese readers to Simplified — Low [shipped]

- **Evidence (Observed):** `site-e127eaf5b4b2.js:18-23` matches by primary
  subtag only, so `zh-Hant`/`zh-TW` auto-redirects to the `zh-Hans` page
  from the root. Both apps deliberately do the opposite
  (`app_language.hpp:63-66`, `AppLanguage.kt:60-71`: Traditional is *not*
  mapped to Simplified). Portuguese→Brazilian is consistent across all
  three and is documented intent.
- **Minimal fix:** In the website matcher, special-case Chinese like the
  apps: exact/Simplified signals (`zh-Hans`, `zh-CN`, `zh-SG`, bare `zh`)
  → `zh-Hans`; Traditional signals (`zh-Hant`, `zh-TW`, `zh-HK`, `zh-MO`)
  → no redirect (English root with the language menu available).
- **Validation:** Serve `public/` locally with `navigator.languages`
  stubbed per case (existing check script style) or a one-off headless
  profile; assert final path per input tag.

### WEB-4 — Sharing and mobile payload gaps — Low [shipped]

- **Evidence (Observed):** The template emits `og:title/description/type/url`
  but no `og:image` (`src/index.html:9-12`), so shared links render without
  the product image — the cheapest distribution surface a free tool has.
  Hero/tour images are single 1920 px files with no `srcset`
  (`src/index.html:100-118`); the equation capture alone is 402 kB, and a
  phone downloads the same bytes as a desktop. Copy nit in the same area:
  `hero.lede` says the assistant "reads the slide aloud" when asked, but
  Read produces text and speech only starts on the separate Read Aloud
  action — the sentence overpromises by one step.
- **Minimal fix:** Add a dedicated 1200×630 `og:image` (reuse the equation
  capture, content-hashed like other media, with `og:image:alt` in each
  locale or English-only alt), add `srcset`/`sizes` with a ~960 px variant
  for the five captures, and reword the lede to "reads the text and can
  speak it aloud when you choose Read Aloud."
- **Validation:** `npm run check` (extend it to require `og:image`
  presence), and a link-preview debugger fetch of the deployed page.

### AND-1 — Android settings selectors hide their selected state from TalkBack — Low-Medium [shipped]

- **Evidence (Observed in source; TalkBack behavior is Inference — no
  device run in this review):** In `ChoiceSetting`
  (`AdvancedSettingsPanel.kt:313-332`) the selected row is conveyed only
  visually (inverted colors, bold, a "✓" whose semantics are explicitly
  cleared at line 324-325, and "▾" cleared at 313). A TalkBack user opening
  a menu hears the options with no "selected" state. The exposure `Slider`
  (lines 205-208) sets a `contentDescription` containing the label+value,
  but slider adjustments will still announce a percentage rather than the
  −12…+12 step value unless a `stateDescription`/progress semantics is
  provided. `SettingsHint` and panel hints also use
  `TextAlign.Justify` (line 286; `SetupPanels.kt:111,173`), which creates
  uneven word spacing that is measurably harder for low-vision reading;
  the repo documents justified paragraphs as a house rule, so this is a
  deliberate-style recommendation to reconsider, not a defect claim.
  Smaller items: the "upcoming" progress dot uses `#5C5C5C` on black
  (~3.1:1, `Theme.kt:21`, `SetupPanels.kt:219`) — decorative, but at the
  non-text contrast floor; the new Language section sits first in Advanced
  (`AdvancedSettingsPanel.kt:160-162`), pushing camera controls down —
  helpful for escaping a wrong language, costly for the common case;
  consider keeping it first only until first successful stream, or moving
  it below video with its native-name list (names are findable in any UI
  language, which is the actual rescue mechanism).
- **Minimal fix:** Add `semantics { selected = … }` (and a state
  description like the current value) to menu items and the closed
  selector; give the slider a `stateDescription` of the signed step;
  switch hints to start-aligned text.
- **Validation:** TalkBack pass on the validation Pixel (already listed as
  acceptance work in `android/README.md`): open each dropdown, confirm the
  current choice is announced; adjust exposure by swipe and confirm step
  values are spoken. Screenshot at 200% font after the alignment change.

## Gaps not verified (explicitly not findings)

- **No runtime verification in this review.** No builds, tests, GUI
  fixtures, browser automation, APK installs, or device interaction were
  performed, per constraints. Every code finding above needs its listed
  validation before being called reproduced.
- **The six unresolved CUDA GUI cases** from the diagnostic-duration run
  (three desktop interruptions, three assertions: held dock pull, minimum
  viewport chrome, unrelated-input idle fade) remain open evidence, neither
  app bugs nor passes, until a serial desktop rerun.
- **Long-soak/thermal behavior** for the phone stream, older-phone
  coverage (Samsung S21 FE / Galaxy A35 were not attached), TalkBack
  acceptance, and 200%-font device rendering remain as already recorded in
  `android/DEVICE_VALIDATION.md` and `android/README.md`. The one live
  cable cycle and 60-second sessions do not establish them.
- **SuperRes + Extra Stable on a real phone at 16x** is partially covered
  post-fix (validation doc, session `20261013-180042`); stabilization
  *quality* conclusions were explicitly limited by low tracking confidence
  in the test scene.
- **Odd-dimension NV12 apertures:** `ResolveNv12DisplayAperture`
  (`capture_buffer.cpp:56-82`) rejects any odd x/y/width/height, and the
  phone decoder treats that as fatal (`phone_camera.cpp:350-351`; HEVC
  would first fall back to AVC, which would hit the same check). Phone
  encoder modes are even-dimensioned in practice, so this is an unverified
  edge, listed here only so a future odd-crop SPS does not surprise anyone.
- **`QLocale` script behavior** for `zh-CN`/`zh` in the new Windows mapping
  (assumed SimplifiedHan) was not executed; covered by the test request in
  I18N-2.
- **Packaging/signing** was not re-audited beyond DOC-1: v0.2.3 is an
  unsigned preview, the Store is unsubmitted, and winget status was taken
  from the repos' own docs, not independently verified.
- **Performance numbers** quoted anywhere above come from the project's
  own validation docs; this review measured nothing and invents no
  benchmark.

## Method limits

This was a source-and-document review of a moving working tree. Files
changed during the session (the dirty-entry count grew while reviewing),
so in-progress findings describe the snapshot named above and should be
re-checked against the implementing agent's latest state before action.
Shipped-behavior findings (DOC-1, WEB-1…4, CAP-2, NOTE-1/2, AND-1,
WIN-UX-1, and the I18N-2 failure path) were verified against committed or
published artifacts and do not depend on the concurrent work.

## Parent verification and routing

The parent independently confirmed the translator remove-before-load path
(I18N-2), unknown-generation packet parking (CAP-1), completion-only transcript
export/persistence (NOTE-1), and fixed 18px homepage root font size (WEB-2).
These control-flow/CSS findings are not fresh runtime reproductions. Missing
translation files remain an in-progress observation, not a shipped defect.
I18N-2, locale coverage across notes/bridge, and Android selected-value
accessibility were passed to Opus. After its API rate limit stopped the task,
the parent completed 25-language resources, native launcher/notes/bridge/browser
coverage, and Android selected-state/slider semantics. The offscreen compiled
catalog fixture caught and repaired Chinese script matching and verifies that
failed catalog replacement preserves the active translation/locale/direction.
Android assembly, 67 JVM tests and lint pass; the website's 25-language static
checks pass. Native large-type, screen-reader and older-phone acceptance remain
pending. See [localization](../docs/localization.md) for the implementation and
machine-assisted translation limits. These local changes are not a new release.
The owner's requested justified text alignment remains intentional; the review's
alternative alignment suggestion is not an authorization to change it.
No other production fix or new GUI/device test was performed for this review.
