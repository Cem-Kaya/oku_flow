# UCT tilted text and paused visible detail

This development change keeps the detector's four text corners through CTC
word mapping, ink fitting, scene coordinates and painting. Previously the
recognizer rectified tilted text correctly, but later stages reduced its
geometry to an upright bounding rectangle. Large upright plates and incorrect
letter heights could then cover the source. This is a geometry repair, with
unchanged model weights, precision, batch defaults and fixed-width plans.

## Geometry and paint

`TextQuad` stores reading-order corners. CPU and CUDA keep the CTC word spans
in that coordinate frame. Tilts of about two degrees, perspective deformation,
or a corner excursion exceeding half a source pixel use local ink fitting.
A small angle across a wide word can move its ends by several pixels; it must
not switch to an upright box merely because it is below two degrees.
Truly subpixel upright geometry retains the rectangle path.
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
and sometimes its neighbour. Original punctuation without a covered centre on
either axis retains conservative outward coverage. Subtraction remainders carry
explicit classification: only a tiny two-dimensional remainder with no centres
on either axis receives that fallback. Thin carved strips on a single axis add
no artificial bordering pixel. The same plan governs live paint and exports.

Immutable duplicate observations keep the original displayed geometry and
carve only that retained proved footprint. A wider incoming box cannot uncover
neighbouring uncertainty, and a narrower one cannot leave self-protection.
Masks for unsupported notation, explicit backend rejection or invalid geometry remain protected
when a nearby word is refined. Ordinary grouped prose can still be replaced
by independently proved individual words; source identity, references and
proof clocks remain unchanged.

Five additional model regressions reproduce wider/narrower duplicate carving
and cross-line operator, confusable-number and invalid-geometry mask loss.
All five fail before repair. Afterwards the focused CPU/PCH-off UCT contract
passes all 237 rows, no skips, in 32.33 seconds. The original failures remain
retained; combined active-executor and native acceptance are separate.

## Paused-frame checks

A selected paused frame remains immutable. A real neighbouring acquisition may
have rotated even though the selected image is now held. Comparing only
translated upright patches rejected some readable words in that case.
`PlanQuadReference` and `BuildQuadReferencePatch` normalize samples in each
word's own four-corner frame. The neighbouring worker compares the same exact
token at its detected destination. It does not move the selected glyph.

`PreparedWord::registrationSceneQuad` also retains the original neighbour-bounded
recognition search frame, independently of the fitted display quad. If the
fitted patch fails, the worker may compare one additional patch in that original
frame. This avoids treating two different valid ink crops as identical sampling
coordinates. It uses the same .94/.95/two-probe-pixel test and 75% intrinsic-size
ratio, not a lower confidence or correlation threshold. Both recognition results
must still contain the identical eligible token. There are at most two bounded
64-by-12 patches per word, batched in groups of 1024; no extra inference,
UI-thread pixel work, image download or display-footprint change is introduced.

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
is clamped to 1–8 times; a tiny region is expanded around its centre to respect
that bound. This is a maximum enlargement, not a promise to recover blurred
characters or perform OCR at the display refresh rate.

The default and cap increased from 6x to 8x after the historical measurements
below. The minimum requested source-context long edge is now `ceil(960/8) = 120`
pixels, bounded by the image. Existing detector alignment and proof requirements
remain; see [ordinary labels and 8x detail](uct_token_policy_and_detail.md).

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

The subsequent independent review found that outward rounding of a strip
thinner than one pixel could recreate that suppression during panning.
Sixteen additional real-paint rows cover 0.25-pixel strips before and after
words, boundary fractions 0.3/0.7, zoom 1/3 and upright/quad geometry. Eight
failed before the repair. Four additional rows retain a 0.6-by-0.6-pixel
punctuation mark beneath plate padding. The repaired focused CPU/PCH-off
contract passes all 232 rows with no skips (32.45 seconds).

Five matching-source GPU proofs at `051b0f2` keep raw selected/neighbour OCR
output identical to the previous evidence, remove no previously painted
word and retain zero image egress. Book paint coverage increases from 190 to
203 words, and its 6x detail view from ten to fourteen painted words: `to`,
`construction`, `and` and `rhythm` become drawable while `an` remains drawn.
These counts measure rendering coverage, not recognition accuracy. Misreads
and weak fitted geometry remain; the latest comparison is retained as
`evidence/runs/r5-same-source-quality-comparison.json` under the private
evidence root above.


