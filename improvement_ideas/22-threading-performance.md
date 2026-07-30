# Plan 22 — Threading, Latency, and Throughput

Status: **PARTIALLY IMPLEMENTED (2026-07-29); read the absorption record before
the historical body.** Effort of the remainder:
medium-large; still driven by telemetry rather than intuition. Verdicts and
evidence: plan 19.

> **2026-07-29 absorption note.** Since this plan was written:
> - **Phase 2's worst offender is gone**: encoder submission and
>   finalization moved to a bounded 12-frame recording worker with checked
>   async finalize (plan 20 P1/P3). The core-problem paragraph below
>   ("two synchronous encoder submissions") describes the pre-fix state.
> - **Phase 3's headline is done**: zero-copy capture shipped (plans 28/30
>   — MF DXGI samples → GPU conversion → CUDA external memory); the
>   decode-to-RAM, per-frame vector, pinned staging, and PCIe upload are
>   out of steady-state preview.
> - **Phase 3's recording ceiling is landed**: processed and rotation-correct
>   unenhanced streams are cloned into pooled shareable D3D12 canvases and
>   submitted as DXGI samples. Media Foundation sample release returns each
>   lease to a 384 MiB/48-slot pool; exhaustion is counted and never blocks
>   preview. CPU BGRA input remains the compatibility fallback.
> - **Phase 1's user-facing measuring stick is landed**: Advanced Diagnostics
>   reports nearest-rank p50/p95/p99 over independent 240-sample windows for
>   UI-thread camera processing and capture→present latency. The warning
>   budget follows the negotiated camera period. Plan 28's opt-in counters
>   still supply capture-thread CPU/bytes. Detailed CUDA, D3D12 queue,
>   readback, and encoder percentiles remain.
> - **Phase 4's highest-value cleanup is landed**: lecture-note sections append
>   without rereading and rewriting the growing HTML document; closing tags
>   are finalized when the session closes.
> - **Phase 2's image preparation is landed**: paired-photo JPEG writes and
>   annotated PNG rendering/writes use a bounded two-thread application pool.
>   OCR PNG export plus VLM/Codex resize, JPEG, base64, JSON, and temporary-file
>   work use a separate bounded two-thread assistive pool with generation-based
>   cancellation. The Qt thread retains only the unavoidable deep copy needed
>   to transfer ownership safely.
> - **Phase 2's blocking-call audit is complete**: camera retry timers, worker
>   image work, bounded child-process shutdown, nonblocking readback/upload
>   rings, and the redundant post-stream device drain are landed. The waits
>   still present are ownership boundaries rather than cleanup candidates:
>   teardown/resource replacement drains, explicit on-demand CPU readback, the
>   no-semaphore fallback, and the D3D11-video-to-CUDA handoff until it gains a
>   shared cross-API fence. Do not delete these to improve a benchmark.
> - **The recording-time preview-slot loss is now closed (2026-07-30)**:
>   Media Foundation frame arrival wakes the Qt pipeline directly instead of
>   relying on a same-rate polling timer. Preview stays keep-latest, while an
>   active recording has a bounded six-frame camera burst before the existing
>   encoder queue. A 15-second 720p30 direct-GPU run retained all 450 pairs.
> - **Genuinely remaining**: lower-value Phase 4 startup/descriptor cleanups.
>   Any further work must use the shipped percentiles as its acceptance
>   instrument.

## The core problem

One Qt UI-thread timer does everything: consume the newest capture frame, CPU
format conversion, CUDA submission and synchronization, D3D12 present, readback
drain, CPU row copies, original-frame preparation, **two synchronous encoder
submissions**, OCR/VLM image preparation, and JPEG/PNG compression with file
writes.

Evidence: `pipeline_orchestrator.cpp:13-20, 297-314`;
`app_pipeline_runtime.cpp:654-695`; `recording_manager.cpp:259-263`;
`media_writer.cpp:217-243`; `app_pipeline_runtime.cpp:1247-1328`.

