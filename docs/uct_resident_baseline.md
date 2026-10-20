# UCT resident SubmitDevice baseline (diagnostic)

Status (2026-10-20): integrated and measured. The finite resident run completed
normally but failed strict wider-batch agreement; defaults remain unchanged.
It is an unregistered, opt-in QtTest slot in `uct_paddle_tests`; ordinary CTest
and default test runs skip it without touching a model or GPU.

## Why it exists

The roadmap's first change asks for an honest device-resident baseline before
any model, batching or host-handoff decision. The earlier
`optionalTensorRtBatchComparison` builds a CPU `QImage` and calls
`RecognitionScheduler::Submit`. Its worker time therefore includes a source
upload and the CPU-image reference/color/ink path, so the gap between worker
completion and the OCR stream interval is not the resident camera path's
overhead. This diagnostic times the actual `SubmitDevice` path instead.

## Existing versus new

| Capability | Before | This diagnostic |
| --- | --- | --- |
| Scheduler entry | `Submit(QImage)` (upload inside worker) | `SubmitDevice` with an owned `AnalysisPool` image |
| Resident reference/color/ink | Not exercised by the batch diagnostic | Real scheduler `PrepareResidentAnalysis`, timed separately |
| Upload and analysis copy | Inside worker time | Completed before submission, recorded as setup |
| Plan creation | Batch diagnostic may prepare missing wider plans | Refused: inference-only SDK plus loader-rule plan precheck |
| Model pairs per run | One | Up to four, randomized per round |
| Repetition | One warmup, three passes, fixed order | Seeded rounds, one warmup and 1–20 timed passes per source |
| Sources | One | Up to four, shuffled within each block |
| Strict agreement | Wider vs N1 by pass index | Same unchanged gate vs the first timed N1, plus N1 repeatability |
| Identity-matched diagnosis | Offline only | Recorded beside each strict comparison, never a gate |
| GPU/resident proof | `usesGpu` and capacity text | Also device input, zero image egress, resident references, one profile, exact fingerprint (media) |

Reused unchanged: `uct_batch_test::RetirementReceipts`, `Compare`, `Output`
conventions, the TensorRT `UCT_TRT_PROFILE` stage events and model pins.

## Selection

All selectors are private test inputs, not application settings.

| Variable | Meaning |
| --- | --- |
| `OKUFLOW_UCT_RESIDENT_DIAGNOSTIC=1` | Required gate; otherwise the slot skips |
| `OKUFLOW_UCT_RESIDENT_PRECHECK_ONLY=1` | Validate identities and stop before model loading or uploads |
| `OKUFLOW_UCT_RESIDENT_PAIRS` | Required, e.g. `medium:small,medium:medium` (detector:recognizer, up to four) |
| `OKUFLOW_UCT_RESIDENT_CAPACITIES` | Default `1,8,16`; must include `1` |
| `OKUFLOW_UCT_RESIDENT_IMAGES` | Semicolon-separated private sources (up to four); else `OKUFLOW_UCT_TEST_IMAGE` |
| `OKUFLOW_UCT_RESIDENT_ROUNDS` | 1–10, default 3 |
| `OKUFLOW_UCT_RESIDENT_PASSES` | Timed passes per source per block, 1–20, default 5 |
| `OKUFLOW_UCT_RESIDENT_SEED` | Order seed, default 20261020; the planned order is saved first |
| `OKUFLOW_UCT_RESIDENT_STAMP` | `camera` (default, live stamp) or `media` (adds the GPU source fingerprint) |
| `OKUFLOW_UCT_RESIDENT_INPUT` | `single` (default) or `pair` (Temporal Smooth pair with identical halves) |
| `OKUFLOW_UCT_RESIDENT_PROFILE` | `1` (default) enables CUDA stage events; `0` measures uninstrumented worker time |
| `OKUFLOW_UCT_TENSORRT_ROOT` | Required inference-only SDK folder |
| `QTEST_FUNCTION_TIMEOUT` | Required, at least the selection's printed allowance |
| `OKUFLOW_UCT_RESIDENT_PRECHECK_ONLY=1` | Optional: check selection, SDK, plans, models and sources and write the manifest, without loading a model or recording timing |

A selection is limited to 60 blocks and 1000 jobs. An optional UTF-8
`<image>.truth.txt` beside a source enables the reading-order truth record.

## Preconditions and refusals

- The SDK folder must contain `nvinfer_11.dll` and no parser or builder
  resources, so TensorRT cannot compile a missing plan.
- Every required plan (detector, then recognizer at each capacity) is checked
  read-only with the loader's own rule before any model work: key from the
  pinned graph hash, GPU UUID, driver and profile; size and payload digest.
  A missing or invalid plan fails with its key; nothing is prepared.
