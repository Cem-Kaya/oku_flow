# Player and quick-mode UI cleanup (2026-10-25)

The owner's three native screenshots showed a permanently visible volume row,
a large UCT transcript outside its collapsed advanced section, and ten quick
modes competing in one grid. Muse Wonder Full Max and Grok 4.7 Extra High
reviewed those screenshots and the implementation. This change uses the
existing Qt controls, icons, three tabs and large-target visual style.

## Applied behavior

- Saved-video volume is a vertical popover above the speaker. First activation
  opens it; another activation toggles mute while it remains open. Zero mutes,
  a positive adjustment unmutes, and Unmute restores the last audible session
  level if necessary. Escape returns to the speaker; Tab/Shift+Tab leave it.
  Hidden controls and source changes dismiss it without changing audio.
- UCT model selectors, backend details, status and recognized text share the
  existing collapsed advanced disclosure. Enable and Show original remain
  outside. Closing it retains recognition and text. Its accessible description
  follows status, and important status announcements target the visible header
  while closed. Positioned viewport text is independent of this optional pane.
- Four featured modes lead the picker: Read a Page, Document, Projector Screen,
  Whiteboard. Custom modes follow. More modes exposes the other six built-ins;
  a selected older mode remains available when the list is collapsed. All ten
  identifiers and recipes remain resolvable by settings and agent controls.
  Featured/custom rows alone receive shortcuts; legacy tiles cannot remap keys
  5–9. More preserves the inspected candidate without applying it. Four tiles
  use two columns when space permits and one on narrow views.

Review repairs preserve the listening preference while recording forces
silence. Volume/mute edits during active or finishing recording cannot modify
stored gain or mute behind the slashed icon. Popup dismissal captures focus
ownership before hiding; outside clicks and other windows retain their input.
Mute changes while open produce one polite accessible announcement.

The reviewers proposed different recipe/default changes. This change keeps
the existing new-install default, saved configurations, recipes, colors and UCT
opt-in. It promotes Document without renaming it to Read a Page. Further
recipe changes, including the existing Keep It Steady stabilization setting
and preservation of reading colors across preset application, are separate
work. Neither is claimed repaired here.

Four source labels were added to the translation catalog: More modes,
Document, Projector Screen and Whiteboard. All 96 target entries received
read-only model review; seven proposed replacements were applied. The 24
catalogs contain 895 complete source keys. This is linguistic model review,
separate from native-speaker, RTL rendering and spoken screen-reader checks.

## Evidence and limits

Private source reviews, original screenshots, unchanged failed attempts and
actual offscreen QWidget captures are retained under
`Z:/Projects/OkuFlow/local-work-data/ui-cleanup-20261025-r1/`.
The focused transport, UCT and actual-app fixture checks cover interaction
signals, volume restoration, accessible value/name synchronization, retained
hidden text, and legacy/custom mode preservation. Preview-only setup failures
were retained: the trigger is a QToolButton, and the offscreen platform cannot
activate native floating tool windows. Capture now occurs synchronously after
opening the actual widget. Preview font loading is explicit; these images are
fixture renders, not native desktop captures or screen-reader evidence.
New outside-click checks assert closure, exactly one delivery to the clicked
control and no mute command/focus redirection. The offscreen plugin reports no
active OS window after its popup grab; that synthetic condition cannot prove
the clicked control regains native focus. Speculative receiver-focus/replay
changes were removed instead of changing native input behavior for that plugin.
Explicit Escape, Tab/Backtab, resize, source-loss, collapse and hide destinations
remain asserted separately. All failed diagnostic attempts remain retained.

The owner's running application is left untouched. Compilation uses a separate
runtime output location, preserving its executable. GUI tests remain excluded
while the desktop is in use. Background checks do not establish native focus,
Windows stacking, acoustic playback, NVDA/Narrator or installer acceptance.

The broader matrix exposed an independent camera-reconnect defect: reader
adoption saved a format through the manual-edit API, which changed the request's
origin and generation. A silent adopted reader could then survive the recovery
deadline. `CameraSourceIntent::RememberAdoptedFormat` records that preference
without changing authority, and rejects stale/temporary attempts. Manual format
edits retain their existing supersession behavior. The original native expiry
assertion is unchanged; a separate unit covers all ordinary transition origins.

Final validation: the tracked background matrix compiled Release and passed
CPU **174 passed / 9 optional skipped / 0 unresolved**, plus the CUDA subset
**15 passed / 5 optional skipped / 0 unresolved**. Four CPU passing fixtures
contain skipped Qt data rows; their limits remain in the ledger. The separate
PCH-off UCT, transport and source-equivalence selection passed **3/3**. All
24 translation catalogs passed with 895 keys. Muse's final focused source
review retained no P1/P2 findings. These results remain partial desktop-free
development validation, not full GUI, acoustic or installer acceptance.
