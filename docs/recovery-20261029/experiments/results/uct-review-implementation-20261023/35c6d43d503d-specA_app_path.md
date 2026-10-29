# Specialist A: app-side UCT path audit

Frozen production baseline: 9bdd5d13f2352342860ba146ad164c7e46645d05. On-disk HEAD is a7d4757; the targeted controller/Resume document diff against 9bdd is empty. Contrary to the brief's chronology, the inspected controller already contains the held-copy Resume guard. This report does not re-report root 618. Read-only source inspection only: no execution, models, GPU, tests or desktop use. Worker execution internals and uct_region_planner.cpp were excluded.

Paths below are relative to the frozen tree. U = Qt UI thread; C = camera delivery thread; D = playback worker; V = independent device-validation scheduler; P = ProbeWorker; O = recognition scheduler worker.

## Three regimes

### 1. Live camera

- Acquisition: src/capture/media_capture.cpp:1781-1788 stamps native sample time separately from QPC-derived host arrival (QueryClock100ns, :727-739); :1857-1860 delivers on C. src/app/app_pipeline_runtime.cpp:1898-1938 enqueues bounded ingress and one queued U wake; src/app/pipeline_orchestrator.cpp:75-97 adds another coalesced queued dispatch. U consumes at app_pipeline_runtime.cpp:2521-2565. These acquisition/wakeup boundaries have no UCT trace events.
- Analysis is captured during CUDA processing, before appearance: src/cuda/cuda_interop.cpp:5110-5123, 80 ms steady-clock throttle. src/cuda/uct_analysis_pool.cpp:299-479 admits two slots, whole-pair 128 MiB limit, event-query/ownership refusal, stream-ordered copies, steady-ms lease timestamp. Busy skips; clean failure retries after 250 ms (include/okuflow/cuda/uct_analysis_pool.hpp:39-68). Copy and display GPU work overlap; no UCT capture/lease trace.
- U presentation: app_pipeline_runtime.cpp:1360-1366 calls controller preflight; :1442-1470 submits and reports success; only success plus matching frame/processing generation hands analysis over (:1496-1501). src/d3d12/presenter.cpp:609-611 refuses busy slots without waiting; :729-757 queues GPU dependencies/draw, calls Present(1,0), then signals reuse. Success means successful DXGI submission return, not completed scanout/photon delivery. The noninterop compatibility branch waits for GPU (:765-770; deadline :1969), so blanket “no presentation waits” would be false.
- U -> V: src/app/ultra_clear_text_controller.cpp:1084-1115 checks steady lease age <=1000 ms and exact frame/source, then SubmitDevice. V completion queues U (:225-260); U submits prepared current-frame probe and accepted-line references to P. P completion queues U (:205-217). U ApplyChecks/current validation (:1239-1289) precedes possible O submission (:1291-1345). Probe interval 80 ms, residual budget 60 ms, stability >=0.92, stable OCR refresh 1500 ms; GPU OCR is not given a parked pending lease while busy. These gates overlap ongoing presentation/OCR; they are not additive fixed delays.
- CPU fallback: app_pipeline_runtime.cpp:3079-3088 copies complete prezoom CPU scene to P before Present (:3145); :3165-3167 supplies the later success receipt. It bypasses V and the CUDA pool. Failure suppression still gates publication.

### 2. Advancing saved video

- D reads native samples at src/common/video_playback.cpp:615-669: PTS/duration are integer 100 ns; sampleId derives from PTS. PlaybackNowLocked (:262-267) advances a media origin using steady elapsed time or a recent audio clock. Bounded lookahead decode (:1205-1263) overlaps presentation/OCR; due-frame wait is interruptible, capped at 20 ms per iteration (:1275-1298). D publishes the snapshot (:1302-1310); U discovers it through a 16 ms timer, src/app/app_video_playback.cpp:194-196, :1252-1300.
- U GPU processing: src/app/app_video_gpu.cpp:192-212 -> src/app/media_gpu_scene.cpp:509-518 (nonblocking idle poll), :617-729 (history/source identity, processing, analysis). Same-frame retries use RefreshUctAnalysis (:544-561); cuda_interop.cpp:193-229 preserves the 80 ms throttle. App retries missing analysis at 80/160/320/640/640 ms, five attempts (app_video_gpu.cpp:86-134; include/okuflow/app/app.hpp:304).
- PresentSceneTexture receipt is committed at app_video_gpu.cpp:312-335, then U sends either owned analysis (:387-397) or a truthful no-analysis display receipt (:410-420). PTS is divided by 10,000 into SourceStamp milliseconds, never replaced by the lease's steady timestamp. CPU alternative is app_pipeline_runtime.cpp:2847-2998, especially :2945-2950 and :2994.
- controller.cpp:520-644 separates displayed-frame identity from last analysis input; :647-695 sends each frame to V/P once, with 80 ms probe spacing. :698-731 uses displayed media PTS for model time plus independent 350 ms wall freshness when display differs from validated frame. O admission additionally requires no outstanding advancing job and >=350 ms since submission (:1318-1345); a current job has a 30 s wall deadline (:1425-1435). Continuous no-analysis receipts do not refresh proof (:624-640).

