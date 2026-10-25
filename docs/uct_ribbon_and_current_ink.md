# Curved-line OCR and offline current-ink diagnostics

Implementation experiment, 24 October 2026, based on `caf523e0` (Windows 0.3.1
development). The owner subsequently requested main-app integration and then
all-mode use: curved-line sampling is automatic in every existing eligible OCR
job when UCT is enabled. This is
not a measured accuracy improvement, release acceptance, or a new precision policy.

## Explicit scope

The owner lowered the default minimum recognized word height from seven to
five analysis-source pixels on 25 October. Confidence, independent source proof,
glyph collisions, original-ink protection and the painter's six-pixel projected
minimum remain separate. A five-pixel source word can become drawable when
magnified; admission alone does not promise a replacement at 1x.

Offscreen paint receipts now include every prepared word's source height,
confidence, disposition and ink-fit status, plus the terminal omission reason
of every admitted word. `OKUFLOW_UCT_RESIDENT_PAINT_HEIGHT_CONTROL=7` adds a
`paint-height7/` control alongside `paint/`, using the exact same completed
OCR result and source. This does not perform another inference or manufacture
a temporal observation. It isolates the height rule from recognition variance.
An independent follow-up restores ribbon crop padding at both line ends using
the fit's existing local unclip margin. The quadratic and affine height are
rebased onto the same [0,1] interval; exact image-boundary/positive-height checks
remain in force. An extended crop outside the source is refused and protects
original ink. Straight components retain the rectangle arithmetic. The
diagnostic-only `Fit(..., false)` control omits endpoint padding; app callers
use the default. Comparisons retain separate pre-padding and padded binaries.

The model-free ribbon is enabled by default on **live camera, advancing video,
Freeze and paused OCR jobs**, including the existing paired Temporal Smooth
path. It follows `EvidenceAcquisition`, not the selected-frame diagnostic flag.
The scheduler still refuses unknown/crossfade pixels before recognition; a held
acquisition never becomes a new observation. A temporal blend still requires
its separately owned exact current frame for all existing validation/proof.
The scheduler sets and clears the processing context around exactly one backend
call, and the automatic adapter forwards it without changing the selected model
tier. Windows OCR ignores it.
`OKUFLOW_UCT_CURVED_RIBBON=0` retains the rectangle diagnostic control;
unset/`1` uses curved sampling where eligible. Other explicit values disable it.
No additional UI toggle, saved setting or model switch is needed. The existing
OCR cadence, full-read deadlines and worker/slot limits do not change;
this does not run a neural read for every presented camera frame. This integration is
development source, not the published 0.3.1 installer.

Both Paddle CPU and TensorRT production paths carry the ribbon through detection,
recognition windows, rectification and CTC source geometry. CPU and CUDA scoped
checks and resident BF16 measurements have completed, as recorded below. Real
curved-page quality, native Freeze-to-paint latency and device/release acceptance
remain separate.

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
stream. It downloads only the existing compact crop/token metadata. The GPU
scan uses one 256-thread block per component with a fixed-order reduction;
see the review round and measured whole-worker cost below.

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
original owned image. On the paired Temporal Smooth path, the blend is only the
recognition input: references, source identity and current-match checks still
come from the separately owned current half. Nothing is read synchronously from
the UI or present stream.

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

Explicit immutable diagnostic selection refuses paired input and frozen detector-crop replay;
the detector must execute afresh in each arm. Existing harness bounds/preflights
and strict capacity diagnostics remain. Its direct default factory is FP32;
do not label this command as a BF16 application measurement. For shipped BF16
timings repeat the same recipe with
`$env:OKUFLOW_UCT_RESIDENT_PRODUCTION_FACTORY='1'`, which uses the production
factory's BF16-preference catalog (same-tier FP32 fallback per role) and pins
the actual per-role precision from each job's recorded backend detail. Use
only prepared warm plans: the harness refuses builder-capable SDKs, applies
the ordinary cached-startup deadline and fails if the plan-cache inventory
changes, so a cold cache is a refusal, never a build. Capacity 1 with the
shipped tiny pair is the N1-relevant arm; do not relabel direct-FP32 numbers
as BF16. This does not alter the application's BF16 defaults.

The measured production run used `OKUFLOW_UCT_RESIDENT_EXACT_N8=1`,
`OKUFLOW_UCT_RESIDENT_CAPACITIES=8`, one round, 20 passes and
`QTEST_FUNCTION_TIMEOUT=600000`; profiling was off for the main comparison.
The separate stage visits used two passes with profiling on. The selected
converted graphs must exist under the test executable's `models/` directory,
which is the production factory's actual discovery path. Use explicit original
model and inference-only SDK roots. ROOT used a private process-local
`LOCALAPPDATA` containing verified copies of existing warm plans; the owner's
model/cache directories were never inference write targets.

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

