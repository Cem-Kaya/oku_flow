# Composed UCT scoped validation — 23 October 2026

The exact-batch/helper, precision Phase A and driver-independent cache/runtime
composition at `b002b67a472b983eb95f3b57701efdf4f64309a1` passed the selected
model-free CPU and CUDA build/test gates in ROOT's finite C4 slot. The outer
controller **exited 125**, although its work child exited 0: two owned Job
members remained after successful work. Their identities are unresolved.
This is scoped build/correctness evidence, not an overall controller pass,
full matrix, shipping acceptance or measured performance improvement.

## Source and finite slot

The reviewed composition was frozen at
`8f262c3c9c535d3a4a3c191b2ea06ab01bfc76bf`, based on ROOT
`618ee9a57acb14a601d1fa60e518c0465808968f`. Separate commit `b002b67` changes
only 19 occurrences of the CUDA CTC fixture's local `slots` identifier to
`bufferElementCount`, plus documentation. Reversing that rename restores the
entire prior test source after line-ending normalization. Production source,
assertions, all eight CTC rows and Qt's `private slots:` remain unchanged.

ROOT launched and serialized C4 under explicit SCOPED authorization. It ran
from `2026-10-23T14:33:36.3082748Z` to `14:37:03.7567963Z`, within the absolute
`14:48:36.2974889Z` deadline and 900-second process-start cap. Builds used at
most eight jobs and reserved 30 seconds for cleanup. Configure/build held the
tracked runner lock; each tracked exact gate acquired its own lease after
the build lease was released. This report was prepared after RELEASE using
file reads, hashes, copies and Git only, with no new execution or lease.

## Selected results

| Selection | Result | Evidence scope |
| --- | --- | --- |
| CPU PCH-ON configuration, three targets | Build passed | `uct_paddle_tests`, `ultra_clear_text_tests`, `ultra_clear_text_controller_tests` |
| CPU headless gate | 5 of 235 passed | Two fresh results; three matching fingerprint passes reused by tracked `Resume:true` |
| CUDA PCH-ON configuration, three targets | Build passed | Actual TensorRT-enabled source and all ten native CUDA architectures |
| CUDA headless gate | 5 of 246 passed fresh | No reused CUDA results |
| Full synthetic `uct_paddle_cuda` | Passed, 11.41 s | 61 Qt passes, zero failed/skipped/blacklisted rows; serial GPU partition |
| Optional CPU PCH-OFF | `uct_paddle_tests` compiled | Compile only; no separate gate |

The five headless names in both configurations are
`shared_source_equivalence`, `uct_paddle_processing`,
`uct_precision_artifact_contract`, `ultra_clear_text_contract` and
`immutable_uct_contract`. CPU freshly ran the Paddle processing and precision
contracts. The remaining three retained results have the exact prior attempt
`20261023-171020-672-57003c05`; its detailed logs and XML are archived alongside
the fresh CPU attempt `20261023-173411-830-6d531416`. CUDA headless attempt
`20261023-173449-205-d88a7482` and GPU attempt
`20261023-173627-470-c2ca7da5` are fresh. Every selected gate reports zero
skipped tests, skipped Qt rows and unresolved test failures.

Detailed output includes the factory failure and both recognition-group
completion/cancellation fixtures, the role-precision contract, exact batched
rectification, and all CTC rows: `singleton-blank`, `singleton-ctc`, `tail3`,
`tail7-cross-lane-ties`, `full8`, `tail15`, `full16`, `different-actual-T`.
The retained tracked `agent_check.ps1` result is the expected FULL-required
exit 2; no `AllowFull` was used. Separate ROOT authorization permitted these
exact private builds/gates and did not convert them into full acceptance.

The caches record PCH ON and OFF, but the tracked PCH target list does not
enable PCH on these UCT targets. All three build receipts record zero selected
PCH compile commands. This establishes cache-mode compilation, not actual
PCH-enabled source equivalence. Build/gate wall times are not a speedup study.

## Source-to-binary evidence

The owned short caches are under
`Z:/Projects/OkuFlow/local-work-data/c2-2592d44d/`: `cpu`,
`cuda-hostpath-fixed` and `off`. They use VS2022 MSVC 14.44.35207
`HostX64/x64/cl.exe`, Qt 6.12.0, and CUDA 12.9.2 / NVCC 12.9.86. CUDA uses
`61;75;80;86;89;90;100;103;120;121`; actual cache values, generated compiler
metadata and native architecture flags are retained. Production CUDA/TensorRT
objects compiled in C3 remain hash-checked in C4; the repaired test target
was rebuilt and linked. No fresh compilation is claimed for retained objects.

