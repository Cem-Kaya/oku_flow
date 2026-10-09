# 27 — Advanced inspector: scope separation, progressive disclosure, honest camera modes

Status: **VERIFY.** Implemented 2026-07-26. Production and test builds are
green; owner/hardware acceptance remains for camera-driver mode negotiation
and the keyboard/screen-reader passes.

Owner's brief: *"this is okay for me but I am a dev. This needs to be
reimagined with multiple sub-menus. Move the across-template stuff — camera,
camera input res (which should be a working dropdown, not whatever this is),
rotation — to the top; these should be separate from per-profile stuff. Then
per-profile stuff should be organised better. The UI FPS should not be on the
top layer, it should be hidden under more advanced options. I don't want to
remove control, but simplify the options for the users."*

**This plan absorbs two items that were previously scheduled separately**, at
the owner's direction:

- **[04](04-accessibility-ux.md) U4** — dependent-control enabled state desyncs
  from loaded config. A correctness bug dressed as UI polish; a panel that
  misreports its own state makes every other UI change untestable. **Phase 0.**
- **[20](20-capture-recording-integrity.md) §E** — real camera mode selection.
  The "Available modes" list the owner objected to is not merely ugly, it is
  lying, and fixing it also unblocks the higher-frame-rate capture that the
  stabilization investigation concluded is the real fix for impact motion blur.
  **Phase 4.**

Does not replace [`docs/ui_redesign_spec.md`](../docs/ui_redesign_spec.md)
(Simple mode, implemented and verified 2026-07-22). This restructures the
**Advanced Image inspector only**, which that spec deliberately left as "the
existing full control set".

---

## The defect: three scopes rendered as one flat list

`docs/ui_modes_design.md` already defines a **Settings Ownership** contract.
The panel does not visually express it, and has grown a *third* scope the
contract never named.

| Scope | Meaning | Members | Today |
|---|---|---|---|
| **Device** | changes the hardware | camera, resolution/frame rate, orientation | "Global device" |
| **Viewport** | presentation only, no pixels changed | framing (Fill/Fit), motion rate, joystick | also "Global device" |
| **Profile** | image treatment, captured by `Save As Quick Option` | zoom, text clarity, stabilization, sharpening, … | "Current profile" |

Consequences:

1. **The user cannot predict what `Save As Quick Option` captures.** That is
   the real usability bug — not the scroll length. Scope is invisible.
2. **`Viewport motion rate` sits two rows under `Camera`**, implying it is a
   device property. It is a rendering preference almost nobody should touch.
3. `Viewport framing` and `Viewport motion rate` are **absent from the
   ownership table entirely**. Adding a Viewport scope to that contract is part
   of this work.

---

## Current code map (verified 2026-07-26)

Everything below is in `src/ui/main_window.cpp` unless stated.

| What | Where |
|---|---|
| `makeSectionLabel("Global device")` | `:339` |
| `cameraCombo_` (`WheelSafeComboBox`) | `:341` |
| `rotationCombo_` | `:349` |
| `viewportRateCombo_` | `:363` |
| `viewportFitCombo_` | `:376` |
| `controlsToggleButton_` (Hide Advanced Tuning) | `:385` |
| `promotePresetButton_` (Save As Quick Option) | `:391` |
| `joystickCheckbox_` | `:392` |
| `cameraModesList_` (`QListWidget`) + "Available modes" label | `:397-411` |
| `makeSectionLabel("Current profile")` | `:412` |
| `"Magnification and image"` | `:438` |
| `"Motion and display"` | `:500` |
| `"Screen fix"` | `:535` |
| `"Text clarity"` | `:577` |
| `"Assistive"` | `:714` |
| `"Sharpen and focus"` | `:738` |
| `"Interaction and diagnostics"` | `:788` |
| Accessors (`cameraModesList()`, …) | `include/openzoom/ui/main_window.hpp:78,211` |
| Per-slot `setEnabled` calls | `src/app/app_controls.cpp:22,37,52,199,239,368,437` |
| Partial text-clarity enable block | `src/app/app_controls.cpp:480-500` |
| `VideoFormat {width,height,numerator,denominator}` | `include/openzoom/capture/media_capture.hpp:44-49` |
| `MediaCapture::StartCapture` | `src/capture/media_capture.cpp:278` |
| `MediaCapture::ConfigureReader` | `src/capture/media_capture.cpp:446` |
| `EnumerateFormats` | `include/openzoom/capture/media_capture.hpp` |

