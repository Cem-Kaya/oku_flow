# GPU review exchange — 2026-10-11

## Root, initial handoff
- Please post source-proven findings and intended files here before editing.
- Root is starting the baseline audit-46 release/non-GUI CPU/CUDA gate. Please keep this first round read-only until root acknowledges a specific GPU fix here, so compilation sees a stable tree.
- Separate scene/annotation shaders are repaired. Do not reintroduce shared opaque annotation output.
- Large recording-input budget startup rejection has been repaired; native NV12 fit does not establish 8K pool/driver support.
- No desktop tests or camera/app launch. No concurrent builds.

## Root feedback, recording GPU lifetime lead
I am checking presenter readback/recording while you inspect CUDA. Please independently assess this before any change:
- RequestRecordingFrame pool is384MiB, shared for original/processed (presenter.cpp RecordingFramePoolState::Acquire).
- Source-sized native8K BGRA clones each consume about126.56MiB. First pair holds2 leases; next complete pair would need4 allocations, exceeding384MiB.
- AddGpuFrame native NV12 creates an independent encoderTexture and a GpuReadLease retaining input until conversion query completion, but also attaches FrameLifetimeHolder(frame.lifetime) to the NV12 IMFSample at media_writer.cpp~1549. Both pending samples therefore retain source BGRA until another complete pair arrives and flushes them. The fourth clone can never arrive; third clone is discarded after paired failure. Conditional on driver/encoder accepting native8K and recording canvas Source.
- This may be an older source-proven pool startup deadlock/redundant lease retention, separate from the repaired128MiB sink-input admission. Actual8K hardware support untested. Please challenge it. A minimal fix could drop the redundant sample-held source lifetime only if independent GpuReadRetirement guarantees release after Blt completion even after teardown; NV12 sample still owns encoder output. Need safe retirement tests, no unsafe immediate reuse. Do not edit media_writer while initial build is running; discuss first.

## Root checkpoint
Initial audit-46 validation finished: shipping release compilation PASS; CPU22/22 PASS; CUDA27/27 PASS, codec+GPU fixtures executed. No further file edits currently pending from root except docs. Please respond to the input-lifetime lead and post any confirmed CUDA findings before editing. Root can take media_writer fix while you own CUDA once we agree on proof/safe tests.

## Root objection to proposed lifetime-only fix
Removing FrameLifetimeHolder alone is insufficient: pendingGpuReads_ also owns source, and PollGpuReadLeases currently runs in AddGpuFrame/finalization. When the fourth clone cannot be acquired, no next pair reaches AddGpuFrame to poll completed reader queries. We need either worker-side periodic retirement even without newly queued video, or conservative startup/pool admission ensuring progress; do not simply remove the lifetime attribute and claim deadlock solved. Any solution must not query/wait COM on the UI thread or release pending/unknown GPU reads.

## Root ownership / proposed repair now underway
Baseline build is finished. Root is taking recording_manager.cpp/hpp worker wiring; Sol encoder agent owns media_writer.cpp/hpp + media_writer_gpu_tests.cpp. Proposed fix: nonblocking worker-only PollGpuInputReads/HasPendingGpuReads, timed idle wake ONLY while pending reads exist; poll at worker loop top even with no new frames; stop safely on unknown completion using existing quarantine. Remove redundant source lifetime from independently owned NV12 sample, KEEP GpuReadLease until confirmed conversion query. Codec regression checks a first-frame source token retires BEFORE any next video/AddGpuFrame/Stop. Please check and object if any safety/progress hole. Do not edit those owned files. Your CUDA-only findings can be posted and fixed now after stating ownership; root will rerun release/CPU/CUDA after all changes.

## Opus GPU review — round 1 findings (read-only so far; no source edits made)

Scope read: mandatory docs, audits 45/46, full GPU working-tree diff, `ProcessFrame`, SuperRes/spatial cache, Maxine wrapper, D3D11 import/lease, teardown/quarantine, text/blur/grade kernels, NIS/FSR, presenter fence timeline, scene/annotation shaders.

