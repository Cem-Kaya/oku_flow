# Tilted menu repaint diagnosis (development)

Reading text, glyph eligibility and actual painting are separate evidence.
A recognized spelling in the reading pane does not establish independent
pixel proof, and a proved word can still be suppressed to protect adjacent
original ink. None of the observations below establishes general OCR accuracy.

## Reported application path and evidence scope

The owner's primary report is functional: after opening/pausing the video,
some button words remain raw **and are unavailable to the positioned screen
reader**, even though Recognized Text contains their strings. This was a
functional application-path failure, not merely a font or plate appearance
issue. The later bridge capture reproduced missing paint and duplicate
positioned words, and the angle repair and controller replay below address
that captured path. The exact prior owner session and native NVDA behavior
remain unverified. The standalone direct-BF16 source replay below does not
exercise the
complete playing-to-paused transition, initial Windows OCR bridge, later GPU
refresh, actual paint generation and positioned accessibility publication.
Its all-drawn result does not close that report.

The existing code has a reachable distinction worth checking in the controller
audit: `UpdateReadingText` publishes `OverlayModel::ReadingText`, while
`RefreshOverlay` takes `SceneWords` and `ScreenTextSnapshot` takes `ScreenLines`.
Reading-only immutable words may appear in ReadingText; SceneWords excludes
them and ScreenLines turns their ineligible word slots into omissions.
Therefore pane text alone does not prove video/positioned-reader eligibility.
This identifies a diagnostic branch, not the confirmed cause in the owner's
running application.

Conversely, conservative painter suppression alone does not establish the
missing positioned-reader symptom. `UctScreenLineAccessible::LocalGeometry`
accepts an eligible word's last-painted record even when `drawn` is false and
uses its original-source character boxes with `VisiblePart(..., false)`.
Unavailable/current paint geometry can hide positioned bounds independently.
The captured menu19 mask baseline therefore explains that retained word's
visual suppression only. Controller/eligibility/actual-paint/UIA lifecycle
validation remains the separately owned investigation; this contribution
changes none of those paths.

## Retained source and paint evidence

The original 1920×1080 menu19/menu25 sources and r5 proof reports remain under
`Z:/Projects/OkuFlow/local-work-data/uct-geometry-r3-20261021/evidence/`.
These are earlier source frames, not the owner's newly reported 6/38-second
AV1 pauses. The source hashes are respectively
`b113b6e1e78b9137c41651c529d614fdf5251af27bf35e29196b26fec5eafe97`
and `c1ab2b1243e21440129b9dea9f61f61bacfe8d6698c11a49e3e8748efc33bbc0`.

| Source/word | Model decision | Recorded paint | Explanation |
| --- | --- | --- | --- |
| Menu19, upper Left / Hand | Both independently proved | Both drawn | The original protection does not intersect either quad. |
| Menu19, upper Right | No independent word proof | No eligible glyph | Reading text alone cannot authorize repainting. |
| Menu19, upper trailing Hand | Independent word proof | Undrawn | Its quad enters the preceding Right's conservative original detector rectangle. |
| Menu19, lower Head / Left / Hand | Independently proved | Drawn | These are distinct from the lower Right / Hand pair. |
| Menu19, lower Right / trailing Hand | Right unproved; Hand proved | Both undrawn | The same protection mechanism applies to the trailing Hand. |
| Menu19, 34% | Independently proved | Undrawn | Nearby unproved Size has conservative rectangular protection overlapping its word quad. |
| Menu25, lower Head | OCR actually returned `lead`; ineligible | Undrawn | Recognition/proof gap; no spelling substitution is justified. |
| Menu25, lower Left / Hand / Right / Hand | Eligible | All drawn | The last Hand's fitted source height is 43.007353 pixels. |
| Menu25, magnifier Q | One Q eligible; overlapping Q unproved | Eligible Q undrawn | Duplicate/icon uncertainty retains original source pixels. |

For the upper menu19 pair, Right's original detector rectangle ends at
`(1170.536625303071, 430.430899258318)` while the following Hand's bottom-left
corner is `(1165.290336296474, 430.660327393649)`. The small intersection is
real in the supplied rectangular protection. Shrinking that rectangle or
checking only the replacement glyph's outline would not prove that all
original Hand ink clears it. The whole-word closure therefore remains intact.

`tests/uct_tilted_paint_test_cases.hpp` replays the exact captured upper menu19
and lower menu25 button geometry, original detector protection and eligibility
at the model-to-painter boundary. It checks snapshot and live software-paint protection pixels,
the captured per-word drawing decisions, the 43-pixel fit, and a separate
diagnostic control showing that the menu19 Hand is drawable without its
neighbour's mask. Production never removes that mask. It embeds factual
metadata only: no footage, reference patches, model or manufactured proof.
The fixtures are not a replay of inference/admission and do not assert quality
completeness. Their original reports are:

- `proofs/uct-pause-proof-20261021-201612-78b59283-0390-4be1-aedd-afc5c319721a/proof.json`,
  SHA-256 `3425b04bc245697e5e1a63de9e94367423a5aaadf6b11742796c55c82f6b634e`.
- `proofs/uct-pause-proof-20261021-201652-95bd5469-57a4-4974-be97-c148f28cd224/proof.json`,
  SHA-256 `2c7935a77e38e52b9f9579518dad6430bf8d86be267ca2d806ffbf6ddf25db52`.

## Newly reported AV1 source

The exact file is `Z:/Vids/Dragable_vid_av1.mp4` (one g), 6,031,955 bytes,
SHA-256 `9b09203c3638bcbb3cf023568128ebfb538a9d8e273617e28e047a17b0486671`.
One authorized CPU decode with `-hwaccel none -threads 2` retained two actual
1280×720 frames at PTS 92160 and 92672 in time base 1/15360: 6000 and
6033.333333 milliseconds. Neither frame is a copy counted as new evidence.

The private extraction manifest, original log and PNGs are under
`Z:/Projects/OkuFlow/local-work-data/uct-tilted-repaint-r24-20261022/`.
The selected/next PNG hashes are
`5fed5b0bdccbf02d7132e00fcaadbba868f768faff0f0297464f11819d6ec4d5`
and `61aa7f5bffa236e1974c2834923dded7d789271362d72e3eb8b17a164e98baa8`
(also recorded in the manifest).
Both Head labels and the Left Hand / Right Hand rows are visibly present;
the lower Right is crossed by a scene pointer. Visual legibility is distinct
from the recognizer's output, pixel proof and renderer eligibility.

Extraction is preparation only; the parent separately ran one controlled
production-factory BF16 paired proof on these two frames. Its report is
`evidence/proofs/uct-pause-proof-20261022-013938-f683273b-8478-4fc0-8676-b858b38224d7/proof.json`
under the earlier geometry evidence root, SHA-256
`795ccc752283152bc2e28bf00fe32ce71ea73aeabf5b72911af0a11ce33498fa`.
The child inspected that report and its original `overlay.png` and
`zoom-after.png`; it did not run inference.

The full result has 27 candidates, 26 model scene words and 26 actual painted
words. Both Head labels, both Left / Hand pairs, the upper Right / Hand,
Zoom / Level, 42%, both Display / Location words, Display / Size, 34% and both
100 values paint. X, both zero labels and the magnifier-shaped Q stay original.
The focus `[.1,.12,.7,.70]` contains 23 words and all 23 paint before and after
optional detail. That detail examined 20 candidates in source rectangle
`[128,86,896,505]`. It does not reproduce the reported partial button repaint.

The lower pointer-occluded Right is a separate recognition error: both real
frames produce the literal `F.ight`, which the line proof admits and the
painter draws. Agreement and a high OCR score do not establish correct
spelling. It would be unjustified to silently replace it with Right. This
case must remain an explicit quality limitation, not a passed accuracy check.
The images also show minor fit/plate artifacts, including residual teal near
42%; complete word counts do not establish complete original-ink coverage.

The parent reports zero image egress, unchanged cached plans and exact input
retirement for this controlled run. These are ownership/technical checks,
separate from the observed recognition error. An exact 6.000-second direct
BF16 replay cannot identify the state of a Windows OCR bridge or a different
PTS while the UI displays the same rounded second. The 38-second report,
native UI and original reported runtime state remain unvalidated here.

No production geometry, protection, font-size or overlap threshold is changed
by this evidence-only contribution. Source/current-pixel, distinct-neighbour,
0.70 confidence, then-current seven-source-pixel and atomic optional-admission gates remain
intact. The two new fixture rows are **UNRUN** in this child. By explicit
coordination, the parent will compile and run them after import into its warm
CPU build/combined gate; the child performed no configure, build or test run.

## Actual Windows bridge coordinate defect

A later parent run retained both real Windows bridge outputs before the same
preferred BF16 pair. Its private report is
`uct-pause-proof-20261022-015915-253564f5-a277-4d96-a4ae-b33e69be2d48/proof.json`
under the geometry evidence root. It has 38 model scene words, including
upright/tilted duplicate fragments, against 26 in the direct run. In the focus,
19 of 33 paint before detail and 18 of 34 afterwards. Head is visibly missing;
several retained bridge plates and glyphs are displaced from the source ink.
This is a reproduced failure, not a quality pass because the diagnostic exits
normally. It does not by itself establish native screen-reader behavior.

