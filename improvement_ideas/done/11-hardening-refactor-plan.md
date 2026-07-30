# Plan 11 — Remaining Stability + Performance + Architecture Work (2026-07-23)

This plan implements the open remainder of three older analysis documents:
plan 01 (stability/threading items S4 and S6b), plan 03 (performance items
P4, P6, P8, P11, P12–P15), and plan 02 (architecture items A1–A5). It is
organized as four waves that MUST be executed strictly in this order:
measurement before optimization, hardening before refactor, and the refactor
last and incremental. The reasoning behind the ordering is not stylistic —
each wave produces something the next wave depends on. Wave 1 built the
always-on frame-timing instrumentation that Wave 3 uses to prove or refute
every optimization; Wave 2 hardened the D3D12/CUDA fence protocol so that the
performance work in Wave 3 (which reorders GPU work and adds asynchrony)
cannot silently corrupt the present pipeline; and Wave 4 restructures
`src/app/app.cpp` only after the code it moves has stopped changing.

Each wave is one agent pass. A wave is complete only when the build is green,
a manual smoke test passes, and CHANGELOG.md plus docs/code_reference.md have
been updated. The build recipe is the standard `build/agent_build.bat` + pwsh
bridge loop; it is spelled out in full detail in plan 13 ("Build recipe —
fully spelled out"), which also defines the report format the reviewing agent
expects. Plans 09 and 13 agree on the recipe; plan 13 is the more detailed
statement.

Orientation for an agent with no prior context: the application core is
`src/app/app.cpp` (~4,169 lines) with its header at
`include/openzoom/app/app.hpp` (note: the header is NOT in `src/app/`; all
public headers mirror the source tree under `include/openzoom/`). The CUDA
interop surface and frame pipeline live in `src/cuda/cuda_interop.cpp`
(~2,252 lines, header `include/openzoom/cuda/cuda_interop.hpp`), and every
GPU kernel plus its launch wrapper lives in `src/cuda/cuda_kernels.cu`
(~2,033 lines, header `include/openzoom/cuda/cuda_kernels.hpp`). Settings
persistence is `src/app/settings_store.cpp` with
`include/openzoom/app/settings_store.hpp`. Line numbers cited below were
verified against the tree at the time of writing; treat them as "within a few
dozen lines" anchors, not exact offsets, and re-locate symbols by name.

## Wave 1 — Measure first (P8) — DONE 2026-07-23

Implemented: `OnFrameTick` wall-clock via `QElapsedTimer` with a 60-frame
rolling average (`RecordFrameTickSample`, app.cpp); GPU `cudaEvent` pair around
the ProcessFrame kernel chain sampled every 30th frame
(`CudaInteropSurface::ConsumeProcessTiming` / `LastGpuFrameMs()`); both shown
in the status tooltip ("NN ms/frame - CUDA|passthrough - GPU NN ms"); qWarning
after the average stays above 40 ms for 3 s. Always on, including release.

Expanded completion notes, for anyone who needs to read or extend the
instrumentation (Wave 3 depends on it for every measurement):

- CPU side. `OpenZoomApp::OnFrameTick()` (src/app/app.cpp ~3609) starts a
  `QElapsedTimer`, runs the real tick body `RunFrameTick()` (~3647), and — only
  when the tick actually processed a frame (`frameTickProcessedFrame_`, set at
  ~3672 once a non-empty camera frame was picked up) — feeds the elapsed
  nanoseconds into `RecordFrameTickSample()` (~3619). That function maintains a
  60-entry ring (`frameTickSamplesMs_` and friends, declared with an
  explanatory comment block at include/openzoom/app/app.hpp ~519–531) and a
  running sum, so the rolling average is O(1) per frame. Idle ticks are
  deliberately excluded so they cannot dilute the average.
- GPU side. `CudaInteropSurface::ProcessFrame()` records
  `processTimingStartEvent_` right after the external-semaphore fence wait
  (src/cuda/cuda_interop.cpp ~1584–1602 — the placement is intentional: the
  sample covers upload + kernels, not time spent waiting for the graphics
  queue) and `processTimingStopEvent_` just before the fence signal
  (~2212–2216). Sampling happens on every 30th frame; results are consumed by
  polling (`cudaEventQuery`, never a blocking wait) in
  `ConsumeProcessTiming()` (~1016–1033) and surface through
  `LastGpuFrameMs()` (include/openzoom/cuda/cuda_interop.hpp ~177–180).
- Display. `UpdateProcessingStatusLabel()` (src/app/app.cpp ~2790–2803)
  appends the line `"NN.N ms/frame - CUDA|passthrough - GPU NN.N ms"` to the
  status label's TOOLTIP (the corner label itself stays short on purpose).
  Hover the colored processing status text in the bottom corner to read it.
- Alarm. `RecordFrameTickSample()` warns via `qWarning` once per episode when
  the rolling average stays above 40 ms for 3 seconds (~3627–3644), and
  re-arms after recovery.

1. CPU side: wall-clock OnFrameTick with a 60-frame rolling average; expose
   "NN ms/frame · CUDA|passthrough" in the status tooltip (keep the corner
   label short). Log a qWarning when the average exceeds 40 ms for 3 s.
2. GPU side: cudaEvent pair around the ProcessFrame kernel chain, sampled
   every 30th frame (events are cheap but not free); report ms in the same
   tooltip. All instrumentation always-on (release builds included) — it is
   the regression alarm for every later wave.
3. NO degradation policy yet (that needs data first); just measurement.

Acceptance: numbers visible on hardware; overhead itself < 0.1 ms/frame.

## Wave 2 — Stability remainder (S6b + S4) — DONE 2026-07-23

Implemented: `FenceSequencer` struct in app.hpp owning the fence timeline with
`BeginCudaFrame()` (reserve) / `CudaSignaled()` (commit) / `CudaFailed()`
(rollback) / `GraphicsSignaled()` / `ReadbackObserved()`; contract documented
on the struct. Three consecutive ProcessFrame failures trigger
`presenter_->WaitForIdle()` + `ResetCudaFenceState()` + one status message
(`HandleCudaProcessingFailure`). S4 audit findings (all verified safe /
refuted, with file:line evidence) recorded in improvement_ideas/01.

Expanded completion notes — Wave 3 and Wave 4 both touch this machinery, so
here is exactly where it lives and how it is used today:

- The `FenceSequencer` struct is defined in include/openzoom/app/app.hpp
  (~89–142) with the full S6b contract written as a comment block directly
  above it (~68–88). One monotonic timeline on the shared D3D12 fence: per
  CUDA frame, CUDA waits `max(lastGraphicsSignal, lastReadbackSignal)` and
  signals a reserved ticket value; the graphics present waits that CUDA
  signal and signals the next value; async readback copies ride the graphics
  queue and `ReadbackObserved()` records the newest fence value covering such
  a copy. Two rules are encoded: handed-out values strictly increase
  (`BeginCudaFrame()` re-seeds from the presenter's last signaled value so
  interleaved CPU-path/debug frames can never move the fence backwards), and
  a value becomes a wait target only after its signal was actually enqueued
  (`BeginCudaFrame()` merely reserves; `CudaSignaled()` commits;
  `CudaFailed()` rolls the reservation back so nobody ever waits on a value
  that will never be signaled).
- The single call site is `OpenZoomApp::RunCudaPipeline()`
  (src/app/app.cpp ~3209–3256): ticket issued at ~3215–3222, rollback on
  ProcessFrame failure at ~3226, commit at ~3237, present wait/signal pair at
  ~3242–3256, and readback observation in `HandleGpuFramePresented()`
  (~3317). The app-level recovery policy is
  `HandleCudaProcessingFailure()` (~2923–2934): the third consecutive failure
  drains the graphics queue (`presenter_->WaitForIdle()`), re-seeds the
  timeline via `ResetCudaFenceState()` (~2911–2916), and shows ONE status
  message instead of per-frame spam. `consecutiveCudaFailures_` is reset to
  zero on the first successful frame (~3234).
- The S4 audit conclusions are recorded in improvement_ideas/01 under S4
  (status REFUTED with file:line evidence): the only cross-thread traffic is
  `latestFrame_`, written by the Media Foundation capture callback under
  `cameraMutex_` (app.cpp ~3372–3375) and deep-copied on the Qt tick under
  the same mutex (~3663–3667); capture errors marshal to the Qt thread via
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` (~3376–3383);
  `MediaCapture`'s cross-thread flags are `std::atomic`. All tuning state is
  Qt-thread-only. Do NOT introduce a worker thread before Wave 4's
  PipelineOrchestrator step — that is A1-step-5 territory.

1. S6b fence contract: new `include/openzoom/app/fence_protocol.md`-style
   comment block OR a small `FenceSequencer` struct in app.hpp owning
   sharedFenceCounter_/lastCudaSignalValue_/lastGraphicsSignalValue_/
   lastReadbackSignalValue_ with methods `BeginCudaFrame()`, `CudaFailed()`,
   `GraphicsSignaled()`. Rules to encode: values strictly increase; a failed
   ProcessFrame rolls the counter state back so no one ever waits on a value
   that will never be signaled; after 3 consecutive CUDA failures call
   ResetCudaFenceState() + presenter WaitForIdle() (full resync) and surface
   one status message, not per-frame spam.
2. S4 threading audit: enumerate every member touched by (a) the MF capture
   callback thread and (b) the Qt thread; verify each is either
   mutex-guarded, atomic, or single-thread-only. Known suspects: latestFrame_
   handoff (guarded), reconnect flags (atomic — verify), lastCameraError_,
   frame dimension members. Fix only VERIFIED races (snapshot-struct pattern
   under one mutex at tick start); write findings into
   improvement_ideas/01 as DONE/refuted entries. Do NOT introduce a worker
   thread in this wave — that is A1-step-5 territory.

Acceptance: contract in one place; kill-the-camera + fail-injection (force
ProcessFrame false) never wedges the present loop.

## Wave 3 — Performance remainder (P11, P12–P15, P4, P6) — DONE 2026-07-23

Batch-B result: P11-P14 were implemented in order. Capture/app handoff now
moves frame ownership, and `CudaInteropSurface` packs BGRA/NV12/YUY2 into a
two-slot `cudaMallocHost` ring whose upload-complete events guard reuse. P12
removed `gGaussianKernelSize`; P13 uses stream-ordered symbol copies without
`cudaDeviceSynchronize`; P14 reduces 16x16 luma-block partials in shared
memory.

Reference measurements (RTX 4090 Laptop GPU, CUDA 13.1, five-run means except
the rejected prototype): representative 1080p frame handoff + H2D stage
1.307 -> 0.841 ms; Gaussian symbol update 0.064 -> 0.025 ms; 320x180
projection 0.039 -> 0.037 ms, with 0.0 maximum absolute error against CPU
row/column sums. P12 has no steady-frame cost; its removed copy is included in
the P13 update measurement. P4's 3x running-sum box prototype was
benchmark-rejected and removed: 4.159 ms at radius 25 versus 0.355 ms for the
existing exact Gaussian (11.7x slower), so P4 is CLOSED-WONTFIX for this
implementation and the pixel-identical exact path remains. P6 was deferred:
the interop surface itself is recreated for a resolution change, so retaining
allocations across that ownership boundary is not trivial and would violate
the batch's "only if trivial" gate. P15 was skipped as optional/non-trivial.

Work items inside the wave run strictly in this order: P11 first (biggest
win), then the three small kernel-file cleanups P12/P13/P14 (P15 only if
trivial), then the benchmark-gated P4 blur rewrite, then P6 only if time
permits. Every item follows the same measurement protocol: read the Wave-1
numbers from the status tooltip BEFORE the change (let the app run ~10–15
seconds so the 60-frame average and the every-30th-frame GPU sample settle),
apply the change, rebuild, and read the numbers again under the same scene
and settings. Record every before/after pair in the batch report. No item in
this wave may change what is rendered, with the single exception of the
approved P4 blur approximation described below.

### 1. P11 — Pinned staging ring for frame uploads — DONE 2026-07-23

WHY. Every frame the app uploads the camera image host-to-device with
`cudaMemcpy2DAsync`, but the source memory is ordinary pageable memory
(a `std::vector`). CUDA cannot DMA from pageable memory directly, so the
driver secretly stages the data through an internal pinned bounce buffer and
the "async" copy becomes effectively synchronous with respect to the host and
cannot overlap kernel execution. Uploading from pinned (page-locked) memory
makes the copy truly asynchronous on `stream_`, letting the upload of frame
N+1 overlap the kernels of frame N. At 1080p this is worth several
milliseconds per frame — for a magnifier whose entire product is
input-to-photon latency, this is the single largest remaining win, which is
why it goes first and why every later item is measured against the improved
baseline.

HOW. The upload call sites in `CudaInteropSurface::ProcessFrame()`
(src/cuda/cuda_interop.cpp) are:

- BGRA path: one `cudaMemcpy2DAsync(deviceBufferA_, ...)` at ~1617–1621. Its
  host source is `cpuPipeline_.StageRaw()` — a `std::vector` handed in by
  `OpenZoomApp::ProcessFrameWithCuda()` (src/app/app.cpp ~3034–3062).
- Raw NV12 path: two copies (Y plane ~1641–1645, interleaved UV plane
  ~1648–1652) into `deviceRawPlane1_`/`deviceRawPlane2_`.
- Raw YUY2 path: one copy at ~1660–1664 into `deviceRawPlane1_`.
  Both raw paths receive their host pointers from
  `OpenZoomApp::TryProcessRawFrameWithCuda()` (src/app/app.cpp ~3070–3149),
  which points into `MediaFrame::data` — the Qt-tick deep copy of
  `latestFrame_` made in `RunFrameTick()` (~3663–3667).

The fix, exactly as decided: two pinned host buffers (`cudaMallocHost`),
alternating per frame, sized to the largest plane set of the raw frame; the
capture path memcpys the incoming sample into the current pinned slot, and
`ProcessFrame` uploads from pinned memory so `cudaMemcpy2DAsync` becomes
truly asynchronous and overlaps with kernels. This applies to the BGRA path
AND the raw NV12/YUY2 path. Concretely:

- Allocate the slots inside `CudaInteropSurface` next to the other
  Ensure/Release pairs (`EnsureRawInputBuffers()` ~829–860 is the model), or
  as an `EnsurePinnedStaging(bytes)` helper. There is existing precedent for
  pinned allocations in this file: `hostKeystoneLuma_` (`cudaMallocHost` at
  ~933–935, freed with `cudaFreeHost` in `ReleaseKeystone()` ~1044–1046) and
  `hostFocusStats_` (~1274–1275). Release paths must call
  `SynchronizeStream()` first, exactly like every other Release* method in
  the file (see the comment at ~402–404).
- The memcpy into the pinned slot replaces the pageable staging copy the
  capture path already performs today (the `latestFrame_ = frame` vector
  assignment in the `FrameCallback` at app.cpp ~3372–3375 and the tick-side
  deep copy at ~3663–3667), so steady-state copy count does not increase —
  the copy target simply becomes pinned memory. The slot index alternates
  every frame.
- Document, in a comment at the allocation site, exactly which
  synchronization value guards slot reuse (the plan requires this to be
  written down). The correct statement is: the shared-fence values sequenced
  by `FenceSequencer` order GPU work against GPU work only — they can NEVER
  protect a host-side memcpy into a slot, because host code does not wait on
  those fences. The guard that protects reuse is a per-slot `cudaEvent`
  recorded on `stream_` immediately after the last `cudaMemcpy2DAsync` that
  reads the slot; before the CPU writes into a slot again, it must observe
  that slot's event complete (`cudaEventQuery` poll, or
  `cudaEventSynchronize` as the safe fallback). With two slots the event is
  in practice always already signaled by the time the slot comes around
  again (a full frame has elapsed), so the check costs nothing — but it must
  exist for correctness.

PITFALLS — what a naive implementation gets wrong:

- Reusing a slot without fencing against the in-flight `cudaMemcpy2DAsync`.
  Writing new pixels into a slot while the DMA engine is still reading the
  previous frame out of it produces silent tearing/corruption in the
  uploaded image, and only under load (when the GPU falls behind), which
  makes it maddening to reproduce. The per-slot event described above is not
  optional.
- Thread ownership confusion. Today the MF capture callback thread writes
  `latestFrame_` under `cameraMutex_` and the Qt thread does everything CUDA.
  Whichever thread ends up performing the memcpy into the pinned slot, the
  ownership rules must be written down at the member declarations: who
  writes a slot, who reads it, and which mutex/event mediates. If the
  capture thread fills slots directly, the "current write slot" index and
  per-slot dimensions must be guarded by `cameraMutex_` (the mutex that
  already exists for exactly this handoff), the Qt thread must never read
  the slot being filled, and a camera frame arriving faster than the tick
  consumes must simply overwrite the same slot (dropped frame) rather than
  advance onto the slot the upload is reading.
- Forgetting the fallback paths. `TryProcessRawFrameWithCuda()` can return
  false at half a dozen points, after which the SAME frame is re-processed
  through the CPU path (`cpuPipeline_.ConvertFrameToBgra`, app.cpp
  ~3691–3697), and `PrepareOriginalFrame()` reads the frame bytes for
  recording/photos. Those consumers must keep working — do not destroy the
  metadata or the CPU-visible copy the moment a pinned slot exists.
- Resolution/format switches. Slot size depends on camera format and
  extent; on change, drain (`SynchronizeStream()`), free, and reallocate,
  mirroring `EnsureRawInputBuffers()`. Also keep the existing defensive
  stride validation in `ProcessFrame()` (~1495–1545) intact — pinned staging
  does not make driver-provided strides trustworthy.

ACCEPTANCE for P11:
- [ ] Build green (msvc-release) via the plan-13 recipe.
- [ ] Tooltip GPU ms and CPU ms/frame recorded before AND after at 1080p,
      same camera/scene/settings; expect several ms improvement on the CPU
      figure at 1080p (report the actual numbers, whatever they are).
- [ ] Both paths verified: a raw NV12 camera mode AND a forced BGRA/CPU-path
      frame (enable Debug View once to force the CPU composite, then back).
- [ ] No visual change (eyeball golden frames; the upload is byte-identical).
- [ ] Rapid camera switching (10+ switches) with blur + temporal smoothing
      on: no crash, no corruption — this exercises slot free/realloc under
      in-flight work.
- [ ] Comment at the slot declarations documents thread ownership and the
      per-slot event guard, as required above.

### 2. P12 — Delete the dead `gGaussianKernelSize` constant — DONE 2026-07-23

WHY. `src/cuda/cuda_kernels.cu:348` declares
`__constant__ int gGaussianKernelSize;` and `UploadGaussianKernel()`
(~2019–2023) uploads it with a THIRD synchronous `cudaMemcpyToSymbol` — but
no kernel anywhere reads it (both blur kernels read `gGaussianRadius`
instead, lines ~359 and ~394). Every constant-symbol copy is a
device-visible operation; deleting a dead one is free performance and less
code to misunderstand.

HOW. Grep the whole repo for `gGaussianKernelSize` to confirm there are
exactly two references (the declaration and the upload), delete both, build.

PITFALLS. None beyond verifying the grep — do not touch `gGaussianKernel`
or `gGaussianRadius`, which are live.

ACCEPTANCE:
- [ ] `grep -rn gGaussianKernelSize` over the repo returns nothing.
- [ ] Build green; blur still visually works at radius 3 and radius 25.

### 3. P13 — Make `UploadGaussianKernel()` asynchronous on the stream — DONE 2026-07-23

WHY. `UploadGaussianKernel()` (src/cuda/cuda_kernels.cu ~1983–2029) receives
a `cudaStream_t stream` parameter and then never uses it: it issues its
`cudaMemcpyToSymbol` calls on the default stream and finishes with a full
`cudaDeviceSynchronize()` (~2024). Thanks to the caching in
`CudaInteropSurface::EnsureGaussianKernel()` (src/cuda/cuda_interop.cpp
~1447–1469, which skips the upload when radius/sigma are unchanged) this
only fires on slider moves — but a slider drag emits MANY changes and each
one stalls the entire device, so dragging the blur radius slider visibly
hitches the live video. That hitch is exactly the kind of jank a low-vision
user tracking text cannot tolerate.

HOW. Replace the remaining `cudaMemcpyToSymbol` calls (after P12 removes one,
two remain: the weight array at ~2009 and `gGaussianRadius` at ~2014) with
`cudaMemcpyToSymbolAsync(..., cudaMemcpyHostToDevice, stream)` and DELETE the
`cudaDeviceSynchronize()`. The subsequent blur kernel launches are enqueued
on the same stream by `LaunchGaussianBlurLinear()` (~623–640), so stream FIFO
order already guarantees the copies land before any kernel reads the symbols
— no synchronization is needed.

PITFALLS.
- Source lifetime. The weights live in a function-local
  `std::vector<float> kernel` (~1992). An async copy from PAGEABLE memory
  returns only after the driver has staged the data, so the local vector is
  in practice safe — but that behavior is a driver detail, not a contract you
  want to lean on silently. Either add a comment citing the CUDA docs
  (pageable-source async copies are synchronous with respect to the host), or
  make the buffer persistent (e.g. a `static std::array<float, 2*50+1>` —
  `kMaxBlurRadius` is 50, line ~345) so the question cannot arise. Do not
  "fix" it by re-adding a sync.
- Error handling: keep the `return false` on each failed copy; the caller
  (`EnsureGaussianKernel`) already treats false as "kernel not uploaded" and
  aborts the blur for that frame (cuda_interop.cpp ~2126–2129).

ACCEPTANCE:
- [ ] Build green; no `cudaDeviceSynchronize` remains in the function.
- [ ] Dragging the blur radius slider across its range while the camera runs
      produces no visible hitching (compare against pre-change behavior).
- [ ] Blur output unchanged at radius 3 / sigma 1 and radius 25 / sigma 5.

### 4. P14 — Shared-memory reduction for the stabilization projections — DONE 2026-07-23

WHY. `StabilizationProjectionKernel` (src/cuda/cuda_kernels.cu ~773–785)
computes column/row luma profiles for video stabilization by issuing TWO
global `atomicAdd`s per pixel of the downsampled luma image
(`atomicAdd(&colProj[x], value)` and `atomicAdd(&rowProj[y], value)`). All 16
threads of a block column contend on the same `colProj[x]` word, and all 16
threads of a block row contend on the same `rowProj[y]` word; profiling shows
this contention is not negligible despite the in-source comment (line ~772)
claiming otherwise. The same file already demonstrates the correct pattern:
the auto-contrast histogram kernel accumulates into per-block `__shared__`
bins and merges once per block (~1199–1205).

HOW. Give the 16×16 block two small shared arrays (`float colPart[16]`,
`float rowPart[16]`), zero them, have each thread `atomicAdd` into SHARED
memory (fast), `__syncthreads()`, then let 16 threads perform one global
`atomicAdd` each per array — cutting global atomics by roughly the block
area. The launch wrapper `LaunchStabilizationProjections()` (~1279–1297)
does not change: it still zeroes the output arrays with `cudaMemsetAsync`
before the launch, and the estimate kernel that consumes the profiles
(`StabilizationEstimateKernel`, ~805–893) is untouched.

PITFALLS.
- Floating-point atomics are order-nondeterministic, so exact bit patterns in
  the profiles can differ run-to-run — that is ALREADY true of the current
  kernel and is harmless (the consumer is an argmin over SAD values, robust
  to last-ulp noise). Do not chase bit-identical profiles; verify the
  stabilization BEHAVIOR is unchanged instead.
- The downsampled image is small (profiles are at most a few hundred floats;
  see `kStabSearchRadius`/related constants at ~732–736), so the absolute
  gain is modest — this item is included because it is cheap and removes a
  known bad pattern, not because it is a headline win. Measure honestly and
  report whatever the numbers say.

ACCEPTANCE:
- [ ] Build green; stabilization visually identical (hold the camera, jitter
      it, confirm the smoothing behaves as before at strength 0.85).
- [ ] Before/after GPU ms with stabilization enabled recorded in the report
      (expect a small or negligible delta; report it either way).

### 5. P15 — texture objects for NIS/FSR — DEFERRED 2026-07-23

The NIS and FSR EASU/RCAS kernels sample multi-tap neighborhoods with
hand-rolled bilinear from raw global pointers; `cudaTextureObject_t` with
hardware filtering would be cleaner and faster. This is explicitly optional
in this wave: attempt it only if, after P11–P14, it looks trivial; otherwise
leave it for a future pass. Do not let it delay P4.

### 6. P4 — Box-chain blur — CLOSED-WONTFIX 2026-07-23

Gate result: the 3x separable running-sum prototype measured 4.159 ms at
1920x1080, radius 25, sigma 8.333, versus 0.355 ms for the existing exact
Gaussian on the RTX 4090 Laptop GPU (11.7x slower). It failed the first gate,
so the prototype was removed before merge and the exact pixel path remains.
Radius-50 and golden-image evaluation were not pursued after the mandatory
radius-25 speed gate had already failed.

WHY. The current separable Gaussian
(`GaussianBlurHorizontalKernel`/`GaussianBlurVerticalKernel`,
src/cuda/cuda_kernels.cu ~350–418) reads `2*radius+1` global-memory samples
per pixel per pass with no staging; radius can reach 50 (`kMaxBlurRadius`,
~345; the UI exposes radii up to 50 via `kSupportedBlurRadii` in
include/openzoom/app/constants.hpp ~19). At large radii this is the most
expensive kernel in the pipeline. An iterated box blur computes each output
pixel in O(1) time regardless of radius (running-sum/sliding-window), and
three iterations converge closely to a Gaussian. The trade is that it is an
APPROXIMATION — hence the gate below.

HOW.
1. Implement the 3× iterated box blur as a NEW kernel (or kernel pair)
   behind the SAME `LaunchGaussianBlurLinear` signature
   (include/openzoom/cuda/cuda_kernels.hpp ~29–33: dst, scratch, src,
   pitches, width, height, stream). Keep the OLD Gaussian kernels compiled
   and callable.
2. Select by radius inside the launcher: radius > 8 → box chain; radius ≤ 8
   → existing Gaussian. The call site in `ProcessFrame()`
   (src/cuda/cuda_interop.cpp ~2125–2137) and the `EnsureGaussianKernel`
   caching do not change (the box path can ignore the uploaded taps but the
   sigma→box-width derivation must be principled — use the standard
   three-box-widths-from-sigma construction so the result approximates the
   SAME sigma the user chose).
3. Edge semantics must match the old kernels: clamp-to-edge (the old code
   clamps sample coordinates at ~368–369 and ~402–403). Blur all four
   channels including alpha, as the old kernels do.
4. Parallelization: a running-sum pass is inherently sequential along its
   axis. One thread per row (horizontal pass) yields only ~1080 threads at
   1080p — that can still win because each thread does O(width) work total
   instead of O(width*radius), but coalescing on the vertical/column pass is
   poor if done naively (stride-pitch accesses). Acceptable structures:
   thread-per-column with pitched reads (measure it), or a transpose-based
   scheme using `deviceScratch_`. Whatever you pick, MEASURE — that is the
   whole point of the gate.

THE GATE (owner decision — do not rationalize around it): using the Wave-1
timing numbers, the box chain must be measurably FASTER than the current
kernel at radius 25 and above, AND a golden-frame visual diff of the
approximation must be acceptable (it approximates a Gaussian; small smooth
differences are expected, banding or directional artifacts are not). If
either half fails, REVERT to the old kernel (delete the box path) and mark
P4 CLOSED-WONTFIX in this plan with the measured numbers so nobody re-opens
it without new information.

PITFALLS.
- Do not benchmark at radius 3 and extrapolate; the crossover matters and
  the gate is defined at radius 25+.
- Do not delete or bypass the old kernels — the ≤8 radius path keeps using
  them, and the revert path requires them.
- Box widths: naive "three boxes of width radius" does NOT approximate the
  requested sigma; use the standard w/wl/wu box-size derivation from sigma.
- The GPU timing sample is every 30th frame — hold the settings steady for
  several seconds per measurement, and take more than one reading.

ACCEPTANCE:
- [ ] Timing table in the report: old vs new GPU ms at radii 8, 25, 50
      (1080p, same scene).
- [ ] Golden-frame diff at radius 25: side-by-side screenshots (old/new) in
      the report; no banding/directional artifacts.
- [ ] Radius ≤ 8 path verified unchanged (still the original Gaussian).
- [ ] If the gate failed: old kernel restored, P4 marked CLOSED-WONTFIX here
      with the numbers.

### 7. P6 — Device-buffer reallocation churn — DEFERRED 2026-07-23

The buffers belong to a D3D12/CUDA interop surface that is itself recreated
when the resolution changes. Retaining allocations across that ownership
boundary requires a pool or broader lifetime redesign and did not meet Batch
B's "only if trivial" condition.

`EnsureDeviceBuffers()` (src/cuda/cuda_interop.cpp ~588–625) frees and
re-allocates buffers A/B/scratch whenever dimensions change (mode/camera
switch) — never per frame, so this is a hitch on switches, not a
throughput cost. If, and only if, the earlier items went smoothly: allocate
the three buffers once per surface lifetime at the camera-max resolution
(or use `cudaMallocAsync` pools). Otherwise explicitly defer — write one
line in the report saying P6 was deferred and why.

Wave acceptance: before/after ms in the report for EVERY item attempted; no
visual diffs anywhere except the P4-approved blur approximation.

## Wave 4 — Architecture decomposition (A2 → A3+A1.1 → A1.3 → A1.4 → A1.2/A4/A5) — CODE COMPLETE 2026-07-23

WHY this wave exists. `src/app/app.cpp` is ~4,169 lines and `OpenZoomApp`
owns everything: ~90 raw widget pointers (include/openzoom/app/app.hpp
~302–396), dozens of `QSignalBlocker` uses, three independent "suspend sync"
flags, the frame pipeline, CUDA/fence state, recording, settings, and
assistive features. Every future change pays a tax on this file. The
decomposition is deliberately LAST (after the perf work has stopped moving
the code) and deliberately incremental, because a big-bang refactor of a
file this size on a hardware-dependent app with almost no tests is how
regressions are born. The discipline matters more than the code.

RULES (non-negotiable, restated from the original plan): ONE extraction per
step; build green + manual smoke test after EACH step; NO behavior changes
mixed with moves (if you spot a bug mid-move, note it in the report, do not
fix it in the same step); app.cpp's line count must shrink monotonically and
be recorded in the report after every step. Precondition (strongly
recommended, and mandatory before step 3 per the gate below): land plan-06
B2's settings_store round-trip tests BEFORE touching SettingsController.

A structural note that affects every step: `MainWindow` and
`InteractionController` are `friend` classes of `OpenZoomApp`
(include/openzoom/app/app.hpp ~146–147) and reach into its privates. Each
extraction that moves members those friends touch must either route the
access through the new collaborator's public API or keep a thin delegating
accessor on `OpenZoomApp` — grep `src/app/interaction_controller.cpp` and
`src/ui/main_window.cpp` for the moved members before each step.

### Step 1 — A2: `SuspendGuard` RAII (mechanical warm-up)

WHY. Three boolean flags suppress feedback loops between the UI and the
config state: `configTrackingSuspended_` (app.hpp ~515 — blocks
`SyncCurrentConfigToPersistence()`, checked at app.cpp ~1272),
`presetSelectionSyncSuspended_` (app.hpp ~514 — blocks
`OnPresetSelectionChanged`, checked at ~1575), and `suspendControlSync_`
(app.hpp ~422 — blocks the zoom-center slider echo, checked at ~2127 and
~2135). Each is manually set, a long mutation block runs, then it is
manually cleared — `ApplyAdvancedConfig()` holds
`configTrackingSuspended_` across ~230 lines (~1296 set, ~1523 clear). An
early return or exception anywhere in between leaves the flag stuck and the
UI silently stops syncing forever. A 10-line RAII guard removes that entire
failure class and is the zero-risk warm-up for the bigger moves.

HOW. Add a tiny struct (in app.hpp or its own header):
set-in-ctor, clear-in-dtor, copy/move deleted, holding a `bool&`. Replace
every SAME-SCOPE set/clear pair: `PopulatePresetList()` (~1155/~1172),
`RefreshPresetSelection()` (~1218/~1233), `ApplyAdvancedConfig()`
(~1296/~1523), `SetZoomCenter()` (~2538/~2547). 

PITFALLS. One site is NOT a same-scope pair: the constructor sets
`configTrackingSuspended_ = true` at ~335 and the flag is only cleared at
the end of the first `ApplyAdvancedConfig()` run during
`ApplyPersistentSettings()` (~1523). That cross-function latch cannot become
a scoped guard without changing behavior — leave it as a plain assignment
with a comment. Converting it anyway (and thereby clearing the flag at
constructor exit) would re-enable config tracking during startup and corrupt
the persisted config with half-initialized UI state.

ACCEPTANCE:
- [ ] Build green; no remaining manual `= true; ... = false;` pair for the
      three flags except the documented constructor latch.
- [ ] Smoke: switch presets, drag zoom-center sliders, apply a preset —
      no double-updates, persistence still works after each.
- [ ] app.cpp line count recorded (expect a small net reduction).

### Step 2 — A3 + A1.1: `RecordingManager` (+ real state machine)

WHY. Recording state is a bare `bool recording_` plus eight loose members
(app.hpp ~472–484). Error paths each hand-roll "set flag false, block
signals, un-check button, stop writer" with inconsistent messaging
(`StopRecordingUi()` ~4010–4023 vs the record-button slot ~529–553 vs the
three failure branches inside `MaybeRecordFrame()` ~4102–4165). There is no
"starting" state, so a frame arriving mid-initialization hits ambiguous
logic. Recording is self-contained (writers, paths, size checks, 12-hour
cap) which makes it the ideal first real extraction.

HOW. Create `src/app/recording_manager.cpp` +
`include/openzoom/app/recording_manager.hpp`. Move: `videoRecorder_`,
`originalVideoRecorder_`, `pendingOriginalReadbacks_`, the recording_*
members; `StopRecordingUi()`; `StartPairedRecorders()` (~4025–4097,
including the AV1→H264 codec fallback loop and output paths under
`EnsureOutputSubdir("vid")`); `MaybeRecordFrame()` (~4102–4165, including
the disk-full branch at ~4145–4149 and the 12-hour cap at ~4160–4164).
Introduce `enum class RecordingState { Idle, Starting, Recording, Stopping,
Error }` with ONE `SetRecordingState()` transition function that owns all
UI updates (the record button text/checked state — today toggled with a
`QSignalBlocker` at ~4019) and all writer start/stop calls; every error
path routes through it. `OpenZoomApp` keeps only the button slot delegating
in, and passes a status-message callback (or the manager exposes a Qt
signal) so `ShowStatusMessage()` (~2863) keeps working. The async-readback
consumption stays where the data arrives: `HandleGpuFramePresented()`
(~3268–3320) still drains the presenter ring and matches request ids to
pending originals, but hands matched pairs to the manager instead of
calling `MaybeRecordFrame` free-floating.

PITFALLS. The processed/original pairing by readback `requestId`
(~3283–3301) is subtle — originals are stashed in
`pendingOriginalReadbacks_` when the readback is REQUESTED (~3310–3313) and
consumed when it COMPLETES frames later; stale entries are pruned by id
ordering. Move that map with the manager and keep the prune logic intact,
or recordings will pair frame N video with frame N-2 originals. Also keep
`UpdateProcessingStatusLabel()`'s `[REC]` text working — it reads
`recording_`/`recordingCodecName_` (~2771–2778); give the manager cheap
state accessors.

ACCEPTANCE:
- [ ] Build green; record 30+ seconds, both `VID_*_processed.mp4` and
      `VID_*_original.mp4` written and openable, in sync.
- [ ] Stop/start recording rapidly; toggle while camera reconnects; button
      state never desyncs from actual recording state.
- [ ] All five states reachable in a debugger walkthrough or via logging.
- [ ] app.cpp line count recorded.

### Step 3 — GATE, then A1.3: `SettingsController`

THE GATE (mandatory, from plan 06 B2): BEFORE touching settings code,
create minimal round-trip tests for `settings_store`. The cmake option
`OPENZOOM_ENABLE_TESTS` already exists and currently warns that no
`tests/CMakeLists.txt` exists (cmake/CMakeLists.txt ~123–128). Wire it:
`tests/CMakeLists.txt` with Catch2 (FetchContent) or QtTest, registered
with ctest. Minimum test set against `src/app/settings_store.cpp`:
Save→Load round-trip preserves every `AdvancedConfig` field (ConfigToJson
~262–318 / ConfigFromJson ~320–384); legacy v1 flat format migrates
(`LegacyConfigFromRoot` ~486–522, version sniffing at Load ~569–577);
corrupt JSON returns `std::nullopt` (~559–563); out-of-range fields clamp
(the clamps are inline in ConfigFromJson); `AreConfigsEquivalent`
(~778–834) tolerances behave. These tests MUST run CPU-only: build them
with the `msvc-cpu` preset (CUDA OFF) so any machine can run them. Wave 4
must not proceed past this point without these tests existing and passing —
they are the only safety net under the two riskiest steps.

WHY the controller. Persistence logic is interleaved with widget code:
load in the constructor (~322–334), `SavePersistentSettings()` (~3590–3604,
also hooked to `aboutToQuit` at ~336), `ApplyPersistentSettings()`
(~3559–3588), capture from live state (`CaptureCurrentAdvancedConfig()`
~1089–1147), preset resolution/promotion (`SyncCurrentConfigToPersistence()`
~1270–1292, `PromoteCurrentConfigToPreset()` ~1528,
`PopulatePresetList()`/`RefreshPresetSelection()`/`UpdatePresetDescription()`
~1149–1268). Decoupling the data side makes it testable and shrinks app.cpp
substantially.

HOW. `SettingsController` owns `persistentSettings_`, `settingsPath_`,
load/save, capture-from-plain-state, and preset resolution/promotion. The
UI-blocker choreography — all the widget writes inside
`ApplyAdvancedConfig()` (~1294–1526) — STAYS in app.cpp in this step; it
moves in step 4. This split is deliberate: moving data logic and widget
choreography in one step is exactly the compound change the rules forbid.

PITFALLS. `SyncCurrentConfigToPersistence()` consults
`configTrackingSuspended_` (~1272) — the guard flag and the data logic now
live on opposite sides of the boundary; pass the suspension state in (or
keep the check at the call sites) rather than duplicating the flag.

ACCEPTANCE:
- [ ] Settings tests exist, run via ctest on the msvc-cpu build, and pass.
- [ ] Build green on BOTH msvc-release and msvc-cpu.
- [ ] Smoke: change settings, quit, relaunch — everything restores;
      promote a custom preset and re-select it.
- [ ] app.cpp line count recorded.

### Step 4 — A1.4: `UIStateManager` (+ A5 non-null policy)

WHY. ~90 widget pointers and every `QSignalBlocker` in the app live in
`OpenZoomApp`. The apply/read choreography is the most repetitive code in
the file, and the null-checking is inconsistent (~71 scattered checks,
other paths dereference freely — plan 02 A5). Centralizing widget access
behind `ApplyConfigToUI(const AdvancedConfig&)` / `ReadConfigFromUI()`
ends both problems: `OpenZoomApp` stops touching widgets, and the manager
enforces ONE policy — widget pointers are non-null references from
construction to destruction (assert once at construction; DROP the
scattered checks).

HOW. Move the widget pointer members (app.hpp ~302–396), the harvesting
block (app.cpp ~337–425), and the widget-write half of
`ApplyAdvancedConfig()` into the manager. The signal connections (~501–711)
target `OpenZoomApp` slots; they can stay app-side initially (connected via
accessors) or move — but whichever you choose, do it uniformly in this one
step.

PITFALLS — the double-fire trap (this is THE thing naive implementations
get wrong). `ApplyAdvancedConfig()` uses a strict per-control pattern:
block signals with `QSignalBlocker`, write the widget, then call the slot
MANUALLY exactly once (e.g. `bwSlider_` write at ~1306–1311 followed by
`OnBlackWhiteToggled`/`OnBlackWhiteThresholdChanged` at ~1312–1313; blur at
~1331–1350; display colors at ~1449–1467 with the manual
`OnDisplayColorModeChanged(displayColorMode_)` call at ~1467). The blocker
and the manual call are two halves of one contract. If the widget write
moves into `UIStateManager` but the manual slot invocation stays behind (or
vice versa), every control either fires TWICE per apply (blocker lost) or
ZERO times (manual call lost) — both corrupt state in ways a quick smoke
test can miss. Move each blocked-write + manual-call pair WHOLESALE and
keep the invocation order identical. Second trap: A5 says drop the null
checks ONLY after asserting non-null at construction — if some widget
accessor can legitimately return null in a UI mode (grep
`src/ui/main_window.cpp` accessors ~2101–2214, which today never return
null after construction), the assert will tell you at startup instead of a
crash telling you mid-session.

ACCEPTANCE:
- [ ] Build green; apply every built-in preset in sequence and verify each
      control reflects it; toggle each Simple/Advanced control once.
- [ ] No `if (widget_)` null checks remain outside the manager; the
      construction-time assert exists.
- [ ] Quit/relaunch round-trip still restores everything (this catches
      silent double-fires that dirty the config).
- [ ] app.cpp line count recorded.

### Step 5 — A1.2 `AssistiveFeatureManager`; A1.5 `PipelineOrchestrator`; A4 last

Three extractions, still ONE PER STEP with build+smoke between:

- A1.2 `AssistiveFeatureManager`: owns `assistiveRuntime_` and overlay
  wiring (ctor ~311–320), `UpdateAssistiveRuntimeState()` (~1591),
  `MaybeRequestAssistiveAnalysis()` (~1602), `AssistiveAnalysisDue()`
  (~3322–3326), `BuildAssistiveRuntimeConfig()` /
  `ApplyAssistiveSettingsToRuntime()` (~1635/~1662), and the analysis
  interval timer.
- A1.5 `PipelineOrchestrator`: owns frame ticking (`OnFrameTick` ~3609,
  `RunFrameTick` ~3647–3721, `RecordFrameTickSample` ~3619), CPU-vs-CUDA
  path selection (`ProcessFrameWithCuda` ~3034, `TryProcessRawFrameWithCuda`
  ~3070, `RunCudaPipeline` ~3154–3260), `EnsureCudaSurface()` (~2936–3032),
  the Wave-2 `FenceSequencer` plus failure recovery
  (`ResetCudaFenceState`/`HandleCudaProcessingFailure` ~2911–2934), and the
  camera reconnect state machine (`BeginCameraReconnect` ~3487,
  `DriveCameraReconnect` ~3502–3557, backoff constants included). This is
  the largest and riskiest move; it is last-but-one on purpose. If a
  processing worker thread is ever introduced (plan 01 S4's long-term
  note), it happens INSIDE this class, later — not during this extraction.
- A4 constructor→`Initialize()` split LAST, once every collaborator owns
  its own setup: keep the `OpenZoomApp` constructor minimal and throw-safe,
  move risky work into `bool Initialize()` called from `main()`
  (src/app/main.cpp ~11–17 currently catches exceptions from a
  partially-constructed app), with idempotent cleanup on failure. Known
  ordering constraints to preserve and document:
  `ResolveCudaBufferFormatFromOptions()` before presenter creation
  (~299–302), settings-path resolution after `QApplication` exists
  (~322).

ACCEPTANCE (per extraction, same as always): build green, feature smoke
unchanged (assistive overlay, camera reconnect by unplugging, CUDA path
active, photo + recording), app.cpp line count recorded.

End-state target for the whole wave: `app.cpp` below 800 lines of pure
composition — construction, wiring, and delegation only.

### Wave 4 implementation record

- `SuspendGuard`, `RecordingManager`, `SettingsController`,
  `UIStateManager`, `AssistiveFeatureManager`, and `PipelineOrchestrator` now
  have focused public headers and source files. `OpenZoomApp` construction is
  minimal and fallible startup moved to `Initialize()`.
- The settings gate is active in the CPU preset. It covers round-trip,
  legacy migration, corrupt input, clamping, and comparison tolerances.
- The former application body is split by responsibility across
  `app_bootstrap.cpp`, `app_settings.cpp`, `app_controls.cpp`,
  `app_interaction.cpp`, `app_assistant.cpp`, and
  `app_pipeline_runtime.cpp`. The composition-only `app.cpp` is 5 lines,
  below the 800-line target.
- UI implementation was also separated into `RenderWidget`,
  `ColorSchemePicker`, `AssistiveOverlay`, `JoystickOverlay`, and
  `ResponsiveSliderRow` translation units. This was an additional cleanup
  after the required manager extractions.
- The original approximately 4,169-line baseline and final 5-line result are
  known. Intermediate per-extraction counts were not retained because the
  extractions were carried in one continuous uncommitted worktree; no
  artificial intermediate numbers are recorded here.
- Deviation: `PipelineOrchestrator` owns both clocks, timing, fence sequencing,
  CUDA failure accounting, and reconnect state, while the low-level CPU/CUDA
  path functions remain private `OpenZoomApp` methods isolated in
  `app_pipeline_runtime.cpp`. Moving their large state graph into a second
  owning object during the aspect/presentation change would have mixed a risky
  ownership rewrite with behavior changes.
- A controlled 45-second live-camera CUDA run remained responsive and exited
  normally through the window close path with code 0. Camera reconnect, paired
  recording, and the full presentation/multi-monitor matrix still require
  targeted interactive checks.

## What is explicitly OUT of scope here

P7 CPU-pipeline copies (low value now that NV12/YUY2 convert on the GPU and
the CPU path only serves debug view / fallback), S2 (investigated and
REFUTED — see improvement_ideas/01 and verified-non-issues.md; do not
"re-fix" it), and new features of ANY kind. Tests/CI (plan 06) remain a
separate effort EXCEPT for the hard gate above: Wave 4 step 3+ must not
proceed without B2's settings tests existing. If you find yourself adding a
feature, changing visual output (outside the gated P4 approximation), or
"improving" refuted items, stop and re-read the plan.
