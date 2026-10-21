# OkuFlow Documentation Guide

[Paused UCT continuity](uct_paused_continuity_repair.md) records the reproduced
partial-refinement and late-neighbour defects, exact-source retention and the
private resident recognition/admission diagnostic.

[UCT software timing](uct_timing_trace.md) describes bounded opt-in worker,
admission, software-paint and accessibility-publication metadata, with explicit
dropped/incomplete chains and separate native screen-reader acceptance.

[Shadow regional planning](uct_region_planner.md) describes the opt-in metadata
observer, source-bound spatial occurrences, requested focus and finite coverage
debt. Full OCR stays authoritative; no recognition reuse is enabled.

[Batch-eight quality acceptance](uct_batch8_quality_acceptance.md) records the
owner-approved 10-pixel geometry tolerance, exact text/disposition checks, the
affected microphone icon, and the development GPU default change.

[UCT performance roadmap implementation](uct_performance_roadmap_2026_10_20.md)
tracks the revised performance-first stages, resident measurement boundaries and
quality prerequisites before regional reuse.

[Character verification](uct_character_verification.md) records the finite
CPU/resident change-sensitivity diagnostic and its blocked OCR-reuse gate.

[Resident UCT baseline](uct_resident_baseline.md) describes the owned-device
model/batch comparison and exact measurement boundaries.

[GPU OCR batching](uct_gpu_batching.md) describes dynamic TensorRT batches,
allocation ceilings and the finite latency/quality comparison.

[UCT color and readiness repairs](uct_color_startup_repair.md) cover local word
colors, active GPU/CPU status and paused-frame recovery.

[Compact playback and dense UCT repair](video_uct_detail_repair.md) documents
same-frame pan continuity, bounded GPU detail/long-line recognition and long-GOP
seek handling in the development player.

[Accessible video implementation](accessible_video_implementation.md) records
the corrected recording/playback contract. The [native archive/player](video_archive_backend.md),
[document lifecycle](video_document_lifecycle.md), [transport](video_transport.md),
[Original recording](original_only_recording.md), [presentation](media_presentation.md),
[sound](video_audio_output.md), [immutable UCT](uct_immutable_controller.md),
[OCR uncertainty](uct_uncertainty_repair.md) and [agent actions](video_agent_control.md)
describe the development implementation and its separate acceptance boundaries.

[GPU video playback](gpu_video_playback.md) documents native GPU conversion,
private CUDA/presentation ownership, exact image actions and CPU fallback.

[Positioned UCT screen text](uct_screen_text_accessibility.md) describes the
passive Windows accessibility model. [Video/UCT language review](uct_recap_localization_review_2026_10_18.md)
records complete small-agent review of the new UI copy.
[Review dispositions](uct_recap_review_2026_10_18.md) separate confirmed repairs
from adapter limitations and native acceptance still required.

