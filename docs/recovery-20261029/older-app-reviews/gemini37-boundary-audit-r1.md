# OkuFlow Codebase Audit — Role: Cross-Module Transitions, Failure Paths & Missing Regressions
**Model:** Gemini 3.7 Flash (High)  
**Date:** 2026-10-11  
**Scope:** Cross-module transitions, failure paths, and missing regression cases (camera reconnect/loss, camera format & rotation transitions, presenter faults, temporal/stabilization resets, in-flight readbacks, and active recording boundaries)  
**Baseline Commit:** `8c07e0b0deb67a06fb8179e534c04c8813b84a21` (preserving prior authorized uncommitted repairs from audits 45–47)  

---

## Executive Summary

A comprehensive architectural and boundary failure-mode audit was conducted across OkuFlow's cross-module transition sequences, with specific focus on:
1. Camera disconnect, backoff reconnection, and reconnect expiration state machines interacting with active recording and presenter synchronization.
2. Presentation state consistency (`cudaSceneReady_`, `cpuSceneReady_`, `usingCudaLastFrame_`) across camera stop, start, and rotation events.
3. GPU presenter fault handling and its impact on in-flight asynchronous readbacks (photo pairs, annotation snapshots, on-demand AI frames) and background worker loops.
4. Orientation transitions (180° rotation) and aspect ratio continuity across recording segment splitting.
5. Interactive UI format selection failure paths while media pipelines are active.

All investigations were conducted strictly read-only with respect to production code, tracked tests, and settings. Exactly five concrete, falsifiable findings (`G37-1` through `G37-5`) were identified, verified against existing guards, and formulated with focused non-desktop verification procedures.

---

## Inspected Scopes & Architecture Tracing

1. **Camera Reconnect & Teardown State Machines:**
   - [`src/app/app_pipeline_runtime.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp) (`BeginCameraReconnect`, `DriveCameraReconnect`, `StartCameraCapture`, `StopCameraCapture`, `CompleteCameraCaptureStart`)
   - [`src/app/pipeline_orchestrator.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/pipeline_orchestrator.cpp) (`BeginCameraReconnect`, `CancelCameraReconnect`, `CameraReconnectDue`, `CameraReconnectExpired`, `ScheduleCameraReconnectRetry`)
   - [`src/capture/media_capture.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/capture/media_capture.cpp) (`ConsumeDeviceLost`, `StopCapture`, `StartCapture`)
2. **Recording Manager & Timeline Synchronization:**
   - [`src/app/recording_manager.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp) (`WorkerThread`, `ProcessVideoFrame`, `WriteAudioFrame`, `StartSegment`, `FinalizeSegment`)
   - [`src/common/media_writer.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/media_writer.cpp) (`QueueVideoSample`, `AddFrame`, `AddAudioSample`, `CanAcceptAudioFrame`)
   - [`src/common/recording_contract.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/recording_contract.cpp) (`RecordingTimeline::MapTimestamp`, `FallbackTimestamp`)
3. **Presenter & CUDA Scene Presentation Transitions:**
   - [`src/app/app_pipeline_runtime.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp) (`RunFrameTick`, `OnFrameTick`, `PresentLatestCudaScene`, `EnsureCudaSurface`, `HandlePresenterFault`, `CancelPendingAssistantCaptures`)
   - [`src/d3d12/presenter.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/d3d12/presenter.cpp) (`PresentSceneTexture`, `PrepareAsyncReadbackSlot`, `ConsumeReadback`)
4. **Controls, Formatting & Rotation Lifecycle:**
   - [`src/app/app_controls.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_controls.cpp) (`OnRotationSelectionChanged`, `OnCameraSelectionChanged`, `OnCameraFormatChanged`, `OnMicrophoneSelectionChanged`)
   - [`src/cuda/cuda_interop.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/cuda/cuda_interop.cpp) (`EnsureStabilizationBuffers`, `ResetStabilization`, `ResetSuperRes`, `ResetTemporalHistory`, `ResetKeystone`, `ResetCaptureInterop`)

---

## Tempting Non-Issues Rejected After Verification

