Notes for future assistant
Project: OpenZoom (A11Y_ZOOM) – Windows real-time camera processing playground

Vision & initial ask
- Build a Windows desktop app that captures camera frames, runs GPU kernels (multi-frame, e.g. temporal average, digital stabilization), chains several effects, and displays the result live with the ability to snap still pictures.
- Preferred stack: CUDA for compute, D3D11 for presentation, Media Foundation for capture, zero-copy if possible.
- User explored frameworks (D3D11/MF, CUDA interop, Vulkan, OpenCL, Python prototypes). Ultimately we iterated toward a CUDA+D3D11/MF solution but temporarily fell back to CPU compositing for debugging.

Chronology / key accomplishments
1. Environment hygiene
   - Batch script now purges stale CMake caches when switching between WSL path and Windows share.
   - CMakeLists updated for CUDA 13 + VS 2022 (sm_86/sm_89), removed unsupported `-arch=native` and added multi-arch fatbin builds.
   - Resolved missing CUDA headers / toolkit path issues; documented Ninja vs MSBuild quirks (VS CUDA targets can misquote args).

2. Media Foundation capture pipeline
   - COM/MF initialization, SourceReader enumeration, hardware transforms enabled, DXGI device manager wiring.
   - Capture thread copies samples into CPU buffers with subtype metadata; handles BGRA32, RGB32, NV12, YUY2; stride/row pitch tracked.

3. Rendering + UI shell
   - Win32 window with child render control; D3D11 swap chain + interop texture management; resizing logic reworked to avoid flicker.
   - UI controls (combo box for camera selection, checkboxes/sliders for filters) with window layout adjustments.
   - Graceful shutdown on WM_CLOSE (camera thread stops, COM uninitializes, PowerShell prompt returns).

4. Debugging view (current state)
   - CPU pipeline converts each frame to BGRA, produces per-stage buffers (raw, black/white, zoom, blur, final).
   - Composite debug texture auto-lays the available stages into a grid, so new effects appear without manual wiring.
   - Gradient fallback removed from active path (only black clear if no frame yet) to eliminate the colored noise overlay.

Current limitations / open items
- CUDA kernels are disabled except for legacy gradient helper; interop still registered but unused. Need to restore GPU path once debug verifies inputs.
- Blur currently runs on the UI thread; acceptable for testing but ideally moves to a worker (or GPU) before shipping.
- Debug grid still uses nearest-neighbour scaling; consider a higher-quality resample for clarity.
- No overlays for format info / diagnostics; could add text or ImGui.
- Still-frame capture pipeline absent in this branch.

Build/run guidance (2025-01 snapshot)
- Use CMake + Ninja preferred: `cmake -S . -B build-ninja -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES="75;86;89"` then `cmake --build build-ninja`.
- For VS CMake mode ensure CUDA toolkit installed, Ninja available; avoid MSBuild CUDA target quoting issue.
- Runtime expects camera privacy permission enabled. Launch via `scripts\build_and_run.bat` or Ctrl+F5 in VS. Closing window should exit batch immediately.

Next steps for future agent
1. **Major refactor: Qt + Direct3D 12 presentation**
   - Move the rendering layer to Qt (or SDL) hosting a D3D12 swap chain; follow an easy-to-debug architecture where capture, processing, and presentation are separated.
   - Keep the CUDA pipeline zero-copy by sharing textures with D3D12 (CUDA has D3D12 interop); structure code so CPU fallbacks remain possible (debug builds should be able to toggle CPU path).
   - Preserve the 4-quadrant debug view as an optional overlay, but ensure default view is single full-resolution output.
   - Use Qt’s event loop/resize handling to avoid the manual Win32 layout bugs.

2. Re-enable CUDA compute pipeline (after D3D12 refactor)
   - Implement ring-buffer of GPU frames, port filters (temporal average, black/white, zoom) to CUDA kernels, ensure no gradient overlay.
   - Add temporal features explicitly requested: frame averaging, digital optical stabilization (motion estimation + warp), multi-frame chaining.
   - Maintain CPU fallbacks for debugging and as a safe mode.

3. Implement actual multi-frame features from original vision
   - Temporal averaging with adjustable window size.
   - Digital stabilization prototype (motion estimation, smoothing, warp) with CUDA kernels.
   - Snapshot capture (PNG/JPEG), ideally GPU path (CUDA to host readback) with metadata.

