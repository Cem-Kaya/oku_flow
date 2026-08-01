# Plan 21 — One Home for the User's Work (+ Open Folder)

Status: **IMPLEMENTED 2026-07-28** — code, migration, UI, tests, bundle policy,
and documentation landed in the working tree. Priority: complete.

Owner's framing: photos, videos, and the notes HTML should live **under one
parent directory**, and the app should have a **shortcut that opens Explorer
in that directory**. Whether that parent should be AppData was left open —
this plan answers it.

## The problem

Every user artifact is written next to the executable:

| Artifact | Current location | Code |
|---|---|---|
| Photos (original + processed) | `<install>/output/photos` | src/app/app_pipeline_runtime.cpp:1211 |
| Recordings (original + processed) | `<install>/output/recordings` | src/app/recording_manager.cpp:292 |
| Lecture notes HTML + images | `<install>/output/notes` | src/app/assistive_feature_manager.cpp:146 |
| Settings | `%APPDATA%/OpenZoom/OpenZoom/settings.json` (correct already) | src/app/settings_store.cpp:610 |
| Downloaded tools (Tesseract, NVIDIA runtime) | `GenericDataLocation/OpenZoom/tools` (correct already) | src/common/assistive_runtime.cpp:597 |

Three consequences, in order of how much they hurt:

1. **The owner has already lost data to this.** Release bundling deleted the
   `dist/OpenZoom/output` tree; the current mitigation is a
   preserve-and-restore hack inside `scripts/build_release_bundle.bat`. User
   data living inside a build output directory is the actual bug — the hack is
   treating the symptom.
2. **A real installation cannot write there.** Under `C:\Program Files`, every
   photo, recording, and note fails. This blocks any signed installer (plan 23),
   so it blocks distribution.
3. **The work is unfindable.** A student who needs to email a lecture note to a
   professor, or hand recordings to a disability office, has to navigate into an
   application folder. Nothing is indexed by Windows Search or backed up.

## Decision: Documents, not AppData — everything in ONE folder

**Root: `%USERPROFILE%\Documents\OpenZoom\`**
(`QStandardPaths::DocumentsLocation` + `/OpenZoom`), user-configurable.
**Confirmed by the owner, 2026-07-24.**

> **This plan explicitly does NOT scatter files into the per-type Windows shell
> folders.** Photos do *not* go to `Pictures`, recordings do *not* go to
> `Videos`, notes do *not* go loose in `Documents`. Everything a lecture
> produces lives together under the single `OpenZoom` root, in typed
> subfolders. Splitting one lecture across three shell folders is exactly the
> outcome this plan exists to prevent.

Rationale:

- **AppData is wrong for user work.** `AppData\Roaming` is hidden by default
  *and* roams — on a university-managed domain profile, roaming a semester of
  1080p lecture recordings is a disaster. `AppData\Local` is also hidden. Both
  are effectively invisible to Windows Search and to most backup tools.
- **A blind or low-vision student must be able to find these files with a
  screen reader, from the desktop, without knowing what AppData is.** That is
  the deciding argument. Documents is spoken, indexed, backed up by
  OneDrive/Windows Backup, and reachable in one keystroke from Explorer.
- **Per-type standard folders (Pictures/Videos/Documents) are the strict
  Windows convention, and we are deliberately diverging from it.** Sending
  images to `Pictures`, video to `Videos`, and notes to `Documents` disaggregates
  a single lecture into three places the user then has to reassemble — the exact
  outcome the owner rejected. One lecture is one unit of work: the photo of the
  slide, the recording of the explanation, and the notes about both belong
  side by side. Ship one root with typed subfolders and let the user relocate
  it wherever they like.

Layout:

```
Documents/OpenZoom/
  Photos/          YYYY-MM-DD/  photo_<timestamp>_original.jpg | _processed.jpg
  Recordings/      YYYY-MM-DD/  rec_<timestamp>_original.mp4  | _processed.mp4
  Notes/           NOTES_<timestamp>.html + images/
  Analysis/        stabilization + diagnostics exports
