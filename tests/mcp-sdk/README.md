# Pinned independent MCP client fixture

The optional `scripts/test_mcp_sdk.mjs` runs the official TypeScript SDK 1.32.1
against the private QCoreApplication test app and actual stdio bridge. It never
uses a user's OkuFlow instance, providers, camera, phone, browser or native GUI.
The fixture directory must end in `mcp-fixture` and both private executables must
be siblings. Input approval commands simulate an explicit trusted UI action and
pin the peer id displayed by the fake app.

`package-lock.json` pins all 95 development packages; the reviewed SDK integrity
is `sha512-2DdE+SJDtzLEEWzY1ZjY7Q+VcPhcV1KisD3zI4u0XZyktsjHum1mwbMI+JaulUBi2OZk+KJAi2uPXzxichPkdw==`.
The exact package's MIT license is retained as `LICENSE` (Anthropic 2024).
The locked transitive declarations are MIT, ISC, BSD-2-Clause or BSD-3-Clause.
Upstream: <https://github.com/modelcontextprotocol/typescript-sdk>.
This dev-only client and its dependencies are not linked, bundled or deployed;
their complete package notices stay with the private npm install.

Install outside the checkout/Drive, with npm lifecycle scripts disabled:

```powershell
$sdkRoot = Join-Path $env:LOCALAPPDATA 'OkuFlow/mcp-sdk-fixture-1.32.1'
New-Item -ItemType Directory -Force $sdkRoot | Out-Null
Copy-Item tests/mcp-sdk/package*.json $sdkRoot
npm.cmd ci --prefix $sdkRoot --ignore-scripts --no-audit --no-fund
$env:PATH = 'C:/Qt/6.12.0/msvc2022_64/bin;' + $env:PATH
node scripts/test_mcp_sdk.mjs --sdk-root $sdkRoot --app-fixture <build>/tests/mcp-fixture/oku_flow.exe --bridge <build>/tests/mcp-fixture/okuflow_mcp.exe
```

Build `mcp_tests` first through the normal test configuration. The client checks
actual initialization, catalog/output-schema validation, text-only errors,
unapproved/approved/revoked lease behavior, bounded zoom, synthetic JPEG image
content and EOF cleanup. It requires the app's compiled edition to match the
bridge; the default `dev-camera` catalog advertises 19 tools. All tool effects
execute against the finite synthetic target, never production media. For
`OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY=ON`, pass the fixture's sibling `OkuFlowApp.exe`
instead of `oku_flow.exe`; this tests the deployed image-name policy without
staging a release.
SDK fixture interoperability does not establish live-app/provider/device,
accessibility, cross-account token/session or release acceptance.

The historical 19-tool script covered 14 scenario groups. The current expanded
`dev-full`/`v1` fixture asserts 43 tools, 98 parameters, 17 protected entries and
46 actions, including its paged metadata checks. It retains a
persistent synthetic target and real service/operation tracking. Use
`--evidence-dir` under LocalAppData for JUnit, JSON scenario receipts and logs.
It verifies source/frames/capture denial, user-wins, durable photo/recording
states, cancellation and revoke ownership as well as basic stdio/IPC/schema
interoperability. The target produces synthetic camera handles, JPEG and
operation outcomes; it performs no physical capture, real file write, provider
inference or native approval UI. A synthetic first-frame event cannot establish
production camera switching or release acceptance.

`scripts/test_mcp_native_sdk.mjs` is a separate opt-in harness for the actual
`AppControlTarget` fixture built with `OKUFLOW_ENABLE_NATIVE_CONTROL_FIXTURE=ON`
and the `dev-full` or `v1` catalog. Both executables must be exact siblings under
`tests/native-control` in LocalAppData; the app must report `realTarget:true`.
It launches only that fixture with `--stdio-fixture`, Qt offscreen, private
settings/output and platform/device/provider bootstrap disabled. It uses the
same pinned development SDK; no additional dependency is installed or deployed.

```powershell
node scripts/test_mcp_native_sdk.mjs --sdk-root $sdkRoot --app-fixture <build>/tests/native-control/oku_flow.exe --bridge <build>/tests/native-control/okuflow_mcp.exe --evidence-dir $env:LOCALAPPDATA/OkuFlow/mcp-native-sdk-evidence
```

For deployed identity policy, pass `OkuFlowApp.exe` instead of `oku_flow.exe`.
The fixture reports its actual compiled catalog, which must match the bridge.
Earlier nine-scenario native runs passed the older catalog; they do not validate
the expanded V1 harness repairs. Its separate
`mcp-native-sdk-junit.xml` and JSON receipt identify the actual production target,
offscreen CPU subset and omitted hardware/provider/full-effects acceptance.
Zoom and private saved-state checks, native user-wins, exact annotation
approval and terminal undo/lease checks must actually pass before claiming that
subset. Existing synthetic SDK evidence is never relabeled as native acceptance.
