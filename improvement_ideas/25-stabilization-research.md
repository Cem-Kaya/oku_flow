# Plan 25 — Stabilization: What Everyone Else Does, and What We're Doing Wrong

Status: **research complete; fixed-reference Virtual Tripod implemented
(2026-07-25).** Reference checkouts live in the git-ignored `ref/` (see
`ref/README-openzoom.md` for the pinned commit table). No reference source,
binary, or asset is copied into OpenZoom or its release bundle.

Owner's report that prompted this: at maximum strength, explicit CUDA feature
tracking visibly helps while Automatic/RTX barely does, *and neither is close to
strong enough* for a heavily zoomed view. Measured evidence from the owner's own
recordings: dominant vibration around 0.67-1.2 Hz, roughly 89 px of slow
vertical drift over 3.4 s, and only ~10.5% peak-to-peak attenuation in the
processed output.

## The headline

**OpenZoom is running a general-purpose handheld-video algorithm on what is
actually the easiest case in the entire stabilization literature, and is
therefore leaving almost all of the available performance unused.**

Our scenario is not handheld cinematography. It is:

- a camera **rigidly clamped** to a laptop — the intended camera motion is
  *zero*, not "smooth and cinematic";
- pointed at a **static, planar, textured scene** (a board or slide);
- viewed at **heavy zoom**, so a large crop reserve already exists;
- disturbed only by **small vibration plus slow mechanical drift**.

## Implemented in the first correction

- Stabilization now owns a 640x360 analysis surface; keystone remains at
  320x180. At 720p this changes stabilization sampling from one value per 4x4
  source region to one per 2x2 region.
- Automatic evaluates the CUDA Harris/Lucas-Kanade similarity model first.
  NVOFA stays initialized and current, but its dense model is used only when
  the sparse model is rejected. Projection correlation remains the final
  compatibility fallback. The gate is device-side and adds no host wait.
- Near-lock enters progressively above 70% strength and becomes an exact
  mounted-camera hold at 98%, until crop authority is exhausted.
- Release builds temporarily retain a terminal. A bounded diagnostic sample
  prints every 30 camera frames: estimator, analysis size, inliers, residual,
  raw motion, correction, and GPU time.
- CUDA regression coverage uses the 640x360 dense-flow geometry and verifies
  that a valid sparse estimate gates NVOFA conversion.

> **Follow-up (2026-07-25):** the shipped Virtual Tripod holds far more weakly
> than this architecture should allow. The registration against the frozen
> reference is still the unmodified frame-to-frame tracker, which breaks the
> assumptions a fixed reference depends on.
> See [`26-virtual-tripod-strength.md`](26-virtual-tripod-strength.md) for the
> diagnosis and the fix; Findings 2 and 3 below are inherited there unfixed.

These changes address estimator selection, pre-estimation information loss,
and strength mapping. The second correction adds a transient `Virtual Tripod`
mode: it freezes a dedicated 640x360 reference, sends fixed-reference
correspondences through deterministic GPU RANSAC, and uses the resulting
similarity as an absolute correction rather than adding it to `actualPath`.
Rejected frames hold the last valid transform. A sustained confidence loss or
manual re-lock captures a new reference while preserving the current
correction as the anchor offset, so reacquisition cannot jump. This removes the
random-walk mechanism for mounted-camera use. Physical-screen quad/homography
anchoring across arbitrary content transitions remains a separate Screen Lock
extension.

Every one of those is a simplification that the current design does not exploit.
Three structural problems follow, in descending order of how much they explain
the owner's measurements.

---

## Finding 1 — The drift is structural, not a tuning failure

**The 89 px drift cannot be tuned away, because the estimator is built to
accumulate it.** OpenZoom copies the current analysis frame into the "previous"
buffer on *every* frame (`src/cuda/cuda_interop.cpp:2554-2560`) and integrates
pairwise motion into `actualPath`. Every frame's small estimation error is added
permanently to that running total. There is no absolute reference anywhere in
the system, so error integrates into a random walk — the textbook definition of
drift. No Kalman tuning, no correction budget, and no better optical-flow
estimator removes it; they only change how fast it accumulates.

**How vid.stab solves it — in four lines.** Its "virtual tripod" mode simply
*stops updating the reference frame* (`ref/vid.stab/src/motiondetect.c:267-270`):

