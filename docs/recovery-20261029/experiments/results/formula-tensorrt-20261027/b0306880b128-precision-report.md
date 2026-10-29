# BF16, TF32 and source-image formula overlays

Native Windows, RTX 4090, TensorRT 11.4.0.106 / CUDA 12.9. Three warmups and twelve timed completed calls per case. No CPU/Paddle/ORT execution fallback. Owner applications remained running.

| Model | FP32 equation | TF32 equation | BF16 equation |
|---|---:|---:|---:|
| PP-FormulaNet_plus-S | 16.10 ms | 11.36 ms | 12.04 ms |
| PP-FormulaNet_plus-M | 86.89 ms | 53.30 ms | 64.55 ms |
| PP-FormulaNet-L | 130.06 ms | 78.10 ms | 88.55 ms |
| PP-FormulaNet_plus-L | 121.69 ms | 77.58 ms | 86.69 ms |

BF16 here is explicit reduced convolution/matrix arithmetic with FP32 normalization, Softmax, residual/control operations and interfaces. TF32 uses the original FP32 graph with Tensor Core permission; it is not a claim that every layer uses TF32. All TF32 timed tokens match the FP32/Paddle reference. BF16 matches except the small model’s work formula, where `cos` is expressed as an operator name; it still misreads source W as N. That output is longer and is not an equal-token-length performance comparison.

Further optimization replays the fixed encoder CUDA graph and caches separate decoder bindings for two output banks. Alternating baseline/optimized/optimized/baseline visits (24 timed samples per mode per case) gave:

| plus-S equation | Standard runtime | Tuned runtime | Reduction |
|---|---:|---:|---:|
| tf32 | 12.42 ms | 10.84 ms | 12.7% |
| bf16 | 12.93 ms | 10.82 ms | 16.3% |

The tuned TF32/BF16 equation medians are effectively tied. Benefits vary with output length and model. The medium-model dispatch candidate regressed on the equation/complex sample, and wider BF16 plus optimization level 5 also regressed; neither becomes a default. All candidate receipts and earlier failures remain beside this report. The older FP32 reference was measured in a prior run, so it is not a counterbalanced precision comparison.

The actual plus-M BF16 tokens are decoded to LaTeX and typeset by pinned MathJax into vector paths. Black plates and white/yellow glyphs occupy only supplied formula regions. The source-image and prediction hashes are checked; output RGB outside those regions is byte-identical. Original files are untouched. The source capital K is still recognized as lowercase k and is rendered faithfully. Regions come from the benchmark crop manifest, not an automatic formula detector.

The renderer exports reusable SVGs and lossless full-image PNGs. `overlay-bf16/comparison.html` shows original/white/yellow output and full-board context. Cold module setup and PNG export are development artifact costs; they are not native app painting latency. Vector cache lookup time alone is not rendering latency. No native app integration, live-frame authority, UIA math reading, GUI acceptance or gallery publication is claimed.

Validation: 13 CPU contracts and three Node renderer contracts pass; actual native GPU runs cover the four model/precision pairs and source changes in cached execution. New Python/Node tools do not change shipping C++ behavior. The earlier tracked agent check returned FULL-required in this fresh unbuilt inventory; no full application matrix is claimed.
