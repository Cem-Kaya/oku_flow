# 26 — Virtual Tripod: making the lock actually hold

Status: **PHASES A AND B IMPLEMENTED BUT DEFECTIVE — two reproducible bugs
found 2026-07-26, see "Post-implementation defect report" below. Fix before any
further phase work.** Written 2026-07-25
after reading the shipped implementation against plan 25's research and the
`ref/` checkouts. Phase A landed the prediction-seeded tracker, zoom-aware
absolute fit, display-pixel deadband, strength-controlled response, and
ratchet-free confidence hold. Phase B landed the real Gaussian pyramid,
prepared inverse-compositional translation tracker, adaptive subpixel
reference corners, sharp-frame selection, and quality-gated aligned reference
accumulation. The Phase C keyframe map/reacquisition ladder and Phase D NVOFA
fixed-reference/single-resample work remain unimplemented.

Owner's report: *"the virtual tripod does not feel strong enough."*

Implementation checkpoint (2026-07-26): CUDA tests recover a seeded
100-source-pixel translation and select the visible 4x zoom region over a
differently moving full-frame majority. The Phase B prepared-reference path
replays a known 0.7-3 Hz clamp vibration, desk impact, brightness change, and
foreground occlusion at 4x zoom: 97.33% RMS attenuation with 0.36
display-pixel P95 on the reference RTX 4090 Laptop GPU. Sharp-frame selection
and moving-pixel rejection in the multi-frame reference builder are covered
directly. Runtime camera validation is still required before Phase C.

Recorded-camera checkpoint (2026-07-26): the saved 1280x720 clamp recording
was decoded to BGRA and replayed through the production Phase B CUDA reference
selection, prepared tracker, absolute estimator, and final warp. An independent
fixed-reference KLT/robust-affine measurement found that the accumulated
four-frame reference reduced 0.5-8 Hz vertical translation by only 3% and
increased horizontal translation. Replaying the same frames with the sharp
selected keyframe and no accumulation reduced the main translation/rotation
bands by 24-38%. Production accumulation is therefore disabled until it can
reject a misregistered reference before replacing the selected sharp frame.
The synthetic comparison remains useful as a deterministic geometry regression
but is not an end-to-end quality claim for real footage.

---

## The headline

**The Virtual Tripod's architecture is right and its implementation throws away
almost all of the benefit.** Freezing the reference removed the random walk —
that part worked. But the registration that runs against that frozen reference
is still the frame-to-frame tracker, unchanged, and a frame-to-frame tracker is
built on an assumption the tripod deliberately violates: *that the two images
being compared are almost identical.*

Against a frozen reference they are not. The offset from the reference grows
without bound, and the tracker is re-solved from zero every frame. So the
tripod gets weaker the longer it holds, fails within seconds of real drift,
freezes for three seconds, then silently re-anchors and starts again. On top of
that, the absolute measurement it does produce is fed to the display with **no
filtering at all**, so every bit of estimator noise is visible — magnified by
the zoom factor.

Six specific defects follow. The first three explain the owner's report on
their own.

---

## Finding 1 — The tracker starts from zero every frame, so the problem it must
solve grows without bound

`TrackFeaturePyramidal` (`src/cuda/cuda_kernels.cu:1165-1243`) initialises its
displacement estimate to zero on entry:

```cpp
displacementX = 0.0f;
displacementY = 0.0f;
```

and nothing is ever passed in to seed it. In frame-to-frame mode this is
correct and free: consecutive frames differ by a fraction of a pixel, so zero is
an excellent starting guess. **In tripod mode it is the wrong guess by exactly
the accumulated offset from the reference**, and that offset only grows.

At the same time the model being solved is a local linearisation with a hard
per-iteration step clamp (`:1229-1231`) and a 21x21 window (`radius = 10`,
`:1180`). The tracker therefore has a hard capture ceiling of about **10
analysis pixels** — beyond that the template window and the target window stop
overlapping and there is no signal left to solve with.

The owner's own measurement in plan 25 is ~89 px of drift over 3.4 s at 720p,
i.e. roughly 26 source px/s. The analysis surface is 2x downsampled, so that is
~13 analysis px/s. **The reference leaves the tracker's capture range in well
under a second of sustained drift.**

The acceptance gate meanwhile allows displacements up to 64 analysis px
(`LaunchVirtualTripodFeaturePairs`, `src/cuda/cuda_kernels.cu:2269`) — a range
the solver cannot reach. The gate and the solver disagree by 6x, so the failure
is silent rather than reported.

**Fix (T1).** Pass the last accepted similarity transform into the feature
kernel and initialise each patch's LK solve at its predicted position instead of
zero. The per-frame residual then returns to the fraction-of-a-pixel regime the
solver was designed for, *regardless of how far the rig has drifted from the
reference.* This is the single highest-value change in this document: it is
small, local, and it converts the tripod from "holds for a few seconds" to
"holds indefinitely".

## Finding 2 — The pseudo-pyramid does not actually extend capture range

The three-level loop at `src/cuda/cuda_kernels.cu:1178-1238` subsamples the
same image at 4/2/1-pixel steps, with a comment describing it as "a three-level
sparse LK solve without materializing image pyramids". Two things break it:

1. **No low-pass filter before subsampling.** Sampling a sharp image on a
   4-pixel lattice aliases; the coarse "level" is not a coarse-scale image, it
   is an undersampled fine-scale one. Board text and slide edges are exactly the
   high-frequency content that aliases worst.
2. **The gradient stencil stays at ±1 pixel at every level** (`:1200-1209`).
   The linearisation is therefore always fine-scale, valid only for
   sub-pixel displacement, no matter which "level" is running. A coarse level
   whose gradients are fine-scale buys nothing.

So the levels cost 3x the work of a single-level solve and deliver
approximately the capture range of a single-level solve.

**Fix (T2).** Either build a real pyramid — Gaussian blur, then subsample, with
the gradient stencil scaled to the level — or delete the fake levels and spend
the budget on iterations. For the tripod the real pyramid is close to free (see
Finding 3): the reference is frozen, so its pyramid is built **once at lock
time**. Only the current frame needs per-frame pyramid construction, which is
two small separable blurs on a 640x360 surface.

With T1 in place, deep pyramids matter much less for steady-state tracking —
they matter for *re-acquisition* after a bump, which is exactly where the
current design fails hardest.

## Finding 3 — The frozen reference is re-analysed from scratch 30 times a second

`StabilizationFeaturePairsKernel` runs Harris corner detection over
`previousLuma` every frame (`src/cuda/cuda_kernels.cu:1272-1296`). In tripod
mode `previousLuma` **is the frozen reference** — the same pixels, producing the
same corners, recomputed 30 times a second for the entire session.

This is not primarily a performance complaint. It is a missed opportunity, and
it is the structural asymmetry the current implementation never exploits:

> **The reference never changes, so any amount of work spent on it is amortised
> to zero.** A frame-to-frame tracker cannot afford expensive template
> preprocessing because the template changes every frame. A tripod can afford
> almost anything.

What that buys, all computed once at lock:

- **Adaptive corner selection.** The current gate is an absolute magic number,
  `scores[0] > 2.0e5f` (`:1310`), on unnormalised 0-255 luma gradients. On a
  dim lecture hall or a low-contrast whiteboard a large fraction of cells
  produce no feature at all, which starves the model exactly when it is hardest.
  Replace with top-K by score over the whole reference, so the point count is
  stable across lighting.
- **Sub-pixel corner refinement** on the reference (parabola fit on the Harris
  response). Every reference point's position error is a permanent bias in the
  fit; refining once removes it for the whole session.
- **Inverse-compositional LK** (Baker & Matthews, *Lucas-Kanade 20 Years On*).
  Because the template is fixed, the Hessian and the steepest-descent images are
  constant and can be precomputed and inverted once. Per-frame cost drops to
  warping the current patch and one matrix-vector product. The same time budget
  then buys far more points, more iterations, or a larger window — i.e. real
  precision.
- **A denser, better-distributed point set.** One corner per 16x16 cell caps the
  model at ~880 points on a 640x360 surface, against a pair budget of 4096. With
  the template preprocessing amortised, 3-4k points is affordable, and more
  points is directly less noise in the fit.

**Fix (T3).** Split the kernel into a one-shot `PrepareTripodReference` (corners,
sub-pixel refinement, gradients, per-patch inverse Hessians, pyramid) and a
per-frame `TrackAgainstPreparedReference`.

## Finding 4 — The absolute measurement is sent to the display unfiltered

`UpdateVirtualTripodPath` (`src/cuda/cuda_kernels.cu:866-906`) sets

```cpp
state->correction = clamp(anchor - referenceMotion)
```

with the Kalman state explicitly zeroed (`state->velocity = {0,0,0,0}`,
`:894`). There is no smoothing, no deadband, no confidence weighting. Every
frame's estimation noise goes straight to the warp.

That noise is small in source pixels and large on screen. A residual of 0.15
analysis px is 0.3 source px is **1.2 display px at 4x zoom** — a visible
shimmer on text, present even when the scene is perfectly still. To a user
reading a board at high magnification, a view that shimmers does not read as
"stabilized", it reads as "not strong enough". This is very likely a large part
of the owner's subjective report, independent of the drift problems above.

The key insight the implementation misses:

> **Filtering an absolute measurement is free of drift.** The reason the
> frame-to-frame path had to filter cautiously is that its measurement is a
> *delta* — over-filtering a delta stream loses real motion permanently and
> biases the integrated path. An absolute measurement has no memory, so
> low-passing it is pure noise reduction. The tripod can filter as hard as the
> latency budget allows and lose nothing.

**Fix (T5).**

- A per-axis measurement filter on the absolute similarity, with measurement
  noise derived from the live RANSAC residual and inlier count (both already
  computed and stored in `state->diagnostics`). High-confidence frames update
  fast, low-confidence frames barely move the output.
- A **sub-pixel deadband** — ignore measured changes below ~0.15 source px —
  scaled with zoom so it is defined in *display* pixels, not source pixels. A
  static scene should produce a bit-identical output frame. Nothing else makes a
  view feel as locked as a view that is genuinely, perfectly still.
