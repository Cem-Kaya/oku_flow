# OkuFlow Codebase Audit — Role B: App Wiring, Recording & Accessibility
**Model:** Gemini 3.1 Pro (High)  
**Date:** 2026-10-11  
**Scope:** Role B (App wiring/settings persistence, recording pipeline, audio clocks/transforms, assistant/Codex stream/history/temp cleanup, transcription, UI/settings ownership and accessibility)  
**Baseline Commit:** `8c07e0b0deb67a06fb8179e534c04c8813b84a21` (with prior authorized uncommitted repairs from audits 45–47 preserved)  

---

## Executive Summary

A comprehensive architectural and static code audit was conducted across OkuFlow's application orchestration, media recording engine, vision assistant / Codex integration, and UI accessibility layers.

All analyses were performed in strict read-only mode without desktop UI automation, camera access, or background build interruptions. Exactly five substantive findings were identified, each with a complete contract definition, triggering sequence, observable failure mode, and verification path without desktop automation.

---

## Inspected Scopes & Architecture Tracing

1. **Recording Pipeline & Audio Clocks:**
   - [src/app/recording_manager.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp) & [include/okuflow/app/recording_manager.hpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/include/okuflow/app/recording_manager.hpp)
   - [src/common/media_writer.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/media_writer.cpp) & [include/okuflow/common/media_writer.hpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/include/okuflow/common/media_writer.hpp)
   - [src/capture/audio_capture.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/capture/audio_capture.cpp)
   - [src/common/recording_contract.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/recording_contract.cpp)
2. **Vision Assistant & Codex Lifecycle:**
   - [src/common/assistive_runtime.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/assistive_runtime.cpp)
   - [src/common/codex_app_server_client.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/codex_app_server_client.cpp)
   - [src/app/transcription_session_controller.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/transcription_session_controller.cpp)
3. **Settings Persistence & Secret Storage:**
   - [src/app/settings_controller.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/settings_controller.cpp)
   - [src/app/settings_store.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/settings_store.cpp)
   - [src/app/protected_secret_store.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/protected_secret_store.cpp)
4. **UI Accessibility & Settings Presentation:**
   - [src/ui/ai_settings_dialog.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/ui/ai_settings_dialog.cpp)
   - [src/ui/live_status_text.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/ui/live_status_text.cpp)
   - [src/ui/main_window.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/ui/main_window.cpp)
   - [src/ui/annotation_overlay.cpp](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/ui/annotation_overlay.cpp)

---

## Tempting Non-Issues Rejected After Verification