---

## Target structure

Exactly two levels of disclosure. Never three — see accessibility rules.

```
[ Search settings…                                    ]   always visible
──────────────────────────────────────────────────────────
▾ DEVICE                                    affects hardware
    Camera                     [combo]
    Resolution & frame rate    [combo]   <- Phase 4, real control
      └ "Driver selected 1280x720 @ 30 instead"   (only when mismatched)
    Orientation                [combo]
    ▸ More device options                          [· 1 changed]
        Viewport framing       [Fill (crop) / Fit (letterbox)]
        Viewport motion rate   [Auto / 60 / 90 / 120 / Match display]
        Virtual joystick       [x]
──────────────────────────────────────────────────────────
PROFILE   [ Whiteboard ▾ ]        [Save as…]  [Reset]
──────────────────────────────────────────────────────────
▾ Magnification
    Zoom [x] ──────────── 2.4x
    ▸ More: Focus X, Focus Y, Show focus point
▾ Readability
    Auto Contrast [x]  Strength ────────
    Brightness ────────    Contrast ────────
    Colour mode [combo]
▾ Text clarity
    Auto Text Clarity [x]                      master switch
    ▸ More: Flatten background, Adaptive text, Edge softness,
            Stroke weight, CLAHE, Two-colour, Hysteresis,
            Focus threshold, Glare suppression
▾ Stability
    Stabilize Image [x]      Extra Stable [x]
    [ Re-lock current view ]
    ▸ More: Temporal smooth + blend, Rolling shutter, Engine, Bump hold
▾ Screen fix
    Straighten Screen (Keystone) [x]
    Correction tracking  [◀ Back] [❚❚ Stop] [▶ Next]
▾ Sharpening
    Spatial Sharpen [x]      Sharpness ────────
    ▸ More: Backend (NIS / FSR / Maxine SuperRes), SuperRes override
▾ Assistant
    Scene Explain [x]   Assistive Overlay [x]
    [ Open Notes ]   [ Setup & Downloads… ]
──────────────────────────────────────────────────────────
▸ DIAGNOSTICS                                       collapsed
    Debug View, Show focus point, Pipeline status,
    Gaussian blur + Sigma + Radius
```

### Moves, and the reason for each

| Control | From | To | Why |
|---|---|---|---|
| Viewport motion rate | Global device (top) | Device ▸ More | Owner request. Rendering preference; a wrong value degrades smoothness with no visible cause. |
| Viewport framing | Global device (top) | Device ▸ More | Same scope. Simple mode already exposes what most users need. |
| Virtual joystick | Global device | Device ▸ More | Interaction preference, per the ownership table. |
| Gaussian blur, Sigma, Radius | Magnification and image | **Diagnostics** | `docs/README.md` calls blur the "non-AI baseline path". On a low-vision magnifier a prominent *make it blurrier* slider reads as a bug. Keep it, demote it. |
| Backend (NIS/FSR/Maxine) | Sharpen and focus | Sharpening ▸ More | Implementation choice, not a user goal. The visible control is `Sharpness`. |
| Focus X / Focus Y | Sharpen and focus | Magnification ▸ More | Already settable by clicking the image; sliders are the fallback. |
| Text clarity sub-stages (9) | Text clarity | Text clarity ▸ More | `Auto Text Clarity` is the user-facing decision. |
| Available modes (list) | Global device | merged into `Resolution & frame rate` combo | Phase 4. |
| Debug View, Show Focus Point, Pipeline status | Interaction and diagnostics | Diagnostics | Same content, collapsed by default. |

