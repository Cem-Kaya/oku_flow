# Open image and PDF sources

State: **remaining feature; earlier owner-selected outcome**.
Baseline: Windows 0.3.1; reviewed 2026-10-24.

Camera input, held frames and saved-video playback exist. The released Open
action accepts video; there is no dedicated open-image/PDF page workflow or
PDF rendering dependency.

## Useful first slices

1. Open a bounded static raster image as a source and reuse zoom/pan, rotation,
   colors, UCT, annotation and explicit Read/Explain.
2. Add PDF rendering and accessible page navigation after reviewing the actual
   dependency, runtime notices, payload and SBOM changes.
3. Save a recoverable original/processed render pair without modifying the
   opened file. For PDFs, retain page number and source provenance in notes.
4. Return to the previous camera/format/view without overwriting saved intent.

## Design boundaries

- Use a real source identity and generation, not forged camera samples.
- Decode/render off the UI thread with bounded dimensions, page counts and
  cancellation. An old page must not replace a newly selected page.
- Upload/prepare an immutable source once; do not simulate a 30-FPS camera.
- Keep source, processed-canvas and viewport coordinates explicit.
- Reuse the current immutable UCT/media ownership and capture writer instead
  of creating another recognizer or recorder.
- Include encrypted/corrupt/oversized-file failures, focus/reading navigation,
  high zoom, RTL/large-text controls and source-preserving saves in the design.

See [video document ownership](../docs/video_document_lifecycle.md) and
[standalone Freeze](../docs/uct_standalone_freeze.md) for reusable contracts.
PDF is still not a dependency of the released application.
