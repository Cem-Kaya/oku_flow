# UCT on an initially paused video

An opened saved video is a selected immutable source before Play is pressed.
Its first native sample can legitimately have sample ID and PTS zero. The
controller can recognize that owned sample when its backend becomes ready;
Play, a camera heartbeat and a positive sample ID are not prerequisites.
Selectable reading text and replacement glyphs have different evidence needs.
A single selected result can supply reading text while original glyphs remain.

Replacement glyphs ordinarily require a genuine neighbouring acquisition within
250 ms, different complete source pixels, agreeing recognition and the existing
registration proof. Paused saved files have a duration-backed following-only
exception, described below. A new buffer, a refreshed timestamp or waiting longer
on the same held frame does not supply evidence. Registration, visibility,
exact-text and distinct-source requirements remain unchanged.

## The reported six-second clip

One authorized CPU-only extraction of the owner's private `Tr 9_av1.mp4` on
2026-10-22 found a 900 x 800 AV1 stream containing three frames over six seconds
at 1/2 FPS. The actual first two decoded PTS are 0 and 2000 ms, each with a
two-second duration. Their RGB pixels differ. There is no actual following
sample within 250 ms of the opening frame.

Source SHA-256:
`417e64556e371d5f2fdaa0622555722fb03598f15f89f7a284d46766a415a672`
(77,896 bytes). Packed RGB SHA-256 for the CPU-decoded first two frames:

- 0 ms: `67eaacfe4234e976b261b7691238b94fb59100e196f3db77ce02783d302fe89f`
- 2000 ms: `ec87163bb5e1f1cbb9831dc91bc05087af8258e54c798cb3d4c245810dd9d0bf`

The software extraction used libdav1d, two CPU decoder threads, no hardware
acceleration, no audio, no frame-rate conversion and a two-frame output limit.
PNG conversion is provenance evidence; it is not a Media Foundation decoder
comparison, OCR run or native presentation trace. The clip and extracted images
stay outside Git and release payloads. The path is private, not a default.
The extraction log and `provenance.json` are under
`Z:/Projects/OkuFlow/local-work-data/uct-initial-frame-zero-20261022/`.
The receipt binds the source, PTS, image hashes and inspected base
`e2d433fcd184d669d79ef40096a2a03180b1c28e`.

Static tracing at that base shows `VideoPlayback` arming optional following
preparation after paused Open. `FollowingFrame` then accepted only an issued
sample with a strictly later PTS at most 250 ms away. A 2000-ms sample was queued
for playback but not supplied to paused OCR. That base could not produce
replacement glyphs for this sparse pair. The reviewed policy below addresses
that admission gap; it does not establish a backend-readiness defect. The native
decoder still supplies the real nearest following sample at its original PTS.

## Bounded saved-file cadence policy

Only a paused File document can obtain `ImmutableCorroborationPolicy` from its
own `VideoPlayback`. Issuance verifies the identical native frame object and
shared ownership block, player, file/track, current request and sealed
sample/PTS/duration. Copied, borrowed, foreign and stale frames cannot issue it.
The descriptor retains metadata rather than another decoded image or player.

For a strictly following genuine sample, a positive representable native selected
sample duration sets the window to `max(250 ms, min(duration, 2500 ms))`.
Comparisons use integer 100-ns ticks. Unknown/invalid durations retain 250 ms;
earlier samples always retain 250 ms. This does not infer cadence from elapsed
time, two image timestamps, cache reuse or a job ID. Archive, memory/RAM/recap,
live and advancing-media policy remains ordinary. The two-second Tr 9 sample
can therefore receive its real two-second following sample without Play.

CPU and GPU paused presentation carry the same sealed descriptor through the
controller selection, selected/neighbour stamps, references and worker proof.
Line and word proof share the same guard. Descriptor changes invalidate old
selection and exact-input coverage. Independent source identity and different
complete pixel digests, exact text, NCC >= .94, visibility >= .95, the two-pixel
bound and all size/work limits still apply. Different or unchanged content does
not become proof merely because a sparse time gap is allowed.

## Regression coverage and validation limits

`initialPausedFrameZeroWaitsForReadyAndNeighbour` adds eight scripted rows:
CPU pixels and a resident protocol oracle, each with the neighbour present at
selection, arriving during backend initialization, arriving while selected
recognition is held, or arriving after its result. Every row starts with frame
and PTS zero without Play/live presentation. It requires bounded jobs, selected
provenance, stable generation/authority, no periodic work while held and no
UI-thread pixel access. The resident oracle is CPU-backed, not a CUDA test.

`initialPausedFrameZeroWithoutIndependentNeighbour` adds four rows covering
an absent qualifying neighbour and separately owned byte-identical nearby
pixels. They require reading text, original glyphs, empty positioned screen
text and no repeated idle work. They use ordinary policy. The startup bridge
refinement test also begins at frame/PTS zero.

