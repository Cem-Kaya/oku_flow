# UCT text coverage and stable placement (development)

This repair addresses missing replacement words and unstable placement in the
owner's saved atlas video. Recognition, validated model coverage and actual
painted glyph coverage are different measurements. A total word-agreement
percentage alone cannot establish that a readable headline is being replaced.

## Reproduced defects

- Both fixed-width and bucketed GPU recognition read
  `scripts/integrate_castle_runtime.py`, but the notation filter rejected its
  underscores. Conventional file tokens with a one-to-sixteen-letter ASCII
  extension now allow underscores and Windows path separators. Standalone
  confusables, mixed ambiguous digit/letter tokens, `x_2`, `a_b` and math
  notation keep their existing rejection rules.
- The crop merger joined the small script/diff column to the much larger
  `The tribe, the machines` title. That coupled their reference validation and
  shared font fitting. Row association now uses a nearby individual word with
  comparable type size (at least half the larger ink height), compatible vertical
  position and a gap of at most four smaller word heights. Even a previous
  preferred row must pass this test. Overlapping copies still replace words
  within their existing line at the original resource cap.
- An overlapping crop contributed a stray `m` at the beginning of `machines`.
  It missed the ordinary 70% overlap threshold by a fraction, remained
  unconfirmed, and protected pixels needed by the complete word. A contained
  literal prefix/suffix fragment occupying less than 35% of the larger box
  now participates in duplicate resolution at 65% overlap. Existing confidence
  and candidate precedence decide which observation survives. Non-fragments
  retain 70%; geometry rejects unrelated pairs before string comparison.
- A half-pixel change to an omitted token's box prevented retention of an
  otherwise current-validated line. The new observation replaced its ID and
  glyph positions. Matching exclusions within 10% of their smaller height can
  now retain the old word objects, references, placement and validation clocks.
  Exclusions become the union of old and incoming rectangles, so retention
  never reduces protection around uncertain ink. Larger or structurally changed
  exclusions still prevent this retention.
- A full-view OCR refresh discarded independently checked static text when
  detection omitted it, even while another region was the only one scrolling.
  Live/advancing admission now keeps that existing object only if a real pixel
  probe validated it at/after the OCR acquisition and no prepared observation
  or admitted replacement overlaps it. Rejected/uncertain spans count as
  observations, so a conflicting spelling cannot revive old text. The exact
  old identity, reference, geometry and proof clocks survive; omission buys
  no additional lifetime. A later checked pixel mismatch still removes it.

These changes add no recognition pass, frame transfer, model or engine plan.
Source/seek/processing revocation, independent corroboration, explicit conflicts
and freshness expiry stay authoritative. Small OCR corrections are stabilized
at their proved placement. Actual pan/zoom continues to follow the presentation
transform immediately; glyphs are not animated away from the image they label.

## Validation and evidence

The ordinary CPU fixtures reproduce the original column merge, path rejection
and line-ID churn before repair. Their original failed logs remain in private
LocalAppData evidence. Regressions also cover contained fragments, independent
adjacent words, unchanged proof clocks and eventual expiry.

The separate regional omission fixture failed with zero shown words before
repair and one independently proved static word afterwards. The real
controller path covers both live and advancing video, and the CUDA residual
fixture compares local CPU/GPU matches when the right side changes enough
to fail global stability. Existing local residual matching already supplies
the needed regional evidence; this repair adds no optical-flow pass.

The opt-in paused-video diagnostic now runs the production glyph overlay under
Qt offscreen, registers Segoe UI explicitly, records `paintedWords` and
`paintedCount` separately from model eligibility, and saves `overlay.png` from
the production snapshot path. Its selected/neighbour worker durations are
individual observations, not warm distributions or native display latency.
The initial unregistered-font preview is retained as unsuitable evidence.

Private source: `2026-10-01_16-48-10.mp4`, with selected/following images at
0/50 ms and 500/550 ms. In the repaired GPU proof, all four title words and the
complete script path paint together. GPU image-egress remains zero; cached plans are unchanged
and the exact TensorRT State completes retirement. This is a labelled five-token
coverage check, not an accuracy claim for every word in the video.

At 0/50 ms, actual painted words increased from 159 to 161: `machines` and the
script path were added, with no previously painted occurrence lost. The
correct-font baseline did paint `The tribe, the`; the entire-title absence
reported in a different moment was not reproduced at this exact sample.
Both fixed and bucketed policies painted all five labelled targets. A separate
56,000/56,050-ms paused proof painted 171 words, including the script path;
this alone is not an advancing-scroll continuity test.

The same saved 0/50-ms proof also records `drawn: true` for all five words in
the small brown heading, `HUMAN CIRCUIT / MASTER ASSET LIBRARY`. It contains
two identical painted `/` records at one rectangle, and the reading text has
`/ /`; the follow-up four-corner geometry repair now reconciles this post-fit
same-ink duplicate (see its separate validation). The heading
is not missing from this sample. This audit adds no recognition job and does
not establish coverage at a different video position.

There is no global four-times OCR zoom clamp in this baseline. Detection
downsizes each region to a 960-pixel maximum edge; the overlapping detail grid
has at most four columns and four rows. Those are separate limits. Six-times
visible-region detail is implemented separately by the paused-navigation
[geometry/detail repair](uct_oriented_detail_repair.md); the earlier coverage
measurements below do not validate that follow-up.

All nine N8 timed before/after pairs from three fixed source images had equal
source digests. The raw candidate audit retained every occurrence except the
contained stray `m`; matched edges were equal within floating-point roundoff.
This measures candidate consistency, not ground-truth recognition accuracy.

The finite warm complete-path comparison used medium/medium, batch eight and
three timed jobs per source. Median worker milliseconds were:

| Source | Fixed 1024 | Bucketed |
| --- | ---: | ---: |
| Chat/game frame 55 | 224.337 | 201.951 |
| Chat/game frame 89 | 217.974 | 195.489 |
| Atlas frame 0 | 168.432 | 168.732 |

These shared-PC observations include detection and preparation, exclude cold
compilation, and establish neither p95 nor RTX 3050/120-FPS performance. Both
diagnostic processes retained their original strict N1/N8 assertion failures
(nine each); their complete timing/ownership records and cache/retirement
checks are separate evidence, not passing strict quality gates. The width
policy remains opt-in. No model, precision or cache plan changed here.

Logs, proof receipts and finite processing comparisons stay under
`LocalAppData/OkuFlow/uct-coverage-stability-20261021/`. Native GUI, screen-reader
delivery and live-camera visual acceptance remain separate from offscreen
paint and resident recognition checks. See [recognizer widths](uct_recognizer_width.md)
for the still-opt-in canvas policy and its independent quality limits.
