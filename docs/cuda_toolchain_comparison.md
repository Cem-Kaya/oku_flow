# One-time CUDA 12.9 / 13.4 comparison

Measured 2026-10-15 on an NVIDIA GeForce RTX 4090, driver 617.42, Windows x64.
This is a single bounded experiment, not a recurring test, performance gate or
GTX 1070 benchmark. No camera, phone display, provider or Maxine runtime was used.

## Toolkits and builds

CUDA 12.9 Update 2 is the latest 12.x release in NVIDIA's archive. Its official
[redistrib manifest](https://developer.download.nvidia.com/compute/cuda/redist/redistrib_12.9.2.json)
pins the Windows component ZIPs. SHA-256 was checked before extraction into
`%LOCALAPPDATA%/OkuFlow/toolchains/cuda-12.9.2/`; individual notices and the
installation receipt remain there. The assembled `toolkit/` includes nvcc
12.9.86, cudart 12.9.79, CCCL, nvrtc, cuobjdump, nvdisasm, nvprune and Visual
Studio integration. The existing CUDA 13.4.92 toolkit and driver were unchanged.

The owner-scoped `OKUFLOW_CUDA_TOOLKIT_ROOT` pointer selects this 12.9 toolkit
for fresh OkuFlow builds without changing the system's `CUDA_PATH` or PATH.
CUDA 12.9 is the project default. A default configure that discovers CUDA 13
fails with an instruction to select 12.9; 13.x requires an explicit toolkit
root for comparisons.
Use a fresh build directory when changing it. Set the CMake cache variable of
the same name explicitly to select another toolkit. Ninja uses the portable
toolkit directly; Visual Studio generators also need the matching integration
and CUDA toolset selected. The bundle script takes the EULA from the selected
root before considering other CUDA environment variables.

The release configuration compiled successfully with 12.9.86 / MSVC 19.44.
The shared architecture inventory adds native `sm_61` for the intended GTX 1070
test system and retains `75;80;86;89;90;100;103;120;121`. CUDA 13 excludes `61`.
Cubin presence is checked separately from hardware execution. This does not
enable Maxine on Pascal, which lacks the required Tensor Cores.

## Method

Four standalone executables compile the same benchmark and production
`cuda_kernels.cu` / `spatial_upscalers.cu`: nvcc 12.9.86 and 13.4.92, each with
MSVC 14.34 and 14.44. Identical flags:
`-std=c++17 -O3 -arch=sm_89 -Xcompiler=/O2,/MT,/utf-8`.
No fast-math, clock locking, power setting, driver or display change was made.

Twenty linear-kernel stages are measured at 1920x1080 and 3840x2160: zoom,
black/white, Gaussian blur, temporal smoothing, rotation, keystone warp, NV12
conversion, color grading, auto contrast, text statistics, Sauvola, stroke
weight, mask hysteresis/compositing, smart sharpen, stabilization luma,
feature pairs/warp, NIS 2x and FSR EASU+RCAS 2x. Composite stages include their
production wrapper launches; this is not every kernel or a whole-frame pipeline.

Each measurement uses 20 warmups followed by 31 batches of ten calls. CUDA
events measure device elapsed time; host submission medians are separate.
Allocations, initial uploads and checksum readback are outside timing.
Small-stage event spans can include gaps while the host submits launches;
they do not isolate device instruction throughput from submission behavior.
Inputs are deterministic and device-resident; repeated inputs/caches do not model
streaming camera traffic. Each executable ran twice in a balanced forward /
reverse sequence, with no concurrent compilation or other OkuFlow GPU process.
Reported values average the two run medians. Natural clock/thermal and desktop
load variation remains; small differences do not establish a significant win.

## Results

Across all 40 stage/size pairs, the geometric mean CUDA 12 / CUDA 13 timing
ratio was **0.9942 with MSVC 14.44** and **0.9969 with MSVC 14.34**: under 1%
overall difference in this experiment. Individual cases favor either compiler.
This equal-weight summary is not an FPS estimate or total pipeline speedup.

Selected 4K device timings, milliseconds per stage:

| Stage | CUDA 12 / MSVC 14.44 | CUDA 13 / MSVC 14.44 |
| --- | ---: | ---: |
| Zoom | 0.03072 | 0.03052 |
| Gaussian blur | 0.17433 | 0.17099 |
| Text local statistics | 2.43564 | 2.43123 |
| Smart sharpen | 0.20694 | 0.21217 |
| Stabilization feature pairs | 0.85042 | 0.87281 |
| NIS 2x | 0.39972 | 0.37999 |
| FSR EASU + RCAS 2x | 0.27197 | 0.27325 |

MSVC 14.34 / 14.44 geometric-mean device-time ratios were 0.9866 for CUDA 12
and 0.9839 for CUDA 13. That small observed difference does not justify changing
the project's supported VS2022 baseline. **All 40 selected output-buffer
checksums matched across all four variants and both runs.** Multi-output stages
checksum one selected result (for example feature-pair count or text mean),
not every intermediate buffer. Existing correctness fixtures remain the
behavioral authority; checksums alone are not correctness proof.

CUDA 12.9 is a reasonable compatibility choice for testing older GPUs: this
measurement found no broad kernel-performance penalty on the RTX 4090.
GTX 1070 throughput, D3D12/driver interop, Maxine compatibility on RTX, live
camera presentation and end-to-end latency still require their own acceptance.

## Retained evidence

The tracked `scripts/agent_build.bat` completed release compilation and the
desktop-restricted suites: **43 CPU and 49 CUDA CTest cases passed**, with no
skipped Qt rows or unresolved cases. CUDA coverage includes YUV conversion,
Text Clarity, stabilization, spatial upscaling, replay and the native cubin
inventory. GUI cases were explicitly excluded; this is not full release,
installer, physical GTX 1070 or Maxine acceptance. The final source also
corrects documentation/comments about PTX: CMake embeds both PTX and native
images, while the architecture gate specifically requires the native images.
The toolkit license-priority/normal-return/missing-license paths were checked
separately without running the publisher or creating an installer.

`%LOCALAPPDATA%/OkuFlow/cuda-toolchain-comparison-20261015/` retains compiler
commands/logs, executable hashes, telemetry, eight raw JSONL runs,
`benchmark-receipt.json` and `comparison.json`. It also holds the CUDA 12
release build. Original failed benchmark linking evidence is retained; the
fixed command includes both production CUDA translation units.

The optional `cuda_toolchain_benchmark` CMake target is `EXCLUDE_FROM_ALL` and
has no CTest registration. Build/run it only for an explicitly requested future
investigation; normal checks do not rerun this experiment.
