# Assistant chat and control consent

This describes the local 0.3.1 implementation, not a published download or
native acceptance. [Plan52](../improvement_ideas/52-agentic-app-control-api.md)
remains authoritative for runtime authority. The approved visual reference is
[the chat-first concept](../improvement_ideas/design_assets/52-chat-first-concept.png).
All controls are native Qt widgets; the concept is never painted as the UI.

Advanced retains Image / Assistant / Settings. Assistant is one chat view with
a compact connection, New, History and settings header, a large transcript and
a fixed composer. History opens a drawer with selected-row Rename/Export/Delete.
The settings drawer overlays the transcript, closes with Escape and restores
focus. AI Settings alone owns the persisted embedded/external master switches:
Assistant control initially on and External MCP control initially off. Saved
choices are preserved; absent keys and pre-control settings use these defaults.
Enabling Assistant control unchecks/disables ordinary coding and internet
permissions; saving cannot retain conflicting enabled bits. Session permissions
are never duplicated as master switches.

Advanced and floating Explain render the same app-owned `VisibleChatSession`:
conversation identity, rows, draft, attachment and busy state. Each view retains
its own selection and scroll. Incoming snapshots never raise, focus or reopen
the floating window. Only an explicit admitted Explain action shows it. Read
uses a separate result pane and never overwrites or selects Explain chat.
Enter submits once; Shift+Enter inserts a newline. A refused submission keeps
its draft until the model accepts it. Chat text, including affirmative phrases,
never authorizes an action. Read Aloud is manual and resolves completed assistant
messages only. Explicit Explain-origin rows carry a small translated Explain
camera tag from finite app-owned metadata, never a backend identity.
The body uses root-level paragraphs instead of HTML table cells: Qt accessibility
must report the actual word positions in both docked and floating views.
Image-bearing Explain/follow-up and authorized control turns display the prepared camera frame
as a bounded thumbnail. Up to eight previews remain in memory only; local chat
history, exported history and subsequent text context exclude their bytes.
Control thumbnails use the same preparation worker and size limits; a retired
turn cannot publish a late preview or start a provider request. The initial
control image is rechecked against its source epoch and current frame grant
after preparation, before provider submission.

The composer footer places manual Read Aloud at the leading edge, followed by
flexible space, a compact camera attachment switch and Send. Read Aloud stays
visible but disabled until a completed answer is available. The switch retains
native checkbox accessibility, Space activation, a 44px hit target, visible
keyboard focus and the existing camera-permission tooltip. Its thumb position
and palette colors distinguish on/off; the row mirrors in right-to-left layouts.
Tab order follows composer, Read Aloud, attachment, Send. Stop/Undo remain in a
separate strip only while needed.
Floating chat uses native Qt traversal within its full view, including routing,
approval, Stop/Undo and drawer controls, rather than the outer chrome's short
focus list. Routing and approval controls precede the composer in that chain.
Attach current view is the current-turn/current-source camera grant.
Photos and control-owned recordings have separate session grants in
the settings drawer, with the recording-permission reminder. No grant is saved.
Source changes and retired contexts revoke authority in the app service.
The drawer also contains external permissions, explicit endpoint tool testing,
lease identity/times/budgets/revoke and collapsed About control and Activity log.
Incoming traffic never opens or issues a lease; fresh optional grants start off,
and active bundles cannot be widened. The capability test uses synthetic input,
no camera image and no settings mutation.
When control is enabled but provider tool support has not been established,
the routing row exposes the explicit capability test instead of offering only
a tool-free fallback. A successful test enables the control-conversation choice.
The settings drawer shows the disabled-control status; AI Settings remains the
sole owner of the master permission. Both chat views share connection status,
and each Connect ChatGPT button reaches the same login action. A provider
configuration change invalidates stale capability readiness.

Approvals are contextual native chat cards with a human action question,
optional app-owned target display, expiry, Allow once and Deny. The question uses an explicit 20px native font;
secondary expiry uses 16px and retains measurable width when decisions stack
at narrow sizes. Details starts
collapsed and exposes exact canonical tool/target/normalized JSON as plain text.
Only the explicit buttons emit the opaque immutable approval identity; the
runtime rechecks expiry, source, revisions and stored arguments. Allow clears
the row before emitting and cannot fire twice. Arrival changes no focus.
Ctrl+Shift+A deliberately reveals Assistant and focuses Deny; Escape on the
approval controls denies. Ctrl+Shift+U and Stop and undo share the app-owned
stop/undo path, including when the ordinary chrome is hidden.

First-party human action receipts appear in their original assistant row,
separate from provider text/context. The expandable activity log is bounded to
128 blocks /32 KiB, with each entry at most 2 KiB; it contains no image data and
is never automatically written to notes. MainWindow owns a persistent live
region outside hideable chrome; approval announcements have distinct request
numbers. Normal status is polite, high-frequency status coalesced.

## Wording and validation

