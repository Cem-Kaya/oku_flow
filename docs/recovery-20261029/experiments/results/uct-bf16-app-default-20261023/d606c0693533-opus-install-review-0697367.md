# Opus read-only review: BF16 app default, fresh installs (frozen 0697367)

Reviewer: Opus 5.5 sub-agent, 2026-10-23. Read-only source review. No build, test,
GUI, GPU/CUDA API, model preparation, download or cache write was performed.

Tags: **[S]** source fact at `0697367`; **[E]** root's recorded evidence (read-only);
**[I]** inference, not verified. No runtime, performance or legal claim is made.

Exact source: `0697367c6a685178507dc12e63c7de22e8103f4e`. During review HEAD advanced
to `2fdabac` (docs only: `docs/uct_public_bf16.md` +41 lines recording real-golden
evidence). `git diff 0697367 -- src include tests scripts` is empty, so every source
line cited below is `0697367`. The working tree shows mixed EOL on some files
(`git ls-files --eol`), content-equal to the commit.

## Policy compliance confirmed in source

- [S] No CUDA/API/context/enqueue/copy/event fault or unknown completion reaches
  another engine attempt. Detector fallback (`uct_tensorrt.cpp:1606-1619`) admits
  only `AllowsTensorRtCapacityFallback` types, and only `ArtifactError` once the smoke
  has started. It checks `cudaPeekAtLastError`, then confirms drain before discard.
  The recognizer path catches only `ArtifactError` (`:1655-1669`) and confirms drain
  before discard. Golden `RawText`/`CheckGolden`/`ReadyReceipt` refusals happen after
  `ConfirmPublicCompletion`.
- [S] Each BF16 `Network` is attached to `State` before any factory call
  (`:1598`, `:1622`, `:1673`, `:1698`, `:956`). Parser/builder/config/serialized/model
  live in `PublicResources` until `ReleaseBuildResources`, which runs after
  `ConfirmPublicCompletion` (`:1727-1728`). Member order destroys context, then
  engine, then runtime, then builder resources (`:711-716`).
- [S] Detector and recognizer fall back independently (`:1588` vs `:1638`). [E] The
  small-tier run kept the medium BF16 detector (4/4 smokes) while the recognizer fell
  back to small FP32.
- [S] CPU Paddle is unchanged (FP32). No settings are written. Tiny keeps N1 service
  (`uct_model_settings.hpp:14-16`, `:978`); the engine profile is N1..N8.
  [E] The tiny run reported "recognizer batch=1".
- [S] Driver is absent from the detector `PlanKey` (`uct_bf16_detector.hpp:28-32`),
  from `Fp32PlanKey` and from the v2 `BuildIdentity`. It appears only in receipt
  provenance. Admission requires `major==11 && >= 11.4.0.106`
  (`uct_tensorrt_identity.hpp:35-40`, `uct_tensorrt.cpp:226`).
- [S] The public path has no dependence on the owner's card. `kDeviceUuid` is used
  only by schema-1 `Unsupported`/`ReadPlan`. Note: a set
  `OKUFLOW_UCT_LOCAL_BF16_MANIFEST` disables both public BF16 roles (`:1588`, `:1638`).
  On the owner's PC that variable must be unset to exercise the new default path.
- [S] The detector uses the new graphs 5699/05f6/cfb0. `24e3` is not referenced
  anywhere in src/include/tests/scripts. All six entries in `bf16-models.json`
  match the compiled pins (`uct_bf16_detector.hpp:6-13`, `uct_public_bf16.hpp:30-38`,
  `uct_model_pins.hpp`). I checked this by hand; no fixture checks it (P3-7).

## P1

### P1-A: a healthy BF16 attempt that runs out of time disables the whole TensorRT backend, not just BF16, on every launch

- [S] Detector deadline: 600 s (`uct_tensorrt.cpp:1589-1590`). On expiry the progress
  monitor stops the build, `buildSerializedNetwork` returns null, and `artifact()`
  throws a typed error (`:566-568`). The catch block then throws `runtime_error`
  because `detectorStop()` is true (`:1613`). That reaches the outer catch and sets
  phase 2 (`:1738-1747`). A smoke that fully succeeds just after expiry is also
  terminal (`:1603`).
