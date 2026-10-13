# OkuFlow Tests

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
Render all three drafts (`--lang en`, `--lang tr`, `--lang de`) first; it expects
the `_tr.html` and `_de.html` siblings of the selected `_en.html` file.
Run it serially with the Windows GUI matrix. Evidence is not release media.

`phone_camera_contract` checks USB connection-type protobuf parsing without
a Windows bus path, malformed lengths/varints, saved webcam/phone/lens-fallback
selection and actual NV12 output from Windows H.264 and optional HEVC decoders.
First-run selection prefers only a companion that answered its handshake;
saved webcam and legacy numeric preferences still win. Android settings tests
verify a new host session clears only the light. The HEVC row explicitly skips only when the optional decoder probe fails;
H.264 remains required.
It uses no real camera, adb server or GUI. The original gray-scene fixture
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
windows only while its process owns the foreground. Otherwise it warns and
skips the image instead of saving another app's desktop. Use distinct
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
