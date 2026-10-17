# OkuFlow Tests

The 2026-10-15 scoped chat regression repair preserves native foreground,
Escape focus, drag latching/cancellation, streaming selection/scroll, and Hide UI
assertions. Fixtures use the visible chat header and `VisibleChatSession` shared
by both panes; Read uses its independent result accessor. Mode-suppression setup
seeds synthetic results without a camera or provider. The interrupted failed gate
is retained and is not acceptance evidence. Native screenshots from that gate
contain foreign magnifier/toolbar content and are unsuitable visual evidence;
the inspected offscreen r5 captures remain separate layout evidence.
The follow-up repairs inspect the owned color-editor popup while hidden to
verify absent-recipe reset, and seed a selected history row before asserting
Tab/Escape focus restoration. `controlledToggleRetainsInkAndSelectedTool`
also checks owner focus through controlled toggles and pointer/eraser/pen
changes while retaining its ink, undo/redo and manual-exit assertions.

`control_diagnostics_contract` is desktop-free and exercises the completed
device-diagnostic interpretation: locked versus invalid models, held/recovery
pixels, unready buffers, pending or unknown keystone geometry, corrupt/fractional
state tags, and immutable frame provenance. It does not validate actual CUDA
copies, detection quality, frame synchronization or a physical camera.

Settings-controller snapshot regressions distinguish unsaved live edits from
last committed values, missing first-run files, failed writes, recovered backup
state and partial credential-cleanup commits. Credential operations in these
fixtures use private fake storage; no owner's credentials are read or deleted.

The remote Windows workflow retains its complete standard-scale gate, then runs
the immediate native panel movement fixture at Qt scale 1.25, 1.5 and 2. Each
scaled diagnostic keeps its own JUnit/log and leaves the standard CTest evidence
archived separately. These selected cases are additional scaling coverage, not
full scaled-suite, mixed-monitor, CUDA or physical-device acceptance.

## GitHub Actions away from the owner desktop

The [Windows checks workflow](../.github/workflows/windows-checks.yml) runs on
GitHub's disposable `windows-2022` VM for main pushes and pull requests. It
installs Qt 6.12.0, selects MSVC 2022, validates all Qt catalogs and builds the
tracked `msvc-cpu` preset. Background checks run first through the existing
gate. Native GUI checks then run serially on the remote Windows desktop and
complete that same CPU inventory. An unavailable or locked input desktop is a
failure, not an offscreen pass. Nothing installs a runner on the owner's PC.
Before building, the workflow requests a 1920×1080 display on that disposable
VM and verifies its actual size. The default 1024×768 clips large native windows
and changes geometry restoration. A refused/unchanged mode cannot pass GUI
acceptance; the `desktop` manual scope checks only this prerequisite quickly.
The first remote preparation verified session 2, the Default input desktop and
an actual 1920×1080 mode change; no owner display settings were changed.
The contrast regression checks actual light/dark text pixels before and after
a runtime scheme switch. Inspector panes explicitly paint the matching Window
surface; native-style rendering cannot silently substitute a white pane.

The Qt downloader is pinned to an upstream aqt commit that supports Qt 6.11+
split Windows repositories; released aqt 3.3.0 cannot locate this SDK.
Qt SVG/Linguist come from the base package; Multimedia, Speech and Image Formats
are explicit additional modules.
The host-protocol capture fixture streams until the client closes and has a one-second
capture window; its packet/control assertions do not depend on 50 ms scheduling.

In GitHub **Actions → Windows checks → Run workflow**, select `background`,
`gui-all`, a `gui-state/layout/input/idle` group, or `two-cases`. The last option
runs only the docking-hold and mouse-idle slots previously interrupted locally.
Manual selections preserve every Qt data row and run each selected CTest entry
once; they do not replace complete gate evidence or retry until passing. The
remaining GUI entries without a subgroup are included in `gui-all` and `all`.

Each run uploads `windows-evidence-<run-id>-<attempt>` for 14 days with CTest
logs/JUnit, input-desktop evidence, local gate ledgers and fixture screenshots
when captured. A newer commit cancels an older run on the same ref; the workflow
has read-only repository permissions and pinned action commits. A separate
Linux job runs Python host-protocol tests without a phone or credentials.

These hosted checks cover the CPU build and native UI, not the shipping CUDA
matrix, Maxine, a physical phone/camera or MSI acceptance. They do not publish
or sign anything. Those hardware checks need a suitable separate machine;
never silently fall back to running GUI fixtures on the owner's desktop.
The future Luna screenshot-review/test redesign is a separate backlog item,
not an automatic visual reviewer implemented by this workflow.

Idle-chrome fixtures establish and drain native cursor/focus input before
manually expiring the idle timer. Requested Qt cursor coordinates allow only
one logical pixel of mixed-DPI rounding; interruption checks compare exact
native pixels. Hidden-chrome, unrelated-input and deliberate-move assertions
remain unchanged.

## Background language checks

`windows_runtime_contract` checks the public launcher with both flat and nested
owned children, including Unicode arguments, original working directory,
environment, stdout/stderr and exit code. It never installs the system runtime.
`user_data_paths` rejects the parent bundle as well as nested runtime directories
when selecting capture storage. `scripts/test_bundle_layout.ps1` uses synthetic
private files for byte-preserving relocation, legacy/nested inventory checks,
tampering and owner-data refusals. It is not clean-VM MSI acceptance.

`language_catalog_contract` sets Qt's offscreen platform and creates no windows.
It loads all 24 real compiled non-English catalogs, checks the 25-language
registry, system/saved precedence and RTL direction, and confirms that a failed
catalog replacement retains the current translation and locale. It also checks
the native launcher's language/message table. Its `headless` label does not
relabel or replace any existing native GUI fixture.

Android `AppLanguageTest` validates canonical tags, regional matches, Chinese
script precedence and legacy Hebrew/Indonesian tags through the JVM test gate.
The sibling website's `test-transcribe-languages.mjs` checks all 25 Transcribe
catalogs and locale selection through `npm run check`. Structural coverage does
not verify fluent wording, screen readers or visible native layouts; see
[localization](../docs/localization.md).

## Selectable GUI cases and exact-inventory resumes

`main_window_interactions.<slot>`, `annotation_overlay_interactions.<slot>`
and `ai_settings_dialog_interactions.<slot>` are individual CTest entries using
the existing three executables and Qt's test-function selectors. Every data
row stays with its slot, including English/Turkish/German, viewport widths,
provider choices and docking sides. `tests/gui_cases.cmake` discovers these
out-of-class slot definitions at configure time; keep that source style when
adding slots. The old suite names are still useful CTest regex prefixes.

Labels `gui-idle`, `gui-input`, `gui-layout` and `gui-state` allow focused runs.
They all retain `gui`; layout/state cases can still open native windows or
exercise focus, and none is a desktop-free substitute. In particular, the
three idle cases are separately selectable:

- `main_window_interactions.mouseFocusedChromeCanIdleFade`
- `main_window_interactions.unrelatedNativeInputCannotRevealChrome`
- `main_window_interactions.keyboardFocusedChromeSurvivesNativeHandoff`

All GUI entries have `RUN_SERIAL` and the `okuflow_desktop` CTest resource lock.
Every native GUI executable also acquires the same per-user `QLockFile` before
creating QApplication, refusing competing fixtures in another build/CTest
process. This does not reserve the desktop against a person or browser; arrange
a free desktop and never run browser automation concurrently.