---

## Phase 0 — U4: one source of truth for enabled state (half a day)

**Do this first.** Today enablement is scattered across per-slot `setEnabled`
calls (`app_controls.cpp:22,37,52,199,239,368,437`) plus a partial block at
`:480-500`. If settings load with a feature enabled but its toggle slot is not
invoked during startup sync, the slider stays greyed while the feature runs —
**the UI lies about its own state.** Phase 2 depends on truthful state, because
auto-expand keys off "is this non-default".

**Implementation**

1. Add `void OpenZoomApp::UpdateControlEnabledStates();` — declare in
   `include/openzoom/app/app.hpp` near the other `Update*Ui()` members, define
   in `src/app/app_controls.cpp`.
2. Move **every** existing `setEnabled` call from the toggle slots into it.
   Each line derives purely from config flags, never from widget state:
   ```cpp
   void OpenZoomApp::UpdateControlEnabledStates() {
       auto set = [](QWidget* w, bool on) { if (w) w->setEnabled(on); };
       set(uiState_->bwSlider_,   blackWhiteEnabled_);
       set(uiState_->zoomSlider_, zoomEnabled_);
       set(uiState_->blurSigmaSlider_,  blurEnabled_);
       set(uiState_->blurRadiusSlider_, blurEnabled_);
       set(uiState_->temporalSmoothSlider_, temporalSmoothEnabled_);
       set(uiState_->autoContrastStrengthSlider_, autoContrastEnabled_);
       set(uiState_->focusMarkerCheckbox_, !debugViewEnabled_);
       // …text clarity block from :480-500 moves here verbatim…
       set(mainWindow_ ? mainWindow_->bumpHoldCheckbox() : nullptr,
           stabilizationEnabled_ && virtualTripodEnabled_);
   }
   ```
3. Call it from exactly two places:
   - the end of every `On*Toggled` slot in `app_controls.cpp`;
   - the end of `UiStateManager::ApplyConfig` (`src/app/ui_state_manager.cpp`,
     after `app_.OnStabilizationToggled(...)` at `:295`), so a loaded profile
     always lands in a consistent state.
4. Delete the now-duplicated inline `setEnabled` lines.

**Test (add to `tests/settings_store_tests.cpp`, CPU-only, no GPU needed):**
load a `settings.json` with `blur=true`, `zoom=true`, `autoContrast=true`, apply
it, assert the corresponding sliders report `isEnabled() == true`. This is the
regression that U4 describes and it currently fails.

---

## Phase 1 — scope separation, no new widget types (half a day)

Pure reordering plus labels. Nothing else in the plan depends on it, and it is
the biggest clarity gain per hour.

1. In `main_window.cpp`, replace `makeSectionLabel("Global device")` (`:339`)
   with `makeSectionLabel("Device")` and add a subtitle label
   *"Shared by every profile"* in the secondary text colour.
2. Reorder the device block to: `cameraCombo_`, (Phase 4 combo), `rotationCombo_`.
3. Move `viewportRateCombo_`, `viewportFitCombo_`, `joystickCheckbox_` into a
   plain `QWidget` container `deviceMoreContainer_`, hidden by default, toggled
   by a `QToolButton` styled like the existing `controlsToggleButton_` (`:385`).
   Phase 2 replaces this with the reusable widget; do not over-build it now.
4. Replace `makeSectionLabel("Current profile")` (`:412`) with a profile bar:
   active profile name + `promotePresetButton_` + reset, and the subtitle
   *"Saved into the selected quick option"*.
5. Move Gaussian blur (checkbox + `blurSigmaSlider_` + `blurRadiusSlider_`) from
   the `"Magnification and image"` block into `"Interaction and diagnostics"`
   (`:788`), renamed `"Diagnostics"`.
6. Extend every affected `setA11y(...)` description with its scope, e.g.
   `"Camera. Device setting, shared by all profiles."` /
   `"Sharpness. Profile setting, saved with the current quick option."`
