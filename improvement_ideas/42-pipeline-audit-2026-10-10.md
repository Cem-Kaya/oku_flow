# Pipeline and security audit — 2026-10-10

This is a repair backlog, not a claim that all pipeline findings are fixed.
Eight Astra agents reviewed separate modules, Daybreak reviewed security, and
20 Gemini 3.8 Flash High reviews were requested for independent coverage.
The latter run's completion and provider failures are recorded in the local
validation summary. Reviews used the current working tree, including pending
UI changes, so source line numbers can move during implementation.

The detailed local reports are under `build/ui-validation/audits/`. They contain
call paths, source line numbers, primary references, counterexamples, and
proposed tests. These artifacts are ignored by git; the actionable findings
are retained below. Reviewers did not exercise cameras, microphones, accounts,
network AI requests, installer elevation, or destructive failure scenarios.
Static confidence is distinct from observed frequency on hardware.

## Implementation checkpoint

The working tree now contains bounded repairs for recording preservation,
recording producer/consumer resource ownership, the shared CUDA/graphics fence
timeline, mapped CUDA array cleanup, and Codex request generations and
initialization/cancellation recovery. The larger assistant, Hide UI suppression,
keyboard modality, chrome stacking, and responsive layout fixes are also present.
Integration review caught and corrected an additional compatibility-readback
lease gap and assistant resume/thread-start edge cases.

These changes do not resolve the remaining camera identity, pixel conversion,
recording format/aspect/timestamp, stabilization, text processing, or security
backlog below. A later CPU run passed all 26 tests, including the native UI
cases, after fixing a question-field Escape failure. The final translation
change was compiled only after the user requested stopping further tests.
See [the finding-by-finding adjudication](43-audit-verdicts-2026-10-10.md) for
accepted claims, rejected claims, duplicates, evidence strength, and the latest
validation status. A source repair is not proof of real-device fault recovery.

## Repair order

1. Preserve recordings and establish producer/consumer GPU completion before
   resource reuse. These failures can lose output or invalidate the device.
2. Make assistant callbacks belong to one request generation and guarantee a
   terminal response on cancellation/initialization failure.
3. Close the installer verification-to-launch race and validate remote VLM
   transport before preparing or transmitting camera images.
4. Correct camera identity, pixel layout, recording aspect/format transitions,
   and deterministic text/stabilization math.
5. Measure optional throughput/quality changes only after correctness tests.

## GPU resources and presentation

| Finding | Source | Regression needed |
| --- | --- | --- |
| Recording slots become reusable when a consumer releases its lease, even if the producer draw is unfinished. Pool eviction has the same gap. Independently found by buffer, presentation, and recording reviewers; parent checked the acquire/release paths. | `src/d3d12/presenter.cpp`: `RecordingFramePoolState::Acquire/Release`, `RequestRecordingFrame`; early drops in `app_pipeline_runtime.cpp` and `recording_manager.cpp` | Hold producer GPU work behind a test fence, release the lease, then request matching and different dimensions. No reuse, allocator reset, descriptor rewrite, or eviction until completion. |
| A resize drain can independently signal the same shared-fence value as unfinished CUDA work after viewport admission is Busy. Increasing counters alone cannot repair missing cross-queue ordering. | `D3D12Presenter::WaitForGpu`, `PresentSceneTexture`; app fence orchestration | Skip present with CUDA pending, then resize; the graphics drain must depend on CUDA before signaling. |
| Same-extent rotation resets/disables fence interop while retaining the live CUDA surface; failure recovery also uses this reset. | `app_controls.cpp`: rotation; `app_pipeline_runtime.cpp`: reset/ensure; `pipeline_orchestrator.cpp`: `ResetFence` | 0→180 and square-frame rotations preserve pending graphics-reader dependencies and interop mode. |
| Three mapped CUDA mipmapped arrays lack explicit frees on safe teardown and partial-construction failure. | `src/cuda/cuda_interop.cpp`: primary, SuperRes, original maps and cleanup | Every successful map is freed once before external-memory destruction; unknown-completion quarantine remains intact. |
| NV12 staging readback locates chroma using visible height instead of allocated texture height. | `src/capture/media_capture.cpp`: `CopyGpuFrame` | Visible 1080 / allocated 1088 frame with known luma/chroma rows. |
| Explicit RGBA readback is relabeled RGB32/BGRA without swapping channels. | `media_capture.cpp`: DXGI fallback; CPU `CopyRgbxToBgra` | Distinct red/blue test pixels through the actual fallback route. |

