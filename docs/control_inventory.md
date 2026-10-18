# Expanded native control inventory

The implementation inventory is [app_control_inventory.json](../tests/data/app_control_inventory.json).
It records **98 adjustable values**, **13 protected entries** (including the
four-field external lease bundle and an internal credential locator), and **46
action/workflow identities** found in the current Windows production source.
The original 19 MCP tools and their private SDK proof cover a subset. This
inventory alone does not establish native target or hardware acceptance. The
Qt-free registry layer now defines the expanded catalog and schemas; native
target integration remains separately gated.

| Group | Adjustable values | Native owner |
| --- | ---: | --- |
| Image | 53 | `app_controls.cpp`, `app_ultra_clear_text.cpp`, `ColorSchemePicker`, `UIStateManager` |
| View/navigation/presentation | 9 | `app_controls.cpp`, bootstrap, `PipelineOrchestrator` |
| Camera/source | 5 | `app_controls.cpp`, bootstrap, `MediaCapture` |
| Recording preferences | 2 | bootstrap, `RecordingManager` |
| Assistant preferences | 10 | `app_assistant.cpp`, `AiSettingsDialog`, `AssistiveFeatureManager` |
| Notes/output | 2 | AI settings, `ChangeUserDataFolder` |
| UI/locale/layout | 9 | `MainWindow`, `AssistiveOverlay`, `LanguageManager`, setup assistant |
| Annotation | 8 | `AnnotationOverlay`, bootstrap preference persistence |

Every adjustable entry has a stable `fieldId`, finite type/range or runtime
choice contract, units, native source/handler/widget, saved-member mapping,
effects, permission domain, hardware availability, existing test-file references
and required validation. `nativeDefault` means a source default, never a claim
about the owner's live configuration. Test-file references identify related
fixtures; they do not assert that every expanded field is already tested.

Image coverage includes black/white and threshold, custom color recipes and
all 23 built-in color schemes, contrast/brightness, automatic contrast,
blur sigma and snapped radius, temporal blending, FSR/NIS sharpening,
stabilization and session-only bump hold, keystone enable/pause and step
actions, debug view, every Text Clarity refinement, focus/glare controls,
Maxine strength/quality and its session-only performance override. Fine-tune
ranges come from `MainWindow` and `OnTextClarityControlsChanged`, rather than
guessed GPU parameters. Custom color mode, count, stops and stepped state must
be applied as one validated recipe. Internal derived polarity/effect/legacy
metadata is not an extra writable color knob.

Camera format and acceleration preferences, orientation, temporary source
selection and phone torch are distinct. Formats, camera sources, microphones,
voices and models require runtime enumerations and safe session handles.
Do not expose driver links, endpoint IDs, phone serials or host paths as model
authority. Unsupported CUDA, Maxine, torch, voice and format capabilities must
return explicit availability, requested/effective state and a fixed reason;
listing a preference is not proof that its effect is active.

## Catalog and shared native implementation contract

Retain `dev-core` index 0 (14 tools), `dev-capture` index 1 (17), `dev-camera`
index 2 (19), and unaccepted `v1` index 3. Implement expanded groups under a new
immutable **`dev-full` index 4**, compiled identically into the application,
bridge and private fixtures. Keep the current default until the expanded
implementation is ready. Neither inventory work nor selecting an edition
promotes V1 or adds the bridge to shipping packages.

The required typed additions are `app.controls.list` and paired `get`/`set`
methods for `image.settings`, `view.settings`, `camera.settings`,
`recording.settings`, `assistant.preferences`, `notes.preferences`,
`ui.preferences` and `annotation.preferences`. Use static per-group schemas
with finite properties and `additionalProperties:false`; never expose a raw
settings key/value object. Existing semantic tools remain compatible aliases.
Runtime list metadata can report availability and current enum choices but
cannot change the static MCP descriptor catalog.

`app.controls.list` returns at most 16 compact field descriptors per page,
with finite group/offset/limit arguments and a catalog edition/next offset.
Keep source-owner and testing prose in this tracked inventory, outside wire
results, so discovery stays within the existing 32 KiB structured-result cap.
This is tool-result pagination; the pinned MCP `tools/list` cursor policy stays
unchanged.

