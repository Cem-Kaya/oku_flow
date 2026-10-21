# Opt-in UCT precision experiment

Status: isolated diagnostic and finite measurements completed, 2026-10-22.
The owner selected BF16 recognition with FP32 detection for separate local
integration. This experimental checkout changes no production selection,
model, width, CPU behavior or release enablement; original quality verdicts
remain recorded below.

## Verified baseline

The six actual local `inference.onnx` files match `scripts/uct/pins.json`.
Every floating initializer/tensor attribute uses ONNX `FLOAT` (FP32). All six
graphs have FP32 input and output. Small/medium recognizers additionally have
39 INT64 shape tensors (51 elements); these are not quantized neural weights.

| Pinned graph | FP32 tensor count | FP32 elements |
| --- | ---: | ---: |
| Tiny detector | 169 | 428,420 |
| Small detector | 169 | 2,453,368 |
| Medium detector | 226 | 15,486,640 |
| Tiny recognizer | 120 | 1,104,524 |
| Small recognizer | 202 | 5,267,681 |
| Medium recognizer | 215 | 19,115,212 |

`uct_tensorrt.cpp` creates a strongly typed TensorRT 11.4.0.106 network with
`createNetworkV2(0)`, workspace 256 MiB, optimization level two and zero
auxiliary streams. It requires `kFLOAT` I/O. There is no FP16/BF16/INT8 builder
switch. It does **not** clear TF32, which TensorRT allows by default. Thus
FP32 graph/storage/interfaces are established, but full-mantissa FP32
products or a particular accumulation tactic do not follow from the I/O.
The baseline cache tag's `fp32` is a graph identity, not a layer profiler.

FP32 uses 32 bits with eight exponent and 23 stored fraction bits. FP16 uses
16 bits with five exponent and ten fraction bits, reducing range as well as
precision. BF16 uses 16 bits with eight exponent and seven fraction bits,
retaining FP32's exponent range with coarser precision. TF32 retains FP32
tensor storage but permits reduced-precision products on Tensor Cores.
INT8 uses signed eight-bit integer values plus scales; it is not a floating
format. FP8 E4M3 uses eight-bit floating values plus quantization scales.
INT4 weight-only stores four-bit GEMM weights and computes at a higher
precision; it is neither activation INT4 nor NVFP4.

TensorRT 11 requires explicit graph types/Cast or Q/DQ rather than legacy
precision flags. See NVIDIA's [migration guide](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/api/migration/tensorrt-10x-to-11x.html),
[precision control](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/precision-control.html),
[explicit quantization](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/quantized-types-explicit-quantization.html)
and [quantization schemes](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/quantized-types-schemes.html).
The measured device was an NVIDIA GeForce RTX 4090, SM 8.9, 24 GiB, CUDA
driver API 13040/runtime 12090. Ada supports FP8; native NVFP4 requires
Blackwell. Native NVFP4/MXFP8 hardware experiments were not attempted on Ada.

## Isolation and artifacts

The private workstation root is
`Z:/Projects/OkuFlow/local-work-data/uct-quantization-20261022`. Never put
models, engines, calibration tensors or source frames in Git or a bundle.
`scripts/uct/precision_models.py audit` records graph/weight types, shapes,
operators, versions and exact hashes without CUDA or inference.

Explicit `convert --precision fp16-linear|bf16-linear --roles det rec`
converts only Conv/MatMul/Gemm islands. Inputs/outputs and sensitive remaining
operations stay FP32 through explicit casts. Lower-precision constant operands
are stored in their actual lower type. BF16 uses the official ONNX version
converter to opset 22, where Conv supports BF16. FP16 overflow refuses
conversion rather than silently clipping. `--precision int4-woq --roles rec`
creates symmetric, block-64, round-to-nearest weight-only constant MatMul
weights. Dynamic attention operands and incompatible shapes stay FP32.
Every attempted conversion has an immutable source-hash directory, failure
receipt or converted-hash graph and manifest. There is no unchanged retry.

`TensorRtPrecisionExperiment` and `CreateTensorRtPrecisionExperiment` are
private diagnostic APIs, never used by the application's automatic backend.
Manifests require an explicit mode, both original and derived hashes, explicit
precision, absolute artifact/receipt roots and explicit graph paths. The
runtime rechecks hashes and ties original identities to compiled model pins.
Experiments require medium/medium, fixed1024 and N8; a failed configuration
cannot silently become another batch, model, precision or CPU backend.

