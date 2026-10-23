# Exact host UCT merge: CPU replay results, 2026-10-23

Frozen implementation `c3e3fea7321aa251d9c341366a2714c5eb3e67e1` passed a
**SCOPED 3-of-235 headless CPU gate**, all-field replay and a separately
authorized paired function microbenchmark. The constructed 383-word book
visit median fell from 136.073150 to 3.239800 ms; the regrouped archived book
median fell from 169.030000 to 5.312200 ms.

These are sums of merge-function microtimes over constructed or regrouped
sequences, **not actual OCR call traces, worker latency, CPU fallback speed,
GPU/BF16 throughput or FPS**. Background MCP/unknown Python could be active.
An isolated system was not measured; raw visits, blocks and deltas describe
this run only. The [implementation and arithmetic boundary](uct_merge_optimization.md)
remain unchanged by this report.

## Frozen identity and execution scope

- Oracle base: `bb53ac884c24d15f0c77b2dc43cace0e8e650c24`.
- Measured implementation: `c3e3fea7321aa251d9c341366a2714c5eb3e67e1`.
- Canonical implementation patch SHA-256:
  `b12c43adb1d3cf7a9db7f4a7945ff98d8e6546de66c27ae2071dd59ee1e219ba`.
- Root-owned Window 2: `2026-10-23T14:25:59.7464747Z` through
  `14:28:33.0041498Z`; controller/child exit 0. Its grant is closed.
- Limits: 900 seconds total, 870 seconds work plus 30 seconds cleanup,
  eight CPU jobs, unchanged 240-second in-process replay/benchmark cap.
- Short private build: `Z:/Projects/OkuFlow/local-work-data/merge-c3e3fea/cpu`.
  No application/source/test/baseline/build-definition edits were made for
  Window 2. This subsequent commit changes documentation only.

The external controller created the child suspended, assigned it to its
non-inherited kill-on-close Windows job, then resumed it. Cleanup requested
termination of two remaining owned helpers and observed zero active job
members, an empty PID list and root exit before an independent exclusive
runner-lock probe succeeded. No owner or foreign process was stopped.

`agent_check.ps1 -Base bb53ac8…` returned the expected **FULL-required exit 2**
because validation infrastructure changed; `-AllowFull` was not supplied.
This retained refusal is not a passing full gate. Under the separate root
grant, only `uct_merge_replay_tests` and `uct_paddle_tests` were built.
The exact tracked gate used `-Parallel 8 -ExcludeGui -Resume:$false`:

| CTest case | Result | Seconds |
|---|---|---:|
| `uct_merge_replay` | PASS | 1.34548 |
| `uct_paddle_processing` | PASS | 1.94475 |
| `shared_source_equivalence` | PASS | 16.22070 |

The gate reported 3 passed, zero skipped Qt rows, zero skipped tests and zero
unresolved cases. The Paddle Qt log records 43 passed, zero failed/skipped.
Assertions were unchanged. The mandatory shared-source guard was added to the
external selection before launch; it did not broaden compilation targets.
No GUI, camera/audio, model preparation/download, inference or GPU workload ran.
The full shipping/CPU/CUDA matrix, PCH-ON validation and release acceptance remain
outside this scoped result.

## Compiler and source-to-binary receipts

Both arms were compiled into the same executable in separate translation
units, with MSVC 19.44.35229.0 from VS2022 Community toolset directory
`14.44.35207`, Qt 6.12.0 `msvc2022_64`, Release/Ninja, C++20, `/O2 /Ob2`,
`/MT` and `/DNDEBUG`. CUDA and Text SR were OFF; both project PCH and global
precompiled headers were disabled. Recorded target commands contain no
`/fp:fast`, `/GL` or `/arch` switch. No compiler/toolchain variant was compared.

The private archive independently matched all 17 frozen working-file pins,
all five recorded compiler/CMake/Qt file identities and 21 compiled-object
fingerprints. It retains compile commands, cache, Ninja graph/log, configure
log, binaries, source snapshots, tracked ledger/JUnit/detailed logs, actual
executed controllers, raw reports and serialized inputs.

| Artifact | SHA-256 |
|---|---|
| MSVC `cl.exe` | `fe251ef50a1545b1b0835ee17b1e785459712b38d79e45b5c1d3d28970a36619` |
| CMake cache | `1662e6dcd936f39b5516927f4337dfdc43b9d319a2d9eaf5cb92dc334b03e5fd` |
| Replay executable | `3ffc5ee3b265c94a27fdb3e32af3fbcd81e5a0f8a7bbf131150f42c0445aa793` |
| Paddle executable | `b642d0a0a57689bb538eb6c6fbbb9580133aec4fe155109fa7d4682c5f965cbd` |
| Serialized input bytes, both runs | `e34def4fa498be50aee3ffec20310d5537e7677d07407bf94b53deb110a44585` |
| Paired raw report | `2222a93c85906b1da9ff62e19dae9b39924d234a2784ec532e77cd54560faba5` |
| Independent statistics | `e4171acd1a2fd89608f661e5f8df7f1bc9d70fa50ef44fd61879f96aa51a9d59` |
| Archive manifest | `7df351b8b56655ab2fe51acb0fc86c5ee895fd3288097c8d056e4eb0a8ae176a` |

