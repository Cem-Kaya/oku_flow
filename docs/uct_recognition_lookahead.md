# Recognition-group look-ahead experiment

Opus implemented and measured this candidate on 2026-10-23 from baseline
`8f02fd11ed5ace89119b51d5cc2fb0576ea27e6d`. Sol continued this worktree after
the provider rate limit and archived the measured source without changing it.

The TensorRT worker retains one context and one stream. Two bounded pinned
metadata slots and completion events let the next recognition group run while
the host waits for and merges the preceding group. Look-ahead requires equal
group size and canvas width. Partial groups and shape transitions remain
serial. Stream order preserves reused device crop/output storage. Cancellation
drains any issued look-ahead before releasing its owned input; unknown completion
uses existing whole-State retirement/quarantine. No model, threshold, image
readback, spelling or admission change is intended.

On the shared-desktop Ryzen 5900X / RTX 4090 machine, real resident FP32
medium/medium fixed1024 dense `frame-55` N8 worker p50/p95 changed from
299.51/333.18 to 252.92/274.66 ms (24 steady samples/variant, ABBA). Per-run
medians were 293.22, 248.47, 259.39, 323.23 ms. The sparse menu did not trigger
look-ahead and showed no gain. Dense N1 was noisy and regressed in the pooled
sample; no general N1 benefit is established. The profiled dense N8 worker
improvement was smaller, 17.85 ms; the recognizer event interval increased
8.80 ms. This is host/GPU overlap evidence, not faster kernel arithmetic or FPS.

Semantic output hashes match the original executable at each capacity. Dense
diagnostics still exit 1 at the pre-existing strict N8-versus-N1 gate. That is
separate from baseline-versus-candidate equality and is not a passing run.
The scoped tracked `uct_paddle_processing` and `uct_paddle_cuda` selection passed
2/245, no skipped/unresolved cases. The first malformed comma-list gate
invocation is retained as an invocation failure. Full matrix, native UIA,
production local BF16 and camera/display acceptance were not run.

Raw evidence, commands, original failures, GPU state and executable/source hashes:
`Z:/Projects/OkuFlow/local-work-data/uct-throughput-opus-20261023`.
Original source SHA-256:
`5B83A7BEA82862C82F6DA7E1FE4EDBA322CC758FDA9B79B10A2F265B1488EB91`.
Original measured candidate executable:
`954E616E7A1BE6EEAC41A9650FDA75DB6120BF588D8656640FF0F9867D301B6E`.
No dependencies or third-party code were added; licensing is unchanged.
