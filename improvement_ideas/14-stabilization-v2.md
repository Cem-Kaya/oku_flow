# Plan 14 — Stabilization v2 (Tier 1 + Tier 2) — handoff spec (2026-07-23)

## Implementation status (2026-07-23)

Tier 1 and Tier 2 are implemented on
`agent/stabilization-v2-rtx-optical-flow`. The automatic device-resident chain
is NVIDIA Optical Flow Accelerator → CUDA Harris/Lucas-Kanade features →
projection fallback, with GPU RANSAC similarity fitting, constant-velocity
Kalman path filtering, affine correction, default-on rolling-shutter
interpolation, per-profile controls, and sampled active-engine diagnostics.

Automated validation is green: the release app builds; a CUDA test recovers a
known translation/rotation/scale model despite foreground outliers and verifies
lost-track decay; and a direct `nvofapi64.dll` test on the reference RTX 4090
Laptop GPU returns the expected `(3, 0)` flow at grid size 4. Camera-specific
tremor, deliberate-pan, foreground, jello, and keystone interaction checks
below remain manual acceptance work. Tier 3 remains a separate future mode.

The 2026-07-24 live tremor check exposed a rolling-shutter composition defect:
the first implementation blended the whole previous correction into the top
of the current frame, canceling much of an alternating shake. The warp now
keeps the current global correction at the center scanline and applies only a
bounded readout-time offset above/below it. A CUDA image-warp regression test
locks that behavior, and sampled diagnostics now include inliers plus actual
translation/rotation correction for manual verification.

Owner-approved scope: Tier 1 (Kalman path filter; rotation+scale model) and
Tier 2 (sparse-feature estimator with RANSAC; NVIDIA hardware Optical Flow
when present; rolling-shutter correction). Tier 3 "Screen Lock" planar
tracking is now APPROVED as a SEPARATE OPT-IN mode — see the Tier 3 addendum at the end of this file. Implement only after Tiers 1-2 land.
Read the current implementation first: stabilization state + kernels live in
src/cuda/cuda_interop.cpp / cuda_kernels.cu (small-luma downsample,
projection profiles, motion state float4, warp kernel; reset via
ResetStabilization, called at camera switch/rotation/profile sites).

## Architecture: pluggable estimator, upgraded model, one filter, one warp
- Motion model upgrades from translation (dx,dy) to SIMILARITY
  (dx, dy, rotation, scale). All downstream stages consume a 2x3 affine.
- Estimators (selected per frame, auto-fallback chain):
  1. **hwflow** — NVIDIA Optical Flow Accelerator (Turing+ dedicated engine,
     ~zero SM cost). Dynamic-load `nvofapi64.dll` (driver-shipped; probe at
     runtime exactly like the Maxine wrapper — no link-time dep, no redist).
     Use the CUDA interop variant (NvOFCuda) on the small luma (or 1/2-res
     luma — the engine is fast); output = coarse block flow field →
     robust similarity fit (see below). If probe fails → estimator 2.
  2. **features** (default when no hwflow) — GPU Harris/FAST corners on the
     small luma (cap ~256 strongest, min-distance grid), pyramidal
     Lucas-Kanade (3 levels, 21px window) against the previous small luma,
     then robust similarity fit.
  3. **projection** — the existing profile correlator, kept as final
     fallback and for A/B comparison. Do not delete.
- **Robust fit**: RANSAC (or MAGSAC-lite) similarity from point pairs;
  inlier threshold ~1.5 small-luma px, ~50 iterations, refit on inliers via
  least squares. Flow/track counts are tiny (≤4096 pairs) — fit on CPU from
  a pinned async copy (event-polled, NEVER stream-blocking; reuse the
  keystone detection readback pattern) or fully on GPU if simpler. The fit
  MUST reject moving foreground (lecturer walking through frame) — that is
  the main robustness goal over projections.
- **Path filter (Tier 1, do FIRST — independent of estimator work)**:
  replace the exponential smoother with a constant-velocity Kalman filter
  per parameter (state [value, velocity]; process noise mapped from
  stabilizationStrength: high strength = low process noise). Correction =
  filtered path minus raw path, clamped to the crop margin as today.
  Acceptance behavior: hard tremor removed AND a deliberate slow pan tracks
  with no visible lag or post-pan drift-back.
- **Warp**: extend the existing bilinear warp to the full affine; then
  **rolling-shutter correction**: interpolate the per-frame transform across
  scanlines (top rows use blend of prev→curr transform, bottom rows the
  newer end; single lerp factor per row inside the same kernel — near-free).
  Gate RS correction behind a settings bool default ON.

