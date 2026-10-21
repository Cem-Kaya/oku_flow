# Ultra Clear Text OCR engines (research, 2026-10-21)

Research only. No backend, model, dependency, license file, or application
code was changed. Base commit `876ff4a3fb672979953344eb7cab83664d093790`.
Published timings below are other people's hardware and image sets. None of
them is an RTX 3050 measurement, a 120 FPS result, or a quality gain on
OkuFlow frames.

## Recommendation

Keep the current GPU pair: PP-OCRv6 medium detection and PP-OCRv6 medium
recognition, TensorRT CUDA, batch eight. There is no larger PP-OCRv6 tier.
On Paddle's own June 2026 comparison, medium is the strongest of the three
tiers, and PP-OCRv5 server sits behind it. Replacing that pair is unlikely
to fix the paused book page or the tilted card.

The useful next work is in our geometry and in what we choose to OCR:

1. Keep the detector's oriented quad through admission and paint. Today the
   quad is thrown away and the overlay draws a horizontal word stretched to
   an axis-aligned rectangle.
2. Raise the connected-component cap and compare our probability-map fitting
   with Paddle's contour quadrilateral on the same map, before any new
   weights.
3. On a paused or zoomed view, OCR the visible scene region at its native
   pixels. Pan and zoom currently reproject words already accepted. A
   recognizer has no separate zoom control.

Run those three against the frozen screenshots before spending time on
another engine. If the book page is still unreadable after oriented quads
and a visible-region crop, the one optional preprocessor worth a bounded
trial is Paddle's UVDoc unwarper, with the boxes mapped back onto the
original page. The one external engine worth a frozen bake-off after that
is docTR's word detector plus a small recognizer. RapidOCR, Surya 2, and
PaddleOCR-VL do not earn a place on the live path.

## What the owner is seeing

Three private screenshots were inspected locally. The videos were not
decoded.

- `...\d044797c-e3f5-46c0-bd22-ca7f498d43be.png` is an open book,
  *Divina Commedia*, Inferno, preface. Many replacement words are horizontal
  and collide: lines run through each other, and several tokens are fused
  or garbled (`torsalready`, `expressedomo of comperi`, a cluster over
  "homely" / "Aristotle"). Large stretches of readable print have no
  replacement. The page is curled, so a line is an arc, not a straight quad.
- `...\d0014a53-1f4c-4caf-bc62-7d38735d44e4.png` is a closer crop of the same
  page, paused at 00:00 / 00:06. The same collision pattern remains at
  higher magnification. Zooming the display did not produce a clean reread
  of the visible lines.
- `...\c607be75-1249-4dab-924e-daac4e5a5666.png` is
  `Untitled_video_-_Made_with_Clipchamp_av1.mp4`, paused at 00:25 / 00:38.
  The in-world panel is tilted in perspective. Replacements such as
  `Toggle`, `RightHand`, `LeftHand`, and `ZoomLevel` are upright horizontal
  words on white or green plates. `100` and `42%` remain the original
  pixels. `MAGNIFIER SETTINGS` is only partly covered.

That file is present at `Z:\Vids\Untitled_video_-_Made_with_Clipchamp_av1.mp4`
(19,424,638 bytes, last written 2025-11-18). `Z:\Vids\006_av1.mp4` was not
there on 2026-10-21. Nearby names include `2_av1.mp4` and several other
`*_av1.mp4` files. This report does not treat any of them as `006`.

## What a new recognizer can change

A recognizer can improve:

- Whether a line or word is found in the pixels we actually pass in.
- The string, once a crop is reasonably straight.
- A tighter quad or polygon, if the engine returns one and we keep it.
- Languages outside PP-OCRv6's 50-language set, with a different dictionary
  or model.

A recognizer does not fix:

- Horizontal stretched plates. `RecognizedWord.boundsPx` is an axis-aligned
  rectangle. `PaddleDecodeTokens` builds that rectangle from the min and max
  of the quad slice (`src/common/uct_paddle_processing.cpp`).
  `UltraClearTextOverlay::PaintWords` then scales an upright `drawText` run
  with independent X and Y scales and no rotation
  (`src/ui/ultra_clear_text_overlay.cpp`). Official Paddle output is also a
  four-corner quad. Feeding a better quad into this painter still draws a
  horizontal word in the quad's bounding box. The tilted card is this path:
  several strings were read, then drawn flat.
