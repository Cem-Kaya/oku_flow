# Public BF16 model and local attestation (offline, 2026-10-23)

## Current real-runtime checkpoint at `0697367`

The real CUDA/TensorRT adapter compiled and all four scoped contracts passed:
Paddle processing, precision artifacts, public BF16 and shared-source equivalence.
The build controller retained exit125 for an unidentified owned residual process;
its exact job cleanup reached zero and the runner/slot release was verified.
This is not complete release acceptance.

Root then ran the actual local-build/strict-golden path once for each selected
recognizer, using separately owned model/corpus/cache roots. All three native
fixtures and their controllers exited0, with confirmed State retirement. The
golden **quality checks failed** and same-tier FP32 was selected as designed:

| Recognizer | Exact rows | Golden accepted | Effective service batch |
| --- | ---: | --- | ---: |
| Medium | 8/16 | No; FP32 fallback | 8 |
| Small | 8/16 | No; FP32 fallback | 8 |
| Tiny | 12/16 | No; FP32 fallback | 1 |

Sixteen rows mean the same eight owned strings at N1 and full N8, not sixteen
independent documents or a word-error-rate benchmark. Expected pixels/text were
not changed after observing output. For example medium returned `00 00 00`
for `O0 OO 00`, `111111` for `Il1 I l 1`, and `café naive` for `café naïve`.
These failures do not establish that FP32 reads those examples correctly.
No production recognizer corpus was activated and no passing ready receipt was
published for these candidates.

The small-recognizer run independently preferred the **medium BF16 detector**.
That detector completed all four tensor compatibility smokes, then remained BF16
while the recognizer fell back to small FP32. This verifies independent role
fallback and real detector inference on the test GPU. Zero-input probability
checks are not detector recall/geometry/quality acceptance. Small/tiny BF16
detectors and public cold/warm install behavior remain unvalidated.

Evidence: `Z:/Projects/OkuFlow/local-work-data/uct-bf16-app-default-20261023/`
contains `detector-role-build`, `runtime-medium-0697367`,
`runtime-small-0697367`, `runtime-tiny-0697367` and
`actual-runtime-summary-0697367.json`. Ordinary owner caches, GUI and running
application instances were untouched. Production packaging/activation remains
separate; these results must not be described as BF16 shipping by default.

## Owner app-preference decision and tier validation work

The owner now requests BF16 preference in both GPU model dropdowns, with
same-tier FP32 fallback, rather than leaving this as diagnostic-only work.
CPU fallback stays FP32; no automatic live/Freeze tier switch is authorized.
The first implementation step generalizes recognition model/corpus identities
to tiny, small and medium, without activating the production corpus catalog yet.
Tiny retains N1 service; the local engine still supports N1..N8 for its exact
golden check. This is not promotion of tiny service to N8.

Small/tiny corpus variants retain all eight original owned pixel payloads and
expected text. Only their exact model identities/versions and provenance labels
change. Their decoded dictionary hashes use the same length-prefixed UTF-8
`uct-dictionary-v1` contract, not the resident diagnostic's newline-joined hash.
The private `optionalPublicBf16Preparation` slot can exercise the real build,
golden and typed FP32 fallback for an explicitly selected recognizer in isolated
model/corpus/cache/evidence roots. This stage is unbuilt/unrun. Detector BF16,
actual runtime validation, app activation and payload staging remain in progress.

The next checkpoint adds independent detector preference for each GPU tier.
Three newly converted BF16 detector graph hashes are compiled in
`uct_bf16_detector.hpp`; the old slower mixed-FP16 detector is not reused.
`scripts/uct/bf16-models.json` inventories all six converted model identities,
original source/configuration pins and upstream revisions. It is not runtime
approval supplied by a writable manifest, a plan shipment or a quality result.

Detector engines use the existing detector profile and new driver-independent
BF16 cache keys. Minimum, optimum, maximum and portrait **zero-input tensor**
checks require the actual FLOAT DEVICE LINEAR detector-map ABI, finite bounded
probabilities and confirmed completion. These are compatibility checks, not
region-recall, recognition quality or performance attestation. A healthy-stream
artifact/build/typed smoke refusal selects the same-tier FP32 detector; actual
inference/API/CUDA faults, cancellation and unknown completion retain normal
State retirement/Automatic fallback. Detector refusal does not reject an
independently eligible BF16 recognizer. The pair is published ready only after
both roles are final. Candidate validation keeps FP32 baseline engines isolated
and cache-only; BF16 detector preparation is a separate explicit opt-in there.

Both ordinary and precision engine preparation now attach Network ownership to
State before fallible work. Host graph/parser/builder/config/serialized owners
remain retained through completion, then are released before readiness.
GPU status appends actual detector/recognizer precision, without changing tier
settings or CPU choices. This detector checkpoint is UNBUILT/UNRUN; production
recognizer corpus activation and app payload staging still await root validation.

