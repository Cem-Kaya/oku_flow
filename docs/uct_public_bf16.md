# Public BF16 model and local attestation (offline, 2026-10-23)

Owner approved shipping the converted **model**, with an engine built and checked
on each eligible machine. This follow-up preserves the composed batch/helper/cache
policy ending `7c59a59423a0cefa6e19a78ba81908e16461196c`. Private background
integration is authored, unbuilt and unrun. It stages nothing and supplies no
engine, golden pass, inference, quality or performance result. The private schema-1
owner/dev artifact remains cache-only and separate from public model approval.

## Implemented boundary

`src/common/uct_public_bf16.hpp` has no CUDA/TRT calls, builder, file writes,
desktop interaction, environment selector or downloader. `ModelIdentity` approves
the existing converted graph `ecd8ffc9…`, source `9c09abf0…`, configuration
`991b700f…`, decoded dictionary `f1d7cde7…`, FLOAT DEVICE LINEAR I/O and CTC
probability ABI. Profile min `[1,3,48,1024]`, opt/max `[8,3,48,1024]`, classes
18710, default TF32/opt2/workspace256MiB/aux0 remain bound. Arithmetic is mixed
BF16/FP16/FP32. `ReadModel` rechecks source/config/dictionary and graph bytes.

`CheckEligibility` respects selected tiers/settings: medium/medium N8 fixed1024
without width preparation; actual GPU compute major >=8; runtime ABI11 tuple
>=11.4.0.106. Other settings retain their backend. A global GPU UUID/plan hash is
not a public model approval. Driver and DLL hashes remain provenance.

`ReadCorpus` requires one compiled corpus version/hash pin, exact manifest model/
policy, source/provenance and eight bounded crops. Each is immutable GRAY8,
48 rows, width1–1024, row-major, one byte/pixel, with SHA and exact Unicode text.
IDs are lowercase ASCII basenames. The callback reader must confine them to the
bundled corpus and read at most48*1024+1 bytes. Production uses its normal crop
normalization `(sample/255-.5)*2`, padding to1024 with **normalized zero**, and
CTC path, matching production `rectify`. White pixels would be+1 and are not the
padding value. The versioned preprocessing policy is part of model/corpus/cache
identity; no runtime font or codec exists.

`CheckEngine` checks actual tensor names, one input/output/profile, FLOAT DEVICE
LINEAR and exact dimensions. `CheckGolden` requires confirmed completion and16
observations: eight N1 calls and one full N8 group, covering every crop in both
shapes. Actual T/classes, pixel identity and exact raw decoded text must match.
No trimming, normalization, sorting or case folding is allowed. Missing/duplicate
rows fail. The tested plan/device/builder identity is bound; another plan cannot
reuse a passing result. Failed observations stay in `GoldenResult::Record`.
Unknown completion is an ordinary runtime error, never a healthy-stream refusal.

`BuildIdentity`/`CacheKey` bind approved model/profile/dictionary, actual local GPU,
builder TRT and corpus/policy. `ReadyReceipt` additionally binds actual plan hash
and passing results, checking actual runtime/device admission. Driver and loaded
runtime DLL/version/hash are separate provenance; no exact loaded-runtime equality
gate exists. A receipt is local cache integrity, not a signature or authority to
skip engine checks. Actual factories/deserialization remain authoritative.

`ProductionCorpusCatalog()` is deliberately empty. Eight frozen owned synthetic
crops now exist as a candidate validation corpus, with source/visual review;
they have no real N1/full-N8 runtime attestation or production approval. Public
readiness therefore refuses. A private compiled candidate catalog pins manifest
`26432369ed6831431737072f857620b19c81fc9acd8c21579d50ee7bc0e9ec33`.
No environment value, writable manifest, receipt or candidate OCR supplies its
authority or expected text. Tests manufacture no passing golden records.

## Corpus and quality requirements

Freeze owned pixel bytes, version, manifest hash, acquisition/render provenance
and independently checked Unicode truth. Cover O/0, I/l/1, numbers/signs/decimals,
negation and accents present in the model dictionary; include Turkish dotted/
dotless I when making that language promise. Inspect every actual image: tags
alone do not establish glyph coverage. Never derive truth from candidate outputs
or remove failing cases until the model passes. A build-time render using a
licensed hash-pinned font may be reviewed and frozen; runtime rendering is not
allowed. No corpus production/rendering is performed here.

Startup golden success is narrow functional attestation, not broad OCR accuracy,
detector coverage, multilingual or quality acceptance. Preserve wider held-out
CER/WER, spatial omissions/extras and sensitive occurrences separately. The old
mixed-FP16 detector graph `24e3…` stays rejected as a speed candidate. Approximate
width/small-model arms follow exact host work and require fresh worker/callback
and independent-truth evidence.

## Network/factory integration contract

The following path is implemented in source, preserving the six-step contract.
Every actual execution and production activation remains held:

