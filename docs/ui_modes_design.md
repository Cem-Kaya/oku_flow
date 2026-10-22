# Simple and Advanced UI Design

This note records the intended main-window structure and settings ownership.
It is the reference for future UI work so the live magnified image does not
gradually lose space as controls are added.

## Primary Goal

The camera image is the product's primary surface. Controls must remain easy
to find and large enough for a low-vision user without pushing the image below
the fold or surrounding it with permanent tool panels.

## Simple Mode

The shared Settings tab's View and navigation section owns live recap duration
(1–5 minutes, default5), its format-dependent decimal-MB estimate, and the
separate working-memory helper. The control is independent of image quick
modes and recording; changing it preserves saved camera/zoom/framing intent.

### Selected-video transport (development)

When a recording or saved video is selected, a native transport occupies its
own layout row below the render surface in both modes. It never covers camera
pixels. Its source label distinguishes the current recording from a saved file;
live camera use also has a distinct silent RAM recap timeline. Native Pause/Resume, timeline,
forward-to-live/end, Return to camera, Mute and Save frame controls retain
keyboard and accessibility semantics. Opening a file first finalizes recording.
Open a file through Settings > Camera > View video, Ctrl+O or a single local-file
drop. The conditional Select video button sits below the source dropdown;
the five corner actions remain Photo, Record, Explain, Read and Draw. See
[transport layout](video_transport.md).

Selected media shares image controls, including orientation. The independent
CUDA media scene supports the shared effects pipeline; CPU compatibility retains
zoom, rotation, reading colors and UCT. Camera resolution/frame rate and the phone
light are hidden while video is selected. Video rotation preserves the camera's
saved orientation and restores it on Return to camera.

Simple mode is the normal operating view:

- The live camera fills the complete client area. Three primary control clusters
  sit flush to its corners: mode switch at top-left, profile navigation at
  bottom-left, and immediate actions at bottom-right. Processing status is
  intentionally absent from Simple mode and lives in Advanced diagnostics.
- Profiles that enable Straighten Screen add a fourth, contextual strip beside
  the carousel with Previous, Stop/Continue, and Next correction controls. It
  disappears for profiles that do not use keystone correction.
- In a camera view too narrow for both bottom corner clusters on one line
  (typically Advanced, or a small Simple window), the left cluster folds first:
  mode name above Previous / mode grid / Next. Further narrowing wraps the
  right actions into two rows within their own corner; see Layout Invariants.
- Measure alternative row/face arrangements without resizing visible controls.
  Apply only the selected arrangement, retaining existing layouts when the
  topology stays the same. A folded native group must not briefly expand to
  its wide layout during activation or small resizes within one layout range.
  The native stability regression observes every intermediate Resize event:
  the original code fails all four folded LTR/RTL rows, while the repaired
  code passes all six wide/folded rows. Six focused native GUI cases also pass
  translated, minimum-width, movement and visual-layout checks. These are
  geometry/interaction checks; actual camera optics and high-refresh display
  latency remain separate acceptance.
- The clusters are frameless windows owned by the main window. This keeps them
  above the native D3D swap chain while retaining normal Qt buttons,
  accessibility metadata, focus, and tooltips.
- Each cluster uses an opaque background and a 3 px high-contrast border so it
  remains legible over every possible camera image.
- Chrome fades after five seconds without input. Mouse movement, a mouse
  button, wheel input, a key press, or application activation reveals it.
  Keyboard-focused controls, dialogs, and an open mode grid remain visible;
  focus left on a button by a mouse click does not prevent idle hiding.
  Stationary pointer notifications caused by native panel hiding do not reveal
  controls. Fading an active control window returns activation to the magnifier
  before hiding it, preserving application focus. Native wake input is scoped
  to magnifier-owned surfaces.
- The persistent top-right `Hide UI` / `Show UI` button and `Ctrl+H` hide or
  restore controls and the assistant together in either mode. Hiding retains
  the mode, inspector tab and width, answer, and drawing. Streamed answers
  continue updating without reopening hidden UI. Ordinary idle fading leaves
  the assistant readable. `Ctrl+F` and `F6` also restore hidden controls.
- The current profile is flanked by previous/next buttons. The grid button or
  current-profile button opens a temporary tile grid with plain-language names
  and number badges. The carousel paints a separate shortcut badge and elides
  its label within the available width; an unmatched configuration shows
  `Custom Setup` without a badge.
