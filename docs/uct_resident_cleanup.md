# Resident UCT cleanup ownership

The recovered resident-quarantine bug was confirmed by source inspection on
2026-10-23. Previously `Quarantine` removed `cachedContext` before converting
its lambda to `std::function` and before `AsyncCleanup::Start` allocated its
object and shared-pointer control block. Worker replacement similarly moved
the cached owner first. Those operations could throw before the existing
thread-start catch, allowing the exact context and its GPU inputs/storage to
unwind while completion was unknown. This was an exceptional host-allocation
safety gap, not a reproduced normal-path GPU use-after-free.

## Ownership order

`Context` reserves an initially empty `cleanupRetention` shared pointer.
`Quarantine` calls the shared `detail::StartRetainedCleanup` terminal handoff:

1. Take a by-value copy of the existing resource owner. This allocates neither
   a resource nor a control block and protects an argument aliasing the cache.
2. Copy that owner into the resource's own pre-existing anchor, without
   allocating. Remove the cache only when it owns that exact resource.
3. Construct the cleanup closure and call `AsyncCleanup::Start` inside a catch.
   Its closure owns the exact context, anchor member identity and drain function
   pointer. It borrows no caller or cache storage.
4. Keep the anchor through device selection and stream synchronization. Only a
   successful drain resets it, while the closure still owns the resource.
   `AsyncCleanup` then clears the closure, releasing resources when their final
   known-complete owner disappears.

Setup exceptions cannot remove the already-established anchor. Failed or thrown
drain work and thread-start failure retain the whole closure until process exit.
There is no retry or circular-owner reset without confirmed completion. Each
resource enters this terminal handoff once, on its owning worker. Cached worker
replacement now follows exactly the same policy as error/timeout/cancellation.
The stream, event, two-second foreground-worker completion deadline, polling,
CPU/GPU proof thresholds and recognition/model code retain their existing
behavior. The caller starts cleanup and never waits for its drain.

This change covers ownership at the terminal cleanup handoff. It is not an
audit or repair of every possible exception before reaching that handoff.
There are no new dependencies, downloaded inputs or third-party source.

## Model-free fixture

`retained_cleanup_contract` is a standalone single-translation-unit C++ fixture
using the actual shared retention helper and `AsyncCleanup`. It links no Qt,
CUDA, TensorRT or model code. Its private
`OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS` definition exposes thread-local failure
checkpoints only in that executable. Production targets contain none of those
hooks or the fixture allocator; there are no global allocation overrides.

The 35 authored cases comprise:

- 16 setup faults: simulated submitted-work entry after error, timeout,
  cancellation and cache replacement, each with `bad_alloc` at closure
  conversion, object allocation or control-block allocation, plus a thread-start
  `system_error`. Conversion/object/thread faults are injected immediately before
  the named operation; the actual `shared_ptr` rebound allocator raises the
  control-block fault. This tests real closure unwinding and the actual
  `AsyncCleanup` thread-start catch without relying on allocator sizes.
- 12 gated outcomes: those four entry reasons each with confirmed completion,
  returned error and thrown drain work. Pending work must keep exact context,
  input, integral and scratch identities alive. Confirmed completion must destroy
  each once; unknown completion must destroy none.
- Unrelated-cache preservation and completion already available at handoff.
- One gated case discards the cleanup receipt and leaves its caller scope
  before releasing work, matching production `Quarantine` ownership.
- Four independent closure-lifetime cases use a captured token with no context
  or self anchor: success, returned error, thrown drain and thread-start failure.
  A fixture-only `TestHasSelfRetention` observation after terminal acquire
  verifies cleanup's own cycle without depending on worker-exit scheduling.
  Capture destruction counters and weak cleanup/capture witnesses separately
  verify failed closure retention after dropping the final caller receipt.

The fixture drops original caller/cache owners, checks all reached fault stages
and verifies the self anchor. Failed setup/drain cases deliberately retain their
small fake resources until fixture-process exit. The entry reasons model arrival
at the terminal handoff; they do not execute CUDA `Finish`, its real timeout or
its cancellation polling. Source review covers that unchanged routing.
Named case output and an exact invocation-count assertion preserve complete
coverage accounting. The independent cases were added after read-only review
flagged receipt-discard and closure-witness gaps in the original 30 cases.

## Scoped CPU validation

