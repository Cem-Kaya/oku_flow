# 49 — Unused-code census and release-symbol verification (2026-10-11)

Status: **CLEANED AND VERIFIED — desktop checks deferred by owner**.
The owner asked how much code has become unreachable or unused. This review
examines the current working tree at HEAD `8c07e0b0deb67a06fb8179e534c04c8813b84a21`,
including existing authorized repairs. The initial census changed no application
source. The owner subsequently authorized deletion; the implementation follow-up
below removes only the verified unused set. Desktop restrictions remain in force:
no application, camera, GUI test, AI request, deployment, commit or push is used.

## Pre-cleanup measurement and scope

The census covers first-party C++/CUDA implementations and headers under `src/`
and `include/`, with `tests/` used as reference evidence. Third-party libraries,
generated files, sandboxes, websites and scripts are excluded from source-size
and definition denominators. Actual generated Qt moc files are checked as
additional dispatch evidence. Preprocessor alternatives are retained, including
CPU/CUDA and Maxine fallback definitions; counts are **definitions**, not unique
compiled functions or runtime instruction counts.

| Measurement | Result | Meaning |
| --- | ---: | --- |
| First-party source/header files | 122 | 56 implementation translation units plus public/private headers |
| Physical source/header lines | 57,739 | Includes blank/comment lines |
| Nonblank, noncomment source lines | 51,333 | Includes preprocessor, braces and embedded shader strings |
| Parsed function definitions | 1,715 | Tree-sitter extraction, including conditional variants and inline bodies |
| Definitions without apparent code or test references | 26 | 23 distinct qualified names; review candidates |
| Those candidate body lines | 156 | 0.304% of the source-line denominator; **not the percentage of all dead code** |
| Definitions referenced only by tests | 17 | 102 body lines; test APIs/fixtures are not automatically deletable |
| Additional name-graph-only helper candidates | 5 | 61 body lines; helpers reachable only from the preceding unused/test-only paths |
| Combined conservative graph candidates | 48 | 319 body lines; includes the 17 test-only definitions |
| Potential indirect/framework roots preserved | 44 | Zero ordinary-reference definitions protected for constructor, virtual, string or metadata reasons |
| Generated moc files checked | 54 | Across release, CPU and CUDA-test configurations |
| Uncalled named functions explicitly discarded by release linker | 14 | Exact discard evidence; host executable does not retain these function bodies |
| Remaining no-reference names | 9 | Inline accessors with no public map symbol, not a measured binary-size cost |
| Whole implementation files absent from CUDA release compilation | 0 | All 56 current implementation files are included |
| Implementation files absent from CPU compilation | 2 | Expected CUDA `.cu` files; optional-build exclusion is not dead code |

These measurements show a small, concrete unused API/helper cleanup set. They
cannot establish that only 0.304% of the project is dead: conditional code inside
live functions, disconnected states, indirect dispatch, and missed definitions
require stronger semantic/flow analysis. There is no statistically sampled
confidence interval here; this is a census with explicit tool limitations.

## Module breakdown

Headers are attributed to their corresponding module. Candidate rows include
conditional duplicate definitions, while the detailed list below merges them.

| Module | Files | Code lines | Parsed definitions | No-reference definitions | Candidate body lines |
| --- | ---: | ---: | ---: | ---: | ---: |
| app | 43 | 16,036 | 463 | 3 | 19 |
| capture | 11 | 2,782 | 98 | 1 | 1 |
| common | 33 | 10,326 | 377 | 10 | 78 |
| cuda | 5 | 8,743 | 304 | 7 | 40 |
| d3d12 | 6 | 2,020 | 59 | 2 | 13 |
| ui | 24 | 11,426 | 414 | 3 | 5 |

## Individually inspected no-reference names

Each name was checked against production/test token references and generated Qt
metadata. Source searches also covered `sandbox/` for the larger legacy helpers.
These are standalone application APIs, not a published shared-library API
analysis. Public visibility alone does not imply a caller, but removal still
needs its intended diagnostic/compatibility contract reviewed.