Source inspection found that the Windows wrapper discarded `OcrResult.TextAngle`
and the adapter treated every `BoundingRect` as original-image coordinates.
Microsoft documents that these boxes use the rotated coordinate frame and
must be rotated clockwise about the image centre to overlay the original.
See [TextAngle](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrresult.textangle)
and [BoundingRect](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrword.boundingrect).
The bridge's Head at `[130,243,97,31]` and Zoom at `[62,320,83,23]` are displaced
from preferred source bounds around `[137.79,223.31,95.18,33.87]` and
`[64.34,296.12,83.11,27.27]`; the direction changes across the image, consistent
with the missing global rotation. The repaired same-source run below retains
the actual native angle and outputs.

The repair retains that optional native angle, rotates all four corners in
the actual sampled image, then maps them back with the actual X/Y resize
ratios. Native null/zero angle keeps the upright path. Nonfinite metadata
fails closed; no guessed angle, spelling substitution, protection shrink,
confidence reduction or proof relaxation is introduced. The Windows adapter
now supplies the existing oriented-geometry pipeline rather than upright
boxes from a different coordinate frame. Pure mapping and real printed-page
fixtures are added.

The first repaired CPU Windows OCR gate passes all 11 Qt rows, no
skips, in 0.80 seconds, including real +/-3 and +/-6 degree printed pages.
That target's compile commands use no precompiled header; the surrounding
cache had `OKUFLOW_USE_PCH=ON`. Whole-configuration PCH-off acceptance is separate.
The current CUDA/PCH-on Paddle kernel gate also passes in 8.68 seconds,
including the 8x normalization path. Both are scoped results; the automatic
`agent_check` refuses the incomplete full Ninja dependency inventory rather
than claiming scoped coverage.

The repaired real Windows-bridge -> BF16 proof is
`uct-pause-proof-20261022-133746-cc11f6c0-7554-4a3a-94cf-d5731222fa1c/proof.json`,
SHA-256 `5de577bc630233dfa85fb5bd488dfb3a193a3dd6d8a9dbdd30a631ec1442ce62`.
The native bridge angle is +2.4 degrees. Both Head labels paint, each appears
once, and all 21 in-focus scene words paint before and after detail. The
previous 38-word duplicate union becomes 23 scene words. This lower count
reflects deduplication and retained literal disagreements, not an accuracy
score or a comparison against the direct run's 26 words. The pointer-occluded
Right/F.ight conflict remains original; it is not corrected by spelling rules.
The exported zoom image has been inspected. Minor background/plate artifacts
remain, and this source pair does not close the atlas's reported blurred-14
state or native NVDA behavior.

This replay uses the actual production BF16 factory and cached plans. It
reports exact input retirement, unchanged cache inventories, two intentional
bridge image readbacks and no additional GPU image egress. The raw build,
gate/Qt logs and receipts are under the private
`uct-windows-angle-20261022` work directory. Corrected controller/UIA replay,
combined shipping/PCH/CPU/CUDA checks and native acceptance remain separate.

## Combined background validation of the repaired source

Frozen production source `40a112d1becc6f3e7062e3d2d41f30209cd8ede4`
passed the tracked `scripts/agent_build.bat` background matrix with Qt 6.12,
VS 2022 and the private CUDA 12.9.86 toolchain. Shipping compilation passed;
the CPU/PCH-OFF gate reported 162 passed, 12 optional skipped cases and zero
unresolved results. Four passing cases contain opt-in Qt timing skips;
the explicit timing selectors also passed. The CUDA/PCH-ON delta reported
14 passed, five optional native-codec skips and zero unresolved results.
Desktop fixtures were deliberately excluded, so this is partial validation,
not the complete release gate.

The root model contract passed in 36.96 s, native Windows OCR in 0.77 s and
the hash-bound captured controller contract in 92.83 s. All required button
paint and positioned-character assertions passed. Actual-app offscreen
contracts passed in 5.96 s and 0.35 s, including recap Freeze/Resume and
collapsed viewport Space. `shared_source_equivalence` passed with PCH OFF
(15.81 s) and in an additional explicit PCH-ON CUDA selection (12.06 s).
The first matrix's missing optional native-fixture executable refusal is
retained; the target was explicitly built before the completed matrix.

Logs, immutable per-attempt Qt/JUnit archives, ledgers, source tree and binary
hash receipt are under
`Z:/Projects/OkuFlow/local-work-data/uct-combined-40a112d-20261022/`.
No native GUI, NVDA, physical-camera/presenter-clone, audible-device or
five-minute soak acceptance follows from these background checks.