```

Date subfolders are optional but recommended: a semester of lectures in one
flat folder is hostile to screen-reader navigation.

Stays in AppData (internal state, correctly placed already): `settings.json`,
downloaded tool installs, caches, temp frames.

### Optional refinement (owner's call, default off)

A **per-session folder** mode: `Documents/OpenZoom/2026-07-24 Lecture 3/`
containing that session's photos, recordings, and notes together. This is the
purest expression of "one lecture, one folder" and pairs naturally with plan
18's annotation snapshots. Recommend shipping the typed layout first and
adding session folders once naming/UX is settled.

## The Open Folder shortcut

- A button in Advanced (near Setup & Downloads) **and** in the Simple-mode
  overflow: **"Open my OpenZoom folder"**, calling
  `QDesktopServices::openUrl(QUrl::fromLocalFile(root))`.
- Global keyboard shortcut (suggest `Ctrl+Shift+O`), listed in the Help dialog.
- Accessible name/description set via the existing `setA11y` pattern; announce
  the action for screen readers (announcement, never TTS — standing rule).
- **Reveal-after-write**: after a photo or recording is saved, the status
  message becomes actionable ("Saved — press Ctrl+Shift+O to open the folder").
  If cheap, add a per-file reveal using `explorer.exe /select,<path>`.
- If the root is missing or was deleted mid-session, recreate it before opening
  and say so rather than failing silently.

## Implementation

1. **Single source of truth.** Add `UserDataPaths` (suggested:
   `src/app/user_data_paths.{hpp,cpp}`) exposing `Root()`, `Photos()`,
   `Recordings()`, `Notes()`, `Analysis()`. Every call site above switches to
   it; `applicationDirPath()` must not appear in any output path afterwards.
   Creation is lazy, with one clear error surfaced to the status label if it
   fails (currently these failures collapse into generic save errors).
2. **Configurable root.** New `paths.userDataRoot` in settings.json (empty =
   default). A "Change folder..." button next to Open Folder using
   `QFileDialog::getExistingDirectory`. Validate writability by creating and
   deleting a probe file before accepting; reject a root inside the install
   directory with an explanation.
3. **Migration, copy-not-move.** ~~On first run with the new build, if
   `<install>/output` exists and the new root has no corresponding content,
   **copy** it across, write a `MIGRATED.txt` breadcrumb into the old
   location, and show a one-time status message naming the new folder.~~
   **REMOVED by owner decision 2026-07-31.** The shipped implementation
   skipped an entire category when the destination held even one file, then
   wrote the suppress-forever marker anyway (reported as a High review
   finding). The owner chose removal over repair: the migration prompt,
   `UserDataPaths::MigrateLegacyOutput`/`HasLegacyData`/`LegacyOutputRoot`,
   the `.legacy-output-migrated` marker, and the `MIGRATED.txt` breadcrumb
   are all gone. Old `output/` trees are left untouched forever; anyone who
   wants the files copies them by hand. Do not reintroduce.
4. **Free-space check.** Before starting a recording, verify available space on
   the root's volume (`QStorageInfo`) and refuse to start with a plain-language
   message rather than failing mid-lecture. Pairs with plan 20's recording work.
5. **Retire the bundle hack.** Once no user data can live under `dist/`, remove
   the preserve/restore logic from `scripts/build_release_bundle.bat` and its
   lock-check special case. Keep a one-line comment saying why it is gone.
6. **Docs.** Update `docs/hardcoded_paths.md`, README (where files land), and
   the Help dialog. After the plan 17 rename, the folder name follows the new
   product name with the same copy-if-missing migration.

## Acceptance

- Fresh install under `C:\Program Files`: photo, recording, and note all save
  successfully; nothing is written under the install directory, and nothing is
  written to `Pictures`, `Videos`, or the root of `Documents` — a single
  lecture's photo, recording, and notes are reachable from one folder.
- Open Folder opens Explorer at the root from both Simple and Advanced, by
  button and by keyboard, and is announced by the screen reader.
- Upgrade path: an existing `dist/OpenZoom/output` tree with photos, recordings,
  and notes appears in the new root after first run, and the original is still
  present on disk.
- Changing the root to a user-chosen directory relocates *new* writes only,
  survives restart, and rejects an unwritable or install-internal choice with a
  spoken-friendly message.
- Rebuilding the release bundle no longer touches user data (verify by pointing
  the root at the default and rebundling).