The measured production working-file hashes are
`7f965ff82067d6a2da37f0d41c068e74ed9b7bb90d9524c0d7e8b4b1e5e56fd9`
for `uct_paddle_processing.cpp` and
`280f5c7d94bf5e2a89fd84986f5fd8e801ed8c6650ec03ef1187c9a54da68a37`
for `ultra_clear_text.cpp`. The oracle source files are archived verbatim.
Compiler, Qt and floating-point environment changes require fresh validation;
finite replay does not prove bit equivalence for every possible input/platform.

## Correctness and quality fields

Both paired and frozen-input reports have `complete=true`. All 1,816 paired
visits passed `correctAfterEveryCall`, covering 98,221 checked merge calls.
The separate serialized-input replay passed 74 visits/803 calls. Both
8,755,276-byte serialized input files hash identically and the replay required
byte-identical serialization roundtrip. The 74 coverage entries and their
expected all-field output digests also match between reports.

Every call compares ordered rows/words and all 11 `RecognizedWord` fields:
`text`, `boundsPx`, `paddedBounds`, `inkSearchPx`, `disposition`, `confidence`,
`protectedBoundsPx`, optional foreground/background `colors`, `quadPx`,
`inkSearchQuadPx` and `inkFitted`. Rectangle scalars and every quad point use
raw double bits, including signed zero/NaN payloads; both quad validity flags
and optional-color presence are included. Padding bytes are excluded.
Geometry controls independently compare the frozen area/intersection routines.

Controls include threshold/adjacent-double boundaries, first-target and later
veto behavior, rejected-word preference, confidence/disposition ties and NaNs,
same-call cross-row deletion/empty reuse, stable sort ties, crop-edge translation,
angle wrap, malformed/overflow quads, line cap and 8191/8192/8193-word cache
boundaries. The capacity-only fallback condition with spare vector capacity
exceeding 8192 while live word count remains below 8192 was not separately forced.

This checks merge preservation, not new OCR accuracy or rendered readability.
Text fragment/line matching, validity/threshold policy, CTC confidence, quality
budgets, look-ahead/integral behavior and source/acquisition/freshness policy are
unchanged. No new dependencies or licenses were introduced.

## Timing method and descriptive results

There are 13 timed cases, each with five warmups per arm followed by 31
four-visit blocks alternating ABBA/BAAB. A is frozen bb53, B is c3. This yields
62 timed visits per arm/case, 1,612 timed visits, 403 paired blocks and 806
adjacent pairs. The other raw records are 74 correctness visits (block -2)
and 130 warmups (block -1), excluded from statistics.

Timing surrounds only each merge call; a visit value sums those call times.
Initial-state copying, expected-output generation, snapshots, serialization and
comparison are outside timing. A block value is the mean of its two visits
per arm; a block saving is mean(A) minus mean(B). Adjacent pairs use positions
(0,1) and (2,3). Positive means A took longer. Median uses the middle value(s);
p95 uses nearest rank `ceil(0.95*n)`. No timed outlier was removed.

All values below are milliseconds. “Block saving” is a paired statistic,
not the difference between the two visit medians. Independent offline arithmetic
matches every median/p95 in root's retained descriptive summary.

| Workload | Visit median A / B | Visit p95 A / B | Block saving median / p95 | Positive blocks; pairs |
|---|---:|---:|---:|---:|
| Generated book, 383 words | 136.073150 / 3.239800 | 159.838400 / 3.538900 | 132.886750 / 156.190200 | 31/31; 62/62 |
| Generated frame55, 262 words | 60.488500 / 1.930850 | 86.862500 / 2.652400 | 58.808550 / 78.244450 | 31/31; 62/62 |
| Generated atlas, 175 words | 26.909100 / 1.024500 | 32.109000 / 1.251800 | 26.027950 / 29.043400 | 31/31; 62/62 |
| Generated high density, 768 words | 508.958250 / 9.356750 | 599.741500 / 12.535100 | 503.560800 / 550.653900 | 31/31; 62/62 |
| Regrouped archive book_001 | 169.030000 / 5.312200 | 183.705100 / 5.850800 | 164.160100 / 190.978550 | 31/31; 62/62 |
| Regrouped archive frame55 | 75.912450 / 4.744850 | 93.002600 / 5.260800 | 71.050800 / 79.536200 | 31/31; 62/62 |
| Regrouped archive atlas_0 | 33.706650 / 1.840600 | 44.213000 / 2.239100 | 31.905450 / 44.322800 | 31/31; 62/62 |
| No overlap | 2.348600 / 0.595200 | 2.527300 / 0.665200 | 1.757800 / 1.865800 | 31/31; 62/62 |
| Rotated overlap/independent lines | 0.053900 / 0.014900 | 0.068400 / 0.026800 | 0.039050 / 0.050500 | 31/31; 62/62 |
| Fragments/cross-column | 0.021600 / 0.005650 | 0.038300 / 0.013500 | 0.015950 / 0.025950 | 31/31; 62/62 |
| Duplicates/confidence/preference | 0.023600 / 0.006700 | 0.043500 / 0.013300 | 0.017050 / 0.027150 | 31/31; 62/62 |
| Thresholds/nextafter | 0.495300 / 0.096800 | 0.621700 / 0.121000 | 0.411900 / 0.521100 | 31/31; 62/62 |
| Detail-edge translation | 0.003800 / 0.002500 | 0.004100 / 0.003000 | 0.001350 / 0.001600 | 30/31; 61/62 |

