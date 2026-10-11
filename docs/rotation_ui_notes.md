# Rotation UI + Pipeline Notes

- Advanced > Settings > Camera owns Orientation. It calls
  `OkuFlowApp::OnRotationSelectionChanged`, rebases the zoom center and ink,
  and resets temporal/stabilization/text histories for the new orientation.
  Orientation is global device state, not a quick-mode value; legacy profile
  rotation values are accepted only during settings migration.
- Supported GPU camera formats rotate in CUDA; remaining CPU formats use
  `CpuFramePipeline::RotateRawBuffer`. Quarter turns swap the source dimensions.
  `D3D12Presenter::PresentSceneTexture` uses the canonical `ViewTransform` to
  apply uniform Fill cropping or Fit letterboxing at the native viewport size.
  The old CPU `ResampleToFill` routine had no callers and was removed in audit 49.
- The shipping CMake release preset builds
  `build/msvc-release/cmake/oku_flow.exe`. Use the tracked build scripts and
  confirm which bundle/executable is running when reviewing a rebuilt UI.
