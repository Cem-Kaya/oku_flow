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
   ancestors before creation, and retain existing/new ordinary ancestors through
   reads, writes and new-only rename of exact held staging. The initial sharing
   exclusion request proved ineffective in root's later native probe; the separate
   pin repair below corrects it. The explicit root must already exist. No
   replace/copy fallback is used.

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

At the policy/cache handoff, native `DirectoryPins::Hold` and its existing sharing
fixture remained unchanged pending root's real fixture result. The then-probable
attribute-only-sharing finding was not repaired or declared closed at that point.
Root retains the initial combined CPU compile failure and owns the test-only
`TextQuad` near/far rename needed to run the unchanged native fixture. This
public-author checkout did not import that separate fixture fix. The combined
checkout retains `a7d4757`'s exact eight identifier replacements. Composition runs
no build/test/native API; focused independent closure is owned by root.

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
finite execution grants. At that handoff the probable native sharing issue was open.

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

## Root native probe and separate directory-pin repair

Root completed a new isolated filesystem diagnostic (not a failed-test retry) at
`2026-10-23T15:17:27.2730543Z`, in 1.41 seconds, with job active0, no helpers or
termination and verified lock-free release. Evidence is under
`Z:/Projects/OkuFlow/local-work-data/uct-public-native-pin-probe-20261023/C2-20261023-151725-980723380368444a8f1cd08b86d68819/observations.json`
and sibling `receipt.json`. The two scenarios confirmed:

- Attribute-only access/share-READ allowed outside DELETE and WRITE|DELETE opens
  (error0); exact-staging DELETE and new-only rename also succeeded.
- List/attribute access/share-READ|WRITE excluding DELETE rejected both outside
  opens (error32), while exact-staging DELETE and the same new-only rename succeeded.

This confirms Opus's probable P2 as a platform defect, separate from the earlier
unattributed Qt exception. The probe wrote its non-model file before staging was
held; it did **not** establish that file creation while the staging handle is held
succeeds or explain the Qt exception at the first production publication.

The separate source repair requests FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
shares READ/WRITE without DELETE, and retains DELETE on exact staging for rename.
All original junction, denial and atomic-new-only assertions remain. Additional
cases check DELETE-only refusal, a non-delete writer and actual `PublishFilesNew`
under an already pinned parent, with new-only collision and unchanged byte checks.
The production publisher still holds staging **before** creating/writing its
files. A narrow outer private Qt-slot catch reports unexpected `std::exception`
via `QFAIL(error.what())`, without changing expected-refusal or other assertions.

This implementation agent ran no native API, tests or build. Project changes
remain **UNBUILT/UNRUN**; root owns fresh focused source closure and the one
changed-fingerprint failed-case execution, retaining the first failure. No model
golden success, public activation or causal diagnosis follows from this probe.

## Root source inspection: per-generation read-pin lifetime

Root's later read-only inspection identified another concrete source defect:
`FindPlan` retained root/ancestor and every visited child handle in one
`DirectoryPins` object. Its ten-handle path capacity could therefore fail after
roughly seven skipped child generations, before the advertised 32-entry scan
bound or a current-builder match. This inspection was not an executed test and
does not attribute the earlier Qt exception.

The separate narrow repair retains root/ancestor pins for the complete lookup,
but uses a distinct `DirectoryPins` in the loop's iteration scope for each child.
Receipt/plan reads and every digest/identity/confinement check stay pinned. A
skip closes that child; a return copies owned bytes/metadata before the local
pin closes. The path capacity, inventory limit, runtime admission, builder filter
and retry/fault policies are unchanged.

The existing integrity-only fixture gains a full 32-generation missing-builder
walk, successful current-builder selection, unchanged inventory and a 33rd-entry
bound refusal. It creates no golden/pass record or model success. Source changes
and fixtures remain **UNBUILT/UNRUN**; root's fresh targeted independent review
must include this lifetime seam alongside the prior repairs. All previous probe,
review and first-failure evidence is retained; no new execution or retry ran here.

<<<<<<< HEAD
## Combined source follow-up and pending R4

The combined checkout imported the four source commits through
`6d5f4744b4f7e0f4c2e0d8442b58373475bb7f14` onto clean
`a7d4757877278b82f42191df2e88ad6be4670a2e`. Only this review history conflicted;
root's completed narrow R3 closure and the new findings were retained. No manual
production conflict resolution occurred. All original C1 compile-failure and C2
scoped 5-pass/1-failure logs, ledger and build artifacts remain archived. The
original Qt exception remains unattributed; the separate native probe established
only its stated platform-sharing observations. No later project fixture pass is
implied.

