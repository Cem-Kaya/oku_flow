# Source-supported perspective text

Rotating replacement text cannot reproduce a receding surface. The painter
already maps four corners projectively, but the detector starts with a principal-
axis rectangle. That rectangle does not identify the surface's shear or taper.

The optional worker refinement examines a closed, locally uniform source
surface around an already fitted word. It samples a bounded paper-color grid,
finds a connected enclosing component and requires four independently supported
straight sides. Rounded corners are outliers. Missing source pixels, open/cropped
surfaces, weak contrast, inconsistent sides or unstable projective mappings
retain the earlier geometry. A source rectangle is evidence here; a word's
spelling, filename or neighboring frame is not used to invent a plane.

After finding the plane, the worker fits the actual source ink in that plane's
coordinates and maps its bounds back to four source corners. Existing geometry,
candidate confidence, minimum source size, references, protection, overlap and
clipping checks still apply. Already projective word geometry is preserved.
This does not reconstruct arbitrary curved pages or invisible surface corners,
and does not promise recovery at every depth/angle.
If a fitted word box also includes foreground-like pixels outside the observed
surface, refinement refuses it. It does not trim those pixels away to force a
new shape; large lettering near a strongly sheared card edge is one such case.

The rule has no word, filename or screenshot-specific branch. Its limits are
dimensionless or source-size bounds. In the word's current local frame, a
supported side can lean by up to 2.5 horizontal pixels per vertical pixel;
top/bottom slopes are bounded to 0.5. The projected ink envelope may extend
sideways by half its old height times `(1 + measured side shear)`. This follows
the observed surface instead of assuming upright letters. Vertical excursion
and bounding-area checks still limit the result. A measured slant alone cannot
supply hidden corners or recover an unobservable surface.

CPU and resident CUDA use the same first-party decisions. At most 128 candidates
per result are considered. Each uses a 96 by 40 surface grid, four bounded line
fits and at most 16K ink samples. A second grid expands vertical context from
four to eight source word heights, and horizontal padding from eight to sixteen
heights on each side, only when the first has no enclosing component;
it cannot override an invalid plane or a nearly upright result. This covers small
print on larger cards without an unbounded search. GPU results are compact corner metadata; no
image is downloaded and no model/backend/dependency is added. Work remains on
the existing recognition worker/stream and owns the exact source through its
completion. These finite work bounds are not hard latency guarantees.

Projective words retain their own fitted ink height. A common screen-space font
height across a line would flatten a near/far size difference. The same cached
four-corner plan drives live paint, saved overlays and positioned character
bounds. Ordinary affine line typography retains its shared size/baseline policy.

`sourceSurfacePerspective` in `uct_paddle_processing` covers both shear signs,
multiple spellings, source sizes and tapers, white/green surfaces, refusal controls and actual near/far glyph/character
geometry. `gpuOrientedInkMatchesCpu` additionally checks resident corner parity.
The generated 108-case matrix combines three unrelated labels, two font/card
ratios, three source scales, three signed shears and two tapers. It also retains
the outside-surface refusal above. These source images are generated independently
of the video and its words. CUDA compares 16 positive corner fits with CPU,
including the larger-context path.
Private real-frame evidence lives under local-work-data `uct-perspective-20261022`.
Native UIA, arbitrary surface recovery and release acceptance are separate.

## Development validation, 2026-10-22

CPU/PCH-off: tracked `ultra_clear_text_contract`, `uct_paddle_processing`,
`immutable_uct_contract` and `shared_source_equivalence` passed 4/240, no skips
or unresolved selections (153.20 s). CUDA/PCH-on: processing, resident primitives
and the source guard passed 3/246, no skips/unresolved selections. The application
Release target compiled. These scoped passes are not full release acceptance;
the earlier unrelated phone-contract blocker and desktop/hardware work remain.
The initial geometry-fixture and Windows `near` macro compile failures are retained.

The cache-only production-factory replay of clip PTS 7000 ms has 12 actual paints
from 12 scene words and zero GPU image egress. Head/Left/Hand/Right/Hand receive
the measured surface shape and paint before and after settled zoom. Focused views
paint 8/8 then 7/7 eligible words. The clipped Display/Location label returns to
source pixels after the detail read; this is not universal coverage or a native
UIA acceptance claim. Its genuine 7033-ms comparison frame is diagnostic only:
standalone admission does not borrow temporal evidence.

## Measured geometry cost

A private Release harness links the exact tested `SurfaceFrames` object and calls
the same CPU helper on already fitted source metadata. On Ryzen 9 5900X / RTX
4090, CUDA 12.9, five warmups and 31 samples per batch gave these medians:

| Input batch | CPU helper | CUDA kernel | CUDA 95th percentile |
| --- | ---: | ---: | ---: |
| One accepted Head rectangle | 0.096 ms | 4.91 ms | 5.67 ms |
| Ten fitted candidates from this capture | 1.01 ms | 6.15 ms | 6.99 ms |
| 128 repeats of that accepted rectangle | 8.72 ms | 5.10 ms | 5.47 ms |

The 128-repeat case is a finite workload, not a worst-case bound. Different
surfaces and GPUs can cost more; small batches do not necessarily run faster.
These times exclude OCR, initialization, allocations, transfers, decoding and UI.
The captured batch's host launch/wait median is 6.25 ms. The recognition worker
owns this work, but it can still contend for GPU time; no sustained-FPS or hard
deadline guarantee follows from it. Raw results, source/object hashes and the
private harness are under local-work-data `uct-perspective-20261022/cost`.

This timing harness is not an acceptance or parity gate. Its captured fitted
metadata gives four optional CPU fits and five CUDA fits; the CPU retains prior
geometry on its refusal. That count difference was retained when the first
diagnostic assertion exposed it; the cost report lists the counts independently.
The separate 16 generated corner comparisons pass. Neither result establishes
universal bitwise CPU/GPU agreement or complete actual-image quality.

The private paused diagnostic can report `surfaceCpuReplay` refusal stages for
still-affine words by reading its already owned source PNG. This is a CPU replay
of optional geometry only; it changes neither resident output nor admission and
does not download an image from the GPU.
