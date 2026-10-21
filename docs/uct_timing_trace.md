# Private UCT software timing trace

Set `OKUFLOW_UCT_TIMING_TRACE=1` before launching a private diagnostic build.
The process caches this flag on first use. Ordinary launches leave it disabled;
event hooks then return through a cached branch without clock reads, recording
allocation or file access. No model preparation/download is requested by tracing.

An enabled process retains at most 4096 metadata records (under 512 KiB of
record storage). Producers use a try-lock and drop on contention or capacity;
they never wait for trace storage. There are no new workers, inference contexts,
capture/OCR queues, pool entries, timers or pacing changes. Source ownership,
admission policy and shutdown deadlines remain unchanged.

Every record contains only numeric correlation and a fixed event name: producer,
channel, source epoch, geometry generation, actual frame/acquisition, job,
scheduler revocation authority, original source timestamp, monotonic steady
nanoseconds, numeric status/count/flags, related corroborator job and publication
revision. Events cannot contain arbitrary strings, paths, recognized text,
pixels, digests, model identifiers or credentials. `DeviceInput` describes input
residency, independently of `GpuBackend`. `BackendKnown` and `GpuBackend` come
from the recognizer's existing status; `InferenceAttempted` identifies an actual
recognizer call. Backend state is numeric, and backend class names are omitted.
No claim about Windows/Paddle/TensorRT class is inferred from a status name.

Source PTS and the steady clock remain separate. A worker can resolve unknown
hold acquisition identity from its owned image; unknown-to-known is permitted,
whereas conflicting known identity in one job makes its chain incomplete.
Authority is the scheduler authority, fenced whenever source/seek/geometry
authority changes. This is software-event timing, never sensor/input-to-photon
latency. Submit/dequeue/done/UI-receipt/admit show distinct boundaries, and
backend state, cancellation, reset and fault are explicit records.

The controller captures publication provenance from the result actually used
by model admission. A paused neighbor's Admit record uses the exact selected
result ticket and names the neighbor with `related_job`. It never relabels the
neighbor as selected evidence. Positioned overlay/accessibility tickets are
empty for reading-only evidence. Changed content promotes the captured ticket
and increments publication revision; pan/reprojection keeps the ticket/revision.
Clear/reset uses an empty ticket. Mixed retained proof is flagged as an
aggregate artifact: the ticket identifies the latest model operation, and does
not claim every retained word was first proved by that job. Partial revocation
with surviving text identifies the operation that caused the revocation.

Publication consumers distinguish assignment from actual software publication.
`OverlayAssigned` is content assignment; `OverlayPainted` is an actual widget
backing-store paint for that content, not export rendering or photon delivery.
`PaneAssigned` describes a changed text assignment. Accessibility scheduling
and publication carry the ticket/revision copied when content was scheduled;
they do not sample the controller's current frame at timer expiry. Coalesced
retained/new lines carry aggregate provenance. These hooks do not certify a
screen reader received or spoke the text.

The first normal controller teardown closes the recording window after its
existing owned worker/callback retirement, without adding a wait. A process-exit
fallback covers standalone workers. Enabled tracing flushes once to a unique
`%LOCALAPPDATA%\OkuFlow\uct-timing\uct-timing-<pid>-<steady-ns>-<n>.jsonl`
created with `CREATE_NEW`; no existing output is replaced. All serialization
and file I/O occur after closing the window. The writer uses a classic numeric
locale, caps serialized bytes at 4 MiB, and treats allocation/path/write failure
as a diagnostic failure only. An interrupted or partial file is not evidence of
a complete trace. No automatic upload is performed.

The header reports capacity/contention/outside-window dropped counts and
whether snapshot lock contention prevented copying records. Late events after
closing are outside the trace window; detached work may outlive it. The summary
also latches any unresolved record attempt at close: a producer preempted before
publishing its drop counter cannot make the snapshot appear complete. Closing
never waits for that producer. `closed_at_ns` bounds all copied receipts,
including a writer that already passed its close check. This overlap is reported
explicitly and disqualifies complete-chain timing. The summary counts known
incomplete/cancelled/faulted chains and mismatched tickets. Any
capacity/contention loss disables complete-chain latency eligibility because
entire missing jobs cannot be counted. Recognition chains require submit,
dequeue, done, UI receipt and an admission operation; corroborator/probe/device
validation jobs require the first four. A fully observed chain does not imply
admission succeeded. This first summary covers worker/controller phases only;
publication records must be joined by exact admission ticket plus revision,
and missing/dropped publication stages cannot be treated as completed paints.

Validation inputs are `uct_timing_trace_contract` (disabled path, exact bound,
drop/close accounting, deterministic contention, concurrent ticket integrity,
fault/cancel/incomplete chains and JSON schema) and
`uct_timing_controller_contract` (real scripted two-frame paused admission and
exact worker/UI/admit correlation). The controller case uses Qt offscreen and
downloads nothing; its ordinary-suite invocation checks the disabled path.
`uct_screen_text_timing_publication` separately exercises consumer tickets and
publication behavior. These are hardware-free fixtures; actual captured/UI
trace timings and native screen-reader behavior require separate acceptance.

Pane and positioned publication keep separate tickets. A pane-only selectable
reading-text change allocates a pane revision and updates only the panel's
ticket. Overlay/accessibility retain the exact earlier positioned ticket until
`SetWords`/`ContentChanged` actually changes positioned content, or a revocation
clears it. A common monotonic allocator may give simultaneous positioned/pane
changes the same revision; it does not reticket unchanged surfaces. In
particular, panning after a changed held/read-only OCR token cannot generate a
new content-paint or accessibility-publication record for the pane revision.
`uct_timing_pane_publication_contract` exercises real controller/worker admission
with proved glyphs and two differently recognized, still-unproved held tokens
at the same protected rectangle. It checks unchanged glyph identity/geometry,
changed pane assignment, and no new positioned publication on a later real
offscreen paint and pan. Each staged 80-ms fake-clock step freezes until
worker completion, the model's exact-frame validation and any recognition
admission callback are consumed. Initial live acquisitions establish proof;
new held acquisitions obey the ordinary 1500-ms refresh gate, and repeating
one held acquisition supplies validation without another OCR observation.
A fixed bounded accessibility cadence is advanced only through these completed
validation stages before comparison. Rapid fake-clock advancement with queued
receipts would correctly expire the 350-ms proof and schedule restored text,
which is separate from this pane-only publication contract. Negative event
assertions require zero capacity/contention loss
and no unresolved attempt at close. Its separate process owns one trace window.
