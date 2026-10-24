# Curved-line ribbon and current-ink experiments

Implementation experiment, 24 October 2026, based on `caf523e0` (Windows 0.3.1
development). Defaults stay unchanged. This is not a measured OCR improvement,
release acceptance, or a new model/precision policy.

## Explicit scope

`OKUFLOW_UCT_CURVED_RIBBON=1` enables the model-free ribbon on **selected,
immutable, single-frame** Freeze/paused inputs. The controller marks only its
selected frame; advancing/live inputs and corroborating neighbours do not gain
this context. Temporal Smooth, held/composite/unknown provenance cannot select
the experiment. The scheduler sets and clears the processing context around
exactly one backend call, and the automatic adapter forwards it without changing
the selected backend or model tier. Windows OCR ignores it. Unset/`0` preserves
the established rectangle path. This setting is deliberately absent from the UI
and saved settings pending review and measurements.

Both Paddle CPU and TensorRT production paths carry the ribbon through detection,
recognition windows, rectification and CTC source geometry. The CUDA implementation
has not yet been compiled or exercised in this task: only CPU compilation and
desktop-free correctness work were authorized. Keep its native GPU correctness,
runtime ownership, quality and cost acceptance pending.

The **current-ink verifier is CPU/offline only**. No per-frame application call,
hide trigger, native GPU capture, scheduling change or reuse permission is wired.
Freeze is immutable and needs no change checker. In particular, an `Unchanged`
verdict neither admits text nor postpones authoritative full reads. The old NCC
policies, full-read cadence, BF16 preference/same-tier FP32 fallback, tiny N1,
CPU FP32 and one-worker/two-recognition-slot ownership remain in place.

## Ribbon and bounds

`uct_ribbon.hpp` retains a quadratic centerline and linear half-height profile
in a component's PCA frame. A window is a `start/end` interval on that same
geometry. Its backward sampling map is

```
t = start + (end-start)*u
P(u,v) = origin + along*t
       + across*(a + b*t + c*t*t + (2*v-1)*(h0+h1*t))
```

`along`/`across` include detector-to-analysis pixel scaling. Cross sections use
the fixed PCA transverse axis, rather than intersecting curve normals. Positive
height and a nonsingular basis make this quadratic shear one-to-one. This does
not correct gutter foreshortening, recover hidden print or flatten a page mesh.
No ABCNet/UVDoc code, weights or dependency is included.

Fitting uses 24 fixed cross-section bins, at least 20 supported bins and both
ends, component/PCA length at least 24 detector pixels, at least 3 pixels local
height and length at least twice that height. The existing 128-component limit
and original-plus-eight recognition-window limit remain. Both component and
bounding scan are capped at 262,144 detector pixels per fit; no per-map-pixel
bin array is allocated. The GPU experiment retains labels with a separate
prefix-offset buffer, then scans at most 128 selected components on its existing
stream. It downloads only the existing compact crop/token metadata. The serial
per-component scan is intentionally a candidate for the separately commissioned
Opus cost review.

The straightness gate is the larger of 0.75 detector pixels and 8% local height.
Below it, rectangle coordinates, rectification/zero padding and CTC output retain
their established arithmetic. Fitting refuses excessive slope, missing support,
large center residual, unstable heights and any exact quadratic boundary extremum
outside the source. A supported curved hypothesis that then fails these checks
is marked unsupported; its rectangle OCR may still run diagnostically, but all
resulting words have invalid geometry and protect the complete detector envelope,
retained across every overlapping recognition window so merging cannot shrink it. Earlier
refusals without enough evidence to identify a curve retain the baseline path.