```c
// for tripod we keep a certain reference frame
if (md->conf.virtualTripod < 1 || md->frameNum < md->conf.virtualTripod)
    // copy current frame (smoothed) to prev for next frame comparison
    vsFrameCopy(&md->prev, &md->curr, &md->fi);
```

After frame *N*, `prev` is frozen. Every subsequent frame is registered against
that **same fixed keyframe**, so the estimate is absolute rather than
incremental and drift is impossible by construction. Gyroflow exposes the same
concept as a first-class smoothing mode, `Fixed camera`
(`ref/gyroflow/src/core/smoothing/fixed.rs`).

**Why this fits OpenZoom perfectly:** the phone is clamped and the board does
not move, so a keyframe stays valid for minutes. This is precisely the
already-approved **Screen Lock** feature (plan 14, Tier 3 addendum) — the
research says it is not merely a nice extra mode, it is the *correct primary
algorithm* for our hardware setup.

Design sketch (for whoever implements it):

- Keep the current frame-to-frame path as the fallback for when the reference
  becomes invalid.
- Latch a reference keyframe when motion is small for ~1 s. Register each new
  frame to that keyframe (the existing NVOF/feature/RANSAC machinery works
  unchanged — only the *second* input buffer changes).
- Re-latch when inlier count or residual collapses (lecturer wipes the board,
  slide changes, someone bumps the rig, lighting changes). Re-latching must be
  smooth, not a jump: ease the correction to the new anchor.
- A drift-free hold means the correction can grow slowly to a large *constant*
  offset, which is exactly what the crop reserve is for — and unlike the current
  design, it stops growing once registered.

---

## Finding 2 — A clamp is not a constraint: we saturate where the field optimizes

OpenZoom smooths with a Kalman filter and then **clips** the resulting
correction to a fraction of the frame. When the desired correction exceeds the
budget, the clamp binds and the residual shake passes straight through to the
screen. That is exactly the failure mode the owner measured: a 6% limit
(43.2 px at 720p) saturating against an 89 px path, yielding ~10.5% attenuation.
Raising the limit (now up to 45%) helps, but it is the same architecture — it
just saturates later.

