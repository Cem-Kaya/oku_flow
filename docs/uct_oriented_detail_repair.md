# UCT tilted text and paused visible detail

This development change keeps the detector's four text corners through CTC
word mapping, ink fitting, scene coordinates and painting. Previously the
recognizer rectified tilted text correctly, but later stages reduced its
geometry to an upright bounding rectangle. Large upright plates and incorrect
letter heights could then cover the source. This is a geometry repair, with
unchanged model weights, precision, batch defaults and fixed-width plans.

## Geometry and paint

`TextQuad` stores reading-order corners. CPU and CUDA keep the CTC word spans
in that coordinate frame. Tilts of about two degrees or perspective deformation
use local ink fitting; smaller upright words retain the rectangle path.
The painter maps the glyph and background through all four projected corners,
including perspective height changes. Its actual painted-character bounds feed
accessibility and image exports. Invalid geometry keeps the original pixels.

The oriented ink fit permits a complete glyph body occupying at least one
quarter of a loosely padded detector height, with a four-source-pixel floor.
The former 35% height guard rejected readable small print and kept oversized
detector boxes, producing tall letters on book pages. The 35% width guard,
sample-count, contrast and detached-mark checks remain. A controlled real-font
fixture reproduces the rejection; a thin-rule control must still fall back.
The seven-pixel word-size boundary permits only 1e-5 source pixels of arithmetic
error. A failed-fit isolated near-vertical character retains the original
symbol, preventing the close-button X from becoming a stretched X or 4.

Post-fit duplicate reconciliation removes a second observation of the same ink
without merging adjacent identical words. The painter also suppresses duplicate
observations and leaves original pixels where different strings claim the same
ink. Protected omissions, glyphs and plates use the same eligibility decision.
The protection closure remains bounded. No interpolation grants tracking proof.
Projected uncertainty cuts and word shapes use one half-open pixel-centre
coverage grid. Independently rounding both sides outward previously made
disjoint fractional spans overlap by a pixel, suppressing a newly proved word
and sometimes its neighbour. Tiny punctuation without a covered centre still
protects a conservative pixel. The same plan governs live paint and exports.

## Paused-frame checks

A selected paused frame remains immutable. A real neighbouring acquisition may
have rotated even though the selected image is now held. Comparing only
translated upright patches rejected some readable words in that case.
`PlanQuadReference` and `BuildQuadReferencePatch` normalize samples in each
word's own four-corner frame. The neighbouring worker compares the same exact
token at its detected destination. It does not move the selected glyph.

The existing NCC 0.94, visible fraction 0.95 and two-probe-pixel refinement
remain. Distinct acquisition/source identity, the 250 ms neighbour window,
high-risk token exclusions and smoothed-input current-frame checks remain.
Intrinsic edge sizes must retain the existing 75% ratio. Samples are bounded to
64 by 12 cells; resident references stay on the GPU and only match verdicts
return. Invalid, nonfinite, crossed or out-of-image quads cannot establish proof.
This adds no optical-flow reuse and no held-image temporal evidence.

## Visible detail after paused navigation

Pan and zoom still immediately reproject existing text without new capture.
After a 300 ms settled view, the controller may submit one selected/neighbour
refinement pair from their retained full-scene images. It uses the current
visible source rectangle, only if the backend supports detail and detector
resolution improves by at least 25% over established full-scene passes.

The detector's long edge stays at most 960 pixels. The requested enlargement
is clamped to 1–6 times; a tiny region is expanded around its centre to respect
that bound. This is a maximum enlargement, not a promise to recover blurred
characters or perform OCR at the display refresh rate.

At most 32 regions are attempted for one selection. Completed and abandoned
regions are retained in that bounded coverage list, so panning back cannot
create a retry loop. A finer scale may earn another pass. A new source/seek or
settings authority resets it. A view that changes during work is reconsidered
once that pair finishes. If the settle deadline expires while presentation is
latched failed or the backend is still preparing, the recovery event itself
(presentation success after a failure, or the backend becoming ready) asks
once more for the settled view, because no view change or OCR result follows
such a recovery; ordinary receipts do not restart the settle timer.
Refinement preserves prior independently proved text;
new glyphs still require a distinct real neighbour.

