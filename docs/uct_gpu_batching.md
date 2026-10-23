# UCT GPU recognition batches (development)

An additional [opt-in width policy](uct_recognizer_width.md) reduces padding
within the same contiguous groups. It changes no model tier or default and
requires its own prepared dynamic-width plans and cross-width quality evidence.

The pinned PP-OCRv6 recognizers accept a dynamic batch dimension. Previously,
queuing eight batch-one calls reduced completion waits but still executed each
network separately. The new path rectifies multiple crops into distinct input
slots and submits one TensorRT inference for up to eight or sixteen crops.
Per-sample CTC decoding and source geometry remain separate. Detector/detail
coverage, FP32 weights, confidence thresholds and admission rules are unchanged.
Repeated TensorRT input shapes and unchanged tensor addresses are cached.

Rectification and CTC decode now also launch once per neural batch instead of
once per crop: `gpu::RectifyBatch`/`gpu::DecodeBatch` take a device slot-index
array (one compact upload per submission group) and evaluate the identical
per-element bilinear, argmax tie-break and compaction arithmetic as the
per-crop kernels, which remain for fixtures. Each batch's token counts, quads
and tokens return as three contiguous copies with the same bytes and layouts
as the former per-crop copies. Decoded lines are buffered per original crop
index and merged, hashed and captured in detector order at region end, so
output words, order, dispositions and geometry are unchanged by construction;
the profiler's merge interval now covers token decoding plus the deferred
merge. `gpuBatchedRectifyAndDecodeMatchPerSampleKernels` checks bit equality
against the per-crop kernels, including permuted slots, ties, a non-finite
sample and width-limited steps. Cross-binary strict output comparison and
timing for this launch/transfer reduction are recorded with the optimization
race evidence; no speedup is claimed here without that measurement. Race
status (2026-10-22): the fixture and scoped gate passed and a fixed1024
resident visit on the batched binary strict-passed its N1/N8 comparisons,
but no same-workload baseline-binary timing exists, so the launch/transfer
reduction remains unpriced; one bucketed N8-vs-N1 confidence cell drifted
0.0010389 (threshold 0.001) with zero geometry delta and identical truth
scores, unattributed between this change and pre-existing batch numerics.

The opt-in profiler additionally hashes ordered crop descriptors already present
in compact host metadata. Its versioned little-endian encoding includes detection
region, quad, score, useful width and internal-cut edges. This establishes whether
comparisons received equal crop work; no image hash/readback or synchronization
is added. Character sensitivity remains a separate
[verification gate](uct_character_verification.md).

`OKUFLOW_UCT_TRT_BATCH` explicitly selects `1`, `8` or `16`; the development
default is eight for medium/small recognizers and one for tiny. Sixteen
remains experimental. Existing saved model choices and explicit overrides are
preserved. The separately authorized [quality decision](uct_batch8_quality_acceptance.md)
retains the historical strict failures. Invalid
overrides make this optional backend unavailable, preserving ordinary CPU
fallback. Batch sixteen preparation can fall back to
eight, then one; batch eight can fall back to one. Diagnostics report the actual
capacity rather than calling a fallback the requested mode. Dynamic profiles
include their exact min/opt/max dimensions in new recognizer cache identities;
old batch-one and detector plans are retained. Inputs remain owned through
completion or quarantine, with cancellation between bounded groups.

## Memory and allocation

A tensor ceiling validates a dynamic output size before allocation. It is not
reserved memory, a recording buffer or a total application VRAM limit. Only the
actual tensor extent is allocated. Detector and batch-one/eight outputs retain
the 256 MiB ceiling; batch sixteen permits up to 1 GiB. Checked element products
and batch/output dimensions guard per-sample offsets. The builder workspace cap
remains 256 MiB, separate from weights, context memory and input/output buffers.

The measured medium recognizer output had 128 time steps and 18,710 classes:
batch-eight FP32 output used 73.09 MiB and sixteen used 146.17 MiB. Inputs used
4.5 and 9 MiB. A 256-step output would double those output sizes. Raising the
ceiling alone provides no speedup and does not allocate 1 GiB eagerly. Smaller
GPU capacity can still require the lower-batch fallback.