Owner approved shipping the converted **model**, with an engine built and checked
on each eligible machine. This follow-up preserves the composed batch/helper/cache
policy ending `7c59a59423a0cefa6e19a78ba81908e16461196c`. Private background
integration is authored; the real model path and latest repair remain unbuilt/unrun.
Root subsequently chose public-v2 ready-last as a separately versioned product
protocol. It supersedes the unreleased v1 directory publisher; it is not a narrow
equivalent repair, a rename-probe reroute, or a power-loss durable transaction.
Root's partial model-free failures are recorded below. It stages nothing and supplies no
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
   cache writes are new-only with logical all-or-nothing v2 reader admission.
   Do not enable host-code/plugin trust or
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
in that private root before a granted runtime attempt. Ordinary FP32 detector
preparation has cancellation checks but no BF16 deadline. Eligibility/catalog
admission then precedes the actual candidate attempt and its 600-second deadline;
an unavailable/ineligible request retains cancellation-only FP32 fallback.
An attempted candidate still treats deadline expiry/cancellation and runtime
faults as terminal, including during confirmed-refusal cleanup. Ordinary
production/schema-1 cache policy is unchanged.

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
cache files. The separate `ocr-engines/public-bf16-v2/<identity>/` parent binds
approved model/device/corpus; the actual builder remains in `CacheKey`. Its
cache-key domain and ready-receipt schema are v2, with unchanged model/profile/
GPU/builder/corpus identity fields and driver/DLL hashes only provenance. V1 is
unreleased and production-disabled; no v1/legacy read, rekey or migration occurs.

Each generation is exactly lowercase `<recipe64>-<nonce32>`. A new UUID nonce is
only a collision-resistant locator, not approval. A zero-wait named mutex over
the canonical parent serializes cooperating publishers' bounded inventory scan,
validation, capacity reservation and ONE exclusive new-directory creation. Busy
or unavailable serialization refuses; there is no wait/retry loop or lockfile.
The mutex releases before writes/observers. An already observed, fully validated
committed generation with the same recipe returns false unchanged, even if its
plan bytes differ. Overlapping attempts which already reserved distinct nonce
directories may both commit; they cannot overwrite one another or supply authority.

Root/ancestor/generation pins share READ only, excluding WRITE and DELETE, with
list/attribute/traverse access and no DELETE right. `NtCreateFile(FILE_CREATE)`
returns the SAME newly created directory handle, closing the create-to-pin
adoption gap. Existing ordinary ancestors are checked/pinned before descendants;
reparse points refuse. No generation is adopted, overwritten, repaired or removed.
This uses the documented Windows system entry point, not a new redistributed DLL.
The source implementation and native compatibility remain UNBUILT/UNRUN.

After actual completion/golden success, the worker creates `recognizer.plan` and
`ready.json` new-only. It closes every writer, acquires exact regular/non-reparse/
single-link READ-only file pins, and hashes the actual stored bytes against the
supplied plan/receipt before commit. Those pins reject existing incompatible
writers and remain through commit/readability. The close-to-read-pin handoff has
no authority: any substitution must still pass type/link/bounds and exact rehash.
Directory pins alone do not make files immutable. The final `COMMITTED` file is
CREATE_NEW LAST, containing exactly `okuflow-public-bf16-v2\n` (LF, no NUL).
Its bytes/type/link count are verified on its creation handle BEFORE closing
makes it readable. This provides logical reader admission; unfinished files and
directories are visible, and there is no directory rename or durable-transaction
claim. Native writes here do not claim power-loss persistence ordering.

Only an actually absent fixed marker makes a recognized generation incomplete.
Such generations remain untouched and count toward the same32-entry bound;
after a pre-marker crash a fresh nonce can reserve another generation without
repairing the old one. A marker-present failure may be committed, corrupt or
indeterminate, never silently incomplete. Corrupt markers/committed data refuse;
post-close observer failure preserves the committed generation. No automatic
cleanup or retry is introduced. Exhaustion requires a separate diagnosed policy
decision, not deletion by this path.

The native inventory iterator includes files/hidden/system entries and reports
enumeration errors. It collects at most32 names in fixed storage BEFORE sorting;
unknown names refuse, including marker-absent unknowns. Cooperating reservations
cannot create a33rd entry. Uncooperative external additions can still exceed the
bound and make admission refuse; pins do not freeze directory additions/marker
transitions. Lexical selection is deterministic for the observed inventory.
The reader pins the marker and both exact files, checks fixed marker bytes,
receipt schema/digest/identity, actual plan hash and recipe prefix. ALL committed
entries validate before any selected plan is returned or production duplicate
returns false; different builders are skipped only after integrity validation.
The first sorted current-builder candidate is copied into owned bytes. A child
pin closes each iteration, preserving the independent ten-handle path bound.
Receipts are at most256KiB and plans strictly below256MiB; only selected bytes
plus the current bounded read are retained. Reads/validation stay on preparation,
not CPU bridge/UI/presentation. File-system calls are not hard-preemptible.

`ValidateCommitted` is shared between lookup and typed product publication; the
opaque file-storage fixture overload supplies no model authority. Neither marker
nor receipt approves expected text or skips real deserialization/profile/golden
checks. Runtime ABI11 tuple admission remains >=11.4.0.106; actual deserialization
decides compatibility. ProductionCorpusCatalog remains EMPTY.