**The field's answer is to make the crop window a hard constraint inside the
optimization instead.** Grundmann et al. (CVPR 2011, the algorithm behind
YouTube's stabilizer) minimize a weighted sum of the first, second, and third
derivatives of the camera path, subject to two constraint families
(`ref/L1-optimal-paths-Stabilization/L1optimal_lpp.py:92-136`):

- **inclusion constraints** — the crop rectangle's corners must remain inside
  the warped frame, for every frame;
- **proximity constraints** — the transform must stay near-rigid (e.g.
  `0.9 ≤ a ≤ 1.1`), so the result never looks warped.

The objective produces paths built from *constant, linear, and parabolic
segments* — deliberately imitating a tripod hold, a dolly, and a smooth
acceleration. The constant segment is literally "hold perfectly still", which is
what we want almost all the time. OpenCV ships a production implementation as
`LpMotionStabilizer` with a trim ratio and four weights
(`ref/opencv_contrib/modules/videostab/include/opencv2/videostab/motion_stabilizing.hpp:117-146`).

The difference in behavior is the whole ballgame: **a constrained optimizer
spends the entire crop budget optimally and never gives up; a clamp uses the
budget until it runs out and then stops stabilizing.**

Caveat, stated honestly: the classic L1 formulation is an offline LP over a
whole clip, and we are a live magnifier where latency is a serious accessibility
concern. Two viable adaptations:

1. **Receding-horizon (MPC-style) LP** over a short future window. Costs that
   window in latency — at 30 FPS even 4 frames is 133 ms, which is probably too
   much for a live magnifier, so this is likely only acceptable for the
   *recording* path, not the preview path.
2. **Constraint-aware causal filtering** — keep the causal filter, but instead of
   clipping the output, feed the remaining crop budget back into the filter as
   state (i.e. stiffen the path as the budget is consumed, rather than hitting a
   wall). This preserves zero added latency and captures most of the benefit.
   **This is the recommended direction for the preview path.**

---

## Finding 3 — At high zoom we estimate and correct the wrong region

The estimator runs on a downsampled analysis image (recently raised to 640×360,
`ComputeStabilizationLumaDims`, `src/cuda/cuda_interop.cpp:139-145`, used at
`:885`) and fits **one global similarity over the whole frame**, weighting every
part of the image equally.

But at high zoom the user sees only a small window of that frame. At 4× zoom the
visible region is a quarter of each axis — about 160×90 of the analysis image —
while the fit is dominated by the ~94% of the image the user cannot see. Worse,
any residual error in that global fit is **magnified by the zoom factor on
screen**: 1 px of residual at 720p becomes 4 displayed px at 4× zoom. The owner's
"it is still not strong enough when zoomed in a lot" is consistent with exactly
this: the estimate is not wrong so much as it is *not optimized for the part
being magnified*.

Two mitigations, cheap to try in order:

- **Weight the fit toward the visible ROI.** Feed the RANSAC/least-squares refit
  a sample distribution biased toward the zoom window (or restrict inliers to
  it, with a fallback to full-frame when the ROI lacks texture). The pair budget
  is already fixed at 4096; spending those samples where the user is looking
  costs nothing extra.
- **Scale precision targets with zoom.** Acceptance thresholds, inlier radius,
  and the residual tolerance are currently expressed in source pixels; at 4×
  zoom they should tighten proportionally, because the display magnifies the
  error.

MeshFlow (`ref/Mesh-Flow-Video-Stabilization`) generalizes this properly — a
grid of local camera paths instead of one global model — but for a planar board
viewed head-on, ROI weighting should capture most of the benefit at a fraction
of the complexity.

---

## Secondary observations

- **Rolling shutter:** gyroflow is the reference-quality open-source
  implementation and is worth reading before further tuning of our per-scanline
  offset, especially since a phone camera's readout is a real, measurable
  parameter rather than the fixed 0.35 fraction we assume
  (`kStabRollingShutterReadoutFraction`).
- **Full-frame / crop-free methods** (FuSta, DIFRINT) synthesize the missing
  borders instead of cropping. **Not worth pursuing for us** — they are offline
  and expensive, and when the user is zoomed in there is no border problem to
  solve. Cloned for completeness only.
- **Learned methods** (DUT, StabNet, deep-online-video-stabilization) target
  handheld footage and would add a large runtime dependency for a case where a
  well-posed geometric method should win outright. Read for ideas about
  trajectory smoothing objectives, not as a candidate architecture.
- **Sanity baseline:** `python_video_stab` (MIT, tiny) can process the owner's
  recorded clips offline. If a 200-line Python KLT baseline stabilizes those
  clips visibly better than our GPU pipeline, that is decisive evidence the
  problem is our estimator/filter design rather than anything fundamental —
  a very cheap experiment with a clear verdict either way.

## Recommended order of work

1. **Reference-frame (tripod/Screen Lock) registration.** Highest impact,
   directly eliminates the measured drift, and our hardware setup is the ideal
   case for it. Ship it as the automatic behavior for a mounted camera, not
   just as a manual mode.
2. **ROI-weighted estimation at high zoom.** Small, targeted at the owner's
   actual complaint.
3. **Constraint-aware causal filtering** to replace clip-and-saturate.
4. Offline L1/LP path optimization for the *recording* path only, where latency
   is free.
5. Rolling-shutter parameter measurement informed by gyroflow.

## Sources

- Grundmann, Kwatra, Essa, "Auto-Directed Video Stabilization with Robust L1
  Optimal Camera Paths", CVPR 2011 —
  https://research.google/pubs/auto-directed-video-stabilization-with-robust-l1-optimal-camera-paths/
  (PDF: https://research.google.com/pubs/archive/37041.pdf)
- Liu et al., "MeshFlow: Minimum Latency Online Video Stabilization", ECCV 2016
- Liu et al., "Hybrid Neural Fusion for Full-frame Video Stabilization"
  (FuSta), ICCV 2021 — https://alex04072000.github.io/FuSta/ ,
  https://arxiv.org/pdf/2102.06205
- Xu et al., "DUT: Learning Video Stabilization by Simply Watching Unstable
  Videos" — https://github.com/Annbless/DUTCode
- vid.stab (Georg Martius) — https://github.com/georgmartius/vid.stab
- Gyroflow — https://github.com/gyroflow/gyroflow
- OpenCV `videostab` module — https://github.com/opencv/opencv_contrib
- NVIDIA Optical Flow SDK programming guide —
  https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html
- NVIDIA VPI (alternative GPU CV stack, KLT + optical flow backends) —
  https://docs.nvidia.com/vpi/