- Number keys `1` through `9` apply the first nine quick modes from the
  Simple camera controls, without intercepting typing or dialog input.
  `Tab` and `Shift+Tab` move across the separate corner clusters. `F6` and
  `Shift+F6` move between regions in both modes. In the grid, arrows browse;
  Enter, Space, or a click applies the highlighted mode. `Esc` cancels.
  Carousel arrows always advance from the applied mode, even while the grid
  highlights another choice.
- View navigation (wheel, keyboard, joystick, and middle-drag pan/zoom) updates
  the live focus without reclassifying the active mode as a custom setup. A
  real processing-control edit in Advanced still creates a custom setup.
- A profile change displays a large centered toast and sends an assertive Qt
  accessibility announcement. It does not start text-to-speech. The toast
  disappears automatically.
- Built-in modes use action-oriented names such as "Read a Page", "Keep It
  Steady", and "See in Low Light"; internal preset names remain unchanged for
  persistence and configuration lookup.
- The immediate actions are Photo, Record, Explain, Read, and Draw. Draw pins
  the Simple chrome and opens a compact high-contrast vertical tool rail flush
  to the left edge between the permanent corner panels. Tool-specific color,
  thickness, text entry, and save-on-exit controls appear immediately to the
  right of the selected tool. Freehand, line, and
  text items are anchored to normalized processed-scene coordinates, so
  viewport pan and zoom do not detach marks from lecture content. Move
  selects one item with an explicit bounding box; Erase removes whole items.
  Closing the mode clears session ink after the configured annotated snapshot.
- A large `Text Clarity` switch beside Simple/Advanced is the only
  direct image-processing toggle in Simple mode. It automatically chooses
  paper/board/mixed behavior; all component parameters remain in Advanced.
  It retains native Qt checked-state/keyboard accessibility. The shared UCT
  opt-in is saved across sessions; switching Text Clarity off stops UCT without
  clearing that preference or the retained fine-tune settings. Switching back on
  restores the enabled components. UCT remains off by default for old settings.
- Device selection and detailed numeric controls are intentionally absent.

Adding a new profile must not create persistent chrome. It joins the mode grid
and remains reachable by scrolling; only the first nine entries receive
number shortcuts.

## Advanced Mode

Advanced mode keeps the live image visible beside an inspector with three tabs:
`Image`, `Assistant`, and `Settings`. Image changes how the
picture looks; Settings changes how OkuFlow is set up. The tab labels can elide
at narrow widths while retaining their complete tooltips and accessible text.
`Ctrl+Tab` cycles tabs. The Simple/Advanced switch and camera actions remain
available after Alt-Tab and when moving between tabs.

Image starts with a pinned search field, current-mode summary, and
`Save as Quick Mode` / `Reset Mode` actions. Its groups follow everyday tasks:

1. **Zoom** â€” magnification, horizontal/vertical position, and focus marker.
2. **Colors and contrast** â€” display colors, contrast, brightness, automatic
   contrast, and black-and-white thresholding.
3. **Text clarity** â€” automatic text treatment first; individual algorithms
   sit inside a collapsed **Fine-tune text** group.
4. **Steady image** â€” stabilization, Extra Stable (hold on shake), and temporal smoothing.
5. **Straighten screen** â€” keystone correction and history controls.
6. **Sharpness** â€” spatial sharpening, NVIDIA Super Resolution, and blur.
7. **Assistant behavior** â€” profile-owned explanation and overlay switches.
8. **Diagnostics** â€” debug view and pipeline/performance status.

Settings contains shared controls, never saved into quick modes:

1. **Camera** â€” source, session-only phone-light button, orientation, capture resolution and frame rate.
2. **View and navigation** â€” framing, motion rate, joystick, wheel acceleration.
3. **Recording** â€” processed resolution and microphone.
4. **Notes and files** â€” lecture notes, save drawing on exit, and output folder.
5. **Language** â€” application language.
6. **AI and downloads** â€” AI Settings and dependency setup.
7. **Troubleshooting** â€” camera acceleration and camera test.

The phone-light button appears only for a selected phone, enables only
after the phone reports an available light, and reflects actual capture metadata.
It always starts off after reconnect and is never saved into preferences or modes.

