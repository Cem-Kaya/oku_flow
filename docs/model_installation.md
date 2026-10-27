# Local OCR model setup (development, 27 October 2026)

Choose models under **UCT advanced settings**, then **Install and set up**.
Selections remain drafts until setup succeeds; current OCR keeps running.
Unrelated settings saves cannot activate drafts. The button does not enable UCT.

The background worker checks bundled/cached source hashes, downloads missing
ONNX/configuration/model cards from pinned HTTPS revisions, verifies exact lengths
and SHA-256, then publishes complete revision directories. No downloaded code or
script runs. Interrupted downloads stay temporary. Damaged previously owned
packages are preserved under `damaged-*` before repair; unknown or redirected
write directories are refused. Downloads time out after 30 seconds of inactivity
or 15 minutes per file.

The app prepares selected native TensorRT engines and the CPU fallback, then
runs bounded real blank-image inference to check execution and tensor/dictionary
compatibility. No accuracy or text-agreement gate is added. GPU preparation keeps
its ten-minute cooperative deadline; vendor calls cannot all be preempted.
Readiness, cancellation, configuration generation and settings-save checks precede
activation. Tested objects transfer into UCT without restarting preparation.
Failure/cancellation leaves the previous running recognizer. Complete verified
source downloads can remain cached after preparation fails; that is not activation.
No startup network request is added.

Fresh CPU/GPU defaults and bundles use medium detector/recognizer only. Existing
explicit choices survive; tiny/small remain optional downloads. The compiled
catalog matches `scripts/uct/download-models.json` and reviewed source pins.
New bundles ship two original FP32 medium graphs. The native recipe
`okuflow-native-bf16-conv-matmul-v1` casts linear operands and convolution
weights/bias to BF16 inside TensorRT, restoring FLOAT around surrounding
operations. Shape/nonlinear operations and FLOAT interfaces stay unchanged.
Source digest, recipe, device, TensorRT version, profile and precision bind cache
identity. BF16 refusal retains same-tier FP32; CUDA faults keep ordinary failover.
CPU stays FP32. No Python or external converter runs on the user's machine.

Sources live in `%LOCALAPPDATA%/OkuFlow/model-cache/sources/ocr-ROLE-TIER-REVISION/`;
native engines in `%LOCALAPPDATA%/OkuFlow/model-cache/engines/`. Source/ready
receipts are private diagnostics; app settings own activation. Runtime DLL roots
remain separate. Historical converted graphs/converter remain development provenance.

The optional **Math (experimental)** draft and **Math model (TF32)** dropdown use
the same button. Plus-L is the default; plus-S, plus-M and L remain optional. The
compiled catalog pins native encoder/decoder graphs, original configuration/card,
license and modification notice. The app builds hardware-specific TF32 engines;
raw Paddle weights and developer Python converters are not executed by the app.
All requested models must be ready before either OCR or Math selection commits.
Math source caches use `math-MODEL-REVISION` directories alongside OCR sources;
Math engines use `%LOCALAPPDATA%/OkuFlow/model-cache/math-engines/` with separate
recipe identities. Cached sources alone grant no activation authority. See
[native Math integration](uct_native_formula.md) for selected-view rendering.

`uct_model_installation_contract` checks corruption, preparation failure,
cancellation, cache reuse/repair, unknown-directory preservation and serialization
with private synthetic transport. The offscreen UCT contract checks button state.
The real `optionalNativePrecisionSetup` built both medium BF16 engines on RTX4090,
ran inference and confirmed exact state retirement in 117.1 seconds cold. This
is setup time, not ongoing OCR latency. `optionalInstalledModelSetup` additionally
tests the transaction and ready-object handoff; it requires
`OKUFLOW_UCT_INSTALL_SETUP_TEST=1`, fresh absolute
`OKUFLOW_UCT_INSTALL_SETUP_EVIDENCE` and explicit absolute
`OKUFLOW_UCT_INSTALL_SETUP_CACHE`. Normal fixtures never download/build cold
engines. Native desktop and full release acceptance remain separate.

The repaired tracked background matrix passed release compilation, CPU and CUDA
delta checks with GUI explicitly excluded: CPU 265 passes (four contain skipped
Qt rows), nine skipped tests; CUDA delta 16 passes, five skipped, zero unresolved.
Scoped PCH-OFF model/settings/UCT/equivalence checks passed 5/341, plus the rebuilt
actual-app model-draft/persistence case 1/341. These are background/scoped results.
Real pinned tiny-detector download verification passed (1,797,537 bytes), and the
real medium GPU/CPU setup plus second-worker handoff passed in 7.0 seconds warm.
Source transport and inference/handoff are distinct evidence, not a native
end-to-end button or installer acceptance claim. Initial failures are retained.

The explicit source transport diagnostic is `optionalPinnedSourceDownload` in
`uct_model_installation_tests`, selected by `OKUFLOW_UCT_DOWNLOAD_TEST=1` and a
fresh absolute `OKUFLOW_UCT_DOWNLOAD_EVIDENCE`. It downloads one pinned role and
verifies bytes only; it does not perform model inference or change app settings.

The optional Math source transaction was exercised against its actual public
732,481,638-byte plus-L package, with all six pinned files verified and native TF32
preparation successful. Setup and real CPU/device image-to-overlay validation are
separate receipts: the first setup passed before a sparse-ink preprocessing
refusal, which was preserved and repaired. Later warm runs reuse those verified
sources; they do not claim another download or native UI-button acceptance.

Final persistent-worker RTX4090 sample, after the tracked gates stopped: chalk-kinetic 119.07 ms, chalk-work 83.77 ms, chalk-delta 63.33 ms. Medians of seven after two warmups, plus an initial run. These worker timings include current-image preparation, TF32 inference and native vector layout; they exclude source capture, queue wait, UI paint and model setup. The Windows GPU was shared (44% utilization before the run), so this is not an isolated speed claim or a measured delta against old UCT. The model retained lowercase `k`; no prediction was manually corrected. Warm verified-source/cache setup in this transaction took 13.17 seconds.