## Settings / UI
- Keep stabilizationEnabled/stabilizationStrength semantics.
- The top value is a true 100% mounted-camera hold. Translation correction uses
  the crop reserve already supplied by zoom, never less than the legacy 6% and
  never more than 45%; 1.25x zoom therefore permits 10% correction per axis,
  2x permits 25%, and 4x permits 37.5%.
- Add `stabilizerMode` int: 0 auto (hwflow→features→projection), 1 features,
  2 projection (persist per-preset like other fields; Advanced combo
  "Stabilizer engine" with accessible names; tooltip shows the ACTIVE
  engine + estimator ms via the P8 tooltip line).

## Pitfalls
- NvOF API/driver versioning: probe capabilities, require >= Turing; treat
  any NvOF error as permanent-for-session fallback (latch, one log line).
- Corner tracking on pure-motion-blur frames: if inlier count < 12 or fit
  residual high, reuse previous transform (decay toward identity) — never
  inject a bad fit into the Kalman.
- All new device state: dims-cached Ensure*, SynchronizeStream before free,
  reset in ResetStabilization; previous-frame feature/luma buffers swap, not
  reallocate.
- Rotation around wrong origin: rotate/scale about the frame CENTER, and
  compose with the crop-margin clamp in the same space as the warp kernel.
- Keystone interaction: stabilization runs BEFORE keystone (unchanged);
  similarity correction must not fight the keystone homography smoothing —
  keystone already lerps corners; no change needed, but verify no visible
  double-correction wobble with both enabled.

## Acceptance
- [x] Build green (recipe as in plan 13); no per-frame host image readback or
      blocking estimator synchronization added
      (verify with P8 GPU ms before/after; estimator budget ≤ 2 ms without
      hwflow, ≤ 0.5 ms SM cost with hwflow).
- [ ] Tremor test: phone on laptop, typing on keyboard — visibly steadier
      than current build at same strength, including small ROTATIONAL wobble.
- [ ] Pan test: slow deliberate pan follows without lag; no drift-back.
- [ ] Foreground test: hand waved through frame does not yank the image
      (RANSAC inliers hold the background).
- [ ] Jello test: RS correction visibly reduces skew on sharp taps.
- [ ] hwflow: on RTX 4090 the tooltip shows "hwflow"; forcing mode=features
      and mode=projection both still work; non-NVIDIA path unaffected.
- [ ] Camera switch / rotation / profile change reset cleanly (no transform
      carryover); disable/re-enable re-baselines.
- [ ] CHANGELOG, plan updates (mark 11-adjacent stabilization notes),
      code_reference.md for new APIs.

Report back per the plan 13 reporting contract, including before/after P8
numbers and which estimator ran in each test.

---

## Second recorded validation — 2026-07-24

The owner supplied a new paired 3.43-second, 30 FPS AV1 capture,
`VID_20260724_015520_996`. It is preserved with hashes, CSV traces, an SVG
plot, contact sheets, and a reproducible ffmpeg/NumPy/SciPy analyzer under
git-ignored `local_evidence/stabilization/VID_20260724_015520_996/`.

Sequential phase correlation measured a dominant 1.165 Hz vertical component,
about 5.25% robust peak-to-peak source movement, and roughly 89 px of net
vertical drift. The processed output retained about 5.88% peak-to-peak
movement, only approximately 10.5% attenuation. Saved settings prove the
capture used Automatic RTX stabilization, 98% strength, rolling-shutter
correction, and 1.25x zoom.

The estimator was active, but two limits remained: the fixed 6% translation
clamp allowed only 43.2 px of vertical correction at 720p and saturated well
before the recorded path, while the 98% near-lock still followed too much of
the slow drift. The runtime now derives the translation allowance from zoom's
existing crop reserve: `max(6%, 0.5 * (1 - 1 / zoom))`, capped at 45%. The
recorded 1.25x configuration therefore receives 72 px of vertical authority.
The mounted-camera trajectory uses a 0.001 update coefficient at 98%, and the
strength slider reaches 100%, where the path is held exactly until that
authority is exhausted. `stabilization_cuda_tests` replays the measured
1.165 Hz, 18.9 px-amplitude wobble plus 89.34 px drift at the recorded 98%
strength and requires no more than 25% residual path RMS; the legacy 6% cap
retains about 43%.

### Live Automatic-vs-CUDA correction (2026-07-24)

