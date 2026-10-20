# UCT local colors, backend status and readiness recovery

UCT keeps logical lines for reading order and validation, but each recognized
word now carries its own optional foreground/background pair through preparation,
admission and scene/export projection. A yellow selected button therefore keeps
dark lettering on yellow while adjacent buttons retain light lettering on dark.
Ink fitting uses that same local pair. Small refresh variations use the existing
brightness grid and color hysteresis; absent metadata retains the line fallback.

The CUDA worker measures line and word rectangles together and returns compact
color/geometry metadata. Word images are not downloaded. Inputs remain leased
through exact stream completion, and existing scratch caps/unknown-completion
quarantine remain in force. CPU fallback performs equivalent local measurements.
The line reference, omission protection and independent evidence rules do not
change.

Advanced UCT displays the actual selected OCR backend as GPU or CPU, including
Windows OCR during GPU preparation. The native visible label and cached
accessibility name update together, without unsolicited speech. Off/unavailable
states hide the active-backend label. Both new labels have complete wording
review across 24 translated catalogs; this is model review, not native-speaker
or interactive screen-reader acceptance.

Cached TensorRT plans now load only the inference runtime initially. Parser and
architecture-specific builder-resource DLLs are loaded on a cache miss or invalid
plan. Exact model/cache hashes, runtime version, profile and precision checks
remain unchanged. Recognition enqueues up to eight existing batch-one inference
calls on its private stream before one completion wait, using distinct pinned
metadata slots. This is bounded submission, not a new engine profile or concurrent
access to shared output textures. Cancellation and exceptions retain resources
until completion or quarantine.

During GPU preparation a complete Windows OCR result can bridge readiness.
Configured Paddle CPU sessions are deferred until GPU failure/unavailability,
so loading them and running a dense CPU job cannot delay a newly ready GPU.
Fallback results still pass normal admission and independent-neighbour checks.
A successful provisional fallback can be refined after its neighbour completes;
repeated reads of identical pixels never become independent evidence.

A transient failure previously consumed a paused source's only recognition
attempt. Refused submissions, current-job cancellation, input faults and expired
completions now schedule bounded retries tied to the same geometry/source ticket.
Non-preparation failures permit at most three retries; preparation refinement is
spaced by 500 ms and bounded by the existing 30-second horizon. Real source/seek/
settings changes cancel the old timer's authority. Successful final paused reads
remain one-shot. No partial/unvalidated glyphs are published, and failure limits
continue to keep original pixels visible.

The rebuilt tracked gate passed shipping Release compilation, 108 CPU cases
(two contain optional skipped Qt rows) and all 17 CUDA delta cases, with no
whole-test skips or unresolved results. Exact unchanged per-case passes were
reused by the tracked fingerprint runner. All 24 translation catalogs match 879
source keys. Native GUI checks remain separate and run on GitHub's disposable
desktop; the owner's running app is never terminated or replaced to unlock
outputs. Private media/model caches and raw diagnostic evidence remain under
LocalAppData. Original scoped failures and the resume prerequisite refusal were
retained; the diagnosed controller oracle and backend-status issues were repaired
before the passing integration gate.

Independent Sol integration review identified two refinement gaps. Preferred
preparation timeout now retains a healthy validated CPU fallback; terminal
recognition exhaustion clears glyphs, reading text and accessible text together.
A new selected refinement is staged until its complete neighbour pair is ready,
so it cannot temporarily replace a proven artifact with an uncorroborated one.
Tests hold that new neighbour and check visible/readable/UIA continuity, then
expire preferred preparation and require the healthy fallback to remain.

One cached real M/M source-frame diagnostic succeeded against a private
inference-only SDK directory containing no parser or builder-resource DLLs.
It retained 178 candidate words, 364 ms worker time and 4009 ms total QtTest
elapsed, versus the earlier 354 ms / 3910 ms sample. These single measurements
do not show a raw inference/startup speedup; grouping removes waits but model
execution and cached preparation still dominate this case. CPU fallback remained
usable (179 candidate words, 9290 ms worker time). The separate automatic-chain
diagnostic measures whether early bridge completion avoids that CPU startup wait;
worker completion is not glyph, display or end-to-end camera latency.

The automatic-chain diagnostic exposed a stale backend identity: the Windows
adapter returned its identity only from initialization, so runtime status fell
back to the preparing GPU's earlier label. The adapter now retains and publishes
its CPU identity. Real Windows OCR rows require post-recognition status updates,
and the startup diagnostic requires a usable result with preparation finished
before counting GPU completion. Original false-positive diagnostic output and
the command-line argument failure are retained with the private evidence.

After that repair, the real automatic scheduler returned a complete Windows OCR
candidate result at 902.6 ms (177 candidate words), then a completed TensorRT GPU
result at 4398.6 ms (178 words), without loading the Paddle CPU sessions. This
single cached-input diagnostic used the inference-only SDK folder and correctly
reported CPU then GPU. It establishes early candidate availability and promotion,
not first validated glyph, full-controller, native screen-reader or camera
latency. The selected button's words measured dark foreground on `#eeba65`;
the neighbouring Robots label measured light foreground on `#253a33`.
Raw diagnostics stay in `%LOCALAPPDATA%/OkuFlow/uct-color-startup-20261020/`.
