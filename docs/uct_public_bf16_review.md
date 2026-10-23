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
   At the R1 repair the deadline started before the detector; the later Opus
   follow-up below corrects that scope. Cancellation is checked before
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
deadline/positive runtime behavior is unverified. Root reported focused R3
complete on `89f839b03285862aa00f082e057c7a4c2d15dc70`, task
`node:delegated-task:command%3Amcp%3A818e8417-f389-479c-bab6-e9b3eca72944%3Adelegate-task%3Auct-public-bf16-final-deadline-closure-r3-20261023`.
R3 closed R2's P2 with no P1/P2: exactly one guard after `RunPublicGolden` and
before `CheckGolden`, ordinary-error routing, retained incomplete observations
and unreachable publication on expiry. Root also reported 11 preservation files
unchanged and receipt/patch hashes matched. This narrow source closure is accepted;
it is not runtime acceptance. ProductionCorpusCatalog remains empty and all
previous evidence is retained. The combined source includes `b002`'s test-only
Qt identifier repair once; see [integration scope](uct_combined_source_2026_10_23.md).

## Root-reported Opus audit: deadline scope repair

Root supplied the substantive independent Opus audit, task
`node:delegated-task:command%3Amcp%3A818e8417-f389-479c-bab6-e9b3eca72944%3Adelegate-task%3Aopus-public-bf16-quarantine-seams-source-review-20261023-r1`.
It reported no P1, two concrete P2 findings and one probable P2. Root reports
the prior R1/R2/R3 closures retained; earlier findings and artifact hashes are
not replaced by this follow-up.

**P2, BF16 deadline applied without an attempt:** the worker-start 600-second
clock included ordinary FP32 detector preparation, even when the production
catalog was empty. A later pre-candidate/fallback check could therefore fail
ordinary FP32 preparation. The source repair uses cancellation alone for baseline
work, runs existing eligibility/catalog admission before starting the candidate
clock, and retains cancellation-only fallback when admission refuses. Once an
actual candidate starts, its deadline/runtime-fault policy and normal retained
retirement remain unchanged. Existing model-free eligibility/baseline seams gain
candidate admission and disabled-request cases; they do not execute or simulate
the worker's elapsed-clock sequence. All changes remain UNBUILT/UNRUN.

The separate cache-builder repair follows; native `DirectoryPins::Hold` and its
existing sharing fixture remain unchanged pending root's real fixture result.
The probable attribute-only-sharing finding is not repaired or declared closed.
Root retains the initial combined CPU compile failure and owns the test-only
`TextQuad` near/far rename needed to run the unchanged native fixture. This
implementation checkout executes no build/test/native API and imports no such fix.
Fresh focused independent closure of these new repairs is owned by root.

**P2, stale builder generation masks a new recipe:** `FindPlan` previously returned
the first internally valid child regardless of the actual current builder, then
the caller adopted that child's builder version. A TensorRT upgrade could keep
retrying an old non-version-compatible plan on every launch instead of preparing
the current builder's recipe. The separate cache repair validates every visited
receipt identity and plan hash, skips valid children with a different builder,
and leaves the caller's actual builder identity unchanged. A missing current
recipe returns missing; no alias/delete/migration or runtime admission pin is
introduced. The 32-entry scan bound, ABI11/minimum runtime tuple and driver policy
are unchanged.

The existing model-free publication slot gains two-builder selection in both
directions, missing matching builder, unchanged bytes/inventory and corrupted
nonmatching-child refusal. These are labelled integrity-only fixture bytes with
no golden/pass field, not manufactured model success. They and both source repairs
remain **UNBUILT/UNRUN**, awaiting root's targeted independent closure and fresh
finite execution grants. The probable native sharing issue remains open.

Root subsequently reported a combined CPU run on `a7` with public production
bytes identical to `89f839b03285862aa00f082e057c7a4c2d15dc70`, after the separate
test-only near/far rename. That gate passed 5/6 cases: approved-corpus contract
passed, but `publicBf16PublicationAndLifetime` threw an unhandled C++ exception
(`0xe06d7363`) at 10 ms without `error.what()`. This is not a diagnosed sharing
assertion failure and does not cover these newer policy/cache repairs. Root
retains the first log at
`Z:/Projects/OkuFlow/local-work-data/uct-combined-validation-20261023/scripts-C2/C2-20261023-151300-d37e040a277e452b88b7cba0f926327e/cpu-tracked-gate.log`
and reports release at 15:13:59, lock-free/job0. A separate bounded native probe
is root-owned and pending; this checkout executes nothing and preserves the
original denial/new-only assertions without a behavior or diagnostic workaround.
