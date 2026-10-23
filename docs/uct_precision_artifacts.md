# Role-aware UCT precision experiments (offline implementation, 2026-10-23)

This branch prepares a cache-only artifact contract. It has **no schema-2
production approvals, runtime integration, new engine, inference result or speed
claim**. Root must grant a finite START slot before configuring, compiling,
testing, preparing a model/plan or running inference. Source/graph/hash inspection
is the only work executed here. The starting commit is
`bb53ac884c24d15f0c77b2dc43cace0e8e650c24`. The attached additional-performance
report is a source of hypotheses, not new measurements.

## Contract and approval authority

`src/common/uct_precision_artifacts.hpp` is a separate private schema-2 loader.
Schema-1 graph/plan/device pins and manifest remain supported. Owner's follow-up
removes driver admission/key pinning and admits runtime versions at least
11.4.0.106; new versioned keys coexist with read-only exact legacy lookup. Old
artifacts are never copied/rekeyed/overwritten. No environment selector is added.
`ParseManifest`/`LoadManifest` require an explicit independently reviewed catalog
provided by trusted application/diagnostic code. The manifest cannot supply or
load that catalog. There are currently zero production schema-2 catalog entries.
Synthetic catalog records occur only in the model-free fixture.

Every entry binds role, medium tier, conversion policy, source and converted
graph hashes, configuration and decoded dictionary identities, conversion recipe
and graph-audit hashes, whole-plan hash, exact graph names, FLOAT/DEVICE/LINEAR
I/O, output ABI, min/opt/max profile, capacity, width policy, UUID, exact compute
capability, builder TensorRT version and builder policy. CUDA API driver
identity, actual `nvcuda.dll` version/hash and loaded runtime TensorRT version/hash
are provenance only. Neither driver changes nor a different TensorRT DLL
hash changes approval. Schema-2 identities cannot contain driver or DLL-hash fields.

Only these bounded proposed families are represented:

| Role | Precision graph | Profile minimum / optimum / maximum | Output |
|---|---|---|---|
| Medium detector | FP32 or mixed FP16 linear islands | `[1,3,32,32]` / `[1,3,544,960]` / `[1,3,960,960]` | FLOAT `[1,1,H,W]` map |
| Medium recognizer | mixed BF16 linear islands, fixed1024 | `[1,3,48,1024]` / `[8,3,48,1024]` / `[8,3,48,1024]` | FLOAT `[N,T,18710]` probabilities |
| Medium recognizer | mixed BF16 linear islands, ordered widths | `[1,3,48,128]` / `[8,3,48,512]` / `[8,3,48,1024]` | FLOAT `[N,T,18710]` probabilities |
| Medium recognizer fallback | independently approved FP32, fixed1024 only | `[1,3,48,1024]` / `[8,3,48,1024]` / `[8,3,48,1024]` | FLOAT `[N,T,18710]` probabilities |

Representing a family does not approve it or establish graph support. Exact
engine I/O/profile validation and actual shape execution remain mandatory.
Detector map postprocessing remains FLOAT; detector thresholds 0.2/0.4, 960-edge
resolution, coverage, component order/cap and recognition settings stay fixed.
The existing small FP32 recognizer is a separate configuration arm, outside this
medium-only schema. No smaller dictionary, sorting or new output ABI is enabled.

Schema 2 has exactly `schema`, `prepare`, `artifactRoot`, `detector`, `recognizer`;
`schema=2` and Boolean `prepare=false` are mandatory. Each role has exactly
`identity`, `graphPath`, `planPath`. The complete identity must equal one unique
catalog entry. Profile arrays are named `minimum`, `optimum`, `maximum`.
`graphRelativePath` and `planRelativePath` in the approved identity bind the
original locators under the selected local root. Canonical paths cannot escape
that root. `IdentityKey` is a receipt identity, **not a new filename/key for an
old engine**. Existing cached plans must never be rekeyed, aliased or copied to
pretend to be a new profile or builder/runtime result.

