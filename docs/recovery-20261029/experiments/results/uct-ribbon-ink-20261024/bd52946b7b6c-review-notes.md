# Post-implementation review notes (t3-b137fab1)

Base caf523e0, imported 6d4e29e as cherry-pick 2c425f8e. SOURCE+STATIC only; nothing built/run.

## P1
- P1-1 Merge drops unsupported-curve ink protection. PaddleMergeRecognition:
  dropped new word (use=false) and erased old words lose protectedBoundsPx.
  Trigger: overlapping candidate from duplicate detection/window replaces the
  single unsupported word carrying the full envelope. Fix: union inheritable
  protection into the overlap winner (both directions), predicate scoped to
  fromRibbon or protection extending beyond own bounds (baseline words have
  protected==bounds, so straight behavior unchanged). Chains propagate via
  the beyond-bounds limb.
- P1-2 GPU fitRibbons serial scan: 128 blocks x 1 thread, up to 262144
  sequential label reads/component (worst ~33.5M reads on 128 threads).
  Fix: 256 threads/component, strided coalesced scan, per-thread bins,
  fixed-order shared reduction (count/low/high exact; sumU defined order).
  No atomics, no extra transfers, same stream/memory bounds. UNMEASURED.

## P2
- P2-1 gpu::Quad grew 48->200 bytes; rectify/rectifyBatch copy whole Quad per
  pixel thread and compute bilinear coords then discard when ribbon valid.
  Fix: const Quad& + branch-first (identical expressions per branch).
  Disabled path now loads only xy/validWidth/valid flag fields touched.
- P2-2 No host/device ABI lock for Quad/Geometry/Bin. Add static_asserts
  (Geometry 144, Bin 32, Quad 200, ribbon@48, componentRoot@192).
- P2-3 CPU PaddleDetect fills bins even when Fit would refuse length<24.
  Add exact u1-u0>=24 pre-gate (curve default either way).
- P2-4 CPU PaddleRectify recomputes centerline polynomial per pixel.
  Precompute per-column terms once per crop; identical double op order.
- P2-5 Straightness gate in detector px vs word error in source px: with
  detector downscale >1, sub-gate curves can exceed 1 source px yet take the
  baseline rectangle path. Pre-existing baseline behavior; changing the gate
  would alter straight outputs. Documented, not changed.
- P2-6 CPU/GPU bin sumU summation order differs (BFS vs raster/strided):
  Fit inputs differ in last ulps; exact CPU/GPU parity never held (float vs
  double PCA/extents). Add bin-order tolerance test; ROOT CUDA run compares
  within tolerance, not bytes.
- P2-7 Ribbon Width() 32-segment arc recomputed per line/window/decode.
  Per-line cost trivial vs per-pixel rectify; caching would change ABI/hash.
  Left as is (speculative).
- P2-8 PrepareLines ribbon band uses destinationHeight division; only runs
  when protectedScene usable (valid transform in practice). Noted, no change.
- P2-9 fitRibbons/GPU: vertical (w<h) lines never fit; matches CPU w>=h gate.

## Verified sound (no change)
- Transform chain H once -> A -> region -> ribbon -> CTC u -> +origin once ->
  inverse A once -> present once. TensorRT cropPixels is region-offset pointer,
  ribbon coords region-local consistent with xy.
- Scheduler DetailScope sets/clears ribbon context per job incl. exceptions;
  Automatic forwards; env ANDed in both Paddle adapters; immutable flag only
  from ImmutableStamp selected frame; resident diagnostic Stamp consistent.
- Detect offsets: ribbon mode preserves labels via separate buffer sized
  (w*h+255)/256 == blocks; disabled mode reuses labels scratch as before.
- Fit Gauss-Jordan partial-row swap is sound (earlier columns already identity).
- Inside() exact quadratic extrema; height positivity at endpoints sufficient
  for affine height. Error() sagitta exact for this polynomial family.
- Decode ribbon branch: recursive local decode recovers u exactly; search-span
  error also gated; unsupported branch after recursion marks all invalid.
- Frozen crop plans bind curvedRibbon mode; replay carries ribbon geometry.
- Current-ink SAT indexing correct; guards fail closed; offline only.

## Current-ink robustness
- Imported counts preserved: original 72/72 0/0/0; held-out 24/24 0 FA,
  12 FH, 0 U. Change: matched [1,2,1]/4 blur of BOTH normalized grids; only
  demotes Changed->Unverifiable(Blur) when blurred worst <= existing .035.
  Unchanged still requires sharp <=.035, so false accepts cannot increase by
  construction. Scale (2.5%) drift beyond blur tolerance likely remains;
  no scale search (could align edits; cannot verify blind). New counts UNRUN.

## Benchmark
- Resident A/B reachability verified: scheduler SubmitDevice + immutable
  media stamp + env reaches TensorRT curved branch; fit cost lands in the
  existing Regions profile stage, rectify delta in Rectify stage.
- Added OKUFLOW_UCT_RESIDENT_PRODUCTION_FACTORY=1: CreateProductionTensorRt-
  Recognizer (shipped BF16-preference catalog), exclusive of local/experiment
  BF16, exactN8, frozen/shadow. Actual per-role precision pinned via backend
  detail in every job JSON; Validate requires recognizer BF16 marker.
  Direct-FP32 plan-key preflight skipped (wrong keys for BF16 graphs);
  cold-cache refusal = inference-only SDK + 60s startup deadline +
  cache-inventory-unchanged final gate. Direct factory remains labelled FP32.

## Final commits (branch t3/review-optimize-curved-ocr)
- 2c425f8e cherry-pick of 6d4e29e (tree-identical, parent caf523e0)
- 73e3e4d0 fix: preserve ribbon ink protection across merge replacement
- 4e92abcd perf: parallelize GPU ribbon fit and lean rectify sampling
- 04ecc756 feat: demote blur-only current-ink changes to unverifiable
- 540a6d83 feat: add production-factory resident benchmark selection
- 7831288a docs: record ribbon review round and BF16 A/B recipe (final HEAD)
Status: UNBUILT / UNRUN (source+static only by assignment).
