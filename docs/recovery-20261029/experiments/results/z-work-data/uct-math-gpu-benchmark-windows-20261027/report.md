# Formula recognition GPU benchmark

Native Windows11 RTX 4090, Paddle GPU3.0.0 / PaddleOCR3.0.3 / PaddleX3.0.3, FP32, batch1, no TRT/HPI; no WSL

Measured on the owner's RTX 4090. Owner apps remained running; GPU execution was not exclusive. These are native Windows Paddle GPU measurements, not TensorRT or integrated OkuFlow frame latency. No model or feature was added to the application.

Three warm-up calls per repeated input, followed by 12 timed batch-one calls. Inputs are resident CPU arrays. Every call waits for actual GPU completion. The model-call interval includes input/output transfers and autoregressive decoder execution; the end-to-end interval also includes normal PaddleOCR preprocessing and LaTeX decoding. Disk input, precomputed contrast normalization, formula detection, rendering and screen presentation are excluded. nvidia-smi sampling and evidence writes occur outside the timed intervals.

## Warm end-to-end time

| Model | Handwritten K=1/2 mv² median / p95 | Complex printed formula median / p95 | Predictor creation |
|---|---:|---:|---:|
| PP-FormulaNet_plus-S | 67.4 / 74.2 ms | 313.2 / 370.9 ms | 8.07 s |
| PP-FormulaNet_plus-M | 307.0 / 340.1 ms | 1923.2 / 2036.1 ms | 10.71 s |
| PP-FormulaNet-L | 457.1 / 517.0 ms | 2586.1 / 3078.3 ms | 7.12 s |
| PP-FormulaNet_plus-L | 443.4 / 519.3 ms | 2655.2 / 2871.0 ms | 7.38 s |

Predictor creation is one observation, with already downloaded files and uncontrolled OS caches. It excludes Python imports, CUDA context setup performed by the GPU preflight, and first inference. It is not a cold machine-start benchmark.

## Recognition and other inputs

### PP-FormulaNet_plus-S

- chalk-kinetic-normalized: end-to-end median 67.38 ms, p95 74.17 ms; model-call median 55.89 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 37.84 ms, p95 46.09 ms; model-call median 27.09 ms. Exact output(s): ["N=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 33.53 ms, p95 35.79 ms; model-call median 23.77 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 313.19 ms, p95 370.90 ms; model-call median 301.27 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\breve{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 0.567 s.

### PP-FormulaNet_plus-M

- chalk-kinetic-normalized: end-to-end median 307.04 ms, p95 340.08 ms; model-call median 297.71 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 149.87 ms, p95 164.11 ms; model-call median 138.57 ms. Exact output(s): ["W=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 109.92 ms, p95 125.98 ms; model-call median 100.21 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 1923.24 ms, p95 2036.09 ms; model-call median 1910.07 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\breve{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 0.945 s.

### PP-FormulaNet-L

- chalk-kinetic-normalized: end-to-end median 457.08 ms, p95 517.02 ms; model-call median 423.34 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 237.56 ms, p95 276.03 ms; model-call median 198.73 ms. Exact output(s): ["W=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 186.25 ms, p95 208.54 ms; model-call median 151.66 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 2586.13 ms, p95 3078.34 ms; model-call median 2539.76 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\check{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 9.886 s.

### PP-FormulaNet_plus-L

- chalk-kinetic-normalized: end-to-end median 443.36 ms, p95 519.35 ms; model-call median 405.32 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 242.30 ms, p95 271.76 ms; model-call median 200.81 ms. Exact output(s): ["W=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 178.48 ms, p95 196.87 ms; model-call median 142.44 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 2655.18 ms, p95 2871.03 ms; model-call median 2609.01 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\breve{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 1.685 s.

## Source and preprocessing

The three handwritten formulas are crops of the existing generated-board.png, source SHA-256 2a92f74c9fb85b277230af21ae963a149b210fdb6525f4bebdbfd63c2abe3d5c. Untreated crops were retained and tested. The normalized versions use one general rule for all three: grayscale, a monotonic linear 1st-to-99th-percentile contrast stretch, then polarity inversion to dark ink on light. No thresholding, stroke reconstruction, inferred characters or formula-specific correction was used. Normalization was precomputed and its runtime is not included in the table. The fourth input is Paddle's official complex-formula example. Exact crop bounds, percentiles and image hashes are in inputs.json.

This native rerun uses the same pinned upstream model bytes and same precomputed input bytes as the earlier WSL experiment. Compatible Python dependency versions were pinned in a private Windows venv, separately from all owner app environments. Untreated dark crops are diagnostic-only single calls. All four normalized/printed comparison inputs have three warmups and twelve timed calls per model. Raw results, package versions, native process GPU DLL paths, scripts and hashes are retained.

## Limits and provenance

Four inputs do not establish general mathematical recognition accuracy. In particular, preserve any lowercase-k or other transcription errors in the raw outputs. These are image-to-LaTeX models, not per-symbol geometric alignment. No live-video, native UI, formula region detector, app scheduler, BF16, FP16 or TensorRT benchmark was run. No WSL/Linux process executes these native Windows measurements. The previous WSL report remains a separately labeled experiment and must not be substituted for these timings. Different background activity prevents attributing timing differences to the operating system alone.

The four official PaddlePaddle model repositories declare Apache-2.0 in their model cards. All exact revisions, file hashes and source URLs are in models.json. Downloads, runtime and results are private only; they do not authorize application redistribution or establish bundle license acceptance.

Sources: [PaddleOCR formula module](https://www.paddleocr.ai/main/en/version3.x/module_usage/formula_recognition.html), [plus-S](https://huggingface.co/PaddlePaddle/PP-FormulaNet_plus-S), [plus-M](https://huggingface.co/PaddlePaddle/PP-FormulaNet_plus-M), [L](https://huggingface.co/PaddlePaddle/PP-FormulaNet-L), [plus-L](https://huggingface.co/PaddlePaddle/PP-FormulaNet_plus-L).
