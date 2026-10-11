# 45 — Current codebase audit (2026-10-11)

## Implementation checkpoint — 2026-10-11

The owner subsequently authorized implementation. Six Sol implementers and root integrated the following changes; Astra re-reviewed CUDA ownership and assistant/settings changes. This section supersedes the initial audit's no-code-change statement for the subsequent implementation phase only. Baseline finding evidence below remains historical.

The table distinguishes a source repair from its actual automated coverage. "Implemented" does not mean every proposed hardware/integration acceptance case ran. **No measured FPS, latency, or battery improvement is claimed.** The full native release gate is not green: an initial CPU run passed 28/29 groups but failed MainWindow focus/auto-hide assertions; one failure recorded Chrome in the foreground, another had no active Qt focus, and the auto-hide failure remains unresolved. The owner then explicitly requested code/build checks only. Further native tests are deferred without changing or weakening their assertions.

| Entry | Implementation state | Change | Coverage and remaining limit |
|---|---|---|---|
| REC-1 | Implemented | Worker launch moved to constructor end. | 256 production construction/shutdown cycles; no scheduler-forced race injection. |
| REC-2 | Implemented | CPU frames retain ink snapshots; processed canvas gets annotations. | Production composer tests cover pan, clear/undo and clean originals; no decoded CPU-only ink session. |
| REC-3 | Implemented | GPU-started writer converts CPU fallback BGRA to the negotiated NV12 contract. | Decoded GPU→CPU→GPU H.264/AV1 fixture checks 90 frames, timestamps and fallback colors. |
| REC-4 | Implemented | Session freezes its selected recording canvas; later choices apply next session. | Starting/stopping/next-session and portrait/landscape resolver fixture; no interactive live resolution change. |
| REC-5 | Deferred | Encoder resource pooling needs GPU completion AND encoder-consumer retirement. | No pool implemented; stable resource identity and delayed-consumer tests required first. |
| CUDA-1 | Implemented | Inference/presentation share supported integer SR crop geometry, including default 1.33×. | 720p/1080p geometry, fractional pans, aspect and stale-cache rejection; live Maxine fidelity pending. |
| CUDA-2 | Implemented | Upload Busy drops the arrival without drain, failure count or CPU retry. | Caller/source review and release build; injected three-slot CUDA backlog test remains pending. |
| CUDA-3 | Implemented | Unrelated text controls retain loaded Maxine; changed strength reloads through Ensure. | Source/build validation; SDK load-counter fixture and measured latency still pending. |
| CUDA-4 | Implemented; policy selected | Warmup/average/disabled state follows effective dimensions/strength. | Old pending timing discarded; fault propagation reviewed. Synthetic timing-event test remains pending. |
| KERNEL-1 | Implemented | CLAHE clamps independently derived neighboring tile indices. | CUDA produced-map interpolation coverage at normal, odd and tiny dimensions. |
| KERNEL-2 | Implemented | Relative tracking rejection preserves correction and accumulated motion. | CUDA accepted/rejected/zero-motion sequence and absolute reacquisition without reset. |
| KERNEL-3 | Implemented | Recovery-keyframe ROI subtracts capture-time origin consistently. | CUDA nonzero-origin competing-motion fixture. |
| KERNEL-4 | Implemented | Mean-only first pass removes discarded squared statistics; explicit adaptive mode skips discarded sharpening. | CUDA mean parity against full statistics; application throughput unmeasured. |
| KERNEL-5 | Partially implemented | NIS reuses its existing 6×6 neighborhood for edge maps. | Spatial CUDA regression coverage; per-device coefficient initialization and benchmarks deferred. |
| CAP-1 | Implemented | Requested identity survives failed camera opens and stop. | Failed-A/failed-B descriptor fixture; real busy-device reconnect pending. |
| CAP-2 | Implemented | Persist symbolic link; ResolveSavedCameraIndex resolves identity before legacy index. | JSON roundtrip plus reorder, insertion, removal, duplicate names, missing identity and empty list. |
| CAP-3 | Implemented | NV12 UV begins at allocation height, not visible height. | Production mapped WARP texture fixture with padded allocation and visible packing. |
| CAP-4 | Implemented | Explicit RGBA readback swaps R/B into BGRA; BGRA is unchanged. | Asymmetric RGBA/BGRA WARP fixtures; full camera→photo chain pending. |
| CAP-5 | Implemented | Reject unsupported dynamic format before accepting its layout; report error/recovery. | RGB24-with-valid-stride rejection fixture; scripted complete source-reader transition pending. |
| APP-1 | Implemented | Drain completed GPU readbacks on idle ticks before camera-readiness returns. | Source/build validation; delayed fake-presenter delivery test still pending. |
| APP-2 | Implemented | Stop, rotation and successful GPU publication clear obsolete CPU presentation pixels. | Source/build validation; end-to-end A→failed B assistant-capture fixture pending. |
| APP-3 | Implemented | Keyboard zoom explicitly marks viewport dirty despite blocked slider signals. | Source/build validation; stalled-camera keyboard integration pending. |
| APP-4 | Implemented | Rotate annotations and undo/redo with camera orientation; text labels remain upright. | Production model regression for quarter turns, undo/redo and restored coordinates. |
| APP-5 | Implemented | Scene dirtiness no longer renews the user-motion tail. | Deterministic scheduler fixture: settled 30 FPS camera uses 33 ms interval; interaction uses 8 ms at 120 Hz. |
| UI-1 | Implemented | Independent append cursor preserves selection/scroll; follow tail only when appropriate. | Overlay streaming regression passed in initial native run; Advanced app wiring source-reviewed only. |
| UI-2 | Implemented | Draw rails scroll within the safe viewport instead of clipping tools. | Compact production overlay fixture passed in initial native run; final desktop gate deferred. |
| UI-3 | Implemented | Simple captions show bounded trailing plain text; full transcript retained. | Long/newline-heavy caption regression passed in initial native run. |
| UI-4 | Implemented | Missing/loading voice entries preserve saved voice fields. | Dialog fixture passed in initial native run. |
| UI-5 | Implemented | Caption updates coalesce and use caption-only placement. | Source/UI regression coverage; layout-pass counts and heartbeat benchmark unmeasured. |
| AI-1 | Implemented | Preparation cancellation emits one terminal event after retiring ownership. | Mode-off/explicit-stop held-generation fixture. |
| AI-2 | Implemented | Stale preparation cleans only its own temporary files and leaves replacement notes intact. | A-cancel→B→late-A fixture checks durable B question/image/answer. |
| AI-3 | Implemented | History generations retire on selection, New, deletion and prompt admission. | Fake A-after-B and A-during-answer tests; app admission wiring source-reviewed. |
| AI-4 | Implemented | Accept same-thread transcript deltas during stopping, reject after closure. | Fake stop-window delta followed by closed without final; existing late-final case retained. |
| AI-5 | Implemented | Persist deletion intent across retry/restart; replacements use fresh ID when old ID is pending. | Fake stores cover save/remove failures, restart and replacement safety; no real credential access. |

Additional integration review caught and repaired a SuperRes drain-failure propagation error, a floating-point reversed-clamp interval, and a pending-deletion record that could target a replacement key. The added test harness also needed its Windows-only outer guard removed so Qt moc could see its class. Preparation fixtures now initialize image pixels and isolate their temporary-file sweep.