Image and shared settings ownership stays unchanged. Obsolete speech-processing
keys are ignored on load and dropped on save without migration. Changed-count
badges describe controls actually inside their group. Ordinary edits preserve
the user's collapsed groups. Each tab searches its own groups in visual order;
when only the other tab matches, Enter transfers the search there. Search
temporarily expands matching groups and restores disclosure choices when
cleared. Active searches refresh after a language change. `Ctrl+F` reveals the
appropriate search field, including from Simple or Assistant. Keyboard navigation stays inside dialogs and editors.

Slider readouts follow the active locale and dim with their sliders. Disabled
slider fills and handles also lose the active accent, including dormant Text
Clarity refinements. Search icons and clear buttons retain Qt's compact side-
widget geometry instead of inheriting the large toolbar-button padding. Responsive
rows preserve usable track space; horizontal/vertical position controls disable
when Zoom is off. Long selector names elide in the middle, retaining the complete
value on hover and through Qt accessibility.
Quick color shortcut labels use at most two lines, reducing the font by at most
one point (11-point minimum) when needed and ending overflow with an ellipsis.
Their complete names remain available through tooltips and accessibility.

Dependent options sit below their parent switch inside an indented group with
a left border. This covers Zoom position, Extra Stable, sharpening backend and
strength, Super Resolution modes and blur parameters. These visual containers
preserve each control's application-owned enabled state and saved preferences.

Before the first presented camera frame, during reconnection, and while capture
is stopped, a centered high-contrast placeholder explains camera state in both
UI modes. It hides when OkuFlow is inactive. This does not yet detect a camera
that stalls after its first frame. Setup Assistant is raised above owned chrome
and fits its rows to the available screen; optional Maxine alone no longer
triggers its startup prompt.

The compact question-mark button in the tab header opens help without taking
permanent camera space. The guide lists Controls before Features.

The inspector defaults to 520 logical pixels, with a 360-pixel minimum and a
saved preferred width capped at 1200 pixels. The camera retains its minimum
size. Detailed controls scroll within the inspector.

Assistant is a separate work surface rather than another image-processing
section. It shows Codex/ChatGPT connection and usage state, a camera-aware chat,
and a history tab. A new conversation can attach the current processed frame;
follow-up questions can keep or omit that attachment. Read creates a new
temporary thread each time and never appears in history. Explain continues one
saved thread across launches; it is listed as "Explain conversation" so it can
be reviewed or deleted. Deleting it, or pressing New while it is open or
selected, makes the next Explain start a new one; Simple mode gains no extra
control. Advanced Assistant creates
persistent threads and lists only ids created by OkuFlow, with resume, rename,
export, and delete actions. Codex owns the transcript store while OkuFlow keeps
the small title/preview/timestamp index in `settings.json`. Internet and coding
are explicit global Assistant permissions in AI Settings. Coding requires a
workspace folder and affects only persistent Advanced Assistant turns; Read and
Explain remain restricted even when those permissions are enabled. A follow-up
asked from the Assistant tab inside the Explain conversation is an Assistant
turn and uses the Assistant permissions for that turn only.

AI Settings groups service selection, the selected provider's connection,
shared Assistant instructions and Explain prompt, Advanced Assistant permissions,
Read Aloud, and lecture notes in that order. The built-in Codex prompt is a
collapsed read-only reference. The Explain prompt is editable for both providers.
Hidden provider preferences are retained; saving a server configuration does not
validate an inactive Codex workspace. Active Codex coding requires an existing
absolute workspace directory. On narrow dialogs, form labels move above fields.

Simple Explain and Read results use one solid Assistant, floating over the
camera. Without saved geometry it occupies about 43% of viewport width and
75% of height, constrained below top controls; saved placement takes priority.
The panel can also dock beside the camera. The Panel position selector offers Floating,
Dock left, and Dock right. Docking shrinks and shifts the camera viewport;
dragging the floating header to either app edge also shows a purple docking
preview with a release instruction. Releasing docks to that side; moving away
or pressing Escape cancels the docking gesture. The preview does not take
focus or mouse input and remains visible above the native camera and Draw.
The dock separator adjusts panel width. To drag a docked panel off its side,
pull the header away and hold for about 350 ms; a short click or small movement
keeps it docked.
After releasing, move clear of that edge before docking there again. The
highlight uses a debounced latch: 180 ms of stable edge proximity to appear,
then 250 ms outside its wider release zone to clear. Brief boundary crossings
keep the same highlight and drop target.
Viewport resizing preserves image proportions while the rendering surface
catches up with the new size. Returning to Floating restores its
previous camera-relative geometry. Draw mode excludes the floating panel's
current bounds from ink and native mouse input as it moves or resizes, keeping
the chat's controls and drag handle usable. The panel preserves incremental streaming, exposes its text through a
focusable read-only text view, and provides a high-contrast Close control and
manual Read Aloud action. Its header is a drag handle, its edges and corners
resize within the camera bounds, and its question field attaches the current
view to the shared persistent Assistant conversation. Closing it (or pressing
Escape while it has focus) keeps the current result hidden until the next
user-requested analysis. Text readings, scene explanations, and mode changes never start
speech automatically; only Read Aloud or the AI Settings Preview action does
so. First use positions the panel below the top Simple controls. Position and
size are stored relative to the camera surface, restored on restart, and
clamped when the available view changes.

