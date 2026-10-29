# UCT end-to-end latency and shadow-tracker audit (Opus, 2026-10-23)

Source: `Z:/Projects/T3-worktrees/oku_flow/t3-f93349e5` at `9bdd5d13`. The worktree
HEAD `a7d4757` changes only a test fixture and docs. `618ee9a` **is an ancestor of
9bdd** (`git merge-base --is-ancestor`), so the Resume/held-probe fix is part of the
audited source. It is not re-reported.

This was source reading only. Nothing was built, tested, inferred, run on a GPU or
run on the desktop. Two read-only Codex specialists (gpt-6.1-sol, high) covered
disjoint scopes (`specialists/specA_app_path.md`, `specialists/specB_shadow_planner.md`;
task IDs in `TASK_LEDGER.md`). I verified every finding promoted below against
source myself (section 6). Routed public-prep issues (candidate deadline, FindPlan,
directory pin) are excluded. Worker-internal GPU items from the earlier Opus report
(E1–E5, A1–A5) are not repeated.

## 1. Findings

No P1 was established. All items are source counterexamples; none has been run.

**P2-1. Idle text expiry is sampled by a 200 ms watchdog, not timed to the deadline**
(confidence: high on the mechanism).

- **Mechanism:** `IsReadable` allows `nowMs - validatedAtMs <= 350` or the exact
  validated frame (`ultra_clear_text.cpp:4138-4141`). With presentations flowing,
  `OnPresented → RefreshOverlay` enforces this on every present
  (`ultra_clear_text_controller.cpp:890`).
- **When receipts stop:** a stalled or slow display, a source switch or a placeholder
  leaves only the watchdog. It runs every 200 ms (`:152-167`) and handles both
  advancing-wall expiry and live `PresentationAvailable`.
- **Counterexample:** advancing display B moves past validated frame A, then receipts
  stop about 1 ms before A's 350 ms wall bound. Glyphs, reading text and positioned
  accessibility (`ScreenTextSnapshot` follows `overlayWords_`, `:2058-2066`) can
  survive up to about 200 ms past the bound, plus event-loop delay.
- **Why it matters:** this is a stale-ghost window on the low-vision safety surface,
  not a throughput issue.
- **Exact fix:** a single-shot timer armed at the earliest outstanding expiry
  (validation wall bound or presentation-availability bound). Keep the 200 ms
  heartbeat as a backstop. Thresholds and the same-frame exception stay unchanged.
- **Falsifier:** fake-clock controller test with no receipts and the watchdog phase
  offset by 199 ms. Assert overlay, pane and screen text are revoked at the deadline.

**P2-2. A worker completion never re-triggers admission, so the OCR worker idles until
the next probe cycle** (high).

- **Submission rule:** OCR is submitted only from `HandleProbeOutcome`
  (`:1291-1345`). On the GPU device path `canSubmit` needs `!scheduler_->HasWork()`
  (`:1318-1321`), so leases are never parked.
- **Refused candidates are lost:** a probe that arrives while OCR is busy has its
  candidate exchanged out and released (`:1291`). `HandleResult` ends with
  refresh/status only (`:1508-1514`) and schedules nothing.
- **Live camera:** the next OCR needs, in order:
  - the next present after the 80 ms gate (`:902`);
  - a capture that passes the separate 80 ms capture throttle
    (`cuda_interop.cpp:5112`);
  - V (`PrepareResidentAnalysis`);
  - a queued UI hop (`:225-260`);
  - P (`CheckLines`, ≤60 ms budget);
  - a second queued UI hop (`:205-217`);
  - a stable probe (similarity ≥ 0.92).
- **Advancing media** is worse: `:660` refuses to re-probe the same displayed frame.
  A distinct frame B refused while job A ran cannot be admitted after A completes,
  so OCR waits for frame C. Low-frame-rate or long-duration samples make that wait
  long. (Specialist A1, verified.)
