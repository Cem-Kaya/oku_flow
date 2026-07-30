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
- The clusters are frameless windows owned by the main window. This keeps them
  above the native D3D swap chain while retaining normal Qt buttons,
  accessibility metadata, focus, and tooltips.
- Each cluster uses an opaque background and a 3 px high-contrast border so it
  remains legible over every possible camera image.
- Chrome fades after five seconds without input. Mouse movement, a mouse
  button, wheel input, a key press, focus entry, or application activation
  reveals it immediately. Focused controls and an open mode grid remain
  visible. `Ctrl+H` pins the controls on screen or restores automatic hiding.
- The current profile is flanked by previous/next buttons. The grid button or
  current-profile button opens a temporary tile grid with plain-language names
  and number badges.
- Number keys `1` through `9` apply the first nine quick modes from anywhere
  in Simple mode. `Tab` and `Shift+Tab` move across the separate corner
  clusters in a predictable order, and `Esc` closes the mode grid.
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
- A compact `Text Clarity` checkbox beside the profile carousel is the only
  direct image-processing toggle in Simple mode. It automatically chooses
  paper/board/mixed behavior; all component parameters remain in Advanced.
- Device selection and detailed numeric controls are intentionally absent.

Adding a new profile must not create persistent chrome. It joins the mode grid
and remains reachable by scrolling; only the first nine entries receive
number shortcuts.

## Advanced Mode

Advanced mode keeps the live image visible and opens a narrow inspector on its
right. Its top-level tabs are `Image` and `Assistant`; previous/next arrows
wrap across current and future sections. A full-width AI Settings pop-out row
sits directly below the tab strip on both pages instead of crowding navigation.
The top-left Simple/Advanced switch remains pinned in Advanced and is restored
after Alt-Tab or other application deactivation.

The scrollable Image inspector exposes four explicit ownership scopes:

1. **Device** contains the camera and physical orientation. These settings
   change capture hardware.
2. **Viewport** lives under `Device > More device options` and contains
   motion rate, Fill/Fit framing, Virtual Joystick visibility, and optional
   wheel acceleration. These settings change presentation and interaction,
   never profile pixels.
3. **Recording** contains processed-video resolution, camera/original-media
   resolution and frame rate, and microphone selection. These are global
   recording/capture choices, never quick-profile image treatment.
4. **Profile** contains image-processing and assistive-mode values, plus
   commands that save the current configuration as a quick option or reset
   profile-owned tuning to defaults.

Profile controls use reusable, keyboard-focusable collapsible headings.
Disclosure state persists globally, non-default groups expand automatically
and show a changed count, and `Ctrl+F` focuses the pinned settings search.
Search expands matching groups regardless of their saved disclosure state and
restores that state when cleared.

The compact question-mark button in the tab header opens help without taking
permanent camera space. The guide lists Controls before Features.

The inspector is constrained to 380-520 pixels. Detailed controls scroll
inside it instead of increasing the height of the main control area.

Assistant is a separate work surface rather than another image-processing
section. It shows Codex/ChatGPT connection and usage state, a camera-aware chat,
and a history tab. A new conversation can attach the current processed frame;
follow-up questions can keep or omit that attachment. Simple Explain creates a
temporary thread and never appears in history. Advanced Assistant creates
persistent threads and lists only ids created by OpenZoom, with resume, rename,
export, and delete actions. Codex owns the transcript store while OpenZoom keeps
the small title/preview/timestamp index in `settings.json`. Internet and coding
are explicit global Assistant permissions in AI Settings. Coding requires a
workspace folder and affects only persistent Advanced Assistant turns; Simple
Explain remains restricted even when those permissions are enabled.

Simple Explain and OCR results use one solid floating Assistant over the
camera. The panel preserves incremental streaming, exposes its text through a
focusable read-only text view, and provides a high-contrast Close control and
manual Read Aloud action. Its header is a drag handle, its edges and corners
resize within the camera bounds, and its question field attaches the current
view to the shared persistent Assistant conversation. Closing it (or pressing
Escape while it has focus) keeps the current result hidden until the next
user-requested analysis. OCR, scene explanations, and mode changes never start
speech automatically; only Read Aloud or the AI Settings Preview action does
so. First use positions the panel below the top Simple controls. Position and
size are stored relative to the camera surface, restored on restart, and
clamped when the available view changes.

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
| Assistive View position and size | Restores the user's chosen floating panel layout. |
| Inspector section states | UI preference, not image treatment. |
| Selected quick profile | Restores the active workflow. |
| Annotation color, width, line style, shape, text size, and save-on-exit | Drawing preferences are shared across profiles; annotation strokes remain session-only. |
| VLM/OCR endpoints, credentials, language, Read Aloud voice/speed, and note options | Service configuration is shared by profiles. |

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
| Assistive behavior | OCR, scene explanation, and assistive overlay enabled states. |
| Diagnostics | Debug view and focus marker. |

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
  inspector. If the camera area is too narrow, it compresses to icon-only
  buttons with unchanged accessible names instead of disappearing. Application
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
- New camera mounting/selection controls belong in Device. New recording
  format and audio controls belong in Recording. New presentation and
  interaction preferences belong in Device > More device options. New
  processing controls belong in Profile and must participate in config
  persistence, changed counts, and equivalence checks.
