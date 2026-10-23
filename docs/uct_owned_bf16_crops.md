# Owned BF16 crop proposal (2026-10-23)

`okuflow-owned-typeset-gray8-v1` is an **unapproved synthetic corpus proposal**
for the public-model contract at independent commit
`faa7fbfe7b87983802c881411a856328bc873dfd`. It supplies original authored text,
eight static typeset GRAY8 crops and an actual-pixel contact sheet. Expected
Unicode text comes from `SAMPLES` in the first-party generator, never OCR output.
It is synthetic ground truth, not human-transcribed real-camera quality.

The production corpus catalog remains empty/unapproved. This change has no
application, Network, factory, cache, model-preparation, staging or inference
integration. No golden pass, ready receipt, speed, broad accuracy, detector,
camera or multilingual acceptance is established. Strict N1 plus full N8
exact-text attestation, actual runtime quality and parent review remain future
gates. Retain failing crops and observations; do not relabel, remove or rerender
cases to obtain a passing result.

## Files and source truth

The first-render files remain in the owned work-data `render-01/` directory
recorded below. Only after independent read-only review completed were all
12 files copied unchanged into `assets/uct/golden/okuflow-owned-typeset-gray8-v1/`
for this source commit, with every copied hash compared to its original.
These repository copies are an unapproved source proposal, not an active
application asset or release payload.
`manifest.json` has the exact seven-field root and five-field sample schema;
its `model` reproduces all `ModelIdentity()` fields, binding graph/source/config/
decoded dictionary, FLOAT DEVICE LINEAR ABI, N8 fixed1024 and preprocessing.
The independent contract requires selected medium detector/medium recognizer,
capacity 8, no variable/prepared width, an actual Ampere-or-newer device
(compute major >=8), and TensorRT ABI11 >=11.4.0.106. This proposal grants
no hardware/runtime eligibility or other backend selection.
Extra render metadata stays in `render-provenance.json`, whose exact hash is
bound by the strict manifest's provenance string. It records source/text hashes,
pixel hashes, font filenames/file hashes, renderer versions and draw parameters.
`pin-proposal.json` binds manifest/provenance/sheet/raw hashes and explicitly
grants no authority for runtime admission.

| ID | Exact authored expected text | Rendering | Source coverage |
| --- | --- | --- | --- |
| words-regular | `OkuFlow` | Sans regular 36 px, dark on light | word |
| phrase-bold | `Please read this line` | Sans bold 32 px, dark on light | longer phrase |
| date-regular | `2026-10-23` | Sans regular 32 px, dark on light | numbers, hyphens |
| decimal-bold | `+12.50 -0.75` | Sans bold 32 px, dark on light | signs, decimals |
| o-zero-regular | `O0 OO 00` | Sans regular 36 px, dark on light | letter O, digit 0 |
| i-l-one-inverse | `Il1 I l 1` | Sans bold 36 px, light on dark | capital I, lowercase l, digit 1 |
| negation-bold | `Do not enter` | Sans bold 34 px, dark on light | literal negation |
| accents-inverse | `café naïve` | Sans regular 34 px, light on dark | U+00E9, U+00EF |

These are authored test phrases, not user records. The date is a synthetic
literal and the decimal sequence has no financial/measurement meaning. No
private capture, third-party source artwork, font binary, model or tool is
included. Tags describe source coverage, not successful recognition. Turkish
dotted/dotless I and other languages are not promised.

Each `<id>.gray8` stores exactly `width * 48` bytes: one unsigned byte per pixel,
top-to-bottom rows, left-to-right columns, no header/stride/padding, upright
height 48 with useful width <=1024 including eight background pixels per side.
`contact-sheet.png` displays every raw crop at 1x and nearest-neighbour 2x with
source labels. It is review material, not model input. Compare actual pixels
with source text, not just the sheet labels.

Encoding is `gray8-upright-h48-zero-pad1024-v1`. Future runtime must apply
`paddle-rectify-normalized-zero-pad-v1`: `(sample/255-.5)*2`, repeat grayscale into
three channels, then fill to width 1024 with **normalized zero**. Background bytes
0/255 inside useful width are intentional; no raw pad bytes are stored. Do not
append white padding, invert inputs, trim, case-fold or normalize expected text.