## Finite comparison and scope

The unregistered `optionalTensorRtBatchComparison` slot is explicitly opt-in via
`OKUFLOW_UCT_BATCH_DIAGNOSTIC=1` and `OKUFLOW_UCT_TEST_IMAGE`. The authorized
comparison uses the same private source at capacities one, eight and sixteen,
one warmup plus three timed passes each. It permits at most one new preparation
per wider profile, retains old plans and performs no download or cache clearing.
`OKUFLOW_UCT_BATCH_TEST_TIER=small` selects both small models for the separately
requested follow-up; the default `medium` selects both medium models. Model
repositories, revisions and graph/configuration pins are recorded with the
source and each output. Independent optional overrides
`OKUFLOW_UCT_BATCH_TEST_DETECTOR_TIER` and
`OKUFLOW_UCT_BATCH_TEST_RECOGNIZER_TIER` accept `tiny`, `small` or `medium`;
an omitted override inherits the selected shared tier. These are private test
selectors, not application settings. Wider results compare with their own
detector/recognizer pair's batch one.
First completion including preparation is reported separately from steady work.
Raw outputs and full word/geometry/confidence records stay in a unique
`%LOCALAPPDATA%/OkuFlow/uct-batch-*` folder.

Every timed wider result must retain batch one's exact text, order and token
dispositions, with at most 0.001 pixel geometry and 0.001 confidence variation.
Counts alone are not accuracy evidence. A failed comparison retains its output;
it is not permission to relax recognition thresholds or repeat until passing.

`OKUFLOW_UCT_TRT_PROFILE=1` enables bounded, reusable CUDA event pairs and compact
JSON stage/call/binding/memory diagnostics; ordinary use allocates no profiling
events and emits no per-job profile logs. Stream event intervals include launch
gaps and external contention. Worker timing also includes input upload and
scheduler metadata work, while the stage intervals exclude source upload.
Reported free-memory deltas are approximate, not peak or exclusively owned VRAM.
The profiler also reports useful crop-width bins, padding fraction and existing
detector-count/group host wait intervals. These add no readback or new wait and
are attribution data, not a speedup claim. See the
[revised performance stages](uct_performance_roadmap_2026_10_20.md).
These measurements do not establish native presentation, first validated glyph,
camera latency or a 16-ms complete OCR budget.

## One-time RTX 4090 result (2026-10-20)

The private 1920-wide video frame used the medium detector/recognizer, three
detection passes and 136 recognizer crops. Each profile had one warmup and three
timed scheduler passes. The driver was 617.42. The same source and pinned models
were retained; no plans were deleted or downloaded.

| Metric | Batch 1 | Batch 8 | Batch 16 |
| --- | ---: | ---: | ---: |
| Median scheduler worker, ms | 392.49 | 218.43 | 241.01 |
| Timed worker range, ms | 380.71–404.49 | 217.11–219.55 | 235.87–251.75 |
| Median OCR stream wall, ms | 333.84 | 167.23 | 185.62 |
| Median recognizer event intervals, ms | 290.31 | 130.19 | 149.46 |
| Recognizer enqueues | 136 | 19 | 11 |
| Output tensor allocation, MiB | 9.14 | 73.09 | 146.17 |
| First result including preparation, s | 4.45 (cached) | 78.01 (new plan) | 80.29 (new plan) |

Batch eight reduced measured worker completion by about 44%; sixteen by 39%.
Sixteen was slower than eight despite fewer launches. This does not establish
the best configuration across GPUs, images or model tiers. A complete 16-ms OCR
budget was not achieved: the detector alone took about 17 ms in wider profiles.
Cold wider-profile compilation is not comparable to cached batch-one startup.
Exactly two new plans were created; the four previous plans remained identical.

All timed outputs retained the same text, order, dispositions and 178 candidate
words. Final fitted word bounds were identical. The strict comparison nevertheless
**failed**: batch eight changed the word `models`' protected/search envelope by
up to 1.5633 pixels, and maximum confidence differences were 0.004679 (eight) and
0.004345 (sixteen), above the preselected 0.001 limits. This is limited same-image
agreement evidence, not a recognition-accuracy evaluation. The assertion was not
weakened and the experiment was not rerun to obtain a pass.

