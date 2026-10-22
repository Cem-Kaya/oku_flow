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

## Cooperative CUDA evaluation, 2026-10-23

The parent kernel launched 32 threads per request but thread 0 ran the whole
algorithm serially; its FP64 grid sampling, line fits and two-division inverse
maps per ink corner then ran at single-lane latency. The helper is now generic
over an execution team. CPU workers use a one-member `SerialTeam` with the same
evaluation order. CUDA uses one 128-thread block per request: grid sampling,
the ink-plane rescan, side-fit hypotheses and refits are split across threads,
and paper components flood level by level with atomic claims, while seeds
are still taken in raster order. Combined values are integer counts, minima/maxima whose ties
keep the first serial occurrence, or quarter-integer sums exactly representable
in FP64. The CPU additionally reuses a horizontal ink neighbour's two mapped
corners, which come from bit-identical source points. Thresholds, stages,
the 128-request and 16K-sample caps, both contexts, refusals, fallback geometry,
source ownership and the public kernel interface are unchanged.

A private harness built the production `uct_kernels.cu.obj` twice from this
checkout's own fresh CUDA test cache (parent `4fa8ffb` and this change). It
linked each object with the matching CPU header and recorded every output bit for
4,410 requests: 2,250 jittered captured words on the receipt-bound 7000-ms frame
and 2,160 generated labelled cards (four labels, three sizes, five shears, two
tapers, three scales, white/green paper). CPU stage, fit and corner bits match
on all requests. CUDA fit decisions match on all requests (2,666 fitted both
times); nine fitted results differ by one ULP in a corner coordinate, at most
1.1e-13 source pixels. Restructuring changes where the compiler contracts a few
FP64 multiply-adds, so CUDA output is decision-identical but not universally
bit-identical to the parent. The separate CPU/GPU acceptance difference is unchanged in both
builds: captured jitter fits 887 CPU and 890 CUDA requests, and generated cards fit 1,776
each. This corpus is not a proof for every input.

The tracked runner then passed `uct_paddle_processing` and `uct_paddle_cuda`
(2 of 245, no skips or unresolved selections) from that cache. This scoped
pass is not release acceptance; the app target was not rebuilt.

The timed parent/optimized comparison was not run. The owner started the
application for testing before the timing step, so GPU times would not have
been comparable; parity runs used the harness's untimed mode. The earlier
parent-only table above remains the only timing evidence. No speedup is claimed
until a same-session comparison runs on an otherwise idle GPU.

## Measured cooperative and filtered CUDA, 2026-10-23

The owner application had exited before this run; no OkuFlow process existed
before or after any timed run. Each run used the same hash-bound harness: 5
warmups and 31 samples per workload, with CUDA event medians/95th percentiles
in milliseconds. Runs alternated ABBA: parent, cooperative, cooperative, parent;
then cooperative, filtered, filtered, cooperative.

| Workload | Parent | Cooperative | Filtered |
| --- | ---: | ---: | ---: |
| One Head rectangle | 4.82 / 4.24 (p95 5.35 / 5.18) | 0.232-0.246 (p95 0.235-0.254) | 0.212 (p95 0.214-0.219) |
| Ten captured candidates | 5.66 / 6.19 (p95 6.35 / 7.03) | 0.246-0.260 (p95 0.247-0.264) | 0.226 (p95 0.232-0.234) |
| 128 repeated Head | 4.59 / 4.90 (p95 5.43 / 5.50) | 0.236-0.249 (p95 0.240-0.250) | 0.215-0.216 (p95 0.217) |
| 128 jittered generated card | 7.92 / 7.95 (p95 8.44 / 8.41) | 0.336-0.355 (p95 0.337-0.356) | 0.268 (p95 0.269) |

CPU medians stayed within run-to-run noise: 0.07-0.09 ms, 0.89-1.02 ms, 8.7-9.3 ms
and 14.3-15.3 ms respectively. Every run kept the 4/5 captured-mix CPU/CUDA
fit counts. Host launch-plus-wait adds about 0.02 ms for small batches and
0.1 ms at 128 requests. These are warm geometry times on one Ryzen 9 5900X /
RTX 4090. They are not end-to-end, sustained-FPS or other-GPU claims.

The filtered variant keeps FP64 for sampling, the plane, line fits and every
reported extreme. In the ink rescan, each corner is first mapped in FP32 with
a conservative bound on its distance from the FP64 map. Only corners that
could reach an extreme, or whose denominator is near the FP64 failure limit,
are mapped again in FP64 and compete with their serial first-occurrence order.
The CPU never uses this path. Against the cooperative build on the same 4,410
requests, CPU output is bit-identical and every CUDA fit decision matches; 19
fitted results differ by at most 2 ULP (1.14e-13 source pixels). A missed
extreme would move a corner by a sample spacing, so these are rounding-level
differences from where the compiler places FP64 contractions, not filtering
losses. Against parent, 18 fitted results differ by at most that size, again
with no decision change. Tracked `uct_paddle_processing` and `uct_paddle_cuda`
then passed (scoped 2 of 245, no skips/unresolved). Per-sample raw arrays were
not retained; each run's output keeps median, p95, minimum and maximum.