Predicted symptoms, all of which the owner can check against lived experience:
controls stop responding during recording, resize stutters, preview frames
vanish while recording, Stop appears frozen, OCR/Explain causes a visible
hitch, and 4K fails before 1080p does.

## Phase 1 — Measure first (small, do this before any rearchitecture)

The current warning threshold is ~40 ms
(`pipeline_orchestrator.cpp:329-342`), which only fires below ~25 FPS. Real
budgets are 16.67 ms at 60 Hz, 11.11 at 90, 8.33 at 120.

**Implemented subset (2026-07-29):** refresh-aware camera thresholds and a
diagnostics panel report
**percentiles, not averages**: capture→present latency (p50/p95/p99),
and UI-thread camera tick time. Still add CUDA time, D3D12 queue wait, readback latency, encoder
queue depth, encoded-frame latency, and the four drop counters from plan 20.
This panel is also the acceptance instrument for every later phase, and it
gives the owner something concrete to report instead of "it feels slow".

## Phase 2 — Get heavy work off the UI thread

Target execution domains:

1. UI + presentation (Qt thread) — submits commands, consumes status events.
2. Capture ingestion (exists).
3. GPU processing/readback coordination.
4. Recording + file I/O worker.

Move first, in this order: encoder submission and finalization; photo JPEG
encode and write; OCR PNG encode and write; VLM copy/resize/JPEG/base64/JSON
serialization (`assistive_runtime.cpp:664-716, 739-800, 855-875`). The UI
thread should never compress an image or write a video frame.

**Implemented (2026-07-29):** all image encode/write and assistive frame
preparation items above now run in bounded worker pools. Saturation is reported
as a visible busy condition instead of growing an unbounded queue. Completion
returns through queued Qt callbacks; generation tokens discard cancelled or
superseded assistive work. The UI thread still performs a deep `QImage::copy()`
before dispatch because captured frame storage is reused by the live pipeline.

Also replace the blocking calls that can stall the UI: camera-start retry
sleeps of 150/300/600 ms (`media_capture.cpp:298-312`), unbounded capture-thread
join (`media_capture.cpp:416-426`), `WaitForGpu()` in the degraded interop path
(`presenter.cpp:422-428`), `cudaEventSynchronize` on staging-slot reuse
(`cuda_interop.cpp:2230-2244`), `cudaStreamSynchronize` without an external
semaphore (`cuda_interop.cpp:2868-2876`), and the Codex/Tesseract shutdown
waits. Prefer polling, completion queues, and bounded async shutdown with
progress reported to the UI.

## Phase 3 — Reduce copies (the scaling ceiling)

The external review's bandwidth arithmetic is correct but computed for 4K60
dual-stream, which is not today's operating point (the app negotiates 1280x720
with the owner's phone). Treat this as the ceiling that decides whether 4K
recording is ever viable, not as a present emergency.

Current worst-case path per frame: MF sample → `std::vector` → original-frame
conversion → another vector → pinned staging → host-to-device → GPU work →
device-to-readback → row copies → sink-writer buffer copy → encoder read.

Direction: reusable buffer pools instead of per-frame allocation; pass frame
handles and metadata rather than vectors; do conversion and rotation on the
GPU; render preview and recording from the same processed GPU result; feed a
hardware encoder from GPU-backed surfaces where available; read back only when
a CPU consumer genuinely needs pixels.

### Implemented project: GPU-fed recording

**Landed 2026-07-29.** The current CUDA/D3D12 path is:

```text
processed scene -> fixed recording canvas clone --\
                                                  +-> pooled shared D3D12 BGRA
pre-effect rotation-correct original clone -------/   -> GPU fence
                                                       -> D3D11 DXGI sample
                                                       -> hardware encoder MFT
```

The pool grows lazily, is bounded to 384 MiB and 48 slots, and reuses a slot
only when Media Foundation releases the submitted sample. It therefore obeys
both independent ownership rules: the shared fence orders GPU producer and
consumer work, while the sample-held lifetime lease proves the encoder no
longer retains the allocation. Exhaustion records `recordingPoolBusy` and
drops the pair rather than waiting on the UI thread. The CPU BGRA path remains
the permanent compatibility fallback.

