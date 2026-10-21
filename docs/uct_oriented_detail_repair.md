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

Post-fit duplicate reconciliation removes a second observation of the same ink
without merging adjacent identical words. The painter also suppresses duplicate
observations and leaves original pixels where different strings claim the same
ink. Protected omissions, glyphs and plates use the same eligibility decision.
The protection closure remains bounded. No interpolation grants tracking proof.

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
once that pair finishes. Refinement preserves prior independently proved text;
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

Early rotation-only evidence on the owner's 19,000/19,033 ms menu pair changed
actually painted words from 2 to 11 while keeping strict proof and zero image
egress. That intermediate overlay still has a button-color/ink-height defect;
it is not final visual acceptance. On the 25,000/25,033 ms pair, one previously
painted `Q` no longer passes word proof; the source is a magnifier icon.
Counts alone are not accuracy. Final combined color/geometry evidence and the
tracked background gate must be recorded before promotion. Native desktop,
Windows screen-reader, camera and packaged-release acceptance remain separate.
