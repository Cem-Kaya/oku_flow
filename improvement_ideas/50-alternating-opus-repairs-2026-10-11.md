# 50 — Alternating implementation and review with Opus (2026-10-11)

The owner requested one Opus subagent and root/Sol to work through verified
issues, alternating the author and reviewer. Existing working-tree repairs,
including review 49's unused-code cleanup, are preserved. This batch does
not commit, push or deploy. Camera sessions and native desktop tests remain
deferred at the owner's instruction.

## 1. ROOT-48-KEY — annotation keyboard feedback (P1)

Author: **root/Sol**. Independent reviewer: **Opus 5.5**; **ACCEPTED for headless scope** after the final recheck.

The production overlay filters both its owner and render ancestors. Before
repair, a navigation key calls `ForwardKeyToOwner` while filtering the same
owner, synchronously re-entering the filter. Inactive Draw also intercepts
tool, Escape and undo keys. A bounded headless reproducer prevents a stack
overflow while exercising the production class: 72 pan/zoom routes and the
inactive-shortcut check fail before the repair (73 failures; three checks pass).

`AnnotationOverlay` now intercepts keys only while active and visible. A scoped
`filteringKey_` flag leaves unhandled filtered navigation to the original Qt
propagation route; `forwardingKey_` bypasses the overlay filter during explicit
owner forwarding. Each navigation branch returns after its one forwarding
attempt, including when the owner ignores it. Auto-repeat/count/modifiers are
preserved. Draw's local tool/selection/undo logic remains active.

Opus independently reproduced the original recursion even with Draw off, and
identified child-control acceptance loss, inline-editor navigation leakage,
and unmatched pan releases. The revision restores incoming acceptance on
unhandled filtered keys, confines pending text entry to its editor, tracks
accepted forwarded pan arrows, ignores auto-repeat releases, and flushes held
keys on hide, disable, focus loss or window deactivation. The hidden test now
asserts an immediate synthetic release without sending a key to a hidden window.
Opus also requested explicit inactive navigation coverage, which is added.

`annotation_key_routing_headless` uses Qt's offscreen platform, asserts that
platform, and opens no native desktop window. It checks owner/render/overlay
input, accepted/ignored owner keys, arrows/PageUp/PageDown/zoom, Ctrl zoom,
auto-repeat metadata, inactive Draw and a local tool shortcut. A test-only
redispatch fence bounds the unsafe before case. All 91 checks in the original
final revision pass after repair; the inline-focus follow-up below brings the
total to 94. The original 91-check scope has Opus acceptance; native focus and
layout acceptance remain a separate deferred GUI gate.

Evidence: `output/alternating50/key-before.log`, `key-after.log` and the tracked
`tests/annotation_key_routing_tests.cpp`. Opus's extended independent probe
passes all ten acceptance cases (one additional focus observation is
informational); its verdict is recorded in
`output/alternating50/opus-key-final-review.md`. Native acceptance remains
outstanding. Opus's nonblocking observations included untouched empty text
entries remaining pending after focus loss (now repaired in section 4),
hypothetical re-entrant flush handlers, and pre-existing Draw-off pan flags
on window deactivation.
The final tracked matrix is recorded below. No FPS or native focus result
is claimed.

## 2. G36-4 — bounded settings ingestion (P2)

Author: **Opus 5.5**; reviewer and completion author: **root/Sol**.
**Accepted by root for the tested scope.**

Load reads binary chunks bounded to 16 MiB plus one byte, including CRLF;
file size is only a reserve hint, so growth after querying cannot bypass the
read bound. Save checks its exact disk representation before touching files.
The cap fits 10,000 maximum-created-length conversations and 500 quick modes
(10,325,496 bytes). Primary/backup recovery preserves oversized files.

Opus's first implementation passed 33 QtTest checks; five restored old
behaviors each failed a regression in its mutation builds. Root then reproduced
an oversized read-only primary being renamed before QSaveFile checked whether
replacement was allowed. The added refusal test failed on that implementation.
Opus revised publication staging and recovery before hitting Claude's rate
limit. Root completed review, fault injection, the error wording and docs.

