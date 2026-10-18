# PaddleOCR implementation review — 2026-10-18

Development changes on top of `3ea8e10`, not a published installer. Opus reviewed
the complete implementation read-only. The coordinating agent checked the
findings against production code, applied the following repairs and added
regressions. A second independent review checks those repairs.

| Finding | Repair / evidence |
| --- | --- |
| Input-copy faults could retire every engine | Separate `inputFault`; copy CPU input once outside backend iteration; preserve a healthy fallback for the next frame. `copyFaultKeepsFallbackUsableAndCopiesOnce`. |
| Device OCR bypassed stability/refresh admission | Exact matching device candidate goes through the common stability/coverage/refresh gate. `staticDeviceSceneDoesNotContinuouslyRunOcr` requires accepted text and bounded inference count. |
| In-flight and queued OCR held both snapshot slots | Device OCR retains no pending lease while a job runs. A bounded two-image fake sustains at least ten probes/captures during held inference; the actual CUDA pool sustains thirty second-slot probes with the first slot held. |
| Cold GPU engine compilation blocked all recognition | Owned asynchronous preparation; CPU/Windows serves until GPU is ready, then promotion between jobs. Deterministic preparation/promotion test; actual cached TensorRT upload/direct-device acceptance. Progress monitor cooperatively cancels on backend-session retirement. Vendor-call preemption and interrupted cold-build acceptance are not established. |
| Bad decoder rows demoted GPU while CPU omitted them | Both skip untrustworthy crops; CUDA finite-value/CTC fixture checks the invalid row. Minimum box dimensions align. |
| Every copy allocated and freed driver resources | Reader threads retain their own streams and pinned/device buffers; reduced buffers free asynchronously. Growth/errors preserve both DMA endpoints. Presentation-tail improvement is not measured. |
| Full-scene detector cap may miss tiny print | Confirmed design limitation, documented rather than claimed solved. Focus/context tiles and real small-text recall remain future work. |
| Build-machine CUDA path and optional SDK ambiguity | Runtime CUDA dependency discovery; explicit adapter-required CMake options. Separate SDK-free build coverage. Installer dependency inventory remains unapproved. |
| Cancellation lease, repeat-free and partial extraction | Release proven-complete input on cancellation; clear allocation pointer before checked free; validate original extracted bytes against pinned archives before a completion marker. Unknown GPU completion latches the pool and retains uncertain resources. |

The engine cache digest detects accidental corruption; it is not protection
against another process running as the same user. FP32 inference and estimated
CTC word coordinates are intentional; no exact character-outline, handwriting,
math, diagram or multilingual accuracy claim is made.

Private evidence is under `%LOCALAPPDATA%/OkuFlow/uct-paddle-20261018/`:

- `paddle-reviewed-final.txt`: 17 Qt rows passed, no failures/skips; real CPU
  and TensorRT recognize six known synthetic printed words. GPU kernels and
  snapshot ownership/decimation run on the development RTX4090.
- `final-fallback-contract.txt`: ten rows passed after fallback/promotion fixes.
- `final-device-gate.txt`: six rows passed, covering the static device gate,
  bounded two-slot progress at 16x and both initialization-retry states.
- `verified-preparation.txt`: exact existing ORT/TensorRT extraction bytes,
  models/configuration/cards and retained notices verified by the tracked helper.
- `no-sdk-contract.txt`: fourteen selected rows pass in a separately built
  SDK-free CPU executable. `strict-dependency-refusal.txt` records the expected
  configure error when CPU adapter headers are required but absent.
- Earlier failed build/test attempts are retained beside their repairs.

The first tracked background gate caught a Qt include leaking through the public
CUDA interop header; forward declarations restored the Qt-free boundary. A later
gate compiled release and passed the CUDA delta but could not run one packaging
case because the installed Qt source notice was absent. The exact original
`v6.12.0` Qt Multimedia license was obtained from upstream and installed with
private URL/hash provenance. No notice was invented and no assertion was removed.
Final gate results are recorded after the last code changes.

Native GUI, real-camera optics/small-print recall, actual GTX1070 behavior,
end-to-end frame tails and installer/clean-VM acceptance remain separate. The
owner's desktop and phone display configuration were left untouched.

## Second-review repairs

Opus identified probe starvation above the 80 ms admission interval, silent
resource pressure, input-fault engine retirement, fallback resurrection and
nondeterministic dense-region truncation. These were confirmed and repaired:

- Device admission serializes the complete readback/check path with a timeout.
  The slow-probe regression uses 120 ms copies, proves eventual OCR submission,
  reports repeated copy faults, and recovers without backend replacement.
- Clean allocation pressure backs off 250 ms; uncertain GPU completion remains
  quarantined. Pool and repeated-input health hide overlays/reading content
  while failed, use the existing status, and recover on healthy input.