Credential scope limit: a newly written replacement key whose subsequent JSON publication fails can remain unreferenced after a process exit. Its fresh identity prevents the old deletion record from removing it; this is not a cross-store atomic transaction.

### Validation checkpoint after audit-45 integration

- Shipping release compilation passed. Non-GUI CPU passed 22/22 groups and CUDA passed 27/27 groups (`ctest --preset msvc-cpu-tests -LE gui` and `ctest --preset msvc-cuda-tests -LE gui`).
- The CLAHE fixture now separates host/CUDA fused-arithmetic bin-boundary differences from interpolation correctness; the pixel tolerance remains ±1 byte. Final CUDA shader/regression groups all passed.
- The tracked `scripts/agent_build.bat` earlier failed the native `main_window_interactions` group (CPU 28/29), so the full pre-submission gate remains unresolved. Seven GUI groups are excluded from the subsequent non-GUI validation at the owner's request.
- These results precede the additional changes in [review 46](46-additional-opus-review-2026-10-11.md). They must not be represented as verification of that later batch.

### Optimization follow-ups

- Gaussian blur already uses horizontal and vertical separable kernels in `src/cuda/cuda_kernels.cu`. A paired bilinear-sampling or tiled implementation needs GPU timing and pixel-error comparisons against this baseline. Do not claim the improvement percentages of a naive 2D baseline apply here.
- Dual Kawase is a possible separately named large-radius approximate effect, not a drop-in replacement for text-processing statistics or exact Gaussian output. No blur algorithm was replaced in this batch.
- Measure mean-only statistics, eliminated adaptive sharpening, NIS neighborhood reuse, retained Maxine models, idle timer wakeups, and caption-only layout independently before combining performance claims.
- Encoder pooling (REC-5) and per-device NIS coefficient initialization remain deferred with their acceptance requirements intact.


Baseline: `8c07e0b0deb67a06fb8179e534c04c8813b84a21` (clean application checkout at review start).

The strongest new finding is **REC-1**, a recording-worker construction-order race that is reachable at ordinary application startup. Other concrete new findings include SuperRes crop containment failure at 1.33×, blocking recovery on upload-ring pressure, missing CPU-recording annotations, keyboard-zoom invalidation, and assistant/settings lifecycle failures.

The original section is an **analysis report, not release verification**. It records 34 review entries across seven scopes. The subsequent implementation checkpoint above tracks repairs and actual coverage separately. Some entries refine existing reports, several have conditional triggers, and optimization entries have no measured speedup. Entry counts are neither unique historical bug counts nor a model-accuracy score.

## Review provenance and limits

The seven requested original Opus reviews ended at Claude's rate limit before returning substantive findings. One fresh Opus interop retry also failed with “Claude API rate limit reached. Try again later.” It produced no findings and has no pending children. Capacity failures are not invalid technical claims.

Seven **GPT-6 Astra fallback reviewers** completed the same scopes. The root reviewer reconciled prior verdicts, inspected key call paths and independently checked the SuperRes and CLAHE arithmetic. Findings below must not be attributed to a completed Opus review.

| Scope | Completed fallback reviewer | Main entries |
|---|---|---:|
| CUDA lifetime / Maxine | `audit_resume_cuda_lifetime` | 4 |
| CUDA kernels / upscalers | `audit_resume_cuda_kernels` | 5 |
| Capture / presenter | `audit_resume_capture_presenter` | 5 |
| Application runtime | `audit_resume_app_runtime` | 5 |
| Recording | `audit_resume_recording` | 5 |
| Qt UI / accessibility | `audit_resume_ui` | 5 |
| Assistant / settings | `audit_resume_assistant_settings` | 5 |

No application code changed, no app/camera/GPU/AI session was started, no credentials were read, and no native desktop tests, release builds or runtime benchmarks ran for this audit. A small CPU arithmetic check reproduced the source formulas for SuperRes crop sizes and CLAHE first-tile weights; it is not a GPU or image-quality test. References are anchored to the baseline and must be re-located after edits.

“Source-supported” means the reachable control/data path was traced. It does not establish hardware occurrence, service ordering, visible severity or measured performance where those are explicitly conditional. UI clipping is predicted from source geometry, not claimed as a new desktop reproduction.

## Recommended order

1. **REC-1 first:** start the recording worker only after its synchronization/state is initialized.
2. Protect output and request ownership: REC-3/REC-4, APP-1/APP-2, AI-1/AI-2/AI-3/AI-5, then camera identity/recovery CAP-1/CAP-2/CAP-5.
3. Correct displayed/recorded geometry and image quality: CUDA-1, KERNEL-1/2/3, CAP-3/4, APP-3/4 and REC-2. Include the UI and transcript regressions in the corresponding bounded changes.
4. Optimize work whose output is unnecessary before replacing algorithms: CUDA-3, KERNEL-4, APP-5 and UI-5. Address CUDA-2's Busy semantics without weakening fence retirement.
5. Measure resource pooling and NIS changes (REC-5/KERNEL-5), and decide the timing-guard policy (CUDA-4).

P1 indicates the startup lifetime defect; P2 indicates normal corrective work or the particularly direct reset optimization; P3 indicates lower-priority behavior, measurement-led work or policy. These are triage priorities, not observed incident frequencies.

## One-by-one verdict table

