# d3d12 module

This module owns the Direct3D 12 presentation path.

Current responsibilities:
- select a hardware adapter and create the D3D12 device
- keep the swap chain equal to the render HWND's native client size and manage
  its frame-latency waitable object, per-frame resources, and upload buffers
- use `DXGI_SCALING_NONE` during brief client/back-buffer size mismatches;
  DXGI must not stretch the previous frame into a different aspect ratio
- present CPU-generated BGRA frames
- draw persistent CPU/GPU scene textures through the canonical aspect-safe
  Fill/Fit transform using a full-screen triangle and bilinear sampler
- read back GPU textures for recording and capture, returning stable request
  ids so processed frames can be paired with their original camera frames
- bound fence completion waits to 1000 ms, detect device removal, and stop
  submission on terminal faults. Failed completion retains the full GPU
  resource graph until process exit; a timeout never authorizes reuse
- poll normal-frame allocator completion and swap-chain admission without
  waiting on the UI thread. A busy frame remains dirty for a later tick and
  counts as a missed present; it does not reset resources or fault the device.
  Fence completion is checked before consuming the latency event. Resize,
  teardown, and the non-interop ownership fallback keep their bounded drains
- recording clones wait explicitly on the source CUDA signal and publish a
  newer graphics signal, allowing recording to continue when only viewport
  admission is busy without allowing CUDA to overwrite a texture being read
- exclusively own the shared D3D12/CUDA fence timeline: external CUDA values
  are reserved, committed after enqueue, or canceled after a bounded drain;
  every graphics submission queues the latest committed external dependency.
  Canceled reservation holes are never waited upon, including after a busy
  viewport present or resize
- require producer fence completion as well as consumer lease release before
  reusing or evicting a recording canvas. Pending submission, device removal,
  or unknown completion cannot make a slot available