1. Supply explicit application model/corpus roots and a compiled corpus catalog.
   Graph locator is `<modelAssetRoot>/uct/rec/medium/bf16-linear/inference.onnx`.
   No shipping path may reference an owner's cache/private SDK/manifest. Preserve
   the selected normal detector/original configuration/dictionary. Private
   schema1 requests retain their independent cache-only semantics.
2. Use the existing single preparation worker/CPU bridge. CPU OCR continues
   during initial construction; an already valid FP32 selection remains usable
   until atomic replacement between jobs. Do not mutate a live Network or create
   a second OCR worker. The candidate owns its context/stream/inputs independently
   until adoption or exact retirement. Capture/presentation stays responsive.
3. Verify model/corpus/configuration/hardware first. Prepare at most one missing
   local plan per identity, using the actual loaded builder TRT version. Driver
   changes alone do not trigger preparation. Existing plans are read-only; new
   cache writes are new-only and atomic. Do not enable host-code/plugin trust or
   version compatibility implicitly. No unchanged rebuild-until-pass loop.
4. Validate actual engine I/O/profile, then run the nine bounded golden calls
   through production padding/CTC on that exact serialized plan. Keep actual
   N/T/classes, existing same-shape two-slot limits and serial shape changes.
   After a successful drain call `CheckGolden`; retain failure results and leave
   CPU/FP32 selected on a typed mismatch. Memoize failed identity/plan/corpus
   attempts within the session; require changed inputs or a diagnosed explicit
   retry before repeating the same failure.
5. A null build/deserialize refusal with no reported CUDA error may be typed.
   Check CUDA before classification. Genuine CUDA/factory/allocation/context/
   enqueue/copy/event faults or unknown completion retire/quarantine the exact
   candidate and its golden buffers; never try another engine on uncertain work.
6. Create/write the ready receipt for the tested plan/device/builder/corpus.
   On cached loads verify bytes/model/config/dictionary/profile and actual GPU,
   attempt deserialization under runtime admission, and attest before readiness.
   A writable receipt alone never skips attestation. Commit both engines and
   effective width/capacity/ABI atomically. Preserve cancellation, source proof,
   leases, old-state closure, quarantine, lookahead and cooperative integrals.

## Implemented private runtime and publication

`CreateProductionTensorRtRecognizer` supplies application-relative `models` roots
and the empty production catalog. Ordinary diagnostics remain FP32. The owner's
nonempty schema-1 selector takes precedence and remains wholly cache-only.
`CreatePublicBf16ValidationRecognizer` is a private C++ seam with explicit model,
corpus and isolated cache roots, plus a worker-thread attestation observer. It
uses the compiled **candidate** pin without changing production approval. It
must be invoked by a separately authorized private source diagnostic; no default
test or mutable environment switch invokes it.
Its selected detector and all FP32 fallback plans are **cache-only in the same
explicit isolated cache root**. Missing baseline plans refuse; they never cold
build or access the ordinary cache. Verified baseline plans must already exist
in that private root before a granted runtime attempt. The public deadline starts
before detector construction. Ordinary production/schema-1 cache policy is unchanged.

Both selected networks remain in one unpublished preparation `State`. The
existing CPU bridge continues while the same preparation worker runs. BF16
construction first attaches an empty `Network` to that retained State, then
retains exact model bytes, parser/builder/configuration, serialized plan, context,
input, upload staging, output and metadata through confirmed completion or the
existing async retirement/quarantine. No public refusal destroys a partially
constructed live candidate. Confirmed typed refusals discard it before the
ordinary FP32 path. CUDA/API/context/enqueue/copy/event faults and cancellation
or uncertain completion leave through ordinary retirement, without another
engine attempt. The unchanged two-slot recognition/helper bodies are preserved.

The golden path packs the actual GRAY8 crops into an owned pinned BGRA atlas,
then calls the unchanged `gpu::RectifyBatch` normalization/normalized-zero padding
and `gpu::DecodeBatch` argmax/compaction. It retains raw dictionary text, including
spaces and code points, rather than reconstructing text from split words.
Eight N1 calls precede one distinct full N8 call, with serial confirmed shape
changes. Actual tensor N/T/classes and exact outputs reach `CheckGolden`; the
observer retains mismatching records too. Existing wider N8/N1 diagnostic and
quality failures are unchanged.
Completed observations accumulate in retained State and are reported after each
confirmed call. A later fault/cancel/timeout reports those exact earlier outputs
with `complete=false`, `passed=false` and overall completion `unknown`; this
incomplete record never authorizes publication or a healthy-stream retry.
Complete records additionally retain explicit completion and compute capability.

