# 48 — Gemini reviews with independent root verification (2026-10-11)

Status: **REVIEW COMPLETE — all four reports independently adjudicated**.
This is an audit and a follow-up list, not an implementation or benchmark claim.
The owner requested two Gemini 3.1 Pro High reviewers and one each of Gemini
3.7 Flash High and 3.6 Flash High. The initial 3.5 request was corrected to 3.6.
All four tasks are read-only. Root preserves the prior authorized working-tree
repairs from reviews 45–47 at HEAD `8c07e0b0deb67a06fb8179e534c04c8813b84a21`.
Current source, including those uncommitted repairs, is the verification baseline.

Desktop tests remain deferred by the owner. No camera, application window,
AI request, focus check, screenshot, deployment, commit, or push was performed
for this audit. Ignored console probes use generated pixels/PCM only.

## Review provenance and progress

Provider for all four tasks: `antigravity`. The live catalog maps Pro High to
`gemini-pro-agent`; the Flash model ids are explicit below. Raw reports remain
unaltered under ignored `output/` so their original mistakes are inspectable.

| Review | Exact model id | Task suffix / local raw report | State |
| --- | --- | --- | --- |
| Pro A — capture/GPU/presentation | `gemini-pro-agent` | `okuflow-gemini31-capture-gpu-audit-20261011-r1`; `output/gemini31-capture-gpu-audit-r1.md` | Completed; all five findings adjudicated below |
| Pro B — app/recording/assistant | `gemini-pro-agent` | `okuflow-gemini31-app-recording-audit-20261011-r1`; `output/gemini31-app-recording-audit-r1.md` | Completed; all five findings adjudicated below |
| Flash C — transitions/failures | `gemini-3.7-flash-high` | `okuflow-gemini37-boundary-audit-20261011-r1`; `output/gemini37-boundary-audit-r1.md` | Completed; all five findings adjudicated below |
| Flash D — CPU/copies/I/O | `gemini-3.6-flash-high` | `okuflow-gemini36-cpu-io-audit-20261011-r1`; `output/gemini36-cpu-io-audit-r1.md` | Completed; all five findings adjudicated below |

Full task ids use the prefix
`node:delegated-task:command%3Amcp%3A566c877f-d50a-488a-8331-3b0c90f51531%3Adelegate-task%3A`
followed by the task suffix above. Launch manifests are preserved in
`output/gemini31-review-orchestration-r1.json` and
`output/gemini-flash-review-orchestration-r1.json`; their launch-time status
is not a later completion record.

## Pro A — individual verdicts

