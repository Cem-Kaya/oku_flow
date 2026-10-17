# app module

This module owns Qt startup, application wiring, settings persistence,
interaction orchestration, and recording sessions.

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