- Glyphs that follow a curl. A single rotated rectangle around an arc
  becomes a tall box that overlaps the next line. Bilinear sampling of that
  quad into a 48-pixel-high strip also warps the letters, which is a
  plausible source of the fused book tokens. Character outlines are not in
  the public result. CTC time spans are estimates; the product docs already
  say they are not glyph outlines.
- Text we never submit. Analysis is the pre-zoom scene. Completed stills
  do not schedule another OCR when the user pans or zooms
  (`docs/ultra_clear_text.md`). Opus is changing paused-navigation behavior
  separately. That work, and a visible-region crop, stay necessary whatever
  the engine is.
- Admission policy. Confidence below 0.7 stays a low-confidence omission.
  `IsHighRiskToken` rejects a single confusable character, mixed
  digit-and-confusable-letter tokens, and symbols outside a small
  punctuation set. Plain `100` is not in that rejection class, so a missing
  `100` is detection, recognition, size, or validation, not that filter.
  Two agreeing reads can still share one mistake. Handwriting, math, and
  curves are outside the accepted use case.

## How the current path is built

`Recognizer` (`include/okuflow/common/ultra_clear_text.hpp`) is initialized
on the scheduler worker and returns lines of words. The automatic chain is
TensorRT, then ONNX Runtime CPU, then `Windows.Media.Ocr`. GPU defaults are
medium/medium. CPU defaults are tiny detector and small recognizer.
Cancellation is cooperative between bounded groups. The UI never waits on
OCR. One worker holds one in-flight job and one latest-only pending slot.

Pinned inputs (`scripts/uct/pins.json`), prepared locally, not bundled:

| Role | Repository revision | Tier used on GPU |
| --- | --- | --- |
| Detector | `PaddlePaddle/PP-OCRv6_medium_det_onnx` @ `61323801669c338b7891481ec7bac61ce31b576a` | medium |
| Recognizer | `PaddlePaddle/PP-OCRv6_medium_rec_onnx` @ `50c7eacafc52fa7bcf4194e8cd08e46f8558504b` | medium |
| CPU runtime | ONNX Runtime 1.31.0 win-x64 | CPU fallback |
| GPU runtime | TensorRT 11.4.0.106, CUDA 12.9 | preferred |

Detection resizes each region so the long side is at most 960
(`PaddleDetectionSize`). Extra regions target about a 1280-pixel edge, with
160 pixels of overlap, and at most a 4 by 4 grid plus the whole scene.
`PaddleDetect` walks the probability map at 0.2, keeps components of at
least 9 pixels whose mean score is at least 0.4, fits one rotated rectangle
with PCA, and stops at 128 components in scan order before sorting. The v5
server config on GitHub uses contour post-processing with
`max_candidates: 1000`. That cap is ours. A dense page can drop later
components inside one tile even when the network responded.

Recognition samples each quad into a 48-pixel-high strip, width 1024 or the
opt-in 128/256/512/1024 bucket. Batch eight is the development default for
medium and small. The public word is still the axis-aligned box.

### Timing scopes already measured

These are RTX 4090, driver 617.42, TensorRT 11.4.0.106, unless noted. They
are individual or small-sample observations, not a latency distribution.

| Scope | Figure | Source |
| --- | --- | --- |
| Whole scheduler worker, one 1920-wide frame, medium/medium, batch 8, median of 3 | 218.43 ms | `docs/uct_gpu_batching.md`, 2026-10-20 |
| OCR stream wall on that same run | 167.23 ms | same |
| Recognizer CUDA-event intervals only | 130.19 ms | same |
| Detector alone, wider profiles of that frame | about 17 ms | same |
| First result including a new batch-8 plan | 78.01 s | same |
| Cached batch-1 first result on that frame | 4.45 s | same |
| Batch-8 recognizer output tensor | 73.09 MiB FP32; input 4.5 MiB | same |
| Frozen recognition only, problem frames at 55 s and 89 s, fixed 1024 | 217.30 ms and 216.49 ms median | `docs/uct_recognizer_width.md` |
| Same frozen crops, bucketed width | 182.16 ms and 183.26 ms | same |
| Complete path, those frames, fixed / bucketed | 234.28 / 216.67 ms and 224.06 / 206.45 ms | same |
| Warm neighbour-read wall, owner's 55 s / 89 s pairs | 231.68 / 220.65 ms | `docs/uct_performance_roadmap_2026_10_20.md` |
| Host merge inside that profile | 3.85 / 3.34 ms | same |
| Windows OCR, synthetic 1600×900 page, development PC | about 25–32 ms | `docs/ultra_clear_text.md` |