| ID | Gemini claim | Root verdict | Current evidence and limit |
| --- | --- | --- | --- |
| CAP-GPU-01 | Camera texture re-registration replaces the interop state while a previous CUDA copy is still running | **Refuted by the entry guard** | `UploadD3D11Frame`, `cuda_interop.cpp:2800`, returns before the mismatch/reset branch whenever PollCaptureCopy is pending or failed. On completion, GpuCopyLease::Poll clears pending state and releases its lease; no new copy starts before ResetCaptureInterop. The alleged early-return/overwrite sequence cannot occur under that current flow. The report omitted the entry guard and quoted a mismatch condition that differs from the current source. |
| CAP-GPU-02 | Maxine leaves uninitialized border texels around a partial output cache | **Refuted by full-cache output geometry** | `UpdateSuperResCache:1874–1875` fixes outputWidth/outputHeight to superResWidth_/superResHeight_. It selects an exact supported scale or falls back; the copy at `:2034` fills that entire cache, and the ROI reports the same dimensions. D3D12 uses whole-texture clamp addressing (`presenter.cpp:1004–1008`). Partial-cache guard padding is required for the separate NIS/FSR path, not this full-cache Maxine route. The proposed smaller-output-inside-larger-cache reproducer violates production preconditions. |
| CAP-GPU-03 | Percentile index calculation accepts fraction zero, casts a negative index, and returns the maximum | **Refuted as a reachable current defect** | CalculatePercentiles is private; its lambda is called only with 0.50, 0.95 and 0.99 (`pipeline_orchestrator.cpp:38–40`). Empty input returns before the lambda, and sampleCount is bounded by the 240-entry capacity. Arithmetic checking all 720 nonempty count/fraction combinations finds valid indices. No caller can request zero; the proposed test would require changing the implementation's inputs. Generalizing the helper in future would require a new contract. |
| CAP-GPU-04 | Synchronous Maxine inference causes 12–28 ms UI stalls and should run asynchronously | **Known architectural candidate; claimed latency and benefit unmeasured** | `maxine_superres.cpp:302` calls run_(effect_, 0), which the checked vendor SDK defines as synchronous. This happens in the UI processing path, but no Maxine runtime timing, input-event loss or p99 pacing benchmark was obtained. Plan 44 stage 17 already requires independent lifetime/quality validation before changing async flags. Preserve generation-matched output and explicit transfer/stream/buffer ownership; changing the flag alone can expose previous-frame output. |
| CAP-GPU-05 | Direct RCAS sharpness attenuation contradicts the documented exp2(-stops) mapping | **Refuted mathematically; perceptual curve claim unmeasured** | `spatial_upscalers.cu:649–653` defines stops=-log2(slider). For 0 < slider <= 1, exp2(-stops) equals slider exactly; the direct multiplier is the algebraic simplification, with zero explicitly retained as no sharpen. The suggested (1-slider)*3 mapping changes the chosen UI curve; it does not repair this identity. The alleged upper-10–15% response compression was not measured. Broader vendor-output fidelity remains a separate open comparison. |

### Pro A contract checks and rejected hypotheses

AMD's [official FSR reference header](https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/master/ffx-fsr/ffx_fsr1.h)
transforms RCAS stops with exp2(-sharpness), confirming the formula used in the
CAP-GPU-05 check. The local copy is `third_party/amd_fsr1/ffx_fsr1.h:662`.
The installed [Maxine SDK contract](../third_party/maxine/Maxine-VFX-SDK/nvvfx/include/nvVideoEffects.h)
at lines 173–179 explicitly distinguishes synchronous zero from nonzero async;
it does not establish the raw report's exact blocking duration or internal
cudaStreamSynchronize call sequence. Root's small arithmetic check exits 0 and
is recorded in `output/gemini-verification/gpu-claim-arithmetic.log`. It checks
current percentile bounds and the RCAS formula, not GPU output or perception.

| Raw non-issue disposition | Root check |
| --- | --- |
| Sharpen/bilateral early exits are safe before document replacement | Supported by current caller and composite contracts: replacement mode writes every output pixel from the text mask (`cuda_kernels.cu:3475–3494`); the gated sharpen caller passes replacement analysis only when the following composite is active (`cuda_interop.cpp:4025–4053`). This is an existing review-46 discarded-work repair, not a new speed measurement. |
| Spatial cache edge duplication is intentional | Correct for the NIS/FSR partial-cache path (`cuda_interop.cpp:2111`). It does not establish the nonexistent partial-cache Maxine premise of CAP-GPU-02. |
| Texture and slot fence signals serve distinct purposes | Supported by `presenter.cpp:730–751`: the shared texture signal follows its queued draw, while the later slot signal follows Present and records reuse/readback completion. No new cross-driver soak was run. |
| Capture normalizes negative RGB pitch before CPU conversion | Confirmed: CopyCaptureBuffer packs rows and reports positive outputStride (`capture_buffer.cpp:160–169`), used by the production source reader. This further narrows G36-5: its negative-stride helper allocation is real in isolation, but is not shown to be a hot path for current camera capture. |
| Faulted CUDA destruction retains resources deliberately | Confirmed: destruction calls SynchronizeStream and quarantines failed/in-flight resources (`cuda_interop.cpp:534–550`). Quarantine preserves C++/D3D owners as well as CUDA handles. This is a deliberate failure policy, not a normal-path freeable leak. |

