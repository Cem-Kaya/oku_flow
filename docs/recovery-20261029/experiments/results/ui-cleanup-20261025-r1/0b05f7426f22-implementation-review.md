# UI cleanup candidate — implementation review (Muse, read-only)

Candidate: `Z:/Projects/T3-worktrees/oku_flow/t3-b137fab1`, branch
`fix/ui-hierarchy-volume-popover`, uncommitted from `cf7eaac3`, reviewed via
`implementation-review.patch` plus every changed production function and the
relevant surrounding callers/tests in that tree. The primary
`feature-uct-recap-player` checkout was not used as review evidence.
Prior reports read: `muse/design-review.md`, `grok/findings.md`.
Nothing was built, run, or edited.

**Verdict:** one P1 (forced-mute state mutation, §1) should be fixed before
merge. The rest are P2 accessibility/focus/spec-deviation items and test
gaps. The three settled moves themselves — UCT disclosure placement,
above-speaker vertical popover with app-owned last-audible restore, and the
four-featured preset diet — are implemented as reconciled.

## P1

### 1. Volume/mute edits mutate state behind a forced (recording) mute

`PollVideoPlayback` folds forced silence into the transport snapshot:
`outputMuted = kind != File || doc.muted || doc.active || doc.finishing`
(`src/app/app_video_playback.cpp:1298`), `ui.muted = outputMuted` (:1329).
That forced state is reachable with the speaker visible: reviewing a saved
file while a lecture recording is active/finishing (the timeline update at
:1110 keeps running for `kind == File`). The candidate then misbehaves:

- `RefreshText` maps any muted snapshot to slider 0
  (`src/ui/video_transport.cpp:713`), so the first keyboard step emits
  `VolumeChanged(5)`. The new app handler (`app_video_playback.cpp:224-232`)
  overwrites the stored gain (e.g. 50 → 5), overwrites
  `lastAudibleVolumePercent` with 5, and clears `doc.muted`. The next poll
  resets the thumb to 0, so repeated Up keys can never pass 5%.
- A mouse drag (thumb protected by `isSliderDown`) or a second speaker
  click sets `doc.muted = false` behind the forced mute. The snapshot
  keeps reporting muted, the speaker keep saying "Unmute playback", and
  there is no way to set the underlying mute back to true. When the
  recording finishes, the file becomes audible at the dragged level even
  though the UI showed the slashed/amber muted face throughout.

Audible output stays suppressed while forced (`audio.Configure(...,
outputMuted)` at :1301 is reasserted by the synchronous `PollVideoPlayback`
in both handlers), so this is a state-integrity/surprise-unmute defect, not
a live microphone-feedback leak. Smallest fix that preserves the settled
"slider zero = effective silence" presentation: in both app handlers,
treat edits as no-ops (still `PollVideoPlayback()` to resync the thumb)
while the forced predicate holds for a file (`kind == File && (active ||
finishing)`), instead of clearing `doc.muted`/rewriting gain. The same
guard also covers the currently unreachable `kind != File` signal paths.

## P2

### 2. Non-Escape popup closes do not own the focus destination

Only the Escape path restores focus (`HideVolumePopup()` default,
`video_transport.cpp:433-438,878-880`). Every other close passes
`false`: source change (:484), unavailable/mute hidden (:649), collapse
(:545 — which hides the popup *before* its own focus-rescue check at
:552-559), UI hidden (:535), window move/resize (:887-888). The slider is
the focus owner whenever the popup is open (`ShowVolumePopup` :430), so in
these paths the destination is whatever Qt chooses when a focused popup
window disappears; `ReturnFocus()` then early-returns because focus is no
longer a descendant (:675-676). Scenario: a keyboard user adjusting volume
loses their place entirely when the file ends/errors, the bar collapses, or
the window is resized, instead of landing on the speaker/Return/play.
Fix: centralize in `HideVolumePopup` — if focus is inside the popup, move
it (speaker if ready, else play/viewport) *before* hiding, keeping the
outside-click path exempt so the incoming press can set focus itself.

### 3. The application-wide event filter is not scoped to the popup/transport

