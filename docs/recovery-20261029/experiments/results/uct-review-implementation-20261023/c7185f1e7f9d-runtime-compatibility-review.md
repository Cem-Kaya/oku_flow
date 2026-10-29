# UCT runtime-compatibility review: removing driver pins, minimum TensorRT version (Opus, 2026-10-23)

This is a separate round from the a6f041ff report (`uct_gpu_independent_opus_review_2026_10_23.md`).
Source read-only at `C:/Users/USER/.t3/worktrees/oku_flow/feature-uct-recap-player`,
HEAD `bb53ac884c24`, with a dirty tree.
Files read: `src/common/uct_tensorrt.cpp`, `src/common/uct_runtime_precision.hpp`,
`tests/uct_paddle_tests.cpp`, and the prior Opus report §7 and §8.2.
I did not read the precision lead's schema-2 or `5c2bc526` sources. Findings about schema 2
are based only on the brief.

**Not run:** no build, tests, GPU, TensorRT, or deserialization. The claim that TensorRT
refuses a plan from an older build comes from NVIDIA's 11.4 engine-compatibility page (by
default, engines load only on the TensorRT version that built them). It is not measured
here. Historical numbers (13040/13050 runs, 49–53 ms, 24e3 detector) stay tied to the plan
files and drivers they used. Nothing here re-measures them.

## Owner policy, as implemented here
- The driver plays no part in any key, equality check, or approval. It is diagnostic only.
- TensorRT check is a minimum version, `>= 11.4.0.106`, inside ABI major 11.
- A newer runtime may *try* an older verified plan. TensorRT's own deserialization decides
  whether it loads.

## Urgent findings (concrete, smallest fix)

1. **The minimum-version comparison must be ordered as a tuple, not checked
   field-by-field.** `uct_tensorrt.cpp:204-210` compares each field for equality. If that is
   changed to "each field `>=`", then 11.5.0.0 is rejected (build 0 < 106) and 12.0.0.0 is
   accepted.
   - Fix: `major == 11 && std::tie(maj,min,pat,bld) >= std::tie(11,4,0,106)`.
   - Major stays at exactly 11, because `nvinfer_11.dll`, `nvonnxparser_11.dll`,
     `*_11.dll` builder resources and the `NV_TENSORRT_VERSION` headers passed to
     `createInfer*_INTERNAL` are the compiled ABI. Exact major 11 is the ABI boundary. It is
     not a version pin. Do not let a `>=` check suggest TensorRT 12 support.
   - Error text: "TensorRT 11.x ≥ 11.4.0.106 required (found X)".
   - Store the loaded tuple in `Libraries` for provenance.

2. **Precision deserialization failure is thrown as the wrong type.**
   - In `uct_tensorrt.cpp:326-332`, a null `deserializeCudaEngine` on the BF16 plan throws
     `std::runtime_error`. That path goes to outer failover and the whole GPU backend fails,
     instead of falling back to FP32.
   - Under the new policy this is the expected result when a newer runtime meets the
     11.4.0.106 plan, because that plan is not `kVERSION_COMPATIBLE`.
   - Deserialization happens before any inference. Nothing has been enqueued on the stream,
     and no output exists.
   - Fix: keep `Check(cudaPeekAtLastError())`. A reported CUDA error still escapes as a real
     fault. Otherwise throw `runtime_precision::ArtifactError("local BF16 plan refused by
     TensorRT <loaded tuple>: " + logger.Error())`.
   - The comment "Exact file/device/runtime checks already passed" becomes false and needs
     rewriting.
   - Leave context creation, allocation and enqueue faults uncaught, as they are now
     (`:1192-1205` catches only `ArtifactError`).

3. **`Unsupported` still enforces exact driver and TensorRT values.**
   `uct_runtime_precision.hpp:89-91` requires `driver != 13040` and
   `tensorRtVersion != "11.4.0.106"`. That is why BF16 falls back to FP32 on 13050 today.
   - Fix: delete both terms. Keep tier, N8, fixed1024, no preparation, `major >= 8` and
     `kDeviceUuid`.

4. **The `Device` fields `driver` and `tensorRtVersion` are traps.**
   - `tensorRtVersion` defaults to the literal `"11.4.0.106"` (`:44`) and is never filled
     from the loaded DLL.
   - `Device` is built at `uct_tensorrt.cpp:1159-1163`, *before* `Libraries` exists (`:1186`).
     Any equality check on it is comparing against a constant.
   - Fix: remove both fields from `Device`. They then cannot reach a key or a comparison.
   - Report the driver and the loaded TensorRT tuple only in `precisionDetail` and status
     text, after `Libraries` is constructed.

