# Native formula backend (27 October 2026, development)

`uct_formula.hpp` adds a worker-confined C++ TensorRT encoder/autoregressive
decoder, separate from OCR. It replaces the experiment's Python inference loop.
The owner's precision is FP32 graphs with TF32 permitted on Ampere or newer.
There is no BF16, CPU, Python or subprocess fallback.

`scripts/uct/formula-packs.json` and compiled `uct_formula_catalog.hpp` pin four
first-party prepared source pairs, original configurations/cards, upstream
revisions, initial states and tensor profiles. These are partitioned experiment
graphs, not raw Paddle weights. Exact lengths/SHA-256 precede parsing. Optional model-data release URLs and source notices are pinned alongside these hashes. No model enters Git or the standard application bundle.

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
Normal application close first revokes UCT/Math callbacks while Qt and the
windows still exist, then completes app-owned persistence and child shutdown.
The final Windows process boundary avoids CRT/CUDA module unload competing
with detached TensorRT destruction; it does not catch an inference crash or
convert an error status to success. Windows reclaims any retained allocations.
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

Math selection and **Install and set up** now share the OCR setup transaction.
Four model choices default to plus-L; TF32 is fixed. Enabling Math in this draft
selection changes no active worker until all requested OCR/Math models pass
execution compatibility and the atomic settings save succeeds. Cancellation,
stale configuration, a damaged source or a failed engine build retain old models.
Startup prepares only already verified local Math sources; it never downloads.

Pause/Freeze a source or open a picture/PDF, zoom to one complete formula with a
small margin, and choose **Read formula in this view** under UCT advanced settings.
This explicit immutable-view path uses UCT's existing worker and owned corrected
analysis image. It waits for ordinary OCR instead of replacing queued OCR. GPU
preparation reads the exact current validation surface, not its temporal blend;
only bounds/colors and final tokens return to the CPU. Stacked fractions and
exponents retain a 2D input layout instead of ordinary line rectification.

The bounded MicroTeX adapter produces a native vector path on that same worker.
No inference or typesetting runs in UI paint. Original pixels supply the ink and
background descriptor. UCT's readable-color/palette mapping, scene/view mapping,
chrome clipping and capture snapshot painter render the formula. Rotation,
stabilization and keystone happen upstream once; pan/zoom reprojects the retained
scene artifact without another recognition. Source, selected frame or model
changes cancel Math. Resume clears it; no unverified formula follows live video.
A separate selectable LaTeX pane exposes the prediction; no fake OCR words,
painted-word counts, source-ink sketch text or semantic MathML claim are added.
Show original, presentation failures and source revocation obey UCT visibility.
Clear formula restores ordinary OCR without changing its retained proof/model.

This is one selected formula at a time, not automatic document formula detection.
Recognition can be wrong; compare with the original. Existing tensor timings below
predate image preparation and the integrated renderer; fresh integration evidence
is reported separately. Native desktop acceptance remains separate.

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

Integrated validation (27 October 2026) additionally passed the native source
transaction, setup, ordinary CPU-image input and resident current-validation input
on three original chalk equation crops. The model recovered `\frac{1}{2}` and
`v^{2}` without manually correcting its lowercase `k`. The deliberately blank EMA
half in the resident fixture cannot supply the result. Source colors, 2D source
bounds, layout, shared scheduler and snapshot rendering all run natively.

A sparse whole-formula crop needs histogram separation before estimating its ink
colors: ordinary word boxes are already tightly detected. The new bounded Math
input sampling shares UCT color-policy primitives, uses at most 65,536 samples,
and keeps the crop-edge refusal. CPU/device bounds/colors match; normalized FLOAT
samples agree within 1e-4, with output-tail guards intact. No ordinary OCR admission
threshold was relaxed. This fix followed a preserved real chalk input refusal.
The archived tightly cropped complex formula remains covered by the earlier
supplied-tensor diagnostic; it is not claimed as a successful selected-image run.

The final integration background matrix passed release compilation, CPU suite
and CUDA delta; GUI excluded. PCH-OFF model/settings/UCT/layout/equivalence passed
6/343. Read-only translation review covered all 288 new targets (twelve fixes,
zero unread), and translation parity passed 24 catalogs of 934 entries. Native
desktop/installer acceptance is not established by these background results.

Final persistent-worker RTX4090 sample, after the tracked gates stopped: chalk-kinetic 119.07 ms, chalk-work 83.77 ms, chalk-delta 63.33 ms. Medians of seven after two warmups, plus an initial run. These worker timings include current-image preparation, TF32 inference and native vector layout; they exclude source capture, queue wait, UI paint and model setup. The Windows GPU was shared (44% utilization before the run), so this is not an isolated speed claim or a measured delta against old UCT. The model retained lowercase `k`; no prediction was manually corrected. Warm verified-source/cache setup in this transaction took 13.17 seconds.