`qApp->installEventFilter(this)` (`video_transport.cpp:311`) makes the
first block of `eventFilter` run for every event in the process while the
popup is visible (:868-882):

- Any `Escape` anywhere — e.g. meant to cancel a modal dialog that appeared
  over the popup — is consumed to close the popup and steal focus back to
  the speaker.
- Any left press whose *global* position falls inside the speaker rect
  emits `MutedChanged` and is swallowed (:872-875), even if the press was
  destined for an overlapping tool/modal window at those coordinates.

The second-click replay itself is single-dispatch on the synthetic path
(press eaten → no `clicked`), and application filters run before the
receiver, so the design is sound; it just needs scoping. Require the
watched object's window to be the popup or the transport's window (that
still covers the native popup-grab case where the receiver is the popup
window) before applying the Escape/speaker-rect rules. Note the same
object is also installed as an object filter on `volumePopup_`/`volume_`/
buttons, so non-consuming branches (e.g. `FocusOut` at :889-892) run twice
and queue duplicate idempotent `UpdateActions` — harmless, but another
reason to scope the global block.

### 4. Mute/unmute is never announced

Both design reports asked for one polite mute-edge announcement. Focus sits
on the slider while toggling; the only feedback is the slider value
changing (stored % ↔ 0) and the unfocused speaker's name flipping
(`RefreshText` :704-706). "0%" does not distinguish muted-at-50% from
gain-zero. Fix: on a `snapshot_.muted` edge in `SetSnapshot` while the
popup is visible, post one `QAccessibleAnnouncementEvent` with the
existing "Mute playback"/"Unmute playback" source strings plus the locale
percent, deduplicated like `AnnounceSettledState`.

### 5. Demoted presets still get number badges and number keys

Grok's settled numbering was keys 1–4 for the featured modes, 5–9 for
customs only, none for demoted stock. The candidate reuses row-based
numbering: the `rowsInserted` handler badges every row < 9
(`src/ui/main_window.cpp:2165-2179`), `UpdateCurrentPresetUi` mirrors it
on the carousel (:3685-3690), and the Simple-mode key handler activates
any row (:4833-4839). Consequences: with More expanded (a state that
persists for the session after first use) keys 5–9 activate legacy modes;
a pinned legacy selection also receives a number after the customs; and
collapsing/expanding More silently remaps what keys 5–9 mean — a
muscle-memory hazard for the keyboard users this grid serves. If the
parent accepts visible-row numbering instead, this is a decision to record
in the docs/help text; otherwise mark shortcut eligibility per item in
`PopulatePresetList` (featured/custom only) and gate both the badge and
the key path on it (clicks still use `ActivatePresetRow` for any row).

### 6. Toggling More discards the grid-browsing candidate

`OnQuickModeOptionsChanged` (`src/app/app_settings.cpp:46-49`) always ends
with `RefreshPresetSelection(true)`, which reselects the *applied* preset
(`MatchPreset` preserve path, `settings_controller.cpp:227-229`). A user
arrow-browsing the grid who tabs to More modes and toggles it silently
loses the candidate they were inspecting and is returned to the applied
row with no announcement. Cancel semantics stay correct (the toggle lambda
recomputes `gridOriginalRow_` from the refreshed current row,
`main_window.cpp:2085`), so this is a browsing-continuity issue only.
Fix: when `gridBrowsing_` is active, capture the browsed preset id before
`PopulatePresetList` and reselect it afterwards if still present.

### 7. Test gaps against the settled contracts (nothing here was executed)

Existing additions are meaningful as far as they go (grid order/pin/custom
preservation and config non-mutation in `native_app_control_fixture.cpp:
920-957`; app-level zero/restore sequencing at :960-980, albeit via direct
signal emission; UCT visibility/focus-rescue/text retention in
`ultra_clear_text_tests.cpp:795-821`; popover value/locale/no-seek in
`video_transport_tests.cpp:931-986`). Missing or too weak:

- `speakerOpensVolumeThenMutesWithoutOptimisticState` (:767-798) never
  asserts the `MutedChanged` *count* after clicks, so a double dispatch
  (filter + `clicked`) would pass unnoticed; "stays open" is only implied
  by the later accessible-name check. Assert size increments of exactly
  one and `popup->isVisible()` after the second activation, for mouse and
  for Space-on-slider.
