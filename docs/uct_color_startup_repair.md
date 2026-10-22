# UCT local colors, backend status and readiness recovery

UCT keeps logical lines for reading order and validation, but each recognized
word now carries its own optional foreground/background pair through preparation,
admission and scene/export projection. A yellow selected button therefore keeps
dark lettering on yellow while adjacent buttons retain light lettering on dark.
Ink fitting uses that same local pair. Small refresh variations use the existing
brightness grid and color hysteresis; absent metadata retains the line fallback.

The CUDA worker measures line and word surfaces together and returns compact
color/geometry metadata. Word images are not downloaded. Inputs remain leased
through exact stream completion, and existing scratch caps/unknown-completion
quarantine remain in force. CPU fallback performs equivalent local measurements.
The line reference, omission protection and independent evidence rules do not
change.

## Neutral video ink (2026-10-22 development)

The atlas recording exposed a renderer color-policy defect: endpoint colors
such as `#99abab`, `#9dbec5` and `#a2b7b6` were treated as intentional chromatic
ink because their RGB channel spread exceeded 16. Adjacent words could therefore
alternate between pure white and muted cyan/green. The painter itself uses a
single opaque foreground and background per word, without a glyph gradient.

`ReadableColors` now reserves source-hue preservation for RGB channel spreads
of at least 64 and existing contrast of at least 7:1. Lower-chroma ink uses the
same solid black/white polarity treatment as neutral ink. This deliberately
normalizes some muted source colors; clearly colored, readable yellow/cyan/
purple labels retain their source foreground. Background sampling, brightness
hysteresis, minimum 7:1 contrast, geometry and protection are unchanged. It does
not denoise original video outside replacement plates or authorize repainting
an unproved word. Color work remains cached outside the pan/zoom paint path.

The maintained renderer rows include the three actual atlas colors and dark
tinted ink, plus upright/quad checks that readable chromatic ink is preserved.
The private paused proof can explicitly select the production factory with
`OKUFLOW_UCT_PAUSE_PRODUCTION_FACTORY=1`; this requires the reviewed local BF16
manifest, records actual backend status and checks both cache inventories.
The Windows-bridge diagnostic forwards detail focus to the resident backend.
Neither diagnostic changes the public factory or application settings.

Focused CPU/PCH-off validation passed 313 Qt rows, zero failed/skipped, in the
tracked `ultra_clear_text_contract` (36.07 s). Four new atlas/tinted-ink rows
failed against the old renderer before the color-policy change. Cached
production BF16 selected/neighbour/detail proofs passed on the actual atlas
frames with both direct resident and Windows-bridge startup. The direct proof
reported zero image egress; the bridge reported its two explicit CPU reads and
no additional image egress. Both preserve cache inventories and complete exact
TensorRT retirement. In these replays `14` is actually drawn sharply; the
owner's intermittent original-looking number is not reproduced by this proof.
The background outside replacement plates remains compressed source video.
Evidence is under local-work-data `uct-atlas-solid-colors-20261022` and the
`atlas-paint-bf16-*` receipts in `uct-geometry-r3-20261021/evidence/runs`.
`agent_check` refused incomplete Ninja dependency inventory with FULL required;
it is not reported as a scoped pass. Combined/native acceptance remains separate.

## Small upright source ink (2026-10-22 development)

The saved 6x book detail exposed sampling aliasing rather than evidence for a
universal font-size clamp. Its upright `to` detector encloses narrow stems at
source columns 645 and 649. The fixed two-pixel body lattice misses those dark
cores (luma 72--92), yielding foreground `#bbb4ad` against paper `#e8e3dc`.
The resulting midpoint near 204 admits blur and disconnected next-word ink.
This is source-color/geometry evidence, not proof that OCR spelling is correct.