The owner verified on the same vibrating camera that explicit CUDA feature
tracking produced visible improvement while Automatic RTX was nearly
indistinguishable from off. Inspection found that NVOFA was configured for the
throughput-oriented `MEDIUM` preset and explicitly preferred its coarsest 4x4
grid. That is the wrong tradeoff for subpixel 1-2 Hz shake that becomes large
only after magnification. Automatic now requests `SLOW` and prefers the finest
reported grid (1x1 on Ampere/Ada, with 2x2/4x4 fallback). Because a 320x180
1x1 field contains 57,600 vectors, conversion uniformly samples the full field
into the fixed 4,096-pair RANSAC budget instead of accepting a top-biased
prefix. High zoom may use its actual crop reserve up to the 45% guard.

### Quality-policy correction and higher-resolution analysis - 2026-07-24

Live comparison showed explicit CUDA feature tracking was visibly steadier
than Automatic on the owner's high-zoom, low-texture cabinet scene. Automatic
had treated any valid NVOFA fit as authoritative and never evaluated the sparse
model, even though NVOFA is a dense-flow primitive rather than a complete
camera-motion selection policy.

Automatic now evaluates the CUDA Harris/Lucas-Kanade similarity model first,
keeps NVOFA warm as a fallback when that model is rejected, and retains
projection correlation as the final compatibility estimator. Stabilization
analysis increases from the keystone-oriented 320x180 surface to at most
640x360 so 1-2 source-pixel tremor survives preprocessing; keystone remains at
320x180. Near-lock now enters progressively above 70% and becomes an exact
mounted-camera hold at 98% until crop authority is exhausted. The detailed
reference comparison and pinned research revisions are recorded in
[`25-stabilization-research.md`](25-stabilization-research.md).

---

## Tier 3 addendum — "Screen Lock" as a separate opt-in mode (owner-approved 2026-07-23)

**Partial implementation 2026-07-25:** Advanced now exposes transient
`Virtual Tripod`, `Re-lock current view`, and the plain `L` shortcut. This
ships the drift-free fixed-reference core: absolute CUDA feature/RANSAC
registration, last-good hold on confidence loss, and continuity-preserving
manual/automatic re-anchoring. It anchors the captured scene texture, not yet
the physical screen quad described below. Therefore the full Tier 3 acceptance
case for seamless arbitrary slide transitions remains open.

NOT part of default stabilization. A distinct, explicitly-engaged mode
(Advanced toggle + a hotkey, suggest `L`, announced via accessibility like
mode changes): capture the current detected plane as a reference and pin the
view to it by solving the plane homography per frame (reuses the Tier-2
tracking machinery + the keystone quad detector).

**Anchor on the PHYSICAL SCREEN, not the slide content.** This is the answer
to the slide-to-slide concern: the lock tracks the projected screen's
rectangle — quad edges/corners, bezel, the bright-region boundary — which is
static in the room even while slides change completely. Content features
(corners on text) are used only as secondary refinement and are re-baselined
whenever a large content change is detected (frame-difference spike =
slide transition; we already planned this detector). Slide changes therefore
do NOT break the lock; the hard cases are instead: screen leaves the frame,
lighting collapse, someone stands in front of most of the screen.

**Heuristic-failure handling (required, the owner's core concern):**
1. Confidence = tracking inlier ratio + quad-detection agreement. When it
   drops below threshold: FREEZE the last good transform (do not snap or
   drift), show a status/announced hint "Screen lock weakened — hold steady",
   auto-reacquire when confidence returns for ~10 consecutive frames.
2. **Manual adjustment while locked**: the existing pan/zoom controls
   (arrows, wheel, joystick, focus sliders) move the VIEWPORT WITHIN the
   locked plane — i.e., the lock defines the coordinate system; the user
   freely repositions inside it in stable, trackable offsets. No new control
   surface needed — same inputs, remapped semantics while lock is active.
3. **Re-lock** action (same hotkey pressed again, or a button): re-captures
   the reference from the current view — the user's escape hatch whenever
   the heuristic picked the wrong target. Disengage = plain toggle off,
   returning to normal (Tier 1/2) stabilization seamlessly.
4. If the screen is lost entirely for > ~3 s, drop to normal stabilization
   with a clear announced message rather than showing a frozen stale view.

Settings: `screenLockEnabled` transient (NOT persisted per-preset — it is a
session action, like recording, not a profile property). Acceptance: lock
survives a full slide transition unmoved; waving a hand over half the screen
does not shift the view; walking the camera slightly keeps the pinned view
until margins are exhausted; every state change is announced accessibly.
