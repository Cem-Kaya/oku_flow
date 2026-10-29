# UCT GPU latency and throughput investigation, 2026-10-23

Research baseline: `8f02fd11ed5ace89119b51d5cc2fb0576ea27e6d` in the isolated
`Z:/Projects/T3-worktrees/oku_flow/t3-33a5151f` worktree. This is a private
development experiment, not a shipping setting, release gate or camera/FPS claim.
Opus authored and measured recognition-group look-ahead in the first block.
After its rate-limit interruption, the owner switched this same worktree to Sol,
which hardened graph identity/error handling and ran the second block.

## Decision

The useful measured lead is dense N8 recognition look-ahead: worker median
299.51 to 252.92 ms, a 46.59 ms / 15.6% reduction in this corpus. Sparse text
shows no benefit, and N1 look-ahead is inconclusive. Same-executable graph
replay helps launch-bound N1, but yields no useful N8 gain and regresses one
menu input. Neither experiment establishes app FPS or first-sharp-text latency.

Prioritize removing gaps in the real resident recognition path and reducing
recognition work, rather than further optimizing the already integrated surface
geometry kernel. Keep graph replay default off: the N8 comparison shows no useful gain and one menu regression. The
normal owner's local BF16 production configuration still needs its own matched
measurement: these experiments deliberately use cached FP32 diagnostics.

## Scope and evidence

Machine: Ryzen 9 5900X / RTX 4090, Windows 11, Qt 6.12.0, VS2022, CUDA 12.9.2,
TensorRT 11.4.0.106; driver, UUID, executable, model, engine and input hashes are
in each retained diagnostic manifest. The owner continued ordinary video and
desktop use. All timings are **shared-desktop measurements**; GPU video decode,
composition, clocks, scheduling and memory contention can affect them. Serial
agent compute and the tracked exclusive runner lease do not make the GPU idle.

Evidence root (private, not bundled or committed):
`Z:/Projects/OkuFlow/local-work-data/uct-throughput-opus-20261023`.
`runs/` contains original job JSON, comparisons, cache inventories, manifests and
stdout/stderr. `gpu/` retains before/after state and a graph mid-run sample.
`runs.json`, `runs-dense.json`, `runs-graphs.json` bind commands/executables to runs.
`analysis.json` and `analysis-graphs.json` retain raw timings alongside statistics.
`block.log` retains original failures and lease events. Analysis is offline Python
standard library only; the benchmark calls real inference every time.

The production `RecognitionScheduler::SubmitDevice` path is exercised using
owned resident inputs. Source upload and the analysis device copy finish before
submission and are separately recorded as setup. Worker time covers TensorRT,
resident reference/color/ink preparation, line preparation and callback work.
The diagnostic requires successful GPU inference, requested capacity, resident
references, exact owned input, no image egress, unchanged cache and complete
TensorRT State retirement. It does not exercise controller admission, source-to-
capture delay, live validation scheduling, Qt painting, UIA or presentation.

The original look-ahead block used `menu_7000` (1280x720) and `frame-55` (1920x1080).
The graph continuation additionally used `menu_7033` (1280x720). Both use pinned
medium/medium, fixed1024 recognition, capacities 1 and 8. The private production
BF16 manifest, regional execution and shadow planner were cleared only in the
child benchmark environment; saved app settings and persistent variables were
untouched. Inference-only SDK loading and the plan precheck forbid cold builds.
No engines/models/dependencies were prepared or downloaded.

## Measurement interpretation

Each block warms once before timed passes. Analysis additionally excludes timed
passes 1-4 from each block, retains them in raw evidence, and uses nearest-rank
quantiles. Unprofiled ABBA runs establish end-to-end worker comparisons; profiled
pairs attribute CUDA event intervals and host work separately. Small sample
profile pairs are attribution, not causal timing proof. Per-run distributions
must accompany pooled summaries; serial samples are not independent trials.

CUDA stage intervals include time between their stream events, including launch
gaps and contention. `networkHostMs` overlaps GPU execution. Host waits overlap
the stream work they await. Do not add these columns together. A reduction in
event intervals can result from better feeding of unchanged kernels; it is not
necessarily reduced arithmetic. Neither `1000 / kernel_ms` nor `1000 / worker_ms`
is measured display FPS or admitted OCR updates/sec.

## Measured worker comparisons