| Qualified name | Representative source | Body code lines | Release evidence |
| --- | --- | ---: | --- |
| `okuflow::MediaCapture::LastStopCompleted` | `include/okuflow/capture/media_capture.hpp:126` | 1 | Inline accessor; no public map symbol |
| `okuflow::AssistiveRuntime::responseLanguage` | `include/okuflow/common/assistive_runtime.hpp:77` | 1 | Inline accessor; no public map symbol |
| `okuflow::processing::CpuFramePipeline::RawWidth` | `include/okuflow/common/frame_pipeline.hpp:63` | 1 | Inline accessor; no public map symbol |
| `okuflow::processing::CpuFramePipeline::RawHeight` | `include/okuflow/common/frame_pipeline.hpp:64` | 1 | Inline accessor; no public map symbol |
| `okuflow::VideoRecorder::LastStopReason` | `include/okuflow/common/media_writer.hpp:271` | 1 | Inline accessor; no public map symbol |
| `okuflow::VideoRecorder::ActiveCodec` | `include/okuflow/common/media_writer.hpp:272` | 1 | Inline accessor; no public map symbol |
| `okuflow::CudaInteropSurface::SuperResAverageMs` | `include/okuflow/cuda/cuda_interop.hpp:269` | 1 | Inline accessor; no public map symbol |
| `okuflow::CudaInteropSurface::RunGradientDemoKernel` | `src/cuda/cuda_interop.cpp:2760` | 13 | Explicitly discarded by release linker |
| `okuflow::SharedFenceTimeline::LastReserved` | `include/okuflow/d3d12/shared_timeline.hpp:36` | 1 | Inline accessor; no public map symbol |
| `CollapsibleSection::changedCount` | `include/okuflow/ui/collapsible_section.hpp:25` | 1 | Inline accessor; no public map symbol |
| `okuflow::debug_log::CurrentPath` | `src/app/debug_log.cpp:206` | 5 | Explicitly discarded by release linker |
| `okuflow::RecordingManager::StorePendingOriginal` | `src/app/recording_manager.cpp:714` | 9 | Explicitly discarded by release linker |
| `okuflow::UIStateManager::RunWithSignalsBlocked` | `src/app/ui_state_manager.cpp:140` | 5 | Explicitly discarded by release linker |
| `okuflow::CodexAppServerClient::IsSignedIn` | `src/common/codex_app_server_client.cpp:200` | 1 | Explicitly discarded by release linker |
| `okuflow::CodexAppServerClient::SelectedModel` | `src/common/codex_app_server_client.cpp:205` | 1 | Explicitly discarded by release linker |
| `okuflow::processing::CpuFramePipeline::ResampleToFill` | `src/common/frame_pipeline.cpp:398` | 69 | Explicitly discarded by release linker |
| `okuflow::MaxineSuperRes::IsAvailable` | `src/common/maxine_superres.cpp:424` | 1 | Explicitly discarded by release linker |
| `okuflow::LaunchBlackWhiteKernel` | `src/cuda/cuda_kernels.cu:182` | 6 | Explicitly discarded by release linker |
| `okuflow::LaunchZoomKernel` | `src/cuda/cuda_kernels.cu:190` | 6 | Explicitly discarded by release linker |
| `okuflow::LaunchBlendLinear` | `src/cuda/cuda_kernels.cu:509` | 12 | Explicitly discarded by release linker |
| `okuflow::D3D12Presenter::PresentFromTexture` | `src/d3d12/presenter.cpp:568` | 12 | Explicitly discarded by release linker |
| `okuflow::AnnotationOverlay::CurrentViewTransform` | `src/ui/annotation_overlay.cpp:592` | 3 | Explicitly discarded by release linker |
| `okuflow::AnnotationOverlay::CurrentTool` | `src/ui/annotation_overlay.cpp:646` | 1 | Explicitly discarded by release linker |

The larger obsolete paths are CpuFramePipeline::ResampleToFill, the surface-based
CUDA black/white and zoom launchers, the gradient demo, LaunchBlendLinear, and
D3D12Presenter::PresentFromTexture. Current processing and presentation use
other paths. Smaller entries are status accessors or convenience wrappers.
The two Maxine IsAvailable bodies, two SuperResAverageMs accessors, and CPU
RunGradientDemoKernel stub explain why 26 definitions represent 23 names.

## The recording path that triggered the question

StorePendingOriginal (`recording_manager.cpp:714`) is the only map inserter and
has no caller. Its body is explicitly discarded by the release linker.
HandleProcessedReadback (`:729`) is still called at
`app_pipeline_runtime.cpp:1354`, still appears in the linked map, and locks the
mutex before searching the map. With current application wiring, the map stays
empty, so it returns false before the pairing/encoding tail. ClearPendingReadbacks
also retains related bookkeeping.

