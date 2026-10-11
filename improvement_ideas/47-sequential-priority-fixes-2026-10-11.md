# 47 — Sequential priority repairs with Opus (2026-10-11)

The owner requested root/Sol and one Opus reviewer at a time to work down the
remaining priorities. Existing working-tree repairs in 45/46 are preserved.
Desktop tests remain deferred at the owner's request; no deployment is claimed.

## Recording audio input admission (P2, reliability)

Source confirmed: the pre-worker microphone queue was bounded, but paired
`WriteAudioFrame` submitted PCM indefinitely to unthrottled AAC sink writers.
The video admission counter cannot safely apply: AAC packets and PCM input
blocks need not correspond one-to-one.

Root implemented `RecordingAudioBacklog`: per writer, at most 128 outstanding
blocks, 4 MiB of estimated PCM input, and two seconds of outstanding PCM
duration. The configured Microsoft raw LC AAC contract emits one output packet
per 1024 PCM frames. Confirmed packet progress retires cumulative PCM frames,
retaining two packets of conservative completion credit. It never treats a
packet as one input block. Failed/regressed/impossible statistics retain prior
credit only. Cumulative block endpoints and a fresh-output requirement keep
unchanged old progress from retiring newly admitted PCM, including tiny blocks
inside a padded packet's rounded tail. Capture-clock gaps, overlaps and drift do not
affect this admission calculation. Channel alignment follows the negotiated
input. Invalid/overflowing/unaligned input is rejected before buffer allocation;
native and CPU startup reset the ledger. A different AAC MFT with a different
packet contract has not been validated by this policy.

Both writers are queried before either paired block is submitted. Exhausted
audio admission ends the current session with an explicit message and the
existing paired-artifact preservation/finalization watchdog. It does not
silently discard speech and later resume or accumulate unbounded stream-tick
markers. `AddAudioFrame` enforces admission for direct callers too. This bounds
estimated submitted PCM backlog, not undocumented MFT/driver allocations or
synchronous COM/Finalize latency. Statistics support is required for sustained
recording beyond the finite initial allowance.

Audio queue overflow and rejected audio input now have separate counters,
excluded from dropped VIDEO frame totals. Existing pre-worker dropped-audio
gap signaling/timing is a separate follow-up; no claim that this repairs clock
jitter or microphone optics/audio hardware.

### Opus review and root feedback

One Opus 5.5 agent independently reviewed the live audio/capture/recording
source and primary documentation. It raised clock-domain coupling as a
blocking concern with root's initial timestamp-based admission draft. Root
replaced that draft with cumulative PCM-frame accounting rather than accepting
a stop-on-cap policy tied to QPC versus synthesized AAC timestamps. Timestamp
credit and the experimental gap-marker change are not in the final code.

The child hit **"Claude API rate limit reached. Try again later."** before
writing its final report. Its terminal status is failed, not a technical
rejection or approval. Partial review context and root responses are preserved
locally in `output/priority47-root-feedback.md` and the durable delegated child
thread. No Opus final approval of the revised sample-count policy is claimed.

Task: `okuflow-opus-priority47-audio-20261011-r1`, provider `claudeAgent`, model
`claude-opus-5-5`, effort `xhigh`. No second Opus ran concurrently; no new task
was launched after the actual rate limit.

### Validation completed

- CPU incremental compilation and revised `recording_manager` tests passed.
  Fixtures separate packet/block counts, stale/failing/impossible progress,
  sink delivery, byte/duration/alignment/overflow bounds, stereo alignment,
  and old credit versus new PCM. A 200,000-block synthetic sequence covers
  opposite clock drifts and repeated delivery gaps without admission failure.
- A tiny-block/padded-packet regression failed against the first cumulative
  draft: unchanged packet credit retired later PCM inside the rounded tail.
  Requiring output advancement fixes it. The complete recording-manager
  fixture now reports 21 QtTest checks passed, with no skips. The audio queue
  overflow fixture feeds 1024 blocks against the separate 512-block pre-worker
  queue; that queue must not be confused with the 128-block writer budget.
- Actual H.264 and AV1/AAC codec fixtures passed admission and full audio
  decode: each yielded 144,384 PCM frames, last timestamp 29,866,666 ticks,
  across the three-second fixture. Audio stream headers alone no longer pass
  this check. Ten fMP4 fragments per codec were observed; no skips.
- Turkish/German extraction manifest parity passed at 743 complete keys.
- Shipping release, CPU and CUDA builds passed through the tracked presets.
  Non-GUI CTest: **CPU 22/22** and **CUDA 27/27**, including all five CUDA
  groups and both real codec legs. Seven GUI groups per preset were excluded
  with `-LE gui`, as requested by the owner. No full native desktop/release
  gate or deployment is claimed. Final log:
  `output/audit47-validation/okuflow-audit47-nondesktop-r3.log`; full test
  detail is preserved in `cpu-final-r3-LastTest.log` and
  `cuda-final-r3-LastTest.log` in the same ignored evidence directory.