### 3. Paused saved video, Freeze -> Resume

- Freeze: app_video_playback.cpp:1003-1013 invalidates selection/UCT, selects the actually presented lease, closes audio and calls PauseAtFrame. video_playback.cpp:189-218 validates issuance and holds exact pixels/PTS. Optional following-neighbour preparation waits 50 ms and has a 2 s decode budget (:1020-1079); it overlaps the held view.
- Current app admission is Standalone, not the historical pair: app_video_gpu.cpp:385-408 and app_pipeline_runtime.cpp:2914-2928. Thus neighbour arrival does not gate current paused glyphs. The opaque saved-file duration policy still exists at video_playback.cpp:1392-1397 for explicit corroborated clients.
- controller.cpp:414-478 retains exact selected input, stops watchdog, and submits once on backend readiness. :1574-1632 hands the selected job to O; :1716-1840 consumes its queued result and calls AdmitImmutable. Retry timers are 100 ms or 500 ms while preparing (:1674-1713), bounded by retry count/deadline. Settled optional detail waits 300 ms, at most 32 regions (:1843-1926). No periodic recognition of unchanged completed stills.
- Resume: app_video_playback.cpp:1028-1045 -> controller.cpp:481-517 -> src/common/ultra_clear_text.cpp:2769-2783 retains proven exact-source lines, clears pending evidence and fences optional work. The selected acquisition is already marked probed; only a distinct advancing frame can probe next. A normal native seek still occurs on Resume; media_gpu_scene.cpp:536-539 resets temporal history when request generation changes. This is separate from retaining accepted OCR references.

## Shared service boundary, consumption and release

Submit/SubmitDevice ownership API: src/common/ultra_clear_text.cpp:2443-2487; cancellation fences authority without freeing executing input (:2490-2519). Completion/status callbacks run on workers (include/okuflow/common/ultra_clear_text.hpp:515-549), then queued U receipt/admission at controller.cpp:267-291, :1378-1514. Trace has submit/dequeue/done per channel, UiReceipt, Admit, cancellation/reset/fault; worker hook locations were grepped only.

U model ApplyChecks preserves newer timestamps and unchecked proof, removes mismatches (ultra_clear_text.cpp:4042-4115). Eligibility uses <=350 ms or exact currently validated frame (:4138-4141); live recognition age is 4000 ms (:3375). controller.cpp:2001-2055 gates health/source/presentation/freshness, assigns changed words and calls accessibility ContentChanged. This schedules Qt update, not synchronous paint.

src/ui/ultra_clear_text_overlay.cpp:226-241 prepares cached layout; :642-784 computes projection/protection; :868-913 paints backing-store content, preserving the on-screen plan across export paints. OverlayAssigned/first-revision OverlayPainted are software endpoints. AnnotationOverlay only paints vector ink (src/ui/annotation_overlay.cpp:837-854); its chrome is excluded by the UCT plan.

Positioned accessibility is U-only: controller.cpp:2058-2082 filters model tokens; src/ui/uct_screen_text_accessible.cpp:665-805 revokes immediately, creates/rebinds semantic lines. Additions use one 1000 ms tick, up to 3000 ms reading hold (:843-860); initial/replacement publication can be immediate. Geometry is lazy from last paint (:300-343), not a new OCR/probe. AccessibilityScheduled/Published do not certify a client query, positioned readiness, receipt or speech.

Lease release is shared ownership, not a timed explicit API: controller exchanges candidate at :1291, releases rejected/unneeded local candidates on return, and drops advancing scene's strong device reference at :683. However app_video_gpu.cpp:123-131 retains mediaGpuAnalysis_ until recipe replacement/reset (:71); paused retention is intentional for detail/export. Pool reuse requires storage ownership and copy completion (uct_analysis_pool.cpp:340-370); uncertain storage retires/quarantines off-thread (:277-296). No release trace exists.

