# Plan 17 — Renaming the Project (target: FrontRow — read Phase 0 first)

Status: DRAFT — blocked on the Phase 0 naming decision by the owner.
Everything after Phase 0 is name-agnostic: it uses `<Name>` (display form,
e.g. `FrontRow`), `<name>` (lowercase identifier, e.g. `frontrow`), and
`<name_snake>` (e.g. `front_row`) placeholders so the same plan executes
regardless of which name survives clearance.

## Why rename at all

"OpenZoom" has three problems. First, it is generic — "open" + "zoom"
describes half the magnifier tools ever written and is nearly
unsearchable. Second, it collides with existing software: openzoom.org was
a reasonably well-known open-source Deep Zoom imaging library, so our name
is second-hand. Third, "Zoom" is the flagship trademark of Zoom Video
Communications, which defends it actively; a camera application whose name
embeds "Zoom" invites a dispute we would lose on legal budget alone, and
we are about to formalize commercial offerings (COMMERCIAL.md) that make
us a more visible target. A rename before public launch is cheap; a rename
after adoption is expensive. Now is the moment.

## Phase 0 — Name clearance (evidence gathered 2026-07-23; owner decision gate)

**FrontRow: BLOCKED — do not use without a lawyer saying otherwise.**
Two independent, serious conflicts were found:

1. "FrontRow" is a **registered trademark of FrontRow Calypso LLC**
   (gofrontrow.com), part of Boxlight / the William Demant Group — a
   classroom audio, lesson-capture, and campus-communication company
   deployed in roughly 5,000 US K-12 school districts, with international
   offices. That is our exact market (education technology sold to
   schools) and an adjacent mission (William Demant is a hearing-assistive
   group). Same name + same market + registered mark = textbook
   infringement exposure the moment we sell an SLA to a school.
