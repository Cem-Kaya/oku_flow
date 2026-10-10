# 41 — Project Review and Improvement Priorities (2026-10-09)

Status: **REVIEW COMPLETE — recommendations, not implemented.**

This is the owner-requested analysis of the directory and current project.
The implementation tracker remains [00-status-and-priority.md](00-status-and-priority.md).
Existing work keeps its original plan number; the references below refine that
work rather than create competing implementation plans.

## Scope and baseline

- Reviewed branch `agent/okuflow-integrity-hardening`, HEAD `078cdde`, plus
  the existing uncommitted Qt 6.12 migration, UI polish, and lecture-camera work.
  Findings describe the working tree, not just the committed snapshot.
- Read the root agent guide, README, architecture guide, hardcoded defaults,
  third-party notices, existing reviews, and verified non-issues. Inspected
  capture/presentation ownership, photo recovery, assistive storage, settings,
  UI styling, build scripts, CMake targets, and test registration.
- Changes in this review are documentation only. Existing code changes and
  user-owned output were preserved; no release bundle was published.
- **Confirmed in source** means the stated control flow or omission was
  inspected. **Conditional risk** means the consequence requires a condition
  such as a stalled drive and was not reproduced. **Acceptance gap** means
  implementation exists but the required user/hardware evidence is missing.
- Source line numbers are navigation hints for this snapshot. Relocate by
  the named function after subsequent edits.

## Project assessment

The project already has a substantial implementation and regression suite.
Its strongest architectural choices are explicit CUDA/D3D12 ownership,
separate camera and viewport clocks, bounded recording queues, asynchronous
image preparation, protected API credentials, and tested staged packaging.
Preserve those contracts while improving the remaining boundaries.

The directory layout is useful: `src/` and `include/okuflow/` mirror the
runtime modules; `tests/` contains deterministic and hardware-dependent
checks; `scripts/` owns supported validation; `docs/` describes shipped
behavior; `improvement_ideas/` owns future work. `build*`, `dist`,
`local_evidence`, `ref`, `ui ideas`, and `media_campaign` are local/ignored
working assets, not evidence of committed build artifacts. Git ignore rules
do not keep those directories out of Google Drive synchronization.

The best next investment is preserving saved work and making failure/recovery
behavior predictable, followed by accessibility acceptance and reproducible
validation. Additional image effects are a lower priority than those gaps.

## R1. Protect already-saved photos during recovery and rollback — P1

**Confirmed in source; no real user files were altered to reproduce it.**

[UserDataPaths::RecoverInterruptedPhotoPairs](../src/app/user_data_paths.cpp)
(lines 302–460) enumerates every `IMG_*` pair. When one final JPEG exists
without its partner or a recoverable `.writing` file, it deletes that final
even when no `.pair.lock` exists. Startup calls recovery with `now + 1 second`
in [OkuFlowApp::Initialize](../src/app/app_bootstrap.cpp) (lines 90–95), so
ordinary existing files qualify immediately.

A user can save a valid pair, move/delete only the original, then restart:
the remaining processed image is treated as a failed transaction. The
existing `removesUnrecoverablePhotoOrphan` test in
[user_data_paths_tests.cpp](../tests/user_data_paths_tests.cpp) (line 150)
explicitly expects deletion of an unmarked final; it does not distinguish
crash residue from an intentionally retained photo.

There is a second ownership problem in
[SaveCapturedPhotoPair](../src/app/app_pipeline_runtime.cpp) (lines 2431–2555):
names use only a millisecond wall-clock timestamp. If an already-committed
pair has the same stem, the new writer can acquire the now-absent lock, fail
to rename its temporary file over an existing final, then remove both
pre-existing finals in its rollback. That collision is conditional; it was
not observed during this review.

**Suggested change:** recover/delete only files proven to belong to an
unfinished transaction. Leave ambiguous legacy finals in place and report
them without presenting them as a newly completed capture. Use collision-safe
capture identities/reservations and track which paths the current writer
actually created before rolling anything back.

**Acceptance:** preserve a lone user-retained original or processed JPEG;
preserve pre-existing bytes under an injected filename collision; still
complete a genuine interrupted second rename; still remove owned failed
temporary files; still leave a live writer's transaction alone. Integrate
with [plan 20](20-capture-recording-integrity.md) and plan 37's recovery work.

## R2. Bound storage work during startup and shutdown — P1 under stalled storage

**Confirmed execution paths; conditional hang/latency risk.**