Baseline plans are read from the unchanged baseline cache/key and cannot be
prepared or replaced. Converted plans use `engines/<precision>/<key>.plan`
under the private root. Their separate versioned key binds original and
converted graph hashes, precision strategy, GPU UUID, driver, TensorRT,
profile, batch and builder policy. A missing explicitly selected plan gets
at most one build attempt. Existing invalid plans and failed attempt markers
refuse replacement. Preparation never performs image inference.

Every network load retains its plan/full-file/payload hashes, exact graph,
one-time build/load cost, context memory and raw `IEngineInspector` JSON.
New engines retain detailed inspection metadata. Existing baseline plans may
have only names; missing precision/tactic information remains unknown and
never authorizes a baseline rebuild. NVTX is disabled in all experiment
contexts to avoid asymmetric detailed-annotation overhead during measurement.

## Finite serial workflow

`scripts/run_uct_precision.ps1 -Mode Fixtures` prepares manifests only. Choose
explicit configuration tokens such as `baseline`, `fp16-linear-rec`,
`fp16-linear-both`, `bf16-linear-rec`, `int4-woq-rec`, `fp8-qdq-rec` or
`int8-qdq-rec`. Baseline has no preparation manifest. External PowerShell
invocations accept comma-separated configuration tokens.

After a coordinated runner grant, `-Mode Prepare` selects exactly one model
pair, explicit full SDK and fresh output directory. `-Mode Measure` requires
the inference-only SDK, existing plans and fresh evidence. The tracked helper
acquires the same `runner.lock` as `run_test_gate.ps1`, refuses other active
gates and releases the lease only after its owned child exits. It never
terminates an owner's process. Explicit workload bounds are one round, one
startup, one warmup per source/configuration and one to three timed passes,
at most four images and five configurations per invocation.
An explicit owner override authorized `-AllowContended` for this sequence.
It retained the lease plus per-configuration before/after `nvidia-smi`
utilization, memory, power and process snapshots. No idle-GPU inference follows
from an absent tracked root gate. These results are **contended**, not unloaded
or isolated timing.

Phase A prepares FP16 detector+N8 recognizer, BF16 N8 recognizer and INT4
weight-only N8 recognizer: four unique missing engine keys, no N1 preparation.
Full-path measurements include baseline, FP16 recognizer-only, BF16
recognizer-only, INT4 recognizer-only and FP16 both. Frozen measurements omit
FP16 both: frozen crop work must use the identical pinned FP32 detector.
Selected and real-neighbour sets each contain menu19, menu25, book and atlas.
Three timed passes produce 216 timed reads in 18 blocks (306 jobs including
startup/warmup). Preparation is separate; no cold-build loops are timed.

The optional `optionalTensorRtPrecision` Qt slot reuses the production resident
`SubmitDevice` worker, profile validation, exact input metering and exact State
retirement. It preserves source file/active-row/GPU-tree fingerprints, ordered
crop hashes and zero Readback/ReadValidation/ReadProbe calls. Full-path timings
include detection, recognition, resident result preparation and scheduler
work. Frozen timings prove zero timed detector calls and identical ordered
crop work. Detector and recognizer CUDA intervals are separately retained.
Source upload and analysis copies finish before measured submission.

Memory receipts distinguish plan/context bytes, approximate device-wide free
VRAM after jobs and sampled peak process working set. None is falsely called
exclusive peak GPU allocation. Existing strict N1/N8 assertions remain
unchanged and their historical failures remain failures. N8-only comparison
does not establish that strict gate or native UI/screen-reader acceptance.

## Calibrated lower precision

Phase B requires its own grant (received for this completed sequence). The four CPU-extracted calibration frames
come from the owned menu recording at 10/32 seconds and atlas recording at
10/60 seconds; source hashes, exact requested timestamps, decoder settings and
frame hashes are retained. The evaluation frames at menu19/25 seconds,
book0/42 ms and atlas0/50 ms remain held out. The book original was not found:
calibration coverage is limited to menu/atlas, not comprehensive OCR data.

Baseline frozen calibration export creates CPU reference tensors using the
original CPU source pixels, compact detector quads and production
`PaddleNormalize`/`PaddleRectify`. It never downloads a GPU image. The cap is
eight detector and sixteen recognition tensors. `precision_qdq.py` uses only
CPUExecutionProvider, two CPU threads and max-absolute symmetric calibration;
INT8/FP8 reuse the same exact source/calibration-hash-keyed ranges. It refuses
evaluation-frame identity overlap. Calibration is still model work and must
not overlap the parent's reserved GPU/test session.
`run_uct_precision.ps1 -Mode Quantize` owns the same runner lease for each
explicit recognizer conversion and records script/input hashes and normal exit.
Export binds randomized compact plans to exact retained source leases, never
their vector position; a model-free test covers reorder/ambiguity refusal.