Direct3D requires allocator execution to finish before reset and permits fence
rewinds when independent signalers race. See Microsoft's
[allocator contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset)
and [multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization).

## Capture, geometry, recording, and audio

- Failed camera activation leaves the reconnect identity empty or pointing to
  the previous camera. Track intended identity before attempting activation.
  Enumeration is not refreshed after the retry window, and saved selection
  is an array index rather than a stable symbolic link. An acceleration probe
  can restore an obsolete selection after the user chooses another camera.
  Owners: `media_capture.cpp`, `capture_facade.cpp`, app controls/runtime/settings.
- Native-mode selection may negotiate a converted output instead of the
  requested source mode. This remains a hardware-dependent candidate; inspect
  native media type and actual arrival rate before changing negotiation.
- Unsupported dynamic media types can be accepted and then dropped forever;
  non-square pixel aspect metadata is also discarded. Add explicit supported
  type validation and PAR policy with synthetic media-type tests.
- Processed recording reuses viewport-normalized geometry on a fixed canvas
  with a different aspect, stretching content. Compute recording geometry for
  its destination or preserve the viewport aspect with uniform containment.
  Rotation changes do not transform scene-anchored annotations.
- An internal encoder finalization clears sample counts; the manager later
  interprets a one-stream failure as an incomplete pair and deletes both files.
  Parent checked the counter reset and pair-deletion path. Preserve finalized
  results and nonempty recoverable outputs; inject one-stream write/finalize
  failures in regression coverage.
- Changing processed canvas size while recording can terminate the GPU path.
  CPU/GPU fallback can feed BGRA to an NV12 sink, or vice versa. Segment or
  convert at a deliberate format boundary, and test both transition directions.
- Delivery-time audio clocks can overlap after callback bursts; video timing
  also loses source PTS spacing. Test delayed/bursty delivery against a known
  timestamp trace before changing clock domains.

## Image quality and stabilization

- Light-on-dark Sauvola applies the dark-text threshold to the wrong polarity,
  marking a uniform dark background as ink. Derive the threshold from inverted
  intensity/mean; paired image/inverse masks should agree.
- CLAHE clamps the first tile index before deriving its neighbor, causing
  left/top interpolation seams. Clamp the two original neighboring indices
  independently; check first/last half-tiles and odd dimensions.
- Glare-only mode still passes nonzero background-flatten strength to a shared
  kernel. A nonglare constant image must remain unchanged with flatten off.
- Every text-control edit resets Maxine, including unrelated CLAHE/focus/glare
  changes. Separate text-history invalidation from model configuration changes;
  count model loads and preserve the performance latch in tests.
- Stabilization fallback rejection snaps correction back while retaining
  accumulated relative state. Extra Stable can then trust stale/speculative
  motion as an absolute solution. Recovery-keyframe ROI checks compare raw
  recovery coordinates against original-anchor coordinates. Each needs a
  targeted state/coordinate regression before changing tracking policy.
- Additional robustness limits, not memory-safety findings: exposure gain can
  cause indefinite Extra Stable hold; raw patch MAE can prevent anchor
  reacquisition after brightness changes; recovery-derived anchors can chain
  registration error. Test exposure, occlusion, and >300-frame recovery traces.
- The production replay currently proves execution succeeds, not tracking
  quality. Add ground-truth scoring that fails when correction is disabled or
  all estimates are rejected. Existing short synthetic scores do not establish
  real-camera quality across these transitions.

## Assistant request ownership

