# TensorRT CUDA graph experiment (default off)

Opus prepared the initial graph idea. Following its provider rate-limit
interruption, Sol hardened ownership/capture cleanup and measured graph OFF/ON
in one executable on 2026-10-23, with Opus' look-ahead held constant.

`OKUFLOW_UCT_TRT_GRAPHS=1` opts into enqueueV3 graph capture. Empty/0 retains
ordinary enqueue. No new engine/profile/precision, preparation or result cache
is introduced. `Network` owns one execution context, fixed optimization profile
and automatically allocated workspace; it retains at most one graph for that
current context state. Shape, input binding, input allocation generation or
stream change drains and destroys the graph before context reconfiguration.
Output growth is driven by shape change and happens after that drain.
`DeviceBuffer::Generation` distinguishes allocation reuse at the same address.
Every changed state gets an ordinary enqueue to flush TensorRT deferred state
before capture. Context/engine/workspace mutations must remain centralized here;
adding any new mutation requires invalidation before it can replay a graph.

Capture always ends, including enqueue failure. Only explicit unsupported or
invalidated capture errors permit cleared capture error, successful drain and
ordinary-enqueue fallback with graph use disabled for that Network. Other
CUDA/API/instantiation/launch failures propagate to existing retirement and
quarantine. State retirement establishes stream completion before destructors;
unknown completion retains the complete State, graph, context and buffers.
No graph is persisted or reused between networks or sessions.

`OKUFLOW_UCT_TRT_GRAPH_DIAGNOSTIC=1` prints capture setup and final
capture/replay/invalidation/refusal counters. Private
`OKUFLOW_UCT_TRT_GRAPH_REFUSE_CAPTURE=1` deliberately invalidates a capture with
an unsupported stream synchronization, to exercise actual EndCapture cleanup
and fallback. It is a diagnostic fault seam, not an application preference.

Shared-desktop real resident FP32 M/M fixed1024 ABBA worker p50/p95 in ms:

| Source / capacity | OFF | ON | Finding |
| --- | ---: | ---: | --- |
| menu_7000 N8 | 25.11 / 27.74 | 25.08 / 26.74 | No useful gain |
| menu_7033 N8 | 24.33 / 26.68 | 26.99 / 29.68 | Regression |
| frame-55 N8 | 258.98 / 289.21 | 261.70 / 274.31 | Inconclusive under drift |
| menu_7000 N1 | 33.21 / 36.84 | 27.81 / 29.35 | Launch-bound research benefit |
| menu_7033 N1 | 31.18 / 40.04 | 26.17 / 28.19 | Launch-bound research benefit |
| frame-55 N1 | 512.93 / 625.73 | 337.46 / 364.18 | Benefit with large OFF-run drift |

24 steady samples/variant/source; first four timed samples of each 16-pass
block additionally excluded. Per-run raw values are retained. Dense N8 OFF
run medians 282.87 and 254.52 ms bracket ON 264.11 and 260.10 ms: no N8 win.
Dense N8 ON reports 54 recognizer captures, 360 replays and 54 invalidations
per lifetime. Across all captures setup p50/p95 is 1.09/1.78 ms, max 3.48 ms.
Shape churn erases much of the benefit. Graphs remain default **off**.

All selected semantic output hashes match at each source/capacity across
OFF/ON/refusal, with zero cache mutations or image egress. The actual refusal
diagnostic exits 0; dense diagnostics still exit 1 at the original strict
N8-versus-N1 gate. The first block3 invocation failed in its PowerShell date
handling; the repaired invocation used the same original deadline and retained
both attempts. Neither issue is hidden by semantic-parity results.

Incremental build and exact tracked processing/CUDA selection passed 2/245,
no skips/unresolved. The measured source SHA-256 is
`F1FEBA22D9E358C7D9FF1A7A9167BA610EC4B97D1715949AB7C0B43CDA85720B`;
same-executable OFF/ON hash is
`AB51D2A7D9A9A39B91B2D08F0D3C57FFB168F44F69E4958955DE2A829BE9468F`.
After compute release, static review corrected invalidation to drain the
**recorded old stream**, rather than the incoming stream. The real caller uses
one fixed State stream, so this changes an unexercised stream-switch branch.
The final one-line correction was not rebuilt or run; the measured snapshot is
preserved separately. A future integration must rebuild and validate it.

Remaining acceptance: model-backed cancellation/fault retirement with graphs in
flight, explicit same-address reallocation and stream switching, variable-width
shape transitions, native publication and the full shipping matrix. Dense
fixed-width runs exercise detector shape and recognizer partial-batch switches,
but do not establish the unrun cases. Production local BF16 was not measured.
No display FPS, native UIA, real camera or release claim follows.

Evidence/commands/raw samples and GPU state:
`Z:/Projects/OkuFlow/local-work-data/uct-throughput-opus-20261023`.
The [TensorRT 11.4 guide](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/performance/optimization.html)
documents context-specific capture and shape-update constraints. No dependency
or third-party code was added; license inventory is unchanged.