- [S] Recognizer: the same pattern applies (`:1649-1652`, `:1665`). The golden phase
  is terminal at `:967` even though `RunPublicGolden` returned, so every golden call
  was already confirmed complete.
- [S] On phase 2, `AutomaticRecognizer` retires the TensorRT candidate and moves to
  Paddle CPU (`uct_paddle.cpp:247-259`). Paddle CPU uses the CPU tiers, which
  default to tiny/small (`uct_model_settings.hpp:10-11`). Same-tier FP32 GPU is never
  tried in that recognizer chain.
- [S] Nothing durable is recorded. The plan is not written and the attempt latch is
  in memory only. The next launch repeats the cold build and the terminal failure.
  Within one session, a UCT off/on does reach FP32 GPU, because the latch then
  refuses BF16 with a typed error.
- Repro (source trace): Ampere+ GPU, BF16 detector graph staged, no cached plan, cold
  build longer than 600 s. Result: Windows OCR bridge for 10 minutes, then Paddle CPU
  tiny/small for the rest of the session, on every launch.
- How likely: [E] root's whole diagnostics took 61/100/34 s on the test GPU. That is
  not a bound for laptop-class Ampere. [I] Unmeasured on slow machines.
- Why this is not "uncertain work": once the build has returned, the stream can be
  confirmed idle with the existing bounded poll. Falling back to FP32 at that point
  uses a different engine on a confirmed-idle stream. BF16 is still never authorized
  or published, so the R2 "late final N8 must not authorize" rule is kept.
- Smallest fix:
  - `:1603`: test `cancelled()` instead of `detectorStop()`.
  - `:1613-1615`: throw only if `cancelled()`, then call
    `ConfirmPublicCompletion(*state,cancelled)` (fresh 30 s bound; unknown completion
    still throws, so it stays terminal), then discard.
  - `:1661`/`:1665`: same change. A build-phase deadline (null serialized network,
    typed `BuildRefused`) then selects same-tier FP32.
  - Optional: turn `:967` into a typed `Refusal(GoldenMismatch,"golden deadline
    expired")`. Publication still happens only after `CheckGolden` passes.
  - Keep the in-smoke and in-golden expiries (inference in flight) terminal.

### P1-B (activation gate; inert at HEAD): staging the detector graph file is the same as turning BF16 detectors on for every Ampere+ user

- [S] The production `Request` is enabled (`uct_tensorrt.cpp:1845-1846`), and
  `preferDetector` defaults to `true` (`uct_public_bf16_runtime.hpp:24`). The `:1588`
  condition has no catalog or activation check. Compare the recognizer, which needs
  a non-empty compiled `ProductionCorpusCatalog()` (`uct_public_bf16.hpp:59-65,76`).
  So copying `5699…` to `app/models/uct/det/medium/bf16-linear/inference.onnx`
  activates the BF16 detector by default before any detector acceptance exists.
  [E] Root's doc: small/tiny BF16 detectors and cold/warm install are unvalidated.
- Is the zero-input smoke enough for activation?
  - Technical safety: yes. It gates NaN/Inf, the [0,1.0001] range, FLOAT DEVICE
    LINEAR ABI, profile shapes and confirmed completion.
  - Product quality: no. It cannot detect an all-zero response (silent "no text",
    and nothing errors, so FP32 never takes over) or saturation (values up to 1.0001
    pass). It does not check that the map geometry matches the input
    (`:857` only bounds dims to 960; `CheckMap` does not compare dims). It says
    nothing about recall versus FP32.
  - For a low-vision-first product, silent text loss with a "[BF16 / …]" label is
    the worst failure mode.
- Smallest fix:
  1. A compiled `bf16_detector::ProductionActivated()` returning `false`, required
     at `:1588` when `use==Production`. Validation stays opt-in.
  2. Tighten the smoke: after `ConfirmPublicCompletion`, require
     `d[2]==height && d[3]==width` (typed `ArtifactError`; [E] root's run observed
     equality for all four sizes). Record max/mean probability per smoke as evidence.
  3. Before flipping (1): one owned-pixel positive/negative detector check (the eight
     frozen GRAY8 crops on a white canvas at known boxes; thresholds set offline from
     FP32), plus offline per-tier page comparison of each converted graph against
     FP32.

