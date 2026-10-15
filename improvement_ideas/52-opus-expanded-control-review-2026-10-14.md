# 52 — Opus expanded-control review (2026-10-14)

Review sub-agent report for Plan 52 / 0.3.1 "all user-adjustable knobs". Source:
`C:\Users\cemka\AppData\Local\OkuFlow\worktrees\agentic-control`, branch
`feature/agentic-control-mcp`, uncommitted shared WIP. Nothing was committed,
pushed, published or promoted, and no child agent was used. No GUI, camera,
phone, provider, account, browser, secret or owner-settings operation ran; all
checks were background, desktop-free builds in a private LocalAppData tree.

**Source capture.** The tree changed repeatedly during the review (core, image
and HTTP workers were landing code). Findings and results below refer to the
final private build at **22:11 TST**, with these files:

| File | mtime | SHA-256 (12) |
|---|---|---|
| `src/app/agent/app_control_service.cpp` | 22:10 | `2cb1cc881ae3` |
| `src/app/agent/app_control_target.cpp` | 22:09 | `1e0c1727095d` |
| `src/app/agent/app_control_image_parameters.cpp` | 22:09 | `908a26265175` |
| `src/app/agent/app_control_assistant_parameters.cpp` | 22:08 | `f1a457e4c6a9` |
| `src/app/agent/assistant_parameter_policy.cpp` | 22:08 | `1dce42fcd125` |
| `src/app/agent/parameter_registry.cpp` | 21:51 | `14269be14853` |
| `src/app/agent/control_registry.cpp` | 21:51 | `b015d39fb01a` |
| `include/okuflow/app/agent/control_schema.hpp` | 20:08 | `4044049e37ae` |
| `src/mcp/json.cpp` | 20:18 | `455b92c5d095` |
| `tests/data/app_control_inventory.json` | 21:53 | `29b3173e063a` |
| `tests/agent_control_tests.cpp` | 20:55 | `ee990371d310` |

`app_control_target.cpp` and the app glue were read, not compiled: they need
`OkuFlowApp`. In-progress code is described as in progress, not as a final bug.

## Summary

- **94 adjustable fields.** 69 have production pure planners that this review
  exercised field by field (58 image/view, 11 assistant/notes). None has a
  validated native grouped effect yet. At capture the target routed only the
  assistant/notes groups; image/view grouped tools still returned `unavailable`
  (`app_control_target.cpp:119`). 25 fields have no planner: camera
  orientation/selection exist only as legacy typed tools, and 23 fields
  (camera format/acceleration/torch, recording 2, notes root, UI 9,
  annotation 8) have no planner and no `Field` enum value.
- **New module:** `tests/agent_control_coverage_tests.cpp` has 35 slots: 35 passed and
  0 failed. 9 open reviewed defects report as conditional `XFAIL`. They do not
  fail the gate and pass unchanged once repaired.
- **Repaired during review (re-verified):** F-01 approval plan comparison (core,
  22:06), F-02 frame-egress domain for `sceneExplain` (`requiresFrames`, core
  22:06 plus HTTP worker 22:08), F-05 code-point counting (HTTP worker, 21:46),
  F-15 registration of `assistant_parameter_policy.cmake` (parent, by 22:10),
  F-17 incomplete planner file (image worker, 22:05). Their slots are now hard
  regression checks.
- **Gate snapshot (private build, not CTest):** `agent_control_contract` sources
  ran **29/31 with 2 failures**: core `Begin()` now refuses an active context, but two
  reentrant-replacement regressions written earlier expect replacement (F-16).
  The policy contract passed 10/10 and the schema contract 6/6.

## Evidence classes

| Class | Used here | Proves | Does not prove |
|---|---|---|---|
| Structural registry/inventory | yes | IDs, groups, schemas, persistence, handlers, protected/action classification, paging | Native effects, hardware |
| Schema validators | yes (both) | App QJson subset and Qt-free bridge subset agree on a 1789-value corpus; keyword subset; finite bounds | Provider SDK behavior beyond the subset |
| Fake target (labelled) | yes | Service admission, approvals, budgets, events, receipts, fencing | That native handlers behave the same way |
| Native pure planner/policy | yes | Real production planning, coupling, quantization, approval domains | Widget/handler execution |
| Native pure normalizer | yes | `ImageParameterEffectiveIntent` equals `ProcessingSettings::ApplyTextClarityMaster/TextClarityStages` for all 1024 combinations | GPU output |
| Offscreen UI | no | n/a | No screenshot, focus, screen-reader or layout claim |
| Hardware / CUDA / Maxine | no | n/a | No device, GPU, phone or camera acceptance |
| Live provider | no | n/a | No Codex/HTTP provider acceptance |
| Release | no | n/a | No `agent_build.bat`, bundle, MSI, `publish_windows_release.ps1` or V1 claim |

## Prioritized findings

