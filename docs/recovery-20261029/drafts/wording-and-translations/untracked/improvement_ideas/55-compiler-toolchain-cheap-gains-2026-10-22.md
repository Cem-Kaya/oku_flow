# Compiler / toolchain cheap gains — audit 2026-10-22

Scope: flags, defaults and toolchain settings only. No kernel or algorithm
rewrites (see 03-performance-gpu-cpu.md and 44-... for those). Hard product
constraint given by the owner: **GTX 10-series (sm_61, Pascal) and RTX
50-series (sm_120, Blackwell) must both keep native support.** That keeps
CUDA 12.9 as the release compiler (CUDA 13 dropped Pascal offline targets).

All measurements below were taken on the dev machine (Ryzen 9 5900X, MSVC
14.44.35207 / cl 19.44.35229, Qt 6.12.0, CUDA 13.4 probe compiles) against
the real compile commands from `compile_commands.json`. Probe scripts and
scratch builds live outside the repo (`%TEMP%\okuflow-flag-probe`,
`%LOCALAPPDATA%\OkuFlow\{ipo,noipo}-probe`). Nothing in this document has
been applied yet; it is a decision list.

## Finding 0 (fix before any "gain"): the current dist bundle has no Pascal code

`cuobjdump --list-elf -all dist\OkuFlow\app\OkuFlowApp.exe` shows 27 cubins
for sm_75/80/86/89/90/100/103/120/121 plus 18 PTX images — **zero sm_61**.
A GTX 10-series user gets "no kernel image" failures from that bundle.
Causes:

- Only CUDA v13.4 is installed on this machine (`C:\Program Files\NVIDIA GPU
  Computing Toolkit\CUDA` contains only `v13.4`). 12.9 is not present.
- `cmake/CudaTargets.cmake` correctly prepends `61` only for compilers
  < 13.0, and `tests/cuda_target_policy.cmake` blesses the 13.x inventory
  without 61. `tests/check_cuda_architectures.cmake` only verifies the
  inventory it is handed, so a 13.x-built release passes its own check.
- `cmake/CMakeLists.txt:33` fatals on default CUDA >= 13, but an explicit
  `OKUFLOW_CUDA_TOOLKIT_ROOT` pointing at 13.4 builds fine and silently
  drops Pascal.

Fix: install CUDA 12.9 side-by-side, build releases with
`OKUFLOW_CUDA_TOOLKIT_ROOT` pinned to it, and add a **shipping** inventory
assertion with a fixed expected list (at minimum `61` and `120`) run against
the final bundle executable, independent of the per-compiler policy function.

## Current effective settings (evidence)

From `build/msvc-release/CMakeCache.txt` + `build.ninja` (Release):

- CXX: `/O2 /Ob2 /DNDEBUG /EHsc -std:c++20 -MT -Zc:__cplusplus -permissive-
  -utf-8` — plain CMake defaults; `/O2` already bundles `/Og /Oi /Ot /Oy
  /GF /Gy`. No `/GL`, no `/arch:` (SSE2 baseline), no warning level flag
  (compiler default). PCH on (`cmake/PrecompiledHeaders.cmake`).
- Link: `/machine:x64 /INCREMENTAL:NO /subsystem:windows`. With no `/DEBUG`,
  link.exe already defaults to `/OPT:REF,ICF,LBR` (MS docs), so dead-code
  and identical-COMDAT folding are active implicitly.
- CUDA: host side `-Xcompiler="-O2 -Ob2"`, C++17, `CUDA_SEPARABLE_COMPILATION
  ON` (`-rdc=true` + a separate device-link step), static cudart, and the
  10-architecture inventory from `cmake/CudaTargets.cmake`
  (61, 75, 80, 86, 89, 90, 100, 103, 120, 121 on 12.9). CMake emits each as
  `arch=compute_X,code=[compute_X,sm_X]`: SASS **and** PTX per architecture.
- No explicit nvcc optimization flags (no `--use_fast_math`, no `--threads`,
  no ptxas options). nvcc device default is already `-O3`.

## Tier 1 — cheap, measured or mechanical, near-zero risk