2. **frontrow-notes.com** ("FrontRow — Classroom Accessibility for Every
   Student") already ships a computer-vision classroom-accessibility
   product aimed at **visually impaired / low-vision college students** —
   our product category almost verbatim. Even ignoring trademark law, the
   SEO and word-of-mouth collision would be fatal: every recommendation of
   "FrontRow for low-vision students" would find them, not us.

There is also "MyFrontRow" (classroom audio control) on the Apple App
Store. The name is beloved but the field is occupied — three times over,
in our own niche. The emotional idea behind it ("any seat becomes the
front row") is still ours to use as a *tagline* — taglines are not
trademarks in conflict here.

**Screened alternatives (from the owner's shortlist):**

| Candidate | Result of first-pass screening |
|---|---|
| StudyLens | BLOCKED — multiple existing "StudyLens" education apps on the Apple App Store (AI study companions). |
| OpenLoupe | **CLEAR so far** — no software product found under this name. A loupe *is* a magnifier; "Open" signals open source. Strong candidate. |
| OpenVantage | CLEAR in first pass, but "Vantage" appears as a product name in the video-magnifier market (verify Ash Technologies' "Vantage" line before committing). |

**Remaining clearance checklist for the chosen name (owner or implementer):**

- [ ] USPTO trademark search (tmsearch.uspto.gov) for the exact word and
      close variants in classes 9 (software) and 42 (SaaS); repeat on
      EUIPO eSearch if EU distribution matters.
- [ ] Domains: `<name>.org` (preferred for a GPL project), `.com`, `.app`.
- [ ] GitHub organization / repository name availability.
- [ ] Microsoft Store / winget package ID availability.
- [ ] Plain web search: first two result pages must not contain a
      software product, especially not an assistive-technology one.
- [ ] Say it aloud to three people who haven't seen it written (screen
      readers will speak it constantly — it must sound unambiguous;
      "OpenLoupe" pronounces cleanly as "open loop"-ish, decide if that
      homophone is acceptable, it arguably even fits a camera pipeline).
- [ ] Owner picks: styling (`OpenLoupe` vs `Openloupe`), and the decision
      is recorded here, replacing this checklist with the result.

**Recommendation:** OpenLoupe as primary candidate, pending the checklist.
Do not proceed to Phase 2 until Phase 0 is signed off in this file.

## Phase 1 — Identity decisions (half a day, no code)

Decide and record in this file before touching code, because every later
step consumes these constants:

- `<Name>` display name, `<name>` identifier, `<name_snake>` file form.
- Executable name: `<name>.exe` (currently `open_zoom.exe`).
- Windows AppUserModelID: `CemKaya.<Name>` (currently `"OpenZoom.OpenZoom"`
  at src/app/app_bootstrap.cpp:21). Changing it un-pins any existing
  taskbar pins — acceptable now, painful later; another reason to rename
  before distribution.
- Settings identity: the settings root under
  `QStandardPaths::AppDataLocation` (src/app/settings_store.cpp:597-602)
  and the Setup Assistant tool root under `GenericDataLocation/OpenZoom/`
  (src/common/assistive_runtime.cpp:597-598).
- Tagline (marketing, README headline, Store listing). Suggested, keeping
  the FrontRow spirit legally free of the FrontRow mark: **"Every seat is
  the front row."**
- Icon: keep the current chroma icon initially (rebrand later if desired);
  only the embedded strings change in this plan.

## Phase 2 — User-visible rename + data migration (1-2 days)

Order matters: migration code ships *in the same build* as the rename so
no user ever starts the renamed app without their settings following them.

1. **Settings migration.** In `SettingsStore`, when the new
   `%APPDATA%/<Name>/settings.json` does not exist but the old
   `%APPDATA%/OpenZoom/settings.json` does, copy (not move) it to the new
   location on first run, then log one line. Leave the old file in place
   for at least three releases so downgrades keep working; document the
   eventual cleanup in [`00-status-and-priority.md`](00-status-and-priority.md).
   Add a `migratedFrom` field to the JSON so
   support can tell.
2. **Tool directory migration.** The Setup Assistant installs Tesseract
   and the NVIDIA runtime under `GenericDataLocation/OpenZoom/tools/`.
   Same copy-if-missing strategy to `<Name>/tools/`, or simpler: keep
   probing both roots (new first) in `assistive_runtime.cpp` and only
   install new downloads to the new root. Choose the simpler probe-both —
   these are multi-hundred-MB payloads; copying them is hostile.
3. **Windows identity.** AppUserModelID, `assets/openzoom.rc` version
   strings (ProductName, FileDescription, InternalName, OriginalFilename),
   `.ico`/`.qrc` file names may stay physically the same in this phase —
   only string content changes.
4. **Visible strings sweep.** Window title, About/Setup Assistant texts,
   accessible names and screen-reader announcements containing the app
   name, the Maxine attribution line (re-read NVIDIA EULA §3.1 wording
   with the new name), error dialogs, the assistive runtime's notes/config
   headers. Grep targets: `OpenZoom`, `open_zoom`, `openzoom` across
   src/, include/, assets/, scripts/ — but in this phase change only
   user-visible strings, not identifiers.
5. **Bundle and scripts.** `scripts/build_release_bundle.bat` output dir
   `dist\OpenZoom` → `dist\<Name>`; windeployqt paths follow the new exe
   name if the target is renamed here (defer target rename to Phase 3 if
   preferred — then the bundle script renames the exe at packaging time,
   temporary but lower-risk).
6. **Docs headline pass.** README title becomes `<Name>` with an explicit
   "formerly OpenZoom" note kept for at least a year (search engines and
   returning users need the bridge). CHANGELOG gets a rename entry.
   docs/hardcoded_paths.md updated with the new persistent paths.

Acceptance: fresh machine → old-version install → settings created → new
version first run → all presets/custom schemes/AI keys present; Tesseract
found without re-download; screen reader announces the new name; taskbar
pin of the new exe survives relaunch.

## Phase 3 — Internal mechanical rename (1-2 days, one atomic PR)

The dangerous phase, purely because of repo mechanics. Do it in a single
PR containing *no functional change whatsoever*, after Phase 2 has been
runtime-verified.

- `namespace openzoom` → `namespace <name>`; `include/openzoom/` →
  `include/<name>/` via `git mv` (preserves history); every `#include
  "openzoom/..."` rewritten.
- CMake: `project(...)`, target name `open_zoom` → `<name>`, output name,
  test target names, presets untouched (they reference build dirs, not the
  name).
- `assets/openzoom_resources.qrc`, `openzoom.rc`, `openzoom.ico` renamed
  and re-referenced; qrc resource *prefixes* checked for name strings.
- agents.md, docs/code_reference.md, docs/README.md path references.
- **Line-endings hazard (critical):** this repo has MIXED CRLF/LF endings
  per file. Do the mass rewrite with a script that reads each file as
  bytes, replaces tokens, and writes back without touching newlines —
  never with a blanket `sed` that normalizes endings, and never let an
  editor "fix" endings in passing. Verify with `git diff --stat` (should
  be ~rename-sized) and `git diff --ignore-all-space` (should show only
  token changes).
- Gates: msvc-release build green, msvc-cpu build green, ctest green,
  app launches, camera runs, settings load — before merge.

What deliberately does NOT change: git history (never rewrite), the
`improvement_ideas/` texts (historical documents keep saying OpenZoom),
old CHANGELOG entries, and the LICENSE's GPL body. The LICENSE *notice*,
COMMERCIAL.md, and CLA.md get the new name — CLA.md §1 already defines
the Project as "including any renamed successor of the same code base",
so existing agreement language survives the rename with no re-consent.

## Phase 4 — Ecosystem (an afternoon + ongoing)

- GitHub: rename the repository (GitHub auto-redirects old URLs and git
  remotes, but update local remotes and any CI references anyway), update
  description, topics, social-preview image.
- Register `<name>.org` (+ `.com` if cheap) immediately after the
  decision — domains are the race-condition here.
- **Trademark filing** for `<Name>` in class 9 (and 42 if services are
  sold under the name) — this is the moat that makes COMMERCIAL.md
  sellable; budget for a basic filing, it is the one paid step that
  should not be skipped.
- Announcement copy: one paragraph — what renamed, why, nothing else
  changed, links redirect. Reuse the "every seat is the front row" story.
- Update the assistant memory files that reference `open_zoom.exe` paths
  (build recipe, crash-forensics recipe) after the exe rename lands.

## Risks and rollback

- Biggest risk is Phase 0 being skipped under enthusiasm: FrontRow's
  conflicts are disqualifying, and StudyLens is taken. Choose from
  cleared names only.
- Phase 2 and 3 are independently revertible single PRs. If Phase 3's
  mechanical rename misbehaves (endings churn, moc/AUTOMOC surprises),
  revert it alone — the user-visible rename from Phase 2 stands on its
  own indefinitely; internal identifiers saying `openzoom` harm nobody.
- Settings migration is copy-based, so a rollback to the old exe finds
  its old file untouched.

## Sources (Phase 0 evidence)

- FrontRow trademark/company: https://www.gofrontrow.com/ and
  https://www.gofrontrow.com/about/ ("FrontRow ... trademarks or
  registered trademarks of FrontRow Calypso LLC").
- Low-vision classroom product with the same name:
  https://www.frontrow-notes.com/
- MyFrontRow app: https://apps.apple.com/us/app/myfrontrow/id6479542761
- StudyLens collisions: https://apps.apple.com/us/app/studylens/id6751152966
  and https://apps.apple.com/us/app/studylens-ai-quiz-flashcards/id6757408010
