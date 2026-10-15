# Agent control core (development)

The immutable, Qt-free `ControlRegistry` is shared by provider and MCP adapters.
Its `dev-core`, `dev-capture`, and `dev-camera` editions are development
contracts. `v1` requires plan 52's complete both-adapter acceptance gate.
Development bridges stay outside release inventories.

`AppControlService` owns GUI-thread admission, strict validation, context
arbitration, exact approvals, scoped grants, field revisions, bounded events,
idempotency, budgets and conflict-aware undo. Providers submit `ControlRequest`
and receive `ControlResult` callbacks. IPC workers first post onto the GUI
thread. Only trusted app UI invokes permission/lifetime methods. The built-in master defaults on and external MCP defaults off; saved choices
are preserved. Session image/capture grants stay off. Permission deadlines use a monotonic, fixture-injectable clock.

`AppControlTarget` adapts typed fields to existing production transitions.
It detects no-ops, preserves zoom/pan preset identity, rejects unsupported preset
differences and saves once after a composite delta. It never clicks buttons or
synthesizes input. A failed save leaves values live until exit; restart loads
the last committed file.

`OperationTracker` retains terminal outcomes for fifteen minutes. Photos remain
pending through paired commit; recording cleanup checks exact session ids.
Safe summaries expose bounded names/codes. Accepted writing/finalization work
survives tool-delivery cancellation.

Current core fixtures run under QCoreApplication. Observation freshness,
permission UI, async camera transitions, accessibility, both-provider integration
and genuine MCP acceptance remain required before enabling the complete feature.

Observation freshness uses the original processed timestamp with a 1000 ms monotonic bound; re-presenting cached output cannot refresh it. Automatic adjustment refuses held/converging or unknown temporal state. Image observations may still describe stale output truthfully.

Production camera selection is now wired through the split asynchronous capture retirement and ordinary background start. Accepted source changes return a durable pending operation; normal turn completion permits that admitted operation to finish, while owner retirement/cancellation/native source choice retires its generation. Camera failure restoration is requested explicitly and is not reported as already active. Hardware first-frame, GPU cleanup and physical USB acceptance remain unvalidated.

Grouped native parameter integration is in development. All eight finite parameter groups route to production handlers, including asynchronous camera/format/acceleration/torch transitions and exact microphone-stop preference operations. Output destinations use handles for native-approved directories. Approval DTOs show canonical normalized changes, and grouped receipts use registry enum values. Whole-patch preparation performs no filesystem writes. Successful desktop-free compilation and synthetic fixtures do not establish camera, microphone, native desktop or V1 release acceptance.

The default immutable application catalog remains `dev-camera`; `dev-full` exposes the expanded 94-field contract only in explicitly selected development builds. Native source choices supersede actor generations, and actual producer activation invalidates earlier pixel permission. Torch retries use a native-only unacknowledged marker, bounded cancellation/deadline tokens and ACK-derived effective state; provider arguments cannot request that marker.

Native decision-time synchronization now protects dispatch, approvals, delayed preference commits and Undo. BeginFieldPlan validates without effects; CommitFieldPlan stages coupled writes only after inverse capture, and failed batches retire processing provenance even after successful restoration. Device-qualified acceleration and temporary source/format journals preserve native user wins. Discovery reports native support separately from acceptance. The opt-in actual-app CPU/dev-full fixture uses production controls with isolated settings and no platform, device or provider bootstrap; its offscreen and SDK evidence covers only the executed native subset.
