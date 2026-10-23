# Local BF16 recognition selection (2026-10-22)

The separate [public model local-build/golden path](uct_public_bf16.md) is now
authored in private background source, UNBUILT/UNRUN with an empty production
catalog. It neither reuses this schema-1 global plan/UUID approval for public
users nor changes this selector's cache-only behavior. Candidate synthetic crops
require actual strict N1/full-N8 attestation and separate activation.

The separate [role-aware precision experiment contract](uct_precision_artifacts.md)
is offline preparation for ordered BF16 widths and independently approved role
pairs. Owner's 2026-10-23 policy removes driver key/admission pins and admits a
TensorRT runtime at least 11.4.0.106, while retaining the same reviewed schema-1
graph/plan/device. Ordinary FP32 preparation cannot approve BF16. The archived
mixed-FP16 detector graph is rejected as a speed candidate based on negative
archived stage/layer evidence; no repeated build is queued.

The owner approved local use of the measured BF16 recognition variant despite
visible recognition/geometry differences. This is an unreleased, explicit
application opt-in, with the normal pinned FP32 medium detector. Other users
and diagnostic factories retain the existing FP32 graph and FLOAT I/O baseline
(TF32 allowed). There is no saved precision preference or new UI control.

The single selector is `OKUFLOW_UCT_LOCAL_BF16_MANIFEST`, an absolute local path
to a bounded schema-1 manifest with explicit `prepare=false`. Empty/unset keeps
the existing default. Only the Automatic/default application's GPU candidate
reads it, once at construction. Public `CreateTensorRtRecognizer`, including
strict resident/batch/width diagnostics, never reads this selector. Existing
explicit tiers, batch and width settings are preserved; only medium/medium,
N8, fixed1024 without width preparation qualifies for this variant.

The reviewed local manifest is
`Z:/Projects/OkuFlow/local-work-data/uct-quantization-20261022/manifests/bf16-linear-rec-cached.json`.
Its artifact root is the sibling `uct-quantization-20261022` directory. Root
owns enabling the environment and launching the integrated app; implementation
does not persist environment/settings changes or launch the owner's process.

The selection pins:

- FP32 medium detector source/graph:
  `eb13b44b25bb36f89528b68720af8a61d9cf381176107f465db1757b65d086e1`,
  at the selected normal runtime's `det/medium/inference.onnx`.
- Medium recognizer source:
  `9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba`.
- BF16-linear recognition graph:
  `ecd8ffc92aa1b9f7a33b0ad39757b64887c465f15bb54853936b2136f94e5835`,
  under `models/bf16-linear/<source SHA>/<graph SHA>.onnx` in the artifact root.
- Reviewed legacy recognition plan key (read-only fallback):
  `e596b8ac0110f01e88fc94bcbdeda4511b83af90efb761aa4e194de3ea496073`,
  under `engines/bf16-linear/<key>.plan`. The whole file SHA-256 is
  `155c74b5b856467e49853190c881077c9af3200203a196f9e737f7533e6f5495`.
  The existing 32-byte payload digest header is also checked.
- RTX 4090 UUID `230556cb9d98d5c17318d9ac52e6697d`, builder TensorRT
  `11.4.0.106` (CUDA 12.9.86 experiment). Historical driver API identity `13040`
  is provenance only. BF16 requires compute capability major at least 8 and
  the same UUID. Driver 13050/newer uses the same approval and plan bytes.

The versioned `uct-precision-v2` key binds source and
converted graph hashes, explicit precision, device UUID, builder TensorRT,
TF32/default optimization/workspace/aux-stream policy, fixed-width profile,
network role and batch. It omits driver and loaded runtime identity. For the
reviewed plan its stable key is
`6c5a22298560858aa3bb9539fe40646aa89d3eedda3ca66838210639027e8d01`.
`PlanPath` names that stable location; `ExistingPlanPath` uses it when present,
otherwise only the exact legacy filename above. Both require the same whole-plan
hash. A corrupt/unreadable present stable file fails without trying legacy.
No copying, aliasing, rekeying, cache deletion or overwrite occurs.

Runtime admission compares the four-part numeric tuple against 11.4.0.106 inside
compiled major ABI11; 12.0.0.0 fails this boundary.
The actual loaded runtime version/hash and driver remain provenance. Newer
runtimes may try verified older plans, but this is not a serialization guarantee:
[TensorRT's default plans require their build version; explicit build-time
version compatibility enables later runtimes](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/engine-compatibility.html).
The compiled ABI11/DLL path, factory, deserialization and exact engine profile/I/O
remain authoritative. No ABI12 discovery or compatibility build flags are added.
The minimum-version and ordinary FP32 cache wiring in `uct_tensorrt.cpp` are
owned by the batch lead and pending integration in this branch.