Runtime admission compares the numeric major/minor/patch/build tuple against
11.4.0.106 within major ABI11; 11.4.1.0 and 11.5.0.0 pass, 12.0.0.0 fails.
Older, incomplete, signed, whitespace or otherwise malformed strings fail.
Builder `tensorRtVersion` remains part of the exact approved artifact, not a
loaded-host equality check.
The existing compiled ABI11 and `nvinfer_11.dll` path remain; passing the numeric
floor does not discover ABI12 or prove factory/serialization compatibility.
[NVIDIA's compatibility documentation](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/engine-compatibility.html)
states that default engines require their build version, while explicit build-time
version compatibility permits later runtimes. This policy permits an attempt with
verified older bytes; deserialization, factories, I/O and profiles remain final
authorities. No compatibility flags or embedded host-code permission are enabled.

The loader rechecks selected source/configuration bytes, decoded dictionary,
converted graph, whole plan and its 32-byte payload digest. No directory is
created and no file is modified by the C++ loader. `LoadPaddleAssets` now hashes
and parses the same bounded (8 MiB) configuration snapshot, closing its earlier
verify/reopen gap. Dictionary hashing uses `uct-dictionary-v1\n` then decimal
UTF-8 byte length, `:`, exact UTF-8 bytes for every ordered entry, including blank
and appended space; there is no Unicode normalization.

## Precise integration seam for root / batch lead

`src/common/uct_tensorrt.cpp` is deliberately unmodified in this branch.
Integration is required before any schema-2 runtime claim:

1. On the existing preparation worker, obtain the actual UUID, compute capability,
   CUDA driver API identity and exact loaded `nvcuda`/`nvinfer` paths/versions/hashes.
   Hash the loaded images, not a different PATH candidate. Use
   `LoadManifest(path, reviewedCatalog)` then `ReadSelection(selection,runtime,assets)`.
   Keep every nonempty precision request cache-only for **both** networks and
   every fallback. Driver observations never gate approval or key selection.
2. Extend `Network`'s optional verified-plan argument with the approved `Identity`.
   Deserialize its `payload` before inference. A null refusal with no reported
   CUDA error is a typed `ArtifactError`; preserve the reason/loaded version and
   allow verified cache-only FP32 fallback. A reported CUDA fault escapes `Check`
   and retires/quarantines, never trying another engine. Context/allocation/
   enqueue faults remain ordinary failures. No embedded host-code flags are enabled.
   Before context creation, require exactly one input, one output and one profile,
   then use `CheckEngineProfile` with actual names, types, DEVICE location, LINEAR
   format and min/opt/max dimensions. Keep `cudaPeekAtLastError` before any typed
   metadata rejection. Its success is a necessary condition for `ArtifactError`,
   not proof that an earlier CUDA/API fault can be ignored.
3. Build detector and recognizer into local owning values while the State owns
   the stream, input-independent allocations and retirement path. After both are
   valid, commit one bundle containing **both engines, both identities and
   RecognizerPolicy**. Derive `State::variableWidth`, `recognizerBatch` and ABI
   exclusively from this committed policy before publishing preparation ready.
   A helper returning just a new engine pointer is insufficient.
4. Fallback is an explicit ordered list of independently verified *pairs*:
   requested ordered BF16 pair, approved fixed1024 BF16 pair, approved fixed1024
   FP32 pair if supplied. The last pair requires its own whole-plan approval;
   the old ordinary FP32 cache's self-contained digest alone is not provenance.
   Only typed pre-inference artifact/policy/profile rejection plus confirmed
   stream completion may advance this list. Rebuild the complete policy on each
   attempt: fixed fallback resets width, capacity, ABI and all group descriptors.
   Do not catch `std::exception` as a reason to try another precision engine.
5. `CheckOutput` consumes actual runtime dimensions. It throws an ordinary runtime
   error after submission, never `ArtifactError`. Retain Network's checked tensor
   byte/product ceilings; do not derive T from W. The slot retains its own N/W/T
   through host consumption. Width/tail changes stay serial; identical shapes
   retain the existing two-slot lookahead. Cancellation drains every issued
   group; CUDA/copy/event/context failures retain State/input ownership through
   exact retirement or quarantine. Cooperative integrals stay enabled.
6. Publish `VerifiedSelection::Receipt()` and `ActualStatus` **only after** actual
   engine validation/commit. Diagnostic JSON must retain both full role records,
   actual width/capacity/ABI, requested and effective arm, fallback reason and
   actual loaded runtime identity. A fallback changes the arm and invalidates a
   mislabeled performance cell. Default public/resident factory remains FP32;
   add an explicit production-selection seam and same-capacity comparisons.
   Keep historical N1/N8 strict failures visible. For the detector arm disable
   frozen-crop replay (its current source-graph-only key cannot identify changed
   detector arithmetic); use full detection and downstream work as authoritative.

The API supplies a complete policy value but cannot make separately assigned
State fields atomic on its own. Root/batch-lead integration and its ownership
tests must enforce steps 2–6; the new model-free fixture is not that evidence.

## Read-only local evidence

Private receipts live below
`Z:/Projects/OkuFlow/local-work-data/uct-artifact-precision-20261023/offline`.
`inspect_precision_artifacts.py` uses the existing private ONNX 1.20.1 interpreter,
never imports CUDA/TensorRT, and does not run inference, a graph checker, shape
inference or conversion. It retains first failures by exclusive receipt creation.
Reshape shape-input dependencies and the graph tail are recorded, including
explicit unresolved/depth-bound leaves. This is source inspection only.

The BF16 graph hash matches
`ecd8ffc92aa1b9f7a33b0ad39757b64887c465f15bb54853936b2136f94e5835`.
Its input is FLOAT `[N,3,48,W]`, output FLOAT `[N,T,18710]`, opset 22. Reshape
dependencies use inferred `-1`, constants including `[0,-1,3,8,24]` and
`[0,-1,192]`, and a final Shape-derived sequence dimension. This makes the
proposed profile plausible; no bucket has been executed by this branch.
The retained driver-identity-13040 plan still hashes to
`155c74b5b856467e49853190c881077c9af3200203a196f9e737f7533e6f5495`
and its envelope digest matches. It was not deserialized or relabeled.

The existing mixed-FP16 medium detector graph hashes to
`24e3f6f4f003da711fe466429dbf93037da4b7c521e729f32262629c64ff771f`;
FLOAT I/O and FLOAT ConvTranspose/Sigmoid tail are retained. The old detector
plan hash is `2caeb2117dd8d80355f06a5cd18b12e00e78cd9999ce0e3be67d3599b717ece5`.
The new Opus report (`a6f041ff3d5cdbafc9d40b39d71c5b5c5461c740`, private
`uct-review-implementation-20261023/opus/uct_gpu_independent_opus_review_2026_10_23.md`)
re-derived negative archived evidence: this graph's engine had 735 layers versus
278 FP32, with detector stage 26.0–29.7 ms versus 16.2–19.1 ms (n=3 medians per
scene, contended shared desktop). These are archived measurements, not this
branch's results. Graph `24e3…` is rejected as a speed candidate; P3/B2 below
are withdrawn. No build or repeated negative arm is justified. A different
conversion needs its own source/fusion hypothesis and root slot. Schema support
for independently approved detector roles does not approve that graph.

The same report prioritizes exact `PaddleMergeRecognition`/quad-area host work
(archived 49–53 ms of a roughly 95 ms BF16 book worker). Root's merge lead owns
that work. This branch's `uct_paddle_processing.cpp` diff is entirely within
`LoadPaddleAssets`; `PaddleMergeRecognition` is unchanged. Approximate width/small
arms follow the exact host fixes and require fresh complete-worker comparisons.

Read-only installed file inspection found `nvcuda.dll` version `32.0.16.2101`,
SHA-256 `e52f89d528f4cc2977c90cb6337c2bc1c2fcb31bfa93822b15e936007d7cd26d`.
The report's newer CUDA driver identity 13050 remains supplied historical
evidence until root's actual-runtime START slot. No driver was changed.

## Frozen finite START requests

Each slot is a separate grant, owns the tracked global runner lease where
applicable, stops at first failure/deadline and preserves all output. No automatic
retry or cascade to a different model/plan. All new large outputs belong under
`Z:/Projects/OkuFlow/local-work-data/uct-artifact-precision-20261023`. Existing
inference-only SDK/model roots remain read-only. Parent must serialize these
against its current eight-job CPU gate and all other compute. Owner video/app
load is allowed; record it as contention and never request apps be closed.

| Slot | Finite request | Purpose / stopping condition |
|---|---|---|
| C1 | At most 8 CPU jobs, 780 s total; tracked scoped checks | External owned build under work-data; compile affected `uct_paddle_tests` and run `uct_precision_artifact_contract` + `uct_paddle_processing` + required shared-source guard. Bound PCH-on build/test to 600 s, PCH-off affected compile to 150 s, receipts to 30 s. Stop if selection requires a broader gate, first failure or deadline; no full-gate substitution |
| C2 | 1 enabled-TensorRT CUDA compile, at most 8 jobs, 1200 s | Compile actual batch-lead integration before any engine use; then tracked `uct_paddle_cuda` + retirement/ownership cases in a separate 180 s GPU slot |
| P0 | Actual runtime/cache-only precheck, 120 s, one child, separately granted | After batch-lead enabled build receipt exists: try the SAME approved BF16 plan bytes, stable path then exact legacy key; actual runtime minimum/ABI11/factory/deserialize/FLOAT/profile/device acceptance and exact closure. No image inference, builder, cache writes or cold preparation. Record actual status/failure, loaded DLL/version/hash, driver provenance and inventories; stop on refusal |
| P1 | Withdrawn as a driver-update prerequisite | Driver changes do not justify a new plan or new approval. A deserialize incompatibility is a preserved failure; any genuinely needed new build requires a distinct root decision and slot |
| B0 | Production BF16 fixed1024 functional/readiness baseline, 240 s | Assert actual BF16+FP32 detector identity and exact State closure, zero image egress, unchanged caches; then four sources × three timed rows plus startup/warmups = 17 jobs, minimum fixture allowance 191 s. No baseline claim if fallback |
| P2 | One ordered BF16 dynamic profile preparation, 600 s | Same graph/recipe, min128/opt512/max1024 N8 only. Then separately 180 s shape probe for N1/N7/N8 × W128/256/512/1024; retain actual T/classes and failures |
| B1 | Fixed vs ordered BF16 pilot, 2400 s maximum | Two high-padding sources, same model/helper/medium FP32 detector; predeclared ABBA then BAAB visits, 20 timed rows/source/visit with first four excluded identically but retained. 43 jobs/visit, 269 s minimum allowance/visit; 64 steady rows per arm/source. Descriptive tails only |
| B1b | Separate sparse/long-line controls, 1200 s | Two sources, ABBA, same 20/first-four rule and 43 jobs/269 s allowance per visit; 32 steady rows per arm/source. Tilted/small-font controls require a separate equivalent slot |
| P3 / B2 | Withdrawn for archived graph `24e3…` | Existing negative stage/layer evidence rejects repeated build/timing of that conversion. A source-backed different conversion would be a new hypothesis/commit, keeping recognizer, coverage and thresholds constant |
| B3 | Optional existing medium-detector/small-FP32-recognizer arm, 900 s | Separate configuration, existing original graph/caches, fixed1024 N8. Two sources, ABBA, eight timed rows/source/visit, first two excluded but retained: 19 jobs/197 s minimum allowance per visit, 12 steady rows per arm/source. Compare against actual current-driver production BF16; no dependable tail or historical-transfer claim |

The revised immediate request is C1 then separately P0: **900 s total ceiling
(15 minutes)** with no automatic handoff between slots. C2 and later rows are
future proposals, not part of C1/P0 or permission to run. P0 requires root/batch
integration and an existing matching enabled binary; this branch alone cannot
execute it. The tracked
fixture's existing watchdog minimum may exceed a requested slot when many
sources/blocks are selected; reduce the visit to its bounded fitting workload or
request a different finite slot, never weaken its timeout check. The default
resident fixture currently enforces N1 reference; production BF16 benchmarking
needs the explicit same-capacity seam first. Root should grant B0 before B1/B2.

## Quality and measurement decision record

Freeze source digests, exact acquisitions/PTS, independent truth and intended arm
before any timed pass. Preserve original text/disposition and 10-pixel geometry
criteria as separate historical verdicts. The owner's 5% word-difference budget
is agreement, not truth accuracy. Score all retained outputs, including
low-confidence/protected/omitted words and extras. Report CER/WER with
denominators, spatial recall/omissions/extras, detector/protected/search/fitted
quads, confidence/disposition crossings, crop fingerprints/counts, N/W/T and
shape transitions. Human-verified truth must be identified as such; otherwise
label it unavailable and retain the output disagreement without an accuracy claim.

Freeze a separate sensitive-token set containing decimals/signs, inequalities,
negation, O/0, I/l/1, Turkish dotted/dotless I and accents, plus independently
verified scripts actually supported by the chosen dictionary. UI language count
does not establish OCR script coverage. A proposed synthetic source can contain
`0.5 mg`, `-0.5`, `x <= 0`, `do not`, `O0`, `Il1`, `İ I ı i`; retain its actual
render/font provenance and verify visible glyphs before using it as truth.
Confident O0→00 errors are not assumed detectable by confidence routing.

Primary endpoints are complete scheduler worker and submit-to-callback time,
including resident preparation, host merge, shape transitions, waits and changed
recognition work. Stage counters are attribution only; do not add overlapping
host/GPU intervals or invert worker latency into delivered FPS. Preserve
cancellation/stale/failure outcomes and actual lease retirement. Source-to-visible,
paint/UIA, sensor-to-photon and live presentation claims require their own closed
ticket/native evidence and are not supplied by these worker slots.

Promotion is deferred. Before a slot, root should record the material-gain and
held-out quality/tail budgets (the report suggests 10% and 5 ms, ≤1 percentage
point WER increase, and a separate sensitive-token tradeoff; these are proposals).
Use visit-level paired deltas and retain raw order/scatter; 64 rows per cell do
not establish robust p95 acceptance. Large promotion runs are new finite slots,
not automatic extensions. Stop dominated/quality-failing candidates before sorting.

Before/after inventories and every receipt must bind source commit/tree + dirty
file hashes, binary/CUDA-unit hashes, compile flags/toolchain/PCH, runtime DLLs,
role artifacts and profile, source pixels, actual arm and operation deadline.
Preserve all first failures, exact stdout/stderr, CTest attempts/JUnit/ledger and
repair reasons; never clear a blocker or rerun until pass. No release, push, PR,
root integration or owner settings/environment mutation is authorized here.

## Validation status

`uct_precision_artifact_contract` is a new desktop-free CTest case using the
existing `uct_paddle_tests` executable with explicit runtime inputs. It exercises
approval/refusal, roles, profile/type/device/hash mismatches, driver-independent
approval/keys, builder-bound identities, runtime floor/malformed-version refusal,
exact historical lookup vectors, distinct
fixed/ordered identities, dictionary framing, read-only missing-artifact refusal,
and post-submission output errors which cannot trigger typed fallback.
Synthetic graph/plan bytes cover successful envelope validation and tampering;
`ReadSelection` metadata/dictionary refusals are covered. Successful source/config
verification against pinned real model bytes remains a separately gated asset
fixture; a model-free case cannot manufacture those source hashes.
It has been authored but **not built or run** pending root START. Existing
production state-lifetime tests remain necessary after integration.