5. **The BF16 plan path is derived from the driver.**
   - `PlanPath` calls `PrecisionPlanKey(..., device)`, whose hash includes the driver and the
     TensorRT string (`:71-80`). On 13050 the path would point to a file that does not exist.
   - Fix for schema 1: resolve the approved legacy filename as a constant,
     `kSchema1PlanKey = "e596b8ac0110f01e88fc94bcbdeda4511b83af90efb761aa4e194de3ea496073"`.
     That is `PrecisionPlanKey` with 13040 and 11.4.0.106. It already exists as the test's
     independent value (`tests/uct_paddle_tests.cpp:234`).
   - The whole-file pin `kPlanFileSha256` (155c74…) remains the approval. The filename is
     only a lookup.
   - This reuses the existing BF16 plan on 13050 with no rebuild, no rename and no copy.
   - Keep `PrecisionPlanKey` as a frozen v1 *legacy* function. A new v2 key, for future
     artifacts, drops the driver and TensorRT version.

6. **The ordinary FP32 cache key embeds the driver and a literal TensorRT label.**
   - `uct_tensorrt.cpp:303-308`: `graphSHA + UUID + driver + "-trt11.4.0.106-fp32-v1" + profile`.
   - Once the runtime check is a minimum, the `trt11.4.0.106` label is wrong on any newer
     runtime, and every driver update causes a cache miss.
   - There is a coupling hazard. Changing the key without legacy lookup **breaks every
     cache-only start**. Precision-requested runs build the detector with
     `allowPreparation=false` (`:1187-1190`). So does bucketed width without preparation.
     A key miss there throws "OCR plan is missing/invalid", which fails the whole backend.
   - The key change and the legacy read path must land in the same commit.
   - Fix:
     - **New key (v2):** `graphSHA + UUID + "-fp32-v2" + profile tag`. No driver, no
       TensorRT version.
     - **New file format:** magic `OKUTRT2`, then builder TensorRT tuple and build-time
       driver (provenance only), then the 32-byte payload SHA, then the payload. The SHA
       covers header and payload.
     - **Read order:**
       1. v2 key.
       2. Only if the loaded runtime is exactly 11.4.0.106 (the only builder of v1 files),
          the legacy v1 keys with the current driver, then known 13050, then 13040. Accept
          the first file whose SHA prefix verifies and that deserializes.
       3. Rebuild only if `allowPreparation`. Write only to the v2 key. Never delete or
          overwrite v1 files.
     - Put the selected file stem and its payload SHA in status. Then historical vs new
       measurements stay attributable when both a 13040 and a 13050 FP32 plan exist; they
       can differ in tactics.

7. **The parser version is unchecked.** `PrepareBuilder` loads `nvonnxparser_11.dll`
   without a version check. With a minimum-version policy, a mixed SDK folder becomes
   possible.
   - Smallest fix: check the parser's major version against the loaded `nvinfer` major
     version, and record it as provenance.
   - Separately, the default SDK path is hard-coded to `TensorRT-11.4.0.106-cuda12.9`
     (`:1314-1317`, `:1325-1326`), and the status says "TensorRT 11.4 runtime"
     (`:656`). A newer runtime arrives only through `OKUFLOW_UCT_TENSORRT_ROOT` or a
     changed default. Status should print the loaded tuple, not "11.4".

## When a rebuild is actually needed, and what the app does

| Change | Ordinary FP32 cache | Approved BF16 (schema 1) |
|---|---|---|
| Driver only (13040→13050→later) | Load via v2, or via the legacy current-driver/13050/13040 key. **No rebuild.** | Fixed filename plus whole-file hash. **No rebuild.** Loads on 13050. |
| TensorRT 11.x > 11.4.0.106 | v2 plan built by the older version: TensorRT refuses at deserialization (null, no CUDA error). Rebuild into v2 if preparation is allowed; otherwise a typed "plan refused by runtime X; preparation required" and CPU OCR. Legacy v1 is skipped. **Rebuild needed.** | Typed `ArtifactError` (finding 2), FP32 recognizer, status names both versions. The BF16 plan cannot be used until a new artifact is built and approved under the new runtime. **Rebuild plus new approval needed.** |
| GPU UUID or model change | Key miss, then rebuild. | `Unsupported` refusal, then FP32. |
| Graph, profile or precision change | Key miss, then rebuild. | Manifest or hash refusal, then FP32. |

Optional, not required: to skip a deserialization that is known to fail, v2 files can compare
the header's builder tuple with the runtime and skip early when they differ. Calling
deserialization anyway is also correct, and matches "newer can try older" literally.

For future BF16 artifacts (schema 2), the approval binds:
- graph, source and device UUID;
- profile and configuration;
- the whole-plan hash;
- the *recorded* builder TensorRT tuple, as provenance.

