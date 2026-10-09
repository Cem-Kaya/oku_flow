# Third-Party Licenses and Attributions

OpenZoom relies on several third-party SDKs and code drops. Keep this summary with any redistributed build and update it whenever dependencies or bundled notices change.

## Qt 6
- Upstream: <https://www.qt.io/>
- License family: LGPL-3.0 / GPL-3.0 / commercial, depending on how Qt is obtained
- OpenZoom usage: dynamically linked Qt Widgets runtime deployed via `windeployqt`
- Notes:
  - Release bundles stage the applicable Qt license text as
    `licenses/QT_LICENSE.txt`.
  - They also stage the exact `qtbase`, `qtimageformats`, `qtmultimedia`,
    `qtpdf`, `qtspeech`, and `qtsvg` SPDX 2.3 JSON documents supplied by the
    selected Qt runtime under `licenses/qt-sbom/`. The OpenZoom SBOM links each
    document by namespace and checksum instead of flattening or weakening Qt's
    detailed component-level license declarations.
  - If you modify Qt itself, those changes must be handled under Qt's licensing terms.
  - Development builds expect the user to provide a local Qt installation.

### FFmpeg libraries deployed by Qt Multimedia
- Upstream: <https://ffmpeg.org/>
- Version: the exact version declared by the staged Qt Multimedia SPDX
  document (7.1.1 for the documented Qt 6.9.3 toolchain)
- License: LGPL-2.1-or-later plus the permissive component licenses enumerated
  in Qt's `qtmultimedia` SPDX document; the official Qt binary does not include
  FFmpeg's optional GPL-only components
- OpenZoom usage: `windeployqt` deploys the Qt Multimedia FFmpeg backend and
  its `avcodec`, `avformat`, `avutil`, `swresample`, and `swscale` libraries
- Attribution: release bundles stage the matching source tree's complete
  LGPL-2.1-or-later text as `licenses/QT_FFMPEG_LGPL_2_1.txt` and retain the
  exact Qt Multimedia SPDX document under `licenses/qt-sbom/`

## NVIDIA Image Scaling
- Upstream: <https://github.com/NVIDIAGameWorks/NVIDIAImageScaling>
- Version/commit: 1.0.3,
  `35e13ba316c98eeecf16f37eae70ce88019911f6`
- Local notice file: [`third_party/nvidia_nis/LICENSE.txt`](../third_party/nvidia_nis/LICENSE.txt)
- Local reference source: `third_party/nvidia_nis/NIS_Config.h` and
  `third_party/nvidia_nis/NIS_Scaler.h`
- License: MIT
- Usage: optional CUDA NVScaler adaptation using the reference 64-phase
  coefficient banks, 6-tap scaler, four directional filters, edge detector,
  and adaptive sharpening equations
- Attribution: release bundles stage the full upstream notice as
  `licenses/NVIDIA_NIS_LICENSE.txt`

## AMD FidelityFX Super Resolution 1.0
- Upstream: <https://github.com/GPUOpen-Effects/FidelityFX-FSR>
- Version/commit: 1.0.2,
  `a21ffb8f6c13233ba336352bdff293894c706575`
- Local notice file: [`third_party/amd_fsr1/LICENSE.txt`](../third_party/amd_fsr1/LICENSE.txt)
- Local reference source: `third_party/amd_fsr1/ffx_a.h` and
  `third_party/amd_fsr1/ffx_fsr1.h`
- License: MIT
- Usage: optional two-pass CUDA adaptation of the reference FP32 EASU scaler
  followed by RCAS
- Attribution: release bundles stage the full upstream notice as
  `licenses/AMD_FSR1_LICENSE.txt`

## NVIDIA CUDA Toolkit
- Upstream: <https://developer.nvidia.com/cuda-toolkit>
- License: NVIDIA CUDA Toolkit EULA
- OpenZoom redistribution scope:
  - CUDA-enabled builds link `cudart_static` into `open_zoom.exe`
  - no standalone CUDA Toolkit runtime DLLs, headers, compilers, or developer
    tools are copied into release bundles
  - the Toolkit EULA from the exact build toolkit is staged as
    `licenses/NVIDIA_CUDA_EULA.txt`; the separately installed display driver
    is not part of the bundle
- Review NVIDIA's current redistribution terms before shipping CUDA-enabled
  binaries.

## NVIDIA Maxine Video Effects SuperRes
- Upstream headers: <https://github.com/NVIDIA/MAXINE-VFX-SDK>
- Runtime download page:
  <https://www.nvidia.com/en-me/geforce/broadcasting/broadcast-sdk/resources/>
- Local notices: [`third_party/maxine/Maxine-VFX-SDK/LICENSE`](../third_party/maxine/Maxine-VFX-SDK/LICENSE)
  and [`third_party/maxine/LICENSE.txt`](../third_party/maxine/LICENSE.txt)
- Header/sample snapshot license: MIT
- Runtime license: NVIDIA SDK License Agreement; the supported 0.7.6 Video
  Effects runtime is obtained and installed separately by the user.
- OpenZoom usage: optional runtime-loaded SuperRes on supported NVIDIA GPUs.
  The GPL application resolves `NVVideoEffects.dll` and `NVCVImage.dll` with
  `LoadLibrary`/`GetProcAddress`; it has no import-library dependency.
