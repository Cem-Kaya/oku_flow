# Simple and Advanced UI Design

This note records the intended main-window structure and settings ownership.
It is the reference for future UI work so the live magnified image does not
gradually lose space as controls are added.

## Primary Goal

The camera image is the product's primary surface. Controls must remain easy
to find and large enough for a low-vision user without pushing the image below
the fold or surrounding it with permanent tool panels.

## Simple Mode

Simple mode is the normal operating view:

- The live camera fills the complete client area. Three primary control clusters
  sit flush to its corners: mode switch at top-left, profile navigation at
  bottom-left, and immediate actions at bottom-right. Processing status is
  intentionally absent from Simple mode and lives in Advanced diagnostics.
- Profiles that enable Straighten Screen add a fourth, contextual strip beside
  the carousel with Previous, Stop/Continue, and Next correction controls. It
  disappears for profiles that do not use keystone correction.
- In a camera view too narrow for both bottom corner clusters on one line
  (typically Advanced, or a small Simple window), the bottom clusters merge
  into a full-width toolbar; see Layout Invariants.
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
- A large, button-styled `Text Clarity` checkbox beside Simple/Advanced is the only
  direct image-processing toggle in Simple mode. It automatically chooses
  paper/board/mixed behavior; all component parameters remain in Advanced.
- Device selection and detailed numeric controls are intentionally absent.

Adding a new profile must not create persistent chrome. It joins the mode grid
and remains reachable by scrolling; only the first nine entries receive
number shortcuts.

## Advanced Mode

Advanced mode keeps the live image visible beside an inspector with four tabs:
`Image`, `Assistant`, `Transcript`, and `Settings`. Image changes how the
picture looks; Settings changes how OkuFlow is set up. The tab labels can elide
at narrow widths while retaining their complete tooltips and accessible text.
`Ctrl+Tab` cycles tabs. The Simple/Advanced switch and camera actions remain
available after Alt-Tab and when moving between tabs.

Image starts with a pinned search field, current-mode summary, and
`Save as Quick Mode` / `Reset Mode` actions. Its groups follow everyday tasks:

1. **Zoom** — magnification, horizontal/vertical position, and focus marker.
2. **Colors and contrast** — display colors, contrast, brightness, automatic
   contrast, and black-and-white thresholding.
3. **Text clarity** — automatic text treatment first; individual algorithms
   sit inside a collapsed **Fine-tune text** group.
4. **Steady image** — stabilization, Extra Stable (hold on shake), and temporal smoothing.
5. **Straighten screen** — keystone correction and history controls.
6. **Sharpness** — spatial sharpening, NVIDIA Super Resolution, and blur.
7. **Assistant behavior** — profile-owned explanation and overlay switches.
8. **Diagnostics** — debug view and pipeline/performance status.

Settings contains shared controls, never saved into quick modes:

1. **Camera** — source, orientation, capture resolution and frame rate.
2. **View and navigation** — framing, motion rate, joystick, wheel acceleration.
3. **Recording** — processed resolution, microphone, and transcription options.
4. **Notes and files** — lecture notes, save drawing on exit, and output folder.
5. **Language** — application language.
6. **AI and downloads** — AI Settings and dependency setup.
7. **Troubleshooting** — camera acceleration and camera test.

Existing persistence keys and settings ownership are preserved. Changed-count
badges describe controls actually inside their group. Ordinary edits preserve
the user's collapsed groups. Each tab searches its own groups in visual order;
when only the other tab matches, Enter transfers the search there. Search
temporarily expands matching groups and restores disclosure choices when
cleared. Active searches refresh after a language change. `Ctrl+F` reveals the
appropriate search field, including from Simple,
Assistant, or Transcript. Keyboard navigation stays inside dialogs and editors.

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
strength, Super Resolution modes, blur parameters, and finalized microphone
transcripts. These visual containers preserve each control's application-owned
enabled state and saved preferences.

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
follow-up questions can keep or omit that attachment. Simple Explain creates a
temporary thread and never appears in history. Advanced Assistant creates
persistent threads and lists only ids created by OkuFlow, with resume, rename,
export, and delete actions. Codex owns the transcript store while OkuFlow keeps
the small title/preview/timestamp index in `settings.json`. Internet and coding
are explicit global Assistant permissions in AI Settings. Coding requires a
workspace folder and affects only persistent Advanced Assistant turns; Simple
Explain remains restricted even when those permissions are enabled.

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

Read Aloud and New Conversation sit directly below the answer, with the
question and Ask controls below them. The two answer actions stack when a narrow
floating or docked panel cannot fit their natural widths. Keyboard order follows
this visual sequence before Panel position and Close.

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
| Processed recording resolution | Defines the fixed processed MP4 canvas independently of the viewport. |
| Camera/original resolution and frame rate | Defines live capture plus original photo/video dimensions. |
| Microphone | Selects audio for both synchronized MP4 outputs. |

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
  inspector. Narrow camera areas use measured wrapping for the mode switch,
  carousel, and labeled action rows. Bottom chrome is always rectangular:
  when the carousel, any keystone strip, and all five actions cannot share
  one line, they become one full-width bottom toolbar of stacked rows
  (actions above the carousel) instead of corner fragments. Wrapped action
  rows keep equal outer edges; a shorter final row uses wider buttons, so
  3 + 2 never forms an L shape. Application
  deactivation may temporarily hide the native tool window, but reactivation
  must restore this bar in both Simple and Advanced modes.
- Record remains actionable while Draw is active. Vector ink is composited
  into the processed recording only; the paired original recording remains
  unannotated. The persistent action bar stays above the transparent drawing
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
