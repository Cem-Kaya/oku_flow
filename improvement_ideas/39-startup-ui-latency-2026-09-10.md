# Startup and UI latency — 2026-09-10

The requested work targets slow camera appearance, UI responsiveness, and
frame latency. Each finding below records its implementation and practical
limits. Measurements and the final bundle verification are appended after
the controlled camera runs.

1. **Initial camera activation blocked the Qt thread.** Startup now opens a
   separate worker-owned capture session after app services/settings exist and
   before showing the window. Camera opening overlaps native graphics setup.
   Completion adopts the session through revocable, generation-checked ingress;
   camera selectors are disabled until adoption. Errors received before adoption
   are retained, and acceleration fallback remains on the worker.

2. **Mode discovery opened the camera before opening it again for capture.**
   The streaming reader now snapshots native formats and resolves the saved
   stable ID itself. Missing IDs retain automatic negotiation; explicit native
   format callers keep precedence. The UI populates modes from the live snapshot
   and preserves negotiated-format notices. This also avoids using a stale
   format pointer when switching between physical cameras.

3. **Ordinary presentation could wait on the UI thread for a frame slot.**
   Admission now checks allocator completion before polling the swap-chain
   latency object with zero timeout. Busy work retains a dirty scene without
   resetting an in-flight allocator or submitting new work. Device loss and
   wait failure remain terminal faults. Synchronized `Present(1, 0)` and bounded
   teardown/resize drains remain; zero-timeout admission does not make every
   graphics API call asynchronous.

4. **D3D11 conversion polling spent up to 3 ms per attempt on the UI thread.**
   Each attempt now queries once and returns to Qt if pending. The existing
   retained-frame retry protocol and 25 ms overall bound preserve the producer
   allocation until conversion/CUDA handoff is safe. Startup validation still
   examines 30 frames and already delivered early frames before this change.

5. **Recording depended on successful viewport admission for synchronization.**
   Both recording clones now queue the actual CUDA producer fence explicitly,
   even if the viewport skips an attempt. Subsequent CUDA work waits on actual
   recording completion; an unsubmitted presentation reservation is never used
   as a dependency. The no-semaphore fallback keeps its ownership drain.

6. **Latency statistics could count processing without a displayed submission.**
   A processed scene retains its source timestamp through deferred presentation.
   Its first successful present consumes that sample once. CPU and CUDA paths
   count successful presents consistently. Profiling separately reports actual
   callback arrivals, ingress drops, processed scenes, presentation attempts,
   and negotiated camera rate.

7. **Startup improvements lacked a repeatable measurement.** The tracked
   PowerShell profiler alternates camera-opening sequences in one executable,
   stores explicit isolated settings and per-trial data, disables AI/notes/setup,
   checks the source settings hash, and lets each child exit normally. It refuses
   an already-running OkuFlow instance. Timing starts in the app constructor,
   and presentation is measured at successful submission; this is not a
   sensor-to-photon benchmark. Preliminary runs made before correcting Windows
   known-folder isolation are excluded from the controlled comparison.

8. **Startup validation pixels were converted again for presentation.**
   Initial accelerated frames already contain BGRA bytes from the required
   validation readback. The app now reuses those bytes through its existing
   normalization/rotation/CUDA-upload path instead of starting another D3D11
   conversion and retry wait. This choice happens after polling any previous
   producer-copy lease; it preserves ownership and does not disable direct
   GPU transfer for subsequent frames without materialized CPU pixels.

9. **CPU row orientation incorrectly rejected GPU textures.** Native ARGB32
   mode metadata can advertise negative CPU stride. After validation stops
   materializing CPU bytes, that metadata remains on the GPU-only frame. The
   app's unconditional negative-stride check routed it through CPU readback on
   every frame, although D3D11 texture access has no CPU row-stride contract.
   The check now applies only to system-memory input. Bottom-up CPU frames keep
   the existing normalization path; GPU textures use their own dimensions and
   subresource metadata. The earlier `direct_gpu` status could retain a startup
   success despite these later CPU copies, so handoff and preparation counters
   were necessary to establish actual steady behavior.

10. **Already-BGRA textures went through another video processor.** Matching
    BGRA input now copies its visible rectangle directly into the NT-shared
    allocation. YUV and other formats retain color-aware video processing.
    The production helper checks formats, sample counts, extents, array slices,
    and mip levels; a WARP GPU test verifies cropped channel bytes from a
    nonzero slice/mip. Completion queries and producer leases remain required.

11. **A slow handoff could pay the 25 ms deadline on every frame indefinitely.**
    Three consecutive D3D11 query deadlines now latch safe-copy input for the
    capture session. Actual query success resets the streak and a new session
    resets the latch. This does not save a permanent compatibility preference.
    CUDA-copy pending work has a separate reason and retains its frame until
    actual completion or the existing CUDA ownership fault; it cannot use the
    D3D11 deadline as permission to overwrite or release resources. Stateful
    tests cover recovery, demotion, late completion, and session reset.

The work retains synchronous platform discovery, first graphics/CUDA resource
creation, manual camera changes, and bounded ownership drains. Camera exposure,
driver delivery, and monitor refresh limit achievable frame rate independently
of UI responsiveness.

## Controlled findings

All trials used the integrated 1280x720 camera on this machine with nominal
30 FPS negotiation and isolated copies of the saved settings. Source settings
hash checks passed. Raw JSON/logs are in the ignored `build/startup-profile-*`
directories; figures here preserve the relevant evidence.

