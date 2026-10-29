# UCT source-ink outline implementation review (Opus, read-only)

Checkout `C:/Users/USER/.t3/worktrees/oku_flow/feature-uct-recap-player`, HEAD
`ea66aa0d3f61bdadf5300bb66728a2b2edd1e0d3` (clean) versus
`b3dfa2ee5ff8c4902152c3ee4d5f30379aca749c`. This was a read-only review. I made no
repository edits and ran no builds, tests, inference, network, GUI or model changes.
For evidence I only read the JSON receipts and logs. I also wrote nearest-neighbour
crops of the two `uct.png`/`original.png` pairs to a private temp directory so I could
view them.

Labels used: **Defect** means it is visible in the code with a concrete trigger.
**Hypothesis** means it is plausible but unmeasured. **Design** means a policy or
coverage concern, not a bug.

## 1. Bottom line

- **No P1 found.** Every outline path stays non-text. Outlines add nothing to
  `ScreenLines`/UIA, `overlayWords_`/`shownWords_`, or recognized words. No plate is
  painted. Fill uses the readable foreground through the display palette, clipped to
  `visibleClip`, so native chrome is excluded. Geometry is mapped analysis→scene once
  in `PrepareLines` and scene→view once in the painter transform. The CUDA path
  downloads only the `Summary` scalars and per-level segment lists, each capped. No
  coverage grid, prefix or source pixel leaves the device.
- **One P2 defect (smoothed live admission).** The new `originalInkOnly` bypass lets
  an EMA-only protected line be admitted without current-input agreement. Two things
  follow. (a) It can evict an independently current glyph line. (b) It adds a `…`
  reading-pane line only when an outline was drawn. That contradicts the docs and
  agents.md wording, and the toggle changes reading output. Details in §2.1.
- **One P2 design concern (unmeasured).** The resolved-area guard is sound for the
  two real inputs. The structural correspondence is a correct bijection argument
  (§3). The faint-ink rule, however, only refuses *wholly* faint components. Partly
  faint components and threshold-unstable tiny islands are still painted at full
  readable contrast, without proof. That can change which mark looks dominant to a
  low-vision reader (§2.2).
- The rest are P3 refusal-coverage gaps, diagnostics, optimisation and test coverage.
  None of them can invent text.

## 2. Findings, in priority order

### 2.1 P2 defect: `originalInkOnly` gives smoothed OCR replacement authority and changes reading text

Code: `src/common/ultra_clear_text.cpp` `AdmitImpl`, around lines 3783–3795.
`currentInputMatches` is produced at about line 2500 (CPU) and at
`src/cuda/uct_device_analysis.hpp:936–966` (resident). Both set it to `false` for
any line without a candidate word.

Before this change, a temporally smoothed, non-immutable line with no candidate words
always failed `currentInputMatches`. It went into `failedRegistrationRects`, which
deliberately keeps an `independentlyCurrent` old line (kept loop, about line 4110).
The new bypass admits such a line whenever `PreparedLine::inkOutlines` is non-empty.
That only happens when an outline was actually *drawn*, because refused outlines have
no geometry and `PrepareLines` drops them. Consequences:

1. **Eviction.** The admitted outline-only line enters `admitted`. An old,
   independently current glyph line that intersects it no longer passes the keep
   condition: it is not in `failedRegistrationRects` any more, and an admitted
   replacement intersects it. So it is dropped. The replacement is `valid=false` /
   `matchDeferred` (correct: EMA never proves itself), so the region shows the plain
   original until a probe validates it. A protected verdict that came from smoothed
   pixels has therefore withdrawn current-proven glyphs. That is exactly what the
   comment "No new evidence: do not transfer recognition or its clock" forbids.
   Trigger: live camera with temporal smoothing. The board line was earlier accepted
   as glyph text. A later EMA read classifies the same span as invalid geometry or
   low confidence (ghosting, a ribbon misfit after ink was added, motion blend). The
   outline passes on the current validation pixels.
2. **Reading-pane change.** Once validated, the line is `IsReadable` with empty
   tokens, so `ReadingText` gains a `…` line (`ReadingText`, about line 4608). With
   `OKUFLOW_UCT_INK_OUTLINE=0`, or whenever the outline is refused, it does not. That
   contradicts the doc ("An outline never contributes to … reading-pane content") and
   agents.md ("Keep recognized/readable/UIA text … unchanged"). It is a placeholder,
   not text, but the reading output now depends on contour success. The new
   protected rects also join the model's masks.