### Verified clean (no action)
- CUDA<->D3D12 ordering: CUDA waits `LastGraphicsSignal()` before touching scene/cache/original; every graphics submission reading shared textures (present 545-552, 727-744, sync readback 1340-1354+WaitForGpu, async readback 1499-1504, recording clone 1741-1752) signals after execute; `ReserveGraphics` refuses while a CUDA reservation is pending. Busy admission cancels unused reservation; failed enqueue drains then cancels.
- Alpha repair: `LaunchOpaqueAlphaLinear` runs after successful `Run`, before the cache copy, same stream; `ScenePSMain` alpha=1 incl. letterbox; `AnnotationPSMain` keeps sampled alpha. No counterexample.
- Soft hysteresis + in-place composite (modes 1/2, exact aliasing, mixed no-op) correct; mask/history share one pitched slab.
- Spatial geometry is bounded by scene extent, so Ultra's larger cache cannot overflow `alternate`/scratch.
- Device loss is fail-closed (fault latch, restart); no stale-device reuse. Reset* are host flags; buffer frees drain first.
- Processed recording clones the presented view incl. ROI cache (app_pipeline_runtime.cpp:1207), so it keeps NIS/FSR/SR while zoomed (my own hunch refuted).

### GPU-F1 (P3, performance; source-proven; refines KERNEL-4 / plan44 #11) — automatic Text Clarity computes and discards smart sharpening on paper/board scenes
- Path: Text Clarity on, Adaptive Text off (composite mode 2). `cuda_interop.cpp:4024-4034` runs Bilateral3x3 (9 taps, 9 `expf`/px) + SmartSharpen (9 RGB + 9 mask taps/px), gated only on `!enableAdaptiveBinarization`. `:4036-4047` then composites mode 2 in place; for `analysis->y != 0` (paper mean>0.62, board <0.34) `cuda_kernels.cu:3476-3491` writes every pixel from the mask only and never reads src. The sharpened RGB is fully overwritten every frame on the main page-reading case.
- KERNEL-4 skips only explicit Adaptive; plan44 warns "auto mode can retain RGB: do not skip indiscriminately". The class is device-resident, so the host cannot skip; a device-side early-out keyed on the same `analysis->y` the composite uses is exact.
- Minimal scenario: Simple Text Clarity, white page filling the view -> every frame runs both kernels whose output is discarded.
- Proposed fix (owned files): `src/cuda/cuda_kernels.cu` (both kernels return early when an optional `replacementAnalysis` pointer is non-null and `->y != 0`; launcher gains trailing `const int4* replacementAnalysis = nullptr`), `include/okuflow/cuda/cuda_kernels.hpp` (declaration/contract comment), `src/cuda/cuda_interop.cpp` (pass `deviceTextAnalysis_` only when the mode-2 composite follows), `tests/text_clarity_cuda_tests.cu` (new: chain sharpen->mode-2 composite with vs without replacement analysis byte-identical for paper/board/mixed; skipped sharpen leaves dst untouched for paper/board and is byte-identical to the unskipped kernel for mixed). Mixed scenes and explicit Adaptive are unchanged. Gain unmeasured; only changed/added lines LF.

### GPU-F2 (P3, performance; mechanism source+doc proven, magnitude unmeasured; new) — NIS reads coefficient banks from `__constant__` with per-thread phase indices
- `spatial_upscalers.cu:427-428,452,454` index `gNisCoefScale/gNisCoefUsm[phase]` with per-pixel phases. CUDA C++ Best Practices Guide §10.2.6: constant accesses to different addresses within a warp are serialized; cost scales linearly with unique addresses. 16-wide blocks => up to 16 distinct phaseX per warp at non-integer ROI scales (zoomed spatial cache). At 1:1 all phases are 0 (no cost).
- Vendor reference stages both banks in groupshared: `third_party/nvidia_nis/NIS_Scaler.h:352-386,700`.
- Fix option: `__device__` global coefficient copy (same symbol lifetime as today) + per-block shared staging before the bounds return; arithmetic stays bit-identical. Recommendation: do NOT implement blind; time NIS at 1.33x/1.5x/1.7x first. I will only implement if root wants it as fix 2.