## P2

### P2-C: no durable failure memo, so deterministic BF16 failures repeat full cold work every launch

- [S] `Attempts` lives in process memory (`uct_public_bf16_runtime.hpp:91-102`). Only
  passing plans are published (`uct_tensorrt.cpp:974-976`). After a golden failure,
  `FindPlan` finds nothing, so the next launch cold-builds again (`:951-958`).
- [E] Every recognizer tier failed strict golden on the test GPU (8/16, 8/16, 12/16).
  With the catalog active, each launch on such a machine would do a BF16 cold build,
  run golden, and then fall back to FP32.
- Fix: a new-only negative receipt keyed by the exact attempt identity
  (`CacheKey(build,corpus)+":missing"`, or the detector `PlanKey`) in the cache root.
  Check it before attempting. It never authorizes success. A changed
  builder/model/corpus/GPU gives a new key and a retry. Status names the receipt
  path so a diagnosed retry is possible. This is doc rule 4, extended across launches.

### P2-D: a cancelled attempt is recorded as a failure, so any off/on or model change during an attempt disables BF16 for the rest of the session

- [S] Only `Passed()` erases a latch key (`runtime.hpp:93-97`; `:1597`, `:953`). Any
  model-dropdown change, including a CPU tier, triggers off/on
  (`ultra_clear_text_controller.cpp:975-981`). Off/on and Text Clarity toggles build
  a "fresh backend chain, including candidates that previously failed" (`:372-374`).
  The latch outlives the chain.
- Repro: during the first-run build, change any UCT model setting. The status then
  reads "BF16 detector already failed or in flight this session", and FP32 is used
  until restart.
- Fix: when the attempt exits with `cancelled()==true`, erase its key; keep every
  other failure latched. An overlapping instance still gets FP32 (no waiting), but a
  later enable retries.

### P2-E: a storage failure after a passing golden throws away the attested engine; the inventory-full check runs only after a full build

- [S] Any `PublishNew` refusal becomes a typed FP32 fallback (`uct_tensorrt.cpp:975-976`).
  An unwritable or full cache therefore repeats a cold build and golden on every
  launch. Inventory full (`runtime.hpp:444`) is detected only at publication, after
  the build.
- Fix:
  - Return the observed inventory count from `FindPlan`, and refuse before building
    when the count is ≥32 and no cached plan exists.
  - Optionally keep the just-attested engine for this session, with status
    "attested this session, not cached: <reason>". Golden has just passed on these
    exact bytes, device and builder.

### P2-F: exact missing seams for a fresh machine

All are [S] unless marked. Root already knows these exist; listed here precisely.

1. TensorRT discovery is only `OKUFLOW_UCT_TENSORRT_ROOT` or
   `%LOCALAPPDATA%/OkuFlow/ocr-deps/TensorRT-11.4.0.106-cuda12.9/TensorRT-11.4.0.106`
   (`uct_tensorrt.cpp:1841-1843`; `UctOcr.cmake:18`). There is no app-relative
   candidate, and newer ABI11 runtimes are reachable only through the env var.
   `Initialize` requires `<sdk>/bin/nvinfer_11.dll` (`:1035`).
2. CUDA DLL search uses developer locations only (`:195-206`). Whether the TRT 11.4
   DLLs import any toolkit DLL is unverified: [I] check `dumpbin /dependents` plus a
   clean-VM load. The app itself links `cudart_static`.
3. Local per-machine engines need `nvonnxparser_11.dll` and the
   `nvinfer_builder_resource_sm<cc>_11`/`ptx_11` DLLs on every eligible machine
   (`PrepareBuilder`, `:234-268`), not just `nvinfer_11.dll`.
4. The original FP32 models and their `inference.yml` (dictionary) are required even
   for BF16. `LoadPaddleAssets` gates `Initialize` (`:1029-1034`), and
   `ReadGraphPath`/`ReadModel` re-verify them. They come only from
   `prepare_uct_runtime.ps1`, which needs network access.
   `DefaultPaddleRoot` probes `app/ocr/det/inference.onnx`, which is the **tiny
   detector** (`uct_paddle_processing.cpp:55`; pins dir `det`). A bundle with some
   tiers but no tiny detector would silently fall back to LocalAppData.