Explain chat uses the shared Assistant composer: the question sits above a
single footer with Read Aloud at the leading edge and a compact camera attachment
switch beside Send at the trailing edge. The switch and Read Aloud retain native
keyboard/accessibility behavior; the footer mirrors for right-to-left layouts.
New Conversation remains in the compact header. Read's separate result pane
retains its own presentation.

## Settings Ownership

Device values change capture hardware and do not change with a quick profile:

| Device value | Reason |
| --- | --- |
| Camera selection | A physical capture source is an application choice. |
| Camera resolution and frame rate | A requested and negotiated capture format belongs to the selected hardware. |
| Camera orientation | Describes how the physical camera is mounted. |

Recording values remain global and do not change with a quick profile:

| Recording value | Reason |
| --- | --- |
| Camera/original resolution and frame rate | Defines live capture plus original photo/video dimensions. |
| Microphone | Selects audio for the single Original MP4 output. |

The old processed-canvas control is hidden and disabled; persisted legacy
values are ignored. Photos retain their separate original/processed pair.

Viewport values affect only presentation or interaction and do not change
captured camera pixels:

| Viewport value | Reason |
| --- | --- |
| Fill/Fit framing | Controls crop or letterboxing while preserving source aspect ratio. |
| Viewport motion rate | Controls presentation smoothness independently of camera FPS. |
| Virtual joystick visibility | Interaction preference. |
| Zoom wheel acceleration | Interaction preference; keyboard zoom remains deterministic. |

Other global UI/service values also remain outside profiles:

| Global UI or service value | Reason |
| --- | --- |
| Simple/Advanced state | Restores the user's preferred working view. |
| Assistive View dock side and floating position/size | Restores the chosen panel layout; floating geometry is retained while docked. |
| Inspector section states | UI preference, not image treatment. |
| Selected quick profile | Restores the active workflow. |
| Annotation color, width, line style, shape, text size, and save-on-exit | Drawing preferences are shared across profiles; annotation strokes remain session-only. |
| VLM endpoints, credentials, language, Read Aloud voice/speed, and note options | Service configuration is shared by profiles. |

Profile values describe how the current image should be treated and are saved
when the user creates a quick option:

| Profile value | Examples |
| --- | --- |
| Magnification | Zoom enabled, amount, and focus position. |
| Image cleanup | Black and white threshold, blur, and temporal smoothing. |
| Jitter reduction | Stabilization enabled and strength. |
| Display treatment | Color mode, contrast, and brightness. |
| Sharpening | Backend, enabled state, and strength. |
| Text clarity | Master/individual stages, Sauvola and softness, polarity, stroke weight, CLAHE, two-color output, hysteresis, focus threshold, glare suppression. |
| Assistive behavior | Scene explanation and assistive overlay enabled states. |
| Diagnostics | Debug view; the focus marker remains profile-owned under Zoom. |

`AdvancedConfig::rotationQuarterTurns` remains readable from old profile JSON
only for backward migration and is no longer written into profiles. Current
code persists and applies orientation at the top level of
`PersistentSettings`; profile comparisons ignore the legacy field.

## Layout Invariants

- Switching modes must not recreate or hide the render surface.
- Simple mode leaves the render surface full-size and overlays only the three
  primary clusters plus the contextual keystone strip described above.
