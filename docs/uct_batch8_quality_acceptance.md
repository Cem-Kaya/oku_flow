# Batch-eight quality decision; model-default guard

The owner explicitly accepted source-box differences up to 10 pixels and
confidence variation on 2026-10-20, after reviewing the earlier strict failures.
This is a separate quality policy; the historical 0.001-pixel/0.001-confidence
comparison, raw failures and final diagnostic assertion remain unchanged.
Actual text, order, token dispositions and padding must still agree. Every
fitted/protected/search box edge must remain within 10 source pixels. Nonfinite
values, changed text and candidate-to-omission changes fail the new comparison.
No recognition confidence threshold or display-admission rule is reduced.

## Recheck of retained resident outputs

All 27 small-recognizer N8 trials across the three existing sources retained
their own N1 output's exact text/order/dispositions/padding. The largest recorded
coordinate/extent difference was 2.94056 pixels and confidence difference
0.015157. The original strict failures remain real; this decision does not
rename them passing strict tests.

The N16 candidate count drop was **a microphone icon**, at approximately
source `(520,976,13,11)`, not a readable word. Small N1 misread it as `(0` at
confidence 0.786915; N16 returned the same string at 0.595176 and marked it low
confidence, preserving its source ink. Small N8 retained N1's disposition.
This diagnostic counts pre-admission OCR candidates, not final displayed words.
N16 remains an explicit experiment rather than the default.

A manual, non-blinded check transcribed 23 readable spatial areas (731 characters
excluding whitespace) from the original dense image. Medium/N1 made 10 edits;
small/N1 and small/N8 made four each. Shared errors included duplicate separators
and a UI chevron attached to time text. The initial request-area scoring crop
excluded the visible `3d`; that report is retained and the corrected-region
report includes it. Neither model lost those source characters. This is one
scene, with icons and some truncated fields unlabelled, not general accuracy.
The earlier synthetic sparse/long sources also remain distinct limited evidence.

## Development defaults

GPU model defaults remain medium detection and medium recognition. The new
critical-symbol fixture exposed small recognition's `O0` → `00` serial-token
error, while medium retained `O0`. Small had fewer total character errors on
that fixture (six versus seven), but its sensitive-token recall was lower
(0.9245 versus 0.9434). Under the roadmap's explicit critical-symbol go/no-go,
that blocks changing the model default. Both models still make OCR errors.

Medium and small TensorRT recognition use capacity eight when no explicit
override is set; tiny retains capacity one. Existing saved model preferences
and explicit `OKUFLOW_UCT_TRT_BATCH=1/8/16` overrides win. Batch-eight preparation
can fall back to one, and unsupported/failed GPU operation retains CPU/Windows
fallback. CPU model defaults remain tiny detection/small recognition.

No installer or optional model is published by this decision. The measured
269.54-to-85.07-ms dense worker comparison is still specific to the RTX 4090
resident diagnostic, not display latency or a guarantee across GPUs/documents.

## Evidence

Read-only original output root:
`%LOCALAPPDATA%/OkuFlow/uct-resident-20261020-175106-94b2d99b-ae60-4887-98ea-dc0012300fdc`.
New audit JSON, source-image highlight and corrected spatial labels:
`%LOCALAPPDATA%/OkuFlow/uct-quality-promotion-20261020/`.
The highlight draws on an evidence view of the original pixels; it is not a
modified application capture or a claim that the app rendered those rectangles.
Private source images, fonts, plans and raw outputs are not Git/release inputs.

Opus xhigh was requested again for independent review but the provider returned
its API rate limit before doing work. This is root's measured/manual quality
decision under the owner's stated tolerance, not an Opus sign-off.

The completed frozen-crop follow-up added an independent symbols/accents image,
retained four exact image leases and produced 96 timed jobs. All had zero detector
calls, zero image egress and identical ordered crop hashes per source. All 88
comparisons passed the separate user policy; 18 failed the retained strict policy.
All eight State closures completed, plans stayed byte-identical and the process
exited normally with code one for that strict assertion. See the full
[frozen results](uct_resident_baseline.md#completed-frozen-crop-run).


Validation: the tracked background matrix passed shipping Release compilation,
108 CPU cases (two with optional skipped Qt rows) and all 17 CUDA delta cases,
with no whole-case skips or unresolved failures. The earlier scoped CPU gate
passed 14 cases and the scoped CUDA processing/kernel gate passed. GUI checks
remain pending for this commit on GitHub's disposable Windows VM; the owner's
desktop was not driven. This is development acceptance, not a complete release
bundle, real-camera or screen-reader certification.