CTC operates on the same normalized `u` spans. Four mapped corners are **chord
quads**, not exact curved glyph reconstruction. The exact maximum deviation of
this polynomial ribbon from its chord is `abs(c)*delta_t^2*length(across)/4`, in
original analysis-image pixels. Both the word and widened ink-search span must
stay within the smaller of **1 pixel** and **8% word height**. Unsupported spans
have `kInvalidGeometry`, omit their text and preserve a conservative envelope of
the original curve as non-carvable protection. `ribbonErrorPx=-1` records an
unsupported fit whose chord error is unknown; otherwise it records the mapped
word sagitta. This bound measures the representation error, not detector accuracy,
CTC alignment accuracy or the quality of the fitted page hypothesis.

Supported words retain even a nearly horizontal quad through CPU/resident ink
fitting, scene conversion, painting and positioned accessibility. Reading rows
use the median source centerline points/heights, rather than the curved envelope's
vertical center. The same model eligibility and omission/protection inventory
feeds the reading pane, painter and Qt accessibility projection; native UIA-client
acceptance remains separate.

## Composition with rotation, keystone and reprojection

The source of a ribbon is the **original owned analysis image**, already after
camera rotation, stabilization and keystone, before appearance/viewport zoom.
It is not a raw sensor frame. Let `H` denote that existing raw-to-processed
geometry and `A` the stamped processed-scene-to-analysis-copy mapping. Detection
operates in a local region of `A(H(raw))`. The complete chain is:

```
raw --H once--> processed scene --A--> owned analysis image
    --> detector region local pixels --> ribbon P(u,v) / OCR tensor
    --> CTC u spans --> word chord in region pixels
    --> +region origin --> analysis pixels --inverse A--> scene coordinates
    --> current presentation ViewTransform --> overlay logical pixels
```

No ribbon code reapplies or inverses `H` during painting. The crop origin is
added once by `PaddleMergeRecognition`; `PrepareLines` inverts the job's stamped
view once; current presentation reprojection runs once. A geometry-generation
change still revokes old authority. Analytic tests compose a projective keystone,
rotation/crop transform, local crop, stamped analysis mapping and a different
presentation transform, then check the actual production CTC/merge/preparation/
reprojection output against the known map.

Rectified tensors are recognition inputs only. Worker colors, ink fitting,
source fingerprints, line/word references and registration continue sampling the
original owned image. The existing paired-current-half rule is unchanged;
smoothed inputs cannot enter this initial ribbon experiment. Nothing is read
synchronously from the UI or present stream.

## CPU current-ink oracle

`uct_current_ink.hpp` captures a privately owned word plus three-pixel halo from
the exact analysis image. It requires matching stamped image dimensions, refuses
smoothed/unknown/composite input and geometry/viewport changes, and samples at
analysis pixel density. Limits are 512 columns, 96 rows and 32,768 samples;
oversize/out-of-image inputs are unverifiable, never silently shrunk. A decimated
probe cannot satisfy the original stamp's dimensions. This is **analysis-image
resolution**, not a claim of sensor-native resolution.

The comparison oracle consumes two bounded owned luma grids. It normalizes
bounded photometric changes using 2%/98% quantiles, rejects weak contrast and
large contrast/edge-energy changes, and chooses one global symmetric translation
over a two-pixel radius in half-pixel increments. It does not independently dilate
each stroke. A summed-area table scores the worst 4–12-pixel local window in
either comparison direction. Provisional residual cutoffs are 0.035 (unchanged)
and 0.10 (changed); intermediate/unsupported observations are unverifiable.
These are experimental constants, not calibrated safety thresholds. Alignment
can conceal some edits, while scale, blur and noise can falsely flag unchanged
text. Ordinary recognition errors remain accepted by the owner's precision policy.

Live resident integration would need an independently bounded exact-current
capture/lease, owned reference storage, a CUDA comparison kernel, cancellation,
slot-pressure evidence and shadow disagreement/cost telemetry. Those are **not
implemented**. The owned-grid CPU oracle is the resident-ready algorithm seam,
not evidence of native GPU or live coverage.

## Correctness and diagnostic commands