Unprofiled steady values, milliseconds. `n` is samples **per variant** after the extra four warmups per block. p95 is reported only for n >= 20. Negative change is faster.

| Experiment | Source / capacity | n | Baseline/off p50 / p95 | Candidate/on p50 / p95 | p50 change |
| --- | --- | ---: | ---: | ---: | ---: |
| Opus look-ahead | menu_7000 / N1 | 48 | 32.89 / 40.64 | 32.79 / 41.41 | -0.10 ms (-0.3%) |
| Opus look-ahead | menu_7000 / N8 | 48 | 23.23 / 26.17 | 23.78 / 27.55 | +0.55 ms (+2.4%) |
| Opus look-ahead | frame-55 / N1 | 24 | 483.05 / 685.24 | 534.78 / 695.80 | +51.72 ms (+10.7%) |
| Opus look-ahead | frame-55 / N8 | 24 | 299.51 / 333.18 | 252.92 / 274.66 | -46.59 ms (-15.6%) |
| Sol graphs, look-ahead constant | menu_7000 / N1 | 24 | 33.21 / 36.84 | 27.81 / 29.35 | -5.40 ms (-16.3%) |
| Sol graphs, look-ahead constant | menu_7000 / N8 | 24 | 25.11 / 27.74 | 25.08 / 26.74 | -0.03 ms (-0.1%) |
| Sol graphs, look-ahead constant | menu_7033 / N1 | 24 | 31.18 / 40.04 | 26.17 / 28.19 | -5.00 ms (-16.0%) |
| Sol graphs, look-ahead constant | menu_7033 / N8 | 24 | 24.33 / 26.68 | 26.99 / 29.68 | +2.66 ms (+10.9%) |
| Sol graphs, look-ahead constant | frame-55 / N1 | 24 | 512.93 / 625.73 | 337.46 / 364.18 | -175.47 ms (-34.2%) |
| Sol graphs, look-ahead constant | frame-55 / N8 | 24 | 258.98 / 289.21 | 261.70 / 274.31 | +2.72 ms (+1.1%) |

The dense N8 look-ahead reduction is 46.59 ms, 15.6% of original worker service. It removes overlap gaps rather than 46.59 ms of GPU kernel arithmetic. The sparse menu did not trigger look-ahead: 10 crops split 8+2. Dense N8 graph medians do not establish a gain, while menu_7033 regresses by 2.66 ms (10.9%). N1 graph benefits are diagnostic launch-bound results, not the owner's default N8 production result.

### ABBA run distributions

Order is A1, B2, B3, A4. Each cell lists steady p50 [min, max], in ms. Full raw order is retained in the analysis JSON. Each dense/graph run has only 12 steady samples: per-run p95 is deliberately withheld. Original sparse look-ahead runs have 24.

| Experiment / workload | A1 | B2 | B3 | A4 |
| --- | ---: | ---: | ---: | ---: |
| Look-ahead menu_7000 N1 | 32.48 [29.59, 35.34] | 33.77 [30.29, 42.78] | 29.56 [28.90, 45.19] | 33.50 [29.36, 46.65] |
| Look-ahead menu_7000 N8 | 22.69 [21.53, 26.30] | 23.01 [21.71, 27.92] | 24.40 [21.54, 28.27] | 23.71 [21.96, 26.74] |
| Look-ahead frame-55 N1 | 465.76 [456.07, 483.05] | 674.54 [642.09, 744.85] | 451.49 [437.85, 534.78] | 582.90 [550.43, 706.09] |
| Look-ahead frame-55 N8 | 293.22 [288.35, 301.13] | 248.47 [242.17, 254.37] | 259.39 [250.40, 279.37] | 323.23 [298.63, 335.62] |
| Graphs menu_7000 N1 | 31.28 [29.75, 36.84] | 27.16 [25.81, 28.44] | 28.43 [27.62, 29.50] | 33.72 [32.50, 37.44] |
| Graphs menu_7000 N8 | 25.44 [23.49, 29.25] | 23.87 [22.56, 26.25] | 25.78 [24.98, 27.73] | 24.73 [23.62, 25.59] |
| Graphs menu_7033 N1 | 30.20 [28.72, 31.18] | 25.58 [24.08, 28.22] | 26.34 [25.19, 28.19] | 33.95 [33.21, 41.47] |
| Graphs menu_7033 N8 | 24.42 [22.89, 26.94] | 26.66 [25.05, 29.99] | 27.13 [25.85, 29.68] | 24.33 [23.19, 25.62] |
| Graphs frame-55 N1 | 585.80 [512.93, 665.19] | 360.73 [335.01, 376.27] | 330.85 [328.95, 339.18] | 462.09 [440.91, 557.48] |
| Graphs frame-55 N8 | 282.87 [252.58, 320.79] | 264.11 [254.34, 283.91] | 260.10 [243.82, 274.10] | 254.52 [245.56, 264.84] |