- **New live text pays this twice.** New words need a second full read from a later
  acquisition (`ultra_clear_text.cpp:3599-3627`).
- **Ceiling:** unknown, but bounded per OCR cycle by roughly one probe-gate residual
  plus the V→U→P→U chain. It is not additive with OCR time. See idea I1 and
  falsifier M2.

**P3-1. Probe-check ordering can starve visible lines** (specialist B's P2, downgraded;
medium on mechanism, low on reachability).

- **Mechanism:** `LineReferences` sorts invalid lines first, then the oldest
  (`:4024-4037`). Lines that are invalid and not visible keep their `validatedAtMs`
  (`:4080-4086`), so they stay first. `CheckLines` restarts at offset 0 every probe.
  The budget is checked only between 64-line resident batches or between CPU lines
  (`:1514-1565`).
- **Reachability:** the GPU path always checks at least 64 lines per probe.
  Starvation needs more than 64 leading invisible lines plus budget exhaustion, for
  example a deep zoom into a dense page.
- **Symptom:** visible lines go unchecked and hide at 350 ms, which the user sees as
  flicker or loss. This is not a false accept.
- **Fix:** I6.

**P3-2. Untaken live analysis captures hold a pool slot, and the two 80 ms throttles
can drift out of phase** (medium).

- **Capture runs unconditionally:** `ProcessFrame` captures whenever
  `now - uctLastCaptureMs_ >= 80` (`cuda_interop.cpp:5112-5123`).
- **It is only taken when wanted:** the live path takes it only if
  `OnPresented(...) && presented` (`app_pipeline_runtime.cpp:1366,1496-1501`).
- **Refusals that strand a capture:** `OnPresented` refuses while a candidate or
  probe is pending, or within 80 ms of `lastProbeRequestMs_` (`:892-902`).
- **What the stranded capture does:**
  - It stays in `uctAnalysisFrame_` (`cuda_interop.hpp:334`) and occupies one of the
    two slots.
  - It advances the capture clock but not the probe clock.
  - With OCR holding the other slot, the next capture fails as busy. The stale frame
    is later taken and dropped on a `frameSequence` mismatch (`:1499`).
- **Contrast with media:** the media path explicitly releases orphans
  (`media_gpu_scene.cpp:638`). The live path does not.
- **Effect:** up to one extra capture period (about 80 ms) plus a frame of probe
  delay in phase-slip cases. Frequency is unknown.
- **Falsifier:** count captures vs takes vs frame-mismatch drops (M3). Zero
  mismatches and no capture/take phase gap means no benefit.

**P3-3. Unsupported or stale regional plans run a full call priced with the regional
estimate** (B, verified, high).

- **Mechanism:** the worker checks `slack > estimatedMs + 80` with an estimate of at
  least 750 ms. An invalid plan with enough slack and no queued full then runs a full
  `Recognize` (`ultra_clear_text.cpp:2091-2138`).
- **Effect:** the call is not preempted, so a full arriving later waits behind it
  (overrun is recorded afterwards, `:2302`). This qualifies the documented
  reservation. It is not a correctness failure. Opt-in mode only.

**P3-4. Shadow coverage is hypothetical, not actually serviced** (B, interpretation).

- `Observe` marks every selected item as serviced (`uct_region_planner.cpp:885-939`).
  The executor runs only the selection nearest the focus (`:344-352`). Do not report
  shadow debt as regional service.

**Design note D1 (not a defect).** Regional OCR can never shorten time to new text in
9bdd. Pending words, invalidation, lack of coverage or due refresh all force `needed`
(`controller:1309-1312`). Regional reads run only when `!needed` and idle (`:1347`),
and only when slack > 750 + 80 ms, meaning within about 670 ms of the last full
(`:1354`, `hpp:604`). It refreshes stable text only. The doc's stated benefit
("focused current text arriving sooner") is not reachable for changed text. That is
deliberate and conservative. I10 is the policy alternative.

