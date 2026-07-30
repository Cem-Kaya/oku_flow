# OpenZoom Tests

The hardware-independent targets cover settings persistence, canonical
Fill/Fit viewport geometry, cached SuperRes ROI registration, monotonic
CUDA/D3D12 fence sequencing, and annotation model/render geometry. Annotation
tests exercise freehand/line/rectangle/ellipse/text creation and rendering,
matched dashed halo/core output, shape-outline hit testing, edit undo/redo,
selection/move/scale/erase, one undo record per scale gesture, permanent
session reset, zoom-scaled hit tolerance, and deterministic
scene-to-viewport rendering. The Windows overlay interaction test verifies
that middle-button drag crosses the separate frameless-window boundary and
that Text uses click-then-type inline placement. Settings tests cover
annotation style, shape, and
text-size round trips plus invalid-value migration. Run them with the CPU-only
Windows preset:

`recording_integrity_tests` covers exact 15/30/60 and 30000/1001 timing,
long-run fractional-rate drift, real timestamp gaps, missing/backward timestamp
recovery, drop accounting, terminal completion truthfulness, legal recording
state transitions, fixed-canvas orientation and Fit bars, bilinear resampling,
and negative-stride normalization without requiring a camera or Media
Foundation encoder.

`media_writer_audio_tests` creates a short synthetic video plus 48 kHz mono
PCM tone, encodes them into one fragmented MP4, finalizes it, and verifies
that Media Foundation can discover the resulting audio stream. It does not
open a physical microphone.

```bat
cmake --preset msvc-cpu
cmake --build --preset msvc-cpu-build
ctest --preset msvc-cpu-tests --no-tests=error
```

Run the CUDA suite, including its generated-input replay gate, with:

```bat
cmake --preset msvc-cuda-tests
cmake --build --preset msvc-cuda-tests-build
ctest --preset msvc-cuda-tests
```

CUDA-enabled builds add `stabilization_cuda_tests`. It exercises the
single fixed-reference CUDA path with known translations, rotation, scale,
brightness changes, moving-foreground outliers, clamp impacts, and rejected
measurements. The tests require a near-zero displayed path, verify rejected
frames hold the last correction, check that reference reset preserves continuity,
and cover the optional Extra Stable presentation hold. They also verify
prediction-seeded fixed-reference tracking recovers a 100-source-pixel
translation and that a 4x zoom solve follows the visible textured region
instead of a differently moving full-frame majority. Phase B coverage builds a
real Gaussian pyramid and prepared inverse-compositional reference, verifies
the sharpest lock candidate is retained, rejects a moving foreground during
aligned reference accumulation, and replays a deterministic clamp-impact path
with a brightness change and occlusion. The required gate is at least 90% RMS
attenuation and at most 1 display-pixel P95 at 4x zoom; the reference RTX 4090
Laptop run measured 97.33% and 0.36 px. A high-zoom regression requires
correction beyond the former 12% ceiling. Systems without a CUDA device report
a clean skip.

The same device test exercises the user-facing Extra Stable mode (internally
named Bump Hold) using real CUDA pixels: it captures a
settled frame, injects a transform discontinuity, verifies the cached frame is
presented while live tracking continues, waits for five stable measurements,
and checks the four-frame crossfade returns exactly to live output.

`tests/fixtures/virtual_camera/` contains the tracked deterministic generator
and CSV expectations rather than generated media. The CUDA replay harness
synthesizes the same four-second sequence in memory, so CTest needs no Python,
ffmpeg, or committed video. It then replays those frames through the production
CUDA kernels. The modeled sequence includes 0.7-3 Hz clamp vibration, a desk
impact and ring-down, temporary focus loss, a lighting change, and a moving
foreground occluder.

Y4M and MP4 outputs are intentionally ignored because they are regeneratable.
For local visual review, the generator can also create an H.264 input preview
and a target/input/output comparison MP4. The comparison consumes the measured
correction trace exported by:

```powershell
build\release-bundle\tests\Release\stabilization_cuda_tests.exe `
  --export-corrections tests\fixtures\virtual_camera\clamp_bump_openzoom_corrections.csv
```

Regenerate the input and comparison media with `ffmpeg` available on `PATH`:

```powershell
python tests/fixtures/virtual_camera/generate_clamp_bump.py
```

Camera capture, D3D12 presentation, encoder availability/finalization on real
storage, mid-recording device-mode changes, and reconnect behavior still
require manual testing on Windows hardware.
