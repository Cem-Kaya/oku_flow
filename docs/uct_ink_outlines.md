# Source-ink outlines (development)

UCT can now sharpen some ink that its recognizer refuses, without inventing
recognized characters. On the first-party equation fixture this keeps the
written **1 above 2**, the fraction bar, the handwritten m and v, and the
separate smaller 2 above v. They remain source-derived shapes, not a substituted
fraction character or a reconstructed formula. This is not a math OCR engine.

The previous character-filter change could admit a recognized superscript but
could not recover symbols the line recognizer dropped. In the archived rectangle
control it painted `K=mv2` across the fraction. The curved control protected the
span instead. Relaxing geometry or confidence alone would expose that wrong
replacement. This change leaves those decisions intact and adds an independent
visual outcome for bounded, measurable original ink.

## Extraction and refusal

`PlanInkOutlines` selects protected spans already marked invalid geometry,
low confidence or unrecognized. Eligible recognized words retain their existing
path. High-risk and too-small tokens are not newly admitted. Acquisition must
be attributable to one source frame; unknown and crossfade inputs are refused.
Each region is at most 512 by 256 original analysis pixels, with at most 65,536
pixels. A job has at most eight nonoverlapping regions and 131,072 total pixels.
Oversize or unmeasurable ink stays as the original image.

Integer projection onto the measured background-to-ink colour axis creates a
bounded coverage grid. Four threshold levels feed deterministic marching
squares, with 24.8 fixed-point subpixel intersections and eight-connected ink.
CPU and CUDA use the same arithmetic and directed contour case table. The
central contour supplies the shape; no character dictionary, model output,
formula template or source-specific coordinates supply its strokes.

Extraction refusals distinguish bounds, low contrast, edge-crossing ink, contour limits,
open contours, unstable components, unstable holes, faint resolved ink and thin
ink. Resolved components and holes (at least four source-pixel squared in area)
must correspond across the central threshold and its two neighbours. The lower
threshold must have resolved central support reaching its extents within
max(2 pixels, 15% of the corresponding extent). This refuses a faint bar with
only a dark speck left at the central threshold. Across thresholds, nested
components/holes use their smallest containing owner, so circled marks are not
counted as part of the surrounding ring's topology.
Median estimated stroke width must be at least 1.5 source pixels. A region has
at most 8,192 segments per level and 256 loops; the final job has at most 16,384
vertices. Hole ownership is computed once rather than with a cubic search.

This is a bounded shape-stability check, not a proof that every photographed
mark has been identified. Tiny texture pinholes and dots can change topology
between thresholds. They are excluded from structural counts. A tiny central
ink island is amplified only if it contains an island at the strict threshold;
otherwise its original pixels remain untouched. Every hole in a painted ink
component remains a hole, including tiny pinholes. Negative hole contours are
never emitted without their painted ink owner. Refusal preserves the original image.
Oversize, overlapping and job-budget spans are skipped by planning before
extraction; they currently have no individual refusal receipt in production.

## Painting, ownership and reprojection

The worker caches vector paths. The overlay fills only the measured source-ink
contours using UCT's existing readable foreground and display palette. It paints
**no opaque background plate** for these outlines. Original texture and faint
marks outside their contours remain visible. This also distinguishes the
implementation from the initial proposed sketch design.
The paths follow mid-contrast edges: blurred strokes can look thinner than the
original blur. The glyph stroke-weight preference does not change these paths.

Current validation pixels, not temporally smoothed recognition pixels, supply
the shape. On CUDA, coverage, histograms and prefix scratch stay on the device.
Only capped contour segments and scalar diagnostics reach the host: never a
coverage grid, feature array or source-image readback. The existing worker,
owned image, fence/stream completion, retained cleanup and quarantine rules apply;
there is no second inference worker or UI/presenter readback.
The upper transfer bound is 524,332 bytes per region (four segment lists plus
the 44-byte summary), or 4,194,656 bytes for eight regions. If any level exceeds
its cap, the host skips all segment copies for that region and reports refusal.
Extraction exceptions quarantine retained GPU work. Cancellation is classified
as cancellation; other resident extraction faults conservatively refuse the
whole job under the existing input-fault policy, without a CPU readback fallback.

Existing rotation/stabilization/keystone has already produced the analysis
image. Preparation maps its contour points into scene coordinates once. Normal
line tracking adds the validated scene offset, and presentation maps scene to
view once. No second curve or reprojection correction is applied to the paths.
Paths are cached across pan/zoom rather than re-traced on the UI thread.

A valid line reference, current readability/freshness, its own protected mask,
and absence of immutable conflicts or accepted-glyph collisions are required.
Smoothed protected spans, as well as candidate words, first require direct
smoothed-to-current registration. The selected bounds and registration do not
depend on outline enablement, success or refusal. No outline bypasses admission
or replaces independently current glyphs on a registration mismatch. Admitted
smoothed results remain hidden until a current single-frame check validates
them. Failed checks withdraw them. Exact selected immutable frames retain their
artifact without periodic jobs. Source changes, Show original, disabled UCT,
window/modal suppression and cancellation use existing overlay revocation.
Native controls remain excluded from live painting. Saved overlay snapshots,
like existing glyph exports, deliberately exclude UI chrome from the image.

