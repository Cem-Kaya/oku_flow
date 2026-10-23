# Ordinary UCT TensorRT cache and runtime policy

This is a separate offline policy candidate after the exact helper batching
commits. No compiler, configure, test, inference, GPU job, model preparation or
download has run for it. Root's finite START and tracked runner lease remain
required. The precision lead owns the shared pure identity/version helpers,
their fixtures, schema-1 BF16 approval and schema-2 artifacts; this change owns
the ordinary `Network` cache and `Libraries` wiring in `uct_tensorrt.cpp`.

New ordinary FP32 keys exclude the CUDA driver. They retain the graph SHA-256,
GPU UUID, role, exact batch/width profile, precision and TensorRT builder recipe
identity. New builds bind the actual loaded builder version, never a stale
11.4 literal. Lookup tries the stable key for that builder, the original 11.4
builder's stable key, then exact historical keys for 13050 and 13040. A
recorded builder version need not equal the current loaded runtime. Historical keys
are locators only. Loading one does not copy, alias, rekey, delete or overwrite
it. Payload digest/size checks remain mandatory. Existing files remain intact;
only a genuinely new compiled plan can be written at an unused stable path.

`Libraries` reads all four exported inference DLL version components and
admits versions at least 11.4.0.106, compared numerically in component order.
The DLL basename remains `nvinfer_11.dll`; the compiled major-11 factory ABI,
parser/builder APIs, engine deserialization, FLOAT I/O and exact profiles still
control actual usability. The current driver, loaded TensorRT version and DLL
hashes belong in provenance, not key/approval equality. The artifact's builder
version and digest remain bound to that artifact.

The private `TensorRtRuntimeForDiagnostic` seam uses the same `Libraries`
loader/admission to record the actual exported tuple, loaded module path and
fresh DLL SHA-256. It never invokes a factory, context, CUDA API or builder.
Hashing is receipt collection, not a product approval pin. Resident, active
and paused diagnostics use the same ordered stable/legacy keys, preserve every
candidate's file preflight, and match the actual backend's accepted plan key
and payload hash. A digest-valid preflight is never labelled a deserialization
result. The loader emits plan receipts after successful context construction.

The existing resident model-free contract adds actual-builder identity,
legacy-only lookup under a newer runtime, unchanged file inventories and
matching/mismatching receipt cases. Golden legacy detector and fixed/bucketed
N1/N8/N16 key vectors belong to the precision lead's shared fixture. All are
unrun until START. Using this diagnostic on the exact bb53/helper pair requires
the identical receipt-only seam on both arms, disclosed and hashed; the cache
policy itself stays out of that pair.

Removing an app-level precheck does not make every old plan work on every
newer runtime. NVIDIA's [TensorRT 11.4 engine compatibility documentation](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/inference-library/engine-compatibility.html)
states that default plans normally need their builder runtime version; later
runtime support requires appropriate build-time version compatibility. A
runtime/factory/deserialization/profile/ABI rejection must remain visible.
BF16 deserialization refusal without a reported CUDA error is a typed
pre-inference artifact refusal, allowing cached FP32 fallback. CUDA faults,
runtime-factory/execution-context failures and enqueue failures never authorize
another plan attempt. There is no scalar-helper or alternate-engine retry
after uncertain GPU work.

The separate source-only factory repair after frozen `679f16d` closes an
inherited policy gap: a missing N8 plan plus a null builder-side factory could
load an already cached N1 plan and hide the API failure. Builder, network,
parser, configuration and optimization-profile creation now use the shared
terminal API guard; missing factory exports are terminal too. A parser that
was created successfully but rejects the graph remains a graph-policy refusal,
as do existing cache/shape/capacity refusals. No cache keys or plan bytes change.
`tensorRtFactoryFailureDoesNotLoadCachedN1` exercises the production guard and
fallback decision with injected null factories, valid creation, legitimate
refusals, CUDA and allocation failures. This model-free fixture is registered
under `uct_paddle_processing` but remains unrun until a fresh root START;
native factory-fault behavior is not claimed as validated.

Before START, freeze exact source/helper commits and raw hashes. Authorized
validation must run the shared pure version/key fixtures, including lower,
equal and newer tuples, invalid versions, driver independence and unchanged
historical keys. Separately validate real ABI11 loading and same-plan reuse on
the actual current driver, preserving every first failure and full cache
inventories/hashes before and after. Do not change the owner's caches to make
a fixture pass. Prepare any cold plan only in an explicitly authorized slot
and private cache; no preparation belongs in a timing visit.

The [helper benchmark plan](uct_helper_batching.md) remains paired ABBA/BAAB,
unprofiled whole-worker and submit-to-callback measurement. A cache-policy arm
uses the same helper source and exact plan bytes on both sides. Schema-1 BF16
changes and future schema-2 runtime integration are separately identified.
Shared desktop/video load is allowed and recorded. No performance, quality,
release or universal binary-compatibility acceptance is claimed.
