The TextAngle defect you found is real. The retained bridge proof independently confirms it, and a replay suggests your narrow Windows-only fix should resolve this pair on its own. I also found three new defects in this range, one older Resume defect, and one concrete hazard in the sparse-file proposal.

This was read-only against frozen `b17805b`. I did no edits, builds, tests or app/GPU launches. The only thing I ran was read-only Python over the retained `proof.json`. I make no quality, release or performance acceptance claim. No rate limit or other failure occurred. You can start root builds/tests now.

## 1. Bridge TextAngle is the cause of the paused-video symptoms (confirms root; existed before this range)

- **Source:** `src/common/uct_windows_ocr.cpp:163-171` never reads `TextAngle`. `src/common/uct_default_recognizer.cpp:86-96` maps `BoundingRect` straight to source pixels and leaves `quadPx` invalid. Production uses Windows OCR as the startup bridge (`src/common/uct_paddle.cpp:156-158`).
- **Measured error:** one rotation of +2.442° about the image centre (640,360), with about 1 px of translation, explains all 25 same-text bridge→GPU word offsets. RMS residual is 0.82 px, max 2.36 px. In y-down pixels that rotation is clockwise, which matches Microsoft's description.
- **Pair proof doesn't catch it:** selected and neighbour frames share the same deskew error, so registration only shows the pixels are stable. 25 of 26 misplaced bridge words became glyph-eligible (`bridge.shownWords=25`).
- **How the overlap between bridge and GPU boxes decides each outcome:**

| Overlap | What the code does | Words in this proof |
|---|---|---|
| ≥ 0.5, same text | Merge keeps the bridge box and drops the GPU word (`ultra_clear_text.cpp:3051-3066`) → misaligned glyphs/plates | SETTINGS .67, Toggle .93, Left/Hand/Right/Hand .66–.92, 100 .57/.75 |
| 0.2–0.5 | Merge keeps both; painter marks both as collisions and draws neither (`ultra_clear_text_overlay.cpp:36`, `:519-528`, `:674`); their boxes then protect neighbours (`:716-728`) | MAGNIFIER .37, Camera Location .45, Head .42, Level .30, 42% .30 |
| < 0.2 | Both drawn → visible duplicates | Zoom .14; Display/Size/34% sit close to .2 |

- **Effect on reading text:** the pane duplicates lines ("Head\nHead", "Camera Location\nCamera Location", "persp\nPersp"). Positioned screen-reader rectangles inherit the misplaced boxes.
- **Harness caveat:** in `tests/uct_paused_video_diagnostic.hpp:381-390`, the `glyphEligible` field means "this GPU word survived the merge with its own box". It is not admission eligibility, so don't read it that way.
- **Replay of the fix:** rotating the bridge boxes by 2.442° raises every same-text overlap to 0.95–1.0. Nothing stays in the 0.2–0.5 collision band. The only exception is the lower "Right", which the GPU reads as "F.ight" (the known OCR limit). This supports your scope of no merge or painter edits.
- **Things to get right in the fix:**
  - `TextAngle` is nullable; treat null as 0.
  - Rotate in the OCR input image's pixels, about its centre, then apply the existing inverse scale.
  - Set `quadPx` to the rotated rectangle and `boundsPx` to its bounding box, for both selected and neighbour outputs.
  - Update the comment at `ultra_clear_text.hpp:309-311` that says Windows words keep `valid == false`.
- **Pass criteria for this pair:** native angle ≈ +2.4; all same-text overlaps ≥ 0.95; no duplicated reading lines; Head, Camera, Location, MAGNIFIER, Level and 42% drawn.

## 2. New in 47af5c0: the ordinary-label change has no confidence gate for the Windows bridge (medium)

- `RecognizedWord::confidence` defaults to 1.0 (`ultra_clear_text.hpp:306`), and the Windows recognizer never sets it. Every bridge word in the proof shows `"confidence": 1`.
- So the 0.7 gate (`ultra_clear_text.cpp:1553`) never rejects anything from this engine. After `:1770-1810`, a Windows misread like "l4" for "14", or an isolated l/1/0/O, becomes paintable.
- Its only remaining check is the same engine re-reading a near-identical neighbour, which finding 1 shows can pass even wrong geometry. This contradicts `docs/uct_token_policy_and_detail.md` and the owner's "high-confidence" condition.
- **Fix:** mark Windows words as having no engine score, and keep the old lookalike veto (or reading-only) for score-less words. Add a fixture with a Windows-origin "l4" pair.

## 3. New in b17805b: collapsing the transport disables Space on the video (medium for keyboard users)

- `ActionShown` returns false for play/pause while collapsed (`video_transport.cpp:450-463`).
- `main_window.cpp:4630-4637` then accepts Space and does nothing, because the bar is still visible.
- This breaks the documented contract (`docs/video_transport.md:34-36`, `:84-85`): Freeze/Resume silently stops working whenever the bar is collapsed.
- The new collapse test never presses Space on the video surface.
- **Fix:** don't let collapse block play/pause for the viewport and timeline key paths, and add a MainWindow test for Space while collapsed.

## 4. Older defect in the Resume wiring: Resume can wait forever (low-medium)

- Pause stays available even when recap is unhealthy (`app_video_playback.cpp:366`).
- Resume only proceeds when `r.available` is true and the held time is at or before the recap end (`:323-328`).
- If recap is unsupported, never published, or latched off before the held frame was published, the state stays Buffering indefinitely. The Unavailable override at `:379` excludes held frames.
- Since `:363`, the button now also shows "Pause video" during that wait. Jump to live still escapes, but nothing is announced.
- **Fix:** when that condition holds and there is a recap error or unsupported source, clear the pending Resume, show Unavailable and announce it.

## 5. New in b17805b: an empty segment now permanently disables recap (low)

- If the encoder accepts no frames for a whole 2 s window because of backpressure, `finish(true)` now records an error that stops recap until the source resets. The relevant code is `video_recap.cpp:133-139`, `:158-163` and `:194-199`.
- Before this change the empty segment was discarded and a new writer started.
- The error text, "finalization produced no playable segment (HRESULT 0)", is misleading because nothing was finalized.
- **Fix:** treat an empty segment as pressure and discard it; only latch a non-empty segment that fails to finalize, seal or publish.

## 6. Hazard in the sparse SavedFile proposal

- The 250 ms neighbour window is enforced in two independent places: `video_playback.cpp:37` (used at `:177` and `:197`) and an unnamed literal at `ultra_clear_text.cpp:1591`.
- Line-level registration (`:2203-2209`) has no time limit. When the 1591 check rejects, word registration returns empty without reporting a failure.
- Upright words then still pass through the line-level path (`:3570`), but tilted words silently become reading-only. A 2.5 s expansion that misses line 1591 would therefore look fixed on upright fixtures.
- The sealed SavedFile descriptor needs to govern both places, with a fixture of a tilted word more than 250 ms apart.

## Checked, no defect found

- **8x detail:** every consumer shares the same constant, and the 960 px long-edge cap and the 480/704 supplemental limits still hold. This proof's detail pass ran at an effective 1.07x, so 8x itself wasn't exercised here.
- **Sequence zero:** camera and phone sequence numbers increase within a session, and reconnects go through `StopCameraCapture` → `ResetLiveRecap`.
- **App metadata wiring:** the new identifiers and types match their declarations. It is still uncompiled.
- **Build wiring:** the new `video_recap_native_gpu` test is registered correctly.
- **Colour threshold (33c94df):** a design change, not a defect.
- **Known limits, unchanged:** "F.ight" and the conservative Hand protection.
