# Local MCP control (development)

Image delivery retains the native observation's original control stamp, frame
sequence and monotonic processing time through local preview, HTTP/Codex and
MCP serialization. The final delivery fence rechecks that exact record, current
modal state and frame-grant generation. Turning image permission off and back
on cannot authorize an earlier captured image. Synthetic targets may report
unknown frame sequence/time; native observations supply their encoded frame's
provenance. No new image lifetime or hardware acceptance is implied by this
internal authorization record.

Plan 52's development implementation builds `okuflow_mcp.exe`, a Qt-free `/MT`
console MCP server. It uses MCP **2025-11-25**, newline-delimited UTF-8 JSON-RPC
on stdin/stdout, and Windows named pipes to the running app. It never launches,
raises or terminates OkuFlow. Development binaries are excluded from release
bundles and MSI; `v1` still requires Plan 52's full both-adapter acceptance gate.

`OKUFLOW_CONTROL_CATALOG_EDITION` selects one immutable compiled catalog:
`dev-core`, `dev-capture`, `dev-camera` (the default), `dev-full`, or `v1`. CMake rejects
other values and applies the same index to the app, bridge and private fixtures.
`BuiltControlCatalogEdition()` is their common Qt-free factory; the pipe
handshake rejects a different app catalog instead of changing discovery at
runtime. Selecting `v1` does not clear acceptance or enable packaging.

The preserved development editions contain 14, 17 and 19 tools respectively.
`dev-full` and the still-gated `v1` contain 36 tools: the original 19 plus
`app.controls.list` and eight finite grouped get/set pairs. Their inventory
describes 94 adjustable parameters, 17 protected metadata entries and 39
workflow actions. A listed field is not proof that its native implementation,
hardware or permission is available. Runtime rows expose availability,
writability and unsupported reasons; protected entries expose no secret values.
Native-only workflow metadata is not a callable automation method. See the
[control inventory](control_inventory.md) for ownership and field semantics.

The protocol revision is deliberately pinned to the official
[lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle),
[stdio](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)
contracts. The server always answers `initialize` with `2025-11-25`, advertises
only `tools: {listChanged:false}`, and allows `ping` after initialize but before
the initialized notification. It has no shutdown method, HTTP listener,
resources, prompts, sampling, elicitation, task augmentation or progress output.

`tools/list` comes from the same compiled Qt-free `ControlRegistry` as the app
and works without OkuFlow. Authorization is independent of discovery. A present
cursor (even null/empty), unknown tool and task-augmented call are invalid
params. Metadata, including progress tokens, is accepted and discarded. Tool
argument failures are text-only common error DTOs; protocol errors use pinned
JSON-RPC codes and fixed diagnostic strings. Success/pending results contain
structured metadata and its text equivalent. JPEG observations are separate
MCP image content; errors never carry structured content or images. The bridge
validates its finite registry schema subset before IPC and before successful
structured output.

MCP annotations are advisory. Reads, viewport observation and additive photo /
recording-start operations use `destructiveHint:false`. Replacements, resets,
source changes and recording stop use `true`; grouped setters conservatively
use `true` because patches can replace saved preferences, microphone intent,
output destinations or drawing state. These hints neither grant authority nor
replace field permissions, exact approvals, revision checks or undo rules.

Grouped setters accept only their declared, nonempty field patch. Discovery
pages and optional getter `fields` filters are limited to 16 entries. Only
`assistant.preferences.get` accepts `valueKind: all | requested | saved`
(`all` is the default). A saved-only read does not refresh the current mutation
baseline. Valid 4096-codepoint text remains complete; the bounded result may
request a narrower field filter/projection using `filterRequired` and
`response_budget`, never silent truncation. Equal copies may use
`{sameAs:"requested"}` when needed to fit. Out-of-contract stored text remains
unchanged and is explicitly marked for native UI handling.

## Integration owned by the app

Create `mcp::AppIpcEndpoint` with the app's `AppControlService` on the GUI thread.
The constructor opens nothing. The owner invokes `SetEnabled(true)` only for
the explicit External MCP master switch; the persisted-on switch may listen at
startup, but restores no lease. The endpoint never changes either master.

