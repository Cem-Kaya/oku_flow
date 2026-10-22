# app module

This module owns Qt startup, application wiring, settings persistence,
interaction orchestration, and recording sessions.

`app_video_playback.cpp` additionally owns silent live RAM recap selection and
the existing transport, independent of recording/file documents. The clean
Original path keeps buffering during review; bounded presented-scene leases
support exact manual Freeze during encoder warmup. Source/format/stop reset
old recap authority, and jump-live restores camera view intent. See
[recap ownership](../../docs/live_ram_recap.md).
Its selected-video metadata includes the recap error and observed source
dimensions/frame rate; the existing transport status exposes native failure
detail without enabling an empty seek range. Pending Resume retains its play
intent until genuine coded history at the held capture time is readable.
Permanent failure or an unsupported source cancels an impossible pending Resume
with the existing unavailable state/reason, retaining the exact held pixels,
capture clock and Jump to live. Transient native-busy metadata keeps waiting.

UCT's opt-in regional controller pacing supplements baseline full reads with
one idle-only bounded region, without updating full timers or held acquisition
proof. Paused selected/neighbour refinement remains separate. See
[active execution](../../docs/uct_active_regional_ocr.md).

Protected VLM credential ids are immutable generations. A changed key, provider
or endpoint stages a new id before publishing settings; save retries reuse that
staged binding. Previous generations remain available for settings-backup
recovery rather than being overwritten. Plaintext keys and the controller's
runtime `CredentialBinding` never enter settings JSON or diagnostic receipts.

Normal source changes now retire capture through asynchronous producer and
consumer phases in `app_camera_transition.cpp`. Queued camera frames and imports
belong to an independent cleanup worker; the GUI only polls bounded state.
Failed retirement retains exact resources and prevents source reuse. Ordinary
settings saves preserve the explicit saved startup camera. Agent camera tools
remain unavailable until source identity/format and adapter acceptance are complete.

`PipelineOrchestrator` invokes camera work and then the independent
`PresentPendingScene` callback on every tick. A retained D3D11/CUDA handoff
can wait for completion while pan, zoom, and resize continue over the newest
cached scene, including Extra Stable's held image. Viewport redraws do not
advance temporal effects, SuperRes inference, or paired recording.

`RecordingManager` preserves synchronized original/processed video, optional
microphone AAC audio, bounded queues, finalization, and worker watchdogs.
Its terminal state is published only after the old session's queues, flags and
callback identities have retired. Video/audio error epilogues never clear a
replacement session admitted at that boundary.
`RecordingSessionInfo` lives in `include/okuflow/app/recording_session_info.hpp`;
queued saved-segment and session-ended callbacks retain its stable identity.
Saved-media callbacks continue adding recordings and photos to lecture notes.

The Advanced inspector has Image, Assistant, and Settings tabs. Assistant
conversation history, Read, Explain, and manual Read Aloud remain available.
The app persists the Explain conversation id (`explainThreadId`) and lists that
thread in Assistant history as "Explain conversation"; deleting it, or New
while it is open or selected, makes the next Explain start fresh.
Microphone capture runs only while recording and sends PCM to the recorder.
Obsolete recording settings are ignored on load and omitted on save; there is
no settings migration.

Agent control is in development under agent/ (plan 52), with built-in
Assistant control on by default, external MCP off, and desktop-free core fixtures. app_agent_capture.cpp owns named photo,
recording-admission and exact-session stop methods shared by native buttons and
typed adapters. Recording admission precedes microphone acquisition; callbacks
retain operation/session identity through finalization. This is not a published
MCP API or native acceptance claim.

Agent camera owner retirement retires pending source authority immediately. If an owned recording is still draining, producer retirement/restoration waits for that exact recording lifecycle to end, with a generation check so a newer native source choice wins. Temporary webcam/phone discovery intent is separate from the saved automatic-phone startup preference.

Finite assistant preference controls use the pure `assistant_parameter_policy`
planner plus `app_control_assistant_parameters` native adapter. Sensitive config
writes require exact native Configuration approval. Agent Scene Explain is a
separate context-only intent, obtains each image through core Observe budgets,
and preserves unrelated native periodic activity. It never persists a frame
grant or enables legacy periodic egress on the agent's behalf.

`UltraClearTextController` (wired in `app_ultra_clear_text.cpp`) attaches
Ultra Clear Text to `PresentLatestCudaScene`, `PresentFitted` and the existing
presenter readback ring. It never blocks presentation, keeps one copy and one
recognition in flight at most, and invalidates text on camera session,
scene size, rotation, keystone or stabilization changes. See
[`docs/ultra_clear_text.md`](../../docs/ultra_clear_text.md).

UCT now snapshots applied scene identity only at successful processing and
invalidates immediately on camera retirement. Probe work is off-thread; the
GPU readback handoff swaps immutable pooled vectors after other consumers.
Requested/applied geometry mismatch hides text while processing catches up.

Physical camera reopen, format changes and reconnect use the same asynchronous
startup owner as initial capture. Reconnect retains its retry state until the
worker completes and reuses the requested symbolic link without enumerating
hardware on the UI tick. The legacy synchronous comparison remains opt-in for
startup profiling only.

Microphone activation, reader setup, Flush, Shutdown and final COM release now
belong to one independently retained worker. Start returns Disabled/Pending/
Failed; UI callbacks retain both the recording ID and callback generation.
Opening has a three-second decision deadline, after which recording proceeds
without audio. An unretired/abandoned microphone refuses another acquisition
without blocking video or UI. Cancellation immediately revokes deliveries.
Recording remains Starting and discards pre-decision video frames until actual
audio readiness is known; successful start then permits the first segment.
None never claims audio. Shutdown skips process-global MF teardown while the
worker is live or native resources were abandoned. These ownership contracts
have native fixture coverage, separate from physical-microphone acceptance.

Save as Quick Mode commits its unique preset/config to settings before the
native mode list reports success. Failed publication leaves prior modes and
live configuration intact and reports the storage error; shutdown is no longer
required for the new mode to survive a crash.