This is **reachable work whose useful state cannot be populated**, not an
entirely unreachable function. The simple no-caller scanner will not classify
HandleProcessedReadback as dead. Removing its call and obsolete pairing state
could eliminate actual work on completed photo/assistant/analysis readbacks;
no mutex contention, FPS gain or frame-budget saving was benchmarked. Do not
remove AddSceneFrame, which has a current production caller.

## Test-only and transitive candidates

Keep these distinct from unused product code: many are intentional test
observability or fault-injection seams. Their existing tests may justify them.

| Test-only qualified name | Source | Body code lines |
| --- | --- | ---: |
| `okuflow::CaptureHandoffPolicy::Demoted` | `include/okuflow/app/capture_handoff_policy.hpp:19` | 1 |
| `LanguageManager::rtlTestMode` | `include/okuflow/app/language_manager.hpp:44` | 1 |
| `TranscriptionSessionController::CurrentSessionId` | `include/okuflow/app/transcription_session_controller.hpp:50` | 1 |
| `okuflow::RecordingAudioBacklog::OutstandingBytes` | `include/okuflow/common/media_writer.hpp:111` | 1 |
| `okuflow::RecordingAudioBacklog::OutstandingBlocks` | `include/okuflow/common/media_writer.hpp:112` | 1 |
| `okuflow::RecordingTimeline::LastTimestamp100ns` | `include/okuflow/common/recording_contract.hpp:29` | 1 |
| `okuflow::RecordingTimeline::UnknownTimestampCount` | `include/okuflow/common/recording_contract.hpp:30` | 3 |
| `MainWindow::isUiHidden` | `include/okuflow/ui/main_window.hpp:223` | 1 |
| `okuflow::AppLanguageLayoutDirection` | `src/app/language_manager.cpp:82` | 3 |
| `okuflow::RecordingManager::NotifyCaptureFrame` | `src/app/recording_manager.cpp:664` | 3 |
| `okuflow::settings::Load` | `src/app/settings_store.cpp:899` | 3 |
| `okuflow::TranscriptionSessionController::SetUnavailable` | `src/app/transcription_session_controller.cpp:223` | 10 |
| `okuflow::CodexAppServerClient::SetRequestTimeoutMs` | `src/common/codex_app_server_client.cpp:194` | 3 |
| `okuflow::LaunchPreparedVirtualTripodFeaturePairs` | `src/cuda/cuda_kernels.cu:2692` | 22 |
| `okuflow::LaunchVirtualTripodSimilarityEstimate` | `src/cuda/cuda_kernels.cu:2898` | 16 |
| `okuflow::SetLiveTextCoalesced` | `src/ui/live_status_text.cpp:153` | 31 |
| `okuflow::MainWindow::annotationButton` | `src/ui/main_window.cpp:2867` | 1 |

The extra graph candidates are CodexJsonRpcProcess::SetRequestTimeoutMs (through
a test-only client setter), LaunchGradientKernel and GradientFillKernel (through
the unused demo), BlackWhiteKernel and ZoomKernel (through unused surface
launchers). The graph conservatively joins same-name overloads/classes and keeps
any plausible string reference as a root. For example, BlendLinearKernel is
kept by an error-message string even though its only launcher has no caller:
a manually checked false negative, illustrating why the graph is not exhaustive.

## CUDA code retained despite unused host launchers

`cuobjdump --dump-elf-symbols` and extraction of the three embedded ELF images
confirm four unlaunched legacy/demo kernel entries remain in the diagnostic
release executable: GradientFillKernel, BlackWhiteKernel, ZoomKernel and
BlendLinearKernel. Its host map also contains their generated registration/stub
symbols even though unused public launchers were discarded.

| Device architecture | Four kernels' `.text` bytes | All device `.text` bytes |
| --- | ---: | ---: |
| sm_75 | 10,240 | 301,568 |
| sm_86 | 10,496 | 307,328 |
| sm_89 | 10,496 | 307,328 |
| **Total across embedded variants** | **31,232** | **916,224** |

These are uncompressed ELF instruction-section sizes, not a prediction of
executable download savings, resident memory or FPS. Registration, metadata,
compression and alignment complicate byte-for-byte savings. No kernel was run.
A removal experiment with equivalent build outputs would measure the actual
size impact. The unused kernels do not establish a per-frame execution cost.

## Method, reproduction and validation

Ignored local evidence lives in `output/dead-code-analysis/`:

- `scan_unused.py`, `scan.json` and `candidates.csv`: reproducible syntax/name
  census, source hashes and compile-database hashes.
- `scanner_fixture.cpp` and `scanner-fixture.log`: assertions for prototype
  exclusion, ignored comments, address-taken callback references, recursive-only
  references, and constructor/main/virtual roots. Exit 0 after correcting the
  fixture's expected definition count from seven to six.
- `audit-link.rsp`, `run-link-audit.bat`, `link-ref.log`, `linked-audit.map` and
  `candidate-link-evidence.json`: isolated link of existing shipping objects,
  successful exit 0. No first-party object was older than its source by file
  timestamps; this is not a cryptographic source/build correspondence proof.
- `cuda-elf-symbols.txt`, three extracted cubins and
  `cuda-unused-section-sizes.json`: embedded-device-code evidence; tool exit 0.

The syntax tools are pinned locally, outside the synced checkout:
`tree-sitter==0.25.2` and `tree-sitter-cpp==0.23.4` under
`%LOCALAPPDATA%/OkuFlow/analysis-tools`. They are optional investigation tools,
not project/runtime dependencies and are not bundled. The scanner can be rerun
with `python output/dead-code-analysis/scan_unused.py` from the repo. Raw inputs
and the script are local evidence, excluded from git.

The scanner removes comments/literals for name-reference counting, excludes
function declaration/definition names, and treats references anywhere else
conservatively. It does not resolve overloads, preprocess build variants, prove
branch satisfiability, or compute a compiler-accurate whole-program call graph.
Forty-one files contain parser error/recovery nodes, including CUDA launch and Qt
macro syntax. Manual review and actual linker evidence are therefore essential;
1,715 is the number extracted, not a certified exhaustive C++ function count.

The diagnostic link changes output paths and adds only `/VERBOSE:REF` and `/MAP`.
It does not change source or optimization policy, publish the executable, or
replace the deployed bundle. It invokes LINK directly for analysis and is not
a replacement release build/manifest/test gate. VsDevCmd emitted its existing
vswhere lookup warning, but the explicit VS2022 linker succeeded. No runtime
acceptance or performance benchmark is claimed.

