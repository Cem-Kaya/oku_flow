# Ultra Clear Text on-screen text for screen readers

Ultra Clear Text (UCT) replaces verified camera text with sharp glyphs. This
module is intended to also make that text available to Windows UI Automation
(UIA), so screen readers and Magnifier could read it straight off the camera
view, with highlights placed over the glyphs actually drawn. The passive UCT
reading pane is unchanged and still exists alongside it.

Status: the central Qt offscreen gate passes all 24 QtTest rows at each
scale (1, 1.5 and 2). The first run's 13/20 result and later two-fixture
failures remain preserved in the evidence ledger. Repairs delivered queued
expose events before geometry assertions and used Fill for real pan/zoom;
Fit intentionally ignores magnification. Painted-font, right-to-left,
fully-hidden-range and offscreen-export authority regressions also pass.
This is the QAccessible/paint model contract, not native Windows acceptance.
**Native UIA, Narrator, NVDA and Magnifier behaviour has not yet been verified**
(see [Native acceptance](#native-acceptance-not-yet-run)).

## Contract

The pan/zoom fixture uses Fill: Fit deliberately ignores magnification and
would not exercise changing geometry. Its assertions retain actual glyph
pixels, range positions and interface identity across 300 projections.

- **Hosting.** The elements are virtual (object-less) accessibility children
  of the render viewport (`RenderWidget`). They are not attached to the
  click-through overlay window.
  - Qt's UIA adapter raises no StructureChanged event, so clients can only
    discover them by querying (see **Events** below).
  - `ElementFromPoint` over a glyph is expected to resolve to its line
    element through the host's `childAt`; this is not natively verified, and
    whether Windows hit-testing reaches `RenderWidget` past the transparent
    overlay window is an open acceptance item.
- **Tree.**
  - The **root** has role `Grouping` and is read-only.
    - Name: *Ultra Clear Text on this view*.
    - Description: *… marks text left as the original image.*
    - Rect: the viewport.
  - Under the root is one **line element per shown OCR line**, in the shared
    reading order. Each line element:
    - has role `StaticText` (UIA Text: no ValuePattern);
    - is read-only and not focusable;
    - takes its Name from the line text;
    - carries a `QAccessibleTextInterface`, plus `Locale` from the recognizer
      language (UIA Culture).
  - There are no per-word elements.
- **Text.** Line text is exactly what the overlay replaced
  (`uct::OverlayModel::ScreenLines`):
  - Verified glyph-eligible words, joined by single spaces.
  - Every omission, rejected span and reading-only (immutable,
    uncorroborated) word becomes one `…` per run.
  - The text never contains `'\n'`, so Qt's word and line units match the
    tokens.
  - Reading-only evidence is never exposed as text.
  - A line is exposed only if each of its glyph words is in the overlay's
    current word set.
- **Empty root.** Show original, settling, unsupported composites, presentation
  failure/watchdog or loss of source authority leave no drawn lines and an
  empty root. Paired Temporal Smooth and exact Extra Stable holds are supported.
  A terminal optional refinement failure may retain already proved text on the
  exact immutable selected frame; that exception never admits new work or text.
- **Geometry.** It is computed when a client asks, from the overlay's **last
  actual paint** (`UltraClearTextOverlay` paint plan).
  - **Coordinates:** global logical pixels via
    `RenderWidget::mapToGlobal`. That is the window Qt's adapter scales with,
    so DPI is applied exactly once.
  - **Drawn words:** grapheme-cluster cells from `QTextLayout` caret
    positions, using the reference font stored with that paint plan — the
    same font and shaping as its `drawText`. A later stroke-weight change
    replaces the overlay's live font and clears the caret cache before the
    next paint; queries in between still shape with the painted font, so
    they keep describing the pixels on screen. Cells are scaled by the
    painted transform and vertically span the fitted ink.
  - **Words shown as original ink** (too small, under chrome, or caught by
    the protection closure): proportional cells inside the projected source
    box, normalized against the visual (minimum/maximum) caret extent. On
    a right-to-left word the first logical character therefore stays on
    the right instead of being mirrored to the left edge.
  - **`…`:** the gap between its neighbouring words on the line box.
  - **Spaces:** the gap between neighbours.
  - **Right-to-left lines:** gaps are computed between neighbouring boxes
    whichever is leftmost, and a run with only one neighbour extends from
    it in the line's reading direction (derived from the painted boxes),
    so Hebrew/Arabic omissions and spaces keep a real width and hit-test
    correctly.
  - **Offscreen paints are not authority:** rendering the overlay into an
    export image (`QWidget::render`, as frame saving does) or grabbing it
    draws with a scratch plan and restores the last on-screen plan; only a
    paint into the window's backing store advances the painted plan.
  - **Clipping:** replacement pixels clip to the painted region; original
    pixels clip to the viewport minus floating chrome.
  - **Fail closed:** if the overlay is not visible, no longer exactly covers
    the viewport, or its size, screen or device pixel ratio changed since the
    paint, every character reports as hidden. The line is then `offscreen`
    until the next paint.
  - **Never `QRect()` and never outside the viewport.** Hidden characters
    return a zero-size rect at the viewport point nearest the text, so union
    highlights ignore them. Every character of one contiguous hidden run —
    in particular a fully occluded line — shares a single anchor point:
    Qt 6.12's UIA text-range provider (`GetBoundingRectangles`) builds a
    range rect by min/max over only the first and last character rects and
    ignores emptiness/`IsOffscreen`, so distinct per-character fallback
    centres would invent a positive-area range over text that is entirely
    hidden. With the shared anchor that aggregation yields zero area.
  - Rects are rounded outward to integer logical pixels. That is up to
    ±0.75 physical px at 150%, a limit of Qt's API.
- **Pan and zoom** send no events and do no accessibility work; clients
  re-query and get the new geometry.
- **Cadence.**
  - Genuinely new lines publish on a 1 s semantic tick, measured from the first pending
    change.
  - If any UCT element was queried within the last 3 s, additions are held,
    but never more than 3 s after the first pending change.
  - The first content after enable, activation, or any revocation (seek,
    settling, Show original) publishes at once.
  - One single-shot timer is used. Churn never restarts it.
  - Optional timing hooks carry a copied admission ticket/content revision,
    including coalesced updates, rather than reading a newer frame at timer
    expiry. They record software publication, never reader receipt or speech.
  - Changed text under an existing line ID is replaced in the same update.
    Its old interface/ranges are revoked synchronously; withholding its new
    interface cannot preserve the old reading position. Other unchanged lines
    retain their interfaces, and unrelated new lines still wait for the tick.
- **Revocation is immediate.** Any loss of validation or authority deletes
  the element synchronously (`QAccessible::deleteAccessibleInterface`) on the
  UI thread, before any timer fires. Ranges a client holds then fail with
  `UIA_E_ELEMENTNOTAVAILABLE`.
  - A line that leaves the drawn set is removed now. A line whose text changes
    loses its old ranges now and gets its replacement immediately.
  - There is no soft hold.
- **Identity.** Re-recognizing identical text under a new model line id, at
  an IoU of at least 0.5 with the previous box, silently rebinds the existing
  element. Its interface id and the reader's position are kept, and no event
  is sent.
- **Events.** Only `ObjectCreated` (for additions) and the cache's automatic
  `ObjectDestroyed`. Qt 6.12's Windows adapter
  (`QWindowsUiaAccessibility::notifyAccessibilityUpdate`) maps neither; it
  maps only Announcement, Focus, StateChanged, ValueChanged, NameChanged,
  RoleChanged, SelectionAdd and the Text* events, and it has no
  StructureChanged mapping at all. There is therefore no QAccessible event
  that both announces new children to UIA and preserves the no-unsolicited-
  speech contract: the mapped alternatives would either risk speech
  (Announcement, Focus, Text*) or misreport a property that did not change
  (Name/Value/State). The module deliberately sends none of them.
  Discoverability instead relies on live queries: the adapter's `Navigate`
  walks `childCount`/`child`/`indexOfChild` on every call with no tree cache,
  so a client that navigates into the viewport, rescans the window, or
  hit-tests finds the current elements. A client that caches the host's
  children before text appears will not be woken; that is a Qt limitation,
  recorded here rather than papered over with a fake property event.
- **Bounds.**
  - At most 128 line elements.
  - At most 512 UTF-16 code units per line, cut at a word boundary with a
    trailing `…`.
  - At most 8192 code units (16 KiB) in total, cut at the end of reading
    order.
- **Inactive accessibility.** While `QAccessible::isActive()` is false,
  nothing is created and every hook returns immediately. An
  `ActivationObserver` builds the current state when a client attaches and
  tears it down when the client detaches. The module never blocks, never
  touches the OCR or probe workers, and never forces a repaint.
- **`scrollToSubstring`** does nothing: a screen reader must never pan the
  camera. There is no selection or caret.

## API (module `okuflow/ui/uct_screen_text_accessible.hpp`)

- `kAccessibleVirtualChildHostProperty`: a dynamic property.
  - A widget whose value is `true` at its first accessibility query gets a
    `QAccessibleWidget` (role Client) host interface.
  - That interface appends the elements of its direct
    `AccessibleVirtualChildren` child after its widget children.
  - Widget children win hit testing.
  - The factory only claims the most derived class name, so the host needs
    its own `Q_OBJECT` class. `RenderWidget` sets the property in its
    constructor.
- `MarkAccessibleVirtualChildHost(QWidget*)`: installs the factory (once) and
  sets the property. The factory is also installed at static initialization.
- `AccessibleVirtualChildren` (QObject, direct child of the host):
  - `VirtualChildCount`, `VirtualChild`, `IndexOfVirtualChild`,
    `VirtualChildAt(globalX, globalY)`.
- `UctScreenTextSnapshot { enabled, lines, languageTag }`: what to expose
  now.
- `UctScreenTextTiming { cadenceMs = 1000, readingHoldMs = 3000 }`.
- `UctScreenTextAccessibility(overlay, host, provider, clock, timing)`:
  - Hooks: `SetEnabled`, `ContentChanged`, `RevokeAll`, `ProcessDueWork`.
  - Diagnostics: `IsActive`, `RootInterface`, `ExposedLineCount`,
    `HasPendingAdditions`, `Publications`, `Revocations`, `SnapshotsTaken`.
  - `RootNameSource()` and `RootDescriptionSource()` return the
    translatable source strings.

Related additions elsewhere:

- `okuflow/common/ultra_clear_text.hpp`:
  - `uct::SceneWord::wordIndex`.
  - `uct::ScreenToken`, `uct::ScreenLine`, `uct::ScreenTextLimits`.
  - `uct::OrderLinesForReading()`, the reading order now shared with
    `ReadingText()`.
  - `OverlayModel::ScreenLines()`.
- `okuflow/ui/ultra_clear_text_overlay.hpp`:
  - `SceneWord::wordIndex`, `RenderTarget()`.
  - Last-painted geometry: `PaintedGeneration()`, `PaintedGeometryCurrent()`,
    `PaintedWordGeometry`, `PaintedWord()`, `PaintedSceneRect()`,
    `VisiblePart()`.
  - `paintEvent` now consumes the private `ComputePaintPlan()`. Its
    decisions are unchanged.
- `okuflow/app/ultra_clear_text_controller.hpp`:
  - `ScreenTextAccessibility()` and `ScreenTextSnapshot()`.
  - The controller creates the module as a child of the overlay's render
    target.
  - Hooks:
    - `Invalidate` and the clear branch of `RefreshOverlay` call `RevokeAll`.
    - `OnPresentationFailed` calls `RevokeAll`.
    - A changed word set in `RefreshOverlay` calls `ContentChanged`, and so
      does `HandleBackendStatus`.
    - `SetEnabled` calls `SetEnabled`.
    - The destructor deletes the module first.
- `RenderWidget`: sets `kAccessibleVirtualChildHostProperty`. Its only new
  dependency is the header constant; nothing new is linked.

## Integration and build notes

- **Main application.** `cmake/CMakeLists.txt` globs `src/*.cpp`.
  `include/okuflow/ui/uct_screen_text_accessible.hpp` (`Q_OBJECT`) must be
  added to the explicit header list, next to `ultra_clear_text_overlay.hpp`,
  so AUTOMOC runs.
- **Controller targets.** Every target that compiles
  `src/app/ultra_clear_text_controller.cpp` (today `ultra_clear_text_tests`)
  must also compile `src/ui/uct_screen_text_accessible.cpp` and list its
  header. It already has `ui_translation.cpp`.
- **Targets with only `render_widget.cpp`** need nothing new.
- **This module's tests.** `tests/uct_screen_text_accessibility.cmake`
  defines `uct_screen_text_accessibility_tests` and three CTest entries at
  `QT_SCALE_FACTOR` 1, 1.5 and 2, all with label `headless` and offscreen
  platform. Include it from `tests/CMakeLists.txt`, and add a
  `tests/test_impact.json` mapping if wanted.
- **Translations.** Context `OkuFlow`, resolved through `TranslateUi` at
  query time, so a language switch applies without rebuilding. The new
  English sources:
  - `Ultra Clear Text on this view`
  - `… marks text left as the original image.` (begins with U+2026)

## Limitations and open risks

- **Not natively verified:**
  - Magnifier following, Narrator scan and mouse reading, and NVDA review and
    mouse tracking are expectations derived from Qt 6.12 sources.
  - Whether Windows hit-testing skips the transparent overlay window, so that
    `ElementFromPoint` reaches `RenderWidget`.
  - The earlier assistant-chat Magnifier offset bug was never root-caused.
- **Geometry approximations:**
  - Character cells are caret-advance cells, not ink. Side bearings can put
    an edge up to a fraction of a glyph outside the ink.
  - Vertical extents are per-word fitted ink, not a line-wide band.
  - `…` and spaces are gaps between neighbours, not the exact rejected
    spans, because `AcceptedLine::protectedSceneRects` is not parallel to the
    tokens.
- **Mixed-DPI:** a rect that straddles two monitors is scaled by the
  viewport window's screen only.
- **Accessibility cost:** each new paint invalidates a line's geometry cache.
  A client query after a pan rebuilds it for that line only. Nothing runs
  per frame.
- **Bounds:** a single token longer than the per-line cap drops that line
  instead of splitting a word.
- **Additions are invisible to UIA events:** Qt's UIA adapter maps no
  structure events, so clients discover new lines by re-querying.
- **Partially occluded or disjoint UIA ranges:** the shared-anchor rule
  makes a *fully* hidden line or hidden run zero-area under Qt's
  endpoint min/max aggregation, but a range that spans visible text and
  a hidden run (or two disjoint visible fragments) still gets the single
  bounding rect Qt's provider computes from its endpoints, which can
  cover the occluder or the gap. That is a Qt adapter limitation; the
  per-character rects and the line's `offscreen`/visible geometry remain
  exact for clients that query them.

## Native acceptance (not yet run)

Run serially on a free interactive desktop, never concurrently with other GUI
tests:

1. Inspect or Accessibility Insights: tree shape, roles, patterns, Culture,
   and IsOffscreen.
2. A small IUIAutomation client compares BoundingRectangle and TextPattern
   range rects against captured glyph pixels at 100%, 150% and 200%, and on
   mixed-DPI monitors.
3. `ElementFromPoint` over a glyph returns the line element.
4. NVDA object navigation, review cursor and mouse tracking.
5. Narrator scan mode and mouse reading.
6. Magnifier following the Narrator cursor, and its reading mode: no offset,
   and no unsolicited speech during pan or OCR refresh.
