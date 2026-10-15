# common module

This module contains shared processing and output helpers that are reused across the app.

`ComputeZoomAnchorCenter` shares the real cropped view geometry with native
wheel/keyboard zoom. Its desktop-free fixture verifies anchor position across
landscape/portrait/aspect changes and clamped old centers, rejecting malformed
points. `AsyncCleanup` owns detached driver-resource closures and retains
failed cleanup resources; camera callers must acknowledge release only after
its completed status. These development changes still require hardware gates.

Current contents:
- shared annotation rendering preserves caller clip regions, including the
  live Assistant exclusion, while restricting ink to the scene destination
- on-demand Read transcribes through the vision provider, using Luna 6/low as
  the Codex default; Explain retains its scene prompt. Both share bounded
  image preparation, busy/cancel handling, notes, and manual Read Aloud.
  Read always opens a new ephemeral Codex thread; Explain continues one stored
  thread (replaced transparently if it cannot be resumed). OpenAI-compatible
  Explain resends up to four recent answers as text, kept in memory only.
  Provider, URL, model, API-key changes and explicit resets clear that history
  and revoke admission of pending answers, so late replies cannot restore it.
- asynchronous assistive analysis in `assistive_runtime.cpp`, including shared
  response instructions for Codex and OpenAI-compatible requests plus
  incrementally maintained HTML lecture notes; ordered bounded background
  storage handles note creation, image encoding, appends, flushes, and closing
  tags with asynchronous failure reporting
- the permission-aware native `codex app-server` JSON-RPC transport in
  `codex_app_server_client.cpp`, with full image-model catalog/reasoning
  metadata forwarding, a shared built-in identity prompt, guarded
  user-configured response preferences, generation-owned turn opening, and
  identity-checked cancellation/notifications. Initialization and transport
  failures retire the failed child before a fresh request can retry
- CPU frame conversion and image effects in `image_processing.cpp`
- stage orchestration and debug compositing in `frame_pipeline.cpp`
  (the unused `ResampleToFill` path and its private dimension cache were removed;
  presentation uses the canonical viewport transform)
- microphone PCM feeds AAC recording only; browser speech processing is separate
  from the desktop media pipeline
- live AV1/H.264 Media Foundation sink-writer recording in `media_writer.cpp`;
  the app owns two instances for synchronized original/processed output. An
  automatic stop retains its terminal result and original failure; an early
  encoder sample release cannot recycle a D3D11 source texture until its
  independent GPU read query completes. Nonempty failed output is preserved
- a `LoadLibrary`/`GetProcAddress`-only NVIDIA Video Effects SuperRes adapter in
  `maxine_superres.cpp`, with validated pitched crop views, synchronous
  inference to prevent stale-frame ghosting, and clean NIS fallback when the
  optional runtime is absent, fails, or exceeds its measured 24 ms latency
  target; the application may explicitly override latency-only fallback
- canonical camera-to-viewport geometry in `view_transform.cpp`; D3D12,
  passthrough presentation, pointer mapping, and cached SuperRes ROI checks
  derive from the same uniform transform

If additional cross-cutting helpers land later, keep them small and explicitly reusable rather than turning this directory into a misc bucket.

CPU quarter-turn rotation keeps its two BGRA vector allocations between
frames, swaps the completed destination into the raw stage, and overwrites
all destination pixels. No bulk zero-fill is required for an equal-size
frame. The byte-exact regression includes padded source rows, changing
non-square dimensions, alpha, negative turns and no-op turns. Peak capacities
remain retained; this does not establish an application FPS improvement.

The dedicated agent profile is isolated from the ordinary Codex client. Its
offline version/feature gate produces no live provider traffic. See plan 52;
agent controls and MCP remain under development, not public release capability.
Profile-driven periodic analysis changes preserve user-requested reply ownership
through `SetPeriodicAnalysisEnabled`, while explicit Stop stays unconditional.

`http_agent_client.cpp` supplies the isolated OpenAI-compatible agent loop. It
requires an explicit frame-free capability test and app-issued control context;
editing endpoint/model/key never sends inference. Modern tools and legacy functions
use distinct transcript roles. Every round is prevalidated before sequential core
submissions, and observation images follow all tool replies and are not resent.
Network errors expose fixed codes; API keys stay in Authorization headers. Private
loopback HTTP fixtures exercise the adapter without a provider, camera or desktop.

Shared chat presentation lives in `visible_chat_session.cpp`, independent of
widgets and credentials. The runtime admits generation-tagged Explain/followup
turns, replaces streamed replies once, and keeps Read/periodic analysis outside
the transcript. Completed text context can bridge isolated backend segments;
historical frames and control grants cannot cross that bridge. Explicit
attachments remain turn-scoped. Notes/media ownership remains runtime-owned.

`visible_chat_session_store.cpp` persists bounded mixed logical chat text via
atomic QSaveFile records in an owned app-data directory. Restart restores the
full shared transcript and draft without backend IDs, images or authority.
Legacy provider histories remain separate; pending streamed rows are not saved
as completed answers. Failed/interrupted rows stay outside followup context.

The scoped scene lane in AssistiveRuntime is independent from shared chat and
native Read/periodic ownership. Its app-issued frame guard fences actual HTTP
submission, delayed Codex turn/start and terminal presentation/notes. It stays
tool-free, temporary, silent and bounded by the requesting core context.
