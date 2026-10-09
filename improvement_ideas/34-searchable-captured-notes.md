# Plan 34 — Searchable Captured Notes

Status: **REVISION REQUIRED — 2026-09-10**

The separate local recognition path has been removed. The remaining capture
association/index work must use explicit vision-assistant requests (Luna by
default), with user control over whether saved images are submitted. This
plan does not authorize automatic background subscription requests.

Priority: **HIGH**

Estimated effort: **3-4 days**, including tests and documentation.

## Outcome

Every photo or annotation the user deliberately saves becomes searchable in
the lecture notes. The existing original/processed media remains visible and
portable; one useful OCR transcript is associated with that capture, and a
semantic index lets keyboard and screen-reader users jump directly to saved
moments.

This plan promotes idea F from
[plan 29](29-idea-inbox-2026-07-29.md). Image text comes from the vision assistant; live
recording transcription and its notes append are owned by
[plan 36](36-recording-transcription-to-notes.md). Share stable section-id and
index contracts rather than implementing either pipeline twice.

## Verified current behavior

The media-saving part already exists and must not be reimplemented:

- `OpenZoomApp::SaveCapturedPhotoPair(...)` writes timestamp-matched
  `_original.jpg` and `_processed.jpg` files.
- Only after both saves succeed, it calls
  `AssistiveRuntime::NoteCapturedPhotoPair(originalPath, processedPath)`.
- `NoteCapturedPhotoPair(...)` calls `AppendNoteMediaPair(...)`, which writes
  a responsive two-column notes section containing direct relative links and
  images for both the original and processed photo.
- Fully finalized recording segments likewise add both original and processed
  MP4 players.
- Annotation snapshots add one processed, ink-composited PNG to the notes.
- Successful on-demand Read adds a separate `Text on screen`
  section, but that text is not associated with the saved capture that caused
  the user to care about the moment.
- No automated test currently verifies the generated paired-photo notes HTML.

Therefore the missing feature is **search association and navigation**, not
original/processed capture.

## User-visible behavior

### 1. Read saved captures on request

After a paired photo is durably saved and appended to notes, an explicit
request to read that capture can:

1. enqueue a background notes-OCR job for the processed image;
2. submit the saved image through the configured vision provider;
3. append the recognized text inside the same logical photo section;
4. if the processed image produces no useful text, optionally retry the
   original once;
5. store only one transcript for the pair, labelled with which image produced
   it, so Ctrl+F does not encounter duplicate versions of every sentence.

After an annotation snapshot is saved, offer the same explicit reading
action for its PNG. If a clean pre-ink processed frame is available without
another readback or file, prefer it for recognition while keeping the
annotated PNG as the visible artifact. Otherwise OCR the saved annotation.

Do not OCR video frames in this plan. Slide-change OCR and recording chapter
markers remain separate ideas.

### 2. Keep capture responsive

Photo/annotation save success must not wait for the vision assistant:

- Use a bounded, single-consumer notes-OCR queue distinct from the visible
  foreground OCR state.
- Never replace the current OCR result panel with a background job's status.
- Deduplicate jobs by canonical capture path and session.
- A saturated queue reports `Photo saved; searchable text is pending` or
  `Photo saved; searchable text was skipped because OCR is busy` without
  claiming the image was lost.
- An unavailable vision provider leaves a valid media section and one concise
  note that searchable text was unavailable. It must not repeatedly prompt or
  retry without another user request.
- Cancellation and application shutdown are bounded. Already saved images and
  valid notes remain intact even if their OCR job has not completed.

### 3. Associate OCR with the correct media section

Give every note section a stable unique id, for example
`capture-20260731-140312-123`. The capture media and its OCR text must share
that section; do not append a detached generic `Text on screen` section.

Because notes are append-only while the lecture is live, use an explicit
pending-result mechanism:

- append the media section immediately with its stable id and an optional
  `Searchable text pending` state;
- let the background job produce a small, atomic sidecar/result record keyed
  by section id, or use another bounded append-only representation;
- materialize the final combined section/index once at session finalization,
  rather than rereading and rewriting the growing HTML file after every OCR
  result;
- if OpenZoom or Windows exits unexpectedly, opening the notes must still show
  every already-appended media section. A later repair/finalize pass may merge
  completed sidecar results.

The implementation may choose a different representation, but it must retain
the existing O(1) live append behavior and crash-tolerant valid HTML contract.

