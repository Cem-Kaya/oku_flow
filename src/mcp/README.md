# Local MCP development bridge

This is first-party implementation code, not a generated MCP SDK. The console
`okuflow_mcp` target links only the Qt-free registry, original bounded JSON/
protocol code, the C++ runtime and Win32 libraries. It uses the static CRT and
does not link Qt. The app endpoint is compiled separately with the application's
Qt configuration. No third-party source or runtime dependency was introduced.

Read [the IPC and protocol contract](../../docs/mcp_control.md) and
[the current inventory](../../docs/control_inventory.md) before changing
transport behavior. The shared immutable `OKUFLOW_CONTROL_CATALOG_EDITION`
must agree across app, bridge and private fixtures. Shipping bundles use `v1`
and deployed-image policy; development catalogs remain outside bundle and MSI inventories.
The C++ and pinned official SDK fixtures verify private protocol and client
interoperability, not live-app, provider, camera, accessibility or shipping
acceptance.
