# File sources and phone recovery

Development implementation, 26 October 2026; separate from published 0.3.1.

Settings > Camera > **View file**, **Select file**, Ctrl+O and a single local-file
drop accept videos, PNG/JPEG/BMP/WebP/TIFF pictures, and PDFs. Camera discovery
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

## Build and validation

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