### 1.1 `nvcc --threads 0` (build time, measured 3.8x on the heavy TU)

nvcc compiles the 9–10 gencode targets serially today. Replaying the real
`src/cuda/cuda_kernels.cu` (3632 lines) compile:

| Variant | Time | Object bytes |
| --- | --- | --- |
| 9 archs, current flags (`-rdc=true`) | 26.5 s | 2,082,479 |
| 9 archs + `--threads 0` | **7.0 s** | 2,082,479 (identical size) |
| 3 archs, current flags | 11.0 s | 1,005,335 |
| 3 archs + `--threads 0` | 6.5 s | 1,005,335 |

`--threads 0` = one compiler thread per CPU for the per-arch passes; output
is unchanged. On 12.9 (10 archs) the absolute saving is larger. Apply to
`oku_flow`, `cuda_toolchain_benchmark` (`cmake/CMakeLists.txt`) and the CUDA
test targets:

```cmake
target_compile_options(oku_flow PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--threads=0>)
```

Caveat: Ninja already parallelizes across the 3 `.cu` files, so peak compiler
processes multiply; fine at 3 TUs, revisit if `.cu` count grows (Ninja job
pool for CUDA as a brake).

### 1.2 Drop redundant per-arch PTX (`*-real`), keep all SASS + top PTX

Only the highest PTX is ever used (JIT for future GPUs); every real device
with a matching/major-compatible cubin takes SASS. The current bundle carries
18 PTX images of which at most one per TU is useful. In
`cmake/CudaTargets.cmake`:

```cmake
set(architectures 75-real 80-real 86-real 89-real 90-real 100-real 103-real 120-real 121)
# + 61-real prepended on < 13.0, as today
```

This keeps native sm_61 (GTX 10) and sm_120 (RTX 50) exactly, keeps
`compute_121` PTX for next-gen JIT, and shrinks the fatbin (today's fatbin
section is ~5.4 MB of the 11.9 MB `OkuFlowApp.exe`) and module-load work.
Follow-ups: update the expected list in `tests/cuda_target_policy.cmake`
and check release-facts consumers (`cmake/ReleaseBuildFacts.cmake` writes
`CUDA_ARCHITECTURES` verbatim) tolerate the `-real` suffix. Optional extra
squeeze: `--compress-mode=size` (12.9 default is already `balanced`).

### 1.3 Turn `CUDA_SEPARABLE_COMPILATION` off (nothing uses RDC)

Evidence: no `__device__`/`__global__` declarations in
`include/okuflow/cuda/*.hpp` (host `Launch*` wrappers only), no kernel
launches outside the 3 `.cu` files, no `extern __device__` anywhere. All
60 device helpers are TU-local. RDC therefore buys nothing and costs a
device-link step, `cudadevrt`, and deferred device-code optimization.
Flipping `cmake/CMakeLists.txt:62` to `OFF` fails loudly at link time if a
cross-TU device call ever appears — a safe failure mode. Per-TU compile time
measured neutral (11.0 s vs 11.4 s at 3 archs); the gain is the removed
device link and full per-TU inlining. Mirror the change in the CUDA fixture
targets and `tests/shared_sources.cmake`'s property inventory. Validate with
the serial `gpu`-label fixtures.

### 1.4 Precompile the 3 D3D12 shaders offline (startup)

`src/d3d12/presenter.cpp:51` compiles HLSL at runtime with `D3DCompile(...,
D3DCOMPILE_OPTIMIZATION_LEVEL3, ...)` for `VSMain`, `ScenePSMain`,
`AnnotationPSMain` (lines 1127–1131) on the presenter init path, and the exe
hard-imports `D3DCOMPILER_47.dll`. Compiling the same source offline with
the SDK `fxc.exe /O3 /Fh` in a CMake custom command and embedding the DXBC
removes 3 runtime compiles and that import. Same compiler family as runtime
today (lowest risk; DXIL via dxc is the follow-up if wanted). Measure with
`scripts/profile_startup.ps1` before/after; unmeasured here because the
owner desktop was not available for a live launch.