The tracked build and bundle scripts call `scripts/run_test_gate.ps1`, which
archives each CTest invocation's JUnit and console output under
`%LOCALAPPDATA%\OkuFlow\test-evidence\`. Evidence is stored per test, build
directory and configuration. Each test's fingerprint covers its command
executable, rooted file arguments, adjacent DLL/QM files, CTest properties,
`REQUIRED_FILES` contents, Qt/system runtime DLLs, culture and relevant
environment values (including PATH and Qt scaling). Graphics driver, display/DPI
and boot identity apply only to `gui`-labelled tests. Only digests are saved.
Production sources are not hashed directly; a rebuilt executable changes its
own hash, so build before validating. Declare runtime data and helper binaries
that are not command arguments with `REQUIRED_FILES` (absolute paths). Shared
runtime inputs are deduplicated by digest, and each test keeps its current
fingerprint plus three recent reusable alternatives. Older Failed, Interrupted
and Unfinished fingerprints retain compact blockers: reverting inputs never
silently retries an old failure. Attempt JUnit, output and detailed CTest logs
remain archived. `hash-cache.json` in the same
directory reuses a file's SHA-256 only while its full path, length and
LastWriteTimeUtc ticks are unchanged. A selected test's post-run fingerprint
must still match. CPU, CUDA and bundle evidence never substitute for one
another. After changing monitors, native DPI, drivers or hardware, GUI evidence
is invalidated automatically.

PowerShell-backed tests resolve the actual interpreter image through
`cmake/PowerShell.cmake`, including when an existing cache contains a Windows
Store execution alias. The alias can launch successfully while refusing file
hashing; generated test commands use its real process image to avoid a pre-test
evidence failure. The cache retains the launcher and resolves its current image
on configure, including rediscovery when a cached versioned path disappears.
Reconfigure after updating/removing PowerShell before running cached CTest commands.
Registered Qt fixtures set `QT_FORCE_STDERR_LOGGING=1` so Qt assertions and
skipped data rows remain visible in JUnit and detailed CTest logs. This does not
change their platform, assertions or deadlines.
CTest retains up to 1 MiB of output per passing/failing test in JUnit. A passing
result with CTest's truncation marker remains `Unfinished`; inspect the archived
full LastTest log rather than accepting potentially hidden Qt row skips.

Background tests run in parallel across logical processors (`-Parallel`
overrides). Tests labelled `gpu` or `codec` run serially in a second CTest
invocation, followed by serial `gui` tests.

Assistant preparation slots register as `assistive_preparation.<slot>`, each
with the existing 30-second CTest deadline and every associated Qt data row.
Select the `assistive_preparation` label to run that whole fixture family;
there is no additional aggregate invocation. Registration follows inline private
slots in the fixture source and reconfigures when that source changes. A new or
reformatted slot must remain discoverable; compare CTest names with the built
fixture's `-functions` list after changing registration. All cases retain the
fake-provider binaries and compatibility profile in `REQUIRED_FILES`.

The phone-camera and legacy-adb fixtures additionally use `-v1` to retain row
entry and failure output through CTest on Windows.
Without that environment, Qt may send its plain-text output to the system
debugger instead. Keep original empty-output failures archived: a later local
pass is diagnostic evidence, not proof of the original failure's cause.

`PhoneCameraTests::alreadyBufferedFakeAdbRequestIsNotDiscarded` deliberately
buffers a complete private host-version request before the fake adb handler
starts. `FakePhoneAdb::Serve` must consume those bytes immediately instead of
waiting for another readiness event; the regression fails against the former
unconditional wait. It creates no runtime hooks, adb process or real-device
connection. Borrowing and enumeration-after-shutdown assertions remain intact.

`alreadyQueuedFakeAdbConnectionsAreHandled` completes two TCP handshakes before
letting the fake server accept either. Its worker must drain Qt's pending
connection queue before waiting for a new arrival; the regression fails on the
former unconditional wait. `FakePhoneAdb::ArrivalMode` selects normal traffic,
an already-buffered request or this deterministic backlog without real devices.

Legacy recovery scans the native process inventory and deliberately fails closed
when a candidate image disappears before classification. `adb_legacy_recovery`,
`windows_runtime_contract`, `mcp_bridge_contract` and the optional
`native_app_control_offscreen` share `okuflow_process_identity` so that scan does
not race the other fixtures' short-lived `oku_flow.exe`/`OkuFlowApp.exe` images.
This declares a host-resource dependency, not a retry or weaker safety check.
Unexpected successful-recovery status assertions also print the detailed guard
and diagnostic; the earlier CI Busy assertion did not identify its guard.

First run the normal `scripts\agent_build.bat`. It now continues to CUDA after
a CPU test failure, preserving independent configuration results. If the only
problem is a recorded desktop interruption, stop competing input, then run:

```powershell
./scripts/agent_build.bat --retry-interrupted msvc-cpu-tests main_window_interactions.mouseFocusedChromeCanIdleFade
$env:OKUFLOW_TEST_RESUME = '1'
./scripts/agent_build.bat
Remove-Item Env:OKUFLOW_TEST_RESUME
```

The retry command restores the same Visual Studio environment but does not
rebuild or publish anything. Keep the original Qt/OKUFLOW environment overrides.
It retries exactly one recorded interrupted case once, retains its original
failure/log, and still returns failure if another required case is unresolved.
The following resume runs normal compile/translation gates and reuses only
matching CTest evidence. If an unresolved test has different shared runtime inputs, resume
refuses before starting another attempt: diagnose the cause, unset the flag,
build and deliberately run a fresh gate. A diagnosed per-test repair/rebuild
can resume under the same shared runtime. Passing tests with changed inputs
still require new evidence. Batch CUDA result/scope/selection variables use
`AGENT_CUDA_*` names so bookkeeping cannot masquerade as CUDA runtime input.
PATH remains a real fingerprint input; use the same developer environment.
Do not use CTest's mutable LastTestsFailed
file as evidence for another configuration or use `--repeat until-pass`.

Only a single Qt failure explicitly marked `OKUFLOW_DESKTOP_INTERRUPTED:`
(confirmed foreign foreground ownership or observed physical pointer movement), or
desktop-lock refusal before QApplication, is eligible for `-RetryInterrupted`.
Qt activation failure while the fixture owns foreground, or unknown native
foreground ownership, remains an ordinary failure.
An ordinary assertion, mixed failures, timeout, crash or unfinished invocation
remains unresolved; it is never automatically retried. Fix the cause and build
a new inventory. To investigate an unclassified failure, use a direct selected
CTest run; that diagnostic cannot overwrite/clear ledger failure evidence.
Missing results remain pending. Optional CTest/Qt skips remain visible and are
executed again on a whole-suite resume, not reused as confirmed hardware passes.

Lower-level commands, from the same configured build environment:

```powershell
# Discovery only: no fixture execution or QApplication.
./scripts/run_test_gate.ps1 -Preset msvc-cpu-tests -List
# Diagnostic native run: serial, but does not write gate-ledger credit.
ctest --preset msvc-cpu-tests -L gui-idle --parallel 1 --output-on-failure
# Explicit local build, including a bundle build's independent inventory:
./scripts/run_test_gate.ps1 -BuildDirectory $env:OKUFLOW_BUNDLE_BUILD_DIR -Resume -RetryInterrupted 'main_window_interactions.mouseFocusedChromeCanIdleFade'
```

`-Test` selects exact CTest names and `-Label` selects exact labels with
any-match semantics; combined selectors form a union. Through `pwsh -File`, pass
a mixed list as one argument: `-Select "headless,deadline_worker"`. A selected
run reports `SCOPED PASS` or `SCOPED FAIL` for exactly those tests; missing
executables of unselected tests are allowed and unknown names or labels fail.
A scoped pass never establishes the release gate. `-ExcludeGui` is explicitly
partial; a later matching whole-suite resume fills in GUI entries. A complete
gate still requires the shipping compile, CPU and CUDA suites, and independent
bundle CTest before publishing. The bundle's existing emergency test bypass
remains visibly untested.

The CUDA step of `scripts/agent_build.bat` runs only what the CPU suite cannot
cover: tests labelled `gpu` or `cuda-variant`, and names absent from the CPU
inventory (`-UncoveredByPreset msvc-cpu-tests`). Common test translation units
compile identically in both configurations. Set `OKUFLOW_FULL_CUDA_SUITE=1` to
run the complete CUDA suite.

### Fast scoped checks while editing

```powershell
# Changes since the merge-base with origin/main, plus staged, unstaged and untracked files.
pwsh -NoProfile -File scripts/agent_check.ps1
pwsh -NoProfile -File scripts/agent_check.ps1 -Files src/app/settings_store.cpp
# Selection only, as JSON: mode none|scoped|full, tests, guiTests, targets, extraChecks.
pwsh -NoProfile -File scripts/select_tests.ps1 -Preset msvc-cpu-tests -Files src/ui/ai_settings_dialog.cpp
# Explicit scoped build and gate through the tracked helper.
scripts\agent_build.bat --scoped msvc-cpu headless deadline_worker
```

`scripts/select_tests.ps1` follows the configured Ninja graph transitively
(changed source or header, objects, OBJECT/static libraries, executables, CTest
cases) and adds tests whose `REQUIRED_FILES` or rooted command arguments name a
changed file. `tests/test_impact.json` covers inputs the graph cannot see:
build, preset, runner and workflow changes select `full`; Markdown, `docs/`,
`improvement_ideas/` and `website/` select `none`; Android code adds the host
protocol unittest; translations add the catalog check and language GUI cases.
Unknown inputs and a missing, stale or incomplete `.ninja_deps` select `full`.
`scripts/agent_check.ps1` builds only the selected targets in `msvc-cpu`, runs the
selected background tests, lists selected GUI cases as pending unless
`-IncludeGui` is given, and runs the extra checks. A `full` selection refuses
and points to `scripts/agent_build.bat`; `-AllowFull` instead builds `msvc-cpu`
and runs every background test. Its SCOPED result never replaces the release gate.

For selector changes, run `pwsh -NoProfile -File scripts/test_select_tests.ps1`
from a VS 2022 x64 developer shell; it uses a private synthetic CMake/Ninja
project. For runner changes, run `pwsh -NoProfile -File scripts/test_test_gate.ps1 -CMake
<cmake.exe>`. Its private command-line CTest fixtures verify partial/resume
coverage, pass reuse, explicit interruption retry, refusal to retry assertions
or mixed failures, retained logs, configuration/input/runtime/scale invalidation
and visible optional skips. A mid-run mutation check verifies rejection and
identification of the changed input; environment values remain hashed. It opens
no GUI, camera, browser or adb server.
Fixture CLI flags `--output` and `--trace` are reserved for generated results.
Their paths remain part of the command fingerprint; the produced bytes do not.

### Shared production objects and precompiled headers

Production sources reused by the CPU application and test fixtures are declared
explicitly as OBJECT groups in `tests/shared_sources.cmake`, so each compiles
once per group instead of once per executable. A group is valid only while every
original consumer's environment produces byte-identical preprocessed output and
the same code-generation flags. The mandatory headless
`shared_source_equivalence` test re-preprocesses each shared source under each
recorded consumer environment and names the source, consumer and first differing
line on failure. Shared source files and project headers are declared as runtime
inputs, so their edits select the guard and invalidate its cached result. Header
selection is intentionally conservative. It requires PowerShell 7 and the configured MSVC/SDK headers.
When it fails, restore equivalence or split the group; never filter out the
guard. Per-source compile overrides are rejected. QObject headers/AUTOMOC and
resources stay owned by their executables. The CUDA application keeps its own
sources. `OKUFLOW_USE_PCH` (default ON) adds precompiled standard/Qt/Windows
headers to the application and larger fixtures; shared objects compile without
PCH. Validate both PCH states and the shipping CUDA build when changing source
ownership.

Annotation drag/keyboard fixtures use the native foreground helper before
synthesizing input. Drag cancellation events log grab/activation state to aid
diagnosis without weakening the production release, deactivation or hold rules.

The refactor replaces unconditional layout/activation waits with bounded
condition waits. Continuous-resize validation exits once its existing ten-change
and presentation conditions hold, with the same 220 ms deadline. Negative
idle-fade, stationary-pointer and docking debounce/hold observation intervals
are retained. Earlier logs (`agent-build-zoom16-final.log`) measured main-window
17.44/18.02 s and annotation 13.41/13.88 s in CPU/CUDA, with suite totals
70.52/74.10 s; `bundle-0.2.3.log` recorded 80.62 s total. Those are pre-refactor
measurements. Full/targeted GUI timing and desktop acceptance must be measured
serially after integration; compilation or script fixtures do not establish it.

`pipeline_timing_percentiles` checks pending-scene presentation during retained
camera GPU-handoff retries: pan, zoom, and a native resize redraw use the held
scene without advancing camera effects or recording. This scheduler fixture
uses no camera, proprietary runtime, GPU submission, or desktop window;
real-phone SuperRes/stabilization acceptance remains a separate hardware check.

`settings_store_round_trip` preserves existing 12x values and focus positions,
round-trips 16x current/custom profiles, and bounds modern/legacy/serialized
out-of-range magnification. `viewport_transform_geometry` checks the 16x
GPU viewport crop against CPU pixel mapping across wide/portrait viewports and
edge focus positions. `main_window_interactions` checks the real slider's
16x limit; it remains part of the serial native desktop matrix.

`assistive_preparation` covers retained Explain threads, independent temporary
Read threads, Assistant identity preservation, cancelled start/resume openings,
and loopback HTTP history isolation after URL/model/provider/key changes, reset,
cancel, failure and bounded-history truncation. It never submits real AI calls.

`scripts/test_notes_site_browser.mjs` is an opt-in Chrome design fixture using
generated notes and the sibling website's `public/` directory. Set
`OKUFLOW_PUPPETEER_MODULE` to the file URL of an externally installed
`puppeteer-core` module, `OKUFLOW_TEST_CHROME` to Chrome's executable,
`OKUFLOW_TEST_PROFILE` to a private local profile, `OKUFLOW_TEST_WEBSITE` to
the website publish directory, `OKUFLOW_TEST_NOTES` to the English generated
notes file, and `OKUFLOW_TEST_EVIDENCE` to a local evidence directory.
It checks carousel states/reduced motion/RTL and local notes media, filters,
text-size persistence, contrast and narrow layouts; no camera/AI is used.
Render five drafts (`--lang en`, `tr`, `de`, `nl` and `ar`) first; it expects
those siblings of the selected `_en.html` file. It checks localized toolbar
labels and Arabic direction at 200% text. The carousel check requires one
navigation dot per actual slide rather than a stale fixed scene count.
Run it serially with the Windows GUI matrix. Evidence is not release media.

The torch rows in `phone_camera_contract` use private loopback socket pairs
and the production queued-command/guarded-write/status-ACK seams: canceled queue,
expired dequeued record, newer native command, rejected unbounded/canceled/expired
actor admission, retry retirement, and actual state changing only after ACK
(including an ACK after owner cancellation). They never call transport startup,
probe adb or open a real device. `capture_shutdown` additionally checks that a
synthetic phone session admits the bounded command before split stop, retires
its token at Begin and rejects later admission. These rows establish command
ownership/protocol behavior, not real-phone light or camera acceptance.

The remaining `phone_camera_contract` rows check USB connection-type protobuf parsing without
a Windows bus path, malformed lengths/varints, saved webcam/phone/lens-fallback
selection and actual NV12 output from Windows H.264 and optional HEVC decoders.
First-run selection prefers only a companion that answered its handshake;
saved webcam and legacy numeric preferences still win. Android settings tests
verify a new host session clears only the light. The HEVC row explicitly skips only when the optional decoder probe fails;
H.264 remains required.
Private fake-server rows also verify targeted USB recovery, a healthy device
with a stuck camera, a hung shared server, stalled transport selection,
replug races, old listing transport IDs, unsupported-service throttling,
unauthorized/Wi-Fi exclusion, busy/problem/cancellation boundaries and automatic
startup-failure recovery. Preferred idle discovery can reset its stalled phone;
ordinary browsing and live ownership cannot. No fixture reaches port 5037,
starts/stops a real server, opens a camera or steals desktop focus.
The same contract uses a Qt-free `adb_owned_child_fixture` to verify exact
job-owned child stop, cleanup after assignment failure, normal-owner and
crashed-owner cleanup, confirmed idle-hang restart, one-slow-probe/live-session
boundaries, cancellation during readiness, and repeated-call cooldown after
failed launch or child exit. Sleep modes have no windows/network; fake-host modes
bind only private fixture ports. Missing fixture registration is a test failure,
never a skipped pass.
Its native fake-adb mode checks actual bootstrap arguments/environment, PID
ownership, private endpoint routing, explicit webcam intent/release, default
server fallback after a child exits, owned recovery classification and the
shutdown latch. Scripted lifecycle rows also cover concurrent callers, stopping
during readiness, nonblocking observations/canceled lock waiters, temporary
live-session deferral and foreign listener rejection. Production wiring also
checks borrowed-hang shadow/handback, no handback with private transports, and
epochs when a new server reuses the same transport ID.
It uses no real camera, adb server or GUI. The original gray-scene fixture
`adb_legacy_recovery` uses a separate Qt-free listener inside synthetic flat and
nested bundles, pinned to fixture hashes on ephemeral ports. Native fixtures
check raw-kill and exact-handle fallback, manifest/launcher mismatches, sibling
and session refusal, cancellation, serialization, listener replacement before
and after TCP connect, and no bytes sent to the replacement. The test-only sibling
scope keeps a running owner app outside fixture assertions; no owner process is
closed and no row is skipped for desktop use. These tests establish host lifecycle
behavior, not real-phone WinUSB recovery or long-soak acceptance.

`data/phone-camera.h264` was generated locally with:

```text
ffmpeg -f lavfi -i color=c=gray:s=64x48:r=30 -frames:v 4 -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 1 -bf 0 -x264-params aud=1:repeat-headers=1 -f h264 phone-camera.h264
```

The matching original HEVC fixture `data/phone-camera.hevc` was generated with:

```text
ffmpeg -f lavfi -i color=c=gray:s=64x48:r=30 -frames:v 4 -c:v libx265 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 1 -bf 0 -x265-params aud=1:repeat-headers=1:log-level=error -f hevc phone-camera.hevc
```

The generation tool is separately installed and is not a runtime/test dependency.
The scene contains no third-party media. `python -B -m unittest discover -s
android/tests -v` runs seven standard-library host-probe tests with a private
fake adb server; it does not change the live server. Android JVM tests and lint
run through `scripts/build_android.ps1 -Lint`. Actual phone images, thermal
behavior, TalkBack and unplug/replug acceptance still need a USB phone.

`annotation_key_routing_headless` covers active/inactive Draw key ownership and
single owner delivery from owner/render/overlay inputs, including ignored keys,
Ctrl zoom, auto-repeat metadata, press/release and hide/deactivation pairing,
slider acceptance, inline editing, selected-ink precedence and missing targets.
Actual offscreen Qt focus transfers check that untouched empty/whitespace
entries close, typed text commits once and empty entries release owner navigation.
Its main selects Qt's offscreen platform
before constructing QApplication; it is safe in the desktop-restricted matrix.
A test-only redispatch fence bounds the broken pre-fix loop. It verifies event
routing, not Windows focus, screenshots or native overlay geometry.

When the desktop is reserved, set `OKUFLOW_AGENT_EXCLUDE_GUI=1` before running
`scripts/agent_build.bat`. It compiles the same release/CPU/CUDA matrix and
excludes only tests labelled `gui`; its summary explicitly marks partial
validation. Leave it unset for the full gate. A filtered pass does not verify
native focus, layouts or auto-hide. Audit 49 removes only unused APIs and
preserves the 17 test-only seams, including recording and CUDA observability.

Audit-47 extends `recording_manager` with AAC packet-versus-PCM accounting,
failed/stale statistics, bounded input duration/bytes, stereo alignment,
200,000-block clock-drift sequences, and audio-only queue overflow. The latter
fills the 512-block pre-worker queue without opening a camera or encoder;
the separate writer admission limit is 128 outstanding blocks. Audio losses
must not increase dropped-video totals.

`media_writer_codecs_aac` now decodes each complete H.264/AV1 audio stream and
checks PCM length and timestamps, rather than accepting audio headers alone.
Invalid PCM layout, duration, timestamp arithmetic, and oversized buffers are
rejected before submission. The opt-in `OKUFLOW_TEST_AAC_GAP_DIAGNOSTIC=1`
branch retains a strict reproducer for the still-open playback-gap defect;
it is expected to fail on the tested machine and is not part of the ordinary
codec gate. See [review 47](../improvement_ideas/47-sequential-priority-fixes-2026-10-11.md)
for results, exact scope, and the diagnostic command.

Audit-46 adds deterministic encoder-statistics/admission and exact capture-loss
accounting, including exact 64 MiB startup boundaries, payload-overflow rejection
and failed oversized startup that creates no output file; saturated burst
retirement; fallback preference retention; completed
photo-library/legacy-staleness recovery; and current/
stale history errors. CUDA fixtures cover opaque-alpha RGB/padding guards, soft
hysteresis/jitter, exact composite alias parity, and automatic document
sharpening skip/chain parity with mixed-scene preservation. Coalescer, translation and
presenter-readback additions remain in GUI targets for serial desktop execution.
Generated codec fixtures use bounded test-only admission retries; production
does not wait for credit. See [review 46](../improvement_ideas/46-additional-opus-review-2026-10-11.md)
for actual results and limitations.

Audit-45 regressions extend the tracked release/CPU/CUDA matrix. See
[the implementation checkpoint](../improvement_ideas/45-codebase-audit-2026-10-11.md)
for actual passes, native focus failures, and deferred checks. A non-GUI subset
does not replace the full release gate; decoded codec fixtures remain separate
from camera/optics validation.

- `recording_manager` constructs/shuts down the real null-button manager 256
  times, checks active/next-session canvas selection, CPU ink snapshot/clear/
  undo/pan with clean originals, and BGRA-to-NV12 layout/color/padding rejection.
  It uses temporary user-data paths and no camera or encoder submission.
- `media_writer_gpu_codecs` now exercises GPU → CPU red fallback → GPU in one
  writer, decodes 90 frames, checks increasing timestamps and fallback color,
  and retains AAC/finalization checks. Hardware absence retains the existing
  skip behavior; a supported encoder failure is a failure, not a skip.
  On native NV12 it also checks that the first source lease retires through
  worker-only, flush-capable idle polling while its encoder sample remains pending, before any
  next video/audio frame or Stop. Compatibility mode explicitly skips this
  ownership assertion. This small-frame fixture does not validate large-mode
  camera/driver support or reproduce the full presenter-pool pressure case.
- `assistive_preparation` uses fixture runtime state and the fake Codex child
  to cover mode-off/explicit cancellation, late preparation isolation, and
  history replies after newer loads/answers. No provider request is submitted.
  Preparation cancellation isolates TEMP/TMP before runtime construction so its
  production stale-file sweep is confined to the fixture directory.
- Capture regressions check requested retry identity after failed opens,
  padded NV12 allocation/chroma, RGBA normalization, and invalid layouts.
  Settings regressions cover physical identity serialization/migration and
  injected credential deletion/settings failures with durable opaque ids.
  They also cover the 16 MiB settings limit. That includes exact-limit and
  one-byte-over files with CRLF counted, `Serialize` matching the bytes on
  disk, a save at the limit round-tripping through the backup, and a refused
  save leaving the primary and backup byte-identical. Oversized primaries fall
  back to a valid backup or defaults and are preserved as `.too-large-*`, and
  a 10,000-conversation capacity case fits. A growing file during the bounded
  read is covered by construction only.
- CUDA regressions cover recovery-keyframe ROI, rejected relative-motion
  continuity, CLAHE edge behavior, and mean-only/full-statistics equivalence.
  View-transform fixtures cover SuperRes containing/canonical crop geometry.
  These assertions do not establish vendor fidelity or measured speed gains.
  CLAHE keeps grayscale cases but reads their device-produced luma to avoid
  host/CUDA fused-arithmetic bin-boundary disagreement; colored cases use an
  independent host luma/bin crosscheck. Spatial interpolation remains independently
  computed with a one-byte output tolerance and exact alpha/padding checks.
- `pipeline_timing_percentiles` checks scene-only arrivals do not restart the
  user-motion tail: a settled 30 FPS camera returns to a 33 ms interval while
  interaction retains 8 ms scheduling on an injected 120 Hz display.
- UI fixtures extend selection/scroll preservation, short Draw rail reachability,
  rotated ink/edit history, and missing-voice retention. Native
  GUI tests remain serial in an interactive Windows desktop session.

`ai_settings_dialog_interactions` exercises the production AI Settings dialog:
clearing a placeholder API key retains the opaque credential id in its result,
the Codex Explain prompt remains editable, switching to a server preserves
hidden Codex preferences without validating its workspace, and active Codex
coding rejects relative or missing folders before accepting an existing
temporary folder. It also checks Tab/Shift+Tab through the shared text editors
and field reachability in the narrow scrollable layout for both providers.
The fixture closes its own validation warnings, uses no protected-secret
storage, account, camera, network, or speech services, and compiles speech
discovery out. Only the unrelated static built-in Codex reference text is
substituted; dialog validation and result handling are production code. Run
it serially with other GUI tests in an interactive Windows desktop session
through `scripts/agent_build.bat` or the tracked CPU/CUDA CTest presets.

`main_window_interactions` creates the production MainWindow and native control
windows without initializing a camera, AI account, or GPU presenter. It checks
tab ownership/order, quick-mode browsing and activation, carousel navigation
from the applied mode, search disclosure and refresh after language changes,
keyboard focus boundaries, dependent controls, and translated Explain/Stop
state. Run GUI tests serially in an interactive Windows desktop session.
For layout evidence, set `OKUFLOW_UI_TEST_SCREENSHOTS` to an output directory;
the fixture saves English, Turkish, and German views including native tool
windows only when the whole capture fits the screen's available work area.
It verifies native ownership/occlusion before and after the grab and records
success/refusal, logical bounds, DPI ratio and physical image size in
`native-captures.jsonl`. Original wide/tall geometry contracts retain their
sizes and assertions; unsuitable optional images are refused, never clipped.
`desktopVisualLayouts` adds required desktop-fitting visual examples for all
three languages. `desktopVisualChat` adds light/dark native Advanced/floating
chat, thumbnails, wrapped text, attachment states, keyboard focus, owner moves
and Approval Details selection/scroll before and after a refresh. These cases
set both Qt's application color-scheme hint and palette, so native style assets
match the labelled light/dark fixture without changing the Windows theme. The
chat footer's physical hit targets must belong to its native window after the
queued splitter layout settles within a bounded wait; the test deadline stays 30 s.
These cases
use actual available geometry including frame margins and taskbar space, with
no OS scale/resolution changes. When screenshots are requested, failure to
capture any of these new examples fails its test. Without a capture directory
the same native geometry/behavior assertions still run. Their private HWNDs
disable opening transitions so captures cannot land in a compositor cross-fade;
no system animation setting is changed. Each layout row additionally proves
that an intentionally off-screen capture is refused without writing a PNG.
Use distinct
directories when repeating with `QT_SCALE_FACTOR=1.5`
or `2`, and inspect the images for clipping as well as checking CTest results.
These tests do not validate live AI responses or screen-reader speech.

`textClarityMasterPreservesRefinements` also checks the disabled-but-preserved
child settings, master refresh after blocked settings restoration, explanatory
text, and nested layout at a narrow inspector width. Settings-store tests cover
the matching effective processing gate and preservation of explicit model
choices when the new default is `gpt-6-luna`.

`color_scheme_picker` checks direct favorite selection without duplicate
signals, programmatic check-state refresh, two/four-column reflow, the complete
More colors catalog, and trigger translation. Run it serially with other GUI
tests; it does not change persisted application settings.

The focused annotation tests `assistantReadingActionsAndSafeDefault`,
`assistantPlacementActionsRetranslate`, `assistiveModesCannotBypassUiSuppression`,
and `suppressedChatRetainsStreamingContentAndDock` can run with
`QT_QPA_PLATFORM=offscreen` and `QT_FORCE_STDERR_LOGGING=1`. They cover readable
answer controls, chrome-safe/RTL defaults, valid saved geometry, Escape draft
clearing after activation/focus settles, placement-menu translation, and mode
changes during UI suppression. Offscreen fonts and window behavior differ
from Windows; this subset does not validate native stacking, hit testing,
desktop focus transfers, or rendered appearance.

`user_data_paths` tests photo-pair reservation and recovery using temporary
directories: retained lone originals/processed images, filename collisions,
failed encodes/renames, interrupted commits, invalid markers, and active
writers. It verifies that existing final JPEG bytes survive recovery and
rollback. No user photos are used by these tests.

The startup/presentation work extends `fence_wait` with nonblocking slot
admission, allocator-before-latency-signal ordering, device removal, and failed
wait handling. `pipeline_orchestrator` keeps an independent Qt heartbeat alive
while a simulated allocator is busy and checks eventual presentation after
completion. `fence_wait` and `view_transform` exercise the presenter-owned
shared timeline: canceled external reservations never become waits, graphics
submissions queue committed CUDA dependencies, busy presents retain that
dependency, and recording slots require producer completion even after an
encoder lease drops. `fence_wait` also models independent D3D11 reader lease
retirement, including pending and unknown completion.
`scripts/profile_startup.ps1` additionally exercises normal launch/capture/exit
against an available physical camera using isolated settings; it is optional
hardware measurement, separate from the deterministic CTest matrix.

`capture_buffer_layout` also copies cropped pixels from a real WARP D3D11
array/mip subresource into an NT-shared BGRA target using the production helper,
verifies every channel after GPU readback, and rejects invalid layouts and RGBA
input requiring conversion. `cuda_surface_retry` additionally exercises a
sequence of D3D11 deadlines, CUDA ownership waits, recovery, session demotion,
late completion, and a fresh session through `CaptureHandoffPolicy`.

The review-37 coverage adds `stabilization_viewing_zoom`,
`capture_buffer_layout`, `yuv_color_conversion`, `cuda_surface_retry`,
`spatial_cache_geometry`, `capture_shutdown`, `fence_wait`, and
`gpu_copy_lease`. Existing annotation and notes targets exercise recorded
scene geometry and ordered asynchronous notes storage. CUDA adds independent
601/709 limited/full-range pixel reference checks, offset upscaler crops,
and stale-pixel guard borders. Driver-stall ownership cases use controlled
worker/event policies; they do not intentionally wedge a physical camera/GPU.

The hardware-independent targets cover settings persistence, canonical
Fill/Fit viewport geometry, cached SuperRes ROI registration, monotonic
presenter-owned CUDA/D3D12 fence reservations, and annotation model/render geometry. Annotation
tests exercise freehand/line/rectangle/ellipse/text creation and rendering,
matched dashed halo/core output, shape-outline hit testing, edit undo/redo,
selection/move/scale/erase, one undo record per scale gesture, permanent
session reset, zoom-scaled hit tolerance, and deterministic
scene-to-viewport rendering. The Windows overlay interaction test verifies
that middle-button drag crosses the separate frameless-window boundary and
that Text uses click-then-type inline placement. It also verifies floating
Assistant input and native-mask exclusion during Draw, movement/resize and
streaming geometry, left/right docking through the nested viewport hierarchy,
hide/show space recovery, floating-position restoration, and complete canvas
occlusion. A pixel test checks annotation rendering's preservation of a
caller-provided exclusion region. Settings tests cover the persisted dock
position, annotation style, shape, and
text-size round trips plus invalid-value migration. Run them with the CPU-only
Windows preset:

The overlay tests also simulate native floating-window moves, checking left
and right preview painting, input transparency, drop-to-dock, both window-edge
and grabbed-title-point targets, moving away, native/Escape cancellation,
click-only gestures, and hiding before a queued drop can commit. They suppress
only the OS modal move command so tests do not move the user's real pointer.
They also exercise repeated edge jitter before and after acquiring the target
latch, brief release-zone excursions, duplicate native move completions,
release/leave/hide before the acquisition debounce expires, latched undocking
on both sides, prevention of immediate re-docking, quick release, returning to
the press point, Escape, and window deactivation during the hold. The simulated
native-move fixture waits for owner activation and isolates synthetic drag
coordinates from spontaneous real-pointer movement; production drag behavior
and deactivation/ungrab cancellation remain intact.
The docking and viewport GUI suites force Qt's console logging so CTest
captures assertion details on Windows instead of sending them only to the
debugger output stream.

`viewport_resize_presentation` drives continuous resize events through the real
`RenderWidget` and D3D12 presenter, checking that back-buffer dimensions update
before motion stops. It uploads a circular source, renders it at narrow/wide
and portrait viewport sizes in both Fill and Fit, and checks GPU readback pixels
for equal horizontal/vertical diameter. Frame submission retries busy slots for
up to two seconds, retaining the first successful readback request without
submitting twice. It skips if no D3D12 device is available.

`recording_integrity_tests` covers exact 15/30/60 and 30000/1001 timing,
long-run fractional-rate drift, real timestamp gaps, missing/backward timestamp
recovery, drop accounting, terminal completion truthfulness, legal recording
state transitions, fixed-canvas orientation and Fit bars, bilinear resampling,
and negative-stride normalization without requiring a camera or Media
Foundation encoder. It also checks cached automatic-stop results and
conservative session-owned zero-byte cleanup, including asymmetric,
small-but-nonempty, symlink, and unknown-size cases. These policy tests do
not inject a real Media Foundation sink failure or prove a retained MP4 plays.

`media_writer_audio_tests` creates a short synthetic video plus 48 kHz mono
PCM tone, encodes them into one fragmented MP4, finalizes it, and verifies
that Media Foundation can discover the resulting audio stream. It does not
open a physical microphone.

```bat
cmake --preset msvc-cpu
cmake --build --preset msvc-cpu-build
ctest --preset msvc-cpu-tests --no-tests=error
```

Run the CUDA suite, including its generated-input replay gate, with:

```bat
cmake --preset msvc-cuda-tests
cmake --build --preset msvc-cuda-tests-build
ctest --preset msvc-cuda-tests
```

`cuda_architecture_coverage` needs no GPU: it runs `cuobjdump --list-elf`
on the built application and fails unless a native cubin exists for every
architecture in the selected inventory (`sm_61` is included with CUDA 12.9).
A missing native image (for example Pascal `sm_61` or Blackwell `sm_120`)
can otherwise leave kernel launches dependent on driver/PTX compatibility.
The gate requires native cubins regardless of embedded PTX.
`cmake/CudaTargets.cmake` supplies the expected inventory and every
CUDA target. `cuda_target_policy` checks both toolkit branches without a GPU.
CUDA 12.9 includes `sm_61`; CUDA 13 deliberately excludes it.

`cuda_toolchain_benchmark` is an explicit `EXCLUDE_FROM_ALL` target, never a
CTest case or CI/release gate. It measures 20 production linear-kernel stages
at 1080p and 4K using CUDA events (20 warmups, 31 batches of 10), reports median
and p95 GPU time, host submission median and output checksums. Inputs are
deterministic; allocations/transfers/readback are outside timing. It uses no
camera, phone, provider, Maxine or native window. The one-time toolchain report
in `docs/cuda_toolchain_comparison.md` records compiler flags, hardware,
correctness checks and limitations; no automatic recurring benchmark is added.

`text_clarity_sauvola_cuda` launches the production Sauvola mask kernel on
pitched, generated luma and independent local-statistics fixtures. It checks
uniform and slightly noisy dark backgrounds, bright and complementary dark
strokes, explicit and device-analysis-selected polarities, and complementary
input symmetry including soft edges. Row padding and a bottom guard must stay
unchanged. The fixture does not duplicate the threshold formula or require a
camera, graphics interop, or stored image. It skips with code 77 when no CUDA
device is available and runs only in the tracked CUDA CTest preset.

CUDA-enabled builds add `stabilization_cuda_tests`. It exercises the
single fixed-reference CUDA path with known translations, rotation, scale,
brightness changes, moving-foreground outliers, clamp impacts, and rejected
measurements. The tests require a near-zero displayed path, verify rejected
frames hold the last correction, check that reference reset preserves continuity,
and cover the optional Extra Stable presentation hold. They also verify
prediction-seeded fixed-reference tracking recovers a 100-source-pixel
translation and that a 4x zoom solve follows the visible textured region
instead of a differently moving full-frame majority. Phase B coverage builds a
real Gaussian pyramid and prepared inverse-compositional reference, verifies
the sharpest lock candidate is retained, rejects a moving foreground during
aligned reference accumulation, and replays a deterministic clamp-impact path
with a brightness change and occlusion. The required gate is at least 90% RMS
attenuation and at most 1 display-pixel P95 at 4x zoom; the reference RTX 4090
Laptop run measured 97.33% and 0.36 px. A high-zoom regression requires
correction beyond the former 12% ceiling. Systems without a CUDA device report
a clean skip.

CUDA-enabled builds also add `spatial_upscaler_cuda_tests`. It exercises the
production FidelityFX FSR 1.0 EASU + RCAS and NVIDIA Image Scaling paths at 2x,
checks that their sharpening controls affect textured input, verifies constant
color and BGRA opacity preservation, and enforces the NIS reference
implementation's 1x-to-2x per-pass scaling range. Systems without a CUDA device
report a clean skip.

The same device test exercises the user-facing Extra Stable mode (internally
named Bump Hold) using real CUDA pixels: it captures a
settled frame, injects a transform discontinuity, verifies the cached frame is
presented while live tracking continues, waits for five stable measurements,
and checks the four-frame crossfade returns exactly to live output.

`tests/fixtures/virtual_camera/` contains the tracked deterministic generator
and CSV expectations rather than generated media. The CUDA replay harness
synthesizes the same four-second sequence in memory, so CTest needs no Python,
ffmpeg, or committed video. It then replays those frames through the production
CUDA kernels. The modeled sequence includes 0.7-3 Hz clamp vibration, a desk
impact and ring-down, temporary focus loss, a lighting change, and a moving
foreground occluder.

Y4M and MP4 outputs are intentionally ignored because they are regeneratable.
For local visual review, the generator can also create an H.264 input preview
and a target/input/output comparison MP4. The comparison consumes the measured
correction trace exported by:

```powershell
build\release-bundle\tests\Release\stabilization_cuda_tests.exe `
  --export-corrections tests\fixtures\virtual_camera\clamp_bump_okuflow_corrections.csv
```

