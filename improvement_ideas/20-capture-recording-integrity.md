# Plan 20 — Capture and Recording Integrity

Status: **P0-P4 IMPLEMENTED — hardware verification and P6 remain.**
Proposed 2026-07-24; re-audited and expanded 2026-07-28, then implemented
through P4 the same day. The audit below is retained as pre-fix evidence; its
old line numbers describe the state that motivated the implementation.
Original verdicts and evidence: plan 19.

Implementation result (2026-07-28):

- P0: capture timestamps, sequence identity, signed stride, exact negotiated
  fractional rate, and VFR/gap-preserving writer timeline landed.
- P1: checked finalization, explicit states, asynchronous teardown, and
  per-cause loss summaries landed. Encoder startup or failed finalization can
  no longer claim a recording was saved.
- P2: processed video now uses a persistent Source/1080p/1440p/2160p canvas;
  window/DPI changes do not alter it, and camera-format changes continue in
  paired `_partN` segments.
- P3: a bounded 12-frame recording/encoder worker queue landed. On 2026-07-30,
  the remaining same-rate single-mailbox loss was removed: Media Foundation
  frame arrival now wakes processing directly, preview remains intentionally
  keep-latest, and active recording retains a six-frame camera burst before
  the encoder queue. A 15-second 720p30 hardware run wrote all 450 paired
  direct-GPU frames with zero capture, pool, queue, or encoder drops.
- P4: negative-stride normalization and fail-closed CUDA/DXGI adapter matching
  landed.
- Automated result: release compile passed, CPU CTest 6/6, CUDA CTest 8/8.
  `recording_integrity_tests` covers timing, gaps, state/completion decisions,
  canvas geometry/resampling, drop arithmetic, and negative stride.
- P5 remains a Windows hardware matrix: reconnect/activation lifetime has not
  been reproduced or disproved by automation.
- P6 remains its explicitly separate DXVA/zero-copy mini-plan. No partial
  zero-copy path was shipped. **That mini-plan now exists:
  [plan 28](28-dxva-zero-copy-capture.md) (2026-07-29) supersedes §I below
  with a staged sandbox-first sequence and explicit owner gates.**

**2026-07-30 field failure and fix — bounded stop.** A real recording left
the UI on "Finishing" forever with 83-byte header-only files: the worker was
wedged inside a synchronous first `WriteSample` (or another unbounded
encoder/driver call), Stop had no deadline, and the destructor's unbounded
join could freeze app close. Fixed the escape path: per-call stage telemetry
in `VideoRecorder` (`WriterStage`, observable while blocked) plus a
worker-op tracker in `RecordingManager`; an independent 8-second stop
watchdog that names the exact blocked call, forces `Failed`, restores the
UI, and disables recording until restart; truthful "Stopping" vs
"Finishing" status; a bounded destructor join with detach fallback; and
suppression of stale "saved" reports from a worker that un-wedges after
abandonment. Remaining from the incident handoff (deliberately not done
blind): the standalone GPU recording probe matrix (H.264/AV1 × audio ×
GPU-fed/readback × stop-under-load ×20 cycles) to find *why* the first
WriteSample wedged, runtime codec fallback on first-sample stall, and — if
the probe proves driver-level wedging is reproducible — the helper-process
encoder isolation. Root-causing the wedge is hardware work; the app is no
longer hostage to it.

**2026-07-30 ROOT CAUSE FOUND AND FIXED.** A new standalone GPU-fed codec
test (`tests/media_writer_gpu_tests.cpp` — shared D3D12 texture + fence
built with the production pool's exact parameters, driven through
`StartGpu`/`AddGpuFrame`) reproduced the incident in isolation: a permanent
wedge at the third submitted frame, which the new writer-stage telemetry
named as `ID3D11DeviceContext::Flush`. Cause: `InitializeGpuDevice` handed
its D3D11 device to `IMFDXGIDeviceManager` **without
`ID3D10Multithread::SetMultithreadProtected(TRUE)`** — a documented
requirement — so the hardware encoder MFT's worker threads raced the
recording worker on the immediate context and wedged the driver right when
the encoder's async threads spun up. The capture-side device always had
the protection; the writer-side device was missing it. Three-line fix in
`media_writer.cpp`; the repro test now passes for GPU-fed H.264 **and**
AV1 (90 frames, correct fragment cadence, clean finalize) and runs in the
standard gate with a clean skip where D3D12 sharing is unavailable. The
watchdog/heartbeat/abandon machinery stays as defense in depth.