| ID | Severity | Area | Status at 22:11 | Test slot |
|---|---|---|---|---|
| H-1 | High (on routing) | Core wiring | Open | n/a (needs `OkuFlowApp`) |
| F-03 | High | Service atomicity | Open | `failedApplyLeavesNoPartialEffects` (XFAIL) |
| H-5 | High | Service domain gates | Open (frames/photos gates landed; others pending) | n/a |
| F-06 | Medium | Image planner | Open | `coupledResetsFollowActualChanges` (XFAIL) |
| F-07 | Medium | Image planner / legacy tools | Open | `explicitZoomRequestIsNeverSilentlyInverted` (XFAIL) |
| F-08 | Medium | Receipts | Open | `groupedReceiptsUseTheRequestVocabulary` (XFAIL) |
| F-09 | Medium | Discovery | Open | `discoveryApprovalFlagsMatchActualPlanners` (XFAIL) |
| F-04 | Medium | Validator | Open | `anchoredPatternsRejectTrailingLineBreaks` (XFAIL) |
| F-13 | Medium | Async grouped setters | Open | n/a (camera planner absent) |
| F-11 | Medium | Catalog/promotion | Open | n/a |
| F-22 | Medium-Low | Saved vs live | Open | n/a (needs app) |
| F-16 | Gate | Core contract | Open (in flight) | existing `agent_control_contract` |
| F-10 | Low | Wire bounds | Open | `everyAgentInputIsFinitelyBounded` (XFAIL) |
| F-12 | Low | Approval text hardening | Open | `assistantSingleLineValuesRejectInvisibleFormatting` (XFAIL) |
| F-14 | Low | Bounded reads | Open | `assistantReadsProjectAndBudgetWithoutTruncation` (XFAIL) |
| F-18 | Low | Inventory completeness | Open | n/a (inventory data) |
| F-19 | Low | Inventory/doc drift | Open | `inventoryAndRegistryAgreeForEveryAdjustableField` (records 25 advisory keywords) |
| F-20 | Low | Assistant normalization | Open | n/a |
| F-21 | Low | SR approval | Open | n/a |
| F-01 | High | Approvals | **Fixed 22:06** | `approvalExecutesOnlyTheReviewedPlan` (hard) |
| F-02 | High | Frame egress | **Fixed 22:06/22:08** | `frameEgressPreferenceNeedsFrameGrantBeforeEffects` (hard) |
| F-05 | Medium | Unicode bound | **Fixed 21:46** | `assistantPromptBoundCountsCodePoints` (hard) |
| F-15 | Gate | Test registration | **Fixed by 22:10** | n/a |
| F-17 | Gate | WIP compile | **Fixed 22:05** | module compiles the real file |
| F-23 | Low | Handler churn | **Addressed (forward path, unvalidated)** | n/a |

### H-1 — New fields are invisible to baselines, user-wins and `Run()` lookup (High once routed)

- Evidence: `AppControlTarget::Fields()` (`app_control_target.cpp:73-81`) merges
  `AssistantParameterFields()` but not `ImageParameterFields()`.
  `AppControlService::Run` looks up every write in `target_.Fields()` and marks
  the batch `partial` when a field is absent (`app_control_service.cpp:499`).
  `NoticeNativeChanges` diffs the same map, so native edits to SR, debug view,
  bump hold, keystone pause, presentation rate, joystick, wheel acceleration and
  the custom recipe never bump a `user` revision.
- Effect once image/view routing lands: every new-field write is `partial`, and
  native-user-wins/undo-skips are not enforced for those 18 fields. `Begin()`
  also seeds baselines from `Fields()`.
- Fix: merge `ImageParameterFields()` and later groups into `Fields()` before
  routing Prepare. Extend `NoticeNativeChanges` triggers to the new widgets and
  session states. Add a desktop-free app-level fixture where possible.

### F-03 — A failed `Apply` leaves earlier writes of the batch applied (High)

- Evidence: `app_control_service.cpp:499` and `:505` (`partial = true; break;`)
  with no rollback. `Save()` still runs and an `applied` event is emitted (`:519`).
  `BeginFieldPlan` (`:491`) pre-validates image widgets only; assistant writes
  (`ApplyAssistantParameter`) and later failures are not covered.
- Repro (fake target, labelled): plan `{contrast: 2.0, brightness: 0.5}` with
  `Apply(brightness)` returning false leaves contrast at 2.0 and returns
  `{ok:false, partial:true}`. The inventory contract is "whole invalid batch,
  zero effects".
- Fix: on failure, restore this batch's applied writes in reverse through
  `Apply(previous)`, revert their revisions and drop their journal entries.
  Alternatively, make `BeginFieldPlan` validate every group and treat a later
  failure as rollback plus `internal`.

### H-5 — Grouped capture/output domains still rely on planner flags (High, partly addressed)

- Evidence: `HasDomainGrant` (`app_control_service.cpp`, ~248-262 at 22:14) now
  denies when a plan declares `requiresFrames` (22:06) or `requiresPhotos` (added
  about 22:13, after the 22:11 test build). Recording/capture configuration,
  output destination and non-camera source effects still reach
  `default: return true`. Affected setters include `recording_settings_set`
  (Tier Capture: a microphone change stops recordings) and
  `notes_preferences_set` (`notes_output.root`). Every flag must also be declared
  by the planner, so a forgotten flag silently widens authority.
- Fix: derive the required grant from the registry `ParameterPermission` of
  every planned write (`CaptureConfiguration`, `PhotoEgress`, `OutputDestination`,
  `AnnotationEdit`, `Source`, `FrameEgress`). Keep the planner flags as an
  additional assertion, not as the only gate. This needs the `Field` to
  registry-id map from F-08.

### F-06 — Couplings follow mentioned fields, not changed fields (Medium)

- Evidence: `app_control_image_parameters.cpp:241-245` couples
  `KeystonePaused=false` whenever stabilization/keystone is mentioned.
  `:236` enforces the SR zoom minimum whenever any text field is mentioned.
- Repro (pure planner): with stabilization on and keystone paused,
  `{image.stabilization.enabled:true}` plans `KeystonePaused=false`. A native
  `setChecked(same)` emits nothing, so the agent resumes keystone tracking without
  `ResetKeystone`. With SR on and zoom off, `{image.textClarity.enabled:false}`
  (unchanged) plans `ZoomEnabled=true`.
- Fix: compute couplings from `desired != current`.

### F-07 — Explicit zoom requests are silently rewritten under SR (Medium)

- Evidence: `app_control_image_parameters.cpp:236-240`. Natively,
  `OnZoomToggled`/`SetZoom` (`app_controls.cpp:499-547`) do not enforce the SR
  minimum. Processing just skips SR below 1.33
  (`app_pipeline_runtime.cpp`: `enableMlSuperRes && !ultra && zoomAmount >= 1.33f`).
- Repro (pure planner): SR on, zoom 1.5. `{view.zoom.enabled:false}` plans
  `true` with no error, and `{view.zoom.amount:1.0}` plans 1.33.
- Related: legacy `view_reset` writes 1.0 (`app_control_target.cpp:143`).
  `PresetAllowed` (`:34-46`) denies any SR difference, while the new planner
  admits SR changes behind exact approval.
- Fix: choose one policy for all three tools. Either mirror native behavior
  (couple only when SR/text values actually change) or reject with a fixed reason.
  Report normalization explicitly (for example a `normalized` map) and reconcile
  `PresetAllowed` with the new approval domain.

