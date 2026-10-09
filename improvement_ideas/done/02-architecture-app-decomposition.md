# Architecture: Decomposing the App God-Object

`src/app/app.cpp` is 3,396 lines (as of 2026-07-22; it was 2,233 when this analysis was
written and keeps growing) and `OkuFlowApp` owns everything: 30+ raw widget
pointers, 39 `QSignalBlocker` uses, three independent "suspend sync" flags, the frame
pipeline, CUDA/fence state, recording, settings, and assistive features. Almost every
other improvement in this backlog is easier after this file is split.

**Sequencing advice:** do NOT attempt this as one big-bang refactor. Extract one
collaborator at a time, building and manually testing after each. Add the first unit
tests (see [06-build-tooling-docs.md](../06-build-tooling-docs.md)) *before* starting.

---

## A1. Extract collaborators from `OkuFlowApp`

- **Priority:** HIGH · **Effort:** large (do incrementally) · **Status:** Confirmed (file size/shape verified)
- **Evidence:** `src/app/app.cpp` — widget-pointer harvesting (~lines 279–318), 170 lines of signal connections (~343–512), pipeline orchestration (~1932–2022), recording (~2155–2229), assistive logic (~940–1004)

**Target shape** (extraction order = suggested implementation order):

1. `RecordingManager` (small, self-contained — good first extraction): owns
   `videoRecorder_`, recording state, output paths, the 12-hour cap. See A3.
2. `AssistiveFeatureManager`: owns `AssistiveRuntime` wiring, the analysis interval
   timer, and overlay updates.
3. `SettingsController`: owns `PersistentSettings` load/save, config↔UI mapping,
   quick-option promotion. Decouples persistence from widget code.
4. `UIStateManager`: owns the widget pointers and *all* `QSignalBlocker` /
   suspension-flag logic behind two methods: `ApplyConfigToUI(const AdvancedConfig&)`
   and `ReadConfigFromUI()`. After this, `OkuFlowApp` never touches a widget directly.
5. `PipelineOrchestrator`: owns CPU-vs-CUDA path selection, `EnsureCudaSurface`,
   fence state, and frame ticking. This is where the threading fix S4
   ([01-stability-threading.md](01-stability-threading.md)) lands.

Keep `OkuFlowApp` as a thin composition root that constructs and connects these.

---

## A2. Replace manual suspend-flag choreography with RAII guards

- **Priority:** MEDIUM · **Effort:** small · **Status:** Confirmed (three flags exist: `configTrackingSuspended_`, `presetSelectionSyncSuspended_`, `suspendControlSync_`)
- **Evidence:** `src/app/app.cpp` ~lines 277/721/743/872, 613/630/667/682/924, 1183/1191/1350/1359

**Problem.** Each flag is set, a long block of mutations runs, then the flag is manually
cleared. An early return or exception leaves the flag stuck and the UI silently stops
syncing. `ApplyAdvancedConfig()` (~741–875) is 130 lines between set and clear.

**Fix.** A 10-line RAII guard (`SuspendGuard g(configTrackingSuspended_);`) used at every
site. Delete-copy, set in ctor, clear in dtor. This is a mechanical, low-risk change and
a good warm-up for A1 step 4.

---

## A3. Recording needs a real state machine

- **Priority:** MEDIUM · **Effort:** medium · **Status:** Reported
- **Evidence:** `src/app/app.cpp` — record button slot (~415–430), `MaybeRecordFrame()` (~2155–2229) with three duplicated error-recovery blocks (~2172–2179, 2205–2213, 2224–2227)

**Problem.** Recording state is a bare `bool`. Error paths each hand-roll
"set flag false, block signals, un-check button, stop writer" with inconsistent
messaging; a failed `Start()` can leave the button in a wrong state. There is no
"starting" state, so a frame arriving mid-initialization hits ambiguous logic.

**Fix.** `enum class RecordingState { Idle, Starting, Recording, Stopping, Error }` with
one `SetRecordingState()` transition function that owns all UI updates and writer calls.
All error paths route through it. Fold into `RecordingManager` (A1 step 1) — doing A1
and A3 together is less work than sequentially.

---

## A4. Split construction from initialization; define init phases

- **Priority:** LOW · **Effort:** small · **Status:** Reported
- **Evidence:** `src/app/app.cpp` constructor (~247–318) — CUDA format resolution must precede presenter creation (~249→252), settings path resolution depends on `QCoreApplication` being set (~247–248, 264–276); `src/app/main.cpp:11-17` catches exceptions from a partially-constructed app.

**Problem.** Initialization order constraints are real but undocumented; a partially
thrown constructor leaves cleanup incomplete (timer started, window not shown, etc.).

**Fix.** Keep the constructor minimal and throw-safe; move risky work into
`bool Initialize()` called from `main()`, with an idempotent `CleanupPartial()` on
failure. Assert initialization completeness at public entry points. This becomes nearly
free after A1, since each collaborator owns its own setup/teardown.

---

## A5. Widget-pointer null-safety policy

- **Priority:** MEDIUM · **Effort:** small–medium · **Status:** Reported (severity depends on lifetime analysis — verify before large investment)
- **Evidence:** `src/app/app.cpp` — ~71 scattered null-checks on widget pointers, yet unguarded dereferences at e.g. `MapViewToSource()` (~1582), `PresentFitted()` (~2013–2015); `src/app/interaction_controller.cpp` `HandlePanScroll()` (~47 vs ~82)

**Problem.** The code is inconsistent: some paths null-check every widget pointer, others
dereference freely. Since all these pointers are set once in the constructor and the
window outlives the app loop, most checks are probably dead code and most "missing"
checks probably fine — but nobody can tell, which is the actual problem.

**Fix.** Pick one invariant and enforce it: widget pointers are non-null from
construction until destruction (assert once at end of constructor, drop the scattered
checks), OR they are nullable (then every access must check). The `UIStateManager`
extraction (A1 step 4) is the natural place to encode this — it can hold references
instead of pointers.
