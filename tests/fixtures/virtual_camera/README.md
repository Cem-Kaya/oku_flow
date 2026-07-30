# Virtual Clamp Camera

`clamp_bump_160x90_30fps.y4m` is an optional, generated four-second YUV420
camera input for local visual and future capture-integration testing.
`clamp_bump_160x90_30fps.mp4` is its H.264/YUV420 preview copy for browsers
and ordinary video players. Both generated files are ignored and must not be
committed. CTest synthesizes the same deterministic sequence in memory and
does not read either file. The sequence models a phone clamped above a laptop
or lecture desk:

- mixed 0.7-3.0 Hz translation with sub-pixel source motion;
- one short desk-impact impulse and ring-down;
- a temporary brightness reduction;
- a moving foreground occluder; and
- a short focus-loss interval.

`clamp_bump_motion.csv` is the ground-truth camera path. Positive `dx` and `dy`
mean the scene moved right/down relative to the locked first frame.

`clamp_bump_comparison.mp4` is the three-second master visual regression:

```text
TARGET | INPUT | OUTPUT
```

- **Target** is the known locked reference.
- **Input** is the disturbed virtual camera.
- **Output** applies the corrections measured by OpenZoom's Phase B CUDA
  Virtual Tripod test.

The measured corrections are retained in
`clamp_bump_openzoom_corrections.csv`; the output is not an ideal
ground-truth inverse warp.

After changing stabilization code, refresh the correction trace with the
Release CUDA test:

```powershell
build\release-bundle\tests\Release\stabilization_cuda_tests.exe `
  --export-corrections tests\fixtures\virtual_camera\clamp_bump_openzoom_corrections.csv
```

Then regenerate the ignored Y4M input, both ignored MP4 files, and CSV ground
truth together:

```powershell
python tests/fixtures/virtual_camera/generate_clamp_bump.py
```

This command requires `ffmpeg` on `PATH` and fails if the MP4 cannot be
refreshed or the measured correction trace is absent. The Y4M file remains the
lossless, stable camera-like input for future capture/pipeline integration
tests; the MP4 is the convenient viewing copy. The CUDA stabilization test uses
the same motion formula directly at analysis resolution, avoiding video-decoder
differences in its sub-pixel assertions.

For recorded-camera investigations, the Windows CUDA build also provides
`stabilization_replay_cuda`. It accepts decoded, packed BGRA frames and replays
the production Virtual Tripod lock sequence and final CUDA warp:

```text
stabilization_replay_cuda --input input.bgra --output stabilized.bgra \
  --trace diagnostics.csv --width 1280 --height 720 \
  --zoom 1.25 --strength 1.0
```

Pass `--no-reference-accumulation` to compare the selected sharp keyframe
against the experimental four-frame accumulated reference. Production uses the
single sharp keyframe until accumulated-reference alignment has a real-footage
quality gate.

Use FFmpeg to decode and encode the raw stream. Raw inputs, replay outputs, and
personal recordings belong under the gitignored `local_evidence/` directory,
not in this tracked fixture directory.