- Rate-limit the correction so a single bad frame cannot produce a visible jump.

## Finding 5 — Loss of lock costs three seconds of no stabilization, then a
silent ratchet

On a rejected model the path holds the last correction and counts the failure
(`src/cuda/cuda_kernels.cu:873-883`). The host auto-relocks only after **90
consecutive rejected frames** (`src/cuda/cuda_interop.cpp:1679-1682`) — three
seconds at 30 FPS. During those three seconds the correction is frozen while
the camera keeps shaking, so the user sees **completely unstabilized video**,
worse than having the feature off.

The re-lock then preserves the current correction as the new anchor. Combined
with Finding 1 — the reference goes out of range in about a second of drift —
the steady-state behaviour over a lecture is: lock, drift out of range, three
seconds unstabilized, re-anchor, repeat. Each re-anchor bakes the accumulated
error into the new anchor. **The tripod degrades into a slow ratchet with
periodic three-second dropouts**, which is a fair description of "not strong
enough".

The in-code comment defends the freeze on purity grounds ("a rejected absolute
model must freeze, never smuggle pairwise drift into the lock",
`src/cuda/cuda_interop.cpp:2634-2638`). The principle is right; the conclusion
is not. Drift-free-but-absent stabilization is worse for the user than
bounded-drift stabilization for a few hundred milliseconds.

**Fix (T7).** A two-tier state, the standard relocalisation structure from
visual odometry:

- **Absolute term** — the anchored correction, updated only from accepted
  fixed-reference models.
- **Relative term** — a bounded frame-to-frame correction that runs *only* while
  the absolute model is rejected, using the existing pairwise estimator. It is
  clamped to a small magnitude, decays toward zero, and is reset to zero the
  moment the absolute model re-acquires.

Output = absolute + relative. Stabilization never stops; the relative term
cannot accumulate because it is discarded on re-acquisition, not folded into
the anchor.

Alongside this: escalate re-acquisition instead of waiting. Frame 1-5, retry at
the predicted transform with a widened threshold; 5-15, retry through the full
reference pyramid; only past ~15 frames capture a new reference — and ease the
anchor to the new value over ~200 ms rather than substituting it.

## Finding 6 — The strength slider does nothing in Virtual Tripod mode

`LaunchVirtualTripodSimilarityEstimate` takes no strength parameter
(`src/cuda/cuda_interop.cpp:2549-2556`); `strength` is read at `:2495` and used
only in the frame-to-frame branch. The correction budget comes from zoom alone:

```cpp
maxCorrectionFraction = clamp(max(0.06f, zoomCropReserve), 0.06f, 0.45f);
```

Meanwhile the slider stays enabled and focusable in the UI
(`src/ui/main_window.cpp:522-526`, enabled with stabilization at `:1185`).

So the one control a user reaches for when stabilization feels weak is inert in
this mode. Whatever else is true, **a user who turns the strength up and sees no
change will correctly report that it is not strong enough.**

**Fix (T12).** Map strength in tripod mode to the things that genuinely trade
off against each other: deadband width, measurement-filter time constant,
anchor-bleed rate (T6), and correction budget. If a control cannot do anything
useful in a mode, disable it and say why in its accessible description — never
leave a live control that silently does nothing.

---

## Secondary defects worth fixing in the same pass

**S1 — Slow drift consumes the entire correction budget.** The correction is
absolute, so a rig that settles 40 px over ten minutes spends 40 px of budget
holding a *constant* pose offset, leaving that much less for shake. When the
clamp binds (`ClampStabilizationCorrection`, `src/cuda/cuda_kernels.cu:843-864`)
shake passes through at full amplitude and the tripod is effectively off — plan
25's Finding 2 failure mode, now reached by a slower road.

*Fix (T6): DC/AC split.* Let the anchor track the very-low-frequency component
of the measured pose — time constant on the order of 30 s, rate-limited to
about 1 display px/s, well below any perceptible motion. Real mechanical settling
is absorbed; the correction stays near zero mean; the budget serves shake. This
does **not** reintroduce the random walk, because the anchor is following an
*absolute* measurement, not integrating deltas — the distinction that makes the
whole tripod architecture work. Expose it as "Hold absolutely" versus "Let it
settle" if the owner wants the choice; default to letting it settle.

**S2 — A four-DOF model on a two-DOF problem.** A clamped phone vibrating
against a laptop translates; it does not meaningfully rotate or scale. Fitting
rotation and log-scale anyway (`:1773-1787`) gives noise two extra dimensions to
hide in, and rotation error is *worst at the frame corners* — at high zoom, off
to one side, that is precisely the region being displayed. A 2-DOF
translation-only fit from the same correspondences is materially more precise.

*Fix (T8):* default to translation-only; estimate rotation/scale continuously
but admit them only when supported by a strong, well-spread inlier set and
sustained over several frames (a genuine bump or re-mount, not per-frame noise).

**S3 — Registration ignores where the user is looking.** Plan 25's Finding 3 was
never implemented, and the tripod inherits it: one global fit weighted uniformly
across a frame of which the user sees a small fraction at high zoom.

