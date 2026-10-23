# Viewport zoom and same-file Resume

Fill still uses the full viewport at 1x, preserving existing saved profiles.
Zooming out now reveals the rest of a wide or portrait source with symmetric
bars instead of independently stretching two clamped crop axes. Its lower
bound is `min(viewportWidth/sourceWidth, viewportHeight/sourceHeight)` divided
by the corresponding maximum scale. Rotation uses the oriented source size.
At that bound the picture matches explicit Fit, with the entire source visible.

The persisted Fill value `0.01` denotes the whole-source endpoint, rather than
literal hundredfold reduction. Its effective scale follows viewport/source
aspect changes, including ratios below 0.01. Other positive amounts retain their
meaning and clamp at the current fit scale for display. Modern and legacy
settings/profile serialization preserve sub-1x values; invalid nonpositive or
nonfinite inputs retain the prior 1x fallback. The ceiling remains 16x.

The slider minimum is the ceiling of the exact fit scale in hundredths; selecting
that minimum stores the whole-source endpoint, avoiding fractional-edge crop.
Its readout shows the effective scale. Range refreshes block slider signals and
do not rewrite preferences. A two-hundredth slider magnet lands at 1x; geometric
wheel/key steps crossing 1x stop there once and the next step leaves it. A step
in from whole-picture starts at the effective fit scale immediately. A native
step with Zoom disabled starts from the displayed 1x rather than a saved dormant
amount. Agent writes validate the persisted domain independently of the current
slider minimum.

A native zoom gesture leaves explicit Fit mode, while loading a saved profile does not.
Cursor mapping and zoom anchoring use the actual selected-media dimensions,
crop and letterbox rectangles; pan remains available on a still-cropped axis.
Existing neural-upscaling activation thresholds are unchanged.

Freeze retention reconstructs Fill scale from both source and destination
extents. A partially letterboxed image can still crop its long axis; bars alone
are not evidence of explicit Fit. Frozen presentation recomputes the canonical
transform on resize, and whole-source holds keep their whole-source endpoint.
The successful camera receipt retains explicit Fit and whole-source Fill intent
alongside its transform and pixels; Freeze transfers that committed snapshot,
not a newer requested setting awaiting presentation. This also applies when matching
aspects make both numerically equal to legacy 1x Fill; a later resize must not
turn an intentional whole-picture hold into a crop.
The clean Original fallback carries the same successful-receipt intent when a
processed-scene clone is unavailable. A geometrically identical matching-aspect
transform alone cannot distinguish explicit Fit, whole-source Fill and legacy
1x Fill, so all three intents are retained separately and tested after resize.

## UCT Resume ownership

A normal Resume of the same selected file and currently displayed sample can
retain accepted OCR instead of invalidating the document and model. The
controller requires the exact held source authority, frame ID and media PTS,
valid selected input and proven glyphs. It keeps accepted references, geometry,
protection masks and original media validation timestamps; it discards pending
observations and cancels optional immutable work. A retained selected read is
not credited as another advancing acquisition.

The existing independent advancing-frame probes check those references.
The selected acquisition is marked already probed: Resume does not enqueue a
held-copy probe that could complete after a newer, unanalysed frame and renew
stale text. Only a distinct advancing acquisition can supply the next probe.

Changed pixels remove text, and missing analysis expires it under the existing media
and wall-clock freshness bounds. No freshness threshold is extended. A new full
read replaces the retained conflict inventory. Click-owned captures and control
grants are retired even when the display/OCR scene is retained. Other sources,
seeks, recovery opens and unsupported transitions keep full invalidation.

The shadow/active regional planner does not decide whether accepted glyphs
survive a mode change. Cancelling old refinement changes scheduler authority,
which fences its plans; controller regional state is cleared and full OCR takes
priority after Resume. Only later real full results rebuild planner occurrences.
No held copy or retained word supplies a fabricated planner observation.

## Bottom controls and Assistant startup

The video-controls chevron occupies the leading cell, including the compact
quick-mode row, and has a neutral checked fill with a visible keyboard focus
ring. Its action still hides the complete transport without changing playback.
The four bottom actions are Photo, Record, Explain and Draw, wrapping two by two.
Manual Assistant Read Aloud and the reading implementation remain available.
The native rectangular-layout and resize-stability regressions use a camera
viewport no wider than 400 pixels to exercise the two-row state: four actions
can remain in one row at widths where the former five-action group folded.

Restricted Codex launch profiles override the complete `mcp_servers` table with
`{}` for that child. Disabling individual entries was insufficient: Codex still
validated their transport schema and could exit before `initialize`. CLI
version/feature checks and capability restrictions remain; no user config file
or secret is edited and no external MCP entry is sent to the restricted child.
This does not establish live inference or MCP release acceptance.
