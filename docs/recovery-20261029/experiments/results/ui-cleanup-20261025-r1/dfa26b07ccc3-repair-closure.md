# UI cleanup candidate — repair closure review (Muse, read-only)

Candidate: `Z:/Projects/T3-worktrees/oku_flow/t3-b137fab1`, branch
`fix/ui-hierarchy-volume-popover`, dirty from `cf7eaac3`. Scope: only the
repairs to `muse/implementation-review.md` §1–§7, their callers, and the
camera-adoption matrix repair. Nothing was built, run, or edited; runtime
results below are never claimed.

**Verdict: no P1 remains. One P2 remains (§A, forced-edit thumb resync).**
Every other prior objection is closed by inspection, including the
independent `RememberAdoptedFormat` repair.

Evidence note (not a code defect): `repairs-review.patch` (15:39) predates
the live `src/ui/video_transport.cpp` (15:44), `tests/video_transport_tests.cpp`
(15:41) and `tests/ultra_clear_text_tests.cpp` (15:42). This review inspected
the live tree, including the unarchived outside-click receiver-focus block
(`video_transport.cpp:897-906`) and the post-patch test adjustments; re-archive
the patch before using it as the review record.

## A. Remaining P2 — a refused forced-mute edit never resyncs the slider

The app-side refusal is correct, but the claimed resync does not happen.
`src/ui/video_transport.cpp:339-345` emits `VolumeChanged` synchronously, so
the refusal `PollVideoPlayback()` re-enters `SetSnapshot` with identical
muted/volume values; `changedText` (`:502-505`) stays false, `RefreshText()`
is skipped (`:538`), and the only slider reset lives inside `RefreshText`
(`:729-733`). `RefreshVolumeText` (`:753-758`) then publishes the local
stepped value. Trigger: File source, recording `active||finishing`, popup
open — each Up/Down key or a finished drag leaves thumb, `%` label and the
accessible value at the refused level while stored gain/`lastAudible` stay
intact. If the file was already muted, guard release produces no snapshot
change either, so the first accepted step after the guard starts from the
stale thumb instead of the stored gain. No audible leak; this is
presentation/state-basis desync only.

Minimal fix: reconcile in `SetSnapshot` independent of `changedText` —
if `!volume_->isSliderDown()` and `volume_->value()` differs from
`snapshot_.muted ? 0 : snapshot_.volumePercent`, reset it under
`QSignalBlocker` and call `RefreshVolumeText()`. Add one native-fixture
assertion that moves the actual slider under `RecordingAudioGuard`: thumb
returns to 0, stored 65 retained, first post-guard step bases on 65. The
existing forced test emits signals directly and cannot see the thumb
(`tests/native_app_control_fixture.cpp:1000-1005`).

## Prior objections — closed by inspection

- **§1 Forced-mute (was P1):** both handlers refuse exactly the forced
  predicate `kind != File || active || finishing` matching `outputMuted`
  (`src/app/app_video_playback.cpp:214-215,229-230,1304`); no
  gain/`lastAudible`/`muted` mutation is reachable behind it, and the
  `Recording` assignments at `:219,234` are now inert defence. Active and
  finishing branches are both asserted at app level
  (`tests/native_app_control_fixture.cpp:997-1006`). Sole residue is §A.
- **§2 Focus ownership:** `HideVolumePopup` captures popup focus ownership
  before hiding and resolves preferred → speaker → play → viewport
  (`src/ui/video_transport.cpp:433-448`). Source change, unavailable,
  move/resize and Escape restore (`:494,667,910,918-919`); UI-hide and
  collapse pass explicit viewport/collapse targets (`:553,563`) with
  `ReturnFocus()`/collapse second-stage rescue (`:534,554,570-576`).
  Outside press and deactivate stay exempt (`:897-908,912`), and the
  outside branch retains the press's real ClickFocus target. Tests cover
  open-focus, Tab/Backtab, resize, collapse, hide, source-end and outside
  (`tests/video_transport_tests.cpp:810-840`). Caveat for the native pass:
  offscreen mouse-mute lands focus on the named speaker, not the slider
  (test comment `:801`); not a loss, but confirm under the real popup grab.