Four of this report's five main bug allegations do not survive current caller,
geometry or mathematical checks; the remaining synchronous-inference observation
is a known, unbenchmarked optimization candidate. That is a verdict on these
claims, not a model-wide accuracy score. Its assertion of an exhaustive audit is
not independently established.

## Pro B — individual verdicts

| ID | Gemini claim | Root verdict | Current evidence and limit |
| --- | --- | --- | --- |
| REC-AUD-01 | Overlapping/nonmonotonic audio timestamps make AAC reject samples and abort recording | **Known clock risk; claimed failure refuted on tested path; erroneous HRESULT rejected** | `audio_capture.cpp:447` uses delivery QPC minus block duration; `recording_manager.cpp:1299` subtracts segment origin directly. This is already AST-REC-R4 in review 45, not NEW. A production H.264/AAC writer accepted all eight blocks and finalized in nominal, overlapping, equal and backwards timestamp cases. Successful encoding does not prove A/V alignment or correct playback timing. |
| REC-AUD-02 | Mode changes silently discard transition audio and omit its drop count | **Conditional source-confirmed refinement** | `StartSegment` resets the origin at `recording_manager.cpp:1022`; `WriteAudioFrame:1290` skips any block starting before it without a counter. However, `WorkerLoop:1708` routes already-queued older audio before the next video frame while both writers are ready. A late callback or a block straddling the new origin can lose a usable tail; not every queued transition block is lost. No mode-change runtime fixture was run. |
| REC-AUD-03 | `QueueVideoSample` incorrectly reports encoder errors as DiskFull | **Refuted; do not apply proposed fix** | `media_writer.cpp:1207` returns false from `WritePendingSample` only for recognized disk-full HRESULTs. Other errors throw at `:1211`, and `AddFrame:1367` / `AddGpuFrame:1662` finalize with WriteFailed. The false branch at `:1310` correctly selects DiskFull. Changing it as proposed would misclassify actual disk-full failures. |
| REC-AUD-04 | Unused readback-pairing API takes a useless mutex on every completed readback | **Source-confirmed cleanup; speed unmeasured** | Repository symbol search finds no caller of `StorePendingOriginal`, the only inserter into `pendingOriginalReadbacks_` (`recording_manager.cpp:724`). `HandleProcessedReadback:737` therefore locks and misses; `app_pipeline_runtime.cpp:1354` still calls it. This happens on completed photo/assistant/analysis readbacks, not necessarily every camera frame. No contention or application FPS gain was measured. |
| REC-AUD-05 | Audio losses can follow “No frames were dropped” in the same summary | **Confirmed wording improvement** | `BuildDropSummary`, `recording_manager.cpp:1517–1529`, uses the video-only Total() for its prefix, then appends audio block losses. The counters are correctly separate; qualify “video frames” to remove ambiguity. This is wording, not corrupted accounting. The claimed existing substring tests were not found. |

### REC-AUD-01 verification and the invented error contract

The raw report incorrectly calls the capture loop WASAPI; this implementation
uses a synchronous Media Foundation source reader. More seriously, the claimed
`MF_E_SAMPLES_OUT_OF_ORDER (0xC00D4A38)` does not match the installed SDK:
`um/Mferror.h:1751` defines that value as `MF_E_STREAMSINK_REMOVED`.
The same definition is in Microsoft's
[published SDK header](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/um/Mferror.h).
The [WriteSample contract](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-writesample)
does not establish Gemini's blanket overlap-rejection claim.

Root's isolated console probe links the existing CPU production writer and
recording-contract objects. Each case supplies eight generated 320×180 video
frames plus eight 960-frame mono PCM blocks, each 20 ms long:

