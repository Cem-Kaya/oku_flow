# Verified Non-Issues

Plausible-looking "bugs" that were investigated during the 2026-07-21 analysis and
**refuted**. Future agents: check here before filing or "fixing" these — each one
pattern-matches to a real bug class but is actually correct in this codebase.

---

## 1. "Assistive/joystick overlays are leaked (raw `new`, no `delete`)"

- **Claimed at:** `src/app/app.cpp:259` (`new AssistiveOverlay(renderWidget_)`) and `:320` (`new JoystickOverlay(renderWidget_)`); destructor only nulls the pointers (~552–553).
- **Why it's not a bug:** `renderWidget_` is passed as the Qt parent in the constructor.
  Qt's parent-child ownership deletes both overlays when `renderWidget_` is destroyed.
  Nulling the raw pointers in the app destructor is bookkeeping, not a leak.
- **Legitimate residual concern:** if an overlay ever outlives or is reparented away
  from `renderWidget_`, ownership breaks — a one-line comment at the `new` sites
  documenting Qt ownership would prevent this report from recurring.

## 2. "OCR frames have swapped R/B channels (`Format_ARGB32` used for BGRA data)"

- **Claimed at:** `src/common/assistive_runtime.cpp:260` — `QImage(bgraData, w, h, w*4, QImage::Format_ARGB32)`.
- **Why it's not a bug:** `QImage::Format_ARGB32` is defined as `0xAARRGGBB` stored in
  host byte order. On little-endian x86/Windows the in-memory byte order is
  B, G, R, A — exactly the BGRA layout the pipeline produces. Colors are correct.
  `CHANGELOG.md` records this as a deliberate fix ("BGRA frame wrappers use the
  Qt-supported `QImage::Format_ARGB32`"). Do not add a swizzle.

## 3. "`cudaStreamDestroy` on a busy stream is undefined behavior"

- **Claimed at:** `src/cuda/cuda_interop.cpp` destructor / release paths.
- **Why it's not (that) bug:** CUDA documents `cudaStreamDestroy` on a stream with
  pending work as safe — it returns immediately and defers resource release until the
  work completes.
- **The real bug nearby:** destroying the *surface object / external memory / device
  buffers that queued kernels still reference* before the stream drains. That one is
  real and filed as **S1** in [01-stability-threading.md](done/01-stability-threading.md).
  Fix S1; don't "fix" stream destruction in isolation.

## 4. "Capture loop races StopCapture/StartCapture on `sourceReader_`" (was S2)

- **Claimed at:** `src/capture/media_capture.cpp` — `CaptureLoop()` copying
  `sourceReader_` without a lock while `StopCapture()` flushes/resets it.
- **Why it's not a bug (verified 2026-07-21):** `running_` is
  `std::atomic<bool>` (media_capture.hpp:79). `StopCapture()` sets it false,
  calls `Flush()` (the intended wakeup for a blocking synchronous
  `ReadSample`), **joins the capture thread, and only then** resets the COM
  members. `StartCapture()` calls `StopCapture()` first, so the members are
  only ever written when no capture thread exists. No lock is needed.

## 5. "Build artifacts are committed to git"

- **Checked:** `git ls-files` at commit `9e069d9` — 58 tracked files, none under
  `build/`. The `build/` tree on disk is untracked working state; `.gitignore` covers it.

## 6. "Capture-thread vs UI-thread races on app pipeline state" (was S4)

- **Claimed at:** `src/app/app.cpp` — tuning state, frame dimensions,
  reconnect flags, and `lastCameraError_` accessed "without synchronization"
  alongside the MF capture callback.
- **Why it's not a bug (audited 2026-07-23, plan 11 Wave 2):** the MF capture
  thread touches exactly two things in the app: `latestFrame_` under
  `cameraMutex_` (app.cpp:3372-3375; the Qt tick deep-copies it under the same
  mutex, app.cpp:3664-3667) and a `QMetaObject::invokeMethod(...,
  Qt::QueuedConnection)` marshal in the error callback (app.cpp:3376-3383).
  Everything else — tuning values, frame dimensions, reconnect flags,
  `lastCameraError_` — is written and read only on the Qt main thread
  (`OnFrameTick` is a `QTimer` slot, app.cpp:1016). `MediaCapture`'s
  cross-thread flags are `std::atomic` (media_capture.hpp:101-103), its
  `lastError_` is control-thread-only (the capture loop reports through the
  by-value callback string), and `currentFormat_` follows a clean ownership
  handoff (control thread writes only while no capture thread exists). Full
  member-by-member table in 01-stability-threading.md S4. Do not add a
  snapshot mutex until processing moves off the UI thread (A1-step-5).
