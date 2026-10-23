# UCT contiguous helper batching candidate

This offline implementation starts at
`bb53ac884c24d15f0c77b2dc43cace0e8e650c24`. Both arms retain the integrated
two-slot recognition look-ahead and cooperative resident integral. The attached
23 October review is a source-backed hypothesis, not new performance evidence.
No build, configure, test, inference, GPU inspection, model preparation or
download has run for this candidate; root must first grant a finite START slot.

`gpu::RectifyBatch` uses grid Y for contiguous detector-order samples and the
unchanged scalar bilinear arithmetic, channel order, zero padding and
`min(canvas, validWidth)` behavior. It writes `[N,3,48,W]` into the existing
recognizer input allocation. There is no index upload, sorting, new precision,
vectorized sampling or padded-row optimization. The original scalar API/kernel
is retained for exact-bit comparison. Shapes are bounded before launch.

`issueGroup` still uses one stream and one recognition context. Only equal-size,
equal-canvas groups look ahead. Shape transitions and tails retain the existing
serial boundary. Device input/output and scratch reuse stay stream ordered;
each pinned slot's event fences its own copies, and host consumption finishes
before that slot is reused. The owned image and whole State remain subject to
the existing cancellation drain, asynchronous retirement and unknown-completion
quarantine. No release or current-driver speed/quality acceptance is claimed.

The registered CUDA fixture compares scalar and batched output bits at each
128/256/512/1024 canvas and sample counts 1/3/7/8/15/16. It uses fractional,
tilted and out-of-image coordinates, nonpositive/oversized useful widths,
nonzero descriptor offsets, poisoned unused descriptors and guard/high-water
tails. Tests are prepared but have not run.

New large evidence/build artifacts belong under
`Z:/Projects/OkuFlow/local-work-data`; the owner app, desktop, settings,
devices and credentials remain untouched. Shared desktop/video load is allowed
and must be recorded when root authorizes measurement. There are no new
dependencies, downloaded assets or third-party code.