`color_policy::BodySampleStep` visits every body pixel for bodies of at most
4096 pixels. Larger bodies keep stride two. All three CPU/CUDA body passes
share that integer decision. Perimeter votes, RGB medians, opaque quartiles
and oriented coordinates keep their existing rules. The dense path has at most
4096 reads per body pass and adds no histogram, device allocation or readback.

Upright fitting labels eight-connected components on a weak/AA mask halfway
between the ink/paper midpoint and paper. A component needs a midpoint-strength
core inside the original detector; whole connected strokes may extend outside
it. Detached marks retain the existing 22%-height/two-pixel vertical allowance.
Disconnected padding ink is excluded. An anchored component spanning opposite
search edges preserves detector geometry rather than silently losing a glyph.
Unanchored rules/adjacent lines spanning those edges are excluded. Original
row-band, detached-mark, eight-sample, contrast and 35% width/height fit guards
remain. This keeps connected AA instead of filtering pixels to one ink RGB.
A substantial weak-only in-detector component also preserves the whole detector:
a lighter glyph must not be silently discarded while fitting its neighbours.

The grid keeps its roughly 16K sample budget and refuses more than 32768 cells,
8192 source columns or 4096 source rows. CPU uses an exact bounded queue. CUDA
uses fixed shared bit/label/row scratch (about 46 KiB) and at most 64 propagation
scans; incomplete propagation returns the complete detector. Such cap fallbacks
can differ from exact CPU fits. The oriented fitter and its 48 varied button
fixtures are unchanged.

The downstream seven-source-pixel minimum is unchanged. A five/six-pixel core
cannot be padded to seven or used to bypass admission. The source regression
requires actual connected AA rows to keep `to` drawable, and explicitly requires
the six-pixel `an` body to retain detector fallback under the existing 35% guard.
Its larger fallback remains a geometry limitation, not a precise ink fit.

`uct_small_ink_test::Run` adds 33 source/controlled cases to the existing
`orientedInkAndMapping` and `gpuOrientedInkMatchesCpu` slots: all four lattice
phases, both polarities, chromatic paper, gradients, AA, dots, descenders,
connected clipped strokes, nearby disconnected ink, rules/noise and winding
component fallback, including connected lighter ink and weak-only glyph refusal.
The exact 46x36 crop is embedded in
`uct_book_crop_test_support.hpp`; its packed RGB digest is checked independently.
Original PNG SHA-256:
`df25907f060d7b8f8a59b39dfc1b633950500b0fc3941fe6a77eacee6089868f`;
crop `[630,252,46,36]`, packed RGB SHA-256
`b8d7336e070cd539f50a35523e98efe03db377486fff5b122c83ec6016c9c36b`.
No resampling/recoloring is applied. Detector/search metadata is taken from the
saved proof and translated only by the crop origin.

The first registered CPU baseline reproduced the exact saved defect:
foreground `#bbb4ad`, source fit `[641,266,14,8]`. It failed as expected (37 Qt
rows passed, one failed, none skipped); its first failure ledger and full output
remain retained. After repair, CPU/PCH-off `uct_paddle_processing` passes all
38 Qt rows with no skips (1.60 seconds); CUDA/PCH-on `uct_paddle_cuda` passes all
52 rows with no skips (8.63 seconds), including all 33 new cases and the existing
oriented controls. Both are tracked **SCOPED** gates, not release acceptance.

The exact crop now measures `to` as `#615b54` on the same `#e8e3dc` paper and
fits source `[642,267,9,7]`, identically on CPU and resident CUDA. Its seven rows
are actual connected source AA pixels, not an inflated five-pixel core. Both
prepared words remain candidates under the unchanged seven-pixel minimum.
`an` keeps `#615c57` and its original detector fallback
`[655.7547169811321,261.29469450464785,9.056603773584904,18.29944423459611]`;
this is not a successful six-pixel ink fit. The fitted word gap is at least
four source pixels. GPU cases retain zero host image egress; the winding case
retains the complete detector while the exact CPU queue fits. Complete ordinary
fits/colors match CPU exactly; cap fallback deliberately need not.