5. ONNX Runtime CPU DLL for the Paddle CPU fallback (`ocr-runtime/cpu`).
6. The six graphs at `app/models/uct/{det,rec}/<tier>/bf16-linear/inference.onnx` are
   not staged. No CMake/dev staging exists and no override exists (by design), so a
   dev build cannot exercise the production path without manual copies next to
   `OkuFlowApp.exe`.
7. Corpora at `app/models/uct/golden/<CorpusVersion>/` are not staged. The repo
   manifests match the candidate pins (I verified the SHA-256s) and are protected by
   `.gitattributes`.
8. `ProductionCorpusCatalog()` is empty.
9. The converter source (`precision_models.py`, SHA `da22b96f…`) is not tracked in
   this checkout.
10. SBOM: the manifest enumerates files recursively (`generate_release_metadata.ps1:101`),
    but SBOM packages are explicit. PaddleOCR models, ORT and TRT need entries. The
    MSI tables and the layout test need updates too.

## P3 (brief)

1. `:1041-1044`: "No CUDA device is available." is also shown for an
   insufficient/old driver. Include the `cudaError` name. This matters for new PCs
   with OEM drivers.
2. `:1667-1668`: the status shows a bare numeric "typed reason=%1". Add a name.
3. `:974`, `:979-981`: on the warm path, "Local ready receipt SHA256" hashes a freshly
   rebuilt receipt, not the stored `ready.json`.
4. `:1729-1731`: "detector tensor smoke=N/4" is printed on the selected-detector line
   even after FP32 fallback, when it refers to the discarded BF16 detector.
5. Ordinary cache:
   - [S] `:581-592` write failures are silent, so plans are rebuilt every launch with
     no status.
   - [S] A present-but-refused stable plan can never be replaced (no-replace move),
     so it is rebuilt every launch.
   - [S] In v2, a refused committed plan is a permanent typed refusal (no self-heal).
   - [S] Old plans are never cleaned up.
   - [S] A corrupt v2 sibling or an unknown entry permanently disables that identity,
     and only some refusals name the path. Add the path to every `ValidateCommitted`
     refusal and document the manual reset.
6. [I] v2 pins the shared `ocr-engines` root with `FILE_SHARE_READ` only
   (`runtime.hpp:195-196`). A concurrent ordinary `MoveFileExW` into that directory
   would likely fail (rename opens the target directory for write) and the plan
   would silently not persist. A dedicated v2 cache root avoids this.
7. No fixture reconciles `bf16-models.json` with
   `GraphHash`/`Model()`/`kPaddleModelPins`. Add it to `uct_public_bf16_contract`
   `REQUIRED_FILES`.
8. `docs/ultra_clear_text.md:169` is stale: it says cache identity binds the driver.
9. `goldenPixels`/`detectorSmokeMap` pinned host buffers stay allocated for the whole
   State lifetime (a few MiB).

## Activation observation from root evidence (not a code defect)

[E] The failures sit on confusable rows: `O0 OO 00` read as `00 00 00`, `Il1 I l 1` as
`111111`, `café naïve` as `café naive`, and a missing space in `+12.50 -0.75`.
[S] Production splits words only on explicit space tokens
(`uct_paddle_processing.cpp:670`), so the raw-text golden matches production
semantics.

Without an FP32 control on the same eight crops through the same
RectifyBatch/DecodeBatch path, the evidence cannot separate a BF16 regression from a
limit of the model tier. Recommendation: record the FP32 rows in the private
validation seam (record-only, no authority).

If FP32 produces the same output, the exact ground-truth gate on confusables cannot
be passed at any precision. "Prefer BF16" would then need a policy decision, for
example BF16==FP32 on this machine plus ground truth on rows chosen before observing
BF16. That decision belongs to the root/owner.

## Fresh-machine flow (source trace at HEAD)

