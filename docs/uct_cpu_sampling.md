# CPU UCT sampling

`PaddleNormalize` and `PaddleRectify` prepare BGR float tensors for the optional
CPU Paddle backend. The sampling candidate shares the two source rows, clamped
pixel indices and fractional coordinates across B/G/R. Normalization also
plans source columns once per call. It retains the previous interpolation and
per-channel normalization operation order, image-format conversion, stride,
border clamping, valid-width and zero-padding rules. No source/result cache,
resolution, model, confidence or admission change is involved.

The extra normalization column plan is `target.width() * sizeof(SampleAxis)`
(12 bytes per column on the expected MSVC ABI, at most 48 KiB under the unchanged
4096-column bound). Rectification adds only stack addressing state.
Output tensors and their allocation sizes are unchanged.

This affects CPU fallback preprocessing. Device-resident TensorRT sampling and
the owner's cache-only mixed-BF16 selection do not call these CPU functions.
It establishes no live video FPS, OCR-update rate, queue-age or source-to-sharp
latency gain. Those measurements require a complete independently authorized
pipeline session. The first CPU bridge can also be Windows OCR, which does not
use these functions.

## Validation and explicit diagnostic

`uct_paddle_processing`'s existing `normalizationAndBounds` slot compares every
tensor byte against separately compiled function bodies from commit
`8f02fd11ed5ace89119b51d5cc2fb0576ea27e6d`, retained in
`tests/uct_cpu_sampling_reference.cpp`. Its reference is deliberately independent
of production helpers. Tiny and padded-row images, four source formats,
positive/negative tilt, border samples, valid widths, padding and invalid bounds
are covered. Exact tensor equality is stronger than aggregate hash agreement;
it does not constitute measured recognition accuracy or native paint/UIA acceptance.

The optional `uct_paddle_tests cpuSamplingBenchmark` slot is not in CTest's
routine command. Set process-only `OKUFLOW_UCT_SAMPLING_FRAMES` to the private
folder containing `menu_7000.png` / `menu_7033.png`, and
`OKUFLOW_UCT_SAMPLING_REPORT` to a fresh private JSON path. Run with Qt offscreen
on a finite exclusive compute slot under the tracked global runner lease.
No model/runtime download, preparation, inference or GPU operation occurs.

The diagnostic measures normalizations at 480x256, 704x384 and 960x544, plus
1/10/128 explicitly generated tilted rectification quads on each full image and
a 703x401 region. Those are synthetic crop counts, not recognized word counts.
It retains exact points/extents, tensor bytes/hashes, first-call times, five
warmups per variant and 31 alternating ABBA/BAAB blocks (62 samples per variant).
Every timed output is compared byte-for-byte outside timing. Timing includes
format conversion, allocation, coordinate planning and sampling. Batched crop
timing also includes equal aggregate-result allocation/copy work; hashing and
comparison are excluded. First-call timing starts after image loading and is
neither model cold-start nor process-start latency. No allocation/inference/GPU
event split or process CPU-time attribution is inferred from CPU wall time.
Preserve command logs, first failures, source/input/executable hashes and
background/GPU pre/post observations with the raw report.

Race evidence and measured conclusions are recorded in the isolated Sol
handoff under `local-work-data/uct-optimization-race-20261023/sol`; a scoped CPU
pass is separate from the full Windows/CUDA/release and native accessibility gates.