3. **Tests.** `protectedInkOutlinesSharpenWithoutAddingText` exercises this bypass
   (`live.currentInputMatches = {false}`). It checks hidden→validated→withdrawn, but
   not eviction or `ReadingText` for the live model.

**Fix (recommended).** Give outline-only smoothed lines their own current-input
proof instead of a bypass. In the CPU loop at about line 2486, a line with no
candidate but with outlines should register `BuildReferencePatch(recognized, ViewRectToScene(... outline.bounds))`
against `result.reference` with `kInputRegistrationTuning`. Require `matched &&
sceneOffset.isNull()` for every outline, and push that result. Do the same in the
resident `currentInputMatches` block using the existing resident `Matches`. Then
delete `originalInkOnly`, so non-matching lines follow the old failed-registration
path. With that in place, the "…" line appears only when EMA and the current frame
agree on the protected span. That is the same rule candidate lines already follow.
Either state that in the doc, or (stricter) keep outline-only smoothed lines out of
`ReadingText` until a non-outline observation of the line exists. Add tests:
(a) an old current-proven glyph line plus a mismatching outline-only EMA line, where
the old line must be kept; (b) `ReadingText` is identical with outlines on and off
for a smoothed protected-only line.

### 2.2 P2 design / hypothesis: emphasis without proof for partly faint components and tiny weak islands

Code: `include/okuflow/common/uct_ink_outline.hpp` `Assemble`, lines 140–147, and the
"every central contour" policy.

With no plate, *not* painting a mark is safe: the original stays visible. *Painting*
one at full readable contrast is the only way to mislead. Three cases are not
covered:

- **Partly faint components.** `FaintInk` refuses a resolved cut-0 component only if
  it contains *no* central component. It accepts a component that contains any
  central component, even a tiny one with area under 4. Example: a faint fraction bar
  or minus sign with one darker chalk cluster is painted as a crisp dot, while the bar
  stays faint original. Or a `7` whose stem is faint: the painted top bar reads as
  `-`. Unmeasured, but it is the clearest way this design can change how a math
  symbol reads.
- **Threshold-unstable tiny islands** (under 4 px²) are painted at full contrast even
  though they are excluded from every stability check. On a chalk board, a texture
  speck near a stroke can look like a decimal point, a multiplication dot, a prime or
  an i-dot. Between live jobs it can also flicker on and off.
- **The resolved threshold is absolute (4 px²), not relative to scale.** At small
  writing sizes (x-height 12–16 analysis px), real dots are about 3–8 px², so they
  sit on either side of the guard. The probe data shows a wide margin on these
  inputs: noise holes and islands are 1–2 px², and the smallest real component is
  47 px². That margin is not general.

**Fix options (owner policy).** Keep every tiny *hole*: filling one would cover real
background. Only *paint* a tiny island if it is strong, meaning it survives at
`cuts[3]` (contains a contour at the strict level). Islands left unpainted remain
visible as original ink, so nothing is deleted or filled. Make `FaintInk` require a
*resolved* central component (`center.area >= 4`). Add a per-component guard: the
contained central components' union bounding box must reach the cut-0 component's
extents within, for example, `max(2 px, 0.15·extent)`. That catches the
truncated-stem case. This conflicts with agents.md "Preserve every central contour,
including tiny dots", so it needs the owner's decision. My recommendation is to
reword that rule to "never fill holes and never hide source pixels; paint only
proven contours".

### 2.3 P3: hole-unaware containment refuses nested content (false refusal, safe)

`Contains(a, b)` tests b's first vertex against a's outer polygon only. In the
loose→strict component check, a resolved loose component that *encloses* another
component therefore counts that one too. Examples: a circled answer, ⊙, a
parenthesised fraction drawn as a ring, a dot inside an `o`. The check gets
`contained == 2` and refuses with `unstable_components`. In the strict-hole→loose-hole
check, a ring around any glyph that has a hole (0, 4, 6, 8, 9, a, b, d, e, o, p, q)
refuses with `unstable_holes`. Circling answers is common on boards. Fix: give each
strict outer an owner, namely the *smallest-area* loose outer that contains it. The
stroke-width code already does this for holes. Then require each resolved loose
outer to own exactly one resolved strict outer, and do the same for holes. Add a
"circled 8" fixture, plus "dot in o".

### 2.4 P3: the optional outline stage can fail the whole text job and miscounts cancellation

