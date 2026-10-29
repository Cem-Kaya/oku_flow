Adds the revised roadmap's shadow regional planner on the existing UCT worker. `OKUFLOW_UCT_SHADOW_PLANNER=1` enables bounded spatial occurrence tracking, requested-focus priority and reserved coverage of unseen areas. Full OCR stays authoritative; no selection can skip inference or publish text, and default operation allocates no planner.

Acquisition/job/authority guards reject held duplicates, stale prepared words, invalid geometry and oversized metadata. Observer faults preserve full OCR. Fixed top-eight overlap selection and ranked missing eviction reduce bookkeeping cost while preserving deterministic metadata. The optional frozen-crop recorder now refuses a fifth source locally rather than discarding completed recognition and retiring healthy engines.

Validation so far:
- 39 scoped CPU cases pass: 31 planner, 7 real-scheduler and model-free Paddle processing; original intermediate failures are retained.
- Two finite 17-scenario/241-observation metadata runs pass every invariant and retain identical non-timing JSON. Shifted 2048-word median: 11.76 to 2.11 ms; shifted 8192-word median: 28.21 to 7.00 ms on Ryzen 9 5900X. These are observer timings, not OCR or display latency.
- PCH-off, GPU resident integration and the final tracked Release/CPU/CUDA matrix are in progress. Native GUI checks run on the disposable GitHub Windows VM; no local desktop automation was used.

See `docs/uct_region_planner.md` for limits, retained evidence and the expensive adversarial overlap case. Character-sensitive OCR reuse remains blocked by the existing verifier evidence. No model default, translation, dependency or release payload changes are introduced here.