Root validated the frozen, clean implementation commit
`356583f8e1aeb546bc501978d06b384a36885d9b` on 2026-10-23. Its tested tree is
`49927a9d74ec27f2d0c16ea909936cbc4de91c1f`. The first fixture build completed
in 2.04 seconds using VS2022 Hostx64/x64, Qt 6.12.0, Release/Ninja and CUDA OFF.
The tracked `agent_check.ps1 -Base b86e29f...` returned the expected FULL
refusal (exit 2) because test registration changes touch CMake. Root then
explicitly authorized only `retained_cleanup_tests` and the two exact tests:

- `retained_cleanup_contract`: PASS, all 35 named cases, 0.0584671 seconds.
- `shared_source_equivalence`: PASS, 15.4427 seconds.

The tracked gate recorded **SCOPED PASS: 2 of 235 tests**, with zero skipped Qt
rows, skipped tests or unresolved results. Its attempt identity is
`20261023-162937-160-b61d540e`. No application build/test failure or automatic
retry occurred. The initial malformed external wrapper, root's toolchain
bootstrap failure, and a cache-checkpoint mismatch are preserved separately.
The last mismatch expected `Qt6_DIR:PATH`, although successful configuration
used the exact pinned Qt path with `UNINITIALIZED` metadata. Root corrected
that checkpoint without reconfiguring; the first fixture compilation followed.

The actual cache is under the private Z: evidence root
`uct-quarantine-20261023/C1-root-toolchain-repair/cpu-pch-on`; the final
controller/release records are in sibling `C1-root-cache-checkpoint`.
Executable SHA-256:
`9c0b1b0a534f470595fb20cf1432a3e0139a7d0d0408f4fd55f8b6f0e15ca293`.
Release at `2026-10-23T13:29:55.8070068Z` preceded the unchanged
`13:34:57Z` deadline. Owned-child exit was verified before lock release;
`lockFree=true`, with no remaining owned children or compute processes.

A byte-verified archive of 87 evidence files, including the executable/object, cache,
compile commands, source snapshot, all controller attempts, gate ledger,
JUnit and detailed 35-case logs, is retained under
`Z:/Projects/OkuFlow/local-work-data/uct-quarantine-20261023/archive-356583f-C1-20261023T132955Z`.
Its `archive_receipt.json` binds the tested source, executable and manifest.
Original logs and failed prerequisites remain untouched. The validation
documentation is a subsequent docs-only commit; it does not change the tested
implementation identity above.

The cache has `OKUFLOW_USE_PCH=ON`, but this standalone fixture explicitly
disables PCH, so a PCH-OFF repeat would exercise the same sole TU and was not
performed. This proves the model-free helper/cleanup lifetime contract, not
actual CUDA `Context` fault routing, driver completion behavior, inference,
native GUI/hardware quality or the full shipping/CPU/CUDA release gate.

## Finite CPU plan and remaining acceptance

The original finite plan below remains a reference for a separately authorized
future run. The completed root slot grants no continuing compute or lease.
Any further run needs a fresh finite `ROOTSTART`; the coordinating root may
request a maximum 900-second, eight-job CPU-only window. Stop on the first failure; preserve its
original output and do not repeat to obtain a pass. Use the existing tracked
MSVC 2022 environment and Qt 6.12.0, with new build/evidence storage under
`Z:\Projects\OkuFlow\local-work-data` and the CPU preset's expected build path
preserved. Do not replace a populated build directory or run another agent's
build concurrently.

Proposed sequential budget: environment/build-path inspection plus CPU configure
at most 240 seconds; build only `retained_cleanup_tests` at most 300 seconds;
one tracked fixture gate at most 120 seconds; reserve 240 seconds for evidence
and normal child shutdown. The root's finite runner must enforce the aggregate
900-second deadline. The commands below are a plan only:

```powershell
$env:OKUFLOW_AGENT_EXCLUDE_GUI = '1'
$env:CMAKE_BUILD_PARALLEL_LEVEL = '8'
cmake --preset msvc-cpu
cmake --build --preset msvc-cpu-build --target retained_cleanup_tests --parallel 8
pwsh -NoProfile -File scripts/run_test_gate.ps1 -Preset msvc-cpu-tests -Test retained_cleanup_contract -Parallel 8 -ExcludeGui
```

This would be SCOPED CPU ownership evidence only. Because registration changes
touch CMake, normal `agent_check.ps1` selection requires the full gate; this
explicit single-case plan does not replace it. The tracked shipping/CPU/CUDA
matrix and real-GPU ownership acceptance remain separate, unrequested work.