| Audio timestamp sequence (100 ns ticks) | Accepted PCM blocks | FullyCompleted | Final HRESULT |
| --- | --- | --- | --- |
| `i * 200000` — nominal | 8/8 | Yes | 0 |
| `i * 10000` — intervals overlap | 8/8 | Yes | 0 |
| `0` — repeated equal times | 8/8 | Yes | 0 |
| `2000000 - i * 10000` — backwards | 8/8 | Yes | 0 |

Evidence: `output/gemini-verification/audio_clock_probe.cpp`,
`audio-clock-probe.log`, and four generated MP4s in that directory. Exit code
was 0. An initial probe link used /MD against /MT objects and failed; the
corrected /MT probe is the successful run above. This tests acceptance and
finalization, not decoded timing, microphone hardware, other MFTs, or long
recordings. Do not infer that the existing delivery-clock or AAC-gap issues
are solved. A blanket clamp to the previous block end can change clock drift
and gap behavior; it is not an accepted repair from this review.

### Pro B's rejected hypotheses also need scrutiny

| Raw disposition | Root check |
| --- | --- |
| Repeated credential deletion is safe | Confirmed: `protected_secret_store.cpp:141` treats ERROR_NOT_FOUND as success. Pending ids persist before removal in `settings_controller.cpp`. |
| Hidden provider controls do not participate in tab traversal | Source supports the visibility design (`ai_settings_dialog.cpp:797`); native focus correctness was not newly tested. Preserve the existing interaction gate. |
| Annotation key forwarding has a recursion guard | **Incorrect safety assertion; a source-confirmed feedback cycle is described below.** Checking `isAccepted()` after sendEvent cannot stop recursion during that call. |
| Process-exit recorder COM abandonment is intentional | Confirmed policy: `ShutdownForProcessExit` is bounded; sticky abandonment prevents MFShutdown in `app_bootstrap.cpp`. It is not permission to free in-flight resources. |
| AAC/MP4 gap collapse is not NEW | Correct deduplication, not a declaration of safety: the strict review-47 gap diagnostic still fails. |

## Flash 3.6 — individual verdicts

| ID | Gemini claim | Root verdict | Current evidence and limit |
| --- | --- | --- | --- |
| G36-1 | Recording allocates/rasterizes an unchanged full-frame ink layer each frame | **Known optimization candidate, not NEW; work confirmed, benefit unmeasured** | Already AST-REC-O5 in review 45 and explicitly deduplicated in review 46. `app_pipeline_runtime.cpp:1187–1203` creates/clears/rasterizes a QImage only for new camera frames with active recording, a valid original, and ink (`:1086`, `:1180`). Viewport-only ticks do not trigger recording. 1080p is 8,294,400 bytes/layer; the reported MB/s figures are arithmetic at assumed FPS, not a measurement. Cache must invalidate for ink edits/undo, transform, canvas, rotation and styling. |
| G36-2 | CPU fit/recording resamplers clear then overwrite pixels and recalculate x coordinates per row | **Source-confirmed P7 refinement; overstated work estimate** | `PresentFitted:2278–2302` and `ResampleRecordingCanvas:154–253` show redundant active-area initialization and row-invariant x-coordinate calculations. Fit margins still require clearing; it is not always a 100% overwrite. Per-channel bilinear rounding depends on that pixel's colors, so the four channel roundings cannot be eliminated merely by a column lookup. No measured frame drops or speedup. |
| G36-3 | CPU rotation discards scratch capacity and performs millions of memcpy function calls | **Allocation defect confirmed and repaired in review 50; function-call claim refuted** | `frame_pipeline.cpp:193`, `:232` assigns scratch, moves its allocation into StageRaw, then clears the moved-from vector. A 20-frame post-warm-up production probe allocates a full payload 20 times on the rotated path and zero times without rotation. Optimized VS2022 assembly inlines the constant-four-byte copies; RotateRawBuffer has no memcpy call. Review 50 swaps retained vectors: the same warmed allocation probe falls from 20 to zero allocations, with 264 byte-exact rotation frames checked. Application FPS remains unmeasured. Cast-based pixel access was not added. |
| G36-4 | Settings parser reads arbitrary file sizes before parsing | **Implemented/root-verified in review 50; OOM scenario not executed** | `settings_store.cpp:853` performs readAll without a file-size or bounded-read limit. SettingsController loads primary and backup through it. A corrupted unusually large local file can consume excessive memory before graceful recovery; no hostile file, allocation failure or crash was created. Review 50 adds a 16 MiB bounded binary read and matching serialized-save cap with a documented capacity fixture, 41 settings checks, staged publication and failure restoration probes. The original OOM scenario remains unexecuted. |
| G36-5 | Negative-stride RGB conversion allocates/copies twice; disabled CPU stages are copied | **Helper allocation confirmed; current-camera reachability unproved; known stage-copy refinement** | `frame_pipeline.cpp:89–100`, `:115–126` use a fresh topDown vector and then the normal row converter. The isolated probe confirms one full-payload allocation per negative-stride ARGB frame, but production CopyCaptureBuffer already emits packed positive stride (`capture_buffer.cpp:169`), so this is not a demonstrated camera hot-path saving. BuildStages copies at `:254`, `:262`, `:287`, already P7. They also occur in CPU fallback with Debug View off. Retained capacity means repeated copies are not automatically fresh heap allocations; StageFinal can be needed as mutable temporal output. |

