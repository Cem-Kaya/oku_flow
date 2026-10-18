# Asynchronous Maxine producer

The independent producer is implemented in the audit branch. Maxine model
loading, synchronous SDK execution, private GPU completion and teardown run
outside the Qt/presentation thread. NVIDIA's `Run(..., 0)` remains synchronous
inside the independent worker; changing the SDK async flag is not the solution.

## Ownership and admission

`MaxineAsyncProducer` owns one low-priority nonblocking CUDA stream, pitched
input/output allocations and input-ready, inference timing and consumer-done
events. Its constructor starts the worker; allocation and SDK setup occur there.
The adapter's existing deadline worker is called only from that producer.
Neither worker borrows mutable processing A/B buffers, a presenter, an app
pointer or a camera-owned texture.

There is one in-flight GPU job and one overwriteable CPU request descriptor.
A request contains dimensions, strength and an unconditional ticket, never a
source pointer. Once the slot is ready, the presentation stream snapshots the
current full-scene ROI into private input and records input-ready. The private
stream waits on that event before inference. Later camera work may replace the
full scene because the snapshot was ordered before that replacement.

The ticket includes camera epoch, frame sequence, successful processing serial,
settings generation and request generation. It exists even when optional agent
diagnostics are disabled. A completion must match every ticket component and
the current request. A stale result releases its slot without exposing pixels.
Only its timing/configuration may contribute to the performance guard.

Native image edits advance `cudaProcessingGeneration_` and immediately revoke
pending publication through `InvalidateSuperResRequest`, before another camera
frame arrives. This path does not depend on Assistant/MCP being enabled. It
preserves the healthy model and existing performance/failure latches.

## Presentation and capture policy

The current camera frame is presented immediately through its spatial/base
fallback. A completed neural result may replace only that exact processed
scene; a newer camera scene, settings/request generation or source invalidates
it. There is no older-neural-frame hold or retargeting onto fresh pixels.

This policy can produce base-to-neural sharpness transitions. If inference is
slower than camera arrivals, stale results are dropped and the live view keeps
its current-frame fallback. This deliberately favors current-frame identity and
responsive navigation over retaining older neural pixels. No flicker, optical
quality or native latency improvement is claimed without a native measurement.

The existing presentation clock polls completion between camera arrivals.
Publication reserves the existing external CUDA/graphics fence, waits for all
graphics readers, enqueues the output/alpha copy and signals/commits its exact
reservation. Without an external semaphore, both APIs are polled for quiescence
and graphics does not read the new cache until its CUDA copy has completed.
There is no presentation-thread SDK call, stream synchronization or worker join.

The private output remains exclusively leased until the consumer-copy event
retires on the producer worker. Reconfiguration and cleanup cannot overwrite
or free it earlier. Navigation admission and its 150 ms tail still prevent new
inference snapshots; model/guard state survives ordinary navigation.

Photos and recordings retain immediate same-frame pairing. They capture the
processed viewport chosen for that camera presentation, normally spatial/base
while SR is pending; they do not wait for neural output or pair an older SR
result with a newer original. Adding a delayed, ordered SR-quality recording
path would be a separate capture policy, not an implicit change here.

Async publication advances annotation content authority. UCT distinguishes
base/spatial from neural presentation even for the same camera frame, revoking
accepted text, held recognition and pending readbacks across that transition.
Agent observation metadata states the actual selected enhancement, source epoch,
frame and cache generation. Source/geometry coordinates remain full-scene.

## Cancellation, failures and diagnostics

Reset/invalidation revokes desired/publication authority immediately. It does
not join a worker or tear down a live SDK call. Shutdown is a stop request;
the worker drains private producer/consumer work and tears down resources itself.
An SDK deadline or unknown CUDA completion retains the entire private closure
until process exit. The retaining self-reference requires no failure-path
allocation. A private SDK failure disables SR, while healthy base presentation
continues. Uncertain work on the shared presentation stream retains that stream
and its external reservation through the existing terminal GPU-fault policy.

Ordinary configuration failures remain latched until a changed configuration or
explicit reset/retry. `Invalidate(false)` revokes publication only; the default
strong invalidation also permits retry, including a failure completing after
that reset. A deliberate retry re-probes the runtime off the caller thread.
Faulted, potentially still-running SDK owners are never reused.

`SuperResStatistics` reports submitted/completed jobs, stale discards, overwritten
CPU descriptors and the age of the last accepted result. Timing comes from
private CUDA events, including stale jobs for the matching configuration, so
late-result starvation cannot evade the existing 10-warmup/60-sample, 24 ms
performance guard. These counters are not optical or sensor-to-photon evidence.

## Validation scope

`maxine_async_cuda` uses real CUDA with private fake SDK DLLs to check immutable
input snapshots, one-slot/coalesced admission, all five ticket components,
consumer-copy lifetime, revoked shutdown and explicit failed-configuration retry.
`maxine_navigation_cuda` drives the actual CUDA surface and shared D3D12 fence,
checks immediate current-frame fallback, exact neural pixels and ready-model
reuse, plus immediate settings invalidation without tearing down that model.
`maxine_lifecycle_cuda` retains the adapter's deadline/late-return cases.
The UCT offscreen fixture rejects held base-image recognition after neural
publication. The opt-in native-app offscreen fixture checks image-settings
generation with the agent target absent. All fixtures keep native windows,
cameras, phones and the owner's runtime untouched.

The background matrix for the first implementation passed shipping Release
compilation, 83 selected CPU tests and nine CUDA delta tests. One optional
development MCP Qt row was skipped; no selected CTest case was skipped or
unresolved. GUI was explicitly excluded. After the final presentation-clock and
native-settings refinements, Release and CPU compilation passed again; scoped
checks passed for the two affected CUDA fixtures, three CPU cases (scheduler,
UCT and shared-source equivalence), and the native-app offscreen fixture.
The adapter lifecycle fixture also passed after the clock/ownership changes.
The runner contract completed all 517 assertions; its calling shell returned
1 by propagating an intentionally failing child fixture's stale exit code.
That original wrapper log is retained rather than relabelled a clean command
exit. Evidence is under `%LOCALAPPDATA%/OkuFlow/maxine-async-20261017`.

The historical actual 60-second off/on virtual-camera pair measured frame host
wait p95/p99 7.583/12.469 ms, maximum 80.766 ms and a 201.780 ms model load. It
predates this producer and cannot measure its improvement. Actual native
pan-to-submission p95/p99, acceptance/drop age, sharpness transitions and
recording quality remain separate acceptance. See [performance evidence](performance_diagnostics.md).