Regenerate the input and comparison media with `ffmpeg` available on `PATH`:

```powershell
python tests/fixtures/virtual_camera/generate_clamp_bump.py
```

Camera capture, D3D12 presentation, encoder availability/finalization on real
storage, mid-recording device-mode changes, and reconnect behavior still
require manual testing on Windows hardware.

UI regressions cover explicit Hide UI/Show UI restoration, mouse-idle fading,
streaming while the assistant is suppressed, larger unsaved floating defaults,
saved and docked geometry retention, and native Draw hit-test exclusion for
both camera actions and the persistent Hide UI control. Keyboard navigation
checks that focus transferred between native control windows keeps chrome
visible at the idle deadline and that F6 restores an explicitly hidden UI.
Assistant overlay checks cover reading-size text, the primary Read Aloud
action, compact placement menu, draft-clearing Escape behavior, and default
placement below live chrome. Native GUI cases must be run on an available
interactive desktop; authored assertions alone are not a passing run.

`notes_html` also runs Read and Explain against the fake Codex app-server.
It validates the Luna 6/low default, transcription versus scene prompts across
English/Turkish/German interface settings without translating source text, real
prepared-image attachment, read-only/no-network policy despite Advanced
opt-ins (ephemeral for Read, stored for Explain), busy-request ownership, and
full reading text in the overlay and notes. These checks require no account or
external service.