A bounded 64-entry process-session attempt latch retains failed/in-flight
identity/plan attempts. There is no rebuild/retest-until-pass loop. Builder
progress observes cancellation and a 600-second deadline; golden work has a
120-second deadline and each event poll a 30-second limit, all on preparation
only. Immediately after `RunPublicGolden` returns, `PreparePublicCandidate`
rechecks the golden deadline/cancellation before `CheckGolden`. A final N8 event
that completes after expiry cannot authorize golden success or publication:
ordinary `std::runtime_error` retains the observations for incomplete failure
reporting and normal retirement, without typed FP32 fallback.
Vendor builder calls are cooperatively cancellable, not forcibly preempted;
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
raw text preservation, failure records, latching, new-only storage and
pending-input/model/staging/output quarantine cases. Storage fixtures are labelled
non-model bytes, never fake passed receipts. All additions are **UNBUILT/UNRUN**.
The R1/R2 independent source reviews and subsequent unretested repairs are recorded
in [review dispositions](uct_public_bf16_review.md). Existing model-free BF16
fixtures have no clock seam for the final-N8 deadline sequence; this narrow repair
adds no mirrored helper/test. The actual deadline sequence and positive runtime
path remain unverified pending a separate ROOT START. Native v2 create/pin,
marker-last admission, junction refusal and directory sharing require execution.
The existing eligibility and baseline-policy seams additionally cover accepted
candidate preflight versus unavailable production and disabled candidate requests
leaving ordinary baseline preparation enabled. They do not execute the preparation
worker or prove its deadline control flow; those source repairs remain UNBUILT/UNRUN.
Integrity-only cache fixtures also select between two builder generations in
either enumeration order, check absent matching builders, preserve all bytes/
inventory and reject a nonmatching child's corrupted plan hash. Their labelled
non-model receipts contain no golden/pass field; positive lookup is not model
acceptance. These new fixtures are likewise UNBUILT/UNRUN.
Root reports an earlier combined CPU gate on the pre-repair `89f839b` public
bytes: 5/6 passed, with the publication/lifetime slot throwing an undiagnosed C++
exception before completion. The first log and root-owned pending native probe
are recorded in [review dispositions](uct_public_bf16_review.md). That run does
not cover these policy/cache repairs or establish native sharing/rename behavior.
Root's subsequent isolated native probe confirmed that the original attribute-only
pin allowed outside DELETE and WRITE|DELETE opens, whereas list/attribute access
with READ/WRITE sharing excluding DELETE refused them and allowed exact-staging
new-only rename. The probe created its non-model file before holding staging;
it did not test production's file creation after the staging hold or diagnose the
earlier Qt exception. The separate source repair preserves that production write
order, adds delete-only/no-delete opener and pinned-parent publication cases, and
prints unexpected `std::exception::what()` from the private Qt slot. Those project
changes remain UNBUILT/UNRUN pending root's changed-fingerprint execution.
Root's later source inspection found accumulated skipped-generation handles in
`FindPlan`. R5 closed that iteration-scope repair, deadline scope and builder
selection in source. Frozen `6d5` compiled in root's composed C3 gate, which again
finished 5 PASS / 1 unresolved: the publication/lifetime slot reported
`unknown public BF16 cache generation`, with no entry/phase at 221 ms. The
32/33-generation cases have no passing acceptance; the precise trigger remains
unknown. R5 left native confinement open and the new WRITE-exclusion repair and
post-hold mutation fixtures are UNBUILT/UNRUN. Unknown-entry refusal now includes
bounded JSON phase, entry, ordinal and parent path, with fixed fixture-phase
breadcrumbs and an explicit malformed-generation negative case. No unknown name
is newly ignored. Root owns one future changed-fingerprint run and fresh source
closure; no automatic retry, model attestation or C3 causal diagnosis is implied.

R6 subsequently closed WRITE/reparse exclusion in source but confirmed the
READ-only absolute-target rename P2 through root's native R2 diagnostic. R3's
relative-parent probe launch was rejected by automatic approval review as
“blocked by policy”; it never executed, and no result/reroute exists. Root then
explicitly selected the different v2 protocol above. The one authorized nested
v2 design review found creation/pin ownership, capacity reservation, shared
production validation, snapshot/duplicate semantics, marker-close classification
and enumeration/file-handoff obligations. The source addresses them; this is
not final independent closure. See [full dispositions](uct_public_bf16_review.md).
V2 fixtures author marker-last/missing-marker/fresh-nonce recovery, exact file-pin
exclusion and concurrent readability, overlapping unique generations/determinism,
duplicate preservation, corrupt marker/plan/receipt/later sibling, recipe mismatch,
single-link enforcement, v1 isolation,32/33 limits and retained native reparse/
post-hold publication assertions. All are UNBUILT/UNRUN; no positive model path,
quality, timing, platform publication or release acceptance is claimed.

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