- No coverage: focus == slider immediately after opening (the popover test
  re-focuses manually at :958, masking a `ShowVolumePopup` focus failure);
  Tab/Backtab exit targets; outside-click close; window move/resize close;
  muted snapshot mapping to slider 0 with stored gain retained in the app;
  the forced-mute scenario in §1; wheel gating.
- UCT: no assertion touches the new announcement or the toggle's
  accessible description (collapsed important vs expanded, language
  switch). `CaptureAnnouncement`-style capture already exists in the
  transport tests and can be reused.
- Presets: no activation of a legacy/custom row after a More toggle, and
  no number-key/badge assertions (would have pinned down §5 either way).

## Checked clean (by inspection)

- **UCT disclosure:** backend/status/pane are inside `uctAdvancedContents`
  with object names, buddies, focusability and `UpdateDependentState`
  semantics preserved (`ultra_clear_text_panel.cpp:73-147,299-306`);
  collapse rescues focus to the toggle before hiding (:110-115).
  `SetStatus` (:201-219) stores the source via
  `SetLiveAccessibleDescription`, which re-derives through the formatted-
  template translator on language change (`ui_translation.cpp:411-423`),
  and announces exactly once per change: explicitly on the header when
  important+collapsed, politely on the label when expanded.
  `DescriptionChanged` is not an announcement, so there is no double
  speech. The pre-existing `!backendLabel()->isHidden()` assertion
  (`ultra_clear_text_tests.cpp:6282`) remains consistent with the explicit
  `setVisible(true)` in `SetBackendStatus`; `isVisible()` is correctly
  false while collapsed. (Grok's predicted failure there does not apply.)
- **Volume sequencing (non-forced):** drag/key to 0 mutes and retains the
  last audible level; > 0 unmutes at the chosen value; unmute at volume 0
  restores `lastAudibleVolumePercent` *before* `player.SetMuted(false)`
  (`app_video_playback.cpp:212-232`); recording kind stays muted; no
  optimistic toggling (all faces remain snapshot-driven). Second
  activation stays open per the reconciliation, by design.
- **Lifetime/dispatch:** the popup is an owned child; the destructor
  removes filters before hiding (`video_transport.cpp:403-408`); press
  consumption prevents a paired `clicked`; slider Space is autorepeat-
  guarded (:950-953); source/UI-hide/collapse/deactivate all close.
- **Presets:** featured ids exist exactly once in storage order with blobs
  and `DefaultPresetId()` untouched (`settings_store.cpp:289-307,1214`);
  customs keep order between featured and legacy rows; a saved legacy
  selection is pinned while collapsed; `MatchPreset`/`ResolvePreset` still
  search every built-in, so agent ids and demoted selections resolve;
  repopulation runs under `SuspendGuard` and no handler dereferences a
  deleted item (`previous` is only pointer-compared in
  `UpdateCurrentPresetUi`); `quickModeActivated` re-fetches `item(row)`.
  Robustness note only: the `quickModeOptionsChanged` connection is
  (re)made inside `PopulatePresetList` relying on PMF `UniqueConnection`
  deduplication (`app_settings.cpp:21-22`), and `moreQuickModesShown()`
  dereferences the button unguarded — both safe on current call paths
  (post-construction), tidier if the connect moves to bootstrap.

## Limitations

Read-only review: no build, test, GUI, or provider action was taken, so
all pass/fail statements about tests are by inspection. Native Windows
behavior of the `Qt::Popup` grab (second-click replay and outside-click
delivery), NVDA/Narrator delivery of the collapsed UCT announcement, and
real focus restoration remain unverified and need the serial native GUI /
screen-reader pass. Docs and the four new catalog keys' 24-language parity
(`Document`, `Projector Screen`, `Whiteboard`, `More modes`, each present
exactly once in `translation_catalog.cpp`) were in flight under the parent
and are excluded. Deferred items from the design reports (steady-preset
stabilization flag, color preservation on apply) are absent as instructed
and are not counted against this candidate.
