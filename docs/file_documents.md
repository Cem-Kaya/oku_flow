# File sources and phone recovery

Development implementation, 26 October 2026; separate from published 0.3.1.

Settings > Camera offers **View video or picture** and a separate **View PDF**.
Their selection buttons open the matching file filter. Ctrl+O opens the selected
file category; from a camera it accepts any supported file. A single local-file
drop also accepts videos, PNG/JPEG/BMP/WebP/TIFF pictures, and PDFs. Camera discovery
continues without owning the file selection. A slow phone startup leaves the
source picker usable. An explicit webcam choice cancels the old startup and
automatic phone selection; an explicit file choice temporarily suspends new
camera opens. Returning to Camera retains the saved phone preference and its
normal reconnect/fallback behavior. A cable drop while the phone remains the
intended source still retries that phone. Temporary fallback never saves a new
camera preference.

## Qt components

`FileDocument` uses Qt's `QImageReader` for pictures and `QPdfDocument` for PDF
parsing, rendering and embedded-text extraction. OkuFlow does not implement a
PDF parser, font renderer, image codec or text extractor. A serial worker owns
the held file and parser. One latest request replaces queued navigation;
generation checks discard stale completion after another file, page or Camera
selection. Close and destruction do not wait for a decoder on the UI thread.

Pictures honor Qt's orientation metadata. PDFs render onto white and retain
their existing text/fonts. Magnification can request a higher-resolution vector
render after 250 ms of settled zoom, with the preceding page staying visible.
Pages initially render at a 4096-pixel long edge; requests are capped at 8192
pixels and 16 megapixels (64 MiB BGRA). This is bounded raster presentation of
PDF vectors, not unlimited-resolution rendering. The selected file must be at
most 256 MiB; encrypted/password PDFs are currently refused. TIFF uses its first
image. Unsupported/invalid files report failure rather than becoming a camera.

Presentation validates one owned BGRA frame with a 64-MiB limit. It must not use
the video decoder's multi-frame queue budget: an ordinary 4096-pixel portrait
PDF fits the page limit but cannot fit six such video buffers. That mismatch
previously left the preceding source visible despite successful text extraction.
Video decode queue limits remain unchanged. A document GPU refusal keeps the
same owned page for CPU presentation instead of reopening it as a video.

## Reading and accessibility

PDF page controls use a native Qt spin box with one-based page numbers and
Previous/Next page buttons. Video play, time scrub and audio controls are hidden
for documents. Pan, zoom, rotation, colors, image processing and Save frame reuse
the selected-media path. Camera framing is restored when returning to Camera.
Static files cannot issue video play/seek operations through Assistant/MCP.

Embedded PDF text is shown in the read-only **Document text** pane under
Settings > Camera. This is the PDF's own text, available for selection, copying
and Qt's native accessibility text interface without UCT. It is not an OCR
claim or positioned text mapped onto the magnifier viewport. It clears on a
scanned page or when leaving that document. Rendering retains the original
page layout; the pane follows the PDF extractor's reading order.

When Text Clarity and UCT are enabled, pictures and rendered PDF pages use the
existing immutable single-frame OCR path, including scanned content and images
inside mixed pages. No video neighbor is invented. Changing pages revokes old
OCR source authority. Embedded text and OCR reading text remain separate.

## Page drawings and one saved copy

PDFs render one selected page at a time, including long documents. **Previous
page**, **Next page**, and the page-number field navigate without decoding every
page. Drawings belong to their PDF page or picture, in normalized source
coordinates. They remain visible after leaving Draw and follow pan, zoom and
rotation. Returning to a page restores its drawing and in-session undo history.
Camera and video drawing behavior is unchanged.

**Save document**, the Draw save action, and Photo while viewing a static file
save the entire annotated document into `OkuFlow/Documents` under the configured
OkuFlow folder. The first save creates one working copy; subsequent saves update
that same file. A drawing on page 1 and another on page 3 are saved together in
one PDF. Clearing a drawing and saving removes that drawing from the copy. The
original is never replaced. A matching `.ink.json` sidecar preserves editable
page ink when reopening the original in OkuFlow; keep it beside the working
copy. A modified original has a different content identity and working-copy name.
An externally changed working copy is preserved and reported instead of being
overwritten. If another program locks the destination, saving fails without
creating a succession of new copies.

Qt writes vector drawing overlays and qpdf merges them into the original PDF.
This preserves all pages and native text instead of rasterizing the document.
Only edited pages need overlay pages; unrelated pages are retained. Pictures
save as full-picture PNG copies with the drawing embedded, independent of the
current viewport crop. Picture export has the same 16-Mpixel bound as document
presentation. Save runs on the image I/O worker and takes a snapshot of every
page's ink at the time of the request. Later drawing edits require another Save.
PDF parsing and the export subprocess are local; no provider receives the file.