`SetPendingChanged` notifies the app of a sanitized pending client name. The app
must show that row only in the opened control-settings section, without a
tool-triggered popup. `ApprovePending(expectedPeerId, ControlGrants)` is a GUI-only trusted UI
action that calls the service's external `Begin`. There is no approve, enable,
lease, grant or shell method in IPC. `Revoke()` ends the exact lease. Use
`ActiveLease()` and `AppControlService::ContextStatus()` for grant, budget and
remaining-time UI. `SetEnabled(false)` closes the owned listener, removes its
descriptor, retires the lease and clears pairing. App shutdown uses the same
path. The service enforces the 300-second absolute / 60-second idle lease and
30-second minimum remaining time for recording start; tools cannot renew it.

The UI captures PendingPeerId() when displaying the pending row and passes that stored id at approval. The endpoint rejects a replaced/stale peer id; reading the latest peer id only at click time would lose this protection.

The app can produce local client configuration with the absolute bridge image
path and `args: ["--instance", "<InstanceSelector()>"]`. Paths are local client
configuration only and never tool data. Development verification expects exact
sibling `oku_flow.exe`; future deployed builds must explicitly switch to the
verified sibling `OkuFlowApp.exe` and complete the nested bundle/MSI gates.
No live endpoint should be enabled without the lease-issuance UI.

`OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY=ON` selects the finite deployed-image check
against sibling `OkuFlowApp.exe`; its default is OFF for sibling `oku_flow.exe`.
This option does not promote a development catalog, enable a listener, stage a binary or
clear the `v1` release gate. The private test app adopts the matching name so
the same desktop-free contract can exercise either image policy.

Future `v1` packaging must copy the validated `/MT` bridge to
`app/okuflow_mcp.exe`, require its deployed image policy/catalog to match the
inner app, sign and hash it with both existing executables, include it in the
exact nested bundle/MSI/SBOM inventories, compare staged/published bytes to the
built image, preserve the pinned Windows runtime/license notices and validate
the clean installer guest. None of those release gates is bypassed by this
development option; development bridge binaries remain outside packages.

## Identity, pairing and lifetime

Instances use `%LOCALAPPDATA%\OkuFlow\mcp-instances\<random-128-bit>.json`.
The descriptor contains exactly protocol version, random selector and process
epoch. It contains no token, lease, camera data or pairing secret. Its selector
maps to `\\.\pipe\OkuFlow.Mcp.<selector>`; bridges read only the selected
descriptor and never scan pipes or guess another app. Creation uses a protected
owner logon-SID DACL and `CREATE_NEW`; descriptor reads reject reparse files and
files over 1024 bytes.

The single pipe instance uses first-instance ownership, the owner logon-SID
DACL, remote-client rejection and overlapped bounded transfers. Both ends hold
process/image handles and compare actual pipe-peer PID, process creation time,
same Windows session, user SID, logon SID, integrity SID, canonical image path
and image file identity against the exact sibling. Claimed client name, PID,
argv or metadata grants no trust. No impersonation or elevated helper is used.
This excludes accidental/foreign attachment, not malicious code already running
as the same account and integrity.

Four identities stay distinct: one `appEpoch` per endpoint owner's process,
random `mcpSessionEpoch` per bridge process, random `pipeEpoch` per connection,
and UI-issued lease id. The bridge retains a 256-bit random pairing secret in
memory only. Reconnect re-verifies held process identity and presents the same
secret and session epoch; the app preserves an unexpired lease only for that
same process/pair. A new bridge, app restart, expired lease or revoked lease
needs fresh UI approval. Transport loss drops outgoing frames and late
connection callbacks; a retry keeps the bridge request key and uses the shared
service reservation, never a JSON-RPC id as app authority. EOF sends an explicit
disconnect, and a dead held bridge process is detected without terminating it.

Workers only parse/authenticate/frame messages. GUI dispatch, cancellation,
lease checks and shared target operations use queued invocation. The bounded
writer runs off the GUI thread and performs a queued final GUI authorization
fence before image egress; revoke, source/geometry change, expiry or disconnect
removes that frame. MCP cancellation suppresses the later JSON-RPC response;
it is not undo and does not erase durable accepted capture operations.

## Bounds and validation