### Five-pixel admission and crop-end padding (25 October 2026)

Both changes use the existing shared worker path in live camera, advancing
video, Freeze/paused and paired-current OCR. No new read, worker, model switch
or presentation wait is added. Live/paired source proof is unchanged;
`curvedRibbonAllFrameWorkerContexts` now also checks five-pixel candidates on
live, advancing and paired-current submissions. Its high-contrast source patch
retains the original current-frame match assertions. The initial smooth-gradient
fixture could not provide enough contrast for that additional small patch;
the failed attempt is preserved, rather than weakening proof.

The combined follow-up was measured against clean `b4930956`, with the original
ribbon already enabled in both executables. RTX 4090, TensorRT 11.4.0.106,
CUDA 12.9.86, Qt 6.12.0, medium BF16 detector/recognizer, fixed1024 N8. Four
visits used baseline/candidate/candidate/baseline; each source had one warmup
and 20 timed passes per visit. The first four timed passes were excluded in
advance and retained in raw evidence: 32 steady samples per arm/source, 256
total. Profiling and paint export were off. All retained steady jobs confirmed
the same actual BF16 plan keys/payloads and zero image-egress calls. All four
visits confirmed completed State retirement;
all 18 private cache files stayed byte-identical. No engine was built.

Resident worker milliseconds, median / descriptive nearest-rank p95:

| Input | Prior curved path | Five-pixel + padded ends | Added median |
| --- | ---: | ---: | ---: |
| Synthetic curved line | 8.661 / 9.568 | 8.710 / 10.105 | +0.050 (+0.57%) |
| Synthetic straight line | 8.903 / 9.631 | 8.919 / 9.742 | +0.016 (+0.18%) |
| Book photo | 38.979 / 39.736 | 39.377 / 41.387 | +0.398 (+1.02%) |
| Dense frame 55 | 144.147 / 151.913 | 144.214 / 152.965 | +0.067 (+0.05%) |

These are combined worker costs on this GPU, including scheduler preparation;
the two changes were not timed independently. Shared-desktop timing variation
remains possible. More admitted words may also affect model/paint work, which
this worker timer excludes. This does not measure native live camera latency,
Freeze-to-paint time or FPS, and must not be added to the earlier separate
ribbon-off/on comparison as though it were one controlled experiment.

The earlier offscreen sample isolates the height rule with the identical OCR
result: whole-book painted words 362 at seven pixels, 381 at five. Adding end
padding gives 386. The bottom four left-page lines show 25, 33 and 31 drawn
words respectively; padding fixes `ranslation` to `translation` and the curved
synthetic `1` to `18`, but other spellings change in both directions. Dense-frame
counts are 231, 242 and 241; straight-control PNG bytes are identical. Counts
are coverage, not accuracy. Actual omission tracing attributes the original
11 bottom-line admitted omissions to 10 protected-ink decisions and one
geometry/projected-size decision. Neither protection nor the independent
six-pixel projected painting minimum was lowered.

Final tracked scoped validation: CPU **5/242 PASS**, CUDA **4/254 PASS**,
PCH-OFF **2/242 PASS**, zero skips/unresolved in those selections. The shipping
release compiled. The broader tracked matrix remains **FAILED**: CPU resume
refused changed shared `PATH` inputs for the pre-existing unresolved
`video_playback_contract`; that case was not retried unchanged or cleared.
The CUDA delta retained **15 passed / 5 optional skipped / 0 unresolved**.
Native GUI and installer checks were not run; these are development results,
not full release acceptance.

Commands, source/binary seals, failed attempts, ledgers, unchanged cache hashes
and the four raw visits are under
`Z:/Projects/OkuFlow/local-work-data/uct-height5-integration-20261025-r1/`.
The measured campaign is `worker-20261025T103406Z-b9d7668933dd47b2921c6af15b8b39bd/`
(`plan.json`, `summary.json`, `complete.json`); the earlier actual paint samples
and comparison are under `uct-height5-paint-20261025/`. No public release or
native desktop operation is implied.

### All-mode extension (25 October 2026)

The owner requested curved sampling on existing live/advancing OCR jobs as well
as selected frames. `curvedRibbonAllFrameWorkerContexts` exercises the real
scheduler with scripted OCR on live, advancing and paired-current inputs;
references remain original pixels, the paired current check still runs, the
context clears after each call and unknown provenance never enters inference.
This is a CPU protocol fixture, not physical-camera or real live GPU inference.