The Windows figure is a different image, engine, and device. The 78 s figure
includes engine compilation. The 17 ms detector figure is one stage of one
frame. Builder workspace is capped at 256 MiB and is not a measured process
VRAM total. No RTX 3050 number exists. At 120 recognitions per second the
budget would be 8.33 ms; the measured dense worker is on the order of
200 ms. Display refresh of text already proved is a separate, cheaper path.

## Shortlist

### 1. Keep PP-OCRv6 medium/medium

This is the production candidate to beat, not a legacy model waiting for a
replacement.

Detection is a DB-style map (RepLKFPN neck, Dice plus focal loss in the
paper) turned into rotated rectangles by our code. Recognition is LCNetV4
plus a light SVTR neck and CTC. The NRTR head is training-only and is
removed at inference. Official exported boxes, in the PaddleX text-detection
example, are four vertices (`dt_polys` of shape 4×2) plus a separate
axis-aligned rectangle. That is an oriented quad, not a curve and not a
character box. Baidu's product page describes polygon coordinates; the
engineering sample that can be checked is the four-corner quad.

Dense small print depends on the 960-pixel limit and on our 128-component
stop. Curved print is the "Warp" column in Paddle's private detection table
(medium Hmean 88.6). That number is not this book. Rotation Hmean is 93.8
on that same private table. Neither number describes replacement plates.

There is no zoom API. Language coverage for medium and small is 50
languages: simplified and traditional Chinese, English, Japanese, and 46
Latin-script languages listed in the Chinese PP-OCRv6 page (French, German,
Italian, Spanish, Portuguese, Dutch, Polish, Czech, Swedish, Turkish,
Vietnamese, Indonesian, and the rest of that list). Tiny drops Japanese.
Arabic, Persian, Hebrew, Hindi, Bengali, Urdu, Thai, Korean, Russian, and
Ukrainian are outside that list. Korean has a separate PP-OCRv5 mobile
recognizer, which is a different model and still needs a detector.

Paddle's end-to-end table, 200 general and document images, includes image
I/O and pre/post-processing. Medium is 0.29 s/image on an A100 with
PaddlePaddle and 0.67 s on a V100 with ONNX Runtime. The TensorRT cell for
medium is blank on both A100 and the published medium row. Tiny TensorRT is
0.16 s on A100 and 0.23 s on V100. CPU on an Intel Xeon 8350C is 2.05 s
(Paddle) or 1.40 s (OpenVINO) for medium. These are not our worker times.

Batching is a dynamic recognizer batch. We already submit eight crops per
enqueue. VRAM evidence on our machine is the tensor sizes above, not a
full-process peak. Windows integration already exists: ONNX, TensorRT 11.4,
and a CPU ONNX Runtime fallback. Code and the pinned weights are Apache-2.0,
recorded in `third_party/paddleocr/LICENSE` and
`docs/THIRD_PARTY_LICENSES.md`. The TensorRT runtime remains under NVIDIA's
11.4 SLA. That split is already accepted for this optional component. Shipping
the weights in a public installer still needs the inventory the license doc
requires.

Vendor accuracy (medium, in-house): detection Hmean 86.2, recognition
accuracy 83.2, versus PP-OCRv5 server 81.6 and 78.1 on the table in
`PP-OCRv6.en.md`. A different official page says v6 and v5 numbers come from
different sets and should not be compared directly. Treat the gain as
Paddle's claim about its own sets, not as a public-benchmark result and not
as a prediction for this book.