It must not bind the driver, `nvcuda.dll` hashes or TensorRT DLL hashes. Those must leave the
`5c2bc526` approval schema, not just the key. Only building with `kVERSION_COMPATIBLE`
(restricted, with a lean runtime and possibly lower speed) would make a plan load on later
11.x runtimes. That is a separate decision; nothing here assumes it.

## Model-free tests (no GPU, no TensorRT)
- Version predicate (a pure helper):
  - accepts 11.4.0.106, 11.4.0.107, 11.4.1.0 and 11.5.0.0;
  - rejects 11.4.0.105, 11.3.9.999, 12.0.0.0 and 10.x.
- `Unsupported` is empty for drivers 13040, 13050 and 13060. `Device` has no TensorRT or
  driver field. Update the inverted assertions at `uct_paddle_tests.cpp:239-240` and
  `:259-263`.
- `PrecisionPlanKey` v1 frozen value is still `e596b8ac…` with 13040 and 11.4.0.106.
  Schema-1 `PlanPath` equals `<root>/engines/bf16-linear/e596b8ac….plan` regardless of the
  current driver.
- The v2 ordinary-key helper (extract the identity build from `Network` into a pure
  function): independent of driver and TensorRT version; changes with graph, UUID and
  profile.
- Legacy candidate list:
  - for runtime 11.4.0.106 and current driver 13060 → [legacy(13060), legacy(13050),
    legacy(13040)];
  - for runtime 11.5.x → empty.
- v2 header round-trip: a corrupted header, payload or SHA is rejected.
- Read-only: legacy lookup creates, renames and deletes nothing (directory listing before and
  after, as in the existing `:296-300` pattern).

## Verdict
The root design is correct and matches owner policy, with these conditions:
- the tuple-ordered minimum inside exact ABI major 11 (finding 1);
- deserialization refusal reclassified as a typed `ArtifactError`, with CUDA errors still
  escaping (finding 2);
- `Device.driver` and `Device.tensorRtVersion` removed, not just ignored (findings 3–4);
- the schema-1 BF16 plan resolved by its fixed approved filename (finding 5);
- the FP32 v2 key landing together with read-only v1 legacy lookup (finding 6).

With these, the existing BF16 plan runs on 13050 without a rebuild. Driver updates never
force a rebuild. A newer TensorRT 11.x is allowed and tries every plan; plans not built as
version-compatible are refused by TensorRT, which leads to a typed FP32 or CPU fallback and a
rebuild (FP32) or rebuild plus re-approval (BF16). That rebuild is the only one that is truly
necessary.

## Addendum: owner decisions from follow-up conversation (2026-10-23)
- **BF16 shipping (owner decision):** ship the BF16 *model*, not a pre-approved plan.
  - Approve the converted BF16-linear ONNX graph (source plus graph hash).
  - Each user machine builds its own engine, like FP32 does today.
  - Before use, a local golden-crop check confirms the built engine. If the check fails,
    the recognizer stays on FP32.
  - BF16 is offered only on Ampere or newer (`major >= 8`).
  - The schema-1 hash-pinned plan (`155c74…`, owner's card UUID) stays a dev/owner
    artifact, not the shipping path.
  - Prebuilt Ampere-plus hardware-compatible engines remain a possible later
    optimization. They are untested and not required.
- **TensorRT redistribution:** the 11.4 SLA (v. May 24 2021, TensorRT supplement v. Feb 25
  2021) allows "the runtime files .so and .dll".
  - The app must have material additional functionality.
  - The DLLs may be accessed only by our application, so they belong in a private app folder
    (the current `AddDllDirectory`/full-path loader already does this).
  - Headers, tools and samples must not be shipped.
  - The builder-resource and parser DLLs are not named individually; written confirmation
    from NVIDIA is advised before public release.
  - The CUDA runtime DLLs fall under the separate CUDA license list. I have not checked it.
- **TensorRT version:** 11.4.0 is marked latest on NVIDIA's release-notes page. The zip
  packages are CUDA 13.x builds (13.4). The CUDA 12.x builds ship as the `tensorrt-cu12`
  wheels.
  - We use 11.4.0.106 for CUDA 12.9.
  - Choosing CUDA 12.9 or CUDA 13 for the installer is still open. It mostly affects the
    users' minimum driver; I have not checked the exact minimums.
- **GPU floor:** TensorRT 11.4 requires SM 7.5 or newer. The GTX 10-series (Pascal) and
  Volta use CPU OCR, which the existing gate at `uct_tensorrt.cpp:663-666` already does.
  The GTX 16-series is supported.