### 1.5 Private PDB for Release (diagnostics, ~zero runtime cost)

Release currently builds with no `/Zi` at all: user crash dumps cannot be
symbolized. Use embedded debug info + a private PDB that stays out of the
bundle (CI already builds this way for sccache:
`.github/workflows/windows-checks.yml:125`):

```cmake
-DCMAKE_POLICY_DEFAULT_CMP0141=NEW -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
# link: /DEBUG:FULL /OPT:REF /OPT:ICF
```

Trap to pin at the same time: adding `/DEBUG` flips link.exe defaults to
`/OPT:NOREF,NOICF`, so `/OPT:REF /OPT:ICF` must become explicit the moment
any debug info is enabled. Pin them regardless — zero change today,
protection tomorrow.

## Tier 2 — cheap to try, keep only with measurements

### 2.1 `--use_fast_math`, scoped away from UCT

Probe (3 archs, no-rdc): object 2,085,009 → 1,757,825 bytes and 11.4 s →
9.4 s; the device code is genuinely smaller (approximate div/sqrt/
transcendentals, FTZ). Presentation kernels (color conversion, keystone,
blur/sharpen, FSR/NIS in `cuda_kernels.cu` / `spatial_upscalers.cu`) are the
natural beneficiaries. Precision risk is real but testable: GPU fixtures mix
integer-exact checks (alpha path — unaffected) with ±1 LSB tolerances
(`tests/text_clarity_cuda_tests.cu`, `tests/spatial_upscaler_cuda_tests.cu`).
Keep `src/cuda/uct_kernels.cu` precise (its thresholds feed OCR decisions) by
scoping the flag per-source. Gate: full `gpu` fixture run + pixel A/B.

### 2.2 `/GL` + `/LTCG` for the release preset only (probed)

Same-tree A/B (CPU config, app target, scratch builds):

| | Plain | `/GL`+`/LTCG` |
| --- | --- | --- |
| `oku_flow.exe` | 6,470,144 B | 6,396,416 B (−1.1%) |
| Clean build wall | ~33 s | ~32 s |
| App link step | 0.7 s | 16.4 s |

It compiles and links cleanly today (PCH and `/MT` included). Runtime gain
is **unmeasured**; the app is GPU-bound, so expect low single digits on CPU
orchestration at best. The 16 s relink penalty makes it release-only
(`CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` in the bundle/release preset, never
dev presets). Before adopting for the CUDA build, probe the combination
explicitly: CMake IPO also speaks CUDA (device LTO wants separable
compilation + nvJitLink), which conflicts with 1.3 — and is pointless here
since the `.cu` files share no device functions. Target: CXX-only LTCG with
RDC off; nvcc host objects (no `/GL`) link fine under `/LTCG`. Decide with
`assistant_chat_benchmark` + `scripts/profile_startup.ps1` A/B. PGO
(`:PGINSTRUMENT`/`:PGOPTIMIZE`) is the heavier follow-on; only worth it
with a representative profile from the lecture-camera harness.

### 2.3 Dev-loop build time

- Opt-in dev override for the arch inventory, e.g.
  `-DOKUFLOW_CUDA_ARCHITECTURES=native` for local presets: 1 arch instead
  of 10. Release/bundle paths must refuse non-inventory values (release
  facts already record the effective list). Combined with 1.1 this makes
  `.cu` iteration ~2–3 s/file.
- Local `sccache` launcher mirroring CI (`.github/workflows/windows-checks.yml:105`):
  not installed on this machine. Needs the Embedded debug-info setting from
  1.5. Gains land on branch-switch/clean rebuilds, not single-file edits.
- Non-flag, noted for completeness: `src/cuda/cuda_kernels.cu` is 3632
  lines; every edit recompiles all of it × 10 archs. Splitting it is the
  structural fix 1.1 only mitigates.

### 2.4 Micro: `/Ob3`, `/Gw`, delay-load TTS

