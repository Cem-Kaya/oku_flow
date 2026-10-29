# Native formula backend (27 October 2026, development)

`uct_formula.hpp` adds a worker-confined C++ TensorRT encoder/autoregressive
decoder, separate from OCR. It replaces the experiment's Python inference loop.
The owner's precision is FP32 graphs with TF32 permitted on Ampere or newer.
There is no BF16, CPU, Python or subprocess fallback.

`scripts/uct/formula-packs.json` and compiled `uct_formula_catalog.hpp` pin four
first-party prepared source pairs, original configurations/cards, upstream
revisions, initial states and tensor profiles. These are partitioned experiment
graphs, not raw Paddle weights. Exact lengths/SHA-256 precede parsing. Public
downloads remain disabled; no graph URLs are invented. No model enters Git/bundles.

`Prepare` is worker-only. It reuses OCR's exact DLL loader/version policy, builds
locally with TF32, optimization 2, 512-MiB workspace and no auxiliary streams.
Cache identity binds sources/configuration/profiles, recipe
`okuflow-formula-tf32-partitions-v1`, model, GPU UUID and actual TensorRT version.
Formula-only bounds are 512-MiB plan files, 1-GiB context per partition, 64-MiB
individual and 512-MiB total explicit buffers. Ordinary OCR limits stay unchanged.
Cache roots must be absolute ordinary local directories without redirected
ancestors. A damaged existing plan is refused and preserved. Complete plans commit
atomically; a cached plan alone is not compatibility or activation.

`Recognize` accepts a supplied preprocessed FLOAT 1x1x768x768 tensor. Encoder
features and recurrent caches stay on device. Only tiny controls and final token
IDs return to the CPU. Two output banks, bounded shapes/tokens, iteration-boundary
cancellation/deadlines and native ByteLevel decoding keep the loop bounded.
Unknown completion quarantines only Math resources. Disposal is asynchronous,
without UI/shutdown joins. Confirmed cancellation/token exhaustion allow reuse.
The result is predicted LaTeX; it executes no TeX and draws nothing.

`uct_formula_contract` is desktop/model-free: catalog/profile bounds, cache
separation and decoding, including fractions, unknown IDs and malformed UTF-8.
`optionalNativeFormulaSetup` in `uct_paddle_tests` is separate opt-in GPU work:
`OKUFLOW_UCT_FORMULA_TEST=1`, absolute `OKUFLOW_UCT_FORMULA_SOURCES` (plus-L's
four pinned files), `OKUFLOW_UCT_FORMULA_REFERENCE` (archived inputs/tokens),
fresh absolute `OKUFLOW_UCT_FORMULA_EVIDENCE`, isolated absolute
`OKUFLOW_UCT_FORMULA_CACHE`. NPY reading is test-only; no NumPy app dependency.

The first RTX4090 run passed all four cases, cross-worker handoff and
cancellation/token-bound reuse. Tokens matched the reference; agreement is
diagnostic, never an activation gate. Cold verification/build/loading took
45.8 seconds. First-call times are not steady-state latency. The kinetic output
retains the model's lowercase `k`, without manual correction.

Still unfinished: public model-pack downloads, Math settings/dropdown, resident
image preprocessing, scheduler/selection integration, native formula layout and
source-colour rendering. This backend does not change UCT output or make the
ordinary OCR setup button install Math.

The final warm diagnostic passed, including cancellation during complex decode,
expired deadlines and successful reuse. Median of seven measured runs after two
warmups: kinetic 71.36 ms (29 iterations), work 40.82 ms (12), delta 30.53 ms (8),
complex 374.50 ms (198). This covers supplied-tensor H2D, encoder, decoder/control
synchronization, final IDs and native decoding, excluding image preparation and
drawing. RTX4090, nonexclusive Windows session; these are not app end-to-end or
production-camera timings. Verified-source/cache loading took 8.96 seconds warm.

Tracked background matrix: release compile PASS; CPU 266 passes (four contain
skipped Qt rows), nine skips; CUDA delta 16 passes, five skips; zero unresolved.
GUI was explicitly excluded, so this is partial validation, not release/native
desktop acceptance. PCH-OFF formula/processing/equivalence checks passed 3/342.
The initial MSVC single-literal size failure is preserved; the compiled catalog
generator now chunks literals. An earlier equivalence run invalidated by a source
edit during execution is also preserved; the fresh frozen-source matrix passed.
