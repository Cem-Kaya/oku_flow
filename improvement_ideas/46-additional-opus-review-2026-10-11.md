# 46 — Additional Opus quality/performance review (2026-10-11)

## Current checkpoint

The owner authorized continued fixes. Two Opus 5.5 reviews completed (capture/recording and UI/assistant); the GPU review reached Claude's rate limit before its final report. The fresh GPU continuation completed with its prior progress and refuted leads preserved; three additional findings are included below. A failed provider run is a capacity failure, not an invalid technical finding.

The reports below are source reviews, not runtime reproductions. Root and Sol implemented bounded repairs and Astra independently checked the initial recording and GPU contracts. The initial audit-46 release build and non-GUI CPU 22/22 and CUDA 27/27 checks passed after the thirteen repairs. A further GPU double-check prompted the later changes recorded below; the corrected final rerun also passed release compilation, CPU 22/22 and CUDA 27/27. Opus's resumed final source review completed and approved both later fixes without a new blocking counterexample. Root corrected two explanatory comments afterward; executable behavior is unchanged. No measured speedup is claimed. Native desktop tests remain deferred at the owner's request.

Validation evidence is kept locally in `output/audit46-validation/`: initial
`non-gui-r1.log`, `cpu-last-test-r1.log`, and `cuda-last-test-r1.log`, followed by
the corrected final `non-gui-r3.log`, `cpu-last-test-r3.log`, and
`cuda-last-test-r3.log`. Failed `non-gui-r2-failed.log` and targeted reader logs
preserve the idle-retirement reproduction and correction. The actual
H.264/AV1 GPU-to-CPU-to-GPU recording and AAC fixtures executed; CUDA alpha,
soft-mask, in-place and automatic-sharpening parity fixtures executed, without
hardware skips. Turkish/German parity passed
with 741 complete keys. Seven GUI groups were excluded from each preset,
including the new coalescer/translation/annotation composition checks. The
previous native focus/auto-hide failure remains unresolved by this subset;
the normal full release gate is still pending and no deployment is claimed.

| ID | Scope | Claim | Current verdict / action |
|---|---|---|---|
| OP-C1 | Camera preference | Automatic fallback overwrites saved identity | Valid source defect; repaired. Preference/migration tests added; actual camera hotplug remains untested. |
| OP-C2 | D3D11 conversion | Driver auto-processing remains enabled by default | API default confirmed; explicitly disabled in both converters. Actual vendor fidelity effect remains conditional/unmeasured. |
| OP-C3 | Capture queue | Sustained overload retains old frames during recording | Valid overload mechanism; saturation now retires backlog and keeps newest frame. Under-six bursts preserved; real-camera latency unmeasured. |
| OP-C4 | Encoder queue | CPU samples bypass pre-worker queue bound | Valid missing video bound; paired admission implemented with conservative encoded statistics. Hardware backlog occurrence unmeasured; AAC backlog and blocking Finalize remain unresolved. |
| OP-C5 | Recording accounting | CUDA upload Busy drops omitted from summary | Valid source omission; definitive arrival loss now counted once. Internal retries are not counted as new losses. |
| OP-U1 | Status diagnostics | Continuous updates restart single-shot indefinitely | Valid starvation/work issue; first deadline preserved and percentile computation throttled to visible-only 400 ms timer. GUI assertions pending; no measured speedup. |
| OP-U2 | Transcription | Unexpected/fatal close discards pending words | Valid source loss; bounded pending tail finalized before recovery gap/fatal completion. Fake-carrier regressions added. |
| OP-U3 | Translation | Show/Polish repeatedly visits whole widget subtrees | Valid repeated work; event-widget translation and English formatted fast path implemented, full-tree language switch retained. GUI assertions pending. |
| OP-U4 | Startup recovery | Completed photo pairs generate redundant probes | Valid avoidable probes; only transaction artifacts nominate recovery, nominated finals retain staleness checks. Completed-library regression added; scan timing unmeasured. |
| OP-U5 | Conversation history | Failed/cancelled history leaves loading placeholder | Valid terminal-state omission; generation-filtered failure and owned-placeholder cancellation implemented. Fake-server coverage added; actual provider/UI checks pending. |
| OP-G1 | GPU alpha | SuperRes transfer leaves alpha undefined | Valid transfer-contract defect; alpha-only CUDA pass and opaque scene shader implemented. Separate annotation shader preserves ink alpha. Maxine hardware fidelity unmeasured. |
| OP-G2 | Text softness | Hysteresis quantizes soft mask | Valid refinement of earlier design-choice assessment, not a separate historical bug. Soft-byte history now preserves edge softness; binary compatibility retained. |
| OP-G3 | Text composite | Automatic nondocument path copies identity | Valid redundant traffic; modes 1/2 compose in place and automatic mixed scenes return without pixel loads. Alias/parity fixtures added; timing gain unmeasured. |

## Independent repair review

- **Astra GPU:** caught an introduced shared-shader regression: forcing scene
  alpha opaque also made transparent annotations opaque. Sol separated scene
  and annotation entry points and pipeline bindings. A recording-composite
  fixture covers transparent/nonzero-RGB, translucent, and opaque ink, but its
  native GUI execution remains deferred. Astra's source recheck found no further
  counterexample in these alpha/hysteresis/in-place changes.
- **Astra recording:** caught an introduced admission deadlock for input payloads
  over 64 MiB: one pending sample could fill the budget so the next frame never
  arrived to flush it. Startup now rejects contracts that cannot fit pending
  plus incoming, before CPU/native sink creation and before compatibility
  fallback. Boundary/overflow/no-output-file fixtures cover the repair. Astra's
  source recheck passed; fallback file preservation is source-checked rather
  than fault-injection tested. A native NV12 payload fitting the budget does not
  establish texture-pool or driver support for that resolution.