The scoped CPU gate passed **4/242**, including this new row. The broader tracked
build compiled release/CPU/CUDA and the CUDA delta passed **15**, with **5
optional skips**, zero unresolved. CPU completed **166 passed / 9 skipped /
1 unresolved**: `video_playback_contract` failed its unmute/resume PCM assertions.
Its production/test source is unchanged from `62d7c236`; the cause is unconfirmed.
The first failure/logs are retained and the case was not retried unchanged. The
overall tracked matrix is **FAILED**, not full validation or release acceptance.

The separate PCH-OFF build passed the ribbon/ink contract plus source-equivalence
guard (**2/242**, zero skips/unresolved). Native desktop/hardware validation was
not run for this extension.

An optional N8 resident invocation with camera/paired settings was refused by
that benchmark's existing media/single contract before any model/GPU work. It is
not live inference evidence. The separate selected-input GPU fit, rectification,
source-proof and both-role BF16 sample evidence remains as recorded below.

Word-box follow-up reuses and freshly verifies the original sample/job/PNG hashes.
The straight control has identical ordered OCR output and identical painted PNG
bytes (**4 → 4** words). The dense flat frame has **232 → 231** painted items:
one long URL's replacement is withheld for invalid geometry (**1/232, 0.43%**),
and the original pixels in that source area are unchanged. The other **231**
spatially matched painted items retain the same text. This is a two-image sample,
not a flat-image accuracy guarantee or a zero-loss claim. Matching uses greedy
source-box IoU ≥0.5; split/merged words can appear as additions/omissions.

Transparent SVG word-bound overlays, the box comparison gallery, exact matching
records, full failed matrix and scoped receipts are retained under
`Z:/Projects/OkuFlow/local-work-data/uct-ribbon-all-frames-20261025/`. No original
image is repainted to manufacture a result. Boxes are source bounds of actually
painted words; orange dashed references also show former omitted locations.

### Main-app integration validation (24 October 2026)

`scripts/agent_check.ps1 -Files tests/uct_paddle_tests.cpp` passed its four CPU
cases. The tracked `scripts/agent_build.bat` then compiled the shipping CUDA
application, all CPU targets and all CUDA test targets with Qt 6.12.0, VS2022,
CUDA 12.9.86 and the actual OCR adapter headers. Its explicitly desktop-free
matrix completed: CPU **167 passed / 9 skipped / 0 unresolved** (five passing
cases contain optional skipped Qt rows); CUDA delta **15 passed / 5 skipped /
0 unresolved**. Reuse required matching per-test fingerprints. The actual
`uct_ribbon_cuda_contract` and `uct_paddle_cuda` passed. Optional GPU playback
cases remain skipped; native GUI was explicitly excluded. This is background
development validation, not full shipping/installer acceptance.

A separate PCH-OFF configuration compiled `uct_paddle_tests` and passed both
`uct_ribbon_ink_contract` and `shared_source_equivalence` (SCOPED **2/242**,
zero skips/unresolved). Frozen merge oracle bodies are unchanged from `caf523e0`.
The real BF16 sample runs also completed both State retirements normally and
left all 18 private cache files unchanged. Logs, ledgers, sample/code/image
hashes and the self-contained gallery remain in the integration evidence root
named below. No native desktop, phone, installer or public release ran.

### Visual samples from actual OCR

Set `OKUFLOW_UCT_RESIDENT_PAINT_SAMPLES=1` alongside the existing resident
diagnostic's explicit immutable/single/media settings and Qt offscreen. Each
source warmup exports `paint/block-<block>-source-<index>/original.png`,
`uct.png` and `sample.json` under that run's new evidence directory. The actual
completed GPU result goes through production standalone admission and the Qt
snapshot painter. The receipt binds the source, job, PNG hashes, candidate,
admitted and actually drawn counts, and individual word positions/text. This
extra work occurs after job/callback timing; use a separate run for visuals.
Compare explicit ribbon `0` with unset/`1` using identical models and source
pixels. More candidate or painted words do not establish higher accuracy.
These offscreen exports omit application chrome and are not native screenshots.

