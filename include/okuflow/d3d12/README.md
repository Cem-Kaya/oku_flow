# d3d12 public headers

Public D3D12 presentation interfaces live here.

Currently exported:
- `presenter.hpp`: native-client-sized swap-chain presentation, aspect-safe
  scene-texture shader mapping, frame-latency pacing/missed-frame counters,
  plus synchronous and request-id-bearing asynchronous texture readback
