# Compact playback and dense-text repair (development)

The transport places playback icons, the scrubber and time on one line at normal widths. A slim
filename/status line preserves full accessible names and tooltips. Targets are
at least 44 logical pixels; secondary actions wrap below only when necessary.
At narrow widths time joins the metadata line and secondary actions wrap below.
A fixed vertical size policy prevents the bar filling spare viewport height;
its wide minimum does not prevent wrapping. A focused action survives row changes.
The existing seek, pause/resume, jump, save, mute and return actions are unchanged.
See [transport](video_transport.md) for keyboard and screen-reader semantics.

## Panning a paused video

ROI-dependent sharpening can reprocess the same selected frame and return a new
analysis allocation. Replacing UCT's admitted lease made the immutable controller
treat each pan as a new selection, clearing its artifact while OCR ran again.
`SelectedMediaGpuSceneProcessed` now retains the admitted lease while source
epoch, sample sequence and appearance still match. View changes project the
existing scene-space glyphs on the presentation clock, without waiting for OCR.
Actual frame/source/effect changes still revoke old evidence. This does not
change freshness rules for advancing video or promise measured 120 Hz rendering.

## Dense and small print

One detector input with a 960-pixel long edge shrank 1080p screen lettering by
half. Both Paddle backends now run complete-scene context followed by bounded,
overlapping detail crops. Each invocation keeps the existing detector/engine
shape limit. The region planner covers the complete image with at most four
crops per axis and 160 source pixels of overlap (desired source edge 1280).
For 1920×1080 this is the context pass plus two detail passes. Beyond the
four-per-axis cap, crops grow: 8192-pixel edges use 2168-pixel crops. Very
large sources still lose small-print resolution; 1280 is a target, not a guarantee.

Very long lines also exceeded the recognizer's 1024×48 canvas. Each detected
line keeps its whole-line recognition and may add up to eight overlapping
windows targeting 20 text heights wide with eight heights of context overlap.
At the eight-window cap, windows widen to preserve full coverage and overlap.
Internal cut-edge tokens are excluded;
source edges remain eligible. CPU/GPU coordinate and cut-edge conventions match.
Mapped word metadata is reconciled inside logical reading lines before admission:
internal crop-edge
fragments are excluded and overlapping lower-confidence evidence cannot erase
a reliable token. The merged output is bounded to 1024 nonempty lines.

TensorRT crops are strided subviews of the owned GPU image. Window generation,
normalization, detection, rectification and CTC remain device operations; only
the existing region count and final compact metadata return to the host. No new
CPU pixel copy, inference dependency, model download or engine profile is added.
Explicit CPU fallback uses CPU crops. All passes stay on the cancellable OCR
worker, independently of rendering. Additional recognition work can increase
artifact latency; this is not an OCR throughput or universal accuracy claim.
Uncertain/math/handwritten text still obeys existing omission/validation rules.

## Long-GOP seeking

A private 1080p60 HEVC recording reproduced failed 3-, 7- and 23-second seeks:
120 preroll reads finished in about 60 ms but had not reached the target frame.
The player now permits up to 4096 reads under the same two-second request-age
deadline, memory credit and generation-cancellation checks. Pre-target frames
do not allocate presentation copies. A new seek supersedes the old request;
neither the UI nor a new seek waits to drain a frame queue. This removes an
arbitrary short-GOP restriction without allowing unbounded work.

## Validation scope

The external codec fixture preserves failed baseline evidence and exercises
Open, seeks at 1/3/7/13/23 seconds and actual End on the same HEVC file. The repaired
path passes and remains GPU resident. Model-free tests cover detail coverage,
overlap, duplicate reconciliation, protected/search coordinates and long-line
windows. Actual CUDA tests compare GPU window geometry/cut-edge flags with CPU.
The actual-app offscreen fixture supplies fresh allocations at 125 Hz for the
same selected sample and requires stable UCT lease identity, then verifies real
appearance changes and foreign leases revoke correctly.

`PaddleTests::optionalDenseImage` is an explicit private-image diagnostic.
Set `OKUFLOW_UCT_TEST_IMAGE` to a local image; `OKUFLOW_TEST_PADDLE_GPU=1` selects
the pinned TensorRT pair, otherwise the configured CPU default is used. It
reports candidate text and elapsed worker time. A positive word count is not
accuracy acceptance; inspect the actual source and output. Private owner images,
recordings and model caches remain outside Git and release payloads.
Native focus, screen-reader and sustained display-rate validation remain separate
from these headless checks.

Muse independently identified capped-window gaps, reading-order fragmentation,
empty successful OCR for oversized CPU images and destructive cap admission.
The repaired planner widens capped windows, retains a whole-scene pass for large
CPU images, and replaces words inside their existing reading line before any
capacity decision. Tests cover aspects 104/132/160/200, cut flags, interleaved
replacements and replacement at 1024 lines. GPU geometry checks include rotation.

The first integration run additionally exposed an existing AV1 finalization
race: Media Foundation backwrites or SetLength revoke the archive duration just
before Finalized is called. The archive now separately retains only the maximum
duration established by parsed fragments and restores that mapping evidence on
finalization. It never restores read ranges, seek permission or writer-accepted
PTS as proof. A deterministic native fixture revokes a copied recording's bytes
before finalization and requires retained mapping with denied read permission.
Original failed AV1 evidence remains archived.

The final tracked local integration run passed shipping Release compilation,
108 CPU cases (two contain optional skipped Qt rows) and all 17 CUDA delta
cases, with no whole-test skips or unresolved results. All 24 translation
catalogs retained the 877-key parity contract. Native GUI checks were excluded
for this run; they are delegated to GitHub's disposable Windows desktop.
This is background acceptance, not a full release or native screen-reader gate.

A final single-image diagnostic on the unchanged private source frame produced
178 candidate words with cached GPU medium/medium inference (354 ms in the OCR
worker) and 179 with CPU tiny/small (9351 ms). Both returned the long explanatory
paragraph omitted in the reported output. These are inspected diagnostic samples,
not accuracy scores or throughput benchmarks; duplicates and OCR typos remain.
Model preparation/startup is excluded from those worker times. The final rebuilt
native GPU decoder again passed Open, all selected seek targets and actual End
on the owner's HEVC source without modifying that file.
