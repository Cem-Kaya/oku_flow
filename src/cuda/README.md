# CUDA module

`cuda_interop.cpp` imports shared D3D12 textures, owns the pitched processing
workspace, and schedules effects on the existing CUDA stream. `cuda_kernels.cu`
contains the image conversion, stabilization, grading, and Text Clarity kernels;
`spatial_upscalers.cu` implements the pinned NIS and FSR paths. Public launch
interfaces mirror this directory under `include/okuflow/cuda/`.

The old gradient demo, surface-based black/white and zoom, and unused blend
kernel/launcher chains were removed after source and release-symbol review.
Current linear-buffer effects and viewport zoom remain the active paths.

Text Clarity's Sauvola mask marks foreground ink using local luma mean and
variance. The light-on-dark branch applies the dark-ink formula to inverted
intensity and mean; variance stays unchanged. This keeps an ordinary dark
board background out of the foreground mask. Explicit polarity or the device
scene-analysis light-text flag selects the branch. Softness, dark-text behavior,
and automatic scene classification keep their existing settings.

`text_clarity_sauvola_cuda` exercises the production mask launcher with generated
boards, strokes, complementary images, and padded-buffer guards. Run it through
the tracked CUDA CTest preset or `scripts/agent_build.bat`; a missing CUDA device
reports a clean skip. See `docs/code_reference.md` and `tests/README.md` for the
launch contract and regression scope.

The effective per-frame settings zero flatten strength when that child is off,
including glare-only launches of the shared kernel. This does not erase the
stored strength. CUDA regression inputs cover neutral ordinary pixels, reduced
bright glare, independent flattening, alpha and pitched-buffer padding.

Resident UCT `SurfaceFrames` shares the CPU closed-surface decisions and returns
only bounded corner metadata after ink fitting. It uses the existing owned
source/stream and completion/quarantine path, with at most 128 requests and no
image readback. See [surface perspective](../../docs/uct_surface_perspective.md).

One 128-thread block cooperates on each request: grid sampling, level-parallel
flooding, line hypotheses/refits and the ink-plane rescan share the work.

Resident UCT validation-integral rows use one cooperating 256-thread block per
row, retaining the serial table's exact unsigned values and unchanged column
pass. No extra global buffer, transfer or launch is added. See
[GPU integral scope and validation](../../docs/uct_gpu_integral.md).
