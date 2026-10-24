# Remaining lecture workflow ideas

Baseline: Windows 0.3.1; reviewed 2026-10-24. These are proposals; no new
implementation is authorized by listing them.

## Most useful small additions

- **View bookmarks:** save named zoom/pan positions for board left, board right
  or lectern and recall them with accessible keyboard actions. Bind them to the
  relevant source instead of applying an old view to unrelated media.
- **Compact always-on-top magnifier:** keep a readable viewport beside notes,
  with a clear keyboard return to the normal window.
- **Framing guidance:** on-demand spoken help aiming the phone, such as visible
  edges or tilt. Report what the detector actually knows.
- **Status query:** one spoken summary of recording/source/storage/fault state.
- **Recording chapters:** timestamp snapshots, Read requests and annotation
  saves against the recording timeline, export a chapter sidecar and expose
  keyboard-accessible jumps in notes.

## Larger workflow features

- **Crash/session recovery:** a bounded, atomic session journal for view,
  source and unsaved ink. Offer recovery explicitly. Ordinary settings restore
  and video Resume already exist; never automatically restart recording.
- **Battery/thermal saver:** an explicit or user-approved low-power policy for
  presentation rate, Super Resolution and optional background work. Measure
  real energy/readability effects; no power-aware mode currently exists.
- **Live recap additions:** existing recap is silent video, with Save frame.
  Audio history and exporting a selected recap clip remain unimplemented.
  Any microphone use needs the normal independent recording/privacy rules.
- **Change alerts:** an opt-in, debounced "the slide/selected region changed"
  announcement. A motion probe is not already a user-facing alert feature.
- **Support export:** a user-reviewed diagnostics bundle with build/runtime
  identities and selected logs. Durable debug logs exist; a one-action,
  privacy-reviewed export is a separate feature.

## Covered elsewhere

Alternative browser speech-recognition models remain parked research, including
the unverified audio-embedding/decoder proposal. The existing browser Whisper
workflow is implemented; no new decoder or speed claim has been established,
and this does not reopen the retired desktop transcription feature.

[Saved capture search](34-searchable-captured-notes.md),
[PDF/image sources](35-pdf-image-source-mode.md),
[phone controls](51-okuflow-camera-android.md),
[smaller downloads](03-performance-gpu-cpu.md) and
[reading tools](55-uct-quality-and-reading.md).

Audio recording, Freeze, live rewind, Setup Assistant, multilingual UI,
versioned MSI/ZIP delivery, async media work and current control APIs are
already implemented. The previous inbox's missing-feature claims for them
have been removed.