### CPU allocation and compiler evidence

`output/gemini-verification/cpu_allocation_probe.cpp` overrides the allocation
operator to count requests at least as large as one 640×360 BGRA frame
(921,600 bytes). It exercises current production `frame_pipeline.cpp` and
`image_processing.cpp`, compiled with VS2022 /O2 /Ob2 /MT, with one warm-up and
20 measured iterations per route. It does not benchmark FPS or elapsed time.

| Route | Full-payload allocations in 20 post-warm-up frames |
| --- | --- |
| Positive stride, no rotation | 0 |
| Positive stride, 90° rotation | 20 |
| Negative stride, no rotation | 20 |

The probe exits 0. Log and invocation are `cpu-allocation-probe.log` and
`run-cpu-allocation-probe.bat` in the same ignored directory. The optimized
production rotation listing is `rotation-assembly.txt`: 487 lines, no memcpy
call, with inline DWORD pixel loads/stores. These observations contradict the
suggested millions of runtime memcpy calls. `vector<uint8_t>::resize` still
initializes newly created elements; it is not a general uninitialized-allocation
operation. Reusing existing size/capacity and preserving initialized Fit margins
are separate implementation requirements.

Flash 3.6 correctly rejected a naive-2D Gaussian accusation: the CPU helper
already runs horizontal and vertical one-dimensional passes
(`image_processing.cpp:341`, `:363`). It also found the three-entry/192 MiB
note-image retention guard (`assistive_runtime.cpp:1945`). BGRA/ARGB byte order
is the existing verified Windows little-endian contract, not a new swizzle bug.
The Codex stdout parser has 8 MiB message / 16 MiB retained-buffer checks
(`codex_json_rpc_process.cpp:185`, `:208`); however, append(readAllStandardOutput())
happens before those checks. They are not proof of an absolute peak-allocation
cap on a single oversized arrival. Neither “negligible cost” nor an OOM-proof
parser was established by this audit.

## Flash 3.7 — individual verdicts