Exact source restoration initially retained older timestamps, making Ninja
report no work. This was caught before the repaired gate: the original logs were
preserved, the two owned source timestamps advanced without changing bytes,
and both affected targets rebuilt. No pass was inferred from stale objects.

`agent_check.ps1 -Base 01697be` refuses automatic selection because the private
target-only build lacks the complete Ninja dependency inventory. No broad build
was substituted; the two explicit tracked gates above remain scoped evidence.

No new neural inference, model preparation, whole-source cached-model proof,
native display/screen-reader, broad matrix, accuracy, performance or release
acceptance is claimed. The test source identity/recognizer metadata is fixed;
model output is not ground truth. Private proposals, raw failures, archived Qt
logs, executable/source hashes and scoped receipts are under
`Z:/Projects/OkuFlow/local-work-data/uct-small-ink-20261022/`.

## Tilted button color repair (2026-10-21)

The oriented-geometry draft still measured colors in an axis-aligned word box.
On the menu's tilted light `Right Hand` button, that box's corners/perimeter
crossed the dark panel. Its inverted pair made the oriented ink fit capture the
button: `Right` grew to 67 source pixels high while `Hand` was about 40.

The CPU sampler and resident CUDA color kernel now apply the existing robust
local policy in the detected word's bilinear frame, including body samples,
corner votes, the immediate one-pixel perimeter and the wider qualified ring.
Clipped samples are ignored. The measured pair feeds the oriented search-frame
ink fit. Color alone cannot distinguish letters from an identical dark panel
inside search padding: the fit first removes eight-connected ink components
spanning opposite search edges in that local frame. Components touching a
single edge or corner remain, protecting legitimate stems/dots. The existing
dominant-band/detached-mark policy then fits the interior glyphs. CPU uses a
bounded queue; CUDA packs ink plus six component reachability bits per cell
(four sides, compatible ink and interior support), with parallel whole-run
propagation, about 46 KiB shared scratch and no per-word device image/mask
allocation. GPU propagation stops after at most 64 scans;
incomplete labels return `fitted=false` and preserve detected geometry, never a
partial fit. CPU remains exact/linear, so pathological incomplete GPU fits do
not claim CPU geometry parity.
The existing 24-channel-level RGB compatibility policy anchors entire connected
ink components, rather than filtering individual pixels. Anchored glyphs keep
their darker cores, lighter strokes and antialiased edges. Paper-enclosed
interior components also remain regardless of RGB variation. Unanchored edge-only
fragments, or unanchored components bridging two search edges, are unrelated
surround; an unanchored boundary component extending into the interior instead
preserves the complete detected word when it reaches the existing eight-sample minimum. It may be a
real glyph missed by the representative ink quartile; smaller isolated AA
fragments cannot veto the complete fit.
The grid retains its roughly 16K sample budget, caps rows at 4096 and columns
at 8192, and refuses more than 32768 cells. No global palette, confidence change
or word omission is used. The CUDA color and ink samplers share floor/bilinear pixel addressing with
non-fused double arithmetic for CPU parity. Axis-aligned words/line fallbacks
retain their rectangle policy. Input leases, completion/cancellation handling,
quarantine and the four-MiB resident metadata budget remain in force. Only
compact colors and fitted coordinates return to the host.

