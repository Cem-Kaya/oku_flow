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

## Parked owner discussion

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