## Findings and exact optimization candidates

**A1 P2, high: app admission leaves O idle after a usable probe.** Counterexample: a distinct B probe finishes while A's advancing job is outstanding; controller.cpp:1318-1321 refuses B and :1291 consumes its candidate. A's result later clears advancingMediaJob_ (:1435), but :1508-1514 never schedules another probe/admission. :660 blocks B from probing again. Work waits for another distinct frame, even when B is a long-duration sample and pending text needs its second observation. Live GPU also relies on a later probe after busy refusal. Ceiling unknown, cadence-dependent. Exact opportunity: event-driven demand reconsideration on completion without parking another GPU lease or counting B twice. Risk: duplicate/stale authority. Falsifier: scripted A-busy/B-probe ordering showing B admitted after A completion without any new presentation/probe event.

**A2 P2, high: saved-video worker publication has no UI wake.** video_playback.cpp:1307 updates snapshot; app_video_playback.cpp:194-196/:1300 polls it. A just-missed tick adds nominally up to one 16 ms period, plus UI scheduling; high-rate snapshots can be overwritten before presentation. Exact candidate: coalesced generation-fenced publication notification, retaining pacing/backpressure. Risk: wake flooding/audio scheduling. Ceiling is only polling slack, not decode/inference time. Falsifier: publication-to-UI trace proving an existing immediate callback.

**A3 P2, high: freshness predicates are stricter than idle enforcement.** controller.cpp:153 polls every 200 ms; :161-166 removes expired advancing text only then if receipts stop. After display B advances beyond validated A, stop receipts just before the 350 ms wall deadline: cached glyphs can survive until the next watchdog tick, nominally <200 ms extra even with a responsive event loop. Live 600 ms watchdog has the same sampling issue. This is not root 618's renewal bug. Exact candidate: arm earliest outstanding expiry; retain same-frame exception and thresholds. Risk: timer churn/revocation ordering. Falsifier: fake-clock test with no receipts and phase-offset watchdog proving removal at deadline.

**A4 P3, high mechanism / unknown benefit: whole-widget repaint work.** overlay.cpp:222/:241/:284 invalidate all; :870 ignores dirty region; :660-703 repeats widget lookup/protection construction; :765-776 repeats bounded closure and :924-949 traverses all words. Font layout is already cached, so do not claim per-paint font measurement. Exact candidates: cache unchanged plan/chrome dependencies and damage old/new projected plate unions. Risk: missed protection/occlusion damage; ceiling unmeasured. Falsifier: identical-pixel replay plus paint CPU profiling showing negligible cost.

No demonstrated P1. No new source/seek/rotation/zoom reset hole, full-priority inversion, or unbounded app queue found in inspected ranges: generation fences and exact tickets reject stale results; full-needed submission precedes optional regional work (:1322-1348). V and O receive the same device image through separate submissions, but no prepared-probe transfer argument exists (:1115/:1323); quantify reusable GPU preparation with the worker specialist before calling it redundant.

## Instrumentation needed

PresentationReceipt is declared/named but has no production emission found. Add acquisition/decoder-publication, UI dequeue, analysis opportunity/refusal/copy-ready, present attempt/return/status, receipt identity, and last-consumer release. Export separate submission duration/interval and capture-to-submission p50/p95/p99; current 240-sample windows exist (pipeline_orchestrator.cpp:234-280), GPU live presentation sample at app_pipeline_runtime.cpp:1450, but saved GPU has no equivalent there. Actual displayed-frame distributions require display/compositor feedback; photons require external measurement.

At paint/publication record each line's original source/acquisition and latest validation identity, steady validation receipt, current displayed frame/PTS, paint generation and actual drawn count. Aggregate admission tickets cannot express retained-word ages. OverlayPainted occurs only once per content revision and counts assigned words, even if all plan words are suppressed (:908-912). AccessibilityPublished can precede paint and only records new interfaces (:752-775); add first valid positioned-geometry endpoint. Never subtract saved PTS from steady_ns; add explicit timestamp-domain metadata.

All existing events can drop: try-lock contention, 4096 capacity, outside-window attempts; close can lose snapshot/overlap active attempts (uct_timing_trace.cpp:57-105). Missing stages/negative-event claims require a closed lossless window, matching tickets and revisions; publication is not included in the basic worker-chain completeness proof.