Inbound/outbound MCP lines: 1 MiB including newline. Three malformed/oversized
lines close the bridge. JSON nesting: 32; nodes: 65536; duplicate keys, malformed
UTF-8 and invalid Unicode escapes are rejected. Arguments: 16 KiB; structured
metadata: 32 KiB; JPEG: 192 KiB before base64; IPC frames: 512 KiB. Bridge pending
calls: eight. IPC outgoing frames: ten (eight results plus control messages).
String request ids are limited to 256 bytes. Stdout writes have a two-second deadline plus a one-second cancellation bound; stalled output closes the bridge without blocking the app. Native I/O permits eight slots with at most 512 KiB each; an unconfirmed cancellation retains its owned buffer/OVERLAPPED for process lifetime instead of freeing memory still used by the kernel.
Private transfers have two-second write/hello deadlines, 6.5-second response
deadlines, eight-second peer frame deadlines and two-second idle heartbeats.
Cancellation and stop are checked every 20 ms while an overlapped transfer is
pending. Final image authorization has a one-second bound and cannot block GUI.

`mcp_bridge_contract` runs a QCoreApplication fixture only: protocol lifecycle,
schemas, errors, budgets, cancellation/reused ids, split UTF-8 stdio, absent app,
private pipe peer/image verification and a private fake app/real bridge flow
(no implicit grant → fixture UI approval → state → revocation → EOF cleanup).
It opens no native window, camera, adb server, browser or provider, and never
touches a user's app. It is registered in the tracked CPU/CUDA matrices; these
fixture results do not establish independent standard MCP-client interoperability,
cross-account token/session rejection, actual UI grants, real-camera quality or
the full release gate. Those remain separate Plan 52 acceptance work.

The separate [pinned SDK fixture](../tests/mcp-sdk/README.md) uses the official
TypeScript MCP SDK 1.32.1 against the actual stdio bridge and private app. It
checks negotiation, the matching compiled catalog, schema-valid structured
results, text-only errors, explicit lease approval/revocation, bounded zoom,
separate synthetic JPEG image content and EOF cleanup. This establishes
independent client interoperability for that private fixture, not live-app,
provider, device, accessibility or shipping acceptance.
## Expanded private acceptance and packaging preparation

The independent SDK fixture now exercises all 19 tools in 14 scenario groups
through actual stdio, IPC and service enforcement. Its persistent finite
synthetic target uses the real OperationTracker for explicit pending/terminal
capture states. Evidence directories stay in LocalAppData and contain JUnit,
JSON receipts and logs (`--evidence-dir`). Both development and deployed peer
identity policies passed; these are still `dev-camera` builds.

This establishes neither actual camera selection nor media/file output, native
approval/accessibility, live-provider behavior or full shipping acceptance.
Native user-wins and frame/capture/source denials are simulated and explicit.
`--build-info` starts no endpoint and lets future packaging reject a development
catalog or identity policy. Release preparation includes signing/hash/import
checks and mandatory inventories; no V1 package has been produced.

The separate `scripts/test_mcp_native_sdk.mjs` is prepared for the opt-in
`native_app_control_fixture` and exact sibling bridge under `tests/native-control`.
It requires an explicit real-target marker and exercises the production
`AppControlTarget`, service and IPC against private settings with Qt offscreen.
It skips application platform/device/provider bootstrap and does not open a
camera or owner's instance. Its intended CPU subset covers all eight grouped
reads, bounded discovery, native zoom/save handlers, user-wins,
single-use annotation approval, grant denial, undo/lease retirement and normal
EOF cleanup. On 2026-10-15 it passed all 9 scenarios in three consecutive
runs with SDK 1.32.1 (evidence under LocalAppData `native-sdk-evidence-final-*`),
after an ungranted external `view.observe`/capture call was corrected to
report `denied_by_policy` before any availability check (§14.3).
Cold image effects remain explicitly unavailable without CUDA; CPU fixture
availability is never fabricated to obtain a passing mutation.
Its results must stay separate from the synthetic fixture above, and neither
proves all 94 native effects, real camera/media, live provider, rendered approval
UI, accessibility, the embedded assistant adapter or shipping acceptance.

Operation-state waits compare the complete public status projection at admission
and each poll. Unchanged deferred preference metadata is not a transition.