In the worker (about lines 2445–2480), `PrepareResidentInkOutlines` returning false
sets `ok=false` and `inputFault=true`, and clears all lines and text. On
cancellation, `PrepareInkOutlines` returns false and `cancelled` is not set. This
matches the existing `PrepareResidentAnalysis` pattern, and cancelled jobs are
normally non-current and dropped, so the impact is low. Still, a 2 s `Finish` timeout
or a non-sticky copy failure in a purely optional visual stage discards correct OCR
text. It also feeds `consecutiveInputFailures_`, which can mark analysis unhealthy.
Fix: on `cancel`, set `output.cancelled = true` and leave `inputFault` unset. On any
other outline failure, clear only `inkOutlines`, record the error in diagnostics and
keep the text. A sticky CUDA error will fail the next resident stage anyway. The CPU
path already just `break`s on cancel.

Minor: `gpu::InkOutline` can throw `std::invalid_argument` after `WaitValidationReady`
has queued work. The job `catch` handles it, but the context is neither quarantined
nor has its `owners` cleared. It is currently unreachable, because `SubmitDevice`
enforces viewport equal to device size and `PlanInkOutlines` pre-validates. Wrapping
the call or quarantining on throw would keep the retained-cleanup contract true.

### 2.5 P3: diagnostics do not record planner skips; `Refusal::Bounds` is unreachable in production

`PlanInkOutlines` silently `continue`s on oversize, invalid, overlapping or
job-pixel-cap spans. `ink::Build`'s `Bounds` refusal can never be reached from the
worker, so `sample.json` cannot tell "oversize, left original" apart from "no
protected span". The doc says refusals distinguish bounds. Fix: emit a geometry-less
`Outline{refusal=Bounds}` record, with a distinct reason for overlap or cap, for
every eligible protected span the planner skipped. It is cheap, and it makes "refusal
reported as refusal" true for the most likely real-world refusal.

### 2.6 P3 coverage gaps (safe; original ink stays)

- **No margin.** Regions are the protected envelope rounded *inward*
  (`ceil(left)`, `floor(right)`). Ink touching the envelope edge becomes
  `border_ink`, which is the source-0 second region. One bounded expansion, as in
  the design (for example 2 px or 0.15·h), clamped to the image, would help. It
  would need `SceneInkOutlines`' `mask.contains(outline.bounds)` to test against the
  unexpanded mask, plus the glyph-collision rule.
- **Glyph collision is all-or-nothing.** Any glyph-eligible word rect that intersects
  the outline bounds suppresses the whole region. This includes `K=` drawn as a glyph
  next to a protected `½mv²`, as in the owner's live screenshot, if their envelopes
  overlap. A follow-up could split the region by central components and drop only
  the components whose bounds intersect a glyph rect.
- **Loop cap counts tiny loops.** `Trace` refuses above 256 loops, tiny pinholes
  included. Heavily textured chalk could hit `contour_limit` on texture alone.
  Unmeasured.
- Recognizer-level `Candidate` words that `PrepareLines` later downgrades (empty text
  becomes `kUnrecognized`) are never planned. This is minor.

## 3. Is the resolved-area guard plus ink-only painting sound?

**Mostly yes, with §2.2 as the caveat.**

- **Nesting holds by construction.** Superlevel sets are monotone. Interpolated
  crossings are monotone in level. The cut spacing is at least 6 (span ≥ 64,
  delta ≥ span/10), so clamping cannot make two levels share a crossing. The saddle
  rule (8-connected ink) is the same at every level. So contours at different levels
  never cross, and testing a single first vertex is valid for containment.
- **The correspondence is a bijection on resolved features.** The checks are: equal
  resolved counts at 1/2/3, each resolved loose outer containing exactly one resolved
  strict outer, and each resolved strict hole containing exactly one resolved loose
  hole. Strict regions are subsets of loose ones, and a strict outer's container has
  area at least its own, so it is resolved. Merging, splitting, vanishing and newly
  appearing resolved components all refuse. So do holes that open or close. The only
  tolerance is a resolved feature gaining or losing a sub-4 px² fragment, which is
  the intent. Over-counting with nested shapes only causes false refusals (§2.3).
- **What is not proven.** Tiny islands are painted without a stability proof, and
  emphasis of partly faint components is unbalanced (§2.2). Neither invents text,
  and no pixel is hidden, because there is no plate and holes are never filled. But
  both can change which marks look dominant.
- **Real evidence supports it** (`probe-ink-hole-areas.jsonl`). Source 0 had 1 px²
  holes at levels 69/93/116 and none at 139. Source 1 had 1–2 px² islands at 76 and
  1–2 px² holes at 127/152. Every real component was 47–475 px², and both inputs held
  9 resolved components at every level. Exact topology would have refused both
  inputs, as root observed. The 4 px² guard sits more than an order of magnitude from
  both populations here.

## 4. Verified as correct (by reading)