- **§3 Global filter scope:** replay/Escape require the watched widget's
  window to be the popup or the transport window and no foreign active
  modal (`:886-911`); foreign press closes without consuming or emitting
  (`:908`), which the foreign-window test pins (`:827-839`). Single
  dispatch is count-asserted for mouse and slider Space (`:788-803`).
- **§4 Mute announcements:** one polite edge announcement while open, with
  localized percent and existing action string; computed pre-assignment,
  rechecked after `UpdateActions`, no repeat on duplicate snapshots
  (`:495,539-545`), asserted at `tests/video_transport_tests.cpp:842-856`.
- **§5 Number eligibility:** only featured/custom rows carry
  `kQuickModeShortcutRole` (`src/app/app_settings.cpp:25-44`); badge,
  carousel and key path all share `PresetShortcut`
  (`src/ui/main_window.cpp:106-110,2186,3698,4849`). Legacy rows stay
  clickable/relative-activatable but badge- and key-free; customs take
  5–9 by row. Role/text asserted (`tests/native_app_control_fixture.cpp:957-973`).
- **§6 Browse candidate:** More captures the candidate id (`UserRole+1` ==
  `kPresetIdRole`), records the applied cancel row after repopulation,
  then restores by id if still present (`src/ui/main_window.cpp:2088-2099`);
  browsing never applies (activation is only via `quickModeActivated`,
  `src/app/app_bootstrap.cpp:485-488`). Continuity asserted at
  `tests/native_app_control_fixture.cpp:954-956`.
- **§7 Test gaps:** materially closed — exact dispatch counts, focus after
  open (`:786`), Tab/Backtab/source/collapse/resize/outside, foreign
  Escape/press, announcements, forced active+finishing, badge/browse, and
  UCT translated description + single announcement with real
  `RetranslateWidgetTree` (`tests/ultra_clear_text_tests.cpp:831-856`).
  Non-blocking residues only: no direct number-key press or wheel-gate
  assertion, no modal-owned or candidate-disappears-on-collapse case, and
  `PresetShortcut`'s missing-role fallback is fail-open but unreachable
  (Populate is the sole preset-item creator).
- **Four-mode geometry:** cap of 2 columns at ≤4 rows, 1 column when
  narrow (`src/ui/main_window.cpp:4014-4017`); the root-supplied offscreen
  capture `after-repairs/four-quick-modes.png` shows a complete 2×2 with
  the More button, no clipped row (inspected, not produced here).

## Camera-adoption repair — closed by inspection

`RememberAdoptedFormat` refuses stale generations, inactive state and
temporary ownership, and touches only `saved_.formatStableId`
(`src/app/agent/camera_source_intent.cpp:16-19`). The single replaced call
runs immediately after successful `Activate(starting…)` under the same
temporary-empty guard (`src/app/app_pipeline_runtime.cpp:2120-2140`), so
origin/generation/operation survive to the `MarkFirstFrame(starting…)` and
reconnect-expiry checks (`:1499,3230`). Genuine user format changes still
use `SetSavedFormat`'s Manual request (`src/app/app_controls.cpp:358`);
saved-link ownership stays with `LoadSaved`/`SelectManual`/
`FillStartupIdentity`. Four origins + stale + temporary are covered at
`tests/agent_control_tests.cpp:310-327`; the native expiry assertions are
unchanged (`tests/native_app_control_fixture.cpp:2288-2294`).

## Confidence and runtime limits

Source confidence is high for the traces above (every cited function and
caller chain was read in the candidate tree). Runtime confidence is
unchanged by this review: builds, focused gates and the matrix are
root-owned and in flight, and native Windows popup-grab delivery,
NVDA/Narrator announcement delivery, real focus restoration, and the
native reconnect-expiry pass remain unproven here. Docs/translations were
still in flight and are excluded, as are the deferred Steady-recipe and
color-preservation topics.