`assistive_preparation` drives the fake app-server's `explain-threads`
scenario: two Explains reuse one stored thread while each Read opens a new
ephemeral one, never the Explain thread; a remembered thread the server does
not know is replaced transparently and remembered; deleting or resetting the
Explain conversation makes the next Explain start fresh; Explain answers still
reach the lecture notes. A loopback OpenAI-compatible fixture verifies that
only Explain resends earlier answers, bounded to four, and that a model change
or reset clears them. `settings_store_round_trip` covers `explainThreadId` and
the conversation `explain` flag.

Audit-50 settings publication regressions cover read-only oversized/normal
primaries, a read-only backup, an open primary that denies rename, actual
commit refusal with retained current settings, and startup with a missing
primary plus absent/valid/corrupt backup. They assert no temporary-file leak.
Oversized-original rollback after a forced late commit error is verified
separately through an ignored build of production copies. Controlled insertion
of another writer's replacement primary verifies no-replace restoration;
removing rollback makes the same original-preservation probe fail. Native
disk-full, crash, concurrent-process and file-growth races are not simulated
by the normal gate.
Rotation checks 264 padded/changing frames byte-for-byte against independent
forward scatter; the optimized allocation probe separately establishes reuse.

## Phone/GPU liveness regressions

`deadline_worker`, `capture_shutdown`, `capture_buffer_layout`,
`phone_camera_contract` and `fence_wait` exercise bounded vendor calls, retained
readback pixels/resources, nonblocking mutex admission, padded NV12 crop planes,
packet-without-frame stalls and idle GPU completion checks. WARP texture tests
use fixture pixels and no camera or adb server. These checks do not establish
real-phone + Maxine + Extra Stable acceptance. The NVIDIA runtime may be absent
on a validation machine; report that limitation rather than treating the worker
fixture as a hardware inference test.