Explicit INT8/FP8 Q/DQ islands preserve FP32 interfaces and unconverted
operations. Conservative FP8 channel/group constraints leave incompatible
layers FP32 and list every exclusion. No compatible quantizable operations,
missing/nonfinite ranges or unsupported hardware are refusals. Native NVFP4
is not attempted on Ada. A build failure is reported exactly; a low-bit model
file is not proof of low-bit engine execution or a performance gain.

## Evidence and validation

`precision_report.py <run>... --output <fresh-directory>` reads raw receipts,
requires completed exact retirement, unchanged caches, source fingerprints,
N8/fixed1024, finite sample coverage and zero image egress. Frozen comparisons
also require identical ordered crops and zero timed detector calls. It emits
HTML/JSON with every changed/dropped/added word, candidate edit denominators,
reading-order edits, dispositions, confidence and common-occurrence geometry.
The owner's five-percent word-difference allowance and ten-source-pixel
geometry allowance are separate descriptive policies, never ground-truth
accuracy. Confidence changes alone are permitted. Unmatched words and original
strict failures remain visible. No RTX3050, 120FPS or sensor-latency claim
follows from measurements on this Ada machine.

Private Python tools need ONNX 1.20.1, NumPy 2.4.4 and their retained notices;
CPU QDQ calibration additionally needs a private ONNX Runtime Python package.
No Python/package/runtime dependency is added to the application. Run
`python -B scripts/uct/test_precision_models.py -v` in that private environment.
The model-free `precisionExperimentManifestAndCacheIdentity` Qt slot is part
of `uct_paddle_processing`; ordinary CUDA ownership/algorithm coverage remains
in `uct_paddle_cuda`. Coordinate all gates and compiler load with the parent.
The full shipping/CPU/CUDA matrix remains a separate integration requirement.

`precision_overlay.py --report <report.json> --output <fresh-directory>`
renders the BF16 book comparison over unchanged source pixels. Green means
every returned BF16 candidate, including possible misreads; red means an
omitted/suppressed baseline word and amber a changed reading. Reading-order
edits are checked against spatial occurrences before calling a word omitted.
Click a word to inspect its source region. `annotations.json` preserves the
exact comparison and raw receipt paths. Run the model-free standard-library
suite with `python -B scripts/uct/test_precision_reports.py -v`.

## Completed finite results (contended RTX 4090)

Each cell below is the median of three timed complete cached-worker jobs in
milliseconds. Startup and one explicit warmup per source/configuration are
excluded. FP16/BF16/INT4 use their phase-A baseline; FP8/INT8 use a fresh
phase-B baseline. Those baselines differ, so cross-phase format rankings are
not controlled comparisons. Source uploads/analysis copies finish before
submission; these are not display FPS or end-to-end sensor latency.

| Selected frame | A baseline | FP16 rec | BF16 rec | INT4 WoQ rec | FP16 both | B baseline | FP8 rec | INT8 rec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| menu19 | 81.77 | 79.35 | 63.71 | 79.98 | 97.31 | 86.74 | 58.33 | 82.17 |
| menu25 | 82.39 | 79.84 | 67.11 | 85.58 | 95.65 | 85.16 | 62.05 | 75.09 |
| book | 118.95 | 108.68 | 94.42 | 118.24 | 123.73 | 130.70 | 113.22 | 66.36 |
| atlas | 209.00 | 199.40 | 147.35 | 217.62 | 223.14 | 220.94 | 151.29 | 191.85 |

| Real neighbour | A baseline | FP16 rec | BF16 rec | INT4 WoQ rec | FP16 both | B baseline | FP8 rec | INT8 rec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| menu19033ms | 75.66 | 67.89 | 56.04 | 74.72 | 78.65 | 72.22 | 53.69 | 61.76 |
| menu25033ms | 91.99 | 86.49 | 66.31 | 84.58 | 102.47 | 89.70 | 65.96 | 72.02 |
| book42ms | 120.74 | 123.72 | 95.66 | 119.16 | 113.34 | 127.17 | 118.71 | 59.39 |
| atlas50ms | 224.31 | 215.65 | 145.31 | 215.41 | 234.77 | 210.16 | 139.07 | 180.39 |

