# Narrower UCT recognizer inputs

Status: opt-in development implementation; measured acceptance is recorded below.
`OKUFLOW_UCT_TRT_WIDTH=bucketed` selects the experimental TensorRT policy.
An empty value or `fixed1024` retains the existing path and cache identities.
Model tiers, detector coverage, FP32 precision and publication safeguards are
unchanged. No model, SDK or dependency is downloaded or bundled by this option.

## Work and ownership

The recognizer previously processed a 1024-pixel canvas for every crop, even
when most of it was padding. The new policy keeps the existing contiguous
submission groups and chooses 128, 256, 512 or 1024 from the largest useful
width in that group. It never reorders crops or clips additional useful pixels.
Invalid or oversized width metadata retains 1024. The maximum in a group can
still be large: individual padding fraction does not predict the speedup.

The device computes at most 144 group widths (576 bytes) beside the existing
detector descriptors. Those integers return with the existing count transfer
and completion wait. There is no additional wait, image download, CPU crop
preparation or second OCR pass. Frozen diagnostics use already owned metadata.
Batch-one queued subcalls share a canvas until their existing group wait;
output steps, CTC strides and geometry use the actual completed network shape.
Input leases, cancellation and allocation-safe retirement/quarantine retain
their existing contracts.

Dynamic profiles can change tactic availability and memory requirements;
[NVIDIA's profile guidance](https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/dynamic-shapes-basics.html)
is another reason to measure the complete path rather than infer speedup from
the removed padding.

One dynamic profile per selected capacity uses MIN 1x3x48x128,
OPT Nx3x48x512 and MAX Nx3x48x1024. Deserialized dimensions must match exactly.
The versioned width-policy cache tag includes every bucket/profile dimension
and N, in addition to the pinned model, device UUID, driver and TensorRT
identity. Fixed-width plans remain untouched. N8 and N16 maximum input sizes
remain 4.5 MiB and 9 MiB; high-water allocations are retained, so this does not
promise lower peak VRAM. Existing output/workspace ceilings remain unchanged.

## Sorted grouping (separate opt-in, development)

`OKUFLOW_UCT_TRT_GROUPING=sorted` additionally permutes each region's crops
into ascending useful-width order before the existing contiguous groups are
formed, so a group's canvas is set by similar-width crops instead of the
widest incidental neighbour. It requires `OKUFLOW_UCT_TRT_WIDTH=bucketed`;
any other grouping value, or sorted without bucketed, makes the TensorRT
backend unavailable with an explicit selector error. The default remains
detector-ordered execution (`ordered`), and fixed1024 is unaffected: grouping
cannot narrow a fixed canvas. The reviewed local BF16 selection requires
fixed1024 and therefore never combines with sorted grouping.

Sorting changes execution order only. The worker downloads the region's
compact quad descriptors once after the existing detector-count wait (no new
synchronization, no image transfer), computes each sorted group's canvas on
the host with the same invalid-width rule, and skips the per-batch quad
downloads. Every crop keeps its own quad, a canvas at least its useful width
(invalid widths keep the full canvas) and its original output slot: decoded
lines are buffered per original crop index and merged, descriptor-hashed and
captured in detector order through the inverse permutation. Cancellation
between groups, per-crop token-count refusals, input leases and retirement
ownership are unchanged. The profiler records `recognizerGrouping` beside
the width policy; the status detail names the sorted experiment.

Quality impact is the same class as bucketed canvases — cross-canvas
TensorRT numerics — never clipped useful pixels or reduced coverage. It must
be measured as three separate arms on identical sources: fixed1024 baseline,
existing unsorted bucketed, and sorted bucketed, so gains from enabling the
existing width selector are not credited to sorting. Only already-cached
dynamic-width plans may run (medium recognition N1/N8 at the time of
writing); no plan is prepared by this selector. Report changed/missing/extra
words, dispositions, confidence and geometry against the fixed baseline,
with long-line (no-gain), short-word, mixed-width, small-font and tilted
controls; baseline agreement is not ground-truth accuracy. Model-free
fixtures cover the permutation/inverse mapping, concrete group canvases, the
all-long control and fail-closed selector combinations; they establish no
inference speed or quality result.

## Preparation and validation

Missing width plans refuse inference before loading builder/parser resources.
An explicitly selected, separate `OKUFLOW_UCT_TRT_PREPARE_WIDTH=1` child may
prepare the requested capacity once using the installed full SDK. It does not
cascade into preparing smaller profiles. Clear this variable for measurement.
The normal inference-only SDK cannot prepare plans. No cache clearing,
renaming fixed plans or borrowing another GPU's plans is permitted.

The resident diagnostic accepts `OKUFLOW_UCT_RESIDENT_WIDTH=fixed1024|bucketed`
and refuses preparation. Compare the same sources, model pair, capacities,
rounds/passes and seed, retaining fresh cache inventories and exact retirement
receipts. Frozen runs must retain identical ordered crop hashes and zero timed
detector calls. Complete-path runs separately include detection and resident
result preparation. Upload, startup and warmup are outside steady timings.

The original strict 0.001-pixel/confidence comparison remains visible. It is
not the cross-width quality decision. The owner's later tolerance permits up
to 5% word-level recognition difference; report all edits and denominators,
dispositions, omissions and geometry separately. Known-text sources support
word-error comparisons; video baseline agreement is not ground-truth accuracy.
Confidence variation alone is accepted; source-edge drift remains bounded by
the separately authorized ten-pixel policy. Keep disagreements and failed runs.

Five ordinary model-free fixture slots cover scalar bounds/group ordering,
CPU reference pixels/padding, exact profile extents, actual device group widths
and CUDA rectification across nonmonotonic widths using reused storage and
stride guards. Real prepared-engine inference must additionally exercise
dynamic output steps, token strides, recognition quality and normal retirement.
Neither these checks nor RTX 4090 measurements establish 120 FPS or RTX 3050
performance. Native GUI/screen-reader acceptance is separate.

## Measured result

RTX 4090, driver 617.42, TensorRT 11.4.0.106, CUDA 12.9.86 and pinned medium
detector/medium recognizer, capacity eight. Six unchanged sources were tested
with three seeded rounds and three timed passes per source/configuration.
Frozen crop work and source hashes matched across policies. Two additional
complete-path runs used the three video frames, one round and three passes.
Uploads, first inference and per-source warmups are outside these medians.

| Source | Fixed frozen worker, ms | Bucketed frozen worker, ms | Fixed complete path, ms | Bucketed complete path, ms |
| --- | ---: | ---: | ---: | ---: |
| Problem video, 55 s | 217.297 | 182.161 | 234.279 | 216.668 |
| Problem video, 89 s | 216.485 | 183.260 | 224.055 | 206.454 |
| Earlier dense frame | 152.178 | 149.437 | 174.524 | 177.790 |
| Sparse print | 9.064 | 8.562 | — | — |
| Long lines | 71.449 | 74.147 | — | — |
| Critical symbols | 44.558 | 46.297 | — | — |

Narrowing improved the two problem frames by about 15–16% with frozen crops
and 7.5–7.9% on the complete path. Long/critical text became about 4% slower,
and the earlier dense complete path about 2% slower. The policy is therefore
available opt-in; these results do not support a universal speedup or automatic
default promotion. Nine frozen observations per cell and three complete-path
observations are descriptive medians, not tail-latency or significance claims.

All known-text candidate tokens were unchanged: sparse truth WER 0/13,
long lines 1/105 and critical symbols 5/80 before and after. These small synthetic
sources are not representative general accuracy. Video candidate baseline word
edit rates were 13/266 (4.887%), 12/249 (4.819%) and 6/178 (3.371%) in all nine
paired observations of each source. This meets the owner's later 5% difference
allowance, not a 95%-accuracy claim. Changes include symbols, token boundaries,
omissions and numeric text: for example `0/6` became `0/8`; a room code expanded
by three trailing digits. Neither policy is assumed to be ground truth.

Original per-run N1/batch strict assertions remain failed and their normal
exit-one outputs are retained. Cross-width original strict comparisons also
remain separate. Positional comparison can pair different words after a token
change (for example `Revoke` versus `watch`), so its 36-pixel maximum is not
reported as an identical word moving. Full per-word records are retained.
An additional exact-text, protected-envelope-IoU >= 0.5 one-to-one occurrence
diagnostic found maximum common-word edge changes of 7.0, 7.4925 and 4.0323
pixels on the three video frames. It keeps unmatched words, order changes and
candidate/protected omissions visible; it does not convert the old
exact-structure ten-pixel policy into a pass.

Exactly two new medium recognition plans were prepared once. All existing
plans remained byte-identical. Timed runs used the inference-only SDK and
unchanged cache inventories, with exact State retirement and zero image egress.
The initial N1 preparation command had a log-argument error: its actual model
inference and retirement passed, while two nonexistent test names failed.
That original exit-two log is retained; the later cache-only resident N1 blocks
completed normally. No cold preparation was repeated to replace that evidence.

Private raw logs, receipts and source/output trees are indexed in
`%LOCALAPPDATA%/OkuFlow/uct-recognizer-width-20261021/`; sources and plans are
outside Git/release payloads. `evidence-index.json` and
`verified-run-counts.json` bind six completed runs, 252 timed reads (216 frozen,
36 complete-path) and 28 completed exact State closures. The offline standard-
library comparison tools, invocation instructions, all word differences and
tool hashes remain in the sibling private
`agent-patches/uct-cross-width-compare-20261021/` package. They are evidence
analysis tools, not application dependencies. The main comparator SHA-256 is
`863a4e6465079be0d5421c4546bf07db91057633fc36833e176a9c5b7c2f3b3d`.
Both the author and coordinating agent executed its 25 synthetic tests;
hash, lease, cache, duplicate/missing-pass and exact 5% boundary refusals pass.

## Integration checks

The tracked background matrix passed shipping Release compilation, 151 CPU
cases (five contain optional skipped Qt rows) and 17 CUDA delta cases, with
zero whole-case skips or unresolved results. This includes the five new width
slots in the ordinary Paddle selectors. `final-background-matrix.log` retains
the matrix. With precompiled headers disabled, rebuilt `uct_paddle_processing`
and `shared_source_equivalence` passed separately (two cases, no skips), retained
in `width-pch-off-build.log` and `width-pch-off-gate.log`.

An independent Sol source review found no concrete lifetime, stride, bound,
cache or retirement issues; it did not execute models. Prepared-engine evidence
here covers medium recognition N1/N8 only. Small/tiny, N16, native GUI,
screen-reader delivery, other GPUs and driver-failure injection remain separate.
Desktop tests were excluded: this is partial validation, not full release or
installer acceptance. The report-only Astra cleanup recommendations were not
implemented by this work.