- **Further Opus GPU double-check:** requested by the owner after the three
  completed reviews. Its source findings and root's independently challenged
  recording lead are recorded below. Opus's r3/r4/r5 implementation continuations
  were cancelled before publishing a completed result; the approved CUDA patch
  and fixture were recovered. Root completed source review and strengthened
  mixed-scene RGB, padding and mask-preservation assertions before validation.

## GPU double-check and feedback round

| ID | Finding | Verdict and action | Validation / limit |
|---|---|---|---|
| ROOT-GPU-L1 | Native NV12 samples unnecessarily retain BGRA source leases; no reader polling when pool pressure blocks new pairs | Valid conditional progress defect, independently confirmed by Opus/Sol. Remove redundant sample-held source ownership and retire readers on the worker independently of video arrival. Correct idle query progress after the first fixture failure. | Final release/CPU/CUDA rerun PASS. Both native codecs release the first source before next video/audio/Stop; Pending/Unknown quarantine tests retained. Timed manager/pool-pressure behavior source-reviewed; no actual 8K support claim. |
| GPU-F1 | Automatic Text Clarity sharpens RGB that document replacement discards | Valid refinement of KERNEL-4/plan 44, not an independent historical bug. Analysis-gated early returns implemented; mixed content keeps sharpening. | CUDA paper/board/mixed chain parity, actual mixed RGB change, untouched skipped scratch/destination, padding and mask guards PASS. Performance gain unmeasured. |
| GPU-F2 | NIS per-pixel phase indexes diverge in constant-memory coefficient reads | Valid documented mechanism; actual bottleneck/benefit unmeasured. Defer storage rewrite pending fractional-scale/ROI benchmarks and 1:1 regression comparison. | No code change or claimed speedup. |
| GPU-F3 | Display LUT also indexes constant memory divergently | Plausible low-priority observation, one lookup per pixel. No evidence that it materially limits performance. | Note only; no code change. |

Root initially proposed removing only the source hold. Both root and Opus
challenged that as insufficient because unpolled reader graphs still retained
the same source. The implemented worker path polls even without another frame,
using ordinary `DONOTFLUSH` admission and a 10 ms idle wake only while readers remain.
Idle retirement with no queued video permits asynchronous query-command flushing.
NV12 media
buffers continue to own their separate encoder output. Unknown completion still
quarantines the reader graph; no UI-thread COM polling or unsafe reuse is added.

The source counterexample requires a held pair plus the next pair to exceed the
384 MiB pool (allocation rounding/ink affect the threshold). It includes an
8K Source canvas or some smaller source/canvas combinations with annotations,
conditional on those driver/encoder modes being accepted. The new small-frame
fixture proves the ownership regression; it does not certify those large modes.

Root's source recheck verified that the new idle retirement uses the recording
worker, reports its operation/stage to the existing watchdog, and finishes the
session on unknown completion while preserving the self-retaining reader graph.
The small-frame test exercises the writer API; timed manager polling under real
presenter-pool pressure remains source-reviewed rather than a hardware fixture.
The additional Astra recheck was interrupted without a returned result, so it
is not counted as an independent approval of this later change.

The first final-gate attempt exposed a progress error in the proposed
`DONOTFLUSH`-only idle poll: both H.264 and AV1 held their first conversion reader
for the entire two-second fixture deadline (`pending=1`, one source owner, no
query error). Release compilation and the other 21 CPU groups passed; CUDA did
not run after the failed CPU gate. Evidence is `non-gui-r2-failed.log`.
Root corrected the idle path to allow `GetData` command flushing only when no
video is queued, retaining non-flushing ordinary admission. The targeted native
codec fixture then passed both codecs, retired the source before any next
video/audio/Stop, and decoded all 90 mixed-route frames with AAC/finalization
checks. The final complete non-GUI matrix then passed after this correction.
Microsoft's [GetData flag reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag)
documents that `DONOTFLUSH` can leave commands unprocessed indefinitely. This
correction does not authorize release on Pending/Unknown, add a completion wait,
or submit an encoder sample merely to obtain progress.
The exact driver mechanism is unproven: the recorded observation is that
non-flushing polling did not report completion of an already-flushed query,
whereas flush-capable polling did. The flag reference is general API guidance,
not proof that this driver's query marker had remained unsubmitted.

### Completed final Opus verification

The resumed r6 task completed with bounded source approval of both GPU-F1 and
ROOT-GPU-L1; its own response heading calls this r7. It reviewed the current
source, fixtures and saved root validation logs, without editing production
code or running tests. Root accepted the comment-accuracy correction and kept
empty-video-queue flushing: ordinary admission remains non-flushing. No
functional change or repeated test run was needed after this comment correction.

Remaining notes are explicitly limited:
- **R1:** a continually nonempty queue whose frames never reach GPU submission
  can delay retirement until the queue empties. Opus found no pool-exhaustion
  deadlock from this and suggested always permitting worker flushing as optional
  hardening. Root retained the current policy; no measured benefit supports a
  broader flush policy yet.
- **R2:** unresolved GPU reads after the existing 100 ms finalization window
  retain their pool leases until process exit. This is pre-existing safe
  quarantine, not a new release/reuse defect; its pool cost remains a limitation.
- Mixed-chain RGB change does not isolate sharpening from the preceding
  bilateral filter. Full-frame interop guard wiring and timed manager polling
  are source-reviewed; the fixtures exercise their public kernels/writer API.
