# Plan 18 — Annotation Mode (draw on the lecture, never lose the ink)

Status: **R2 IMPLEMENTED — AUTOMATED GATES PASS; OWNER/HARDWARE ACCEPTANCE
PENDING (2026-07-28).** The shipped overlay is an owned frameless tool window rather than a
child widget because the D3D swap-chain HWND covers ordinary children on
Windows. It still tracks the render viewport exactly and uses the same
scene-coordinate model and `ViewTransform` specified below. The first
owner-review revision added explicit Line and Text items, one-item Move
selection with a visible handle box, a continuously adjustable width/text-size
slider, a flush-left vertical tool rail, and contextual settings immediately
to the right of the selected tool. A one-alpha backing surface fixes Windows
hit testing over the otherwise transparent D3D overlay.

The [R2 target UI](#r2--2026-07-28-owner-mock-target-draw-ui) is implemented:
Shape, swatch-grid color picking, Solid/Dashed styles, transient options
flyouts, the right-edge action rail, Paint-style scale handles, explicit
wheel/key navigation forwarding, the Advanced action bar, and the documented
contrast/text fixes. Model/settings/viewport CPU tests and a Release Windows
build pass. The remaining unchecked acceptance items below require live
mouse, trackpad, D3D viewport, screen-reader, and notes-output verification;
they are not silently claimed by the automated gate.

## The idea (owner's words, developed)

A pen button in the bottom-right control cluster. Clicking it enters
**annotation mode**: a small tool panel appears and the student can draw
directly on the live camera view — circle a formula on the board, underline
a word, mark the diagram the lecturer is pointing at. A pointer tool lets
them click a drawn stroke and move it; an eraser removes strokes; Clear
wipes the canvas. Nothing drawn is ever silently lost: when annotations are
cleared (or the mode is exited with ink on screen), the annotated view is
captured as an image and appended to the lecture-notes HTML. The panel also
carries a photo button so a clean frame capture lands in the notes with one
click from the same place.

Why this fits the mission: a legally blind student following a lecture
through a magnifier constantly loses their place. Ink that *sticks to the
board content* while they pan and zoom is a place-keeping aid, and the
capture-on-clear rule turns transient marks into permanent study notes
without any extra effort during the lecture.

## What already exists (build on it, don't duplicate)

- **Overlay pattern.** `RenderWidget` (the D3D12 present surface) already
  hosts child QWidget overlays: `AssistiveOverlay` and `JoystickOverlay`
  (`src/ui/main_window.cpp:4-5`, overlay lookup at `main_window.cpp:2212`).
  The annotation canvas is a third overlay of the same kind.
- **Bottom-right chrome panel.** `bottomRightPanel_` is built at
  `src/ui/main_window.cpp:1062-1077` and holds Photo / Record / Explain /
  Read buttons with 88×58 px minimums (`main_window.cpp:201`). The pen
  button joins this row.
- **Photo → notes pipeline.** `capturePhotoButton_` sets
  `photoCapturePending_` (`src/app/app_bootstrap.cpp:198-203`); the
  pipeline pairs it with the async presentation readback
  (`src/app/app_pipeline_runtime.cpp:637-641`, completion at `:669-677`),
  saves via `SaveCapturedPhotoPair` (`:1297`), and already appends the
  processed photo to the notes HTML via
  `assistiveManager_->Runtime().NoteCapturedPhoto(processedPath)`
  (`:1325`). The "photos into lecture notes" half of the idea is DONE —
  the panel only needs to expose the existing action.
- **Notes HTML.** `AssistiveRuntime` owns the notes file
  (`NOTES_*.html`, `src/common/assistive_runtime.cpp:940`), with
  `AppendNoteSection(heading, text, imagePath)` (private) and
  `NoteCapturedPhoto` / `notesFilePath()` public
  (`include/okuflow/common/assistive_runtime.hpp:88-93`).
- **Canonical geometry.** `ViewTransform` + `ComputeViewTransform` +
  `ComputePixelViewMapping` (`include/okuflow/common/view_transform.hpp`)
  already define the scene↔viewport mapping used by shader, CPU path, and
  ROI remap. Annotations reuse it; no new geometry math.

## Core design decisions

### D1. Strokes are vectors anchored in scene space

Each stroke is a polyline of points in **normalized processed-frame
coordinates** (0..1 across the camera scene), plus color, width, and id.
On every repaint the overlay maps points through the current
`ViewTransform` to widget pixels. Consequences:

- Ink sticks to board content during pan/zoom — the whole point.
- Move/erase/undo operate on whole strokes (objects), not pixels.
- Snapshot burn-in is a CPU-side `QPainter` pass using the same transform —
  resolution-independent, no GPU work.

The phone is physically clamped, so camera space ≈ board space. If the rig
is bumped, ink drifts with the old framing — accepted v1 limitation; see
Future for the optical-flow re-anchor idea.

### D2. The canvas is a Qt overlay, not a pipeline stage

`AnnotationOverlay : QWidget`, child of `RenderWidget`, resized to cover it
(same as `AssistiveOverlay`). `QPainter` with antialiasing draws each
stroke **twice**: a wider dark underlay, then the bright core color — a
telestrator halo, so ink stays visible over any video content (yellow
marker on a whiteboard, white chalk on black). Repaint triggers: stroke
model change, view-transform change, resize — NOT every 120 Hz present
tick. The overlay never touches D3D12/CUDA; the F1/F2 class of fence bugs
cannot recur here.

Mouse routing: when a drawing tool is active the overlay accepts mouse
events; in pointer mode outside a drag, and whenever annotation mode is
off, it sets `WA_TransparentForMouseEvents` so the existing pan/zoom event
filter (`main_window.cpp:2257-2284`) keeps working untouched. Wheel zoom
is always forwarded (never consumed) — zooming while inking is a core use.

### D3. Deletion is capture, not loss

- **Clear** first renders the current annotated view to a PNG and appends
  it to the notes HTML under a heading like "Annotations cleared —
  snapshot", then wipes the model. One confirmation-free click; undo can
  restore the strokes anyway.
- **Exiting annotation mode with ink present** does the same capture
  (setting-gated, default ON), then hides the overlay.
- **Individual erases do NOT capture** — erasing three stray strokes must
  not spam three images into the notes. Undo (D5) covers accidents. This
  is a deliberate refinement of the raw idea: capture at the moments of
  *bulk* loss only.
- A dedicated **Snapshot** button captures without clearing, for "I want
  this marked-up view in my notes and I'm not done yet".

### D4. Snapshot implementation — reuse the photo readback, burn in on CPU

No new GPU readback path (doc-16 lesson: every new fence consumer is a
race candidate). The snapshot request piggybacks exactly like the photo
path: set a pending flag, pair with the next async presentation readback
(`app_pipeline_runtime.cpp:637-641`), and on completion (`:669-677`) wrap
the BGRA buffer in a `QImage`, `QPainter`-draw the strokes using the
`ViewTransform` that produced that frame, save PNG next to the photos, and
append to notes. New public method on `AssistiveRuntime`:
`NoteAnnotationSnapshot(const QString& filePath)` (or generalize
`NoteCapturedPhoto` with a heading parameter). Timeout/abandon handling
copies the existing pending-photo timer (`:579-587`).

### D5. Tools, panel, and interaction model

The pen button (icon: pen; checkable) lives in `bottomRightPanel_`. When
checked, an **annotation toolbar** appears — same chrome styling, docked
adjacent to the bottom-right panel (or a second row of it), with big
high-contrast buttons:

| Tool | Behavior |
|---|---|
| Pen | Left-drag draws. Points captured with a min-distance threshold and light smoothing (shaky-hand friendly). Hold Shift: straight line from stroke start. |
| Pointer | Click hit-tests strokes (distance to polyline < threshold scaled by 1/zoom). Drag moves the stroke. Drag empty space for a marquee multi-selection; Shift/Ctrl extends it. Arrow keys nudge the selected item/group; Delete removes it. Selected strokes get a marching-dash or thicker-halo highlight. |
| Eraser | Click or drag over strokes deletes whole strokes (stroke-level, not pixel-level — pixel erasing demands precision low-vision users should not need). |
| Color / width | Small set of preset high-contrast ink colors (with halo, so all remain visible on any background) + S/M/L width. Defaults: thick, high-saturation. Persisted. |
| Snapshot | Capture annotated view → notes (D4), keep drawing. |
| Photo | The existing clean photo capture (already notes-wired). |
| Clear | Capture-then-wipe (D3). |
| Exit | Leave annotation mode (capture-if-ink per D3), uncheck pen button. |

Undo/redo: Ctrl+Z / Ctrl+Shift+Z while the mode is active, covering add,
move, delete, and clear (clear restores as one step).

Keyboard: every tool reachable by button focus + Space (standard Qt), plus
single-key accelerators while the mode is active (P pen, V pointer, E
eraser) announced in the buttons' accessible descriptions.

### D6. Accessibility rules (non-negotiable)

- Every button gets accessible name + description (follow the `setA11y`
  pattern, `src/ui/ai_settings_dialog.cpp:444`).
- Entering/leaving the mode and switching tools fire **screen-reader
  announcements only** — never TTS (TTS is strictly user-triggered,
  standing project rule).
- Hit targets match the existing 88×58 px chrome minimums; toolbar honors
  UI scale.
- Ink rendering is unaffected by the CUDA color-scheme processing (it
  composites after), so ink colors are predictable regardless of the
  active scheme/inversion — document this in the About text.
- Focus never gets trapped in the toolbar; Esc exits the mode from
  anywhere (same capture-if-ink rule).

### D7. State and settings

- New `AnnotationModel` (strokes, selection, undo stack) — plain C++/Qt,
  no GPU types, unit-testable in the msvc-cpu preset.
- Mode/tool state lives in `UiStateManager` alongside existing UI state;
  `OkuFlowApp` wires overlay ↔ model ↔ pipeline (snapshot flag) in
  `app_controls.cpp` / `app_bootstrap.cpp` following existing patterns.
- Settings (`SettingsStore`, new `annotations` object in settings.json):
  ink color, width, capture-on-exit toggle. Strokes themselves are
  session-only in v1 (not persisted across restarts) — the notes HTML is
  the durable record.

## Implementation phases

**Phase 1 — Canvas + pen + eraser + clear (2-3 days).**
`AnnotationOverlay`, `AnnotationModel`, pen button + toolbar panel, halo
rendering, scene-anchored mapping, stroke eraser, clear (without capture
yet), mouse routing that provably leaves pan/zoom intact. Acceptance:
draw at 1× zoom, zoom to 4× — ink stays glued to the board content; pan
works with pointer tool active; toolbar fully keyboard-operable; screen
reader announces mode and tools.

**Phase 2 — Pointer/move + undo (1-2 days).**
Hit-testing, drag-move, arrow-nudge, Delete, selection highlight, undo
stack for all operations. Acceptance: move a stroke, undo returns it
exactly; Delete + undo round-trips; hit radius feels right at both 1× and
high zoom (threshold scales with zoom).

**Phase 3 — Notes capture (1-2 days).**
Snapshot piggyback on the async readback, CPU burn-in, PNG save,
`NoteAnnotationSnapshot`, capture-on-clear and capture-on-exit (setting),
Photo button exposed in the toolbar. Acceptance: Clear with ink → notes
HTML gains an image section whose PNG shows frame + ink exactly as on
screen (same crop/aspect); no capture when clearing an empty canvas; the
readback timeout path recovers like the photo path does.

**Phase 4 — Polish (1 day).**
Color/width preference persistence, explicit straight-line and text tools, Esc
handling, Simple-mode Draw action, CHANGELOG + docs/code_reference.md + a11y
docs. The implemented toolbar remains compact by moving contextual color,
thickness, and text-entry controls to a separate left-side panel.

Tests (msvc-cpu, alongside existing suites): model operations + undo;
polyline hit-testing incl. zoom-scaled threshold; scene↔widget mapping
round-trip through `ViewTransform`; burn-in renders deterministic pixels
for a synthetic stroke; notes append writes the expected HTML section
(temp dir, follow existing settings_store_tests.cpp style).

## Risks / open points

- **Event-routing regressions** are the main hazard: the pan/zoom filter
  at `main_window.cpp:2257-2284` must behave identically when annotation
  mode is off. Gate: manual pan/zoom/joystick pass before merge.
- **Airspace**: a QWidget child over a native D3D12 surface works today
  for AssistiveOverlay (Qt handles native-child composition on Windows),
  but verify no flicker at 120 Hz re-present with the overlay visible on
  the real rig; if flicker appears, fall back to a transparent
  `Qt::Tool`-window overlay tracking the render widget geometry (the
  color-picker popover already ships that pattern).
- **Snapshot transform mismatch**: the burn-in must use the transform of
  the frame that was read back, not the transform at completion time —
  store it with the pending request (the photo path's pairing pattern
  already demonstrates the bookkeeping).
- Rotation/keystone presets: ink is anchored in *processed*-frame space
  (post-rotation), so changing rotation mid-session moves the content out
  from under the ink. Acceptable v1: switching presets clears ink via the
  capture-on-clear rule (announce it).

## Future (not in this plan)

- **Optical-flow re-anchoring**: when stabilization v2's motion vectors
  land, use the global shift estimate to re-anchor strokes after the rig
  is bumped.
- Text labels and shape tools (rectangle/arrow) as additional tools.
- Persisting ink across sessions keyed by camera preset.

---

# R2 — 2026-07-28 owner mock: target Draw UI

Source: owner mock screenshot (Simple mode, Draw active, Pen selected) plus
two follow-up instructions: (a) contextual option panels must **not stay open
24/7**, (b) zoom must **work while drawing**, (c) Move mode must show
**Paint-style scaling dots** around the selected item, (d) fix the bad
color/text contrast visible in the Advanced inspector screenshots, and
(e) the bottom-right action bar must **not disappear when the Advanced panel
is open** — it should shift left of the inspector, and the bottom bars should
squeeze/stack rather than overlap or vanish.

All `file:line` references verified against the working tree on 2026-07-28.

## R2.0 What the mock shows vs. what is shipped

| Aspect | Shipped today | R2 target (mock) |
|---|---|---|
| Tool rail | Text-only buttons Move/Pen/Line/Text/Erase, 54×46 px min (`src/ui/annotation_overlay.cpp:106-114`, `:147-163`) | Icon **+** label buttons, ~84×72 px min; order Pen, Line, **Shape (new)**, Text, Erase; Move kept (see R2.2-B) |
| Color choice | `QComboBox` with icon swatches (`annotation_overlay.cpp:179-188`) | 2×3 **swatch grid** of big color buttons, selected swatch outlined |
| Thickness | Horizontal slider + number (`:190-200`) | **Vertical** slider with live "N px" value below |
| Line style | Solid only | **Solid / Dashed** toggle pair in the flyout |
| Options panel | Always visible while mode is on; also hosts Undo/Redo/Save/Clear/Exit (`:215-230`) | Transient **flyout** next to the active tool; actions move out to a right-edge rail |
| Actions | Inside the options panel | Right-edge vertical rail: Undo, Redo, Save, Clear, **Done** (primary) |
| Move selection | Dashed box + 4 *decorative* corner dots (`src/common/annotation_model.cpp:489-510`) | 8 **interactive** scale handles (4 corners + 4 edge midpoints), Paint-style |
| Zoom while drawing | Broken — see R2.4 | Ctrl+wheel zoom (with plan-27 acceleration) and wheel pan work at all times |
| Advanced mode | `bottomRightPanel_` is hidden (`src/ui/main_window.cpp:2494`) | Bar stays visible left of the inspector; bottom bars stack when they'd collide |

Note on the mock: it does not show a Move button in the rail. Move must
still exist (the owner's scaling-dots instruction requires it); keep it as
the **first** rail item, matching the shipped layout and Paint's
select-first convention. Owner can veto the position, not the tool.

## R2.1 Layout map

```
+--------------------------------------------------------------------------+
| [OkuFlow]  [Simple|Advanced] [ ] Text Clarity          (top-left chrome) |
|                                                                          |
| +------+  +-----------------+                          +---------+       |
| | Pen* |  | Pen             |                          | Undo    |       |
| | Line |  |  [m][y][g]      |   <- flyout, transient   | Redo    |       |
| | Shape|  |  [c][b][w]      |                          | Save    |       |
| | Text |  |  Thickness      |                          | Clear   |       |
| | Erase|  |   |  8 px       |                          | Done*   |       |
| | Move |  |  [Solid][Dash]  |                          +---------+       |
| +------+  +-----------------+                              (right rail)  |
|  (left                                                                   |
|   rail)                    live camera view                              |
|                                                                          |
| [grid][<][ Read a Page 1 ][>]        [Photo][Record][Explain][Read][Draw]|
| (bottom-left chrome, untouched)              (bottom-right chrome)       |
+--------------------------------------------------------------------------+
```

- Left rail: flush-left, vertically centered in the safe area between the
  top-left and bottom-left chrome panels — the existing
  `PositionToolbar()` safe-area math (`annotation_overlay.cpp:529-552`)
  stays.
- Flyout: opens immediately right of the rail, top-aligned with the active
  tool's button (existing `:554-570` positioning stays), but is now
  **transient** (R2.3).
- Right rail: new, flush-right, vertically centered in the same safe area,
  mirroring the left rail's sizing. Positioned by the same
  `PositionToolbar()` pass.
- All three surfaces share the chrome style: `#111111` background, 3 px
  `#f4f4f4` border, 6-8 px radius (matches `QWidget#bottomRightPanel` at
  `main_window.cpp:188-191`).

## R2.2 Every button, exhaustively

### A. Chrome entry point (outside the overlay)

**Draw** (`annotationButton_`, `src/ui/main_window.cpp:332`, a11y `:1370`,
lives in `bottomRightPanel_` built at `:1163`).
- Checkable. Unchecked→checked: enters annotation mode — overlay shows,
  left rail + right rail appear, Pen is the active tool, Pen flyout opens
  once (R2.3), screen reader announces "Annotation mode on. Pen tool."
- Checked→unchecked (or Done, or Esc ladder): leaves the mode with the
  capture-if-ink rule (D3): if ink exists and *Save drawing on exit* is
  enabled, snapshot to notes first; then hide overlay and rails.
- The button shows the pencil icon (`assets/icons/lucide/pencil.svg`,
  qrc alias `draw.svg`) and stays visually checked the whole time the mode
  is active, in both Simple and Advanced modes (see R2.8).

The other chrome buttons visible in the mock are existing features and keep
their behavior exactly; listed so the spec covers every button on screen:
- **Photo** — clean frame capture into the photos folder + notes HTML
  (pipeline at `src/app/app_pipeline_runtime.cpp:637-677`). Works while
  drawing; captures the *clean* frame without ink.
- **Record** — start/stop recording; unrelated to ink. Recording captures
  the processed stream, never the overlay ink (document this in Help).
- **Explain** — VLM scene description (user-triggered).
- **Read** — OCR + read-aloud (user-triggered TTS; standing rule intact).
- **Simple / Advanced** toggle, **Text Clarity** checkbox, mode grid /
  prev / next / "Read a Page" carousel — untouched by this plan.

### B. Left tool rail — six tools

Shared button spec: icon 28 px + text label beneath, minimum 84×72 px
(honors UI scale), checkable, mutually exclusive (`QButtonGroup` as today,
`annotation_overlay.cpp:156-163`). Checked state = **filled** accent
`#a83cbe` with white icon/text (≈5.2:1 — the shipped border-only checked
state at `:115-118` is too subtle; the mock shows a filled purple Pen).
Focused-but-unchecked keeps the 3 px accent border. Every activation fires
the existing `SetTool` announcement (`:792-821`).

| # | Button | Icon (Lucide) | Shortcut | Click behavior | Canvas behavior while active |
|---|---|---|---|---|---|
| 1 | **Move** | `mouse-pointer` | V | Activates Move; no flyout (hint moves to tooltip + accessible description) | Click an item → select it, show dashed box + 8 scale handles (R2.5). Drag body → move. Drag handle → resize. Click empty → deselect. Arrows nudge (Shift = ×5), Delete removes, Ctrl+±/− resizes (R2.5). |
| 2 | **Pen** | `pencil` (exists) | P | Activates Pen; opens Pen flyout | Left-drag draws freehand with current color/width/style; min-distance point capture as today (`:684-687`). Release ends the stroke. |
| 3 | **Line** | `slash` (new asset) | L | Activates Line; opens Line flyout | Left-drag rubber-bands a straight line start→cursor (`SetStraightStrokeEnd`, `annotation_model.cpp`); release commits. |
| 4 | **Shape** | `square` (new asset) | S | Activates Shape; opens Shape flyout | Left-drag rubber-bands the chosen shape (Rectangle or Ellipse) between opposite corners; Shift constrains to square/circle; release commits. New — R2.6. |
| 5 | **Text** | `type` (new asset) | T | Activates Text and opens the Text flyout | Click the view → open an inline editor at that scene coordinate; type, then press Enter to place (`AddText`). Escape cancels. This canvas-first owner correction supersedes the earlier type-then-click mock interpretation. |
| 6 | **Erase** | `eraser` (new asset) | E | Activates Erase; no flyout | Click or drag across items deletes whole items (stroke-level, `:654-656`, `:690-691`). Undo restores. |

Shortcut note: `S` is new; the existing single-key map P/L/T/V/E
(`annotation_overlay.cpp:737-761`) gains one entry. All shortcuts remain
active-mode-only, never global.

### C. The contextual flyout — every control

One flyout frame (`optionsPanel_`), contents swapped per tool exactly as
`UpdateToolOptions()` does today (`:823-872`), restyled and made transient.
Title label (e.g. "Pen") uses the new readable accent `#d79ae6` (R2.7),
not `#bd52d3`.

**Color swatch grid** (replaces `colorCombo_`, shown for Pen/Line/Shape/Text):
- Six fixed swatches in a 3×2 grid, per the mock:
  Magenta `#ff4bd8`, Yellow `#fff000`, Green `#55ff55`,
  Cyan `#00e5ff`, Blue `#2979ff`, White `#ffffff`.
  (Blue replaces the current Orange preset `#ff9d00` per the mock; any
  persisted non-preset color still loads and simply shows no highlighted
  swatch.)
- Each swatch: flat color button ≥48×36 px, 2 px `#080808` inner outline so
  Yellow/White read as buttons on the dark panel; selected swatch gets a
  3 px white outer ring + checkmark glyph in a contrasting color
  (selection must never be color-only — WCAG 1.4.1).
- Accessible name = color name ("Yellow ink"); clicking announces
  "Yellow" and emits the existing `PreferencesChanged` signal
  (`:279-284`). Arrow keys move between swatches when the grid has focus.
- Selecting a color does **not** close the flyout (users often set color +
  width together); the close rules are R2.3.

**Thickness slider** (Pen/Line/Shape; becomes "Text size" for Text):
- Vertical `QSlider`, ~160 px tall, range 2-24 px (12-72 for text), value
  label "*N* px" beneath, as in the mock. Keeps the wheel-block filter
  (`:441-444`) and the dual-purpose text-size behavior (`:285-295`).
- Accessible name/description unchanged; Qt announces value changes.

**Style toggle** (Pen/Line/Shape): two exclusive buttons **Solid** and
**Dashed**, each showing a sample line glyph. Sets the new per-stroke
`dashed` flag (R2.6). Persisted. Announced ("Dashed line style").

**Text editor** (Text only): clicking the camera creates a `QLineEdit` beside
that scene position. Enter commits the label, Escape cancels it, and
Save/Clear/Done commit non-empty pending text first. The editor is transient
canvas chrome rather than a permanently visible flyout field.

**Removed from the flyout:**
- The action row (Undo/Redo/Save/Clear/Exit) — moves to the right rail (D).
- The **"Save on exit" checkbox** (`:207-209`) — moves to the Advanced
  inspector, Assistant section, next to *Open Notes*
  (`src/ui/main_window.cpp`, Assistant group): checkbox
  **"Save drawing to notes when leaving Draw"**, same settings key, same
  `PreferencesChanged` plumbing. Rationale: it is a preference, not a
  per-stroke option, and the flyout must stay small.
- The hint label (`optionsHintLabel_`) — hints move to tooltips/accessible
  descriptions; the flyout carries controls only.

### D. Right action rail — five buttons

Same button spec as the left rail (icon + label, 84×72 min, chrome style).
Top to bottom, exactly as in the mock:

| Button | Icon (Lucide) | Shortcut | Behavior | Enabled when |
|---|---|---|---|---|
| **Undo** | `undo-2` (new) | Ctrl+Z | `model_.Undo()` — reverts add/move/resize/delete/clear as one step each | `canUndo()` (`UpdateUndoButtons`, `:883-887`) |
| **Redo** | `redo-2` (new) | Ctrl+Shift+Z / Ctrl+Y | `model_.Redo()` | `canRedo()` |
| **Save** | `save` (new) | Ctrl+S (mode-local) | `SnapshotRequested` → burn-in capture to notes **without clearing** (D4). Announce "Saved to notes." | Ink exists (`HasInk()`); disabled otherwise with description "Draw something first — Photo takes a clean picture." |
| **Clear** | `trash-2` (new) | — (deliberately no shortcut) | `ClearRequested` → capture-then-wipe (D3). Undo restores the ink (capture already happened — that is the design, not a bug). | Ink exists; disabled on an empty canvas so an accidental press can't spam empty images into notes |
| **Done** | `check` (new) | Esc (via ladder, R2.3) | Primary action, accent-filled like the mock. `ExitRequested` → capture-if-ink per the setting, exit mode, uncheck the chrome Draw button, focus returns to the Draw button. | Always |

Renames vs. today: "Exit" → **"Done"** (mock wording; accessible name
"Done — leave annotation mode"). Undo/Redo disabled-state colors must meet
the R2.7 disabled floor.

### E. Keyboard map (mode-active, overlay focused)

| Key | Context | Action |
|---|---|---|
| P / L / S / T / V / E | always | Switch tool (opens that tool's flyout per R2.3) |
| Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y | always | Undo / Redo / Redo |
| Ctrl+S | always | Save to notes (= Save button) |
| Esc | ladder | 1st: close flyout if open → 2nd: clear selection if any → 3rd: Done (capture-if-ink). Announce each rung. Replaces today's immediate-exit Esc (`:768-772`). |
| Delete | selection exists | Delete selected item (already conditional, `:762-767`) |
| Arrows / Shift+Arrows | selection exists | Nudge 2 px / 10 px equivalent (`:773-788`) |
| Arrows, PgUp/PgDn, +/− | **no** selection | Forwarded to the main window so keyboard pan/zoom keeps working (R2.4) |
| Ctrl+Plus / Ctrl+Minus | selection exists | Resize selection ±5 % about its center (Shift: ±15 %) — R2.5 |
| Tab | always | Cycles: left rail → flyout (only if open) → right rail → canvas |

## R2.3 Flyout lifecycle — "not open 24/7"

State: `flyoutOpen_` bool + `QPointer` to the frame. Rules, exhaustive:

**Opens when:**
1. A tool that has options (Pen/Line/Shape/Text) becomes active — by rail
   click or shortcut. (Entering the mode activates Pen, so the flyout is
   seen once on entry, matching the mock.)
2. The already-active tool's rail button is clicked/pressed again —
   re-click toggles the flyout back open. This is the discoverable "get my
   options back" gesture; announce "Pen options" / "Pen options closed".

**Closes when:**
1. The user presses the canvas — the same press that starts the
   stroke/placement. **One press, both effects**: collapse the flyout and
   begin the stroke; never swallow the click and demand a second one.
2. The tool switches to Move or Erase (they have no flyout).
3. Esc (first rung of the ladder).
4. The mode exits (Done / chrome Draw / Esc ladder).
5. Text-tool special case: after a successful text placement the flyout
   collapses; while the text field is empty it stays open (the field is
   the tool). Re-open via rule 2 or shortcut T.

**Never:** timer-based auto-hide. A panel that disappears while a
low-vision user is still finding it is hostile; closing is always caused
by an explicit user action.

**Focus:** while closed, flyout controls leave the tab order. Closing
returns focus to the active tool's rail button. Opening puts the flyout
immediately after that button in the order. Screen-reader users get the
open/close announcements above.

Implementation: `SetFlyoutOpen(bool)` in `AnnotationOverlay`; call sites in
`SetTool` (`annotation_overlay.cpp:792`), `mousePressEvent` (`:618`),
`keyPressEvent` Esc branch (`:768`), and the rail re-click handler.
`PositionToolbar()` skips the flyout when closed.

## R2.4 Zoom and pan must work while drawing — confirmed bug + fix

**The bug (real, shipped today).** The overlay is a **top-level**
`Qt::Tool` window (`annotation_overlay.cpp:71-76`). Its `wheelEvent` calls
`event->ignore()` expecting propagation to the render widget (`:716-721`),
but Qt's wheel propagation walks `parentWidget()` and **stops at window
boundaries** — the overlay *is* a window, so the ignored event dies there.
Ctrl+wheel zoom and wheel pan are dead across the entire camera view
whenever Draw is active. The comment at `:717-719` documents intent, not
behavior.

**The fix — explicit forwarding.** In `AnnotationOverlay::wheelEvent`:
1. If the wheel position is over the rails or the open flyout → do nothing
   special (let those widgets scroll normally; the width slider already
   blocks wheel via the event filter at `:441-444`).
2. Otherwise construct a new `QWheelEvent` with the position mapped into
   `renderTarget_` coordinates (map via global: overlay and target share
   geometry by construction, `SyncGeometryToRenderTarget()` `:504-515`,
   but mapping through `mapToGlobal`/`mapFromGlobal` is robust to rounding)
   and `QCoreApplication::sendEvent(renderTarget_, &forwarded)`.
3. `MainWindow::eventFilter` watches `renderWidget_` and already routes
   Ctrl+wheel → `HandleZoomWheel` (accelerated, plan 27 Phase 5,
   `src/ui/main_window.cpp:2713-2720` →
   `src/app/interaction_controller.cpp:179-224`) and plain wheel →
   `HandlePanScroll` (`main_window.cpp:2721-2724`). Event filters run on
   `sendEvent`, so both paths light up with zero new logic.
4. No recursion risk: the overlay is not in `renderTarget_`'s child chain,
   and the forwarded event is delivered to the target only.

**Semantics while a stroke is in progress.** Forwarding stays active
during a left-button drag. Scene anchoring makes this correct for free:
every subsequent mouse point is mapped through the *current* transform
(`MapViewPointToScene`, `:573-601`), and `SetViewTransform` repaints on
change (`:341-356`). So the user can draw a long underline, Ctrl+scroll to
tighten the zoom mid-stroke, and keep drawing — the ink stays glued to the
board content. Add this exact scenario to the acceptance list.

**Keyboard parity.** Keys the overlay does not consume must reach the main
window, or keyboard-only users lose zoom/pan the same way wheel users do
today. In `keyPressEvent`'s fall-through (after `:773-789`): if the key is
not in the mode's map *and* there is no selection consuming arrows,
forward the event to the main window via `QCoreApplication::sendEvent`
before falling back to `QWidget::keyPressEvent`. Explicitly forward:
arrows (no selection), PgUp/PgDn, plus/minus zoom keys — whatever
`MainWindow::keyPressEvent`/`HandlePanKey` (`main_window.cpp:2551-2566`)
handles. Never forward keys the overlay consumed.

## R2.5 Move mode — Paint-style scale handles

Today: selection renders a dashed cyan box with 4 corner dots that are
**pure decoration** (`annotation_model.cpp:489-510`); the only operations
are whole-item drag, nudge, delete. Target: the 8-handle resize model
every Paint user knows.

**Visuals** (in `RenderAnnotationStrokes`, selected branch):
- Bounds: `path.boundingRect()` + 10 px margin, as today.
- 8 handles: 4 corners + 4 edge midpoints. 14×14 px squares (up from
  10 px), `#00e5ff` fill, 2 px `#080808` outline — screen-constant size
  regardless of zoom (they are drawn in view space; nothing to convert).
- Body cursor `SizeAllCursor`, corner cursors `SizeFDiag`/`SizeBDiag`,
  edge cursors `SizeHor`/`SizeVer`, default `ArrowCursor`; the overlay
  already has mouse tracking on (`annotation_overlay.cpp:81`).

**Shared geometry helper.** Extract the per-stroke path construction out
of `RenderAnnotationStrokes` (`annotation_model.cpp:438-467`) into
`QPainterPath BuildStrokePath(stroke, transform, destinationSize)` used by
both the renderer and the overlay. The overlay then computes
`SelectionBoundsInView()` and the 8 handle rects in view space. This also
fixes the text case for free: text bounds come from the same
`QFontMetricsF` path the renderer uses — no scene-space font math.

**Hit priority** in Move-tool `mousePressEvent` (`:657-666` today):
1. Handle hit (within 12 px of a handle center) → begin **resize**.
2. Stroke body hit (`SelectAt`, existing tolerance `:613-616`) → begin
   **move** (existing behavior).
3. Empty → clear selection (existing behavior).

**Resize semantics:**
- Corner drag: scales both axes; **anchor = the opposite corner** (it does
  not move). Default is uniform scale (factor from the diagonal ratio) —
  predictable for low-vision users; **Shift = free** (independent x/y),
  the inverse of Paint's convention, deliberately: uniform-by-default
  never produces accidentally squashed circles.
- Edge drag: scales one axis; anchor = the opposite edge; Shift has no
  effect.
- Text items: always uniform (glyphs must not stretch); any handle acts as
  a corner.
- Minimum: bounds may not shrink below 24 px on screen on either axis;
  dragging past the anchor clamps (no mirror-flip in v1).
- Stroke thickness scales with the item: `sceneWidth *= sqrt(sx*sy)`,
  clamped to the slider-range equivalents, so a resized circle keeps its
  visual weight, like Paint.

**Model API** (mirrors the move-selection trio at
`annotation_model.hpp:72-75`, one undo snapshot per gesture):

```cpp
bool BeginScaleSelection();                       // PushUndo() once
bool ScaleSelection(const QPointF& anchorScene,   // p' = a + (p-a)*(sx,sy)
                    qreal sx, qreal sy);          // absolute since Begin
void EndScaleSelection();
bool ScaleSelectionAboutCenter(qreal factor);     // keyboard path
```

Scale factors are unitless ratios computed in view space by the overlay
and applied unchanged in scene space — valid because the view↔scene
mapping is axis-aligned affine (`view_transform.hpp`). The overlay stores
the gesture's start bounds and anchor at press time and derives absolute
`sx, sy` each move (no incremental drift).

**Keyboard resize:** with a selection, Ctrl+Plus / Ctrl+Minus scale ±5 %
about the item center (Shift: ±15 %), via `ScaleSelectionAboutCenter`.
Announce "Resized to N percent of original" (track the cumulative factor
per selection). This is the accessibility-required equivalent of the
handle drag — handles alone are not keyboard-operable.

**Announcements:** on select — "Selected [freehand stroke | line | shape |
text 'foo']. Drag to move, use handles or Control Plus and Minus to
resize." On resize end — the percent announcement. On deselect — "Nothing
selected."

**Tests** (extend the model suite in `tests/`): corner scale about
opposite-corner anchor is exact for a synthetic rectangle; uniform
default vs Shift-free; min-size clamp; text stays uniform;
`sceneWidth` sqrt rule; one undo step per gesture round-trips; keyboard
center-scale; `BuildStrokePath` bounds match renderer bounds for all four
item kinds.

## R2.6 Model additions — Solid/Dashed and the Shape tool

**`dashed` flag.** `AnnotationStroke` gains `bool dashed{false}`
(`annotation_model.hpp:34-43`). Rendering: apply the **same** dash pattern
to both the halo pen and the core pen with matched `dashOffset`, so the
gaps are genuinely transparent — a solid halo under a dashed core would
read as a black line with colored dashes, which is wrong. Pattern
`{4.0, 3.0}` in units of the core width (`Qt::CustomDashLine`), round
caps as today (`annotation_model.cpp:469-487`). Applies to freehand,
line, and shape kinds; ignored for text.

**Shape kinds.** `AnnotationItemKind` gains `kRectangle` and `kEllipse`.
Storage: exactly two points — the drag's opposite corners in scene space
(reuses `BeginStroke`/`SetStraightStrokeEnd` mechanics; the Shape tool is
the Line tool with a different kind). Rendering in `BuildStrokePath`:
`addRect`/`addEllipse` on the view-mapped corner rect. Hit-testing: the
existing distance-to-polyline test works once `HitTest` samples the shape
outline (rect = 4 segments; ellipse = 32-segment polygon) — keep the
sampling inside the model so the overlay stays geometry-free. Shift while
dragging constrains to square/circle (equalize the view-space extents
before mapping back). The flyout's Rectangle/Ellipse choice is an
exclusive two-button pair, persisted, announced.

## R2.7 Color, contrast, and text fixes (owner screenshots, 2026-07-28)

Every item verified against the code; fix locations are exact.

| # | Defect (screenshot evidence) | Root cause | Fix |
|---|---|---|---|
| 1 | "Shared by every profile" / "Saved into the selected quick option" scope captions are nearly invisible | `QLabel#scopeSubtitle { color: palette(mid); }` — `src/ui/main_window.cpp:172`; palette(mid) is a ~#5a5a5a gray, ≈2.4:1 on the dark panel | Explicit `#b3b3b3` (≈9:1 on `#111`). Keep the smaller font; contrast, not size, was the failure |
| 2 | Purple headings ("Profile", section labels) are muddy on dark | `QLabel#sectionLabel { color: palette(highlight); }` (`main_window.cpp:259`) and `QLabel#annotationOptionsTitle { color: #bd52d3; }` (`annotation_overlay.cpp:130-133`); `#bd52d3` on `#111` ≈4.8:1 — passes AA-large but reads badly on real panels | New **accent-text token `#d79ae6`** (≈8.7:1) for any accent-colored *text*; `#bd52d3` stays for borders/fills only. Apply to both stylesheets |
| 3 | Labels render "Resolution _frame rate" and "Setup _Downloads..." — a literal underscore artifact | Bare `&` parsed as a mnemonic: `QLabel` "Resolution & frame rate" gets mnemonic parsing from `setBuddy` (`main_window.cpp:376`, `:380`) and `QPushButton` always parses it (`:799`) — the `&` is swallowed and the following space is underlined | Escape as `&&` in both strings; then grep all user-visible literals for a bare `&` and escape every hit |
| 4 | Disabled controls are unreadable (Correction tracking Back/Stop/Next; disabled Backend combo showing "NVIDIA Image Scaling (default)") | Dark palette's `QPalette::Disabled` text roles are ≈2:1 | Set explicit disabled roles where the app palette is built: `Disabled` `WindowText`/`ButtonText`/`Text` = `#8f8f8f` (≈5.9:1). Pairs with plan 27 Phase 0 (U4) tooltips that say *why* a control is disabled |
| 5 | Last control in the inspector sits clipped against the scroll viewport edge (Backend combo, screenshot bottom) | Scroll content has no bottom inset | 12 px bottom margin on the advanced-panel scroll contents; `ensureWidgetVisible(..., 8, 8)` margins on search/focus navigation |
| 6 | Draw rail checked state is border-only, easy to miss | `QToolButton:checked` = border swap (`annotation_overlay.cpp:115-118`) | Checked = filled `#a83cbe` + white icon/text (≈5.2:1); focus ring stays the 3 px accent border |
| 7 | Yellow/White swatches would vanish on the dark flyout | (new UI risk, pre-empted) | 2 px `#080808` inner outline on every swatch; selection ring is white **+** a checkmark, never color-only |

Rule going forward (add to the a11y doc): **`#bd52d3` is a chrome color,
not a text color.** Any accent-colored text uses `#d79ae6` or lighter on
dark surfaces; every new label gets a contrast check against its actual
background at ≥4.5:1 (≥3:1 only for 14 pt bold+ / 18 pt+).

## R2.8 Bottom-right action bar must survive Advanced mode

Owner screenshots: in Advanced mode the Photo/Record/Explain/Read/Draw bar
is **gone**. Cause is one line: `setSimpleMode(false)` explicitly hides it
(`src/ui/main_window.cpp:2494` — `bottomRightPanel_->hide();`).

Fix:
- In the Advanced branch of `setSimpleMode` (`:2474-2496`), treat
  `bottomRightPanel_` like `bottomLeftPanel_` is already treated
  (`:2490-2492`): opacity 1.0, `show()`, `raise()` — never hide.
- Geometry needs **no new math**: `UpdateSimpleChromeGeometry()` positions
  the bar at the right edge of `renderWidget_` (`:2301-2306`), and the
  render widget already shrinks when the inspector docks — so the bar
  lands immediately left of the inspector, exactly what the owner asked
  for ("moved to the right maybe squised").
- Bars must not overlap: the existing `bottomPanelsOverlap` stacking
  (`:2301-2304`) lifts the right bar above the left cluster when their
  widths collide on a narrow viewport. Verify it triggers with the
  inspector open at the app's minimum window width; if the two bars plus
  inspector cannot coexist even stacked, the right bar compresses to
  icon-only buttons (drop the text labels, keep 58 px height, tooltips
  and accessible names carry the words) before anything is allowed to
  disappear. **Hiding is not an option**; Draw/Photo/Record are the
  product's primary actions.
- Chrome fade stays Simple-mode-only (`FadeSimpleChrome` already guards on
  `isSimpleMode()`, `:2446`); in Advanced the bar is simply always
  visible, like the mode carousel.
- `SimpleChromeHasFocus` / ApplicationDeactivate paths already include the
  panel in their loops (`:2389`, `:2608-2613`) — no change.

## R2.9 Settings and persistence

Extend the `annotations` object in settings.json (via `SettingsStore`,
same versioned-read style as the existing keys):

| Key | Type | Default | Notes |
|---|---|---|---|
| `color` | string | `#fff000` | existing; non-preset values stay legal |
| `widthPixels` | int | 8 | existing |
| `captureOnExit` | bool | true | existing; UI moves to Advanced ▸ Assistant |
| `dashed` | bool | false | new |
| `shapeKind` | string | `"rectangle"` | new; `"rectangle"` \| `"ellipse"`, unknown → default |
| `textSizePixels` | int | 36 | new (today it resets every session) |

Tests: round-trip all six; unknown `shapeKind` falls back; missing keys
default (settings_store_tests.cpp style).

## R2.10 Implementation order

Independent phases, each shippable alone, each with tests + docs
(`docs/code_reference.md`, `CHANGELOG.md`, a11y notes) in the same commit
per `agents.md`:

| Phase | Content | Files | Effort |
|---|---|---|---|
| **R2-A** | Advanced-mode action bar (R2.8) + all contrast/text fixes (R2.7 #1-5) | `src/ui/main_window.cpp`, palette setup site | 0.5 day — do first, all one-liners with outsized daily-use impact |
| **R2-B** | Wheel + key forwarding (R2.4) | `src/ui/annotation_overlay.cpp` | 0.5 day + rig verify (trackpad and mouse, Simple and Advanced) |
| **R2-C** | Rail/flyout restructure: icons, right action rail, swatch grid, vertical slider, Solid/Dashed UI, flyout lifecycle (R2.2, R2.3), Save-on-exit relocation, R2.7 #6-7 | `annotation_overlay.{hpp,cpp}`, `main_window.cpp` (Assistant checkbox), new icon assets + `assets/okuflow_resources.qrc` | 1.5-2 days |
| **R2-D** | Move-mode scale handles (R2.5): `BuildStrokePath` extraction, handle hit/drag/cursors, keyboard resize, model scale API | `annotation_model.{hpp,cpp}`, `annotation_overlay.cpp`, model tests | 1-1.5 days |
| **R2-E** | Shape tool (R2.6): kinds, rendering, outline hit-test, flyout pair, `dashed` render rule | `annotation_model.{hpp,cpp}`, `annotation_overlay.cpp`, `settings_store.{hpp,cpp}` + tests | 1 day |

New icon assets (Lucide, same license file already vendored at
`assets/icons/lucide/LICENSE`): `mouse-pointer.svg`, `slash.svg`,
`square.svg`, `type.svg`, `eraser.svg`, `undo-2.svg`, `redo-2.svg`,
`save.svg`, `trash-2.svg`, `check.svg`.

## R2.11 Acceptance checklist

Interaction:
- [ ] Middle-button drag anywhere outside Draw chrome pans the camera with the
      same direction and speed as Draw-off mode; release always ends the pan.
- [ ] Select Text, click a scene position, type, and press Enter: the label is
      placed at the clicked point. Escape cancels, while Save/Clear/Done retain
      non-empty pending text.
- [ ] Draw a stroke, Ctrl+scroll mid-stroke: zoom changes (with
      acceleration on a fast flick), the in-progress ink stays glued to
      board content, drawing continues seamlessly. Repeat with plain-wheel
      pan. Repeat on a precision trackpad.
- [ ] Keyboard-only: enter Draw, zoom and pan from the keyboard with no
      selection active; arrows nudge only when a selection exists.
- [ ] Flyout: opens on tool activation and on active-tool re-click; a
      single canvas press both closes it and starts the stroke; Esc ladder
      order is flyout → selection → Done; it never times out on its own.
- [ ] Move: click an item → dashed box + 8 handles; corner drag resizes
      uniformly about the opposite corner, Shift frees the axes; edge drag
      is single-axis; text always uniform; 24 px floor holds; cursors
      change per zone; Ctrl+Plus/Minus resizes with announcements; one
      undo step per gesture.
- [ ] Solid/Dashed: dashed stroke shows transparent gaps (no black
      ghost line); halo and core dashes align at every zoom.
- [ ] Shape: rectangle and ellipse draw corner-to-corner; Shift constrains;
      erase/move/resize/undo all treat a shape as one item.

Chrome:
- [ ] Advanced mode shows the bottom-right bar left of the inspector; at
      minimum window width the bars stack (or compress to icons) — nothing
      overlaps, nothing disappears; Draw works identically in both modes.

Contrast/text (verify with a contrast checker on real screenshots):
- [ ] Scope captions ≥7:1; no accent-colored text below 4.5:1 anywhere in
      the inspector or Draw UI; disabled controls ≥3:1 and each has a
      "why" tooltip; "Resolution & frame rate" and "Setup & Downloads..."
      render with a real ampersand; the last inspector control is never
      clipped by the scroll edge.

Accessibility:
- [ ] Every new button has accessible name + description; every tool/state
      change announces via QAccessibleAnnouncementEvent (never TTS); tab
      order is rail → flyout (when open) → right rail; focus returns to
      the Draw button on exit; all hit targets ≥48 px at 100 % UI scale.

Regression:
- [ ] Photo/Record/Explain/Read unchanged in both modes; pan/zoom/joystick
      identical with Draw off; notes capture (Save/Clear/Done) produces
      PNGs whose ink matches the on-screen view; msvc-cpu test suite green.

## R2.12 Out of scope for R2

- Arrow shape, polygon, highlighter-alpha ink — future.
- Persisting strokes across sessions — unchanged v1 rule (notes HTML is
  the durable record).
- Mirror-flip on drag-past-anchor (Paint allows it; v1 clamps).
- Re-anchoring ink after a rig bump — still the optical-flow future item.