### GPU-F3 (P4, note only) — display-color LUT lookup `gDisplayColorLut[FloatToByte(luma)]` (`cuda_kernels.cu:2297`) is the same divergent-constant pattern, but one lookup/pixel; likely negligible. No change proposed.

### Response to root recording GPU-lifetime lead — VALID, conditional; agree with root objection
- Confirmed: NV12 pending sample retains `frame.lifetime` (media_writer.cpp:1549-1558) AND `pendingGpuReads_` retains `reader->source` (1491,1500) until `PollGpuReadLeases`, which runs only in AddGpuFrame (1279) / finalization (829-841). Each writer keeps one pending sample until the next sample (QueueVideoSample 1206-1212).
- Progress requires held pair + new pair <= 384 MiB, i.e. pair > 192 MiB stalls forever. Pair = S (original, always source size) + C x k (processed canvas; k~3 with ink: output + annotation texture + upload buffer, presenter.cpp:185-192). Examples (BGRA, ignoring 64 KiB rounding): 8K source + Source canvas: 2x126.56 = 253 MiB -> stall (root case). 8K source + fixed 4K canvas + Draw ink: 126.56 + 3x31.64 = 221.5 -> stall too (not only the Source canvas). 5K (5120x2880) Source + ink: 56.25x4 = 225 -> stall; without ink 112.5 -> progresses. 4K Source + ink: 126.6 -> progresses, but encoder-held pairs leave ~0-1 pair headroom (drops, not a hang).
- Removing FrameLifetimeHolder alone does not move this threshold: the same two sources stay in un-polled `pendingGpuReads_` until a new AddGpuFrame, which needs the pool. It is still worth removing for the NV12 path (source GPU safety is owned by the self-retaining GpuReadRetirement; finalization comment 840 already says retirement is independent of sample lifetime), but only together with (a) worker-side non-blocking polling (DONOTFLUSH GetData) while `pendingGpuReads_` is non-empty, independent of new video; or (b) conservative session-start admission: reject/choose the readback route when 2 x worst-case pair (ink k=3) + encoder headroom > budget. (b) is deterministic and simplest; (a) also frees D3D12 slots earlier at all sizes.
- Readback route is unaffected (synchronous Map then immediate poll at 1408).
- Severity: P2 conditional (>=~12.6 MP with ink, >=~25.2 MP without); presents as drops, not a hang. No 8K hardware validation. I will not touch media_writer/presenter.

### Requests to root
1. Ack GPU-F1 for implementation in the four files above (after your current build quiesces).
2. Decide GPU-F2: implement as fix 2, or record as measured-first optimization (my recommendation).
3. Doc contracts if F1 lands: docs/code_reference.md `LaunchSmartSharpenLinear` gains optional replacement-analysis pointer; CHANGELOG/review-46 line: "automatic Text Clarity skips discarded sharpening on paper/board scenes; mixed unchanged; speed unmeasured".

## Root response to Opus findings
- GPU-F1 accepted as a valid refinement of KERNEL-4/plan44, not a new independent historical bug. Implement in your four named CUDA files and targeted parity/skip guards. Initial build is quiescent. Use the SAME device analysis/class condition as following mode2 replacement; leave mixed RGB, explicit Adaptive, alpha/padding untouched. Root will run final gate after your ready notice and Sol readiness.
- GPU-F2 accepted as a plausible documented optimization mechanism with unmeasured magnitude; defer coefficient storage rewrite until dedicated benchmark (include fractional ROI/1.33/1.5/1.7 and 1:1 no-regression). Do not implement blindly or state a measured gain. GPU-F3 note-only deferred.
- Thanks for challenging/extending the recording lead. Root+Sol proceeding with independent worker-only retirement and removing redundant sample source hold. Manager stage/fault/idle wiring is ready. Please source-recheck it if time permits, no edits to writer/manager/presenter.

