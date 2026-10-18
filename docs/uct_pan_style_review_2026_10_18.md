# UCT pan continuity, source colors and ink fit (2026-10-18)

The owner reported one-to-two-frame replacement disappearance during viewport
pan, oversized replacement glyphs and harsh black plates on a gray source.
Opus performed a read-only review of `072548e` and the parent's evolving diff.
It did not build, run fixtures, operate the desktop or approve a final immutable
snapshot. The parent verified the code paths and implemented the repairs below.

## Confirmed causes and repairs

- `TryAcquireFrameSlot` can normally return busy during pan. Both app presenters
  previously reported this as failure, clearing/hiding UCT and its reading pane
  until the next success. `OnPresentationDeferred` now retains the last committed
  projection/frame without renewing the watchdog. Changed viewport dimensions,
  invalid transforms, different geometry and actual faults refuse that reuse.
- A refresh could replace a verified object with an unverified new reference/id.
  A near-identical draft now retains the complete still-fresh old object; proof
  never moves to new reference bytes or coordinates. Fresher validated results
  win. Each retained id is consumed once. A missed check budget retains the old
  proof without renewing it; source/geometry invalidation, residual failures and
  the existing 350 ms freshness/600 ms successful-presentation watchdog remain.
- Probe scheduling now checks unverified lines first, then oldest validation,
  avoiding permanent dense-page tail starvation under a bounded budget.
- Paddle boxes include detector expansion and approximate CTC spans. Marked
  words now receive worker-side ink fitting before glyph layout. Dominant row
  bands avoid unrelated neighbouring-line descenders; dotted/accented tokens
  retain detached marks. A one-step CTC search, bounded by the line and adjacent
  word midpoints, can recover ink beyond the original word edge. Ambiguous
  multi-band, tiny or low-contrast fits retain the original detection box.
- Background-class ring medians avoid averaging blurred ink into board/paper
  colors. Neutral ink snaps to white/black. Source backgrounds remain related
  gray/color, adjusted only as needed for 7:1 contrast. Accepted backgrounds use
  a fixed 64-level brightness grid with hue retained and an 8-channel-level
  hysteresis band. The camera image itself is not quantized; no per-frame
  fitted palette or extra recognition/render synchronization was introduced.

## Evidence and limits

Private evidence is under `%LOCALAPPDATA%/OkuFlow/uct-pan-style-20261018/`.
`focused-rows.txt` records the first 20 focused Qt passes; review repairs then
passed 24 focused rows in `opus-repairs-focused.txt`. These include 300 alternating
successful/busy pan attempts while OCR is held, with constant word count/reading
text and zero overlay Hide/Show events, plus resize refusal, duplicate retention,
probe priority, color hysteresis, accents/descenders, adjacent-line exclusion and
a CTC edge undershoot. Decoder tests also verify bounded neighbour searches.

The first tracked background matrix passed release compilation, 86 CPU cases
and 10 CUDA delta cases before the final review repairs. The final rebuilt
matrix in `final-background-matrix.txt` passed release compilation, 86 CPU cases
and 10 CUDA delta cases after the final repairs. One optional development MCP
Qt row is skipped inside a passed CPU case; no selected CTest case was skipped
or unresolved. Native GUI cases were excluded. Resumed results were reused only
when the tracked runner's exact per-case fingerprints matched.

Native desktop checks were excluded while the owner used the app. Offscreen
visibility/geometry tests do not measure DWM composition or native pan latency.
The overlay remains a separate translucent window from the swap chain; exact
glyph/video scanout synchronization would require measured follow-up and likely
presenter composition. A hidden replacement beside an old visible object is
also deferred; changed recognized words continue through existing conservative
admission. Colors still come from neutral pre-appearance analysis, so explicit
display-LUT matching remains separate from this source-color change. No claim
of exact character contours, handwriting/math accuracy, physical GTX 1070,
camera optics, clean-VM installer acceptance or a newly published installer is
made. The owner's already-running older app was left untouched.
