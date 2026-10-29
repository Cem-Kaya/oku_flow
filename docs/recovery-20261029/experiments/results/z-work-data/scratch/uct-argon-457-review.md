# UCT Argon findings 4/5/7 — independent read-only verification
Worktree: C:\Users\USER\AppData\Local\OkuFlow\worktrees\test-opt (no edits, no build/test run)

## 4a quint32 integral overflow — FALSE on every real path (latent API hazard only)
- Threshold: floor(2^32-1 / 255) = 16,843,009 probe px.
- ProbeWorker path (controller.cpp:423-424, kMaximumProbeWidth=960 at :46):
  decimation = max(2, ceil(W/960)) => probe width <= 960. 8K 7680x4320 -> dec 8
  -> 960x540 = 518,400 px (3% of limit).
- Scheduler reference path (ultra_clear_text.cpp:850-853, controller.cpp:145,
  UctTiming probeDecimation=2 controller.hpp:172): dec fixed 2, no width cap.
  Worst plausible viewport 8K -> 3840x2160 = 8,294,400 px, max sum 2.11e9 =
  49% of quint32 max. 4K -> 1920x1080 (12%). Overflow needs dec-2 source
  > 67.4 Mpx (~16K) or a direct public-API MakeProbeFrame(...,decimation=1)
  call on >16.8 Mpx; no production caller uses 1 (only two production call
  sites :850, :1053; tests use 2/4). Viewport size originates from the
  presenter/window, not arbitrary images.
- Block `sum` (cpp:424-431): perAxis <= 3 for dec>=2 (3 only at dec 3,5),
  area <= 9, sum <= 2295. No overflow.
- Optional hardening only (not a real-path defect): fail closed in
  MakeProbeFrame when (qint64)ow*oh*255 > UINT32_MAX, and compute
  (ow+1)*(oh+1) in qsizetype at cpp:435 and hpp:119 (int product today).

## 4b remainder/centroid sampling drift — VERIFIED in code, bounded, mostly cancelling
- cpp:407-408 ow=W/dec (floor), luma x samples source x*dec.. (cpp:426-428),
  but SceneToViewPixel (cpp:368-377, used cpp:101, :677-678) maps u -> u*ow.
  True coord u*W/dec; error = u*(W mod dec)/dec probe px = u*(W mod dec)
  source px, max <1 probe px at u=1 (2560 dec3: 0.333; 3840 dec2/dec4: 0).
- Sample centroid (cpp:418,425-428): dec2/3/5 centred; even dec>=4 biased:
  dec4 {0,2} mean pixel-centre 1.5 vs block centre 2.0 = -0.5 source px
  (-0.125 probe px); dec8 -1.5 source (-0.1875 probe px).
- Cancellation: same W+dec on both sides => identical error, no effect.
  SceneSimilarity compares two ProbeWorker probes (same formula+width) =>
  Argon's SceneSimilarity claim refuted. Viewport resize invalidates scene
  (controller geometry generation), so W is fixed within a scene.
- Non-cancelling remainder: (a) cross-decimation (finding 5 paths, W>1920),
  (b) cross-zoom (same scene point at different u). Magnitude: differential
  source-px error <= |(W%decP)-(W%decR)| <= 2 px (dec4 vs dec2), scaled by u,
  plus <=0.5-1.5 source px centroid differential. MatchPatch search radius
  is 6 probe px (hpp:283) = 24 source px at dec4, so matching does not break;
  the cost is a spurious sceneOffset of that magnitude (applied verbatim to
  overlay words, cpp:1427) and slight NCC loss across a wide patch (error is
  a scale, not a translation; across a 10%-width patch only 10% of the edge
  value).
- Concrete: W=1003 (dec2 ow=501, dec4 ow=250), line at u=0.9, identical
  frames: expected reported offset ~= 0.9*(3-1) + 0.5 (centroid) ~= 2.3
  source px instead of 0.

### Proposed narrow fix (only 4b; parent to apply after matrix)
Store decimation on ProbeFrame and map with effective fractional size
W/dec instead of floor ow; bounds checks keep using luma dims (the unsampled
right/bottom strip of <dec source px correctly becomes not-visible):
```cpp
// hpp ProbeFrame: add `int decimation{1};` ; cpp MakeProbeFrame: probe.decimation = decimation;
// cpp, near PixelsPerSceneX:
double ProbeViewWidth(const ProbeFrame& p) {
    return (p.decimation > 1 && p.stamp.viewportWidth > 0)
        ? p.stamp.viewportWidth / double(p.decimation) : p.luma.width();
} // + ProbeViewHeight analogously
```
Use ProbeViewWidth/Height in PixelsPerSceneX/Y (cpp:69-79), ProjectPatch
w/h (cpp:92-93), BuildReferencePatch w/h+px (cpp:461-470), SceneSimilarity
(cpp:652-678). Do NOT change sample offsets (centroid bias <=0.1875 probe px
is absorbed by parabolic refinement cpp:568-582 and cancels same-dec).
Docs: ProbeFrame is public => update docs/code_reference.md per AGENTS.md.