- TensorRT preserves prepared engines when pre-enqueue ownership/readiness
  fails. The actual graph test rejects an unowned frame before successfully
  recognizing the known page through both upload and direct owned GPU input.
- Candidate lifecycle records prevent reinitialization/resurrection after GPU
  promotion. The regression covers failed CPU, serving Windows, GPU promotion,
  a recoverable input fault, then GPU failure back to the existing Windows session.
- The redundant same-frame stability gate was removed. Actual admission still
  compares the result with the newest independent probe.
- GPU prefix compaction retains the same first 128 raster components as CPU.
  A 465-component fixture compares every retained quad on three GPU passes.
- Builder graph bytes are hashed at use. Version-only TensorRT/CUDA DLL trust
  and same-user path/cache boundaries are stated explicitly.

`review2-repairs-paddle.txt`: 16 Qt rows passed, no failures/skips, including
actual CPU/TensorRT graphs, CUDA algorithms/ownership and dense selection.
`review2-repairs-controller.txt`: five Qt rows passed, no failures/skips.
The tracked scoped CPU UCT contract subsequently passed. A CPU-only compilation
caught an unguarded CUDA health call; the CUDA compile guard was repaired before
that pass. The original failed build remains archived.

The test runner now supports an explicit diagnosed-repair recheck without
removing failed/unfinished evidence or repeating matching passes. Its private
external-prerequisite regression and existing runner contracts passed all 529
checks (`diagnosed-repair-runner-contract-repaired.txt`). An initial contract
fixture syntax error is preserved separately. This runner change does not grant
GUI availability or turn partial background evidence into release acceptance.

## Final background gate

`review3-final-background-matrix.txt` records the tracked shipping CUDA-12.9
Release compile, all 86 selected CPU CTest cases, and all ten CUDA delta cases
passing with zero unresolved or skipped CTest cases. One optional development
MCP Qt row skipped inside a passed CPU case. Translation parity passed.
All selected cases executed with the changed runner/runtime fingerprints; old
unfinished logs/blockers were preserved, not deleted or rewritten. The restored
Qt packaging prerequisite now passed its actual contract.

GUI cases were explicitly excluded for this run. This is background validation,
not complete release/installer acceptance. No native GUI, owner process, camera,
phone settings, release artifact or deployment was changed by the validation.

## Final independent Opus review

Round three found no blocker in the asynchronous graph path and confirmed the
round-two repairs. It identified a GPU/CPU crop-orientation mismatch: the parent
independently reproduced it (`orientation-repro-red.txt`), aligned emission with
the CPU/PCA convention, and added horizontal/vertical/tilted large-component
corner comparisons. Integer moment accumulation also removes float atomic order
sensitivity. The memory-pool hardware risk was addressed with capability lookup
and a forced legacy-allocation/readback fixture, not a claim of GTX 1070 testing.

Other confirmed repairs cache input preference once per job, preserve engines
and snapshot allocation across CPU uploads, tolerate two clean allocation hiccups,
release rejected candidates, and remove unused same-frame similarity. CPU-path
fallback clears prior GPU pool health only after valid current pixels arrive.

The runner now requires exact blocker names, refuses repeated unchanged failed
rechecks and records repair identity/reason in each case, capability evidence and
summary. Ordinary modes and historical logs remain unchanged. A final private
contract rerun checks these restrictions.

`review3-paddle-accepted.txt`: eight focused Qt rows passed with no failures/skips,
including actual CPU/TensorRT graphs, legacy allocation, oriented geometry and
resource-fault recovery. `review3-controller-accepted.txt`: six focused Qt rows
passed without failures/skips. A compile-time variable-name collision and an
accidental diagnostic run of the older executable remain in separate logs; they
are not acceptance evidence. No inference timing in these diagnostics is a
new benchmark or native presentation-rate claim.

Remaining observations: GPU connectivity-chain costs, CPU full-scene copy cost,
cold vendor-call cancellation boundaries, upright-tall-glyph crop quality,
physical-camera recall, GUI/screen-reader behavior, GTX 1070 and installer
acceptance need separate evidence. They were not presented as proven defects.

## Revalidated final worktree

After the final Opus fixes, `final-accepted-background-matrix.txt` again records
PASS for shipping Release compile, 86 CPU CTest cases and all ten CUDA delta
cases, with zero skipped/unresolved CTest cases and one optional development Qt
row skipped. All selected cases executed under the updated runner fingerprints.
`repair-scope-runner-contract.txt` records all 539 private runner checks passing.
These results supersede the earlier matrix for the current source tree; GUI
remained explicitly excluded and no installer/public deployment was produced.
