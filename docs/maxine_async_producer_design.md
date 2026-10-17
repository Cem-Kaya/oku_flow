# Maxine asynchronous producer — measured follow-up, not implemented

The audit requested measurement before replacing this pipeline. One actual
60-second off/on virtual-camera pair measured Maxine frame host-wait p95/p99
7.583/12.469 ms, maximum 80.766 ms and a 201.780 ms model load. On/off UI
heartbeat-delay p95 was 5.010/1.914 ms, and viewport rate stayed about 21 FPS in
both. This confirms synchronous tails, but does not establish a pan-to-present
budget failure. See [raw measurement scope](performance_diagnostics.md).

The read-only design review recommends keeping NP-010 open while the other 35
audit repairs proceed. Moving only `MaxineSuperRes::Worker` is unsafe: the
existing signal-after-SDK order, shared NIS/SR cache, current-frame capture
pairing and base-scene observation identity cross several module boundaries.

## Bounded design

1. A background producer owns Maxine, its own CUDA stream, private pitched input
   and output allocations, and completion events. Ensure, Run and Teardown all
   execute there. Keep the SDK's synchronous `Run(..., 0)` inside that worker;
   toggling its async flag does not establish resource ownership.
2. Admit one in-flight job, no queued GPU jobs and one overwriteable CPU
   descriptor of the latest processed scene/request. When idle, copy the latest
   scene ROI into private input on the main stream and record an owned
   input-ready event. Never lend mutable A/B buffers or the current alternate
   processing destination to a background SDK call.
3. Carry an unconditional camera epoch, frame sequence, successful processing
   serial, settings/configuration generation, ROI, output extent, strength and
   request generation. Reject obsolete results; optional control-request identity
   cannot substitute for this presentation ticket.
4. Poll nonblocking on the presentation clock between camera ticks. Publishing
   uses the existing external-fence reservation: wait for graphics readers, copy
   completed private output to the SR cache on the main stream, signal and
   commit. Keep private output alive until its consumer-copy event retires.
5. Choose the visual semantics explicitly. Exact-current-frame matching produces
   base → SR → next-frame base transitions and can cause sharpness flicker.
   Holding an older SR frame needs older-frame presentation provenance and can
   introduce temporal regressions. Neither policy is a transparent helper change.
6. Immediate current-frame photos/recordings will usually capture the optical
   fallback while SR is pending. Preserving SR-enhanced capture quality requires
   retained original/processed tickets, ordered asynchronous pair admission and
   bounded retention. Annotations need SR content generation; UCT, readbacks and
   agent observations must carry the actual displayed enhancement/source.
7. UI retirement revokes publication and disconnects ownership only. Background
   cleanup waits for private GPU completion before SDK teardown/free. If completion
   is unknown, retain the whole private stream/event/buffer state; never join or
   run SDK teardown on the UI thread.

## Acceptance before implementation promotion

Require delayed-backend ownership/cancellation tests, CUDA copy and publication
fence tests, native capture/observation/annotation/UCT provenance regressions,
and actual pan-to-submission p95/p99, result age, stale drops, queue bounds and
flicker measurements. Recording fidelity needs explicit validation for any changed
capture policy. The present measurement and controlled sleeps prove none of
these new semantics, so no asynchronous speedup is claimed.