“Original” now has an explicit definition: pre-effect and pre-zoom, but
post-format-conversion and post-user-rotation. It matches what the user saw
from that camera orientation without any OpenZoom enhancement.

Do not confuse hardware **encoding** with hardware **feeding**. The writer
already requested Media Foundation hardware transforms, so supported systems
normally encoded AV1/H.264 through the GPU encoder MFT (NVENC on the owner's
RTX system). Before this landing, pixels still reached that encoder through
this system-memory detour:

```text
processed recording:
  GPU fixed recording canvas
    -> GPU-to-CPU readback
    -> bounded RecordingManager worker queue
    -> VideoRecorder::AddFrame(uint8_t* BGRA)
    -> Media Foundation sample
    -> hardware encoder MFT uploads to GPU
    -> fragmented MP4

original recording:
  camera DXGI sample
    -> recording-only CPU readback
    -> the same worker/writer path
    -> hardware encoder MFT
```

Preview already remained GPU-resident on the top capture rung. The plan-20
queue kept these former recording copies away from the UI thread and preserved
the timestamp/VFR/finalization/drop contract while the direct path was built.

The Phase-3 end state is now the primary path: a second recorder input accepts
GPU-backed Media Foundation/DXGI samples. A compatible D3D device manager is
set on the sink writer and the completed recording-canvas texture is wrapped
in an `IMFDXGIBuffer`, allowing it to flow directly into the encoder MFT. The
original stream uses the persistent pre-effect GPU surface rather than a CPU
readback.

Non-negotiable constraints:

- retain the existing CPU BGRA path as the permanent compatibility fallback;
- preserve exact capture timestamps, fractional rate, fragmented MP4,
  `_partN` segmentation, checked finalization, audio mux, and every drop
  counter from plan 20;
- match adapters by LUID and use the proven shared-fence/external-memory
  ownership rules from plans 16, 28, and 30;
- select the path per stream because the processed canvas and original camera
  format can have different encoder compatibility;
- prove the GPU-fed route removes readback/upload rather than merely moving a
  hidden copy into another API layer.

**Design review record for the clone-per-queued-frame implementation
(2026-07-29):** the architecture — persistent pre-effect surface + per-frame
GPU clone into leased recording textures — is endorsed; its three
premises (MF recycles the sample pool, a 12-deep queue cannot retain recycled
allocations, clone-after-conversion is the only stable frame identity) are
all correct. The four closing requirements were resolved as follows:

1. **Pool, never per-frame allocation — done.** "Unique texture per queued
   frame" now means a lazily grown *recycled pool*, not a
   `CreateTexture2D`/`CreateCommittedResource` replacement every frame.
   Pool exhaustion is an accounted drop cause in the stop summary, never a
   stall and never an unbounded grow.
2. **Reuse gates on MF sample release, not on GPU fences — done.** A shared fence
   orders GPU work; it says nothing about when the encoder MFT stops
   *holding* the sample. Recycling a texture the writer still references
   produces rare corrupted encoded frames under load that will masquerade
   as encoder bugs. Use `MFCreateVideoSampleAllocatorEx` (built exactly for
   this pool-and-recycle contract) or `IMFTrackedSample` release callbacks;
   the fence additionally orders the producer side.
3. **"Original" semantics — decided.** The clone source is post-conversion and
   post-user-rotation, but pre-effect/pre-zoom. Fidelity note: the BGRA route
   keeps today's double chroma
   conversion (camera NV12 → BGRA → encoder converts back to NV12) — no
   worse than the CPU path, but the *better-than-today* option is an
   NV12-input original writer (encoder-native format, no double
   conversion, 12 vs 32 bits/px). Do not foreclose it; it is also the 4K
   bandwidth answer.