BF16-rec complete-worker latency decreased 18.5–35.2% across these eight
frames (18.5–29.5% for selected frames). With identical frozen baseline
crops and no timed detector calls, its complete-worker changes ranged from
2.7% slower to 32.0% faster; the menu25 frozen case was slower. Thus these
receipts do not establish a universal recognition-only speedup. FP16 rec
ranged from 2.5% slower to 10.3% faster on the full path; FP16 both was slower
on seven frames. INT4 WoQ ranged from 4.1% slower to 8.1% faster, with worse
text agreement. None is an unloaded/isolated benchmark.

Candidate reading-order word edits versus the same-run baseline were stable
across the three passes:

| Format | Selected menu19/menu25/book/atlas | Neighbour menu19/menu25/book/atlas |
| --- | --- | --- |
| FP16 rec | 1/27, 0/26, 7/383, 0/175 | 0/23, 0/25, 8/382, 0/177 |
| BF16 rec | 1/27, 0/26, 22/383, 0/175 | 0/23, 1/25, 18/382, 1/177 |
| INT4 WoQ rec | 3/27, 2/26, 42/383, 1/175 | 4/23, 0/25, 42/382, 5/177 |
| FP16 both | 1/27, 0/26, 13/383, 2/175 | 0/23, 0/25, 22/382, 0/177 |
| FP8 rec | 2/27, 5/26, 81/383, 16/175 | 4/23, 2/25, 93/382, 16/177 |
| INT8 rec | 27/27, 26/26, 379/383, 172/175 | 23/23, 25/25, 377/382, 173/177 |

These are baseline differences, not ground-truth error rates. BF16 selected
book is 5.74% different and has 19.97-pixel maximum common-word edge drift,
exceeding the original 5% and 10-pixel descriptive allowances. The neighbour
book is 4.71% different but reaches 12.01 pixels. The owner's exploratory
speed-priority acceptance does not erase those verdicts. Exact occurrence
matches, unmatched words, reading-order changes, dispositions and confidence
deltas remain in the full JSON report. A future crop improvement was not
implemented or measured here and cannot be credited with fixing these words.

FP8 full-worker latency decreased 6.6–33.8%, but word differences were
7.4–24.3%; every frame exceeded the original word allowance. INT8 changed
97.7–100% of baseline candidates. Its lower time, especially on the book,
includes much less useful output and less resident postprocessing; it is
**not an equal-quality speedup**. These policies were not promoted.

### Engine execution, storage and one-time preparation

The original cached detector/recognizer inspectors expose names only. Their
arithmetic remains unproved beyond FP32 graph/I/O and default-allowed TF32.
Detailed new inspectors establish BF16 GEMM/convolution tactics and mixed
BF16/FP16/FP32 tensor formats, FP8 `e4m3e4m3` tactics (53 layers), INT8
`i8i8` tactics (63 layers), and nine INT4 constant weights with FP32 activation
interfaces. The INT4 path is weight-only, not four-bit arithmetic. Tensor
occurrence counts are metadata counts, not time-weighted instruction shares.
BF16 uses opset22 versus the original/FP16 opset11, with different fusion and
tactics; the timing difference cannot be attributed solely to bit width.

| Engine | Plan bytes (32-byte header included) | Context device bytes | One-time build/load ms |
| --- | ---: | ---: | ---: |
| Baseline detector (existing) | 65,372,900 | 261,964,800 | no rebuild |
| Baseline recognizer (existing) | 78,941,116 | 172,580,864 | no rebuild |
| FP16 detector | 38,392,716 | 266,572,800 | 167,419.99 |
| FP16 recognizer | 43,332,940 | 165,117,952 | 139,137.25 |
| BF16 recognizer | 40,898,276 | 169,869,312 | 47,586.70 |
| INT4 WoQ recognizer | 64,035,052 | 168,075,776 | 81,187.20 |
| FP8 recognizer | 32,659,516 | 146,628,608 | 32,952.82 |
| INT8 recognizer | 22,613,620 | 132,120,576 | 90,308.13 |

Exactly six missing engine keys were built once. No baseline engine or graph
bytes changed; no N1 engine was prepared. BF16 detector conversion was kept
as an offline artifact but no BF16 detector engine/inference was attempted.
Cached first-result startup across full/frozen blocks was 4.45–4.99 seconds
for baseline, 3.24–3.80 seconds for BF16; other formats and each per-source
warmup remain in raw receipts. Timed invocations' sampled peak host working
sets ranged from 573,108,224 to 925,450,240 bytes. New-engine build processes
peaked at up to 3,204,648,960 host bytes. These values and exact context bytes
are distinct from device-wide free-memory snapshots, which are neither
exclusive ownership nor peak VRAM measurements.