### F-08 — Grouped receipts use a different vocabulary from requests (Medium)

- Evidence: `FieldName()` (`app_control_service.cpp:31-50`) names 49 of the 58
  image/view fields with legacy tokens (`contrast`, `framing`, `image.debug`,
  `image.superRes.enabled`). Grouped requests and target rows use registry IDs
  (`image.contrast`, `view.fit`, `image.debugView`,
  `image.superResolution.enabled`). `applied`, `effective`, `fieldRevisions`
  and undo `restored`/`skipped` use the former.
- Fix: map `Field` to registry ID for grouped tools. Keep legacy names for the
  19 legacy tools so the dev-camera SDK contract stays unchanged.

### F-09 — Discovery under-reports approval (Medium)

- Evidence: metadata `requiresUi` comes from the static descriptor flag
  (`parameter_registry.cpp:674`). It is false for the five SR fields and for
  `codexModel`, `codexReasoning`, `httpModel`, `instructions` and `explainPrompt`.
  The planner (`app_control_image_parameters.cpp:253`) and policy (`Configuration()`)
  require exact Configuration approval for them.
- Fix: set those flags, or publish `approval: "may_require"` from the same
  table the planners use.

### F-04 — App and bridge validators disagree on anchored patterns (Medium)

- Evidence: `control_schema.hpp:67-68` uses a PCRE search, where `$` also
  matches before a final `\n`. The bridge uses `std::regex_match`
  (`json.cpp:469`). JSON Schema (ECMA-262, non-multiline) rejects the trailing
  line break.
- Repro (validator): `"system\n"` (ttsVoice), `"#A1b2C3\n"` (annotation.color),
  `"cam-…\n"`, `"fmt-…\n"`, `"mic-…\n"` and `"preset-document\n"` pass the app
  validator and fail the bridge. Embedded providers reach the service without the
  bridge. Current consumers reject these values downstream (ttsVoice returns
  `unavailable`), but later handle lookups must not depend on that.
- Fix: compile `QRegularExpression::anchoredPattern(pattern)`. All registry
  patterns are already `^…$`, so semantics match the bridge.

### F-13 — Grouped setters cannot return durable async operations (Medium)

- Evidence: `IsAsync` (`app_control_service.cpp:28`) lists only legacy async
  tools. Grouped setters are registered as pending-capable, but
  `camera_settings_set` (selection, format, torch, acceleration test) would run
  synchronously through `Apply` without an `operationId`.
- Fix: add a plan-level asynchronous flag routed through `Execute` plus
  `OperationTracker`, bound to source epochs and saved-intent preservation.

### F-11 — V1 advertises the full grouped catalog while native support is pending (Medium)

- Evidence: `control_registry.cpp:115` builds V1 with the same 36 tools as
  dev-full. Every parameter descriptor hard-codes
  `"implementation":"target-integration-required"` (`parameter_registry.cpp:679`).
- Fix: the V1 promotion gate must check native support for every advertised
  field, or V1 must exclude unwired groups. Derive the implementation status
  from the target instead of a constant.

### F-22 — An agent mutation commits unrelated live edits (Medium-Low)

- Evidence: `AppControlTarget::Save()` (`app_control_target.cpp:360-366`) saves
  the whole current configuration. Native image sliders defer persistence, so an
  agent zoom change commits the user's unsaved slider edits immediately. The
  inventory requires live, saved and preset state to stay distinct.
- Fix: persist only agent-touched members onto the last committed snapshot, or
  follow the native save timing. At minimum, document the behavior.

### F-16 — Core contract mismatch at capture (gate, in flight)

- Evidence: `Begin()` returns empty while a context is active
  (`app_control_service.cpp:78`, header comment "empty means disabled/busy").
  `notificationListenersMayReplaceContextAfterMutationOrApproval` and
  `queuedNotificationDoesNotReachReplacedSinkOrContext`
  (`tests/agent_control_tests.cpp:394-418`, 20:55) expect replacement. A private
  build ran 29/31.
- Owner: core. Choose the semantics and update the other side. Not changed here.

### Lower-severity findings

- **F-10:** `app.state.get.sinceEvent` has no maximum (`control_registry.cpp:36`).
  `static_cast<quint64>(double)` (`app_control_service.cpp:432`) is undefined
  above 2^64. Observation `px`/`py` lack maximums. Add `maximum` (2^53−1 and the
  observation size) and clamp before the cast.
- **F-12:** `SafeText` (`assistant_parameter_policy.cpp:28`) rejects only `Cc`.
  `U+202E`, `U+200B` and `U+2066/2069` pass in single-line `httpModel`/model
  values that appear in exact approval text. Reject `Cf` in single-line fields.
  Multi-line text may keep LRM/RLM for RTL languages.
- **F-14:** `BoundAssistantPreferenceRead` (29 KiB, `:107`) cannot return a
  native value above the budget, even through `valueKind:"requested"` and a
  single field. AI Settings editors have no length cap. Such a field is
  unreadable and therefore never re-baselines. Cap the native editors at 4096
  code points or add a ranged read.
- **F-18:** the inventory and registry omit trusted UI workflows:
  `startControlConversationRequested` / `sendWithoutControlRequested`
  (`agent_control_panel.hpp:52-53` and the floating overlay),
  `approveLeaseRequested` (`:48`; lease issuance as an action), the help window
  and overlay `Dismissed`. Add them as `native-ui-only` identities.
- **F-19:** the inventory carries 25 advisory `multipleOf`/`format` keywords that
  neither validator enforces. `requiredCatalog` describes setter "expected
  revisions" and a `nextOffset`; the compiled contract uses implicit context
  baselines and `afterField`. The doc says "13 protected entries" while the
  registry publishes 17 (bundle plus four flattened fields). Align or generate.
- **F-20:** `ttsRate` is not quantized to the native slider's 0.01 steps.
- **F-21:** disabling SR while the performance override is on requires
  Configuration approval, because the coupled override reset counts as an SR
  change (`app_control_image_parameters.cpp:250-254`). Reducing GPU work should
  not need approval.
