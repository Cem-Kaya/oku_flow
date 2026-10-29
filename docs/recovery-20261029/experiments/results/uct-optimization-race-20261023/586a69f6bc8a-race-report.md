# Muse Wonder Full — UCT optimization race report

Baseline `8f02fd11ed5ace89119b51d5cc2fb0576ea27e6d`; tested HEAD
`cbdffe91624023c380d082d07912303265ba9d3f`; test exe SHA-256
`09E37DC477EF9255CA1B69EE7FB80C79CF4414BE240D128829422D24847EBCD1`.
Worktree `Z:\Projects\T3-worktrees\oku_flow\t3-ddeff8b8`; own build cache
`build-cuda` in this folder. RTX 4090, shared desktop (owner video load
present and recorded), inference-only TensorRT SDK, cached FP32 plans only.

## Candidates

- A `e7b0668` (exact): batched GPU rectify/CTC launches + contiguous
  per-batch metadata copies + fused 3-channel bilinear. Bit-equality GPU
  fixture `gpuBatchedRectifyAndDecodeMatchPerSampleKernels`.
- B `3778c27`+`bc8c378`+`8157b1e` (approximate, opt-in):
  `OKUFLOW_UCT_TRT_GROUPING=sorted` on bucketed canvases; inverse
  permutation restores detector order; inert under fixed1024/BF16.
- Test repair `cbdffe9`: fixture variable renamed off Qt's `slots` macro.

## Ranked alternatives considered

1. Sorted width grouping (implemented as B).
2. Batched launches/transfers (implemented as A; overlaps Grok).
3. Group pipelining — deferred on my prior hypothesis that the ~3.9 ms
   CPU merge component bounded the gain; that component is not an upper
   bound on whole-worker stream-feeding/control savings (a parallel
   contestant measurement showed materially larger uncovered-gap
   reductions), so this remains open deferred scope, not a capped idea.
4. Detector long-edge cut — small-font recall risk; rejected.
5. Eliminated by inspection: GPU CTC, paint shaping cache, quad-intersection
   early-out, surface geometry (all already present/optimized).

## Validation actually observed

- Configure OK (own cache, VS2022 17.x, CUDA 12.9.2 pinned, Qt 6.12.0).
- Build `uct_paddle_tests` PASS after the rename (104.5 s, 2 jobs).
- Tracked scoped gate `run_test_gate.ps1 -Test
  uct_paddle_processing,uct_paddle_cuda -ExcludeGui` PASS (development
  profile; not the full matrix, no native GUI/camera/NVDA claims).
- Window 1 (driver 13040): prechecks PASS; F1 fixed1024 strict PASS
  (124/124); O2 bucketed/ordered strict FAIL 16/124 — all menu_7033 N8 vs
  N1, confidence delta 0.0010389 vs 0.001, geometry 0, truth identical.
- Window 2 (driver 13050): precheck-F refused; missing keys detector
  `948b17283f7dad0b8e5d553df0138fb79b26de81ead9d83a29c768be491283d0`,
  recognizer N1 `490d8f1bba0a024dcb5de09d7c6f04c5c7c3ad865ae6ba5a4be6ff524261a33b`,
  N8 `ee04fec24d044750fdaf46b87355b0e60ad77763f4294845bca90714196f1538`
  (derived width N1 `8213694ce394f589b5c37ac9a0766eba8f5cac8c82fc20793725d7e9f32fca29`,
  N8 `7f7a02d0043e5f150a47682303fb3760a4aac2f2e5ffc5c3e0848e3df6b699a9`).
  Driver-int-only recomputation reproduces all reported keys exactly;
  window-1 manifest recorded cudaDriverVersion 13040. No plans prepared,
  aliased or re-keyed; cache byte-identical.

## Indicative timing (F vs O only; S UNMEASURED)

Steady medians, 12/cell (first-4-timed drop, one visit/arm, F before O,
shared desktop): see `slot-20261022-231950/offline-analysis.json`.
Deltas O-F ranged +34.7% (n1 menu_7033) to -12.5% (n8 menu_7033);
long-lines no-gain control +7.1% at N8. Truth CER/WER identical
(sparse 0/0; long-lines 0.0551/0.0571). Menu candidate words 12->13 and
11->12 under bucketed (no truth). These numbers price only the
pre-existing width selector on the candidate binary; S-O (sorting) and
A-vs-baseline-binary remain unmeasured. No BF16 benefit is claimed;
the local BF16 profile is fixed1024-only and untouched by B.

## First failures preserved

slot-20261022-231553 (script cache-type refusal), slot-20261022-231649
(build C2059), slot-20261022-231950 (main window-1 evidence),
slot-continuation-20261022-233034 (deadline-skipped), 
slot-window2-20261023-001201 (driver-identity precheck refusal).

## Smallest next steps

1. Under one consistent driver identity (re-prepared 13050 plans via a
   separately authorized step, or driver rollback): run frozen
   run-window2.ps1 unchanged except its pinned 13040 plan filenames.
2. Add an 8f02 baseline-binary arm to price candidate A and attribute
   the 0.0010389 confidence cell.
3. Only promote sorting if S-O exceeds shared-desktop spread on dense
   frames with quantified word-level loss.
