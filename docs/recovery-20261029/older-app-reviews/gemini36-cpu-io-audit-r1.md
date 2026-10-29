# OkuFlow Codebase Audit — Role D: CPU Work, Allocations/Copies, File I/O & Bounded-Memory Quality
**Model:** Gemini 3.6 Flash (High)  
**Date:** 2026-10-11  
**Scope:** Role D (CPU processing helpers, CPU/GPU fallback boundaries, per-frame image/annotation conversions, app/UI repeated text/layout work, notes/recovery/temp/file scanning, and parser buffer limits)  
**Baseline Commit:** `8c07e0b0deb67a06fb8179e534c04c8813b84a21` (preserving prior authorized uncommitted repairs from audits 45–47)  

---

## Executive Summary

A comprehensive architectural, memory-lifecycle, and static code audit was conducted across OkuFlow's CPU processing pipeline, fallback paths, annotation rendering, file I/O transactions, and parser safety bounds.

All investigations were performed in strict read-only mode without desktop UI automation, camera access, background build execution, or git modifications. Exactly five substantive findings (`G36-1` through `G36-5`) were identified, each verified against actual caller reachability, existing guards, and previous audit checkpoints.

---

## Inspected Scopes & Architecture Tracing

1. **CPU Frame Pipeline & Helpers:**
   - [`src/common/frame_pipeline.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/frame_pipeline.cpp) & [`include/okuflow/common/frame_pipeline.hpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/include/okuflow/common/frame_pipeline.hpp)
   - [`src/common/image_processing.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/image_processing.cpp) & [`include/okuflow/common/image_processing.hpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/include/okuflow/common/image_processing.hpp)
   - [`src/common/recording_contract.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/recording_contract.cpp)
2. **CPU/GPU Fallback Boundaries & Presentation:**
   - [`src/app/app_pipeline_runtime.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp) (`OnFrameTick`, `BuildCompositeAndPresent`, `PresentLatestCudaScene`, `PresentFitted`, `PrepareOriginalFrame`)
   - [`src/d3d12/presenter.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/d3d12/presenter.cpp) (`RequestRecordingFrame`)
   - [`src/app/recording_manager.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp) (`AddSceneFrame`, `WorkerLoop`, `ComposeRecordingAnnotations`)
   - [`src/common/media_writer.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/media_writer.cpp) (`AddFrame`, `ConvertRecordingBgraToNv12`)
3. **Annotation Model & Vector Conversions:**
   - [`src/common/annotation_model.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/annotation_model.cpp) (`RenderAnnotationStrokes`, `BuildStrokePath`)
   - [`src/ui/annotation_overlay.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/ui/annotation_overlay.cpp)
4. **Notes, Recovery, Settings & File I/O:**
   - [`src/common/assistive_runtime.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/assistive_runtime.cpp) (`AppendNoteBlock`, `NotesWorkState`, `SweepAssistiveTemporaryFrames`)
   - [`src/app/user_data_paths.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/user_data_paths.cpp) (`WritePhotoPair`, `RecoverInterruptedPhotoPairs`)
   - [`src/app/settings_store.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/settings_store.cpp) (`LoadDetailed`, `SaveDetailed`)
   - [`src/common/codex_json_rpc_process.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/codex_json_rpc_process.cpp) (`ConsumeStdout`)
   - [`src/ui/live_status_text.cpp`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/ui/live_status_text.cpp)

---

## Tempting Non-Issues Rejected After Verification

1. **Gaussian Blur is Naive 2D / Non-Separable:**
   - *Hypothesis:* `ApplyGaussianBlur` in [`src/common/image_processing.cpp:320-385`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/image_processing.cpp#L320-L385) performs an $O(R^2)$ full 2D convolution kernel per pixel.
   - *Verification:* The implementation is explicitly separable and split into two consecutive 1D passes: a horizontal pass writing into `scratch`, followed by a vertical pass reading `scratch` and writing to `dst`. Time complexity is strictly $O(R)$ per pixel. Claiming a non-separable 2D filter is factually refuted.
2. **`noteImages_` Map Scan in `AppendNoteBlock` is Unbounded / Quadratic:**
   - *Hypothesis:* [`AssistiveRuntime::AppendNoteBlock`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/assistive_runtime.cpp#L2116) scans all keys in `noteImages_` and computes relative URLs for each note block appended, creating quadratic filesystem work as notes accumulate.
   - *Verification:* In [`AssistiveRuntime::SaveAnalyzedImageForNotes`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/assistive_runtime.cpp#L1946), `noteImages_` is strictly capped at `noteImages_.size() >= 3` or 192 MB total retained payload. The loop processes at most 3 in-memory entries and erases matched images immediately. Cost is $O(1)$ and negligible.
3. **`QImage::Format_ARGB32` Used for BGRA Pixels Swaps Red and Blue Channels:**
   - *Hypothesis:* Wrapping camera BGRA memory in `QImage::Format_ARGB32` in `recording_manager.cpp`, `app_pipeline_runtime.cpp`, or `user_data_paths.cpp` swaps colors.
   - *Verification:* Confirmed by [`improvement_ideas/verified-non-issues.md #2`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/improvement_ideas/verified-non-issues.md#L21). Qt's `Format_ARGB32` represents `0xAARRGGBB` in native host endianness. On little-endian Windows x86_64, memory layout is byte-identical to `[B, G, R, A]`. No channel swizzling occurs.
4. **Codex JSON-RPC Child Standard Output Buffer is Unbounded:**
   - *Hypothesis:* `CodexJsonRpcProcess::ConsumeStdout` buffers arbitrary standard output into `stdoutBuffer_` without limits if a subprocess emits text without newlines.
   - *Verification:* Line 208 explicitly guards `stdoutBuffer_.size() > kMaximumProtocolBufferBytes` (16 MiB), and line 178 checks `newline > kMaximumProtocolMessageBytes` (8 MiB). Both enforce hard failure before memory exhaustion.

---

## Substantive Findings

### Finding 1: Per-Frame Ink Layer Allocation Churn & Redundant Re-Rasterization in `PresentLatestCudaScene`
- **ID:** `G36-1`
- **Priority:** High
- **Status:** `NEW`
- **Location:** [`OkuFlowApp::PresentLatestCudaScene`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L1190-L1215)
- **Caller:** `OkuFlowApp::BuildCompositeAndPresent` at [src/app/app_pipeline_runtime.cpp:2146](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L2146), invoked every camera tick (`OnFrameTick`, 30/60 fps) when CUDA rendering is active.
- **Trigger:** Active recording (`recordingActive == true`) while ink annotations are present on screen (`annotationOverlay && annotationOverlay->HasInk()`).
- **Wrong Output / Avoidable Work & Scale:**
  1. On every single frame presented to the screen (30 or 60 times per second), `annotationLayer = QImage(canvas.width, canvas.height, QImage::Format_ARGB32);` allocates a new full-frame heap buffer (8.3 MB at 1080p: $1920 \times 1080 \times 4$ bytes; 33.2 MB at 4K).
  2. `annotationLayer.fill(Qt::transparent);` unconditionally zeroes all 8.3 MB on the CPU every frame.
  3. `RenderAnnotationStrokes(...)` executes full vector path generation (`BuildStrokePath`, `addText`, `QFontMetricsF`, antialiased QPainter path stroking) on the main thread for every stroke from scratch, even when ink is completely static and `annotationTransform` has not changed (typical state during lectures or reading).
  4. The freshly allocated `QImage` is passed to `D3D12Presenter::RequestRecordingFrame`, copied row-by-row into mapped upload memory, and then deallocated at block exit.
  5. **Scale:** At 60 fps at 1080p, this causes ~500 MB/s of continuous heap allocation/deallocation churn and ~1 GB/s of CPU memory bandwidth (memset + rasterize + memcpy) on the presentation thread right while encoding video.
- **Why Guards / Tests Miss It:**
  - Automated tests run without annotations or test recording without active ink.
  - Functional tests pass because the final video frame visual is correct; the severe CPU and allocation churn is a silent performance regression on the presentation loop.
- **Proposed Small Repair or Benchmark:**
  - Retain a reusable `QImage retainedAnnotationLayer_` member in `OkuFlowApp` resized only when `canvas` dimensions change, avoiding heap churn.
  - Track a monotonic `inkRevision_` on `AnnotationModel` (incremented on any stroke addition, erasure, or transformation). Cache the rendered layer and compare `(inkRevision_, annotationTransform)` with the cached state; when unchanged, skip `fill()` and `RenderAnnotationStrokes()` entirely, reusing the retained raster.
- **No-Desktop Verification Steps:**
  - Headless benchmark: Run 300 iterations of `PresentLatestCudaScene` with 3 active strokes and measure allocation count and CPU time with vs without retained raster caching.
- **Confidence & Limitations:**
  - Confidence: Very High.
  - Limitations: When the user is actively drawing new strokes at 60 fps, rasterization must still run, but heap reallocation churn is still eliminated by buffer retention.

---

### Finding 2: CPU Fallback Resampling Overhead & Redundant Full-Frame Zero-Fill
- **ID:** `G36-2`
- **Priority:** Medium-High
- **Status:** `REFINEMENT` (Refines P7 by identifying inner-loop floating-point clamping/rounding and redundant full-frame memset)
- **Location:** [`OkuFlowApp::PresentFitted`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L2268-L2298) and [`ResampleRecordingCanvas`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/recording_contract.cpp#L154-L235)
- **Caller:** `OkuFlowApp::BuildCompositeAndPresent` at [src/app/app_pipeline_runtime.cpp:2178](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L2178) on CPU presentation fallback, and `RecordingManager::WorkerLoop` at [src/app/recording_manager.cpp:1200](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/recording_manager.cpp#L1200) for CPU canvas resampling.
- **Trigger:** Running without CUDA (CPU fallback presentation) or recording on CPU fallback / format fallback.
- **Wrong Output / Avoidable Work & Scale:**
  1. In `PresentFitted`: `presentationBuffer_.assign(targetWidth * targetHeight * 4, 0);` zero-fills 8.3 MB per frame, even though the active viewport rectangle is immediately overwritten pixel-by-pixel.
  2. Inside the inner loop of `PresentFitted`: For every pixel `(x, y)`, it recalculates `sampleX = mapping.startX + x * mapping.stepX`, `std::lroundf(sampleX)`, and `std::clamp(...)`. Because column mapping does not vary with row `y`, `lroundf` and `clamp` are executed $1920 \times 1080 = 2,073,600$ times per frame instead of being precomputed once into an array of 1920 integers outside the `y` loop.
  3. In `ResampleRecordingCanvas`: Similarly, `destination.assign(...)` zeroes 8.3 MB unconditionally, and $x$-coordinates (`sourceSampleX`, `floor`, `clamp`, `fractionX`) are recomputed for every pixel in every row (2,073,600 times) rather than precalculated once for the `activeWidth` row span. Furthermore, the channel loop calls `std::lround` 4 times per pixel (8.3 million calls per frame).
  4. **Scale:** Wasted memory bandwidth (~8.3 MB zero-fill per frame) and ~10 million redundant floating-point roundings/clamps per frame, causing avoidable frame drops on systems without discrete GPUs.
- **Why Guards / Tests Miss It:**
  - Correctness tests only assert that the output pixels match expected mathematical resampling; they do not benchmark instruction count or floating-point call rates.
- **Proposed Small Repair or Benchmark:**
  - In `PresentFitted`, precompute `std::vector<int> srcXLookup(mapping.activeWidth)` before the `y` loop; copy 32-bit words `*reinterpret_cast<uint32_t*>(dstPixel) = *reinterpret_cast<const uint32_t*>(srcPixel)`; only clear letterbox/pillarbox margins when aspect ratios do not match 1:1.
  - In `ResampleRecordingCanvas`, precalculate a 1D column LUT of `(sourceX0, sourceX1, fractionX)` for the row width; only zero margins if active area is smaller than target area.
- **No-Desktop Verification Steps:**
  - Benchmark: CTest headless benchmark comparing execution time of `PresentFitted` and `ResampleRecordingCanvas` on 1080p synthetic buffers before and after LUT precomputation.
- **Confidence & Limitations:**
  - Confidence: High.
  - Limitations: On systems with powerful modern multi-core CPUs, the redundant instructions complete within budget, but on constrained CPU fallback systems (where CPU budget is tightest), this directly impacts frame rate.

---

### Finding 3: Heap Reallocation Churn & Per-Pixel Memcpy in `CpuFramePipeline::RotateRawBuffer`
- **ID:** `G36-3`
- **Priority:** Medium
- **Status:** `NEW`
- **Location:** [`CpuFramePipeline::RotateRawBuffer`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/frame_pipeline.cpp#L180-L235)
- **Caller:** `OkuFlowApp::OnFrameTick` at [src/app/app_pipeline_runtime.cpp:2121](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L2121) on CPU frame rotation, and `OkuFlowApp::PrepareOriginalFrame` at [src/app/app_pipeline_runtime.cpp:2420](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L2420).
- **Trigger:** Non-zero camera orientation (`rotationQuarterTurns_ != 0`) on the CPU frame pipeline (portrait orientation with CPU fallback or CPU-only camera formats).
- **Wrong Output / Avoidable Work & Scale:**
  1. `rotatedStageBuffer_.assign(dstWidth * dstHeight * 4u, 0u);` zeroes 8.3 MB per frame.
  2. Inside the nested loops for 90, 180, and 270 degrees:
     `std::memcpy(dstRow + static_cast<std::size_t>(x) * 4u, srcPixel, 4u);`
     is invoked per pixel — making 2,073,600 individual function calls to `std::memcpy` of 4 bytes every frame.
  3. At completion:
     `stageRaw_ = std::move(rotatedStageBuffer_);`
     `rotatedStageBuffer_.clear();`
     `rotatedStageBuffer_` has its internal buffer moved to `stageRaw_` and is left empty. On the very next frame, `rotatedStageBuffer_.assign(...)` MUST perform a fresh heap allocation (`new uint8_t[8294400]`), while `stageRaw_`'s previous memory was released. The capacity is never retained, causing an 8.3 MB heap allocation and 8.3 MB deallocation every single rotated frame.
  4. **Scale:** At 30/60 fps, 250–500 MB/s of heap allocation churn and 2 million `memcpy` function calls per frame.
- **Why Guards / Tests Miss It:**
  - Unit tests verify rotated pixel coordinates on single frames, not heap allocation stability across successive frames.
- **Proposed Small Repair or Benchmark:**
  - Replace `stageRaw_ = std::move(rotatedStageBuffer_); rotatedStageBuffer_.clear();` with `stageRaw_.swap(rotatedStageBuffer_);`. Both vectors retain their heap capacity across frames.
  - Replace `assign(..., 0u)` with `resize(...)` without zero-fill.
  - Replace `std::memcpy(dstPixel, srcPixel, 4u)` with `*reinterpret_cast<uint32_t*>(dstPixel) = *reinterpret_cast<const uint32_t*>(srcPixel);`.
- **No-Desktop Verification Steps:**
  - Unit test in `tests/test_frame_pipeline.cpp`: Run 100 iterations of `RotateRawBuffer(1, w, h)` on a `CpuFramePipeline` instance and assert that `stageRaw_.capacity()` and `rotatedStageBuffer_.capacity()` do not reallocate and pointers remain stable.
- **Confidence & Limitations:**
  - Confidence: Very High.
  - Limitations: Applies only when `rotationQuarterTurns_ != 0`.

---

### Finding 4: Unbounded Settings File Ingestion & Memory Allocation in `LoadDetailed`
- **ID:** `G36-4`
- **Priority:** Medium
- **Status:** `NEW`
- **Location:** [`settings_store::LoadDetailed`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/settings_store.cpp#L853)
- **Caller:** `SettingsController::Load` at [src/app/settings_controller.cpp:32](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/settings_controller.cpp#L32), invoked on app startup (`OkuFlowApp::OkuFlowApp` / `app_bootstrap.cpp`).
- **Trigger:** Corrupted, bloated, or maliciously oversized `settings.json` file in the user data or roaming settings directory.
- **Wrong Output / Avoidable Work & Scale:**
  1. `LoadDetailed` opens `path` and unconditionally executes `const QByteArray data = file.readAll();` without checking `file.size()` or enforcing any upper bound on file size.
  2. If a settings file is bloated (e.g. from an errant script, disk corruption appending null bytes or crash logs, or an untrusted user environment), `file.readAll()` attempts to load the entire multi-megabyte or multi-gigabyte file into memory, followed by `QJsonDocument::fromJson(data)`.
  3. In low-memory environments, this causes an unhandled `std::bad_alloc` or heap exhaustion during startup, preventing the application from booting or gracefully recovering default settings.
  4. Other I/O components in OkuFlow have explicit bounds (e.g., `kMaximumProtocolMessageBytes = 8 MB`, `kMaximumVlmResponseBytes = 2 MB`). Settings parsing completely lacks a bound.
- **Why Guards / Tests Miss It:**
  - Existing settings tests test valid settings JSON or small malformed strings; none test files exceeding reasonable size limits.
- **Proposed Small Repair or Benchmark:**
  - In `LoadDetailed`, add a guard before `readAll()`:
    ```cpp
    constexpr qint64 kMaxSettingsFileSize = 4 * 1024 * 1024; // 4 MiB
    if (file.size() > kMaxSettingsFileSize) {
        result.status = LoadStatus::Unreadable;
        result.error = QStringLiteral("Settings file exceeds maximum allowed size (4 MiB).");
        return result;
    }
    ```
- **No-Desktop Verification Steps:**
  - Unit test in `tests/test_settings.cpp`: Create a temporary file > 4 MiB, call `settings_store::LoadDetailed()`, and verify it immediately returns `LoadStatus::Unreadable` without reading or allocating.
- **Confidence & Limitations:**
  - Confidence: Very High.
  - Limitations: Standard settings files are < 32 KiB; this only triggers on corrupt or anomalous files.

---

### Finding 5: Per-Frame Heap Allocation on Bottom-Up DIBs & Redundant Intermediate Clones in `BuildStages`
- **ID:** `G36-5`
- **Priority:** Low-Medium
- **Status:** `REFINEMENT` (Refines P7 with concrete code locations and negative-stride allocation path)
- **Location:** [`CpuFramePipeline::ConvertFrameToBgra`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/frame_pipeline.cpp#L69-L78, #L107-L116) and [`CpuFramePipeline::BuildStages`](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/common/frame_pipeline.cpp#L250-L280)
- **Caller:** `OkuFlowApp::OnFrameTick` via [src/app/app_pipeline_runtime.cpp:2109, 2208](file:///Y:/Drive/My_Drive/folder/Projects/oku_flow/src/app/app_pipeline_runtime.cpp#L2109).
- **Trigger:** Cameras delivering bottom-up DIB frames (`stride < 0`) with ARGB32/RGB32 subtypes, or user enabling Debug View (`debugViewEnabled_ == true`).
- **Wrong Output / Avoidable Work & Scale:**
  1. When `stride < 0`:
     `std::vector<uint8_t> topDown(frame.size());`
     allocates a brand-new 8.3 MB heap buffer every frame, copies flipped rows into `topDown`, and then calls `CopyArgbToBgra` or `CopyRgbxToBgra` which copies `topDown` into `stageRaw_` a second time.
     `topDown` is immediately destroyed. Direct bottom-up row reading in `CopyArgbToBgra` would avoid the 8.3 MB heap allocation and the duplicate copy entirely.
  2. In `BuildStages`:
     `stageBw_ = stageRaw_;` unconditionally clones 8.3 MB even when `config.enableBlackWhite` is false.
     `stageZoom_ = zoomSource;` unconditionally clones 8.3 MB even when `config.enableZoom` is false.
     `stageFinal_ = *currentStage;` unconditionally clones 8.3 MB again.
  3. In Debug View mode, this causes 25–33 MB of redundant full-frame copying per frame.
- **Why Guards / Tests Miss It:**
  - Most test video streams use positive stride (top-down), so `stride < 0` code is rarely exercised in standard tests.
  - Debug View is an opt-in diagnostic view, so intermediate copying was marked acceptable in initial implementation.
- **Proposed Small Repair or Benchmark:**
  - In `ConvertFrameToBgra`, pass `stride < 0` directly into `CopyArgbToBgra` / `CopyRgbxToBgra` to read `src + (height - 1 - y) * effectiveStride` directly into `stageRaw_`, eliminating `topDown`.
  - In `BuildStages`, only copy into `stageBw_` or `stageZoom_` when the respective stage is actually enabled, or maintain persistent buffers without copying when disabled.
- **No-Desktop Verification Steps:**
  - Unit test in `tests/test_frame_pipeline.cpp`: Supply an ARGB32 frame with negative stride, verify output matches expected orientation, and verify allocation count remains constant across frames.
- **Confidence & Limitations:**
  - Confidence: High.
  - Limitations: Positive stride streams do not execute the `topDown` allocation; `BuildStages` redundant copies only execute when debug view is enabled.

---

## Conclusion & Recommended Next Steps

All five findings (`G36-1` through `G36-5`) target concrete, provable CPU allocations, memory churn, and bounded-resource limits without altering tracked application logic or breaking the established test suite.

Prioritized implementation order:
1. **`G36-1` (High):** Implement retained `QImage` and ink revision dirty-tracking in `PresentLatestCudaScene` to immediately eliminate ~500 MB/s heap churn on the main recording path.
2. **`G36-3` (Medium):** Switch `stageRaw_` / `rotatedStageBuffer_` to buffer swapping and 32-bit pixel copies in `RotateRawBuffer`.
3. **`G36-4` (Medium):** Add a 4 MiB file size guard in `settings_store::LoadDetailed`.
4. **`G36-2` (Medium-High):** Precompute 1D coordinate LUTs for `PresentFitted` and `ResampleRecordingCanvas`.
5. **`G36-5` (Low-Medium):** Direct bottom-up DIB reads in `ConvertFrameToBgra` and stage copy pruning in `BuildStages`.