`capture_shutdown` also covers the split `BeginStopCapture` / `PollStopCapture` /
`ReleaseStoppedCapture` facade with scripted blocked workers and consumer leases.
It verifies immediate callback/GPU-access revocation, exact backing retention,
the shared deadline, fail-closed early acknowledgments, pending-session transfer,
worker-timestamp completion and legacy callback exception handling. These rows
open no camera, phone, adb connection or native window. They complement the
existing WARP texture cases and do not establish physical-device acceptance.

`codex_agent_client_contract` keeps its pre-ack, stale/retired-turn, restricted
profile and reentrant callback cases and adds 27 scope rows through the private
fake dedicated Codex peer and real `AppControlService`. They cover scene center,
reset, mode listing/activation, five reading-color intents, snapped contrast,
automatic-adjustment admission, exact orientation approval/denial, denied-action
suppression, observation consent and next-turn grants, photo pending/committed
status, owned recording start/stop/retirement, manual/stale stop safeguards,
temporary-source consent and saved intent, native-user revisions, and late
observation/accepted-photo cancellation. The synthetic target uses the actual
`OperationTracker` and `CameraSourceIntent`; fake provider replies expose the
result/error/image counts for assertions rather than treating process exit as
success. Original callback and turn-identity coverage remains intact.

This is synthetic adapter/core coverage only. No production widget transition,
microphone, photo writer, GPU readback, source-reader shutdown, camera/phone/adb
access, account or network request runs here. Observation bytes are an explicit
synthetic transport marker, not a rendered JPEG acceptance test. Camera first
frame and photo/recording completion are fixture-driven tracker transitions;
they do not establish actual production switching or media output. Rendered
accessibility, real provider compatibility, all goal/preset recipes, hardware
quality/cancellation/soak and both-adapter production acceptance remain separate
gates. These rows do not promote a development catalog to `v1`.