| ID | Gemini claim | Root verdict | Current evidence and limit |
| --- | --- | --- | --- |
| G37-1 | Camera loss/expired reconnect leaves recording active; reconnect timestamp reset causes catastrophic desync | **Lifecycle policy gap source-confirmed; timestamp-reset mechanism refuted** | `BeginCameraReconnect:1848` calls StopCameraCapture; neither that stop nor expiry (`:1891`) ends recording. With audio enabled and live writers, WorkerLoop may continue draining PCM with no queued video. Deliberate audio continuation during a brief reconnect can be useful; permanent-loss behavior needs an explicit stop/pause/continue policy and status. Crucially, `recording_manager.cpp:1144` replaces source-reader timestamps with process QPC before both video writers. Gemini missed this guard; a reader restart does not by itself prove the claimed reset/desync. Indefinite successful encoding was not tested. |
| G37-2 | Stale CUDA-ready flags cause empty ticks to falsely return success after camera stop/rotation | **Claimed failure refuted; optional state hygiene only** | Flags do remain, but dimensions become zero and PresentLatestCudaScene has a zero-dimension guard (`:972`). RunFrameTick returns false on empty camera input (`:1994`), contrary to the raw report's invented return-true excerpt. It also returns before empty-frame handling when cameraActive/ingress is absent (`:1942`). No false successful camera tick or stale-pixel presentation was demonstrated. |
| G37-3 | Presenter fault strands pending photos/annotation captures and leaves audio recording active | **Source-confirmed fault/lifecycle gap; resource characterization narrowed** | HandlePresenterFault (`:343–356`) cancels assistant capture and marks scenes unavailable, but omits photo/annotation terminal handling and recorder policy. RunFrameTick returns before DrainCompletedGpuReadbacks at `:1910`; readback retry guards at `:1050` are unreachable under the latched fault. A retained photo original and pending annotation work can remain until teardown. This is not proof of an unbounded GPU leak; freeing faulted in-flight GPU resources requires the existing quarantine contracts. Audio continuation shares the G37-1 policy gap. No device-removal runtime injection was run. |
| G37-4 | A 180° image rotation within one MP4 requires a new track matrix/container segment | **Refuted as a container correctness bug** | CPU/CUDA paths physically rotate image pixels before encoding. A 180° content change preserves encoded geometry/rate; WriteFrame's modeChanged check correctly need not split solely for that reason. A track-matrix update is not required to encode changed image content. A requested rotation marker or extra clip split would be a product feature, not a repair justified by the unsupported ISO assertion. |
| G37-5 | Failed camera-format restart leaves recording active with audio but no camera | **Source-confirmed lifecycle refinement; shares G37-1 root cause** | OnCameraFormatChanged (`app_controls.cpp:296`) restarts capture. StartCameraCapture calls StopCameraCapture (`app_pipeline_runtime.cpp:1383`); failed-start handling stops camera/retries or reports a warning but does not notify the active recording session. This is another entry into the missing camera-availability recording policy, not a second independent audio-loop leak. Runtime negotiation failure was not injected. |

The Flash 3.7 report names several nonexistent symbols/counters/test files
(`WorkerThread`, `ProcessVideoFrame`, `AddAudioSample`,
`audioDropsDurationMismatch_`, `tests/app/recording_tests.cpp`), and gives an
unlinked alleged Microsoft quote about reader timestamps restarting at zero.
These are not evidence. Verification above uses current WorkerLoop, WriteFrame,
AddAudioFrame and the real test layout. Its proposed snippets are repair
sketches, not compiling patches.

Its non-issue dispositions also mix old documentation with current code.
The verified-non-issues entry 3 documents deferred CUDA stream destruction;
it does not certify unrelated buffer lifetimes. Entry 4 describes a historical
StopCapture race, not the claimed universal COM teardown proof. Current
session-owned, bounded shutdown and abandonment behavior is documented in
reviews 45/46; it must be checked when touching teardown. BGRA byte ordering,
implemented review-47 PCM admission, and benchmark-first NIS/resource-pooling
deferral remain valid dispositions. No new hardware soak is implied.

## Root discovery while verifying a rejected hypothesis

