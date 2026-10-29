## Measured worker comparisons

Unprofiled steady values, milliseconds. `n` is samples **per variant** after the extra four warmups per block. p95 is reported only for n >= 20. Negative change is faster.

| Experiment | Source / capacity | n | Baseline/off p50 / p95 | Candidate/on p50 / p95 | p50 change |
| --- | --- | ---: | ---: | ---: | ---: |
| Opus look-ahead | menu_7000 / N1 | 48 | 32.89 / 40.64 | 32.79 / 41.41 | -0.10 ms (-0.3%) |
| Opus look-ahead | menu_7000 / N8 | 48 | 23.23 / 26.17 | 23.78 / 27.55 | +0.55 ms (+2.4%) |
| Opus look-ahead | frame-55 / N1 | 24 | 483.05 / 685.24 | 534.78 / 695.80 | +51.72 ms (+10.7%) |
| Opus look-ahead | frame-55 / N8 | 24 | 299.51 / 333.18 | 252.92 / 274.66 | -46.59 ms (-15.6%) |
| Sol graphs, look-ahead constant | menu_7000 / N1 | 24 | 33.21 / 36.84 | 27.81 / 29.35 | -5.40 ms (-16.3%) |
| Sol graphs, look-ahead constant | menu_7000 / N8 | 24 | 25.11 / 27.74 | 25.08 / 26.74 | -0.03 ms (-0.1%) |
| Sol graphs, look-ahead constant | menu_7033 / N1 | 24 | 31.18 / 40.04 | 26.17 / 28.19 | -5.00 ms (-16.0%) |
| Sol graphs, look-ahead constant | menu_7033 / N8 | 24 | 24.33 / 26.68 | 26.99 / 29.68 | +2.66 ms (+10.9%) |
| Sol graphs, look-ahead constant | frame-55 / N1 | 24 | 512.93 / 625.73 | 337.46 / 364.18 | -175.47 ms (-34.2%) |
| Sol graphs, look-ahead constant | frame-55 / N8 | 24 | 258.98 / 289.21 | 261.70 / 274.31 | +2.72 ms (+1.1%) |

The dense N8 look-ahead reduction is 46.59 ms, 15.6% of original worker service. It removes overlap gaps rather than 46.59 ms of GPU kernel arithmetic. The sparse menu did not trigger look-ahead: 10 crops split 8+2. Dense N8 graph medians do not establish a gain, while menu_7033 regresses by 2.66 ms (10.9%). N1 graph benefits are diagnostic launch-bound results, not the owner's default N8 production result.

### ABBA run distributions

Order is A1, B2, B3, A4. Each cell lists steady p50 [min, max], in ms. Full raw order is retained in the analysis JSON. Each dense/graph run has only 12 steady samples: per-run p95 is deliberately withheld. Original sparse look-ahead runs have 24.

| Experiment / workload | A1 | B2 | B3 | A4 |
| --- | ---: | ---: | ---: | ---: |
| Look-ahead menu_7000 N1 | 32.48 [29.59, 35.34] | 33.77 [30.29, 42.78] | 29.56 [28.90, 45.19] | 33.50 [29.36, 46.65] |
| Look-ahead menu_7000 N8 | 22.69 [21.53, 26.30] | 23.01 [21.71, 27.92] | 24.40 [21.54, 28.27] | 23.71 [21.96, 26.74] |
| Look-ahead frame-55 N1 | 465.76 [456.07, 483.05] | 674.54 [642.09, 744.85] | 451.49 [437.85, 534.78] | 582.90 [550.43, 706.09] |
| Look-ahead frame-55 N8 | 293.22 [288.35, 301.13] | 248.47 [242.17, 254.37] | 259.39 [250.40, 279.37] | 323.23 [298.63, 335.62] |
| Graphs menu_7000 N1 | 31.28 [29.75, 36.84] | 27.16 [25.81, 28.44] | 28.43 [27.62, 29.50] | 33.72 [32.50, 37.44] |
| Graphs menu_7000 N8 | 25.44 [23.49, 29.25] | 23.87 [22.56, 26.25] | 25.78 [24.98, 27.73] | 24.73 [23.62, 25.59] |
| Graphs menu_7033 N1 | 30.20 [28.72, 31.18] | 25.58 [24.08, 28.22] | 26.34 [25.19, 28.19] | 33.95 [33.21, 41.47] |
| Graphs menu_7033 N8 | 24.42 [22.89, 26.94] | 26.66 [25.05, 29.99] | 27.13 [25.85, 29.68] | 24.33 [23.19, 25.62] |
| Graphs frame-55 N1 | 585.80 [512.93, 665.19] | 360.73 [335.01, 376.27] | 330.85 [328.95, 339.18] | 462.09 [440.91, 557.48] |
| Graphs frame-55 N8 | 282.87 [252.58, 320.79] | 264.11 [254.34, 283.91] | 260.10 [243.82, 274.10] | 254.52 [245.56, 264.84] |

