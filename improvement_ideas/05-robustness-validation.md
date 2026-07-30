# Robustness, Validation & Error Handling

Input validation, external-process/network hardening, and I/O failure handling.

---

## V1. Settings versioning and migration chain

- **Priority:** MEDIUM · **Effort:** medium · **Status:** Reported
- **Evidence:** `src/app/settings_store.cpp` — version handling (~353–360) only distinguishes `version <= 1`; `ConfigFromJson()` (~219–246) silently defaults missing/invalid fields

**Problem.** A settings file from a *newer* app version is parsed as-if-current with no
warning; corrupt or truncated JSON degrades to silent defaults, discarding the user's
carefully tuned presets (a big deal for this audience) without telling them.

**Fix.**
1. Write `CURRENT_VERSION` on save; on load, run an explicit migration chain
   (`MigrateV1toV2`, …). If the file's version is *newer* than the app, keep a backup
   copy (`settings.json.bak`) before rewriting.
2. Validate ranges after parse (blur radius 0–50, thresholds 0–255, alpha 0–1, non-empty
   preset ids); log each field that was clamped/defaulted.
3. On JSON parse failure, rename the corrupt file aside (don't overwrite it) and notify
   the user that settings were reset.
4. Add unit tests: legacy v1 file, corrupt file, future-version file, out-of-range
   fields. (Needs the test scaffolding from
   [06-build-tooling-docs.md](06-build-tooling-docs.md).)

---

## V2. Stride/dimension validation and overflow checks in frame converters

- **Status: DONE 2026-07-22.** CPU format dispatch and NV12/YUY2 conversion now
  cap dimensions, validate packed/plane strides and source lengths, use checked
  size arithmetic with a 1 GiB BGRA cap, clear stale output on rejection, and
  avoid a second-pixel write for odd-width YUY2 frames.
- **Priority:** MEDIUM · **Effort:** small · **Status:** Reported
- **Evidence:** `src/common/image_processing.cpp` — `ConvertNv12ToBgra()` (~62), `ConvertYuy2ToBgra()` (~124); size arithmetic like `uvStride * ((height + 1) / 2)` (~78–79) and `width * height * 4` (~85)

**Problem.** Stride and dimensions come from Media Foundation / camera drivers and are
trusted. `stride == 0`, `stride < width*bpp`, or huge values produce out-of-bounds reads
or overflowed allocation sizes. Combined with S7 (mid-stream format changes,
[01-stability-threading.md](done/01-stability-threading.md)) this is a plausible crash path
with buggy drivers.

**Fix.** At the top of each converter: reject `width/height == 0` or `> 16384`, require
`stride >= width * bytesPerPixel`, and compute buffer sizes in `size_t` with an explicit
cap (e.g. 1 GB) before `resize()`. Also verify the source buffer length covers
`stride * height` before reading. Small, mechanical, high value — good first PR.

---

## V3. Tesseract subprocess: timeout and temp-file cleanup

- **Status: DONE 2026-07-22.** OCR owns a watchdog timer, kills a hung process,
  removes the pending temporary image on completion/error/timeout/shutdown, and
  prevents a second OCR start while the process is active.
- **Priority:** MEDIUM · **Effort:** small–medium · **Status:** Partially confirmed (`tempFile.setAutoRemove(false)` seen at assistive_runtime.cpp:265; no timeout timer found)
- **Evidence:** `src/common/assistive_runtime.cpp` — `StartOcr()` (~260–290), mode-switch kill with 250 ms wait (~158–160)

**Problem.**
1. No watchdog: a hung `tesseract.exe` stalls OCR forever (busy-flag never clears, so
   all future OCR is dead until restart).
2. Temp PNGs are created with `setAutoRemove(false)`; if the process errors/hangs or the
   app exits mid-OCR, orphaned `openzoom_ocr_*.png` files accumulate in %TEMP%. Verify
   every exit path deletes `pendingOcrImagePath_`.
3. Racy restart: mode toggling kills with a 250 ms grace; a new OCR can start while the
   old process is still dying.

**Fix.** Add a single-shot `QTimer` (5–10 s) started with the process; on timeout,
`kill()`, delete the temp file, and report "OCR timed out". Delete the temp file in *all*
finish/error paths (or switch to `setAutoRemove(true)` + keeping the QTemporaryFile
alive until the process finishes). Guard start/kill with a simple state flag.

---

## V4. VLM HTTP requests: timeout, HTTPS enforcement, response caps

- **Status: PARTIALLY ADDRESSED 2026-07-22.** Requests now use a 30-second Qt
  transfer timeout. HTTPS warnings and an explicit response-size cap remain.
- **Priority:** MEDIUM · **Effort:** small · **Status:** Reported
- **Evidence:** `src/common/assistive_runtime.cpp` — request construction (~337–344); API key from `OPENZOOM_VLM_API_KEY` env var (~255–257)

**Problem.** No request timeout (a stalled endpoint wedges the busy-flag), no warning
when the configured URL is plain HTTP (the API key would then travel in cleartext), and
no size cap on the response body.

**Fix.** Set `QNetworkRequest::setTransferTimeout(30000)` (Qt ≥5.15 — one line); log a
prominent warning at startup if `OPENZOOM_VLM_API_URL` is not `https://`; cap accepted
response size; on `QNetworkReply` error include the HTTP status + short body excerpt in
the overlay error text. Env-var key storage is acceptable for now — documenting that it
is the user's responsibility is enough at this stage.

---

## V5. Recording: disk-full and low-space handling

- **Status: DONE 2026-07-22.** Recording refuses to start below 500 MiB free,
  rechecks every five seconds, finalizes below 200 MiB, maps disk-full HRESULTs
  to the same clear stop reason, and writes fragmented MP4 for crash tolerance.
- **Priority:** MEDIUM · **Effort:** medium · **Status:** Reported
- **Evidence:** `src/common/media_writer.cpp` — `WriteSample()` result handling (~147–151)

**Problem.** A 12-hour H.264 cap exists, but disk exhaustion mid-recording just logs and
leaves the writer/file state inconsistent; the user isn't told their recording stopped.

**Fix.** Check free space (`QStorageInfo` on the output dir) before starting and every
~5 s while recording; stop cleanly (finalize the MP4 so it stays playable) and surface a
clear message when below a threshold (e.g. 500 MB). Map `ERROR_DISK_FULL`-class HRESULTs
from `WriteSample` to the same clean-stop path.

---

## V6. Media Foundation transient-failure retries

- **Status: DONE 2026-07-22.** Camera startup retries transient errors with
  150/300/600 ms backoff; mid-stream loss is classified, surfaced, and handed
  to the app's 2/4/8-second same-device reconnect state machine.
- **Priority:** LOW–MEDIUM · **Effort:** small · **Status:** Reported
- **Evidence:** `src/capture/media_capture.cpp` — `MFEnumDeviceSources` (~83), `MFCreateSourceReaderFromMediaSource` (~152, 197) fail immediately

**Problem.** USB hub resets and driver stalls cause one-shot failures that currently
kill capture until the user manually re-selects the camera.

**Fix.** Retry enumeration/reader creation 2–3 times with short backoff (100/200/400 ms).
For mid-capture device loss, detect the reader error in `CaptureLoop()` and attempt one
automatic reconnect to the same symbolic link before giving up with a visible message.

---

## V7. Consistent readback/pipeline error reporting

- **Priority:** LOW · **Effort:** small · **Status:** Reported (still open 2026-07-22; line refs updated for the async-readback rework)
- **Evidence:** `src/app/app.cpp` — synchronous `ReadbackTexture()` consumers at ~461
  (photo) and ~1501/~1560 (on-demand analysis) discard failure reasons; the async ring
  (`RequestReadback` at ~2771) silently skips a frame when both slots are in flight or
  the request fails

**Fix.** Covered by U5's error-category enum
([04-accessibility-ux.md](04-accessibility-ux.md)) — extend it through
`D3D12Presenter::ReadbackTexture` so photo/recording/assistive failures each report *why*
(device removed, OOM, timeout). Track a failure counter for diagnostics.

---

## V8. Slider-value conversion hardening

- **Priority:** LOW · **Effort:** small · **Status:** Reported (partially improved 2026-07-22: the originally cited `OnTemporalSmoothStrengthChanged()` (~2055) now clamps against the slider's real min/max before dividing; other `On*Changed` slots still hand-roll conversion without a shared helper)
- **Evidence:** `src/app/app.cpp` — per-slot ad-hoc conversion in the various `On*Changed` slots

**Fix.** One shared helper that maps a slider value to a normalized float using the
slider's *actual* min/max with clamping, used by every slot. Trivial once
`UIStateManager` (A1) exists; fine to do standalone earlier.