- **F-23 (addressed, unvalidated):** per-field `setChecked` previously re-ran
  `OnTextClarityControlsChanged` for each text field.
  `BeginFieldPlan`/`ApplyImageParameterBatch` now stage the widgets and run the
  handler once on the forward path. `StopAndUndo` still restores field by
  field, so restoring SR fields can reload Maxine several times.

## Recheck of the brief's known findings

| Known item | Status at 22:11 | Evidence |
|---|---|---|
| 94 native wiring in progress; unavailable guard | Confirmed; assistant/notes routed, image/view not | `app_control_target.cpp:115-119`; matrix below |
| Event sink queued with context/subscriber fences | Present in code; late receipts fenced (my slot passes); 2 core replacement regressions fail (F-16) | `Event()`; `lateApprovalReceiptStaysWithItsContext` |
| External Configuration approval only with base domain grant; idle expiry at resolve | Present; existing core slots for both pass | service `:333`-area, `ResolveApproval` |
| Exact full plan comparison at re-prepare | **Fixed 22:06** (writes, values, domain, frames, approval, recording flags) | `app_control_service.cpp:584-591`; hard slot |
| `LastCommittedSettings` snapshot | Used for `saved` in image and assistant rows; not exercised here (needs `SettingsController`) | `app_control_image_parameters.cpp` query, assistant query |
| Scene Explain scoped intent | **Fixed domain (F-02)**; Apply requires an active context with frames; ticks consume Observe budget (read only) | policy `:58`; `app_agent_control.cpp:353-357` |
| Finalization after secured receipt | `FinalizeMutationReceipt()` hook landed (`:615`, `:641`); ordering against queued delivery not validated | hook H-4 |
| 32 KiB output / 16 KiB args / 4096 code points | Code points fixed (F-05); bounded read tested; 4096 astral code points exceed the 16 KiB argument cap and fail closed before Prepare; F-14 open | `oversizedStructuredResultsFailClosed` |
| GPU held/converging provenance | Helper exists; target still reports unknown while stabilization or keystone is on, so `image_auto_adjust` is `unavailable` there | `app_control_target.cpp` `PresentedFrame` |
| ResetView/SR alias, `PresetAllowed` | Unreconciled (F-07) | `:143`, `:34-46` |
| Atomic recipe / inverse cluster; SR-before-zoom undo | Planner emits recipe first plus four component writes and Scheme; undo journals each component (H-3). Native `SetZoom` has no SR minimum, so enum-order undo (zoom before SR) ends consistent | `customRecipeIsOneAtomicValidatedCluster` |
| CameraSettings truthful async operation IDs | Not implemented (F-13) | `IsAsync` |
| Group approval fallback wording | Not reviewed (UI worker; no wording-dependent checks) | — |
| UX final Muse implementation audit | Still outstanding; no screenshots taken (desktop-free) | — |

## Exhaustive coverage matrix (all 94 adjustable fields)

Generated from `tests/data/app_control_inventory.json` and the compiled
registry; there is no sampling. Test codes refer to slots in
`tests/agent_control_coverage_tests.cpp`:

- **V** validator parity, keyword subset and finite-bound slots (all 94)
- **M** inventory↔registry reconciliation and metadata paging (all 94)
- **Q** per-field boundary/choice quantization and whole-batch rejection
  (54 non-recipe image/view fields, 191 accepted samples)
- **C** custom recipe atomicity
- **S** SuperRes couplings/approval
- **K** stabilization/keystone session dependencies
- **X** coupling-on-change, F-06 XFAIL
- **Z** explicit zoom, F-07 XFAIL
- **G** per-field read baselines
- **I** effective intent vs production normalizer
- **E** enum vs production source
- **D** discovery approval truthfulness, F-09 XFAIL
- **A** assistant policy per field
- **U** code-point bound
- **B** bounded reads, F-14 XFAIL
- **F** format characters, F-12 XFAIL
- **R** frame-egress domain
- **P** trailing-line-break patterns, F-04 XFAIL
- **O** object corpus

All 58 image/view fields are also covered by `groupedReceiptsUseTheRequestVocabulary`
(F-08 XFAIL).

"Native effect" was read from source at capture time and not executed.
`Field` enum values exist for the 58 image/view fields, the 11 assistant/notes
fields, rotation and camera.