**2026-07-30 follow-up hardening (all landed, three-leg gate green):** the
watchdog became a live-session heartbeat (any single recorder call over
10 s — including encoder startup and mode-change finalize, which the
stop-only watchdog could not see — fails the session with the exact stage);
abandoned recorders are poisoned so no thread can Finalize a sink writer a
wedged WriteSample may occupy (teardown deliberately leaks the COM refs);
the session-ended callback can no longer release the microphone under a
newly started session; the keyframe/fragment interval is pinned to ~2 s
(`MF_MT_MAX_KEYFRAME_SPACING`) with a fragment-cadence regression test, so
a process crash after the first fragment loses at most ~2 s of media; and
startup sweeps recent dated folders for header-only leftover pairs.
**Crash-survivability status:** process crash → playable up to the last
completed fragment (bounded ~2 s), both video and audio tracks (they share
fragments); machine crash/power loss → additionally loses the OS
write-cache tail (NTFS lazy writer, typically seconds) — full power-loss
durability would need the sink writer built over an owned `IMFByteStream`
with periodic `Flush()`, noted as an optional follow-up; the window before
the first fragment is unrecoverable by definition (no samples exist) and
its leftovers are now cleaned automatically.

**2026-07-31 external-review fix — detached-worker lifetime closed.** An
external review confirmed a real residual hazard in the 2026-07-30 bounded
destructor: detaching a wedged worker and then letting `~RecordingManager`
destroy members could still (a) run `~VideoRecorder → Stop → Finalize`
against COM objects the wedged worker occupies, and (b) hand the worker
freed queues/callbacks if the blocked driver call returned between manager
destruction and process exit. Fixed with an explicit leak contract:
`ShutdownForProcessExit()` performs the bounded handshake (join on clean
exit); on a wedge it abandons both recorders *before* detaching and returns
false, and app teardown then leaks the whole manager
(`unique_ptr::release`) instead of destroying it. Worker-side UI posting
(`PostStatus`/`PostButtonState`/`PostSegmentSaved`/session-ended) and
`StartSegment` gained abandonment guards so an un-wedged worker in a leaked
manager touches neither Qt objects nor the app-owned `UserDataPaths`. The
reviewer's maximal alternative (helper-process encoder isolation) remains
noted above as the escalation if hardware ever shows wedges that survive
these guards.

**2026-07-31 second review pass — contract completed.** The re-review found
the first fix incomplete and it was right: entry guards in the *manager*
did not stop a call chain resuming past one blocked recorder call, and
`MFShutdown` at app close could race a still-wedged MF thread. Closed by
(1) poisoning every mutating `VideoRecorder` entry (`Start`, `StartGpu`,
`AddFrame`, `AddGpuFrame`, `AddAudioFrame`; finalize was already guarded)
so an abandoned recorder fails fast at the API boundary, and (2) skipping
`MFShutdown`/`CoUninitialize` whenever a detached MF thread may exist —
recording worker (leak contract) or microphone reader
(`AudioCapture::WasAbandoned`). Microphone shutdown itself was rebuilt on
an independently owned session block (shared_ptr owned by the capture
loop, the stop-time flusher, and the object) so a detached audio thread
never references `AudioCapture` members at all, no path performs an
unbounded join, and a restarted microphone cannot alias a stale thread's
flags.