- `git diff --check` passed. The initial overflow fixture failed because it
  submitted 300 blocks to a 512-block queue; the corrected 1024-block fixture
  passed. The padded-packet before/after failure/pass reports are preserved
  alongside the matrix logs. A targeted command also lost `ctest` from PATH
  after changing PATH inside the developer-shell chain; that harness error
  was superseded by the successful unmodified preset matrix above.

### Individual verdicts

| Finding | Verdict | Repair or evidence |
| --- | --- | --- |
| OP-C4: AAC submissions lack an input budget | Valid; implemented | Separate PCM duration/byte/block admission checked before either paired writer receives input |
| Microphone queue drops contaminate video totals | Valid; implemented | Dedicated audio counters; real null-button manager overflow regression passes |
| Timestamp-based retirement couples capture and AAC clocks | Valid objection; draft removed | Cumulative PCM-frame accounting with drift/gap fixtures; no timestamp retirement remains |
| Unchanged padded packet credit can retire new tiny PCM blocks | Valid in first cumulative draft; fixed | Reproducer fails before fresh-output guard and passes afterward |
| AAC/MP4 playback collapses an input timeline gap | Valid reproduction; unresolved | Strict optional diagnostic fails; stream tick/discontinuity experiment did not fix it |
| NIS coefficient storage improves application speed | Unmeasured | Preserve benchmark-first follow-up from review 46; no speed claim |

## Playback-gap reproducer — still unresolved

Root inserted a three-second jump in audio/video input timestamps after the
first second of a 90-frame synthetic recording. All samples submitted, but
decoded AAC still ended at about 2.9866 s rather than 5.9866 s, with 144,384
PCM frames. This demonstrates loss of gap timing in the tested AAC/MP4 path;
it does not yet identify whether transform, muxer or reader timeline mapping
causes it. A temporary stream tick plus next-sample discontinuity did not fix
the decoded result. That ineffective experimental code was removed.

The opt-in `OKUFLOW_TEST_AAC_GAP_DIAGNOSTIC=1` branch of
`media_writer_codecs_aac` retains the strict expected-gap assertion. It is
intentionally separate from the ordinary existing codec gate because this
pre-existing timeline defect remains open. It is not described as a passing
test. Set the variable in a VS2022 developer environment and run:

```powershell
$env:OKUFLOW_TEST_AAC_GAP_DIAGNOSTIC = '1'
ctest --preset msvc-cpu-tests -R '^media_writer_codecs_aac$' --output-on-failure
Remove-Item Env:OKUFLOW_TEST_AAC_GAP_DIAGNOSTIC
```

Failure evidence: `okuflow-audit47-aac-gap-r1.log`, `-r2.log` and final-code
`-r3.log`, preserved under the ignored `output/audit47-validation/` directory.
The final-code diagnostic still fails: normal H.264 and AV1 tracks pass, but
the gap track ends at 29,866,458 ticks. Its generic `LastError` text retains
the fixture's deliberately rejected oversized-input probe; it is not evidence
of pressure during the gap recording. All input admission/submission succeeded
before the decoded timing assertion failed. The reproduction
uses generated PCM/pixels and opens no camera/desktop UI. Admission no longer
depends on this broken timestamp mapping; fixing playback gap/drift remains
the next quality priority (AST-REC-R4).

## Next priorities after capacity exhaustion

1. Resolve AAC/MP4 gap timing and measure source-clock/burst/drift behavior
   (AST-REC-R4). Keep the strict reproducer and test both original/processed
   output; define short versus long missing-audio handling before changing
   clock policy. Simple stream-tick/discontinuity signaling is insufficient
   on this machine.
2. Recheck aspect-correct processed recording (AST-RES-R1): the viewport and
   recording canvas can have different aspects. Use shared contained geometry
   for CPU/GPU recording and annotations; decoded shapes must retain aspect.
3. Native focus/auto-hide assertions remain a release blocker, deferred by the
   owner. Blocking Finalize and post-finalization Pending/Unknown quarantine
   remain explicit limitations; never weaken their ownership safety.
4. NIS coefficient storage and encoder resource pooling remain benchmark-first
   work with the acceptance requirements in 45/46 intact.

Primary contract: [MF sink writer statistics](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/ns-mfreadwrite-mf_sink_writer_statistics).
Packet-size contract: [Microsoft AAC encoder](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-encoder).
Gap signaling references [SendStreamTick](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-sendstreamtick).
No third-party implementation was imported.