## One bounded offline render

`scripts/uct/generate_owned_bf16_crops.py` uses Python standard library and
already installed Pillow 11.1.0/FreeType only. It imports no model/GPU/application/
network library and launches no child or window. Exact installed DejaVu Sans
regular/bold and original installed DejaVu/Pillow license notices are hash-checked.
Rendering uses BASIC layout, index 0, integral positions, no stroke/resize. PNG
parameters are fixed; there is no timestamp metadata. Exact versions and hashes
define the reproduction boundary; another toolchain is a new candidate.

Output must be a fresh absolute local owned directory. Existing output is
refused; first errors and partial output remain. A 100-second loop budget plus
an external deadline <=120 seconds bounds the single permitted pass. There is no retry,
fallback renderer, installation, download, cleanup, overwrite or staging.
A new render requires separate root authorization. Example shape only:

```powershell
& <installed-python.exe> -B scripts/uct/generate_owned_bf16_crops.py `
  --output Z:/Projects/OkuFlow/local-work-data/<owned-new-directory>/render-01 `
  --font-dir <absolute-installed-DejaVu-directory> `
  --pillow-license <absolute-installed-Pillow-license>
```

The only authorized render ran on 2026-10-23 from
`b86e29f81b52f8a0496e485560678fef1a950ad6` plus this generator. Its exact source
SHA-256 is `b4b8093159396aaabbccd8f171fc39c85585f01b743579c9da8add52b290aeca`.
It used installed Python 3.11.13, Pillow 11.1.0, FreeType 2.13.3, zlib 1.2.13 and
the pinned installed DejaVu fonts. A hidden Python process exited 0 before the
115-second external deadline, with empty renderer stderr and zero reruns.
No configure/compiler/test/benchmark/GPU/CUDA/model/inference/preparation/
download/network/GUI/browser/device activity was run. No active catalog,
factory, cache, staging or owner-state writes were made.

Initial output and process records remain under the owned work-data directory
`Z:/Projects/OkuFlow/local-work-data/uct-public-bf16-owned-corpus-20261023-t3-1e42a08c/`.
`render-01/` contains the actual first-render files; sibling `invocation.json`,
`completion.json`, `render.stdout.txt` and `render.stderr.txt` retain execution
evidence. `discovery-errors.txt` preserves an earlier read-only `rg` argument
error and an atomic documentation-patch anchor error; neither reran rendering.

All eight payloads total 86,256 bytes. Useful widths in source order are
170/386/200/245/191/155/257/193 pixels. Frozen candidate SHA-256 values:

- Manifest: `26432369ed6831431737072f857620b19c81fc9acd8c21579d50ee7bc0e9ec33`
- Render provenance: `981ca01ca6d00dccf38b6987b2a867995f9c61e26d19c966a57a7e2ff8a1f4af`
- Contact sheet: `0192f4ef4b17f6153ca9cfe4412ed57138492b55848f55fb24bafc0d87ad59cd`
- Pin proposal: `767b70ed74d82780e640f19bdc4b6647eb55c1f6964ccfab274d3035c8c081d5`

The implementation agent inspected the actual contact sheet using `view_image`
without a desktop. This is not the parent visual review or runtime acceptance.
The [independent review record](uct_owned_bf16_crops_review.md) covers every crop,
all 25 model-identity fields, provenance and all hashes, with 8/8 entries OK.
Its documentation-location finding was resolved by distinguishing first-render
work-data from later unchanged repository copies. The original review is retained
alongside the process records. Parent visual/runtime acceptance is still pending.

## License facts and approval boundary

The pre-existing installed DejaVu notice identifies Bitstream Vera terms,
public-domain DejaVu changes and retained Arev terms. Installed Pillow 11.1.0
identifies MIT-CMU. Exact notice hashes are in provenance. Tools/fonts remain
separately installed and no third-party implementation is copied. See
[third-party notices](THIRD_PARTY_LICENSES.md). These facts assert no new legal
clearance for bitmap/model/runtime distribution or permission to package.

Only a later reviewed change may pin the manifest in the production catalog,
after every crop/source/provenance review, parent visual inspection and actual
runtime quality/attestation gates. A proposal, environment value or mutable
receipt cannot grant approval.