[AssistiveRuntime::~AssistiveRuntime](../src/common/assistive_runtime.cpp)
(line 403) calls `imagePreparationPool_->waitForDone()` without a deadline.
The pool's Codex preparation jobs write temporary JPEGs. `QueueNotesWork`
(line 2033) runs file operations on `QThreadPool::globalInstance()`; its own
teardown comment acknowledges that Qt waits for that pool at application
shutdown. Bounded queue size does not bound a blocked filesystem operation.
The separate photo pool already has a 10-second shutdown policy in
[app_bootstrap.cpp](../src/app/app_bootstrap.cpp) (line 1394).

Startup also validates the configured output root and scans all photo
directories synchronously before building the main window. A large archive,
removable drive, or network destination can therefore delay the first UI even
though camera opening itself has been moved to a worker.

**Suggested change:** give assistive preparation and notes storage explicit
shutdown ownership and deadlines; report pending/failed saves truthfully.
If accepted notes must survive a destination failure, use a recoverable local
spool rather than promise delivery through an unbounded exit wait. Move
archive recovery off the startup UI path after R1 establishes safe ownership.
Do not free state still referenced by a timed-out worker.

**Acceptance:** controlled blocked-writer tests demonstrate responsive startup
and bounded close, no callback into a destroyed QObject, and recoverable or
explicitly reported unsaved work. Compare startup with a populated archive
using [the tracked profiler](../scripts/profile_startup.ps1). Extend
[plan 22](22-threading-performance.md); do not redo its completed offloads.

## R3. Finish system contrast-theme support and accessibility acceptance — P1

**Confirmed styling gap; visual/screen-reader acceptance not run here.**

[MainWindow](../src/ui/main_window.cpp) uses palette roles for many controls,
but overrides disabled text and placeholders with fixed colors and assigns
`#b3b3b3` to scope subtitles (lines 265–333).
[AssistiveOverlay](../src/ui/assistive_overlay.cpp) (line 88), annotation
chrome, and AI Settings also contain fixed foreground/background styles.
The source search found no explicit Windows contrast-theme handling. This
does not establish that every native control fails to follow Windows.

**Suggested change:** implement [04 U2](04-accessibility-ux.md) together with
[40 C1/A2](40-ui-polish-and-checkout-review-2026-10-09.md): one semantic
theme layer, a system-palette path, and contrast-tested optional reading
themes. Keep camera display-color processing separate from UI chrome colors.
Retain visible focus and state cues in addition to color.

**Acceptance:** keyboard-only Read/Explain/photo/record/Draw workflows;
Narrator and NVDA names, values, state changes, and focus return; live Windows
contrast-theme changes; English/Turkish/German at 100/150/200% scaling.
Existing accessibility-event and language tests are valuable but do not
replace this acceptance. Track outcomes in plans 32, 33, and 40.

## R4. Close camera recovery and recording acceptance gaps — P1 verification

**Existing work, not a new claim that capture is broken.**

[Plans 20](20-capture-recording-integrity.md),
[28](28-dxva-zero-copy-capture.md), and
[30](30-external-memory-capture-bridge.md) retain real-device switching and
removal/reconnect acceptance. The tests deliberately simulate ownership
failures rather than wedge real drivers. [Plan 40 S5](40-ui-polish-and-checkout-review-2026-10-09.md)
still leaves the post-first-frame camera-stall status timeout open.

**Suggested change:** complete S5, distinguishing a stopped/stalled producer
from deliberate Extra Stable presentation holds. Record one reproducible
acceptance matrix for camera switch, unplug/replug, a source that remains
connected but stops delivering frames, resize/monitor changes during
recording, disk-full handling, and original/processed `_partN` pairing.

**Acceptance:** visible and accessible recovery state, responsive controls,
truthful drop/finalization reports, and playable paired files with timing
checked against wall clock. Use the isolated
[lecture camera](../docs/lecture_camera.md) for repeatable scene content,
then physical devices for driver recovery; that feed does not validate
optics, autofocus, or microphone capture. Never stop a user's OkuFlow
instance to make this test possible.

## R5. Make the documented build reproducible from a fresh shell — P2

**Confirmed configuration mismatch and local environment dependency.**

[README](../README.md) advertises CMake 3.23, but both
[preset files](../CMakePresets.json) use schema version 6. The locally
installed CMake 3.31 `cmake-presets(7)` manual identifies schema 6 as added
in CMake 3.25. The current presets therefore need at least 3.25, independently
of `cmake_minimum_required(VERSION 3.23)` for a non-preset configuration.

CMake/Ninja were not on this review shell's PATH. For the tracked matrix,
their installed Visual Studio 2022 directories were prepended explicitly;
`agent_build.bat` itself chooses the latest C++ toolchain via `vswhere`.
Preset Qt paths are fixed, and build trees still default inside the synced
checkout; only the bundle helper has `OKUFLOW_BUNDLE_BUILD_DIR`.