`uct_geometry_test_cases.hpp::TiltedButtonColors` exercises the real scheduler
with Segoe UI `Right Hand`, a light button, a dark surround and an adjacent rule,
at three positive/negative tilts with and without perspective, plus leading,
top and bottom search-edge variants, with uniform and varying ink (48 cases).
The varying case puts a black dot and a light-gray letter around measured gray
ink, requiring complete bounds/marks or conservative whole-word fallback. Its search
padding crosses the identical dark surround and the adjacent light rule.
It requires the
actual chromatic source pair, all four glyph-fit corners, a surviving word gap,
unchanged candidate/protection metadata and exact CPU/resident results, with
guarded image egress fixed at zero. Restoring the draft CPU sampler reproduces
the defect: foreground becomes paper `#f4f0f6` instead of ink `#1f1923`.
The winding-component CUDA regression requires the 64-scan incomplete fallback
and zero fitted coordinates, while the exact CPU queue completes. The corrected
focused CPU and real-CUDA gates are scoped development
checks, not combined geometry/paused-registration or release acceptance.

At color-only commit `4f5ea6b`, three cached menu25/25033 GPU proofs preserve 29 candidates, 24 production
ink fits, 21 shown words and 20 actual paints, with zero image egress and exact
TensorRT retirement/cache-inventory checks. The upper `Right`/`Hand` quads fit
at 53/41 pixels (draft 67/40); the lower pair fits at 60.20/43.01 pixels. All four
report completed fits. The earlier lower-Hand 76.14-pixel unanchored fallback
is corrected by retaining paper-enclosed components. The separate X detector/
painter issue remains outside this repair. Counts do not establish OCR accuracy
or final visual acceptance; the draft's 22 shown/21 painted is not improved by
these counts, and combined paused registration remains a separate change.

Final selected/neighbour worker medians are 400.46/96.03 ms across three
processes, versus the historical draft's single 279.31/98.65 ms observation.
The selected-worker increase is not explained by this comparison: these totals
include inference/startup and do not isolate fit overhead. Separate resident
fit replays (one warmup, three CUDA-event samples per source/run) have pooled
medians 6.90/7.10 ms for 29/28 descriptors. They exclude descriptor transfers and
actual worker duration. Selected propagation completes 28/29 descriptors in
2–47 scans; the remaining `lead` hits 64 and retains detector geometry. The
neighbour completes 27/28 in 2–43 scans, likewise with one cap fallback.
Incomplete GPU fits intentionally may differ from exact CPU fits. No native
latency, other-GPU performance or speedup claim follows from these observations.

Raw receipts, images, histograms, per-word reasons and the source/executable
bindings are under
`Z:/Projects/OkuFlow/local-work-data/uct-oriented-color-20261021/evidence/`:
`runs/final-color-menu-25000-{1,2,3}-receipt.json` and `final-summary.json`.
Original baseline/intermediate failures remain retained. `agent_check.ps1`
refused a full selection because the fresh focused build lacks a complete
Ninja dependency inventory; no broad build was substituted. The explicit
tracked CPU/CUDA gates above are scoped evidence only.

The later local-surface repair samples inside the word before trusting its
surroundings. Ink and paper use per-channel medians. A consistent immediate
one-pixel perimeter can identify local paper even when all in-box corners
contain ink; the wider-ring fallback requires local-color compatibility and
corner support. The foreground uses its opaque brightness quartile so antialiased
edges cannot wash its average toward paper. CPU and CUDA share integer decisions,
fixed scratch and exact descriptor parity fixtures, including tight button edges,
dense ink, small gradients and tiny/clipped boxes. A matching, independently
proved immutable refinement may improve colors without changing text identity,
geometry, references or validation age.

The all-corners-ink review regression exposed inversion on tight bold E/H boxes.
Ten added cases reproduced it before repair. A compatible consensus on all four
immediately adjacent one-pixel sides now takes priority, so those glyph corners
need not be paper. It also preserves one-pixel button padding before sampling
farther page colors. All 33 real CUDA descriptor cases pass exact CPU parity;
controlled surface cases additionally require their exact source colors. Clipped
or very small boxes can still use the fallback, whose boundary tests establish
safety/parity rather than universally correct polarity.

