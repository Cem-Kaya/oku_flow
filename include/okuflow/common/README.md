# common public headers

Public common-layer interfaces live here.

Currently exported:
- `assistive_runtime.hpp`: shared provider configuration, including response
  instructions, scene analysis, vision text reading, speech output, and HTML lecture notes
- `codex_app_server_client.hpp`: native app-server transport with configurable
  model, full model/reasoning catalog discovery, visible built-in prompt, and
  guarded response preferences; admitted request generations and scoped
  turn identities protect cancellation/retry, and `SetRequestTimeoutMs` sets
  the transport deadline
- `image_processing.hpp`
- `frame_pipeline.hpp`
- `media_writer.hpp`: live AV1/H.264 fragmented-MP4 recorder and codec state,
  including cached terminal results and independent D3D11 source-read leases
- `recording_preservation.hpp`: conservative session-owned empty-file cleanup
  and terminal-result retention policies
- `gpu_read_retirement.hpp`: pending/unknown GPU read lease retirement policy
- `maxine_superres.hpp`: runtime-only NVIDIA Video Effects SuperRes adapter;
  all proprietary SDK calls are resolved dynamically
- `view_transform.hpp`: canonical aspect-safe Fill/Fit, focus, cached-ROI, and
  CPU pixel mapping shared by presentation and interaction
