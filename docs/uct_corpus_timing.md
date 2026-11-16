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

Before its timed reads each process reads scene zero until the GPU reader is
ready; `startupFirstResultMs` in the report is construction to that first GPU
result. That startup read inherits nothing, so it is scene zero's true first
read in the process, and `startupWorkerMs` records its worker time. The
"first read" in the summary is the first timed read and comes after it. To
measure true first reads, run one scene per process (`--scenes`) and read
`startupWorkerMs`.

## Scene set, version 1

Eighteen scenes. Twelve are generated from this repository's documentation
text and Windows fonts: document-camera pages with perspective, blur, noise and
JPEG compression at 1920x1080 and 3840x2160 (one Turkish, one bent like a book
page), light and dark lecture slides at both sizes, a number table, a rendered
portrait page, a sparse sign scene and a frame without text. Six are real
chalkboard frames from the documented public lecture sample
(`scripts/lecture_camera_sample.json`), enlarged to 1920x1080 as the private
OBS feed presents them.

`perf_corpus.py --variant N` writes another labelled set of the generated
kinds with other text, fonts, sizes and poses, and `--bent N` a labelled set of
nine bent book pages only (one or two columns of type from 24 to 44 px, frames
from 1920x1080 to 3840x2160, bends from barely visible to stronger than
`book-curved-1080`), for checking that really curved text is not read worse.
Every row of a `--bent` page follows the same symmetric arc, the shape the
ribbon fit assumes. `--gutter N` writes nine pages curled one-sidedly instead,
as a single photographed page of an open book is: flat for most of the width,
then dropping towards the gutter on the left or right (from about half a text
height to several), with the depth varying from line to line; one or two
columns, 24 to 44 px type, 1920x1080 to 3840x2160.

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
