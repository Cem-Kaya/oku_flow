# UCT corpus timing and output comparison

Status: development measurement tooling, 15 November 2026. Nothing here changes
application behaviour, and none of it runs in ordinary CTest.

## What it measures

`optionalCorpusTiming` in `uct_paddle_tests` reads a list of image files through
the shipped GPU factory (`CreateProductionTensorRtRecognizer`, BF16 preference
with same-tier FP32 fallback) and the real `RecognitionScheduler::SubmitDevice`
worker. Each image gets one first read and then the requested warm reads, each
on a fresh owned analysis capture whose copy completes before submission. The
reported time is the scheduler worker duration: detection, recognition, merge,
resident reference/colour/ink preparation and line preparation.

It does not measure camera capture, decoding, the presenter, admission, paint
or accessibility publication, and it runs without display or decoder load.

## Tools

| Tool | Purpose |
| --- | --- |
| `scripts/uct/perf_corpus.py` | Builds the local scene set outside the repository |
| `tests/uct_corpus_timing.hpp` | The opt-in slot; one process reads every selected scene |
| `scripts/uct/perf_measure.py` | Runs arms in reversed cohorts, compares timing and complete output |

Selectors for the slot (all private test inputs):

| Variable | Meaning |
| --- | --- |
| `OKUFLOW_UCT_CORPUS_TIMING=1` | Required gate; otherwise the slot skips |
| `OKUFLOW_UCT_CORPUS_IMAGES` | Semicolon-separated images, 1 to 64 |
| `OKUFLOW_UCT_CORPUS_EVIDENCE` | New absolute report file |
| `OKUFLOW_UCT_CORPUS_PASSES` | Warm reads per image, 1 to 50, default 10 |
| `OKUFLOW_UCT_CORPUS_PROFILE=1` | CUDA stage events; inflates timing, so never mix with timing runs |
| `OKUFLOW_UCT_CORPUS_STAMP` | `camera` (default) or `media` |
| `OKUFLOW_UCT_CORPUS_PAIR` | `detector:recognizer` tiers, default `medium:medium` |
| `OKUFLOW_UCT_CORPUS_GAP_MS` | Sleep before each read, 0 to 5000 |
| `OKUFLOW_UCT_CORPUS_ENGINE_CACHE` | Explicit prepared engine cache |

The slot never prepares an engine. Prepare the plans once with
`optionalNativePrecisionSetup` (see [model setup](model_installation.md)), then
keep them in a real directory: a junction in the cache path is refused and the
factory quietly builds FP32 plans instead. Every report records the backend
detail, so check that it names the precision you meant to measure.

## Protocol

`perf_measure.py` starts one fresh process per arm and cohort. Cohort 0 walks
the scenes forward with the arms in the given order; cohort 1 walks them in
reverse with the arms reversed. With the defaults (two cohorts, five warm
passes) each arm has ten warm reads per scene. An arm is a set of
`OKUFLOW_UCT_*` switches on the same executable (`name=KEY=VALUE;KEY=VALUE`,
separated by semicolons because values may contain commas), or a different
executable through `--arm-exe`. `--lease` wraps each process in a shared GPU lease script
and retries exit code 75.

For each scene and arm the summary gives the median of the pooled warm reads,
each cohort's own median, the first read of that scene in each process, and
the comparison with the first arm: whether the complete ordered output is
byte-identical (text, order, dispositions, geometry, confidence and colours),
words lost and gained, and the largest box-edge change when the ordered text
agrees. Scenes with a truth file are scored by exact match against the words
that were drawn. There is no confidence interval or outlier rejection.

## Paced runs

The application does not read back to back. It reads on timers (`UctTiming`:
a full read every 350 ms of advancing media, a 1500 ms refresh of a live view,
300 ms settling of a paused view), and between reads the GPU's power
management lowers its clocks. A back-to-back median says nothing about that
case, so paced runs are part of the protocol:

| Option | Meaning |
| --- | --- |
| `--gap-ms N` | Sleep N ms before every read (the slot's `OKUFLOW_UCT_CORPUS_GAP_MS`). Use 350 and 1000 next to 0 |
| `--passes 24 --cohorts 2` | 25 reads per scene and process; the effect develops over seconds, so five passes are too few |
| `--settled-from K` | Also report the median of the reads from pass K on (default 5: reads 6 onwards) |
| `--gpu-sample-ms N` | Sample `nvidia-smi` (power state, graphics and memory clock, utilization, power draw) every N ms, 50 to 1000, while each process runs; off by default |
| `--frame-load fps:passes` | Emulate the application's per-frame video GPU work during the reads: one NV12 1920x1080 upload and `passes` full-frame device copies per frame, on its own stream (`OKUFLOW_UCT_CORPUS_FRAME_LOAD`) |
| `--tail-ms N` | Keep the process idle N ms after the last read (`OKUFLOW_UCT_CORPUS_TAIL_MS`), to show that keep-warm stops and what the GPU then draws |

The summary always records the gap, the settled medians and every read's time
in order (`series` in `summary.json`, "Per-read series" in `summary.md`). With
sampling, each process keeps its raw samples next to its report
(`cohortN-arm.smi.csv`), the summary gives P-state counts, median clock, mean
utilization and mean power over the timed reads, and each read in the series
carries the P-state sampled last before it. Each read records its wall-clock
submission time (`submittedUnixMs`) for that alignment. Sampling uses
read-only `nvidia-smi` queries and stops with its process; it changes no GPU
setting.

Paced timings are sensitive to anything else that uses the GPU: other jobs on
the shared lease, a driven display, video playback. Take them under the lease,
look at the per-read series before trusting a median, and say when a run looks
disturbed.

Like the application, the slot announces each read before waiting for it
(`RecognitionScheduler::ExpectNextRead`), which is what arms the private
keep-warm of round 13; the report records keep-warm activity and the tail.

`scripts/uct/gpu_clock_probe.py` runs the model-free `optionalGpuClockProbe`
slot with `nvidia-smi` sampling: an in-kernel clock meter while the GPU idles
after load (step-down), under continuous load from a low state (step-up),
under periodic load of a chosen busy fraction (`duty`, `refine`) and under the
emulated frame load (`frames`). It needs no engine.

## Scene set, version 1

Eighteen scenes. Twelve are generated from this repository's documentation
text and Windows fonts: document-camera pages with perspective, blur, noise and
JPEG compression at 1920x1080 and 3840x2160 (one Turkish, one bent like a book
page), light and dark lecture slides at both sizes, a number table, a rendered
portrait page, a sparse sign scene and a frame without text. Six are real
chalkboard frames from the documented public lecture sample
(`scripts/lecture_camera_sample.json`), enlarged to 1920x1080 as the private
OBS feed presents them.

These are stand-ins. No owner camera capture is in the set: the owner's
existing OkuFlow captures on this workstation are lecture test frames and a
room photograph with almost no text. Results on generated pages do not
establish behaviour on real documents, boards or handwriting.

## Baseline, 15 November 2026

RTX 4090, driver 621.14, Ryzen 9 5900X, MSVC 19.44 Release, CUDA 12.9.86,
TensorRT 11.4.0.106, medium detector and medium recognizer, both BF16-linear,
recognizer batch eight, fixed 1024 canvas, source `8bc2c4a` plus this tooling.
`nvidia-smi` reported `display_active: Disabled` (display asleep or not driven).
One profiled process, three warm reads per scene; stage columns are CUDA event
intervals for the last read and include launch gaps.

| Scene | Size | Worker ms | Detector passes | Detector | Regions | Recognizer | Recognizer calls / crops | Words | Host enqueue | Host merge | Resident |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| doc-camera-1080 | 1920x1080 | 217.4 | 3 | 17.0 | 7.3 | 183.4 | 43 / 329 | 491 | 93.2 | 20.3 | 5.0 |
| doc-camera-2160 | 3840x2160 | 498.8 | 9 | 45.2 | 22.9 | 391.8 | 95 / 731 | 1199 | 142.5 | 80.7 | 12.1 |
| page-portrait | 1654x2339 | 394.4 | 5 | 20.8 | 13.3 | 328.6 | 79 / 617 | 983 | 116.6 | 56.8 | 10.3 |
| table-1080 | 1920x1080 | 147.4 | 3 | 14.2 | 4.5 | 114.9 | 28 / 211 | 118 | 44.3 | 2.9 | 6.2 |
| book-curved-1080 | 1920x1080 | 104.7 | 3 | 14.0 | 43.2 | 31.9 | 9 / 53 | 236 | 23.4 | 1.5 | 4.9 |
| slide-light-1080 | 1920x1080 | 77.4 | 3 | 14.0 | 9.7 | 43.1 | 11 / 78 | 154 | 28.3 | 2.2 | 6.0 |
| slide-light-2160 | 3840x2160 | 136.7 | 9 | 43.2 | 27.3 | 52.2 | 14 / 79 | 153 | 58.5 | 2.1 | 11.9 |
| signs-sparse-1080 | 1920x1080 | 35.9 | 3 | 14.1 | 4.8 | 7.6 | 3 / 10 | 13 | 15.3 | 0.1 | 6.9 |
| no-text-1080 | 1920x1080 | 80.3 | 3 | 13.8 | 59.9 | 5.2 | 3 / 3 | 0 | 13.2 | 0.0 | 0.6 |
| board-lecture-00 | 1920x1080 | 26.6 | 3 | 13.7 | 4.2 | 6.4 | 3 / 3 | 2 | 17.6 | 0.0 | 1.1 |

What the profile says:

- A full read of a 1920x1080 frame runs the detector three times (the whole
  frame and two overlapping tiles) and a 3840x2160 frame nine times. Text seen
  by more than one pass is recognized more than once and the more confident
  word is kept.
- On text-dense frames the recognizer is 75 to 85 percent of the worker.
- On frames with texture or without text, detector post-processing (the
  "regions" stage) can be most of the worker: 59.9 of 80.3 ms on the textless
  scene.
- On sparse real frames the three detector passes and TensorRT launch time
  dominate a 26 to 33 ms read.
- Host merging grows with word count: 80.7 ms on the dense 3840x2160 page.
  Round 7 of [the OkuRead port record](uct_okuread_port.md) keeps the merge
  state across one read and brings that to about 15 ms with identical output.

An A/A run of the unchanged build (two cohorts, five passes) gave a geometric
mean of 0.990x with identical output on 18 of 18 scenes. Individual dense
scenes moved by up to 2 percent and short sparse scenes by up to 12 percent, so
smaller differences than these are not evidence of a change.

Evidence is under
`Z:/Projects/OkuFlow/local-work-data/perf-okuread-port/evidence/`
(`r0-baseline-profile-2`, `r0-baseline-aa-1`).
