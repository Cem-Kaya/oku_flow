# Formula recognition GPU benchmark

RTX 4090, WSL2 Ubuntu22.04, Paddle GPU3.0.0 / PaddleOCR3.0.3 / PaddleX3.0.3, FP32, batch1, no TRT/HPI

Measured on the owner's RTX 4090. Owner apps remained running; GPU execution was not exclusive. These are descriptive WSL2 Paddle measurements, not native Windows/TensorRT or OkuFlow frame latency. No model or feature was added to the application.

Three warm-up calls per repeated input, followed by 12 timed batch-one calls. Inputs are resident CPU arrays. Every call waits for actual GPU completion. The model-call interval includes input/output transfers and autoregressive decoder execution; the end-to-end interval also includes normal PaddleOCR preprocessing and LaTeX decoding. Disk input, precomputed contrast normalization, formula detection, rendering and screen presentation are excluded. nvidia-smi sampling and evidence writes occur outside the timed intervals.

## Warm end-to-end time

| Model | Handwritten K=1/2 mv² median / p95 | Complex printed formula median / p95 | Predictor creation |
|---|---:|---:|---:|
| PP-FormulaNet_plus-S | 67.9 / 80.9 ms | 350.9 / 383.8 ms | 7.47 s |
| PP-FormulaNet_plus-M | 325.0 / 364.1 ms | 2024.3 / 2203.3 ms | 9.99 s |
| PP-FormulaNet-L | 431.5 / 516.9 ms | 2491.8 / 2749.8 ms | 9.63 s |
| PP-FormulaNet_plus-L | 431.6 / 510.0 ms | 2397.3 / 2965.7 ms | 8.92 s |

Predictor creation is one observation, with already downloaded files and uncontrolled OS caches. It excludes Python imports, CUDA context setup performed by the GPU preflight, and first inference. It is not a cold machine-start benchmark.

## Recognition and other inputs

### PP-FormulaNet_plus-S

- chalk-kinetic-normalized: end-to-end median 67.93 ms, p95 80.95 ms; model-call median 58.90 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 39.71 ms, p95 43.35 ms; model-call median 30.18 ms. Exact output(s): ["N=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 33.06 ms, p95 43.80 ms; model-call median 24.12 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 350.86 ms, p95 383.78 ms; model-call median 339.22 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\breve{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 0.948 s.

### PP-FormulaNet_plus-M

- chalk-kinetic-normalized: end-to-end median 325.04 ms, p95 364.14 ms; model-call median 314.69 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 144.70 ms, p95 165.94 ms; model-call median 135.18 ms. Exact output(s): ["W=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 108.37 ms, p95 118.78 ms; model-call median 99.55 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 2024.32 ms, p95 2203.31 ms; model-call median 2011.50 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\breve{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 1.107 s.

### PP-FormulaNet-L

- chalk-kinetic-normalized: end-to-end median 431.45 ms, p95 516.92 ms; model-call median 400.02 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 224.22 ms, p95 254.32 ms; model-call median 189.90 ms. Exact output(s): ["W=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 176.59 ms, p95 191.01 ms; model-call median 144.56 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 2491.76 ms, p95 2749.85 ms; model-call median 2451.48 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\check{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 13.435 s.

### PP-FormulaNet_plus-L

- chalk-kinetic-normalized: end-to-end median 431.62 ms, p95 509.96 ms; model-call median 400.27 ms. Exact output(s): ["k=\\frac{1}{2}m v^{2}"]
- chalk-work-normalized: end-to-end median 226.38 ms, p95 267.00 ms; model-call median 190.38 ms. Exact output(s): ["W=F d\\cos\\theta"]
- chalk-delta-normalized: end-to-end median 168.43 ms, p95 208.12 ms; model-call median 134.91 ms. Exact output(s): ["W=\\Delta K"]
- official-complex: end-to-end median 2397.33 ms, p95 2965.67 ms; model-call median 2356.16 ms. Exact output(s): ["\\zeta_{0}(\\nu)=-\\frac{\\nu\\varrho^{-2\\nu}}{\\pi}\\int_{\\mu}^{\\infty}d\\omega\\int_{C_{+}}d z\\frac{2z^{2}}{(z^{2}+\\omega^{2})^{\\nu+1}}\\breve{\\Psi}(\\omega;z)e^{i\\epsilon z}\\quad,"]
- Largest recorded untreated-board call: 1.968 s.

## Source and preprocessing

The three handwritten formulas are crops of the existing generated-board.png, source SHA-256 2a92f74c9fb85b277230af21ae963a149b210fdb6525f4bebdbfd63c2abe3d5c. Untreated crops were retained and tested. The normalized versions use one general rule for all three: grayscale, a monotonic linear 1st-to-99th-percentile contrast stretch, then polarity inversion to dark ink on light. No thresholding, stroke reconstruction, inferred characters or formula-specific correction was used. Normalization was precomputed and its runtime is not included in the table. The fourth input is Paddle's official complex-formula example. Exact crop bounds, percentiles and image hashes are in inputs.json.

The first small-model pilot failed before inference because PaddleX3.0.3's unconstrained LangChain dependency resolved to an incompatible major version. Compatible versions were pinned only in the private venv. The original failure, dependency repair, pip-check and frozen versions are retained. The S and L measurements include 12 repetitions of raw dark crops as well. After observing repeated degenerate decoding on raw inputs, the M and plus-L runs test untreated crops once and repeat only the same four useful comparison inputs. No measured result or failure was discarded.

## Limits and provenance

Four inputs do not establish general mathematical recognition accuracy. In particular, preserve any lowercase-k or other transcription errors in the raw outputs. These are image-to-LaTeX models, not per-symbol geometric alignment. No live-video, native desktop, formula region detector, app scheduler, BF16, FP16 or TensorRT benchmark was run.

The four official PaddlePaddle model repositories declare Apache-2.0 in their model cards. All exact revisions, file hashes and source URLs are in models.json. Downloads, runtime and results are private only; they do not authorize application redistribution or establish bundle license acceptance.

Sources: [PaddleOCR formula module](https://www.paddleocr.ai/main/en/version3.x/module_usage/formula_recognition.html), [plus-S](https://huggingface.co/PaddlePaddle/PP-FormulaNet_plus-S), [plus-M](https://huggingface.co/PaddlePaddle/PP-FormulaNet_plus-M), [L](https://huggingface.co/PaddlePaddle/PP-FormulaNet-L), [plus-L](https://huggingface.co/PaddlePaddle/PP-FormulaNet_plus-L).