### Bounded FP8/INT8 audit and interpretation

The final audit read existing bytes only: no extra graph variants, calibration,
engine builds or inference. Actual derived graph and engine-receipt hashes
match their selected original model. All 270 FP8 and 355 INT8 Q/DQ nodes have
positive finite FP32 scalar scales and matching signed-zero points. No axis
attribute is present: these are scalar per-tensor scales, so a per-channel
axis mismatch is inapplicable. Every stored quantized weight byte matches
the declared max-absolute rounding/clipping policy, and every activation and
output scale equals its bound cached range divided by 448 (FP8) or 127 (INT8).
Both preserve exact FP32 graph interfaces. Per-tensor weight scales are a
supported [TensorRT scheme](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/quantized-types-schemes.html),
but support does not establish adequate OCR quality.

All 24 exported CPU tensors were rechecked for hash, byte size, finite values
and source PNG identity. Their 16 recognizer samples match the CPU-only range
receipts and original-model/calibration-manifest keyed cache. Four actual
source clip/frame hashes and eight held-out evaluation hashes were rechecked;
there is no frame-identity overlap. Book coverage remains absent. Menu is
covered by the disjoint calibration recording, yet INT8 changes 100% of
baseline candidates on all four menu evaluation frames; selected fragments
are `ee,t,ot,t` and `ti,t,H,i`. Missing book data cannot explain this menu loss.

The valid but aggressive policy quantizes 71 constant linear islands,
including the final 192x18710 classifier before bias/softmax. Its activation,
weight and output scales are respectively 0.2231736630, 0.0039623971 and
0.2597442269. FP8 quantizes 54 compatible islands and leaves this unaligned
classifier FP32. Layer-wide max-absolute scales, repeated output requantization
and the sensitive classifier are plausible causes of loss; no new ablation
was run to isolate one cause. No scale/axis/dtype/source/crop/harness binding
defect was found. This supports classifying the INT8 result as a valid but
poor quantization policy, not an inherent INT8 limit. It does not fully rule
out a backend semantic defect without a separate reference inference, which
was outside the finite sequence.

### Exact evidence, scope and release

Under the private root, `final-report/report.html` and `report.json` contain
all eight measurement invocations, 264 paired comparisons and exact raw
receipts. `final-statistics.json` retains complete stage/startup/warmup,
source/format and activity summaries; `final-readonly-audit.{py,json,log}`
retains the bounded audit and every scale binding. `engine-summary.json`
records exact model/plan/source hashes and inspector paths.
`bf16-book-recognized-overlay/overlay.html` and `annotations.json` retain
all 383 green candidates and 17 changed/omitted/suppressed annotations.
`test-gate-archive/` preserves the tracked gate ledger, JUnit, detailed CTest
logs and per-attempt LastTest logs independently of later parent gate runs.

There were 360 timed reads, 120 explicit warmups and 30 successful startup
inferences in 30 measurement blocks. Calibration added nine successful jobs
in one block. All 31 measurement/calibration and five preparation State
closures completed normally, without coalescing/revocation, width changes
or GPU image egress. Backend-preparing scheduler polls are separately counted
and are not extra image inferences. Fixed1024/N8 and source/crop identities
were checked; recognizer-only frozen work used exact common crops with zero
timed detector calls. Preparation failures were not retried. The
nonexistent-executable preflight refusal is retained in
`int8-command-preflight-failure.json`; the earlier fixture argument refusal
remains in the conversation tool receipt. Both occurred before model/engine
work, and neither was an engine retry.

Validation: 14 private Python graph tests and 14 stdlib report/overlay tests
passed. The tracked scoped `uct_paddle_processing` and `uct_paddle_cuda`
selection passed (2/235 CTest cases, zero skipped rows/unresolved attempts);
the calibration source-binding regression subsequently passed processing
again. This is scoped validation, not full shipping acceptance or the prior
optional strict N1/N8 gate, whose failures were not weakened or reclassified.

Final GPU measurement exited normally and released the runner at
2026-10-21T22:03:29.3083850Z (2026-10-22 01:03:29.308 TRT). The final CPU-only
report suite lease ran 22:06:21.7252053Z–22:06:21.9292478Z and was released;
parent integration gates can proceed. All code commits exclude private model,
engine, recording and evidence bytes.
