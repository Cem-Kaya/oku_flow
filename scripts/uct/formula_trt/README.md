# Native Windows formula TensorRT experiment

This developer backend recognizes a cropped formula and returns token IDs. It is
separate from the shipping UCT worker, source-ink contours, text detector and UI.
No Math mode or automatic formula inference is enabled by installing these tools.

The measured native Windows implementation uses the pinned plus-S, plus-M, L and
plus-L models in `models.json`. Never put weights, exported graphs, plans, Python
environments or evidence in Git, a release bundle, or a synced output directory.
Model cards declare Apache-2.0; the runtime is not a redistribution acceptance.

Use native Windows Python 3.10, TensorRT 11.4.0.106 (CUDA 12.9), the matching SDK
Windows wheel, `requirements-runtime.txt`, and a compatible NVIDIA driver. Pass
`--sdk` and `--cuda-bin` explicitly; only the current process DLL search path
changes. There is no WSL path or implicit CPU/Paddle/ORT inference fallback.

Preparation needs a separate environment with `requirements-preparation.txt`.
Export needs a *third*, ABI-matched environment: Paddle2ONNX 2.1.0 and official
CPU Paddle `3.0.0.dev20250426`, plus packaging 25.0. The released Paddle 3.0.0 and
3.2.2 Windows binaries are not interchangeable with that exporter wheel. Keep
the exact downloaded wheels, notices and hashes in private evidence. The
converter uses opset 18 even when 17 is requested.

Example PowerShell sequence (`$work` and `$assets` are private absolute folders):

```powershell
# Download the exact model revisions/files listed in models.json, then verify.
& $prepPython -B scripts/uct/formula_trt/model_pins.py PP-FormulaNet_plus-S --model-root "$assets/models"

# Capture exact prepared arrays and token IDs with the independent native
# Paddle 3.0.0 GPU / PaddleOCR 3.0.3 / PaddleX 3.0.3 reference environment.
# assets/inputs.json contains entries {name,path}; images live in assets/inputs.
& $paddlePython -B scripts/uct/formula_trt/capture_reference.py PP-FormulaNet_plus-S --assets $assets --work-dir $work

& $exporter --model_dir "$assets/models/PP-FormulaNet_plus-S" --model_filename inference.json --params_filename inference.pdiparams --save_file "$work/onnx/PP-FormulaNet_plus-S.onnx" --opset_version 17 --optimize_tool None
& $prepPython -B scripts/uct/formula_trt/rewrite_mask.py "$work/onnx/PP-FormulaNet_plus-S.onnx" "$work/onnx/PP-FormulaNet_plus-S.trt-mask.onnx"
& $prepPython -B scripts/uct/formula_trt/partition_formula.py PP-FormulaNet_plus-S --work-dir $work --model-root "$assets/models"

foreach ($part in @('encoder','decoder')) {
  & $trtPython -B scripts/uct/formula_trt/build_engines.py PP-FormulaNet_plus-S $part --work-dir $work --sdk $sdk --cuda-bin $cudaBin
  if ($LASTEXITCODE -ne 0) { throw 'TensorRT build failed' }
}
& $prepPython -B scripts/uct/formula_trt/artifacts.py "$work/partition-v2/PP-FormulaNet_plus-S"
& $trtPython -B scripts/uct/formula_trt/benchmark.py PP-FormulaNet_plus-S --work-dir $work --sdk $sdk --cuda-bin $cudaBin --output "$work/small-result.json"
```

Run other models serially, never concurrently on the GPU. Preparation is a CPU
oracle only. It requires the `chalk-kinetic-normalized` fixture to verify exact
original tokens before engines can be sealed. Benchmark defaults also exercise
`chalk-work-normalized`, `chalk-delta-normalized` and `official-complex`; pass
`--case` for one supplied fixture. Inputs are already-preprocessed float32 NCHW
`.npy` arrays and exact native reference token arrays. No expected equation or
symbol is inserted by the runtime.

The partition is intentionally limited to these exact pinned exports and batch
one. It preserves the graph's stop condition and uses configured finite profile
bounds; an exceeded bound is an error, never a successful truncated formula.
Other branches, integer masks or exports are refused. Do not use `python -O`:
prototype graph/runtime assertions are part of this opt-in experiment's contract.
New prepared partitions, plans, seals and result receipts preserve prior outputs;
use a fresh `--partition` name or work directory. Keep failed attempts.

`build_engines.py` uses strongly typed FP32 with TF32 disabled. The projected
image features, cross-attention projections and self-attention caches remain on
the GPU; cache states use separate ping-pong buffers. Only the bounded stop flag
and final token IDs return to the host in the four tested plans. Optional host
shape-control bridges are capped to 256 bytes; large host state is refused.
Allocation/execute/completion faults quarantine owned resources until process
exit. There is no retry into a fallback backend.

Warm timing includes the prepared image upload, encoder, all decoder steps,
control completion waits and final tokens download. It excludes image decoding,
normalization/preprocessing, LaTeX decoding, formula detection, OCR composition
and display. It is not a live-video latency or sensor-to-photon measurement.
Backend token equality is distinct from recognition accuracy: small-model W and
source capital K remain recognition errors even when every backend agrees.

CPU-only contracts, no GPU/model/download/GUI:

```powershell
& $prepPython -B -m unittest discover -s scripts/uct/formula_trt -p test_contracts.py -v
```

See [the measured contract](../../../docs/formula_tensorrt_experiment.md). The
complete Windows application matrix and native desktop acceptance remain separate
from this developer-only Python experiment.
