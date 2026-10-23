# UCT whole-worker pilots, 2026-10-23

These are two separate shared-PC pilots of scheduler worker time and
submit-to-callback time. Neither measures capture-to-paint latency, accessibility,
FPS or exclusive-idle performance. No sources were pooled or outliers removed.
Ordinary/private engine caches stayed unchanged.

## Exact optimization comparison

A=`c926e3d`: old merge/scalar helpers. B=`44683b3`: exact merge.
C=`f386454`: merge plus batched rectify/CTC and three bulk copies. All retain
identical two-slot lookahead, integral, FP32 plans and N8 fixed canvases.
Six counterbalanced blocks/18 visits give 96 steady samples per arm/source.
Startup, one warmup and the first four of 20 timed passes were excluded from
statistics in advance, but retained and checked for exact outputs.

Worker milliseconds, median / nearest-rank p95:

| Source | A | B | C | A to C median reduction |
|---|---:|---:|---:|---:|
| Book | 119.80 / 154.79 | 101.59 / 112.99 | 97.04 / 101.02 | 19.00% |
| Dense frame 55 | 406.42 / 435.90 | 386.36 / 401.92 | 369.74 / 384.12 | 9.03% |
| Menu | 35.75 / 41.68 | 35.68 / 39.49 | 34.98 / 38.44 | 2.15% |

B improved each book/dense block median (6/6); menu 3/6. C improved on B
in 6/6 blocks for every source. B-to-C median reductions are 4.48%, 4.30%
and 1.96%; no universal promotion threshold is claimed.
All 1,152 successful startup/warmup/excluded/timed outputs matched final ordered
serialization byte-for-byte, including confidence and geometry bits. Raw CTC
and premerge crop descriptors were not observed. Actual accepted plan payloads,
pixels, executable and runtime identities were checked.

Evidence:
`Z:/Projects/OkuFlow/local-work-data/uct-whole-worker-fp32-20261023/scripts-ABC-benchmark-r6-root/ABC-benchmark-20261023-170142-08943a5a967246a280e1be8f1bc905f3/`.
Both controller and child exited zero, with zero owned job members and a verified
free runner. Earlier wrapper failures and the prior build controller125 remain
failures. `pilot-summary.json` retains both timing boundaries and block deltas.

## Private BF16 and smaller recognizer

One root executable from `f80caeb`, SHA256
`b75de13941e176c00e8e6f31020f79cbbddd015f6735e2a9122c50a3ebfcf2d1`,
ran C=medium FP32, D=medium private BF16, E=small FP32, always with the same
medium FP32 detector and N8 fixed canvas. Schedule C,D,E,E,D,C gives 32 steady
samples per arm/source after the same exclusions. Do not pool campaigns or use
the shifted FP32 control as evidence of another code gain.

Worker milliseconds, median / nearest-rank p95:

| Source | C: medium FP32 | D: medium BF16 | E: small FP32 |
|---|---:|---:|---:|
| Book | 120.17 / 130.99 | 78.40 / 89.37 | 47.60 / 55.54 |
| Dense frame 55 | 450.34 / 505.91 | 274.98 / 320.93 | 193.81 / 226.26 |
| Menu | 41.80 / 47.09 | 34.37 / 37.02 | 28.80 / 32.17 |

Actual application-factory receipts selected the reviewed private BF16 graph and
whole-plan digest `155c74b5b856467e49853190c881077c9af3200203a196f9e737f7533e6f5495`
on the current driver. No fallback was counted as BF16. Each model passed its
own repeated same-capacity exact checks; cross-model equality was not required.

One retained final output per source has these word counts and text-multiset
differences against medium FP32:

| Source | FP32 medium | BF16 medium | FP32 small | Differing entries: BF16 / small |
|---|---:|---:|---:|---:|
| Book | 424 | 422 | 412 | 60 / 230 |
| Dense frame 55 | 290 | 289 | 282 | 7 / 64 |
| Menu | 13 | 13 | 13 | 0 / 8 |

This comparison ignores spatial alignment and is not ground-truth error,
omission, CER/WER or sensitive-token recall. Independent truth was not supplied
for these frames. Every output retains text, confidence, disposition and geometry.
Keep these models opt-in; the pilot does not approve a default or public quality.

Evidence:
`Z:/Projects/OkuFlow/local-work-data/uct-private-model-pilot-20261023/pilot-f80/C2-20261023-172541-eea502ed5c1042b08b0e29c9b9545014/`.
Controller/child both exited zero; job membership zero and runner free.
The prerequisite root CUDA build/four tracked cases passed, but its outer125
for unknown members51948/14840 remains failed; cleanup confirmed removal.

Public BF16's production corpus catalog stays empty. Separate native storage,
local-build/golden, whole-app, width, detector, hardware and release gates remain.