- **Marching squares.** Corners are ordered TL,TR,BR,BL on CPU and GPU. Every one of
  the 16 cases keeps the background on the same side, saddles included. Shared edges
  give bit-identical crossing points from both cells. There is one outgoing segment
  per crossing, so `Trace`'s unique-start map is valid. The odd doubled isovalue
  never equals a sample. A border at or below `cuts[0]` guarantees closed loops at
  every level.
- **Integer CPU/CUDA parity.** Same `Coverage` (int64, rounded), same `Thresholds`
  (int), same `Cell`. Histograms are order-independent counts. GPU output order is
  row prefix plus within-row exclusive scan, per level, which equals CPU's (y, x)
  emission order per level. So segment arrays, loops and paths are exactly equal,
  not merely within tolerance.
- **Kernel bounds.** 512 threads cover at most 511 cells. `inkSegments` returns
  without writing when `counts[k] > MaximumSegments`. The `invalid == 1` early return
  is block-uniform. Atomics only touch `border` and `invalid`. The scratch layout is
  about 1.6 MB of the 4 MB worker, checked before launch. Host unpacking matches the
  skip logic for over-cap levels.
- **Retained cleanup.** `owners` is set before the validation wait and before each
  region. `Finish` and `CopyResult` quarantine on error, timeout or cancel, and
  `CopyResult` waits on the stream before the host buffer is reused.
- **Freshness and visibility.** `SceneInkOutlines` requires `reference.valid` and
  `IsReadable`. `NextReadabilityExpiry` already includes these lines, so the expiry
  timer withdraws them. A smoothed line stays `valid=false` until a probe passes, and
  a failed check withdraws it (tested). Every `RefreshOverlay` suppression branch
  (`!enabled_`, Show original, unhealthy, composite pause, media suppression,
  settling, presentation unavailable) calls `ClearWords()`, which now also clears
  outlines, and returns before `SetInkOutlines`. `SetInkOutlines` equality on
  geometry pointers means repeated refreshes are cheap.
- **Immutable frames.** Outlines persist on the exact frame and vanish on the next
  `frameSeq` (tested). `immutableConflictRects_` and protected-mask containment are
  enforced. Retention (`candidate.line = old`) keeps the old outline together with
  its old reference and proof. That is consistent, but a retained line never picks up
  a newer outline while it stays jitter-stable.
- **Painter and snapshots.** The ink pass runs after plates and before glyphs. Its
  clip is `visibleClip`, not `paintClip`, which is intended. Antialiasing is on.
  `SetDisplayColors` refreshes outline colours. `CaptureSnapshot` includes outlines
  and, like glyph exports, ignores chrome. `PaintedInkOutlineRects` is a diagnostic
  only.

## 5. Optimisation

- **Transfer worst case.** Up to 4 × 8192 × 16 B = 512 KB per region and about 4 MB
  per job (8 regions). That is twice the region's BGRA pixels. It is still "contours
  only", but the doc should state the bound. Cheap reductions:
  (a) if any level's count exceeds the cap, skip *all* copies, because refusal is
  certain;
  (b) pack segments as cell index (17 bits) + entry/exit edge (2 + 2 bits) + two
  8-bit `t` values, 4–5 B instead of 16, and decode on the host;
  (c) batch all regions as kernels, then one summary copy and one sync, then one
  segment copy and one sync. That is 2 syncs per job instead of 2 per region.
  The measured cost is already 1.1–2.7 ms with 91 KB/70 KB, so none of this is
  urgent.
- **CPU `Assemble`.** `Contains` recomputes `boundingRect()` (O(V)) on every call.
  Precompute one bounding rect per contour. The worst case stays bounded (a few
  million point operations) because segments per level are capped.
- **`SceneInkOutlines`** runs on every refresh, at O(outlines × lines × words) rect
  tests. That is negligible at the caps; the cheap win is hoisting `IsShown(other)`
  out of the per-outline loop.
- **CPU `Build`.** `convertToFormat(ARGB32)` on the whole image is a shallow copy
  only because submit already normalises to ARGB32. A region-local conversion would
  make it robust.

## 6. Test coverage gaps

1. `PrepareResidentInkOutlines` itself is not contract-tested. The CUDA parity test
   calls `gpu::InkOutline` directly and skips host staging and offsets, multiple
   regions, the over-cap skip path, `inkOutlineBytes`, cancellation and quarantine.
   Only the private diagnostic exercises it.
2. CUDA parity has no noisy-saddle, many-loop or over-cap (`contour_limit`) cases.
   The CPU noisy-saddle seeds would be ideal parity inputs.