Root's fresh Opus R4 review of the frozen four-repair source is pending:
`node:delegated-task:command%3Amcp%3A818e8417-f389-479c-bab6-e9b3eca72944%3Adelegate-task%3Aopus-public-bf16-four-repair-closure-20261023-r4`.
Source composition is not review acceptance or an execution grant. The repairs
and new cases remain UNBUILT/UNRUN; production catalogs remain empty. See the
[current narrow CPU proposal](uct_combined_source_2026_10_23.md#current-held-four-case-cpu-proposal).
=======
## Fresh independent R5 and WRITE-sharing repair

Root supplied the full independent report at
`Z:/Projects/OkuFlow/local-work-data/uct-review-implementation-20261023/public-repair-closure-r5/review.md`
and sibling `coverage-index.json`, both retained with hashes in the source receipt.
R5 read frozen clean `6d5f4744b4f7e0f4c2e0d8442b58373475bb7f14`, covering the four
commits after `89f839b`. The failed R4 rate-limit attempt adds no coverage. R5
closed deadline scope, actual-builder lookup and iteration-local child pins **in
source**, and found no new P1. Native confinement remained **OPEN P2**: a
GENERIC_WRITE handle admitted by FILE_SHARE_WRITE could apply
FSCTL_SET_REPARSE_POINT after the ordinary-directory check, then redirect later
path-based mkdir/file operations. DELETE exclusion alone was insufficient.

The separate repair keeps list/attribute access and DELETE only on exact staging,
but shares READ only, excluding WRITE and DELETE. The incorrect positive writable
directory expectation becomes denial. Private `SetFixtureJunction` reuses the
existing real FSCTL path with all share flags on the writer; the original
create-junction control still uses it. New ancestor/staging cases attempt reparse
conversion after holding the initially empty directory, then require refusal,
post-hold child writes and unchanged outside inventory. If a regression permits a
junction, cleanup releases the pin and removes that link without traversing its
target. All original junction/new-only/collision/byte assertions and the complete
production publisher's hold-then-write-then-rename body remain intact.

## Preserved C3 failure and bounded diagnostics

Root's `00dd9f9da9a0bce37e8f7e123a804d16720753c5` composition had Git-identical
public/fixture bytes to frozen `6d5` plus its separate near/far fix. C3 compiled
three CPU targets, then the tracked 6/238 selection finished **5 PASS / 1 unresolved**
with zero skipped rows. The publication/lifetime slot now reported
`unknown public BF16 cache generation` through Paddle line301 at 221 ms. Receipt
completion was `2026-10-23T15:40:38.4558642Z`, active children0 and verified release.
The log and receipt remain under
`Z:/Projects/OkuFlow/local-work-data/uct-combined-validation-20261023/scripts-C3/C2-20261023-153944-e0d7f51009824c0dbb691e3dd13c6ec5/`.
The earlier C2 near/far prerequisite failure, C2 generic exception and limited
native probe remain separate first evidence; none is replaced or cleared.

Source inspection confirms the C3 message's single basename-validation throw
before receipt reads. Published fixture keys are checked by the same Hash helper
before publication, but C3 lacks the actual offending entry/lookup phase. No
precise cause is claimed. Refusal now carries compact JSON with fixed phase,
ordinal, entry (128 characters), original entry length and parent path (512
characters); JSON escapes diagnostic text. Fixed fixture breadcrumbs identify
each positive/absent lookup phase. A controlled unknown-generation case checks
the refusal, context and unchanged inventory. The rule still rejects unknown
entries and preserves the existing bounds, digest/identity checks and builder
filter. No arbitrary name is newly skipped.

R5's three source closures remain retained; the WRITE-exclusion source/fixtures
and new diagnostics are **UNBUILT/UNRUN** and await root's fresh independent
review and one changed-fingerprint execution. They do not diagnose C3 or the
older generic exception, prove platform acceptance, approve any model/corpus or
authorize an automatic retry. This agent performed source edits/reads/hashes only.
>>>>>>> 26ebfb8 (fix(uct): exclude reparse writers from BF16 directory pins)
