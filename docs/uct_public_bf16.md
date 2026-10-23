# Public BF16 model and local attestation (offline, 2026-10-23)

Owner approved shipping the converted **model**, with an engine built and checked
on each eligible machine. This follow-up preserves policy commit `0035c6f` and
helper `0f72f8d`. It stages nothing, enables no factory and supplies no new engine,
golden corpus, inference, quality or performance result. The private schema-1
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

`ProductionCorpusCatalog()` is deliberately empty: no actual owned crops and
independently verified truth were supplied. Public readiness therefore refuses.
The model-free fixture covers negative gates and the engine-contract predicate;
it manufactures no readable crops, corpus truth, passing golden result or ready
receipt. Positive corpus/attestation/receipt paths need real reviewed assets.

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

## Assigned Network/factory integration contract

The active backend is unmodified; root must assign this implementation and its
lifetime tests before public enablement:

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

`uct_public_bf16_contract` is authored but unbuilt/unrun. A fresh finite CPU slot
requires a frozen commit. Real corpus production/review, enabled integration,
engine preparation, startup/fault/cancellation proof, quality and worker timing
need distinct root START boundaries. No automatic continuation or release staging.
