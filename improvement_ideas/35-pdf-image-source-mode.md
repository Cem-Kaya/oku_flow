# Plan 35 — PDF and Image Source Mode

Status: **READY — OWNER SELECTED 2026-08-01**

Priority: **HIGH**

Estimated effort: **5-8 days**, including PDF deployment, tests, accessibility,
and documentation.

## Outcome

Let a user open a saved image or PDF page as an alternative to the live
camera, then use the same low-vision reading workflow: zoom/pan, rotation,
colour schemes, brightness/contrast, sharpening and Text Clarity, OCR,
annotation, and Read Aloud.

The source is immutable. Saving from source mode creates a paired original
render and processed render under the normal dated Photos root; lecture notes
embed both. For a PDF, notes also retain a link to the source document and the
page number. OkuFlow must never overwrite or silently recompress the file the
user opened.

## Verified current behavior

- The frame pipeline is driven by `MediaCapture`; there is no static-source
  provider or PDF dependency.
- A camera photo already saves timestamp-matched `_original.jpg` and
  `_processed.jpg` files and adds both to lecture notes after the pair commits.
- OCR, annotation, response-language, and TTS behavior already operate on a
  presented frame or saved image. They should be reused, not cloned into a
  second assistive stack.
- Plan 34 adds search association for material captured into notes. It does
  not open external PDFs/images; the two plans are complementary.

## User-visible behavior

### Open and leave source mode

- Add **Open image or PDF…** with a keyboard shortcut and standard file dialog.
- Support JPEG, PNG, BMP, TIFF, and WebP when the deployed Qt image plugins
  genuinely decode them. Do not advertise formats absent from the bundle.
- Support password-free PDFs initially. Report encrypted, corrupt, oversized,
  and unsupported documents without replacing the current usable source.
- Clearly label the current source and PDF page. Provide Previous/Next page,
  page-number entry, Fit/Fill, and return-to-camera controls.
- Camera capture may be stopped while a static source is active, but the
  selected camera and its working set remain intact for return.

### Reuse the magnifier and assistive tools

- Feed decoded pixels through the existing display transform/effect path:
  rotation, zoom centre, pan, colour mapping, black/white, brightness,
  contrast, Text Clarity, and sharpening.
- Stabilization and camera hardware controls are disabled in static mode; they
  have no meaningful source motion/device to control.
- OCR and VLM analysis target the current processed page/image. Read Aloud
  uses the resulting text and current language/voice preferences.
- Annotation coordinates remain stable across zoom, rotation, Fit/Fill, and
  PDF page changes. Prompt before discarding unsaved ink.

### Preserve original and processed versions

- Never mutate the opened file.
- **Save photo** in source mode writes two recoverable outputs using the same
  pair-commit contract as camera photos:
  - an `original` render before OkuFlow effects, at the chosen PDF render
    resolution or decoded image dimensions;
  - a `processed` render matching the enhanced source canvas, excluding
    transient UI chrome and including ink only when the user selects the
    annotated-save action.
- Add the pair to notes only after both outputs commit. Notes show both renders;
  PDF captures also include a relative/portable source link where possible and
  an accessible `Page N` label.
- Keep the opened source path as provenance, not as one member of the writable
  transaction. If the source later moves, the saved render pair still works.

## Architecture

Introduce a small source abstraction rather than forging Media Foundation
camera samples:

```text
LiveCameraSource ─┐
                  ├─ SourceFrame (pixels + size + identity) ─ existing effects
StaticFileSource ─┘
```

- `StaticFileSource` owns decode/page-render cancellation and a monotonically
  increasing generation. Results from a previous file/page are discarded.
- Decode and PDF rendering run off the UI thread with a bounded queue and
  dimension/pixel-count limits. Upload one immutable source texture and rerun
  effects only when the page or effect configuration changes; do not emulate a
  30-fps camera.
- Keep source-space, processed-canvas, and viewport transforms explicit so
  OCR crops, annotations, and saved originals do not accidentally use window
  pixels.
- Evaluate Qt PDF (`Qt6::Pdf`) first because it matches the existing Qt
  deployment. If adopted, add the module and runtime files to both build and
  bundle scripts, record its actual version in the SBOM, and update
  `docs/THIRD_PARTY_LICENSES.md`. Do not add a second PDF library casually.
- Apply file-size, page-count, decoded-pixel, and render-dimension limits before
  allocation. Treat image metadata and PDF content as untrusted input.

## Accessibility

- Every source/page/navigation control needs an accessible name and state.
- Announce source opened, page changed, render failure, unsaved annotation,
  paired save completion, and return to camera through the live-status policy.
- Preserve logical reading order and full keyboard operation at 200%/400%
  scaling. Page changes must move neither keyboard focus nor zoom unexpectedly.
- Use source filename and page number in generated alt text; never expose only
  `original`/`processed` as the accessible description.

## Implementation sequence

1. Add the source-state model, static raster decode, cancellation generation,
   and one-shot GPU upload while leaving camera mode unchanged.
2. Add source-mode controls, keyboard/accessibility behavior, and image
   processing/zoom/rotation parity.
3. Add PDF page rendering/navigation and bundle/license/SBOM updates.
4. Route OCR, annotation, VLM, and TTS through the selected source frame.
5. Reuse the recoverable paired-photo writer and notes integration, including
   PDF provenance/page metadata.
6. Add tests, documentation, translation keys, and hardware/manual acceptance.

## Tests

- Decode small/large, EXIF-rotated, alpha, corrupt, and unsupported raster
  fixtures with bounded failure behavior.
- Open a multi-page PDF, switch pages rapidly, and prove an old render cannot
  replace the latest generation. Cover corrupt and encrypted PDF failures.
- Verify zoom/rotation/Fit/Fill transforms for OCR and annotation coordinates.
- Save from raster and PDF sources; assert both original and processed renders
  exist, neither can be orphaned after simulated commit interruption, the
  opened source bytes are unchanged, and notes link/embed the correct assets.
- Exercise return to the same camera/format/working set after source mode.
- Run translation parity, release, CPU, CUDA, bundle, and NVDA/Narrator passes.

## Acceptance criteria

- A user can open a supported image or PDF without a camera and use the same
  colour, sharpening/Text Clarity, OCR, annotation, and Read Aloud tools.
- PDF navigation is responsive and cancellation-correct.
- Saving always produces a recoverable original/processed render pair and
  never modifies the opened source.
- Notes contain both renders; PDF captures identify and link the source page.
- Return to camera restores its prior selection and working set.
- Malformed or excessive input fails visibly without unbounded allocation,
  UI hangs, stale-page display, or loss of the previous usable source.

## Explicit non-goals

- Editing/replacing pages inside a PDF, preserving PDF vector structure in the
  processed export, form filling, signatures, or password decryption.
- Video-file playback, slide-deck authoring, or OCR of an entire document in
  the first release.
- Applying camera stabilization or hardware controls to a static file.