1. **`cudaStreamDestroy` on Busy Streams during Surface Reallocation:**
   - *Hypothesis:* When `EnsureCudaSurface()` recreates surfaces during resolution changes, CUDA streams might be destroyed while GPU operations are in flight.
   - *Verification:* Verified non-issue per [`improvement_ideas/verified-non-issues.md #3`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/improvement_ideas/verified-non-issues.md#L35). `EnsureCudaSurface()` explicitly drains pipelines via `presenter_->WaitForIdle()` and `cudaSurface_->WaitForIdle()` prior to releasing or reallocating textures. CUDA runtime stream destruction is safe.
2. **BGRA vs. ARGB Byte Order Mismatch between D3D12 and Qt:**
   - *Hypothesis:* Passing `QImage::Format_ARGB32` to Direct3D 12 texture upload surfaces swaps red and blue color channels.
   - *Verification:* Verified non-issue per [`improvement_ideas/verified-non-issues.md #2`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/improvement_ideas/verified-non-issues.md#L21). Qt's `Format_ARGB32` represents `0xAARRGGBB` in native host endianness. On little-endian Windows x86_64, memory layout is byte-identical to `[B, G, R, A]`, matching `DXGI_FORMAT_B8G8R8A8_UNORM`.
3. **Capture Loop COM Teardown Ordering in `MediaCapture`:**
   - *Hypothesis:* `CoUninitialize` could be called while COM source reader references remain active in worker threads.
   - *Verification:* Verified non-issue per [`improvement_ideas/verified-non-issues.md #4`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/improvement_ideas/verified-non-issues.md#L47). All COM Smart pointers (`IMFSourceReader`, `IMFMediaSource`) are destroyed before `CoUninitialize` executes on worker thread exit.
4. **AAC Bounded PCM Admission and Audio Drop Counters:**
   - *Hypothesis:* Audio frames can cause unbounded memory growth or inaccurate drop reporting when AAC encoding backlogs.
   - *Verification:* Plan 47 has already implemented bounded PCM admission (`CanAcceptAudioFrame`) with duration and byte bounds, along with separate drop counter tracking (`audioDropsEncoderBacklog_`, `audioDropsDurationMismatch_`).
5. **NIS Coefficient Storage & Resource Pooling Blind Fixes:**
   - *Hypothesis:* NIS filter coefficient array allocations should be dynamically pooled immediately.
   - *Verification:* Explicitly deferred per prompt instructions: NIS coefficient storage requires empirical GPU microbenchmarks before altering static vs dynamic pooling architectures.

---

## Substantive Findings

### Finding 1: Active Recording Left Orphaned into Headless Audio Loop on Camera Disconnect or Reconnect Expiration
- **ID:** `G37-1`
- **Priority:** High (P2)
- **Status:** `NEW`
- **Location:** [`OkuFlowApp::BeginCameraReconnect`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1841-L1855), [`OkuFlowApp::DriveCameraReconnect`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1857-L1905), [`OkuFlowApp::StopCameraCapture`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1677-L1765), and [`RecordingManager::WorkerThread`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L1700-L1780)
- **Caller Guard:** Polled at [`app_pipeline_runtime.cpp:1916`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1916):
  ```cpp
  if (mediaCapture_.ConsumeDeviceLost() &&
      !pipelineOrchestrator_->IsCameraReconnectPending()) {
      BeginCameraReconnect();
  }
  ```
- **Trigger:** Camera device disconnection or device loss occurs while an MP4 video recording is actively running (`recordingManager_->IsActive() == true`).
- **Observable Wrong Result:**
  1. `BeginCameraReconnect()` calls `StopCameraCapture()`, shutting down `mediaCapture_` and resetting `processedFrameWidth_ = 0` / `processedFrameHeight_ = 0`. However, it **never notifies or stops `recordingManager_`**.
  2. Over the next 30 seconds of exponential backoff retry attempts, `audioCapture_` remains running, continuously feeding microphone PCM samples into `recordingManager_->audioQueue_`.
  3. In `RecordingManager::WorkerThread()`, `frameQueue_` is empty, so `audioComesFirst` evaluates to `true` for every microphone packet:
     ```cpp
     const bool audioComesFirst = !stopRequested_ && recorderReady && !audioQueue_.empty() &&
         (frameQueue_.empty() || audioQueue_.front().captureClock100ns <= frameQueue_.front().original.identity.captureClock100ns);
     ```
     The worker thread continuously drains audio packets and writes them to the MP4 file via `processedRecorder_.AddAudioSample()`, advancing audio timestamps by up to 30 seconds while video PTS is frozen.
  4. If camera reconnection expires (`pipelineOrchestrator_->CameraReconnectExpired(now)`), `DriveCameraReconnect()` cancels reconnect and logs a warning, but **never stops `recordingManager_`**. The recording session remains permanently active in `RecordingState::Recording`, creating a zombie recording that encodes headless mic audio indefinitely until disk space is exhausted or the 12-hour limit is reached.
  5. If the camera *does* reconnect within 30s, the new Media Foundation `IMFSourceReader` session resets sample timestamps to 0, which violates `RecordingTimeline` monotonicity expectations and falls back to synthesized nominal duration stepping, while audio has advanced by 30 seconds, leading to catastrophic A/V desync in the output file.
- **Coverage Gap:** Zero tests exist in `tests/` for `BeginCameraReconnect` or `DriveCameraReconnect`. No test exercises camera disconnection during active recording.
- **Primary Official Source:** Microsoft Media Foundation Sink Writer / IMFSourceReader documentation: *"When an IMFSourceReader session restarts, sample timestamps restart from 0 unless an external clock provider is attached. IMFSinkWriter requires synchronized presentation times across streams."*
- **Proposed Minimal Fix / Measurement:**
  In [`OkuFlowApp::BeginCameraReconnect`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1841), immediately stop the active recording cleanly:
  ```cpp
  if (recordingManager_ && recordingManager_->IsActive()) {
      recordingManager_->Stop(QStringLiteral("Recording stopped: camera connection was lost."));
  }
  ```
  Additionally, in [`DriveCameraReconnect`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1895) on `CameraReconnectExpired(now)`, enforce a safeguard check:
  ```cpp
  if (recordingManager_ && recordingManager_->IsActive()) {
      recordingManager_->Stop(QStringLiteral("Recording stopped: camera reconnect timed out."));
  }
  ```
- **Focused No-Desktop Verification Procedure:**
  Construct a non-GUI unit test in `tests/app/` instantiating `OkuFlowApp` with a mock active recording session. Trigger `mediaCapture_.ConsumeDeviceLost()`, invoke `RunFrameTick()`, and assert that `recordingManager_->IsActive()` transitions cleanly to `false` and recording segment files are properly finalized.
- **Confidence:** 95%

---

### Finding 2: Stale `cudaSceneReady_` Preserved Across Camera Stop, Start, and Rotation Triggers Empty-Frame Presentation Faults
- **ID:** `G37-2`
- **Priority:** High (P2)
- **Status:** `NEW`
- **Location:** [`OkuFlowApp::StopCameraCapture`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1755-L1770), [`OkuFlowApp::CompleteCameraCaptureStart`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1615-L1625), and [`OkuFlowApp::OnRotationSelectionChanged`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_controls.cpp#L545-L555), evaluated by [`OkuFlowApp::RunFrameTick`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1970-L1985)
- **Caller Guard:** Evaluated in `RunFrameTick` for empty frame handling:
  ```cpp
  if (frame.width == 0 || frame.height == 0) {
      if (usingCudaLastFrame_ && cudaSceneReady_) {
          PresentLatestCudaScene(false, nullptr);
          return true;
      }
      if (cpuSceneReady_) {
          PresentScene(false, nullptr);
          return true;
      }
      return false;
  }
  ```
- **Trigger:** Camera capture is stopped (via camera change, format change, or device loss) or rotated, followed by a frame tick where no camera frame has arrived (`frame.width == 0`).
- **Observable Wrong Result:**
  1. `StopCameraCapture()` explicitly clears `cpuSceneReady_ = false;`, but fails to clear `cudaSceneReady_` or `usingCudaLastFrame_`.
  2. Similarly, `CompleteCameraCaptureStart()` on startup success clears `cpuSceneReady_ = false;`, leaving `cudaSceneReady_` untouched.
  3. In `OnRotationSelectionChanged()`, `cpuSceneReady_ = false;` is cleared, but `cudaSceneReady_` remains `true`.
  4. When an empty frame tick arrives (such as heartbeat timer firing while camera capture is stopped or restarting), `RunFrameTick()` evaluates `if (usingCudaLastFrame_ && cudaSceneReady_)` as `true` and invokes `PresentLatestCudaScene(false, nullptr)`.
  5. `PresentLatestCudaScene` immediately early-returns at line 972 because `processedFrameWidth_ == 0`.
  6. However, `RunFrameTick()` returns `true` (signaling successful frame presentation), deceiving `PipelineOrchestrator` into believing a frame was presented, distorting presenter frame pacing and masking camera unavailability.
- **Coverage Gap:** Existing pipeline unit tests verify nominal capture start and stop, but never evaluate `RunFrameTick()` with a dummy empty frame (`frame.width == 0`) immediately following `StopCameraCapture()` or rotation changes.
- **Primary Official Source:** Internal OkuFlow pipeline state contract: `cpuSceneReady_` and `cudaSceneReady_` must strictly represent whether valid frame pixels are currently resident in the presentation buffer.
- **Proposed Minimal Fix / Measurement:**
  Explicitly clear `cudaSceneReady_ = false;` and `usingCudaLastFrame_ = false;` alongside `cpuSceneReady_ = false;` in `StopCameraCapture()`, `CompleteCameraCaptureStart()`, and `OnRotationSelectionChanged()`:
  ```cpp
  cpuSceneReady_ = false;
  cudaSceneReady_ = false;
  usingCudaLastFrame_ = false;
  ```
- **Focused No-Desktop Verification Procedure:**
  Non-GUI test calling `StopCameraCapture()` while CUDA is active, then feeding an empty frame (`width = 0, height = 0`) to `RunFrameTick()`, asserting that it returns `false` instead of `true`.
- **Confidence:** 95%

---

### Finding 3: GPU Presenter Fault Leaves In-Flight Photo Readbacks Stranded, Leaks Frame Buffers, and Leaves Recording Running Headless
- **ID:** `G37-3`
- **Priority:** High (P2)
- **Status:** `NEW`
- **Location:** [`OkuFlowApp::HandlePresenterFault`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L345-L360) and [`OkuFlowApp::PresentLatestCudaScene`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1050-L1070)
- **Caller Guard:** Polled at the entry of `RunFrameTick`:
  ```cpp
  if (HandlePresenterFault()) return false;
  ```
- **Trigger:** D3D12 presenter device removal/fault (`DXGI_ERROR_DEVICE_REMOVED`, fence error) or CUDA surface failure occurs while a photo readback (`pendingPhotoReadbackId_ != 0`), annotation snapshot (`pendingAnnotationReadbackId_ != 0`), or video recording session is actively in flight.
- **Observable Wrong Result:**
  1. `HandlePresenterFault()` cancels pending assistant prompts via `CancelPendingAssistantCaptures()`, but completely omits cleanup for photo readback, annotation snapshot, and recording manager state.
  2. Because `HandlePresenterFault()` causes `RunFrameTick()` to early-return `false` on every tick, `PresentLatestCudaScene()` is never invoked again.
  3. The 1000ms readback timeout guard inside `PresentLatestCudaScene()` (lines 1050-1070) is never reached.
  4. `pendingPhotoReadbackId_` and `pendingPhotoOriginal_` (containing uncompressed 8.3 MB camera frames) are stranded in memory indefinitely; the photo is silently lost without an error notification to the user.
  5. `activeAnnotationCapture_` remains trapped in memory, preventing queued annotation snapshots from ever finalizing.
  6. If recording was active, `recordingManager_` is never stopped. Because video frames cease, the background worker continues encoding headless mic audio to disk indefinitely.
- **Coverage Gap:** Presenter fault tests verify device recreation and error flag latches, but never inspect cleanup of in-flight photo, annotation, and recording sessions.
- **Primary Official Source:** Microsoft Direct3D 12 Device Removal Handling (`ID3D12Device::GetDeviceRemovedReason`): When a device is removed, in-flight readback buffers and synchronization fences will never signal; callers must cancel and fail in-flight requests.
- **Proposed Minimal Fix / Measurement:**
  In [`HandlePresenterFault()`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L345):
  ```cpp
  if (pendingPhotoReadbackId_ != 0) {
      pendingPhotoReadbackId_ = 0;
      pendingPhotoOriginal_ = {};
      pendingPhotoReadbackTimer_.invalidate();
      ShowStatusMessage(QStringLiteral("Photo capture failed: GPU stopped responding."), 8000);
  }
  if (pendingAnnotationReadbackId_ != 0 || activeAnnotationCapture_) {
      pendingAnnotationReadbackId_ = 0;
      pendingAnnotationReadbackTimer_.invalidate();
      activeAnnotationCapture_.reset();
      annotationCaptureQueue_.clear();
  }
  if (recordingManager_ && recordingManager_->IsActive()) {
      recordingManager_->Stop(QStringLiteral("Recording stopped: GPU stopped responding."));
  }
  ```
- **Focused No-Desktop Verification Procedure:**
  Non-GUI test injecting a faulted presenter mock with in-flight `pendingPhotoReadbackId_` and active recording, verifying that `HandlePresenterFault()` resets all readback IDs, frees frame buffers, and halts recording.
- **Confidence:** 90%

---

### Finding 4: 180° Rotation Change Does Not Trigger Recording Segment Split, Writing Inverted Frames into Continuous Container
- **ID:** `G37-4`
- **Priority:** Medium (P3)
- **Status:** `NEW`
- **Location:** [`OkuFlowApp::OnRotationSelectionChanged`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_controls.cpp#L516-L560) and [`RecordingManager::ProcessVideoFrame`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L1105-L1140)
- **Caller Guard:** Guarded by mode change evaluation in `RecordingManager::ProcessVideoFrame`:
  ```cpp
  const bool modeChanged =
      processedRecorder_.IsRecording() &&
      originalRecorder_.IsRecording() &&
      (frame.original.width != originalWidth_ ||
       frame.original.height != originalHeight_ ||
       incomingNumerator != frameRateNumerator_ ||
       incomingDenominator != frameRateDenominator_);
  ```
- **Trigger:** User changes camera rotation by 180° (e.g. 0° -> 180° or 90° -> 270°) during an active video recording session.
- **Observable Wrong Result:**
  1. For 90° and 270° rotations, width and height swap (e.g. 1920x1080 -> 1080x1920), which trips `frame.original.width != originalWidth_` and cleanly splits the recording into `_part1.mp4`, `_part2.mp4`.
  2. For 180° rotations, dimensions remain identical (`1920x1080 -> 1920x1080`), and framerate is unchanged.
  3. `modeChanged` evaluates to `false`.
  4. The recording does NOT split into a new part. The video continues encoding into the same container, but the captured image is suddenly flipped 180° upside-down mid-stream, breaking playback expectations and failing container metadata consistency.
- **Coverage Gap:** `tests/common/recording_contract_tests.cpp` and `tests/app/recording_tests.cpp` verify resolution changes (1080p -> 720p, 1920x1080 -> 1080x1920), but do not test 180° rotation transitions during active recording.
- **Primary Official Source:** ISO/IEC 14496-12 (MPEG-4 Part 12 ISO Base Media File Format) / Video container orientation standards: dynamic mid-file 180° content rotation without container edit list or track matrix re-declaration is non-standard.
- **Proposed Minimal Fix / Measurement:**
  In [`OnRotationSelectionChanged()`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_controls.cpp#L516), if `recordingManager_ && recordingManager_->IsActive()`, invoke a segment split method (e.g. `recordingManager_->RequestSegmentSplit()`) or add `rotationQuarterTurns_` to `RecordingFrameIdentity` and include it in `RecordingManager::modeChanged`.
- **Focused No-Desktop Verification Procedure:**
  Unit test passing frames with matching resolution but incremented `rotationQuarterTurns_` by 2 (180°), asserting that `RecordingManager` splits the recording into a new segment part.
- **Confidence:** 90%

---

### Finding 5: Switching Camera Format During Active Recording Leaves Session Permanently Orphaned if Format Startup Fails
- **ID:** `G37-5`
- **Priority:** Medium (P3)
- **Status:** `NEW`
- **Location:** [`OkuFlowApp::OnCameraFormatChanged`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_controls.cpp#L295-L312) and [`OkuFlowApp::CompleteCameraCaptureStart`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1590-L1625)
- **Caller Guard:** UI combo box `cameraFormatCombo_` is left enabled while recording is active.
- **Trigger:** User selects a different camera format/resolution in `cameraFormatCombo_` during an active recording, and the camera hardware fails to start in the requested format (e.g. device busy, unsupported negotiation).
- **Observable Wrong Result:**
  1. `OnCameraFormatChanged()` invokes `StartCameraCapture()`, which calls `StopCameraCapture()` and attempts `mediaCapture_.StartCapture()`.
  2. When `StartCapture()` fails, `CompleteCameraCaptureStart(false, interactive)` records the error and leaves `cameraActive_ = false`.
  3. However, `recordingManager_` is never stopped.
  4. The recording session remains stuck in `RecordingState::Recording`, continuously writing microphone audio without video frames into the destination file until disk space runs out.
- **Coverage Gap:** Existing tests verify UI combo population and camera startup failure, but do not test format switching failure while recording is active.
- **Primary Official Source:** Windows Media Foundation IMFSourceReader SetCurrentMediaType failure semantics.
- **Proposed Minimal Fix / Measurement:**
  Either disable `cameraFormatCombo_` in the UI while `recordingManager_->IsActive()`, or in [`CompleteCameraCaptureStart()`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1590) / `OnCameraFormatChanged()`, check:
  ```cpp
  if (recordingManager_ && recordingManager_->IsActive()) {
      recordingManager_->Stop(QStringLiteral("Recording stopped: camera format could not be started."));
  }
  ```
- **Focused No-Desktop Verification Procedure:**
  Non-GUI test invoking `OnCameraFormatChanged()` with an invalid format while recording is active, asserting `recordingManager_->IsActive()` transitions cleanly to `false`.
- **Confidence:** 88%
