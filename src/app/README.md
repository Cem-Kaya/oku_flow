# app module

This module owns Qt startup, application wiring, settings persistence,
interaction orchestration, and recording sessions.

`RecordingManager` preserves synchronized original/processed video, optional
microphone AAC audio, bounded queues, finalization, and worker watchdogs.
`RecordingSessionInfo` lives in `include/okuflow/app/recording_session_info.hpp`;
queued saved-segment and session-ended callbacks retain its stable identity.
Saved-media callbacks continue adding recordings and photos to lecture notes.

The Advanced inspector has Image, Assistant, and Settings tabs. Assistant
conversation history, Read, Explain, and manual Read Aloud remain available.
Microphone capture runs only while recording and sends PCM to the recorder.
Obsolete recording settings are ignored on load and omitted on save; there is
no settings migration.
