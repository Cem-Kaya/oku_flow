# Performance and deployment follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24. The old per-frame-stall,
allocation, readback, CPU-OCR and Maxine audit lists have been retired.
Do not reintroduce them as current defects without a reproducer.

## Remaining investigations

1. **Measure first glyph and Freeze-to-paint latency in the integrated app.**
   Worker microtime is not the whole reading experience. Separate capture,
   probe/admission waits, recognition, UI receipt, paint and accessibility.
   `PresentationReceipt` is declared/named in the timing API but has no
   production emission site in this baseline. Close that trace boundary before
   claiming complete present-to-paint coverage; retain drop accounting.
2. **Evaluate optional regional/width paths.** Shadow planning, supplemental
   regional OCR and narrower recognizer canvases already exist behind explicit
   controls. They are not a validated policy for skipping full reads. Measure
   real end-to-end benefit under video/presentation load before changing defaults.
3. **Profile remaining probe/presentation cost.** Candidates include checking
   only visible lines with fair continuation, fewer UI round trips and avoiding
   redundant word-list preparation. These are hypotheses, not measured wins.
   Recheck current code before reviving individual old audit findings.
4. **CUDA Graphs / launch tuning.** No application-owned graph-capture path is present
   in this baseline. Consider only stable, attributed kernel chains, preserving
   cancellation, dynamic shapes, ownership and immediate presentation.
5. **Laptop power policy.** The user-facing proposal is in
   [29](29-lecture-workflow.md); measure energy and readability, not just FPS.
6. **Alternative text super-resolution.** Maxine is already implemented.
   Portable or text-specific alternatives remain research if a concrete
   readability/hardware gap justifies them. No replacement model is selected;
   retain licensing, output-quality and actual latency comparisons.

## Smaller base download

Proposed after the owner's concern about release size. The pinned OCR payload
is about 2.39 GB before compression: roughly 2.01 GB TensorRT, 265 MB models,
97 MB NVRTC and 18 MB ONNX Runtime. The current complete package works offline
after installation; no optional-download architecture is implemented.

A future small base package could fetch the needed GPU runtime/model subset on
explicit selection, retaining a functional CPU path, exact pins/notices,
bounded atomic staging and truthful offline/failure behavior. Keep the current
full offline download available. Do not remove required builder resources or
make another user's TensorRT cache a distribution dependency.

## Evidence to retain

- [Worker pilots](../docs/uct_worker_pilots_2026_10_23.md)
- [Batch/helper integration](../docs/uct_gpu_batching.md)
- [Regional execution](../docs/uct_active_regional_ocr.md)
- [Width experiments](../docs/uct_recognizer_width.md)
- [Trace contract](../docs/uct_timing_trace.md)
- [Current distribution](../docs/uct_distribution.md)

Medium BF16, small/tiny pilots and helper/merge work have already been done.
More measurements or a different default are separate decisions.
