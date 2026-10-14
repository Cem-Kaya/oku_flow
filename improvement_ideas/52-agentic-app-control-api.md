# Plan 52 — Agentic app control and MCP API (OkuFlow Control API)

Status: **PLAN — DRAFT 4, reconciled after Draft 3 review; not implemented.**
Written 2026-10-14 against commit `34f0cb9`. Draft 3 received two fresh
independent read-only reviews (GPT-6-Astra: A3-01…A3-08; Muse Wonder Full:
D3-B01…B04, D3-M01…M13, D3-m01…m06), both *revise before implementation*.
Draft 4 reconciles all 31 findings; the per-finding disposition table is in
[§21](#draft-3-review-disposition-draft-4). No application,
build-script or website change is part of this document. Review records and
defaults are in [§21](#21-review-record) and
[§22](#22-implementation-defaults-and-unverified-facts). No implementation is
authorized by a model-generated permission; development authorization comes
from the owner, and runtime grants in the finished app (§14) are a separate
user-facing mechanism, not development approval.

## 1. Goal

Let the language model behind the Explain / Assistant panel operate the real
Windows app on the user's behalf: zoom and pan, switch quick modes, choose
color, grayscale or high-contrast reading colors, enable the app's automatic
image adjustments, change the camera source and orientation, take a photo,
start or stop a recording, and look at the result of what it changed. A
legally blind user can say *"make the board readable"* and the assistant does
the steps a sighted user would do with the mouse, then tells the user what it
changed and how to undo it.

Principles, in priority order:

1. **The app owns every effect.** The model requests typed operations; app
   code validates, authorizes and applies them through the same production
   methods the widgets use, and reports the actual result. The model never
   receives a path, a shell, a file handle, a credential, a raw device
   identifier or a raw error string.
2. **Useful autonomy for reversible view changes; scoped grants for capture,
   source changes and extra frames.** Grants come only from user actions in
   OkuFlow's own UI. Text on a board, slide or page — or anything the model
   says — can never create a grant.
3. **Provider-neutral core and a real MCP interface.** One internal control
   registry serves the Assistant provider adapters and a standards-based MCP
   stdio server. Codex dynamic tools and HTTP tool calls remain separate
   adapters; they are not mislabeled as MCP.
4. **Accessibility is the acceptance criterion.** Every agent action is
   perceivable by a screen-reader user, never steals focus, never starts
   speech on its own, and can be stopped and undone from the keyboard even
   with the UI hidden.

## 2. Non-goals

- No mouse/keyboard simulation, UI Automation invocation, `QTest` input,
  `SendInput`, synthetic `QMouseEvent`/`QKeyEvent`, or
  `QAbstractButton::click()` as the control mechanism.
- No code execution, filesystem access, local image viewing, URL opening,
  shell, web search or network fetch reachable from an agent turn. No tool
  takes a path, URL, command or free-form setting key.
- No control over AI provider, credentials, Codex permissions (internet /
  coding / workspace), user-data root, setup downloads, `Test this camera`,
  language, Reset Mode, Save as Quick Mode, conversation delete/export, debug
  view, SuperRes, periodic Scene Explain, or app exit.
- No background or periodic autonomous operation by the embedded Assistant.
  Its actions belong to a user-initiated turn. External MCP clients instead
  need an explicit short-lived app-owned control lease (§5.3); their claims
  about user intent do not constitute proof of a user-initiated turn.
- No strict-JSON "action block" fallback for endpoints without tool calling.
- No remote/network MCP listener in this version. Local stdio MCP is in scope,
  including its Windows IPC authorization and conformance tests (§5.3).
- Phone-camera remote zoom: `MediaCapture::SetPhoneZoom` / `SetPhoneCamera`
  exist but nothing in `src/app` calls them; plan 51's zoom bridge comes
  first. Phone lens *facing* metadata is not modeled in `CameraDescriptor`
  today, so this plan does not promise front/back selection.

## 3. Evidence from the current code

Read from source or generated locally on 2026-10-14. Both reviewers verified
the facts below except where marked as corrected. Re-locate symbols if files
move.

### 3.1 Control state and production paths

| Area | Current production path | Consequence |
|---|---|---|
| Profile-owned image state | `OkuFlowApp` members (`zoomAmount_`, `contrast_`, …) changed by `On*Changed` slots connected to widget signals in `app_bootstrap.cpp`; `UIStateManager::ReadConfigFromUI()` reads members. | Agent changes must go through the same transition methods as widgets. |
| Whole-config apply | `UIStateManager::ApplyConfigToUI()` replays every field, including `OnStabilizationToggled` → `ResetStabilization/ResetKeystone/ResetTextClarityHistory`, temporal resets, keystone resets, `OnTextClarityControlsChanged`, assistive modes, debug view and SuperRes checkboxes. | Never implement one adjustment as read-config → mutate → apply. |
| Transition-sensitive slots | `OnRotationSelectionChanged` compares the new index with the current member; `OnTextClarityControlsChanged` snapshots previous SuperRes/zoom state; re-sending the same stabilization/keystone value still resets history. Paired widgets (Simple and Advanced Text Clarity checkboxes) and slider readouts are synchronized by connected signals that signal-blocked updates bypass. | A generic "set member, set widget blocked, call slot" recipe is unsafe (Astra R07). Field-specific transitions are needed (§6.3). |
| Persistence | `SyncCurrentConfigToPersistence()` updates the in-memory current config only; `SavePersistentSettings()` writes JSON plus backup; some saves are safety markers (camera acceleration attempt) that must stay synchronous. | Deferred disk saves need their own failure event; markers are not coalesced. |
| Quick modes | `MainWindow::ActivatePresetRow(row)` → `quickModeActivated(row)` → `OnPresetSelectionChanged`; stable ids `preset-reading` … `preset-document` plus custom ids (`kPresetIdRole`). `ActivatePresetRow` also closes the mode grid (which can activate a window and move focus) and announces only in Simple mode. Applying a preset restores the **entire** `AdvancedConfig`, including `vlmAssistEnabled`, `debugView` and SuperRes fields. Built-in `preset-scene-explain` sets `vlmAssistEnabled = true`; with the OpenAI-compatible provider, `AssistiveRuntime::WantsAnalysis()` then submits a camera frame roughly every 1.6 s. | Separate preset application from grid navigation; validate the full effect of a preset before applying it (§12.3). |
| Zoom | `InteractionController::ApplyZoom(target, localPos)` takes **widget** coordinates via `MapViewToSource`, clamps to 1.00–16.00, auto-enables zoom, writes widgets directly, and returns early when the snapped value is unchanged (so an anchor-only request would not recenter). `HandleKeyboardZoom` uses geometric 1.10 steps. Fit framing shows 1×; SuperRes raises the minimum. | New scene-space anchor math is new code with tests (§6.2). |
| Pan | `SetZoomCenter(normX, normY, syncUi, preservePreset, persist)` in normalized post-rotation scene coordinates; panning keeps the active quick mode. | — |
| Rotation | Clockwise quarter turns, global/device-owned; rotates zoom center and ink; resets histories; clears CPU scene buffers. No recording guard. | — |
| Display colors | 23 built-in scheme ids (`normal`, `inverted`, `white-black`, `black-white`, `grayscale`, `yellow-black`, … `posterize-6`) plus `custom`. `normal` disables only the final display transform: hard Black & White, adaptive binarization and Text Clarity stay active upstream, and hard Black & White is skipped while Text Clarity / adaptive binarization is active (`cuda_interop.cpp`). | Intent recipes with full deltas and shadowed-control reporting (§12.4). |
| Photo | `capturePhotoButton` lambda sets `photoCapturePending_`; that flag clears when the readback is **submitted**, before encoding and the paired commit finish. | Durable capture operations (§9.3). |
| Recording | `recordButton` toggled lambda starts the microphone (which first stops any existing capture) **before** `RecordingManager::SetRequested`; a successful stop reaches `Completed`, not `Idle`; 8 s stop watchdog; `IsWorkerAbandoned()`. Camera format changes during recording are supported through `_partN` segments. | Richer recording contract (§12.5). |
| Camera | `OnCameraSelectionChanged(index)` returns silently while `cameraStartupPending_`, persists `cameraSelectionExplicit` and the symbolic link, clears automatic phone selection for webcams, then calls `StartCameraCapture` **synchronously** for physical cameras; interactive start failures open a modal `QMessageBox::warning`. Phone entries are named `OkuFlow Camera · <model>` and fall back to the **serial** when the model is empty (`phone_camera.cpp`). | Asynchronous non-modal camera transition is a prerequisite; provider-safe labels (§7, §13). |
| Present-time readback | The viewport present can request a readback of the rendered **back buffer** — exactly the viewport the user sees, including SuperRes/spatial caches, letterboxing and the focus marker. One request id is shared by every consumer that wanted a frame on that present (photo, annotation, on-demand, assistant), and `DrainCompletedGpuReadbacks` dispatches the buffer to all of them. Four-slot ring. | Viewport observations use this path with their own consumer slot and view-version stamp (§11). |

### 3.2 Assistant transport and providers

- `CodexAppServerClient::Start()` launches `codex app-server --listen
  stdio://` with the full system environment, **no** MCP preflight, no
  feature disables, and an `initialize` that sends only `clientInfo` (no
  `experimentalApi`). One client and one child serve Read, Explain and the
  Assistant; `Configure()` restarts the child only when the executable path
  changes.
- `RejectForbiddenAgentItem()` reacts to `item/started` (after the item has
  started — plan 23 calls this a race, not prevention) and interrupts MCP,
  dynamic, collaboration and non-permitted coding/web items.
  `HandleServerRequest()` answers approvals/elicitation; anything else gets
  the transport's `-32601` denial.
- `CodexJsonRpcProcess`: 2 MiB per inbound JSONL line (exceeding it kills the
  child), 4 MiB buffer, no outbound size check, 60 s request deadline;
  deferred `SendResult(id, …)` is supported. Turn watchdog: 90 s idle (touched
  only by `item/` and `turn/` notifications), 180 s vision cap, 30 min
  persistent cap, 5 s interrupt grace. The history loader requests
  `includeTurns: true`.
- **Locally generated Codex 0.160.1 schema** (`codex app-server
  generate-json-schema` into private `%TEMP%` directories; not committed):
  - **Corrected after review:** `ThreadStartParams.dynamicTools` exists only in
    the `--experimental` bundle; the stable bundle has the `DynamicToolSpec`
    *definition* but no `dynamicTools` property. `InitializeParams` documents
    `capabilities.experimentalApi` (default false). Plan 36 proved that another
    experimental surface fails with `-32600` without it. Draft 1's "present in
    the stable schema" claim came from a case-insensitive text match of
    `DynamicToolSpec` and was wrong.
  - Both bundles define server request `item/tool/call`
    (`DynamicToolCallParams {threadId, turnId, callId, namespace?, tool,
    arguments}`) and reply `DynamicToolCallResponse {success, contentItems:
    [inputText | inputImage{imageUrl} | inputAudio]}`, and the
    `dynamicToolCall` thread item (`status inProgress|completed|failed`,
    `contentItems`, `durationMs`).
  - `dynamicTools` is not accepted by `thread/resume`, `thread/fork` or
    `turn/start`.
  - Thread item types: `agentMessage`, `reasoning`, `plan`, `userMessage`,
    `contextCompaction`, `dynamicToolCall`, `mcpToolCall`,
    `collabAgentToolCall`, `subAgentActivity`, `commandExecution`,
    `fileChange`, `webSearch`, `imageView`, `imageGeneration`,
    `functionCallOutput`, `hookPrompt`, `sleep`, `enteredReviewMode`,
    `exitedReviewMode`. `imageView` is a local-image read path the current
    deny-list does not cover.
  - `turn/completed` may carry multiple items, including image-bearing
    dynamic-tool results; resume/read responses can carry turns and items.
  - `codex features list` (read-only) reports `apps`, `plugins`,
    `image_generation`, `multi_agent`, `shell_tool`, `tool_suggest` and
    `code_mode_host` enabled by default on this build.
- Plan 36 (historical, 0.146/0.147): `thread/start` launched user-configured
  MCP servers unless each was disabled by a **bare-key**
  `mcp_servers.<name>.enabled=false` override after a bounded
  `codex mcp list --json` preflight; quoted keys killed the child; a server
  name that is not a bare TOML key had to fail the session closed; built-in
  providers needed `--disable apps --disable plugins`.
- OpenAI-compatible provider: one non-streaming `chat/completions` request
  for Read/Explain (`max_tokens` 4096/180, image as data URI, no `tools`);
  persistent Assistant conversations reject this provider. Error handling
  appends a response-body excerpt after `SanitizeText`, which only
  normalizes whitespace.

## 4. Architecture

```
 Assistant panel / Advanced Assistant  (user turn; grants are UI actions)
            │
            ▼
 ┌──────────────────────── provider adapters ──────────────────────────┐
 │ CodexDynamicToolAdapter          OpenAiToolLoop / LegacyFunctionLoop│
 │ (restricted child, item/tool/call) (tools / tool_calls)             │
 │ McpStdioBridge → authenticated Windows IPC → AppMcpAdapter         │
 └───────────────┬─────────────────────────────────────────────────────┘
                 │ ControlRequest {tool, args, providerCallKey, context}
                 ▼
 ControlArbiter ─► ControlContext ─► ConsentGate (tiers, grants, approvals, budgets)
                 │
                 ▼
 AppControlService (GUI thread, serialized)
   ├─ ControlRegistry (stable ids, JSON Schema, tier, availability)
   ├─ Field transitions → named production methods (OkuFlowApp / MainWindow)
   ├─ OperationTracker (durable photo/recording/camera operations)
   ├─ ObservationService (viewport readback slot, JPEG budget, metrics)
   ├─ StateModel (field revisions, view/processing/source epochs, event ring)
   ├─ AgentChangeJournal (conflict-aware undo) + Announcer (batched)
   └─ ProviderSafeDto (allowlisted fields, generated labels, error codes)
```

New code lives under `src/app/agent/` with headers in
`include/okuflow/app/agent/`. Adapter pieces stay in
`codex_app_server_client.*` and `assistive_runtime.*`. The small console
bridge lives under `src/mcp/` and `include/okuflow/mcp/`; it does not create
Qt windows or start a camera. New dependencies need dual-license review.

| Component | Responsibility | Threading |
|---|---|---|
| `ControlRegistry` | Canonical id (`view.zoom.set`), wire name (`view_zoom_set`), English description, JSON Schema (draft-07 subset, `additionalProperties:false`, discriminated unions for coordinates), result shape, tier, idempotency class, availability predicate, catalog edition (§5.3). Emits MCP-shaped descriptors with per-tool annotations (§12.7); `openWorldHint:false` for every tool. One authoritative tier per tool. Linked by both the app and the Qt-free MCP bridge, so `tools/list` works with the app absent (§5.3). | Immutable, Qt-free, unit-testable. |
| `ControlArbiter` | Owns the single mutation owner and the existing assistant-turn busy state (§4.1). | GUI thread. |
| `ControlContext` | One record, two variants (§4.1): embedded turn or external lease. Common fields: context id, grant snapshot reference, budgets, cancellation token, field-revision baseline, undo-journal id, event cursor. External leases never fabricate thread/turn ids. | GUI thread. |
| `ApprovalService` | App-owned exact, single-use approval records and operation receipts (§14.2). Part of the Phase 1A core, before any live adapter. | GUI thread. |
| `AppControlService` | Schema validation, authorization (re-checked immediately before execution and before any frame is transmitted), dispatch, idempotency reservations, rate limits, event ring. | GUI thread; one mutating call at a time. |
| `IAppControlTarget` | Narrow interface implemented by `OkuFlowApp` (friend access like `UIStateManager`); enables headless tests with a fake target. | GUI thread. |
| `OperationTracker` | Durable ids and explicit states for photo (`queued → readback → writing → committed/failed`), recording (`starting → recording → stopping → completed/failed`) and camera transitions (`requested → stopping → starting → first_frame/failed`). | GUI thread; workers post completions. |
| `ObservationService` | Viewport observation through its own present-readback consumer slot, downscale/JPEG on the assistive pool, size budget, pixel-free metrics. | Request on GUI thread; encode on worker. |
| `ProviderSafeDto` | Builds every result from allowlisted fields; maps internal failures to error codes and fixed, translated-at-UI messages; never forwards controller, driver, settings or provider text. | GUI thread. |

### 4.1 Control contexts and arbitration (D3-M05)

`ControlContext` is a variant, never a turn record with fake fields:

- **EmbeddedTurn**: provider, child epoch, thread id, turn generation,
  user-initiated = true (a real panel submission created it, §14.5).
- **ExternalLease**: lease id, bridge/pipe epochs (§5.3), grant bundle
  (§14.3); user-initiated is *unproven* and never claimed.

Arbitration rules, all enforced app-side:

| Situation | Rule |
|---|---|
| Mutation ownership | At most one context mutates at a time. A lease cannot be issued while an embedded agent turn is active, and an embedded agent turn cannot start while a lease is active; the requester gets `busy`. Native user edits always win (§8.4). |
| Read / Explain during a lease | Allowed. They are tool-free and mutate nothing; they keep using today's single assistant busy state, which the lease does not occupy. They cannot issue control calls. |
| Read / Explain during an embedded agent turn | `busy`, exactly as today's single busy state already behaves. |
| Per-call serialization | One mutating call executes at a time on the GUI thread; Tier 0 reads from the owning context may be served between mutations. Tier 0 reads from a non-owning context return `busy` (except the operation-status carve-out, §5.3). |
| Recording ownership | Separate from turn lifetime: an *agent control session* (created when the embedded master switch is enabled, retired when it is disabled, the provider/config changes, or the app exits) or the lease owns recordings it started (§9.1, §12.5). |

## 5. Provider compatibility

### 5.1 Codex app-server

**Status: experimental dependency.** Agent control on Codex depends on the
experimental `dynamicTools` field in 0.160.1. Phase 0 must prove acceptance;
nothing below assumes it.

1. **Process ownership.** A dedicated restricted child serves agent
   conversations. Today's Read/Explain/ordinary Assistant child and history
   remain intact; coding permission changes never widen the agent child.
   The restricted child is hardened from its initial launch, never
   retro-hardened. If isolation cannot be established, agent control is
   unavailable while existing Read/Explain keep working. The `ControlArbiter`
   serializes turns across both children. This selects Astra's isolation
   boundary over sharing a hardened child with existing non-coding turns;
   avoiding a reading/sign-in/history regression outweighs a second process.
2. **Restricted launch profile, versioned.** `initialize` sends
   `capabilities.experimentalApi: true` only on this child. Launch flags come
   from a per-Codex-version pinned profile (committed with the schema fixture)
   cross-checked against `codex features list`: disable `apps`, `plugins`,
   `image_generation`, `multi_agent`, `tool_suggest`, `shell_tool` and
   `code_mode_host`, plus any feature in the pinned profile that grants
   execution, file or image reading, or tool providers. Unknown Codex version,
   a profile feature missing from the list, or list-parse failure ⇒ agent
   control unavailable. MCP: plan-36 bounded `codex mcp list --json`
   preflight with bare-key overrides only; a non-bare server name fails the
   restricted child closed. The guarantee is stated honestly as
   *best-effort launch suppression with fail-closed detection*: an
   `mcpServer/*` lifecycle notification means a server already started, so it
   disables agent control for the session and is recorded in §18.
3. **Compatibility detection.** `initialize` is not assumed to advertise
   dynamic tools. Availability requires: pinned profile matched, `thread/start`
   with `dynamicTools` accepted, and (Phase 0 probe; later a cached
   per-version verdict) a successful `okuflow_ping` round trip. Otherwise the
   ordinary Assistant continues without tools.
4. **Conversation kinds.** `settings::CodexConversation` gains an explicit
   `kind` (`assistant`, `explain`, `agent`, `coding`; legacy entries infer
   `assistant`/`explain` from the existing `explain` flag; an unknown `kind`
   value is treated as tool-free `assistant`, never `agent`) and
   `toolsetVersion`. Allowed transitions: none — a conversation keeps its
   kind; panel routing creates a new conversation instead of converting one
   (§14.5). The history index is filtered by kind: the ordinary child's
   `includeTurns` loader is never given `agent` thread ids, and the agent
   history path never loads other kinds. **Agent conversations are not
   resumed after an app restart in v1** (D3-M11): after restart they are
   listed with their saved title/preview, marked as a previous control
   conversation, and opening one offers *Start new control conversation*;
   within a session the in-memory transcript is shown. Resuming an `agent`
   thread within a session requires the same toolset version **and** Phase 0
   proof that resume restores dynamic tools; otherwise the app starts a new
   agent thread and says so. If Phase 0 proves a paginated
   `thread/items/list`, restart-resume can be added in a later catalog
   edition; v1 does not depend on it. Read and Explain stay tool-free.
5. **Declaration.** `dynamicTools: [{type:"namespace", name:"okuflow", …}]`;
   fallback to flat `okuflow_`-prefixed functions if the probe shows namespace
   problems. Whether `deferLoading` helps a small catalog is a Phase 0
   measurement, not an assumption.
6. **Authorization happens only at `item/tool/call`.** The handler claims the
   request (returns true) and replies later exactly once, unless the child has
   exited. Checks: child epoch, `threadId` equals the active agent thread,
   turn binding (next paragraph), tool exists in the declared catalog edition,
   arguments validate, tier and grants allow it.

   **Pending-start state machine (D3-M11).** Per agent thread there is at
   most one outstanding `turn/start`, recorded as
   `{generation, state: startSent|bound, turnId?}`. A call arriving before
   the `turn/start` reply is accepted only in state `startSent` with no
   recorded turn id yet: the **first** recorded turn id wins and binds the
   generation. A call carrying a different turn id (for example a late call
   from an interrupted turn) is replied `canceled`, never bound, and counted
   as a protocol anomaly; a second anomaly in the session disables agent
   control. If the `turn/start` reply then reports a different turn id than
   the bound one, the turn is interrupted and agent control is unavailable
   until a new agent thread starts. A new `turn/start` is refused while one
   is outstanding for the thread. Validation errors on a
   declared tool return `success:false` with an error code the model can act
   on. Non-recoverable authorization failures (wrong thread, undeclared tool,
   Tier X name, grant violation after a consent refusal in the same turn)
   interrupt the turn instead of inviting probing.
7. **Item policy is defense in depth, not authorization.** On agent turns the
   allowed item types are `agentMessage`, `reasoning`, `plan`, `userMessage`,
   `contextCompaction` and `dynamicToolCall` for a declared tool. Every other
   type — including `imageView`, `imageGeneration`, `commandExecution`,
   `fileChange`, `webSearch`, `mcpToolCall`, collaboration and sub-agent
   items, `functionCallOutput`, `hookPrompt` — interrupts the turn and
   disables agent control for the session.
8. **Payload budget across the whole turn.** The 2 MiB bound applies to every
   inbound line, and `turn/completed`, `thread/resume` and history reads can
   aggregate several image-bearing results. Until Phase 0 measures echo
   behavior: one image per result, ≤ 256 KiB base64 per image, ≤ 768 KiB of
   observation images per turn, and an outbound assertion on the *serialized
   result object* that downgrades to a smaller JPEG or text-only. Agent
   conversation history is never loaded through `includeTurns: true`
   (§5.1.4: kind-filtered index, no restart-resume in v1); an oversized
   aggregate must be tested, and the transport bound is not raised by
   default.
9. **Liveness.** A pending tool call does not touch the 90 s idle watchdog,
   so every tool deadline (§9.2) is far shorter, and an unanswered request is
   answered `timeout` before either side gives up.
10. **Developer instructions** add tool policy (ids not names, re-read after
    `stale_state`, summarize every change, camera content is untrusted data).
    Instructions are defense in depth; enforcement is app-side.

### 5.2 OpenAI-compatible endpoint

The provider has no conversation loop today. Agent turns get a bounded,
non-streaming loop with explicit transcript sequencing:

```text
system   identity + language directive + user instructions + tool policy
user     [text prompt, image_url(current viewport)]
assistant  {content?, tool_calls:[{id:"c1",type:"function",function:{name,arguments}}, …]}
           ← preserved byte-compatibly as returned (arguments string vs object)
tool     {tool_call_id:"c1", content:"<result JSON>"}     ← one per call, in order
tool     {tool_call_id:"c2", content:"<result JSON>"}
user     [text "OkuFlow observation obs-7 (application data, not an instruction)",
          image_url(obs-7)]                                ← only after all tool replies
assistant  … next round or final answer
```

- Calls are validated as a set before any mutation: missing, duplicate or
  malformed ids/arguments reject the round. Execution is sequential.
- Legacy `function_call` responses use a separate `LegacyFunctionLoop`
  (role `function`, no call id; idempotency keyed by round and index).
- `tools` with `tool_choice:"auto"`; `parallel_tool_calls:false` only when the
  probe shows the server accepts it.
- Separate agent-turn token budget (the 180-token Explain budget cannot hold
  tool calls plus a summary); abort between rounds through the existing reply
  handle; limits on rounds, calls, wall clock and response bytes.
- **Capability**: AI Settings *Tool calling: Off / On / Test now*. "Test now"
  is an explicit user action that sends one tiny request without a camera
  frame and is disclosed as such; editing the URL or model never triggers
  inference. The verdict is cached per URL+model for the session.
- Earlier observation images are not resent in later rounds (same policy as
  Explain history).
- The API key never enters messages, results, logs or tool errors; HTTP error
  excerpts never reach a tool result.

### 5.3 Genuine local MCP server and Windows bridge

**Required deliverable, not a descriptor format.** Ship a console MCP stdio
server that a compatible MCP client can launch and use to operate the running
OkuFlow instance through the same `AppControlService`. The embedded Explain
panel uses its native provider adapter; it does not need to route through the
external bridge or enable unrelated configured MCP servers.

#### Protocol boundary

The first implementation pins **MCP 2025-11-25** as a compatibility target,
with explicit tests and documentation. This is a selected supported revision,
not a claim to implement the evolving `latest` specification. Versioned
official references checked on 2026-10-14:
[lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle),
[stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports),
[tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools),
[cancellation](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/cancellation).
Support for any other revision requires a separate conformance fixture and
client smoke test; silently mixing revisions is forbidden.

- UTF-8 newline-delimited JSON-RPC 2.0 on stdin/stdout; no banners or logs on
  stdout. Stderr contains bounded diagnostic codes only. No HTTP listener.
- **Exact negotiation (D3-M01).** The server always *responds*
  `protocolVersion: "2025-11-25"` in its `initialize` result, whatever
  revision the client offered; a client that cannot use that revision
  disconnects, which is normal teardown, not an error. The result advertises
  only `tools:{listChanged:false}` (static catalog, D3-M03) and no resources,
  sampling, prompts, elicitation or task capabilities. `ping` is legal as
  soon as an `initialize` request has been received — including before
  `notifications/initialized` — and returns `{}`. `tools/list` and
  `tools/call` before `notifications/initialized` return the pinned
  not-initialized error below; a second `initialize` returns invalid-request.
- **Pinned JSON-RPC error codes (D3-m03).** Parse error `-32700`; invalid
  request `-32600` (including duplicate `initialize`, batch arrays, null or
  duplicate in-flight request ids); method not found `-32601`; invalid
  params `-32602` (including unknown tool, any `tools/list` cursor,
  task-augmented calls); internal error `-32603`; not initialized `-32002`.
  Error `data` carries only a fixed sanitized code string, never internal
  text. Notifications never receive responses.
- **Envelope edge cases (D3-M02), one fixture each:**

  | Case | Behavior |
  |---|---|
  | `tools/list` pagination | Single page, no `nextCursor`. Any non-empty `cursor` ⇒ `-32602`. |
  | `params._meta` (incl. `progressToken`) | Accepted and stripped before argument validation; v1 sends no progress notifications. Never a validation failure. |
  | Task-augmented `tools/call` (`params.task`) | `-32602`; tasks are not advertised. Pending work uses operation ids + `app.operation.get` (§8.5). |
  | Batch (JSON array) | One `-32600` error object (not an array); batches are not part of the pinned revision. |
  | Request id null / wrong type | `-32600` with `id: null`. |
  | Request id reused while in flight | Second request gets `-32600`; reuse after completion is allowed. |
  | Oversized line (> §9.4 cap) | Line discarded, `-32700` with `id: null`; three oversized/malformed lines end the bridge process. |

- `tools/list` is served by the bridge itself from the linked Qt-free
  `ControlRegistry` (compiled-in catalog edition), so it works with the app
  absent; it lists exactly the active catalog edition with explicit draft-07
  `$schema`, input/output schemas and truthful annotations (§12.7).
  Authorization is enforced at `tools/call`, never conferred by listing. If
  the app's IPC hello reports a different catalog edition, calls return
  `unavailable` (`catalog_mismatch`) until the bridge is updated. Ordinary
  state changes use query tools, not unsolicited telemetry.
- **Structured-output contract (D3-B03).** Every tool's `outputSchema` is a
  `oneOf` union of its success shape and, for tools that can pend, the
  `pending` shape (§8.3). Success/pending return `structuredContent` plus
  its JSON text equivalent. Errors return `isError:true` with **only** a
  text content item carrying the error DTO JSON — never `structuredContent`,
  so a validating client never sees a non-conforming structured payload. A
  permitted viewport observation also returns MCP `image` content
  (`image/jpeg`, base64); the image is never duplicated into
  `structuredContent` (metadata only). Codex's `inputImage` response is a
  different adapter format. No file/resource URI or path leaves the bridge.
  Envelope duplication counts in the byte caps (§9.4). A validating-client
  fixture schema-checks every fixture response.
- Valid tool calls with bad arguments, denied policy, unavailable hardware
  or operational failures return the common error DTO with `isError:true`.
  Unknown tools use `-32602` per the table above. Operation ids are opaque
  128-bit handles, not paths.
- Camera/photo/recording calls return the bounded `pending` operation
  result. `app.wait_for` polls at most five seconds and
  `app.operation.get` (§12.1, in the first catalog) retrieves durable
  operation state, so client disconnects cannot lose outcome evidence.
- `notifications/cancelled` retires a pending call by its JSON-RPC id; invalid,
  unknown or completed ids are ignored. For MCP cancellation, suppress a later
  response as recommended by the pinned protocol, unlike Codex's required
  terminal tool reply. App-side durable photo/finalization work remains
  truthful (§9.1). Never cancel `initialize` or treat cancellation as undo.
- EOF, broken pipe or app exit tears down the connection without killing
  OkuFlow. No invented MCP `shutdown` method.

**Epochs and reconnect (D3-M04).** Four distinct epochs: `appEpoch` (one per
OkuFlow process), `mcpSessionEpoch` (one per bridge process/stdio session),
`pipeEpoch` (one per bridge↔app pipe connection) and the lease id (§14.3).
The bridge holds a per-process random pairing secret in memory only and
presents it on pipe reconnect: same bridge process + same `appEpoch` + a
lease neither expired nor revoked ⇒ the pipe reconnect **preserves** the
lease and its idempotency reservations (scoped
`(appEpoch, leaseId, bridgeRequestKey)`; same key with different arguments
fails). A new bridge process, an app restart, or an expired/revoked lease
requires fresh user approval. Full tool results are never inherited across
leases; the sole carve-out is read-only `app.operation.get` status under a
later lease in the same `appEpoch` (unguessable operation id, status + safe
summary only, retained 15 minutes after terminal state, §8.5).

**Catalog editions and ship gate (D3-B01).** `catalogEdition: "dev"` exposes
Tier 0–2 only (no photo/recording, no camera selection) and is for internal
and fixture use; it must not be described as a shipped API and the bridge is
not added to release-bundle/MSI inventory in this edition.
`catalogEdition: "v1"` is the first shipped edition and requires the full
Phase 7 gate (§16): Tier 3 capture and camera selection working through
*both* the MCP bridge and the embedded adapters. `tools/list` advertises
only the active edition; a `dev` build never lists ungrantable tools.

#### Bridge, instance selection and authorization

From Phase 7 (`v1`), deploy `app/okuflow_mcp.exe` as supporting runtime beside
`OkuFlowApp.exe`, not as a second root user launcher (`dev` builds keep it
out of bundle inventory). Advanced Assistant settings can copy a
client configuration with an absolute executable path and an opaque instance
selector; these installation paths are local configuration, never model tool
data. The bridge never auto-launches, raises or terminates the GUI app. If the
app is absent, return `unavailable` rather than guessing another process.

OkuFlow opens an instance-scoped Windows named pipe only after the owner
enables **External MCP control**, separate from the embedded Assistant switch.
Use a random per-process pipe suffix, owner logon-SID DACL, same-session and
same-integrity checks, `PIPE_REJECT_REMOTE_CLIENTS`, first-instance ownership,
overlapped bounded reads and a length-prefixed private IPC envelope. The
descriptor in LocalAppData contains only protocol version, pipe selector and
process epoch, not a grant, token or camera data. No scanning arbitrary pipe
names, elevated impersonation or environment-supplied command execution.

The app verifies the connected bridge's held process handle, creation identity,
same user/session and canonical executable image in its own verified bundle;
the bridge verifies the pipe server's held process identity and matching
application image. Recheck on reconnect; PIDs or claimed `clientInfo` alone
are not ownership proof. Exact verification needs separate debug/release
fixture policy. Use the documented native process-id and pipe security APIs:
[client PID](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getnamedpipeclientprocessid),
[pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights).
These controls exclude accidental/foreign-process attachment, not malicious
code already running with the owner's account and integrity. State that limit.

An authenticated bridge is **not authorized to control or inspect the camera**.
The app shows a pending connection row only after the user opens its control
settings; tool traffic never opens a popup. Client name is sanitized untrusted
display text, not an identity credential. The user explicitly approves the
selected connection in OkuFlow. Until then the bridge can initialize/list the
static registry but cannot query live state, pixels, camera lists or operate.
No client method can click Allow or mint/widen a grant.

External clients cannot prove that their calls arose from a user prompt. The
app therefore issues a **bounded control lease** only from the lease-issuance
UI with its explicit grant bundle (§14.3): **300 s absolute, 60 s idle**
(no tool call) expiry, non-renewable by any tool (D3-M06). Tool calls reset
only the idle timer, never the absolute deadline. Each lease has a fresh
`ExternalLease` control context, field-revision baseline and undo journal.
Only one lease exists at a time: issuing a new one requires revoking or
expiring the old; no tool replaces a lease. A queued call whose lease
expires before dequeue is dropped and, if its JSON-RPC request is still
pending, answered `context_expired` (§8.3) — never executed. Expiry, Stop,
revocation, disconnect and restart retire queued effects, approvals and
frame delivery, and stop/finalize the lease's exact owned recording
(§9.1); manual recordings are never stopped by that cleanup.
`capture.recording.start` requires at least **30 s of absolute lease
remaining** (else `context_expired` with a re-approve hint); the recording
announcement states the lease-bound maximum duration. There is no
conversion of a lease recording into a manual one in v1; the user starts a
manual recording instead after the lease ends. No pretense of a trusted
internal `turnId`.

Only one external lease or embedded Assistant turn owns mutation at a time.
The shared arbiter returns `busy` on conflicts; native user edits always win.
IPC workers validate frame sizes, protocol/authentication and deadlines, then
post typed requests to the Qt GUI thread. They never access widgets directly.
Check authorization again on dequeue and before emitting image bytes. Bridge
and pipe queues have fixed caps and cancellation propagation; endpoint loss
cannot leave a dangling target pointer or invoke a completion in a new epoch.

Use a bridge-owned application request key in addition to the JSON-RPC id,
scoped as in *Epochs and reconnect* above. Retry of an in-flight delivery
within the same lease returns the reserved result; same key with different
arguments fails. Disconnected clients cannot accidentally duplicate a photo
by reconnecting and reusing an old id under a new lease: the accepted
operation remains visible to the owner and via the `app.operation.get`
carve-out, and retrying an uncertain capture is a new explicit action.

**Bridge build facts (D3-m06).** The bridge is a console executable with a
static CRT (`/MT`, like the launcher), links no Qt DLLs (Qt-free registry +
Win32 only), and the app creates exactly **one** named-pipe server instance
(first-instance ownership): a second bridge process receives `unavailable`
until the first disconnects.

#### Acceptance for a shipped MCP API

Desktop-free fake app/bridge tests cover handshake/version mismatches, early
requests, stdout purity, split UTF-8 lines, oversize/slow clients, malformed
JSON, list schemas, image/result mapping, error distinction, cancellation
races, duplicate calls, EOF and app/bridge crash. Private named-pipe fixtures
cover denied user/session/image identity, remote-client rejection, stale
descriptor, a spoofed first listener, no implicit grant and revoked/expired
lease during a queued operation. No real app endpoint or owner camera is used.
Run standard MCP client interoperability against the fake target (pin the
client and review its development-only license), then an explicitly scheduled
real-app/provider acceptance separately. Shipping requires clean nested bundle
and MSI inventory/hash/signing updates for the bridge plus licensing/docs;
success with Codex dynamic tools alone does not count as MCP acceptance.
This acceptance is necessary but not sufficient for shipping: the Phase 7
gate (§16) additionally requires the full owner scope through both adapters.

### 5.4 Future providers

The planned Claude-through-Anthropic-CLI provider would consume the same
registry through its own tool surface. Adapters never add provider-specific
semantics to the registry.

## 6. GUI-thread typed control dispatch

### 6.1 Thread, re-entrancy and user activity

- Handlers run on the GUI thread, posted with
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` so they never run
  inside the JSON-RPC parse loop or a network callback.
- One mutating call at a time (bounded FIFO; overflow → `busy`).
- **No dialogs during a tool call.** A tool call never opens a modal or
  nested event loop. Mutating and egress calls arriving while any modal is
  open (`QApplication::activeModalWidget()`) return `modal_active`. Tier 0
  reads (`app.state.get`, `app.operation.get`, `mode.list`, `view.metrics`)
  are exempt, so `state.ui.modal` stays reachable exactly when it matters
  (D3-M13); they mutate nothing and open nothing.
- **User activity pauses the agent.** While the user browses the quick-mode
  grid or color popover, drags a slider, or edits an inspector control,
  conflicting mutating calls return `user_interacting` with a short retry
  hint.
- Agent-initiated camera failures use a non-interactive status path, never
  `QMessageBox`.

### 6.2 Named production entry points (refactor first, behavior unchanged)

| Method | Today | Notes |
|---|---|---|
| `RequestPhotoCapture() → operationId` | `capturePhotoButton` lambda | Returns a durable operation (§9.3). |
| `RequestRecording(RecordingRequest)` | `recordButton` toggled lambda | Reserves exact session id/owner before microphone side effects; explicit per-session audio choice (`saved microphone` / `none`) without changing saved preference; `StopRecording(expectedSessionId, origin)` acts only on that same current session (including starting). Returns requested/actual audio and ownership. |
| `ApplyPresetById(id, PresetSource)` | `OnPresetSelectionChanged` | Applies and announces without grid navigation or focus effects; `MainWindow::ActivatePresetRow` keeps its UI behavior and calls this. |
| `SetRotationQuarterTurns(int)` | `OnRotationSelectionChanged` | Keeps the previous-value comparison. |
| `SetZoom(float, std::optional<ScenePoint> anchor)` | `InteractionController::ApplyZoom` | New scene-space anchor math (inverse of `ComputeViewTransform`) with crop-math tests; an anchor recenters even at unchanged magnification. |
| `RequestCameraTransition(identity) → operationId` | `OnCameraSelectionChanged` | Prerequisite: asynchronous start **and** shutdown ownership, non-modal failures, explicit requested/active/first-frame states; no abandonment of driver-owned resources on cancel. |
| `SetViewportFitMode(mode)` | `viewportFitCombo` lambda | — |
| `SetDisplayColorSchemeById(id)` | picker + `OnDisplayColorSchemeChanged` | — |

Widget handlers become thin adapters over these; existing GUI tests run
unchanged.

### 6.3 Field transitions, not a generic binding

Each controllable field gets a hand-written transition that: reads the
previous value; detects a no-op (unchanged value ⇒ `changed:false`, **no**
reset side effects); applies the change through the same slot logic in the
right order; updates every dependent widget, paired control (Simple and
Advanced Text Clarity) and slider readout; preserves preset identity rules
(zoom/pan keep the quick mode, tuning makes it custom); and bumps the field
revision. `ApplyConfigToUI` can later reuse the same transitions. Agent
preset application uses these transitions, never an `ApplyConfigToUI`
replay (§12.3).

**Persistence transaction (D3-M09).** One tool call performs **at most one**
`SavePersistentSettings` write, however many fields it touches (presets and
recipes included): a `ScopedPersistenceBatch` defers the sync+save that
individual slots request until the call's transitions finish, then saves
once. The single exception is synchronous safety markers (the per-camera
acceleration attempt/fallback state, §13), which save immediately even
inside a batch; that save is then the call's one save unless fields changed
after it. Crash semantics are stated plainly: applied in-memory values are
the truth and are **not** rolled back on restart; the in-memory undo
journal does not survive a crash, and whether the file holds pre- or
post-call values depends only on whether that one save completed. A disk
failure keeps the change applied and emits `settings_not_saved` (§10).

### 6.4 UI synchronization and focus

- Inspector, readouts, changed counts and preset selection update exactly
  as for a manual edit, so the accessibility tree reflects agent changes.
- No focus movement, no grid opening or closing, no window raising, no
  inspector reveal. `ui.mode.set` (Simple/Advanced) is excluded from the first
  catalog because it rearranges layout and focus order.
- With UI hidden (`Ctrl+H`), changes apply and announcements still reach the
  screen reader through a persistent accessible owner; the hidden state is
  kept.
- In Draw mode, view and image tools work; rotation rotates ink exactly as a
  manual rotation does.

## 7. Stable identifiers and provider-safe data

- Canonical ids are dotted (`view.zoom.set`); wire names use underscores
  (`view_zoom_set`, OpenAI pattern `^[a-zA-Z0-9_-]{1,64}$`); Codex namespace
  `okuflow`.
- Enum values are persisted tokens, never UI text: preset ids, scheme ids,
  `fill|fit`, `auto|dark_on_light|light_on_dark`, degrees `0|90|180|270`.
- Lists return `{id, displayName (current UI language)}`; descriptions say to
  select by `id` and speak `displayName`. Provider-facing names are bounded
  and sanitized (D3-m06): `displayName` ≤ 80 chars, descriptions ≤ 200,
  control characters and newlines replaced by spaces. A custom preset whose
  id does not match `^[a-z0-9-]{1,64}$` is omitted from `mode.list` (and
  counted in `omittedCount`) rather than forwarded as-is.
- **Device labels are generated**: `USB camera 1`, `Phone camera 1 (wide)`,
  `Microphone 2`. A product name is added only from allowlisted sources that
  cannot contain a serial (never the phone label, which falls back to the
  serial today). Raw symbolic links, serials, transport ids and endpoint ids
  never leave the app.
- **Handles are session-unique**: `cam-<random>` derived from a per-process
  random salt, so a handle quoted in a resumed conversation from an earlier
  app session never resolves; stale handles return `not_found`.
- Tool descriptions are English (model reliability, as for the built-in
  instructions). Announcements and action-transcript lines go through the
  catalog, `scripts/check_translations.ps1` parity, and the small-agent
  translation review policy for all 25 languages.

## 8. State, queries, replies and events

### 8.1 Versions

- **Field revisions**: every controllable field carries a revision and the
  source of its last change (`user`, `agent:<turn>`, `restore`).
- `viewVersion`: zoom, center and framing (presentation-only state —
  satisfied by the next present without a new camera frame).
- `processingVersion`: effect parameters **and rotation** (needs a newly
  processed camera frame). Rotation additionally bumps `viewVersion` and
  `geometryEpoch`; it cannot be satisfied by relabeling a cached old scene.
- `sourceEpoch`: camera identity/format changes; `geometryEpoch`: rotation,
  resize, cache remap.

### 8.2 State snapshot (`app.state.get`)

Bounded, allowlisted: camera (active, transition state, fallbackActive,
handle, kind, negotiated size/fps, orientation), view (framing,
zoomRequested, zoomEffective + reason, center, visibleSourceRect), profile
(presetId or null, displayName, modified), image (scheme id, monochrome,
contrast, brightness, autoContrast, textClarity, stabilization, keystone,
each with `effective` and an `inactiveReason` when shadowed), recording
(state, operation id, session id, owner kind, elapsed, audio), photo
operations, pipeline (`cuda|raw_passthrough|faulted`, focus), ui (hidden,
drawActive, modal, userInteracting), assistant (agent control status,
grants, remaining budgets), versions. An optional `sinceEvent` argument
(D3-B02) additionally returns `eventsAfter` (at most 32), `nextEventCursor`
and `eventsLost` (§8.5).

### 8.3 Results

Default success (kept small for a low-reasoning model):
`{ok:true, changed, applied, effective?, warnings?, stateVersion}`.
`previous`, `eventsSince` and verbose diagnostics only when requested or
relevant. Pending: `{ok:true, pending:true, operationId, state}`. Failure:
`{ok:false, error:{code, retryable, retryAfterMs?}}` with a fixed
English message per code — never a forwarded internal string. Codes:
`invalid_argument`, `out_of_range` (nearest valid value), `unavailable`,
`busy`, `user_interacting`, `modal_active`, `consent_required` (carries the
`approvalId`, §14.2), `consent_denied`, `denied_by_policy`, `not_found`,
`already_active`, `stale_state`, `stale_observation`, `budget_exhausted`,
`context_expired`, `canceled`, `timeout`, `internal`. The success and
`pending` shapes above are the two members of every MCP `outputSchema`
union; this failure DTO is deliberately outside that union (§5.3, D3-B03).

Values are snapped to UI precision (zoom 0.01×, contrast/brightness 0.01,
threshold 1/255) and the snapped value is returned.

### 8.4 The user always wins (enforced app-side)

At dispatch and again after any asynchronous wait, a mutating call is
rejected with `stale_state` when any field it would write was changed by the
user since the agent last read or wrote it — whether or not the model sent
`expectedStateVersion`. Manual edits during a turn cancel queued conflicting
agent work.

**Baseline definition (D3-M10).** The context's per-field *last-read
revision* is updated by exactly: `app.state.get` (every reported field),
any successful mutating result (the fields in its `applied`/`effective`
report, to their new revisions), `app.operation.get` (that operation's
state), and `mode.list` (the preset-list revision only). `view.metrics`
updates only observation stamps, never writable-field baselines. The
agent's own writes and `restore` (undo) writes within the same context
update the baseline and never count as user changes. Composite calls
(preset, recipe, `image.auto_adjust`) check every field they would write
against the baseline before the first mutation: all-or-nothing
*prevalidation*; an unexpected mid-apply failure stops, reports
`partial:true` with the applied list, and is announced — it never claims
full success.

### 8.5 Events and operation retrieval

Providers cannot push into a running turn, so events are pulled:
per-context sequence-numbered ring (256 events); `app.state.get
{sinceEvent}` returns events after the cursor; overflow reports
`eventsLost:true` and the model must re-read state.
`app.wait_for {condition, timeoutMs ≤ 5000, operationId?}` long-polls
`camera_ready`, `view_presented`, `frame_processed`, `operation_state`
(`operationId` required for that condition), and `grant_changed` (an
approval was resolved or a grant became active, §14.2).
**`app.operation.get {operationId}`** (Tier 0, first catalog, D3-B02)
returns one durable operation's state and safe summary; terminal records
are retained 15 minutes. It is the one read a later lease in the same app
session may make about an earlier context's work (§5.3).

## 9. Cancellation, timeouts, idempotency and undo

### 9.1 Cancellation and recording lifetime

Tool-delivery cancellation and stopping a recording are separate operations.
Cancellation retires provider delivery: claimed Codex server requests get one
terminal reply (`canceled`); MCP cancellation suppresses a reply (§5.3).
Drop queued effects and observations, but an accepted photo remains truthful
and its local media/notes completion still fires. A recording already stopping
completes finalization; an active recording does not stop merely because its
embedded model turn completed normally.

| Event | Queued work / approvals | Accepted embedded-agent recording | External MCP recording |
|---|---|---|---|
| Normal model turn completion | Close tool delivery; exact approval may remain for its separate 30-second UI lifetime (§14.2) | Continues until explicit recording stop; summary states it is still recording | Continues only inside its approved lease |
| New prompt or panel Stop Assistant | Cancel pending tool effects/frame delivery and unconsumed approvals | Continues; Stop Assistant is not Record/Stop | Does not renew or widen an external lease |
| Explicit owned-recording Stop | Cancel pending start for the exact session; otherwise finalize that same session | Stops exact owned id | Stops exact owned id |
| Global Stop-and-undo | Cancel control context, approvals and queued effects; undo conflict-free view fields | Stop only agent-owned recording for that context; finalize it | Retire lease and stop its exact owned recording |
| Control/grant revocation, switch disable, provider failure/config change, child exit | Retire relevant contexts, approvals and delivery | Stop recording owned by that revoked agent-control session | Retire lease and stop its exact owned recording |
| MCP EOF, bridge loss, lease expiry | Retire external context/approvals | Unaffected | Stop/finalize its exact owned session, including a pending start |
| App shutdown | Retire all control contexts | Normal app recording shutdown/finalization | Same |

Every cleanup uses an app-owned recording-session identity and expected revision.
A late callback never invokes today's unqualified `RecordingManager::Stop`
against a replacement/manual recording. If start is still pending, mark that
exact session canceled before microphone/encoder admission and retire its own
resources. Once accepted, a recording is owned by the agent control session
(or lease) rather than one tool request, so a later agent turn in that
session can stop it under the session recording toggle (§12.5, §14.2).
Native Record/Stop remains unconditional. Camera stop or presenter fault retires
observations; recording errors use the manager's truthful failure/stop lifecycle.

### 9.2 Deadlines

| Operation | Tool deadline | Behavior |
|---|---|---|
| Field setter | immediate | — |
| Viewport observation | 3 s (next present after the change) | `timeout`, no frame sent |
| `app.wait_for` | ≤ 5 s | `{met:false}` |
| Camera transition | immediate `pending` | progress via `wait_for` / operation state |
| Recording start | ≤ 5 s to `recording` or failure, else `pending` | — |
| Recording stop | immediate `pending`; existing 8 s watchdog authoritative | terminal `completed` or `failed` |
| Photo | immediate `pending` | `committed` / `failed` event |

All stay below the provider tool timeout measured in Phase 0 and far below
the 90 s idle watchdog.

### 9.3 Idempotency and operations

- Idempotency entries are **reserved at admission** (including in-flight
  calls), keyed by provider, child epoch, thread, turn generation and call id,
  with an argument digest. A duplicate returns the cached or in-flight
  result; a reused id with different arguments is `invalid_argument`.
- Setters are idempotent only through no-op detection (§6.3), because
  re-sending some values has reset side effects today.
- Photo, recording and camera transitions are durable operations with
  explicit states; deduplication and "pending" reporting use operation state,
  not `photoCapturePending_`.

### 9.4 Rate limits and budgets (pinned numbers, D3-M12)

All values below are implementation constants, mirrored into
`docs/hardcoded_paths.md` when implemented. Budgets are reported in state;
exhaustion returns `budget_exhausted`.

| Budget | Value |
|---|---|
| Mutating calls | ≤ 20 per embedded turn; ≤ 60 per lease; token bucket 2/s sustained, burst 4 |
| Composite calls (preset, recipe, `image.auto_adjust`) | Count as **one** mutating call and one composite token; ≤ 1 composite per 2 s. Their internal field transitions do not each consume flashing budget (D3-M10). |
| Color / brightness / contrast field changes | ≥ 250 ms between accepted changes of the same field (no flashing loops) |
| Observations | Embedded: §5.1.8 (≤ 3 images/turn, ≤ 256 KiB base64 each, ≤ 768 KiB/turn). External: ≤ 6 per lease, same per-image cap (JPEG ≤ 192 KiB before base64) |
| Approvals | ≤ 1 pending per context; ≤ 3 created per context (§14.2) |
| Tool arguments | ≤ 16 KiB serialized |
| Tool result `structuredContent` | ≤ 32 KiB serialized; MCP response line ≤ 1 MiB including text equivalent and any image |
| MCP stdio line (inbound) | ≤ 1 MiB including newline (§5.3 strike rule) |
| Windows IPC envelope | ≤ 512 KiB per frame; bridge→app ≤ 8 pending requests; app mutating FIFO ≤ 8; per context 1 mutating + ≤ 4 Tier 0 in flight |
| Event ring / operation retention | 256 events per context; terminal operations kept 15 min |
| Idempotency cache | ≤ 128 reservations per context, evicted at context end |

Codex turns keep the tighter §5.1.8 aggregate limits on top of this table.

### 9.5 Undo: agent-owned, conflict-aware journal

- The journal records inverse operations only for fields the agent changed in
  the turn. Undo restores a field only if its current revision is still the
  agent's last write; fields the user touched are skipped and reported.
- Restoration uses the same field transitions (no whole-config replay). Undo
  states plainly that stabilization reference and keystone history cannot be
  restored once reset.
- Camera source/format changes have a separate *Switch back* action (a new
  transition, Tier 2 rules), not snapshot restore.
- Photos, recordings and saved files are not undoable; the summary says so.
- **Retention (D3-M13):** the last completed context's journal is retained
  until the next context starts or the app exits, so Stop-and-undo still
  works after a turn/lease ends; the announcement names whose changes were
  undone. During an active context, the same command stops it first (§15).

## 10. Interaction with in-flight camera/GPU work

| Condition | Agent behavior |
|---|---|
| Camera transition or reconnect backoff | Camera tools `busy`; view/profile transitions apply to state and take effect on the first frame. |
| Camera or format change while recording | `denied_by_policy`. Production supports `_partN` segments; the agent policy denies it so a lecture recording is not split or re-sourced by the model. |
| Rotation while recording | Denied until a fixture verifies segment behavior for odd quarter turns. |
| Photo operation active | Duplicate → same operation; otherwise `busy` if the ring/pool is saturated. |
| Recording stopping/finalizing | `busy`; abandoned worker → `unavailable` for the session. |
| Presenter/CUDA terminal fault | Image and observation tools `unavailable`; state reports `faulted`. |
| Raw passthrough (no CUDA) | Image-effect tools return `unavailable` (not "stored but invisible"); observation still works. |
| Shadowed control (e.g. hard Black & White under Text Clarity) | Result `effective:false` with `inactiveReason`; recipes handle it (§12.4). |
| Stabilization/keystone/temporal resets | Allowed; observations and metrics report `converging` until the reference is rebuilt. |
| Extra Stable holding an old frame | Metrics/observations report `held:true`; not counted as fresh content. |
| Readback ring shared by consumers | Observations add their own consumer slot and record the `viewVersion` actually presented; a co-tenant readback from before the change never satisfies an observation. |
| `Test this camera` running | Camera tools `busy`. |
| Draw active | Allowed; rotation rotates ink. |
| Disk save failure | Event `settings_not_saved`; the change stays applied in memory. |

## 11. Image-to-control observations

### 11.1 Tools

- `view.observe {maxLongSide: 384–1024 (default 768)}` returns one JPEG
  `inputImage` of the **current viewport** (the present-time back-buffer
  readback, i.e. exactly what the user sees, including SuperRes caches and
  letterboxing) plus metadata. `scene` and `original` targets are not in the
  first catalog: they egress more than the user approved, and `original`
  coordinates are not processed-scene coordinates once stabilization or
  keystone applies.
- `view.metrics {}` is pixel-free: focus score/acceptability, luma mean and
  percentiles, contrast spread, Text Clarity scene class (paper/board/mixed),
  keystone quad presence, frame age, fps, `held`, `converging`, `stale`.

### 11.2 Freshness

Each observation and metrics result is bound to `sourceEpoch`,
`processingVersion`, `viewVersion`, `geometryEpoch`, source frame
sequence/time, the presented transform and Fit letterboxing. View-only
changes are satisfied by the next present; effect changes wait for a
processed frame at the new `processingVersion`. Rotation waits for newly
rotated/processed pixels and their matching presented transform; old cached
pixels cannot inherit the requested rotation stamp. `image.auto_adjust` returns
`pending` until a qualifying frame exists rather than claiming fresh metrics.

### 11.3 Coordinates

`{space:"scene", x, y}` (normalized 0–1, rotated processed scene, origin
top-left, x right, y down) or `{space:"observation", observationId, px, py}`
converted app-side with that observation's stored transform (including
letterboxing). Observations from an older `sourceEpoch`/`geometryEpoch` are
`stale_observation`.

### 11.4 Privacy and budget

- Observations require a **frame grant**: embedded turns get one from the
  pre-turn privacy notice when the user prompt attached the current view
  with control on (extra views of the *same* camera, count limited);
  external leases get one only from the lease bundle toggle (§14.3). After
  a camera-source change, further observations need a fresh grant: embedded
  turns use the frame-approval path in §14.2 (Allow activates the grant for
  a later call or the next turn — it never captures or sends an image
  itself, and no image is delivered into a finished turn); external leases
  cannot widen mid-lease and must be re-issued.
- Per-turn image count and byte budget (§5.1.8).
- The privacy notice is shown **before** the turn when Assistant control is
  on ("may send up to N more views of this camera this turn"), and the
  action transcript lists each sent view.
- Observation frames are never written to lecture notes.

## 12. Tool catalog (first version — deliberately small)

Editions (D3-B01): `dev` = Tier 0–2 rows only; `v1` (first shipped) = all
rows below **plus** camera selection (§13). A build lists only its edition.

### 12.1 First catalog

| Id | Tier | Arguments (units, range) | Production path |
|---|---|---|---|
| `app.state.get` | 0 | optional `sinceEvent` | §8.2 |
| `app.operation.get` | 0 | `operationId` | §8.5 |
| `app.wait_for` | 0 | `condition`, `timeoutMs` 0–5000, optional `operationId` | §8.5 |
| `view.metrics` | 0 | — | §11.1 |
| `view.observe` | 0E | `maxLongSide` | §11 |
| `mode.list` | 0 | — | built-in + custom presets (ids, 1–9 shortcut, `agentApplicable` + reason) **and** `colorSchemes` (built-in scheme ids + display names, D3-m05) |
| `mode.activate` | 1 | `presetId` | `ApplyPresetById` after preset validation (§12.3) |
| `view.zoom.set` | 1 | `magnification` 1.00–16.00 (× relative to the Fill view at 1×), optional `anchor` point | `SetZoom` |
| `view.center.set` | 1 | `point` (§11.3) | `SetZoomCenter` |
| `view.reset` | 1 | — (1×, center) | transitions |
| `display.reading_colors.set` | 1 | `intent` (§12.4), optional `schemeId` (from `mode.list` `colorSchemes`) | recipe |
| `image.contrast.set` | 1 | `contrast` 0.25–4.00 multiplier (1.00 neutral), `brightness` −1.00–1.00 offset (0 neutral) | transitions |
| `image.auto_adjust` | 1 | `goal` `read_text|see_scene|projected_screen|board|low_light`, `allowModeChange` bool | §12.6 |
| `camera.orientation.set` | 2 | `degreesClockwise` 0/90/180/270 | `SetRotationQuarterTurns` |
| `capture.photo.take` | 3 | — | `RequestPhotoCapture` |
| `capture.recording.start` | 3 | `audio` `saved_microphone|none` | `RequestRecording` |
| `capture.recording.stop` | 3 | optional `sessionId` (defaults to the caller's own owned session, §12.5) | `StopRecording(expectedSessionId, origin)` |

### 12.2 Later catalog versions

Added only after the first catalog's tool-selection accuracy is measured
(Phase 0 probe and owner acceptance): `view.zoom.step`, `view.pan.by`,
`camera.sources.list` + `camera.select` (needs §6.2 async transitions),
`camera.formats.list` + `camera.format.set` (format ids enumerated per camera
handle), `camera.orientation.rotate`, `camera.phone_light.set`,
`screen.keystone.*` (with idempotent pause state — the current handler
toggles), `image.stabilization.set`, `image.sharpen.set`,
`view.framing.set` (Tier 2: global and persisted), individual Text Clarity
controls, annotation tools. `deferLoading` is used only if Phase 0 shows it
helps.

### 12.3 Preset validation, field inventory and assistant isolation

Before `mode.activate`, the resolved `AdvancedConfig` is checked as a whole
against current state. A preset is **not agent-applicable** — returns
`denied_by_policy` with the reason, and `mode.list` marks it — if it would:

- change `vlmAssistEnabled` in either direction (enabling starts periodic
  Scene Explain frame upload about every 1.6 s with the OpenAI-compatible
  provider — built-in `preset-scene-explain` sets it; disabling can abort
  the running assistant, next paragraph);
- change `assistiveOverlayEnabled` (panel visibility is the user's, not a
  preset side effect the agent may cause);
- enable debug view or SuperRes (`mlSuperResEnabled` or its mode flags).

Fields are never silently stripped while claiming the preset was applied.

**Assistant isolation refactor (A3-06, required with the preset work).**
Today `AssistiveFeatureManager::SetModes(vlm, overlay)` forwards
`vlmEnabled && overlayEnabled` to `AssistiveRuntime::SetModes`, whose
turning-off path aborts `activeReply_`, cancels preparation and clears
answer/status — so a preset could cancel or hide the very agent turn
applying it (Astra cited the runtime method; the production entry is the
feature manager, verified 2026-10-14 — the effect is the same). Split it
into `SetPeriodicAnalysisEnabled(bool)` and `SetOverlayEnabled(bool)`:
turning periodic analysis off aborts only a reply whose origin is periodic
analysis, never a user-requested Read/Explain/Assistant/agent turn;
overlay visibility never aborts anything. Manual preset application keeps
its full effect through these two calls; agent application is additionally
guarded by the reject-on-change rule above, so an agent preset can neither
start periodic upload nor disturb its own turn or panel.

**Field inventory (D3-M10).** Verified against
`UIStateManager::ApplyConfigToUI` / `ReadConfigFromUI` on 2026-10-14.
*Preset-applied* means today's whole-config apply writes it. Agent preset
application replays only the *Agent rule = transition* fields through
§6.3 transitions (one composite token, one save, §9.4/§6.3):

| `AdvancedConfig` fields | Preset-applied today | Ownership | Agent rule |
|---|---|---|---|
| `id`, `name`, `description` | metadata only | profile | never written; sanitized in DTOs (§7) |
| `zoomEnabled`, `zoomAmount`, `zoomCenterX/Y` | yes | profile | transition (Tier 1) |
| `blackWhiteEnabled`, `blackWhiteThreshold` | yes | profile | transition |
| `blur*`, `temporalSmooth*`, `spatialSharpen* / spatialUpscaler / spatialSharpness`, `focusMarker` | yes | profile | transition |
| `stabilizationEnabled`, `keystoneEnabled` | yes (resets histories) | profile | transition; undo states resets are not restorable (§9.5) |
| `displayColorMode`, `colorScheme`, `contrast`, `brightness` | yes | profile | transition |
| `autoContrast*`, `autoTextClarityEnabled` and the Text Clarity family (`backgroundFlatten*`, `adaptiveBinarizationEnabled`, `sauvolaStrength`, `binarizationSoftness`, `textPolarityMode`, `strokeWeight`, `smartSharpen*`, `clahe*`, `twoColorTextEnabled`, `textHysteresis*`, `selectiveSharpenEnabled`, `focusDetectionEnabled`, `focusThreshold`, `glareSuppression*`) | yes | profile | transition |
| `mlSuperResEnabled`, `mlSuperResStrength`, `mlSuperResPrefer2x`, `mlSuperResUltra1440p` | yes | profile | reject preset if it would enable/change mode; strength-only change while already on is also rejected in v1 (Tier X for the agent) |
| `debugView` | yes | profile | reject preset if it would enable |
| `vlmAssistEnabled`, `assistiveOverlayEnabled` | yes (via `SetModes`) | assistant-owned, not image profile | reject preset on any change (above) |
| `rotationQuarterTurns` | **no** — read from old profile JSON for migration only; `ApplyConfigToUI` never applies it and profile comparison ignores it | **global** (`PersistentSettings.rotationQuarterTurns`) | never part of a preset; only `camera.orientation.set` (Tier 2). The transitions refactor must not start applying the profile copy. |

All other effects of an accepted preset go through rate limits and the
undo journal.

### 12.4 Reading colors and "black and white"

`display.reading_colors.set {intent}` is an atomic recipe (all-or-nothing
prevalidation, one composite token, one save) over exactly these fields:
Text Clarity master (`autoTextClarityEnabled`), adaptive binarization
(`adaptiveBinarizationEnabled`), two-color text (`twoColorTextEnabled`),
text polarity (`textPolarityMode`: 0 auto, 1 dark-on-light,
2 light-on-dark), hard Black & White (`blackWhiteEnabled`) and the display
scheme. This precision matters because production composes them in CUDA:
binarization is active when adaptive binarization *or* the Text Clarity
master is on; the two-color composite takes its foreground/background from
the scheme endpoints selected by `textColorAtHighLuma` (`white-black` and
`black-white` have identical black/white stops and differ only in that
flag — scheme alone guarantees no polarity); hard Black & White runs only
when binarization is inactive (verified in `cuda_interop.cpp` and
`color_schemes.cpp`, 2026-10-14; A3-05).

| Intent | Master | Adaptive bin. | Two-color | Polarity | Hard B&W | Scheme |
|---|---|---|---|---|---|---|
| `photographic_color` | off | off | off | unchanged | off | `normal` |
| `grayscale` | off | off | off | unchanged | off | `grayscale` (gray levels preserved; any binarization would destroy them, hence master off) |
| `high_contrast_dark_on_light` | on | on | on | 1 dark-on-light | off | `black-white` (fg = black stop, bg = white stop) |
| `high_contrast_light_on_dark` | on | on | on | 2 light-on-dark | off | `white-black` (fg = white stop, bg = black stop) |
| `scheme` | unchanged | unchanged | unchanged | unchanged | unchanged | explicit `schemeId` from `mode.list`; result reports the upstream binarization/two-color state and `effective`/`inactiveReason` instead of promising a polarity |

Fields not in the recipe (contrast, brightness, background flatten,
Sauvola strength, stroke weight, sharpening…) are untouched. Turning the
Text Clarity master off/on is reported in the delta like every other field
— never a silent side effect. Tests run every intent from Text-Clarity-on
and -off starting states and assert the composed CUDA-settings snapshot
(which stages are active and which foreground/background pair results),
not only member values.

Descriptions tell the model: for "black and white" in a reading context,
choose the high-contrast intent matching `view.metrics` scene class; use
`grayscale` for scenes and photos; "color" means `photographic_color`.

### 12.5 Rotation, zoom and recording semantics

- **Rotation** turns the camera image content clockwise in 90° steps before
  processing; it is global (every mode). Not the window, not the phone
  sensor, not keystone, not a small tilt. "Rotate left" = counterclockwise;
  "upside down" = 180. Small angles return `unsupported` and suggest
  Straighten Screen for projected slides.
- **Zoom** is desktop magnification of the camera image, not phone optical
  zoom. In Fit framing the view shows 1×; the result reports
  `effective: 1.0, reason: fit_framing` instead of silently changing the
  global framing.
- **Recording** results carry the operation id, session id, owner kind
  (`manual | agent-session | external-lease`), requested and actual audio,
  and a truthful terminal state (`completed` or `failed`), distinguishing
  finalizing, abandoned-worker and failed-before-first-frame.
  `capture.recording.start` while a recording is active (D3-m06): if it is
  owned by the caller's own agent session/lease, succeed with
  `changed:false, alreadyActive:true` and that session id; if it is manual
  or another owner's, fail `already_active` and never claim it. Ownership
  persists across embedded turns for the agent control session (§4.1), so a
  later turn can stop a recording an earlier turn started.
  `capture.recording.stop` resolves its target as: explicit `sessionId` if
  given, else the caller's own owned session; stopping a manual recording
  is possible only through an exact approval bound to that session id
  (§14.2). Stop of an already-terminal owned session succeeds with
  `changed:false`. Every stop/cleanup path uses
  `StopRecording(expectedSessionId, origin)` (§6.2, §9.1) — today's
  unqualified `RecordingManager::Stop` (stops whichever session is current,
  `recording_manager.cpp`) is never called by control code.

### 12.6 `image.auto_adjust`

An app-side, explainable recipe: wait for fresh metrics; map goal and scene
class to an agent-applicable built-in mode when `allowModeChange`
(board → `preset-whiteboard`, projected screen → `preset-projector`, paper →
`preset-document`, low light → `preset-low-light`), otherwise enable the
current mode's automatic features (Text Clarity automatic treatment, Auto
Contrast, automatic polarity, keystone detection for projected screens);
return the full delta, then fresh metrics once a qualifying frame exists.

### 12.7 MCP tool annotations (D3-m04)

Published with the registry; hints only — app-side policy stays
authoritative. `openWorldHint:false` everywhere.

| Tools | readOnly | destructive | idempotent |
|---|---|---|---|
| `app.state.get`, `app.operation.get`, `app.wait_for`, `view.metrics`, `mode.list` | true | false | true |
| `view.observe` | **false** (camera-image egress is a side effect; the title says it sends a camera view, so clients must not auto-approve it as a pure read) | false | false |
| `view.zoom.set`, `view.center.set`, `view.reset`, `display.reading_colors.set`, `image.contrast.set`, `camera.orientation.set` | false | false | true (absolute setters with no-op detection) |
| `mode.activate`, `image.auto_adjust` | false | false | false (history resets) |
| `capture.photo.take`, `capture.recording.start` | false | false | false |
| `capture.recording.stop`, `camera.select` | false | false | stop: true (§12.5); select: true (same target ⇒ no-op transition) |

## 13. Camera source intent (required follow-on catalog)

- Intents: switch device; phone lens (separate camera entries of one phone);
  phone optical zoom (not available); "look at something" (pan/zoom only).
- Source switches need an exact approval or the lease bundle toggle
  (§14.2–14.3), never
  happen during recording, and offer *Switch back*. A temporary fallback
  webcam is reported as `fallbackActive` so the model does not "fix" it.

**Three-identity source state model (A3-03, D3-M08).** "Do not persist in
`OnCameraSelectionChanged`" cannot implement temporary intent, because
today `SavePersistentSettings()` copies the *active* index
(`app_settings.cpp`), startup fills a missing saved identity, delayed phone
discovery can write the saved link, and acceleration safety-marker saves
run during transitions. The refactor therefore separates, as distinct
state with an `origin` carried through startup, discovery, reconnect and
fallback:

| Identity | Meaning | Written by |
|---|---|---|
| `savedStartupCamera` | The persisted startup preference: `cameraSelectionExplicit`, symbolic link, legacy index fallback | Manual selection; startup one-time identity fill when none was saved |
| `requestedSource` | What should be running now, with origin (`startup`, `manual`, `agent-temporary`, `reconnect`, `fallback`) and transition operation id | Every transition request |
| `activeSource` | What is actually streaming (identity, format, first-frame) | Capture pipeline only |

Ordinary saves serialize `savedStartupCamera` **independently of the
active index** (the `cameraIndex := selectedCameraIndex_` copy is removed;
the serialized legacy index is the saved identity's current enumeration
index, or the loaded legacy value when it is not enumerated). Per-field
rules for agent-temporary switches, relative to today's production side
effects (verified in `app_controls.cpp` / `app_pipeline_runtime.cpp`):

| Field / side effect today | Agent-temporary rule |
|---|---|
| `cameraIndex`, `cameraSelectionExplicit`, `cameraSymbolicLink` | Untouched. Only manual picks (and the startup identity fill) write them. Delayed phone discovery updates enumeration only while an agent-temporary source is requested/active; it may fill the saved identity only with origin `startup`/`manual`. |
| `automaticPhoneSelectionAllowed_`, `preferredPhoneCamera_` | Derived from `savedStartupCamera` + manual picks only. An agent switch records their prior values in the transition record and does not change them; temporary phone use sets a separate session-scoped `temporaryPhoneIntent`. |
| `PhoneCameraTransport::AllowAppLaunch(serial)` + owned-adb bootstrap | Still performed for a temporary phone pick (it is real phone-use intent) — reported in the result and announced — but keyed to `temporaryPhoneIntent`, so *Switch back* / session end releases that intent without touching the saved phone preference. |
| Global `cameraFormatStableId` (read at every start) | Agent transitions pass an explicit format stable id (empty = camera default) instead of reading the global value, and agent format changes (later catalog) live in an in-memory per-device override map. Only manual format picks write the global saved value. |
| Per-camera acceleration attempt/fallback + `cameraAccelerationAttempt` safety marker | **Deliberately persisted** exactly as today, including its synchronous save: these are per-device safety facts, not preferences. That save now serializes `savedStartupCamera`, so it cannot leak the temporary index. Documented exception, covered by fixture. |
| Mic endpoint, rotation and other fields copied by `SavePersistentSettings` | Unchanged behavior, but they ride the same independent-camera serialization. |

**Failure and *Switch back*.** A failed/cancelled agent transition leaves
`activeSource` unchanged when the old capture still runs; if the old
capture was already stopped, the previous active source is retried once
and any substitution is reported as `fallbackActive`. `requestedSource`
reverts to the restored active identity. *Switch back* is a new Tier 2
transition to the active identity recorded immediately before the first
agent-temporary switch of the session (not necessarily the saved startup
device), and restoring it also clears `temporaryPhoneIntent`. If that
device is gone, *Switch back* targets `savedStartupCamera` and says so.
Fixtures cover: legacy index-only settings, delayed phone discovery during
a temporary switch, an unrelated settings save during a temporary switch,
an acceleration-marker save during a temporary switch, failed temporary
switch with and without a still-running previous capture, and *Switch
back* to a since-removed device.

## 14. Consent, privacy and security

### 14.1 Tiers

| Tier | Tools | Rule |
|---|---|---|
| 0 Read | state, lists, metrics, wait | Inside an agent turn. |
| 0E Egress | `view.observe` | Frame grant for the turn (§11.4); budgeted. |
| 1 Reversible view | zoom, center, reset, mode, reading colors, contrast/brightness, auto adjust | Autonomous once Assistant control is on; batched announcements; agent-owned undo. |
| 2 Source/global | orientation (later: camera, format, framing, phone light) | Exact approval (§14.2) or the lease bundle toggle (§14.3); never during recording; assertive announcement. |
| 3 Capture | photo, recording start/stop | Session capture toggle, lease bundle toggle, or exact approval (§14.2–14.3). |
| X Never | §2 list | Not in the registry. |

### 14.2 Two distinct mechanisms: standing grants and exact approvals

Runtime authorization has exactly two user-action mechanisms. Neither is a
development/owner permission flow; both exist only inside the finished app.

**Standing grants** are explicit toggles the user sets in OkuFlow's UI,
checked at dispatch:

- **Master switches** (persisted, default off, §14.4): *Assistant control*
  (embedded) and *External MCP control*, in AI Settings. First activation
  shows an accessible explanation. They ship in the core phase, before any
  live adapter — nothing can mutate state before they exist.
- *Frame grant* (embedded, per turn): from the pre-turn privacy notice
  (§11.4); voided by a source change.
- *Session capture toggles* (embedded, panel, default off, never persisted):
  "Allow photos this session" and "Allow the Assistant to start and stop
  recordings it started", the latter beside the "Ask permission before
  recording a lecture or other people" reminder. Revocable in the panel;
  expire on master-switch off or app exit.
- *Lease grant bundle* (external): §14.3.

**Exact approvals** cover everything else Tier 2/3. There is no standing
"source grant" and no grant is ever created as a side effect of an approval.

- **No modal consent during a tool call.** A call lacking authorization
  returns `consent_required {approvalId}` immediately and the app creates
  one **approval record**: unguessable id; context id and context/lease
  epoch; canonical tool id; the immutable normalized arguments (stored,
  with digest); the exact target identity (recording `expectedSessionId`,
  camera handle + `sourceEpoch`, or none); the relevant field revisions;
  creation time; expiry (context end + 30 s grace, at most 5 minutes from
  creation); state (`pending | consumed | denied | expired | voided`) and
  kind (`action` or `frame-grant`). At most one pending approval per
  context; a second `consent_required` returns `busy`.
- The panel shows a persistent, keyboard-reachable approval row ("The
  Assistant asks to stop the recording that started at 10:02 — Allow /
  Deny"), announced assertively with per-request distinct text (§15), no
  timeout-accept, Deny as the Escape action, reachable with the UI hidden.
  Shortcuts are named: **Ctrl+Shift+A** moves to the approval row when one
  is pending (application-scoped; Phase 1A fixture verifies no existing
  binding collides), buttons carry `&Allow` / `&Deny` mnemonics, and
  **Ctrl+Shift+U** remains Stop-and-undo (§15).
- **Allow is single-use and exact (A3-01).** On the GUI thread, atomically:
  verify the record is still `pending` and unexpired; verify the context
  epoch is still valid (or within the post-turn grace for `action`
  approvals); verify the target still matches (the recording session id is
  still the current session — a session that ended and was replaced voids
  the approval; the camera handle still resolves in the same
  `sourceEpoch`); verify no user edit touched the recorded field revisions;
  re-run the full authorization checks; mark the record `consumed`
  **before** dispatch; then execute exactly those stored arguments once
  through `AppControlService` with origin `approval`. The result is an
  **operation receipt** `{approvalId, operationId?, outcome}` in the action
  transcript and in the context's event ring (`grant_changed` wakes
  `app.wait_for`). Allow creates **no** standing grant: an approved photo
  is exactly one photo, and a model retry under a new call id needs a new
  approval (or the session toggle). That closes the "approved once,
  repeated freely" hole.
- **Deny / expire / void.** Deny, Escape, expiry, context retirement,
  revocation, master-switch off, provider/config change or a new prompt
  consumes the record without executing (normal turn completion alone does
  not — the 30 s grace covers reaching the row, §9.1 table). After a Deny,
  the same tool+arguments digest returns `consent_denied` for the rest of
  the context instead of re-asking (no nag loop); different arguments may
  ask again within the §9.4 approval budget. Cancellation of the model
  call never executes a pending approval.
- **Frame-grant approvals are not perform-on-Allow (D3-M07).** Allow on a
  `frame-grant` approval only *activates the grant*: for the remainder of
  the current turn if it is still running (the model learns via
  `grant_changed` / its next result and must call `view.observe` again), or
  for the **next embedded turn** on the same camera/`sourceEpoch` if the
  turn already ended (expires in 5 minutes or on source change). No image
  is ever captured by Allow itself or delivered into a finished turn.
  External leases have no frame approvals: without the bundle toggle,
  `view.observe` returns `denied_by_policy` and the lease must be
  re-issued.
- An agent may stop **only a recording session its own control session
  started** under the recording toggle/bundle; stopping a manual recording
  always needs an exact `action` approval bound to that session id. Native
  Record/Stop stays unconditional.
- Grants and approvals are re-checked immediately before execution and
  before any frame is transmitted. A model statement, keyword match, or
  text in the camera image never satisfies a grant or approval check.
- **Injection boundary, stated honestly**: content in the camera view can
  cause Tier 1 changes (announced, undoable) and can exercise any
  capability the user has *currently* granted. It cannot create or widen a
  grant, consume an approval, or renew a lease.

### 14.3 External lease grant bundle (D3-B04)

An authenticated bridge connection (§5.3) has no authority until the user
issues a lease in **AI Settings → External MCP control**. The pending
connection row appears only when the user opens that section (tool traffic
never opens anything). *Approve lease* opens an inline bundle panel — every
toggle default **off**, shown with its budget — and one explicit Approve
action issues the lease (§5.3: 300 s absolute / 60 s idle) with exactly:

| Toggle | Grants | Bound |
|---|---|---|
| Adjust view (shown fixed-on: it is what a lease is) | Tier 1 reversible tools | §9.4 lease budgets |
| Send camera views | `view.observe` of the current camera | ≤ 6 per lease; voided for a new source until re-issued |
| Orientation and camera source | `camera.orientation.set`, `camera.select` (temporary, §13) | never during recording |
| Take photos | `capture.photo.take` | §9.4 |
| Start/stop recordings it started | `capture.recording.start/stop` for lease-owned sessions only, with the recording-permission reminder text | ≥ 30 s lease remaining to start; auto-finalizes at expiry |

Without a toggle, the corresponding calls return `denied_by_policy`
(not an approval prompt): there is **no mid-lease widening**. Narrowing is
always available (*Revoke lease*, master switch, client disconnect §15).
The bundle, remaining time and budgets are shown in the same settings
section and announced at issuance ("External control allowed for 5 minutes:
view adjustments and camera views. Disconnect the MCP client or press
Revoke to stop it.").

### 14.4 Persisted control settings (D3-m06)

Settings schema rises from today's version 16 to **17** (newer files keep
failing closed; v16 files load with the defaults below). New persisted
fields, and nothing else: `assistive.agentControlEnabled` (bool, default
false), `assistive.externalMcpEnabled` (bool, default false), and per
`CodexConversation`: `kind` + `toolsetVersion` (§5.1.4). Session capture
toggles, frame grants, approvals, leases and journals are **never
persisted**. A persisted-on external switch opens the named pipe at every
startup (listening only: no lease, no grants, `tools/call` unauthenticated
gets `unavailable`); a persisted-on embedded switch makes control
available to the next agent conversation but restores no grant.

### 14.5 Panel routing into an agent conversation (A3-07)

Both follow-up surfaces (floating Explain/Assistant panel and the Advanced
Assistant editor) already funnel into `SubmitAssistantPromptText` with the
selected conversation id; routing is decided there, before any submission,
and never rewrites or discards existing history:

| Control switch | Selection at submit | Route |
|---|---|---|
| Off | anything | Today's tool-free behavior, unchanged. |
| On | an `agent` conversation | Agent turn in that conversation (restricted child or HTTP tool loop). |
| On | none / after *New* | Create a new `agent` conversation, select it, submit there. |
| On | legacy `assistant`/`explain`, or `coding` | **Do not submit.** Keep the typed prompt in the editor and show an inline, assertively announced panel notice (not a modal): *"Controlling OkuFlow needs its own conversation."* with two buttons — **Start control conversation** (creates a new `agent` conversation, selects it, submits the preserved prompt) and **Send without control** (today's tool-free/coding submit into the selected conversation). Focus stays in the editor; the buttons follow it in tab order. |
| Turned off while an `agent` conversation is selected | `agent` | Same notice pattern: *Send without control* starts a new ordinary `assistant` conversation with the preserved prompt; the agent conversation stays listed, history intact, and cannot continue tool-free. |

The one-shot **Read** and **Explain** buttons never use agent
conversations and stay tool-free even when control is on; the connected
flow is their follow-up field, which routes by the table above. Coding
conversations show "control unavailable" and never contain app-control
tools. The history list badges each conversation's kind. Required
end-to-end fixture: *Explain panel → follow-up "make the board readable" →
Start control conversation → real zoom/color changes → summary*, plus the
*Send without control* and control-off rows, verifying the pre-existing
Explain/Assistant histories are byte-identical afterwards.

### 14.6 Data handling

- Results are built from allowlisted fields only (§4 `ProviderSafeDto`):
  no API keys, credential ids, Codex account data, absolute paths, symbolic
  links, serials, transport ids, ports, settings JSON, logs, raw controller /
  driver / settings / provider error text. Saved media is reported by
  relative dated file name.
- Diagnostics log tool name, tier, result code and duration only, and only in
  console-attached diagnostic launches.
- No agent log is appended to lecture notes by default. Keep a bounded,
  pixel-free in-memory action transcript available in the control panel;
  disk diagnostics retain today's explicit diagnostic-launch rule.

### 14.7 Threat checklist

Prompt injection through camera content; runaway loops and flashing;
duplicate deliveries; late completions after cancel; user/agent races; modal
re-entrancy; aggregate payloads killing the Codex child; MCP servers or tool
providers started by `thread/start`; `imageView`/execution items on agent
turns; a coding thread gaining camera control; recording without consent;
leakage through device labels or error strings; presets that start periodic
frame upload; settings-file churn.

## 15. Accessibility

- **Announcements are batched.** Per-call progress goes to the action
  transcript and a coalesced live region (`SetLiveTextCoalesced`); one polite
  summary is announced at the end of the turn ("Assistant changed: zoom 3×,
  Whiteboard mode. Press Ctrl+Shift+U to undo."). Assertive announcements are
  reserved for recording start/stop, approval requests and failures. Because
  `SetLiveText` suppresses identical text for 5 s, approval requests use
  distinct text per request.
- Mode changes announce in Simple and Advanced and with the UI hidden, through
  a persistent accessible owner rather than the visual toast alone. That
  owner is named (D3-M13): a new persistent `QLabel`
  (`agentLiveRegionLabel_`, accessible name "Assistant control status")
  owned by `MainWindow` itself — never by the hideable chrome — so it stays
  in the accessibility tree in Simple, Advanced and hidden-UI states;
  `SetLiveText` supports only `QLabel`/`QAbstractButton` (verified in
  `live_status_text.cpp`), so the Announcer writes only to this label (and
  mirrors text into the panel transcript as plain text).
- **One panic command**: `Ctrl+Shift+U` (implementation default, §22)
  stops the running context and undoes its reversible changes (or the
  retained last journal, §9.5); it works with the UI hidden and does not
  change Escape's existing meanings (clear draft, return focus, cancel
  drag). The panel keeps a visible Stop button. It is application-scoped,
  so it cannot fire while the user is in an external MCP client: for
  external control the documented panic paths — stated in the lease
  announcement and the External MCP settings section — are **disconnecting
  the MCP client** and the in-app *Revoke lease* button; both retire the
  lease and stop its owned recording (§9.1). No desktop-global keyboard
  hook is added.
- The panel shows "Assistant is adjusting the view…" while calls run and a
  plain-language action transcript readable by screen readers.
- No focus movement, no window raising, no dialogs from tool calls.
- No automatic speech (existing contract). Summaries remain available through
  the existing manual Read Aloud action.
- Minimum intervals on color/brightness/contrast changes; no flashing.
- All new strings follow the catalog, parity script and translation-review
  policy for 25 languages; `docs/localization.md` records them.

## 16. Phased implementation

Every phase updates `docs/code_reference.md`, `docs/developer_guide.md`,
`docs/README.md`, `docs/hardcoded_paths.md` (limits/budgets), `README.md`
(provider/privacy table), `CHANGELOG.md`, `tests/README.md`, module READMEs,
`docs/ui_modes_design.md` and plan 23's tool-policy text where behavior
changes, and runs the tracked matrix (`scripts/agent_build.bat`; desktop-free
work under `OKUFLOW_AGENT_EXCLUDE_GUI=1` is reported as partial validation).

### Phase 0 — Protocol facts and isolation (desktop-free)

1. Commit a minimal Codex schema fixture (both bundles' relevant fields) and
   the pinned restricted profile for 0.160.1. A gate compares them with the
   installed CLI when present; when absent it reports *compatibility
   unverified*, never a pass.
2. Restricted launch profile with fake-process tests: `experimentalApi`,
   feature list parsing, MCP preflight, non-bare-name failure, lifecycle
   fail-closed, item allow-list.
3. Owner-gated live probe (no camera, no GUI, explicit session): `okuflow_ping`
   on an ephemeral thread with and without `experimentalApi`; namespace vs
   flat functions; `deferLoading`; maximum tool wait; interrupt with a pending
   call; call before the `turn/start` reply; `inputImage` data-URL
   acceptance; echoed bytes in `item/completed`, `turn/completed`,
   `thread/resume` and history reads; whether resume restores dynamic tools;
   tool-selection accuracy of the default model on the first catalog.
4. Optional owner probe of tool calling on local OpenAI-compatible servers.

### Phase 1A — Control core and shared safeguards, without an LLM

Named production entry points (§6.2) for photo, recording (exact-session
stop), preset, rotation, zoom and colors; field transitions with no-op
detection; the persistence transaction (§6.3); the assistant-isolation
split (§12.3); `ControlRegistry`, `AppControlService`,
`IAppControlTarget`, `StateModel` with revisions/epochs, event ring,
`OperationTracker`, `AgentChangeJournal`, `ConsentGate`, `ProviderSafeDto`,
rate limits and budgets (§9.4); preset validation with the §12.3 inventory;
atomic color recipes (§12.4). **All provider-independent safeguards land
here, before any live adapter (A3-08):** `ControlArbiter`/`ControlContext`
(§4.1), `ApprovalService` with the approval row and operation receipts
(§14.2), master switches and session capture toggles, frame-grant handling,
the Announcer with its persistent owner, action transcript, panic
Stop-and-undo including hidden UI, and the pre-turn privacy notice.
Dispatch in this phase is exercised only against the fake target and the
production window in tests — no provider adapter is wired yet.
Desktop-free tests with a fake target; GUI cases registered through
`tests/gui_cases.cmake` (serial, desktop lease) proving widgets, paired
controls, readouts, accessible names and announcements update without
focus change. Rotation-during-recording fixture.

### Phase 1B — Asynchronous camera transitions and temporary source state

The §6.2 async non-modal camera transition (start **and** shutdown
ownership, no abandonment on cancel) and the §13 three-identity source
model with its per-field persistence rules and fixtures. This phase has
its own device-loss/reconnect acceptance and lands as behavior-preserving
manual-path work before any `camera.select` tool exists; hardware
acceptance remains an owner-scheduled gate (§17).

### Phase 2 — Observations and metrics

Observation consumer slot in the presentation readback path with
`viewVersion` stamping; downscale/JPEG budget and serialized-result
assertion; metrics with `held/converging/stale`; coordinate conversion with
letterboxing. Tests: crop/transform math, size caps, stale observation,
ring contention with photo/assistant co-tenants, Fit/debug/cache remap.

### Phase 3A — Standards-based MCP bridge (catalog edition `dev`)

Implement §5.3 over the Phase 1A core (safeguards, approvals, lease UI and
panic/undo already exist there) and Phase 2 observations. External control
switch, lease issuance with the §14.3 bundle, and revoke/stop controls
precede all live dispatch. Add bridge stdout/IPC fake-target, envelope,
validating-client and conformance tests. Deliver an example stdio client
configuration and test with one independent standard MCP client against
fixtures. The bridge ships in **no** release bundle in this phase and the
`dev` edition (Tier 0–2) must not be described as a shipped API (D3-B01).
No provider API charge or owner desktop is needed for this slice.

### Phase 3B — Codex adapter and panel routing (catalog edition `dev`)

Codex-specific work only (shared safeguards are Phase 1A): restricted
child; conversation `kind`/`toolsetVersion` with kind-filtered history;
`dynamicTools` declaration; `item/tool/call` routing with exactly-once
replies and the §5.1.6 pending-start state machine; item policy; the
§14.5 panel routing table and its end-to-end fixture. Fake app-server
tests: unknown tool, wrong thread, call before turn id (first-id-wins, and
a stale-turn late call), duplicate call id with same and different
arguments, late reply after cancel, child exit with pending calls,
aggregate oversized payload, `imageView` item, MCP lifecycle event.

### Phase 4 — OpenAI-compatible adapter

`OpenAiToolLoop` and `LegacyFunctionLoop` with the §5.2 sequencing,
explicit capability test, token/round budgets. Local fake HTTP server tests
for `tool_calls` (string and object arguments), legacy `function_call`,
missing/duplicate ids, malformed JSON, no-tools endpoints and size limits.

### Phase 5 — Capture tier

Photo and recording tools wired to the Phase 1A session toggles, exact
approvals and operation receipts, for **both** the MCP bridge (lease
bundle toggles) and the embedded adapters; exact-session recording
lifecycle (§9.1) fixtures; translations and review.

### Phase 6 — Required camera control

Camera source enumeration/selection from §12.2 is required to finish the
owner's requested scope, not an optional future idea. It requires Phase 1B
asynchronous transitions and the §13 temporary-source model, plus
hardware acceptance. Then add other §12.2 tools after measured selection
accuracy. Both MCP and embedded adapters use the same catalog edition and
runtime availability. No unavailable tool is advertised as working merely
because its schema exists.

### Phase 7 — First shipped catalog (`v1`): required final completion gate

Nothing in Phases 3A–6, alone or together, may be presented as the shipped
feature. The `v1` edition and any public MCP/API claim require **all** of
the owner's original deliverables working through **both** adapters —
the local stdio MCP server driven by an independent MCP client, and the
embedded Explain/Assistant panel (§14.5 routing):

- zoom and pan, quick-mode switching, display colors including
  grayscale and both high-contrast black-and-white intents,
  `image.auto_adjust`;
- camera **selection** (temporary, §13) and **rotation**;
- **photo** capture and **video recording start/stop** with truthful
  terminal states;
- viewport observation under frame grants, with approvals, user-wins,
  Stop-and-undo (including hidden UI) and announcements exercised in both
  adapters.

The phase also registers clean nested-bundle and MSI inventory, signs and
hashes `app/okuflow_mcp.exe`, completes the licensing/docs updates listed
in this section's preamble, and passes the §17
owner acceptance. A prototype missing any row above remains `dev` and is
described as such everywhere, including release notes.

## 17. Validation

- Desktop-free: MCP stdio/IPC/conformance tests (§5.3), registry/schema, tiers and grants, idempotency reservations,
  no-op detection, rate limits, error codes, provider-safe DTOs, coordinate
  transforms, payload invariants, fake Codex and fake HTTP flows.
- GUI-labeled: production `MainWindow` driven only through
  `AppControlService`; run serially on the GitHub Windows VM workflow or an
  explicitly free desktop session — never by taking over the owner's desktop.
- No live provider calls and no camera in CI. Skipped schema checks are
  reported as unverified.
- Owner acceptance (explicit session, later): real camera, real Codex and a
  local endpoint, NVDA and Narrator, approval row, recording grant and stop
  rules, injection attempts written on a whiteboard, panic command with the
  UI hidden.

## 18. Risks and residual risks

| Risk | Mitigation | Residual |
|---|---|---|
| Experimental `dynamicTools` changes or disappears | Pinned profile + schema fixture; runtime acceptance check; ordinary Assistant unaffected | Agent control may be unavailable after a Codex update. |
| MCP servers/providers started by `thread/start` | Feature disables, bare-key MCP preflight, fail-closed lifecycle detection | Detection is after start; a server may already have run once. |
| Reactive item policy | Authorization only at `item/tool/call`; allow-list interrupts and disables | An unexpected item can start before interruption (plan 23). |
| Aggregate payload > 2 MiB | Per-image and per-turn budgets, serialized assertion, kind-filtered history with no agent restart-resume in v1 | Echo multiplicity unverified until Phase 0. |
| Prompt injection | App-side grants; honest boundary (§14.2) | Injection can use capabilities the user has granted. |
| User/agent races | Field revisions, app-side `stale_state`, user-activity pause | — |
| Whole-config resets | Field transitions with no-op detection | — |
| Periodic frame upload via presets | Preset validation | — |
| Device/serial/error leakage | Generated labels, allowlisted DTOs | — |
| OpenAI-compatible server variance | Explicit test, strict sequencing, legacy adapter | Some servers will not work. |
| Low-reasoning model misuse | Small catalog, small results, measured accuracy before growth | — |
| External MCP client invents user intent | Explicit app-owned connection approval and bounded non-renewable lease | Client may use any capability currently granted within the lease; same-account malicious code is outside the isolation guarantee. |

## 19. Next implementation steps

The next slice is desktop-free and needs no camera, phone, provider call
or owner desktop:

1. Phase 0.1–0.2: schema fixture, pinned restricted profile and fake-process
   isolation tests (desktop-free).
2. Phase 1A entry points (photo, exact-session recording, preset, rotation,
   zoom, colors), field transitions, persistence transaction and the
   assistant-isolation split, with the existing GUI tests unchanged in
   behavior (GUI runs on the GitHub VM).
3. Phase 1A registry/service/state/journal/consent/approval core with the
   fake-target suite, including the §12.3 preset inventory and §12.4
   recipe fixtures.
4. Phase 2 observation/metrics provenance fixtures (transform math, size
   caps, stale rules) against fakes.
5. Standards-based MCP bridge and fixture interoperability (Phase 3A,
   edition `dev`) with the same core; no Codex dynamic-tool dependency.
6. Phase 1B asynchronous camera transitions behind the fake capture
   target; real-device acceptance stays owner-scheduled.
7. Owner-gated Phase 0.3 live probe before any Codex adapter code relies on
   dynamic-tool behavior.

## 20. Superseded Draft 1 positions

Recorded so later readers know what changed: `dynamicTools` "in the stable
schema" (wrong); one-step snapshot undo; generic binding recipe; absolute
setters "naturally idempotent"; observation of `scene`/`original` under the
attach checkbox; modal consent during a tool call; agent recording stop
without a grant; "black and white" defaulting to `grayscale`; a ~30-tool first
catalog; storing invisible effects in raw passthrough; automatic endpoint
probes on settings change; strict JSON-action fallback; friendly device names
forwarded as-is. Draft 2's removal of the MCP facade is itself superseded:
the owner explicitly requested a real MCP API, now specified in §5.3.
Draft 3 positions superseded by Draft 4: approval Allow "performs the action
and records the grant" (now exact single-use approval, no grant created,
§14.2); the 120-second lease (now 300 s absolute / 60 s idle with a grant
bundle, §5.3/§14.3); rotation as presentation-only (§8.1); `listChanged:true`
and operation retrieval "when expanding" (now static list plus
`app.operation.get` in the first catalog, §5.3/§8.5); Phase 3A as a shipping
acceptance point (now edition `dev`, with the Phase 7 `v1` gate, §16);
shared safeguards landing with the Codex phase (now Phase 1A, before any
live adapter).

## 21. Review record

Two independent read-only reviews of Draft 1 ran on 2026-10-14 through T3
delegated tasks. Neither modified files, built, ran tests, launched the app or
touched devices.

| Reviewer | Task id | Verdict |
|---|---|---|
| GPT-6-Astra, `reasoningEffort=xhigh` (provider `codex`) | `node:delegated-task:command%3Amcp%3A5c85b8e9-9f0f-4525-a952-4218856c5198%3Adelegate-task%3Aokuflow-plan52-review-astra-r1` | Not approvable unchanged: 3 blockers, 13 major, 2 minor (R01–R18). |
| Muse Wonder Full, `reasoningEffort=xhigh` (provider `muse`) | `node:delegated-task:command%3Amcp%3A5c85b8e9-9f0f-4525-a952-4218856c5198%3Adelegate-task%3Aokuflow-plan52-review-muse-r1` | Revise before implementation: 5 blockers, 9 major, 3 minor, 1 nit (F-01–F-18). |

Parent verification: the stable/experimental `dynamicTools` difference
(JSON parse), the shared present-readback request id and back-buffer source,
the serial fallback in phone labels, the modal camera-failure warning and the
thread-item type list were re-checked in source/schema before acceptance. The
`preset-scene-explain` periodic-upload risk was found during reconciliation.

| Topic | Astra | Muse | Resolution |
|---|---|---|---|
| `dynamicTools` experimental-only | R01 | F-01 | Accepted (§3.2, §5.1). |
| Consent vs injection boundary | R02 | F-04, F-05 | Accepted: scoped grants, viewport-only observations, honest boundary (§11.4, §14.2). |
| Hardening gaps | R03 (`code_mode_host`, item types) | F-08 (non-bare names, pinned profile) | Accepted (§5.1.2, §5.1.7). |
| Payload bound | R04 (aggregate, history) | F-07 (serialized result) | Accepted (§5.1.8). |
| Process ownership | R05: dedicated restricted child + arbiter | F-02: one hardened child for all non-coding turns | Draft 3 selects dedicated agent child + shared arbiter (§5.1.1), preserving existing reading/history. |
| Forbidden-item semantics | — | F-06 | Accepted (§5.1.6–7). |
| Undo | R06 | F-11 | Accepted (§9.5). |
| Binding recipe / idempotency | R07 | F-10 | Accepted (§6.3, §9.3). |
| Presets bypass policy | R08 | — | Accepted and extended to periodic Scene Explain (§12.3). |
| Camera transitions | R09 | F-15 | Accepted (§6.2, §10). |
| Recording contract | R10 | F-15 | Accepted (§6.2, §12.5). |
| Durable operations | R11 | — | Accepted (§9.3). |
| Observation provenance | R12 | F-12 | Accepted (§8.1, §11). |
| Leakage via labels/errors | R13 | F-16 | Accepted (§7, §14.6). |
| HTTP loop sequencing | R14 | F-14 | Accepted (§5.2). |
| Color semantics | R15 | F-17 | Accepted (§12.4). |
| Focus/Escape/hidden UI | R16 | F-13 | Accepted (§6.4, §15). |
| Catalog size | — | F-09 | Accepted (§12.1–12.2). |
| Modal consent | — | F-03 | Accepted (§6.1, §14.2). |
| Contract gaps / tiers | R17 | — | Accepted (§8.3, §12.2). |
| Sequencing / docs / gates | R18 | F-18 | Accepted (§16). |
| JSON fallback and MCP facade | — | Draft 2 removal | JSON fallback remains out; parent corrected MCP removal to satisfy owner scope (§5.3). |

**Remaining disagreements**

1. *Process ownership.* Draft 3 selects Astra's dedicated agent child with the
   runtime-wide arbiter. Muse's shared hardened child could reduce process
   count but changes existing Read/Explain conversations and failure behavior;
   defer such consolidation until separately validated.
2. *Schema gate when the CLI is absent.* Muse suggested failing the
   agent-control release check; Astra required reporting "unverified". The
   plan reports unverified and relies on the runtime acceptance check, because
   release machines need not have Codex installed.
3. *Agent recording stop.* Muse proposed binding stop to the turn's prompt;
   Astra noted that keyword or model judgment is not trustworthy. The plan
   uses session capture toggles/lease bundle for agent-started recordings
   plus exact approvals for recordings the agent did not start (§14.2).

**Coverage limits.** Neither reviewer could verify runtime Codex behavior
(feature defaults beyond the parent's read-only `features list`, echo sizes,
tool timeouts, resume behavior), local OpenAI-compatible server behavior, or
rendered accessibility. Those are Phase 0 and owner-acceptance items. No
Draft 1 review incorporation does not approve Draft 3. The requested Opus
revision task `okuflow-opus-mcp-plan-scope-final-20261014-r2` failed before
editing with `Claude API rate limit reached. Try again later.` The parent
authored Draft 3, which then received the fresh reviews recorded below.
No API code or live provider/device acceptance has been performed.

### Draft 3 review disposition (Draft 4)

Two fresh read-only reviews of Draft 3 ran on 2026-10-14, both *revise
before implementation*; neither modified files or ran anything live:

| Reviewer | Task id | Verdict |
|---|---|---|
| GPT-6-Astra, `reasoningEffort=xhigh` | `node:delegated-task:command%3Amcp%3Adbd8c893-5644-46a7-9f63-9cada8124d01%3Adelegate-task%3Aokuflow-plan52-review-astra-r2-draft3` | 1 blocker, 7 major (A3-01…A3-08). |
| Muse Wonder Full, `reasoningEffort=xhigh` | `node:delegated-task:command%3Amcp%3Adbd8c893-5644-46a7-9f63-9cada8124d01%3Adelegate-task%3Aokuflow-plan52-review-muse-r2-draft3` | 4 blockers, 13 major, 6 minor (D3-B01…B04, M01…M13, m01…m06). |

Every finding was re-verified against source before acceptance (rotation
side effects in `OnRotationSelectionChanged`; active-index copy in
`SavePersistentSettings`; manual-pick side effects in
`OnCameraSelectionChanged` and the global format read in
`app_pipeline_runtime.cpp`; scheme stops/foreground selection in
`color_schemes.cpp` and the text-mask composite in `cuda_interop.cpp`;
the `SetModes` abort path; `ApplyConfigToUI`'s field set including its
non-application of profile `rotationQuarterTurns`; `SubmitAssistantPromptText`
as the common panel funnel; unqualified `RecordingManager::Stop`;
`SetLiveText`'s `QLabel`/`QAbstractButton` limit; settings version 16).
One citation was corrected, not rejected: A3-06 named
`AssistiveRuntime::SetModes`; the production preset entry is
`AssistiveFeatureManager::SetModes(vlm, overlay)` forwarding
`vlm && overlay` to it, so an overlay-off preset aborts too (§12.3).

| Id(s) | Disposition in Draft 4 |
|---|---|
| A3-01 | Accepted. Exact single-use approval records, atomic consume-before-dispatch, target/revision revalidation, operation receipt, no grant on Allow, deny-digest blocking (§14.2). |
| A3-02 + D3-M06 | Accepted, combined. Recording-lifetime table and exact-session cleanup (§9.1, §6.2); lease becomes 300 s absolute / 60 s idle with a 30 s minimum-remaining rule, replacement and queued-expiry semantics, no manual conversion in v1 (§5.3). |
| A3-03 + D3-M08 | Accepted, combined. Three-identity source model with per-field save/restore/persist rules, failure and *Switch back* semantics and the named fixtures (§13). |
| A3-04 | Accepted. Rotation bumps `processingVersion` + `geometryEpoch`; observation/metrics wait for newly rotated pixels, never a relabeled cache (§8.1, §11.2). |
| A3-05 | Accepted. Atomic recipes pin master/binarization/two-color/polarity/hard-B&W/scheme per intent, with composed-settings tests from Text-Clarity on/off (§12.4). |
| A3-06 | Accepted with the citation correction above. `SetModes` split plus agent-preset reject-on-change for `vlmAssistEnabled`/`assistiveOverlayEnabled` (§12.3). (Both of Astra's alternatives are applied together.) |
| A3-07 | Accepted. Explicit panel routing table at `SubmitAssistantPromptText`, prompt preservation, no history rewriting, named end-to-end fixture (§14.5). |
| A3-08 + D3-B01 | Accepted, combined. Shared safeguards move to Phase 1A before any live adapter; catalog editions `dev`/`v1`; Phases 3A–6 make no shipped claim; Phase 7 is the required both-adapters completion gate (§5.3, §16). |
| D3-B02 | Accepted. `app.operation.get`, `sinceEvent` cursor on `app.state.get`, `operationId` on `app.wait_for`, 15-minute terminal retention — all in the first catalog (§8.5, §12.1). |
| D3-B03 | Accepted. `outputSchema` = success\|pending union; errors as text-only `isError` with no `structuredContent`; image never in structured content; validating-client fixture (§5.3, §8.3). |
| D3-B04 | Accepted. Lease-issuance UI with per-lease bundle toggles and counts, expiring with the lease; no mid-lease widening (§14.3). |
| D3-M01 | Accepted. Server always responds `2025-11-25`; `ping` legal before `initialized`; premature/duplicate-initialize codes pinned (§5.3). |
| D3-M02 | Accepted. Envelope table: cursor, `_meta`/`progressToken`, task-augmented calls, batches, null/reused ids, oversized lines (§5.3). |
| D3-M03 | Accepted. Static list + `listChanged:false`; bridge serves `tools/list` from its linked registry; edition mismatch ⇒ `unavailable` (§5.3). |
| D3-M04 | Accepted; chose the in-memory pairing-secret alternative. Four named epochs; same-bridge pipe reconnect preserves an unexpired lease; operation-status carve-out across leases (§5.3). |
| D3-M05 | Accepted. `ControlContext` variant (embedded turn / external lease); Read/Explain explicitly allowed during a lease; one mutation owner (§4.1). |
| D3-M07 | Accepted in the next-turn form. Frame Allow only activates a grant (same turn if still running, else next turn); `grant_changed` wait condition added; perform-on-Allow kept only for one-shot Tier 2/3 actions; no image into a finished turn (§14.2, §11.4). |
| D3-M09 | Accepted. At most one save per tool call (safety markers excepted, counted); crash semantics stated (applied values retained, journal lost) (§6.3). |
| D3-M10 | Accepted. Baseline updated by exactly state/results/operation/list reads; composite all-or-nothing prevalidation with honest `partial`; full preset field inventory including legacy rotation; presets count as one composite token (§8.4, §12.3, §9.4). |
| D3-M11 | Accepted. Pending-start state machine (one outstanding start, first turn id wins, stale-turn calls never bind); no agent restart-resume in v1 with kind-filtered history, so no dependency on `thread/items/list` (§5.1.4, §5.1.6). |
| D3-M12 | Accepted. All MCP/IPC/queue/result budgets pinned numerically (§9.4). |
| D3-M13 | Accepted. Client-disconnect + Revoke documented as external panic paths; journal retention defined; approval shortcut named (Ctrl+Shift+A + mnemonics); Tier 0 reads exempt from `modal_active`; announcer owner named as a MainWindow `QLabel` (§6.1, §9.5, §14.2, §15). |
| D3-m01 | Accepted. Future providers renumbered to §5.4 (moved after §5.3). |
| D3-m02 | Accepted. Phase 1 split into 1A (core) / 1B (async camera); §19 lists Phase 2 explicitly (§16, §19). |
| D3-m03 | Accepted. JSON-RPC code table pinned (`-32700/-32600/-32601/-32602/-32603/-32002`) (§5.3). |
| D3-m04 | Accepted. Per-tool annotation table; `view.observe` is not `readOnlyHint:true` (§12.7). |
| D3-m05 | Accepted. Scheme enumeration lives in `mode.list` `colorSchemes` (§12.1, §12.4). |
| D3-m06 | Accepted. Settings v17 fields and startup pipe behavior (§14.4); bridge static-CRT/Qt-free, one pipe instance (§5.3); preset text sanitization bounds (§7); `already_active` ownership semantics (§12.5). |

No finding was rejected. The only alternatives not taken are recorded in
their rows (M04 pairing secret over always-fresh approval; M07 next-turn
frame grant over delivering into the waiting call, which Draft 4 forbids
for a finished turn). Incorporation here is a design disposition, not
runtime validation: everything in §22 remains unverified until its
fixture or owner acceptance runs.

## 22. Implementation defaults and unverified facts

These are concrete implementation choices, not ten permission questions for
the owner. Product authorization and development review are separate from
runtime camera/recording grants in the finished app.

| Choice | Default |
|---|---|
| Codex process | Dedicated restricted agent child; preserve existing Read/Explain child/history and serialize through the shared arbiter. |
| Master switches | Embedded Assistant control and external MCP control off until their separate first opt-ins. |
| Grants and approvals | Standing grants are UI toggles only (master switches, per-turn frame grant, session capture toggles, lease bundle); everything else Tier 2/3 uses exact single-use approvals that create no grant (§14.2). External lease: 300 s absolute / 60 s idle, bundle-scoped, no tool renewal or mid-lease widening (§14.3). |
| Stopping manual recordings | Exact approval bound to that session id only; no model-derived consent. Native Stop remains unrestricted. |
| Catalog editions | `dev` (Tier 0–2, internal, bridge unshipped) until the Phase 7 gate; `v1` only when camera/photo/video work through both MCP and embedded adapters (§16). |
| Agent history | No restart-resume of agent conversations in v1; kind-filtered index (§5.1.4). |
| Budgets | §9.4 pinned numbers for every MCP/IPC/queue/result limit. |
| Speech | Screen-reader announcements and existing manual Read Aloud; no automatic TTS. |
| Camera preference | Agent changes active source temporarily; saved explicit manual selection survives. |
| Action history | Bounded pixel-free panel transcript, no automatic changes to lecture notes. |
| Stop/undo | Visible accessible action plus application-scoped Ctrl+Shift+U, including hidden UI; never a desktop-global keyboard hook. Source search found no current Key_U binding, but rendered screen-reader/layout acceptance still needs validation. |
| Surfaces | Floating Explain/Assistant and Advanced Assistant, using an explicit agent conversation kind. Coding conversations show control unavailable and contain no app-control tools. |
| Observation | Current viewport only, bounded by source-specific frame grant; no original/full-scene egress. |

Remaining **facts to measure**, not unspecified defaults: Codex experimental
tool acceptance/resume/echo sizes/timeouts; independent MCP client conformance
at the pinned protocol revision; real asynchronous camera cancellation and
recording rotation behavior; NVDA/Narrator and translated/RTL rendering. Mark
unverified until the corresponding fixture or scheduled acceptance proves it.