**Instrumentation gap G1.** `Event::PresentationReceipt` has a name
(`uct_timing_trace.cpp:30`) but no production emitter. Present-to-paint and
frame-age claims therefore have no anchor.

**Small item.** Saved-video snapshots are discovered by a 16 ms UI poll
(`app_video_playback.cpp:194-196`). This adds 0–16 ms to every saved-video frame
present, UCT or not (A2).

## 2. Latency map

Thread key: C = capture, U = UI, V = device-validation scheduler, P = ProbeWorker,
O = OCR worker.

Bracketed waits overlap other work and must not be summed. The worker stopwatch
(`durationMs`) covers dequeue to before the planner. `Done` follows the planner.

**Live camera (CUDA device path):**

1. C: MF sample. Host QPC arrival is recorded separately (`media_capture.cpp:727-739,
   1781-1788`).
2. Queued U wake, then orchestrator dispatch.
3. U: CUDA processing.
4. Analysis capture [80 ms steady throttle, two-slot pool, refusal if busy].
5. Present submission (`Present(1,0)` return = submission, not scanout).
6. `OnPresented` [80 ms gate; refuses while a candidate/probe is pending].
7. `OnDeviceScene` (lease age ≤ 1000 ms).
8. V: resident probe preparation.
9. Queued U: build `LineReferences` on U.
10. P: `CheckLines` (≤60 ms).
11. Queued U: `ApplyChecks`, stability.
12. Submit O, only if O is idle.
13. O: hold provenance → recognize (per the earlier report) → resident analysis →
    [planner if flagged] → callback.
14. Queued U: `HandleResult` → `Admit` (proof against `latestProbe_`).
15. `RefreshOverlay` assigns words → Qt `update()` → software paint
    (`OverlayPainted`).
16. Accessibility: removals immediate, additions on a 1000 ms tick / ≤3000 ms hold
    (`uct_screen_text_accessible.cpp:843-860`).
17. The lease is freed when the last shared owner drops it. No release event exists.

**Time to first glyph for new live text:**

T ≈ [probe residual + unstable→stable probe] + W₁ + [probe cycle] + W₂ + U
admission + paint request + paint.

- The first probe after a scene change is unstable by definition (`:1284-1288`).
  That costs at least one extra probe period. This is policy and is kept.
- Positioned accessibility may add up to the 1000 ms addition cadence.
- Worker gains ΔW count about twice here (two reads). Probe-chain gains count up to
  three times.

**Advancing saved video:**

- Decode publishes to D.
- U discovers it via the 16 ms poll.
- GPU process → present receipt → owned analysis or a truthful no-analysis receipt.
  PTS/10⁴ becomes stamp milliseconds. The PTS domain is never mixed with steady time.
- Same V→P chain, once per distinct frame, with an 80 ms gap.
- O admission additionally needs no outstanding job and ≥ 350 ms since the last
  submission (`:1318-1320`).
- Model time = displayed PTS, plus an independent 350 ms wall bound on validation
  freshness (`:698-731`).

**Paused (Freeze → Resume):**

- Freeze selects the presented lease. Standalone admission is used, so the neighbour
  does not gate (A).
- One submission when the backend is ready, with retries at 100/500 ms. Time to text
  ≈ backend readiness + W + U + paint.
- Detail: settle 300 ms, ≤ 32 regions.
- Resume keeps exact-source accepted lines and clears pending lines (`:2769-2783`).
  Only a distinct advancing acquisition probes next. The native seek on Resume resets
  temporal history (`media_gpu_scene.cpp:536-539`). That is separate from retained
  references.

**Clock domains:**

| Clock | Used for |
|---|---|
| Steady ms | Probe/refresh gates, lease age, live `SourceStamp` ms, regional deadlines (`steady_clock`) |
| Media PTS (100 ns → ms) | Advancing model time |
| Integer 100 ns | Paused neighbour policy |
| Trace `steadyNs` | Trace records; carries `sourceTimestampMs` unconverted |