| # | Field id | Wire contract | Registry permission / UI flag | Pure planner (actual approval) | Native effect at capture (22:11) | Tests | Status |
|---|---|---|---|---|---|---|---|
| 1 | `view.zoom.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed; legacy typed tool live | V,M,Q,Z,S | planner implemented + tested; native pending |
| 2 | `view.zoom.amount` | number 1..16 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed; legacy typed tool live | V,M,Q,Z,S | planner implemented + tested; native pending |
| 3 | `view.center.x` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed; legacy typed tool live | V,M,Q | planner implemented + tested; native pending |
| 4 | `view.center.y` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed; legacy typed tool live | V,M,Q | planner implemented + tested; native pending |
| 5 | `view.focusMarker` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 6 | `view.fit` | string enum(2) | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 7 | `view.presentationRate` | string enum(5) | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,E | planner implemented + tested; native pending |
| 8 | `view.virtualJoystick` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 9 | `view.zoomWheelAcceleration` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 10 | `image.blackWhite.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 11 | `image.blackWhite.threshold` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 12 | `image.contrast` | number 0.25..4 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed; legacy typed tool live | V,M,Q,G,D | planner implemented + tested; native pending |
| 13 | `image.brightness` | number -1..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed; legacy typed tool live | V,M,Q,G | planner implemented + tested; native pending |
| 14 | `image.autoContrast.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 15 | `image.autoContrast.strength` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 16 | `image.blur.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 17 | `image.blur.sigma` | number 0.1..5 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 18 | `image.blur.radius` | integer enum(19) | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,E | planner implemented + tested; native pending |
| 19 | `image.temporal.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 20 | `image.temporal.alpha` | number 0.05..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 21 | `image.spatial.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 22 | `image.spatial.backend` | string enum(2) | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 23 | `image.spatial.sharpness` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 24 | `image.stabilization.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,K,X | planner implemented + tested; native pending |
| 25 | `image.keystone.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,K | planner implemented + tested; native pending |
| 26 | `image.debugView` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 27 | `image.textClarity.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,X,I | planner implemented + tested; native pending |
| 28 | `image.textClarity.flatten.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 29 | `image.textClarity.flatten.strength` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 30 | `image.textClarity.adaptive.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 31 | `image.textClarity.adaptive.sauvolaStrength` | number 0.1..0.5 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 32 | `image.textClarity.adaptive.softness` | number 0..0.25 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 33 | `image.textClarity.polarity` | string enum(3) | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 34 | `image.textClarity.strokeWeight` | integer -3..3 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 35 | `image.textClarity.smartSharpen.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 36 | `image.textClarity.smartSharpen.strength` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 37 | `image.textClarity.clahe.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 38 | `image.textClarity.clahe.clipLimit` | number 1..8 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 39 | `image.textClarity.twoColor` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 40 | `image.textClarity.hysteresis.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 41 | `image.textClarity.hysteresis.strength` | number 0..0.25 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 42 | `image.textClarity.selectiveSharpen` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 43 | `image.textClarity.focusDetection.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 44 | `image.textClarity.focusDetection.threshold` | number 0.001..0.1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q | planner implemented + tested; native pending |
| 45 | `image.textClarity.glare.enabled` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 46 | `image.textClarity.glare.strength` | number 0..1 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,I | planner implemented + tested; native pending |
| 47 | `image.superResolution.enabled` | boolean | View / - | image planner (Configuration approval) | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,S,D | planner implemented + tested; native pending |
| 48 | `image.superResolution.strength` | number 0..1 | View / - | image planner (Configuration approval) | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,S,D | planner implemented + tested; native pending |
| 49 | `image.superResolution.prefer2x` | boolean | View / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,S,D | pending (no planner; no Field enum) |
| 50 | `image.superResolution.ultra1440p` | boolean | View / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,S,D | pending (no planner; no Field enum) |
| 51 | `image.superResolution.performanceOverride` | boolean | View / - | image planner (Configuration approval) | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,S,D | planner implemented + tested; native pending |
| 52 | `image.stabilization.bumpHold` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,K,I | planner implemented + tested; native pending |
| 53 | `image.keystone.paused` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,Q,K,X,I | planner implemented + tested; native pending |
| 54 | `image.colors.schemeId` | string enum(24) | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed; legacy typed tool live | V,M,Q,E,C | planner implemented + tested; native pending |
| 55 | `image.colors.custom.mode` | string enum(3) | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,C,I | planner implemented + tested; native pending |
| 56 | `image.colors.custom.stopCount` | integer 2..8 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,C,I | planner implemented + tested; native pending |
| 57 | `image.colors.custom.stops` | array 2..8 | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,C,P,I | planner implemented + tested; native pending |
| 58 | `image.colors.custom.stepped` | boolean | View / - | image planner | Prepare returns `unavailable` (target.cpp:119); batch apply written, not routed | V,M,C,I | planner implemented + tested; native pending |
| 59 | `camera.orientation` | integer enum(4) | Source / - | none | Prepare returns `unavailable` (target.cpp:119); legacy `camera.orientation.set` live | V,M | pending (no planner) |
| 60 | `camera.selection` | handle `^cam-[a-f0-9]{32}$` | Source / - | none | Prepare returns `unavailable` (target.cpp:119); legacy async `camera.select` live | V,M,P | pending (no planner) |
| 61 | `camera.format` | handle `^(automatic\|fmt-[a-f0-9]{32})$` | Source / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,P | pending (no planner; no Field enum) |
| 62 | `camera.acceleration` | string enum(3) | Source / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 63 | `camera.phoneTorch` | boolean | Source / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 64 | `recording.microphone` | handle `^(none\|mic-[a-f0-9]{32})$` | CaptureConfiguration / UI | none | Prepare returns `unavailable` (target.cpp:119) | V,M,P | pending (no planner; no Field enum) |
| 65 | `recording.canvas` | string enum(7) | CaptureConfiguration / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,E | pending (no planner; no Field enum) |
| 66 | `assistant_preferences.sceneExplain` | boolean | FrameEgress / - | assistant policy (requiresFrames) | routed (target.cpp:115-117); unexercised here | V,M,A,D,R | policy implemented + tested; native wiring landed, unvalidated |
| 67 | `assistant_preferences.overlayEnabled` | boolean | AssistantPreference / - | assistant policy | routed (target.cpp:115-117); unexercised here | V,M,A,D | policy implemented + tested; native wiring landed, unvalidated |
| 68 | `assistant_preferences.provider` | string enum(2) | ProviderConfiguration / UI | assistant policy (Configuration approval) | routed (target.cpp:115-117); unexercised here | V,M,A,D | policy implemented + tested; native wiring landed, unvalidated |
| 69 | `assistant_preferences.codexModel` | string max 256 cp | AssistantPreference / - | assistant policy (Configuration approval) | routed (target.cpp:115-117); unexercised here | V,M,A,D | policy implemented + tested; native wiring landed, unvalidated |
| 70 | `assistant_preferences.codexReasoning` | string max 256 cp | AssistantPreference / - | assistant policy (Configuration approval) | routed (target.cpp:115-117); unexercised here | V,M,A,D | policy implemented + tested; native wiring landed, unvalidated |
| 71 | `assistant_preferences.httpModel` | string max 256 cp | AssistantPreference / - | assistant policy (Configuration approval) | routed (target.cpp:115-117); unexercised here | V,M,A,D,F | policy implemented + tested; native wiring landed, unvalidated |
| 72 | `assistant_preferences.instructions` | string max 4096 cp | AssistantPreference / - | assistant policy (Configuration approval) | routed (target.cpp:115-117); unexercised here | V,M,A,D,U,B | policy implemented + tested; native wiring landed, unvalidated |
| 73 | `assistant_preferences.explainPrompt` | string max 4096 cp | AssistantPreference / - | assistant policy (Configuration approval) | routed (target.cpp:115-117); unexercised here | V,M,A,D,B | policy implemented + tested; native wiring landed, unvalidated |
| 74 | `assistant_preferences.ttsVoice` | handle `^(system\|voice-[a-f0-9]{32})$` | AssistantPreference / - | assistant policy | routed (target.cpp:115-117); unexercised here | V,M,A,D,P | policy implemented + tested; native wiring landed, unvalidated |
| 75 | `assistant_preferences.ttsRate` | number -1..1 | AssistantPreference / - | assistant policy | routed (target.cpp:115-117); unexercised here | V,M,A,D | policy implemented + tested; native wiring landed, unvalidated |
| 76 | `notes_output.lectureNotes` | boolean | AssistantPreference / - | assistant policy | routed (target.cpp:115-117); unexercised here | V,M,A,D | policy implemented + tested; native wiring landed, unvalidated |
| 77 | `notes_output.root` | handle `^dir-[a-f0-9]{32}$` | OutputDestination / UI | none | Prepare returns `unavailable` (target.cpp:119) | V,M,A,P | pending (policy refuses; trusted handle not integrated) |
| 78 | `ui.mode` | string enum(2) | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 79 | `ui.inspectorWidth` | integer 360..1200 | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 80 | `ui.language` | string enum(26) | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,E | pending (no planner; no Field enum) |
| 81 | `ui.hidden` | boolean | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 82 | `ui.sectionExpanded` | object | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,O | pending (no planner; no Field enum) |
| 83 | `ui.assistiveDock` | string enum(3) | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 84 | `ui.assistiveGeometry` | object | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,O | pending (no planner; no Field enum) |
| 85 | `annotation.color` | handle `^#[0-9a-fA-F]{6}$` | AnnotationPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M,P | pending (no planner; no Field enum) |
| 86 | `annotation.width` | integer 2..24 | AnnotationPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 87 | `annotation.textSize` | integer 12..72 | AnnotationPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 88 | `annotation.dashed` | boolean | AnnotationPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 89 | `annotation.shape` | string enum(2) | AnnotationPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 90 | `annotation.captureOnExit` | boolean | PhotoEgress / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 91 | `annotation.tool` | string enum(6) | AnnotationEdit / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 92 | `annotation.enabled` | boolean | AnnotationEdit / UI | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 93 | `ui.setupPromptDeclined` | boolean | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |
| 94 | `ui.activeTab` | string enum(3) | UiPreference / - | none | Prepare returns `unavailable` (target.cpp:119) | V,M | pending (no planner; no Field enum) |

