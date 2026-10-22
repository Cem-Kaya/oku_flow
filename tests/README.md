# OkuFlow Tests

## Live RAM recap production seams

`video_memory_stream_contract` covers actual bounded heap IStream/MF adapters,
overflow/mutation refusal, cursor clones, successful-Finalize sealing and
consumer lifetimes. `video_memory_native_h264` is serial native Windows CPU
H.264 encode/decode/keyframe/PTS evidence using only synthetic memory media.
`video_recap_contract` covers whole-segment time/byte/keyframe-bound eviction,
source epochs and counted evicted pins. `video_recap_native_codec` exercises
the production OpenMemory decoder, exact issued Pause leases, real <=250 ms
neighbours, stale seek cancellation and continued async producer admission
while a consumer holds a prior scene. `video_recap_native_4k30` separately
checks a 65-frame full-resolution 4K30 CPU codec capability and actual compressed
sample count/PTS, including a capture hole; its elapsed
time is not sustained physical camera FPS or five-minute soak evidence.
Transport and opt-in actual-app offscreen rows exercise actual rolling bounds
and exact Freeze before any encoded segment, without camera/audio endpoints.
Model-free composition covers held CPU/GPU sampling, focus markers and rotation.
The app's deferred-receipt rows bind Freeze to the committed epoch/sequence and
clock despite newer ingress, failed cloning, same-capture reallocation and
redraw after latency consumption. Reset removes that authority; unknown timing
refuses manual Pause without entering a pending resume state.
Ring/transport seams cover5→1 minute trimming while a four-minute-old frame
remains pinned at its true textual position, plus natural expiry. Numeric
RangeValue is absent outside the seek window and valid after explicit reentry.

Use tracked `agent_check.ps1` selection and `run_test_gate.ps1` with exact test
names, serial codec ownership and GUI excluded. The granted 2026-10-22 CPU
selection passed 11 of 230 cases, including synthetic AAC/gap verification.
Final source/executable/catalog hashes matched before and after the run; all
initial failures remain archived. The follow-up committed-stamp repair rebuilt
the CPU app/fixture and passed the two-case `native_app_control_offscreen` /
`video_recap_contract` gate, retaining matching pre/post input hashes. That
narrow repair does not rerun the earlier native codec selection.
The full matrix is still required. These fixtures do
not establish native desktop/screen-reader, GPU, camera, long-soak, installer
or release acceptance. See [the contract](../docs/live_ram_recap.md).

## Optional active regional GPU proof