Look-ahead dense N8 candidates remain below both baseline run ranges. N1 look-ahead is inconclusive/regresses in the pooled sample and varies sharply between runs. Dense N8 graphs are inconclusive: the OFF run medians differ by 28.35 ms, much more than the 2.72 ms pooled ON regression. Do not interpret pooled bootstrap intervals in the original exploratory analysis as independent-sample causal confidence.

### Profiled attribution

Medians after the same additional warmup exclusion; four samples/variant for each profile pair. These are event/host intervals from a separate profiled pair, not sums with the unprofiled worker table.

| Workload / comparison | Worker | Detector event | Recognizer event | Network host | Group wait | Host merge | Resident analysis |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Look-ahead menu_7000 N8 baseline | 23.38 | 4.12 | 11.43 | 8.12 | 5.16 | 0.11 | 4.63 |
| Look-ahead menu_7000 N8 candidate | 24.81 | 3.91 | 11.67 | 7.37 | 6.67 | 0.10 | 5.12 |
| Look-ahead frame-55 N8 baseline | 306.60 | 16.58 | 200.54 | 52.04 | 163.57 | 59.47 | 6.02 |
| Look-ahead frame-55 N8 candidate | 288.75 | 19.81 | 209.34 | 88.50 | 77.39 | 87.08 | 6.03 |
| Look-ahead frame-55 N1 baseline | 497.69 | 17.35 | 385.23 | 329.28 | 61.74 | 60.64 | 6.06 |
| Look-ahead frame-55 N1 candidate | 442.78 | 17.22 | 371.07 | 330.72 | 7.64 | 56.63 | 6.01 |
| Graphs menu_7000 N1 0 | 31.71 | 4.00 | 18.97 | 16.69 | 3.75 | 0.09 | 4.41 |
| Graphs menu_7000 N1 1 | 26.63 | 3.41 | 14.68 | 0.44 | 14.23 | 0.14 | 4.62 |
| Graphs menu_7000 N8 0 | 23.86 | 4.07 | 11.42 | 6.57 | 7.17 | 0.10 | 4.54 |
| Graphs menu_7000 N8 1 | 25.60 | 3.39 | 12.41 | 6.54 | 6.32 | 0.16 | 4.60 |

In dense N8 look-ahead, profiled worker time falls 17.85 ms (5.8%), while the recognizer event interval **rises** 8.80 ms (4.4%). Network host and merge also rise in that pair under shared load; the uncovered stream-gap median falls from 61.49 to 28.22 ms. This supports a feeding/overlap explanation but does not establish a fixed GPU-compute speedup. Graph menu N1 reduces the network-host interval 16.69 to 0.44 ms and recognizer event interval 18.97 to 14.68 ms; host/GPU overlap means those savings cannot be added. Dense graph runs were unprofiled; no new dense graph CUDA-event saving is claimed.

### Work sizes, graph churn and setup

- menu_7000 N1: candidate words [12]; prepared lines [10]; profiled detector passes [1], recognition samples [10], calls [10]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- menu_7000 N8: candidate words [12]; prepared lines [10]; profiled detector passes [1], recognition samples [10], calls [2]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- menu_7033 N1: candidate words [11]; prepared lines [9]; profiled detector passes [1], recognition samples [9], calls [9]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- menu_7033 N8: candidate words [11]; prepared lines [9]; profiled detector passes [1], recognition samples [9], calls [2]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- frame-55 N1: candidate words [262]; prepared lines [94]; graph runs unprofiled; original profiled baseline executes 3 detector passes and 197 recognition samples (N1: 197 calls, N8: 26 calls); image egress calls [0]. Counts are candidates, not visible/accurate words.
- frame-55 N8: candidate words [262]; prepared lines [94]; graph runs unprofiled; original profiled baseline executes 3 detector passes and 197 recognition samples (N1: 197 calls, N8: 26 calls); image egress calls [0]. Counts are candidates, not visible/accurate words.