**Suggested change:** reconcile minimum versions, add actionable tool discovery
and preflight output for the actual MSVC/Qt/CMake/Ninja/CUDA selection, and
provide one documented local build-root override across the supported
matrix. Preserve Qt 6.12.0 as the baseline. Keep machine-specific settings in
ignored user presets rather than repeatedly editing tracked paths.

**Acceptance:** a clean shell and a relocated checkout can run the tracked
matrix using documented overrides; unsupported versions fail before a long
configure; build/cache paths remain outside Drive when requested. Extend
[06 B5/B7](06-build-tooling-docs.md) and [40 W3](40-ui-polish-and-checkout-review-2026-10-09.md).
Do not delete or move existing local output as part of this review.

## R6. Distinguish successful execution from skipped hardware validation — P2

**Confirmed gate limitation; not a claim that this run skipped tests.**

[tests/CMakeLists.txt](../tests/CMakeLists.txt) marks CUDA and GPU-codec tests
with `SKIP_RETURN_CODE 77`; the viewport suite can `QSKIP` individual cases.
[agent_build.bat](../scripts/agent_build.bat) and
[build_release_bundle.bat](../scripts/build_release_bundle.bat) use the CTest
exit code without a separate required-hardware-coverage policy. Their existing
empty-test checks are correct, but an unavailable GPU can still leave a green
summary without validating the shipping GPU path.

There is no checked-in `.github` CI workflow in this checkout. Project CMake
also does not define an explicit project warning policy. Existing plans
[06 B1/B3](06-build-tooling-docs.md) and
[29 I](29-idea-inbox-2026-07-29.md) already own these improvements.

**Suggested change:** preserve permissive skips for ordinary development,
but add a release-validation mode that requires the relevant GPU/codec cases
to execute and records passed/failed/skipped counts. Include Qt per-case skips,
not just top-level CTest status. Add a Windows CPU/translation CI gate and a
separate GPU acceptance runner, with an explicit desktop-session requirement
for GUI checks. Enable warnings on OkuFlow targets without imposing them on
fetched dependencies.

**Acceptance:** missing hardware cannot be reported as hardware validation;
the CPU gate remains usable without CUDA; test and translation failures stop
the gate; logs retain test counts and toolchain identity. Use the tracked
scripts, not a new machine-specific runner under `build/`.

## R7. Bound settings input before parsing — P2

**Confirmed missing bounds; oversized-file failure not induced.**

[settings::LoadDetailed](../src/app/settings_store.cpp) (line 839) reads the
whole settings file before JSON parsing. `ParseSettingsRoot` reserves and loads the
full conversation, custom-config, and preset arrays (lines 766–815) without
collection limits. Loading happens during startup on the UI thread.

Schema versioning, corrupt-file preservation, backup recovery, numeric
clamping, and Credential Manager integration already exist; do not implement
those again based on the old plan-05 status row.

**Suggested change:** cap serialized bytes before/during reading, and define
reasonable collection/string bounds for user-created settings. Reject an
oversized file through the existing preservation/recovery path, with a useful
notice, rather than exhausting memory or silently truncating user content.

**Acceptance:** boundary-size files and oversized arrays produce deterministic
results; a rejected primary is preserved; valid backup recovery still works;
ordinary profiles and conversation indices round-trip unchanged. Extend
[plan 23](23-security-privacy-release.md) and
[settings tests](../tests/settings_store_tests.cpp).

## R8. Reduce maintenance concentration around the application and UI — P2

**Confirmed structural observation; file size alone is not a runtime defect.**

This snapshot has 3,803 lines in `main_window.cpp`, 2,726 in
`app_pipeline_runtime.cpp`, 4,170 in `cuda_interop.cpp`, and about 1,100 lines
in `OkuFlowApp::Initialize`. `MainWindow`, `InteractionController`, and
`UIStateManager` remain friends of the app. CMake still recursively globs
all source `.cpp`/`.cu` files, the mechanism involved in the earlier hybrid
checkout failure.

**Suggested change:** continue [40 C2/C3](40-ui-polish-and-checkout-review-2026-10-09.md)
and [29 R](29-idea-inbox-2026-07-29.md): extract panels/actions and staged
startup services behind narrow interfaces, then use explicit source lists.
Separate photo transaction policy so R1 can be tested without constructing
the entire application. Preserve GPU ownership and UI-thread boundaries;
do not combine this with a rendering rewrite.

**Acceptance:** existing behavioral gates stay green, extracted components
have a clear owner/lifetime, and stale source files cannot silently become
shipping inputs. Update `docs/code_reference.md` when implementation changes.

## R9. Surface the existing build provenance inside the application — P2

**Confirmed partial implementation; refine 40 W2 / inbox S.**

