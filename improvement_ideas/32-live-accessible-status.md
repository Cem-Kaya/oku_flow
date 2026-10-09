# Plan 32 — Live Accessible Status (dynamic text must reach the accessibility layer)

Status: **VERIFY — implementation and automated event gate complete
2026-07-30; owner NVDA/Narrator pass remains.**

## Implementation record (2026-07-30)

- Added `include/okuflow/ui/live_status_text.hpp` and
  `src/ui/live_status_text.cpp`. `SetLiveText` keeps visible text and a
  role-qualified accessible name synchronized, explicitly invalidates cached
  UIA names, and applies silent/polite/assertive announcement policy.
- Per-widget full-message announcement history suppresses repeats for five
  seconds even when a state flaps through another message. The trailing
  coalescer publishes only the latest diagnostic update.
- Routed the pipeline status firehose, camera loss/reconnect, recording
  button, negotiated camera format, camera-acceleration probe, Setup
  Assistant progress/errors, Codex connection/usage/actions, SuperRes status,
  quick-option status, annotation values, speech rate, and slider value labels
  through the helper with the appropriate policy.
- Removed paired manual announcements where the same status already flows
  through `ShowStatusMessage`, preventing double speech.
- Added `live_accessible_status` to the CPU QtTest suite using
  `QAccessible::installUpdateHandler`. It verifies live visible/name updates,
  unchanged-text no-op, assertive announcement metadata, five-second
  deduplication across flapping, and trailing-edge coalescing.
- Validation passed: shipping release compile, CPU 10/10, CUDA 12/12, and
  tested release-bundle 12/12. The published `dist/OkuFlow/oku_flow.exe`
  SHA-256 is
  `ad5ab389f379e4f3611907de0a3165f2a13ec5c1d8d2e0ecaae18ba02e665a4c`.
- The remaining `setText` sites were classified as construction-only,
  editable-field content, or deliberate visual compression whose full
  accessible name remains stable. At implementation time Plan 31 transcription
  was dropped, so no transcript row existed. Plan 36 now owns the transcript
  surfaces and must use this plan's live-text policy.

**Owner's report:** visible text changes with app state (the pipeline
status "GPU Ready" line and others), but the accessibility layer keeps
presenting stale text ("pipeline something…"). Correct — and it is worse
than one label: most of the app's dynamic status traffic never reaches a
screen reader at all.

## Root causes (three, all verified)

**RC1 — pinned accessible names mask dynamic text.** The `setA11y` pattern
sets `accessibleName` once at construction, and Qt's accessibility layer
prefers `accessibleName` over a widget's live text. Every dynamic widget
built this way reports its construction-time string forever:

| Widget | Pinned name | Reality it hides |
|---|---|---|
| `processingStatusLabel_` (`main_window.cpp:1606`) | "Processing Status" | "GPU Ready" / "Camera Offline" / "Reconnecting to camera…" / "CPU Debug" + backend detail (`app_interaction.cpp:185-240`) |
| `recordButton_` (`:1538`) | "Record Video" | Record / Stopping / Finishing / Failed states |
| `cameraFormatNoticeLabel_` (`:1562`) | "Camera format notice" | the actual driver-fallback warning text |
| `assistantConnectionLabel_` (`:1688`) | "Codex Connection Status" | connected/disconnected/login state |
| `assistantUsageLabel_` (`:1690`) | "Codex Usage" | live usage numbers |

**RC2 — the status firehose flows into a masked label.**
`ShowStatusMessage` renders *every* transient status ("Recording saved…",
"Camera switched to compatibility mode…", DXVA fallbacks, disk warnings)
into `processingStatusLabel_` via `UpdateProcessingStatusLabel()`
(`app_interaction.cpp:539-544`). Only 9 call sites in the whole app emit
`QAccessibleAnnouncementEvent`; everything else that "shows a status" is
silent for a screen-reader user — the audience this product exists for.

**RC3 — no reliable final-name invalidation policy.** Qt can emit automatic
`NameChanged` traffic while visible text and `accessibleName` are being
updated, but the old code either left a pinned stale name in place or did not
guarantee that UIA clients saw the final role-qualified value. Dynamic value
labels (percentile diagnostics, slider value labels, negotiated-format text)
had no common cache-invalidation contract. `SetLiveText` therefore posts an
explicit final `NameChanged` event after synchronizing both properties; tests
require at least one name-change notification and exactly one announcement,
without assuming how many internal property events a Qt version emits.

## The threading question, answered directly

No second thread — the opposite. `QAccessible::updateAccessibility` and
the UIA bridge are UI-thread-only, and every one of these labels is
already updated on the UI thread (worker threads correctly marshal via
queued callbacks, e.g. `RecordingManager::PostStatus`). The stale
presentation is not a starved thread; it is **missing name updates and
missing change events**. A helper-thread design would add races and fix
nothing. The helper below asserts UI-thread at entry to keep it that way.

## Design — one helper, one policy

### `SetLiveText` (`include/okuflow/ui/live_status_text.hpp` and
`src/ui/live_status_text.cpp`)

```cpp
enum class LivePoliteness { kSilent, kPolite, kAssertive };

void SetLiveText(QWidget* widget,            // QLabel or QAbstractButton
                 const QString& text,        // the new visible text
                 LivePoliteness politeness,
                 const QString& rolePrefix = {});  // e.g. "Pipeline status"
```

