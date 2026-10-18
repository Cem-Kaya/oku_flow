# One-time media output-buffer comparison (2026-10-19)

The two-output prototype is functional, but this run does not justify changing
the application default from one output. It improved the light 4K workload and
slightly worsened latency in the expensive text workload, with no consistent
throughput gain there. The experiment remains explicitly selectable through
`MediaGpuScene(2)` and the opt-in fixture benchmark; ordinary app construction
still uses one. Both throughput and latency informed that decision.

## Results

RTX 4090, driver 617.42, CUDA 12.9.86, VS 2022, Qt 6.12.0, Release. Each cell is
the median of four runs; latency columns are medians of each run's 95th percentile,
not a pooled percentile. FPS counts the isolated GPU processing/drawing stage,
not end-to-end player or monitor FPS. Raw measured rows are in
[the CSV](benchmarks/media-output-buffers-2026-10-19.csv).

| Resolution / workload | Stage frames/s, 1 → 2 outputs | Throughput change | Frame p95 ms, 1 → 2 | Seek-target p95 ms, 1 → 2 | View-change p95 ms, 1 → 2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1080p, neutral | 2486.2 → 2265.2 | -8.9% | 1.248 → 1.302 | 1.227 → 1.313 | 1.235 → 1.271 |
| 1080p, text effects | 552.2 → 549.5 | -0.5% | 4.032 → 4.228 | 3.846 → 3.960 | 4.084 → 4.217 |
| 4K, neutral | 1601.3 → 1701.3 | +6.2% | 1.772 → 1.796 | 1.724 → 1.816 | 1.730 → 1.764 |
| 4K, text effects | 232.3 → 230.6 | -0.7% | 9.149 → 9.698 | 7.913 → 8.564 | 9.053 → 9.661 |

The extra base and spatial/neural output textures add 16,588,800 logical bytes
(15.82 MiB) at 1080p or 47,923,200 bytes (45.70 MiB) at 4K. Shared processing
history, scratch, imports and models are not doubled. Driver allocation padding,
decoder reference surfaces and other app allocations are additional. Peak total
VRAM was not measured.

These are limited local observations, not statistical significance or proof
that one output is optimal on every GPU. Several small differences overlap
run-to-run variation. This run does not establish a GTX 1070 policy or real
camera/lecture performance, and does not warrant switching based on FPS alone.

## Method and boundaries

- Four immutable GPU input textures are prepared before timing; native decoding,
  disk access, codec preroll and source-import cold start are excluded.
- Both variants use one production CUDA effect history/model/stream. Two outputs
  rotate base/cache textures with separate producer/reader fences; they can
  overlap the graphics read of A with processing into B without a deep submitted
  FIFO. The producer remains bounded to one in-flight processing pass.
- The production viewport shader renders into owned GPU snapshot targets. Its
  real graphics completion fence ends each latency sample. This exercises GPU
  drawing and source fences without creating a desktop window, swapchain,
  native frame pacing, VSync or monitor scanout. It is not photon latency.
- Each trial warms for at least 250 ms, then runs steady input for at least two
  seconds and 240 frames. Four paired repetitions balance 1/2 and 2/1 order.
  Each trial also samples 120 source-generation changes (seek-target work) and
  120 cropped view changes on advancing frames. Older waiting seek requests
  are superseded before submission; already submitted GPU work still completes.
- The neutral profile has ordinary conversion/output. The text profile adds
  automatic Text Clarity, smart sharpening, auto contrast and temporal smoothing.
  OCR and vendor Maxine inference are excluded from timed work. Separate
  correctness checks cover shared histories, UCT and fake neural publication.
- Seek-target timing includes processing admission/reset/drawing of an already
  available decoded target. It is not demux-to-screen seek latency. View-change
  timing similarly excludes Qt input scheduling. The hot polling loop's auxiliary
  `api_p95_us` CSV column includes cheap Pending polls and must not be interpreted
  as a native UI responsiveness measurement.
- No timing threshold is a correctness assertion. This is an explicit one-time
  experiment, excluded from ordinary CTest/CI runs. Unattended background load,
  clocks and thermal state were not locked; raw paired rows remain inspectable.

The measured fixture SHA-256 is
`2e62f0b687aa53b90a703fc5fb7009d59c22b7960e729afb689fd265d2512f5c`.
The complete local timed log SHA-256 is
`002bace718bf3e1a72c6674168218d5e01caa7448fbb2511d0f8aebc759dcf0f`.
The log is retained under `%LOCALAPPDATA%/OkuFlow/recap-release-20261018/` as
`two-output-benchmark-timed.log`, alongside earlier diagnostic/pilot attempts.

## Ownership and regression coverage

`DoubleOutputTests` holds a real graphics fence on output A and proves that B
finishes with a different texture and fence, without overwriting A. It verifies
continuous shared temporal history, both-output backpressure without history
changes, newest-seek admission after credit returns, and reset/replay. Existing
full effect/history and spatial/fake-Maxine cases also run with two outputs.
Per-output fences prevent a high value on B from falsely retiring A's read.
All native output resources must match the device and cache dimensions before alternate import.

`MediaGpuScene::Release` polls completion of its exact owned construction and
retirement closures. The benchmark uses it for each trial instead of waiting for
every C++ thread while the shared test device remains alive: graphics libraries
may keep their own persistent workers. The original broad-shutdown failures are
retained; completion deadlines and GPU fence assertions were not relaxed.
No desktop, owner app, camera, browser or phone was driven.

For reproduction and acceptance scope, see [tests](../tests/README.md#one-time-output-buffer-experiment)
and [GPU playback](gpu_video_playback.md).


## Post-experiment validation

The tracked background `scripts/agent_build.bat` passed shipping Release,
108 CPU cases and all 17 CUDA delta cases. Two CPU cases retain optional skipped
Qt rows; no case was skipped and no background failure remains. The native app
fixture passed all 53 offscreen Qt rows; shared-source equivalence passed with
PCH ON in CPU/CUDA configurations and PCH OFF in the isolated native cache.
The normal media GPU fixture passed its expanded one/two-output effect/history,
spatial and private fake-neural contracts. No timed benchmark ran as part of
those gates. Native GUI, real screen readers, full player seek latency, real
vendor inference, long soak and installer acceptance remain separate.