### 4. Add a semantic index

At finalization, place a keyboard- and screen-reader-friendly table of
contents near the start of the document:

```html
<nav aria-label="Lecture notes contents">
  <h2>Contents</h2>
  <ol>
    <li><a href="#capture-...">14:03:12 — Photo captured — first OCR line</a></li>
  </ol>
</nav>
```

Requirements:

- Every OCR, scene explanation, photo, annotation, and finalized recording
  section receives an id and index entry.
- Link text includes wall-clock time, section type, and a short text preview
  when available.
- DOM order is meaningful; do not create a visually moved index that a screen
  reader encounters at the end.
- Long OCR text remains in the section, not duplicated in the index.
- Empty OCR results still produce a useful media entry such as
  `14:03:12 — Photo captured`.
- Existing relative media URLs and portability of the complete OpenZoom root
  remain unchanged.

Building the index once when closing the session is acceptable. If the user
opens notes during an active session, show a truthful `Contents updates when
this session closes` message or generate a bounded snapshot without changing
the append-only source.

## Architecture boundaries

- Keep `AssistiveRuntime` responsible for lecture-note composition and OCR
  result association.
- Extract a small notes document/index model if necessary; do not grow another
  collection of interdependent string replacements in
  `assistive_runtime.cpp`.
- Inject or abstract the OCR runner enough that tests do not require a real
  subscription request or external service.
- Keep file writes atomic where a whole document or sidecar is replaced.
- Use the configured notes directory and relative paths from
  `UserDataPaths`; introduce no new hard-coded output root.
- A photo or annotation save alone must not send images to a provider. Only
  an explicit reading request may use Codex or the configured endpoint.

## Implementation phases

### Phase 1 — Regression-test the behavior that already ships

- Create notes in a temporary root.
- Call the paired-photo note path with two fixture paths.
- Verify one section contains both original and processed relative links,
  distinct accessible alt text, and direct link captions.
- Verify a paired section is not added when either path is empty.
- Verify annotation output remains a single annotated image.

### Phase 2 — Section identities and OCR job contract

- Introduce stable section ids and an internal capture-note/OCR-job record.
- Add a fake OCR runner and bounded scheduler tests.
- Preserve foreground OCR status and existing capture acknowledgement.

### Phase 3 — Searchable capture text

- Run processed-first, original-fallback OCR for photo pairs.
- Associate one sanitized transcript with the correct photo section.
- Add annotation OCR.
- Handle empty, timeout, missing executable, cancellation, and queue
  saturation paths.

### Phase 4 — Index and crash recovery

- Accumulate lightweight index metadata.
- Generate semantic contents once at clean finalization.
- Recover a valid media-only document after interruption and merge any
  completed pending results safely.

### Phase 5 — Accessibility, localization, and documentation

- Translate every new visible/status string and keep Turkish/German catalog
  parity.
- Use the session language for generated headings, link text, `lang`, and
  accessible labels.
- Update `README.md`, `docs/README.md`, `docs/code_reference.md`,
  `docs/hardcoded_paths.md` if a sidecar is introduced, and `CHANGELOG.md`.
- Run `scripts/check_translations.ps1` and `scripts/agent_build.bat`.

## Acceptance criteria

- A successful paired photo still saves and displays both original and
  processed images.
- The photo has one searchable OCR transcript associated with its own section.
- An annotation snapshot can be found by its recognized text.
- Ctrl+F finds saved-capture text without duplicate original/processed OCR.
- The contents links work with keyboard navigation and NVDA/Narrator.
- Photo save acknowledgement is not delayed by OCR.
- OCR failure never removes or invalidates saved media.
- No remote AI request occurs as a result of saving a photo or annotation.
- Active-session appends remain O(1); final index generation is at most one
  bounded rewrite per session.
- Abrupt termination leaves readable notes and saved media.

## Explicit non-goals

- Implementing lecture audio transcription, Whisper, or SRT/TXT output in this
  plan. Plan 36 owns live transcript capture/notes; this plan may index the
  resulting typed sections through its shared notes index.
- OCR of every frame or every video segment.
- Opening a PDF or saved image as the live magnifier input. That is
  [plan 35](35-pdf-image-source-mode.md); this plan concerns images already
  captured into notes.
- Changing the paired original/processed media naming or storage layout.