A bounded 1280 x 2160 scripted page experiment (60 lines, 180 words, one warmup
excluded, three timed samples per version) measured preparation-only median/max
as CPU 8.95/9.70 ms and GPU 3.48/4.27 ms before, CPU 8.90/9.33 ms and GPU
2.98/3.05 ms after the adjacent-perimeter repair. It runs no neural inference,
decoder or presentation. These few observations do not establish a statistical
speedup, dense OCR throughput, native UIA latency or another GPU's performance.
Raw samples are `color-preparation-comparison.json` and the before/final QtTest
logs in `LocalAppData/OkuFlow/uct-paused-continuity-20261021/`.

This is still a solid representative fill. It does not reconstruct arbitrary
gradients or the final colors of spatial Text Clarity/display effects. GPU
analysis precedes those effects; CPU saved-media preparation already grades its
pixels. Correct full effect matching needs an owned per-source appearance/color
input, rather than sampling mutable display storage or grading twice. Renderer
contrast policy remains separate from raw source-color accuracy.

Advanced UCT displays the actual selected OCR backend as GPU or CPU, including
Windows OCR during GPU preparation. The native visible label and cached
accessibility name update together, without unsolicited speech. Off/unavailable
states hide the active-backend label. Both new labels have complete wording
review across 24 translated catalogs; this is model review, not native-speaker
or interactive screen-reader acceptance.

Cached TensorRT plans now load only the inference runtime initially. Parser and
architecture-specific builder-resource DLLs are loaded on a cache miss or invalid
plan. Exact model/cache hashes, runtime version, profile and precision checks
remain unchanged. Recognition enqueues up to eight existing batch-one inference
calls on its private stream before one completion wait, using distinct pinned
metadata slots. This is bounded submission, not a new engine profile or concurrent
access to shared output textures. Cancellation and exceptions retain resources
until completion or quarantine.

During GPU preparation a complete Windows OCR result can bridge readiness.
Configured Paddle CPU sessions are deferred until GPU failure/unavailability,
so loading them and running a dense CPU job cannot delay a newly ready GPU.
Fallback results still pass normal admission and independent-neighbour checks.
A successful provisional fallback can be refined after its neighbour completes;
repeated reads of identical pixels never become independent evidence.

A transient failure previously consumed a paused source's only recognition
attempt. Refused submissions, current-job cancellation, input faults and expired
completions now schedule bounded retries tied to the same geometry/source ticket.
Non-preparation failures permit at most three retries; preparation refinement is
spaced by 500 ms and bounded by the existing 30-second horizon. Real source/seek/
settings changes cancel the old timer's authority. Successful final paused reads
remain one-shot. No partial/unvalidated glyphs are published, and failure limits
continue to keep original pixels visible.

The rebuilt tracked gate passed shipping Release compilation, 108 CPU cases
(two contain optional skipped Qt rows) and all 17 CUDA delta cases, with no
whole-test skips or unresolved results. Exact unchanged per-case passes were
reused by the tracked fingerprint runner. All 24 translation catalogs match 879
source keys. Native GUI checks remain separate and run on GitHub's disposable
desktop; the owner's running app is never terminated or replaced to unlock
outputs. Private media/model caches and raw diagnostic evidence remain under
LocalAppData. Original scoped failures and the resume prerequisite refusal were
retained; the diagnosed controller oracle and backend-status issues were repaired
before the passing integration gate.

Independent Sol integration review identified two refinement gaps. Preferred
preparation timeout now retains a healthy validated CPU fallback. The later
[paused continuity repair](uct_paused_continuity_repair.md) narrows terminal
exhaustion: without previously corroborated glyph proof it still clears glyphs,
reading text and accessible text together. A failed optional refinement of the
identical owned paused input stops its worker attempts while retaining the old
proved artifact. Source/seek/geometry changes and presentation failure still
revoke or hide all affected surfaces; a reading-only candidate is insufficient.
A new selected refinement is staged until its complete neighbour pair is ready,
so it cannot temporarily replace a proven artifact with an uncorroborated one.
Tests hold that new neighbour and check visible/readable/UIA continuity, then
expire preferred preparation and require the healthy fallback to remain.