Look-ahead dense N8 candidates remain below both baseline run ranges. N1 look-ahead is inconclusive/regresses in the pooled sample and varies sharply between runs. Dense N8 graphs are inconclusive: the OFF run medians differ by 28.35 ms, much more than the 2.72 ms pooled ON regression. Do not interpret pooled bootstrap intervals in the original exploratory analysis as independent-sample causal confidence.

### Profiled attribution

Medians after the same additional warmup exclusion; four samples/variant for each profile pair. These are event/host intervals from a separate profiled pair, not sums with the unprofiled worker table.

| Workload / comparison | Worker | Detector event | Recognizer event | Network host | Group wait | Host merge | Resident analysis |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Look-ahead menu_7000 N8 baseline | 23.38 | 4.12 | 11.43 | 8.12 | 5.16 | 0.11 | 4.63 |
| Look-ahead menu_7000 N8 candidate | 24.81 | 3.91 | 11.67 | 7.37 | 6.67 | 0.10 | 5.12 |
| Look-ahead frame-55 N8 baseline | 306.60 | 16.58 | 200.54 | 52.04 | 163.57 | 59.47 | 6.02 |
| Look-ahead frame-55 N8 candidate | 288.75 | 19.81 | 209.34 | 88.50 | 77.39 | 87.08 | 6.03 |
| Look-ahead frame-55 N1 baseline | 497.69 | 17.35 | 385.23 | 329.28 | 61.74 | 60.64 | 6.06 |
| Look-ahead frame-55 N1 candidate | 442.78 | 17.22 | 371.07 | 330.72 | 7.64 | 56.63 | 6.01 |
| Graphs menu_7000 N1 0 | 31.71 | 4.00 | 18.97 | 16.69 | 3.75 | 0.09 | 4.41 |
| Graphs menu_7000 N1 1 | 26.63 | 3.41 | 14.68 | 0.44 | 14.23 | 0.14 | 4.62 |
| Graphs menu_7000 N8 0 | 23.86 | 4.07 | 11.42 | 6.57 | 7.17 | 0.10 | 4.54 |
| Graphs menu_7000 N8 1 | 25.60 | 3.39 | 12.41 | 6.54 | 6.32 | 0.16 | 4.60 |

In dense N8 look-ahead, profiled worker time falls 17.85 ms (5.8%), while the recognizer event interval **rises** 8.80 ms (4.4%). Network host and merge also rise in that pair under shared load; the uncovered stream-gap median falls from 61.49 to 28.22 ms. This supports a feeding/overlap explanation but does not establish a fixed GPU-compute speedup. Graph menu N1 reduces the network-host interval 16.69 to 0.44 ms and recognizer event interval 18.97 to 14.68 ms; host/GPU overlap means those savings cannot be added. Dense graph runs were unprofiled; no new dense graph CUDA-event saving is claimed.

### Work sizes, graph churn and setup

- menu_7000 N1: candidate words [12]; prepared lines [10]; profiled detector passes [1], recognition samples [10], calls [10]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- menu_7000 N8: candidate words [12]; prepared lines [10]; profiled detector passes [1], recognition samples [10], calls [2]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- menu_7033 N1: candidate words [11]; prepared lines [9]; profiled detector passes [1], recognition samples [9], calls [9]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- menu_7033 N8: candidate words [11]; prepared lines [9]; profiled detector passes [1], recognition samples [9], calls [2]; image egress calls [0]. Counts are candidates, not visible/accurate words.
- frame-55 N1: candidate words [262]; prepared lines [94]; profiled detector passes [], recognition samples [], calls []; image egress calls [0]. Counts are candidates, not visible/accurate words.
- frame-55 N8: candidate words [262]; prepared lines [94]; profiled detector passes [], recognition samples [], calls []; image egress calls [0]. Counts are candidates, not visible/accurate words.

All graph capture setup records: {'n': 191, 'p50': 1.0907, 'p95': 1.7816, 'min': 0.6352, 'max': 3.4824} ms. This measures capture/instantiate host setup, not model preparation or first warm replay. Original startup and every first timed sample remain in raw jobs. There is no cold engine-build measurement. Dense N8 graph ON reported 54 recognition captures / 360 replays / 54 invalidations per network lifetime, and 18 detector captures / 18 replays / 17 invalidations: shape churn is material. N1 menu recognition reuses one capture for hundreds of calls.

Graph hash groups cover full OCR output (including geometry/confidence/dispositions), colors, prepared lines, reference flags/counts, device input and current-input agreement. Every source/capacity group has one semantic hash across OFF/ON/refusal; no candidate word, confidence or geometry difference was observed in this corpus. No ground truth or rendered/UIA acceptance follows. Original look-ahead semantic parity also holds. Every diagnostic cache-before/cache-after inventory matches.

