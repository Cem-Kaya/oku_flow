# Native Windows formula TensorRT experiment

The opt-in developer backend in `scripts/uct/formula_trt/` runs a cropped formula
through two real TensorRT engines. It adds no application setting, Math mode,
detector, UI string, model download, release payload or automatic inference.
Shipping UCT glyph/ink proof, scheduling, reprojection and accessibility stay
unchanged. LaTeX tokens lack the per-symbol source geometry needed for direct
replacement in the magnified view.

## Measured result (2026-10-27)

Native Windows 11, RTX 4090, driver 621.01, TensorRT 11.4.0.106 / CUDA 12.9,
strongly typed FP32, TF32 off, batch one. Three warmups and twelve timed calls
per input; owner applications remained running. All 192 measured calls matched
the native Paddle reference token arrays exactly.

| Model | Kinetic equation | Work equation | Delta equation | Complex printed formula |
|---|---:|---:|---:|---:|
| PP-FormulaNet_plus-S | 16.10 ms | 8.09 ms | 6.52 ms | 94.02 ms |
| PP-FormulaNet_plus-M | 86.89 ms | 42.14 ms | 31.00 ms | 493.26 ms |
| PP-FormulaNet-L | 130.06 ms | 72.78 ms | 56.07 ms | 655.35 ms |
| PP-FormulaNet_plus-L | 121.69 ms | 70.05 ms | 58.18 ms | 728.52 ms |

These wall-clock model calls include prepared NCHW image upload, encoder,
autoregressive decoder, synchronized control feedback and final tokens download.
They exclude decoding, normalization, standard preprocessing, LaTeX detokenization,
detection, composition and display. They are not pure kernel, whole-page OCR or
live-camera latency. The earlier native Paddle model-call equation medians were
55.89/297.71/423.34/405.32 ms respectively (about 3.3â€“3.5 times these timings).
Those earlier-run comparisons are not interleaved ABBA or exclusive-GPU results.
No FP16/BF16 formula measurement or general accuracy result is claimed.

The kinetic crop was normalized by a monotonic grayscale percentile stretch and
inversion before standard model preprocessing, without thresholding or symbol
repair. All four models return `k=\frac{1}{2}m v^{2}`: stacked fraction and raised
2 are recognized, but the source capital K becomes lowercase k. The small model
still misreads W in another sample. Exact backend agreement is not semantic
correctness, and this sixteen-input corpus is not a new accuracy gate.

## Implementation

The pinned export's ONNX Loop carries tensors whose lengths grow. Parsing the
whole graph was insufficient: TensorRT rejected recurrence shape changes. The
experiment extracts a fixed image encoder and a one-iteration decoder, with a
bounded host stop loop. Large neural computations run exclusively in TensorRT.

- Rewrite an integer `a & ~b` only after proving initial binary state, binary
  operand and unchanged feedback. Use exact Boolean masking; refuse other masks.
- Lower known cross-attention cache branches into invariant image projections
  computed once by the encoder, with shape/transpose work left in the decoder.
  Pure scalar controls become Or/Where, avoiding TensorRT conditional-scope errors.
- Compute state liveness to a fixed point. Respect shadowing of outer initial
  constants by recurrent body inputs; do not treat changing state as invariant.
- Evaluate only input-independent initial state/shape constants on the CPU during
  preparation. The separate ORT CPU oracle verifies the original token sequence;
  it is never a measured runtime fallback.
- Keep image features and recurrent caches on GPU. Use capped, separate output
  banks rather than aliasing input/output KV. The four tested engine inventories
  have device-resident inputs and outputs; return only a stop byte per iteration
  and final token IDs. Generic tiny host-control bridging is capped separately.
- Retain graph stop criteria and finite configured profiles. Overrun is failure,
  never a successful truncated answer. An uncertain GPU failure quarantines owned
  resources until process exit; no unsafe reuse or fallback occurs.
- Bind model source pins, exact ONNX exports, rewrite receipts, partitions, state,
  engine hashes, profiles and compiler receipts before benchmark execution.

Preparation is limited to the exact four reviewed exports, batch one. It is a
prototype rather than an arbitrary ONNX Loop compiler. Use ordinary Python,
never `python -O`, since opt-in assertions enforce its known graph contract.

## Reproduction and evidence

See [commands, dependencies and bounds](../scripts/uct/formula_trt/README.md).
Developer tools and models stay outside the checkout. Four models and twenty
upstream files are pinned by revision and hash in its `models.json`; the model
cards declare Apache-2.0. Export uses the exact ABI required by Paddle2ONNX 2.1.0:
CPU Paddle `3.0.0.dev20250426`, not released 3.0.0 or 3.2.2.

Local evidence is under
`Z:/Projects/OkuFlow/local-work-data/formula-tensorrt-20261027/`:
`comparison.json`, `report.md`, four `*-final-fp32.json` files, compiler receipts,
content seals, references, failed attempts and complete logs. Earlier native
Paddle receipts remain in the sibling
`uct-math-gpu-benchmark-windows-20261027/` folder. WSL results are not used here.

The tracked plus-S preparation reproduced byte-identical encoder/decoder ONNX
and initial-state files, and the final tracked runtime executed all four plans.
Eight CPU-only mask/branch/content-binding contracts pass. `agent_check.ps1`
configured the fresh CPU tree, then returned FULL-required (exit 2) because
there is no built Ninja dependency inventory; it did not run a SCOPED gate. No full application
matrix, native GUI or public gallery capture is established by this Python-only
experiment, and no public payload is staged.

Primary documentation: [Paddle formula recognition](https://www.paddleocr.ai/main/en/version3.x/module_usage/formula_recognition.html),
[Paddle2ONNX Windows release](https://github.com/PaddlePaddle/Paddle2ONNX/releases/tag/v2.1.0),
[exporter ABI pin](https://github.com/PaddlePaddle/Paddle2ONNX/blob/v2.1.0/pyproject.toml),
[TensorRT loops](https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/work-with-loops.html).
