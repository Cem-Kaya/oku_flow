# UCT on an initially paused video

An opened saved video is a selected immutable source before Play is pressed.
Its first native sample can legitimately have sample ID and PTS zero. The
controller can recognize that owned sample when its backend becomes ready;
Play, a camera heartbeat and a positive sample ID are not prerequisites.
Selectable reading text and replacement glyphs have different evidence needs.
A single selected result can supply reading text while original glyphs remain.

Replacement glyphs require a genuine neighbouring acquisition within 250 ms,
different complete source pixels, agreeing recognition and the existing
registration proof. A new buffer, a refreshed timestamp or waiting longer on
the same held frame does not supply that evidence. No threshold is changed.

## The reported six-second clip

One authorized CPU-only extraction of the owner's private `Tr 9_av1.mp4` on
2026-10-22 found a 900 x 800 AV1 stream containing three frames over six seconds
at 1/2 FPS. The actual first two decoded PTS are 0 and 2000 ms, each with a
two-second duration. Their RGB pixels differ. There is no actual following
sample within 250 ms of the opening frame; the issue is the time gap rather
than identical decoded contents.

Source SHA-256:
`417e64556e371d5f2fdaa0622555722fb03598f15f89f7a284d46766a415a672`
(77,896 bytes). Packed RGB SHA-256 for the CPU-decoded first two frames:

- 0 ms: `67eaacfe4234e976b261b7691238b94fb59100e196f3db77ce02783d302fe89f`
- 2000 ms: `ec87163bb5e1f1cbb9831dc91bc05087af8258e54c798cb3d4c245810dd9d0bf`

The software extraction used libdav1d, two CPU decoder threads, no hardware
acceleration, no audio, no frame-rate conversion and a two-frame output limit.
PNG conversion is only provenance evidence; it is not an application input,
Media Foundation decoder comparison, OCR run or native presentation trace.
The clip and extracted images stay outside Git and release payloads.

The source path is explicitly private, not a discovery/default path. The
extraction log and `provenance.json` are under
`Z:/Projects/OkuFlow/local-work-data/uct-initial-frame-zero-20261022/`.
The receipt binds the source, PTS, image hashes and exact inspected base
`e2d433fcd184d669d79ef40096a2a03180b1c28e`.

Static source tracing at that base shows `VideoPlayback` arming optional
following-sample preparation after a paused Open. `FollowingFrame` accepts
only an issued sample with a strictly later PTS at most 250 ms away. Thus a
2000-ms following sample is queued for future playback but not supplied to
paused OCR. This is a source-level trace, not evidence of a live callback run.
Successful Play uses the separate advancing-media policy and cannot establish
that this sparse immutable pair meets the paused policy.

Original glyphs at paused frame zero are therefore expected for this clip
under the retained evidence rules. This investigation does not establish a
controller lifecycle defect or claim to repair one. If selectable reading
text is also absent, that is a separate symptom requiring a reachable
decoder/presentation/backend trace. Rendering replacement glyphs here would
require a separately reviewed proof policy for sparse video, not a lifecycle
retry or a fabricated nearby sample.

## Regression coverage and validation limits

`initialPausedFrameZeroWaitsForReadyAndNeighbour` adds eight scripted rows:
CPU pixels and a resident protocol oracle, each with the neighbour present at
selection, arriving during backend initialization, arriving while selected
recognition is held, or arriving after its result. Every row starts with
selected frame/PTS zero without any Play/live presentation. It requires two
bounded jobs, selected-source provenance, stable generation/authority, no
periodic work while held, and no UI-thread pixel access. The resident oracle
is CPU-backed and does not validate CUDA kernels or real model readiness.

`initialPausedFrameZeroWithoutIndependentNeighbour` adds four rows covering
an absent qualifying neighbour and a separately owned but byte-identical
nearby sample. These require reading text, original glyphs, empty positioned
screen text and no repeated idle work. The absent-neighbour rows model what
the decoder supplies for the sparse clip; they do not feed an out-of-window
sample directly into the controller. The existing startup bridge/refinement
test now also begins at frame/PTS zero.

`LongGopSeeking` adds native CPU-reader assertions that opening the existing
first-party fixture selects frame zero and obtains a real following neighbour
without Play or Pause, preserving the exact selected lease, request/file
identity, 250-ms bound and decoded quotas. The private owner clip is not used
by automated tests.

These changes add regression coverage and documentation only. Builds and tests
have not been run: parent resource coordination holds this checkout's build.
No native UI, GPU, model, device, audio-endpoint, owner-app or settings work
was performed. No production lifecycle fix or release acceptance is claimed.
