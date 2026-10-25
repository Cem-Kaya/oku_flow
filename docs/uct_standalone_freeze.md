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
match. Recognition confidence and the five-source-pixel minimum remain; invalid
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

The frozen production source `fafc0fce732ef938bef6be92adbd01351e52d3fd`
also passes `scripts/agent_build.bat` with GUI excluded: shipping compilation,
CPU/PCH-OFF 162 passed / 12 optional skips / zero unresolved and CUDA/PCH-ON
delta 14 passed / five optional codec skips / zero unresolved. Four passing
CPU cases include opt-in timing-row skips. The captured controller contract
passes in 92.91 s. Source equivalence passes with PCH OFF in 18.17 s and ON in
13.69 s. The optional actual-app fixture was explicitly rebuilt before this
matrix; its fresh app/recap cases pass in 6.02 s and 0.36 s. Raw attempts,
ledger snapshots and source/binary receipts remain in the work directory above.
These background results do not establish native GUI/NVDA, camera or release
acceptance. No new real-model coverage count is claimed for standalone mode.

The private optional paused diagnostic can select `OKUFLOW_UCT_PAUSE_STANDALONE=1`
to feed only the selected result into the real standalone model/paint path. Its
historical neighbouring OCR remains a separately reported comparison and is
never consumed for this admission; the JSON records that distinction. This is
an offline diagnostic, not a claim that the app submits a neighbour job.

## Preparing fallback to preferred recognition

A complete startup fallback can publish while the preferred recognizer loads.
That provisional full read is not an independent witness against the preferred
engine. On the same source/frame/PTS/digest and scheduler authority, a complete
preferred standalone full read prepares a fresh model and atomically replaces
the fallback only on accepted preparation. It uses its own source references,
spelling, confidence, geometry and protection. This corrects cases such as a
fallback `Will` conflicting with preferred `will`; no word whitelist, case folding
or spelling substitution is involved. Failed/empty reads preserve the fallback.
A crop cannot replace the whole artifact or consume this full-read handoff.
Subsequent same-engine disagreement still keeps the conflicting original ink.
Corroborated and live admission retain their existing policies.

For display colors, clearly colored source paper with RGB channel spread at
least 64 stays unchanged when solid same-polarity ink reaches a 3:1 contrast
floor. Thus white text remains white on the original blue chat bubble without
separate dark word plates. Below that floor, and on neutral paper, the existing
7:1 adjustment remains. This is a source-color choice, not a guarantee about
projected font size or native screen-reader acceptance.

The retained chat-frame diagnostic reproduces a real Windows `Will` to Paddle
`will` handoff refusal before this repair. Preferred-only admission already drew
the word; the startup refinement removed it. After repair the same production
factory/selected pixels produce 293 scene words and 277 actual paints, including
`will`. It remains drawn before and after same-image zoom detail (8 of 9 words
painted in that focus; the tiny adjacent `i` remains original). Actual blue paper
is retained with white replacement ink. This is an offline resident/production
paint replay, not decoder, native display or native screen-reader acceptance.
Its neighbour job is explicitly comparison-only under standalone admission.

Current focused CPU/PCH-OFF contracts pass 349 model/paint Qt rows and 166
controller Qt rows, zero failures/skips, in 132.97 seconds. They cover preferred
handoff painting and positioned character bounds, failed/empty/stale input,
crop authority, subsequent contradictions, colored paper and below-floor/neutral
controls. Initial fixture failures are retained: hidden-overlay export, a
rejection-code expectation, and one-channel QPicture antialiasing differences
on the existing floating-color fallback. Source-paper colors and the blue live/
export images are compared exactly; only that old fallback's edge rounding has
a one-level bound. Private inputs, first attempts and hashes remain under
`Z:/Projects/OkuFlow/local-work-data/uct-chat-will-20261022`.

The shipping application compiles successfully. CUDA delta: 14 passed, five
optional codec skips, zero unresolved. The broader CPU invocation remains
blocked by the previously retained `phone_camera_contract` failure: its shared
PATH/toolkit environment differs, so Resume refuses an unchanged retry. No
phone repair or full-matrix PASS is claimed. After explicitly rebuilding the
optional actual-app fixture, both offscreen app cases and source equivalence
pass (3/240, 21.45 s); source equivalence also passes PCH ON (1/246, 11.94 s).
The real chat replay retains exact TensorRT retirement and cache inventories;
its two image-egress calls are the diagnostic's explicit Windows bridge reads
(selected fallback and comparison-only neighbour). GUI/NVDA, camera/presenter
and release acceptance remain unrun.