### PDF export runtime

Run `scripts/stage_pdf_export.ps1 -Install` once for development and tests.
It installs the pinned upstream qpdf 12.4.2 Windows runtime into
`%LOCALAPPDATA%/OkuFlow/qpdf/12.4.2`; `OKUFLOW_QPDF_ROOT` selects an explicit
development cache. There are no configure-time or application-time downloads.
The release bundle stages the verified runtime at `app/qpdf/`, including exact
qpdf, OpenSSL, zlib and libjpeg-turbo notices. The runtime uses the existing
supported system VC runtime; vendor CRT copies are not deployed. Only the
bundled path or explicit/versioned cache is used, never a tool found on PATH.

`file_document_contract` checks a 600-page PDF using first/last/middle page
requests, repeated saves into one PDF, retained native text and page count,
rotated/cropped PDF placement, source-coordinate picture export, restored ink,
cleared drawings, and refusal to overwrite an externally changed output.
`pdf_export_distribution_contract` checks actual pinned runtime staging, SPDX
membership and tamper/extra-file refusals. The opt-in native file fixture
checks page ink and the real application's two-save path with an unshown D3D12
presenter. These headless checks do not claim spoken screen-reader acceptance.

## Build and validation

Local validation on 26 October: the tracked release compile, complete selected
non-GUI CPU suite (262 passing, 9 skipped, four passes with skipped Qt rows),
and CUDA delta (15 passing, 5 skipped) completed with zero unresolved cases.
This used `OKUFLOW_AGENT_EXCLUDE_GUI=1` and remains partial desktop validation.
PCH-disabled document, transport, distribution and shared-source checks passed
4/331. All 24 catalogs have 915 complete source keys; the 48 added translations
received a complete model review with the flagged Dutch wording corrected.
Native screen-reader speech and GUI acceptance are separate from these results.
Failed receipts remain preserved: the first PDF-copy test held its output open,
the file-release assertion initially repeated a successful rename, and the
cleared-ink test initially expected white from Qt's default transparent render.
The repaired checks close the reader, make release polling idempotent, and use
the same explicit white PDF background as production, respectively.

Qt PDF 6.12.0 is required alongside the existing Qt SDK. Run
`scripts/install_qt_pdf.ps1 -QtPrefix C:/Qt/6.12.0/msvc2022_64` to add the pinned
official extension. The helper verifies the archive and every pre-existing
destination, retains its original SPDX notices, and never replaces a differing
SDK file. Downloads/extraction stay outside the repository. Hosted Windows CI
runs this helper before configuring. Qt's deployment tool discovers `Qt6Pdf.dll`;
the release bundle retains its exact qtpdf SPDX document.

`file_document_contract` creates its own PDF with embedded text, a scanned page
and a final text page. It checks extraction, rendering, page limits, pixel caps,
latest-request/Close fencing, and picture colors. `video_transport_contract`
checks document page controls and the return to video controls. The opt-in
`native_app_file_sources_offscreen` uses the actual app with private capture
callbacks, injected completed discovery snapshots, private PDF/picture/video
files and the native Qt accessibility interface. It never contacts adb, a camera
or an AI provider. Build its excluded target explicitly before running it:

```powershell
cmake --build build/msvc-cpu --target native_app_control_fixture
scripts/agent_build.bat --scoped msvc-cpu file_document_contract video_transport_contract native_app_file_sources_offscreen
```

The document-pane geometry checks wait for Qt's queued first layout after
showing Settings, then require the whole pane inside its scroll viewport at
wide and narrow sizes. The hosted native case additionally checks accessibility
bounds, native hit testing and keyboard page navigation.

These desktop-free checks do not establish physical USB recovery, actual
screen-reader speech, GUI focus or installer acceptance. Run the tracked full
matrix and hosted native GUI inventory before integration; preserve failed
receipts and source/binary identity.

`documentPixelsReplacePreviousSourceInHiddenPresenter` submits real picture/PDF
pixels through the application and D3D12 into an unshown, non-activating private
window, checking the viewport pixel buffer through picture → PDF → next page.
The original failing run retained correct green PDF pixels while the viewport
remained red. `media_gpu_scene_cuda` additionally reads back two 4096-pixel
portrait BGRA scenes after a smaller scene, checking every output pixel. These
are presentation/texture checks, not visible desktop or screen-reader speech
acceptance. Source-choice tests retain each category through camera discovery
and synchronize the selection button's visible and accessible names.
