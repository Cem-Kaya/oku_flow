# common module

This module contains shared processing and output helpers that are reused across the app.

Current contents:
- shared annotation rendering preserves caller clip regions, including the
  live Assistant exclusion, while restricting ink to the scene destination
- on-demand Read transcribes through the vision provider, using Luna/low as
  the Codex default; Explain retains its scene prompt. Both share bounded
  image preparation, busy/cancel handling, notes, and manual Read Aloud
- asynchronous assistive analysis in `assistive_runtime.cpp`, including shared
  response instructions for Codex and OpenAI-compatible requests plus
  incrementally maintained HTML lecture notes; ordered bounded background
  storage handles note creation, image encoding, appends, flushes, and closing
  tags with asynchronous failure reporting
- the permission-aware native `codex app-server` JSON-RPC transport in
  `codex_app_server_client.cpp`, with full image-model catalog/reasoning
  metadata forwarding, a shared built-in identity prompt, and guarded
  user-configured response preferences
- CPU frame conversion and image effects in `image_processing.cpp`
- stage orchestration and debug compositing in `frame_pipeline.cpp`
- live AV1/H.264 Media Foundation sink-writer recording in `media_writer.cpp`;
  the app owns two instances for synchronized original/processed output
- a `LoadLibrary`/`GetProcAddress`-only NVIDIA Video Effects SuperRes adapter in
  `maxine_superres.cpp`, with validated pitched crop views, synchronous
  inference to prevent stale-frame ghosting, and clean NIS fallback when the
  optional runtime is absent, fails, or exceeds its measured 24 ms latency
  target; the application may explicitly override latency-only fallback
- canonical camera-to-viewport geometry in `view_transform.cpp`; D3D12,
  passthrough presentation, pointer mapping, and cached SuperRes ROI checks
  derive from the same uniform transform

If additional cross-cutting helpers land later, keep them small and explicitly reusable rather than turning this directory into a misc bucket.
