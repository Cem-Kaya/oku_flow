# Sol R4 read-only cache/resource review

Frozen clean HEAD: `8f04e4f6d9cf853b6676b08c5ad739f3b70d75f2`.
Reviewed delta: `40f8fb4..8f04`, 2026-10-23.

**Verdict: no new P1/P2 found. Prior Sol P2-R1 is closed in source.** This is source closure, not executed Windows persistence or release acceptance.

## Prior P2-R1: closed

- `src/common/uct_engine_cache.hpp:8-20` defines the shared bounds without addition overflow. Cold payloads require `0 < P < 256 MiB - 32`; warm digest-prefixed files require `32 < F < 256 MiB`. Thus the largest accepted payload produces exactly the largest accepted file. The checks also reject unsigned maximum values and negative context sizes; context size may equal 256 MiB.
- `src/common/uct_tensorrt.cpp:503` uses the shared warm-file bound. Cold builds check `PayloadFits(plan->size())` at **567-568**, before deserialization, QByteArray size conversion, hashing or publication. Both cold and warm engines converge on the same contract checks.
- BF16 tensor/profile/class validation is now at **626-642**, followed by declared context-memory validation at **643-644**, before `createExecutionContext` at **646**. The normal recognizer therefore no longer bypasses the historical resource ceiling or allocates the context before checking its selected-capacity contract.
- Bound refusals use `artifact`/`reject`, which check CUDA error state before throwing a typed BF16 artifact refusal (**454-457,580-585**). The recognizer catch (**1740-1755**) and detector catch (**1693-1711**) still require confirmed drain before discarding and selecting FP32. Context creation failures remain `TensorRtApiFailure` (**646-649**); CUDA/API/allocation faults and unknown completion do not become alternate-engine permission.

## Cache publication and ownership

`engine_cache::WriteNew` (**22-39**) uses a same-parent QTemporaryFile, writes exactly SHA256 plus payload, checks both write counts and flush, then uses QTemporaryFile's non-replacing rename. It avoids the prior external MoveFileEx operation against Qt's retained handle. Auto-removal stays enabled on every failure and is disabled only after successful rename. An existing destination, including a racing winner, is preserved; there is no overwrite, delete, migration or repair branch.

`PreparePinned` now saves only after the common engine contract and successful context creation (**650-654**). A false save result changes only `cacheStatus_`; it does not reset the engine, fail readiness or select FP32. `PlanReceipt` reports `loaded`, `saved`, or `not-saved: <error>` (**659-663**). An existing refused/corrupt stable file can still prevent persistence of a rebuilt engine; that is the preserved new-only policy, not a new regression.

The raw QByteArray passed to WriteNew borrows `PublicResources::serialized`, which remains attached to the retained Network throughout this synchronous call. No new detached owner or callback was introduced. Network member ordering (**723-731**) retains build resources beyond context/engine/runtime destruction; successful preparation releases build resources only after confirmed completion (**1856-1857**). Existing fallback and retirement ownership is unchanged.

## Fixture, documentation and policy

The new `engineCachePersistsWithoutReplacing` slot checks surviving bytes after temporary destruction, digest/payload identity including embedded NUL, unchanged existing bytes, failed-open/empty-payload behavior, temporary cleanup and allocation-free size boundaries. It is included in `uct_paddle_processing`; the new header is an absolute REQUIRED_FILES input. I inspected the complete new slot and changed registration.

All four changed documentation files were reviewed. They describe the current bounds/save behavior and mark semantic activation as historical. Normal runtime preference still bypasses strict golden/v2 machinery. No accuracy, FP32 agreement or corpus gate is requested. Prior deadline, cancellation, negative-memo and discovery closures are not reopened. Packaging/SBOM/licensing and clean-VM acceptance remain the known separate limitations.

## Coverage and unread limits

Consulted `agents.md` end-to-end and the required README, developer guide, documentation index, hardcoded-path and third-party-license documents, focusing on relevant current policy/toolchain/dependency sections. Read the supplied complete Sol and Opus reports. Reviewed every changed hunk in all nine delta files, complete PreparePinned/WriteNew/PlanReceipt and the new fixture, plus surrounding BF16 profile, preparation, fallback, completion and resource-lifetime paths.

No builds, tests, compiler/helper execution, runtime/API/GPU/model operations, desktop/browser interaction, downloads, or owner cache/payload/build-log inspection occurred. Only this report directory was written. No nested agents were used. The pinned Qt implementation was not independently audited (the documented local qtbase source path was absent); Qt rename semantics are a library-contract dependency, not new native evidence. The fixture was not run here and does not inject short writes, disk-full/flush failures, concurrent writers or crashes. Power-loss durability, actual cold/warm startup, full unchanged inference/kernel/controller logic and installer behavior were not validated. Root's reported six successful startups are supplied context, not independently verified acceptance.