- `medium:small` needs the medium detector and small recognizer N1/N8/N16
  plans. `uct_gpu_batching.md` records that earlier runs created each of
  these; the precheck confirms they are still present and valid.
- The plan cache inventory (fresh full-file hashes, names, sizes, times, headers)
  is saved before and after; any change fails the run. Metadata-preserving
  payload mutation is covered by the model-free self-test.
- Models are read through the pinned `LoadPaddleAssets` path. Nothing is
  downloaded and no cache is cleared.

## Procedure

For each planned block (one model pair at one capacity), in seeded order:

1. Create a direct TensorRT recognizer and scheduler (production decimation
   and minimum word height). Startup polls until the first non-preparing
   result; this cached load plus first inference is the startup record only.
2. For each source in the block's shuffled order: one warmup, then the timed
   passes. Every job gets a fresh owned analysis capture whose copy completes
   before submission. Submissions never overlap.
3. Cancel, destroy the scheduler and wait up to 10 s for the exact TensorRT
   State receipt. A missing or failed receipt stops later blocks.

## Measurements

| Field | Definition |
| --- | --- |
| `submitToCallbackMs` | Host time from `SubmitDevice` to the worker's completion callback |
| `schedulerWorkerMs` | Scheduler worker duration: RecognizeDevice, resident analysis, PrepareLines, status |
| `recognizeDeviceProfiledWallMs` | TensorRT profile wall, detector through merge/sort |
| `residentAnalysisMs` | Scheduler's real `PrepareResidentAnalysis` (reference, colors, ink; fingerprint/registration when selected) |
| `inputWaitReadyMs` | TensorRT's wait enqueue on the owned image |
| `unattributedWorkerRemainderMs` | Worker minus the three items above |
| `stage.*Ms`, `streamStageSumMs` | CUDA event intervals per stage (include launch gaps and contention) |
| `streamIntervalGapMs` | Profiled wall not covered by any stage interval: host waits, merges, launch latency |
| `metadataBytes`, `recognizerCalls`, `recognizerSamples`, `detectorPasses` | Compact transfer and call counts from the profile |
| `detectorCountHostWaitMs`, `recognizerGroupHostWaitMs` | Host intervals around existing completion waits; not extra waits or pure copy cost |
| `analysisCaptureSetupMs` | Owned device-to-device capture and sync; setup, not steady time |
| `startupFirstResultMs` | Construction to first real result; separate from steady statistics |

Statistics keep raw run order; p95 is reported only with at least 20 samples.
Per-round medians and pair differences paired by round are descriptive, not
significance tests.

## Validity and quality gates

A job that is not an actual resident GPU result fails the run and is never
timed: unsuccessful/preparing output, non-TensorRT backend, a recognizer
capacity other than requested, no device input, TensorRT not consuming the
owned image, resident analysis not run exactly once, any full-image egress,
non-resident references, a media fingerprint differing from the exact source
bytes, a missing or wrong profile, or no candidate words.

The strict gate is unchanged from the batch diagnostic: identical text, order,
dispositions and padding, at most 0.001 px geometry and 0.001 confidence
variation versus the pair/source's first timed batch-one output. Batch one must
also repeat. Identity-matched occurrence diagnosis, threshold-margin counts,
cross-pair agreement and truth CER/WER are recorded beside it and never relax
it. Failed outputs are retained. Earlier wider-batch evidence failed this gate
for every tested pair, so exit code one after complete evidence is an expected
strict outcome, distinct from a crash.

For pairs sharing a detector, the cross-pair record includes both pairs'
recognizer sample counts and the ordered crop-descriptor SHA-256. Equal counts
alone do not establish equal work. Matching descriptor fingerprints, source and
detector identities establish identical ordered crop geometry/width/score/cut
metadata; these descriptors were already downloaded for normal publication.
Detector execution still runs in each trial. This is an equal-crop complete-path
comparison with recognizer stage intervals, not a frozen-crop recognizer-only
entry point. No crop cache or skipping inference is introduced.

## Evidence

A unique `%LOCALAPPDATA%/OkuFlow/uct-resident-<UTC>-<uuid>/` folder holds
`manifest.json` (selection, planned order, model/plan/source/GPU/SDK/host
identities), `cache-before.json`, `jobs/` (every startup, warmup and timed job
with full outputs), `blocks/` (startup and retirement per block),
`comparisons.json`, `cache-after.json` and `summary.json`. An early exit leaves
`incomplete.json` instead of a summary; a precheck-only run writes
`precheck.json` after the manifest and stops.

## Scope and lifecycle limits