- Advanced controls live beside the camera, never above it as a tall form.
- The bottom-right Photo/Record/Explain/Read/Draw action bar remains available
  in Advanced, positioned against the camera viewport rather than the
  inspector. When both clusters cannot share a line, the left mode cluster
  gets a second row first, with its name above the three navigation buttons.
  The five actions initially stay in one row at the bottom trailing edge.
  If necessary their icons move above their full labels, keeping the same font
  size. Further narrowing wraps that group into three actions above two, with
  equal widths within each row and matching outer row edges. Only a viewport
  too narrow for even that labeled two-row group uses icon-only faces;
  full button text, screen-reader names and tooltips are retained. If both
  compact clusters still cannot fit beside each other, the compact left cluster
  sits above the actions, without stretching into a full-width second band.
  Contextual keystone controls remain with the left cluster. Application
  deactivation may temporarily hide the native tool window, but reactivation
  must restore this bar in both Simple and Advanced modes.
- Record remains actionable while Draw is active. New recordings remain clean
  Original video; vector ink belongs to the view and selected-frame saves.
  The persistent action bar stays above the transparent drawing
  surface for native hit testing: Photo, Record/Stop, Explain, Read, and Draw
  remain clickable, and pressing the checked Draw action leaves annotation
  mode.
- Draw uses a left tool rail, a transient options flyout, and a right action
  rail. Wheel and unconsumed navigation keys are explicitly forwarded across
  the frameless annotation window to the canonical viewport interaction path.
  Middle-button drag is forwarded through the same boundary and always pans
  the viewport outside Draw chrome. Text placement is canvas-first: click the
  scene anchor, type in the inline editor, and press Enter to commit or Escape
  to cancel. Move supports empty-space marquee selection; selected groups move,
  nudge, delete, and undo together, while resize handles remain a single-item
  operation.
- Processing status belongs under Advanced Image diagnostics. Its visible
  value is deliberately short (`GPU Ready`, `Camera Offline`, `CPU Debug`, or
  `GPU Required`); full processing/error detail remains in its tooltip.
- Every interactive control keeps an accessible name, description, visible
  keyboard focus, and a logical tab order.
- Static widgets may receive a fixed accessible name during construction.
  Dynamic labels and buttons must instead use `SetLiveText` so their visible
  text, role-qualified accessible name, and UIA change events stay in sync.
  Camera loss is assertive; normal status changes are polite; slider values,
  usage counters, and high-frequency diagnostics are silent. Repeated
  announcements must be deduplicated and diagnostic updates coalesced.
- New camera mounting/selection controls belong in Settings > Camera. New
  recording format and audio controls belong in Settings > Recording. New
  presentation preferences belong in Settings > View and navigation. New
  processing controls belong in Image and must participate in config
  persistence, changed counts, and equivalence checks.

Text Clarity is the master for automatic enhancement and its stored refinements. The indented Fine-tune text group remains inspectable while off, but its controls are disabled; switching off retains their values.

## Assistant chat and control consent

Assistant owns chat only within the unchanged Image / Assistant / Settings
contract. One compact header contains connection, New, History and a settings
gear; the transcript fills the body above a fixed composer. History and settings
are overlays, never additional tabs. AI Settings alone owns the persisted
embedded/external masters: built-in control defaults on, external MCP off, and
saved choices are preserved. The gear contains unsaved session grants,
external lease review/status/revoke, capability testing, About control and Activity.

Advanced and floating Explain are views of one `VisibleChatSession`, including
shared draft/attachment/busy state and immutable saved kinds. Scroll/selection
stay local to each view. Snapshots never raise/focus/reopen the floating view.
Read has a separate result presentation and cannot replace Explain chat.
Both chat surfaces use root-level document paragraphs so accessible text bounds
follow the painted words, including after wrapping and scrolling. Image-bearing
Explain and follow-up requests show an aspect-preserving thumbnail of the frame
prepared for that request. The last eight thumbnails are memory-only; restarting
or restoring history does not reload camera images. Later model context remains
text-only.
Human-readable permission cards expose native Allow once/Deny controls; exact
canonical arguments stay in collapsed Details. Chat sentences confer no consent.
Ctrl+Shift+A explicitly reveals/focuses Deny, and Ctrl+Shift+U stops and undoes.
MainWindow owns the persistent live region outside hideable chrome. Refused
routing preserves the shared draft. See [the UI contract](agent_control_ui.md)
for the approved concept, review coverage and validation limits.


Video source recovery also preserves the camera-only phone-light control:
cancelling or failing the native video picker reconciles the source dropdown
and recomputes phone availability from the current camera, rather than leaving
the button hidden. Wrapped action rows use independent horizontal layouts with equal button
minimums derived from translated content. Each row owns its spacing, including
a three-button row above two wider buttons; mixed grid spans cannot suppress
internal gaps or make neighbouring buttons uneven.