`codex_agent_profile_contract` uses a first-party offline CLI feature/schema
fact fixture; no Codex child or live service is contacted. It rejects version/
feature drift, duplicate/malformed inventory and unsafe MCP names, and checks
explicit experimental initialization. `assistive_preparation` separately proves
periodic-mode disable preserves explicit user preparation but cancels periodic
work, retaining the existing explicit Stop regression.

## Agent-control core fixtures (development)

`image_parameter_planner_contract` compiles the production image/view planner
without native app wiring. Its 92 behavior rows cover all 58 independent
registry mappings, whole-invalid-batch rejection, complete custom recipes,
blur normalization, Super Resolution quality/minimum-zoom dependencies and
no-op resets, explicit contradictory zoom rejection, getter baselines and
Text Clarity/consumed-mask truth tables. An invalid built-in recipe selection
rejects an otherwise valid refinement patch with zero published writes.
These pure checks complement the service Begin/Commit/rollback contracts;
they do not execute native image handlers or establish rendering, device or
GUI acceptance.

`agent_control_contract` uses QCoreApplication with a private target/optional
monotonic fake clock. It checks strict schemas, no-ops, duplicate call identities,
user-wins/conflict-aware undo, modal reads, exact approval replacement/replay,
denial without nagging, canceled/late delivery, settings-save failure, post-turn
frame approval without capture, legal operation progress and safe outcomes.
`settings_store_round_trip` covers schema-17 switches/conversation kinds,
legacy Explain inference and unknown-kind tool-free fallback. The added
recording-manager fixture uses a reserved id and proves stale Stop cannot stop
a replacement, with a null button and no window, microphone, camera or codec.
These desktop-free checks do not replace native accessible controls, observation
rendering, real capture/provider acceptance, MCP interoperability or full gates.