**2026-08-01 third review pass — delivery and recovered-abandonment gaps
closed.** The session block alone did not protect callbacks that had already
passed its post-`ReadSample` check and then stalled in buffer conversion or
locking: those lambdas still captured raw `OpenZoomApp*`, so a detached reader
could resume into freed app state or a new session. Frame/error delivery now
retains an independently owned `MicrophoneCallbackTarget`; Stop cancels and
advances its generation before waiting, delivery is serialized by its mutex,
and destruction clears the app pointer under the same mutex. Capture also
rechecks cancellation after conversion, after lock, and immediately before
dispatch. The same review found that a poisoned recording worker could recover
and join, making the manager safe to destroy while its intentionally leaked
recorder COM objects still made `MFShutdown` invalid. App teardown now treats
sticky `IsWorkerAbandoned()` as a separate MF/COM teardown veto regardless of
the shutdown handshake's safe-to-destroy result.

Recording currently produces files that *look* fine and are quietly wrong:
wrong playback speed, silently missing frames, window-dependent resolution,
and a "saved" message that is not backed by a checked finalize. For a student
recording a lecture they cannot re-attend, a silently corrupted recording is
the worst possible failure in the product.

## What improved since 2026-07-24 (verified — do not re-fix)

The recording stack was hardened while other plans landed; the original text
of this plan understates it. Credit before criticism:

- **Fragmented MP4 container** (`MFTranscodeContainerType_FMPEG4`,
  `src/common/media_writer.cpp:157-160`): the file is playable up to the last
  completed fragment even if the process dies. This genuinely softens D.
- **Disk-space guards**: 500 MB pre-flight check before starting
  (`media_writer.cpp:95-100`) and a periodic in-flight check that stops
  cleanly before the volume runs dry (`:199-207`), with a `DiskFull` stop
  reason surfaced as a calm message
  (`src/app/recording_manager.cpp:299-307`).
- **AV1 → H.264 codec fallback** at start (`recording_manager.cpp:147-156`).
- **Original/processed pairing by readback request id**:
  `StorePendingOriginal` (`src/app/app_pipeline_runtime.cpp:678`) pairs the
  CPU original with the async processed readback completion
  (`HandleProcessedReadback`, `:747`) — the two files can no longer pair
  arbitrary frames.
- **Size change now stops gracefully** with a "saved so far" message
  (`recording_manager.cpp:281-292`) instead of an opaque failure. (It still
  *stops* — that is section C.)
- **§E camera mode selection implemented** via plan 27 Phase 4:
  `ConfigureReader` sets `MF_MT_FRAME_SIZE`/`MF_MT_FRAME_RATE`, reads back
  the negotiated format into `negotiatedFormat_` incl. frame-rate
  numerator/denominator (`src/capture/media_capture.cpp:373-378`), reports
  driver fallbacks (`:385`), and handles mid-stream
  `MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED` (`:759+`).
- **12-hour cap** (`recording_manager.cpp:318-322`) and recordings routed
  through the plan-21 `UserDataPaths::RecordingsForDate` layout.

Everything below records what was wrong at re-audit time and the requirements
used to judge the implementation.

## The verified data flow (read this first)

```
capture thread (media_capture.cpp:714-725)
  ReadSample → timestamp OBTAINED then DISCARDED → MediaFrame{pixels only}
    ↓ keep-latest slot (correct for preview, silent for recording)
pipeline tick (app_pipeline_runtime.cpp)
  gate: newCameraFrame && recording active        (:631)
  GPU path: async viewport readback requested     (:656-664)
            original stored, keyed by request id  (:678)
            completion → HandleProcessedReadback  (:747) → AppendFrame
  CPU path: AddFrame(presentationBuffer_,
            mapping.targetWidth/Height, original) (:1300-1304)
    ↓
writer (media_writer.cpp)
  CFR: SetSampleTime(rtStart_); rtStart_ += rtDuration_   (:233-243)
  rtDuration_ = 10'000'000 / fps, fps = constexpr 30
                (recording_manager.cpp:146)
  FinalizeAndStop: sinkWriter_->Finalize() HRESULT ignored (:132-140)
```

Two structural facts fall out of this flow:

1. **The processed recording is the viewport readback** — both paths record
   at `mapping.targetWidth/Height`, i.e. whatever size the window happens to
   be. That is the root of C.
2. **Wall-clock time never enters the file.** The camera's real cadence, every
   drop, and every stall are erased by the synthesized 30 fps timeline. That
   is the root of A.