Behavior, in order:
1. `Q_ASSERT` UI thread.
2. No-op when both visible text and the composed accessible name are unchanged
   (kills duplicate announcements and event spam at the source while still
   repairing a previously pinned name).
3. `setText(text)` on the concrete type.
4. `setAccessibleName(rolePrefix.isEmpty() ? text
                      : rolePrefix + ": " + text)` — the screen reader
   hears *both* what the thing is and what it now says
   ("Pipeline status: GPU Ready"), fixing RC1 without losing context.
5. Post `QAccessibleEvent(widget, QAccessible::NameChanged)` via
   `QAccessible::updateAccessibility` — fixes RC3 (cached UIA values are
   invalidated).
6. For `kPolite`/`kAssertive`: additionally post
   `QAccessibleAnnouncementEvent` with the matching politeness — the
   pattern the app already uses in its 9 good call sites.

### Coalescing (for high-frequency sources)

Per-widget trailing-edge coalescer (400 ms `QTimer`, latest text wins) for
sources that update every tick — percentile diagnostics, any per-frame
counter. Announcements are additionally deduplicated by full text within a
5 s window so state flapping (reconnect loops) cannot chant at the user.
Implemented inside the helper behind an opt-in flag
(`SetLiveTextCoalesced`), not at call sites.

### Politeness policy (the product decision, written down)

| Surface | Politeness |
|---|---|
| Camera Offline / Reconnecting / camera errors | **assertive** — the user's primary sense of the room just failed |
| GPU↔CPU/backend changes, DXVA fallbacks, recording state (Stopping/Finishing/Failed/saved) | polite |
| `ShowStatusMessage` default | polite (callers may pass assertive; the recording watchdog already announces assertively and stays as-is) |
| Percentile/diagnostic numbers, slider value labels ("8 px"), usage counters | **silent** — NameChanged only; sliders already announce through Qt's value interface, and nobody wants p95 read aloud every second |
| Codex connection state | polite |

## Phases

### P1 — Helper + unit tests — IMPLEMENTED

`SetLiveText` + coalescer. Tests use
`QAccessible::installUpdateHandler` (designed for exactly this): assert a
NameChanged event fires on change, no event on identical text, the
accessible name composes with the role prefix, announcement events carry
the requested politeness, and the coalescer collapses N rapid updates into
one trailing event. This is a plain QtTest in the msvc-cpu suite — the
accessibility *plumbing* becomes regression-tested even though real
screen-reader output stays a manual pass.

### P2 — Fix the five pinned offenders + the status firehose — IMPLEMENTED

1. `UpdateProcessingStatusLabel()` routes its final text through
   `SetLiveText(label, text, politeness, "Pipeline status")`, with
   politeness chosen by transition class per the table (camera loss
   assertive; backend changes polite; unchanged-text no-op does the rest).
2. `ShowStatusMessage(message, duration)` gains a politeness parameter
   (default polite) and announces through the helper — one change makes
   every existing status call site audible.
3. `recordButton_`: `UpdateButton`/`PostButtonState` route the button text
   through the helper with prefix "Record" (silent for the button itself —
   the state *announcements* already come from the status path; the button
   just needs a truthful name for focus/queries).
4. `cameraFormatNoticeLabel_`, `assistantConnectionLabel_`,
   `assistantUsageLabel_` (usage = silent) converted; their `setA11y`
   pinned names become the `rolePrefix`.
5. Rule change in the `setA11y` helper's doc comment: **pinned
   `accessibleName` is for static widgets only; dynamic text must go
   through `SetLiveText`.**

### P3 — Sweep every remaining dynamic setText site — IMPLEMENTED

Grep-driven: every `->setText(` outside constructors/builders is either
(a) static-after-construction — leave, (b) dynamic — convert with the
policy table, or (c) a value label paired with an already-accessible
control — convert as silent. Known members of the list beyond P2:
percentile diagnostics labels, negotiated-format row, keystone tracking
status, annotation thickness/text-size value labels, and Setup Assistant
progress rows. Record the sweep results in this plan.

### P4 — Owner verification (screen reader in hand, ~20 min) — REMAINS

With NVDA and Narrator, on the real rig: focus the pipeline status label →
hears current state, not "Processing Status"; toggle camera off → hears
the assertive change without moving focus; start/stop a recording → hears
Stopping → Finishing → saved; sit idle on the Diagnostics section → hears
*nothing* unprompted (silent tier verified); rapid zoom changes → no
announcement chatter. Any failure here is a Qt-bridge quirk to document
and work around case-by-case (NVDA and Narrator cache differently — test
both).

## Acceptance

The P1 event tests run green in the CPU suite; every widget in the RC1
table reports its live text with role context when queried; every
`ShowStatusMessage` is announced politely exactly once; camera loss is
announced assertively; the silent tier stays silent under a running
camera; no announcement text repeats within 5 s from flapping; the
three-leg gate stays green; `docs/` accessibility notes gain the
static-vs-live rule.

## Out of scope

UIA LiveRegion property plumbing beyond what Qt's announcement event
already provides (revisit if a Qt upgrade — plan: pre-release wave —
exposes it directly); localization of announcements (idea 29-K owns
string strategy); braille-specific verification (falls out of NVDA pass).