[Ultra Clear Text](ultra_clear_text.md#paddleocr-development-backend-2026-10-18)
documents optional local PaddleOCR/TensorRT, CPU/Windows fallback, independent
pre-zoom analysis, preparation, transfer boundaries and acceptance limits.
[Temporal pause status review](uct_temporal_status_language_review_2026_10_19.md)
records full small-agent review of the new 24-language Extra Stable message.
[Opus implementation follow-up](uct_opus_review_2026_10_20.md) records the held-
acquisition retry repair, registration experiment and remaining coverage limits.
[Implementation review repairs](uct_paddle_review_2026_10_18.md) distinguish
verified fallback/ownership regressions from remaining camera/release acceptance.
[Pan continuity and source-style review](uct_pan_style_review_2026_10_18.md)
records the busy-slot, bounded refresh, ink-fit and stable-color repairs.

[Argon report verification](argon_review_verification_2026_10_17.md) records
every finding's disposition, confirmed repairs and intentional state/tool contracts.

Website: <https://okuflow.com>

[Application languages](localization.md) describes Windows 0.2.4's published
25-language coverage, the Android Alpha submission, notes/Transcribe coverage,
system defaults, RTL behavior and remaining native-speaker/device acceptance.

[GitHub Windows checks](../tests/README.md#github-actions-away-from-the-owner-desktop)
describes remote CPU/GUI validation, selectable diagnostics and evidence artifacts.

[One-time CUDA toolchain comparison](cuda_toolchain_comparison.md) records
CUDA 12.9/13.4 kernel measurements, the two tested MSVC compilers, Pascal
build coverage and the separate physical GTX 1070 acceptance requirement.

[Translation review continuation](localization_review_2026-10-14.md) records
the complete assigned Luna catalog scopes, failed-attempt recovery, per-language
correction counts, rejected proposals and local validation limits.

[Android CLI publishing](android_cli_publishing.md) explains owner-controlled
Play API setup, private DPAPI credentials and validated testing-track releases.

[Windows distribution](windows_distribution.md) covers MSI builds, shortcuts,
signing, winget drafts, the account's Store identity, structural MSIX review
packages and the remaining clean-VM/Store runtime acceptance work.

Recorded-video notes now hand off speech processing to the static Chrome tool
at `okuflow.com/transcribe/`. The Windows app only records and generates the
local notes viewer. The browser uses folder grants, a File/MessageChannel
handoff and versioned sidecars; it never edits the app-owned HTML or uploads
the recording. See the sibling website's `docs/browser-transcription.md` for
model profiles, caching, security headers and browser validation.

The 2026-10-12 removal/handoff implementation passed the tracked Windows matrix:
shipping release compilation, 31 CPU tests and 36 CUDA tests, with GUI fixtures
included and run serially. The website's Chrome fixture separately passed local
recording recognition and notes-sidecar save/restore. Native Chrome folder-grant
UI/persistence and long-recording acceptance remain separate manual checks.

OkuFlow is a Windows-only live magnification application that combines:
- Qt 6 for the desktop shell and input handling
- Media Foundation for camera and microphone discovery, frame capture, and
  live media encoding
- Direct3D 12 for presentation and GPU texture management
- CUDA for optional GPU processing through D3D12 external-memory interop
- Qt Linguist catalogs embedded in the executable for live switching among
  [25 application languages](localization.md)

## Architecture At A Glance
The current frame flow is:

1. Initial device opening runs on an independent COM worker while the native
   window/presenter initializes. Mode discovery reuses the streaming reader;
   cancellation revokes delivery without retaining the application object.
   `MediaCapture` enumerates cameras and streams frames from the selected
   device, preferring NV12, then YUY2, before BGRA formats. Automatic mode
   first creates a D3D11/DXGI-backed Media Foundation source reader, validates
   timestamps and image range for 30 startup frames, and transparently reopens
   the conservative system-memory reader if that camera/driver fails. The
   accelerated reader retains DXGI textures. Matching BGRA uses a direct GPU
   rectangle copy; D3D11 converts other supported formats to a reusable
   NT-shareable BGRA allocation, D3D12 opens that allocation to provide its
   exact resource description and allocation size, and CUDA imports it as
   external memory. A delayed D3D11 completion query retains the same MF
   frame for event-loop retries after a single nonblocking query; the existing
   25 ms budget bounds the D3D11 handoff; three consecutive deadlines select
   safe-copy input for that session. CUDA copy ownership waits retain their
   independent completion/fault rules. CPU buffers preserve native pitch before tight
   normalization, and YUV color matrix/range metadata follows each frame.
2. The direct-GPU rung copies that imported BGRA array device-to-device into
   the established CUDA working surface without a CPU bounce. A completion
   event retains a producer lease until the copy finishes, so the UI need not
   synchronize the processing stream and the conversion texture cannot be
   reused early. Lower rungs
   upload compact NV12/YUY2 planes; conversion and rotation still run in CUDA.
   Host uploads use a three-slot page-locked ring whose per-slot event guards
   reuse. Busy admission queues no GPU work and drops the arriving camera
   frame while retaining the completed preview, without recovery or CPU fallback.
   `CpuFramePipeline` converts/rotates only remaining formats, debug
   composite view, GPU-unavailable passthrough, and fallback frames.
3. `CudaInteropSurface` runs the GPU effect chain when the interop surface is
   valid and publishes the completed camera generation as a persistent scene
   texture. Its first image stage is a device-resident fixed-reference
   stabilizer: CUDA Harris/Lucas-Kanade tracking at up to 640x360 feeds a
   bounded translation RANSAC and a uniform affine warp. A prepared anchor map,
   projection seed used only for wide-range anchor reacquisition, and bounded
   relative fallback preserve lock through bumps without integrating a
   long-running pairwise path.
   Stateful effects, SuperRes inference, recording and assistive
   scheduling advance only on this camera clock.
   NIS/FSR enlargement uses the visible ROI with bounded existing-buffer
   reuse, up to 2x per pass; partial-cache border guards prevent stale pixels.
4. `PipelineOrchestrator` runs a separate viewport clock. During pan, zoom,
   focus animation, or resize it can re-present the latest complete scene up
   to 120 FPS or the active display rate; while idle it reduces to the camera
   rate. This improves navigation smoothness without synthesizing camera
   frames or rerunning temporal effects. Its pending-scene callback runs after
   every camera tick, including retained GPU-handoff retries, so those waits
   cannot skip navigation over an Extra Stable held scene.
5. `D3D12Presenter` keeps its swap chain at the render window's native pixel
   size and draws the persistent scene through one canonical `ViewTransform`.
   Fill crops uniformly and Fit letterboxes uniformly, so camera frames are
   never independently stretched on X or Y. The presenter exclusively owns
   the shared CUDA/D3D12 fence timeline: CUDA reserves and commits producer
   signals through it, and every graphics submission waits for the latest
   committed producer, including after a skipped viewport present. Canceled
   reservations are never waited upon. Busy frame-slot admission
   returns immediately and retains the scene for a later Qt tick. Recording
   clones queue their own CUDA dependency even when the viewport is busy.
   Recording textures require both producer completion and consumer lease
   retirement before reuse or obsolete-size eviction.
   New recordings use a separately owned clean Original source before effects,
   viewport scaling, UCT and ink; periodic assistive grabs use the
   asynchronous readback ring (`RequestReadback` /
   `TryGetCompletedReadback`), as do photos and on-demand analysis. Request ids
   match results to the originating request/frame. Fence and CUDA stream
   drains have deadlines; failed completion retains live resources and stops
   GPU submission until restart. Camera shutdown runs on shared session-owned
   workers with a separate deadline and revocable UI delivery.

Lecture-notes document/image storage uses one bounded ordered background
queue. Queued jobs retain their paths and immutable data through settings
changes; cancellation releases unsubmitted images. The UI reports failures
and waits for actual storage completion before opening notes.

`scripts/profile_startup.ps1` profiles first-camera presentation, Qt heartbeat
delays, actual camera arrival FPS, application drops, and processing/presentation
timings. It uses explicit isolated settings and normal child-process shutdown.
`-OutputDirectory` accepts either a repository-relative directory or an absolute
local path, so phone-test evidence can stay outside the synced checkout.
See [the developer guide](developer_guide.md) for invocation and measurement
limits. Initial platform discovery, graphics resource creation, and manual device changes still include
synchronous work; ordinary frame-slot admission and initial device opening no
longer add blocking waits to the Qt event loop.

6. `RecordingManager` sends clean Original frames through a bounded worker queue
   to one `VideoRecorder`. It encodes fragmented MP4 through Media Foundation
   using source timestamps and the negotiated fractional rate. A selected
   `AudioCapture` endpoint is normalized to 48 kHz mono PCM on its own thread
   and encoded as AAC into that same file. AV1 is preferred, with H.264 fallback.
   Format changes start a new Original part. Only successful finalized outputs
   enter notes; diagnostic bytes from failed attempts remain independently owned.
   Cleanup removes only session-owned regular zero-byte files with no accepted
   samples; there is no startup sweep of old recordings. Legacy paired-video
   notes remain readable. See [Original-only recording](original_only_recording.md).
   Microphone callbacks cross an independently owned, generation-tagged target
   that Stop cancels before waiting; a detached reader can therefore deliver
   to neither a destroyed app nor a later recording session. Recorder
   abandonment is sticky into process teardown even when the worker later
   recovers and joins, so intentionally leaked MF objects are never followed
   by `MFShutdown`.
7. Paired photos reserve an unused timestamp stem under a process lock and
   encode both JPEGs to exclusively created `.writing` files. A
   `.pair.pending` marker certifies completed encoding before the two-rename
   commit. Startup completes a marked interrupted second rename when possible,
   cleans abandoned temps, and reports unresolved marked pairs. It never
   deletes existing final JPEGs, including a user-retained half of a pair.

Completed readbacks drain on idle ticks independently of new presentations.
Camera arrivals mark scene dirtiness without extending the user-motion tail;
keyboard zoom explicitly requests a new viewport presentation. GPU publication,
rotation, and capture stop invalidate old CPU presentation pixels. Annotation
rotation rebases ink and edit history; text anchors rotate with upright labels.

Processed photo and drawing jobs retain immutable UCT commands, ink and view
transforms, then paint text below ink on the image writer. Original photos and
new recordings stay clean. GPU-started video writers retain their source lease;
CPU fallback BGRA is converted to the writer's NV12 contract without adding a
processed-video copy.

Assistant preparation cancellation retires its notes payload and emits one
terminal event; late workers remove only their own temporary file. History
loads have a separate generation canceled by newer loads/turns. Credential
deletions persist opaque pending ids before removal and retain them on failure.
Streamed answers preserve selection/scroll; short Draw rails scroll.

See [audit 45](../improvement_ideas/45-codebase-audit-2026-10-11.md) for actual
validation results and native/hardware checks still pending. Source work
reduction does not establish an application speed improvement; encoder resource
pooling and NIS coefficient initialization remain deferred.


### Additional quality/performance review

[Review 46](../improvement_ideas/46-additional-opus-review-2026-10-11.md) records
completed Opus capture, UI, and resumed GPU reports with root validation.
Follow-ups retain camera preference during fallback, disable driver enhancement
in conversion, catch up saturated capture bursts, and add paired video admission
with explicit loss accounting. Diagnostics publish without starvation; recovery
history errors retire placeholders; photo recovery
ignores completed pairs. GPU soft-mask/opaque-scene contracts and in-place text
composition address quality and redundant traffic. Measured speed and vendor
fidelity remain unverified; native desktop checks remain deferred by the owner.
Codec low latency may affect quality; internal AAC/driver allocations and
blocking finalization remain separate limits.

[Review 47](../improvement_ideas/47-sequential-priority-fixes-2026-10-11.md) adds
separate bounded AAC input accounting in PCM frames, including stale/padded
packet progress, and audio-only loss counters. H.264/AV1 codec fixtures decode
the complete audio track. The original strict playback-gap failure was later
confirmed with distinct decoded audio/video markers and repaired with bounded
PCM silence; the marked native regression now passes. See
[recording timeline evidence](recording_attempts_and_timeline.md) for exact
measurements and the separate real-microphone acceptance limits. Admission does
not depend on timestamp-derived AAC packet credit. Opus hit Claude's rate limit
before its final review.

[Review 48](../improvement_ideas/48-gemini-independent-review-2026-10-11.md)
records independent source/probe verdicts for all four returned Gemini reports
and their 20 main findings. Confirmed CPU allocations and failure-lifecycle
follow-ups are separated from known candidates, unreachable GPU scenarios,
false error contracts and unsupported speed estimates. No application change
or new native desktop validation is claimed by that audit.

[Review 49](../improvement_ideas/49-unused-code-census-2026-10-11.md) measures
unused-code candidates in current first-party C++/CUDA, separating test-only
APIs and optional configurations. Exact linker/CUDA binary evidence distinguishes
functions already discarded from retained kernel code and the live obsolete
recording map check. Its implementation follow-up removes the verified unused
APIs, helper kernels and pairing state, while preserving test-only APIs and
documenting the partial desktop-free build/test gate.

[Review 50](../improvement_ideas/50-alternating-opus-repairs-2026-10-11.md)
tracks alternating root/Opus implementation and independent review. The first
round fixes camera-navigation recursion and Draw keyboard ownership with Opus
acceptance for its original 91-check headless scope. An empty-editor focus
follow-up brings the suite to 94 checks. Bounded 16 MiB settings reads/saves,
failure recovery and retained CPU rotation buffers are also implemented and
root-verified. Opus's final follow-up hit its rate limit; independent review of
the rotation/focus follow-ups and native desktop acceptance remain deferred.

## Language And Locale

`LanguageManager` installs an embedded `QTranslator` before constructing the
main window and re-translates existing widgets after a language change under
`Advanced > Settings > Language`. The registry supports 25 languages, including
Arabic, Persian, Hebrew and Urdu with RTL layout. Persisted choices use the
canonical tags in [localization.md](localization.md), or `system` to follow the
Windows display-language preferences. Explicit saved choices remain intact;
Traditional Chinese is distinct from Simplified Chinese. Language is global,
not part of a Quick Mode.

Hand-built widgets retain English source properties through `ui_translation`;
`SetLiveText` retains dynamic status and accessibility source state. Device
names, model ids, paths, telemetry and serialization tokens remain data values.
Locale-aware presentation does not alter JSON or media metadata.

Built-in AI instructions retain their source text plus the selected response-
language directive. User-authored instructions remain unmodified. Read Aloud
is manual; language changes prefer an installed matching voice and report when
one is unavailable. Locale metadata also drives the process layout direction
and leading/trailing anchors for floating chrome and Draw toolbars. Developers
can use `--rtl-test` or `OKUFLOW_FORCE_RTL=1` for additional layout checks.

`scripts/check_translations.ps1` validates all 24 non-English Qt catalogs,
source-key/format parity, LF endings and `lrelease` compilation. This structural
check does not establish fluent wording, font coverage, RTL usability or live
screen-reader acceptance. See [localization.md](localization.md) for review and
published-build status.

CUDA is the processing path and the CPU effects pipeline is deprecated: when the GPU pipeline is unavailable the app presents unprocessed passthrough video with a persistent "GPU required" notice instead of running effects on the CPU. The debug composite view remains CPU-only as a diagnostic.

The UI now has two states:
- Simple: a full-size live view with three flush, auto-fading primary clusters,
  contextual keystone history controls, a numbered quick-mode grid, and large
  visual/accessibility mode announcements
- Advanced: the same live view beside an inspector with `Image`, `Assistant`,
  and `Settings` tabs. Image contains mode-owned tuning and
  diagnostics; Settings contains shared camera, recording, navigation, files,
  language, and AI setup. Assistant provides subscription status, camera-aware
  chat, and OkuFlow-owned history.

## Module Map
- `src/app` / `include/okuflow/app`: composition root plus focused pipeline,
  recording, settings, UI-state, assistive, and interaction managers. The
  `OkuFlowApp` implementation is split by responsibility across `app_*`
  translation units.
- `src/capture` / `include/okuflow/capture`: Media Foundation camera and
  microphone enumeration, mode discovery, and capture.
- `src/common` / `include/okuflow/common`: CPU image conversion/effects,
  canonical aspect/view transforms, frame pipeline, and media writing.
- `src/d3d12` / `include/okuflow/d3d12`: swap chain, upload, presentation, and texture readback.
- `src/cuda` / `include/okuflow/cuda`: CUDA interop surface, kernels, and fence synchronization.
- `src/ui` / `include/okuflow/ui`: Qt widgets, overlays, and event routing.

## Build Matrix
- `scripts/build_and_run.bat`: default local Windows build and launch helper.
  It explicitly enables CUDA and the runtime-loaded Text-SR adapter unless
  either option is overridden in the environment, preventing stale CMake
  caches from silently disabling NVIDIA Super Resolution.
- `scripts/build_release_bundle.bat`: packages a distributable `dist/OkuFlow`
  folder and explicitly enables CUDA plus the runtime-loaded Text-SR adapter
  unless either option is overridden in the environment. It builds and runs
  CTest before staging, requires `windeployqt` to succeed, validates the
  deployed Qt platform runtime, and publishes only a complete bundle. Existing
  `dist/OkuFlow/output` user captures are preserved. A locked primary bundle
  produces the complete sibling `dist/OkuFlow2` without stopping the app.
- `scripts/agent_build.bat`: tracked Windows compile/test matrix. It locates
  Visual Studio 2022 (17.x) with `vswhere`, compiles `msvc-release`, then runs the CPU and
  CUDA-enabled CTest presets with explicit PASS/FAIL summaries.
  Set `OKUFLOW_AGENT_EXCLUDE_GUI=1` only when desktop use is restricted; the
  explicitly partial matrix then excludes tests labelled `gui` and leaves
  native interaction validation pending.
- `scripts/run_test_gate.ps1`: serial CTest execution with local, per-build
  inventory-bound evidence. Main-window, annotation and AI-settings Qt slots
  are independently selectable; explicit desktop-interruption retries preserve
  earlier results without crediting application failures. The tracked build
  and bundle scripts use it; see [test selection and resumes](../tests/README.md#selectable-gui-cases-and-exact-inventory-resumes).
- `scripts/run_minimal_test.bat`: builds the app without launching it, then
  runs the DX12/CUDA sandbox harness when its `CMakeLists.txt` is present;
  otherwise it reports a successful optional-harness skip. It also builds the
  isolated `mf_dxva_minimal` probe but deliberately does not open physical
  cameras; that owner-gated probe is run explicitly. CUDA builds also expose
  `mf_dxva_minimal --interop external --iterations 100` for watchdog-isolated
  external-memory lifetime stress. The dangerous legacy registration repro is
  opt-in and never part of automated build gates.
- `cmake/CMakePresets.json`: includes `msvc-debug`, `msvc-release`,
  `msvc-cpu`, and `msvc-cuda-tests`. Only `msvc-cpu-tests` and
  `msvc-cuda-tests` are CTest presets; both fail when no tests are discovered.

Core CMake options:
- `OKUFLOW_ENABLE_CUDA=ON|OFF`
- `OKUFLOW_ENABLE_TESTS=ON|OFF`
- `OKUFLOW_ENABLE_TEXT_SR=ON|OFF` (runtime-only NVIDIA Maxine SuperRes adapter;
  enabled by CUDA presets and disabled by the CPU preset)

When operating from the WSL/Linux agent shell, invoke Windows-side tooling with
PowerShell 7 via `pwsh.exe -NoProfile -Command '...'`, for example
`pwsh.exe -NoProfile -Command 'Get-Date'`. This PowerShell 7 bridge can also
run the Windows build helpers, such as
`pwsh.exe -NoProfile -Command '& .\scripts\build_and_run.bat'`; do not use the
legacy `powershell.exe` bridge.

## Runtime Behavior
- Camera modes are listed in the UI for the selected device.
- Global Advanced viewport controls select aspect-safe Fill/crop or
  Fit/letterbox plus Auto-up-to-120, fixed 60/90/120, or Match-display motion.
  Explicit rates clamp to the monitor and are reported once. Diagnostics
  distinguish camera FPS, measured/target viewport FPS, display Hz, viewport
  pixels, scene pixels, framing mode, and missed presents.
- Camera mode probes release their Media Foundation activation session before capture starts, so camera restarts and virtual-camera sources receive a fresh media source.
- Mid-stream device loss triggers an automatic reconnect state machine (2s/4s/8s backoff for about 30 seconds) that re-finds the same physical device by symbolic link; no modal dialogs appear while it runs. Unsupported dynamic format changes and startup failures are reported in plain language in the status label.
- The Simple interaction surface is a bottom-left preset carousel and temporary
  tile grid; each preset maps to a full advanced configuration. Keys `1`-`9`
  activate the first nine entries, including the built-in `Projector Screen`
  and `Whiteboard` modes.
- Simple chrome fades after about five seconds idle and returns on mouse,
  keyboard, focus, or application activity. Mode changes produce a centered
  toast plus a Qt accessibility announcement; they do not start speech.
  The persistent Hide UI button or `Ctrl+H` hides chrome and the Assistant
  without discarding its answer; Show UI restores the prior mode and inspector
  layout. Mouse movement cannot undo explicit hiding. F6 and Ctrl+F restore
  the controls for keyboard navigation.
  Qt and native activity notifications share a fast path while chrome is
  already visible, so high-frequency pointer input does not repeatedly move
  or raise its owned tool windows and starve viewport presentation.
- Advanced edits update a live config and can be promoted into user-defined quick modes without hiding the camera.
- Advanced places the global Virtual Joystick control near the top, provides a
  question-mark Help window ordered as Controls then Features, and offers
  Reset Mode for restoring profile-owned values without changing global
  device or interaction choices.
- Keystone correction retains up to 32 accepted warps. Previous freezes and
  restores older history, Stop/Continue controls live detection, and Next
  restores newer history or requests one fresh correction while stopped.
- Text Clarity runs after stabilization/keystone and before legacy BW/spatial
  sharpening. A shared pitched GPU workspace supplies local luma mean and
  variance to background flattening, Sauvola, glare suppression, focus
  scoring, and the stroke mask. The `Document` preset enables the complete
  classical stack. Simple auto mode classifies paper/board/mixed content from
  the device histogram, replacing paper/board with the reading mask while
  preserving natural color for mixed scenes.
- At zoom levels of 1.33x or more, profile-owned NVIDIA Super Resolution can
  replace the NIS/FSR stage. The wrapper loads NVIDIA Video Effects only at
  runtime, resolves models from the runtime's `models` subdirectory, and runs
  a supported 4/3x pass on the shared CUDA stream; additional magnification is
  applied afterward. It performs no host frame readback and retains the base
  scene when unavailable or when its 60-frame steady-state average exceeds the 24 ms
  latency target. NIS/FSR continues only when separately enabled. Ten warmup
  samples are excluded. Inference uses a supported
  integer crop only when it contains the complete requested view;
  inference and presentation share SR geometry, including exact 4/3 for the
  advertised 1.33 step. Changed source/output extent or strength restarts the
  timing window and retires old work before reload; unrelated text controls
  retain the effect. Advanced shows the source
  crop, viewport target, final zoom, and measured average. Maximum source
  detail is the default. Optional `Faster 2x mode (narrower view)` uses at
  least a 2x stage, changing a 1280x720 target's visible source crop from
  960x540 to 640x360. This is explicitly a speed/field-of-view tradeoff. When
  latency is the only problem, a compact checkbox can keep SuperRes active
  until the feature is turned off. Optional `Ultra quality (full frame, up to
  1440p)` creates a distinct high-resolution D3D12/CUDA cache and applies
  viewport zoom/crop only after full-frame inference: 720p -> 1440p uses 2x,
  1080p -> 1440p uses 4/3x, and native 1440p input bypasses unnecessary AI
  enlargement. The ordinary scene and cache dimensions follow the negotiated
  post-rotation camera mode; 720p is not an internal fixed resolution.
- Focus scoring reduces Laplacian statistics on-device and asynchronously
  copies only two floats about every 15 frames; no image readback or render
  stall is introduced. A low score suppresses text reading and shows a
  refocus prompt.
- Camera selection and orientation are global. Stabilization, display colors,
  contrast, sharpening, zoom, and other image treatment are profile-owned.
  The shipped `Stabilize Image` switch always uses full-strength CUDA
  feature/RANSAC registration and automatically activates the fixed-reference
  lock. Obsolete strength, engine, rolling-shutter, and separate tripod
  settings are ignored when encountered in older profile JSON and are no
  longer written. Their runtime implementations and UI adapters have been
  removed.
- Stabilization's fixed-reference lock for clamped cameras tracks from the captured
  reference to the current frame. Per-feature tracking is seeded by the last
  accepted absolute transform and checked in both directions; RANSAC prefers an
  expanded visible-zoom ROI when it has enough texture and otherwise falls back
  to the whole frame. A zoom-scaled display-pixel deadband suppresses
  subpixel shimmer without integrating pairwise drift. Rejected frames freeze
  the last correction and never auto-replace the reference. Reference capture
  and rebuild occur automatically when stabilization starts or the camera
  pipeline resets.
- Optional `Extra Stable (hold on shake)` is a second transient layer available with
  stabilization. It continuously runs
  registration while presenting a full-resolution CUDA copy of the last
  model-valid, sharp, settled stabilized frame whenever tracking rejects,
  focus falls below 72% of the lock reference, or the absolute transform
  jumps beyond the impact gate. The production motion-entry gate is 20% less
  sensitive than the initial implementation to avoid premature holds. Five
  settled frames arm recovery and a
  four-camera-frame blend returns to live video without a hard cut. The held
  frame is captured before keystone and later effects, so the normal pipeline
  still treats it consistently. The control is off by default, transient, and
  warns that moving people or content may be hidden while a frame is held.
  Display Colors uses an accessible compact swatch grid and a 256-entry GPU
  luma LUT; custom 2-8 stop gradients/posterize schemes persist globally for
  reuse by profiles. Its native popover has an opaque backing surface, so
  content beneath it never shows through the swatch and editor controls. Wheel
  scrolling never edits selectors or sliders.
- Orientation is applied before the rest of the processing pipeline.
- Settings persist to `%APPDATA%\OkuFlow\OkuFlow\settings.json`. A VLM API
  key entered in AI Settings is protected by Windows Credential Manager; JSON
  stores only an opaque credential id and ignores plaintext `vlmApiKey`
  fields. The environment override remains available for local development
  and is never persisted. Settings files are limited to 16 MiB on disk. An
  oversized file is preserved beside the original and the last valid backup is
  restored. A save that would exceed the limit leaves both files unchanged
  and reports the cause.
- Snapshots are saved as timestamp-matched `_original.jpg` and `_processed.jpg`
  pairs under `Documents\OkuFlow\Photos\YYYY-MM-DD\` by default.
- Recordings are saved as timestamp-matched `_original.mp4` and
  `_processed.mp4` pairs (or distinct `_attempt-{UUID}` names after an encoder
  fallback) under
  `Documents\OkuFlow\Recordings\YYYY-MM-DD\`; encoding is live AV1 when
  available and otherwise live H.264.
- The processing status label under Advanced Image diagnostics distinguishes
  CPU, GPU, fallback, debug-view, recording and VLM states without
  covering the Simple camera view. The collapsed Diagnostics group also
  reports rolling 240-sample p50/p95/p99 camera-processing and
  capture-to-present timings; budget warnings follow the negotiated camera
  frame period.
- Photo and annotation image writes use a bounded application image-I/O pool.
  VLM/Codex frame preparation use an independent bounded assistive
  pool, keeping PNG/JPEG encoding, resizing, base64/JSON construction, and
  temporary-file writes off the UI thread. Saturation is reported instead of
  accumulating unbounded work, and cancellation generations prevent stale
  preparation from launching a request.
- The Advanced inspector uses a draggable high-contrast splitter and persists
  its width. Text-clarity and display sliders reflow beneath their labels when
  the inspector is narrow, and feature status labels wrap within the panel.
- Read transcribes the current view through the configured vision provider.
  New Codex settings select Luna 6 with low reasoning; existing explicit model
  choices remain available. Read uses the selected vision provider. The separate
  experimental, opt-in [Ultra Clear Text](ultra_clear_text.md) development
  feature uses installed Windows OCR languages locally; it is not part of the
  published v0.2.5 download.
- The non-blocking first-run Setup Assistant installs or updates the official
  Codex CLI and offers NVIDIA Video Effects on supported hardware. Downloads
  are pinned and verified, with a Windows downloader retry on transfer failure.
- Scene Explain defaults to a native Qt JSON-RPC client for `codex app-server`,
  reusing a ChatGPT-managed Codex login. Read opens a new ephemeral thread
  every time. Explain continues one saved thread (`explainThreadId` in
  settings, listed in Assistant history as "Explain conversation"); a thread
  that cannot be resumed is replaced and the new id remembered. Both always
  use a read-only, no-network, no-approval policy. Advanced
  Assistant threads are persistent and can opt into internet access or
  workspace-scoped coding; only OkuFlow-created thread ids are indexed in
  settings. Server approval and permission-escalation requests are denied,
  unexpected tool items are interrupted, and turn watchdogs always return the
  UI to idle. The stable app-server surface currently lacks a complete
  per-turn tool allow-list, so the interrupt path remains necessary defense in
  depth. OpenAI-compatible HTTP servers remain an optional fallback; there,
  Explain resends up to four recent Explain answers as text (never earlier
  images), kept only in memory and cleared when the server URL, model, API key
  or provider changes, or when New is pressed in the Assistant tab. Pending
  answers cannot repopulate history after a reset or configuration change.
- The streamed result panel is an owned floating tool window with native
  move/resize handling over the D3D camera surface. Streamed fragments update
  its 20-point, 135%-spaced text without reapplying geometry. Its prominent
  Read Aloud control sits below the answer; New Conversation is secondary,
  Panel position opens a compact menu, and Escape clears a draft or returns
  focus to the camera. Its camera-relative geometry persists
  across restarts, and its first-use position clears the top Simple controls.
  Its follow-up field remains editable during a streamed answer while Ask and
  Enter submission remain blocked; once ready, it attaches the current view
  and sends questions into the shared persistent Advanced Assistant
  conversation.
- Lecture notes are valid per-session HTML documents under
  `Documents\OkuFlow\Notes\`.
  They collect timestamped text readings, scene explanations, and relative captured
  image links that render in a browser and remain portable with the complete
  OkuFlow user-data root.
- Photos, recordings, notes, analysis exports, and console diagnostic logs
  share one user-owned root. Advanced Assistant can select another writable
  root outside the install directory; `Ctrl+Shift+O` and the Advanced action
  open it in Explorer. Console-attached launches tee Qt output to a timestamped
  file under `Debug\`, retain the newest 20 logs, and leave ordinary GUI
  launches quiet. These logs may contain local paths, device names, camera
  identifiers, and timing/error details, so review them before sharing.
  Legacy install-relative output can be copied without deleting its source.
- AI Settings uses a bounded, vertically scrollable dialog with distinct Codex,
  OpenAI-compatible VLM, speech, and notes sections. It displays the
  built-in OkuFlow Codex instruction read-only and persists separate user
  preferences for response language, tone, and detail. Those preferences are
  added to Codex developer instructions without weakening its permission
  policy and become a system message for the OpenAI-compatible fallback. The
  image-capable model list and per-model reasoning efforts are populated
  dynamically from Codex app-server `model/list`; a configured model missing
  from the current catalog remains identified as unavailable.
- Read Aloud is manual-only and uses Qt TextToSpeech over the Windows Runtime
  backend when available. AI Settings lists all voices exposed to desktop apps
  by that backend across installed languages, then persists the selected voice
  and speed. Windows 11 Narrator/Magnifier Natural voice packages are not
  exposed by the public Windows Runtime speech API and are not selectable here.
- `Setup & Downloads` in Advanced reopens the dependency assistant at any time.
  Dismissing its automatic first-run prompt is persisted independently of
  manually reopening it. Only missing Codex triggers startup prompting;
  optional Maxine remains a manual setup choice. The dialog fits the screen
  and stays above the floating control windows.
- Advanced separates mode tuning in Image from shared setup in Settings, each
  with its own search and cross-tab match feedback. Slider readouts follow the
  active locale; position controls disable with Zoom. Ordinary edits update
  changed counts without reopening collapsed groups. The grid supports arrow
  browsing followed by Enter, Space, or a click to apply; Escape cancels browsing.
  The carousel separates its shortcut badge from its elided label; long device
  selectors expose their full value on hover.
- Codex camera-frame temporary files include the owning process id, are
  removed on completion/cancellation/shutdown, and stale files from dead
  OkuFlow processes are swept on the next startup.
- Release publishing is designed for the current private/team distribution:
  unsigned bundles are allowed, an installed code-signing certificate can be
  selected through `OKUFLOW_SIGN_CERT_SHA1`, and
  `OKUFLOW_PUBLIC_RELEASE=1` rejects an unsigned build. Every bundle includes
  SHA-256 checksums, a release manifest, and an SPDX SBOM.

## Documentation Index

- [Narrower UCT recognizer inputs](uct_recognizer_width.md): opt-in width
  profiles, same-crop quality comparison and workload-dependent performance.
- [UCT color and readiness repairs](uct_color_startup_repair.md)
- [`android/README.md`](../android/README.md): native Android companion setup,
  USB streaming, Advanced settings, current limits, and interactive drafts.
  [`PROTOCOL.md`](../android/PROTOCOL.md) freezes the implemented wire contract.
  [`BUILDING.md`](../android/BUILDING.md)
  records the installed user-owned toolchain, off-Drive outputs, build/test/lint,
  and optional one-device installation. [`Plan 51`](../improvement_ideas/51-okuflow-camera-android.md)
  tracks hardware acceptance and remaining phone zoom, quality and packaging work.
  [`PLAY_READINESS.md`](../android/PLAY_READINESS.md) records the first
  `com.okuflow` closed-test scope, persistent Pause/Resume, offline privacy/notices,
  private upload signing and remaining Console/device gates.
- [`docs/ui_capture.md`](ui_capture.md): opt-in composed real-app PNG captures
  in isolated startup-profiling sessions, including native chrome and desktop checks.
- [`docs/lecture_camera.md`](lecture_camera.md): downloadable lecture sample,
  isolated OBS Virtual Camera start/stop, and replacement-video workflow.
- [`README.md`](../README.md): user-facing features, downloads, setup and privacy.
- [`developer_guide.md`](developer_guide.md): technical setup, build and test
  commands, runtime controls, architecture details and contribution guidance.
- [`docs/code_reference.md`](code_reference.md): authoritative file/class map.
- [`docs/ui_modes_design.md`](ui_modes_design.md): Simple/Advanced layout and settings-ownership contract.
- [`docs/hardcoded_paths.md`](hardcoded_paths.md): machine defaults and magic values.
- [`docs/rotation_ui_notes.md`](rotation_ui_notes.md): orientation ownership and the
  rotated-frame resample contract.
- [`improvement_ideas/00-status-and-priority.md`](../improvement_ideas/00-status-and-priority.md):
  the single implementation tracker — status of every plan and what to do next.
- [`improvement_ideas/08-ml-text-sr-options.md`](../improvement_ideas/08-ml-text-sr-options.md):
  ML text super-resolution research (supersedes the old GPU upscaling to-do).
- [`improvement_ideas/42-pipeline-audit-2026-10-10.md`](../improvement_ideas/42-pipeline-audit-2026-10-10.md):
  source-backed pipeline/security repair backlog, candidate validation, and
  quality/performance measurement plans.
- [`improvement_ideas/43-audit-verdicts-2026-10-10.md`](../improvement_ideas/43-audit-verdicts-2026-10-10.md):
  finding-by-finding verdicts for Astra, Opus, Daybreak, and Gemini, including
  counterevidence, duplicates, and separate implementation/validation status.
- [`improvement_ideas/44-pipeline-optimization-plan-2026-10-10.md`](../improvement_ideas/44-pipeline-optimization-plan-2026-10-10.md):
  current algorithms across 20 pipeline stages, Gaussian/Kawase alternatives,
  and optimization candidates with quality and measurement requirements.
- [`docs/THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md): third-party attribution and redistribution notes.

## Current Gaps
- Automated tests are still limited.
- CUDA interop still needs broader hardware validation across more driver/toolkit combinations.
- Maxine SuperRes requires a supported NVIDIA GPU and the user-installed
  NVIDIA Video Effects runtime; hardware/runtime and visual-quality validation
  remains necessary across Turing, Ampere, Ada, and Blackwell systems.
- SuperRes inference follows NVIDIA's synchronous sample path on OkuFlow's
  CUDA stream, isolated on a serial host worker with a one-second deadline.
  The UI caller still waits within that bound; fully asynchronous inference
  remains future work. Unknown completion stops video with a persistent restart
  message and quarantines resources. Its enhanced frame is the sole zoom result rather than a layer
  blended over a separately timed conventional zoom frame. Additional zoom
  uses the live focus point mapped into the clamped 4/3x source crop.
- Subscription-backed AI depends on a compatible installed Codex CLI and
  available account usage. Setup can install or update the per-user official
  CLI from a pinned, verified OpenAI bootstrap script; authentication remains
  an explicit `Connect ChatGPT` action. The fallback VLM mode depends on a
  user-provided OpenAI-compatible `chat/completions` server.

USB companion connection recovery, preference persistence and adb bootstrap are
mapped in [code_reference.md](code_reference.md) and
[../android/PROTOCOL.md](../android/PROTOCOL.md). The source-built adb runtime,
rebuild/source package and licensing terms are documented in
[adb_distribution.md](adb_distribution.md) and
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Windows first-use runtime

The release uses a native /MT `oku_flow.exe` preflight launcher and a deployed
`OkuFlowApp.exe` Qt application. Missing Microsoft runtime installation uses
the included signed installer with explicit vendor/UAC approval; Setup
Assistant displays its status too. See [Windows runtime setup](windows_runtime_setup.md).

[Assistant chat and consent UI](agent_control_ui.md) records the local 0.3.1
chat-only layout, shared Explain presentation, sole persisted-master owner,
contextual approvals, drawers, status/shortcuts and complete wording review.


Annotation snapshots save the view that was displayed at the save action. They
retain its strokes, transform and frame identity through asynchronous GPU copy;
an unavailable/expired copy is refused rather than paired with a later frame.
Camera retirement cancels pending annotation copies. This is independent of the
experimental Ultra Clear Text recognition worker.


Recording codec retries and the marked native AAC/video gap diagnostic are
documented in [recording attempts and timeline validation](recording_attempts_and_timeline.md).

[Bounded performance diagnostics](performance_diagnostics.md) records opt-in
offscreen chat CPU tails, synthetic deadline-helper waits, actual Maxine host
call profiling and the still-unmeasured native pan/queue/stale-frame seams.


[Engineering audit closure, 2026-10-17](engineering_audit_closure_2026_10_17.md)
maps all 36 supplied findings to fixes, measurements and remaining acceptance.

[Asynchronous Maxine producer](maxine_async_producer_design.md) records its
private stream/slot ownership, exact-current-frame publication, immediate
capture policy, cancellation and separate native performance acceptance.

[One-time media output-buffer comparison](media_output_buffer_benchmark.md)
records the real GPU throughput/latency tradeoff for one and two outputs. The
experiment preserves one history and leaves the app default unchanged.