All graph capture setup records: {'n': 191, 'p50': 1.0907, 'p95': 1.7816, 'min': 0.6352, 'max': 3.4824} ms. This measures capture/instantiate host setup, not model preparation or first warm replay. Original startup and every first timed sample remain in raw jobs. There is no cold engine-build measurement. Dense N8 graph ON reported 54 recognition captures / 360 replays / 54 invalidations per network lifetime, and 18 detector captures / 18 replays / 17 invalidations: shape churn is material. N1 menu recognition reuses one capture for hundreds of calls.

Graph hash groups cover full OCR output (including geometry/confidence/dispositions), colors, prepared lines, reference flags/counts, device input and current-input agreement. Every source/capacity group has one semantic hash across OFF/ON/refusal; no candidate word, confidence or geometry difference was observed in this corpus. No ground truth or rendered/UIA acceptance follows. Original look-ahead semantic parity also holds. Every diagnostic cache-before/cache-after inventory matches.

The dense original profile executes three detector passes (including normal
detail work), 197 recognition samples and 26 N8 calls, returning 94 prepared
lines and 262 candidate words. About 66% of its fixed canvas width is unused.
This is a full production-path worker workload, not a single detector pass or
a repeated cached recognition result. Graph dense trials retain that work but
were unprofiled. Source file SHA-256 values are:

| Source | SHA-256 |
| --- | --- |
| menu_7000.png | `27423cb90de78806b535bf10a8c71d513118548388d09a5786d73b51637b686d` |
| menu_7033.png | `af30d22349360ce815fca1865c4fa373216563738839f0da8df79a21bb0f8ec0` |
| frame-55.png | `5954d76ee37311ffa64eb8428dba111594ea1efaeb0e69e5929dc2e35e61d396` |

The final local `receipt.json` and `evidence-files.sha256` cover commands,
scripts, raw copied evidence, final/actually measured source snapshots,
runtime/engine/executable hashes and canonical LF patches. Runtime file hashes
are observed when archiving after the run; they are not in-memory module
attestation. Model/engine/input hashes are also captured by each run's manifest.


## Pipeline audit and latency model

| Boundary | Current integrated behavior | Consequence / smallest useful next step |
| --- | --- | --- |
| Capture / scene authority | Neutral geometry-correct scene before appearance/zoom; owned two-slot analysis pool, whole-pair byte bound; busy slots skip | Preserve input lease and exact current-frame pairing. Trace capture-copy readiness and skipped opportunities before changing buffer counts. |
| Motion validation | Separate latest-only probe worker; default 80 ms probe interval and 60 ms residual budget | Can overlap OCR; do not join it to a slow inference or replace current-pixel checks with agreeing smoothed OCR. |
| OCR scheduling | One worker, one in flight plus one overwriteable pending job; full work outranks optional regions | Additional queued frames increase age rather than useful throughput. Measure dequeue age, coalescing, rejected age and accepted completion separately. |
| Cadence | Unchanged live refresh 1500 ms; advancing media minimum submission interval 350 ms | Speeding a 30 ms inference cannot produce 30 new reads/sec under that policy. Changed/pending live text has other triggers; 1500 ms is not a universal live limit. |
| Standalone paused / Freeze | Exact selected image can use standalone admission; no invented independent neighbour | First sharp text need not wait for fabricated temporal evidence. Held redraws are not new acquisitions; pan/zoom reproject caches. Detail settles 300 ms and remains bounded. |
| Resize / detect | GPU normalize; detector dimensions bounded by existing policy; resident connected regions and recognition windows | Detector-count completion is a real dependency before crop count/batches can be issued. Approximate detector size has coverage risk for small print. |
| Recognize | Fixed 48x1024 float I/O, default N8 for medium; crop/CTC/compact metadata on private stream | Serial calls and partial batches limit worker service. Narrower prepared canvases and precision are the largest work-reduction candidates, but require same-source quality evidence. |
| Transfer / synchronization | Resident input/logits; download count then token/quad metadata; group completion before host merge | Pack metadata and feed the next same-shape group before host merge. Never remove input completion or release a leased buffer early. |
| Geometry / protection | Integrated 128 cooperating GPU threads/request with bounded FP32 filter and FP64 extrema; original thresholds/caps | Prior CUDA-only medians ~0.21-0.27 ms, excluding OCR/allocation/UI. Further gains here have little whole-worker leverage. The captured CPU4/GPU5 fit discrepancy remains unresolved. |
| Admission / publication | Source/generation fences, proof, protection and drawability applied after worker result | Faster OCR may still be rejected or hidden. Trace accepted result, model assignment and actual first paint with the same ticket. |
| Shaping / paint | Ink/caret and glyph layout caches; pan/zoom reproject; presented palette maps cached colors without OCR | Avoid redundant `SetWords`/layout only after measuring content-change costs. Do not make a color change trigger OCR. Preserve glyph/plate/protection and accessibility geometry parity. |
| Accessibility / display | Actual painted plan supplies character positions; semantic additions paced, removals immediate; video presenter independent | A one-second semantic cadence is intentional, not detector time. Display FPS, glyph paint and screen-reader availability need distinct clocks. |

