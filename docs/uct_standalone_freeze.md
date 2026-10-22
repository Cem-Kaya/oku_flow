# Standalone paused video and Freeze

The owner's 2026-10-22 policy is that a paused video or manually frozen image
must be usable on its own. The application therefore selects
`ImmutableAdmission::Standalone` on both CPU and GPU presentation paths. Opening
frame zero, pausing playback and freezing the camera do not require an earlier
or following capture, different pixels in another frame, repeated OCR agreement
or cross-frame registration. Waiting for decoder preroll cannot authorize or
veto this frame's words.

The recognition worker reads the exact retained image. Eligible words can become
sharp replacements and positioned screen-reader text on that result. A settled
zoom can run the existing bounded region detector on the same image, at up to
8x under the 960-pixel detector bound. It does not manufacture an independent
acquisition or a neighbour proof. The controller does not submit a neighbour
recognition or registration job, and app presentation does not prepare another
UCT analysis image for this purpose. Playback may still decode ahead for its
own playback/seek responsibilities.

Standalone does not mean unconditional painting. The selected image, result
ticket, source generation, frame identity, real PTS and processed geometry must
match. Recognition confidence and the seven-source-pixel minimum remain; invalid
geometry, unsupported notation and non-reproducible/composite input remain
refused. Smoothed recognition still needs correspondence to its own current
single-frame pixels. Source protection, quad projection, clipping and actual
painter closure remain in force. Low-confidence or protected words can still
remain as original pixels. A valid OCR score is not certainty about spelling;
there is no dictionary or language-model substitution.

The live camera and advancing video keep their existing temporal validation.
`ImmutableAdmission::Corroborated` is retained as an explicit internal protocol
and for existing independent-frame tests/diagnostics. Its 250-ms ordinary window
and sealed saved-file duration exception are no longer paused-app prerequisites.
Changing between modes invalidates the selected work and prevents eligibility
from one mode being reused as evidence for the other. View changes or repeated
receipts do not continually reread the same full image.

The earlier six-word Paddle result and geometry repair remain valid evidence
for the former two-frame policy. They do not measure this new standalone mode.
See [tilted-frame evidence](uct_tilted_paint_gaps.md) for the original refusal and
[initial-frame history](uct_initial_paused_frame.md) for the earlier sparse-video
work. Tests for the new policy cover absent/conflicting neighbours, CPU/resident
inputs, real software paint and Qt character bounds, no extra idle jobs, mode
fencing, source/PTS and confidence refusal, and same-image visible detail.

## Focused validation

Qt 6.12 / CPU / PCH OFF: the model contract passes 337 Qt checks without skips;
the changed controller contract passes 164 without skips in 93.30 seconds.
Initial zoom-fixture identity assertion failures and an existing scheduler
fixture's queued-callback race remain archived under
`Z:/Projects/OkuFlow/local-work-data/uct-standalone-freeze-20261022/`.
The CPU fixture now checks pixel identity through the CPU API and frame identity
through the resident API; its synthetic clock advances only after UI admission.
No deadline, confidence or coverage assertion was loosened. These are scoped
offscreen checks, not native NVDA, camera or release acceptance.