**ROOT-48-KEY — IMPLEMENTED in review 50; headless verified and Opus
accepted, native acceptance deferred.** The original recursion also
affected ordinary camera navigation with Draw off; Opus independently reproduced
it with a depth-limited offscreen probe. The historical source analysis follows. Pro B dismissed this with a nonexistent guard.
`AnnotationOverlay` installs itself as an event filter on its owner and render
ancestors (`annotation_overlay.cpp:150–158`). Its eventFilter dispatches a
QWidget KeyPress directly to keyPressEvent (`:741–749`), without an active-mode
or forwarding guard. With no selected stroke, arrows, Ctrl+/− and PageUp/Down
can call ForwardKeyToOwner (`:1580`, `:1637`, `:1648`). That helper synchronously
sends another KeyPress to the filtered owner (`:1539`), re-entering the same
filter and forwarding branch before its isAccepted() check can return.

Qt documents synchronous delivery through notify and installed filters in
[sendEvent](https://doc.qt.io/qt-6/qcoreapplication.html#sendEvent) and
[installEventFilter](https://doc.qt.io/qt-6/qobject.html#installEventFilter).
Root also inspected MainWindow's application filter: its navigation bridges
handle Tab/F6/search/hide controls, not the owner's forwarded arrow branch.
MainWindow::keyPressEvent's pan handler runs after object filtering, so it does
not break this cycle. Source therefore supports recursive delivery and a
potential stack overflow; no actual stack-overflow child was run.

Minimal direction: separate ancestor geometry observation from active Draw
keyboard interception; add a scoped forwarding guard or a direct owner-command
route. Preserve text editor ownership and distinguish active-overlay shortcuts
from ordinary owner input. Future coverage must prove a single pan/zoom delivery,
no selected ink, and inactive Draw behavior; then complete native keyboard
acceptance when the desktop is available. See
[review 50](50-alternating-opus-repairs-2026-10-11.md) for implementation and
review results.

## Priority after independent verification

1. ROOT-48-KEY: close the recursive key-forwarding route before more UI polish.
2. G37-3: terminal handling for photos/annotation jobs on latched GPU failure;
   preserve in-flight GPU lifetime/quarantine, and share the recording-availability
   policy with G37-1/G37-5. Choose explicit reconnect/permanent-failure semantics
   rather than silently treating all brief reconnects as lost sessions.
3. Keep known AST-REC-R4 / review-47 AAC playback-gap work distinct from false
   timestamp rejection/reset stories. Resolve source-clock mapping and decoded
   playback timing with real timeline fixtures before clamping timestamps.
4. G36-3: retain CPU rotation buffers; the allocator probe establishes churn,
   but real frame-budget savings need benchmarks. G36-4 adds bounded settings
   ingestion with preservation/recovery behavior. G36-5's negative-stride helper
   can be simplified, but current capture has already normalized its input and
   no live-camera speed benefit is established for that branch.
5. Benchmark known AST-REC-O5 ink caching and G36-2 column lookups; preserve
   visual/temporal output and Fit margins. REC-AUD-02 needs a late/straddling PCM
   block fixture at a segment boundary. REC-AUD-05 and REC-AUD-04 are smaller
   wording/cleanup follow-ups, with translation/reference updates when changed.
6. Keep CAP-GPU-04 asynchronous Maxine work as a measured experiment after
   correctness repairs. Do not apply the refuted re-registration, border-padding,
   percentile or RCAS-curve patches from Pro A.

## Validation scope

New evidence is limited to source/caller searches, primary API/reference checks,
two production console probes, a small formula/bounds check and the optimized
rotation assembly inspection. `git diff --check` passed after the report and
documentation index updates (exit 0).
No application source or tracked tests were changed in this audit. Review 47's
shipping release / CPU 22/22 / CUDA 27/27 non-GUI results remain the previous
implementation gate, not a fresh all-features or desktop acceptance claim.
All four reports' 20 main findings and their stated non-issues are adjudicated
above; source-confirmed does not mean hardware/native reproduction completed.