Public plans are raw serialized bytes, separate from schema-1 and ordinary FP32
cache files. A driver-free parent under `ocr-engines/public-bf16/` binds approved
model/device/corpus; each child binds its actual builder via `CacheKey`.
`recognizer.plan` and `ready.json` are written only after completion and exact
golden success, then exposed together by a new-only same-parent directory
rename of the exact held staging directory. Existing generations are never overwritten/repaired. Cache lookup reads
at most 32 directories, bounded 256-KiB receipts and strictly-below-256-MiB plans.
Receipts bind loaded-plan bytes/device/builder/corpus and provenance, but never
authorize expected text or skip deserialization/profile checks/real reattestation.
Driver changes alone cannot change the locator. Runtime ABI11 tuple admission
remains >=11.4.0.106; real deserialization decides compatibility.
The explicit cache root must already exist. Native directory handles deny write/
delete sharing, reject reparse points below its canonical root **before** any
descendant creation and stay held through reads/publication. A racing final
generation is a typed refusal; the losing attempt does not claim publication.

A bounded 64-entry process-session attempt latch retains failed/in-flight
identity/plan attempts. There is no rebuild/retest-until-pass loop. Builder
progress observes cancellation and a 600-second deadline; golden work has a
120-second deadline and each event poll a 30-second limit, all on preparation
only. Vendor builder calls are cooperatively cancellable, not forcibly preempted;
a stalled vendor call stays on its retained worker. BF16 context memory is capped at
256 MiB, builder workspace 256 MiB, output at the existing N8 256-MiB ceiling,
golden input ~4.5 MiB and atlas 1.5 MiB. These checks do not bound all undocumented
TensorRT/driver internal memory. CPU/UI/presentation never waits for readiness.

## Assets, licensing and remaining work

`scripts/uct/public-bf16-model.json` records graph/source/config/dictionary,
upstream revision/license/model card, archived first-party converter SHA and
planned bundle locator. It is metadata, not a staged payload or corpus approval.
The existing graph is reused without conversion/download. Retain the exact
converter source in the reviewed model-source package before staging.

Planned payload is `app/models/uct/rec/medium/bf16-linear/`: graph, original
configuration/model card, exact Apache license, modification/provenance notice.
Golden pixels/manifest/source/notices belong in versioned
`app/models/uct/golden/<corpus-version>/`. No `.plan`, private approval manifest,
SDK header/tool or owner path belongs there. Local engines/receipts stay private.

Root must separately implement tracked model preparation/bundle/inventory helpers
with exact hashes and new-only staging, then complete release acceptance. No
installer helper is connected here. Opus's redistribution observations remain
provisional: actual TensorRT runtime/parser/builder-resource/CUDA inventory,
licenses/notices/SBOM require review before inclusion. No license clearance or
CUDA12.9/TRT11.4 upgrade follows from the model decision or C1 pass.

## Validation

Phase A C1 on frozen0035 passed affected CPU PCH-on/off builds and exactly three
tracked tests: precision-artifact, Paddle-processing, shared-source equivalence,
with zero skipped/unresolved. It released2026-10-23T12:19:00Z lock-free and with
zero children. Source-to-binary evidence is under workstation work-data:
`uct-artifact-precision-20261023/C1-20261023T121247Z/source-to-binary-receipt.json`.
It does not cover this follow-up. All prior grants expired; P0 is not granted.

`uct_public_bf16_contract` also authors actual candidate-byte negative checks,
raw text preservation, failure records, latching, atomic new-only storage and
pending-input/model/staging/output quarantine cases. Storage fixtures are labelled
non-model bytes, never fake passed receipts. All additions are **UNBUILT/UNRUN**.
The one independent source review and subsequent unretested repairs are recorded
in [review dispositions](uct_public_bf16_review.md). Native new-only rename,
junction refusal and directory sharing still require actual fixture execution.

Proposed finite root START C2: one frozen checkout, maximum 8 compile jobs,
900-second total stop budget; build only affected `uct_paddle_tests` and
`shared_source_equivalence` inputs in CPU PCH-on/off configurations, then exactly
`uct_public_bf16_contract`, `uct_precision_artifact_contract`,
`uct_paddle_processing`, `shared_source_equivalence`. Stop on first failure,
preserve evidence; private model-free storage fixtures may use their own temporary
directories, with no owner/model cache preparation or GUI/GPU. Root owns runner/lease
selection and whether this finite scope is granted. Nothing here takes a lease.

Separate proposed real-runtime START P1: frozen source/private diagnostic and
explicit isolated model/corpus/cache/evidence roots, <=900 seconds total,
one eligible device and one preparation worker; at most one cold BF16 identity
build (600-second cooperative budget), one eight-N1/full-N8 golden attempt
(120 seconds), bounded completion/fault/cancel observation, then retire normally.
Supply verified cached FP32 detector/fallback plans in that explicit isolated
root beforehand; this path cannot prepare them. Do not substitute or modify the
ordinary owner's cache. A missing baseline is a prerequisite refusal.
Keep all actual outputs, plan/device/builder/version/hash receipts and failure
records. No unchanged retry or default catalog activation. A second cache-only
reattest/reuse session, exact pipeline/runtime regression and wider independent
quality acceptance need separately finite authorization. No benchmark or release
acceptance follows from P1. The desktop remains revoked. These are plans only;
all heavy compute is HELD pending fresh finite ROOT START.