The separate saved atlas crop also verifies actual sharp, solid-white `14`
alongside its neighbouring words (4/4 focus words painted). Its retained
`zoom-after.png` is in
`uct-pause-proof-20261022-013629-770580a3-c7e6-4daa-8ebc-ee9b3e1e70dc/`
under the same geometry evidence root. Faint source compression remains outside
the replacement plates. This verifies that saved crop, not every earlier live
publication state. See the [offline pipeline guide](uct_pipeline_guide.html)
for the distinction between source pixels, accepted words and actual paint.

## Five-second Paddle-only refusal (2026-10-22)

The owner subsequently requested [standalone paused/Freeze](uct_standalone_freeze.md).
The counts and two-frame checks in this section document the former policy;
they are not measurements or prerequisites of the new standalone path.

The later owner screenshot matches the actual 5200-ms frame of the same
`Dragable_vid_av1.mp4`. A genuine 5233.333-ms following frame reproduces the
refusal using the production BF16 Paddle factory alone: 13 eligible recognition
candidates, one model glyph and one actually painted word (`Persp`), zero image
egress. Every selected candidate had valid preparation/reference geometry;
the withheld labels failed the independent line/word pixel checks. Windows
OCR and painter-mask suppression are not required for this reproduction.

Two general geometry problems were repaired. A sub-two-degree detector quad
could span several source pixels of slope but enter the upright fitter; its
neighbour could cross the angle cutoff and enter the oriented fitter. Keeping
quads with more than half a source pixel of transverse edge excursion raises
this exact pair to five painted words, including all four upper Left/Hand and
Right/Hand words. Separating the original neighbour-bounded recognition frame
from fitted display geometry supplies the additional strict proof for Toggle:
six model glyphs and six actual paints with Paddle alone. No word or filename
special case, model/score change, lower NCC, mask carve or display-box enlargement
is used. A forced real Windows-bridge-to-Paddle run also paints Toggle and all
four upper button words; the literal `persp`/`Persp` disagreement remains original.

The same repaired Paddle path paints 17/23 recognized candidates at 5500 ms and
26/26 at 5900 ms. These are actual coverage counts, not accuracy scores; the
latter includes the literal magnifier-icon misrecognition `Q`. At 5200 ms the
cropped `INGS`/`ETTINGS`, lower `Right`/`Righ`, unregistered lower spans and the
pointer-adjacent number are not authorized merely by high recognition scores.
One lower-Hand detail attempt still supplied no new proof; that refusal is
retained, not described as recovered text or retried until it passes.

The private CPU extraction, source hashes, original one-word result, two
incremental geometry results, bridge/detail and other-position reports are under
`Z:/Projects/OkuFlow/local-work-data/uct-tilted-five-second-20261022/`.
Original source SHA-256 remains
`9b09203c3638bcbb3cf023568128ebfb538a9d8e273617e28e047a17b0486671`.
The 5200/5233 source PNG hashes are
`09d149fd2001ed27b4472892e868a390dc0919407ad0b4ecf1625f42b31d5d76`
and `004fb0866b62fdbf86eccbd533cc02df0b3a69604afc48264d0509db8f164641`.

The first focused CPU/PCH-off contract passes 336 Qt rows with no skips in
40.11 seconds. Its added matrix covers five positive/negative tilts, perspective,
fit jitter, absent original frames, changed ink, oversized/crossed frames and
source fencing through actual worker pixel checks. Positive cases additionally
require production software paint, ScreenLines and nonempty Qt character bounds.
The real CUDA/PCH-on contract passes 52 Qt rows with no skips in 11.18 seconds,
including the equivalent resident worker matrix. These scoped checks and saved
renders do not establish native NVDA or the exact earlier application session.

The repaired production source `8fc3a9f09949e954139d24d215d35e9e171f9051`
also passes the tracked background matrix: shipping compilation, CPU/PCH-OFF
(162 passed, 12 optional skipped cases, zero unresolved) and CUDA/PCH-ON delta
(14 passed, five optional codec skips, zero unresolved). Four passing CPU cases
contain opt-in timing rows. The captured controller contract passes in 93.38 s;
source equivalence passes with PCH OFF in 18.02 s and PCH ON in 14.79 s.
The optional `native_app_control_fixture` target is excluded from the default
build, so it was explicitly rebuilt against this source after the matrix;
fresh exact app/recap cases then pass in 5.99 s and 0.38 s. The earlier matrix
results for that older fixture binary are not used as current-source evidence.
Per-attempt logs, ledger snapshots and binary/source hashes remain under the
five-second work directory. GUI/NVDA, actual camera and release acceptance
remain pending; this is background validation.
