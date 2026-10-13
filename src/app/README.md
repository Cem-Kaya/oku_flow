# app module

This module owns Qt startup, application wiring, settings persistence,
interaction orchestration, and recording sessions.

`PipelineOrchestrator` invokes camera work and then the independent
`PresentPendingScene` callback on every tick. A retained D3D11/CUDA handoff
can wait for completion while pan, zoom, and resize continue over the newest
cached scene, including Extra Stable's held image. Viewport redraws do not
advance temporal effects, SuperRes inference, or paired recording.

`RecordingManager` preserves synchronized original/processed video, optional
microphone AAC audio, bounded queues, finalization, and worker watchdogs.
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