`LongGopSeeking` checks native opening frame zero and a real following neighbour
without Play or Pause, exact selected lease, request/file identity, ordinary
250-ms bound and decoded quotas. `InitialSparseSavedFileNeighbour` constructs
original three-frame raw AVI bytes with two-second sample durations, then uses
the native CPU reader to check paused frame zero, real nearest following
preparation and issuance rejection. The same bytes opened as RAM must retain
the ordinary limit. Ordinary automated tests do not use the private owner clip.

`savedSparseCadenceKeepsIndependentProof` covers ordinary boundary, saved native
duration, unknown/invalid duration, the hard ceiling, identical source pixels
and changed text/pixels through CPU and resident-oracle paths. Positive rows
require actual software `PaintedWord.drawn`, `ScreenLines` and nonempty Qt
accessibility character bounds. `savedSparseCadenceChangesRevokeProof` revokes
an admitted artifact when its descriptor changes and checks earlier boundaries.

`capturedOutputReachesPaintAndPositionedText` checks initially paused and
Play-to-Pause paths, preferred output and CPU-bridge-to-preferred refinement,
then settled zoom. Ordinary CI uses original synthetic text. An explicit
absolute `OKUFLOW_UCT_CONTROLLER_REPLAY_PROOF` loads private source images with
SHA-256 validation and actual captured selected/neighbour recognition output,
including bridge output when requested. It preserves boxes, quads, colors,
candidate disposition and protection. `OKUFLOW_UCT_CONTROLLER_REPLAY_OUTPUT`
selects a private folder for new per-row paint/accessibility reports.

The replay exercises the production controller, scheduler, model, software
paint and Qt accessibility using a CPU-backed resident protocol oracle. It
performs no OCR inference and is not native UIA-client acceptance. The current
capture lacks neighbouring detail-output serialization, so settled zoom reuses
the captured full-scene recognition pair. It checks controller publication and
paint under a view change, not new detail inference geometry. Pane text and
accepted/model word counts do not establish success. Preserve a real captured
bridge failure rather than weakening overlap or proof to obtain a pass.

## Scoped CPU results on 2026-10-22

The fresh MSVC 2022 CPU/PCH-OFF build compiled exactly
`ultra_clear_text_controller_tests` and `video_playback_tests`, with at most two
compiler jobs. The tracked serial/ExcludeGui gate reported:

- `video_playback_contract`: PASS, 8.23 s, including native initial sparse
  selected/following preparation and same-byte RAM refusal.
- `immutable_uct_contract`: 152 passed, two failed, zero skipped, 92.49 s.
  All frame-zero/readiness, 22 sparse-policy and descriptor-revocation rows
  passed. Both failures were the actual bridge-to-preferred capture rows.
- `agent_check.ps1`: refused with FULL required (exit 2), since the fresh
  cache lacked the built Ninja dependency inventory. Its scope was not widened.

The actual private capture was
`Z:/Projects/OkuFlow/local-work-data/uct-geometry-r3-20261021/evidence/proofs/uct-pause-proof-20261022-015915-253564f5-a277-4d96-a4ae-b33e69be2d48/proof.json`,
SHA-256 `bdd475b10294bb7dfdbee9f174739730e1ed6c39af64f55481496205a1e388d2`.
It uses genuine selected 6000-ms and neighbour 6033-ms images. Both initial and
Play/Pause bridge rows reported `Head required=2 painted=1 ScreenLines=3
positioned=3`; the model/accessibility counts include duplicate text while
actual software paint omits a required label. Full-view bridge replay produced
37 SceneWords/25 drawn; settled zoom produced 37/17. Preferred-only replay had
24/24 full and 24/22 zoom, with both required Head occurrences painted.
These are this CPU controller replay's counts, distinct from the parent native
diagnostic's counts. They do not establish the cause of native OCR coordinates.
No mask, overlap, painter or proof threshold was relaxed to make them pass.

Evidence is retained under
`Z:/Projects/OkuFlow/local-work-data/uct-initial-frame-zero-20261022/validation-1/`:
`first-captured-attempt/` contains the failing Qt report and tracked attempt
logs/ledger; `captured-controller/` contains all four per-row full/zoom reports.
The gate's environment fingerprints the private proof path, but this checkout
does not declare the private proof/image bytes as CTest `REQUIRED_FILES`.
Treat the replay as diagnostic-only with a separate exact-hash receipt.
Acceptance must declare those absolute files at configure and rerun using the
corrected source capture; changing an environment path alone is insufficient.

Four additive CPU/resident changed-text and identical-pixel rows outside the
saved duration window were compiled after that gate but remain unrun. The
ordinary synthetic replay mode is also unrun. Compiler/runner resources were
released for parent coordination; there was no repeat of the failing replay.
No native UI, GPU, model inference, device, audio-endpoint, owner-app or settings
work was performed. App presentation handoff translation units were outside
these two test targets. Full Windows/CUDA and native presentation acceptance
remain parent-owned and pending.
