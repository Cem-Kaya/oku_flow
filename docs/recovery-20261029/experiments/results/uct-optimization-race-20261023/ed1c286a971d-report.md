# Resident UCT validation integral

The resident GPU worker builds a decimated luma summed-area table before
reference capture and matching. `resident::Prepare` uses `gpu::ValidationIntegral`
for independent current-frame probes and recognition preparation, including the
normal local mixed-BF16 path. Smoothed inputs may additionally prepare their own
recognition integral. This stage uses no neural network and changes no inference
precision, model, engine, OCR cadence or source identity.

The row pass now uses one complete 256-thread block per row. Each lane computes
the original sampled luma average, then an unsigned warp prefix scan and eight
warp carries produce the exact serial row sums. Tiles after the first carry the
previous total; out-of-range lanes contribute zero and still participate in
every warp operation/barrier. A barrier protects shared offsets before reuse.
The maximum shared scratch is nine unsigned integers (36 bytes) per block.
No new global allocation, transfer, buffer, stream or host/device-wide wait is added.
The existing column pass, zero borders and two-launch API are unchanged.

All samples, integer luma weights/division and downstream NCC/visibility,
freshness, geometry, source protection and admission policy remain unchanged.
This is an exact execution-layout candidate; the separately preserved CPU
sampling experiment does not participate in its comparison.

## Evidence protocol and limits

`gpuAlgorithmsAndOwnedInputs` in the tracked `uct_paddle_cuda` group compares
every table entry to an independent integer CPU oracle. It covers decimations
1/2/3/4/9; widths below a warp, 31/32/33, 127/128/129, 255/256/257,
511/512/513 and 959/960; padded pitch, floor-divided source tails and repeated
poisoned output buffers. Source dimensions stay within 8192. Existing resident
reference/matching, paired-input, ownership and geometry fixtures remain intact.

The race's private source-linked probe separately compiles exact 8f02 serial
functions and the current production CUDA unit with identical flags. Its
sm89-only measurements are separate from the full supported-architecture build.
It adds menu_7000/menu_7033 at decimations 2/3/4/9, dense and flat-blue
960x960-probe controls, and exact production `References`/`MatchReferences`
sample/decision/geometry comparisons. Table readback is diagnostic only.

Five warmups and 31 alternating ABBA/BAAB blocks retain 62 raw samples per
variant/workload/completion mode. CUDA events enclose both integral launches;
host timing covers submission through either event completion or the actual
`resident::Finish` helper. That helper's polling/yield/private-timer behavior
can mask a kernel gain, so its host wall time is reported separately. Allocation,
one-time H2D fixture upload, full-table validation and hashes are outside timing.
Every timed execution still computes the complete table and is compared in full
after completion. No output result is reused to fake execution.

This measures resident-input kernels after an explicit untimed fixture upload,
not cached FP32/BF16 inference, capture-to-glyph latency, full preparation,
live presentation FPS or OCR updates per second. The table stage is on the
resident preparation critical path, but its fraction of total recognition and
publication work remains unmeasured. Native UIA, real camera and full release
acceptance remain separate. Exact private commands, raw results, first failures
and source/input/executable hashes live under
`local-work-data/uct-optimization-race-20261023/sol`.

## Sol race result, 2026-10-23

The first shared-desktop probe completed successfully on RTX 4090, driver
617.42, CUDA 12.9.86 and MSVC 19.44.35229. The owner was using the desktop and
watching videos. This was **not an idle-GPU benchmark**. The retained first
attempt refused ordinary WDDM context rows before configuration; the parent
classified those rows as permitted background load and the corrected second
attempt ran once. No application or background process was stopped.

All 98 correctness cases passed full-table comparisons with the current 8f02
GPU implementation and the independent CPU oracle: 3,128,678 table entries per
variant, repeated three times from poisoned buffers. There were 880 downstream
reference requests: 759 valid and 121 refused. Reference sample bytes/valid
flags and all actual match output fields agreed exactly. All 2,480 timed table
outputs also agreed in full. No approximation or quality tolerance was used.
There is no OCR word-count/accuracy claim from these kernel-only workloads.

Selected results below use the **actual production `resident::Finish` wait**.
Each cell is p50 / p95 milliseconds, baseline to candidate. Complete event-only
and production-wait results for all 20 rows remain in `summary-table.csv` and
`raw-integral-2.json`.