- Distribution scope: no NVIDIA Video Effects runtime binaries, models, or
  installers are stored in the repository or copied into OpenZoom bundles.
  The Setup Assistant fetches a pinned installer directly from NVIDIA, verifies
  its SHA-256 value, and launches NVIDIA's installer after the user chooses to
  install it.
- Attribution: CUDA-enabled release bundles stage the MIT SDK-header license as
  `licenses/NVIDIA_MAXINE_SDK_HEADERS_LICENSE.txt` and the runtime-separation
  notice as `licenses/NVIDIA_MAXINE_INTEGRATION_NOTICE.txt`.
- Required product attribution: `SuperRes powered by NVIDIA Maxine™`.
  This attribution does not imply NVIDIA endorsement.

## Lucide Icons
- Upstream: <https://lucide.dev/>
- Package/version: `lucide-static` 1.25.0
- Local notice file: [`assets/icons/lucide/LICENSE`](../assets/icons/lucide/LICENSE)
- License: ISC; several inherited Feather icons also carry the MIT notice in
  the same license file
- OpenZoom usage: embedded Qt resource icons for camera actions, floating
  Assistant controls, Advanced section navigation, and keystone history
  Previous/Stop/Continue/Next actions
- Attribution: retain the local license file in source and the third-party
  notice in redistributed builds as `licenses/LUCIDE_LICENSE.txt`

## Microsoft Platform Components
- APIs used: Media Foundation, Direct3D 12, DXGI, Windows Imaging Component, and other Windows SDK libraries
- License source: Windows SDK / Visual Studio / OS redistribution terms
- Notes: these platform APIs are not copied into the repository as standalone
  third-party source code. When `windeployqt` stages `D3Dcompiler_47.dll`, the
  top-level SBOM adds the file's actual version as a Microsoft redistributable
  package governed by the applicable Microsoft terms.

## Native WebRTC Stack (live transcription, plan 36 Carrier D)
All components below are fetched by pinned commit at build time (never
committed to this repository), built as static libraries, and linked into
`open_zoom.exe`. Every license is GPL-3.0-compatible; no proprietary
component is involved and no extra runtime DLL ships in the bundle. Exact
upstream license texts are staged in the release bundle's `licenses/`
directory and covered by its checksum manifest.

- **libdatachannel v0.24.5** (commit `443f6934d9007eb7076ab7825ba330f355fcbead`)
  - License: MPL-2.0 (file-level copyleft; OpenZoom uses it unmodified)
  - Role: PeerConnection, SDP, ICE, DTLS-SRTP, SCTP data channel, RTP
    packetization
  - Bundled pinned submodules (also separate SBOM packages):
    - libjuice v1.7.2, commit `3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6`
      (MPL-2.0)
    - libSRTP 2.8.0, commit `24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5`
      (BSD-3-Clause)
    - usrsctp commit `fec583d54493f879d2ae44a743423bf8a04371ab`
      (BSD-3-Clause)
    - plog 1.1.10, commit `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef`
      (MIT)
    - JSON for Modern C++ 3.12.0, commit
      `55f93686c01528224f448c19128836e7df245f72` (MIT; header-only JSON
      dependency, notice staged as `licenses/NLOHMANN_JSON_LICENSE.txt`)
- **Opus v1.5.2** (commit `ddbe48383984d56acd9e1ab6a090c54ca6b735a6`)
  - License: BSD-3-Clause
  - Role: audio encoder for the outbound side of the SendRecv transcription
    track. No decoder is built: the assistant's return audio is never decoded.
- **Mbed TLS 3.6.7 LTS** (commit `068ff080b369adfac81509f9b57b2afabaf82dc5`)
  - License: Apache-2.0 OR GPL-2.0-or-later (dual)
  - Role: DTLS and crypto backend, built with `MBEDTLS_SSL_DTLS_SRTP`
    enabled

Servicing: these pins are recorded in the SBOM; each release reviews
upstream security advisories (Mbed TLS in particular) and bumps the pins as
ordinary changes re-validated by the loopback and live test matrices.

## Not Currently Bundled
- OpenCV DNN
- TensorRT
- external AI model weights
- NVIDIA Video Effects runtime, models, and installers
- OpenAI Codex CLI binaries and installer/bootstrap files

`OPENZOOM_ENABLE_TEXT_SR` adds only the dynamic Maxine adapter built from the
MIT header snapshot. It does not add a link-time or redistribution dependency
on the proprietary runtime.

## Optional External Tools And Services
- OpenAI-compatible VLM services may be used at runtime through user-supplied endpoint credentials, but no hosted model or service SDK is bundled here.
- OpenAI Codex CLI may be launched as an optional external `codex app-server`
  process. At the user's request, Setup Assistant may download an exact pinned
  copy of OpenAI's official Windows bootstrap script, verify its SHA-256, and
  run it with prompts disabled. That upstream bootstrap independently verifies
  the selected official release package against OpenAI's checksum manifest.
  OpenZoom does not bundle Codex source, binaries, model weights, installer, or
  an OpenAI SDK. Codex authentication, service access, usage limits, updates,
  and licensing remain governed by the user's Codex installation and OpenAI
  terms.