The process also exited with access violation `0xc0000005` after QTest printed
its cleanup pass and totals (two passed, one strict comparison failed). All
warmup/timed results had been saved. Process-exit ownership needs investigation;
these wider profiles are not accepted as the automatic default.

The follow-up diagnostic now scopes its scheduler and waits up to ten seconds
for each exact TensorRT State retirement receipt before changing profiles or
exiting. A separate `optionalTensorRtRetirement` uses just one cached N16
inference and an inference-only SDK folder that cannot compile missing plans.
These receipts cover State closure release after known stream completion;
nested analysis storage cleanup and vendor threads have separate lifetimes.
Normal process exit is checked independently. The original failure is retained.
The one-pass cached medium N16 follow-up passed and exited normally with code
zero; its cache inventory was unchanged. This supports that isolated fixture's
receipt/exit behavior, without proving a vendor-wide crash cause or resolution.
Its complete output and receipt are retained in `%LOCALAPPDATA%/OkuFlow/`
`uct-retirement-20261020-020038-cae9d3a6-b05e-4a24-8853-5314d8cd641d/`.

Raw logs, hardware/cache inventories and derived analysis are retained under
`%LOCALAPPDATA%/OkuFlow/uct-master-batching-20261020/`
`batch-run-20261020-044018-9aea83b167864beaae38776350fff331/`.
Full outputs are in `%LOCALAPPDATA%/OkuFlow/`
`uct-batch-20261020-014019-9a2fb8b4-0d56-4c1c-83b0-14499216c2c5/`.

## Small/small follow-up on the same frame

The separately requested small detector + small recognizer comparison used the
same pixel digest, one warmup and three timed passes per capacity. All previous
plans remained unchanged. Exactly three missing plans were prepared once: the
small detector and small recognizer N8/N16; small recognizer N1 was cached.

| Metric | Batch 1 | Batch 8 | Batch 16 |
| --- | ---: | ---: | ---: |
| Median scheduler worker, ms | 279.69 | 143.53 | 129.63 |
| Timed worker range, ms | 270.77–280.43 | 142.50–146.79 | 127.96–143.75 |
| Median OCR stream wall, ms | 229.22 | 90.74 | 77.56 |
| Median recognizer event intervals, ms | 199.39 | 60.87 | 49.86 |
| Recognizer enqueues / crops | 149 / 149 | 20 / 149 | 11 / 149 |
| Output tensor allocation, MiB | 9.14 | 73.09 | 146.17 |
| First result including preparation, s | 40.47 (new detector) | 61.77 (new recognizer) | 63.57 (new recognizer) |

Small N16 reduced measured worker time by about 54% versus small N1, and about
41% versus medium N8 in the earlier diagnostic. Here sixteen was faster than
eight, unlike the medium pair. These separate one-time runs are diagnostic
comparisons, not a controlled hardware-wide ranking. Small N16 still took about
78 ms in the OCR stream path, including its host gaps/merging; its recognizer
event intervals alone were about 50 ms. A complete 16-ms budget was not reached.

Every small pass retained 173 candidate words (195 total tokens, including
omissions/non-candidates), versus medium's 178 candidates. There were 163 exact
case-sensitive candidate occurrences in common. Differences include segmentation
such as `+53-53` versus `+53` / `-53`, and uncertain symbols; counts do not establish
which model reads the scene better. This is not a ground-truth accuracy test.

Within the small pair all text/disposition multisets and spatially matched final
fitted bounds were identical. Nevertheless the original strict comparison
**failed** in every wider pass. N8 swapped `< S Annotate` and `Edited 8 files`:
the latter's protected/search top moved from 129.53884 to 125.52527, crossing
the former's 126.86314. The backend sorts before final ink fitting. Index-based
maxima of 547.24 pixels / 0.5986 confidence therefore compare different words;
they do not describe physical word movement. Offline identity matching found
at most 7.2332 pixels of envelope change and 0.02868 confidence variation.
N16 retained order/envelopes/bounds but changed confidence by up to 0.03100,
also above the unchanged 0.001 limit. No outputs or assertions were reordered
or relaxed to pass. Both wider profiles remain experimental.