One cached real M/M source-frame diagnostic succeeded against a private
inference-only SDK directory containing no parser or builder-resource DLLs.
It retained 178 candidate words, 364 ms worker time and 4009 ms total QtTest
elapsed, versus the earlier 354 ms / 3910 ms sample. These single measurements
do not show a raw inference/startup speedup; grouping removes waits but model
execution and cached preparation still dominate this case. CPU fallback remained
usable (179 candidate words, 9290 ms worker time). The separate automatic-chain
diagnostic measures whether early bridge completion avoids that CPU startup wait;
worker completion is not glyph, display or end-to-end camera latency.

The automatic-chain diagnostic exposed a stale backend identity: the Windows
adapter returned its identity only from initialization, so runtime status fell
back to the preparing GPU's earlier label. The adapter now retains and publishes
its CPU identity. Real Windows OCR rows require post-recognition status updates,
and the startup diagnostic requires a usable result with preparation finished
before counting GPU completion. Original false-positive diagnostic output and
the command-line argument failure are retained with the private evidence.

After that repair, the real automatic scheduler returned a complete Windows OCR
candidate result at 902.6 ms (177 candidate words), then a completed TensorRT GPU
result at 4398.6 ms (178 words), without loading the Paddle CPU sessions. This
single cached-input diagnostic used the inference-only SDK folder and correctly
reported CPU then GPU. It establishes early candidate availability and promotion,
not first validated glyph, full-controller, native screen-reader or camera
latency. The selected button's words measured dark foreground on `#eeba65`;
the neighbouring Robots label measured light foreground on `#253a33`.
Raw diagnostics stay in `%LOCALAPPDATA%/OkuFlow/uct-color-startup-20261020/`.

### Integrated small-ink source proof (2026-10-22)

At root `30c6538`, the shipping, CPU/PCH-off and CUDA/PCH-on configurations
compiled with the imported small-ink and original-mask repairs. The actual
FP32 resident selected/real-neighbour book diagnostic passed with a six-times
detail pair. Its source `to` measured `#615b54` on `#e8e3dc`, fitted to
`[642,267,9,7]`; `an` retained its honest detector fallback. Both remained
candidate words. The exported zoom was inspected: `to` and `an` paint separately.
The chosen view changed from 9 to 12 painted words after refinement; recognition
errors and other unpainted words remain. Counts do not establish accuracy.
Detail worker times were 20.9201 and 21.8118 ms, excluding the controller's
settle delay, scheduling and native presentation. Image egress was zero, exact
TensorRT retirement completed and the baseline cache inventory was unchanged.

Receipt: `uct-geometry-r3-20261021/evidence/runs/
integrated-small-ink-book-detail-fp32-1-receipt.json`. This uses the public FP32
diagnostic factory, not the private BF16 selector. A separate production-factory
BF16 full/704-region/full run passed on the same source pair with zero image
egress and unchanged baseline/variant caches; its receipt is
`integrated-small-ink-active-bf16-704-book-1-receipt.json` in that directory.
Neither run establishes native application, camera or screen-reader latency.

The background matrix's UCT CPU and real CUDA kernel checks passed, as did
`shared_source_equivalence`. Its initial packaging/toolchain case failed during
nested Visual Studio initialization with “The input line is too long.” The
same tracked case passed in 6.44 seconds after removing duplicate process-PATH
entries and omitting redundant outer VS initialization. No machine/user PATH
or test assertion changed; the original failed matrix remains failed evidence.
See `logs/combined-uct-bf16-background-matrix-1.log`,
`logs/bundle-toolchain-environment-repaired-gate-1.log` and
`evidence/bundle-path-repair-environment.json` under the same work-data root.
GUI and optional skipped codec cases remain separate acceptance limits.