- NIS coefficient storage remains a benchmark-first optimization; display-LUT
  divergence remains a note. Neither has a measured gain or implementation here.

Opus also checked CUDA/D3D12 fence ordering, alpha separation, soft hysteresis,
in-place composition, spatial extent bounds and device-loss teardown without
finding a new counterexample. Its suspicion that processed recordings lose the
zoomed NIS/FSR/SuperRes view was refuted: they clone the presented ROI cache.
These are source verdicts, not claims of measured device-loss or vendor fidelity.
NIS divergence evidence uses the official
[CUDA Best Practices constant-memory guidance](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#constant-memory)
and the pinned vendor scaler in `third_party/nvidia_nis/NIS_Scaler.h`.

## GPU — completed Opus continuation

Task: `node:delegated-task:command%3Amcp%3Ae19e0f6a-8928-4420-b8a7-a8f9b63c8d14%3Adelegate-task%3Aokuflow-opus-more-gpu-20261011-r2`

I found 3 new issues in my GPU scope. I'm dropping my third unfinished lead because it's already filed. Nothing was edited, built or run. Line numbers are from the current uncommitted working tree.

## G1 (P1, quality): SuperRes output has undefined alpha, which shows up in saved/read-back frames
- **Source proof:**
  - NVIDIA's own header, `third_party/maxine/Maxine-VFX-SDK/nvvfx/include/nvCVImage.h:407-409`, says that for BGRA destinations "most implementations do not change the alpha channel", and recommends filling it with `memset(-1)` at setup.
  - The code assumes the opposite. `src/cuda/cuda_interop.cpp:2014-2015` claims `NvCVImage_Transfer` "writes the complete BGRA destination", and nothing ever sets the destination's alpha.
  - **Ultra path** (`cuda_interop.cpp:2009-2012`): the buffer comes from a plain `cudaMallocPitch` in `EnsureSuperResOutputBuffer` (`:1732-1738`) and is never cleared. That undefined alpha is copied into the shared cache (`:2031-2041`).
  - **Non-Ultra path:** the output lands in the `alternate` work buffer and inherits whatever alpha the last stage left there.
  - The presenter's pixel shader returns the sampled alpha unchanged (`src/d3d12/presenter.cpp:1090`, `:1106`).
  - The back buffer is read back (`presenter.cpp:697-703`), wrapped as `Format_ARGB32` (`src/app/app_pipeline_runtime.cpp:2654-2658`), and saved as PNG, which keeps alpha (`:2700`).
- **Not new:** none of audits 43–46 mention this. The clear was removed in July (`074855a`), so it's an old bug, not a Sol regression.
- **Hypotheses, not proven:**
  - On screen it's likely invisible, because the swap chain sets no alpha mode.
  - Photo JPGs (`:2557-2560`) probably drop alpha.
  - Most at risk: the annotated-view PNG (`:2700`) and any QPainter drawing of read-back images.
  - Fresh allocations often read as zero, which would make the whole frame transparent.
  - The host BGRA upload (`cuda_interop.cpp:3235-3241`) copies the camera's unused fourth byte as-is, so non-SuperRes frames may carry bad alpha too.
- **Fix:**
  1. Make the shader output opaque: `return float4(color.rgb, 1.0)` at `presenter.cpp:1106`. One line covers every source.
  2. Fill alpha with `cudaMemset2DAsync(..., 0xFF, ...)` once when the output buffer is allocated, and correct the comment at `:2014`.
  - Optional speed follow-up: a single kernel that converts the planar float output and writes straight into the cache with alpha=255. This would remove the intermediate BGRA buffer and the extra copy. It depends on the SDK's planar memory layout, so it's a hypothesis until checked.
- **Test:**
  - GPU suite: a scene texture with alpha=0 must read back with alpha=255.
  - A CUDA hook check that the Ultra output buffer's alpha is all 255 after `Run`.
  - Owner check: Ultra mode, save the annotated view, and confirm the PNG's minimum alpha is 255.

## G2 (P2, quality/UI honesty): "Edge softness" does nothing to edges while Text Clarity is on
- **Source proof:**
  - The Sauvola mask (`src/cuda/cuda_kernels.cu:3394-3401`) uses a symmetric smoothstep centred on the threshold (`:3376-3379`). So `ink ≥ 0.5` holds exactly when `lum ≤ threshold`, whatever the softness `s` is.
  - The hysteresis kernel (`:3431-3434`) then forces the mask to 0/255.
  - Auto Text Clarity always turns hysteresis on (`cuda_interop.cpp:3888`), yet the "Edge softness" slider stays enabled in that mode (`src/app/app_controls.cpp:1034-1035`, label at `src/ui/main_window.cpp:973-975`).
  - Net effect: with Auto Text Clarity or Steady Text Edges on, softness only widens how long borderline pixels hold their previous value. Glyph edges stay hard and aliased in Adaptive Text, and selective sharpen gets an on/off weight (`cuda_kernels.cu:3513-3516`).
- **Novelty:** this refines audit43 G13-F2a, which was filed as a "design choice". The new fact is that a visible control can't do what its label says.
- **Fix, one of:**
  - **(a)** Store the soft value in the history buffer. Pixels inside the uncertain band keep the previous soft value; pixels outside it take the current soft value; drop the 0/255 quantisation.
  - **(b)** Minimal: disable or relabel the slider while hysteresis is active.
- **Test (CUDA):** run a synthetic luma ramp at softness 0.02 vs 0.25 with hysteresis on.
  - Today the outputs are binary at both settings.
  - After (a), there should be intermediate values, and softness should change edge width.
  - A two-frame ±δ jitter near the threshold must not toggle more pixels than today.

## G3 (P2, speed): The text composite does a full-frame identity copy on non-document scenes
- **Source proof:**
  - The host always launches the composite into a second buffer and swaps (`cuda_interop.cpp:4033-4043`), in mode 1 or 2.
  - In mode 2 (automatic), when the scene isn't classified as a document (`analysis->y == 0`), the kernel copies every pixel unchanged (`cuda_kernels.cu:3464-3467`).
  - The blend branch (`:3468-3474`) can't be reached from the only caller.
- **Not filed:** plan44 item 11 covers sharpen/composite fusion, not this copy.
- **Fix:**
  - Run the composite in place on `current`. It's per-pixel with no neighbour reads, so that's safe; skip the swap.
  - Return early before any loads when the image isn't being replaced.
  - In the replace path, don't read the source pixel at all.
  - Delete the unreachable blend branch, or test it.
- **Effect:** removes one full-frame read and one full-frame write on non-document frames. This is logical traffic only; I haven't measured it.
- **Caveat:** please confirm no later stage reads the pre-composite buffer through `alternate`. As far as I checked, later stages use it only as scratch output.
- **Test:** in-place vs current output must match byte-for-byte in both scene states. Time the stage with CUDA events at 1080p and 4K.

## Dropped or refuted
- **Box-filter underuse of the GPU (c):** already filed as audit43 AST-UP-O2 and plan44 item 6. One nuance to add there: the horizontal pass also has uncoalesced reads, because neighbouring threads read different rows (`cuda_kernels.cu:3186-3188`).
- **Focus marker drawn twice:** refuted. The CUDA marker is turned off at `app_pipeline_runtime.cpp:816`.
- **Synchronous `NvVFX_Run`:** a deliberate, documented choice (`CHANGELOG.md` ~992, `docs/README.md:486`).
- **Earlier leads, still clean:** spatial ROI/cache mapping, the reload `SynchronizeStream` (no deadlock), and warps/borders (already plan26 S5/S7).

## Capture/recording — original Opus result

Task: `node:delegated-task:command%3Amcp%3A0991d9b7-83f6-44c8-aaf1-62836ba9c901%3Adelegate-task%3Aokuflow-opus-more-capture_recording-20261011-r1`

No files changed and nothing built or run. I read the required docs, audit 45 (including its implementation checkpoint), `verified-non-issues.md`, audit 43's verdict table and the uncommitted diff in my scope. Five findings are below, ranked by priority. Two are regressions introduced by the current repairs.

### 1. P2 — Introduced regression: a startup fallback overwrites the saved camera (from the CAP-2 repair)
- **Where:** `src/app/app_pipeline_runtime.cpp:1402` sets `persistent.cameraSymbolicLink` on every start, explicit or automatic. `src/capture/media_capture_facade.cpp:32` returns index 0 when the saved camera is missing, and `src/app/app_bootstrap.cpp:1113-1121` starts that camera.
- **Trigger:** the saved camera B is unplugged or not yet available at launch (a USB document camera, or a phone/virtual camera still waking up). The app opens camera A and records A as the saved camera. It is written to disk by any later save, such as `app_pipeline_runtime.cpp:1515` on a background accelerated start, or the save paths for zero-copy and validation status.
- **Effect:** on the next launch with B present, the app opens A. The user's choice is lost just because B was absent once. Before the repair, startup fallback never overwrote the saved index. `cameraIndex` and `cameraSymbolicLink` can now also disagree.
- **Proof:** source-proven.
- **Fix:** write the link only on an explicit pick (`app_controls.cpp:16` already does this). The one exception is a legacy profile with an empty link, which should take the link of the camera it actually opened.
- **Test:** saved link B with only [A] enumerated → start → saved link is still B. Next run with [A,B] → B is selected. Legacy profile (empty link, index 1) gets populated once.

### 2. P2 — Driver "auto processing" is never turned off on either D3D11 video processor
- **Where:** camera NV12/YUY2→BGRA zero-copy conversion at `src/capture/media_capture.cpp:1196` (create), `1354` (colour space) and `1399` (Blt). Recording BGRA→NV12 conversion at `src/common/media_writer.cpp:502`, `520` and `1357`. A repo-wide grep finds no `VideoProcessorSetStreamAutoProcessingMode` call.
- **Effect:** Microsoft documents that by default the driver may apply automatic processing during the Blt. The listed capability flags include denoise, deringing, edge enhancement, colour correction, flesh-tone mapping, image stabilisation and super resolution. If a driver applies any of these:
  - frames entering Text Clarity, Sauvola and stabilisation tracking depend on the driver and its control-panel settings;
  - the zero-copy route gives a different image from the safe-copy/CPU routes;
  - the "original" recording is no longer the original.
- **Proof:** the missing call is source-proven. Whether a given NVIDIA/Intel/AMD driver actually applies anything is unmeasured.
- **Fix:** after each `CreateVideoProcessor`, call `VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE)`. Log `caps.AutoStreamCaps` once under `OKUFLOW_CAPTURE_DIAGNOSTICS`.
- **Test:** a seam or unit test that the call is made for both processors. Then, on real hardware, compare the zero-copy output against the safe-copy output for a static high-detail chart, before and after the fix.

### 3. P2 (latency) — While recording, sustained overload leaves the preview frames behind without reducing drops
- **Where:** `app_pipeline_runtime.cpp:1470-1477` keeps a first-in-first-out queue of up to 6 frames (`kMaxRecordingCameraFrames = 6`, `app.hpp:323`) and drops the oldest when full. `:1965` always processes the oldest frame.
- **Trigger:** recording is active and per-frame processing is slower than the camera interval. Examples: SuperRes close to its 24 ms guard on a 60 FPS camera, or ink rasterised per frame during recording.
- **Effect:** the queue stays full, so each processed frame is about 5 camera intervals old. From the constants that is about 167 ms at 30 FPS and 83 ms at 60 FPS. This is derived from the code, not measured. Under steady overload the number of dropped frames per second is the capture rate minus the processing rate, whichever frames are dropped. So the queue adds lag and saves no frames. Short bursts are still helped.
- **Proof:** the mechanism is source-proven. How often overload happens is conditional. In accelerated mode, the Media Foundation sample pool may cap the queue below 6 (hypothesis).
- **Fix:** keep the burst queue for transient stalls. When it hits capacity, or its oldest frame exceeds an age bound, collapse to the newest frame and count the skipped frames under `captureSlotOverwrite`.
- **Measurement:** use the existing capture-to-present p50/p95 with an injected per-frame delay greater than the camera interval. Drop counts should match; latency should fall. A transient-stall fixture should show no new drops.

### 4. P2/P3 (reliability) — CPU/system-memory recording routes have no backpressure on the encoder
- **Where:** `media_writer.cpp:763` disables sink-writer throttling, so `WriteSample` never blocks (documented behaviour). `AddFrame` allocates a full-canvas buffer per frame at `media_writer.cpp:1098`. `kMaxQueuedFrames = 12` (`recording_manager.hpp:246`) only limits the queue before the writer.
- **Trigger:** the encoder runs slower than real time on CPU/system-memory input. Examples: the ARGB32 route with the sink writer's colour converter, the worker-readback fallback, or two 4K writers.
- **Effect:**
  - Backlog memory grows without limit. Each queued sample is 1920×1080×4 ≈ 8.3 MB per writer.
  - Stop and mode-change finalisation must encode the whole backlog under one worker op (`recording_manager.cpp:1291-1303`).
  - If that takes longer than `kStopWatchdogMs = 8000` or `kWorkerWedgeMs = 10000` (`recording_manager.hpp:251` and `:258`), `AbandonWedgedWorker` (`recording_manager.cpp:549`) declares a false hang. Recording is then disabled until restart and the files may be incomplete.
- **Scope:** the GPU NV12 route is already limited by the recording texture pool.
- **Proof:** the missing backpressure is source-proven. Whether this backlog occurs on hardware OkuFlow supports is a hypothesis.
- **Fix:** before writing a frame pair, read `IMFSinkWriter::GetStatistics` for each video stream. If samples received minus samples encoded (or `dwByteCountQueued`) is over a limit, drop the pair under a new "encoder backlog" cause. Base the stop watchdog on lack of finalisation progress rather than a fixed deadline. Do not simply re-enable throttling: it could block the shared audio/video worker (hypothesis).
- **Test:** an offline fixture that feeds a CPU-route writer faster than real time, asserting bounded queued bytes, correct drop accounting, and that Stop completes before the watchdog.

### 5. P3 — Introduced accounting gap: frames dropped when the CUDA upload ring is busy are missing from the recording summary (from the CUDA-2 repair)
- **Where:** the busy check at `src/cuda/cuda_interop.cpp:3111`. Callers return without notifying the recording manager at `app_pipeline_runtime.cpp:763`, `:2046-2053` and `:2133/2155`. `RecordingDropCounts` (`recording_manager.cpp:1371`) has no cause for this.
- **Trigger:** recording while the safe-copy/host-upload route is active and all three upload slots are pending.
- **Effect:** frames are missing from the recording (a gap on the timestamp timeline), yet the summary can still say "No frames were dropped."
- **Proof:** source-proven.
- **Fix:** add a `NotifyGpuUploadBusy()` counter, include it in `Total()` and the summary.
- **Test:** extend the still-pending three-slot backlog injection test to check one count per dropped arrival and the cause text.

### Checked and not reported (already known or duplicate)
- **Delivery-time timestamps (AST-REC-R4):** native-mode enforcement (AST-C5), recording colorimetry tags (AST-REC-O1), bitrate (O4), pixel aspect ratio (RES-R4), viewport-to-recording stretch (AST-RES-R1, still open), safe-copy blocking Map (AST-B-O1), per-frame ink raster (O5) and REC-5 are all already filed.
- **Possible new input for R4, not counted:** video uses the time `ReadSample` returns (`media_capture.cpp:1605`), while audio uses QPC minus block duration. That likely gives a constant audio-ahead-of-video offset as well as the known jitter. `MFSampleExtension_DeviceTimestamp` shares the QPC epoch. It may not survive the source reader's processing transforms (hypothesis), so check it in diagnostics first. Separately, `README.md:626` says recording "uses the camera's timestamps", but the code uses delivery time.
- **Presenter frame-slot polling:** the readiness check looks at the fence before consuming the latency signal. Looks correct.
- **Repair diffs reviewed with no regression found:** CAP-3/4/5 readback packing, REC-3 NV12 conversion, REC-4 canvas freeze, APP-1 readback drain move.

**References:** Microsoft Learn pages for [`VideoProcessorSetStreamAutoProcessingMode`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videocontext-videoprocessorsetstreamautoprocessingmode), [`D3D11_VIDEO_PROCESSOR_AUTO_STREAM_CAPS`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_video_processor_auto_stream_caps), [`MF_SINK_WRITER_DISABLE_THROTTLING`](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-sink-writer-disable-throttling), [`MF_SINK_WRITER_STATISTICS`](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/ns-mfreadwrite-mf_sink_writer_statistics) and [`MFSampleExtension_DeviceTimestamp`](https://learn.microsoft.com/en-us/windows/win32/medfound/mfsampleextension-devicetimestamp).

## UI/assistant — original Opus result

Task: `node:delegated-task:command%3Amcp%3A0991d9b7-83f6-44c8-aaf1-62836ba9c901%3Adelegate-task%3Aokuflow-opus-more-ui_assistant-20261011-r1`

I found five new issues beyond audit 45, all from reading the source only. I changed no files and ran no builds, tests, GPU, camera, AI or UI. None of these are in the audit-45 list or `verified-non-issues.md`. I give no speed-up percentages; every speed claim below is unmeasured.

### F1 · P2 · Performance diagnostics never update while video runs, and the status label repolishes every frame
- **Where:** `src/ui/live_status_text.cpp:174` (`timer->start(...)` runs on every call). `src/app/app_interaction.cpp:387`, `:469`, `:474`. Per-frame callers: `src/app/app_pipeline_runtime.cpp:1272` (`if (newCameraFrame)`) and `:2403`.
- **Proven from source:** the status update runs on every camera frame. Each call restarts a 400 ms single-shot timer, and restarting a running `QTimer` resets it ([QTimer::start](https://doc.qt.io/qt-6/qtimer.html#start-1)). At camera rates faster than about 2.5 fps the timer never fires, so the visible "Performance percentiles" label stays at whatever it showed before frames arrived. Plan 32 says this coalescer was meant for "sources that update every tick", and its only test covers a short burst followed by quiet (`tests/live_status_text_tests.cpp:157`).
- **Same per-frame path:**
  - `setStyleSheet("color: …")` is called even when the colour hasn't changed.
  - `setAccessibleDescription(performanceText)` bypasses the coalescer.
  - Seven 240-sample percentile sorts run every frame.
- **Not proven:** that Qt's `setStyleSheet` always repolishes and relays out, and that `setAccessibleDescription` sends a UI Automation event every frame. Both are my reading of Qt internals.
- **Fix:**
  - Throttle the coalescer: start the timer only if it isn't already running, and keep the latest pending text.
  - Build the performance text and its description inside the timer callback, and only when the label is visible.
  - Cache the last colour and call `setStyleSheet` only when it changes.
- **Test:** call the coalescer every 10 ms for 300 ms with a 50 ms delay, and require the label to update before the calls stop. Count `DescriptionChanged` and `StyleChange` events over 100 unchanged status updates; expect no extra ones.

### F2 · P2 · Live transcription loses text that was shown but not finalized when it reconnects or fails
- **Where:** `src/app/transcription_session_controller.cpp:444` (`BeginRecovery`) and `:413` (`FailWith`), via `TearDownSession`. Compare `:118`, where a clean close does keep the tail.
- **Proven from source:** an unexpected close (`thread/realtime/closed` during listening) is reported as "Live transcription connection closed." (`codex_realtime_transcription_client.cpp:589`). The controller treats that, "Codex stopped unexpectedly." and other connection or timeout messages as transient, so recovery starts. Recovery clears the pending user text without finalizing it. Words already on the caption disappear and never reach the Advanced transcript or the notes. Only a gap marker is written.
- **Not proven:** how often the service closes mid-utterance, or whether it enforces a session-length limit.
- **Fix:** before teardown in `BeginRecovery` and `FailWith`, call `FinalizeTranscriptText(pendingTranscriptText_, pendingTranscriptTruncated_)` so the text is written before the gap marker.
- **Test:** the existing fake supports this (`EmitDelta`, then `EmitFailed(gen, "network connection lost")`). Expect the segment to be finalized before `GapDetected`. Repeat with a non-transient failure, and check that a late `done` from the old generation creates no duplicate.

### F3 · P3 · The app-wide translation hook re-translates whole widget trees on every Show and Polish
- **Where:** `src/app/language_manager.cpp:124` (filter on the whole application) and `:187`; `src/ui/ui_translation.cpp:390` and `:360` (regex fallback, starting at `:124`).
- **Proven from source:**
  - Qt sends Show and Polish to every descendant individually ([showEvent](https://doc.qt.io/qt-6/qwidget.html#showEvent)). Each one triggers `RetranslateWidgetTree` over that widget's whole subtree, so total work grows with widget count times nesting depth. This happens at startup, on Simple/Advanced switches, and when the faded Simple chrome reappears.
  - In English no translator is installed, so every lookup misses and falls through to up to about 60 regex templates. In English that result always equals the input, so the work is wasted.
  - `SetLiveText` makes two such lookups per camera frame from F1's path.
- **Fix:**
  - For Show and Polish, translate only the widget that received the event; keep the full-tree pass for `LanguageChange` and explicit language switches. Hidden descendants still get their own Show later.
  - Skip the template search entirely when the language is English.
- **Measurement:** a debug counter of translation calls while showing `MainWindow` (expect roughly one per widget), plus the existing "Startup timing: window shown" log via `scripts/profile_startup.ps1`.

### F4 · P3 · Startup photo recovery checks every completed photo pair before the window appears
- **Where:** `src/app/user_data_paths.cpp:416` (the scan includes finished `_original.jpg` and `_processed.jpg` files), the loop at `:480`, and `:515`. Called from `src/app/app_bootstrap.cpp:94`, before `mainWindow_->show()` at `:1125`.
- **Proven from source:** with startup's `staleBefore = now + 1s`, every ordinary saved pair falls through to about six file-system checks (lock, marker, opening the marker, two temp files, marker again). Since the earlier R1 fix, unmarked finished photos are never acted on, so all of this changes nothing. Startup cost therefore grows with the size of the photo library.
- **Not proven:** the actual latency, for example on a OneDrive or redirected Documents folder.
- **Fix:** process only pairs whose listing includes a `.writing`, `.pair.lock` or `.pair.pending` file, or list only those patterns and check the finals for those pairs. Optionally run recovery after the first frame is shown.
- **Test/measurement:** the existing `user_data_paths_tests` must still pass. Add a fixture with many completed pairs plus one interrupted pair and assert the same result; count checks or time `RecoverInterruptedPhotoPairs` against library size.

### F5 · P3 · A failed or cancelled history load leaves "Loading conversation..." in the transcript
- **Where:** `src/app/app_assistant.cpp:317` (placeholder); `src/common/codex_app_server_client.cpp:404` (on error it only sends `ServerStateChanged`, never a loaded or failed signal for the transcript). The cancel calls added by the audit-45 history fix are at `app_assistant.cpp:148` and `codex_app_server_client.cpp:321`.
- **Proven from source:**
  - If the server's `thread/read` errors, the placeholder stays indefinitely. The error shows up in the label named "Codex connection status".
  - If the user selects a past conversation and sends a prompt before it loads, the load is now cancelled (an interaction with the audit-45 history fix). The transcript then reads "Loading conversation…" followed by the new exchange, and the conversation's earlier messages are never shown.
  - Before the fix, the late history load would have overwritten the new exchange instead, so this is a milder behaviour of the same interaction rather than a new break.
- **Fix:** add a load-failed signal that clears the placeholder and shows a status message. When cancelling a pending load because a prompt is sent, clear the placeholder first, or block sending until the load finishes.
- **Test:** make the fake Codex app-server (`tests/fake_codex_app_server.cpp`) return an error for `thread/read`, and assert the placeholder is gone and a message appears. Separately, select, then submit before the reply, and assert the transcript starts with "You".

### Lower-confidence notes (not in the five)
- The new `UpdateSimpleTranscriptGeometry` binary search (`src/ui/main_window.cpp:2961`) measures wrapped text of up to about 26,000 characters roughly 15 times per update, at up to about 13 updates a second. It only matters for very long segments. Cutting the text to a window sized from the height budget before searching would bound it.
- The AI settings dialog (`app_assistant.cpp:19`) replaces the whole assistant settings block with values copied when it opened. A Codex path changed in a Setup Assistant that was already open would be reverted. This is a rare edge case.


## Full final Opus source response

Task: node:delegated-task:command%3Amcp%3A566c877f-d50a-488a-8331-3b0c90f51531%3Adelegate-task%3Aokuflow-opus-gpu-doublecheck-20261011-r6

The child response is preserved below; the verdicts above incorporate root's response and final limits.

## Opus response, r7: bounded source approval (read-only; no source edits, builds, tests, app/camera, commit or push)
Scope: the current four GPU-F1 files plus the code_reference entry; `PollGpuReadLeases`/`PollGpuInputReads`, the AddGpuFrame NV12 and readback leases, and `FinalizeAndStop` in media_writer; `RetireGpuInputReads`, `WorkerLoop`, paired admission and the heartbeat in recording_manager; the fixture's first-pending block. I also read Microsoft's `GetData`, `D3D11_ASYNC_GETDATA_FLAG` and `Flush` references. My r3/r4/r5 were cancelled and are not counted as reviews. No new counterexample, so no edit is requested.

### Mixed sharpening (GPU-F1): ACCEPT, no counterexample
- Same predicate: the kernels skip when `replacementAnalysis && ->y != 0`, and the composite replaces when `compositeMode == 2 && analysis && analysis->y != 0`. `compositeFollows` (cuda_interop.cpp:4026) is the composite launch condition (`binarizationActive && activeTextMask`, :4040). The sharpen branch requires `!enableAdaptiveBinarization`, so the composite that follows is always mode 2. Both run on `stream_`. `deviceTextAnalysis_` is written only by `LaunchTextSceneAnalysis` (:3907), earlier in the frame. Nothing between sharpen and composite writes it.
- Skipping still runs `swapBuffers()`, so the new `current` holds stale pre-sharpen pixels. The replace path then writes every visible pixel from the mask alone with alpha 255. Row padding is written by neither path, so the result is the same as before. The stale `deviceScratch_` is never observed: its other users, FSR EASU->RCAS (:2100, :4081) and the Gaussian separable pass (:4105), write it before reading it in the same frame.
- Neither kernel uses shared memory or `__syncthreads`, so the early return after the bounds check is safe. The analysis load is warp-uniform.
- Unchanged paths: explicit Adaptive (host-gated), manual sharpen without binarization (passes nullptr), and mixed scenes.
- The fixture covers dst and scratch guards, mixed parity, actual RGB change, untouched paper/board dst and scratch, post-composite parity, and unchanged source and mask, at 37x23 and 3x5. Two non-blocking notes:
  - (a) The RGB-change check would also pass if only the bilateral changed RGB. Both arms share the kernel, so the gating proof holds.
  - (b) The interop `compositeFollows` wiring is proven from source only; there is no non-GUI ProcessFrame harness.
- Line endings agree with root. Every added line in the four files is LF; no `+` diff line contains CR. In the test file, the working-copy CRLF lines are only the pre-existing ones. Its index blob is LF, so with `core.autocrlf=true` it normalizes on add.

### Idle query flushing (ROOT-GPU-L1): SAFE; PROGRESS sufficient for the pool-exhaustion stall
Pending/Unknown safety:
1. The flag changes only whether `GetData` flushes, not how its result is read. A lease is released only on `hr == S_OK && complete`. `Unknown` stays latched in `GpuReadRetirement`. Leases still pending at finalization stay self-retained (media_writer.cpp:829-833). No path releases a Pending or Unknown reader.
2. Per Microsoft, flags-0 `GetData` does not wait for the GPU: it flushes and returns S_FALSE until the data is ready. This adds no new kind of hazard beyond the existing per-frame `Blt`/`End`/`Flush` on the same immediate context. The device is `SetMultithreadProtected(TRUE)` (:499-503). A removed device is checked first, and a racing removal makes `GetData` fail, which becomes `Unknown` and quarantines the lease.
3. Thread confinement: `pendingGpuReads_` and `recording_` are touched only on the worker. `HasPendingGpuReads()` is called inside the `queueMutex_` scope but by the worker itself. The UI never polls COM.
4. The redundant owner is gone: no `FrameLifetimeHolder` remains, and `reader->source` (:1361, :1482) is the only lease. The fixture's expiry check proves something. Its owners are the reset local copy plus `reader->source`, and neither Unknown nor finalize quarantine can let the source expire, so expiry implies confirmed completion.

Watchdog:
- `RetireGpuInputReads` sets `WorkerOp::kRetireGpuReaders` around each recorder's poll. The writer sets `WriterStage::kRetireGpuReaders` through `StageResetGuard`.
- The heartbeat (recording_manager.cpp:534-551) abandons the worker if any single non-idle op exceeds `kWorkerWedgeMs`, so a flush-capable `GetData` that wedges in the driver is caught. `DescribeWorkerStage` names it (:605).
- After abandonment, `RetireGpuInputReads` and the timed wait both stop on `workerAbandoned_`.

Progress:
- In the stall case the pool is exhausted, so no pair is queued. The snapshot then sees an empty queue and runs a flush-capable poll at least every 10 ms. Microsoft's `Flush` remarks say flushed commands "eventually always complete", with event queries as the documented way to detect it.
- The saved logs give a direct A/B on this device. `reader-donotflush-failed.log` shows pending=1, owners=1 for H.264 and AV1. `reader-idle-flush-targeted.log` and `reader-idle-flush-last-test.log` show both codecs releasing the source before any next video, audio or Stop.
- A stale snapshot is harmless either way. Empty then a frame arrives: one extra flush. Non-empty: that pair reaches AddGpuFrame, whose own `Blt`+`Flush` drives progress, as steady state already does.

Residual R1 (non-blocking, not a deadlock): the snapshot is non-empty, yet every queued pair is dropped before AddGpuFrame. Two drop paths do this without a D3D11 flush on that recorder's device:
- `CanAcceptVideoFrame` rejects for encoder backlog (recording_manager.cpp:1153-1166).
- A recorder that lost its GPU surface takes the CPU fallback (:1183+, :1225+).

Pending reads (at most one per recorder) then wait for a loop-top snapshot that sees an empty queue. This cannot deadlock: a queued pair proves the pool had headroom, drops return their leases immediately, and the drop path is much faster than frame arrival.

Optional zero-heuristic hardening: always pass `allowFlush=true` in `RetireGpuInputReads`, keeping DONOTFLUSH only for AddGpuFrame's admission poll, which its own `Blt`+`Flush` immediately follows. Root's call; I do not require it.

Residual R2 (pre-existing, note only): a read still Pending after FinalizeAndStop's 100 ms flush-capable window stays quarantined, together with its source pool lease, for the rest of the process. That is safe, but at 8K one such lease is about 126.56 MiB of the 384 MiB pool. This change does not introduce it, and the flags-0 finalize loop makes it unlikely.

Comment correction (accuracy, small; docs/comment only):
- recording_manager.cpp:1624-1625 says DONOTFLUSH "may leave the driver's query marker unsubmitted indefinitely, even after the Blt flush". That asserts a mechanism. It conflicts with the Flush remark, and the evidence does not show it. The evidence shows only that DONOTFLUSH polling never *reported* completion of an already-flushed query within 2 s.
- The readback route (`End` -> synchronous `Map` -> DONOTFLUSH poll) retires normally. That fits completion being reported only after a later synchronizing or flushing call, but that is unproven too.
- Suggested wording: "On the tested driver, DONOTFLUSH polling alone never reported completion of the flushed conversion query; flush-capable GetData did."
- code_reference.md:1319 is already worded as an observation. Review-46's quote of the flag reference is accurate, but it should not be presented as explaining a query that had already been flushed.

Optional diagnostics: on Pending, the fixture's failure line prints an empty `LastError()`. If this regresses on another driver, printing `lastReaderQueryHr_`/`Complete` and the producer completed/required values (media_writer.cpp:266-273) would tell an unsatisfied producer-fence `Wait` apart from unreported completion.

Coverage note: the GPU fixture passes `allowFlush=true` explicitly. The manager's empty-queue decision is reviewed from source only. Acceptable for this change.

### Evidence status (read from saved logs; I ran nothing)
- `reader-idle-flush-targeted.log` (13:46:58): targeted PASS.
- `non-gui-r3.log` (13:51:49): release `oku_flow.exe` linked, CPU 22/22, CUDA 27/27.
- No file under src/include/tests is newer than r3; the last source edit was media_writer.hpp at 13:48:36.
- This is root's non-GUI gate, not the normal full release gate, which root reports as still pending with seven GUI groups excluded. I make no gate claim.

### Summary
- Mixed sharpening (F1): approved.
- Idle query flushing, Pending/Unknown safety and watchdog: approved. R1 is optional hardening, R2 is pre-existing, and one source comment should state the observation rather than a mechanism.
- GPU-F2 stays deferred until 1.33/1.5/1.7 fractional-ROI and 1:1 benchmarks; GPU-F3 stays note-only.
- No performance claims.