Use an isolated CPU build and `OKUFLOW_AGENT_EXCLUDE_GUI=1`, at most eight build
jobs. The new `uct_ribbon_ink_contract` runs under Qt **offscreen**, with analytic
detector/sampling maps, straight-path byte equality, component/buffer/refusal
bounds, curved-word fallback, composed geometry, actual worker original-pixel
proof, snapshot painting and Qt positioned-text queries. It opens no native
desktop window and performs no inference. Existing processing/controller, frozen
merge replay and accessibility regressions remain required.

Set `OKUFLOW_UCT_RIBBON_EVIDENCE` to a fresh private directory before running it
to retain `ink-held-out.json` and deterministic `curve-typeset.png` /
`straight-typeset.png`, sibling truth text and `ribbon-fixture-inputs.json`.
The manifest pins the installed Arial font hash, source-authored text, analytic
warp, source-pixel hashes and word spans. No font file or raw evidence is tracked.

The existing optional 12-edit diagnostic retains all six original NCC summary
rows unchanged and appends `current-ink-cpu-offline` for 72 changed / 72 unchanged
original-resolution edited-token controls. It does not duplicate native evidence
for D2/D4 baseline probes. Run separately:

```powershell
$env:QT_QPA_PLATFORM='offscreen'
$env:OKUFLOW_UCT_CHARACTER_DIAGNOSTIC='1'
$env:OKUFLOW_UCT_RIBBON_EVIDENCE='<fresh-private-evidence-directory>'
build/msvc-cpu/tests/uct_paddle_tests.exe optionalCharacterChangeCpu
```

The held-out diagnostic adds 48 cases: installed Arial/Segoe UI at 19/31 px,
identity, true bilinear half-pixel jitter, 2.5% scale, 3x3 blur, integer jitter and
exposure. Report changed/unchanged, false accepts, false hides and unverifiable;
completion is not a perfect-accuracy gate. Physical unchanged phone/camera pairs,
occlusion/defocus distribution and screen-reader/native GPU acceptance remain open.

## Deterministic A/B measurement entry points (do not run without compute grant)

Model-free primitive measurement uses the same production functions, fixed
analytic map/source and original-resolution edited-ink fixture:

```powershell
$env:QT_QPA_PLATFORM='offscreen'
$env:OKUFLOW_UCT_RIBBON_BENCHMARK='1'
$env:OKUFLOW_UCT_RIBBON_EVIDENCE='<fresh-private-evidence-directory>'
build/msvc-cpu/tests/uct_paddle_tests.exe optionalRibbonPrimitiveAB
```

It records five warmups plus 31 alternating AB/BA pairs, exact source/map/crop
hashes, rectangle/ribbon detector-fit, rectify, CTC/map/merge stages and total
primitive-stage sum plus separately labelled wall time including diagnostic
hashing; old NCC and new current-ink time/verdict separately. There
is no model, neural worker, display or camera timing in this microbenchmark.

For **actual resident whole-worker** time, reuse the existing production
`optionalResidentBaseline` harness with the same inference-only SDK, read-only
pinned models/plans and identical inputs for both arms. See
[its complete preflight](uct_resident_baseline.md). For example, after ROOT's
serial grant, and with the CUDA test executable/Qt/SDK on the intended paths:

```powershell
$env:OKUFLOW_UCT_RESIDENT_DIAGNOSTIC='1'
$env:OKUFLOW_UCT_RESIDENT_PAIRS='medium:medium'
$env:OKUFLOW_UCT_RESIDENT_CAPACITIES='1,8'
$env:OKUFLOW_UCT_RESIDENT_ROUNDS='1'
$env:OKUFLOW_UCT_RESIDENT_PASSES='5'
$env:OKUFLOW_UCT_RESIDENT_SEED='20261024'
$env:OKUFLOW_UCT_RESIDENT_STAMP='media'
$env:OKUFLOW_UCT_RESIDENT_INPUT='single'
$env:OKUFLOW_UCT_RESIDENT_IMMUTABLE='1'
$env:OKUFLOW_UCT_RESIDENT_PROFILE='1'
$env:OKUFLOW_UCT_RESIDENT_IMAGES='<fixture-root>/curve-typeset.png;<fixture-root>/straight-typeset.png'
$env:OKUFLOW_UCT_TENSORRT_ROOT='<existing-inference-only-sdk>'
$env:QTEST_FUNCTION_TIMEOUT='300000'
$env:OKUFLOW_UCT_CURVED_RIBBON='0' # repeat as 1, then alternate visit order
build/msvc-cuda-tests/tests/uct_paddle_tests.exe optionalResidentBaseline
```