Never subtract PTS from steady time.

## 3. Shadow planner / tracker checklist

Mostly from specialist B; spot-verified scheduler lines 1952-2372 myself.

| Area | Status |
|---|---|
| Occurrence identity | Geometry-first greedy pairing, ≤ 8 overlaps. IDs are never reused and minting stops at exhaustion. Caps are clamped. |
| Epoch / authority resets | Precede observation. Plan execution requires identical mapping, processing epoch, smoothing, and a later acquisition and frame (`planner:311-329,451-495`). |
| Full-baseline priority | A region never replaces a queued full; a full replaces a queued region and clears the plan (`:1963-1983`). The worker rechecks before inference (`:2091-2113`). No preemption once a native call starts (P3-3). |
| Held identity | Resolved from the owned device record after the copy. Held copies never age the planner or confirm pending words (`:3599-3614`). Regional runs exclude held inputs. |
| Ordering | One worker, serial callbacks. Consumer fences use exact job IDs (`regionalJob_`, `advancingMediaJob_`, `lastOcrJob_`) plus scheduler generation. No out-of-order admission path found. |
| Wakeups | Both workers' predicates depend only on pending/shutdown, with a notify on every insertion. No missed worker wakeup. The missed *admission* wakeup is P2-2. |
| Memory | Planner, regional unions (8192) and device preparation (1024 lines / 8192 words) are capped. Full admission has no local pending cap; it is bounded by producer output. Unknown-completion quarantine is deliberately unbounded-lifetime. |
| Thresholds | Confidence ≥ 0.7; temporal current-input NCC 0.94 / visibility 0.95 / zero search; agreement IoU 0.5; freshness 350 ms; live result age 4000 ms. None should be relaxed. |
| Stale ghosts | P2-1 is the only window found. |

## 4. Ranked optimization ideas

E = exact (same pixels, proof, thresholds, coverage), A = approximate or policy.
"Ceiling" means mechanism-bounded, not measured.