The assistant auditor traced eight concrete lifecycle gaps in
`codex_app_server_client.cpp`, `assistive_runtime.cpp`, and app capture wiring:

1. Late thread/turn replies can restart canceled work or consume a replacement.
2. Initialization error/timeout can leave the queued request busy indefinitely.
3. Turning modes off during persistent image preparation misses completion.
4. A stale image worker can clear the next request's note data.
5. A stalled camera stops draining an otherwise completed GPU readback.
6. Old non-delta notifications can alter or finish a newer turn.
7. Out-of-order history replies can select/overwrite the wrong conversation.
8. A previous CPU/debug presentation can be attached after GPU camera failure.

Use one generation/turn ownership model for callbacks, workers, notifications,
and history operations. Extend the fake app-server tests with delayed and
reordered replies. Test stalled-camera capture independently of a fresh frame.
These cases require no real ChatGPT account or uploaded camera image.

## Security

- **Installer integrity:** the verifier closes its file before a later elevated
  launch opens the predictable path. A same-session local attacker plus user
  UAC approval is required; this was not exploited. Parent checked the lifetime
  gap. Keep verified file identity protected against write/delete through
  launch, use exclusive per-operation staging, and test attempted replacement
  at a launcher seam. [Windows file-sharing semantics](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
  describe the relevant protection.
- **VLM transport:** arbitrary remote HTTP permits plaintext camera frames and
  bearer credentials. Preserve deliberate local-server support while defining
  and testing a remote HTTPS and redirect policy before request preparation.
- **Hardening hypotheses:** scrub inherited environment and isolate Simple
  Read/Explain tools before execution; bound setup download sizes; make Codex
  executable provenance explicit. Daybreak did not demonstrate an assistant
  sandbox bypass. PATH trust often shares the existing same-user trust boundary.
- No concrete credential logging, notes HTML injection, path traversal, or
  response-size defect was found in the reviewed paths. This is bounded review
  coverage, not a security certification.

## Gemini validation notes

All 20 requested tasks were dispatched. Eight wrote substantive report files
(02, 03, 04, 05, 06, 08, 10, 13); the remaining 12 hit provider capacity limits.
Report 04 was recovered from disk despite an empty terminal response. A report
is a lead, not an accepted defect or a benchmark result.

- Rejected Gemini 06's alleged single shared SRV descriptor race: the current
  presenter allocates `kFrameCount` descriptors and offsets both CPU and GPU
  handles by `backIndex` after frame-slot admission. Its proposed repair is
  already implemented. The same report's rotation-reset finding corroborates
  Astra, but merely re-enabling a flag would not preserve pending dependencies.
- Astra rechecked Gemini 02: missing frame-rate metadata really falls back to
  nominal 1/1 at the encoder, but explicit source timestamps limit the claimed
  playback-speed corruption. Treat as conditional P2, test final-sample duration
  and metadata transitions. Native format failure remains hardware-dependent.
  Stale startup profiling metadata is not evidence of live format corruption;
  missing GPU texture bounds checks are a separate hardening case.
- Gemini 05 independently corroborates recording-pool lifetime and padded
  readback issues. Its claimed exact CUDA destruction error needs runtime/API
  verification; the missing explicit mapped-array cleanup is the established
  ownership violation.
- Color metadata, NIS constant uploads, 1x EASU work, temporal history resets,
  and fMP4 recovery suggestions remain candidates. In particular, do not adopt
  a larger file-size deletion threshold for recordings: size alone does not
  prove a user capture is empty or safe to remove.

## Performance experiments

Measure these separately from fixes: coalesced camera/UI wakeups, explicit
viewport deadlines, nonblocking fallback staging, reusable CPU/NV12 buffers,
Maxine configuration invalidation, annotation raster caching, and bounded
double-buffered completed scenes. Record p50/p95 latency, allocation counts,
actual arrival/presentation rates, and image-quality deltas. GPU submission
timing is not sensor-to-photon latency. No percentage speedup is established
by these audits.
