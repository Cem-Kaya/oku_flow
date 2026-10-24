# Searchable saved captures

State: **remaining feature; earlier owner-selected outcome**.
Baseline: Windows 0.3.1; reviewed 2026-10-24.

## Existing behavior

Paired photos, annotation saves, portable notes HTML, grouped conversations,
media-type filters, explicit Read/Explain and browser recording transcription
already exist. New recordings use the current Original-only contract, not the
old two-video design. None of that needs reimplementation.

## Missing outcome

Associate readable text with the saved capture the user cares about, and make
those captures discoverable through a keyboard/screen-reader-friendly index.

- Give each capture a stable section identity; associate its requested reading
  result with that identity instead of a detached generic "Text on screen".
- Offer explicit reading of a saved photo/annotation. Use current local UCT or
  the explicitly selected vision workflow only after deciding its source,
  quality and privacy contract; do not revive the retired recognition service
  or automatically submit subscription-backed requests.
- Keep save success independent of reading success. Deduplicate bounded work;
  unavailable/busy reading must leave the saved image and notes intact.
- Preserve O(1) live appends. Sidecars or a bounded finalization pass can join
  text to media and generate a useful table of contents.
- Index time, type and a short preview without duplicating the entire transcript.
  Retain relative URLs and portable ownership of media/notes.
- Extend source-appropriate search/navigation to completed transcripts without
  replacing the existing browser transcription/file-access workflow.

The useful first slice is "read this saved capture and find it again", not a
new OCR engine or background cloud watcher.

Implementation references:
[notes templates](../include/okuflow/common/notes_document_template.hpp),
[notes runtime](../src/common/assistive_runtime.cpp),
[original recording policy](../docs/original_only_recording.md).