*Fix (T9):* bias corner selection and inlier weighting toward the zoom ROI with
a full-frame fallback when the ROI lacks texture; at high zoom, register at
**full processed resolution inside the ROI** rather than on the 2x-downsampled
full frame. A 640x360 native-resolution crop costs exactly what the current
full-frame analysis surface costs, and doubles angular precision at 4x zoom.
Scale acceptance thresholds with zoom — they are currently in source pixels
while the error the user perceives is in display pixels.

**S4 — The hardware optical flow engine is idle in tripod mode.**
`nvidiaOpticalFlow_->Reset()` is called every tripod frame
(`src/cuda/cuda_interop.cpp:2558-2560`). NVOFA can register an arbitrary image
pair and accepts an external hint field, both of which are exactly what a
seeded, fixed-reference tripod wants — a dense absolute displacement field
(~14k vectors at a 4x4 grid, against ≤880 sparse points) with the predicted
transform as the hint. The current wrapper cannot express it: `Process()` takes
a single image and keeps its own previous frame
(`include/okuflow/cuda/nvidia_optical_flow.hpp:32-34`).

*Fix (T10):* extend the wrapper to accept an explicit (reference, current) pair
plus a hint field. Gate it behind validation against the sparse model, as the
existing estimator-selection logic already does. Lower priority than T1-T7 —
more correspondences will not rescue an unseeded tracker, but they will sharpen
a well-seeded one.

**S5 — Stabilization resamples the image twice and softens text.** The warp is a
full-frame bilinear resample (`src/cuda/cuda_interop.cpp:2682-2688`,
`src/cuda/cuda_kernels.cu:1804-1845`) and the zoom is a second bilinear resample
(`:2941`). Two chained bilinear resamples of a sub-pixel shift measurably soften
edges — on a text magnifier, that is a real cost paid *for turning stabilization
on*, and it partly cancels the benefit.

*Fix (T11):* compose the stabilization similarity into the zoom sampler so there
is exactly one resample from source to display, with a higher-order kernel
(Catmull-Rom) since it is now the only one. Warp only the visible region. This
is simultaneously a quality win, a bandwidth win, and it removes a full-frame
pass.

**S6 — The reference is whatever frame happened to arrive first.** Capture is
unconditional on the first frame after enabling (`:2519-2539`) — possibly
motion-blurred, possibly mid-shake, possibly mid-autofocus. Reference noise is a
**permanent** noise floor for the entire lock.

*Fix (T4):* capture over ~0.5 s. Score candidates by focus (variance of
Laplacian), reject the blurred ones, register the survivors to each other and
average. A multi-frame averaged reference has roughly `1/sqrt(N)` the sensor
noise, and every point tracked against it is correspondingly more precise, for
one-time cost.

**S7 — At low zoom the tripod warps with no crop reserve.** The budget floor is
6% but at 1x zoom there is no crop at all, so the warp samples past the frame
edge and `BilinearSample` clamps (`src/cuda/cuda_kernels.cu:83-105`), smearing
the border. Tripod should either force a small crop (~6%) when enabled below the
zoom level that supplies its own reserve, or scale its budget to the reserve
actually available.

**S8 — Dead work in the tripod path.** The frame-to-frame previous-luma copy
still runs every tripod frame (`src/cuda/cuda_interop.cpp:2673-2680`) even
though nothing consumes it. Harmless, but it should either be removed or
deliberately kept to feed the T7 relative fallback — which is the better answer.

---

## Buffering — which frames are worth keeping

Raised by the owner, 2026-07-25: *"shouldn't we keep more frame buffers?"*

Yes — but the useful buffers and the expensive one are not the same, and it is
worth being precise about which is which. Today the stabilizer keeps exactly
**two** analysis frames: one previous (`deviceStabLumaPrevious_`) and one
reference (`deviceStabLumaReference_`, `src/cuda/cuda_interop.cpp:901-906`).
That is the minimum the algorithm can run on.

Memory is not the constraint. A 640x360 float analysis frame is 0.9 MB; a
1280x720 RGBA processed frame is 3.7 MB. Eight of either is under 30 MB on a
card with gigabytes free. **Latency is the only real cost, and only one of the
four uses below incurs any.**

### B1 — Look-ahead (future frames): no for preview, yes for recording

This is the classic answer, and for the frame-to-frame architecture it is the
right one: buffering future frames is what lets an offline algorithm like
Grundmann's L1 optimal paths plan where a smooth camera path *should* go.

But it costs exactly what it buffers. At 30 FPS, four frames is 133 ms of added
latency between the world and the screen. For a magnifier that a low-vision user
watches while a lecturer points at a board — and sometimes while pointing at it
themselves — that delay is an accessibility regression, not a quality feature.

More to the point:

> **The Virtual Tripod does not need look-ahead at all.** Look-ahead exists to
> decide where a smooth camera path should go. A tripod already knows: nowhere.
> That is the deep reason the fixed-reference architecture suits this product —
> it buys offline-quality "hold perfectly still" behaviour at zero added
> latency, which no amount of trajectory smoothing can do.

