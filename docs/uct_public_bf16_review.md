# Public BF16 independent source review and repairs (2026-10-23)

The independent read-only nested R1 review examined
`19d46605bfa9c6d18be004a6e04c181c05146244..f4fd73bf01f1fe8fcd943ab5d590445a842e493d`.
It performed source reads/static hashes only, with no writes, children, builds,
tests, CUDA/inference, desktop, owner state, model/cache preparation or lease.
The reviewer reported three findings; all are accepted and repaired in source:

1. **P1, isolated-cache bypass:** detector construction and FP32 fallback could
   prepare plans into the ordinary cache, outside the private root/deadline.
   `BaselinePolicy` now disables both preparations and supplies the explicit
   private root to every baseline constructor. Missing cached plans refuse.
   The deadline starts before the detector, with cancellation checked before
   candidate/fallback. Normal production and schema-1 defaults stay unchanged.
2. **P2, lost partial golden evidence:** earlier completed outputs were local
   and disappeared after a later failure. State now retains observations and
   actual model/corpus/plan/device/builder identity; each completed call reports
   them. Later failure emits an explicitly incomplete, non-passing record with
   unknown overall completion, without `CheckGolden(Confirmed)` or publication.
   Full mismatches remain complete failed records with exact outputs.
3. **P2, late canonical check:** `mkpath` could create a directory beyond an
   existing junction before refusing. Native directory pins now reject reparse
   ancestors before creation, hold existing/new ordinary ancestors without
   write/delete sharing, and retain them through reading, file writes and the
   new-only rename of the held exact staging directory. The explicit root must
   already exist. No replace/copy fallback is used.

Additional coordinator repairs reject a raced final publication rather than
claiming success, check cancellation before FP32 fallback and retain explicit
completion/compute metadata. Model-free source fixtures cover isolated policy,
failed/partial records, a real private junction with unchanged outside inventory,
native directory pin sharing, storage collisions and retained resource closure.
No fake successful model/plan/golden record is created.

The reviewer confirmed the empty production catalog, candidate pin and all eight
raw hashes/dimensions; attached candidate lifetime, real helper reuse, serial N1/
N8 calls and typed-vs-fault separation. It found no changes to RecognizeDevice,
CUDA helper or UI bodies. The repair adds only an optional baseline cache locator
inside the existing Network constructor; all ordinary key/profile/build behavior
keeps its default. Exact recognition/two-slot and CPU/CUDA arithmetic stay intact.

At the R1 handoff, the repairs had coordinator static review only, **not a second
independent review and not executable acceptance**. Every changed/new fixture and
positive model path remains UNBUILT/UNRUN. Native sharing/rename semantics, vendor lifetime,
real fault/cancel/quarantine, strict golden, quality, performance and production
activation remain separate ROOT START gates. See [finite proposals](uct_public_bf16.md).

Retained R1 orchestration identities (original task):

- Client request: `public-bf16-lifetime-identity-readonly-20261023-r1`
- Provider/model/options: `codex`, `gpt-6.1-sol`, `reasoningEffort=xhigh`, `serviceTier=default`
- Task/node: `node:delegated-task:command%3Amcp%3Aa7e253f2-fae6-41f9-9666-c08b36f8dc38%3Adelegate-task%3Apublic-bf16-lifetime-identity-readonly-20261023-r1`
- Child thread: `thread:delegated-task:command%3Amcp%3Aa7e253f2-fae6-41f9-9666-c08b36f8dc38%3Adelegate-task%3Apublic-bf16-lifetime-identity-readonly-20261023-r1`
- Child run: `run:thread:thread%3Adelegated-task%3Acommand%253Amcp%253Aa7e253f2-fae6-41f9-9666-c08b36f8dc38%253Adelegate-task%253Apublic-bf16-lifetime-identity-readonly-20261023-r1:ordinal:1`

## Root-reported independent R2 and narrow repair

Root supplied an independent R2 review of
`b3ba9c82e29ee8f50818d37e4f1ed591c8431dad`, task
`node:delegated-task:command%3Amcp%3A818e8417-f389-479c-bab6-e9b3eca72944%3Adelegate-task%3Auct-public-bf16-runtime-repair-closure-20261023-r2`.
R2 closed all three R1 findings and covered the full headers, Network,
preparation, storage, golden, fallback and test entry points. It confirmed
unchanged helpers, recognition and UI. This records root's supplied findings;
the implementation agent did not start another review or execute validation.

R2 found one **P2, final-N8 deadline bypass**: the last completion event could
return success after the 120-second golden deadline, with no later golden
deadline check before `CheckGolden` and publication. The separate source repair
checks `goldenStop()` immediately after `RunPublicGolden`, before any
`CheckGolden` call. Expiry/cancellation throws ordinary `std::runtime_error`,
retains all observations in State and reaches incomplete failure reporting and
normal retirement. It cannot enter the typed ArtifactError/FP32 retry path or
publish a receipt/plan.

Existing model-free BF16 fixtures have no clock seam exercising this final-N8
sequence. No helper/framework or mirrored test was added solely for the check.
The repair has source inspection only and remains **UNBUILT/UNRUN**; actual
deadline/positive runtime behavior is unverified. Root's focused R3 closure is
pending. ProductionCorpusCatalog remains empty, prior evidence is retained, and
the separate `b002` test-only Qt identifier dependency is left for later composition.
