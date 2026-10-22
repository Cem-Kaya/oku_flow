# Tilted menu repaint diagnosis (development)

Reading text, glyph eligibility and actual painting are separate evidence.
A recognized spelling in the reading pane does not establish independent
pixel proof, and a proved word can still be suppressed to protect adjacent
original ink. None of the observations below establishes general OCR accuracy.

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
0.70 confidence, seven-source-pixel and atomic optional-admission gates remain
intact. The two new fixture rows are **UNRUN** in this child. By explicit
coordination, the parent will compile and run them after import into its warm
CPU build/combined gate; the child performed no configure, build or test run.