Every case has a positive median saving. The detail-edge case is not positive
in every pair: block 23 (BAAB) averages A=3,650 ns and B=3,700 ns, a -50 ns
saving; its second adjacent pair is A=3,700 ns and B=4,900 ns (-1,200 ns).
There are 402 positive/one negative blocks and 805 positive/one negative pairs,
with no ties. Positive recorded call times have a 100 ns greatest common
divisor; this is observed quantization, not calibrated timer resolution.
Timer overhead was not subtracted. Tiny-case deltas deserve particular caution.

The archive cases contain final recognizer rows (book 426 words, atlas 188,
frame55 291), regrouped into calls and followed by an identical second pass.
Original call order, quads and `inkFitted` were absent; missing fields retain
defaults. Generated cases and these output-derived sequences are not captured
production calls. Their microtimes cannot be substituted for historical
49–53 ms BF16 book merge samples or 59–78 ms dense FP32 samples obtained under
different look-ahead conditions.

Ten phase snapshots each recorded 14 known MCP Python control processes and
zero unknown Python processes. Snapshots do not observe every instant or prove
idleness. The root explicitly authorized shared-desktop functional tests and
CPU microtiming with unknown Python recorded rather than refused solely by
image. Native compiler/project-test/foreign-runner conflicts still refused.
No statistical significance, cross-machine generalization or isolated-system
performance claim follows from these sequential, shared-desktop samples.

## Pipeline ceilings and remaining evidence

The code is host work used by the GPU recognizer pipeline, but this run excludes
recognition, GPU work, capture, scheduling, queueing, paint and display. For an
otherwise unchanged *serial* worker with old total W, critical-path merge M and
merge speed ratio r, the conditional total is `W - M + M/r`, with an infinite-r
ceiling of `W/(W-M)`. Neither current W nor critical-path M was measured here.
Look-ahead/overlap can further reduce how much merge time lies on that path.

For scale only, historical 49–53 ms merge within a roughly 95 ms worker would
give an idealized elimination ceiling of about 2.1–2.3× under that serial model.
Those historical samples span different look-ahead conditions and are not
current fractions or a forecast. The much larger function ratios implied by
this table provide no measured whole-worker, BF16/GPU or FPS multiplier.
A matched real-call trace and separately granted current full-worker/GPU run
are needed before estimating that benefit.

## Private evidence and first refusal

All paths below are under
`Z:/Projects/OkuFlow/local-work-data/uct-review-implementation-20261023/merge/`:

- Original successful run:
  `cpu-20261023-142559-7991f0a9841e453c8942b5c4a507e0fd/`.
- Evidence copy: `window2-evidence-archive/manifest.json` inventories 74
  files/28,042,502 bytes; `source-to-object.json` records 21 source/command/object
  fingerprints (SHA-256
  `42926cd0adb26729b5011660e1c1b4d4d0162861602617da8930e767598ab9df`).
- `window2-evidence-archive/analysis/` retains the standalone analysis script,
  complete statistics, 1,612-row visit CSV, 403-row block CSV and 806-row
  adjacent-pair CSV. Per-call raw nanoseconds remain in `run/paired.json`.
- `tracked-gate/` preserves ledger-v2, JUnit, gate log and the detailed
  `20261023-172638-282-ca4e0254-LastTest.log`, rather than the later overwritten
  build-tree `LastTest.log`.
- `pinned-input-receipts/` retains all three original hash-verified final-output
  receipts. OCR text, serialized data, binaries and opaque toolchain environment
  receipts remain private, outside Git/release payloads.
- The first strict Window 1 receipt/log and exact executed scripts remain in
  `window1-preflight/` and their original run directory. It exited 1 at preflight
  on unclassified `python.exe:58272` at 13:56:23–13:56:26Z, before any
  configure/build/test. That process's provenance/workload is unknown.
  No inference or application-fault conclusion is supported.

Window 2 had a fresh root grant after explicitly authorized short-path,
Python-contamination and actual-runner filtering repairs. Neither window
automatically retried an application/test failure; Window 1 never reached a
target and Window 2 passed its selected gate. No rebuild, test, job API, lease,
GUI or GPU action was performed for the subsequent offline analysis/report.
