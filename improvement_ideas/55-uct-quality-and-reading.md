# OCR quality and reading follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24.

Medium BF16 preference, all selectable model tiers, FP32 fallback, batching,
merge/helper improvements, oriented/perspective painting, paused detail and
independent probing are implemented. Old screenshots/engine reports that
predate those repairs are not current bug reports.

## Remaining research

- **Character-sensitive change detection:** review/calibrate the new offline CPU
  current-ink oracle and design bounded native GPU leases before shadow/live use.
  Preserve the measured [NCC baseline](../docs/uct_character_verification.md).
  Skipping authoritative full reads remains unaccepted, independently of ordinary
  ML recognition errors.
- **Curved pages:** review the explicit selected-frame ribbon experiment and
  measure its actual quality/cost. Four-corner painting remains approximate;
  per-glyph/page-mesh alternatives and learned dewarping are unimplemented.
- **Detection/recognition quality:** compare contour/word-level geometry or
  alternative engines on the same difficult images if misses remain. Verify
  scripts/dictionaries and retain omissions, boxes and wrong words. No
  replacement engine or broader alphabet has been selected.
- **Reading tools:** Reading Anchor, explicit line/reading-lane navigation,
  source-slide fidelity/board-state recovery, table-aware reading and
  user correction remain product proposals.
- **End-to-end experience:** measure first painted text and Freeze latency,
  not just worker microtime. See [03](03-performance-gpu-cpu.md).

## Implemented first experiments, pending review/measurement (2026-10-24)

The model-free [ribbon/current-ink experiment](../docs/uct_ribbon_and_current_ink.md)
is implemented in the production Paddle processing chain. It adds no model,
automatic live/Freeze tier switch or new UI strings. The initial execution scope
is an explicit `OKUFLOW_UCT_CURVED_RIBBON=1` opt-in on the controller's exact
selected immutable single frame. Live/advancing/smoothed/held inputs stay on the
baseline. CPU correctness is separate from the unrun GPU/latency acceptance.

**Character changes.** A bounded owned CPU oracle compares a word's original
analysis-resolution ink, with global symmetric jitter/photometric handling and
the worst small-region residual. It returns changed, unchanged or unverifiable.
It is offline only: no runtime hiding, reuse or read deferral is wired, and full
reads remain authoritative. The old six NCC summary rows remain intact; a new
CPU row covers the original 12 edits, with a separate 48-case held-out
font/subpixel/blur/scale/exposure diagnostic. Report false accepts, false hides
and unverifiable, not a perfect-accuracy pass. Thresholds remain provisional.
Native GPU current-ink capture/reference leases and shadow telemetry remain work;
changing the scorer cannot recover pixels already lost in the decimated probe.

**Curved pages.** CPU and resident CUDA source now retain a compact quadratic
centerline/height ribbon from detector components. Recognition windows and CTC
word spans use the same backward source map. CPU analytic tests cover actual
production fitting/sampling, straight-path byte equality, component bounds,
refusals and rotation/crop/keystone-to-present composition. The CUDA source is
not yet compiled/executed in this implementation phase.

Word quads remain approximations to curved ribbons. Both word and widened search
span chord error must remain <= min(1 original analysis pixel, 8% word height).
Unsupported word geometry retains non-carvable source protection and omissions,
not reading-pane-only guessed words. Identified but unsupported curve fits also
retain ink. Median centerline row geometry, original-pixel proof, paint and Qt
positioned text share the production model path. Detector and CTC inaccuracies,
gutter foreshortening and hidden print remain ordinary limitations; this is not
an exact curved-glyph renderer or a whole-page learned dewarper.

ROOT will commission Opus review/optimization in the frozen implementation
checkout, then serial latency and quality measurements using the deterministic
A/B entries and source/pixel hashes. No performance measurements, inference,
model downloads or native desktop/phone validation occurred during implementation.
Measure actual whole-worker/callback and first painted Freeze latency separately
from primitive stage microtime. Review omissions, word errors, source geometry,
plates and false hides before promoting any scope/default.

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
