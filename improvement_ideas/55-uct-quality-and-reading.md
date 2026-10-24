# OCR quality and reading follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24.

Medium BF16 preference, all selectable model tiers, FP32 fallback, batching,
merge/helper improvements, oriented/perspective painting, paused detail and
independent probing are implemented. Old screenshots/engine reports that
predate those repairs are not current bug reports.

## Remaining research

- **Character-sensitive change detection:** the current pixel/NCC verifier can
  miss single-character edits. Preserve the measured
  [baseline](../docs/uct_character_verification.md); investigate a better
  current-ink verifier before using tracking to skip authoritative full reads.
  This is separate from accepting ordinary ML recognition errors.
- **Curved pages:** investigate dewarping as a recognition aid with a verified
  inverse mapping back to the original page. Rotation/perspective quads do not
  make glyphs follow a curved line. No dewarping model is integrated.
- **Detection/recognition quality:** compare contour/word-level geometry or
  alternative engines on the same difficult images if misses remain. Verify
  scripts/dictionaries and retain omissions, boxes and wrong words. No
  replacement engine or broader alphabet has been selected.
- **Reading tools:** Reading Anchor, explicit line/reading-lane navigation,
  source-slide fidelity/board-state recovery, table-aware reading and
  user correction remain product proposals.
- **End-to-end experience:** measure first painted text and Freeze latency,
  not just worker microtime. See [03](03-performance-gpu-cpu.md).

## Proposed first experiments (root and Opus, 2026-10-24)

These are designs, not implemented behavior or measured gains. Prioritize book
gutters and single-character changes before adding another OCR model.

**Character changes.** The current whole-line correlation averages a reference
into at most 64×12 cells. One changed digit or stroke can be diluted by the
unchanged line. Investigate per-word current-ink comparisons that score the
worst small region, allow bounded alignment error and distinguish changed,
unchanged and unverifiable. Keep the original 12-edit baseline, then add held-out
fonts, blur, exposure/scale changes, subpixel motion and real unchanged phone
pairs. Measure false hides and cost as well as missed edits. Native-resolution
patches need a separately bounded capture/lease design: the existing decimated
probe cannot recover lost pixels merely by changing its scoring function.

Start offline, then shadow-only. A later conservative mode could hide a suspect
line and request a fresh read. It would not validate new text, relax recognition
rules or defer full reads. Thresholds and jitter tolerance remain experimental;
one-pixel tolerance can itself conceal a one-pixel character edit. No runtime
change is authorized by this design note.

**Curved pages.** First retain a compact centreline and height profile from each
detector component, and test sampling a straight recognition strip along that
curve. Each sample retains its coordinate on the original image. Map recognized
word spans back to source geometry. This resembles the curved alignment used in
[ABCNet](https://arxiv.org/abs/2002.10200), without adopting that model or code.
Keep the existing straight-line path as the control.

A word quad is still only an approximation to a curved ribbon. Measure that
approximation in original pixels and leave severely curved words as original
ink; do not call four mapped corners an exact curved-glyph reconstruction.
Reading order, ink fitting, protection, painting and positioned accessibility
must agree on the geometry. Flattening for recognition alone does not solve the
overlay. Strong gutter foreshortening or hidden print can remain unreadable.

Use a synthetic curved-page fixture with a known coordinate map before real
book photographs. Compare omissions, word errors, geometry/plate errors, added
recognition windows and whole-worker time. Consider immutable Freeze inputs
first to bound the experiment; this changes no model by mode. A whole-page mesh
or a learned dewarper such as [UVDoc](https://arxiv.org/abs/2302.02887) is a later
alternative with additional model, licence, memory and mapping costs.

Recommended order: character-change diagnostic, curved-line geometry fixture,
then owner review of actual error/flicker/latency results before app integration.

## Parked live/Freeze model discussion

**Fast recognition live / a slower accurate model on Freeze** was discussed,
and the owner explicitly said not to implement it yet. A dual-model Freeze
verifier/agreement display is also only a proposal. Current saved tier choices
apply without an automatic live/Freeze switch.

If revisited, preserve one worker/authority, model identity, selected-frame
ownership and honest Resume behavior. Model agreement is not ground truth.
Do not silently discard current text or add cold model loading on Freeze.

## Precision policy

The owner accepts normal ML errors and some accuracy loss. Do not add perfect
human-text, numerical equality, model-agreement or golden-corpus gates to normal
BF16 startup. Quality experiments inform the owner's choice; they do not revoke
the shipped BF16 preference.

[Worker/model pilots](../docs/uct_worker_pilots_2026_10_23.md) retain the measured
tradeoffs. Tiny has a smaller dictionary; speed alone does not establish the
same language coverage.