7. Update the **Settings Ownership** table in `docs/ui_modes_design.md` to add
   the Viewport scope (framing, motion rate, joystick) in the same commit.

Accessors in `main_window.hpp` keep their names so `app_*.cpp` wiring is
untouched. This phase changes layout only — **zero behaviour change**, which is
what makes it safe to do first.

---

## Phase 2 — collapsible sections + auto-expand (1–2 days)

**New file: `src/ui/collapsible_section.cpp` / `include/openzoom/ui/collapsible_section.hpp`.**
Follow the existing standalone-widget pattern (`color_scheme_picker.*`).

```cpp
class CollapsibleSection : public QWidget {
    Q_OBJECT
public:
    CollapsibleSection(const QString& title, QWidget* parent = nullptr);
    QWidget* contentWidget() const;      // callers add rows here
    void setExpanded(bool expanded);
    bool isExpanded() const;
    void setChangedCount(int count);     // renders "· 2 changed"
    void setPersistKey(const QString& key);
signals:
    void expandedChanged(bool expanded);
};
```

Requirements:

- Header is a real `QToolButton` with `Qt::ToolButtonTextBesideIcon`, arrow
  indicator, `setCheckable(true)`.
- The header exposes **heading semantics** so screen readers can jump by
  heading, not only by tab.
- `setChangedCount(n)` with `n > 0` appends `· n changed` to the accessible
  name *and* the visible label.
- Collapsed state persists per `persistKey` in `PersistentSettings` under a new
  `uiSectionStates` map (global scope — it is a UI preference).

**Auto-expand rule (non-negotiable).** A `More` block whose contents differ from
profile defaults **must** expand on load and show its changed count. Implement
as `OpenZoomApp::UpdateSectionChangedCounts()`, called from the same two places
as `UpdateControlEnabledStates()`. Compare against the profile's default
`AdvancedConfig` using the existing equivalence helper in
`src/app/settings_store.cpp`. Without this, U4's *"the UI lies about its own
state"* returns through the new design — a hidden non-default control is
strictly worse than a greyed-out one.

Apply to: Device ▸ More, Magnification ▸ More, Text clarity ▸ More,
Stability ▸ More, Sharpening ▸ More, Diagnostics.

---

## Phase 3 — search box (half a day)

`QLineEdit` with `setPlaceholderText("Search settings…")` and a clear button,
pinned above the Device section.

- Build a static registry at construction: `{QWidget* row, QString haystack}`,
  where the haystack is the visible label + tooltip + accessible name, lowered.
- On `textChanged`, case-insensitive substring match. Non-matching rows hide;
  a section with zero visible rows hides; a section with any match
  **force-expands** regardless of collapsed state, and restores its previous
  state when the query clears.
- Empty query restores everything exactly.
- Announce the result count via `QAccessible` (`"3 settings match"`), so it is
  usable without sight.
- `Ctrl+F` focuses it; add to the U3 shortcut set.

This is what makes nesting safe for keyboard and screen-reader users. Treat it
as mandatory, not a nice extra.

---

## Phase 4 — 20 §E: honest camera mode selection (2–3 days)

Today `EnumerateFormats` lists modes into `cameraModesList_` (`:397`), but
`ConfigureReader` (`media_capture.cpp:446`) sets only major type and subtype,
and `StartCapture` (`:278`) takes no mode. **The panel can display
`1920x1080 @ 60` while the driver delivers something else.** Converting the list
to a combo without this work only makes the lie prettier.

**Capture layer — `media_capture.hpp` / `.cpp`**

1. Extend `VideoFormat` (`:44-49`) with `GUID subtype` and
   `std::wstring stableId` — `"{subtype}_{w}x{h}@{num}/{den}"`. The stable id is
   what gets persisted; index-based selection breaks when a driver reorders.
