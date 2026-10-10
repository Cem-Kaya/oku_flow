# d3d12 public headers

Public D3D12 presentation interfaces live here.

Currently exported:
- `presenter.hpp`: native-client-sized swap-chain presentation, aspect-safe
  scene-texture shader mapping, frame-latency pacing/missed-frame counters,
  synchronous and request-id-bearing asynchronous texture readback, and the
  sole D3D12/CUDA shared-fence reservation/commit/cancel interface
- `shared_timeline.hpp`: monotonic graphics/external reservation and dependency
  policy; canceled external values are never waited upon
- `recording_slot_policy.hpp`: producer completion and device-loss policy for
  recording-canvas reuse and eviction