- Before the final startup-frame optimization, two alternating comparisons
  measured legacy first presentation at 1970/1984 ms versus worker startup at
  1776/1754 ms, a 194–230 ms improvement. Maximum 20 ms Qt heartbeat delay fell
  from 1564/1570 ms to 721/697 ms. Both camera-opening modes share the other
  rendering changes in this executable; this comparison isolates their opening
  sequence, not the whole patch against an old shipped executable.
- The initially suspicious 30-arrival/15-present FPS pattern came from paired
  upstream deliveries: 312 arrivals included 156 intervals under 8 ms and 155
  over 45 ms. Only three presenter admission attempts were missed. Compatibility
  capture delivered 14.98 callbacks/s, with 140 receipts, one ingress drop,
  138 presentations, and zero presenter misses. It retained the same nominal
  30 FPS type. The evidence is consistent with upstream rate conversion of a
  slower camera/source cadence; pixel identity and exposure causality were not
  measured. No sensor settings or ingress capacity were changed.
- Compatibility startup took 880 ms, with 1.99 ms capture-to-present p95.
  Accelerated startup involved 30 validation readbacks; 26 early frames hit the
  conversion-query retry bound in one run. Its later diagnostic window averaged
  about 4.4 ms, so the roughly 55 ms aggregate p95 included startup behavior.
- An experiment with `MF_LOW_LATENCY=TRUE` did not eliminate paired arrivals
  on this camera and was not retained. The option follows Microsoft's
  [Source Reader low-latency configuration](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-low-latency);
  source/reader native modes are logged separately for diagnosis. See also the
  [advanced video processor's conversion capabilities](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-enable-advanced-video-processing).

## Final implementation measurements

The final direct-copy run lasted 20 seconds (`build/startup-profile-direct-copy`):

| Measurement | Result |
|---|---:|
| First successful camera presentation | 1656 ms |
| Window shown | 649 ms |
| Maximum startup Qt heartbeat delay | 615 ms |
| Qt heartbeat delay p95 | 0.963 ms |
| Camera processing tick p95 | 1.809 ms |
| Capture-to-present p95 | 15.202 ms |
| Direct GPU transfer active at end | Yes |
| D3D11 handoff deadline expiries | 0 |
| Maximum observed handoff retry interval | 8 ms |

Later five-second windows logged about 4.5–5.8 ms average capture-to-present
latency and zero capture-thread CPU bytes/frame. A real processing improvement
is supported: earlier fallback-path runs spent roughly 5–7 ms per camera tick
at p95, and the exposed redundant VideoProcessor path repeatedly hit 25 ms
deadlines. The final GPU copy avoids those expiries. The controlled startup
opening comparison above remains the isolated comparison of opening sequences;
the final figures also include the subsequent input-path fixes.

FPS is deliberately not presented as a fixed speedup. During the last direct
run the reader still reported about 30 callbacks/s in bursts, while a subsequent
compatibility probe observed 7.49 actual callbacks/s, zero ingress drops, and
2.03 ms application latency p95. Earlier compatibility capture observed 14.98
callbacks/s under the same nominal 30 FPS mode. The source cadence changed
during measurement; no exposure control or image-content comparison established
its cause. Buffering converted arrivals would not establish more distinct camera
updates and would increase live-view age.

The normal-close probe exited successfully with camera opening still pending
at 1016 ms, no frames delivered, and the expected process-global MF teardown
skip while its independently owned worker was active. It did not terminate a
pre-existing application. A missing saved-mode probe opened automatic 1280x720
capture, presented successfully, retained direct GPU input, and had zero handoff
deadline expiries. Every controlled script run checked that its source settings
hash remained unchanged.

## Validation notes

One full CPU gate caught an intermittent right-side undock fixture failure.
An activation wait alone did not resolve it. Instrumentation then captured
spontaneous native mouse moves at the real pointer position overriding synthetic
held-drag coordinates; these could either cancel the hold or re-arm it after a
synthetic return to the start. The simulated-native-move fixture now ignores
only spontaneous mouse moves, retains injected moves/releases and native
deactivation/ungrab handling, and waits for owner-window activation before
injecting input. Deactivation during a pull has explicit cancellation coverage.
Production docking behavior and its 350 ms latch are unchanged. Final
matrix/bundle results follow below.

- Final `scripts/agent_build.bat`: release compile passed, CPU CTest **24/24**,
  CUDA CTest **28/28**. Translation validation passed for all **664** complete
  Turkish/German source keys.
- Final `scripts/build_release_bundle.bat`: release-bundle CTest **28/28**;
  tested staging bundle published successfully to `dist/OkuFlow/`. Logs are
  `build/startup-latency-build.log` and `build/startup-latency-bundle.log`.
- The published executable was built on **2026-09-10 at 19:27 (+03:00)** and
  is 8,108,544 bytes. Its SHA-256 exactly matches
  `build/release-bundle/cmake/Release/oku_flow.exe`:
  `B7F364AF814BABBD5BA33391C3C8125A4DB652669139CC6DA4EA98A8E7F472E5`.
  The primary bundle was available for replacement; `OkuFlow2` was not needed.
- The corrected docking fixture passed three focused runs and the complete
  annotation suite before the final successful matrices. No production docking
  behavior was altered by that fixture correction.