| Source / decimation | Two-launch GPU time | Host submit through Finish |
| --- | --- | --- |
| menu_7000 / 2 | 0.6020 / 0.8935 → 0.0698 / 0.1389 | 1.0015 / 1.2621 → 0.3514 / 0.7584 |
| menu_7033 / 2 | 0.5700 / 0.8826 → 0.0665 / 0.1286 | 0.8865 / 1.3186 → 0.3532 / 0.6842 |
| menu_7000 / 3 | 0.7432 / 1.1855 → 0.0527 / 0.1152 | 1.0671 / 1.4331 → 0.4261 / 0.6588 |
| menu_7033 / 3 | 0.7540 / 1.2578 → 0.0528 / 0.1138 | 1.0650 / 1.4591 → 0.5499 / 0.9080 |
| menu_7000 / 4 | 0.3011 / 0.3652 → 0.0460 / 0.1311 | 0.4117 / 0.9313 → 0.4160 / 0.6023 |
| menu_7033 / 4 | 0.3014 / 0.4497 → 0.0461 / 0.1084 | 0.5412 / 0.7731 → 0.4546 / 0.5804 |
| menu_7000 / 9 | 0.2309 / 0.2954 → 0.0340 / 0.1797 | 0.4704 / 0.5759 → 0.4702 / 0.6277 |
| menu_7033 / 9 | 0.2309 / 0.3008 → 0.0346 / 0.1202 | 0.4718 / 0.6237 → 0.4730 / 0.5834 |
| Dense 960x960 probe / 2 | 0.9513 / 1.4680 → 0.1515 / 0.2229 | 1.3028 / 1.7183 → 0.4888 / 1.2543 |
| Flat blue 960x960 probe / 2 | 0.9117 / 1.4279 → 0.1448 / 0.2259 | 1.2800 / 1.9237 → 0.5109 / 1.3485 |

At decimation two, the measured menu GPU interval fell by 0.504–0.532 ms
(88.3–88.4%); host median fell by 0.533–0.650 ms (60.2–64.9%). Every one of
the 31 paired blocks improved both GPU and host time for each menu at
decimations two and three. The d4/d9 host results are inconclusive as a useful
latency gain: only 9–19 of 31 blocks improved, and d9 includes a p95 regression.
These are fractions of this integral stage/wait, **not total OCR worker time**.
Its total-pipeline fraction is unmeasured; a sub-millisecond stage saving does
not establish the owner's requested large end-to-end acceleration.

The GPU monitor recorded a transition from P8/495 MHz into P2/2520–2670 MHz
around the probe. Before/after observations and one-second monitoring retain
utilization, memory, power, clocks and process inventories. State is not
correlated to each individual sample; ABBA cannot remove all shared-desktop
effects. The paired d2/d3 result supports a promising stage candidate, with a
controlled repeat and full-worker trace still needed for race-wide ranking.
No 128-thread comparison was run; 256 is the sole tested setting.

The own-cache CUDA fixture target also compiled successfully for all ten
documented architectures. The tracked PCH-OFF gate reported **SCOPED PASS
3/245**, zero skipped or unresolved: `uct_paddle_processing` (43 Qt rows),
`uct_paddle_cuda` (52 Qt rows), and `shared_source_equivalence`. The CPU
sampling companion's exact regression passed in that processing group; its
withdrawn timing benchmark was not run. PCH ON, the complete shipping/matrix,
native GUI/UIA/NVDA, real camera and inference-to-publication timing remain
unrun. Compute was released at 22:16:19 UTC, before the 22:25:17 deadline,
with compiler/probe/test/monitor processes gone and the tracked runner lock free.

## Ranked follow-ups from the source audit

1. **Cooperative integral rows (implemented and measured here).**
   `uct_validation_kernels.cuh::integralRows` and
   `uct_device_analysis.hpp::Prepare` put the exact candidate on both current
   validation and recognition-preparation paths. It is small and reviewable;
   its measured absolute saving is bounded and full-worker impact is unknown.
2. **Recognition submission/host gaps.** `uct_tensorrt.cpp` waits for compact
   detector count metadata and again at group completion. Reducing host launch
   gaps or grouping work could matter more to total inference time. The current
   path already transfers compact tokens rather than full logits; this is not
   a proposal to remove required completion/ownership fences. Other race
   contestants own this area; no independent speed claim is made here.
3. **Cooperative reference extraction.** `referenceBody` samples and normalizes
   up to a bounded grid serially, and `References` launches one thread per
   request. Shared sampling with carefully controlled reductions is a next
   exact/explicit-approximation experiment. Validate sample, deviation/validity
   and downstream match changes before promoting it.
4. **Resident integral/reference allocation reuse.** `resident::Allocate` and
   `CaptureReferences` allocate new owned results; retirement already uses an
   asynchronous memory pool. A bounded leased slab could reduce remaining host
   costs, but gains are unmeasured and it must never overwrite a retained
   reference or change unknown-completion quarantine.
5. **Projection/protection work during publication.**
   `UltraClearTextOverlay::ComputePaintPlan` rebuilds projected protection
   regions on every paint while glyph shaping/colors are already cached.
   Exact-key reuse could help presentation CPU time, provided transform,
   chrome, protection closure and last-painted accessibility authority remain
   correct. It needs actual pan/paint timing and positioned-text regressions.

The separately committed CPU bilinear-addressing candidate is secondary. Its
tensor equality passed; its performance is unmeasured and it is not a GPU race
win. The next smallest primary step is one combined recognition/probe timing
trace using the unchanged cached production BF16 selection, so the measured
integral saving can be placed on the real worker-to-publication critical path.