[generate_release_metadata.ps1](../scripts/generate_release_metadata.ps1)
(lines 65–79) already records commit and dirty-tree state in the release
manifest. Do not add a second independent provenance format. However,
[okuflow.rc](../assets/okuflow.rc) contains only the icon resource and
[MainWindow](../src/ui/main_window.cpp) presents a plain OkuFlow title/help
surface without the build identity described by the backlog.

**Suggested change:** derive Help/About, diagnostics startup logging, and
Windows executable version metadata from the same build-time identity.
Include a stable build identifier or source fingerprint so two builds of
the same dirty commit can be distinguished. Retain the manifest's executable
hash as the link to the exact packaged binary.

**Acceptance:** a copied executable can identify its build; Help, logs, and
bundle metadata agree; an unavailable Git checkout is handled explicitly;
two different dirty builds are not indistinguishable. This is traceability
work, not evidence that the newly published Qt 6.12 bundle is mismatched.

## R10. Reconcile the backlog with shipped behavior — P2, before feature intake

**Confirmed documentation drift.** Examples requiring targeted reconciliation:

- The tracker still requests GPU recording, image-preparation offload, and
  stage timing work under plan 22 despite that plan's absorption notes and
  the current implementation. The actual storage remainder is R2 above.
- Plan 24 still calls key tests and `scripts/agent_build.bat` untracked;
  `git ls-files` confirms they are now tracked. Its historical test counts
  should not be presented as the current suite size.
- Tracker row 34 and its recommended order still ask for local OCR, while
  [plan 34](34-searchable-captured-notes.md) explicitly requires revision
  around user-requested vision reading and forbids implicit background
  subscription requests.
- `TODO.md` still asks to prototype a VLM overlay that already exists, while
  plan 06 says that file was deleted. Several older accessibility claims
  likewise predate labels, shortcuts, dependent-state fixes, and frame limits.
- README promises legacy `output/` copy migration and `MIGRATED.txt`;
  `docs/hardcoded_paths.md` both denies migration and later assigns it to
  the app. The current `UserDataPaths` API has no migration operation, and
  plan 21 records its removal. Bundle protection of an existing legacy
  `output/` directory is still real and must remain documented.
- In `docs/README.md`, frame-flow steps 6–7 appear after the intervening
  Language And Locale section. Restore the architecture sequence so
  recording and photo transactions are easy to find.

**Suggested change:** reconcile each claim against current symbols/tests,
mark historical evidence clearly, replace the root TODO with a tracker link,
and update the tracker rows and recommended order together. Do not close
pending hardware acceptance merely because source files are now tracked.

**Acceptance:** one current status per work item, no recommendation to rebuild
retired recognition components, no promise of nonexistent file migration,
and valid relative links. This review adds an entry point; it does not claim
to have completed that wider reconciliation.

## Suggested implementation sequence

1. R1 photo ownership, then R2 storage lifecycle, with regression coverage.
2. R3 system contrast/accessibility and R4 camera/recording acceptance,
   continuing the existing plan-40 polish work.
3. R5/R6 reproducible validation and R10 backlog reconciliation.
4. R7 bounded settings, R9 visible build identity, and incremental R8 extractions.
5. Resume owner-selected [34](34-searchable-captured-notes.md) and
   [35](35-pdf-image-source-mode.md) using explicit vision-reading consent
   and a shared accessible notes index. Keep vendor-independent rendering
   and further GPU tuning behind measured needs.

## Validation performed for this review

- `scripts/check_translations.ps1`: passed; 674 complete entries per catalog
  match the extraction manifest and compile with Qt 6.12.0.
- `scripts/agent_build.bat`: passed release compilation, CPU 24/24, and CUDA
  28/28. All CTest targets executed; the Qt test summaries reported zero
  skipped cases. This is a fresh incremental validation of the existing
  working tree, not a clean-clone or fresh-machine test.
- Full matrix log (local, outside git): the `%TEMP%` agent-build log with
  run id `4fb4b6ac0b4c44fd80f6a6dc88f2ffd7`.
  Per-case output remains in each preset's `Testing/Temporary/LastTest.log`.
  Both suites emitted a temporary-directory cleanup warning in the notes
  failed-storage test; the test itself passed. No compiler warnings were
  found in this incremental build log.
- Documentation checks: all 89 relative file links across this note and the
  two backlog indexes resolve; the new tracker row remains in its table;
  Markdown-scoped `git diff --check` and the new file's whitespace check pass.
  Whole-tree `git diff --check` reports pre-existing CR-at-EOL warnings in
  the two modified Windows build batch files (their bytes use CRLF with no
  trailing spaces/tabs). Those unrelated files were left unchanged.
- No new physical-camera, live-account, screen-reader, stalled-drive, or
  release-publication test was performed. Hardware/GUI tests in the tracked
  matrix are reported separately from those acceptance activities.
