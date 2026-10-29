# UI cleanup candidate — final focused closure (Muse, read-only)

Candidate: `Z:/Projects/T3-worktrees/oku_flow/t3-b137fab1`, dirty from
`cf7eaac3`, production and tests frozen. Scope: only the one P2 left by
`muse/repair-closure.md` (R3 §A) and the final outside-click policy.
Nothing was built, run, or edited. Focused builds are root-owned and in
flight; no runtime result is claimed here.

**Verdict: closed. No concrete P1 or P2 remains in this residue.**

Evidence note: `final-review.patch` (16:04) postdates the frozen sources
(16:03) and its hunks match the live tree. The R3 stale-patch note is
resolved.

## R3 §A — refused forced-mute edit now resyncs

- `VideoTransport::SetSnapshot` reconciles unconditionally after the
  conditional `RefreshText()` (`src/ui/video_transport.cpp:538-543`). A
  refused edit's identical snapshot therefore reaches the repair.
- The repair compares against the effective volume
  (`snapshot_.muted ? 0 : snapshot_.volumePercent`). It respects
  `!volume_->isSliderDown()`. It resets under `QSignalBlocker`. It calls
  `RefreshVolumeText()` only on an actual mismatch, which republishes
  the `%` label, accessible description, and tooltip from one value
  (`:758-763`).
- Keyboard path: the refusal poll re-enters `SetSnapshot` synchronously
  inside `VolumeChanged` (`src/app/app_video_playback.cpp:229-230`), so
  the thumb snaps back before the slider handler's own
  `RefreshVolumeText()`. Drag path: the guard skips while held, and the
  16 ms playback timer pushes a snapshot every tick while a file is
  open (`:195-197,471,589,1377`; stops only on shutdown or return to
  camera). A finished drag resyncs on the first tick after release.
- The already-muted guard-release sub-case from R3 is covered by
  construction. The thumb is corrected at refusal time, so it no longer
  depends on a later snapshot change.
- Native test: `tests/native_app_control_fixture.cpp:997-1017` loops
  active and finishing guards. It opens the real popover via the
  speaker (`slider->isVisible()`, value 0), drives the actual slider
  (`setValue(5)` through the production `valueChanged` connection), and
  asserts thumb 0 plus accessible description `0%`. It then asserts
  stored 65 retained, unmute restores 65 on the thumb, and an accepted
  70 applies. This would fail on the pre-fix code.
- `SetVideoRecordingGuardForTest` is compile-guarded by
  `OKUFLOW_VIDEO_DOCUMENT_TESTS` on both sides
  (`include/okuflow/app/app.hpp:250-266`,
  `src/app/app_video_playback.cpp:1666,1690-1696`). It sets only the
  active/finishing flags and `fileHasAudio`/`fileAudioAvailable`
  metadata, then polls. Under the guard `outputMuted` holds, so no
  audio endpoint is fed (`:1304-1319`).

Non-blocking note: the test drives the slider programmatically, not by
a literal mouse drag. The drag-finish half rests on the poll-tick
argument above. This matches the R3 fix request and is not a P2.

## Outside-click policy — removal is clean and consistent

- No speculative residue remains. There is no `widgetAt`, no
  receiver-focus computation, no pending native-close state in the
  header, and no queued focus attempt. The only `singleShot` in the
  file is the pre-existing `UpdateActions` on `FocusOut` (`:919`). All
  `setFocus` sites are synchronous show/hide/Tab/collapse paths.
- Outside press, owned or foreign window, calls `HideVolumePopup(false)`
  and does not return `true` (`:902,904`). The event is delivered
  unconsumed. With `restoreFocus=false` the hide path assigns no focus
  target (`:433-448`). Focus then follows the pressed widget's own
  click-focus, as in the original settlement.
- Scoped paths remain intact. Speaker replay emits one toggle and
  consumes the press (`:898-901`). Owned-window Escape hides with focus
  restore and consumes (`:905-907`). A foreign active modal blocks both
  (`:894`). `ApplicationDeactivate` hides without focus (`:908`).
- New test (`tests/video_transport_tests.cpp:839-847`) asserts exactly
  what offscreen can prove: popover closed, outside target received
  exactly one `clicked`, zero `MutedChanged` dispatches, and focus was
  not forced to the speaker. It correctly does not assert OS-restored
  target focus, and says why (offscreen cannot restore an active
  window after the grab). Native Windows grab/focus behavior stays
  explicitly unverified, with prior failed attempts retained.
- Retained assertions verified in place: focus lands on the slider at
  every open (`:817`, before later mouse activations), Tab/Backtab
  (`:818-819`), host resize to speaker (`:820`), collapse to the
  externally reparented chevron (`:69,821`), UI-hide and source loss
  to the viewport (`:822-825`), and foreign Escape/press scoping
  (`:832-838`). The UCT test still calls the real
  `RetranslateWidgetTree` (`tests/ultra_clear_text_tests.cpp:848,850`).

## Limits

Source confidence is high for the traces above. Runtime confidence is
unchanged: focused builds/gates are root-owned and in flight, and
native popup-grab, focus restoration, and screen-reader delivery
remain unproven here. Unchanged areas and the deferred recipe/color
topics were not re-audited, per brief.