The 24 October integration sample run used the same warmed medium BF16 pair and
unchanged private plans in both arms, with explicit `0` versus the unset default.
Actual candidate/admitted/painted words were book **373/347/276 → 449/388/362**,
dense frame **263/243/232 → 262/242/231**, and both synthetic controls **4/4/4**.
The curved synthetic line regressed from the correct `Curved page reading 18`
to `Curved page reading 1`; the straight control kept `18`. The gallery includes
this error, original ink, both actual PNGs and admitted text. No accuracy claim
or new startup quality gate follows from these samples. Their fresh receipts,
commands and interactive gallery are under
`Z:/Projects/OkuFlow/local-work-data/uct-ribbon-integration-20261024/`.

### Final root measurements and scope

RTX 4090, TensorRT 11.4.0.106, CUDA 12.9.86 and Qt 6.12.0. Both selected
medium roles loaded warm BF16 plans, verified by graph/key/payload hashes on
every successful job. No engine was compiled. The same final CUDA executable
(`a999b6811b3d2a5e9c16adbfff4a2d5343de1fa255b2b2f03a1d1b363ec6aadf`)
ran off/on/on/off, one warmup plus 20 timed passes per source/visit. The first
four timed passes were excluded in advance and retained: 32 steady samples per
arm/source, 256 total. Nearest-rank p95s are descriptive shared-desktop results.

Resident worker milliseconds, median / p95:

| Input | Ribbon off | Ribbon on | Median added | Candidate words off/on |
| --- | ---: | ---: | ---: | ---: |
| Synthetic curved line | 8.838 / 12.572 | 8.998 / 10.783 | +0.160 (+1.81%) | 4 / 4 |
| Synthetic straight line | 9.558 / 11.314 | 9.314 / 10.106 | -0.244 (-2.56%) | 4 / 4 |
| Book photo | 38.617 / 39.878 | 39.225 / 40.471 | +0.607 (+1.57%) | 373 / 449 |
| Dense frame 55 | 142.868 / 146.685 | 143.691 / 145.829 | +0.823 (+0.58%) | 263 / 262 |

The straight result is timing variation, not evidence of a speed optimization.
The separate two-pass profiled visits prove active ribbon work: 1 curved-line,
0 straight-line, 27 book and 2 dense recognition crops; no unsupported crop in
these four sources. More candidates are not verified correct words. No new
human-truth book score or actual painting/reading acceptance is inferred.

Recognizer input/output capacities stayed 4,718,592 / 76,636,160 bytes in both
arms; this is not a peak-memory measurement. Descriptors grow even when off;
their default-off cost against the pre-feature binary remains unmeasured.
These timings cover the actual scheduler/resident worker and callback, with
owned upload/capture setup outside steady time. They do not include camera,
decoder, UI admission or paint, and do not establish native Freeze latency/FPS.

The CPU component comparison alternated original/optimized/optimized/original,
with 31 measured AB/BA pairs per visit. Original and optimized source/map/crop
hashes matched. Current-ink median fell from 3.590 to 2.055 ms per tested word
(about 43%); p95 3.822 to 2.247 ms. The original 72 changed/72 unchanged controls
still have zero false accepts/hides/uncertain. Held-out 24/24 retain zero false
accepts, **8 false hides and 4 uncertain**, improved from 12 false hides. The
oracle remains offline; cost and nuisance errors do not justify live authority.

Final tracked CPU selection: **12/242 PASS**, including the unchanged frozen
merge oracle, controller and source-equivalence guard. Three accessibility
suites skip only their independently selected timing row, which passed. Final
CUDA selection: **4/254 PASS**, including the real ribbon fit/rectifier and
existing GPU OCR regressions. Original build/refusal/defocus/merge failures and
the benchmark's stale backend-name expectation are preserved. No native desktop,
physical phone, installer, full release gate or publication ran.

All 19 changed code/build-input pins and 18 private plan copies matched after
execution; owner/source plans were unchanged. Raw visits, profile/callback
records, code/binary seals, cache inventories, exact commands and failed attempts:
`Z:/Projects/OkuFlow/local-work-data/uct-ribbon-ink-20261024/root-final/`.
Start with `worker-summary.json`, `primitive-summary.json`, `source-frozen.json`
and `final-preservation.json`. These measurements preceded the owner's automatic
selected-frame integration request. Measured compute cost is small here; real curved-page geometry and
word quality still need evaluation before expanding that scope.

See the frozen implementation handoff for exact head/binary hashes, commands and
preserved original failures. The following paragraphs describe the earlier
implementation-only validation; the final root results above supersede its
then-unrun GPU/performance status.

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

## Post-implementation review and optimization round

### Root validation repairs