The final save first opens, writes and flushes its QSaveFile staging file.
Only then does it preserve an oversized original using a same-volume Windows
rename that cannot replace another file. A failed final commit attempts to
restore that original without replacing a newly created primary. A missing
primary at startup now recovers a valid bounded backup; absent/invalid backups
retain ordinary first-run defaults. A deliberate reset must remove both the
primary and backup.

All 41 settings QtTest checks pass, including read-only primary/backup,
locked oversized primary, real commit refusal, backup recovery and size limits.
The normal-primary commit-failure fixture also proves that the backup may
already have rotated to the current document; root removed the inaccurate
error promise that both generations remain unchanged on every failure.

A separate ignored build compiles production copies with one late-failure
injection: `cancelWriting` after preserving an oversized primary. It verifies
restoration, unchanged backup and no temporary-file leak. A second controlled
insertion of a replacement primary verifies it is retained and the oversized
original remains intact in its sidecar. Removing rollback from the copied
source fails with exit 5 because the original is not restored. These are
controlled fault cases, not native disk-full, crash or concurrent-process races.

Evidence: `output/alternating50/settings-failure-before-direct.log`,
`settings-safety-direct.log`, `settings-safety-first-run.log` and
`settings-rollback-probe/{restored,concurrent}.log`. The original Opus report is
`opus-settings-implementation.md`; its final follow-up terminated with a
rate-limit error before tests or a verdict. Root's completion review is
`root-settings-final-review.md`.

Limits: the byte cap bounds ingestion, not total JSON working memory or
serialization of in-memory settings. A minified document can serialize above
the cap; conversation count is not automatically pruned. Startup's existing
copy fallback has no elapsed-time bound. File-growth races, hardware storage
failures and crash durability were not tested. If restoration itself fails,
the preserved oversized original remains in its sidecar; the no-replace
policy does not promise that every failure restores the original pathname.

## 3. G36-3 — retained CPU rotation vectors (P2)

Author: **root/Sol**; independently requested Opus review did not finish
before its rate limit. Root completed the pixel/allocation verification.

Rotation now resizes its scratch vector and swaps it with the input vector,
retaining both size and capacity. Every output pixel is overwritten, including
alpha; no equal-size bulk zero-fill is required. `yuv_color_conversion`
adds a forward-scatter reference across 264 changing, padded, non-square
frames and positive/negative/no-op turns.

The same optimized VS2022 allocation probe at 640×360 finds 20 full-payload
allocations over 20 post-warm-up rotated frames before, and zero after.
Unrotated positive-pitch control remains zero; the separate negative-pitch
conversion allocation remains 20 and is not changed by this repair. Files:
`output/alternating50/rotation-baseline.log`, `rotation-after.log` and
`rotation-quality.log`. Peak capacities remain retained. No end-to-end FPS
or frame-time improvement is claimed.

## 4. Opus K1 — retire untouched inline text on focus loss

Author: **root/Sol**; root verification complete. Independent Opus review
of this follow-up remains pending after its rate-limit failure.

An actual Qt focus transfer (not a synthetic FocusOut) reproduces the
untouched-empty editor remaining visible/pending before this revision.
Whitespace and typed-label controls already pass. The editor now installs
the overlay filter, which commits or cancels a pending text entry when
focus leaves; existing guards retire pending state before hiding to avoid
double insertion. The three focus rows pass after the repair, including
restored owner-arrow handling for an empty entry and exactly one text item
for a typed label. Evidence: `output/alternating50/inline-before-direct.log`,
`inline-probe/test-result.txt` and `key-with-focus-followup.log`.
The standalone build uses production sources and Qt's offscreen platform,
not native desktop acceptance. The final headless key suite passes all
94 checks, and the implementation and behavior docs are updated.

## Remaining queue

- G37-3 / G37-1 / G37-5: terminal photo/annotation handling and explicit recording
  availability policy after permanent GPU/camera failure. Preserve in-flight
  GPU quarantine and bounded asynchronous recording shutdown.
