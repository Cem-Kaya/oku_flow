# Native Windows formula TensorRT benchmark

TensorRT-only prototype completed on RTX 4090, driver 621.01, TensorRT 11.4.0.106 / CUDA 12.9, FP32 with TF32 disabled. Batch one; three warmups and twelve timed completed calls per input.

| Model | Kinetic equation | Work equation | Delta equation | Complex printed formula |
|---|---:|---:|---:|---:|
| PP-FormulaNet_plus-S | 16.10 ms | 8.09 ms | 6.52 ms | 94.02 ms |
| PP-FormulaNet_plus-M | 86.89 ms | 42.14 ms | 31.00 ms | 493.26 ms |
| PP-FormulaNet-L | 130.06 ms | 72.78 ms | 56.07 ms | 655.35 ms |
| PP-FormulaNet_plus-L | 121.69 ms | 70.06 ms | 58.18 ms | 728.52 ms |

All 192 timed calls and 48 warmups produced the same token arrays as their native Paddle reference. All four models read the normalized kinetic crop as `k=\frac{1}{2}m v^{2}`: the fraction and superscript are recovered, while the source capital K is still recognized as lowercase k. Exact backend agreement is not proof of correct recognition; the small model still misreads W in the work equation.

Each call starts with an already-prepared NCHW array, uploads it, runs the TensorRT encoder and decoder, synchronizes bounded control feedback, and returns final tokens. The timing excludes normalization, standard image preprocessing, LaTeX decoding, detection, drawing, UI and disk I/O. No Paddle/ORT fallback runs in the measured backend. The image features and recurrent caches stay on the GPU; only a one-byte stop condition per decoder iteration and final token IDs return to the host.

Prior native Paddle model-call medians are in comparison.json. The equation speedups versus those earlier medians are PP-FormulaNet_plus-S: 3.47x, PP-FormulaNet_plus-M: 3.43x, PP-FormulaNet-L: 3.25x, PP-FormulaNet_plus-L: 3.33x. These are earlier-run comparisons, not interleaved ABBA or exclusive-GPU measurements; owner applications were left running.

Implementation and evidence:

- Tracked source: `Z:/Projects/OkuFlow/local-work-data/worktrees/formula-tensorrt-20261027/scripts/uct/formula_trt/`.
- Four final `*-final-fp32.json` receipts and logs in this folder.
- `partition-v2/MODEL/runtime-seal.json` binds model metadata, original states, ONNX graphs, engine files and compiler receipts.
- Independent source preparation of plus-S produced byte-identical encoder, decoder and initial-state files.
- Eight CPU-only contracts pass; full application matrix and GUI were not invoked for this developer-only Python backend.
- Original converter, whole-loop, conditional, scalar-rank and preparation failures are retained in their earlier logs; none are counted as passing TensorRT runs.

This does not add a Math mode to OkuFlow, replace UCT, stage these models into a release or alter app settings. Native GUI and website promotional captures remain separate work.

The tracked agent_check invocation returned exit 2 (FULL required in a fresh, unbuilt Ninja inventory), not a SCOPED test pass. Application matrix acceptance remains unclaimed.