The first post-review CPU run preserved a real defocus-control failure: matched
blur of both normalized grids still called unchanged box-defocused ink Changed.
The diagnostic now also evaluates a fixed 3x3 box point-spread model before
contrast normalization, in either direction. It can only demote Changed to
Unverifiable at the existing cutoff. This is an offline nuisance model, not
permission to accept text or a claim that scale/defocus is solved.

The GPU fit uses fundamental shared arrays and constructs its `Bin` objects on
the reducing thread; shared storage requires no dynamic object initialization.
`uct_ribbon_cuda_contract` runs the actual parallel fit, analytic centerline and
CPU/GPU source rectification checks. Profiles now count valid and unsupported
ribbon recognition crops so an inactive branch cannot be reported as curved work.

Production-factory diagnostics verify both converted graphs and digest-valid
warm BF16 plans before model work, then match both role keys/payload hashes and
actual precision on successful jobs. Exact-N8 production visits may profile the
stages; direct/private exact-N8 restrictions stay unchanged. Use an isolated
process-local LocalAppData cache snapshot for measurement, not owner cache writes.
The recorded root CPU/CUDA gates exercised these repairs; earlier failures
remain intact. The first CUDA build also exposed a mixed float/double extrema
overload unavailable to device code, repaired with explicit double operands.

The frozen merge replay subsequently exposed an ordinary-rectangle regression:
larger padded protection is not unique to ribbons. Inheritance now requires
refused ribbon geometry (or its invalid, unknown-error carrier), not merely an
expanded rectangle. A replacement overlapping that unresolved envelope remains
invalid geometry, so existing model admission retains a non-carvable original
mask. Tests cover replacement chains and preparation as well as bounds; the
frozen baseline oracle is unchanged.

The ink alignment loop specializes its four exact integer/half-pixel phases
outside the inner sample loop. It retains all 81 shifts and symmetric/tie rules;
it does not subsample, approximate a search or add per-pixel deformation.

Muse Wonder Full at maximum reasoning performed the independent source review
and initial optimizations. It built and ran nothing. Its predictions were
subsequently checked by ROOT's builds, regressions and measurements below.

Correctness fix: overlapping-word merge could drop a refused ribbon word's
non-carvable envelope when a duplicate candidate won or replaced it.
The initial fix inherited any enlarged protection. ROOT's frozen replay
disproved its assumption about baseline rectangles; the narrower rule above
replaces it. `curvedRibbonMergeProtection`
covers both orders, the unchanged baseline, windowed decode against an
independent analytic map, and bin fill-order invariance (counts/extrema
exact, fit coefficients within summation rounding), which bounds CPU/GPU fit
parity to tolerance rather than byte equality.

Optimizations, with straight outputs and all refusal bounds preserved: the
GPU ribbon fit now uses one 256-thread block per selected component with
fixed strided pixel ownership and a fixed-order reduction instead of one
serial thread scanning up to 262,144 labels; counts and extrema stay exact
and no atomics, transfers or extra buffers appear. The fit cost lands in the
existing Regions profile stage. Scalar/batch GPU rectification reads its
quad by reference and evaluates only the selected sampling branch, so the
disabled path no longer copies the enlarged descriptor per pixel. CPU
detection skips bin filling when the unchanged length refusal already
applies, and CPU ribbon rectification evaluates centerline terms once per
column with the same double operation order as `ribbon::Geometry::At`.
Host/device `gpu::Quad`/`ribbon::Geometry` layout is locked by
`static_assert`s. The 32-segment arc estimate is recomputed per line/window;
caching it would change the descriptor ABI for negligible per-line work and
was left alone.

Current-ink robustness: imported counts stand as the baseline record
(72/72 with 0/0/0; held-out 24/24 with 0 false accepts, 12 false hides,
0 unverifiable). A matched `[1,2,1]/4` blur of both normalized grids now
demotes a sharp Changed verdict to Unverifiable (Blur) when the blurred
worst residual reaches the existing 0.035 unchanged level; Unchanged still
requires the sharp residual, so false accepts cannot increase by
construction. `Result::blurredResidual` records the second score (-1 when
not scored) in both diagnostics. Independent defocus controls assert a
blurred unchanged word is never Changed and a blurred edit is never
Unchanged. Scale drift beyond blur tolerance is candidly unresolved, no
scale search or cutoff retuning was added, and the verifier remains
offline-only with no live authority.

Residual review notes: the straightness gate is in detector pixels while
word error is limited in source pixels, so heavily downscaled sub-gate
curves keep the pre-existing baseline rectangle behavior; tightening it
would change straight outputs and needs measured evidence. Vertical lines
(width < height in the PCA frame) never fit on either backend.
