# Paused UCT continuity repair

The reported development-player case is the owner's local
`2026-10-01_17-34-58.mp4`, a 1920 x 1080, 60 FPS HEVC recording. The screenshots
show paused selections near 55 and 89 seconds after random seeks. The private
source and extracted frames remain outside Git; they are not release assets.

## Reproduced defects

A complete preferred-backend refinement could contain fewer independently
corroborated words than an earlier Windows OCR bridge result. Its verdict was
still `kAccepted`, so the immutable model replaced the complete earlier
artifact with the subset. The controller's existing rollback protected failed
refinements, but did not protect successful partial refinements. The new
controller regression reproduces four proved words becoming one, both when
the preferred selected result omits words and when its neighbour omits them.
Both rows failed against the unchanged production implementation before repair;
54 other controller rows passed. The failed tracked evidence is retained.

At 55 seconds the selected chat page immediately follows a tab/scene cut. The
old Pause path retained the preceding game image as its only corroborating
frame, then blocked later neighbour updates while holding the displayed frame.
Waiting could not make that different scene prove the newly selected text.
`VideoPlayback::PauseAtFrame` now freezes the exact issued displayed lease and
obtains a genuine following frame through the existing bounded decoder worker.
Pause does not seek, play audio or advance the held image/PTS. The app accepts
later neighbour-only receipts only while they name that identical held frame.
An opaque issuance lease rejects copied records, foreign players, borrowed
ownership blocks and previous seek generations. EOF without another frame
remains honestly uncorroborated.

An optional neighbouring frame could also arrive after the exact selected
frame's first OCR submission. Neighbour equality was part of the selection's
identity, so its arrival invalidated and restarted the unchanged selected frame.
The controller now attaches the first usable distinct neighbour to the retained
selection. It preserves the selected job, source authority, geometry generation,
reading text and deadline. Later omitted/replaced optional descriptors cannot
create another selected acquisition or an unbounded stream of confirmations.

## Exact-source refinement

Immutable admission retains already proved coverage only when the complete
selected-source ticket and full source digest match. This includes source and
geometry generations, frame/PTS, image extent/transform, scheduler authority,
digest kind, smoothing history and held-acquisition provenance. A seek, source
replacement or different pixels cannot inherit this evidence.

Old proved words retain their own identities, positions, reference and validation
clocks. Independently corroborated additions can extend the same selected
artifact; omissions do not erase earlier exact-source proof. Explicit overlap
contradictions retain original ink instead of restoring contradictory cached
glyphs. Reading text and positioned accessibility follow the resulting model.
The union is bounded; the implementation and exact limits are documented in
the code reference. No motion/recognition confidence threshold is lowered.

Live and advancing-video admission remains separate. Repaint, a new job number
and repeated copies of a held frame still cannot manufacture independent
recognition evidence. Actual presentation failure still suppresses glyphs,
reading text and positioned accessibility until a truthful successful receipt.

## Private-source diagnostic

`uct_paddle_tests optionalPausedVideoProof` is an explicit, unregistered
CUDA/TensorRT diagnostic. It uploads the two privately extracted images once,
submits owned resident analysis leases to the production scheduler, runs actual
selected/neighbour recognition and worker registration, and calls production
immutable admission. It records candidates, dispositions, line/word proof,
glyph eligibility, reading text, source hashes and image-egress counts.

Select it with `OKUFLOW_UCT_PAUSE_DIAGNOSTIC=1`,
`OKUFLOW_UCT_PAUSE_SELECTED`, `OKUFLOW_UCT_PAUSE_NEIGHBOUR`, and the corresponding
`OKUFLOW_UCT_PAUSE_SELECTED_MS` / `OKUFLOW_UCT_PAUSE_NEIGHBOUR_MS` source PTS.
The sequence mapping is specifically for this 60 FPS source, not an arbitrary
video decoder. It requires existing pinned medium/medium N1/N8 plans and an
explicit inference-only `OKUFLOW_UCT_TENSORRT_ROOT`, refusing parser/builder
resources. It downloads nothing and verifies the unchanged cache inventory.
Evidence uses unique `LocalAppData/OkuFlow/uct-pause-proof-*` directories.

This diagnostic measures recognition and admission only. It does not validate
decoder frame selection, native rendering, screen-reader behavior or latency.
Direct GPU candidate checks found 266 words at 55 seconds and 249 at 89 seconds;
candidate availability does not prove glyph eligibility or OCR accuracy.
The resident diagnostic then measured the same 55.000-second selected image
with two actual neighbouring images: the preceding 54.950-second game frame
allowed 34 glyphs; the following 55.050-second chat frame allowed 224. At
89.000 seconds, the 88.950-second neighbour allowed 219 of 249 raw candidates.
These are proved overlay-word counts, not an annotated OCR accuracy score.
All three resident runs exited normally, reported zero image egress, preserved
the cached plans and observed complete retirement of their exact TensorRT State.
Private receipts are `LocalAppData/OkuFlow/uct-pause-proof-20261020-221848-*`,
`uct-pause-proof-20261020-221900-*` and `uct-pause-proof-20261020-221912-*`.
The old direct-image diagnostic's 89-second process reported QtTest success
then an access violation at exit. Its original log/exit receipt are retained;
the direct diagnostic now waits for the existing exact TensorRT State retirement
receipt. This does not establish a production crash attribution.

## Validation status

The first-party native scene-cut regression observes actual decoded red/blue
pixels, requires adjacent distinct samples 100 ms apart, then checks that its
following proof is blue while the selected lease/PTS stay fixed. The original
test assumed the authored cut at 1.0 s equalled the system decoder's timeline;
it failed because this machine returned the first blue sample at 1.2 s. An
independent FFmpeg decode returned blue at 1.0 s. The fixture bytes and failed
evidence were preserved; the revised test records the 200 ms discrepancy and
uses the observed native transition. Its pass does not resolve reordered-H.264
timeline equivalence, which remains a separate media validation limitation.

The first repair passed the tracked release compile, 146 CPU cases (two contain
explicitly skipped optional Qt rows), and the 17-case CUDA delta, with no skipped
CTest cases or unresolved blockers. The scoped PCH-OFF core, controller, native
playback and shared-source guard also passed all four cases without skips.
Evidence is under `LocalAppData/OkuFlow/uct-paused-continuity-20261021/`, including
`release-cpu-cuda-background-matrix.log` and `pch-off-gate.log`. These checks
exclude native desktop GUI cases and are partial release validation.

Further review is in progress: an optional-refinement failure/exhaustion path
can still erase a prior proved artifact, and local color sampling is being
audited. Opus review and then Astra review follow the Sol implementation. No
native GUI, camera or screen-reader acceptance is claimed from the private-source
or offscreen fixtures. Hosted native GUI validation remains pending.