## Coverage table

“Read” means returned numbered/source text, not whole-file inference. Greps across src excluded worker internals from conclusions.

| File | Read ranges | Grepped / limitations |
|---|---|---|
| SHARED_BRIEF.md | Entire | Scope/known findings honored |
| AGENTS.md | 621-641, 869-889, 932-966, 983-1014 | Headings; general guide returned in oversized preliminary output |
| docs/ultra_clear_text.md | 1-842 | Initial combined output partly truncated; 1-110 reread |
| docs/uct_timing_trace.md | Entire | — |
| docs/viewport_zoom_and_resume.md | Entire | Verified no diff against 9bdd |
| docs/uct_paused_continuity_repair.md | Entire | Historical pair distinguished from current Standalone |
| docs/uct_screen_text_accessibility.md | Entire | — |
| docs/performance_diagnostics.md | 1-208, with combined-output truncation in middle | UCT/ultra-clear searches; 151-208 reread |
| README.md; docs/developer_guide.md; docs/README.md; docs/hardcoded_paths.md; docs/THIRD_PARTY_LICENSES.md | Preliminary combined consultation, truncated | Not whole-file coverage |
| src/app/ultra_clear_text_controller.cpp | 1-2195 | Entire |
| include/okuflow/app/ultra_clear_text_controller.hpp | 191-211 | Timing/state/API symbol searches elsewhere |
| src/app/app_pipeline_runtime.cpp | 91-135, 1264-1510, 1887-1947, 2481-2580, 2829-3180 | UCT/presentation/acquisition hooks elsewhere |
| src/app/app_video_playback.cpp | 141-205, 401-427, 651-810, 941-1060, 1089-1108, 1239-1310 | Poll/selection/generation hooks elsewhere |
| src/app/app_video_gpu.cpp | 1-446 | Entire |
| src/app/media_gpu_scene.cpp | 459-577, 599-738, 836-920 | UCT/state/presentation hooks elsewhere |
| src/app/app_ultra_clear_text.cpp | 1-99 | Entire |
| src/app/pipeline_orchestrator.cpp | 23-108, 229-282, 331-410 | Timing/timer hooks elsewhere |
| include/okuflow/app/app.hpp; pipeline_orchestrator.hpp | None | Constants only |
| src/common/video_playback.cpp | 31-160, 189-223, 262-278, 605-681, 753-778, 1016-1087, 1181-1328, 1389-1475 | Publication/pacing/source symbols elsewhere |
| src/common/ultra_clear_text.cpp | 2443-2535, 2687-2735, 2769-2825, 3350-3380, 4042-4167 | Worker trace hook locations only: 1985, 2029, 2328, 2591, 2635; no worker audit |
| include/okuflow/common/ultra_clear_text.hpp | 49-115, 511-574, 659-734, 813-837 | Model/API symbols elsewhere |
| src/cuda/uct_analysis_pool.cpp | 1-155, 239-508 | Download implementation 156-238 not read |
| include/okuflow/cuda/uct_analysis_pool.hpp | Entire | — |
| src/cuda/cuda_interop.cpp | 148-238, 5093-5143 | Capture/throttle/pool hooks only |
| src/d3d12/presenter.cpp | 435-452, 508-786, 1940-2012 | Wait/present/readback symbols elsewhere |
| src/ui/ultra_clear_text_overlay.cpp | 182-287, 342-417, 429-459, 642-787, 868-1030 | Method/update/cache hooks elsewhere |
| src/ui/annotation_overlay.cpp | 837-900 | UCT/paint/update search; no UCT paint call found |
| src/ui/uct_screen_text_accessible.cpp | 291-355, 541-874 | Publication/geometry/cadence hooks elsewhere |
| src/common/uct_timing_trace.cpp | 1-113, 202-219 | Drop/summary/serialization/event-name searches elsewhere |
| include/okuflow/common/uct_timing_trace.hpp | 17-61 | Capacity/key/publication search |
| src/capture/media_capture.cpp | 727-747, 1771-1802, 1841-1870 | Delivery/clock symbols elsewhere |
| src/capture/phone_camera.cpp | None | Host timestamp assignment at 723 only |
| src/common/uct_region_planner.cpp; OCR worker implementations | None | Excluded; worker trace-hook grep locations explicitly segregated |