All three exact State retirement receipts completed and the test exited normally
with code one for its strict assertion failure, without another access violation.
The full outputs/model pins are retained in `%LOCALAPPDATA%/OkuFlow/`
`uct-batch-20261020-020043-6f625385-5b2a-4c34-bfde-bd78e4e3f184/`.
Raw child logs/exits, cache/hardware inventories and derived analysis are under
`uct-master-batching-20261020/`
`retirement-small-run-20261020-045844-979e90515e6143b387eefed72b73d3f7/`.

## Tiny detector / medium recognizer follow-up

The separately requested mixed pair used the identical private frame and GPU,
one warmup plus three timed passes per capacity. All four required plans were
already cached with valid payload digests. The inference-only runtime contained
no parser/builder resources; no plans were created, replaced or downloaded.
All nine existing plan files retained their exact hashes.

| Metric | Batch 1 | Batch 8 | Batch 16 |
| --- | ---: | ---: | ---: |
| Median scheduler worker, ms | 447.98 | 263.63 | 269.70 |
| Timed worker range, ms | 413.54–453.73 | 243.90–266.54 | 264.05–287.13 |
| Median OCR stream wall, ms | 391.05 | 211.21 | 217.10 |
| Median detector event intervals, ms | 7.75 | 9.75 | 6.93 |
| Median recognizer event intervals, ms | 354.29 | 176.70 | 189.44 |
| Recognizer enqueues / crops | 171 / 171 | 23 / 171 | 13 / 171 |
| Output tensor allocation, MiB | 9.14 | 73.09 | 146.17 |
| First result including cached preparation, s | 3.03 | 2.28 | 2.49 |

Batch eight reduced worker time by about 41% versus this pair's batch one;
sixteen reduced it by about 40%. The lighter detector did not give the shortest
complete read on this image: it produced 171 recognizer crops, versus 136 with
the medium detector, about 26% more recognition work. Its detector was faster,
but the medium recognizer still dominated. The earlier medium/medium N8 worker
median was 218.43 ms and small/small N16 was 129.63 ms. Those are separate,
non-interleaved runs, not a controlled universal ranking. Neither worker time
nor OCR stream time reached the requested 16-ms target. Worker time includes
the private image upload and CPU result preparation; these are not native
camera/display latency or first admitted-glyph measurements.

All mixed-pair variants retained 187 candidate words, 206 total tokens and 42
lines with exact text/order/dispositions. This count is not accuracy evidence:
170 case-sensitive candidate occurrences were shared with the medium/medium
reference's 178 candidates; the differences include symbols and segmentation.
The unchanged strict wider-batch comparison **failed**. N8/N16 both changed
protected/search envelopes by up to 3.1946 source pixels; N8 changed a final
fitted bound by one pixel, while N16 retained final bounds. Maximum confidence
variation was 0.01910 for N8 and 0.03101 for N16, above the 0.001 limit.
No acceptance limits or outputs were changed to obtain a pass. Batch one remains
the application default; this experiment did not change model settings.

All three exact TensorRT State retirement receipts completed. QTest retained
its failed assertion and the process exited normally with code one, without an
access violation. Full model/source pins and outputs are under
`%LOCALAPPDATA%/OkuFlow/`
`uct-batch-20261020-021407-9528d9b1-73a3-45eb-981c-5ff0293eb6ac/`.
Raw logs/exits, before/after cache/hardware inventories and derived analysis are
under `uct-tiny-medium-batching-20261020/`
`run-20261020-021405-fd1b73707b0b4afaa6e93658dc38cc0e/`.

The private diagnostic selector extension passed scoped `uct_paddle_processing`
checks in both CPU and CUDA presets. It changes no production source and does
not replace the integration matrix or establish native GUI/quality acceptance.

## Integration validation

The final tracked background matrix passed shipping Release compilation, all
108 CPU cases (two include optional skipped Qt rows) and all 17 CUDA delta cases,
with no whole-test skips/unresolved results. Exact per-case fingerprints were
reused when unchanged. `retirement-small-final-matrix.log` retains that gate.
Native GUI/current-head screen-reader acceptance remains pending separately.
These normal CTest passes do not turn the optional strict comparisons into passes.
