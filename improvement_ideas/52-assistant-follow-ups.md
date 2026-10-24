# Assistant follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24.

MCP V1, the typed shared control inventory, Codex and OpenAI-compatible HTTP
adapters, automatic Send preparation, native UCT controls, turn-scoped current
view access, independent external-client permissions and label updates ship
in this release. The old core/capture/camera phase ladder is completed work,
not a missing implementation list.

## Remaining provider feature

**Claude through the Anthropic CLI** is still a future integration, as stated
in the public roadmap. No native Anthropic provider implementation is present
in this baseline. It would need explicit account/configuration handling,
streaming/cancellation, observations and the same bounded native control
authority. An OpenAI-compatible endpoint is an existing option, not proof of
a dedicated Anthropic CLI adapter.

## Follow-up rule for future controls

As PDF sources, bookmarks or camera hardware controls are added, expose their
actual supported state through the same typed inventory and native validation
paths. Keep requested intent distinct from readiness/effective results; do
not solve model discovery by bypassing permissions or inventing success.

Current contracts:
[MCP](../docs/mcp_control.md),
[control inventory](../docs/control_inventory.md),
[Assistant UI](../docs/agent_control_ui.md) and
[Send/UCT runtime](../docs/assistant_control_runtime_2026_10_24.md).

No automatic inference, global desktop control, new image grant or provider
switch is authorized by this backlog entry.