Totals: 58 image/view planner fields, implemented and tested; 11
assistant/notes policy fields, implemented and tested, with routing landed but
not validated; 2 legacy-only camera fields; 23 fields with no planner. No field
has validated native grouped effects. Unread: the native handlers for annotation
preferences, UI layout/geometry/language/section states, camera format,
acceleration and torch, and recording canvas/microphone were inventoried but not
re-read in depth, because their planners do not exist yet.

## Protected values (17) and workflow identities (34)

The `protectedAndWorkflowIdentitiesNeverBecomeTools` slot reconciles the
inventory against the registry for every row below. It checks every edition's
tool list and every setter schema, using both validators.

| # | Protected id | Category | Secret | Wire access | Tested |
|---|---|---|---|---|---|
| 1 | `assistant.controlEnabled` | trusted_ui_master | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 2 | `assistant.externalMcpEnabled` | trusted_ui_master | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 3 | `assistant.internetEnabled` | privilege | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 4 | `assistant.codingEnabled` | privilege | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 5 | `assistant.workspace` | privilege_and_host_path | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 6 | `assistant.codexExecutable` | execution_identity | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 7 | `assistant.apiUrl` | network_destination | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 8 | `assistant.apiKey` | secret | yes | never_read_or_write | not a property/tool in any edition; refused by every setter schema (both validators) |
| 9 | `assistant.credentialId` | secret_locator | yes | never_read_or_write | not a property/tool in any edition; refused by every setter schema (both validators) |
| 10 | `assistant.attachCurrentView` | frame_grant | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 11 | `assistant.photosGrant` | capture_grant | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 12 | `assistant.recordingGrant` | capture_grant | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 13 | `assistant.externalLeaseBundle` | ui_lease_issuance | no | read_sanitized_status_only_UI_owns_mutation | not a property/tool in any edition; refused by every setter schema (both validators) |
| 14 | `assistant.externalLease.frames` | ui_lease_issuance | no | flattened lease field (metadata only) | not a property/tool in any edition; refused by every setter schema (both validators) |
| 15 | `assistant.externalLease.source` | ui_lease_issuance | no | flattened lease field (metadata only) | not a property/tool in any edition; refused by every setter schema (both validators) |
| 16 | `assistant.externalLease.photos` | ui_lease_issuance | no | flattened lease field (metadata only) | not a property/tool in any edition; refused by every setter schema (both validators) |
| 17 | `assistant.externalLease.recording` | ui_lease_issuance | no | flattened lease field (metadata only) | not a property/tool in any edition; refused by every setter schema (both validators) |

