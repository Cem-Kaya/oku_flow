# OkuFlow Tests

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
prepared-image attachment, ephemeral read-only/no-network policy despite
Advanced opt-ins, busy-request ownership, and full reading text in the overlay
and notes. These checks require no account or external service.