## Validation and limits

The model-free geometry fixtures cover tilted/perspective fit and paint,
post-fit duplicates, adjacent identical tokens and conflict protection. The
controller cases cover settled navigation, coalescing, source authority,
failure completion and bounded non-evicting coverage. CPU word-reference tests
exercise changed ink, malformed geometry, high-risk tokens and source/acquisition
rejection. CUDA checks compare native resident and compact CPU descriptors at
1/2/4 decimation, including perspective, with score differences below 1e-5.

The private paused-video diagnostic uses existing medium/medium N8 plans,
an inference-only SDK, real owned CUDA images, production admission and
software paint. It saves original inputs, candidate/eligible/actually-painted
records and the overlay. `OKUFLOW_UCT_PAUSE_OUTPUT_ROOT` optionally selects an
absolute evidence parent; the default stays LocalAppData/OkuFlow. No cache
rebuild or model download is part of these checks.

Combined source `0bb7f27` passes the three focused CPU cases with PCH disabled
and four focused CUDA cases with PCH enabled: processing, scheduling, immutable
controller and the CUDA delta, with no skipped Qt rows. The new small-print
regression failed before the height-guard repair; its thin-rule control and
exact CPU/CUDA geometry checks pass afterwards. The presentation-recovery
regression likewise failed before its controller repair. Original failures
remain retained. These are scoped checks, not a complete release matrix.

Finite real-source proofs use the same cached plans, preserve exact TensorRT
retirement/cache inventory and report zero image egress:

| Selected source | Candidate words | Shown words | Actually painted |
| --- | ---: | ---: | ---: |
| Menu 19,000 ms | 27 | 14 | 10 |
| Menu 25,000 ms | 26 | 19 | 18 |
| Book 0 ms | 383 | 351 | 190 |
| Asset gallery 0 ms | 175 | 165 | 160 |

Raw token records/text order are retained (30/29/426/188 records respectively).
The menu/gallery candidate reductions include unresolved isolated symbols now
left as original pixels. The close-button X is no longer stretched into X/4;
the lower Hand fits at about 43 pixels. Book padding no longer produces giant
letters. Its shown count fell from 366 to 351 and actual paints from 206 to 190
after fitting and size/protection checks; these counts must not be labelled
accuracy. The book still contains misrecognitions and undrawn words.

The small brown `HUMAN CIRCUIT / MASTER ASSET LIBRARY` heading is recognized
in the full-scene gallery pass. A narrow focus exercises actual 4x and 6x
detector scales (240- versus 160-pixel source widths under the 960-pixel cap);
both retain the two words already visible there. No extra-word benefit is
claimed for that example. A separate 6x book region examines 11 candidates,
with worker times about 19/20 ms for the selected/neighbour jobs. These exclude
the 300 ms settle and are neither display nor screen-reader latency.

Private receipts, input/executable/source hashes and images are under
`Z:/Projects/OkuFlow/local-work-data/uct-geometry-r3-20261021/evidence/`.
The final runs use `final-*-1-receipt.json`; earlier intermediate evidence is
retained. Optional N1/N8 strict comparison failures remain separate failures.
Full-matrix/shared-source equivalence, native desktop, Windows screen-reader,
camera and packaged-release acceptance remain pending.

The fractional-protection follow-up adds eight offscreen real-paint cases:
upright and quad-framed words at two zoom levels and fractional pan offsets,
with the uncovered punctuation pixels checked unchanged. All eight failed
before the coverage repair. Afterwards `ultra_clear_text_contract` passes
all 205 Qt rows, including the existing glyph/plate eligibility cases, with
no skipped rows (31.84 seconds, CPU/PCH off). This focused result does not
complete combined or native acceptance; the original failed gate is retained.