| ID | Priority | Current verdict | Finding | Prior relationship |
|---|---|---|---|---|
| [REC-1](#rec-1) | P1 | New defect | Worker starts before its mutex and queues exist | New |
| [CUDA-1](#cuda-1) | P2 | New defect | Fresh SuperRes output fails viewport containment | New; distinct from AST-UP-D1 |
| [CUDA-2](#cuda-2) | P2 | New conditional defect | Upload-ring pressure triggers blocking failure recovery | New |
| [CUDA-3](#cuda-3) | P2 | Optimization | Unrelated text controls recreate Maxine | AST-UP-F4 — still open |
| [CUDA-4](#cuda-4) | P3 | Policy / calibration candidate | Timing averages cross SuperRes configurations | New candidate |
| [KERNEL-1](#kernel-1) | P2 | Existing defect | CLAHE interpolation jumps at first tile centers | AST-UP-F2 — still open |
| [KERNEL-2](#kernel-2) | P2 | Existing defect | Rejected relative tracking undoes then reapplies correction | AST-S1 — still open |
| [KERNEL-3](#kernel-3) | P2 | Existing defect | Recovery-keyframe ROI uses the wrong coordinate frame | AST-S3 — still open |
| [KERNEL-4](#kernel-4) | P3 | Optimization | Text Clarity computes discarded statistics and sharpening | Refines AST-UP-O1 / plan 44; older focus-only trigger is obsolete |
| [KERNEL-5](#kernel-5) | P3 | Optimization | NIS reloads overlapping neighborhoods and coefficients | AST-UP-O3 / G10-F1a / G10-F3a — still open |
| [CAP-1](#cap-1) | P2 | Existing defect | Failed selection reconnects the old or empty camera identity | AST-C1 — still open |
| [CAP-2](#cap-2) | P2 | Existing conditional defect | Saved camera index can select a different device | AST-C3 — still open |
| [CAP-3](#cap-3) | P2 | Existing conditional defect | Padded NV12 GPU readback starts UV at the wrong row | AST-B3 / G05-F3a — still open; not proof of validator rejection |
| [CAP-4](#cap-4) | P2 | Existing conditional defect | RGBA fallback is labeled BGRA-compatible RGB32 | AST-B4 / G05-F3b — still open; driver occurrence unverified |
| [CAP-5](#cap-5) | P2 | Existing conditional defect | Unsupported dynamic format silently stalls capture | AST-RES-R3 — still open |
| [APP-1](#app-1) | P2 | Existing defect | Completed GPU readbacks wait for another presentation | AST-A5 / OP-LIFE-A5 — still open |
| [APP-2](#app-2) | P2 | Existing defect | Current-view analysis can reuse a previous camera image | AST-A8 — still open |
| [APP-3](#app-3) | P2 | New defect | Keyboard zoom does not dirty the cached viewport | New |
| [APP-4](#app-4) | P2 | Existing defect | Rotation leaves ink in the old image coordinates | AST-RES-R2 — still open |
| [APP-5](#app-5) | P3 | Optimization | Camera arrivals keep the user-motion timer active | AST-P-O1 / OP-PROJ-P3a — still open |
| [REC-2](#rec-2) | P2 | New defect | CPU recordings omit visible Draw annotations | New; limited to CPU routes |
| [REC-3](#rec-3) | P2 | Existing conditional defect | BGRA fallback can enter an NV12-configured writer | AST-REC-R3 / G03-F1a — still open |
| [REC-4](#rec-4) | P2 | Existing defect | Live canvas-size change ends GPU recording | AST-REC-R2 — still open |
| [REC-5](#rec-5) | P3 | Optimization | Each recorded GPU frame repeats resource setup | AST-REC-O2 — still open |
| [UI-1](#ui-1) | P2 | New defect | Streaming moves the reader's selection and scroll | New functional consequence adjacent to AST-A-O3 |
| [UI-2](#ui-2) | P2 | New conditional defect | Draw rail exceeds supported short viewports | New; distinct from assistant-overlap candidate |
| [UI-3](#ui-3) | P2 | New conditional defect | Simple transcript has no viewport height budget | New |
| [UI-4](#ui-4) | P3 | New conditional defect | Unavailable voice picker can erase the saved voice | New |
| [UI-5](#ui-5) | P3 | Optimization | Transcript partials rebuild all camera chrome | OP-RESP-F2a — still open; frequent caller clarified |
| [AI-1](#ai-1) | P2 | Existing defect | Mode-off during image preparation leaves Assistant busy | AST-A3 / OP-LIFE-A3 — still open |
| [AI-2](#ai-2) | P2 | Existing defect | Canceled preparation clears a newer request's note payload | AST-A4 / OP-LIFE-A4 — still open |
| [AI-3](#ai-3) | P2 | Existing defect | Delayed history replaces the latest selection or answer | AST-A7 / OP-LIFE-A7 — still open |
| [AI-4](#ai-4) | P2 | New conditional defect | Stop-window transcript deltas are lost without a final | New |
| [AI-5](#ai-5) | P2 | New conditional defect | Failed key clearing loses credential deletion intent | New |

## Evidence, changes to consider, and acceptance checks

### CUDA lifetime / Maxine

<a id="cuda-1"></a>

#### CUDA-1 — Fresh SuperRes output fails viewport containment

**P2 · New defect.** New; distinct from AST-UP-D1.

At 1280×720 and 1.33×, inference covers 960×540 but presentation asks for about 962.406×541.353. Exact-scale pan positions also fail from integer crop rounding. Maxine can run while ordinary scene output is used; valid SuperRes metadata also suppresses spatial fallback.

**Source:** [src/app/app_controls.cpp:952](../src/app/app_controls.cpp#L952); [src/cuda/cuda_interop.cpp:1882](../src/cuda/cuda_interop.cpp#L1882); [src/cuda/cuda_interop.cpp:1931](../src/cuda/cuda_interop.cpp#L1931); [src/common/view_transform.cpp:52](../src/common/view_transform.cpp#L52); [src/common/view_transform.cpp:93](../src/common/view_transform.cpp#L93); [src/app/app_pipeline_runtime.cpp:989](../src/app/app_pipeline_runtime.cpp#L989).

**Change to consider:** Use a shared inference/presentation geometry contract covering supported factors, centers and rounding; preserve stale-cache containment checks.

**Acceptance/measurement (not run):** Fresh-cache containment at 1.33×, exact factors, fractional centers, edges, portrait and varying viewport aspects.

<a id="cuda-2"></a>

#### CUDA-2 — Upload-ring pressure triggers blocking failure recovery

**P2 · New conditional defect.** New.

All three pinned upload slots still busy returns generic failure. The UI path drains the stream, clears scene readiness and may convert NV12/YUY2 on CPU before another CUDA attempt. This is legitimate backpressure, not necessarily a GPU error; frequency and stall duration are unmeasured.

**Source:** [include/okuflow/cuda/cuda_interop.hpp:496](../include/okuflow/cuda/cuda_interop.hpp#L496); [src/cuda/cuda_interop.cpp:3143](../src/cuda/cuda_interop.cpp#L3143); [src/cuda/cuda_interop.cpp:3190](../src/cuda/cuda_interop.cpp#L3190); [src/app/app_pipeline_runtime.cpp:897](../src/app/app_pipeline_runtime.cpp#L897); [src/app/app_pipeline_runtime.cpp:918](../src/app/app_pipeline_runtime.cpp#L918).

**Change to consider:** Return a distinct Busy result and perform admission before frame-specific enqueue work; retain the completed scene and safely retire an unused reservation.

**Acceptance/measurement (not run):** Three pending slot events must cause no drain, failure count, CPU conversion or scene invalidation; resume when a slot completes.

<a id="cuda-3"></a>

#### CUDA-3 — Unrelated text controls recreate Maxine

**P2 · Optimization.** AST-UP-F4 — still open.

Changing stroke weight, focus threshold or CLAHE with SuperRes enabled unconditionally resets/drains/reloads SuperRes and its timing state. Cost exists in source; timing benefit is unmeasured.

**Source:** [src/app/app_controls.cpp:997](../src/app/app_controls.cpp#L997); [src/cuda/cuda_interop.cpp:1759](../src/cuda/cuda_interop.cpp#L1759); [src/cuda/cuda_interop.cpp:1676](../src/cuda/cuda_interop.cpp#L1676); [src/common/maxine_superres.cpp:245](../src/common/maxine_superres.cpp#L245).

**Change to consider:** Compare effective SuperRes parameters; invalidate text history separately from model resources.

**Acceptance/measurement (not run):** Count model loads/resets for unrelated versus model-affecting changes; measure cold-load and interaction latency.

<a id="cuda-4"></a>

#### CUDA-4 — Timing averages cross SuperRes configurations

**P3 · Policy / calibration candidate.** New candidate.

Crossing a supported zoom factor recreates inference dimensions but preserves warmup/average counters. Pending old timing can disable a new workload before it is evaluated. Whether the guard should be surface-wide is a policy decision; workload speeds were not measured.

**Source:** [src/cuda/cuda_interop.cpp:1763](../src/cuda/cuda_interop.cpp#L1763); [src/cuda/cuda_interop.cpp:1843](../src/cuda/cuda_interop.cpp#L1843); [src/cuda/cuda_interop.cpp:1861](../src/cuda/cuda_interop.cpp#L1861); [src/common/maxine_superres.cpp:188](../src/common/maxine_superres.cpp#L188).

**Change to consider:** Choose guard scope explicitly; for per-configuration behavior, generation-tag timing and restart warmup on material changes.

**Acceptance/measurement (not run):** Synthetic timing windows across factor changes, pending old events and an already-disabled configuration.

### CUDA kernels / upscalers

<a id="kernel-1"></a>

#### KERNEL-1 — CLAHE interpolation jumps at first tile centers

**P2 · Existing defect.** AST-UP-F2 — still open.

Clamping the lower tile index before computing its neighbor retains the wrong negative-coordinate fraction. At width 1280, tile-1 weight changes from 0.996875 at x=79 to 0.003125 at x=80; the same error exists vertically.

**Source:** [src/cuda/cuda_interop.cpp:3911](../src/cuda/cuda_interop.cpp#L3911); [src/cuda/cuda_kernels.cu:3347](../src/cuda/cuda_kernels.cu#L3347).

**Change to consider:** Derive both neighbor indices from the original floor then clamp each, or clamp continuous tile coordinates first.

**Acceptance/measurement (not run):** Unequal adjacent tile maps; first-center/corner continuity, odd extents, pitches and downstream threshold masks.

<a id="kernel-2"></a>

#### KERNEL-2 — Rejected relative tracking undoes then reapplies correction

**P2 · Existing defect.** AST-S1 — still open.

A rejected estimate restores absolute correction but retains accumulated relative motion. A subsequent accepted zero-motion estimate reapplies it: example −15 → −10 → −15 without new accepted displacement.

**Source:** [src/cuda/cuda_interop.cpp:3558](../src/cuda/cuda_interop.cpp#L3558); [src/cuda/cuda_kernels.cu:2084](../src/cuda/cuda_kernels.cu#L2084); [src/cuda/cuda_kernels.cu:2122](../src/cuda/cuda_kernels.cu#L2122); [src/cuda/cuda_kernels.cu:2149](../src/cuda/cuda_kernels.cu#L2149).

**Change to consider:** Keep displayed correction and accumulated relative state consistent across rejection; preserve the intended hold behavior.

**Acceptance/measurement (not run):** Accepted relative motion → rejection → accepted zero → absolute reacquisition; check both state and warped landmarks.

<a id="kernel-3"></a>

#### KERNEL-3 — Recovery-keyframe ROI uses the wrong coordinate frame

**P2 · Existing defect.** AST-S3 — still open.

Every keyframe uses a common viewport center, but ROI membership tests raw keyframe coordinates. Recovery origin is added only after feature selection, so a nonzero origin can select features outside the intended zoom region.

**Source:** [src/cuda/cuda_interop.cpp:3533](../src/cuda/cuda_interop.cpp#L3533); [src/cuda/cuda_kernels.cu:1733](../src/cuda/cuda_kernels.cu#L1733); [src/cuda/cuda_kernels.cu:1909](../src/cuda/cuda_kernels.cu#L1909); [src/cuda/cuda_kernels.cu:1948](../src/cuda/cuda_kernels.cu#L1948).

**Change to consider:** Apply the common-anchor coordinate transformation consistently during feature counting, hypothesis selection and refit.

**Acceptance/measurement (not run):** Nonzero recovery origins with competing motion groups inside/outside the intended ROI across zoom centers.

<a id="kernel-4"></a>

#### KERNEL-4 — Text Clarity computes discarded statistics and sharpening

**P3 · Optimization.** Refines AST-UP-O1 / plan 44; older focus-only trigger is obsolete.

The first mean-square planes are overwritten before their consumer. Explicit Adaptive Text also fully replaces RGB from the earlier mask after Smart Sharpen has run. Automatic mixed-content mode still needs sharpening.

**Source:** [src/cuda/cuda_interop.cpp:3865](../src/cuda/cuda_interop.cpp#L3865); [src/cuda/cuda_interop.cpp:3919](../src/cuda/cuda_interop.cpp#L3919); [src/cuda/cuda_interop.cpp:3969](../src/cuda/cuda_interop.cpp#L3969); [src/cuda/cuda_interop.cpp:3988](../src/cuda/cuda_interop.cpp#L3988); [src/cuda/cuda_kernels.cu:3451](../src/cuda/cuda_kernels.cu#L3451).

**Change to consider:** Provide initial mean-only statistics; skip sharpening only when final composite guarantees replacement.

**Acceptance/measurement (not run):** Byte comparisons and focus/mask-history checks for production combinations, then GPU frame p50/p95 measurements.

<a id="kernel-5"></a>

#### KERNEL-5 — NIS reloads overlapping neighborhoods and coefficients

**P3 · Optimization.** AST-UP-O3 / G10-F1a / G10-F3a — still open.

Four 3×3 edge neighborhoods overlap the already loaded 6×6 luma array; immutable coefficient banks upload each invocation. Logical loads are not equivalent to DRAM traffic or measured slowdown.

**Source:** [src/cuda/spatial_upscalers.cu:351](../src/cuda/spatial_upscalers.cu#L351); [src/cuda/spatial_upscalers.cu:580](../src/cuda/spatial_upscalers.cu#L580); [src/cuda/spatial_upscalers.cu:697](../src/cuda/spatial_upscalers.cu#L697).

**Change to consider:** Reuse loaded luma; separately investigate device/context-safe coefficient initialization.

**Acceptance/measurement (not run):** Nonconstant vendor-reference patterns, borders, pitches and sharpness; measure registers, caching and end-to-end latency.

### Capture / presenter

<a id="cap-1"></a>

#### CAP-1 — Failed selection reconnects the old or empty camera identity

**P2 · Existing defect.** AST-C1 — still open.

Select B after A succeeds; B fails to open. LastSymbolicLink is updated only on success, so automatic retry targets A. On a first-ever failure, the target can be empty.

**Source:** [src/capture/media_capture_facade.cpp:82](../src/capture/media_capture_facade.cpp#L82); [src/capture/media_capture.cpp:519](../src/capture/media_capture.cpp#L519); [src/app/app_pipeline_runtime.cpp:1569](../src/app/app_pipeline_runtime.cpp#L1569); [src/app/app_pipeline_runtime.cpp:1832](../src/app/app_pipeline_runtime.cpp#L1832).

**Change to consider:** Persist requested retry identity separately from the last successfully opened camera.

**Acceptance/measurement (not run):** Fake A/B descriptors: fail B then make B available; also test first-start failure; retry must target B.

<a id="cap-2"></a>

#### CAP-2 — Saved camera index can select a different device

**P2 · Existing conditional defect.** AST-C3 — still open.

Startup restores an index into a newly sorted enumeration. B at index 1 in [A,B] becomes C at index 1 in [B,C]. Physical-list changes are the trigger, not ordinary stable startup.

**Source:** [src/app/settings_store.cpp:625](../src/app/settings_store.cpp#L625); [src/app/settings_store.cpp:905](../src/app/settings_store.cpp#L905); [src/app/app_bootstrap.cpp:1102](../src/app/app_bootstrap.cpp#L1102); [src/capture/media_capture.cpp:338](../src/capture/media_capture.cpp#L338).

**Change to consider:** Store/resolve a stable symbolic link and define migration/missing-device fallback behavior.

**Acceptance/measurement (not run):** Insert/remove/reorder devices and duplicate friendly names; assert selected physical identity.

<a id="cap-3"></a>

#### CAP-3 — Padded NV12 GPU readback starts UV at the wrong row

**P2 · Existing conditional defect.** AST-B3 / G05-F3a — still open; not proof of validator rejection.

A visible 1080-row image in a 1088-row NV12 allocation is copied as consecutive visible rows, so UV starts at allocation row 1080 instead of 1088. Driver occurrence and visible severity were not measured. The plane layout follows the [Microsoft NV12 resource contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format).

**Source:** [src/capture/media_capture.cpp:1080](../src/capture/media_capture.cpp#L1080); [src/capture/media_capture.cpp:1105](../src/capture/media_capture.cpp#L1105); [src/capture/media_capture.cpp:1118](../src/capture/media_capture.cpp#L1118).

**Change to consider:** Copy visible Y and UV separately using the actual allocation height for UV origin and validate extents.

**Acceptance/measurement (not run):** Extra allocated luma rows with sentinels and distinct UV color bars; verify compact result and decoded colors.

<a id="cap-4"></a>

#### CAP-4 — RGBA fallback is labeled BGRA-compatible RGB32

**P2 · Existing conditional defect.** AST-B4 / G05-F3b — still open; driver occurrence unverified.

An R8G8B8A8 texture is copied unchanged then labeled MF RGB32. CPU conversion interprets those bytes as BGRA, reversing red/blue. Normal GPU conversion and correct existing RGB32 handling are separate.

**Source:** [src/capture/media_capture.cpp:1066](../src/capture/media_capture.cpp#L1066); [src/capture/media_capture.cpp:1115](../src/capture/media_capture.cpp#L1115); [src/common/frame_pipeline.cpp:110](../src/common/frame_pipeline.cpp#L110); [src/common/image_processing.cpp:66](../src/common/image_processing.cpp#L66).

**Change to consider:** Normalize this specific RGBA branch to BGRA, or preserve an explicit format through a real conversion.

**Acceptance/measurement (not run):** RGBA asymmetric red/blue texture through production readback, CPU conversion and photo-original path.

<a id="cap-5"></a>

#### CAP-5 — Unsupported dynamic format silently stalls capture

**P2 · Existing conditional defect.** AST-RES-R3 — still open.

A CURRENTMEDIATYPECHANGED RGB24 format with valid stride can be accepted, but CopyCaptureBuffer rejects every later sample. The loop continues without error/reconnect.

**Source:** [src/capture/media_capture.cpp:904](../src/capture/media_capture.cpp#L904); [src/capture/media_capture.cpp:919](../src/capture/media_capture.cpp#L919); [src/capture/media_capture.cpp:1608](../src/capture/media_capture.cpp#L1608); [src/capture/capture_buffer.cpp:27](../src/capture/capture_buffer.cpp#L27).

**Change to consider:** Validate supported subtype/layout before adoption; report/recover once or renegotiate supported output.

**Acceptance/measurement (not run):** Script valid frames → RGB24 type change → more samples; assert explicit recovery/error rather than endless drops.

### Application runtime

<a id="app-1"></a>

#### APP-1 — Completed GPU readbacks wait for another presentation

**P2 · Existing defect.** AST-A5 / OP-LIFE-A5 — still open.

Readback completion is drained only inside PresentLatestCudaScene. If the last presentation submits a request then the camera stalls, idle ticks skip that call. Assistant times out after five seconds; photo/annotation results can remain pending.

**Source:** [src/app/app_pipeline_runtime.cpp:1017](../src/app/app_pipeline_runtime.cpp#L1017); [src/app/app_pipeline_runtime.cpp:1087](../src/app/app_pipeline_runtime.cpp#L1087); [src/app/app_pipeline_runtime.cpp:1945](../src/app/app_pipeline_runtime.cpp#L1945); [src/app/app_assistant.cpp:239](../src/app/app_assistant.cpp#L239).

**Change to consider:** Poll outstanding completions independently of scene presentation or camera processing.

**Acceptance/measurement (not run):** Complete a fake readback after the final frame; require delivery without another presentation; include resize/cancel.

<a id="app-2"></a>

#### APP-2 — Current-view analysis can reuse a previous camera image

**P2 · Existing defect.** AST-A8 — still open.

Camera stop clears scene state but not CPU presentation pixels/dimensions. Analysis fallback accepts that buffer without a session token; after CPU A → failed B, an old frame can be submitted as current.

**Source:** [src/app/app_pipeline_runtime.cpp:1729](../src/app/app_pipeline_runtime.cpp#L1729); [src/app/app_pipeline_runtime.cpp:2226](../src/app/app_pipeline_runtime.cpp#L2226); [src/app/app_assistant.cpp:76](../src/app/app_assistant.cpp#L76); [src/app/app_assistant.cpp:163](../src/app/app_assistant.cpp#L163).

**Change to consider:** Invalidate or generation-tag the CPU presentation buffer and require current-session readiness.

**Acceptance/measurement (not run):** CPU A → failed B and CPU → GPU → camera failure; assert no stale attachment reaches request submission.

<a id="app-3"></a>

#### APP-3 — Keyboard zoom does not dirty the cached viewport

**P2 · New defect.** New.

Ctrl +/- blocks slider signals and updates zoom/persistence without marking the viewport dirty. Low-rate or stalled capture shows the changed value but not the changed image until another dirtying event.

**Source:** [src/ui/main_window.cpp:4111](../src/ui/main_window.cpp#L4111); [src/app/interaction_controller.cpp:160](../src/app/interaction_controller.cpp#L160); [src/app/interaction_controller.cpp:174](../src/app/interaction_controller.cpp#L174); [src/app/app_settings.cpp:97](../src/app/app_settings.cpp#L97); [src/app/app_pipeline_runtime.cpp:1947](../src/app/app_pipeline_runtime.cpp#L1947).

**Change to consider:** Mark the viewport dirty for every effective zoom change, including zoom enablement.

**Acceptance/measurement (not run):** Cached scene with no subsequent camera frame: keyboard zoom must present again without reprocessing capture.

<a id="app-4"></a>

#### APP-4 — Rotation leaves ink in the old image coordinates

**P2 · Existing defect.** AST-RES-R2 — still open.

Rotation transforms image/zoom focus but not annotation geometry/history. A mark at (0.25,0.5) stays there when a 180° image rotation moves its feature to (0.75,0.5). Recorded ink can inherit the misalignment.

**Source:** [src/app/app_controls.cpp:525](../src/app/app_controls.cpp#L525); [src/ui/main_window.cpp:2977](../src/ui/main_window.cpp#L2977); [src/ui/annotation_overlay.cpp:559](../src/ui/annotation_overlay.cpp#L559); [src/app/app_pipeline_runtime.cpp:1167](../src/app/app_pipeline_runtime.cpp#L1167).

**Change to consider:** Transform strokes and undo/redo consistently with image rotation; define text/shape orientation.

**Acceptance/measurement (not run):** Quarter/half turns with existing ink, undo/redo and recorded/exported alignment.

<a id="app-5"></a>

#### APP-5 — Camera arrivals keep the user-motion timer active

**P3 · Optimization.** AST-P-O1 / OP-PROJ-P3a — still open.

Each camera frame restarts a 150 ms motion tail. A 30 FPS feed never reaches intended idle camera-rate scheduling on a faster display. Extra timer wakeups are established; extra temporal processing or FPS loss is not.

**Source:** [src/app/app_pipeline_runtime.cpp:940](../src/app/app_pipeline_runtime.cpp#L940); [src/app/pipeline_orchestrator.cpp:125](../src/app/pipeline_orchestrator.cpp#L125); [src/app/pipeline_orchestrator.cpp:361](../src/app/pipeline_orchestrator.cpp#L361).

**Change to consider:** Separate scene dirtiness from user-motion notification.

**Acceptance/measurement (not run):** Fake-clock idle/pan/zoom cadence checks, then measure wakeups and CPU use.

### Recording

<a id="rec-1"></a>

#### REC-1 — Worker starts before its mutex and queues exist

**P1 · New defect.** New.

Ordinary construction starts the worker before later-declared synchronization/state members are initialized. Early scheduling can access unconstructed objects; no crash frequency was measured. The member-order rule is specified in the [C++ working draft](https://eel.is/c++draft/class.base.init#15.3).

**Source:** [src/app/recording_manager.cpp:169](../src/app/recording_manager.cpp#L169); [include/okuflow/app/recording_manager.hpp:225](../include/okuflow/app/recording_manager.hpp#L225); [src/app/recording_manager.cpp:1509](../src/app/recording_manager.cpp#L1509).

**Change to consider:** Launch the thread at the end of the constructor body after all member/setup initialization.

**Acceptance/measurement (not run):** Camera-free repeated construction/shutdown plus a controlled worker-entry check.

<a id="rec-2"></a>

#### REC-2 — CPU recordings omit visible Draw annotations

**P2 · New defect.** New; limited to CPU routes.

CPU/passthrough/debug presentation submits plain scene pixels without a stroke snapshot/composite. Only the GPU recording path burns in ink, despite processed-MP4 behavior documented in README.

**Source:** [src/app/app_pipeline_runtime.cpp:2110](../src/app/app_pipeline_runtime.cpp#L2110); [src/app/app_pipeline_runtime.cpp:2320](../src/app/app_pipeline_runtime.cpp#L2320); [include/okuflow/app/recording_manager.hpp:155](../include/okuflow/app/recording_manager.hpp#L155); [src/app/recording_manager.cpp:1116](../src/app/recording_manager.cpp#L1116).

**Change to consider:** Carry immutable strokes/transform to CPU processing and composite processed output while preserving clean originals.

**Acceptance/measurement (not run):** Decoded CPU-route recording with distinctive ink, erase/undo and pan; processed includes ink, original does not.

<a id="rec-3"></a>

#### REC-3 — BGRA fallback can enter an NV12-configured writer

**P2 · Existing conditional defect.** AST-REC-R3 / G03-F1a — still open.

A GPU-started recording negotiates NV12; later CPU fallback AddFrame copies BGRA without conversion or contract change. Encoder-specific failure/damage is unmeasured; ordinary pool exhaustion without CPU pixels instead drops the pair. The negotiated format is the [sink writer input contract](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-setinputmediatype).

**Source:** [src/common/media_writer.cpp:523](../src/common/media_writer.cpp#L523); [src/common/media_writer.cpp:763](../src/common/media_writer.cpp#L763); [src/common/media_writer.cpp:1022](../src/common/media_writer.cpp#L1022); [src/app/recording_manager.cpp:1133](../src/app/recording_manager.cpp#L1133); [src/app/app_pipeline_runtime.cpp:1215](../src/app/app_pipeline_runtime.cpp#L1215).

**Change to consider:** Maintain an explicit input-route contract; convert fallback to NV12 or intentionally segment both streams.

**Acceptance/measurement (not run):** Decode GPU → CPU → GPU transition fixtures; check colors, paired frame identity and timing.

<a id="rec-4"></a>

#### REC-4 — Live canvas-size change ends GPU recording

**P2 · Existing defect.** AST-REC-R2 — still open.

The live resolution combo changes cloned texture dimensions, but segmentation checks only original dimensions/FPS. The processed writer rejects its new extent and the session fails.

**Source:** [src/app/app_bootstrap.cpp:486](../src/app/app_bootstrap.cpp#L486); [src/app/recording_manager.cpp:284](../src/app/recording_manager.cpp#L284); [src/app/recording_manager.cpp:1052](../src/app/recording_manager.cpp#L1052); [src/common/media_writer.cpp:1051](../src/common/media_writer.cpp#L1051).

**Change to consider:** Freeze canvas per session or intentionally segment the synchronized pair when it changes.

**Acceptance/measurement (not run):** Active recording canvas changes in both orientations: continuity or intentional paired parts, no rejection.

<a id="rec-5"></a>

#### REC-5 — Each recorded GPU frame repeats resource setup

**P3 · Optimization.** AST-REC-O2 — still open.

Each of two writers opens a shared resource, creates input/output views, an NV12 texture and a query per sample. The BGRA canvas pool does not remove this work.

**Source:** [src/common/media_writer.cpp:1088](../src/common/media_writer.cpp#L1088); [src/common/media_writer.cpp:1204](../src/common/media_writer.cpp#L1204); [src/common/media_writer.cpp:1213](../src/common/media_writer.cpp#L1213); [src/common/media_writer.cpp:1273](../src/common/media_writer.cpp#L1273).

**Change to consider:** Cache by stable slot/device generation; pool NV12/query resources only after conversion and encoder-consumer retirement.

**Acceptance/measurement (not run):** Allocation/open counts, encoder-submit p50/p95 at 1080p/4K, delayed-consumer/rejected-sample tests and decoded comparison.

### Qt UI / accessibility

<a id="ui-1"></a>

#### UI-1 — Streaming moves the reader's selection and scroll

**P2 · New defect.** New functional consequence adjacent to AST-A-O3.

Every answer delta sets the editor cursor to End and ensures visibility. Selecting or reading earlier text is interrupted by the next delta in floating and Advanced chat. Qt documents the scrolling behavior of [ensureCursorVisible](https://doc.qt.io/qt-6/qtextedit.html#ensureCursorVisible).

**Source:** [src/ui/assistive_overlay.cpp:543](../src/ui/assistive_overlay.cpp#L543); [src/app/app_bootstrap.cpp:1033](../src/app/app_bootstrap.cpp#L1033).

**Change to consider:** Append with a separate document cursor; preserve selection/scroll unless already following the bottom.

**Acceptance/measurement (not run):** Long streamed answer; select/scroll back, append deltas, verify preserved reading state and optional bottom-follow.

<a id="ui-2"></a>

#### UI-2 — Draw rail exceeds supported short viewports

**P2 · New conditional defect.** New; distinct from assistant-overlap candidate.

Six 72-pixel buttons plus spacing/margins require at least 471 logical pixels, while a 320×240 render surface is supported. PositionToolbar has no reflow/scroll path. Source geometry predicts clipped lower controls in short views; no desktop reproduction ran.

**Source:** [src/ui/annotation_overlay.cpp:226](../src/ui/annotation_overlay.cpp#L226); [src/ui/annotation_overlay.cpp:917](../src/ui/annotation_overlay.cpp#L917); [src/ui/main_window.cpp:1721](../src/ui/main_window.cpp#L1721).

**Change to consider:** Reflow or scroll to actual safe area while keeping all tools and Done reachable.

**Acceptance/measurement (not run):** Compact/intermediate production fixtures with chrome, EN/TR/DE and RTL; assert actionable controls remain reachable.

<a id="ui-3"></a>

#### UI-3 — Simple transcript has no viewport height budget

**P2 · New conditional defect.** New.

Three final segments of up to 8192 characters each plus a 2048-character partial go into a word-wrapped, non-scrollable QLabel. Width/top are constrained, not height. Long/newline-heavy supported input can cover or exceed a short view; native appearance untested.

**Source:** [src/ui/main_window.cpp:2928](../src/ui/main_window.cpp#L2928); [src/ui/main_window.cpp:3662](../src/ui/main_window.cpp#L3662); [include/okuflow/common/transcript.hpp:49](../include/okuflow/common/transcript.hpp#L49).

**Change to consider:** Use a measured line/height budget with a trailing text window; keep full transcript/notes intact.

**Acceptance/measurement (not run):** Long and newline-heavy segments in compact windows; containment, latest text and unchanged full transcript.

<a id="ui-4"></a>

#### UI-4 — Unavailable voice picker can erase the saved voice

**P3 · New conditional defect.** New.

Loading/no-voices placeholders have no QVoice data, yet result() unconditionally overwrites saved voice fields from that default value when unrelated settings are accepted.

**Source:** [src/ui/ai_settings_dialog.cpp:705](../src/ui/ai_settings_dialog.cpp#L705); [src/ui/ai_settings_dialog.cpp:635](../src/ui/ai_settings_dialog.cpp#L635); [src/app/app_assistant.cpp:18](../src/app/app_assistant.cpp#L18).

**Change to consider:** Preserve the preferred voice unless a valid enumerated selection replaces it.

**Acceptance/measurement (not run):** Saved-voice fixture with loading/error placeholder; accept unrelated edit; also test valid replacement.

<a id="ui-5"></a>

#### UI-5 — Transcript partials rebuild all camera chrome

**P3 · Optimization.** OP-RESP-F2a — still open; frequent caller clarified.

Changing transcript partials can publish every 75 ms, rebuilding button constraints, grid items/layouts and raising native panels through full chrome geometry. The pointer fast path does not cover this caller.

**Source:** [src/app/transcription_session_controller.cpp:38](../src/app/transcription_session_controller.cpp#L38); [src/app/app_bootstrap.cpp:1193](../src/app/app_bootstrap.cpp#L1193); [src/ui/main_window.cpp:2952](../src/ui/main_window.cpp#L2952); [src/ui/main_window.cpp:3443](../src/ui/main_window.cpp#L3443); [src/ui/main_window.cpp:3781](../src/ui/main_window.cpp#L3781).

**Change to consider:** Separate caption placement from chrome layout; rebuild only when geometry/content state changes and coalesce work.

**Acceptance/measurement (not run):** Count geometry passes under synthetic partials; then measure Qt heartbeat and presentation with identical inputs.

### Assistant / settings

<a id="ai-1"></a>

#### AI-1 — Mode-off during image preparation leaves Assistant busy

**P2 · Existing defect.** AST-A3 / OP-LIFE-A3 — still open.

Disabling assistive mode invalidates preparation without a persistent terminal event. The response-open flag blocks further prompts until New Conversation; not a permanent app hang.

**Source:** [src/app/app_assistant.cpp:191](../src/app/app_assistant.cpp#L191); [src/common/assistive_runtime.cpp:506](../src/common/assistive_runtime.cpp#L506); [src/common/assistive_runtime.cpp:927](../src/common/assistive_runtime.cpp#L927); [src/app/app_bootstrap.cpp:1046](../src/app/app_bootstrap.cpp#L1046).

**Change to consider:** Use request-owned cancellation with exactly one terminal event and note cleanup.

**Acceptance/measurement (not run):** Hold preparation, disable mode, release worker; no provider request, one terminal event, next prompt admitted.

<a id="ai-2"></a>

#### AI-2 — Canceled preparation clears a newer request's note payload

**P2 · Existing defect.** AST-A4 / OP-LIFE-A4 — still open.

Cancel A, start B, then let A's stale callback run: global ClearPendingAssistantNote removes B's question/image. B's answer may succeed but its saved note lacks those inputs.

**Source:** [src/common/assistive_runtime.cpp:774](../src/common/assistive_runtime.cpp#L774); [src/common/assistive_runtime.cpp:854](../src/common/assistive_runtime.cpp#L854); [src/common/assistive_runtime.cpp:927](../src/common/assistive_runtime.cpp#L927); [src/common/assistive_runtime.cpp:1412](../src/common/assistive_runtime.cpp#L1412).

**Change to consider:** Own preparation and note payload by the same generation; stale callbacks clean only their request.

**Acceptance/measurement (not run):** Hold A, cancel, start B, release A before B finishes; B's saved question/image must remain.

<a id="ai-3"></a>

#### AI-3 — Delayed history replaces the latest selection or answer

**P2 · Existing defect.** AST-A7 / OP-LIFE-A7 — still open.

History completions unconditionally replace current thread/transcript and response-open state. Select A then B and deliver A last, or deliver old history during a new answer; newer state is overwritten. Server data is not deleted.

**Source:** [src/app/app_assistant.cpp:295](../src/app/app_assistant.cpp#L295); [src/common/codex_app_server_client.cpp:391](../src/common/codex_app_server_client.cpp#L391); [src/app/app_bootstrap.cpp:988](../src/app/app_bootstrap.cpp#L988); [src/app/app_bootstrap.cpp:1035](../src/app/app_bootstrap.cpp#L1035).

**Change to consider:** Generation-tag selection; invalidate history loads on newer selection, New or prompt dispatch.

**Acceptance/measurement (not run):** Delayed A/B replies and an old history reply during streaming; latest selection/answer must win.

<a id="ai-4"></a>

#### AI-4 — Stop-window transcript deltas are lost without a final

**P2 · New conditional defect.** New.

Stopping rejects user deltas even though the controller preserves partial tail on close without done. A delta after stop followed by closed without done is lost. Normal final/done during stopping is accepted and avoids this issue; live service ordering was not observed.

**Source:** [src/common/codex_realtime_transcription_client.cpp:440](../src/common/codex_realtime_transcription_client.cpp#L440); [src/common/codex_realtime_transcription_client.cpp:535](../src/common/codex_realtime_transcription_client.cpp#L535); [src/app/transcription_session_controller.cpp:109](../src/app/transcription_session_controller.cpp#L109).

**Change to consider:** Accept same-thread user deltas during Stopping under existing limits, reject after closure.

**Acceptance/measurement (not run):** Fake stop → delta → closed without done, plus late-final coverage to prevent duplicate text.

<a id="ai-5"></a>

#### AI-5 — Failed key clearing loses credential deletion intent

**P2 · New conditional defect.** New.

Preparation clears the live credential ID while deletion target is stack-local. Failed JSON save or later credential deletion loses retry identity. Old secret residue can remain; no exposure or continued empty-key runtime use is established.

**Source:** [src/ui/ai_settings_dialog.cpp:614](../src/ui/ai_settings_dialog.cpp#L614); [src/app/settings_controller.cpp:274](../src/app/settings_controller.cpp#L274); [src/app/settings_controller.cpp:232](../src/app/settings_controller.cpp#L232); [src/app/settings_controller.cpp:238](../src/app/settings_controller.cpp#L238).

**Change to consider:** Retain deletion intent until both stores reach the requested state; do not clear the only live identity during preparation.

**Acceptance/measurement (not run):** Fake stores: first settings save fails then retry; separately deletion fails then retry; never use real secrets.

## Previously reported claims that should not be repeated unchanged

| Prior claim | Current verdict | Evidence / reason |
|---|---|---|
| Sauvola mishandles light text on dark backgrounds (AST-UP-F1) | Fixed in source | `cuda_kernels.cu:3387` contains the polarity correction; the earlier CUDA regression exists. The later checkpoint in report 43 supersedes its old open row. |
| Glare-only/focus-only/CLAHE-only production triggers | Obsolete as originally stated | `ApplyTextClarityMaster` clears children when off (`cuda_interop.hpp:166`); when on, automatic core stages are active (`cuda_interop.cpp:3842`). KERNEL-4 uses the currently reachable dispatch instead. |
| All busy CUDA-stream destruction is invalid | Refuted | Stream destruction itself was already distinguished from prematurely freeing referenced resources in `verified-non-issues.md`. |
| Mapped external-image cleanup leaks all mappings (AST-B2) | Fixed in source | `ReleaseMappedExternalImage` frees the mappings on destruction and partial construction (`cuda_interop.cpp:444,485,577`). |
| Failed CUDA frame cancels its fence while writes remain active | Fixed path present | `RunCudaPipeline` drains before cancellation (`app_pipeline_runtime.cpp:897`); failed drain retains faulted resources. CUDA-2 concerns unnecessary recovery for Busy, not permission to remove real-error retirement. |
| Same-extent rotation resets the shared fence timeline (AST-P-2) | Fixed in source | Rotation uses `InvalidateRecordingFramePairing`; that no longer resets the shared timeline (`app_pipeline_runtime.cpp:314`). |
| SuperRes strength changes never reach the SDK | Invalid for the current production path | The UI calls `ResetSuperRes` and recreates load-time parameters. Excessive reset breadth is CUDA-3. |
| ResetKeystone immediately reuses an unsafe host buffer | Not established | Replacement copies are ordered on the same stream, and consumption waits for the replacement event. |
| Gaussian is a naive full 2D convolution | False | CUDA uses horizontal/vertical passes (`cuda_kernels.cu:521,524`), cached coefficients and an 8-bit intermediate. CPU blur is separable too. |
| Camera-texture ownership and presenter pool/timeline repairs remain missing | Fixed guards present | Retained MF samples (`media_capture.cpp:1652`), producer-fence plus consumer-ownership checks (`presenter.cpp:137,203`), external submission dependencies and reservation ownership remain in source. |
| Codex client opening/initialization/idle lifecycle fixes AST-A1/A2/A6 are absent | Fixed guards present | Request generations, opening guards, initialization retirement, scoped notifications and idle terminal guards are present. AI-1/2/3 are distinct remaining app/runtime issues. |
| Recent rectangular chrome, keyboard navigation, Text Clarity gating, startup restore and translated color-label fixes are still pending | Source repairs present | Current `8c07e0b` includes them. Old pending-test notes are superseded by report 43's later checkpoint; this audit did not rerun those tests. |
| Failed recording finalization always deletes the files / rejected samples lose their retry state | Source repairs present | Preservation/finalization history and retry state remain in `media_writer.cpp:618,951` and `recording_manager.cpp:1283`. This does not validate every hardware encoder path. |

These verdicts supersede only the named claims, not every entry in the historical reports. “Not established” is not the same as proving a claim impossible. Prior Gemini, Opus, Daybreak and Astra claim-by-claim outcomes remain in [43 — Audit verdicts](43-audit-verdicts-2026-10-10.md); the 294 rows there include duplicates and split claims and must not be treated as 294 unique bugs or a comparative model score.

## Deferred observations and documentation drift

- **AST-REC-R4:** source timestamps are replaced by delivery timing (`audio_capture.cpp:393–448`, `recording_manager.cpp:1086`). Two 20 ms audio blocks delivered 1 ms apart can receive overlapping intervals. Actual A/V drift was not measured; isolate clock mapping before an implementation.
- **AST-REC-O1/O5:** explicit recording colorimetry and unchanged-ink raster caching remain measurement/quality candidates; this review did not establish a universal hue defect or a speed benefit.
- The upload ring currently has **three** slots (`cuda_interop.hpp:496`); documentation saying two is stale. Keep that corrected when implementing CUDA-2.
- Nonconstant vendor numerical reference patterns are still needed before claiming NIS/FSR fidelity. Existing border/invariant checks do not establish full vendor equivalence.
- This was a bounded cross-module review, not a new exhaustive security audit. AI-5 establishes failed secret-deletion retry/residue, not credential exposure. Previously deferred security claims require their own current validation before implementation.

## Blur and broader optimization strategy

Gaussian blur is already separable. For a seven-tap one-dimensional kernel that is 14 logical taps across two passes instead of 49 for naive 7×7 filtering; it is not a newly delivered speed improvement and is not an end-to-end timing ratio. The default optional blur is off, and the 8-bit intermediate means exact floating-point equivalence should not be assumed.

Kawase/Dual Kawase would be approximate image-quality experiments, particularly risky to substitute blindly into text statistics or sharpening. The immediate source-backed opportunities are narrower: avoid an unused first mean-square result, avoid sharpening pixels that explicit Adaptive Text replaces, avoid unnecessary Maxine reloads, and avoid treating ordinary queue pressure as failure. After those, compare any new blur with original frame detail, thin text, halos, borders, temporal stability and whole-frame timing. No 95%, 98.5% or 99.8% application speed claim is supported here.

Use [44 — Pipeline optimization plan](44-pipeline-optimization-plan-2026-10-10.md) as the measurement map, with the dispatch corrections above. After implementing shared behavior, run the tracked `scripts/agent_build.bat` release/CPU/CUDA matrix and targeted deterministic regressions. Native GUI tests remain serial and require an appropriate desktop window; real-camera/encoder validation is distinct from synthetic fixtures.

## Website continuation completed

The website's public “generated scene” wording removal was already deployed as website commit `1504157`. This continuation verified an HTTP 200 response from [okuflow.com](https://okuflow.com/), the neutral demonstration-scene caption, absence of English/Turkish generated wording in the fetched homepage, and the expected sticky-header CSS reference. The Opus homepage remains the default. No images or internal media provenance were changed by this audit.

The original read-only audit ended before application implementation. See the
later implementation checkpoint at the top for the subsequent working-tree
changes and validation; no commit, push, or deployed bundle is implied.