`uct_paddle_tests optionalActiveRegionalProof` is an explicit cached-model run,
absent from normal CTest. It executes full/region/full calls on two real
extracted acquisitions with owned CUDA inputs and no image egress. The last
full call retains the regional input's exact acquisition; it cannot manufacture
temporal corroboration. See the [selectors and scope](../docs/uct_active_regional_ocr.md#explicit-cached-gpu-execution-proof).
The ordinary `uct_paddle_processing` group includes direct automatic-backend
supplemental failure/preparation regressions, without models or a GPU.

## Optional embedded FFmpeg playback

An explicit `OKUFLOW_FFMPEG_SDK_ROOT` registers
`video_playback_embedded_contract`, `video_playback_embedded_missing_runtime`,
`video_playback_embedded_archive_fallback` and `video_playback_embedded_4k`.
These serial, desktop-free codec cases require actual embedded identity, stable
custom I/O cancellation/deadlines, native fallback, frame issuance/retirement,
PCM continuity and retained-lease pressure. No GUI or audio endpoint opens.
The embedded contract also constructs private container metadata beyond the
box-probe budget and injects a failed cursor for a known audio track. Both
must refuse embedded admission rather than certify a partial scan or silently
drop the soundtrack. The generated temporary files contain no external media.
Run through `run_test_gate.ps1` using the independent build directory. The
default native contract and silent audio-output contract remain separate.
`compare_video_backends.ps1` takes the global lease for an explicit finite
same-source diagnostic and retains input/runtime hashes, original output and
process-memory evidence. See [recipe, measurements and scope](../docs/video_ffmpeg_playback.md).

The embedded 4K pressure case uses original `data/embedded-pool-4k.mp4`, a
5,878-byte ordinary (nonfragmented) MP4 with 12 uniform gray steps at 5 FPS,
3840×2160, one H.264 GOP and no audio. SHA-256:
`b2f2b7ca62153b28f185396b72c972ec683aeaaed753fa7be90e1805e27f7583`.
The owner's existing FFmpeg 7.1.1 generated this synthetic fixture only; no
decoder executable or SDK is included in Git or a release payload:

```powershell
ffmpeg -hide_banner -nostdin -loglevel error -f lavfi -i "nullsrc=size=3840x2160:rate=5:duration=2.4,geq=lum='N*15':cb=128:cr=128" -c:v libx264 -threads 4 -preset veryfast -crf 35 -g 12 -pix_fmt yuv420p -an -movflags +faststart -map_metadata -1 embedded-pool-4k.mp4
```

Default native 4K cases still generate their original recorder fixture. The
embedded case preserves its committed input and all timing, lease-pressure,
same-frame reuse, supersession, pause/order and actual endpoint assertions.
Saved fragmented MP4 is explicitly native fallback until the retained strict
native-versus-FFmpeg timestamp precision failure is resolved.

## Accessible video and positioned UCT (development)

`video_playback_contract` checks pure archive/resource admission. Explicit native
`video_archive_native_avc_aac` and `video_archive_native_av1_aac` cases require
`OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1`; they write private synthetic Original/AAC
files, decode before finalization, keep the writer advancing during rewind,
seek the newly verified prefix and check saved-file EOF/PCM/VFR timing. They
run serially under the `codec` label and preserve unavailable-codec skips.

The one-second active-tail clock assertion uses a separate test-owned observed
archive containing exactly one bounded, verified native prefix (at most 16 MiB,
64 KiB copy scratch). Its generation, mutation sequence, committed/verified byte
extents, sample count and endpoint must stay unchanged while media PTS stays
exact. Native encoder/parser output can continue independently; only an explicit
copy of a newly verified suffix lets this archive grow and resume at media pace.
The earlier original writer/paused-reader and live-prefix seek checks remain on
the original recording. Both private files are retained on failure, with prefix
versions and actual state/PTS diagnostics. Writer admission or the first new
fragment does not prove native output has drained; the former unfenced failure
log cannot attribute a PTS change to clock leakage versus newly verified bytes.

`video_playback_native_4k` and `video_playback_native_dci_4k` additionally retain
displayed-frame leases while decoding UHD/DCI clips, check real ready depth,
adaptive capacity and rapid newest-wins seeks. Their latency output is a small
synthetic native request-to-worker-publication measurement, not visible seek
latency or a sustained 4K60 benchmark. The pure contract covers complete working
admission, stride/PAR bounds and lookahead reduction near the ceiling.
`recording_original_native` verifies one successful Original output by held file
identity and accounts for failed codec attempts. These are not optical-camera,
microphone or long-recording acceptance.

`video_transport_contract`, `video_audio_output_contract`,
`video_agent_control_contract` and `immutable_uct_contract` cover the native
widget contract, bounded audio queue, actual service/recording policy and
single-job held-frame recognition respectively. Ordinary audio tests never open
an endpoint; WASAPI hardware acceptance is explicit and separate.

`uct_screen_text_accessibility_scale_1`, `_scale_1_5` and `_scale_2` exercise the
QAccessible text/tree/paint geometry model offscreen. They do not establish
Windows UIA, Narrator, NVDA or Magnifier behaviour. Those checks require a free
native desktop. New semantic content is bounded to one-second publication with
a three-second reader hold; invalid content is revoked immediately.

The opt-in CPU `dev-full` actual-app fixture adds saved-video opening with the
camera offline, corrected finalization/latest-open routing, cursor preservation,
actual displayed N versus deferred N+1 Pause, and unsupported-recipe rejection.
Configure `OKUFLOW_ENABLE_NATIVE_CONTROL_FIXTURE=ON` and explicitly build
`native_app_control_fixture`; run `native_app_control_offscreen` through the
tracked gate. This fixture creates no native capture or presenter window.

Run only affected cases while editing. The tracked final release/CPU/CUDA
matrix remains required; use fingerprint reuse for unchanged passes and keep
GUI excluded when the owner reserves the desktop. A scoped/offscreen pass is
never a full release or native screen-reader acceptance claim.

## Optional PaddleOCR acceptance (2026-10-18)

Processed-image export regressions use the production overlay and image writer.
The offscreen UCT cases compare frozen vector commands with rendered glyph pixels,
replay them on a worker after clearing/changing the live overlay, and check scaling,
protected omissions, chrome exclusion and ink above text. Capturing must not change
the screen's last-painted accessibility plan. The actual-app
`processedPhotoAndDrawingIncludeUctAndInkWithCleanOriginal` case decodes the saved
JPEG pair and drawing PNG, checks sharp UCT letters and ink, and verifies the entire
Original stays clean. These are desktop-free synthetic-image contracts; they do
not establish live-camera readback pairing or native screen-reader acceptance.
The serial `recording_original_native` fixture also decodes changing Original
colours through end of stream while the supplied legacy processed view remains
unchanged. It proves recording continues independently of that display payload,
with the actual CUDA bump-hold surface checked separately.

`OriginalTextureAdvancesDuringExtraStableHoldTests` in `media_gpu_scene_cuda`
constructs a production `CudaInteropSurface` with private scene, cache, Original
textures and a shared fence. Sharp frames must establish a reference; uniform
blur must hold that exact acquisition; recovery must produce a real crossfade
and return to Live. Every frame's Original pixels must equal the current input
exactly, including held/crossfade frames. Missing hold or recovery evidence fails
the test. This window-free CUDA check does not use a camera or record a file;
the native recorder test above independently checks encoded Original output.

`heldFramesReadOnceAndCrossfadesRevoke` distinguishes a repeated held
acquisition from new live evidence. It covers unchecked/missing line checks
and late OCR after a composite probe. The frozen export comparison permits
only one RGB channel unit of premultiplied edge-rounding between QWidget's
backing layer and vector replay; alpha and all placement checks remain intact.
Mismatch PNGs are retained below LocalAppData's `OkuFlow/test-evidence/uct-export-*`.
Native asynchronous hold-refusal rows cover camera loss and recording
finalization separately, preserving Live intent or an unavailable held-frame
receipt as appropriate. They call the actual acceptance boundary without a
device or native window.

`heldAcquisitionRetriesRejectedOcr` exercises the controller's actual worker
queues with synthetic held-image records. Three rows reject the blocked held
job through a crossfade barrier, backend failure or expiry, then require a retry
that reads additional text. The blocked input checksum and drained warm-up
receipts keep the rejection attached to the intended job. Repeated accepted
held frames must subsequently cause no additional recognition calls.

`uct_paddle_processing` reconciles all six compiled tier pins against the
preparation manifest (a declared runtime input). Settings/planner contracts
cover GPU medium/medium and CPU tiny/small defaults, migration, closed enum
validation and independent choices. `ultra_clear_text_contract` replaces a held
worker without joining it on UI or reviving stale results. The opt-in
`native_app_control_offscreen` exercises the four real UCT combos through the
actual target, including saved projection, undo and native user edits.
Real CPU/TensorRT rows remain explicit opt-in: defaults now select different
model pairs. Reuse valid engine plans; do not repeat cold compilation as a
routine benchmark. All of these checks can run without foreground desktop use.

`uct_paddle_processing` is desktop-free and model-free: CPU reference
normalization/region/CTC checks, missing/tampered model refusal and scripted
backend initialization/runtime/exception/cancellation fallback. `uct_paddle_cuda`
compares actual device normalization, quads, padded crops and CTC against CPU
references and verifies two-slot owned snapshots, exact stamps and explicit CPU
readback after source overwrite. Resident-path cases guard both download APIs,
compare GPU colors/ink geometry and motion/cropped-view validation with CPU
references, check complete GPU fingerprints against a CPU SHA-256-tree oracle,
and retain eight probes without pinning the two full-scene capture slots. A
resident preparation failure must remain an input fault with zero image downloads.
It is serial with other `gpu` tests and does not open a native window. Neither fixture requires TensorRT or model downloads.
Input-copy faults are separately tested so one bad frame cannot exhaust the
backend chain. Preparation tests keep CPU service available and promote the
retained GPU candidate only when ready. A separate SDK-free build passes the
same fourteen selected Qt rows, and required-adapter configuration refuses
missing headers explicitly.

The UCT contract additionally holds a device recognizer to prove presentation
clock progress, cancellation ownership and independent probe progress at 16x.
Its device fake enforces two live images and checks at least ten fresh probes
while inference is held; static accepted text stays below ten recognitions
across at least twenty captures. The real CUDA pool holds one lease while
reusing the other for thirty fresh probes, including decimation above one.
The existing real Windows fixture calls `CreateWindowsRecognizer` explicitly;
installing optional Paddle files cannot silently change which engine it tests.
The Maxine navigation target includes the same pool/kernels and QtGui for its
analysis dependency, while its window-free/private-SDK contract is unchanged.

Actual model acceptance is opt-in, outside the ordinary model-free CTest gate:

```powershell
pwsh -NoProfile -File scripts/prepare_uct_runtime.ps1 -TensorRT
# Reconfigure/build the appropriate tracked CPU/CUDA test preset afterward.
$env:QT_QPA_PLATFORM = 'offscreen'
$env:OKUFLOW_TEST_PADDLE_RUNTIME = '1'
$env:OKUFLOW_TEST_PADDLE_GPU = '1'
# Use the configured Qt bin on PATH, as CTest does.
build/msvc-cuda-tests/tests/uct_paddle_tests.exe optionalRealCpu optionalRealGpu
```

These rows fail (rather than skip) when explicitly selected dependencies are
missing. TensorRT acceptance exercises system-memory upload and an independently
owned direct device input, checking six known printed words. First use may
compile model/GPU-specific cached plans on the OCR worker. Do not repeatedly
delete caches or call this a general camera/FPS benchmark. The one-time RTX4090
synthetic cold compilation was about 86 s; cached warm inference was 7â€“38 ms,
CPU 96â€“142 ms across development runs. These are fixture observations, not
live-camera latency, GTX1070 validation, multilingual accuracy or render-tail
acceptance. Keep fixture logs and SDK/model caches under LocalAppData, outside
Git and installer staging. A background gate excluding native GUI is partial.

## Packaging provenance fixtures (RB-001/RB-002)

Run `pwsh -NoProfile -File scripts/test_release_provenance.ps1 -CMake
<cmake.exe> -Ninja <ninja.exe>`. This desktop-free script configures private
synthetic imported targets only; it never builds OkuFlow or installs anything.
It checks corrupted WiX archives, preinstalled-shim isolation, changed/added
tool dependencies, CPU and explicit CUDA toolkit facts under conflicting
ambient variables, stale cache/configuration, staged-application mismatches,
changed runtime/license bytes, missing/changed build seals, and CPU-to-CUDA
reconfiguration retaining the prior CPU executable. Imported synthetic targets
explicitly invoke the production post-build seal command without compiling.
Actual metadata and SPDX generation use
private synthetic payloads and signature results restricted to that fixture
root. No certificate or credential is accessed. Optional `-WixPackage
<reviewed-cached-wix.4.0.6.nupkg>` additionally runs the real pin's version check
and compiles/extracts a tiny synthetic MSI from a verified private extraction,
without network access, signing or installation.
Evidence stays under `%LOCALAPPDATA%/OkuFlow/release-provenance-fixtures/`.
These are script-contract results, not shipping builds or MSI/VM acceptance.

`node --test scripts/test_notes_transcribe_bridge.mjs` checks the actual embedded
notes bridge with controlled asynchronous file-handle barriers. It requires
Node's built-in test/VM modules only and creates no browser, model or media.
These desktop-free publication contracts complement `notes_html_contract` and
the separate native Chrome handoff fixture; they do not replace browser/device
acceptance.

Test evidence hashes each distinct file's bytes afresh before and after every
invocation, including resume. Length and modification time only optimize
duplicate references within one fingerprint pass; persisted digests are not
authority for skipping a fresh hash. Metadata-preserving replacements must
invalidate previous passes. The post-run pass clears its memo before rereading
runtime and case inputs.

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
Before building, the workflow requests a 1920Ã—1080 display on that disposable
VM and verifies its actual size. The default 1024Ã—768 clips large native windows
and changes geometry restoration. A refused/unchanged mode cannot pass GUI
acceptance; the `desktop` manual scope checks only this prerequisite quickly.
The first remote preparation verified session 2, the Default input desktop and
an actual 1920Ã—1080 mode change; no owner display settings were changed.
The contrast regression checks actual light/dark text pixels before and after
a runtime scheme switch. Inspector panes explicitly paint the matching Window
surface; native-style rendering cannot silently substitute a white pane.

The Qt downloader is pinned to an upstream aqt commit that supports Qt 6.11+
split Windows repositories; released aqt 3.3.0 cannot locate this SDK.
Qt SVG/Linguist come from the base package; Multimedia, Speech and Image Formats
are explicit additional modules.
The host-protocol capture fixture streams until the client closes and has a one-second
capture window; its packet/control assertions do not depend on 50 ms scheduling.

In GitHub **Actions â†’ Windows checks â†’ Run workflow**, select `background`,
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

`main_window_interactions.bottomChromeRetainsNativeSizeDuringRefresh` is a
serial native regression for folded toolbar flicker. Its six data rows cover
wide controls, folded quick modes and folded quick modes/actions in LTR/RTL.
It counts every native peer's synchronous Qt Resize event during unchanged
activation refreshes, small window resizes within a layout range and native
owner moves. The final sizes must remain stable and within the viewport.
All four folded rows fail against the original live-trial-layout code, while
the wide rows pass. Keep that failed evidence; an end-of-loop screenshot alone
cannot detect the temporary wide windows. The ordinary translated geometry,
focus and scaled movement tests remain separate checks.

The toolbar repair's final tracked background matrix passes Release compilation,
108 CPU cases (two contain optional skipped Qt rows) and the 17-case CUDA delta,
with no whole-test skips or unresolved results. The six focused native GUI cases
pass separately with no skipped rows. A 120-second isolated-settings runtime
smoke also presents real 1280x720 virtual-camera frames through the CUDA/D3D
path and exits normally, preserving the source settings hash. The profiler's
incidental timing samples are not a new flicker/video-latency benchmark. The
native regression supplies the interim resize-event evidence; end-state manual
captures do not measure every compositor frame. Both requested Opus review
attempts failed at the provider's API rate limit and produced no review.

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

`canceledUsbRecoveryStopsPromptly` cancels from the fake server's health-probe
callback, after its acknowledgement, rather than polling for arrival within one
second of worker launch. Immediate and deliberately delayed (1,250 ms) startup
rows share the same 500 ms cancellation limit, measured with steady-clock
timestamps at cancellation and production-call completion. Setup has a separate
five-second bound and reports observed services if no probe arrives. Exactly one
health probe, a false recovery result and zero reconnects remain required. This
is private loopback fixture evidence, not physical-phone recovery acceptance.

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
Fake-clock startup rows retain the original 12-second host deadline through
negotiation/codec retry and its one-second decode reserve. They also check a
stalled first mode followed by a decoding fallback, CSD without real input, the
four-second HEVC clock after real input, and the unchanged 12-second silence
bound. Android's `CameraStartupPlanTest` uses the production planner with a fake
monotonic clock: stalled-first/fallback-success, one/two/six candidates, returned
unused shares, depleted budgets, queue/setup time and retired-token cancellation.
Run it with the tracked `scripts/build_android.ps1 -Lint` workflow. These timing
fixtures do not validate physical Camera2/codec-driver latency or cancellation
inside a blocked platform call.
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
- `media_writer_gpu_codecs` now exercises GPU â†’ CPU red fallback â†’ GPU in one
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
sole master ownership, user-only grants, exact/expired approvals, canonical 90Â°
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
LocalAppData folder. The real production MainWindow is rendered at 1672Ã—941 to
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

The desktop-free contract includes padded Paddle-box ink fitting (title,
descenders and accents), rendered source-gray contrast, busy-slot projection
retention without watchdog renewal, bounded missed-probe tolerance, and refresh
retention that keeps old reference/geometry/identity rather than lending proof
to a new reference. Review regressions include 300 alternating successful/busy
pan attempts with held OCR and zero overlay Hide/Show events, resize refusal,
duplicate-retention prevention, fair probe order, palette hysteresis, neighbouring
line exclusion and recovery of a CTC edge undershoot. Native pan smoothness and exact camera glyph fit remain
separate manual/native acceptance; the offscreen cases do not verify DWM timing.

The review follow-up adds fail-before/pass-after cases for presentation timeout
and failed-attempt restoration, ordered omission gaps, plate/glyph eligibility,
and reading-pane cursor/selection/scroll retention. The real Windows OCR rows
also cover a printed region surrounded by the opposite brightness. These
synthetic results do not establish accuracy on real boards or paper.
`main_window_interactions.ultraClearTextOverlayFollowsNativeViewport` is a
separate serial native GUI case with known words, Fill-mode pan/zoom, resize,
owner-hide and complete screenshot evidence. Fit intentionally ignores zoom;
the pan test must establish that its source rectangle actually changed.
Matching/admission cases also preserve a dense incumbent through sparse aliasing
without losing the coarse proposal's odd-pixel neighbor, prevent duplicate text
after return pan, reject seven kinds of borrowed validation proof, retry terminal
initialization explicitly, and preserve dense-word polarity. The held-OCR
presentation test establishes its asynchronous worker before counting the fixed
300 presentations; it does not count worker-startup scheduling as rendering work.

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

## Required release capabilities (TB-002)

The default `run_test_gate.ps1 -CapabilityProfile development` preserves optional
skips and reports capability acceptance as `NotApplicable`. It is useful CPU/CI
coverage, not evidence that unavailable hardware was exercised.
`windows-cpu` requires the entire unfiltered CPU inventory, with every native
`gui` case and the settings/recording/runtime/phone/control/MCP boundary cases
strictly `Passed`. `windows-cuda` additionally requires all six CUDA kernel/cubin
cases and `media_writer_gpu_codecs` (GPU-fed H.264). Their missing/skip/Qt-row-skip
states block acceptance. Optional AV1 is not required on Pascal; its absence
inside a successfully exercised H.264 fixture is reported separately by that
fixture. `windows-cuda-kernels` permits the CUDA delta selection but still
requires each of its six registered cases in that selection to pass.

The bundle script chooses CPU/CUDA profiles from its requested build and the
runner verifies the configured cache matches. The ledger records a separate
`capabilityChecks` receipt with `Passed`, `Failed`, `Blocked`, `NotRun` case
states and exact result fingerprints. Original CTest skip/failure evidence is
never rewritten. Filtered GUI runs cannot pass a shipping profile. The explicit
emergency untested-bundle path remains visibly untested. These minimum automated
profiles do not establish Maxine quality, live provider access, physical phone
recovery or clean-VM installer acceptance. UCT real OCR runtime coverage remains
separate until that experimental feature's shipping profile is defined.

Run `scripts/test_test_gate.ps1 -CMake <cmake.exe>` after policy changes. Its
private command-line fixtures cover required missing/skipped/row-skipped/failed
capabilities, exact fingerprint binding and CPU/CUDA build mismatches without
opening GUI windows or using a GPU.

UCT follow-up contracts exercise actual cached glyph painting during held OCR,
worker-outcome synchronization, processed-scene provenance (including deferred
processing/CPU fallback), immediate source retirement, strict mixed-height
reading order and same-word color refresh. The GPU readback fixture uses pooled
immutable vector handoff; CPU presentation copying remains explicit. Dense 4K
residual costs are reported separately from UI/presentation callback costs.
The 33-case UCT contract and installed Windows OCR passed in both CPU and CUDA
builds on the development PC; this is not physical-page/GTX1070/UIA acceptance.

Android host protocol selection includes `scripts/android_camera_probe.py`, which
its tests import outside the Android tree. Probe-only edits select the Python
host suite without rebuilding Windows. CI also selects the job for workflow
and impact-manifest changes. The change-selector fixture checks this dependency.


### Recording fallback and marked AAC timeline evidence

The recording manager fixture injects failed AV1 metadata in either leg and
verifies distinct H.264 output with all failed bytes retained. Native codec
fixtures check exclusive sink creation; the GPU fixture rejects a native sink
after metadata and then encodes through worker readback. The opt-in AAC gap
run compares decoded 440/880 Hz audio and red/green video markers. See
[the timeline contract](../docs/recording_attempts_and_timeline.md) for settings
and evidence limits. Fake manager passes do not establish codec behavior.

## Bundle publication and toolchain fixtures (RB-003 / RB-004)

Run from a Windows PowerShell 7 terminal:

```powershell
pwsh -NoProfile -File scripts/test_bundle_publication.ps1
$env:QT_ROOT_DIR = 'C:\Qt\6.12.0\msvc2022_64' # actual SDK; CTest supplies its configured path
pwsh -NoProfile -File scripts/test_bundle_toolchain.ps1
```

Publication fixtures use synthetic manifest-owned bundles under LocalAppData,
including a locked secondary DLL and a private copied command interpreter
started without a window and exited normally through redirected stdin. They
assert byte-for-byte old-generation preservation, running-image refusal,
primary/secondary exchange, rollback after injected rename/verification errors,
and rejection of owner output/captures, unknown files/directories, tampering,
external staging and junctions. Failure injection loads only the actual helper
function definitions into the private fixture; production exposes no bypass.

The toolchain fixture invokes the actual batch `--check-toolchain` entrypoint.
It checks default Ninja/VS2022 x64/Qt prerequisites, refuses Visual Studio,
Ninja Multi-Config, Makefiles and generator overrides, and proves stale cache
bytes survive refusal. It creates no build tree or release payload. Run it on
a machine with the documented VS2022/Qt SDK installed. Both commands preserve
private fixture evidence and establish scoped transaction/preflight behavior,
not application compilation, GUI/hardware, MSI or full release acceptance.

CTest splits publication into `bundle_publication_contract` (three exchange,
locked-DLL and running-image scenarios), `bundle_publication_refusals` (seven
owner-data/inventory/path scenarios) and `bundle_publication_rollback` (three
rollback/late-owner scenarios). Each retains a 60-second limit and asserts its
scenario count; the direct script defaults to `-CaseSet All` for all 13.
Named BEGIN/PASS timings identify the last reached scenario on failure.
`bundle_toolchain_contract` remains separate. Every case declares absolute
helper/runtime inputs and carries `headless;packaging`. All publication groups
retain the native process-identity resource lock and run serially because the
exchange group owns a hidden OkuFlow-named console child. No free owner desktop
is needed; these are still scoped fixtures rather than full release acceptance.

The toolchain regression receives CTest's configured Qt SDK/source root and
exercises the real batch through a private relocated SDK junction whose path
contains spaces. It asserts that prefix, preventing a local `C:\Qt` installation
from masking missing CI setup. Explicit overrides are cleared only inside the
fixture and restored afterward. Real qmake/deployment, license and module SPDX
checks remain required, with notice inputs fingerprinted. The relocated view
omits only the SDK's `LICENSES` directory and asserts that the real batch
selects `QT_SOURCE_ROOT/qtmultimedia/LICENSES/LGPL-3.0-only.txt`; all other SDK
entries use real directory junctions or hash-verified root-file copies. The
source file must exist even when a local SDK also includes its own copy.
Hosted Qt setup includes the matching `qtmultimedia` source archive for its
Qt LGPL and FFmpeg notices;
it does not install all Qt sources or fabricate license files. The 60-second
limit and exactly one real VS initialization are unchanged.

## Exact recording identity fixtures (audit PW-004)

The notes bridge Node contracts cover unrelated same-named folder trees, matching
name/size/mtime with different bytes, different UUIDs for identical bytes, wrong
returned identity, stale/legacy sidecar refusal, identity-bearing File handoff,
revalidation before sidecar save and bounded 1 MiB reads. A `createWritable`
barrier replaces the media bytes, marker UUID and saved successor transcript
after old validation; the old completion writes only its own UUID target and
cannot overwrite that successor. The tests also refuse legacy basename-only
sidecars without modifying/migrating them. Existing PW-003 barriers
still prove immutable snapshots and ordered writers. `notes_html` additionally
checks native UUID stability, no-clobber refusal after same-length replacement,
escaped generated metadata and the fixed cross-language fingerprint vector.
Native cases were added but must be compiled/run through the tracked gate.
The sibling website's cancellation contracts verify byte validation before cached
result reuse and Stop during a held fingerprint read. No browser/model/device is
started by either Node command. For the opt-in actual Chrome notes fixture, the
recording must be exactly the file identified by the generated notes and its
sibling `.okuflow-media.json`; metadata-only lookalikes are deliberately refused.

## Opt-in performance measurements (NP-010 / PF-001)

`assistant_chat_benchmark` is excluded from the default build and CTest. It
forces Qt offscreen, constructs production chat widgets with bounded synthetic
snapshots and records per-delta p50/p95/p99 CPU/queued-callback timings, exact
selection endpoints and reading-anchor displacement. Its controlled
`DeadlineWorker` sleeps are explicitly separate from actual Maxine timings.
The app-less `deadline_worker` fixture also tests rejection of a concurrent call
without queueing/executing it. See [commands, evidence and limits](../docs/performance_diagnostics.md).


`recording_integrity` also checks the production PCM planner: exact three-second
silence, delayed onset, independent segment reset, one-frame rounding tolerance,
covered overlaps, 44.1 kHz fractional conversion and huge/overflowing timestamp
refusal without consuming the cursor. Native codec passes check exact submitted
PCM-frame totals (144,000 normal, 288,000 for the three-second diagnostic),
then decode and compare actual tone/video markers. Original failing evidence
remains separate from the post-repair run.

The post-repair Windows marked diagnostic passed: three-second-gap PCM
submission was 288,000 frames, with decoded audio/video markers at 3.999333 /
3.999967 seconds (about -0.633 ms). Normal input stayed at 144,000 submitted
frames. This is native synthetic codec evidence, not a physical microphone or
release-matrix result; see the retained-evidence details in the timeline guide.

## Private Maxine adapter lifecycle fixture

The CUDA-only `maxine_lifecycle_tests` target builds the production runtime
adapter with two private fake SDK DLLs under the build's `tests/maxine-fixture/`
directory. `maxine_lifecycle_cuda` is desktop-free and labelled `gpu`; the tracked
CUDA delta includes it. It requires a real CUDA device (CTest skip code 77 when
unavailable) but never loads a proprietary Maxine runtime/model or opens a camera.
Its exact fake DLLs are declared CTest runtime inputs and selected only through
the existing process-local `OKUFLOW_MAXINE_PATH`. Nothing is installed or staged.

```powershell
cmake --preset msvc-cuda-tests
cmake --build --preset msvc-cuda-tests-build --target maxine_lifecycle_tests
ctest --preset msvc-cuda-tests -R '^maxine_lifecycle_cuda$' --output-on-failure
```

Coverage uses actual adapter transitions: repeated teardown dispatch count,
successful reload after retirement, failed partial-load cleanup and retry,
original device/worker-thread ownership, and a controlled blocked frame whose
deadline/destructor/late completion cannot release quarantined SDK resources.
The quarantine case deliberately retains its small fake effect/images until
process exit. Synthetic malloc-backed images and no-op transfers are lifecycle
fixtures, not SR pixel, driver-failure, model-performance or async-producer proof.
This scoped run does not replace the tracked shipping/CPU/CUDA acceptance gate.

The sibling target `maxine_navigation_tests` / CTest `maxine_navigation_cuda`
adds the actual `CudaInteropSurface`, CUDA/NIS kernels and private D3D12 shared
textures. It opens no native window, presenter or camera. A default-off fixture
control fills neural output with a known CUDA pattern. Readback checks prove
navigation admits no model/run, uses the changed current frame's spatial/base
pixels rather than previous neural pixels, and resumes the same loaded model.
Retirement balances effect/image ownership. Both helpers are exact runtime
inputs; the GPU cases serialize. This establishes neither real Maxine output
quality nor pan-to-submission performance.

```powershell
cmake --build --preset msvc-cuda-tests-build --target maxine_navigation_tests
ctest --preset msvc-cuda-tests -R '^maxine_navigation_cuda$' --output-on-failure
```

The same fixture fails each frame stage once (input transfer, synchronous effect
execution, output transfer), verifies exact stage/status diagnostics and skipped
downstream calls, then checks that a later successful frame reuses the original
loaded effect/images and clears the ordinary error. These controlled SDK statuses
do not simulate or establish recovery from an actual device failure.

## Bundle preflight startup bound (CI follow-up, 2026-10-17)

`bundle_toolchain_contract` retains its 60-second deadline and all original
assertions. Unsupported generators still fail in the batch before any setup;
generator overrides and incompatible caches now fail in a configuration-only
helper before VS initialization. Exactly one valid actual-batch invocation
starts the real installed VS2022 x64 shell and checks the full toolchain/Qt
prerequisites. The fixture records a named BEGIN/PASS and elapsed time for each
case and asserts that rejected configurations never reached initialization.

Additional negative checks cannot replace real tools: they forge only version,
architecture and installation-root environment claims, require the full helper
to reject an unregistered root, and prove that configuration-only success does
not certify a toolchain. The positive batch must repair those same claims by
initializing the installed shell, then independently checking its registered
17.x C++ root and exact x64 compiler path. Installer `vswhere.exe` is required
for that identity verification. No fake VS executable or setup script is run.

The installed-root lookup uses `vswhere -property installationPath` and the
known `Common7/Tools/VsDevCmd.bat` path, avoiding a recursive search through the
entire installed toolchain. The bundle's local environment sets Microsoft's
`VSCMD_SKIP_SENDTELEMETRY=1`: compiler initialization and identity checks still
run, while the initializer does not spawn unrelated background telemetry that
can retain a captured child pipe. Setup stage output streams into CTest and a
per-case log immediately, including on a timeout; it is not held until exit.

Run 39047500019 timed out in the positive case on commit `8f4567c`. Its buffered
output identifies that case, but does not establish which inner setup operation
stalled. Keep that original failure separate from subsequent local and hosted
validation. Let `agent_build.bat` initialize its own developer shell; redundant
caller initialization can overflow Windows batch environment lines.
This is a bounded-work correction for the remote timeout, with no automatic
retry, deadline increase or relaxed assertion. Original failed evidence stays
separate from subsequent validation.

## Argon 54 focused regressions

`capture_shutdown` now drives the production `CaptureLoop` with an owned fake
`IMFSourceReader`: a STREAMTICK then a gated read. It injects only the pending
read's age, checks negotiated low-rate grace and exactly one loss signal, and
verifies nonblocking split stop, late-return suppression, phone exclusion and
explicit cancellation. Existing abandonment tests retain the unsafe-driver
quarantine contract. No camera, phone, system clock or desktop is changed.

`settings_store_round_trip` retains actual size-limit/backup, failed publication,
committed-state and protected-secret ordering coverage through the detailed
`SaveStatus` injection contract. `OutboundQueueTest` exercises the exact Kotlin
control/video factories under advisory saturation, reliable reserve, byte/FIFO,
close/in-flight and concurrent-producer transitions. Run the Android build/JVM/
lint command documented in `android/BUILDING.md`; do not install its debug APK
or substitute queue tests for physical USB acceptance.

The Argon recovery follow-up also exercises the actual app through
`native_app_control_offscreen`: reader adoption alone stays pending, repeated
idle drives do not reopen it, successful presentation completes recovery once,
and a second silent adopted reader expires and retires. The pure orchestrator
case preserves the original 30-second window even if backoff schedules its next
attempt later. Synthetic adoption/presentation acknowledgment is not real
camera or native GUI acceptance.

## Independent Maxine producer validation (2026-10-17)

`maxine_async_tests` / `maxine_async_cuda` runs with real CUDA and the same
private fake SDK DLLs as the adapter/surface fixtures. It checks source mutation
after a private snapshot, one in-flight job while the backend is gated, CPU
request coalescing, rejection of each of five ticket fields, consumer-copy
lifetime across reconfiguration, invalidation/destruction before a held SDK
returns, and an explicit retry after an ordinary model-load failure. Output
pixel echo is a test-only CUDA pattern operation, never neural-quality evidence.
All gates are released by fixture ownership, including exception paths.
Weak settings invalidation must preserve the failed-configuration latch; only
an explicit reset permits a same-configuration retry.

The surface fixture now imports a private shared D3D12 fence and exercises
asynchronous snapshot/publication waits and signals. `AwaitNeural` is a fixture
poll with a deadline; production has no wait loop or native window here.
`pipeline_timing_percentiles` separately verifies fast async-work polling between
5-FPS camera frames, returning to idle policy without a user-motion tail.
`neuralPublicationRevokesHeldRecognition` keeps old OCR from being published
when neural enhancement changes pixels from the same source/camera frame.
The surface fixture also verifies immediate native-settings revocation before
the next frame without retiring the healthy model. The opt-in
`native_app_control_offscreen` case
`nativeImageEditsAdvanceProcessingAuthorityWithoutAgent` verifies the actual
app's settings-generation path with no agent target, then restores its fixture
configuration. This is Qt-offscreen, without a native window or real camera.

The strict `windows-cuda` release profile requires all three Maxine fixtures
(`maxine_lifecycle_cuda`, `maxine_navigation_cuda`, `maxine_async_cuda`) in
addition to its existing requirements. The kernel-only profile is unchanged.
A missing, skipped or incomplete required fixture cannot certify a release.
Helpers remain private test outputs and must never enter application staging.

```powershell
cmake --build --preset msvc-cuda-tests-build --target maxine_async_tests maxine_navigation_tests maxine_lifecycle_tests
./scripts/run_test_gate.ps1 -Preset msvc-cuda-tests -Test maxine_async_cuda,maxine_navigation_cuda,maxine_lifecycle_cuda
```

This is background synthetic acceptance of scheduling, ownership and pixels.
Real runtime pan-to-submission, sharpness transitions, optical quality,
physical-phone/camera and installer acceptance remain separate. Immediate
photos/recordings retain paired current-frame spatial/base output while neural
processing is pending; no delayed SR capture queue is introduced.

### Diagnosed external prerequisite repair

Ordinary fresh/resume gates deliberately retain matching Failed/Interrupted/
Unfinished blockers. If the cause is diagnosed and repaired outside fingerprinted
inputs (for example, a missing SDK prerequisite poisoned a CTest partition), use
`pwsh -NoProfile -File scripts/run_test_gate.ps1 -Preset msvc-cpu-tests -ExcludeGui -Test EXACT_CTEST_NAME -RecheckAfterRepair 'Restored the missing original Qt source license prerequisite.'`.
Name only actual matching Failed/Unfinished blockers. Rebuild affected targets
first. The reason must contain at least 20 characters;
this mode cannot combine with Resume, RetryInterrupted or broad selectors. A
failed recheck cannot repeat against the same blocking fingerprint. It runs the
named cases once and preserves other matching passes, the old log/JUnit and
blocker identity alongside the reason. It does not turn a failed run into a pass,
retry automatically, authorize GUI use, or replace missing release acceptance.
Per-case and capability evidence retains repair provenance; the summary names
passes obtained through this mode, including when later reused.


### GPU selected-video playback

`video_playback_gpu_avc`, `video_playback_gpu_av1`,
`video_playback_gpu_4k`, `video_playback_gpu_dci_4k` and
`video_gpu_conversion` are serial, window-free native codec/GPU cases. Opt in
with both `OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1` and `OKUFLOW_TEST_VIDEO_GPU=1`.
They require actual GPU image output and preserve CPU-fallback checks. NV12/P010
conversion exercises real pixels, retained-lease pressure and a forced private
processor-target/copy path in addition to a shared target.

`video_gpu_snapshot` runs production presenter snapshot shaders and owned
readbacks without an HWND. An unattached composition swapchain also exercises
the real scene draw and busy-slot admission. It checks an initially unsignaled
independent producer dependency, exact source/overlay pixels, retained clone
identity, global timeline isolation and resize-style retry. Hardware absence is
a prerequisite skip, never acceptance. `media_gpu_scene_cuda` uses the real
CUDA media scene and exact-adapter private D3D11/D3D12 resources; rotation,
appearance, UCT device identity, repeated reads and init/lease retirement are
checked without a camera or desktop window. Its fake Maxine helpers remain
private test inputs. Added effect/history cases verify shared effect pixels,
paused replay, seek reset, spatial ROI and asynchronous fake neural publication;
they do not establish real Maxine model quality or latency.

Optional absolute `OKUFLOW_TEST_VIDEO_GPU_FILES` CMake list entries register
`video_gpu_format_<filename>` with each exact file in `REQUIRED_FILES`. These
cases require resident Open/Seek/End output for privately supplied codec/profile
fixtures. No encoder or sample download is added to normal tests. Local HEVC
Main10, AV1 10-bit and VP9 evidence uses synthetic files outside the checkout.

The immutable/controller contract includes device-worker CPU fallback, distinct
versus identical corroboration, stale/late/revoked inputs, paced advancing probes,
lease release and no-analysis gap receipts. Gap receipts must preserve briefly
validated glyphs without scheduling work or renewing validation; media and wall
expiry, source/seek/backward changes and late results revoke exposure.

Run these through `scripts/run_test_gate.ps1` after building their exact targets;
`agent_build.bat` still owns final Release/CPU/CUDA integration. Preserve failed
attempts. Window-free passes do not certify native screen FPS, screen-reader
behavior, HDR, physical A/V sync, long soak or installers. See
[GPU contracts](../docs/gpu_video_playback.md).


The media native offscreen fixture also checks first-GPU diagnostic uncertainty,
125 view replays retaining a valid OCR lease, rejection of foreign OCR identity,
retry deadline accounting, new-document display authority and one-second expiry
of an active annotation copy. The CUDA scene fixture holds a private graphics
fence to prove one in-flight processing pass: newer CPU inputs and orientation
changes remain Pending with unchanged scene/fence/history, then the exact owned
upload remains readable before the newest request runs. This bound keeps old
view updates from accumulating behind a seek. Native action-row assertions cover
all six actions including Open video; they still require a free desktop.

### One-time output-buffer experiment

The normal `media_gpu_scene_cuda` contract includes two-output isolation,
backpressure/newest-seek/reset checks and runs full effect/history plus private
fake-neural cases with one and two outputs. It never runs a timing benchmark.

To explicitly repeat the one-time experiment after building the fixture:

```powershell
$env:OKUFLOW_AGENT_EXCLUDE_GUI = '1'
scripts/agent_build.bat --scoped msvc-cuda-tests media_gpu_scene_cuda
$env:PATH = "C:\Qt\6.12.0\msvc2022_64\bin;$env:PATH"
build/msvc-cuda-tests/tests/media_gpu_scene_tests.exe --benchmark-output-buffers
```

Keep raw output in LocalAppData and serialize it with other GPU/codec work.
No native desktop is needed. The comparison uses production CUDA and viewport
shader commands, already decoded immutable inputs and GPU-completion receipts;
it excludes decoder/preroll, VSync, screen scanout and native input scheduling.
Four order-balanced repetitions compare 1080p/4K neutral and text-effect profiles,
with minimum-duration steady phases and explicit target/view changes. It is not
an automatic benchmark or a release FPS requirement. The measured report and
per-trial CSV are in [the result](../docs/media_output_buffer_benchmark.md).
The ordinary app retains one output because the measured tradeoff did not improve
both throughput and tail latency across the target workloads.

The resident UCT controller protocol also has hardware-free CPU oracles for live
camera acquisition (including identical pixels and return from video), selected
neighbour registration and no UI-thread analysis. These do not replace the actual
CUDA fixture. The opt-in `optionalRealGpu` row now checks resident colors, ink
geometry and line registration using real medium/medium TensorRT OCR output,
in addition to verifying expected recognized words. It never downloads models.
GPU parity includes opposite polarities, multiple lines, rejected middle spans,
detached character marks, decimation 1/2/4, CPU descriptor uploads, compact batch
indexing and cancelled checks. Immutable model tests reject neighbour checks
with the wrong selected-job ticket, line count or matching policy.

`uct_paddle_tests residentDensePageLatencyExperiment` is a separate opt-in
one-time worker-latency experiment: set `OKUFLOW_UCT_RESIDENT_PERF=1`, use
`QT_QPA_PLATFORM=offscreen` and the CUDA build/Qt runtime, then save QtTest output
under LocalAppData. It compares 60 synthetic lines after one warm-up and three
measured trials, asserting registration without a fragile timing threshold.
It excludes real inference, capture, presentation and sensor-to-photon latency;
ordinary CTest never reruns this timing experiment.


UCT residual workers validate resident lines in 64-line chunks, preserving checked
prefixes when their budget expires or the consumer cancels. A hardware-free oracle
covers a 1100-line input and exact unchecked tails. The regular CUDA batch case
uses distinct line contents, different motion/occlusion outcomes, an invalid
middle entry and a CPU descriptor; each slot is compared with the CPU oracle.

PR10's original CI evidence retained a 60-second aggregate timeout after 31
completed `immutable_uct_contract` rows. Its suite deadline is now 120 seconds;
per-row QTRY limits/assertions and the full inventory remain unchanged. This does
not classify a timeout as a pass or erase the archived attempt. The native German
bottom-toolbar regression remains required; offscreen cell-minimum and cancelled
video-picker/phone-light cases complement it without desktop or real-device claims.

### Compact video transport and dense UCT checks

`video_transport_contract` now embeds the production icon resource collection
and verifies the one-row controls/scrubber, narrow secondary-row fallback,
translated accessible names, focus, seek actions and state-dependent faces.
For optional Qt-offscreen widget captures, set
`OKUFLOW_TRANSPORT_PREVIEW_DIR` and invoke its wide/narrow layout slots; these
images do not establish native UIA/focus acceptance.

`uct_paddle_processing` covers detail crop coverage, overlap/edge policy, word
reconciliation, coordinate mapping and long-line recognition windows.
`uct_paddle_cuda` compares actual GPU/CPU recognition-window metadata and retains
resident-input/validation/quarantine cases. `optionalDenseImage` is an unregistered,
explicit private-image diagnostic: set `OKUFLOW_UCT_TEST_IMAGE` and optionally
`OKUFLOW_TEST_PADDLE_GPU=1`, then invoke that slot directly. Inspect its actual
text; candidate word counts alone are not recognition accuracy.

The actual-app offscreen selected-media fixture checks 250 view replays with
fresh same-sample allocations at an 8-ms simulated clock interval, retaining the
same UCT lease and retry budget. The external native video fixture includes
3/7/13/23-second seeks in addition to Open/1-second/End checks. Private recordings
and extracted frames stay outside Git. Cancellation and memory-credit limits
remain bounded; the current progress deadline is described in
`docs/video_audio_preroll_repair.md`. Native desktop/display-rate acceptance is separate.

The default `video_playback_contract` also decodes the committed original
`tests/data/long-gop-h264.mp4`: 192×128, 60 fps, 5.2 seconds with a single long
GOP, no audio. It seeks past 120 preroll samples and checks latest-target
supersession, without a runtime encoder, downloaded footage or GPU requirement.
The absolute file is a CTest `REQUIRED_FILES` input. Generation used the owner's
separately installed FFmpeg/libx264, neither redistributed:

```text
ffmpeg -f lavfi -i testsrc2=size=192x128:rate=60 -t 5.2 -c:v libx264 -preset veryfast -crf 32 -g 360 -keyint_min 360 -sc_threshold 0 -bf 0 -pix_fmt yuv420p -an -movflags +faststart tests/data/long-gop-h264.mp4
```

Reviewed SHA-256: `5459ddb3da0ab0819b015d967e5ef1dfd0d513b9efcb45e357d636a8dbc62dca`.
The same contract now exercises the private production seek-budget helper with
synthetic time (strict PTS progress, stalled/repeated/backward/late samples and
the sixty-second absolute cap). The original `paced-audio-h264.mp4` fixture
silently drains native AAC/PCM to reproduce handoff oversupply without opening
an audio endpoint; bounded lead and mute/Resume floors are asserted. Its
provenance, opt-in private-file playback diagnostics and limits are in
`docs/video_audio_preroll_repair.md`. PCM delivery does not prove audible
speaker/device continuity.
The scene-cut portion also holds an earlier issued frame while the decoder is
ahead, resumes to actual EOS, then replays the same-generation tail. This covers
the shared worker's atomic restart/rewind handoff and pending-Resume End guard.
Its four-second wait is unchanged; a failure additionally reports state, held
PTS, ready-frame count, memory pressure and the worker error.
The native AVC/AV1 fixtures additionally copy their own finalized output to a
private observed archive, prove its duration, revoke its bytes by truncation,
then verify finalization retains only historical mapping and denies old reads.
This deterministically exercises the finalizer-duration race; no owner media is
modified. Initial AV1 failure evidence is retained.

The UCT readiness fixture includes `uct_startup_test_cases.hpp` through moc-visible
Paddle test wrappers in the tracked `uct_paddle_processing` command. It checks
Windows preparation bridging, GPU promotion, failed preparation fallback and
cancellation/input faults without real models or downloads. Immutable controller
rows cover playback-to-pause transient faults and revocation of an old retry timer.
The real resident CUDA scheduler case includes a yellow selected word in an
otherwise dark row, checking CPU/GPU local color/ink parity and zero image egress.
Cached accessibility interfaces must reflect actual GPU/CPU backend switches.
`uct_paddle_cuda` also runs all local-color CPU/CUDA parity rows, including
selected yellow buttons, antialiased ink, clipped rectangles and dense glyphs
whose corners contain ink. These descriptor checks use synthetic owned pixels,
not a real-camera or display-color acceptance claim.

The geometry helpers add 48 actual Segoe UI tilted/perspective `Right Hand`
button variants through CPU/resident preparation, with the identical dark
surround and an adjacent rule inside padded search. They assert local polarity,
all glyph corners, word spacing, dot/descender/leading-edge retention and zero
image egress. Varying ink includes a black dot and lighter letter around a gray
measured foreground: retain full geometry or the complete detected word. The
winding-component GPU case requires conservative incomplete fallback at the
64-scan cap; the exact linear CPU queue may fit that pathological case.
These run through the existing `uct_paddle_processing`/`uct_paddle_cuda` gates.

The explicitly selected `optionalPausedVideoProof` also records resident
search/color fit replays in `selectedFitReplay`/`neighbourFitReplay`: per-word
pass/completion and production/replay fit flags, histograms, one warmup and
three CUDA-event durations. This diagnostic runs after the actual workers and
does not enter their durations, admission or pixel-egress counts. It is not
part of regular CTest or a native display/decoder timing measurement.

The opt-in [bounded timing trace](../docs/uct_timing_trace.md) has four ordinary
headless CTest contracts: `uct_timing_trace_contract` checks storage and close/drop
accounting; `uct_timing_controller_contract` follows real scripted worker/admission
tickets; `uct_screen_text_timing_publication` checks consumer publication tickets;
and `uct_timing_pane_publication_contract` ensures a pane-only change cannot
reticket later overlay paints or positioned accessibility. Negative event-count
assertions require a complete, uncontended trace. Native UI Automation discovery,
reader speech and photon timing require separate acceptance.

The immutable controller fixture also checks that the Text Clarity master stops
recognition and revokes text while retaining UCT intent, resumes one-shot work
when enabled, and respects an explicit UCT opt-out. Settings cover missing-key
defaults and saved opt-in/out. Native UCT controls cover the disabled master state
and retained checked preference through off/on transitions.
Paused scrolling regressions keep one region fixed while moving readable words
by 80 scene pixels in the real neighbour. CPU and resident-protocol rows require
all independently checked words at the selected frame's coordinates, no UI-thread
pixel work, no resident image readback and exactly two one-shot OCR jobs.
The real Windows OCR fixture also requires runtime status publication after
recognition, retaining its initialized CPU identity when used as a bridge.

`optionalAutomaticStartup` is an unregistered private-image diagnostic. Explicitly
set `OKUFLOW_UCT_STARTUP_DIAGNOSTIC=1` and `OKUFLOW_UCT_TEST_IMAGE`; optional
`OKUFLOW_TEST_PADDLE_GPU=1` requires eventual usable GPU completion. Prepare the
pinned models and compatible engine plans beforehand; do not clear caches or use
this as a cold compilation benchmark. The real scheduler initializes and runs the
automatic chain on its worker, logging first complete fallback and preferred GPU
results, with 500-ms gaps and a 30-second horizon. It measures candidate completion,
not glyph admission, native presentation or end-to-end latency. Local word-color
diagnostics and word counts are observations, not recognition-accuracy assertions.

`optionalTensorRtBatchComparison` is a separate, unregistered finite experiment.
Set `OKUFLOW_UCT_BATCH_DIAGNOSTIC=1`, select a private source through
`OKUFLOW_UCT_TEST_IMAGE` and use the full pinned TensorRT SDK for the explicitly
authorized wider-plan preparations. It compares capacities 1/8/16, one warmup
and three timed passes each, saving complete outputs and strict decision,
geometry and confidence comparisons. Opt-in CUDA event diagnostics retain
stage, allocation and binding evidence. They do not measure native latency or
peak process VRAM. See [GPU batching](../docs/uct_gpu_batching.md).
`OKUFLOW_UCT_BATCH_TEST_TIER=small` selects a separately authorized small/small
comparison; the default is medium/medium. Output evidence includes exact model
pins. Independent `OKUFLOW_UCT_BATCH_TEST_DETECTOR_TIER` and
`OKUFLOW_UCT_BATCH_TEST_RECOGNIZER_TIER` overrides accept `tiny|small|medium`
and otherwise inherit that shared tier. Wider batches compare against their
own model pair's batch-one output.
The first 2026-10-20 medium/medium comparison retained every output but failed
its strict numerical gate and crashed after QTest cleanup. Small/small and
tiny/medium follow-ups also failed strict agreement but exited normally after
exact State retirement. Preserve the failed evidence; eight and sixteen remain
opt-in, with batch one still the automatic default. These optional experiments
are separate from the normal background CTest passes.

`optionalTensorRtRetirement` is an independent one-pass cached-engine lifecycle
diagnostic selected with `OKUFLOW_UCT_RETIREMENT_DIAGNOSTIC=1`, the private test
image and an explicit inference-only TensorRT SDK folder. It refuses parser and
builder resources, requires actual N16 GPU completion, destroys its scheduler
and polls exact TensorRT State retirement receipts for at most ten seconds.
Receipt completion excludes nested analysis storage/vendor workers; record the
process exit code separately. It is not another throughput or quality comparison.

`optionalCharacterChangeCpu` / `optionalCharacterChangeGpu` are unregistered,
finite verifier diagnostics enabled by `OKUFLOW_UCT_CHARACTER_DIAGNOSTIC=1`.
Select one slot explicitly. They test signs, decimals, operators and similar
letters through production probes, with small-print and synthetic translation
controls; the GPU variant requires resident samples without pixel/feature egress.
Raw case outputs and false accepts/hides are retained under a unique LocalAppData
directory. Diagnostic PASS is report completion, not a passing quality gate for
region reuse. See [character verification](../docs/uct_character_verification.md).

`optionalResidentBaseline` is a separate explicit, unregistered SubmitDevice
benchmark, with cached verified plans, an inference-only SDK, independent owned
input and no image egress. It measures real resident reference/color/ink work
separately from the OCR stream and CPU publication. Seeded paired rounds support
independent tiers at capacities 1/8/16, up to four sources, complete output and
strict comparison evidence. Its model-free selector/comparison/validity and
exact-byte cache checks run in `uct_paddle_processing` via
`residentDiagnosticSelectionAndComparison`. See the finite
[resident protocol](../docs/uct_resident_baseline.md); this is not a native
display/accessibility latency benchmark, and a State receipt is not process exit.


### Frozen resident UCT comparison

The opt-in `optionalResidentBaseline` slot additionally accepts
`OKUFLOW_UCT_RESIDENT_FROZEN_CROPS=1`; no ordinary CTest case enables it. It
requires profiling and a common detector tier and retains at most four owned
source leases. The first full job records detector windows, then timed jobs
assert zero detector calls while replaying those windows across model/capacity
variants. This completes recognizer isolation separately from complete-path
SubmitDevice measurements. The original strict assertion remains unchanged;
`userTenPixelConfidenceVariationPass` is a separate owner-authorized policy.
See `docs/uct_resident_baseline.md` and `docs/uct_batch8_quality_acceptance.md`.


### Shadow regional planner and scheduler

`uct_region_scheduler.*` also covers opt-in active execution with actual scripted
detail calls on current owned input, full-slot priority, execution-time slack,
unsupported/stale/held/failed/ignored requests and deadline-overrun accounting.
The active flag implicitly enables planning; shadow-only and probe-only modes
never execute regions. `immutable_uct_contract` adds full-deadline comparisons
under focus pressure, failed-optional latching, stale callback fences and an
active-mode paused-pair regression. `ultra_clear_text_contract` covers other-pane
pixel proof, spanning protection residuals and per-observation pending tickets
(full A / ROI B / full C and same-acquisition rejection). All use model-free
workers and Qt offscreen. See [execution limits](../docs/uct_active_regional_ocr.md).

`uct_region_planner.*` and `uct_region_scheduler.*` are desktop-free,
independently selectable 30-second Qt slots registered with the existing
headless helper. Scripted complete outputs test spatial IDs, coverage debt,
priority and source/acquisition boundaries; real scheduler tests assert full
output preservation, disabled observation, held duplicates, backward seeks
and failure injection. They never establish OCR reuse safety or model accuracy.
The optional resident diagnostic can observe actual TensorRT/scheduler jobs
with `OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER=1`, requiring
`OKUFLOW_UCT_RESIDENT_FROZEN_CROPS=1`; repeated source copies retain their
acquisition and are rejected as duplicate shadow evidence. Observer timing
is recorded separately from OCR work and actual callback delivery.

`uct_region_planner_benchmark` is excluded from ordinary builds and CTest.
Build that explicit target in the CPU preset, then run it with `--run
--repetitions 12 --warmups 2 --output <new-local-evidence.json>`. It reports
metadata bookkeeping cost for 17 finite scenarios, including 8192 words,
continual newcomers and heavily overlapping boxes. Raw accepted/rejected
timings, nearest-rank summaries and invariant failures are retained; no timing
threshold is asserted. This model-free experiment measures no inference,
native presentation or accessibility latency. It needs Qt Core/Gui and no
camera, fonts, model runtime, GPU or native window.

### Tilted text and paused visible detail

`uct_paddle_processing` includes oriented fit/mapping, post-fit duplicate and
actual projective-paint cases. `immutable_uct_contract` includes settled paused
pan/zoom, coalescing, bounded coverage and failure/source fencing. The new
`ultra_clear_text_contract` quad-reference cases assert unchanged rotated ink
passes strict matching while changed ink, malformed quads, high-risk tokens and
reused/foreign acquisitions do not. `uct_paddle_cuda` compares CPU, uploaded
compact CPU and native resident references at 1/2/4 decimation with rotation
and perspective, retaining zero image download on the resident path.

The shared `uct_quad_reference_test_support.hpp` uses installed Segoe UI and
first-party synthetic text. Optional real-image diagnostics can write directly
to an absolute `OKUFLOW_UCT_PAUSE_OUTPUT_ROOT`, preserving the default when
unset. See `docs/uct_oriented_detail_repair.md` for bounded six-times detail,
private-image evidence and the distinction from native desktop acceptance.

### Paused UCT continuity and scene cuts

`mergeKeepsIndependentColumnsAndTypeSizes` in `uct_paddle_processing` checks
both merge orders, the title/script/diff example, separated columns and contained
prefix fragments. The UCT contract adds file-path versus notation filtering and
retention of a current-proved line through half-pixel exclusion-box jitter,
including unchanged validation clocks and eventual expiry.
`mergedHeadlineAndFilePathPaintTogether` draws a synthetic title and file row,
runs the real merger/model/overlay, and requires drawn glyph geometry and white
ink for all five target words. Regional-scroll fixtures keep a separately
pixel-validated static word through detector omission, assert unchanged proof
clocks, and reject explicit uncertainty and later pixel changes. The controller
partial-invalidation case exercises static-line omission through the real
probe/scheduler/admission path in live and advancing-video modes. CUDA matching
also checks unchanged left ink while a large right region changes enough to
fail whole-scene stability; CPU/GPU verdicts must agree.
The explicit paused-video diagnostic now also instantiates the production
glyph overlay under Qt offscreen, registers Segoe UI, records actual
`paintedWords`/`paintedCount`, and saves the production `overlay.png` snapshot.
Model eligibility alone is not treated as painted coverage. These artifacts
remain separate from native-window, UIA and screen-reader acceptance.

`immutable_uct_contract` covers late optional evidence without a selected-frame
restart, partial preferred-backend coverage, independent contradiction revocation
and unchanged deadlines. `ultra_clear_text_contract` checks exact-source coverage
union, source/digest negatives, stable word identities, reading/UIA deduplication,
protected omissions and bounded growth. These are offscreen scripted contracts.
See `docs/uct_paused_continuity_repair.md` for the private real-model diagnostic
and the original retained failures; they do not establish native screen-reader
or camera acceptance.

Follow-up rows inject optional refinement failure, CPU-to-resident promotion,
persistent word-local conflicts, and partial real page invalidation with 24
surviving lines. Local-color cases share controlled surfaces and tiny/clipped
bounds between CPU and CUDA, asserting raw descriptors separately from renderer
contrast. The accessibility fixture replaces changed same-ID text immediately
under a reading hold while unrelated new lines retain their cadence.

The sparse VFR/AAC native fixture obtains its five tail timestamps from a
separate bounded CPU `IMFSourceReader` before opening `VideoPlayback`. Playback
must deliver that exact ordered sequence, including after optional Pause proof
finds a full future queue. This avoids equating authored 100 ns timestamps with
the container/native decoder clock or introducing an arbitrary timestamp tolerance.

The first-party native playback fixture `tests/data/uct-scene-cut-h264.mp4`
is 2,449 bytes, SHA-256
`1f6b4f1207daf315658b0becd72ddb691899258b7fe1bf51b95522c372dd4ecb`.
It is 192 x 128, 10 FPS, 30 frames, three seconds, no audio. Red changes to blue
at 1.0 seconds inside one long H.264 GOP. It uses no downloaded media or fonts.
The runtime test needs Windows decoding only; generation uses separately
installed FFmpeg/libx264 and is not part of CI or the application:

```powershell
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'color=c=red:s=192x128:r=10:d=1' -f lavfi -i 'color=c=blue:s=192x128:r=10:d=2' -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0,format=yuv420p' -c:v libx264 -preset veryfast -crf 28 -g 30 -keyint_min 30 -sc_threshold 0 -metadata 'creation_time=1970-01-01T00:00:00Z' tests/data/uct-scene-cut-h264.mp4
```

Do not overwrite the reviewed fixture to obtain a pass. Encoder versions can
produce different bytes; a deliberate regeneration requires inspecting the
cut/PTS, reviewing the new hash and updating its declared CTest runtime input.
The regression observes the native decoder's actual red-to-blue transition
and adjacent 100 ms samples. It prints the authored/native cut offset instead
of assuming timeline equivalence: on the development machine, the first blue
native sample was at 1.2 s while FFmpeg placed it at 1.0 s. This separate
reordered-H.264 timeline discrepancy is not repaired or accepted by this test.
## Recognition-width validation

The model-free `orientedInkAndMapping` and `gpuOrientedInkMatchesCpu` slots
also cover a readable nine-pixel Segoe UI word inside 32-pixel detector padding.
The complete body must fit on CPU and CUDA; a two-pixel rule must retain the
detected geometry. These controlled geometry checks do not prove OCR accuracy.
The same fixture distinguishes a seven-pixel projection-rounding error from
a genuinely sub-threshold body. Post-fit finalization covers unfitted/fitted
near-vertical single scalars and multi-letter words separately.

`uct_paddle_processing` includes `recognizerWidthPolicyKeepsBoundsAndOrderedGroups`,
`recognizerWidthRectificationRetainsUsefulAspectSamples` and
`recognizerWidthProfileRejectsRenamedFixedPlans`. `uct_paddle_cuda` includes
`gpuRecognizerWidthMetadataDoesNotReorderOrReadPastCount` and
`gpuRecognizerWidthRectificationMatchesCpuAcrossCanvasTransitions` with actual
CUDA buffers, initialized metadata, fractional/clipped source coordinates,
nonmonotonic canvas changes and prefix/inactive-tail guards. They require no
models. Prepared TensorRT output/CTC transitions and recognition quality remain
separate opt-in evidence: see [width protocol](../docs/uct_recognizer_width.md).
