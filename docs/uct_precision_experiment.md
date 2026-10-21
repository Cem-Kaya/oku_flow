# Opt-in UCT precision experiment

Status: isolated diagnostic implementation, 2026-10-22. Offline graph checks
and private compilation are distinct from pending model/engine measurements.
There is no production precision selector, model replacement, width change,
CPU behavior change or release enablement.

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
Ada supports FP8; native NVFP4 requires Blackwell. Actual device identity and
engine support must still be checked in the granted serial GPU session.

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

Phase B needs its own serial grant. The four CPU-extracted calibration frames
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