An outline never contributes to recognized words, reading-pane content,
screen-reader text or the painted-word count. Diagnostic `sketchedRegions` is a
separate count. It cannot make a wrong recognition correct or read an unread
equation aloud. Use Show original to compare.
An already rejected, current-matching span keeps the normal reading omission
placeholder regardless of whether its optional contour draws or is disabled.

## Controls and validation

There is no new UI preference or model dependency. With UCT enabled, this path
is available in existing camera, playing-media and selected-frame jobs. It does
not run on every captured frame or change inference cadence. Unset or `1`
`OKUFLOW_UCT_INK_OUTLINE` enables it; `0` (or another nonempty value) disables it
for an explicit A/B control. Default-off UCT remains unchanged.

`uct_ink_outline_contract` covers stacked/raised shapes, holes, subpixel edges,
faint/thin ink, borders, contrast, bounds and noisy saddle connectivity.
`uct_ink_outline_cuda_contract` compares exact CPU/CUDA paths and typed refusals
using strided resident input and geometry-only downloads, including noisy and
many-loop inputs. It also drives the production resident host wrapper with two
regions, an oversize planner skip, byte accounting and cancellation.
`protectedInkOutlinesSharpenWithoutAddingText` exercises real preparation,
model admission and painting, reading-text invariance, palette, native-chrome
exclusion, zoom, immutable retention, EMA validation and failed-check withdrawal.
It also checks smoothed mismatch cannot evict current glyphs and that reading
output agrees with/without successful contour geometry. Primitive fixtures cover
partly faint strokes, weak tiny islands and nested rings/marks. The resident
paired-input fixture registers rejected spans against actual matching/mismatching
GPU pixels without creating contours or relying on their success.
The offscreen font fixture explicitly loads the installed Segoe UI Symbol
fallback used by the native desktop; missing glyph boxes are not valid samples.

Private frozen-image diagnostics use `OKUFLOW_UCT_INK_OUTLINE_MANIFEST` with the
optional `uct_ink_outline_tests optionalFrozenImage` slot. This reports refusals
as refusals; a synthetic isolated contour image is not a native app capture.
The existing `optionalResidentBaseline` production-factory diagnostic records
`inkOutlineMs`, `inkOutlineBytes`, per-region refusals and actual painted outline
rectangles. Use warm unchanged plans and `OKUFLOW_UCT_RESIDENT_PROFILE=0` for
whole-worker timing, with separate correctness exports. Preserve failed attempts.

Real-image correctness and timing evidence is recorded with the implementation
review; synthetic contracts alone do not establish arbitrary handwriting,
video stability, native UI quality or release acceptance.

## Equation worker experiment, 26 October 2026

The following records the initial `ea66aa0d` algorithm before the review repairs
above. Fresh correctness/timing receipts accompany the repaired candidate; do
not treat the historical numbers as measurements of later code changes.

Hardware: RTX 4090, Windows driver 621.01. One unchanged CUDA test executable
(`2abf5a8c3163fe73805213016540573cdbf4ccfc0accfadc30bbeae17cf2446c`)
ran off/on/on/off visits with five timed jobs per source per visit: ten samples
per source per arm, 40 timed jobs total. Production medium detector and recognizer
both reported BF16-linear graphs (mixed arithmetic, FLOAT I/O), batch capacity
eight, unchanged warm caches and completed retirement receipts. Stage profiling
and paint exports were disabled during timing. Input upload/presentation is not
included; these are worker and callback observations, not native display latency.

| Input | Worker off median | Worker on median | Difference | Outline-stage on median |
|---|---:|---:|---:|---:|
| Original 1672x941 generated board | 87.49 ms | 87.45 ms | -0.03 ms | 2.00 ms |
| Decoded 1280x720 video frame | 38.69 ms | 39.05 ms | +0.36 ms | 1.54 ms |

Whole-worker differences are smaller than the observed scheduling spread
(original off 82.79..95.59 ms, on 83.70..92.12 ms; video off 34.79..40.61 ms,
on 37.12..42.42 ms). The negative difference is not a speedup. Budget roughly
1.5..2 ms for the measured extraction stage on these inputs; this small
experiment does not establish a worst-case cost for eight difficult regions.
Submit-to-callback median differences were -0.04/+0.36 ms. Geometry metadata
was 91,064/69,868 bytes per job; every timed job recorded zero image-egress calls.

Separate production-worker paint exports sharpened the stacked/raised original
ink on both sources, with one drawn source-ink region and eight painted OCR
words each. The original had one additional border-crossing region correctly
refused. Original and decoded paths retained 1,414/1,086 contour vertices and
nine resolved components, rather than inferring a count of mathematical symbols.
Reading text did not gain the equation. Other recognition errors, including the
existing `Fdcos` interpretation, remain; this change cannot repair them.

Raw jobs, plans, failed attempts, source hashes, contour diagnostics and ABBA
analysis remain under the private `uct-math-layout-20261026` and
`uct-resident-20261026-*` evidence directories. The executable was archived
before later source formatting. Formatting changes no arithmetic or decisions;
the final source is rebuilt and gated separately. No native promotional capture
or hardware release acceptance is claimed by these offscreen exports.