4. UI/UX improvements
   - Lightweight UI overlay (ImGui or Qt widgets) displaying frame rate, current format, filter toggles, stabilization status.
   - Ability to switch between single view, debug quadrants, and possibly histogram/diagnostics.

5. Build/tooling
   - Add detection of GPU SM config (optional) or keep `75;86;89` as baseline.
   - Document Qt + D3D12 environment setup alongside CUDA toolkit (MSVC, Windows SDK).
   - Keep Ninja workflow documented; note that VS generator may still mis-handle CUDA args.

Remember: the end goal is a stable, zero-copy CUDA pipeline with chained multi-frame kernels, snapshots, and minimal latency. UI must remain responsive, and fallback CPU path should be available for debugging. Primary focus next iteration: refactor presentation with Qt + D3D12 and re-introduce CUDA-based processing within that architecture.

Update (2025-02): Qt+D3D12 refactor landed, Media Foundation capture stable on CPU path. CUDA interop now targets external-memory flow (no legacy cudaD3D12.h). Tasks queued:
- Wire CudaInteropSurface::IsValid into presentation and gate GPU path, collect logs when CUDA device mismatch occurs.
- Port simpleD3D12 sample’s fence import so we can synchronize without blocking the stream.
- Implement real kernels (temporal filters) once the surface plumbing is exercised.

Pause log (2025-??): CUDA interop still failing
- `cudaImportExternalMemory` continues to fail when importing the shared D3D12 texture, so the app keeps falling back to the CPU path. Error strings are not yet surfaced.
- Next session: add verbose logging around the failing CUDA calls, then build a minimal `sandbox/dx12_cuda_minimal` sample that loads `img/img.jpg`, creates a shared D3D12 texture, imports it into CUDA, runs a trivial kernel, and presents the texture. Use the sample to validate heap flags and fence sync before copying fixes into the main pipeline.

Update (2025-02): After refactor the Qt runtime DLLs are required at launch on Windows. Ensure Qtin is on PATH or copy Qt6*.dll beside open_zoom.exe before running the batch.

Update (2025-02): Added .gitignore (filters build/, ref/, IDE, Qt artifacts) and docs/README.md basics. Remember to sync these whenever the directory layout changes, especially when new generated folders appear. Qt runtime DLL note remains: ensure PATH points to Qtin before running.

Update (2025-10): UI/UX polish and CPU blur pass
- Added Gaussian blur stage with adjustable sigma/radius and hooked it into the CPU pipeline; debug grid now visualises every active stage.
- Zoom UX improved: Ctrl + mouse wheel zooms around the cursor, middle mouse drag pans, virtual joystick sits bottom-right with a circular mask.
- Control tray collapses behind a toggle, camera list sorts alphabetically, blur controls report live values.
- `scripts/build_and_run.bat` resolves Qt bin automatically and runs `windeployqt`, producing a self-contained Release folder.

Update (2025-11): Input polish and deployment fix
- Two-finger touchpad scroll without Ctrl now pans the zoomed view; the handler consumes high-precision wheel gestures only when zoom is active.
- A `Show Focus Point` checkbox in the debug panel overlays a red marker at the live zoom center so cursor-driven focus updates are obvious.
- `scripts/build_and_run.bat` now trims the trailing `\` from the deployment directory before calling `windeployqt`, fixing the `Could not create directory ...Release"` error.
- Qt's optional `WrapVulkanHeaders` probe still prints a warning during CMake configure; that's benign for the CUDA + D3D12 pipeline and can be ignored.
- GPU processing path is live: frames upload into a shared D3D12 texture, CUDA kernels run black/white, zoom (with focus), separable blur, and focus-marker passes, and the presenter copies the texture directly to the swap chain. CPU compositing remains as a fallback (auto-enabled whenever the debug grid is requested).
- Added verbose CUDA interop diagnostics (LUID matching, external memory import, per-call cudaError strings) so future failures surface actionable detail.
- The control tray now exposes a "Processing: ..." badge so you can confirm when the CUDA path is active versus when the pipeline has fallen back to the CPU.

Update (2025-11-??): AI upscaling exploration
- Objective: go beyond Gaussian blur so magnified text stays legible without the soft-focus side effects.
- Fast spatial sharpeners (AMD FSR1-style and NVIDIA NIS) are now exposed as a dedicated toggle with sharpness control; Gaussian blur remains a separate post-stage.
- Upcoming tiers: lightweight DL SR via OpenCV DNN, Real-ESRGAN via TensorRT, and text-aware SR models. Keep pipeline modular, log per-backend health, and retain CPU fallbacks.