Receipts cover the exact TensorRT State closure only. The scheduler worker's
thread-local resident context, `AnalysisPool` storage, the resident retirement
queue and TensorRT/CUDA vendor threads have separate lifetimes; the process
exit code must be recorded by the runner. This diagnostic measures no display,
FPS, sensor-to-photon, first validated glyph, admission or screen-reader
latency, and it runs OCR without decoder or presenter contention.

## Model-free self-test

`uct_resident_test::SelectionAndComparisonSelfTest` checks selection refusals,
seeded block planning, plan-key structure, statistics, the strict and
identity-matched comparisons, truth metrics, the metering decorator, profile
capture and the GPU/resident validity rules without a model, GPU or image.
Truth files are bounded to 128 KiB and edit-distance calculations to four million
cells; oversized comparisons are reported as unavailable, never silently passed.
Opus xhigh authored the helper but reached its provider rate limit before final
handoff; root reviewed and completed integration. This is not a completed Opus
independent safety review.

## Finite resident results, 2026-10-20

RTX 4090, TensorRT 11.4.0.106, CUDA 12.9.86 build, Qt 6.12.0. Camera-style
single-frame stamps, three seeded randomized rounds, one warmup and three
timed passes per source/configuration produced 18 blocks, 234 total jobs and
nine timed observations per cell. All blocks completed in 81.35 seconds.
Source uploads and analysis copies were outside steady timing. This is not a
physical-camera or decoder/presenter measurement.

Median scheduler-worker milliseconds, with **medium detection in every row**:

| Recognizer / capacity | Dense video frame | Sparse synthetic print | Long synthetic lines |
| --- | ---: | ---: | ---: |
| Medium / 1 | 269.54 | 13.60 | 138.78 |
| Medium / 8 | 162.87 | 11.62 | 91.87 |
| Medium / 16 | 178.41 | 12.56 | 97.32 |
| Small / 1 | 201.82 | 12.56 | 110.04 |
| Small / 8 | 85.07 | 9.89 | 56.04 |
| Small / 16 | 80.40 | 9.91 | 54.25 |

Resident reference/color/ink preparation took about 1.2–1.9 ms by median. The
dense recognizer stage intervals were 231.89 / 125.95 / 141.79 ms for medium
and 164.43 / 48.69 / 44.51 ms for small at 1/8/16. Stage intervals and host
waits overlap; do not sum them as independent overhead. Nine observations
establish descriptive medians, not p95/p99 or general accuracy. Cached
initialization plus first inference remained seconds, recorded separately.

All 162 timed jobs had zero image-egress calls. Each source had one ordered
crop fingerprint across every pair, capacity, round and pass. The comparison
kept actual detector/crop work equal, including geometry. All 18 exact State
closures completed and the plan cache's fresh full-file hashes were unchanged.
The process exited normally with code one for the final strict assertion;
there was no access violation in this run.

**Quality remains unaccepted.** There were 36 failures among 156 strict
comparisons, all on wider dense-source outputs. Every repeated batch-one
output passed. Small N8 retained text/dispositions but changed protected/search
geometry and confidence. Small N16 also changed one candidate disposition
(170 to 169 candidate words). Medium N8/N16 exceeded geometry or confidence
tolerance. The synthetic sources passed within-pair batch agreement. Known-text
comparison gave small zero edits on both; medium had zero on sparse print and
two character edits / one word edit on long lines. This tiny synthetic corpus
is not representative model accuracy. The dense frame has no independent
spatial ground truth. Defaults remain medium/medium and batch one on GPU.

Useful-width measurements distinguish the workloads:

| Source | Recognizer crops | Padded canvas-width fraction |
| --- | ---: | ---: |
| Dense video frame | 136 | 59.19% |
| Sparse print | 3 | 47.49% |
| Long lines | 62 | 4.93% |

These fractions motivate checking graph support for short-crop width profiles;
they do not predict a proportional speedup. Long lines need almost the entire
canvas. Missing content must not be removed to create a faster result.

Raw evidence root:
`%LOCALAPPDATA%/OkuFlow/uct-resident-20261020-175106-94b2d99b-ae60-4887-98ea-dc0012300fdc`.
The precheck-only root is
`uct-resident-20261020-175059-fbfcd87c-fe4a-4330-81c4-32c27657b27a`.
Exit receipts, original build failures and derived analysis remain under
`uct-paused-scrolling-20261020`; no ledger or failed output was edited.

Dense active-row source SHA-256:
`178622f29136cbdce82093df65faf661092e43f0c194233f67dc7d8a951d115e`.
The other sources are first-party printed-text fixtures created with Pillow
11.1.0 and the locally hashed Arial font; their images, truth and provenance
stay in `uct-paused-scrolling-20261020/resident-samples`. No font, model,
private image, SDK or plan was added to Git or a release payload.