The active manifests contain 39 retained control keys and 32 new chat keys:
`tests/data/agent_control_ui_strings.json` and
`tests/data/agent_chat_ui_strings.json`. All768 new target entries (32×24)
have independent read-only Luna verdicts;73 vetted replacements were applied.
Five obsolete tab/approval keys were removed. Retained control wording already
received complete review in the initial iteration. No entries remain unread.
Drafts/reports and `ui-chat-redesign-reconciliation.json` remain under
`%LOCALAPPDATA%/OkuFlow/agentic-control/`. Parity and lrelease pass all 24 catalogs
with 799 finished sources. German/Dutch Details are reviewed cognates with narrow
locale-specific validator exceptions. Model wording review does not establish
native-speaker, font, RTL or screen-reader acceptance.

The 2026-10-16 default change removes the obsolete default-off sentence from
the control explanation. Read-only Luna reviews covered that changed entry in
all 24 languages; dangling references and misleading screenshot terms were
corrected. This wording review remains separate from rendered/native acceptance.

`agent_control_panel_headless` runs Qt offscreen with no camera, provider, bridge,
settings store or native foreground window. It covers user-only grants, exact
single-use/expired approvals, canonical 90° orientation, lease defaults/no widening,
bounded plain text, explicit capability test, refusal drafts, shared views,
Enter/Shift+Enter, escaped messages, selection retention, manual TTS and Read
isolation. Serial native desktop behavior remains a separate gate.

## Opt-in production-widget captures

Explicitly build `agent_control_capture` (excluded from default builds), then
run `agent_control_capture.exe --output <absolute LocalAppData evidence folder>`.
The fixture forces offscreen and constructs real MainWindow/AiSettingsDialog
with in-memory defaults. At1672×941, matching the concept viewport, it captures
chat with a pending photo approval, the direct approval card, the settings drawer
and AI Settings masters (Assistant control on, External MCP control off), plus a separate 360px Arabic RTL chat
and floating Explain view. The fixture also checks master/coding exclusivity. Connection/message text is synthetic and
clearly fixture-labelled; no provider is contacted. MainWindow capture composes
actual native widget peers; approval/dialog captures are direct QWidget grabs.

The Windows offscreen backend registers the installed
`C:/Windows/Fonts/segoeui.ttf` before rendering. No font is bundled or image
pixels repaired. Retain unsuitable captures and render fresh widgets. These
images prove the disconnected development layout only. Record actual comparison
findings before declaring visual acceptance; native focus, hardware and shipping
bundle acceptance remain separate.

## Development visual comparison (2026-10-14)

Offscreen 100% captures at 1672×941 were actually compared side by side with the
approved image. The initial render clipped the newest reply due to nested rich
text table spacing and an oversized approval row; it was retained, then rebuilt
with compact tables and an adaptive card. The inspected result shows all five
sample messages, one header, fixed composer and unresolved native photo card.
Intentional native differences include rectangular bubbles, an explicit attachment
label, manual Read Aloud and approval expiry/Details. The original camera chrome
remains intact. The approval stays visible below the transcript while it is
pending, including when ordinary chat text says Yes, go ahead.

The LocalAppData `chat-first-comparison-r5/` artifact contains full and sidebar
concept-left/native-right compositions, original hashes and viewport/state facts.
Originals were not repaired. Offscreen dark and 200% chat captures were inspected
for readable chat and reachable consent/composer controls. The restricted platform
can truncate existing native camera-chrome labels; this is not a native GUI gate.
A narrow Arabic capture exposed a collapsed expiry column, which was repaired
with a minimum width. Fresh r5 captures verified visible expiry and reachable
Allow once/Deny at 360px Arabic RTL at both 100% and 200%, alongside the completed
20px question/font and Explain camera tag. Actual Windows focus, accessibility and mixed-monitor behavior
remain unverified locally.

Approval Details display the complete canonical normalized change set, including
implicit dependent changes, and the native target display name as plain text.
Requests exceeding the bounded review payload fail closed; raw arguments remain
bound to the service approval independently of this presentation.
Countdown and service-event refreshes retain the existing Details document when
its exact contents have not changed. This preserves the user's selection,
caret and scroll position; changed contents still replace the document, and a
different approval identity collapses Details for a fresh review.

Known app-normalized image changes also show existing translated native field
labels with localized boolean/numeric values in the card (including Super
Resolution, its strength and coupled zoom). Unknown keys and arbitrary request
strings remain in exact Details and confer no grant. Native target labels are
plain text; the six reviewed grouped settings questions remain unchanged.

Explicit Escape from the floating chat activates the camera owner and returns
keyboard focus to its render surface. Streaming snapshots do not activate or
reopen either chat view. Native regression fixtures use the current visible
header for pointer dragging and the shared chat model for streamed and hidden
updates; Read results remain a separate presentation.
The outer Tab bridge defers to native history/settings drawer traversal.
Controlled annotation toggles and tool changes preserve the user's current
focus; ordinary manual drawing retains its canvas/tool focus behavior.

The interrupted 2026-10-15 native gate exposed the Escape handoff bug and stale
legacy toolbar/result selectors. Its failed and unfinished evidence is retained.
Scoped repairs require a fresh serial native check; earlier offscreen visual
approval does not establish native focus or docking acceptance.
That gate's native screenshots include foreign magnifier/toolbar content and
cannot be used as clean visual evidence; no foreign process or pixels were changed.