Source anchors: [scheduler and ownership](../include/okuflow/common/ultra_clear_text.hpp),
[scheduler implementation](../src/common/ultra_clear_text.cpp),
[controller timing and admission](../include/okuflow/app/ultra_clear_text_controller.hpp),
[controller publication](../src/app/ultra_clear_text_controller.cpp),
[TensorRT execution](../src/common/uct_tensorrt.cpp),
[analysis pool](../src/cuda/uct_analysis_pool.cpp),
[overlay caches and actual paint](../src/ui/ultra_clear_text_overlay.cpp),
[positioned accessibility](uct_screen_text_accessibility.md),
[current geometry](uct_surface_perspective.md).

For an eligible live frame, model latency as capture readiness + admission wait +
queue age + worker service + result dispatch + proof wait (where needed) + model/
layout + next paint/present. These are sequential boundaries for one ticket;
other frames, probes and presentation overlap. Sustained OCR service is bounded
by the serial worker and admission cadence, while display frames can continue
at a different rate. A two-group look-ahead overlaps GPU work with host merge;
it does not run two TensorRT contexts concurrently or double throughput.

## Ranked implementation proposals

1. **Recognition-group look-ahead for dense pages.** Opus' measured candidate
   uses two bounded pinned metadata slots and an event for each group. It
   issues the next group before merging the current group only when canvas
   and group size match; partial/shape transitions remain serial. Preserve
   stream ordering, output order, cancellation drains and failed-completion
   quarantine. Validate cancellation between issue/wait/merge and width/batch
   transitions before integration, then repeat on production local BF16 and
   an independently varied dense/small-font corpus. Added memory is one group
   of pinned metadata plus two events, not a second model context. Higher
   utilization may raise power; no energy measurement was made.
2. **Use less padded recognizer work, under explicit quality review.** Menu
   profile bins show roughly 81% unused fixed canvas width; that is a work
   opportunity, not a promised 81% speedup. The existing opt-in width policy
   and reviewed mixed-BF16 path are preferable starting points to another
   model substitution. Measure exact crop sets, widths and plan identities;
   report missing/extra/changed words, confidence, dispositions and visible
   coverage on both menu frames plus small-font/tilt/color/density controls.
   Baseline agreement is not ground-truth accuracy. Narrower canvases can
   clip context or change CTC tokens; do not silently promote them.
3. **Graph replay where context state stays stable.** Sol's opt-in prototype
   captures only enqueueV3, after an ordinary shape-update enqueue, and keeps
   at most one graph per Network/current context. Shape, input binding,
   allocation generation or stream changes drain/destroy before reconfiguration.
   The Network owns the context, fixed profile and automatic workspace; there
   is no shape-only cache of stale graphs. A typed capture refusal ends
   capture, checks errors, drains and disables graph use for that network;
   unrelated CUDA faults retain normal failure/quarantine. Keep this separate
   from look-ahead. Batch/shape churn incurs recapture and can erase gains.
   A future bounded graph-per-context design would add activation/context
   memory and needs its own measured justification and lifetime tests.
4. **Coalesce compact metadata copies and CPU merge work that starves GPU.**
   Current code queues three small copies per recognition sample. Pack count,
   quad and bounded tokens in device output, then copy one bounded group to
   the existing double-buffered pinned storage. Preserve invalid-token flags,
   steps/count bounds and ordering. Time launch count, transfer interval,
   host merge, event gaps and worker service before/after; a smaller memcpy
   count alone is not a win. No extra full-frame readback or precision loss
   is necessary. The first-party packing kernel needs normal CUDA ownership
   and mixed-width tests.