The private policy allows at most 64 KiB manifest JSON and strictly less than
256 MiB for each graph/plan file. It rejects invalid role/precision/source/hash,
relative/UNC paths, escaping or missing canonical artifacts, changed graph/plan
bytes, unsupported schema and preparation requests. `receiptRoot` from the
offline manifest is ignored: production never creates receipts, converts a
graph, loads a builder for this request, creates cache directories or writes a
plan. Both GPU networks and any FP32 fallback are cache-only while the selector
is nonempty. An unavailable FP32 cache follows ordinary CPU/Windows fallback;
it does not authorize a cold build.

Missing, tampered or incompatible policy/artifacts select the existing pinned
FP32 GPU path before inference and name the reason in `BackendStatus`. A
deserialized variant must have FLOAT input/output and the exact single
min-1/opt-8/max-8 by 3 by 48 by 1024 profile. Typed artifact/profile refusals,
including pre-inference null deserialization with no reported CUDA error, may
choose FP32 inside the same preparation worker. Keep `Check(cudaPeekAtLastError())`
before classifying a null refusal. CUDA, runtime API,
allocation, context creation and genuine inference faults follow normal
retirement/quarantine and Automatic failover, with no BF16 inference retry.
There remains one GPU candidate and one preparation worker; the existing
three-candidate order and Windows preparation bridge index 2 are unchanged.
Supplemental requests still cannot retire/promote or start the CPU bridge.
Resident inputs, retirement hooks and image-egress boundaries are unchanged.

Ready status describes the **BF16 recognition graph with mixed
BF16/FP16/FP32 arithmetic and FLOAT I/O**, never pure BF16. The detector remains
FP32 graph/I/O with TF32 allowed. FP32 fallback names its actual precision and
the refused local selection.

The original contended four-scene experiment measured 19–30% lower complete
worker latency for BF16 recognition. Book text differed by 22/383 relative
candidate edits (5.74%); the maximum common-word edge change was 19.97 pixels.
The original 5%/10-pixel comparison verdict failures remain failures. The
owner's authorization does not establish ground-truth accuracy, 120 FPS, RTX
3050 behavior or a general hardware default. Inspector arithmetic was mixed.
FP8 differed by about 7–21%, INT8 was nearly entirely wrong, and INT4 was flat
or slower; none is imported into production policy.

The three model-free `PaddleTests::localBf16*` slots are registered in
`uct_paddle_processing` and cover schema/roles/paths/hashes/bounds, the reviewed
stable key and its precision/device/profile separation, driver-independent
approval, runtime minimum/malformed-version refusal, legacy lookup precedence,
explicit selector restoration,
missing/tampered artifacts and read-only refusal. CPU-only checks cannot
compile the enabled TensorRT branch or establish actual BF16 execution. Root
owns the separate integrated cache-only GPU proof, combined gate and launch.
The earlier implementation's fresh CPU-only target build and tracked
`uct_paddle_processing` gate passed in 1.12 seconds with zero skipped rows,
skips or unresolved results. This was one scoped development test, not the
full release gate; enabled TensorRT compilation, GPU/native UI and real-camera
execution were not run by the implementation worker.
The 2026-10-23 policy revision has no build/test execution pending root START;
earlier passes do not validate this diff.
Private optional SDK/model distribution remains governed by
[existing notices](THIRD_PARTY_LICENSES.md#optional-experimental-paddleocr-component-2026-10-18);
no new third-party implementation or dependency is added.

The private production factory accepts an optional retirement observer so the
explicit active-GPU diagnostic can exercise the same app selection and verify
its exact TensorRT state closure. Ordinary application calls omit that observer.
`OKUFLOW_UCT_ACTIVE_PRODUCTION_FACTORY=1` additionally requires the reviewed
local BF16 manifest and asserts actual BF16 status; it retains both baseline
and variant cache inventories. This diagnostic still measures scheduler calls,
not controller admission or native publication latency.


### Integrated cache-only GPU proof (2026-10-22)

At root `852086b`, the enabled CUDA/PCH-on three-target build succeeded. The
explicit production-factory active diagnostic then passed on the actual
RTX 4090 using the reviewed BF16 manifest and inference-only TensorRT SDK.
Actual status named the BF16-linear recognition graph with mixed arithmetic
and the FP32 detector. Full/region/full worker times were407.8408/47.9183/
141.1936 ms for the two owned book acquisitions, with a480x288 source ROI.
One exact TensorRT state closure completed; both baseline and BF16 cache
inventories were unchanged and image egress was zero. These are descriptive
worker measurements, not a BF16 speed comparison or native latency proof.

Raw receipt: `uct-geometry-r3-20261021/evidence/runs/
bf16-production-active-book-1-receipt.json` under the workstation work-data root.
This fixture executes the scheduler, not regional text admission. The pending
704-pixel resolution change is not part of this proof. Combined current-head
CPU/CUDA/shared-source and native application acceptance remain separate.

Muse Wonder Full reviewed the frozen selector at `83a34c7` read-only after
Opus encountered a provider rate limit and found no defensible BF16 defect.
The review does not cover the later diagnostic observer seam or constitute
execution/release acceptance. Its full response is retained in
`uct-geometry-r3-20261021/evidence/muse-r15-bf16-review.json`.
