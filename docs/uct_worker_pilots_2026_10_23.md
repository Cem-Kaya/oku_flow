# UCT whole-worker pilots, 2026-10-23

These are three separate shared-PC pilots of scheduler worker time and
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
omission, CER/WER or sensitive-token recall. It preceded the independent book
transcription below; dense-frame and menu truth remain unavailable.
Every output retains text, confidence, disposition and geometry.
Keep these models opt-in; the pilot does not approve a default or public quality.

Evidence:
`Z:/Projects/OkuFlow/local-work-data/uct-private-model-pilot-20261023/pilot-f80/C2-20261023-172541-eea502ed5c1042b08b0e29c9b9545014/`.
Controller/child both exited zero; job membership zero and runner free.
The prerequisite root CUDA build/four tracked cases passed, but its outer125
for unknown members51948/14840 remains failed; cleanup confirmed removal.

## Small and tiny BF16 experiment

The subsequent `0d0b932` executable, SHA256
`5b431ac6cafb0ea10e6b242b798ced82bdf65e51ccb11f6bc27801daec0baa73`,
ran E=small FP32, F=small BF16-linear and G=tiny BF16-linear. The same medium
FP32 detector, N8 capacity, fixed1024 canvas, two-slot lookahead and resident
worker were used throughout. These are explicit diagnostic artifacts; no
production factory or app model policy selects them. Tiny's production default
is N1; this experiment does not approve tiny N8.

The existing pinned converter produced BF16 Conv/MatMul/Gemm islands with
FLOAT input/output from the pinned original models. New local plans used
TensorRT 11.4.0.106, optimization level 2, workspace 256 MiB and zero auxiliary
streams, with N1 minimum and N8 optimum/maximum. Actual receipts selected the
experimental BF16 payloads on RTX 4090/SM8.9, driver13050/CUDA12.9. No fallback
or inference performed by `trtexec --skipInference` is counted as measured OCR.

| Artifact | Small | Tiny |
|---|---|---|
| Converted graph SHA256 | `483b257a64da0b493e64dc58cc3827070eb11a9c7ee8ae0b1ffad7a61aeb03f1` | `7fce45ba34121940382e59fd6a81c78ffab85d80b3fd51d01a9b66c5372f31ad` |
| Raw plan SHA256 | `bb03abf694b4fd9bff8b6434a3507f2a5aa2cb32b48bfb0918f5be9214519929` | `30b32be3328297cf4049c6ed21e85bde378ee64575f296a9eb35d48cd9cbcde4` |

The schedule E,F,G,G,F,E gives 32 steady samples per arm/source after the same
preregistered exclusions. Every one of 384 successful outputs, including
startup/warmup/excluded passes, matched repeated outputs within its own model
byte-for-byte. Cross-model equality was not required. Worker milliseconds,
median / nearest-rank p95:

| Source | E: small FP32 | F: small BF16 | G: tiny BF16 |
|---|---:|---:|---:|
| Book | 52.15 / 56.24 | 41.00 / 44.00 | 30.57 / 33.69 |
| Dense frame 55 | 189.33 / 214.99 | 153.70 / 174.68 | 104.85 / 113.10 |
| Menu | 29.54 / 33.61 | 26.59 / 29.40 | 22.48 / 26.25 |

Versus this campaign's small FP32 control, small BF16 reduced median worker
time by 21.38%, 18.82% and 9.97%; tiny BF16 by 41.39%, 44.62% and 23.88%.
These are descriptive results from two reversed visits, not promotion criteria
or uncertainty intervals. Do not compare against the earlier medium timings
as though every model shared this executable/campaign. Raw callback intervals
are retained separately; neither metric establishes time to painted glyphs.

Evidence:
`Z:/Projects/OkuFlow/local-work-data/uct-small-tiny-bf16-20261023/pilot-0d/C2-20261023-182023-3a1dfc2404d949fd96b4ac479722e8b5/`.
Controller/child both exited zero; owned membership was zero and runner release
verified. Ordinary/private cache inventories were unchanged. Original graph,
configuration, converted graph, payload, executable, loaded runtime and active
source pixels were pinned. The initial engine-build failure remains preserved:
`workspace:256MiB` was parsed as 256 bytes. Correcting the command to `256M`
built the unchanged graphs; it was not evidence of a model convolution defect.