`/O2` already includes `/Gy /GF /Oi /Ot /Oy /Ob2`. `/Ob3` (more aggressive
inlining) and `/Gw` (per-global COMDATs so `/OPT:REF` can prune data) are
one-line experiments to bundle into the 2.2 A/B; expect < 1–2%.
`/DELAYLOAD:Qt6TextToSpeech.dll` (+ `delayimp.lib`) trims startup imports
for users who never use TTS; small next to Qt plugin loading.

## Rejected after checking (do not spend time here)

- **`/arch:AVX2` (and `/fp:fast`) globally.** Probed with `/Qvec-report:2`
  on the CPU per-pixel files: vectorized-loop count is 2 → 2
  (`src/common/image_processing.cpp`) and 0 → 0
  (`src/common/frame_pipeline.cpp`); `/fp:fast` adds exactly one loop.
  MSVC's vectorizer is blocked by loop structure (calls/aliasing), not ISA.
  The flag would hard-crash pre-Haswell CPUs for ~no gain — a realistic
  CPU-only user segment for this product. The real fix is loop
  restructuring/intrinsics (03-... P7), not a flag. If the baseline is ever
  raised, gate it with a cpuid check in `okuflow_launcher`.
- **clang-cl / Intel ICX as "a better compiler".** Qt 6.12's official
  binaries are MSVC-ABI (linkable from clang-cl), but nvcc on Windows
  requires MSVC `cl.exe` as host compiler, so the shipping CUDA build cannot
  move; mixed objects also forfeit `/GL`. Not installed here either
  (no VS LLVM component). The free version of this idea — newest MSVC
  toolset — is already in effect (14.44; the stale 14.34 also present is
  not selected by VsDevCmd).
- **CUDA 13 as default.** Drops sm_61; violates the GTX 10-series
  requirement. The 12.9 pin in `cmake/CMakeLists.txt:33` is correct.
- **`/Ox`, `/Qpar`, `/GR-`.** `/Ox` is a subset of `/O2`; `/Qpar` rarely
  fires on this code and adds runtime risk; `/GR-` fights Qt RTTI usage.
- **Unity builds.** 101 TUs with file-local state plus the shared-source
  equivalence machinery make this a breakage-prone refactor, not a flag.
  Clean app-only build measured ~32 s wall locally — not the pain point.
- **Build directory on local disk vs the Drive mount.** Checked: per-TU
  times are comparable (e.g. `main_window.cpp` 14.7 s in `build/msvc-cpu`
  vs 15.3 s in a LocalAppData scratch build). `cl.exe` CPU time dominates;
  no gain. Existing `OKUFLOW_*_BUILD_DIR` escapes remain for file locks.
- **Static Qt.** Requires building Qt from source and triggers LGPL
  static-link obligations (`docs/THIRD_PARTY_LICENSES.md`). Not cheap.
- **Architecture pruning beyond 1.2.** Datacenter-only targets (80/90/100/
  103, and 121 = GB10) could in theory ride PTX JIT instead of native SASS.
  That trades first-launch JIT latency on those devices for build time and
  size, and it is a support-policy decision, not a flag tweak. Consumer
  natives 61/75/86/89/120 stay regardless per the owner's constraint.
- **Android R8 shrink/optimize.** Release deliberately uses
  `android/app/diagnostic-r8.pro` with shrinking/optimization off for
  diagnostic mapping fidelity. APK size is available there, but it
  conflicts with that policy and is a product decision.

## Validation path for whatever gets adopted

1. `scripts/agent_check.ps1` scoped to the touched CMake files.
2. `scripts/agent_build.bat` full matrix (release + CPU + CUDA delta),
   GUI portion subject to the desktop rules in AGENTS.md.
3. `cuobjdump --list-elf` inventory on the produced executable with the
   fixed shipping expectation from Finding 0; `tests/check_cuda_architectures.cmake`
   already provides the mechanism.
4. Kernel timing A/B with the existing `cuda_toolchain_benchmark` target;
   startup A/B with `scripts/profile_startup.ps1` (serial, free desktop).
5. Docs in the same change: `docs/developer_guide.md` (build flags),
   `docs/code_reference.md` if target properties change, `CHANGELOG.md`
   for the Pascal-bundle fix.
