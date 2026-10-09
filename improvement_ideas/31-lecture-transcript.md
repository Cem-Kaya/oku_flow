# Plan 31 - Lecture Transcript

Status: **SUPERSEDED by owner decision on 2026-08-07. Active work is Plan 36.**

This file preserves the 2026-07-30 decision for provenance. At that time, the
owner directed OpenZoom not to implement:

- Codex realtime audio and transcript events.
- Local Whisper installation, inference, or model downloads.
- Transcript SRT/TXT sidecars.
- Transcript insertion into lecture notes.
- Transcript search, extraction, and post-recording jobs.

OpenZoom continues to record the user's selected microphone into the paired
original and processed videos. Audio recording is not transcription and remains
part of the product.

On 2026-08-07 the owner explicitly reopened the narrower goal of live
transcription during recording with finalized text appended to lecture notes.
Follow `36-recording-transcription-to-notes.md`; do not treat the historical
prohibition below as current implementation policy.

## Historical Probe

Before the feature was dropped, the Windows `codex-cli 0.145.0` app-server was
tested with the owner's ChatGPT Plus login. The experimental
`thread/realtime/start` request was accepted at the JSON-RPC layer, but the
worker returned:

```text
realtime conversation requires API key auth
```

OpenZoom does not deploy with an API key. No production transcription code was
implemented, so dropping this plan requires no product-code removal or data
migration.