## Independently transcribed book reference

The visible foreground contains **550 printed words**: 261 on the left page,
289 on the right, including four heading words. An independent image-only
transcription covered all 52 physical lines; the root reconciled the plural
“achievements” against the source pixels. Remaining uncertainties concern
punctuation/bracket shape and do not affect this normalized score.

Scoring joins the two physical line-wrap hyphenations and excludes two Roman
pagination markers. NFKC/case folding and non-alphanumeric removal ignore case
and punctuation. Hypothesis words are partitioned into left/right pages by
x-centre555, preserving their supplied order within each page, then aligned by
minimum word edit distance. Equal-cost paths prefer the diagonal, then deletion,
then insertion. The substitution/deletion split is alignment-dependent.

| Recognizer | Normalized returned words | Substitutions | Missing | Extra | Total word edits / 550 |
|---|---:|---:|---:|---:|---:|
| Medium FP32, earlier pilot | 420 | 131 | 131 | 1 | 263 (47.82%) |
| Medium BF16, earlier pilot | 419 | 129 | 132 | 1 | 262 (47.64%) |
| Small FP32, both pilots | 412 | 151 | 139 | 1 | 291 (52.91%) |
| Small BF16 | 410 | 148 | 141 | 1 | 290 (52.73%) |
| Tiny BF16 | 437 | 214 | 114 | 1 | 329 (59.82%) |

These are **pipeline word-edit errors**, including missing text and within-page
ordering. They are not spelling-only errors, recognizer-only accuracy,
sensitive-token recall or errors in painted text. All raw word dispositions are
included; admission and native paint were not replayed. Medium's raw 424/422
records include punctuation-only entries removed by normalization. More returned
words do not establish better accuracy: tiny returned more words but had more
substitutions. One difficult photograph is not a general quality acceptance set.

The root/independent transcriptions, exact normalization/alignment script,
per-page edits, raw-output hashes and interactive comparison are retained in
the two pilot evidence directories and their parent roots. The newer
`book-ground-truth-score.json` and `small-tiny-bf16-results.html` report the
small/tiny results. The earlier multiset-difference counts remain differences,
not errors, and are not substituted for this reference.

Tiny has 6,904 configured dictionary entries versus 18,708 in small/medium
(6,906/18,710 output classes with the added blank/space). Its dictionary omits
Japanese kana and many CJK characters. This English-photo experiment therefore
cannot establish equivalent script coverage, even if a speed result is useful.

## Model policy and validation limits

The owner explicitly held the proposed fast-live/accurate-Freeze split.
**No adaptive switching, model default or Freeze text-retention policy was
implemented.** Opus's design feedback recommends measuring medium BF16
end-to-end before a split, testing painted and sensitive-token errors, avoiding
model cold-load at Freeze, and preventing a downgrade on unchanged pixels after
Resume. Its report predates the new small/tiny timing results and supplies
design advice, not an accepted implementation or quality threshold.

The `0d0b932` real CUDA/TensorRT diagnostic compilation and four focused cases
passed (4/249, zero skips/unresolved). Its outer controller125 for unknown owned
member53324 remains failed, with exact-job cleanup to zero and runner release.
The integrated `oku_flow`, MainWindow fixture and controller/model fixtures also
compiled. An evidence parser then failed on RC's `/fo` syntax; that first
failure remains preserved. A parser-only continuation did not rebuild or retry
a failed test: its headless selection passed (4/249, zero skips or
unresolved), including the immutable controller suite. Its child exited zero;
outer125 for unknown member58884 remains failed after cleanup/release. No claim
is made about those members' identities.

Build/check evidence is under
`Z:/Projects/OkuFlow/local-work-data/uct-small-tiny-bf16-20261023/experiment-build/`
and `Z:/Projects/OkuFlow/local-work-data/uct-integrated-app-20261023/`.
Rebuilt native GUI checks have not run; earlier GUI passes do not cover these
new bytes. Public V2's model-free storage contract passed separately, but actual
public model build/golden/promotion, width/detector experiments, integrated
end-to-end latency, hardware coverage and the complete release matrix remain
pending. Public BF16's production corpus catalog stays empty.