## A. Frame identity — timestamps, durations, sequence numbers

**Problem (re-verified).** `MediaFrame` carries pixels and nothing else —
no timestamp, no sequence, unsigned stride
(`include/openzoom/capture/media_capture.hpp:19-26`). The capture loop
receives the sample timestamp and drops it (`media_capture.cpp:717-725`).
The writer synthesizes a fixed-step timeline (`media_writer.cpp:233-243`)
at a hard-coded 30 fps (`recording_manager.cpp:146`).

**Effect.** A 15 FPS phone camera plays at double speed; a 60 FPS camera
loses half its temporal information; every drop, reconnect gap, and encoder
stall silently compresses the timeline. A student scrubbing to "minute 40 of
the lecture" lands somewhere else entirely.

**The negotiated rate is already in hand.** Plan 27 Phase 4 populated
`negotiatedFormat_.numerator/denominator` (`media_capture.cpp:376-377`);
it just never reaches the `constexpr UINT fps = 30`. This is now a plumbing
fix, not a design project.

**Fix, in order:**

1. Extend the transport struct (folds section F's signed stride in):

```cpp
struct MediaFrame {
    std::vector<uint8_t> data;
    GUID subtype{GUID_NULL};
    UINT width{0};
    UINT height{0};
    LONG stride{0};                    // signed — section F
    size_t dataSize{0};
    LONGLONG captureTimestamp100ns{-1}; // from ReadSample, -1 = unknown
    std::uint64_t sequenceNumber{0};    // capture-thread counter
};
```

   Fill both at the one call site (`media_capture.cpp:717-806`); the counter
   is a plain member of the capture loop, incremented per delivered sample.

2. Thread `captureTimestamp100ns` + `sequenceNumber` through the keep-latest
   slot, `PrepareOriginalFrame` (`app_pipeline_runtime.cpp:1041-1047`), and
   both `AddFrame` paths into `RecordingManager` alongside the pixels.

3. Writer goes VFR: `VideoRecorder::AddFrame` takes the frame's timestamp,
   normalizes against a `recordingStart100ns_` origin captured on the first
   frame, calls `SetSampleTime(normalized)` and
   `SetSampleDuration(next − current)` using the negotiated frame duration
   as the estimate for the most recent frame. Fragmented MP4 + Media
   Foundation handle VFR fine; **gap policy: write the real gap** (a stall
   appears as a still moment at the correct wall-clock position, which is
   the truthful rendering of what the student experienced). Keep the
   duplicate-frame CFR alternative out — it fabricates data.

4. `RecordingManager::SetRecordingState` receives the negotiated rate from
   the capture layer and passes it to both `VideoRecorder::Start` calls in
   place of the constant at `:146`; the sink's `MF_MT_FRAME_RATE`
   (`media_writer.cpp:176`, `:188`) becomes the negotiated ratio (keep the
   real numerator/denominator — 30000/1001 must not be rounded to 30).

5. Frames whose timestamp is `-1` (paranoia path) get
   `previous + negotiated duration` and increment a counter that appears in
   the stop summary (B4).

**Tests (CPU-runnable, msvc-cpu):** extract the timestamp-normalization and
duration math into a testable header (no MF types needed for the math);
feed synthetic 15/30/60 FPS and gap sequences; assert emitted
time/duration pairs, gap preservation, and 30000/1001 exactness.

## B. Separate recording from preview

**Problem (re-verified).** Preview and recording share one keep-latest frame
slot and the presenter's small async-readback ring; when the ring is
saturated or a frame is overwritten before the tick consumes it, the frame
simply never reaches the recorder — no counter, no trace, no user-visible
sign. The gate at `app_pipeline_runtime.cpp:631` and the request-id pairing
(`:678`/`:747`) decide what gets recorded as a side effect of presentation
pacing.

**Nuance that shapes the fix (unchanged):** keep-latest is *correct for
preview* — a magnifier must show the newest frame, never a backlog. Do not
add a queue to preview.

**Fix:**

1. **B-lite first (drop accounting, half a day, no threading):** count every
   frame that enters the capture callback while recording is active, and
   every frame actually appended. Counters by cause: capture-slot overwrite,
   readback-ring skip, recording-texture-pool exhaustion, pairing miss,
   encoder reject. On Stop, the summary
   names them: "Recording saved. 3 frames were dropped (display busy)." —
   and the counts go into the session log. Until B-full lands, *honesty
   substitutes for capacity*.
2. **B-full (with plan 22 section A):** a bounded timestamped recording
   queue (start 8-16 frames) fed at capture, drained by a recording worker
   thread; saturation policy = block ≤ one frame interval, then drop-oldest
   with accounting. Original and processed of the same instant share the
   same `sequenceNumber` and are paired before submission — keep the
   request-id pairing as the transport, add a sequence assert so a
   mispairing is a logged bug, not a silent drift.
3. The recording queue consumes the **recording canvas** output (C), not the
   presentation readback, which decouples recording completely from window
   state and present pacing.

## C. Fixed recording canvas

**Problem (re-verified).** Both recording paths append viewport-sized
buffers (`HandleProcessedReadback` from the presentation readback; CPU path
`AddFrame(presentationBuffer_, mapping.targetWidth/Height, ...)` at
`app_pipeline_runtime.cpp:1300-1304`). Consequences: the file's resolution
follows the window; any size change stops the recording
(`recording_manager.cpp:281-292` — graceful now, but still a stop); two
recordings of the same lecture differ by monitor placement; and DPI/monitor
moves are recording hazards.

**Fix:**

1. A dedicated recording render target with a fixed geometry chosen at
   start: **source resolution (default)**, 1080p, 1440p, 2160p. Processed
   recording renders the same processed scene the viewport shows, sampled at
   the recording target's geometry through the canonical `ViewTransform` —
   one extra scaled render + readback per camera frame, only while
   recording. (GPU path: reuse the existing scaling kernels; CPU path: the
   existing resize into a fixed-size buffer.)
2. Window resize, DPI change, monitor move, Simple/Advanced switches, and
   inspector docking **must not touch the recording**. Once the canvas is
   fixed, delete the size-change Stop at `recording_manager.cpp:281-292`
   for the *processed* stream.
3. The **original** stream records at camera-native size, so a mid-recording
   camera mode change (now possible through plan 27's real mode switching,
   and arriving via `MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED`,
   `media_capture.cpp:759+`) is a real event with a policy: **finalize the
   current pair and auto-start a new segment pair** (`VID_x_part2_*.mp4`),
   announced. Never resample the original — it exists to be the untouched
   record.
4. The canvas choice persists in settings (`recording` object) and appears
   next to the recording controls with the standard `setA11y` treatment.

## D. Honest completion

**Problem (re-verified, softened).** `FinalizeAndStop` still discards the
`Finalize()` HRESULT (`media_writer.cpp:132-140`). The fMP4 comment there is
correct that completed fragments survive — but the trailing fragment and the
finalize itself can still fail (disk full at the end, device removal,
antivirus lock), and the UI's "saved" message currently cannot be false*r*
than the writer knows.

**Fix:**

1. Capture the HRESULT from `Finalize()` per writer; `Stop` returns a
   per-file outcome (`Completed` / `CompletedTruncated(hr)` with "playable
   up to ~T" derived from `rtStart_`).
2. Model recording as an explicit state machine in `RecordingManager`:
   `Idle → Recording → Stopping → Finalizing → Completed | Failed` (the
   enum exists in spirit; make the transitions the only mutation path).
   Report success only after **both** writers finalize; on any failure name
   the file, the reason, and what survived.
3. Finalization must not block the UI thread (plan 22): `Stopping` hands
   both writers to a joinable worker; the UI shows "Finishing recording…"
   until the state machine lands in `Completed`/`Failed`. Announce the
   terminal state (announcement, not TTS — standing rule).

**Test:** lock the target file (or inject an HRESULT) → the state machine
must end `Failed`, the toast must not say "saved", and the message must name
the file.

## E. Real camera mode selection — remnant only

> Implemented 2026-07-26/28 through plan 27 Phase 4 (request + negotiated
> read-back + fallback reporting + mid-stream change handling). Two remnants
> live here:

1. **Feed the negotiated rate into the writer** — covered as A4; listed here
   because it is the §E/§A seam and the reason §E stays referenced.
2. **Live hardware verification** — request an unsupported mode and confirm
   the "driver selected X instead" report; switch modes mid-recording and
   confirm the C3 segmenting policy. Owner/hardware checklist item.

## F. Signed stride

Re-verified unchanged: `outFormat.stride = static_cast<UINT>(std::abs(rawStride))`
(`media_capture.cpp:612`) discards bottom-up orientation, which vertically
flips some RGB sources. Fixed by A1's `LONG stride` plus explicit row-origin
normalization at the conversion site; add the negative-stride unit test
(synthetic bottom-up buffer → converter → assert row order).

## G. Adapter pairing (multi-GPU)

Re-verified, with a wrinkle: there are now **two inconsistent policies** in
the same file. `SelectCudaDeviceMatching` falls back to device 0 with only a
`qWarning` (`src/cuda/cuda_interop.cpp:525-551`), while the import path
throws "No CUDA device matches the D3D12 adapter LUID" (`:727-729`).
`EnumAdapterByGpuPreference(0, HIGH_PERFORMANCE)` remains the only
preference query (`src/d3d12/presenter.cpp:473-499`).

**Fix.** One policy: enumerate high-performance adapters in order, reject
software adapters, verify D3D12 feature level, read the LUID, require a
CUDA device with the same LUID; take the first full match. If none matches,
**disable CUDA cleanly with a plain-language explanation** — never silently
run split-GPU. Delete the device-0 fallback at `:550`. (On the owner's
single-GPU 4090 this already resolves correctly — portability fix, not a
local bug.)

## H. Reconnect and activation lifetime — hypothesis to test

Unchanged and still unconfirmed. `EnumerateFormats` activates a descriptor
guarded by `ActivationShutdownGuard` while live capture stores the same
activation; refreshing modes right after a reconnect may shut down the
activation the new session is using. **Test before fixing:** unplug/replug,
confirm reconnect, immediately refresh modes, watch for stream death. If it
reproduces: never enumerate through an activation owned by a live capture —
separate activation instance, or defer refresh until capture stops.

## I. GPU capture — zero-copy is the goal

> **The target architecture is a zero-copy capture path: the camera frame is
> born in GPU memory and is never copied into system RAM at all.** Phase 1
> (DXVA on by default) is compatibility groundwork, not the destination.

**Owner decision 2026-07-24 stands:** DXVA on by default, automatic
per-camera fallback, compatibility check in the camera UI. Today
`MF_SOURCE_READER_DISABLE_DXVA` is still forced TRUE
(`media_capture.cpp:440`), so every frame round-trips through system RAM.

Do this together with the E-remnant hardware pass — both touch
`OpenSource`/`ConfigureReader` and surface in the same camera UI.

### Why two phases, not one

Clearing the DXVA flag permits hardware decode but samples still arrive in
system memory unless the reader also gets a DXGI device manager
(`MF_SOURCE_READER_D3D_MANAGER`), after which samples arrive as
`IMFDXGIBuffer` wrapping `ID3D11Texture2D`; this project then needs
D3D11→CUDA interop (`cudaGraphicsD3D11RegisterResource`) or a shared handle
into D3D12. The flag is sequenced first because the per-camera fallback
ladder and its UI are identical for both phases.

### Phase 1 — accelerated reader with automatic per-camera fallback

1. Default `MF_SOURCE_READER_DISABLE_DXVA` to FALSE.
2. Detect a bad camera and fall back, covering the realistic failure modes:
   creation/negotiation failure with DXVA → silent same-camera retry without
   it; streaming-but-no-sample within a bounded window; degenerate samples
   (uniformly black/constant ~1.5 s while claiming to stream — the classic
   virtual-camera failure; reuse the stabilization luma downsample or a few
   CPU rows, near-free).
3. **Crash-safe marker**: persist `dxvaAttemptInProgress = <cameraId>`
   before opening with DXVA, clear on validated frames; if it survives a
   restart, the attempt hung or crashed — disable DXVA for that camera and
   say why. Same pattern browsers use for GPU blocklisting.
4. Persist per camera keyed by symbolic link (enumeration order shifts):
   `auto | forceOn | forceOff` + last automatic decision's reason and date.
5. **Never leave the user on a black frame.** Every path ends in a working
   picture or a plain-language message, announced (not TTS).

### Camera-selection UI

Per camera: current state ("GPU accelerated" / "Compatibility mode — this
camera had trouble with GPU capture on <date>"), override
Automatic/Force GPU/Force compatibility, and a **"Test this camera"** probe
(open with DXVA, read N frames, validate, close) with progress and an
announced result. The probe runs off the UI thread, cancellable, hard
timeout — a bad driver hangs rather than erroring.

### Phase 2 — zero-copy: the actual objective

`MF_SOURCE_READER_D3D_MANAGER`, take `ID3D11Texture2D` samples, register
with CUDA directly. Deletes the decode-to-RAM, the `std::vector` frame, the
pinned-staging copy, and the PCIe upload from the steady-state path — the
largest copy reduction available anywhere in the project, and what makes 4K
viable. Land it behind the Phase 1 ladder (zero-copy → accelerated-with-copy
→ CPU); frame identity (A) must survive — a GPU-resident frame still
carries timestamp and sequence number and recording still consumes it
through the recording queue, not the preview path.

### Hardware test matrix (document before automating)

Phone-as-webcam (Camo, DroidCam, Iriun — the owner's real setup), plain USB
webcam, laptop internal, OBS virtual camera. Each: first-open with DXVA,
forced fallback, crash-marker path (kill mid-probe, relaunch), re-test after
override toggle.

## Implementation order — phases sized as agent-days

| Phase | Content | Sections | Size |
|---|---|---|---|
| **P0** | MediaFrame identity fields + negotiated rate into writer + VFR timestamps + timestamp-math tests. **Fixes wrong-speed files — the single worst defect — in one day.** | A, E1, F (struct) | 1 day |
| **P1** | Checked finalize + state machine + off-thread finalizing + drop accounting with stop summary | D, B1 | 1 day |
| **P2** | Fixed recording canvas + resize survives + segment-on-mode-change | C | 2 days |
| **P3** | Recording queue + worker thread (coordinate with plan 22 A) | B2-B3 | 1-2 days |
| **P4** | Stride normalization test, adapter-pairing unification | F, G | 0.5 day |
| **P5** | Reconnect hypothesis test, then fix if real | H | 0.5 day + hw |
| **P6** | DXVA Phase 1 (fallback ladder + UI), then Phase 2 zero-copy | I | own mini-plan |

Each phase lands with its tests and doc updates in the same change
(`agents.md` rule). P0-P2 are independent of plan 22; P3 explicitly is not.

## Tests this plan must add (plan 24's gates now exist to run them)

CPU suite (`msvc-cpu-tests`): timestamp normalization/duration math at
15/30/60 FPS incl. 30000/1001 and gap sequences; negative-stride row-order
round trip; drop-counter arithmetic; state-machine transitions incl.
injected finalize failure; recording-canvas geometry via `ViewTransform`
(fixed target vs moving viewport). Windows/hardware (manual matrix first):
recorded duration vs wall clock; induced starvation shows non-zero drops in
the summary; window resize + monitor move during recording leaves the file
untouched; disk-full finalize; mid-stream mode change produces segment
pairs; reconnect activation lifetime; DXVA fallback persistence; degenerate
frame detection.

## Acceptance

A 10-minute lecture recorded from a 30 FPS phone camera plays back at
exactly 10 minutes; the same from a 15 FPS camera plays at correct speed.
Resizing the window and moving it between monitors during recording changes
nothing about the output file. The stop summary reports 0 drops on an idle
machine and honest non-zero counts under load. The completion message
appears only after both writers finalize; an injected finalize failure ends
in `Failed`, names the file, and never says "saved". Unplugging the camera
mid-recording yields a finalized, playable pair plus a clear message. A
mid-recording camera mode change yields two playable segment pairs with an
announcement.