The new immutable selector refuses paired input and frozen detector-crop replay;
the detector must execute afresh in each arm. Existing harness bounds/preflights
and strict capacity diagnostics remain. Its direct default factory is FP32;
do not label this command as a BF16 application measurement. A separately
approved production-factory experiment is needed for both-role BF16 timings.
This does not alter the application's BF16 defaults.

The harness records scheduler whole-worker and submit-to-callback time separately
from CUDA stage intervals, resident preparation and host waits, plus source/model/
plan/executable identities, ordered output, geometry refusals and crop hashes.
Ribbon metadata is included in the crop descriptor hash; frozen crop plans are
also bound to rectangle/ribbon mode. Compare omissions, word errors, changed
geometry and actual plate/positioned-text evidence; worker timing alone does not
establish first visible text, Freeze latency, FPS or sensor-to-photon latency.
The same-build off/on comparison isolates active ribbon work. Compact crop and
word descriptors have grown even with the switch off; measuring that default
metadata/allocation overhead also requires ROOT's separate base/candidate binary
comparison. Pixel equality does not imply zero latency overhead.

## Validation evidence

See the frozen implementation handoff for exact head/binary hashes, commands and
preserved original failures. Performance, GPU inference, native desktop, phone,
release/bundle and installed-app acceptance are intentionally unmeasured here.

The CPU diagnostic on this machine completed 72 changed and 72 unchanged
Consolas controls with 0 false accepts, 0 false hides and 0 unverifiable. The
held-out 24 changed / 24 unchanged cases had 0 false accepts, **12 false hides**
and 0 unverifiable (half-pixel resampling, scale and blur). These small synthetic
counts do not establish live character-change safety; the false-hide result
specifically supports retaining offline-only scope. The historical NCC rows are
retained in the same new report, separately from its 2026-10-20 archived reports.


The frozen CPU configuration built the application and all CPU targets using
Qt 6.12.0 / VS2022 with eight build jobs. The tracked scoped build selected
`uct_ribbon_ink_contract` and passed. The final explicit CPU gate selected 12 of
242 CTest cases: ribbon/ink, Paddle processing, both precision contracts, core
UCT, immutable controller, frozen merge replay, shared-source equivalence,
accessibility scales 1/1.5/2 and timing publication. All passed; the three scale
suites deliberately skip their separately selected timing-publication row, whose
own case passed. There were no unresolved native failures. This remains SCOPED
CPU/offscreen validation, not shipping release/CUDA/native desktop acceptance.

The first ribbon contract failed on near-horizontal quad retention and a hidden
snapshot fixture. Preparation now preserves every ribbon quad; the fixture
requires Qt offscreen, waits for actual paint and checks positioned text. It
keeps all original assertions. The first failed attempt/logs remain in the tracked
ledger and private evidence. Initial `agent_check` attempts also correctly refused
incomplete fresh Ninja inventories until the tracked scoped build populated them;
a misspelled controller test selection was refused before execution. No failure
was erased, timeout increased or quality assertion weakened.

The separate PCH-OFF configuration also compiled `uct_paddle_tests` and passed
`uct_ribbon_ink_contract` plus `shared_source_equivalence` (SCOPED 2/242). The
UCT fixture itself does not use PCH in either configuration; the sharing guard
checks the wider consumer environments. This is not a complete PCH-OFF app or
release matrix.