Sources: [PP-OCRv6 English notes](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/algorithm/PP-OCRv6/PP-OCRv6.en.md)
(page retrieved 2026-10-21; release described as 11 June 2026),
[paper](https://arxiv.org/abs/2606.13108) (arXiv:2606.13108v1, 11 June 2026),
[language list](https://paddlepaddle.github.io/PaddleOCR/main/version3.x/algorithm/PP-OCRv6/PP-OCRv6.html),
[PaddleX quad example](https://paddlepaddle.github.io/PaddleX/3.0/en/module_usage/tutorials/ocr_modules/text_detection.html).

### 2. Same weights, better geometry

This is the change most likely to match the screenshots. It is our code,
not a new engine.

Compare, on frozen pixels, three readers of one detector map:

- Today's PCA rectangle, 128-component stop, axis-aligned paint.
- The same rectangles, painted with the quad's rotation and uniform scale
  so a tilted word stays tilted. Perspective on the card will still be
  imperfect: a rotated rectangle is not a full perspective quad, and the
  current four points happen to be a rotated rectangle.
- Contour quadrilaterals in the style of Paddle's DB post-process
  (`thresh`, `box_thresh`, `unclip_ratio`, candidate cap 1000 in the v5
  server YAML), still using the pinned medium map. Keep our thresholds in
  the comparison so a gain is not just a looser cutoff.

Word and character geometry stay estimated along the strip until we store
the quad. Curved lines remain one quad per component; this trial can reduce
overlap and stretch, and it cannot make glyphs follow an arc. Dense print
changes if the 128 cap or the 960 resize was the miss. Zoom changes only if
the trial also recrops the visible region. Languages, batching, VRAM, and
licenses stay as they are. Cold start does not change if the plans stay
cached.

Do this on the development RTX 4090 first, then repeat the winning reader
once on an RTX 3050 with plans built on that GPU. A plan from the 4090 is
not a 3050 plan.

### 3. UVDoc, only for a curled page, only with an inverse map

UVDoc is Paddle's document unwarper. The model card is Apache-2.0. An ONNX
package exists (`PaddlePaddle/UVDoc_onnx`). It outputs a corrected image,
not words. Reported DocUNet CER is 0.179 (17.9 percent character error on
that set). Paddle's module table lists 19.05 ms GPU inference on a Tesla T4
with TensorRT 8.6.1.6, and a separate table lists 36.66 ms end to end for
the static Paddle engine, including about 15 ms of preprocessing. That is
T4 evidence, model-only or their pipeline, not our stack.

Use it only as a recognition aid for the book frames. The overlay has to
draw on the original curled photo, so every box needs the inverse warp.
Flat UI, the tilted card, and already-straight pages should stay on the
direct detector. Unwarping can invent distortion. It does not add
per-character boxes, a zoom control, or new languages. Storage size in the
module table is 30.3 MB. Dependencies would be another ONNX graph in the
existing worker, plus the inverse-map math. It is not production-ready for
UCT until the inverse map is tested on these pages.

Sources: [HF model card](https://huggingface.co/PaddlePaddle/UVDoc),
[module tutorial](https://paddlepaddle.github.io/PaddleOCR/v3.3.0/en/version3.x/module_usage/text_image_unwarping.html)
(T4, CUDA 11.8, TensorRT 8.6.1.6, paddleocr 3.0.3).

### 4. docTR word detector, as a bake-off, not a port

docTR (maintained by t2k, code Apache-2.0) detects words rather than lines,
and can return polygons when `assume_straight_pages` is false. FAST is
described as an arbitrarily shaped text detector. The pretrained zoo also
includes DBNet and LinkNet. Recognition options include a small CRNN and
PARSeq. Default vocabularies are French-oriented; the vocab has to be
checked against our dictionary before a quality claim.

The published speed table is CPU-only: an i7-11800H, after 100 warmup
iterations, averaged over 1000 samples. Detection at batch 1 is 0.4–1.1
seconds per 1024×1024 tensor (FAST tiny 0.7 s, or 0.4 s in the parenthetical
reparameterized column). CRNN MobileNetV3 large recognition is 0.08 s per
batch of 64 crops; PARSeq is 2.2 s per batch of 64. No GPU, TensorRT, or
Windows C++ latency is published there. FUNSD and CORD are forms and
receipts, 199 pages for detection, and the docTR page says that may not
represent the model. Those exact-match rates (about 87–96 percent on the
combined sets) are not camera-page accuracy.

Word polygons would help the tilted card and overlapping lines if we stored
them and painted along them. A curled line can still be one thick word
region. There is no magnifier zoom control. Character boxes are not the
default output. Integration would be PyTorch, or ONNX through OnnxTR. A
TensorRT engine is not part of the documented path. Weight files are
downloaded from doctr-static.mindee.com; this report did not find a separate
weight-license file, so redistribution under the commercial license is an
open review, not a clearance. Treat the library as a frozen-image bake-off:
export word quads and strings, score them offline, and port nothing unless
the score beats oriented PP-OCRv6 on our pages at a latency we can accept.

Sources: [docTR repository](https://github.com/mindee/doctr) (Apache-2.0),
[model choice and CPU timings](https://mindee.github.io/doctr/using_doctr/using_models.html)
(retrieved 2026-10-21).

### 5. RapidOCR is a runtime wrapper

RapidOCR packages Paddle detection and recognition as ONNX and can build a
TensorRT engine on first run. Current docs show PP-OCRv4, v5, and v6, tiny
through medium, including the medium pair we already pin. The default wheel
ships the small ONNX pair. Code and the converted PP-OCRv6 weights are
described as Apache-2.0 in `python/MODEL_LICENSES.md`. The C++ tree
`RapidOcrOnnx` is also Apache-2.0.

Accuracy follows the Paddle weights and their post-process. It does not add
a curve model, character boxes, or zoom. Adopting the Python package, or a
second C++ runtime beside TensorRT 11.4, would duplicate the path we have.
It is a useful reference for contour post-processing and for checking an
ONNX result against ours. It is not a second product backend.

Sources: [RapidOCR](https://github.com/RapidAI/RapidOCR),
[model list](https://rapidai.github.io/RapidOCRDocs/main/model_list/)
(page dated 2026-10-14),
[model license note](https://github.com/RapidAI/RapidOCR/blob/main/python/MODEL_LICENSES.md).

### 6. Windows OCR stays the last fallback

`Windows.Media.Ocr` returns lines and words. `OcrResult.TextAngle` is one
clockwise angle for the whole image. Microsoft's remark says that mixed
angles are only partly recognized, and that a non-zero angle means boxes are
in the rotated image. `OcrWord.BoundingRect` is an axis-aligned rectangle.
There is no per-word quad, polygon, or character outline. A curled page and
a perspectively tilted panel are outside what that API can place.

It is CPU-only, uses an installed Windows language, and redistributes no
model. Our 25–32 ms point is one synthetic page. The Learn page dated
2026-08-05, contract view `winrt-28000`, says these APIs are supported for
desktop apps with package identity and marks some of that text as prerelease.
OkuFlow already calls the API from the desktop build. That sentence is a
packaging question for a future SDK, not a measured failure and not a reason
to remove the fallback.

Sources: [OcrResult.TextAngle](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrresult.textangle?view=winrt-28000),
[OcrWord.BoundingRect](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrword.boundingrect?view=winrt-28000),
[namespace remarks](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr?view=winrt-28000).

## Considered and not recommended for the live path

**PP-OCRv5 server.** Same two-stage family, larger and older. On the v6
comparison table it is less accurate, and its published GPU times are higher
(0.32 s on A100 Paddle, 0.73–0.77 s on V100). A v5 performance note reports
peak VRAM around 5402 MB for the server pair on the GPU they used. That
figure is their process, not a tensor we measured. Moving back to v5 would
also drop the single 50-language recognizer.

**Surya OCR 2** (Datalab, announced 27 May 2026). A 650M document model.
Code is Apache-2.0. Weights use a modified OpenRAIL-M: the README allows
research, personal use, and organizations under 5 million USD revenue and
funding, and points commercial use above that to a paid license. That
restriction matters for OkuFlow's commercial license. This is an
identification of the term, not a legal clearance. The published RTX 5090
table is a vLLM server at high concurrency: 5.35 pages/s at 128 concurrent
requests, with p50 latency 18.9 s. At concurrency 32, p50 is 6.7 s. That is
batched throughput, not a single-page magnifier update. An older Surya line
detector was much smaller; version 2's main path is the VLM. It is the wrong
latency class for overlay refresh, and word-quad alignment to camera pixels
is not established here.

Sources: [repository](https://github.com/datalab-to/surya),
[announcement](https://www.datalab.to/blog/surya-2).

**PaddleOCR-VL-1.6** (announced in the PaddleOCR docs as 28 May 2026) and the
0.9B v1 VLM. These parse a document into text, tables, formulas, and layout.
The 0.9B model card on the ModelScope mirror states Apache-2.0; confirm the
1.6 card before anyone vendors it. The 0.9B safetensors blob in that mirror
is about 1.9 GB. Official inference wants Paddle or vLLM, with vLLM asking
for compute capability 8 or higher and CUDA 12.6 or higher. An RTX 3050 is
capability 8.6, so it is inside that matrix, and no 3050 latency is
published. The Docker image is described as about 8 GB. Output is a document,
not a cancellable stream of word quads registered to a live frame. A paused
"read this page" feature could revisit a VLM later. It should not replace
the recognizer that feeds glyphs.

Source: [PaddleOCR-VL tutorial](https://paddlepaddle.github.io/PaddleOCR/main/en/version3.x/pipeline_usage/PaddleOCR-VL.html).

**Bezier and character detectors** (ABCNet, DeepSolo, CRAFT, TextSnake) remain
research prototypes. A paper or a model zoo is not a Windows TensorRT backend
with a reviewed weight license. They are the right idea for true curves, and
they are not ready to drop behind `Recognizer`.

**Cloud OCR** would upload camera and video frames. It is out of scope for
this local magnifier.

## Fair local evaluation

Run this only when the parent's GPU and codec jobs are idle. Do not upload
the private frames. Do not compile a TensorRT plan during someone else's
build. Use the inference-only SDK and already cached plans for the baseline.
A new profile may be prepared once, on purpose, and then timed from cache.

Frozen inputs:

- The three screenshots above, unchanged.
- The Clipchamp video, one paused frame at 00:25 and one frame a few hundred
  milliseconds away, matching the existing neighbour-proof rule.
- The book source that produced the first two screenshots, once its filename
  is confirmed. Do not substitute `2_av1.mp4` without checking the picture.
- The existing labelled stills from the width study: problem video at 55 s
  and 89 s, the earlier dense frame, sparse print, long lines, and critical
  symbols.

For each configuration record separate rows, and do not add them into one
"OCR milliseconds" number:

- Time to first useful text. Cold: process start until the first admitted
  word, including plan build if a plan is missing. Warm: the next completion
  on the same pixels after one discarded warmup. Publish both.
- Region refresh: after a pan or zoom that reveals new pixels, the time until
  a new word appears, and whether old proved words remain.
- Detection and recognition stage intervals, host merge, admission, and paint,
  using the existing trace. Whole-worker time stays labelled whole-worker.
- Peak allocated tensors and, if the driver reports it, process VRAM. The
  256 MiB workspace cap is not a measurement.
- Cancellation: set the flag at the start of recognition and during a batch.
  Record when `Recognize` returns and that the UI thread did not block.

Score the three screenshots by hand, because they have no ground-truth file:

- Exact word coverage: human transcript of each visible word, then the
  fraction whose admitted string matches, case-sensitive, with the denominator
  written out.
- False replacements: a plate whose string disagrees with the ink it covers,
  counted separately from omissions.
- Geometry: center error in source pixels, angle error for any quad we keep,
  and the fraction of plate pixels that overlap a different word's plate.
  The current ten-pixel edge allowance is a regression tolerance, not a
  success threshold for this study. Report the errors.
- Omissions that stay original ink. A missing plate is not a success just
  because nothing wrong was drawn.
- Dispositions, so a word rejected by `IsHighRiskToken` or low confidence is
  not called a detector miss.

The oriented-paint trial and the contour-quad trial use the pinned medium
map. The visible-region trial OCRs the magnified book crop and the tilted
panel crop at native resolution, one tile, and compares coverage with the
full-frame job. UVDoc and docTR enter only if those three trials leave a
coverage or collision gap. docTR runs offline on the same frozen bitmaps;
it does not join the app until the numbers say so.

Promote nothing from one RTX 4090 median. A later RTX 3050 run uses the same
bitmaps, a plan built on the 3050, and the same warm and cold rows. Three
repeats describe a median. They do not describe a 95th percentile.

## Unresolved

- The book video's filename, and whether `006_av1.mp4` lives under another
  directory. The Clipchamp file is confirmed.
- Whether Paddle's current PP-OCRv6 runtime defaults to four-corner quads or
  can emit a denser polygon. The checked PaddleX sample is four corners.
  The v6 post-process candidate cap was not published beside the v5 YAML.
- Inverse-warp error for UVDoc on this book. The DocUNet CER does not answer
  that, and it does not say the overlay would land on the right ink.
- docTR weight license and an actual GPU timing. The CPU table is not a
  reason to expect a faster worker than 200 ms.
- PaddleOCR-VL-1.6's own license file and any word-level coordinates. The
  0.9B card's Apache-2.0 line was not re-read as the 1.6 card.
- Surya's single-request latency at concurrency 1. The published table starts
  at concurrency 32.
- RTX 3050 worker time, VRAM, and cold-plan time for the current medium pair.
- Whether the Learn package-identity sentence will apply to the unpackaged
  desktop build on the Windows version we ship. It is not evidence that
  today's fallback is failing.
- Korean, Cyrillic, Arabic, Hebrew, and Indic coverage if those camera
  languages become a requirement. That is a separate model review. The
  screenshots in this task are English.