### Immutable protection independent of word consumption (2026-10-22)

Each incoming candidate now commits as one bounded transaction. A single COW
snapshot covers all touched reading owners, appends and retirements, masks and
original/carved classification, counters, carrier, provisional IDs and proved
footprints. Cross-owner subtraction and its possible storage growth complete
before commit. Semantic-risk, ambiguous-quad or capacity refusal restores the
pre-candidate accepted state and index; it cannot leave an appended word after
discarding that candidate's punctuation. Snapshots are per candidate, never per
word, under the existing 1024-line/8192-slot/32768-mask limits. Retained words
keep their exact geometry, references and validation clocks. Independent
contradiction blocks retain their existing correction authority.

`OverlayModel::MergeImmutableCoverage` preserves candidate protection after every
word has been appended or deduplicated. Ordinary masks are first carved against
retained proved shapes; incompatible non-carvable artifacts are refused. A
containing reading owner stores the remaining masks without changing its words,
geometry, reference or proof clocks; otherwise one bounded, deduplicated mask-only
carrier stores them. Carrier storage shares the existing line/mask limits and
never supplies reading text or new evidence.

Carving now carries `TextQuad`, including the retained displayed shape for a
duplicate. A mask outside that quad is untouched. A partial diagonal cut whose
bounding rectangle includes unproved corners is refused conservatively, rather
than uncovering punctuation through a rectangular approximation.

Four consumed-word mask rows and an outside-quad punctuation case failed before
repair. The changed-input CPU contract passes all258 Qt rows, zero skips, in
32.69 seconds. Three integration failures from the initial carrier draft, a
later remaining grouping failure, and a runner recheck refusal are retained;
none was retried unchanged. This is scoped CPU validation, not combined
CUDA/native acceptance.

### Original thin marks and carved remainders (2026-10-22)

`AcceptedLine::carvedSceneRects` is a subset of the existing protected masks,
bounded by the same 32768-mask union cap. Untouched masks retain classification;
successful subtraction marks only its new residual rectangles as carved.
Ambiguous quad cuts, semantic-risk refusals and cap refusals leave both masks
and metadata unchanged. The subset follows mask-only/reading-owner transfers
and scene offsets, and is cleared with the accepted artifact on reset.

An original mask takes precedence when its exact rectangle also occurs as a
carved remainder, within a line, during transfer, or across different owners.
Conflict and hidden-source protection remain original. Common/UI scene words,
controller content/timing equality and proof conversions carry the subset.
Regional admission treats a change from carved to original at the same geometry
as stronger protection in its atomic no-withdrawal check.

New model-free regressions cover original vertical/horizontal 0.25-pixel marks
at zoom 1/3 with upright/quad words. Each first proves that plate padding covers
the coloured source pixel, then requires the protected pixel to survive both
export compositing and the live software paint while the word remains drawn.
Sixteen rows include eight shared-mask variants with different per-word
classification, so COW deduplication cannot skip an original mask.
The sixteen existing carved-strip rows keep their assertions and explicitly
classify their fabricated masks. Tiny two-axis punctuation now covers both
original and carved classifications. Model cases cover residual classification,
untouched originals, collision order, cross-owner precedence, repeated carrier
reuse, offset conversion and reset.

The fresh isolated `msvc-cpu` build uses Qt 6.12.0, PCH OFF and two compiler
jobs. All three requested targets compile. The coordinated tracked gate runs
eight exact CTest cases with `-ExcludeGui -Parallel 1`: all eight pass in
128.16 seconds, with zero failed/skipped tests and zero unresolved results.
`ultra_clear_text_contract` passes 300 Qt rows with no skips (36.24 seconds);
`immutable_uct_contract` passes 113 with no skips (84.14 seconds). Each of the
three accessibility scale cases passes 25 rows and skips its opt-in timing row;
the separately selected timing case passes all three rows. Both controller
timing selectors also pass three rows each. Total: 497 passed Qt rows and three
explicit scale-only skips, with the timing contract independently covered.

Byte-verified copies of the first-attempt ledger, CTest log, detailed Qt log and
JUnit XML are retained under
`Z:/Projects/OkuFlow/local-work-data/uct-original-mask-f6-20261022/evidence/`.
No unchanged retry, GPU, native app, real-camera or model work was run. This is
SCOPED CPU/PCH-OFF evidence; PCH ON, the complete release matrix and combined
root changes remain separate acceptance.