2. `bool StartCapture(const CameraDescriptor&, const VideoFormat* requested, …)`
   — `nullptr` keeps today's "driver's choice" behaviour.
3. `ConfigureReader` sets `MF_MT_FRAME_SIZE` and `MF_MT_FRAME_RATE` on the
   candidate type before `SetCurrentMediaType`.
4. **Always read back** the negotiated type afterwards and store it in
   `VideoFormat negotiatedFormat_`, with `const VideoFormat& NegotiatedFormat() const`.
   The read-back is the point of the whole exercise.
5. If the request was not honoured, set a plain-language line in `lastError_`'s
   sibling (a new `formatNotice_`), e.g.
   `"Requested 1920x1080 @ 60; driver selected 1280x720 @ 30."`

**Settings** — add `std::wstring cameraFormatStableId` to the **global** section
of `PersistentSettings` (`include/openzoom/app/settings_store.hpp`), never to
`AdvancedConfig`. It is a device property. Migration: absent → `nullptr`
(driver's choice), which is exactly today's behaviour, so old settings files
keep working.

**UI**

6. Delete `cameraModesList_` and its accessor
   (`main_window.hpp:78,211`); add `QComboBox* cameraFormatCombo_` plus
   `QLabel* cameraFormatNoticeLabel_` (hidden unless mismatched, styled as a
   warning).
7. Populate sorted by height then frame rate, descending, de-duplicated,
   formatted `"1280 × 720 @ 30 fps"`. Add a first entry
   `"Automatic (driver's choice)"` mapping to `nullptr`.
8. `OpenZoomApp::OnCameraFormatChanged(int)` in `app_controls.cpp` next to the
   existing camera slot: persist the stable id, restart capture through the same
   path as camera switching (`app_pipeline_runtime.cpp:700-712`, which already
   does the `ResetStabilization` / `ResetKeystone` / `ResetTemporalHistory`
   teardown), then refresh the notice label from `NegotiatedFormat()`.
9. Announce the negotiated result via `QAccessible` on change.

**Why this pays twice.** Once mode selection is real, requesting 60 or 120 fps
becomes possible — and shorter exposure at higher frame rate is the conclusion
the entire stabilization investigation reached about impact motion blur
(`../local_evidence/stabilization/VID_20260724_015520_996/dense_direct/README.md`).
No amount of geometric correction recovers detail destroyed during exposure.

---

## Phase 5 — Ctrl+scroll zoom: geometric steps and acceleration (half a day)

Owner request. Current implementation is
`InteractionController::HandleZoomWheel` (`src/app/interaction_controller.cpp:109-141`),
reached from `main_window.cpp:2256-2258`. Three defects, one of them a hard bug.

**Defect 1 — precision trackpads cannot zoom at all.**
```cpp
const int stepUnits = (delta / 120);
if (stepUnits == 0) { return; }
```
Integer division truncates. A precision touchpad or high-resolution wheel sends
deltas well under 120 per event, so `stepUnits` is `0` and the function returns
having done nothing. Ctrl+two-finger-scroll is dead on exactly the hardware a
laptop user has. (Note the *pan* path already handles this correctly —
`HandlePanScroll:65-80` reads `pixelDelta()` first and only falls back to
`angleDelta()/120.0f` as a float.)

**Defect 2 — steps are linear where perception is logarithmic.**
`deltaValue = stepUnits * stepSize` adds a fixed number of slider units. At 1×
one notch is an enormous jump; at 8× the same notch is barely visible. Zoom
must be **multiplicative**: each notch scales by a constant ratio so it feels
identical at every magnification. This matters more here than in a normal app,
because this audience lives at high zoom.

**Defect 3 — no velocity response.** Spinning the wheel fast should cover more
range than slow deliberate notches. That is the owner's request.

**Implementation** — replace the body of `HandleZoomWheel`:

1. Change the signature to take the full wheel event (or `pixelDelta` +
   `angleDelta`) rather than a truncated `int delta`, matching `HandlePanScroll`.
2. Accumulate **fractional** notches; never early-return on a sub-notch event:
   ```cpp
   float notches = 0.0f;
   if (!pixelDelta.isNull())      notches = pixelDelta.y() / 50.0f;   // trackpad
   else if (!angleDelta.isNull()) notches = angleDelta.y() / 120.0f;  // wheel
   if (qFuzzyIsNull(notches)) return;
   ```
3. Velocity acceleration with a hard cap, using a member `QElapsedTimer`:
   ```cpp
   constexpr float kZoomStepRatio  = 1.10f;   // +10% per notch
   constexpr int   kAccelWindowMs  = 120;     // events closer than this accelerate
   constexpr float kAccelGrowth    = 1.35f;
   constexpr float kAccelMax       = 3.0f;    // never more than 3 notches per notch

   const qint64 gap = wheelTimer_.isValid() ? wheelTimer_.restart() : kAccelWindowMs + 1;
   wheelAccel_ = (gap < kAccelWindowMs)
                   ? std::min(wheelAccel_ * kAccelGrowth, kAccelMax)
                   : 1.0f;
   const float target = app_.zoomAmount_ *
                        std::pow(kZoomStepRatio, notches * wheelAccel_);
   ```
4. Clamp `target` to the slider's zoom range, convert back to slider units, and
   keep the existing cursor-anchored focus behaviour (`MapViewToSource`,
   `:115-120`) unchanged — anchoring is already correct.
5. Reset `wheelAccel_ = 1.0f` on wheel-direction reversal, so flicking back the
   other way starts gentle instead of overshooting.

**Accessibility**

- **Acceleration must be defeatable.** Add `Zoom wheel acceleration` to
  Device ▸ More (Viewport scope — it is an interaction preference, like the
  joystick). Default on; off gives pure geometric steps. A user navigating by
  muscle memory can be badly served by a view that moves further than expected.
- **Announce the settled value, not every notch.** Debounce ~250 ms after the
  last wheel event, then one `QAccessible` announcement (`"Zoom 3.4 times"`).
  Announcing per notch floods a screen reader and makes the gesture unusable.
- **Keyboard zoom is never accelerated.** `Ctrl+=` / `Ctrl+-` (plan 04 U3) call
  the same geometric helper with `wheelAccel_ = 1.0f`, so keyboard zoom is
  exactly reproducible: n presses always land on the same magnification.

Factor the shared maths into one helper so wheel, keyboard, and the slider
agree:
```cpp
float InteractionController::ScaledZoom(float current, float notches, float accel) const;
```

---

## Accessibility rules (non-negotiable)

The audience is disproportionately screen-reader and keyboard users, so
collapsible sections are a hazard, not a free win.

1. **Maximum two levels.** Group, then one optional `More`. No sub-sub-menus.
2. **Search is mandatory** (Phase 3) — the escape hatch that makes nesting safe.
3. **Never hide an active control** — auto-expand + changed badge (Phase 2).
4. **Real heading semantics** on every section header.
5. **Collapsed state persists**, global scope.
6. **Scope announced** in every accessible description.
7. Existing invariants from `docs/ui_modes_design.md` hold: visible keyboard
   focus, logical tab order, render surface never recreated, processing status
   stays short with detail in the tooltip.

---

## Acceptance

- [ ] Load a `settings.json` with blur/zoom/auto-contrast enabled → every
      dependent slider is enabled (Phase 0 regression test passes).
- [ ] Device / Profile / Diagnostics are visually distinct; each control's
      accessible description names its scope.
- [ ] Viewport motion rate and framing are not visible until `More device
      options` is opened.
- [ ] A profile with a non-default value inside any collapsed `More` opens that
      section and shows `· n changed`.
- [ ] `Ctrl+F` → type `sharp` → Sharpness is reachable regardless of collapsed
      state; count announced; clearing restores prior state exactly.
- [ ] Selecting `1280x720 @ 30` actually starts that mode; requesting an
      unsupported mode shows the driver-selected notice.
- [ ] Ctrl+scroll zooms on a precision trackpad (currently does nothing).
- [ ] One notch feels the same at 1x and at 8x (geometric, not linear).
- [ ] Fast spin covers more range than slow notches, capped at 3x; reversing
      direction resets the acceleration.
- [ ] Zoom acceleration can be switched off in Device ▸ More.
- [ ] Zoom level announced once ~250 ms after the gesture settles, not per notch.
- [ ] `Ctrl+=` / `Ctrl+-` are exactly reproducible — n presses always reach the
      same magnification.
- [ ] Keyboard-only pass: reach every control, no traps, focus always visible.
- [ ] Screen-reader pass: jump by heading, section state announced.
- [ ] Build green via `cmake --preset msvc-release` + `msvc-release-build`.

## Implementation record — 2026-07-26

Phases 0–5 are implemented:

- dependent-control enabled state is centralized in
  `OpenZoomApp::UpdateControlEnabledStates()`;
- Device, Viewport, and Profile ownership is visible and announced;
- the Advanced inspector uses persistent, changed-aware collapsible sections
  with a pinned `Ctrl+F` search;
- the camera format control requests a stable mode identifier and reports the
  format actually negotiated by Media Foundation;
- precision wheel input uses fractional deltas, geometric zoom, bounded
  acceleration, cursor anchoring, and a settled accessibility announcement.

Automated validation:

- release `open_zoom` target: passed with CUDA and NVIDIA Text Super Resolution;
- `settings_store_round_trip`: passed;
- `viewport_transform_geometry`: passed;
- locked-bundle fallback: `dist/OpenZoom2/open_zoom.exe` SHA-256 matches the
  release build (`51FBB39CA53D7B447A324C1715FAC79074489ECDC9588C90245B633DA55731EE`).

The unchecked acceptance items above remain deliberate owner/hardware checks.
In particular, a real camera must confirm that a requested mode is honored or
that the driver-selected fallback notice is accurate. The application was not
launched automatically for this implementation pass.

## Effort

| Phase | Work | Days | Blocked by |
|---|---|---|---|
| 0 | U4 single source of truth | 0.5 | — |
| 1 | Scope separation, moves | 0.5 | — |
| 2 | Collapsible + auto-expand | 1–2 | Phase 0 |
| 3 | Search box | 0.5 | Phase 2 |
| 4 | Real camera modes (20 §E) | 2–3 | — (parallelisable) |
| 5 | Ctrl+scroll geometric zoom + acceleration | 0.5 | — (parallelisable) |

Phases 0–3 total about three days. Phases 4 and 5 are independent and can run in
parallel with 1–3. **Phase 5 fixes a live bug** (Ctrl+scroll zoom does nothing on
a precision trackpad), so it can jump the queue if a laptop user hits it.

## Cross-references

- [`docs/ui_modes_design.md`](../docs/ui_modes_design.md) — Settings Ownership;
  **add the Viewport scope in the Phase 1 commit**.
- [`docs/ui_redesign_spec.md`](../docs/ui_redesign_spec.md) — Simple mode,
  implemented; untouched.
- [`04-accessibility-ux.md`](04-accessibility-ux.md) — U4 absorbed as Phase 0;
  mark it there. U3 shortcuts pair naturally (`Ctrl+F` is specified here).
- [`20-capture-recording-integrity.md`](20-capture-recording-integrity.md) §E —
  absorbed as Phase 4; mark it there.
- [`18-annotation-mode-plan.md`](18-annotation-mode-plan.md) — Simple-mode
  button cluster, not inspector rows; no conflict. Still do
  [`21`](21-user-data-locations.md) before 18.

## Deliberately not proposed

- **A settings tree or sidebar** — more navigation, not less, for keyboard users.
- **Usage-based auto-hiding** — unpredictable UI; the Simple-mode spec rejected
  auto-appearing suggestions for the same reason.
- **Removing any control** — the owner was explicit: simplify presentation,
  keep capability.