| # | Workflow id | Registry status | Permission | Inventory access | Tested |
|---|---|---|---|---|---|
| 1 | `camera.keystone.back` | native-ui-only | source_exact_approval | typed_action_required | absent from all five editions |
| 2 | `camera.keystone.next` | native-ui-only | source_exact_approval | typed_action_required | absent from all five editions |
| 3 | `camera.acceleration.test` | native-ui-only | source_exact_approval | typed_action_required | absent from all five editions |
| 4 | `mode.activate` | existing-tool-runtime-gated | view_with_per_field_sensitivity | typed_action_required | exists as DevFull tool |
| 5 | `mode.save` | native-ui-only | mode_library_exact_approval | UI_workflow_only_until_exact_name_and_revision_contract | absent from all five editions |
| 6 | `mode.reset` | native-ui-only | mode_reset_exact_approval | typed_action_with_exact_UI_approval | absent from all five editions |
| 7 | `capture.photo.take` | existing-tool-runtime-gated | photos | existing_typed_tool | exists as DevFull tool |
| 8 | `capture.recording.start` | existing-tool-runtime-gated | recording | existing_typed_tool | exists as DevFull tool |
| 9 | `capture.recording.stop` | existing-tool-runtime-gated | recording_exact_session | existing_typed_tool | exists as DevFull tool |
| 10 | `annotation.snapshot` | native-ui-only | photos | native_UI_only_no_arbitrary_file_browser_action | absent from all five editions |
| 11 | `notes.open` | native-ui-only | trusted_UI | native_UI_only_no_arbitrary_file_browser_action | absent from all five editions |
| 12 | `output.open` | native-ui-only | trusted_UI | native_UI_only_no_arbitrary_file_browser_action | absent from all five editions |
| 13 | `annotation.clear` | native-ui-only | annotation_edit_and_photos | requires_finite_scene_item_handle_and_revision_contract_no_raw_input | absent from all five editions |
| 14 | `annotation.undo` | native-ui-only | annotation_edit | requires_finite_scene_item_handle_and_revision_contract_no_raw_input | absent from all five editions |
| 15 | `annotation.redo` | native-ui-only | annotation_edit | requires_finite_scene_item_handle_and_revision_contract_no_raw_input | absent from all five editions |
| 16 | `annotation.cleanPhoto` | native-ui-only | photos | requires_finite_scene_item_handle_and_revision_contract_no_raw_input | absent from all five editions |
| 17 | `annotation.selection.edit` | native-ui-only | annotation_edit | requires_finite_scene_item_handle_and_revision_contract_no_raw_input | absent from all five editions |
| 18 | `assistant.connect` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 19 | `assistant.send` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 20 | `assistant.read` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 21 | `assistant.explain` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 22 | `assistant.readAloud` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 23 | `assistant.stop` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 24 | `assistant.new` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 25 | `assistant.history.select` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 26 | `assistant.history.rename` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 27 | `assistant.history.export` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 28 | `assistant.history.delete` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 29 | `assistant.capability.test` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 30 | `setup.download` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 31 | `assistant.ttsPreview` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 32 | `consent.approve` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 33 | `consent.revoke` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |
| 34 | `consent.stopAndUndo` | native-ui-only | explicit_UI_action | native_UI_only_not_bulk_preference_setter | absent from all five editions |

## Omitted native knobs and workflows: source sweep

Inspected: `AiSettingsDialog` (17 editors), `ColorSchemePicker` (custom mode,
stop count, stops, stepped), `AnnotationOverlay` (width/text slider 2–24 /
12–72, color grid, dashed, shape, tool), `AssistiveOverlay` (question, dock,
Read Aloud, new chat, start control / send without control, close),
`AgentControlPanel` and `AssistantChatView` signals, `SetupAssistantDialog`,
every `UIStateManager`/`MainWindow` widget member, `PersistentSettings`/`AdvancedConfig`,
and the `MediaCapture` phone API.

- **No persisted user-adjustable preference is missing** from the 94.
  Section keys reconcile 16/16 with `makeSection` IDs. Slider ranges and scales
  match the schemas: zoom 100–1600, center 0–100, contrast 25–400, brightness
  ±100, B&W 0–255, blur σ 1–50 ×0.1 with the radius table, temporal 5–100,
  softness 0–25, stroke ±3, CLAHE ×10, focus ×1000, SR 0–100, TTS rate ±100,
  annotation 2–24/12–72, stops 2–8, inspector 360–1200. Scheme and language
  enums equal `BuiltInColorSchemes()`+`custom` and `SupportedAppLanguages()`+`system`
  (`registryEnumsFollowProductionChoiceSources`).
- **Deliberately not knobs:** `MediaCapture::SetPhoneZoom` / `SetPhoneCamera`
  are API-only. No Windows UI exposes them; phone lenses are separate camera
  entries. `controlsCollapsed` is a hidden compatibility accessor. `ttsEngine`
  has no picker. The quick-mode library has no rename/delete UI, so
  `mode.delete` is not a native workflow.
- **Missing workflow identities (F-18):** start-control routing,
  send-without-control, lease approval/issuance as an action, help window,
  overlay dismiss. All five must remain `native-ui-only`.

## Practical scoped expansion proposal

This proposal is ordered by dependency and contains no schedule estimates.

1. **Close the image/view path.**
   - Apply H-1: merge `ImageParameterFields()` into `Fields()` and native-change
     detection.
   - Route `image.settings.*` and `view.settings.*` through
     `PrepareImageParameters` and `BeginFieldPlan`.
   - Fix F-03 (rollback), F-06, F-07 and F-08.
   - Journal the custom recipe as one inverse cluster (H-3).
   - Derive metadata approval flags from the planner table (F-09).
   - Gate: these slots plus a desktop-free app fixture for native-user-wins on
     each newly visible field.
2. **Validate assistant/notes routing.**
   - Exercise `PrepareAssistantParameters`/`ApplyAssistantParameter` and the
     `FinalizeMutationReceipt` ordering (H-4) in a QCoreApplication fixture with
     fixture settings.
   - Fix F-12 and F-20, and decide F-14 by capping the native editors or adding
     a ranged read.
3. **UI and annotation preferences (17 fields)** with typed setters only.
   - `annotation.enabled` and `captureOnExit=true` need exact approval or a
     photos grant (`requiresPhotos`).
   - Geometry goes through the native placement checks; language goes through
     `LanguageManager`.
   - Not included: scene editing, raw input, search text or popups.
4. **Camera and recording settings (7 fields)**, after F-13 provides the async
   operation contract, source/geometry epochs and saved-intent preservation.
   - A microphone change needs exact session approval because the native
     handler stops recordings.
   - Canvas applies to the next recording.
   - Torch reports actual availability.
5. **Workflows that may gain exact typed actions:** keystone back/next (source
   exact approval), `mode.save` with an exact name and revision, and an
   annotation snapshot under a photos grant.
   - These stay native-only: masters, grants, lease issuance, secrets,
     executable/workspace/URL, account login, downloads, conversation
     rename/export/delete, notes/folder open, history and capability test.
