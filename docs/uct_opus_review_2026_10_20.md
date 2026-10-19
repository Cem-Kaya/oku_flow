# UCT implementation review follow-up (2026-10-20)

An independent Opus 5.5 review examined `29ee439..afa6b3b`, widening into
interacting controller, worker, GPU, export and recording code. It was read-only:
the reviewer did not run the existing validation matrix. It confirmed no high-
or medium-severity defect, one low-severity retry defect, and one registration
hypothesis requiring a synthetic experiment. This is not a completeness claim.

## Held-acquisition OCR retry

The controller recorded an acquisition as read when submitting its OCR job.
If that result failed, expired, or arrived behind a recovery-crossfade barrier,
the acquisition could remain marked as read despite never being admitted.
Returning to the same held image could then suppress another OCR attempt while
older, hidden accepted lines remained in the model. This loses newly readable
text; it does not authorize incorrect replacement text.

The repair must clear that marker on a rejected completion while retaining the
job identity, so an older completion cannot clear a newer job's marker. Accepted,
pending and empty results retain ordinary held-image deduplication. The existing
acquisition/scene/age checks and crossfade barrier remain mandatory.

`heldAcquisitionRetriesRejectedOcr` drives actual controller/probe/scheduler
wiring with scripted recognition and capture-owned hold records. Its cases
cover a crossfade before OCR completion, backend failure and age expiry, with
established lines still present. Recovery must recognize additional held text;
subsequent presentations of the admitted acquisition must not repeat OCR.
All three corrected regression rows failed against the unmodified controller
at `afa6b3b`, specifically because no retry occurred. After the repair, the
tracked scoped selector passed both complete `ultra_clear_text_contract` and
`immutable_uct_contract` cases without skipped rows or unresolved results.
The first fixture attempt had not fully drained a warm-up job; that diagnosed
test-ordering failure is retained alongside the corrected baseline evidence.
Logs are under LocalAppData's `OkuFlow/uct-temporal-integration-20261019`:
`opus-retry-head-regression.log`, `opus-retry-head-focused.txt` and
`opus-retry-repair-scoped.log`. The last is a SCOPED CPU pass, not the full gate.
The final tracked `agent_build.bat` run then passed shipping Release compilation,
108 CPU cases (two contain optional skipped Qt rows) and the 17-case CUDA delta,
with no whole-test skips or unresolved results. Exact fingerprints retained 105
CPU results and all 17 CUDA results on the final pass; the changed UCT suite and
the two cases containing optional rows ran again. The final fixture also waits
for public input admission to become idle, covering its separate device-probe
reader before blocking H. `opus-retry-final-fingerprint-matrix.log` retains that
background result. Native GUI was excluded; the new head's remote GUI acceptance
is separate from the earlier `afa6b3b` run.

## Temporal registration limitation

The reviewer questioned whether a mostly unchanged long word can pass the strict
smoothed/current patch-registration threshold after only one letter changes.
The existing check is per word, not per line, but correlation is a visual
similarity measure and does not itself establish semantic character identity.
A separate synthetic experiment compiled the unmodified production CPU patch
functions and exercised them against rendered words and EMA inputs. Root checked
the harness's mapping/call sequence, unchanged production source identities and
the recorded result. For example, 24 px Consolas `information` blended with
current `infornation` at 90% old / 10% current passed at zero displacement:
score 0.97825 against the 0.94 threshold, with a 142 by 18 pixel tight word box
at (82, 91.03) in a 640 by 200 image, decimation 2 and no added noise or blur.

This confirms a limitation of visual registration, not an observed wrong OCR
result: no OCR backend ran and the text labels were scripted. Resident GPU parity
and an end-to-end stale recognizer output were not tested. The ordinary probe's
looser matching also has limited sensitivity to small character edits; the risk
is not confined to smoothed input. Its effect on displayed words still depends
on recognition, agreement, risk-token filtering and existing accepted text.

The private corpus includes exact, jitter, removed-word and unrelated-word
controls. Higher global correlation thresholds and localized residual policies
also rejected genuine jitter. No production threshold was changed on the basis
of that tradeoff. Character-sensitive validation, raw-text corroboration or
change-aware temporal convergence need separate design and end-to-end evidence.
This review does not claim that those follow-up protections are implemented.

Experiment source, build/run evidence and CSV rows remain in LocalAppData's
`OkuFlow/opus-uct-registration-repro-20261020`. It is a one-time synthetic
registration experiment, not an OCR accuracy or performance benchmark, and is
not added as a recurring broad test.

## Verified protections and remaining scope

The reviewer traced stream-ordered hold records, acquisition independence,
crossfade revocation and late-result barriers, paired EMA/current ownership,
allocation-failure latches, replay/reset, clean Original recording and frozen
text/ink exports. The old phone-light cancellation finding was already repaired.

Coverage gaps remain around real GPU/CPU photo freeze entry points and an
annotation click whose readback completes later. The current image-writer
fixture seeds the frozen overlay value; it does not prove those full native
readback interleavings. Some unsupported-composite status branches are defensive
and have no current production caller. Their tests are not real-path acceptance.

Temporal analysis/replay adds GPU memory traffic whose sustained 4K60 and
physical GTX 1070 costs have not been measured. The earlier one-time output-
buffer benchmark does not measure this feature. Native Windows GUI tests passed
on GitHub at `afa6b3b`; that evidence does not establish interactive screen-reader,
physical camera/phone, 120 Hz end-to-end or installer acceptance.