Unchanged from plan 25, item 4: look-ahead belongs on the **recording** path,
where latency is free, and nowhere near the preview.

### B2 — A reference keyframe map instead of a single reference (recommended)

This is where more buffers genuinely help, and it is cheap. Keep a small set of
prepared references — 4 to 8 — captured at different times and appearances,
rather than one. Each frame registers against the best-scoring candidate.

What that fixes, directly:

- **Occlusion.** The lecturer stands in front of the board for ten seconds.
  Today that is a rejected model, a three-second dropout and a forced re-anchor
  (Finding 5). With a keyframe map, the frames that still match an older
  reference keep the lock alive.
- **Lighting changes.** Hall lights dim for the projector; the single reference
  becomes unmatchable in one step. A map that includes a dim-lit keyframe does
  not.
- **The ratchet.** The worst property of the current design is that every
  re-lock bakes accumulated error into the new anchor. If an *older* reference
  can still be matched, the anchor never has to move — the session's absolute
  frame of reference survives.
- **Recovery after a bump.** Re-acquisition can search several candidates rather
  than betting everything on the most recent one.

Cost: ~7 MB of luma plus the prepared per-reference data from T3, and one
scoring pass per frame to pick a candidate (cheap — it can reuse the previous
frame's winner and only re-score on rejection). Each reference is prepared once,
so T3's amortisation argument applies to all of them equally.

Retention policy: keep the original lock, plus new keyframes admitted only when
they register successfully against an existing one — that is what keeps every
keyframe in a **single common coordinate frame** and is what makes the map
drift-free rather than a chain of accumulated hops. Evict by age and by
redundancy, never the original.

### B3 — A short capture buffer at lock time (already T4)

Transient, held for about half a second while locking: collect ~8 frames, gate
them on focus, align and average. Costs no steady-state memory and no latency,
and removes sensor noise from the reference permanently. Already specified as
T4; noted here because it is the second "more buffers" answer and it is free.

### B4 — Aligned multi-frame accumulation for image quality (the real prize)

The pipeline already has temporal smoothing, but it is a single-history
exponential moving average (`TemporalSmoothKernel`,
`src/cuda/cuda_kernels.cu:450-490`) applied after zoom
(`src/cuda/cuda_interop.cpp:2980-2995`). An IIR with one history buffer has an
infinite tail: any residual misalignment smears forever, which is why it has to
be used conservatively.

A good tripod changes what is possible here, and the relationship runs both
ways:

- Temporal accumulation runs *after* the stabilization warp, so its alignment
  quality **is** the tripod's residual error. Every improvement in Findings 1-5
  makes accumulation safer.
- Once alignment is sub-pixel and known, an **N-frame aligned accumulator with
  per-pixel outlier rejection** beats an EMA outright: ~sqrt(N) noise reduction
  on the static parts of the scene, rejection instead of ghosting on the moving
  parts (the lecturer's hand), and — because the sub-pixel offsets between
  frames are known and non-zero — genuine multi-frame super-resolution on the
  static content.

For this product that last point matters more than it might elsewhere: the
static content is the **text on the board**, which is the entire purpose of the
application. Reading faint whiteboard pen at 4x zoom is exactly the case where
8 aligned frames beat 1 sharp one.

This is a larger piece of work than plan 26's Phases A-D and should be its own
plan once the tripod is holding. Recorded here because it is the strongest
argument for keeping more frames, and because it is only unlocked by accurate
absolute registration — it is a *reason to finish the tripod*, not a competitor
to it.

### B5 — Capture-side buffering (separate problem, already planned)

Not a stabilization question: the capture path keeps a single latest-frame slot,
which is correct for preview and lossy for recording. That is plan 20 section B
and remains independently necessary.

### Summary

| Buffer | Cost | Verdict |
|---|---|---|
| Look-ahead frames (preview) | 33 ms per frame of latency | **No** — and the tripod does not need it |
| Look-ahead frames (recording) | none | Yes, plan 25 item 4, unchanged |
| Reference keyframe map, 4-8 | ~7 MB, one scoring pass | **Yes — recommended, add as T14 in Phase C** |
| Lock-time capture window | transient | Yes, already T4 |
| N-frame aligned accumulator | ~30 MB | Yes, but as its own plan after the tripod holds |
| Recording queue | small | Yes, plan 20 section B |

## Proposed architecture: Tripod v2

Same principle as today — absolute registration against a frozen reference —
with the three structural pieces the current implementation lacks: a **seeded**
tracker, a **prepared** reference, and a **filtered** absolute output.

```
  ON LOCK (once)
    capture N frames -> focus-gate -> align -> average       (T4)
    build blurred pyramid of the reference                   (T2)
    detect top-K corners, sub-pixel refine                   (T3)
    precompute gradients + inverse Hessians (inv-comp LK)    (T3)

  PER FRAME
    predict transform from last accepted state               (T1)
    track prepared reference points, seeded at prediction    (T1/T3)
      -> if ROI has texture, restrict/weight to zoom ROI     (T9)
    RANSAC + refit, translation-only by default              (T8)
    accepted?
      yes -> measurement filter + deadband, confidence-weighted   (T5)
             anchor bleeds toward pose at ~1 display px/s          (T6)
             relative term := 0
      no  -> relative term := bounded frame-to-frame correction    (T7)
             escalating re-acquisition; new reference only past ~15 frames
    correction = absolute + relative
    compose correction into the zoom sampler, single resample      (T11)
```

### Phasing

**Phase A — one day, and it is most of the win.** T1 (seed the tracker), T5
(measurement filter + deadband), T7 relock threshold reduced from 90 frames to
~15 with an eased anchor, T12 (make the strength slider mean something). These
four are small, local, and directly attack the reported symptom.

**Phase B — IMPLEMENTED 2026-07-26.** T3 (prepare the reference;
inverse-compositional LK; adaptive distributed corners), T2 (real Gaussian
pyramid), T4 (quality-gated multi-frame reference), and T8
(translation-only default). This is where precision comes from.

**Phase C — two to three days.** T7 relative-fallback tier in full, T6 (anchor
bleed), T9 (ROI-weighted, full-resolution registration at high zoom), **T14
(reference keyframe map, B2)**, accessible lock diagnostics. T14 pairs naturally
with T7 here: the escalating re-acquisition ladder becomes "retry at prediction
→ retry against other keyframes → capture a new one", and the middle rung is
what stops the ratchet.

**Phase D — two to three days.** T10 (NVOFA fixed-reference dense flow with
hints), T11 (single composed resample).

Phase A alone should be validated with the owner before Phase B starts — if the
tripod still feels weak after seeding and filtering, the diagnosis above is
incomplete and the remaining phases should be re-planned rather than executed.

---

## Post-implementation defect report (2026-07-26) — why it still does not work

Phases A and B shipped and the CUDA gate reports 97.33% attenuation, but the
owner reports the tripod does not work on the real camera. **Two reproducible
defects were found, and the shipped test is structurally incapable of detecting
either.** Method: the estimator was driven directly on a GPU with the *real*
pipeline call sequence and realistic scene content, sweeping the conditions the
test holds fixed. Harness:
`<session scratchpad>/tripod_probe.cu` (built with
`build/agent_build_probe.bat`; the CUDA tests themselves needed a manual
configure because plan 24 §A is still open — no preset can build them).

Baseline first, so the numbers below are comparable: on a realistic lecture
board (flat whiteboard, sparse handwriting, sensor noise, 1280x720 downsampled
to the 640x360 analysis surface) the estimator is **excellent** — 99.4% AC
attenuation, 0.21 display-pixel P95 at 4x zoom, robust to drift, to 0.35
rolling-shutter shear, and to a 60-luma sustained exposure shift. *The
estimator is not the problem.* Note also that "attenuation" must be measured
after removing the mean: a tripod legitimately holds a constant offset (the
reference frame's own pose), and scoring that as error understates the result.

### Defect A — the clamped measurement is fed back as the tracker seed

`UpdateVirtualTripodPath` (`src/cuda/cuda_kernels.cu:887-893`) clamps the
measured motion and *then* stores it:

```cpp
referenceMotion.x = clamp(referenceMotion.x, -0.45f * fullWidth,  ...);
referenceMotion.y = clamp(referenceMotion.y, -0.45f * fullHeight, ...);
state->lastFrameMotion = referenceMotion;   // <-- clamped value
```

`lastFrameMotion` serves two unrelated purposes. As the **measurement** driving
the correction, clamping is right — you cannot correct beyond the crop budget.
But it is also the **seed** for next frame's tracker
(`PreparedVirtualTripodFeaturePairsKernel`,
`src/cuda/cuda_kernels.cu:1573-1580`), and there the clamp is fatal: once true
displacement passes `0.45 x frame`, the seed saturates and can never grow
again, so the tracker is permanently aimed at the wrong place.

Measured, gradual-drift sweep (analysis px offset → tracking error, source px):

| offset (src px) | pairs | inliers | verdict | error |
|---|---|---|---|---|
| 0 – 400 | 734 → 305 | 235 → 120 | OK | 5.3 (constant) |
| 420 | 260 | 90 | **OK** | **12.4** |
| 440 | 256 | 72 | **OK** | **28.2** |
| 460+ | 232 | 0 | REJECT | 46 → ∞ (frozen) |

The cliff lands exactly on `0.45 x 720 = 324`, the y clamp. Note rows 420 and
440: the model is **accepted while wrong by up to 28 source px — 112 display
pixels at 4x zoom** — and nothing reports it.

**Verified fix.** Storing the measurement *before* the clamp (one statement
moved) removes the cliff entirely — same sweep, patched kernel:

| offset (src px) | 420 | 440 | 460 | 480 |
|---|---|---|---|---|
| shipped, error | 12.4 | 28.2 | REJECT | REJECT |
| seed before clamp, error | 5.3 | 5.3 | 5.3 | 5.3 |

Tracking stays exact to the end of the sweep. This was confirmed on a patched
*scratch copy* of the kernel; no repo source was modified.

### Defect B — a bump of ~40 source pixels kills the lock permanently

This is the one the owner is hitting. Cold capture range — the displacement the
tracker can absorb in a single step, i.e. when the seed is stale because the rig
was *bumped* rather than drifting — is only about **40 source pixels, 3% of the
frame width at 720p**:

| jump (src px) | pairs | inliers | verdict |
|---|---|---|---|
| 20 | 727 | 235 | OK |
| 40 | 647 | 17 | OK (marginal) |
| 60 | 606 | **0** | **REJECT** |
| 80 – 140 | ~600 | **0** | **REJECT** |

The failure signature is diagnostic: **pair count stays high (~600) while
inliers go to exactly zero.** Every patch still tracks — to a different wrong
local minimum, because a zero-relative seed puts each 9x9 window on unrelated
texture — so RANSAC finds no consensus and rejects.

What makes it permanent rather than transient:

1. On rejection `UpdateVirtualTripodPath` returns early
   (`src/cuda/cuda_kernels.cu:875-885`) and deliberately does **not** update
   `lastFrameMotion` — so the seed stays at its *pre-bump* value forever.
2. The residual the tracker must solve is therefore the bump size, every
   subsequent frame, which is exactly the case that just failed.
3. The auto-relock that used to fire has been removed; the status text now says
   *"reference lost — holding locked view; use Re-lock"*. There is no
   re-acquisition ladder (Finding 5 / T7 is still unimplemented).

So the tripod holds beautifully until the first knock of the desk, the first
lid adjustment, the first time the clamp settles — then silently stops working
for the rest of the session, and the frozen correction makes the output *worse
than switching stabilization off*, because it warps by a stale constant while
the scene keeps moving. Over a simulated 60 s with sustained drift: **72% of
frames rejected, AC attenuation 15.8%**.

Gradual drift works (the seeded sweep tracks cleanly to 400 source px); it is
specifically the *step* that kills it. That is why the defect never appears in
casual testing and never appears in the test suite.

### Why the shipped test could not catch either

`tests/stabilization_cuda_tests.cu:397-500` reports 97.33% honestly — it just
tests a case that excludes both defects:

- **90 frames — 3 seconds.** Neither defect can appear in 3 seconds.
- **Zero drift and no step.** Motion is a ±3 analysis-pixel oscillation plus a
  decaying desk-impact ring under 8 analysis px — inside the ~20 px cold range,
  and seeded frame-to-frame anyway. Nothing ever approaches the 0.45 clamp.
- **It bypasses the multi-frame reference builder.** The test prepares the
  reference directly from a clean image (`:369-379`); the real pipeline always
  runs focus-selection ×5 → accumulate ×4 → finalize (`cuda_interop.cpp:2742-2804`).
  That path is exercised only by a separate, static assertion.
- **Inlier threshold 2.0 px** (`:445`) versus production's
  `max(0.35, factor/zoom)` = **0.5 px at 4x** (`cuda_interop.cpp:2702-2707`) —
  4x looser, so the test cannot see consensus loss.
- **A maximally textured synthetic pattern** with ~37 luma of gradient per pixel,
  so every 16x16 cell yields a strong corner and every LK solve is
  well-conditioned.

### Fixes, in order

1. **Seed before clamp** (Defect A). One statement moved; verified above.
   Separate the two roles explicitly — keep an unclamped `lastMeasuredMotion`
   for seeding and prediction, and clamp only what feeds `correction`.
2. **Re-acquisition ladder** (Defect B, and T7). On rejection, escalate rather
   than freeze: retry at the predicted transform; then a coarse search over the
   reference pyramid (level 2 alone covers ±4x the level-0 range); then, after
   ~15 frames, capture a new reference and ease the anchor to it over ~200 ms.
   The keyframe map (T14) is the durable version of this.
3. **Never freeze silently.** While the absolute model is rejected, run the
   bounded relative fallback (T7) so stabilization degrades instead of
   inverting. Freezing a stale warp is strictly worse than identity.
4. **Report accepted-but-degraded.** Inlier *ratio* and residual should gate
   acceptance, not just `inliers >= 12`; the 420–440 px rows above pass a
   12-inlier bar while being badly wrong. Surface it in the status line and to
   the screen reader.

### Test cases that must exist before this is called fixed

Every one of these is cheap, deterministic, and needs no camera:

- **Step response.** Lock, then jump 20/40/80/160/320 source px. Assert
  re-acquisition within N frames and no permanent rejection. *Currently fails
  at 60 px.*
- **Clamp crossing.** Drift past `0.45 x height` and assert tracking error stays
  flat. *Currently fails at 420 px.*
- **Duration.** ≥60 s, not 3 s, with bounded wander plus two bumps.
- **Through the real builder.** Drive the 5+4 focus/accumulate sequence, not a
  hand-prepared reference.
- **Production thresholds.** Use `max(0.35, factor/zoom)`, not 2.0.
- **Realistic content.** Flat board, sparse strokes, sensor noise — not a
  full-frame gradient pattern.
- **Accepted-but-wrong guard.** Assert the model is *rejected* whenever the
  recovered transform is off by more than the acceptance tolerance.

And the prerequisite from plan 24 §A still stands: `msvc-debug`/`msvc-release`
set `OKUFLOW_ENABLE_TESTS=OFF` and `msvc-cpu` sets `OKUFLOW_ENABLE_CUDA=OFF`,
so **no preset can build these tests**. Every run above needed a hand-written
configure.

---

## Acceptance criteria

Stated in **display** pixels at the active zoom, because that is what the user
perceives. Source-pixel thresholds are how the current implementation hides its
errors.

| Test | Criterion |
|---|---|
| Static hold, 60 s, 4x zoom, clamped rig | P95 output motion of a tracked point < 0.25 display px; no visible shimmer on text |
| Long hold, 10 min | Absolute offset drift < 1 display px; zero automatic re-locks |
| Slow drift, 100 px over 60 s | Holds without re-lock; with anchor bleed enabled the view stays centred |
| Desk tap | Recovery to < 0.5 display px within 300 ms, no visible jump |
| 50% occlusion for 3 s (lecturer at the board) | No dropout, no re-lock, no jump on clearing |
| Full occlusion for 10 s, then clear | Lock survives via the keyframe map (T14); anchor unchanged, no jump |
| Hall lights dimmed for a projector | Lock survives; at most one new keyframe admitted, anchor unchanged |
| Shake attenuation, 0.5-3 Hz band, 4x zoom | ≥ 90% peak-to-peak reduction (measured baseline: ~10.5%) |
| Sharpness | Text MTF with tripod on within 5% of tripod off (requires T11) |

### How to test without a camera

The tripod is unusually easy to test deterministically, and this should be
built before the fixes rather than after: take one static textured image, warp
it by a **known** path (sinusoid at 1 Hz plus a linear ramp, plus a step to
simulate a bump), feed the sequence through the estimator, and assert that the
recovered transform matches the injected one to sub-pixel tolerance and that the
residual output motion is below threshold. Every finding above has a synthetic
sequence that isolates it — a pure ramp exercises Finding 1, a pure sinusoid at
zero mean exercises Finding 4, a step exercises Finding 5.

This depends on **plan 24** landing first: the CUDA regression tests are
currently unreachable by any preset, which is why stabilization has regressed
repeatedly. Fixing that is a prerequisite, not a parallel task.

### Offline upper bound

Before or alongside Phase A, run `ref/vid.stab` in `--tripod` mode and
`ref/python_video_stab` over the owner's own clips. Neither is real-time and
neither ships, but they establish what a well-implemented fixed-reference
stabilizer achieves on this exact footage. If they hold the view far better than
Tripod v2 does, the remaining gap is implementation, not physics; if they do not,
the crop budget or the mounting is the real limit and the plan should change.

---

## What not to do

- **Do not raise the correction budget further.** It is already at 45%. A clamp
  that binds later is still a clamp (plan 25, Finding 2), and above ~45% the
  crop starts costing real magnification.
- **Do not add more Kalman tuning to the frame-to-frame path.** The tripod's
  whole point is that the absolute measurement makes path filtering unnecessary.
  Tuning the pairwise filter is the work plan 25 already showed cannot succeed.
- **Do not reach for learned or full-frame methods.** Same reasoning as plan 25:
  wrong tool, offline, large dependency, and a well-posed geometric problem
  should not need them.
- **Do not copy code from `ref/`.** Several checkouts are GPL-incompatible or
  unlicensed. Read for algorithms; write our own.

---

## Diagnostics the owner can actually use

The owner is blind/low-vision and cannot judge lock quality by looking at the
preview. The current status strings
(`src/cuda/cuda_interop.cpp:1667-1690`) are a good start but report source
pixels and are visual-only.

- Report residual and correction in **display** pixels at the current zoom.
- Announce lock-state transitions through the existing screen-reader
  announcement path — locked, weakened, re-acquired, budget exhausted. These are
  mode changes, so announcements are the correct mechanism; **not** TTS, which
  stays strictly user-triggered.
- Add a one-key "how is the lock doing?" report (residual, inliers, budget
  headroom, seconds since last re-lock) alongside the existing `L` shortcut.
- Keep the release-build terminal diagnostic sample until Phase A is accepted;
  add predicted-versus-measured displacement to it, which is the one number that
  proves T1 is working.

---

## Cross-references

- Plan 14, Tier 3 addendum — the approved Screen Lock mode this extends.
- Plan 25 — the research this builds on; Findings 2 (clamp vs constraint) and 3
  (ROI weighting) are inherited unfixed and are addressed here as S1 and S3.
- Plan 24 — prerequisite. The synthetic test harness above cannot run until the
  CUDA regression tests are reachable.
- `ref/vid.stab/src/motiondetect.c:267-270` — the four-line tripod reference
  freeze. Note that vid.stab is offline and re-solves generously; its capture
  range does not transfer for free to a real-time seeded tracker.
- `ref/gyroflow/src/core/smoothing/fixed.rs` — `Fixed camera` smoothing mode.
- Baker & Matthews, *Lucas-Kanade 20 Years On: A Unifying Framework* — the
  inverse-compositional formulation that makes a fixed template nearly free.
- NVIDIA Optical Flow SDK programming guide — arbitrary image-pair registration
  and external hint fields, both unused today.