6. **V1 promotion criterion (F-11):** every V1-advertised field has validated
   native effects, both adapters pass, and the release/hardware gates and
   `publish_windows_release.ps1` remain unchanged. No winget or Store submission.

## Integration hooks for owners

- **Parent:** add `include(agent_control_coverage.cmake)` next to
  `include(assistant_parameter_policy.cmake)` (`tests/CMakeLists.txt:37`). The
  module needs Qt Core/Gui/Test and the Windows SDK headers pulled in by
  `cuda_interop.hpp`; it needs no CUDA runtime. Add a `tests/README.md` row for
  `agent_control_coverage_contract` and the improvement-ideas index entry for
  this file.
- **Core:**
  - H-1 (`Fields()`/`NoticeNativeChanges` for image/view);
  - F-03 rollback;
  - H-5 registry-derived domains;
  - F-08 receipt IDs;
  - F-10 bounds;
  - F-13 async grouped setters;
  - F-16 `Begin` replacement semantics against the core regressions;
  - H-4 confirmation that `FinalizeMutationReceipt` runs after the caller's
    receipt is queued;
  - H-2 diagnostics provenance (auto-adjust is unavailable whenever
    stabilization or keystone is on).
- **Image worker:**
  - F-06, F-07, F-21;
  - H-3: `ApplyImageParameter` should treat the four component writes as
    derived from `CustomColorRecipe`, so undo restores one cluster.
- **HTTP worker:** F-12, F-14, F-20.
- **Parent/inventory:**
  - F-09 descriptor flags;
  - F-11 V1 gate;
  - F-18 workflow identities;
  - F-19 schema/prose drift.

## Changed files, commands and results

Changed in the worktree (new files only, uncommitted):

- `tests/agent_control_coverage_tests.cpp`: 35 slots, desktop-free.
- `tests/agent_control_coverage.cmake`: target `agent_control_coverage_tests`,
  test `agent_control_coverage_contract`. It compiles `control_registry.cpp`,
  `parameter_registry.cpp`, `app_control_service.cpp`,
  `app_control_image_parameters.cpp` (`OKUFLOW_IMAGE_PARAMETER_PLANNER_ONLY`),
  `assistant_parameter_policy.cpp`, `app_language.cpp`, `color_schemes.cpp` and
  `src/mcp/json.cpp`. It is not registered yet; see the hook above.
- `improvement_ideas/52-opus-expanded-control-review-2026-10-14.md`: this file.

Private, outside Drive and Git:
`%LOCALAPPDATA%\OkuFlow\opus-coverage-review\`. It holds the `wrapper\CMakeLists.txt`
(which includes the tracked `.cmake` and adds private builds of three existing
contracts), `build.bat`, `build\`, and the logs `final-coverage.txt`,
`final-coverage-junit.xml` and `review_*.txt`.

```bat
:: MSVC 19.44.35229, Qt 6.12.0, Ninja, Release; separate from the parent cpu tree
cmd /c %LOCALAPPDATA%\OkuFlow\opus-coverage-review\build.bat
set PATH=C:\Qt\6.12.0\msvc2022_64\bin;%PATH%
%LOCALAPPDATA%\OkuFlow\opus-coverage-review\build\agent_control_coverage_tests.exe -o final-coverage.txt,txt -o final-coverage-junit.xml,junitxml
```

| Executable (private build, 22:11) | Result | Time |
|---|---|---|
| build (clean / incremental) | exit 0 | 23.3 s / 14.0 s |
| `agent_control_coverage_tests` | **35 passed, 0 failed**; XFAIL F-03, F-04, F-06, F-07, F-08, F-09, F-10, F-12, F-14 | 0.34 s |
| `assistant_parameter_policy_tests` (existing) | 10 passed, 0 failed | 0.07 s |
| `control_schema_tests` (existing) | 6 passed, 0 failed | 0.04 s |
| `agent_control_tests` (existing core) | **29 passed, 2 failed** (F-16) | 0.72 s |

Not run: `scripts/agent_build.bat`, CTest presets, GUI fixtures, the MCP SDK
fixture, the CUDA build, bundle/MSI tests and `publish_windows_release.ps1`.
These are synthetic and pure-planner results only. They are not native, hardware,
live-provider or release acceptance.

## Parent continuation (15 October 2026)

The Opus task ended with a Claude rate-limit error. Its findings above record
that earlier snapshot; they are not a completion claim. The coordinating 6.1
agent removed all nine expected-failure suppressions and registered the hard
coverage contract. Current native routing covers all 94 adjustable fields across
eight groups. Finite discovery reports 17 protected entries and 39 native workflow
actions; actions requiring trusted UI remain explicitly unavailable to actors.

The parent and workers corrected atomic synchronous rollback, failed-inverse
revision/undo invalidation, complete exact approval revalidation, independent
frame/photo/recording fences, pre-effect serialized result bounds, canonical
field/enum receipts, changed-only dependency normalization, native saved-state
projection, scoped scene egress and Codex isolation, UI/annotation routing, and
asynchronous camera/recording/torch operations. Oversized legacy Assistant values
outside the finite contract remain explicitly UI-only rather than being truncated.
Native settings persistence follows the application's whole-settings save
transaction; named quick-mode saving still requires its explicit workflow.

Evidence retained outside Git in LocalAppData:
- `opus-coverage-review/parent-coverage-{tests.log,junit.xml}`: hard coverage passed,
  no QEXPECT_FAIL suppressions.
- `mcp-review-fix-evidence/`: 23 C++ protocol rows and 17 official-SDK synthetic
  scenarios passed, including all 94 parameter definitions.
- `agentic-control/source-recording-torch-junit.xml`: six focused desktop-free
  contracts passed. `core-receipt-retry-rollback-junit.xml` records the subsequent
  core regression pass.
- The production CPU app linked. The shipping CUDA text-stage object compiled.

These checks do not establish native IPC, hardware, live-provider, accessibility,
installer or release acceptance. The 15 October native GUI run found regressions
and was stopped at the owner's ten-minute desktop deadline; original failures
and unfinished evidence are retained. V1 promotion and publication remain gated
until those issues and the corresponding acceptance checks are complete.