### Deterministic regression (public API, existing test style tests:355,404,1417)
Synthetic BGRA 1003x240, sharp dark text-like bars near u=0.9; reference =
MakeProbeFrame(...,2), patch = BuildReferencePatch(reference, lineSceneRect);
probe = MakeProbeFrame(samePixels,4); MatchPatch must be matched and
|sceneOffset in source px| < 1.0. Fails today (~2.3 px), passes with fix
(residual centroid 0.5 px). Guard companion: MakeProbeFrame dec1 on
5000x4000 (20 Mpx) returns invalid IF hardening adopted.

## 5 decimation mismatch scheduler(2) vs probe(ceil(W/960)) — fact TRUE, breakage UNPROVEN;
##    classification: plausible, needs reproduction. Do not align blindly.
- Verified: W 1921-2880 probe dec3, 2881-3840 dec4 (controller:423-424);
  reference always dec2 (controller:145 -> cpp:763,850-853). 4K reference
  integral = 1921*1081*4 = 7.92 MiB (+2.07 MiB luma) per OCR job, real but
  worker-thread and >=1.5 s apart; still 12% of overflow limit.
- By-design counter-evidence: BoxAverage comment cpp:49-51 "Cells cover the
  same scene area in every probe, so values stay comparable when the zoom
  differs"; ProjectPatch (cpp:104-105) recomputes cell half-extents per probe
  via PixelsPerSceneX/Y. Cross-resolution matching is required for zoom
  changes anyway (pixels-per-scene varies far more than 2x), so distinct
  reference/probe resolution is intentional, not an oversight per se.
- True sub-claim: when columns/rows are unclamped (cpp:471-472
  columns~=round(refPxWidth)), cell ~=1 ref px; in a dec4 probe cell ~0.5 px,
  half ~0.25 -> BoxAverage floor 0.5 (cpp:54-55) doubles the box in scene
  terms (4 vs 2 source px). BUT the dominant kernel difference is the luma
  decimation itself (dec4 luma = sparse 2x2 samples of 4x4 block cpp:425-431
  vs dec2 full 2x2), which no BoxAverage change can undo; and when columns
  hit the 64 cap (long lines) no clamp engages and scene-area kernels match.
  NCC is mean/variance-normalized (cpp:147-155, patch values normalized
  cpp:493-495); whether score drops below tuning 0.82 (hpp:281) for small
  text is an empirical question Argon did not test.
- Argon's oracle score>0.99 + equal luma widths is invalid: sparse dec4 vs
  dec2 sampling cannot reach 0.99 on fine text by construction, and width
  equality is not a contract. Existing coverage: full-controller 4K test
  uses 140 px font (tests:1355-1385); dense 4K test is dec4/dec4
  (tests:1404-1418, ResultForView dec4 tests:404-405). The untested gap is
  small text (word height ~8-16 job px) at W>1920 through the controller.
- Decision procedure (deterministic repro BEFORE any fix): synthetic
  2560x1440 page, word heights 8/12/16 job px; reference dec2 (as scheduler),
  probe dec3 = ceil(2560/960) (as SubmitProbe), identical + 3-source-px
  shifted frames; assert matched==true, score >= 0.82 (design threshold,
  not 0.99), |offset - true| < 1 probe px. If it passes: finding closed as
  intentional. If it fails: narrow fix is per-job floor in
  RecognitionScheduler::State::Run, refDec = max(referenceDecimation,
  ceil(job.pixels.width()/960)) (same cap formula, computed where width is
  known, cpp:850) — note trade-off to re-verify: columns/rows then derive
  from coarser px (cpp:469-472), roughly halving patch samples at 4K, and
  cross-zoom behaviour must be re-run; allocation also becomes bounded.

## 7 ReadingText anchorBottom non-update — FALSE as defect; intentional, Argon fix breaks existing test
- Code cpp:1480-1491 explicitly anchors on the row's first line "so
  membership never depends on pair order". Entries are OCR *lines*
  (AcceptedLine, cpp:1463-1466), not word segments; ReadingText emits one
  output line per entry regardless of row (cpp:1502-1519) — row only orders.
- Argon's descender case self-refutes (0.7H < H). Same-baseline segments can
  never split (centre of taller < bottom of shorter when bottoms align).
  Split needs same-top boxes with >2x height ratio — not an OCR line layout.
- Argon's tilt regression PASSES on current code as observable text: centres
  .210/.222/.234 vs anchors .220/.232 give rows 0/1/2 and final order
  L,M,R = correct reading order (monotonic tilt). It would only "fail" an
  internal row-equality or merged-single-line assertion, neither of which
  is the contract. Camera tilt also yields one tall OCR line box, not three
  stepped entries; the view transform is axis-aligned (no rotation term).
- Argon's fix anchorBottom=max(...) is chaining and breaks the existing
  intentional fixture mixedHeightReadingOrderIsStable (tests:1305-1336,
  permutations of Alpha c100/20px, Beta c108/100px, Gamma c116/20px,
  expects "Beta\nAlpha\nGamma"): with max-update Beta (bottom ~158) chains
  Gamma into row 0 and left-sort yields "Gamma\nBeta\nAlpha". The tall-first
  anchor behaviour is precisely the drop-cap/tall-line case Argon cites,
  and the design chooses determinism over merging.
- No fix. At most add a characterization row to existing test documenting
  monotonic-tilt L/M/R ordering above (passes today).