4. **State the VRAM budget — done.** The shared cap is 384 MiB/48 slots.
   Paired 4K recording therefore reaches pool pressure earlier and reports
   drops rather than allocating without limit; an NV12 pool remains a future
   memory-quality optimization.

Gate it with the shipped percentiles plus recording counters. At 1080p30 it is
an efficiency improvement; at paired 4K30 it is the scaling requirement
(roughly 2 GB/s of avoidable BGRA round-trip traffic). Required acceptance:
30-minute original+processed+audio recording, zero unreported drops, playable
fragments after interruption, timestamp alignment unchanged, no UI p95
regression, and a measured reduction in readback bytes/latency versus the CPU
fallback on the same camera and canvas.

**Zero-copy capture is the stated goal of the capture side**, specified in
[plan 20 section I](20-capture-recording-integrity.md) (it lives there because
it shares the camera-selection UI and `OpenSource`/`ConfigureReader` with camera
mode selection). It matters most to *this* plan: taking `ID3D11Texture2D`
samples via `MF_SOURCE_READER_D3D_MANAGER` and registering them with CUDA
directly removes the decode-to-RAM, the per-frame `std::vector`, the pinned
staging copy, and the PCIe upload from the steady-state path. That is the
largest single copy reduction available in the project, it is the thing that
decides whether 4K is ever viable, and it removes per-frame CPU work that
currently competes with the UI thread. Use this plan's Phase 1 telemetry to
measure the win, not to decide whether to pursue it.

Buffer pools now use three pinned CUDA upload slots and four asynchronous
D3D12 readback slots, each with explicit skip/drop accounting rather than an
unbounded wait. The GPU-fed recording path has its separate lazy 384 MiB,
48-slot maximum pool because a Media Foundation encoder may retain multiple
samples beyond the application's 12-pair submission queue. Do not deepen any
of these pools without updating the queue policy and Phase 1 counters; a
deeper ring without accounting only postpones overflow and hides it.

## Phase 4 — Targeted cleanups

- **Lecture notes append was quadratic — implemented.** Every section used to read-all the HTML,
  inserts at a marker, and rewrites the whole document
  (`assistive_runtime.cpp:1015-1050`). The runtime now appends new sections
  directly during the session and writes final closing tags on close, keeping
  the HTML document as the published artifact without O(n²) rewriting.
- **Stabilization redundant launches.** In Automatic mode
  (`cuda_interop.cpp:2400-2502`) NVOF runs, the feature-pairs kernel is *also*
  launched (early-returning on the device-side gate), and projections plus the
  projection-estimate kernel run **unconditionally every frame**. Measured 1-3
  ms total on an RTX 4090, occasionally ~10 ms. Profile each engine
  independently, then skip the projection path when a higher-tier estimator
  already produced a valid model, keeping it warm only as the documented
  fallback. Do not do this blind — the projection buffers double as the
  fallback's history.
- **Per-frame descriptor updates** (`presenter.cpp:305-313`): cache SRVs for
  stable shared textures. Micro; only if Phase 1 shows it.
- **Runtime HLSL compilation at startup** (`presenter.cpp:43-68, 671-736`):
  precompile to bytecode to cut startup latency and remove a runtime failure
  mode. Backlog.
- **CPU fallback** (`frame_pipeline.cpp:339-425`): deprecated diagnostic path.
  Freeze or delete rather than optimize.

## Acceptance targets (engineering goals, not current measurements)

1080p60 preview: UI-thread frame work p95 < 4 ms; capture→present p95 < 50 ms;
no UI stall > 50 ms; preview drop < 1%; recording drop 0.
4K30 paired recording: resolution independent of window; no unreported drops;
bounded encoder queue; aligned sequence numbers; stop/finalize without blocking
the UI; memory plateaus over 30 minutes.
120 Hz viewport: presentation scheduling p95 < 8.33 ms; no synchronous GPU
queue drains in steady state.
AI/OCR: preparation off the UI thread; cancellation visible within 1 s; a
stalled turn always recovers via watchdog (plan 23).