`http_agent_client_contract` runs the real Qt Network adapter against private
127.0.0.1 ephemeral-port HTTP fixtures and a fake app target. It covers explicit
capability/profile isolation, header-only fixture credentials, whole-round rejection
before any mutation, modern/legacy transcript preservation, sequential results,
observation placement and image removal, cancellation/late callbacks, endpoint
reconfiguration, refused redirects, fixed HTTP errors, repeated call IDs, round
limits, oversized bodies and incomplete tool responses. Its test process disables
network proxies so the private HTTP traffic cannot be diverted. It is desktop-free and
uses no real credentials, provider, camera, phone, adb server or browser. Passing
fixtures do not establish compatibility with a live endpoint or rendered UI.

### Chat/control offscreen fixture and opt-in captures

`agent_control_panel_headless` always uses Qt offscreen. Its focused cases cover
sole master ownership, user-only grants, exact/expired approvals, canonical 90°
orientation, unchanged approval Details selection/caret/scroll across refreshes,
bounded activity, explicit testing, lease defaults/no widening,
refused drafts, shared draft/attachment/keyboard submission, escaped messages,
selection, completed-only manual TTS and independent Read presentation.
It also compares accessible character rectangles with painted cursor lines at
multiple widths/scroll positions and verifies actual JPEG resources in both chat
views, aspect ratio and cleanup on New. The compact footer fixture checks
non-overlapping 360px layouts in both directions/themes, native checkable
accessibility/Space/full-target activation and composer-to-footer Tab order.
The shared-chat test also checks the floating window's outer Tab bridge inventory.
`main_window_interactions.floatingAssistantTabTraversal` separately exercises
real foreground Tab/Shift+Tab through the floating routing choices and drawer
actions, with the production MainWindow application-wide filter installed.
The annotation fixture checks the current composer-to-footer order rather
than an obsolete absolute index for Read Aloud.
The fixture embeds the production icon resources for meaningful captures.
This offscreen bridge-level check does
not replace manual Windows Magnifier/Narrator acceptance. It starts
no camera/provider/bridge/browser and claims no native focus or screen-reader
acceptance. Serial disposable Windows native cases remain separate.

