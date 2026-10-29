**Bounded closure round for frozen `40a112d`: no blocking defect found in the three priority areas.** The commit and tree match (`40a112d1becc6f3e7062e3d2d41f30209cd8ede4`, tree `e64050a7…`) and the worktree is clean. The range `e2d433f..HEAD` is 11 commits and 61 files.

I read `agents.md`, `CLAUDE.md`, `README.md`, `docs/README.md`, `docs/developer_guide.md`, `docs/hardcoded_paths.md`, `docs/THIRD_PARTY_LICENSES.md` and the archived r26 review in full. Nothing was built, tested or run, and I make no claim about your matrix.

## Findings (none blocking)

1. **Stale magic-values doc (confirmed).** `docs/hardcoded_paths.md:193-194` still says a neutral ink channel spread up to 16 snaps to pure black/white. Since 33c94df, `src/ui/ultra_clear_text_overlay.cpp:138` keeps the source colour only when spread is at least 64 and contrast is at least 7:1. That file is the authoritative magic-values doc, so the line should say "below 64".
2. **One bad Windows word box can fail the whole read (possible; not shown to happen).**
   - `uct_default_recognizer.cpp:90-96` turns any Windows word with a non-finite, zero or negative width/height into a whole-result backend failure.
   - Enough consecutive failures set the backend to `kFailed` (`ultra_clear_text_controller.cpp:1404-1406`).
   - Before 5104ffe such a word passed through, and `FinalizeRecognizedGeometry` (`ultra_clear_text.cpp:721-722`) already marks zero-area quads per word as `kInvalidGeometry`.
   - I have not shown that Windows.Media.Ocr ever returns a zero-size box. If it can, a per-word disposition is safer than failing the result.

## Priority checks

**Angle transform (5104ffe): correct in source.**
- `uct_windows_ocr.cpp:164` copies `TextAngle` only when the engine reports one.
- `uct_windows_geometry.hpp:13-36` rotates each corner about the centre of the image actually sent to the engine (`prepared`), using the clockwise matrix for Y-down coordinates (the same convention as Microsoft's OCR sample overlay). It then applies the exact per-axis resize. Rotating before the uneven resize is the correct order.
- The Windows recognizer ignores detail focus, so no crop-offset path needs fixing.
- Tilted quads go through `FitOrientedWordInk`. At angle 0 the quads stay axis-aligned and are rebuilt from the refined bounds (`ultra_clear_text.cpp:2164-2173`).
- Limit: I checked the sign convention against the API semantics and your 2.4° corrected proof, not by running anything.

**Native issuance and duration, line and word guards (4c62621): consistent.**
- **Issuance:** `VideoPlayback::CorroborationPolicy` (`video_playback.cpp:1392`) runs under the player mutex.
  - It requires `IssuedFrame` (`:166`), which checks owner, frame identity, current generation, sealed sample ID/PTS/duration, file, track and archive generation.
  - It also requires the sealed `savedFile` flag and a valid timestamp.
  - The app adds `Kind::File` plus the exact `doc.frame` pointer.
  - Both the CPU and GPU decode paths set sample ID equal to PTS (`:561`, `:656`).
  - The descriptor retains only lease metadata, no pixels.
- **One time-window rule everywhere:**
  - Player: `FollowingFrame` (`:180`), the pause path (whose lease is guaranteed non-null) and the `Publish` reset (`:775`).
  - App: `PollVideoPlayback`.
  - Controller: input check (`:116`), scene sameness (`:422`), stamping (`:1509`), submit gate (`:1532`) and result gate (`:1673`).
  - Worker: line and word block (`ultra_clear_text.cpp:2203-2204`) plus `MatchImmutableWords` (`:1591`).
  - Model: `AdmitImmutable` (`:2718-2721`) and `independentNeighbor` (`:3379-3380`).
- **Tilted words (r26 #6): closed.** The shape-patch word proof, line matches, `neighborWords` and `registeredWord` all sit behind the same guard. An out-of-window pair now loses proof for both upright and tilted words. Detail-neighbour jobs go through the same request (controller `:1536-1555`).
- **GPU path:** the resident reference copies the full stamp including the descriptor (`src/cuda/uct_device_analysis.hpp:602`), so GPU results are not wrongly rejected as stale.
- **Limits:**
  - The comparisons are exact integers, so a one-tick rounding mismatch would refuse the expansion. That fails closed and is no worse than before.
  - The opt-in FFmpeg reader builds its sample duration from `AVFrame.duration` (`video_ffmpeg_reader.cpp:328-330`) and also counts as a saved file. The native probe only covered Media Foundation.

**Recap findings r26 #3/#4/#5: all closed in source, no reachable counterexample.**
- **#3 Space while collapsed:** `IsCommandReady` (`video_transport.cpp:465`) keeps the source, hidden-UI, enabled, gate and mute/return checks but drops the collapse check. `main_window.cpp:4634` uses it.
- **#4 Resume forever:** `app_video_playback.cpp:328-330` cancels a pending Resume when the recap has failed or is unsupported and the held point is unavailable, and the held frame then shows Unavailable. A merely busy encoder keeps waiting, and a recap epoch change returns to the camera (`:319`).
  - Residual: if the recap is healthy but its in-progress segment never finalizes (finalizing needs a later frame), Resume stays pending while the button shows "Pause video". Pressing it again cancels (`:935`), and Jump to live still works. I did not check whether a camera can stall without an epoch reset.
- **#5 empty segment latch:** `finish` (`video_recap.cpp:127-151`) discards a zero-admission Stop without an error. A non-empty segment that fails to finalize, seal or publish still latches with specific text. Queued frames stop after a latch (`:161-166`). `Append` guards against empty admissions and enforces sequence order across segments.

**Other r26 dispositions:** #1 fixed in source, with acceptance resting on your corrected proof. #2 is accepted owner policy: the docs honestly state Windows reports a nominal 1.0 confidence, and no veto was reintroduced. `IsHighRiskToken` still rejects `|`, non-ASCII digits, modifier letters and symbols.

## Not inspected
- Test bodies added in the range (controller, model, playback, recap/GPU, native fixture, transport), apart from the native sparse probe, the replay probe's CMake registration and the new strings' translation entries. The new strings exist in the catalog and Turkish file under the right context.
- The pass counts you reported (158/0/0, 4/235) and 24-language parity.
- The 47af5c0 8x/token logic beyond the docs, plus `CHANGELOG.md` and `code_reference.md` completeness.
- Any runtime behaviour: native NVDA/UIA, real GPU inference, camera, or the shipping/CPU/CUDA/PCH gates.
