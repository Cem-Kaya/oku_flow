# BF16 app preference: implementation and functional evidence

Normal app startup prefers BF16 independently for the selected GPU detector and
recognizer tiers. There is no human-text, FP32-agreement, golden-corpus or
accuracy-approval requirement. Supported artifact/build/compatibility refusals
use the same selected tier in FP32 after confirmed completion. Real CUDA/API/
context/inference faults and unknown completion retain normal retirement and
CPU failover. Tiny recognition remains N1; small/medium default to N8. CPU
precision, saved tiers and the one-worker/two-slot pipeline are unchanged.
There is no automatic live/Freeze model switch.

## Tested implementation

Production code: `a954cc26f9a7050d6826a6443b6f7c39e85b0d94`, following the
cache/resource repair `8f04e4f`. The latter's first native run exposed an
incorrect detector application of the recognizer's 256 MiB context ceiling.
That failed run is preserved. The correction keeps the recognizer ceiling,
records both roles' actual declared context bytes, and preserves the existing
detector profile/resource policy.

New cache files use Qt's atomic non-replacing temporary-file rename. The previous
external rename silently failed because Qt retained its native handle. Cold
serialized bytes and warm digest-prefixed files now share one consistent bound.
BF16 ABI/profile/class/context checks precede execution-context allocation.
Saving failure is visible in the technical receipt and does not discard a
healthy engine. Existing files remain untouched.

On the development workstation, the following private native checks passed:

| Check | Actual result |
| --- | --- |
| Medium detector / medium recognizer | BF16 / BF16, N8 |
| Small detector / small recognizer | BF16 / BF16, N8 |
| Tiny detector / tiny recognizer | BF16 / BF16, N1 |
| Repeat all three with inference-only SDK | All six engines loaded from cache; parser and builder resources absent |
| Missing medium BF16 recognizer graph | BF16 detector retained; selected medium N8 FP32 recognizer loaded |
| Warm cache inventory | All ten files' sizes and SHA256 unchanged: four FP32 baselines and six BF16 plans |

No corpus directory or expected-text comparison was used. The corrected
preparation block saved five new BF16 files and reused the already saved medium
recognizer. Both runtime controllers and their children exited zero, with
confirmed retirement and no remaining owned job members. These are functional
startup/cache/fallback checks, not accuracy, whole-worker performance, paint
latency or cross-GPU measurements.

The rebuilt real CUDA/TensorRT adapter passed the four scoped contracts:
`uct_paddle_processing`, `uct_precision_artifact_contract`,
`uct_public_bf16_contract`, and mandatory `shared_source_equivalence`, with no
skips/unresolved results. The cache fixture checks actual persistence, exact
digest/payload bytes, existing-file preservation and boundary/failure cases.
The actual application and native main-window fixture also compiled, followed
by a passing shared-source guard. Six hash-verified BF16 graphs were staged
beside this private development executable under `models/uct`.

The build controllers retained **exit 125**, despite successful child builds and
tracked cases, because an owned job member remained at child exit. Its identity
is unresolved. Exact-job cleanup subsequently reached zero active members and
verified runner release. These statuses are preserved and are not full release
acceptance. No native GUI tests were run during the owner's reserved desktop.

Sol's R4 source review closed the serialized/context-bound finding; focused R5
review found no P1/P2 after the detector-limit correction. Reviews do not replace
runtime evidence. Earlier compilation/runtime failures and the original strict
golden failures remain recorded separately.

## Evidence and remaining delivery work

Workstation-only evidence root:
`Z:/Projects/OkuFlow/local-work-data/uct-bf16-app-default-20261023/`:

- `APP_PRECISION_FUNCTIONAL_RESULT.json` binds the actual role/tier receipts.
- `runtime-cache-cold-a954/C2-20261023-204020-76d515326c364ae9aaf14123f6d9235a/`
  retains corrected preparation evidence.
- `runtime-cache-warm-a954/C2-20261023-204359-640282205028451b8a555e71976af587/`
  retains warm loads and independent FP32 fallback;
  `runtime-cache-warm-a954/CACHE_REUSE_VERIFIED.json` binds unchanged cache files.
- `cache-resource-build-r2/` and `app-build-current/` retain the tracked scoped
  cases, source/object/executable identities and original controller statuses.
- `PRIVATE_APP_MODEL_STAGE.json` binds the private executable and six graph files.
- `sol-cache-review-r4/` and `sol-cache-review-r5/` retain independent reviews.
- `runtime-defaults-40f/METADATA_CORRECTION.json` corrects the old child summary's
  erroneous `planPreparation=false`; its original bytes remain unchanged.

The private app build was not launched and the owner's running app, settings,
model cache and installed bundle were not replaced. App-relative model/runtime
discovery is implemented, but a complete installer payload, runtime dependency/
license/SBOM inventory, clean-machine installation and full release matrix remain
separate delivery work. A corpus is not required for that normal BF16 payload.
The historical [public-v2 experiment](uct_public_bf16.md) remains diagnostic only.