| # | Idea | Path | Ceiling | Where | Risk | Smallest falsifier |
|---|---|---|---|---|---|---|
| I1 (E) | **Completion-triggered re-admission.** If demand remains when `HandleResult` finishes, allow exactly one immediate capture/probe at the next present (waive the 80 ms residual once, in both throttles). For advancing media, allow one re-probe of the current distinct frame whose candidate was refused only because O was busy (a new acquisition relative to the completed job; never counted twice). | live + advancing; affects W gaps ×2 for new text | ≤ one probe residual (≤80 ms) + one stability probe per OCR cycle; unknown share | controller `HandleResult` tail, `OnPresented :902`, `SubmitAdvancingMediaProbe :660`; capture throttle `cuda_interop:5112` | extra probes (bounded to 1 per completion); must not mark a held repeat as new | trace `Done(O,i)→Submit(O,i+1)` with a demand flag; gap ≈ V/P processing only ⇒ small ceiling |
| I2 (E, correctness) | Deadline-armed expiry timer (P2-1) | all, when receipts stop | removes ≤ 200 ms stale window | controller watchdog `:152-167` | timer churn | fake-clock test above |
| I3 (E) | Gate live analysis capture on the controller's pre-present want, or release an untaken capture and roll back `uctLastCaptureMs_` (mirror `media_gpu_scene:638`). One throttle owner. | live | ≤ 80 ms + a frame in phase-slip cases; frees the slot sooner | `app_pipeline_runtime:1366/1496`, `cuda_interop:5112` | the preflight must use the same refusal predicates | M3 counters |
| I4 (E) | Remove the V→U→P hop. Run `CheckLines` on V right after `PrepareResidentAnalysis`, using an immutable line-reference snapshot and `latestProbe_` published by U (COW). Similarity stays as is. | live + advancing, every probe | one queued UI hop + one thread handoff per probe; grows with UI paint load | controller `:225-260`, ProbeWorker | snapshot timing must match today's "references at UI receipt" semantics; keep the lines-admitted-after-probe guard (`ApplyChecks :4058`) | trace V `Done` → P `Submit` gap under paint load |
| I5 (E) | `RefreshOverlay` rebuilds and compares all `SceneWords` on every present (`:890,2021-2041`). Skip it unless the model revision changed, the frame-seq exception changed, or now ≥ the next line expiry. | U thread occupancy, which delays queued callbacks (P2-2 chain) | unknown; O(words) per present at 60–120 Hz | controller `RefreshOverlay`, model revision counter | expiry must be monotone in time (it is: `IsReadable`) | per-present `RefreshOverlay` duration histogram |
| I6 (E) | Exclude lines whose projected reference is wholly outside the probe frame, using the identical visibility predicate. Add a fair continuation cursor for budget exhaustion; unchecked still means unchecked. | probe chain, deep zoom | less P work; fixes P3-1 | `LineReferences`, `CheckLines` | predicate drift ⇒ require identical result on a replay | scripted oracle with >64 invisible lines plus a visible sentinel |
| I7 (E) | Deliver the result callback before `Observe` and publish the plan separately (regional/shadow flags only) | opt-in modes | equals `shadowPlannerMs` (field exists, not traced) | `ultra_clear_text.cpp:2335-2364` | plan must still bind exact job id (it does via `lastOcrJob_`) | trace `shadowPlannerMs`; ≈0 ⇒ drop |
| I8 (E) | Wake the UI on playback-snapshot publication (coalesced, generation-fenced) | all saved video, not only UCT | ≤ 16 ms + jitter | `video_playback.cpp:1307`, `app_video_playback:194` | wake flood | publication→UI-dequeue trace |
| I9 (E) | Overlay damage region / plan cache (A4) | paint | unknown | `ultra_clear_text_overlay.cpp:222-284,642-787,868-949` | missed damage | identical-pixel replay + paint profile |
| I10 (A, owner policy) | **Regional confirmation read.** When the only demand is pending words lying wholly inside one plannable region, run a regional read instead of a full read for the second observation. Proof stays the same (independent acquisition, agreement, current proof); the full refresh still runs at 1500 ms. Needs a pending-word-driven focus, not nearest-focus selection. | new live / advancing text | historical single sample: regional worker 49.5 ms vs first full 458 ms (`uct_active_regional_ocr.md:352`), not warm-comparable | controller `:1309-1347`, `RegionalDetailFocus` | changes documented policy; regional detector scale changes tokenization ⇒ may fail agreement more | quality A/B on independent truth plus time-to-first-glyph; no earlier admissions ⇒ reject |

Not new or rejected here:

- B's same-image resident-setup sharing (E, ceiling unknown; trace multiplicity first).
- B's planner scratch reuse (low ceiling).
- Polling-loop tuning for fences (measure observed-ready delay first).
- No capacity, threshold or freshness changes.
- No third pool slot.

## 5. Minimum measurement plan

Proposed patch seams only; none implemented or run.

- **M1. Present anchor.** Emit the existing `PresentationReceipt` at present return
  (`app_pipeline_runtime:1442-1470`, `app_video_gpu:312-335`) with frameSeq and
  source epoch. For live input, also record the QPC host-arrival → steady mapping
  (already computed for `MeasureCaptureToPresentLatency`). This is software
  submission only. Scanout needs DXGI frame statistics or an external camera, which
  is out of scope.
- **M2. Admission gap.** Record a numeric controller event `AdmissionRefused{reason}`
  for `OnPresented`/`HandleProbeOutcome` refusal reasons (gate, candidate pending,
  O busy, unstable, not needed). Add a `DemandOutstanding` flag on O `Done`. This
  yields the Done→Submit gap split into throttle, probe chain and stability.