The archive holds the complete frozen source-input snapshot and build
receipts with source, command, object, executable, cache and tool hashes.
Recorded object counts are CPU ON 41, CUDA ON 43 and CPU OFF 13; all 97
objects and seven selected executables were copied and matched their recorded
hashes. CMake caches, compile-command databases, generated compiler files,
Ninja inputs and generated sources are included. Original build directories
and evidence remain untouched.

| `uct_paddle_tests.exe` | SHA-256 |
| --- | --- |
| CPU ON | `e7b5ab4d4a017b83a0aa1bced7ad2ba73126dad45ebab9e76d2d5601fe9a619a` |
| CUDA ON | `fe2196be3ed90b21155e408c83ae248b5fd48e8ee9e80118cd2b2b4a7a7a3c63` |
| CPU OFF | `75c98eae534aaf49b051c952f1f7cd49aa70c5f6a448f3e97292a22943c1c21f` |

Runtime hashes are the tracked ledger's declared runtime/input fingerprints,
with tool/header hashes checked separately. They are not a process module-load
inventory and establish neither real TensorRT deserialization/ABI acceptance
nor actual driver/runtime/GPU provenance for inference. No model or engine
preparation, inference, runtime installation, GUI, browser, device or staging
work was authorized in these slots.

## Controller exit and preserved first failures

C4's child recorded all required exact gates passed and exited 0. The outer
receipt observed two active owned Job members, PIDs **52088 and 32760**, then
requested exact Job termination. It retained exit **125** and the message
`Successful work left active owned job members; no acceptance claim.` No
process names or command lines were recorded for those members; attributing
them to helpers, SDKs or any other workload would be unsupported.

Cleanup observed root exit, zero active Job members and an empty process-ID
list before its independent runner-lock probe succeeded. The release proof is
retained separately from the unresolved reason those two members remained.
Passing test receipts remain scoped evidence; they do not erase code 125 or
establish clean natural termination of every owned descendant.

Seven controller attempts, their raw first-failure logs and wrapper/manifest
repair snapshots are preserved:

| Attempt start (UTC) | Preserved outcome |
| --- | --- |
| C2 14:02:21 | PowerShell bootstrap string-array named-argument binding failure; no configure/build/test |
| C2 14:04:19 | CPU configured and FULL exit 2 retained; unknown transient Python preflight refusal before target build |
| C2 14:06:42 | First CPU build stopped on long-output-path MSVC C1083; failed cache/log retained |
| C2 14:09:21 | Short-path CPU build and five tests passed; CUDA compiler-ID configuration rejected the backslash host-compiler path |
| C3 14:15:16 | Runner substring predicate refusal before configure; process workload unknown |
| C3 14:18:55 | All-ten-architecture CUDA configure/production objects built; test compilation failed on Qt's local `slots` macro collision |
| C4 14:33:36 | Repaired source builds/gates passed; child 0, outer 125 and exact cleanup/release proof retained |

ROOT explicitly revised the external functional-only contamination policy:
unknown Python is recorded without inferring heavy work or inference from its
name. Native compiler/project-test/actual-runner conflicts still refuse;
runner filtering identifies actual anchored script/batch launches. Those
external harness repairs and separate finite grants remain archived, with
no claim of unchanged-failure retries or production gate changes.

## Archive and remaining limits

The local archive is
`Z:/Projects/OkuFlow/local-work-data/uct-offline-integration-20261023/`
`t3-2592d44d/C4-released-evidence-20261023-r2/`.
`ARCHIVE_RECEIPT.json` indexes attempts, builds, gate records and cleanup;
`copied-files.json` maps originals to verified copies;
`archive-file-hashes.json` inventories archived bytes;
`declared-runtime-fingerprints.json` preserves the ledger runtime hashes.
The inventory covers 858 files totaling 196,509,085 bytes and was independently
rehash-checked after copying; all entries matched. It includes 855 copied
files plus three generated receipt/index files. The 810 runtime-fingerprint
rows retain their ledger/partition associations and can include duplicates.
The receipt SHA-256 is
`493a76e5bd269353c4f75afc9f20478c1d426e977c9fd83329c927224ce56805`;
the archive inventory SHA-256 is
`d058314765ac6ddc02ea6c336684ad19f57de618f9db1642be8182bb6eda1df0`.
Original frozen `8f262c3` receipts/patches and the separate `b002b67` repair
receipt are retained. The documentation commit above `b002b67` changes no
tested implementation or fixture bytes.

Full shipping/CPU/CUDA matrix, native GUI/hardware, performance/quality,
real-model/private BF16, P0/unregistered diagnostics and public-runtime/golden
acceptance remain outside this evidence. Public runtime/assets and parallel
merge work are not folded into this source composition.