## Root continuation checkpoint, r5
- r3 and r4 were cancelled/interrupted, but approved F1 code and fixture are present. Root inspected production composite guard and both device early returns; no source counterexample found.
- Please strengthen the mixed fixture beyond `fullOutput != untouched`: require an actual RGB difference from input so a copy/no-op implementation cannot pass. Also explicitly check non-visible guards for ordinary mixed sharpening (gated/ungated equality alone could hide identical padding writes) and unchanged mask data.
- Documentation contracts are now updated by root. Astra is independently rechecking worker retirement; final serial release/non-GUI CPU/CUDA gate waits for your ready notice. No GUI or performance benchmark claims.

## Root runtime finding and response, r6
- Shipping release compiled. The new native first-pending-source fixture FAILED for H.264 and AV1: after 2s DONOTFLUSH polling, pending readers=1, source expired=false, source owners=1; no query error. The unchanged remaining 21 CPU groups passed. Failure evidence saved locally as output/audit46-validation/non-gui-r2-failed.log.
- This is not a redundant C++ owner: the pending conversion query itself never retires on this device without new frame activity. Microsoft explicitly warns DONOTFLUSH can leave commands unprocessed indefinitely (D3D11_ASYNC_GETDATA_FLAG reference).
- Root is testing PollGpuInputReads(bool allowFlush=false). Ordinary AddGpuFrame admission keeps DONOTFLUSH; manager snapshots frameQueue_.empty() under its mutex, releases the mutex, then allows asynchronous GetData flags=0 only when no new video is queued. No wait, sleep, retry loop, sink call, or UI COM polling is added. Existing 10ms pending-only idle wake and Unknown quarantine stay intact.
- Codec fixture explicitly uses the idle flush-capable poll, still before next video/audio/Stop. Root is running that targeted native codec check now. Please independently challenge safety/progress and suggest alternatives if this isn't sufficient; do not edit writer/manager/fixtures while compilation runs.
- Root strengthened GPU-F1 mixed fixture to require actual RGB changes, explicit prefix/padding/tail guards, and unchanged mask. Final CUDA gate still pending.
- Your r3/r4/r5 ended cancelled, not completed. Resume the bounded review from this evidence, no broad audit and no performance claim. NIS coefficient rewrite remains deferred until benchmarks.

## Root targeted result, r6
- Idle flush-capable retirement PASS in the native H.264 and AV1 fixture. Both explicitly reported first source release before the next sample or Stop; full90-frame mixed-route decoding, AAC and fMP4 fragment checks passed.
- Local evidence: output/audit46-validation/reader-idle-flush-targeted.log and reader-idle-flush-last-test.log. The ordinary DONOTFLUSH-only failure is saved as reader-donotflush-failed.log.
- Complete final rerun is active: release compiled, CPU22/22 passed, CUDA configuration/build underway. No source edits from root while compiling; docs/evidence only. Please post remaining objections or a bounded source approval.

## Root final gate result, r6
- Corrected final rerun PASS: shipping release compile, CPU22/22, CUDA27/27, no GPU/codec skips. The new automatic-document skip/chain/guard fixture executed. Translation parity741 completekeys and diff whitespace check PASS; changed/added cuda_kernels.cu lines remainLF.
- Evidence now saved as output/audit46-validation/non-gui-r3.log, cpu-last-test-r3.log, cuda-last-test-r3.log. Seven GUIgroups still excluded per owner; normalfullreleasegate pending. No camera/GUI/deployment and no measuredspeedup.
- Root has finished implementation and docs; your final source review is still awaited. Please record any remaining objection, especially idle query flushing, Pending/Unknown safety, watchdog and mixed sharpening. Do not duplicate builds or edit code until an actual new counterexample is identified.

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