- **M3. Capture accounting.** In-process atomic counters for analysis captures, takes,
  mismatch drops, pool-busy refusals and orphan lifetime.
- **M4. Age at paint and at accessibility.** On `OverlayPainted` and on the first
  positioned-geometry answer after a revision, record per-revision min, median and
  max of:
  - (a) steady now − steady validation receipt;
  - (b) steady now − first-observation arrival of the newest admitted word.

  Use steady-domain fields; for media, carry PTS separately and never difference it
  with steady time. Aggregate tickets cannot express retained-word ages, so this
  needs per-line steady receipt fields.
- **M5. Lossless percentiles.** The 4096-record try-lock trace saturates in roughly
  tens of seconds at present rate, and drops on contention. Missing events are not
  negative proof. For p50/p95/p99, add fixed-bucket atomic histograms for:
  - present interval;
  - capture→present;
  - Done→Submit gap;
  - V→P→U chain;
  - `RefreshOverlay` time;
  - UiReceipt→OverlayPainted;
  - age-at-paint (M4).

  Histograms are lossless counts. Keep the event trace for short causal windows only,
  and report its drop counters.
- **Experiment.** On the three FP32 arms (A baseline, B merge-only, C merge+batch;
  all keep two-slot/integral, same engines, inputs and binary hashes), add one
  deterministic advancing-media clip with known text-change PTS. Measure change-frame
  present → first `OverlayPainted` with all words of the new line, and → first
  positioned accessibility. Report W, the M2 gap and age-at-paint percentiles per
  arm.
  - No exact confidence or geometry tolerance is assumed.
  - FP32 results do not bound BF16; BF16 confirmation comes only after an actual
    private factory receipt.
  - Live-camera runs need a free desktop and an owner grant, and stay separate.

**Unknowns:** photon latency; DWM composition; screen-reader receipt or speech; real
frequency of P3-1/P3-2; UI-thread paint load; actual share of P2-2 gaps. I1 and I3–I5
claims depend on M2/M3.

## 6. Evidence and coverage

| Source | My own read (Opus) | Specialist |
|---|---|---|
| AGENTS.md | full (1-1081) | A/B: UCT sections |
| `src/common/ultra_clear_text.cpp` | 1874-2403, 2542-2711, 3590-3669, 1505-1566, 4024-4142; outline grep | B: 1-133, 900-959, 1299-1430, 1510-2888, 3350-4192 |
| `src/app/ultra_clear_text_controller.cpp` | 148-175, 205-334, 640-919, 1084-1116, 1230-1479, 1495-1516, 1929-2108; constant greps | A: full |
| `include/.../ultra_clear_text_controller.hpp` | grep (timing, 604) | A: 191-211 |
| `include/.../ultra_clear_text.hpp` | 825-880 | A/B: most |
| `src/cuda/cuda_interop.cpp` / `.hpp` | 193-230, 5100-5130; hpp:334 | A: 148-238, 5093-5143 |
| `src/app/app_pipeline_runtime.cpp` | 1355-1372, 1490-1508 | A: listed ranges |
| `src/cuda/uct_analysis_pool.cpp` | 299-345 | A/B: full |
| `src/app/app_video_playback.cpp` | 190-198, 1252-1262 | A |
| `src/common/uct_region_planner.cpp/.hpp`, `uct_device_analysis.hpp`, validation kernels | not read by me | B: full / partial |
| overlay, accessibility, presenter, media_gpu_scene, video_playback, capture | not read by me | A: ranges in its table |
| docs | `uct_active_regional_ocr.md` 1-60; `uct_timing_trace.md` 1-60; prior Opus report §4, §5 headings, §9-12 | A/B: listed |

Not covered: `uct_tensorrt.cpp` and recognizer internals (covered by the earlier
report); the color/ink kernel bodies; `phone_camera.cpp`; Android. The required
general docs were consulted only partially by the specialists and not by me in full.
No whole-codebase claim is made.