5. **Measure freshness-to-paint, then tune scheduling/publication.** Use the
   existing bounded trace with explicit drop accounting. Add a closed ticket
   timeline for source capture, pending age, dequeue, worker, accepted proof,
   layout and real paint. Reduce obsolete work at safe group boundaries and
   redundant unchanged layout only when those intervals dominate. Keep full
   priority, optional-region slack, independent validation, truthful UIA
   geometry and standalone selection authority. Raising submission rate alone
   can consume power and increase stale-result rejection.

The owner's allowance for small measured quality loss applies to approximate
precision/resolution/work choices, not to source identity, stale fencing,
bounded memory, cancellation, graph address lifetime or truthful accessibility.

## Validation and retained failures

Opus block1 and Sol block3 each passed the tracked exact
`uct_paddle_processing` + `uct_paddle_cuda` selection: **SCOPED PASS 2/245**,
no skipped Qt rows or unresolved tests. These model-free contracts do not cover
all model-backed graph cancellation, lifetime or publication paths. The private
resident diagnostics are unregistered measurements, not release acceptance.

Every dense diagnostic exits **1** at the original strict N8-versus-N1 output
gate. All outputs and assertions remain intact. Exact output parity between
baseline/candidate or graph OFF/ON is a different check and does not turn that
exit into a pass. Menu and actual capture-refusal runs exit 0. The initial
comma-list invocation of the first tracked gate failed at argument validation;
the corrected exact selection passed, with both attempts archived.

The first block3 invocation failed because PowerShell's typed `Deadline`
parameter collided case-insensitively with `deadline`. Compilation finished;
no inference had started. The distinct `blockDeadline` repair used the same
original hard deadline and preserved the first script/build failure. Offline
analysis also required an explicit UTF-8 read fix and a local module-loading
fix under Python isolated mode; these are analysis repairs, not application
results. Opus' provider rate-limit interruption is retained in this thread and
the local continuation receipt.

After compute release, static review corrected one unexercised graph-stream
switch branch to drain the recorded old stream instead of the incoming stream.
This final one-line correction is **unbuilt/unrun**. The caller in all measured
runs uses one fixed State stream. Both the exact measured snapshot and final
source are archived; do not claim that the final source hash was tested.
See [graph design and remaining lifetime checks](uct_trt_graph_experiment.md).

Sol block3 acquired the runner lease at 01:19:39+03, completed benchmarks at
01:24:00, and finished the tracked gate at 01:24:16. The lock was verified free
at 01:24:50; RELEASE was sent to the parent before the original 01:34 deadline.
No further compiler/GPU/test work was performed. The first Opus block's release
and prior receipts remain in `block.log`.

## Reproduction and acceptance limits

Use the private `block.ps1` / `block2.ps1` receipts for the first block and
`block3.ps1 -Deadline <explicit timestamp>` for the continuation, with parent
serial-compute authorization and the tracked runner lease. `build.cmd tests`
performs only the own-cache `uct_paddle_tests` incremental build with two jobs.
The scripts explicitly select the inference-only SDK and isolated offscreen
Qt diagnostic environment. Never run this with a different source/cache home,
on an expired lease, against an owner's application, or as a release/FPS gate.

Offline analysis:

```powershell
python -B -I Z:/Projects/OkuFlow/local-work-data/uct-throughput-opus-20261023/analyze.py Z:/Projects/OkuFlow/local-work-data/uct-throughput-opus-20261023
python -B -I Z:/Projects/OkuFlow/local-work-data/uct-throughput-opus-20261023/analyze_graphs.py
```

NVIDIA documents the required post-shape-change enqueue, context/address
specificity, capture limitations and nonblocking-stream rules in the
[TensorRT 11.4 optimization guide](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/performance/optimization.html).
This supports the graph design constraints; performance conclusions here come
from the private same-source measurements, not NVIDIA's example timings.

Cold preparation, production BF16/regional throughput, real-camera FPS,
capture-to-first-sharp-text latency, power/energy, native paint/UIA and full
release acceptance were not measured. No owner app, settings, foreground,
camera, microphone, phone, foreign process or parent cache was changed.