These descriptors are implemented in `app/agent/control_registry.cpp` and
`app/agent/parameter_registry.cpp`. `ParameterRegistry`/`FindParameter` return
the immutable 98-field table; `ProtectedControlRegistry` returns 17 metadata
entries (the protected bundle's four fields are flattened), and
`ControlActionRegistry` preserves the existing-tool/native-only classifications
and adds seven selected-video actions. `ParameterGroupForTool` maps the eight appended finite pairs.
`ParameterGetSchema` permits an optional unique filter of 1–16 field IDs;
`ParameterSetSchema` constructs nonempty closed patches with only that group's
properties. `ParameterMetadataPage(group, afterField, limit, edition)` emits
bounded pages, rejects unknown continuation IDs/groups and includes the exact
compiled catalog name. Metadata never contains runtime parameter/secret values.

The schemas bound instructions/prompts to 4096 Unicode code points, other
unconstrained identifiers to 256, custom colors to 2–8 RGB stops, source/format/
microphone/voice/directory values to finite session-handle forms, and geometry
to bounded integers. Native quantization is reported and applied by the target,
not enforced as a floating-point `multipleOf` test. The directory parameter
remains a typed `dir-<32hex>` patch whose target integration must resolve only
Documents/current/owner-approved roots, never a model path.

The registry and schema glue are original first-party code under OkuFlow's
existing dual license. They add no imported SDK, Qt dependency to the console
bridge, runtime file loader or third-party package.

### Assistant preference projections

Only `assistant.preferences.get` accepts optional `valueKind`:
`all` (default), `requested`, or `saved`. The existing unique `fields` filter
still accepts at most 16 known assistant parameter IDs. Other grouped getters
reject this argument. Requested/saved projections omit the other value keys
and retain safe availability/persistence metadata; they do not introduce a
generic text or file read API.

Assistant instruction and Explain strings can contain 4096 Unicode code points.
The native pure `BoundAssistantPreferenceRead` helper reserves a 29 KiB payload
budget for service metadata, preserving the existing 32 KiB result limit. It
does not truncate values or hide them at 1024 bytes. For `all`, equal string
values may use the typed reference `{ "sameAs": "requested" }` only when
deduplication is needed to fit the budget. Distinct current/saved full strings
may still exceed it. Such rows explicitly set `filterRequired:true` and
`unsupportedReason:"response_budget"`, with null selected value keys; request
the field again with `valueKind:"requested"` or `valueKind:"saved"` to obtain
the complete value. References do not indicate that different values are equal.

A successful full non-null requested/current value can refresh only that
field's mutation baseline and expose its revision. Saved-only reads never
refresh the baseline for an unobserved current value. Filtered, protected or
null current values also cannot acknowledge the current revision. Clients must
read the full current value before retrying a stale mutation.

Each grouped setter first constructs the complete typed `ControlPlan` and
validates all values, dependencies, revisions, source identities and permission
domains before applying a field. Unknown keys and mixed unauthorized domains
are rejected without partial effects. Native and agent changes must call the
same app-owned setters: update the corresponding widget with signals blocked,
invoke the existing handler exactly once, collect coupled field changes and
return actual normalized/effective values. Do not replay clicks or emit UI
signals from an IPC worker. `AppControlService` remains the GUI-thread arbiter;
both embedded control and MCP use that service and the same registry.

Extend the field revision/journal to every supported preference, including
coupled changes. Native edits win over stale agent plans, and undo skips fields
the user changed. Capture, account/network requests, committed files and
device actions cannot be represented as reversible preference undo. Async
operations require truthful retained operation states and exact owner/session
identity. Preserve the built-in master default on, external master default off,
explicit saved choices, immutable UI-issued lease bundles,
monotonic 300-second absolute/60-second idle limits and existing budgets.

## Native behavior that must survive expansion

- Image handlers update live configuration and preset matching through
  `SyncCurrentConfigToPersistence`; a named quick mode is created only by the
  explicit Save workflow. The shared agent target currently saves accepted
  mutations, while many native image sliders defer disk persistence to a save
  or shutdown. Keep live, saved configuration and named preset state distinct.
- Rotation transforms pan and annotations, resets CPU/CUDA temporal history,
  stabilization, keystone and Text Clarity, and clears scene/presentation
  buffers. It remains a source-sensitive operation excluded during recording.
- Temporal enable/alpha reset history. Stabilization disable clears bump hold;
  stabilization/keystone changes reset their dependent histories. Bump hold and
  the SuperRes performance override are session-only, with no saved member.
- SuperRes quality modes exclude one another. Non-Ultra enable also enables
  zoom and sets its minimum to 1.33 or 2; zero strength is restored to 0.65.
  Disabling SuperRes clears its performance override. Return all these coupled
  deltas and retain the 24 ms performance policy unless explicitly overridden.
- Manual source/format handlers save startup intent and reopen the source.
  Temporary agent source/format changes must preserve saved intent, advance
  source/geometry epochs, clear frame permission and cancel stale observations.
  Production `camera.select` integration was newly written during this audit;
  compilation and runtime/device acceptance remain pending. A synthetic fixture
  selection does not validate that integration.
- The native microphone handler stops the current recording on change.
  Reusing it without exact session/ownership approval would stop a manual or
  foreign-owned session. Guard before applying. Recording canvas is saved
  immediately and snapshotted at recording start; it changes the next processed
  MP4 canvas, while the original MP4 keeps camera resolution.
- The native Scene Explain checkbox still controls its own periodic analysis
  preference. The MCP parameter uses `SetAgentSceneEnabled` and is context-only:
  every agent-created scene request obeys the live frame grant and Observe
  budgets. Source changes, revoke and expiry stop that agent intent while
  preserving unrelated native periodic activity. The inventory retains the
  original native checkbox/setting mapping as `nativeUiOrigin`.
- Leaving Draw may capture when `captureOnExit` is enabled. Clear snapshots to
  notes before deleting the scene. These are photo-sensitive effects, even
  though annotation styling is an ordinary preference. Scene editing needs
  finite item handles and scene revisions rather than raw keyboard/mouse input.
- Locale changes retranslate immediately; an in-progress assistant request
  retains its original language. Overlay docking, fit, orientation and layout
  changes invalidate pending image geometry. Floating geometry is relative to
  the render target and must obey native placement checks.

## Protected values and workflows

The inventory records account login, provider request buttons, history
rename/export/delete, setup/download/install, Read/Explain, speech preview,
capture and annotation actions separately from preference fields. Broad knob
coverage does not turn these into a shell, browser, arbitrary file API or
unrestricted UI clicker. Existing finite capture actions retain their ownership
and approval contracts; other workflows need separate exact action contracts.

API keys and credential locators are never readable/writable MCP values.
Masters, attachment/frame permission, capture grants and lease issuance remain
trusted UI-owned actions. Internet/coding privileges, executable/workspace paths
and secret-bearing provider destinations cannot be widened or redirected by a
bulk setter. Ordinary model/reasoning/instruction/TTS/notes preferences can use
typed setters with finite wire bounds, cancellation/configuration epochs and
explicit handling of provider identity changes. Output-folder choice uses a
trusted UI-issued directory handle and native validation, never a model path.

The `nonControls` array explicitly records migration fields, diagnostic state,
derived recipe metadata and retained internal settings without a native editor.
OS standard window controls, search text and popup navigation are not image
engine knobs. Android, the website and the browser notes editor are separate
surfaces and are not silently included in this Windows inventory.

## Validation and current limits

The source audit reconciled all user-adjustable `AdvancedConfig` members against
inventory mappings, excluding its id/name/description metadata and derived
legacy display-color alias. JSON parsing, unique top-level identities and every
source/test-file reference were checked without opening a native window, device,
account or provider. This is structural inventory validation only.

Implementation must add tests for every field's schema bounds/enums,
native-versus-agent normalized result, current/saved/preset round-trip,
availability denial, coupled history resets, stale revisions, user-wins/undo,
permission denial and queued revoke/cancel. Both adapters must exercise the
expanded groups; the existing private MCP SDK scenarios remain subset evidence.
Runtime source switching, real device/media quality, CUDA/Maxine effects,
native approval UI, accessibility and complete release gates remain outstanding.

`mcp_bridge_contract` now validates the 98 compiled parameter mappings,
immutable old catalog sizes, all grouped closed schemas, protected fields,
bounded metadata traversal and Protocol admission. The explicit `dev-full`
private app implements metadata, four synthetic view reads and one synthetic
assistant projection field through the production pure packing helper; new grouped
setters return `unavailable` and leave its write count unchanged. That fixture
proves honest discovery/admission through stdio/IPC/service, not native effects
for all 98 controls. The default `dev-camera` SDK contract continues to advertise
19 tools.

The reviewed shared schemas now bound `sinceEvent` to the exact JSON integer
range 0–9007199254740991 and observation `px`/`py` to 0–1024. Actual observation
identity, freshness and image dimensions still require target validation;
the wire bound alone does not authorize an observation-coordinate operation.
