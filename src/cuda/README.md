# CUDA module

`cuda_interop.cpp` imports shared D3D12 textures, owns the pitched processing
workspace, and schedules effects on the existing CUDA stream. `cuda_kernels.cu`
contains the image conversion, stabilization, grading, and Text Clarity kernels;
`spatial_upscalers.cu` implements the pinned NIS and FSR paths. Public launch
interfaces mirror this directory under `include/okuflow/cuda/`.

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