3. Missing fixtures: nested content (§2.3), a partly faint stroke and a weak tiny
   speck (§2.2), and a subpixel-registration check (a known 0.5-coverage edge lands
   within about 0.1 view px at 8× zoom). The current zoom test samples a pixel deep
   inside a rectangle.
4. Model paths not yet tested: smoothed eviction and `ReadingText` (§2.1), glyph
   collision suppression, `immutableConflictRects_` suppression, and an outline
   outside every protected mask.
5. The doc says the model test "exercises real preparation". It injects the
   `Outline` into `RecognizedLine` directly. The worker sequence
   `FinalizeRecognizedGeometry → PlanInkOutlines → Build` (CPU) is covered only by
   the private diagnostic. Either soften the wording or add a scheduler-level CPU
   test.

## 7. Documentation: overclaims and wording

- `docs/uct_ink_outlines.md` "never contributes to … reading-pane content" and
  agents.md "Keep … readable … text unchanged" are false for smoothed live input
  until §2.1 is fixed.
- README "preserving its original shape" and CHANGELOG "preserving stacked
  fractions": the painted shape is the *mid-contrast iso-contour*. In both
  `uct.png` crops the strokes are visibly thinner than the blurred originals. They
  are crisper and higher-contrast, but lighter in weight. Suggested wording:
  "traces the source shape at its mid-contrast edge; blurred strokes can look
  thinner". Also say that the glyph stroke-weight preference does not apply to
  outlines. That matters for a low-vision user who has raised it.
- "Refusals distinguish bounds …": the planner skips oversize spans silently (§2.5).
- "Only capped contour segments": correct. Add the numeric worst case (§5).
- These are accurate as written: the four-source-pixel structural limit, "nothing
  fills those holes", no plate, "not a math OCR engine", the CPU/CUDA arithmetic
  statement, smoothed results hidden until checked, chrome exclusion versus
  snapshots, and the evidence paragraph (one drawn region and eight painted words
  per source, one `border_ink` refusal on the original, `Fdcos` unchanged).

## 8. Evidence checked (no execution)

- `…/uct-resident-20261026-184914-…/paint/block-0-source-{0,1}/sample.json`:
  - Source 0: `sketchedRegions 1`, `paintedWords 8`, `admittedWords 8`, outline
    `[290,302,343,133]` drawn, components `[9,9,9,9]`, holes `[0,0,0,0]`, 1,414
    vertices, 91,020 B. A second region `[1213,488,23,31]` was refused `border_ink`
    with 44 B (summary only). Outline stage 2.70 ms in this run.
  - Source 1: `[220,229,266,107]` drawn, 9 components, 1,086 vertices, 69,868 B,
    1.13 ms.
  - Reading text in both still has `…` for the equation line and the existing
    `W=Fdcost` misread. These are not smoothed inputs, so they do not exercise §2.1.
- Crops of `uct.png` against `original.png` show 1 over bar over 2, `m`, `v` and the
  separate small raised 2 as crisp source shapes on an untouched board texture, with
  no plate. `K` and `=` are traced too, because they are inside the same protected
  envelope. The strokes are thinner than the originals (§7).
- `abba-analysis.json`: 40 timed jobs, off/on/on/off, with outline bytes 0 in the
  off visits. This matches root's summary. I did not recompute the medians.
- `integration-cpu-r4-gate.log`: SCOPED PASS 5/339. `integration-cuda-r3-gate.log`:
  SCOPED PASS 3/345. Both say PARTIAL. `final-background-matrix.log` (22:01) ends
  with "PASS: CPU test suite" and "PASS: CUDA test suite", but marks desktop/GUI
  tests as excluded (PARTIAL VALIDATION). I could not attribute it to the final HEAD
  from the log. I saw no PCH-OFF result. **No acceptance is claimed here.** No
  native or promotional capture exists, so there is no hardware or native-UI claim.

## 9. Suggested order for root

1. §2.1: give outline-only smoothed lines their own EMA-versus-current registration,
   remove the bypass, and add the eviction and `ReadingText` tests. This is small and
   self-contained.
2. §2.4: classify cancellation as cancelled, and make an outline-stage failure drop
   outlines only.
3. §2.5: add planner-skip receipts. Fix the docs wording in §7.
4. §2.3: switch to smallest-owner containment (small) and add the nested fixtures.
5. §2.2: an owner decision on painting only strong tiny islands and on the
   partly-faint guard. If accepted it is a small `Assemble` change plus fixtures. It
   may need a fresh correctness export, but not a new review unless the policy
   changes further.
6. §5 and §6 optimisations and coverage, as convenient.