| Condition | Result | Available | Missing |
|---|---|---|---|
| New PC, current bundle, any GPU | `LoadPaddleAssets` fails, so both the TRT and Paddle CPU candidates are unavailable. Windows.Media.Ocr serves. | Windows OCR | All of P2-F |
| + `prepare_uct_runtime.ps1` (models + ORT), no `-TensorRT` | "TensorRT ABI 11 runtime … not installed". Paddle CPU FP32 (CPU tiers). | Paddle CPU | TRT |
| + TRT SDK at the pinned LocalAppData path, Pascal | "requires Turing or newer". Paddle CPU. | Paddle CPU | n/a |
| Turing | FP32/FP32 GPU. Detector BF16 skipped with no reason text; recognizer shows typed reason 1 (Hardware). | FP32 GPU | n/a |
| Ampere+, graphs not staged | Detector shows a typed "artifact missing"; recognizer shows typed reason 4 (CorpusUnavailable). FP32/FP32 GPU. Windows OCR bridges during preparation (index 2, `uct_paddle.cpp:158`). | FP32 GPU | Graphs, catalog |
| Ampere+, detector graph staged | BF16 detector cold build (≤600 s) plus smokes, cached in `ocr-engines`. Recognizer FP32. | BF16 detector | P1-A, P1-B gates |
| + catalog/corpus active | BF16 recognizer build, golden and v2 publish. On failure FP32, repeated every launch (P2-C). | Per golden | P2-C memo |
| Warm launch | Detector cache hit plus smokes. Recognizer: `FindPlan` validates all generations, then re-runs golden. | as cached | n/a |
| ABI11 TRT update (via env) | New builder gives new keys and cold builds. Old plans kept, no cleanup. | yes | Discovery (P2-F1) |
| Driver update | Keys unchanged, plans reused. A refused plan triggers P3-5. | yes | n/a |
| GPU swap | New UUID gives new keys and cold builds. | yes | n/a |
| LocalAppData unwritable/full | Ordinary plans silently not persisted. v2 refusal gives FP32 after a build every launch. | FP32 GPU | P2-E |
| Program Files read-only | Fine: models are read-only and caches live in LocalAppData. | yes | n/a |
| Offline | Runtime needs no network. Payload acquisition (the script) does. | yes | Bundled payloads |
| Off/on or model change mid-attempt | BF16 latched off for the session (P2-D). | FP32 GPU | P2-D |
| Deadline during cold build | Whole GPU backend lost, CPU tiers, every launch (P1-A). | CPU | P1-A |

## Coverage and limits

Read in full:
- `uct_tensorrt.cpp` (1887 lines)
- `uct_bf16_detector.hpp`, `uct_public_bf16.hpp`, `uct_public_bf16_runtime.hpp`
- `uct_runtime_precision.hpp`, `uct_tensorrt_identity.hpp`, `uct_tensorrt_failures.hpp`
- `uct_default_recognizer.cpp`, `uct_model_pins.hpp`, `uct_model_settings.hpp`
- `prepare_uct_runtime.ps1`, `bf16-models.json`, `public-bf16-model.json`
- the `0697367`/`508700b` diffs and the tests in those diffs
- docs: `agents.md`, `uct_public_bf16.md` (including `2fdabac`),
  `uct_tensorrt_cache_policy.md`, the relevant sections of `THIRD_PARTY_LICENSES.md`
  and `hardcoded_paths.md`, the prior review dispositions

Excerpts only:
- `uct_paddle.cpp` (`AutomaticRecognizer`)
- `uct_paddle_processing.cpp` (root discovery, asset load, token split)
- `app_ultra_clear_text.cpp`, `ultra_clear_text_controller.cpp`
- `UctOcr.cmake`, the packaging scripts (grep), `setup_assistant.cpp`

Evidence: `actual-runtime-summary-0697367.json` and the small-run detector
attestation.

Not read: the CUDA kernels, `RecognitionScheduler`, `AsyncCleanup`/`RetireOwnedState`,
`uct_precision_experiment.hpp`, the MSI/MSIX internals.

Not determinable statically: TRT DLL imports, behaviour on non-test GPUs, real
deadline sequences (no clock seam exists), Qt canonical-path and junction semantics
(assumed), and the share-mode effect of P3-6 ([I]).