- AST-REC-R4: decode-preserving AAC timeline gaps. The strict optional reproducer
  from review 47 still fails; do not apply a timestamp clamp without resolving
  the transform/muxer/reader clock behavior.
- Independent Opus review of root's G36-3 and K1 changes remains deferred by
  its rate limit. The implementations and root verification are complete.
  Application FPS benefit from rotation reuse still requires benchmarking.
- Benchmark-only NIS, Maxine and ink-cache candidates retain their existing
  quality, lifetime and measurement requirements.

## Owner-requested next 25 work items

This is an ordered work queue, not 25 confirmed bugs. Historical repaired
findings are excluded. Items 2/3 share failure handling; each has a separate
acceptance path. Verification items close earlier source-only coverage gaps.

| # | Work item | Type / source |
| --- | --- | --- |
| 1 | Bound settings reads/saves and preserve oversized originals | G36-4; now implemented and root-verified above |
| 2 | Cancel photos and annotation captures on permanent GPU failure | G37-3 |
| 3 | Explicit recording policy after expired reconnect/failed restart | G37-1/G37-5 |
| 4 | Preserve decoded AAC/MP4 gap timing | AST-REC-R4; reproduced |
| 5 | Verify viewport/recording-canvas aspect differences | AST-RES-R1; quality check |
| 6 | Late/straddling PCM at a recording segment boundary | REC-AUD-02; conditional refinement |
| 7 | Qualify no-dropped-frames summary as video-only | REC-AUD-05; wording |
| 8 | Retain CPU rotation buffers | G36-3; implemented, independent review pending |
| 9 | Close untouched empty inline editor on focus loss | Opus K1; implemented, independent review pending |
| 10 | Benchmark per-device NIS coefficient storage | KERNEL-5 / review46; candidate |
| 11 | Cache CPU resampler column calculations | G36-2; candidate |
| 12 | Cache unchanged recording ink layers | AST-REC-O5/G36-1; candidate |
| 13 | Pool encoder resources with independent retirement safety | REC-5; candidate |
| 14 | Investigate asynchronous Maxine | CAP-GPU-04; candidate |
| 15 | Benchmark exact Gaussian alternatives against separable baseline | Review45 blur strategy; candidate |
| 16 | Measure existing Text Clarity/NIS/timer improvements | Review45 optimization follow-ups |
| 17 | Native keyboard/focus regressions | Review45/50; desktop deferred |
| 18 | Automatic UI hide acceptance | Review45/47; desktop deferred |
| 19 | Multi-monitor/DPI/1440p acceptance | Plan15; hardware check |
| 20 | Screen-reader/keyboard-only Draw/settings acceptance | Plan18/27; accessibility check |
| 21 | Real busy-device/reconnect tests | CAP-1/plan20; hardware check |
| 22 | Complete camera-to-photo color fidelity | CAP-4; integration check |
| 23 | Complete dynamic camera-format transition | CAP-5; integration check |
| 24 | Delayed readback with no subsequent camera arrival | APP-1; integration check |
| 25 | Maxine reload counters and configuration timing resets | CUDA-3/CUDA-4; integration check |

## Final validation

The tracked `scripts/agent_build.bat` matrix completed successfully with
`OKUFLOW_AGENT_EXCLUDE_GUI=1` (exit 0):

- Shipping release compilation: PASS.
- CPU CTest: 23/23 selected groups pass, 46.69 seconds.
- CUDA CTest: 28/28 selected groups pass, 49.60 seconds, including all five
  CUDA conversion/Text Clarity/stabilization/upscaling/replay groups.
- Translation integrity: 743/743 complete Turkish/German keys match the
  extraction manifest.
- `git diff --check`: PASS.

Evidence: `output/alternating50/matrix-final.log` and `diff-check.log`.
The exclusion is partial validation: seven native GUI groups in each preset
remain deferred. The known-failing optional AAC gap diagnostic remains open
and was not enabled. Native camera/focus acceptance and end-to-end speed gains
are not claimed. No commit, push or deployment is performed by this batch.
Independent Opus review of the rotation and K1 follow-ups remains pending;
root completed their source review and targeted verification after its limit.