1. **Credential Removal Idempotence in `SettingsController::Save`:**
   - *Hypothesis:* When multiple pending credential removals exist in `pendingCredentialRemovals_` and an error occurs mid-loop, previously removed credentials remain in the list, potentially causing errors on subsequent `removeSecret` calls.
   - *Verification:* [src/app/protected_secret_store.cpp:141](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/protected_secret_store.cpp#L141) reveals that `ProtectedSecretStore::RemoveSecret` explicitly checks `GetLastError() == ERROR_NOT_FOUND` and treats it as success (`return true;`). Deleting an already removed secret is fully idempotent.
2. **Keyboard Tab Order Navigation in `AiSettingsDialog`:**
   - *Hypothesis:* The `tabOrder` vector contains controls from both Codex and OpenAI-compatible sections consecutively, which could cause keyboard focus to jump unexpectedly.
   - *Verification:* In Qt, widgets in hidden containers (`codexGroup_` vs `serverGroup_`) are automatically skipped during tab navigation. Focus moves correctly and sequentially according to visible UI state.
3. **Event Loop Recursion in `AnnotationOverlay::ForwardKeyToOwner`:**
   - *Hypothesis:* Calling `QCoreApplication::sendEvent(owner, &forwarded)` from `AnnotationOverlay::keyPressEvent` might re-trigger the event filter and cause infinite recursion.
   - *Verification:* The forwarded event is dispatched directly to `parentWidget()` and filtered based on `forwarded.isAccepted()`; the event filter specifically avoids intercepting re-dispatched events.
4. **Leaked COM Pointers in `RecordingManager::ShutdownForProcessExit`:**
   - *Hypothesis:* Intentionally abandoning the recording worker and leaking `VideoRecorder` COM pointers could be flagged as a memory/resource leak.
   - *Verification:* This is an explicit, verified safety contract to protect against deadlocks and driver-level access violations during process shutdown when hardware MFT drivers wedge.
5. **AAC / MP4 Playback Gap Collapse:**
   - *Hypothesis:* Playback gaps between audio segments in MP4 files.
   - *Verification:* Recognized as a known, documented open follow-up with strict opt-in test reproducer; not relabeled as new.

---

## Substantive Findings

### Finding 1: Audio Sample Clock Monotonicity & Overlap Risk in `AddAudioFrame`
- **ID:** `REC-AUD-01`
- **Location:** [`VideoRecorder::AddAudioFrame`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/media_writer.cpp#L1159), [`AudioCapture::CaptureLoop`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/capture/audio_capture.cpp#L444), [`RecordingManager::WriteAudioFrame`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L1299)
- **Status:** `NEW`
- **Severity:** Medium-High
- **Confidence:** High
- **Primary Contract:** Continuous, strictly monotonic audio sample presentation timestamps without out-of-order rejection by Media Foundation Sink Writer.
- **Triggering Sequence / Inputs:**
  1. Windows audio capture runs on the WASAPI capture thread (`AudioCapture::CaptureLoop`).
  2. Successive audio buffers are read; when buffer delivery experiences DPC latency or slight thread scheduling jitter, multiple buffers can be dequeued in rapid succession.
  3. `AudioCapture` calculates `frame.captureClock100ns = QueryClock100ns() - frame.duration100ns`.
  4. If iteration $N+1$ executes shortly after iteration $N$, `QueryClock100ns()` for iteration $N+1$ does not advance by a full buffer duration, producing $T_{N+1} < T_N + D_N$ (overlapping timestamps) or even $T_{N+1} \le T_N$.
  5. In `RecordingManager::WriteAudioFrame`, the sample time is directly computed as `sampleTime100ns = frame.captureClock100ns - segmentClockOrigin100ns_` without passing through a timeline or monotonic clamp (unlike video, which uses `RecordingTimeline::MapTimestamp`).
  6. `VideoRecorder::AddAudioFrame` passes this directly to `sinkWriter_->WriteSample(audioStreamIndex_, sample.Get())`.
- **Observable Effect:** Media Foundation Sink Writer's AAC stream sink rejects non-monotonic or overlapping samples with `MF_E_SAMPLES_OUT_OF_ORDER` (`0xC00D4A38`) or `E_INVALIDARG`. `AddAudioFrame` catches the exception, returns `false`, increments `encoderReject_`, and calls `FinishSession(false, ...)`, prematurely aborting the entire recording session.
- **Why Existing Safeguards Miss It:** Existing safeguards in `RecordingAudioBacklog` track cumulative PCM byte budgets and frame limits, but do not enforce monotonicity on `sampleTime100ns`. `RecordingTimeline` is instantiated only for the video stream.
- **Proposed Minimal Repair:** In `VideoRecorder::AddAudioFrame` (or within an audio timeline tracker), enforce that `sampleTime100ns` cannot precede `lastAudioTimestamp_ + lastAudioDuration_` (e.g. `sampleTime100ns = std::max(sampleTime100ns, lastAudioEnd100ns_)`).
- **Verification / Reproducer (Non-GUI):** Unit test calling `VideoRecorder::AddAudioFrame` with synthetic PCM blocks where $T_2 < T_1 + D_1$ and asserting whether `sinkWriter_->WriteSample` accepts or rejects the second buffer.

---

### Finding 2: Silent Audio Loss & Missing Drop Counter on Mid-Session Mode Change
- **ID:** `REC-AUD-02`
- **Location:** [`RecordingManager::WriteAudioFrame`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L1290-L1293), [`RecordingManager::WriteFrame`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L1080)
- **Status:** `NEW`
- **Severity:** Medium
- **Confidence:** High
- **Primary Contract:** Accurate drop accounting and audio frame conservation across mode-change segment boundaries.
- **Triggering Sequence / Inputs:**
  1. Mid-session camera resolution, aspect ratio, or framerate change occurs while recording.
  2. `RecordingManager::WriteFrame` detects `modeChanged`, invokes `FinalizeSegment(false)`, increments `segmentIndex_`, and invokes `StartSegment(frame)`.
  3. `StartSegment` resets `segmentClockOrigin100ns_ = firstFrame.original.identity.captureClock100ns`.
  4. Audio frames that were captured between the old segment's end and the new video frame arrive in `audioQueue_`. Their timestamps satisfy `frame.captureClock100ns < segmentClockOrigin100ns_`.
  5. In `WriteAudioFrame`:
     ```cpp
     if (!audioCaptureEnabled_ || segmentClockOrigin100ns_ < 0 ||
         frame.captureClock100ns < segmentClockOrigin100ns_) {
         return true;
     }
     ```
- **Observable Effect:** Audio frames falling in the transition gap are silently discarded with `return true;`. Neither `audioQueueOverflow_` nor `audioEncoderBacklog_` is incremented. At the end of the session, `BuildDropSummary()` reports `"No frames were dropped."` despite permanent loss of audio at the segment boundary.
- **Why Existing Safeguards Miss It:** Drop accounting only checks queue capacity overflow on admission (`AddAudioFrame`) and buffer backlog rejection (`CanAcceptAudioFrame`). Pruning before `segmentClockOrigin100ns_` has no accounting branch.
- **Proposed Minimal Repair:** Either clamp audio arriving within a bounded window to $t = 0$ for the new segment, or increment a dedicated counter (e.g., `audioPrunedBeforeOrigin_`) and include it in `RecordingDropCounts` and `BuildDropSummary()`.
- **Verification / Reproducer (Non-GUI):** Non-GUI test queueing audio frames immediately before and after a triggered `StartSegment` and asserting whether all audio frames are accounted for in drop counters or successfully written.

---

### Finding 3: Mislabeled Stop Reason (`StopReason::DiskFull`) on Pending Sample Write Failure
- **ID:** `REC-AUD-03`
- **Location:** [`VideoRecorder::QueueVideoSample`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/media_writer.cpp#L1310-L1313)
- **Status:** `NEW`
- **Severity:** Low-Medium
- **Confidence:** Very High
- **Primary Contract:** Truthful diagnostic classification of recording termination reasons.
- **Triggering Sequence / Inputs:**
  1. `QueueVideoSample` is called for a new video sample. Disk space is checked at line 1303 (`CheckDiskSpace()`) and succeeds.
  2. Line 1310 evaluates:
     ```cpp
     if (pendingSample_ &&
         !WritePendingSample(sampleTime - pendingSampleTime100ns_)) {
         FinalizeAndStop(StopReason::DiskFull);
         return false;
     }
     ```
  3. `WritePendingSample` fails due to an encoder failure, hardware MFT device removal, or buffer write fault.
- **Observable Effect:** `FinalizeAndStop(StopReason::DiskFull)` is executed, setting `stopReason_ = StopReason::DiskFull`. Diagnostic logs and UI state report a false "Disk Full" error when the disk was not full. In contrast, lines 1367 and 1662 for the identical write failure in `AddFrame` and `AddGpuFrame` correctly invoke `FinalizeAndStop(StopReason::WriteFailed)`.
- **Why Existing Safeguards Miss It:** Existing tests mock `WritePendingSample` success during standard flush tests; failure paths in `QueueVideoSample` were not tested for the exact `StopReason` enum value.
- **Proposed Minimal Repair:** Change `FinalizeAndStop(StopReason::DiskFull)` on line 1311 to `FinalizeAndStop(StopReason::WriteFailed)`.
- **Verification / Reproducer (Non-GUI):** Code inspection and unit test injecting a mock write failure into `WritePendingSample` and asserting `recorder.StopReason() == StopReason::WriteFailed`.

---

### Finding 4: Dead and Vestigial Async Readback Path in `RecordingManager`
- **ID:** `REC-AUD-04`
- **Location:** [`RecordingManager::StorePendingOriginal`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L714), [`RecordingManager::HandleProcessedReadback`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L729), [`OkuFlowApp::RenderLoop`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1354)
- **Status:** `NEW`
- **Severity:** Low
- **Confidence:** Very High
- **Primary Contract:** Minimal synchronization overhead and clean architectural decoupling in the high-frequency render loop.
- **Triggering Sequence / Inputs:**
  1. Primary pipeline render loop executes and completes an asynchronous texture readback.
  2. Line 1354 of `app_pipeline_runtime.cpp` calls `recordingManager_->HandleProcessedReadback(requestId, ...)`.
  3. `HandleProcessedReadback` acquires `pendingMutex_` and checks `pendingOriginalReadbacks_`.
- **Observable Effect:** `RecordingManager::StorePendingOriginal` is completely uncalled anywhere across the entire codebase. The `pendingOriginalReadbacks_` map is perpetually empty. `HandleProcessedReadback` unconditionally returns `false` while acquiring and releasing a mutex on every readback. Furthermore, this vestigial code creates architectural confusion regarding whether CPU recording utilizes readback pairing (CPU recording actually routes through `AddSceneFrame` in `PresentViewport`).
- **Why Existing Safeguards Miss It:** The empty lookup returns `false` benignly, so no functional failure occurs; however, it wastes cycles in the primary rendering loop.
- **Proposed Minimal Repair:** Deprecate or remove `StorePendingOriginal` and `HandleProcessedReadback` from `RecordingManager`, and remove the unused callback invocation in `app_pipeline_runtime.cpp`.
- **Verification / Reproducer (Non-GUI):** Codebase symbol search confirms zero invocations of `StorePendingOriginal`.

---

### Finding 5: Inconsistent Drop Summary Phrasing When Video Drops are Zero But Audio Drops Occur
- **ID:** `REC-AUD-05`
- **Location:** [`RecordingManager::BuildDropSummary`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L1515-L1525)
- **Status:** `NEW`
- **Severity:** Low
- **Confidence:** Very High
- **Primary Contract:** Clear, grammatically consistent assistive status messaging.
- **Triggering Sequence / Inputs:**
  1. A recording session finishes where no video frames were dropped (`drops.Total() == 0`), but audio blocks were dropped due to queue overflow or encoder backlog (`drops.audioQueueOverflow > 0` or `drops.audioEncoderBacklog > 0`).
  2. Line 1515 sets:
     ```cpp
     QString summary = drops.Total() == 0
         ? QStringLiteral("No frames were dropped.")
         : QStringLiteral("%1 frames were dropped (%2).").arg(...);
     ```
  3. Line 1521 appends:
     ```cpp
     if (drops.audioQueueOverflow > 0 || drops.audioEncoderBacklog > 0) {
         summary += QStringLiteral(" Audio: %1 blocks dropped before encoding, %2 rejected by the input limit.")
             .arg(drops.audioQueueOverflow).arg(drops.audioEncoderBacklog);
     }
     ```
- **Observable Effect:** The user-facing status message becomes:
  `"Recording saved. No frames were dropped. Audio: 3 blocks dropped before encoding, 0 rejected by the input limit."`
  The first sentence directly contradicts the second sentence, causing confusion for assistive screen-reader users and test automation.
- **Why Existing Safeguards Miss It:** Existing drop tests verify the presence of audio drop substrings, but did not assert the absence of the "No frames were dropped" prefix in the compound audio-drop case.
- **Proposed Minimal Repair:** Adjust the prefix to `"No video frames were dropped."` when `drops.Total() == 0` and audio drops are non-zero, or construct a consolidated summary sentence.
- **Verification / Reproducer (Non-GUI):** Non-GUI unit test calling `BuildDropSummary()` with `totalVideoDrops = 0` and `audioQueueOverflow = 2` and checking for internal consistency.