Explicitly build `agent_control_capture` and run `--output` with an absolute
LocalAppData folder. The real production MainWindow is rendered at 1672×941 to
match the approved chat-first concept, with synthetic chat and pending photo
approval, a settings drawer and AI Settings with built-in control on / external
MCP off defaults. Settings round trips cover explicit opt-out, missing keys and
legacy migration; coding dialog fixtures explicitly disable built-in control.
It loads no settings store or service. Actual peer-widget grabs are composed; installed Segoe UI is
registered for Windows offscreen. Keep originals, retain failed captures and
rerender instead of repairing pixels. See `docs/agent_control_ui.md`.

`codex_agent_client_contract` launches only the private fake JSONL peer. The
preflight produces the committed feature facts and no MCP inventory. Its
scenarios put a valid tool call before the start reply, an orphan/retired call
first, or a forbidden command/tool in the stream; assertions check actual
private-target effects, not wording. The runtime preparation fixture also
checks dedicated control-answer single ownership, exact Stop and one terminal
completion while periodic mode is toggled off. These checks do not establish
live account/model acceptance or native focus/screen-reader behavior.
### Dedicated provider reentrant callbacks

The core's Undo regressions use scripted source ownership and temporary batch/
camera-apply refusals. They verify restore intent before cleanup, no false manual
edit events on empty polls, retained inverses after temporary unavailability,
and revision-based protection of intervening user edits. The coverage fixture
also checks a partially failed custom-recipe inverse remains one cluster across
retry and is discarded together when a component is manually edited. Label coverage
checks canonical/legacy identities against existing catalog keys; it does not
judge a rendered UI by English wording. The opt-in actual-app fixture separately
exercises the trusted CancelAgentTurn path with no real device or provider.

The private `codex_agent_client_contract` also exercises configuration changes
from capability/turn notifications and replacement of a turn from conversation
creation. The fake peer refuses mismatched thread starts. Admission failures
emit no terminal event; bounded text bridges reject images, tool roles, backend
IDs and excessive content. These fixtures use a synthetic child process and
prove no live account or provider compatibility.

### Shared visible chat headless contracts

`visible_chat_session_contract` checks shared drafts/busy state, one admitted
question and placeholder, authoritative terminal replacement, isolated backend
IDs, immutable kinds, retirement fences, safe history restoration and bounded
completed text context, bounded memory-only frame previews and stale-preview
rejection. The store fixture verifies that thumbnail bytes never reach disk.
`assistive_preparation` also prepares authorized control thumbnails on the real
worker pool, checks dimensions/aspect and pixel-free context, then retires a
ticket before queued completion to reject both late preview and continuation.
An invalid optional JPEG must not prevent continuation of an admitted turn.
`assistive_preparation` verifies real thumbnail publication in fake Codex and
loopback HTTP image turns, Read isolation and text-only follow-ups, and adds private loopback
HTTP Explain/followup/Read/notes ownership, held-answer reset, retained tool-free
Codex followup, saved Assistant read-only permissions, frozen opening masks,
distinct Coding/Assistant conversation kinds, explicit Explain after Coding New,
restricted control New and prepared-capture retirement. `notes_html` retains
the Read/Explain language, model, permission, busy and exact notes checks while
asserting full Explain output in shared chat and unchanged chat for Read.
`http_agent_client_contract` validates role-pair context bounds and rejection
of historical pixel/foreign fields before any request. These fixtures use no
provider credentials, camera or native windows. Native layout and keyboard
acceptance remain separate GUI gates.

Agent-control freshness fixtures inject a monotonic clock and verify elapsed age, cached-frame re-presentation, held/future timestamps, and exact-owner revocation of post-turn queued frame consent before the next turn. They use no OS clock changes, media or hardware.

`visible_chat_session_store_contract` uses a private temporary directory and
checks mixed Explain/Agent restart restoration, drafts/plain receipts, strict
invalid-file rejection, traversal/nonopaque ids, known-secret filtering,
pending/tool/image/grant field exclusion, owned retention bounds, kind-locality
and rename/delete. On Windows it locks only its exact fixture record to prove
a failed atomic replacement leaves the previous bytes unchanged. Restored
rows expose no camera grant or backend binding and produce no notes callback.

`assistant_parameter_policy_contract` is a desktop-free QtCore fixture for
whole-group rejection, protected settings, exact Configuration classification,
coupled model/reasoning changes, secret/bounds handling and installed opaque
voice handles. Assistive preparation adds private loopback scoped-scene tests
for independent chat/notes ownership, frame revocation before a held response,
configuration/cancel retirement, native periodic preservation, and a fake Codex
opening held until the submission guard is revoked. These checks never use a
live provider, credential, camera, audio voice or native window.

Scoped vision additionally uses `fake_scoped_codex_vision` (the existing
ordinary fake protocol plus private profile output/argv proof). Runtime
contracts cover pinned isolation, malformed version/features/inventory fail
closed, unchanged ordinary launch and delayed frame permission checks. Pure
preference cases cover Unicode codepoint boundaries, complete 4096-symbol
projections, explicit unreadable legacy-value metadata preserved during mixed
response packing, BMP and supplementary format controls,
hundredth-step speech rate, and native-scene guards. These are synthetic
headless boundaries, not native voice/camera/provider or GUI acceptance.

The opt-in actual-app control fixture requires a private CPU dev-full configure with -DOKUFLOW_ENABLE_NATIVE_CONTROL_FIXTURE=ON. Build native_app_control_fixture explicitly and run only native_app_control_offscreen. It uses actual production Qt controls and private settings under Qt offscreen, bypassing platform/device/provider startup. The separate scripts/test_mcp_native_sdk.mjs validates authenticated stdio/IPC against that real target. These checks establish the executed CPU-native subset, not CUDA effects, physical camera/phone or complete 94-field hardware acceptance.

## Ultra Clear Text fixtures (development)

`ultra_clear_text_contract` (label `headless`) runs under Qt offscreen with
production overlay/panel widgets, a deterministic synthetic printed page
(`ultra_clear_text_test_support.hpp`, no downloaded images) and scripted or
held recognizers. It covers geometry/DPI, residual tracking and rejection,
high-risk tokens, temporal agreement, stale/late/moved results, freshness,
latest-only coalescing, cancellation ownership, bounded destruction, and the
controller's presentation-clock independence while OCR is held.
`ultra_clear_text_windows_ocr` (labels `headless;ocr-runtime`) exercises the
real built-in Windows OCR engine on synthetic dark-on-light and light-on-dark
pages and skips when no OCR language is installed; a skip is not a pass.
Neither opens native windows, cameras or network connections. Native GUI
layout, screen-reader and real-camera acceptance remain separate.
