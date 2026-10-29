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

Formula/Math native integration and its downloadable partition packs remain
separate unfinished work. This button currently installs ordinary OCR. Requested
Math precision is TF32, which uses FP32 weights and TensorRT computation policy.

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