The normal release compiler uses `/O2`, which includes function-level `/Gy`,
and the non-debug linker can discard unreferenced COMDATs through `/OPT:REF`.
See Microsoft's [optimization options](https://learn.microsoft.com/en-us/cpp/build/reference/o1-o2-minimize-size-maximize-speed?view=msvc-170)
and [linker reference](https://learn.microsoft.com/en-us/cpp/build/reference/opt-optimizations?view=msvc-170).
Tree-sitter's [Python API](https://github.com/tree-sitter/py-tree-sitter) describes
syntax parsing; it is not a reachability analyzer. NVIDIA documents
[cuobjdump and embedded CUDA binaries](https://docs.nvidia.com/cuda/cuda-binary-utilities/index.html#cuobjdump).

## Cleanup implementation

The owner authorized cleanup after reviewing the census. Removed **34 function
definitions representing 31 names**: all 26 no-reference definitions in the
table above, `LaunchGradientKernel` and its kernel, the old surface black/white
and zoom kernels, `BlendLinearKernel`, and the recording/application functions
`HandleProcessedReadback`, `ClearPendingReadbacks` and
`InvalidateRecordingFramePairing`. Their declarations and conditional CPU/Maxine
variants were removed in the same change.

The recording cleanup also removes `PendingOriginal`, its map/mutex, the unused
`pairingMiss` counter, the corresponding drop-summary branch, and all application
calls to the obsolete pairing path. Active `AddSceneFrame` / `AddGpuSceneFrame`
and `QueuedFrame` annotation snapshots remain. Completed photo/assistant
readbacks now go directly to their independent consumers. Shared GPU timeline
ordering and camera/rotation recovery remain intact.

Removing `ResampleToFill` and `RawWidth` / `RawHeight` left the CPU pipeline's
private raw-dimension fields with assignments only. Those fields and writes
were removed after checking all references. The CUDA surface's separate raw
dimensions remain live. Unused includes and now-empty namespace boundaries were
also cleaned up. The existing recording-integrity fixture was adjusted for the
removed counter while retaining its timestamp-exclusion assertion.

| Same census before and after cleanup | Before | After | Change |
| --- | ---: | ---: | ---: |
| First-party source/header files | 122 | 122 | 0 |
| Physical source/header lines | 57,739 | 57,275 | −464 |
| Nonblank, noncomment source lines | 51,333 | 50,937 | −396 |
| Parsed function definitions | 1,715 | 1,681 | −34 |
| No-reference definitions in this scan | 26 | 0 | −26 |
| Test-only definitions preserved | 17 | 17 | 0 |
| Protected framework/indirect roots | 44 | 44 | 0 |

The removed function bodies account for 281 code lines; declarations, state,
call sites and namespace cleanup account for the rest. Zero remaining scanner
candidates does **not** prove zero dead code: the original parser/dispatch and
preprocessor limitations still apply. The remaining 18 graph candidates are
the 17 preserved test-only APIs plus their JSON-RPC timeout helper.

Ignored evidence now also contains `scan-before-cleanup.json`,
`removed-definitions.json`, `before-cleanup/`, `remove_verified_unused.py`, and
`scan-after-cleanup.log`. The removal script verifies original source hashes and
expected spans before writing; the before-directory preserves pre-existing
working-tree edits. Rerunning the census verifies identical names for all 17
test-only APIs and no remaining names from the removal set. These tools are
local audit evidence, not shipped dependencies.

`scripts/agent_build.bat` now accepts `OKUFLOW_AGENT_EXCLUDE_GUI=1` to honor an
explicitly reserved desktop. It builds the normal three configurations and runs
CTest with `-LE gui`, visibly marking its summary **partial validation**. Its
default full matrix is unchanged. AGENTS.md, the code reference and test guide
document that a filtered pass never replaces the full release gate.

### Fresh implementation validation

- `OKUFLOW_AGENT_EXCLUDE_GUI=1 scripts/agent_build.bat`: **exit 0**, shipping
  release build passed; **CPU 22/22** and **CUDA 27/27** non-GUI tests passed.
  Both real codec groups and all five CUDA groups passed without a skip.
  Seven GUI-labelled groups per preset were intentionally excluded; this is
  explicitly partial validation, not a full release or native focus/layout gate.
- Translation integrity passed: **743** complete Turkish/German source keys.
  This cleanup adds or removes no translation keys.
- `cuobjdump --dump-elf-symbols` on the newly built shipping executable passed.
  All four old kernel names are absent from its embedded CUDA images; the
  current linear black/white and zoom entries remain. The evidence is saved in
  `cuda-elf-symbols-after-cleanup.txt`.
- The same shipping release path decreased from **7,953,920** to **7,854,080**
  bytes (**99,840 bytes smaller**) with the same release preset. This is an
  observed executable-size difference, not an attribution of every byte to
  CUDA instructions or a runtime quality/speed benchmark. The earlier isolated
  diagnostic-link executable is not used as the size baseline.
- Before/after census assertions passed: 34 removed definitions remain absent,
  all 17 test-only API names are preserved, and 44 protected roots remain.
  The source reduction is **464 physical / 396 noncomment lines**. Tests,
  documents and build-helper edits are outside those source denominators.
- `git diff --check` passed. Logs remain in the ignored audit directory:
  `cleanup-build.log`, `cleanup-diff-check.log` and `scan-after-cleanup.log`.

No runtime FPS/latency claim, camera measurement, native desktop check,
deployment, commit or push was performed for this cleanup. Existing unrelated
working-tree repairs are preserved. In particular, review 47's separate strict
AAC-gap diagnostic remains an open issue, outside this removal change.

## Follow-up status

1. **Implemented:** remove the obsolete recording map pairing path as one coherent repair,
   preserving live AddSceneFrame recording and independent photo/assistant
   capture. Update the stale StorePendingOriginal description in code_reference.
2. **Implemented:** review the old CPU resampler and surface-based CUDA/demo functions for
   intentional diagnostic use, then remove complete unused helper/kernel chains.
   Measure binary size and preserve current linear-buffer/viewport processing.
3. **Implemented:** keep test-only seams that existing tests exercise. Remove unused status
   getters/convenience wrappers only when their API purpose is obsolete.
4. For a stronger project-wide estimate, use a semantic call graph across the
   actual CPU/CUDA/Maxine configurations, compiler unreachable-code diagnostics,
   and scenario-labelled runtime coverage. Runtime noncoverage alone must not be
   relabelled unreachable, especially recovery, device failure and security paths.

At the initial measurement checkpoint, census-total, input-SHA-256 and
documentation-link assertions and `git diff --check` passed. Those were source
audit checks; the implementation follow-up requires the fresh validation above.
