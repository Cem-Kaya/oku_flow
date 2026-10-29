# Third-Party Licenses and Attributions

The source-ink contour implementation in `uct_ink_outline*` is original OkuFlow
code under the project's dual contribution license. It adds no third-party
model, library, font payload or downloaded asset. Existing Qt and CUDA notices
continue to apply. Test font registration uses fonts installed with Windows;
those fonts are not copied into the repository or release bundle.

## qpdf PDF drawing export (26 October 2026, development)

Annotated PDF copies use the unmodified upstream qpdf **12.4.2** Windows MSVC x64
runtime (Apache-2.0), separate from Qt's existing PDF viewing. The source PDF is
never an output. Qt creates vector ink overlays; qpdf retains native PDF content
and unedited pages. No PDF content is sent to a service.

`scripts/qpdf/pins.json` pins the upstream release ZIP SHA-256
`db87077e683630c1217e0e8f9a20a9749d952ab676e881c3689187763a5de25d`
and the exact two shipped binaries, `qpdf.exe` and `qpdf30.dll`.
The archive comes from
<https://github.com/qpdf/qpdf/releases/tag/v12.4.2>. Original upstream
`LICENSE.txt` and `NOTICE.md` are retained byte-for-byte. Its release's
`vcpkg.zip` (SHA-256
`82005252fee032135d07c85ad7626a38f26fb253c1e098d1d185f315bea15ebb`)
supplies the exact Windows static dependency notices and SPDX versions:
OpenSSL **3.6.4** (Apache-2.0), zlib **1.3.2** (Zlib), and libjpeg-turbo
**3.2.0** (IJG, BSD-3-Clause, Zlib). All five notices are tracked under
`scripts/qpdf/` and staged at `app/qpdf/licenses/`; the release SBOM records the
four packages, static links and pinned files. qpdf's own NOTICE also retains
the embedded-code attribution. No upstream test executables are shipped.

Static import inspection of both binaries found Windows/system VC runtime
imports and the sibling qpdf DLL. Vendor CRT DLLs in the upstream ZIP are not
deployed; OkuFlow's existing supported centrally installed runtime remains the
prerequisite. `stage_pdf_export.ps1` verifies all bytes before install/staging;
it downloads only when explicitly called with `-Install`. Application startup
and CMake never download this dependency. This records redistribution inputs,
not installer-guest acceptance of a newly assembled release.

## Qt PDF file-source dependency (26 October 2026, development)

File viewing now links Qt PDF 6.12.0 in addition to the existing Qt modules.
The official module headers declare `LicenseRef-Qt-Commercial OR LGPL-3.0-only
OR GPL-2.0-only OR GPL-3.0-only`; this is compatible with OkuFlow's GPL build
and the existing dynamic Qt LGPL/commercial distribution arrangements. PDFium
and the module's other third-party components retain their upstream terms;
OkuFlow's commercial license does not relicense those dependencies.
`scripts/install_qt_pdf.ps1` pins the original Qt MSVC x64 extension archive
and retains its exact `qtpdf-6.12.0.spdx.json` with package/file relationships
and license declarations. The archive source identity is qtpdf commit
`9a169a5eeded781fd7690fd4edd330be0302efaa`, as recorded by Qt's original SBOM.
The deployment script requires that SPDX document when `Qt6Pdf.dll` is staged.
No Qt/PDFium binary or implementation is copied into this repository.
Corresponding Qt sources and complete third-party notices remain required
for a redistributable bundle; a successful SDK or unit-test install alone is
not installer/license-inventory acceptance. See [file documents](file_documents.md).

The curved-line/current-ink experiment adds first-party C++/CUDA processing and
tests only, with no new model, library or copied third-party implementation.
Its private synthetic evidence renders installed Windows Arial/Segoe UI/Consolas
fonts and records their local file hashes; no font binaries or generated raster
evidence are tracked or shipped. These are diagnostic sources, not approved
BF16 golden/public assets. See [the experiment contract](uct_ribbon_and_current_ink.md).

## Requested OCR installer payload (24 October 2026)

Current development staging includes only original medium PP-OCRv6 detector and
recognizer graphs, configurations/dictionaries and model cards. Tiny/small are
optional immutable upstream downloads retaining their model cards and existing
Apache-2.0 notices. Original and historical BF16 pins remain provenance; converted
graphs are no longer staged. OkuFlow's first-party native recipe prepares BF16
inside TensorRT and caches engines locally. The source/recipe notice accompanies
the models. The historical Python converter is provenance, never app/installer
execution. See [model setup](model_installation.md).
No OCR accuracy or golden-corpus requirement controls app precision selection.

The private TensorRT payload is restricted to the runtime, ONNX parser and
selected builder-resource DLLs needed for local engine creation. The versioned
agreement is retained unchanged at `third_party/tensorrt/LICENSE-11.4.html`,
with its retrieval/hash record in the adjacent `PROVENANCE.json`. Its TensorRT
supplement section 2 identifies runtime `.so` and `.dll` files as distributable;
the main agreement requires material application functionality and access only
by the application. These libraries remain in OkuFlow's private runtime folder.
Headers, `trtexec`, SDK archives and developer tools are excluded.

The CUDA 12.9 license's Attachment A lists NVRTC and its builtins as
redistributable. The proposed dependency subset retains that exact toolkit
license and matching DLL bytes. ONNX Runtime CPU retains Microsoft's MIT license,
third-party notices and privacy document. No driver, owner engine cache or
prebuilt GPU plan is distributed. Runtime dependency closure, the staged SBOM
and candidate-specific installer checks must be verified before publication;
this preparation does not change the existing v0.2.5 download inventory.

## BF16 model family preparation (development only)

The owner requested BF16 preference for all GPU detector and recognizer tiers,
with same-tier FP32 fallback. `scripts/uct/bf16-models.json` records six converted
PaddlePaddle graph identities, upstream source/configuration pins and revisions.
They reuse the first-party hash-pinned BF16-island converter, preserving FLOAT
I/O and the existing Apache-2.0 model origins. No additional third-party runtime
or model was downloaded for these conversions. The inventory itself is not
redistribution clearance or quality acceptance. Public bundle notices, converter
source/provenance, TensorRT/CUDA dependency and release gates remain separate.
The exact first-party converter snapshot is now tracked in
`scripts/uct/precision_models.py` with its original `da22b96f…` hash. Its optional
ONNX/NumPy imports are offline developer tools, never app/installer dependencies;
this adds no third-party implementation or model/runtime payload to the bundle.

## Unapproved owned BF16 typeset crop proposal (development only)

The eight original source-authored text bitmaps in
`assets/uct/golden/okuflow-owned-typeset-gray8-v1/` use the separately installed
Pillow 11.1.0/FreeType renderer and installed DejaVu Sans regular/bold fonts.
They were first generated in owned Z: work-data; these source copies were added
unchanged only after independent read-only review of all eight crops.
The existing installed Pillow notice identifies MIT-CMU; the existing DejaVu
notice retains Bitstream Vera and Arev terms and says DejaVu changes are public
domain. Exact installed font and notice hashes, actual tool versions and render
parameters are retained in the companion provenance. No font binary, rendering
tool/library, copied third-party implementation or third-party source artwork
is added. These facts do not assert new redistribution clearance or approve
public staging. The candidate is unapproved, not an active application asset,
and has no model quality/attestation result. See
[owned crop proposal](uct_owned_bf16_crops.md) for source truth and remaining gates.

The separate static website's browser transcription tool uses Transformers.js
(Apache-2.0), MP4Box.js (BSD-3-Clause), ONNX Runtime and Whisper weights (MIT).
Those runtimes/models are not Windows or Android binary dependencies. Their
pinned files and notices belong to the website repository; see
`okuflow-website/docs/browser-transcription.md`. The notes document contains
first-party file-access/MessageChannel glue, not a bundled recognition engine.

OkuFlow relies on several third-party SDKs and code drops. Keep this summary with any redistributed build and update it whenever dependencies or bundled notices change.

The development video player/archive and WASAPI playback output use Windows
Media Foundation, COM and Windows audio APIs from the existing Windows SDK.
They add first-party code, not a bundled media framework or downloaded codec.
The preferred GPU player uses the same Windows D3D11 video processor/DXGI
manager and existing CUDA interop, adding no bundled decoder or media SDK.
Privately generated codec-test files and the owner's FFmpeg installation are
not release payloads or application dependencies. Native decoding depends on codecs installed on the user's Windows system;
successful local AV1 decoding does not grant redistribution rights to an
external codec extension. Existing Qt accessibility APIs serve the native
transport under the Qt terms below.

Live RAM recap reuses those Windows Media Foundation H.264 encoder/decoder,
COM and existing D3D APIs through first-party bounded heap-stream/ring code.
It adds no bundled codec, media SDK, external model or third-party source.
Synthetic in-memory codec fixtures contain original generated colour patterns;
no external footage/audio/fonts or media files enter application payloads.

The new localization resources are machine-assisted translations of first-party
public UI text generated during development. They introduce no translation SDK
or runtime service dependency. See [localization provenance and validation](localization.md).

## Development-only GitHub automation

The Windows workflow uses commit-pinned `actions/checkout`, `actions/setup-python`,
`actions/upload-artifact` and `jurplel/install-qt-action`.
These actions and the Qt downloader `aqtinstall` are MIT-licensed development
tools executed on GitHub runners, not added to OkuFlow release inventories.
MSVC initialization invokes the runner's installed Microsoft Visual Studio
developer shell directly; no third-party MSVC setup action is downloaded.
Their upstream licenses remain in their action repositories; downloaded Qt
retains the separate terms and redistribution requirements below.

Incremental-build caching uses the commit-pinned MIT-licensed
[`actions/cache`](https://github.com/actions/cache/blob/caa296126883cff596d87d8935842f9db880ef25/LICENSE).
Compiler caching uses Apache-2.0
[`mozilla-actions/sccache-action`](https://github.com/mozilla-actions/sccache-action/blob/fc920bf0ec8de6ee65d409111f7ec508035751ba/LICENSE)
and [`sccache` v0.18.0](https://github.com/mozilla/sccache/blob/v0.18.0/LICENSE).
These are CI-only tools downloaded on disposable GitHub runners; their source,
binaries and caches are not added to Windows/Android release inventories.

## Qt 6
- Upstream: <https://www.qt.io/>
- License family: LGPL-3.0 / GPL-3.0 / commercial, depending on how Qt is obtained
- OkuFlow usage: dynamically linked Qt Widgets runtime deployed via `windeployqt`
- Notes:
  - Release bundles stage the applicable Qt license text as
    `licenses/QT_LICENSE.txt`.
    The required-file helper checks that each copied notice exists and is
    byte-identical to its source before metadata generation.
  - They also stage the exact `qtbase`, `qtimageformats`, `qtmultimedia`,
    `qtspeech`, and `qtsvg` SPDX 2.3 JSON documents supplied by the
    selected Qt runtime under `licenses/qt-sbom/`. The OkuFlow SBOM links each
    document by namespace and checksum instead of flattening or weakening Qt's
    detailed component-level license declarations.
  - Qt PDF's exact `qtpdf` document is also retained when present, and is
    mandatory for current file-viewing builds, which deploy `Qt6Pdf.dll`.
    Earlier releases without PDF remain separate inventories.
  - If you modify Qt itself, those changes must be handled under Qt's licensing terms.
  - Development builds expect the user to provide a local Qt installation.
  - Hosted checks install the matching `qtmultimedia` source archive alongside
    the SDK for the bundle preflight's FFmpeg notice. Archive installations
    can also supply the unchanged `LICENSES/LGPL-3.0-only.txt` from that module
    when neither the SDK nor Qt's online-installer directory contains the Qt
    license text. The original bytes are still staged and checked; the source
    fallback is not permission to omit a notice. This adds no runtime dependency.

### FFmpeg libraries deployed by Qt Multimedia
- Upstream: <https://ffmpeg.org/>
- Version: the exact version declared by the staged Qt Multimedia SPDX
  document (9.0.1 for the documented Qt 6.12.0 toolchain)
- License: LGPL-2.1-or-later plus the permissive component licenses enumerated
  in Qt's `qtmultimedia` SPDX document; the official Qt binary does not include
  FFmpeg's optional GPL-only components
- OkuFlow usage: `windeployqt` deploys the Qt Multimedia FFmpeg backend and
  its `avcodec`, `avformat`, `avutil`, `swresample`, and `swscale` libraries
- Attribution: release bundles stage the matching source tree's complete
  LGPL-2.1-or-later text as `licenses/QT_FFMPEG_LGPL_2_1.txt` and retain the
  exact Qt Multimedia SPDX document under `licenses/qt-sbom/`

### Optional embedded FFmpeg/dav1d development SDK

`tests/data/embedded-pool-4k.mp4` is an original generated 12-frame uniform
gray-step fixture, without external footage/audio/fonts. It is test-only under
the project's contribution terms; its generation tool is not shipped. The exact
command and hash are recorded in tests/README.md.

- FFmpeg 7.1.1 sources: <https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz>,
  LGPL-2.1-or-later with upstream permissive components (including IJG terms)
  retained in the complete source archive and `LICENSE.md`.
- dav1d 1.5.1: <https://downloads.videolan.org/pub/videolan/dav1d/1.5.1/>,
  BSD-2-Clause, original `COPYING` retained without modification.
- First-party code calls public libavformat/libavcodec/libavutil/libswscale/
  libswresample APIs through dynamic DLL loading. The reviewed configuration
  disables GPL, nonfree, autodetection, programs, encoders, muxers, filters,
  devices, network/protocols and hardware acceleration; AV1 explicitly uses
  libdav1d. These libraries are not relicensed under OkuFlow's commercial terms.
- `scripts/ffmpeg/pins.json` retains source/tool hashes and configuration.
  `prepare_ffmpeg_playback.ps1` builds only under LocalAppData, retaining exact
  source/configuration, original LGPL/component/dav1d/MinGW notices and actual
  runtime import/hash inventory. Its additional dependency is the existing
  reviewed MinGW winpthread DLL (original MIT/BSD component notices).
- GNU make 4.4.1/diffutils 3.12 (GPL-3.0-or-later) and Meson 1.7.0
  (Apache-2.0) are private build tools only. Existing GCC/MinGW retains its
  separate runtime exception/notices; see the adb source-build inventory below.
  No build tool enters an application payload.
- No SDK binary/source is copied into Git, Qt, an installer or a release bundle.
  The owner's installed full GPL FFmpeg executable is not a library/deployment
  input. Qt's FFmpeg 9 libraries remain separate.
- **Public redistribution is not accepted by this prototype.** Before shipping,
  review exact corresponding sources, configuration/changes, complete component
  notices and SBOM; provide required LGPL library replacement/relinking rights
  and source access/attribution in both licensing channels. Compatible licenses
  and local compilation do not establish distribution or patent acceptance.
  See [development integration](video_ffmpeg_playback.md) and
  [upstream compliance guidance](https://ffmpeg.org/legal.html).

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
- Side-by-side developer comparison uses NVIDIA's official CUDA 12.9.2
  component ZIPs (including nvcc 12.9.86), verified against its redistrib
  manifest. Component notices remain beside the local install; the exact
  compiler-package `LICENSE` is retained at the assembled toolkit root for
  release notice staging. No compiler, toolkit archive or new third-party
  implementation is added to the repository or application payload.
- OkuFlow redistribution scope:
  - CUDA-enabled builds link `cudart_static` into `OkuFlowApp.exe`
  - no standalone CUDA Toolkit runtime DLLs, headers, compilers, or developer
    tools are copied into release bundles
  - the Toolkit EULA (`EULA.txt` or the newer `LICENSE` filename) from the exact build toolkit is staged as
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
- OkuFlow usage: optional runtime-loaded SuperRes on supported NVIDIA GPUs.
  The GPL application resolves `NVVideoEffects.dll` and `NVCVImage.dll` with
  `LoadLibrary`/`GetProcAddress`; it has no import-library dependency.
- Distribution scope: no NVIDIA Video Effects runtime binaries, models, or
  installers are stored in the repository or copied into OkuFlow bundles.
  The Setup Assistant fetches a pinned installer directly from NVIDIA, verifies
  its SHA-256 value, and launches NVIDIA's installer after the user chooses to
  install it.
- Attribution: CUDA-enabled release bundles stage the MIT SDK-header license as
  `licenses/NVIDIA_MAXINE_SDK_HEADERS_LICENSE.txt` and the runtime-separation
  notice as `licenses/NVIDIA_MAXINE_INTEGRATION_NOTICE.txt`.
- Required product attribution: `SuperRes powered by NVIDIA Maxine™`.
  This attribution does not imply NVIDIA endorsement.
- Development lifecycle/navigation fixtures compile first-party fake ABI
  implementations against these existing MIT headers. Their private helper
  DLLs contain no NVIDIA runtime/models, are not Maxine image-quality evidence,
  and must never enter release staging.

## Lucide Icons
- Upstream: <https://lucide.dev/>
- Package/version: `lucide-static` 1.25.0
- Local notice file: [`assets/icons/lucide/LICENSE`](../assets/icons/lucide/LICENSE)
- License: ISC; several inherited Feather icons also carry the MIT notice in
  the same license file
- OkuFlow usage: embedded Qt resource icons for camera actions, floating
  Assistant controls, Advanced section navigation, and keystone history
  Previous/Stop/Continue/Next actions
- Attribution: retain the local license file in source and the third-party
  notice in redistributed builds as `licenses/LUCIDE_LICENSE.txt`

## Optional experimental PaddleOCR component (2026-10-18)

PP-OCRv6 tiny, small and medium detection and recognition ONNX weights, configuration and
original model cards come from PaddlePaddle under Apache-2.0. Exact repository
revisions and hashes are in `scripts/uct/pins.json`. The unchanged upstream
license is retained in `third_party/paddleocr/LICENSE` and staged as
`MODEL_LICENSES.txt`. The dictionary is part of the pinned configuration.
First-party CPU/CUDA DB region extraction and CTC decoding do not add OpenCV,
pyclipper, Python or PaddlePaddle to the application.

Owner approved future distribution of the converted medium BF16-linear model,
not a private one-card engine. `scripts/uct/public-bf16-model.json` records the
existing graph/source/config/dictionary, upstream revision/license/model-card and
first-party converter hashes. No graph, corpus or SDK file is staged by the
offline contract. Public staging must retain the original Apache-2.0 license,
model card, configuration and modification/conversion provenance notice. Golden
pixels/truth must be owned, immutable and independently reviewed; runtime font
rendering is not a reproducible substitute. See [public BF16 preparation](uct_public_bf16.md).

ONNX Runtime CPU 1.31.0 uses MIT and its complete retained third-party terms.
Preparation verifies the official Microsoft archive and DLL hashes, retaining
`LICENSE`, `ThirdPartyNotices.txt` and `Privacy.md` with `onnxruntime.dll`.
This is separate from the website dependency; no ONNX CUDA/cuDNN is added.

TensorRT SDK 11.4.0.106 for CUDA 12.9 is optional and externally installed
under LocalAppData. Its archive is pinned; headers are Apache-2.0, while the
runtime remains governed by the [versioned NVIDIA SDK agreement](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/reference/sla.html).
Its original `doc/Acknowledgements.txt` stays with that installation. OkuFlow's
GPL section-7 NVIDIA exception does not waive NVIDIA redistribution terms.
The preparation command installs it only with `-TensorRT`; the application
does not download it. No TensorRT files or model weights are silently added
to an installer. Public inclusion needs a reviewed inventory, notices and
SBOM entries; private development acceptance does not establish that gate.
The 2026-10-23 Opus redistribution observations remain provisional: exact parser,
builder-resource, runtime and CUDA payload license/inventory validation remains
required before bundling. The model decision does not authorize silently copying
SDK files, changing CUDA12.9/TRT11.4 or claiming release clearance. Prebuilt
Ampere-plus engines remain an untested later option.

## Microsoft Platform Components

- APIs used: Media Foundation, Direct3D 12, DXGI, Windows Imaging Component, and other Windows SDK libraries
- Ultra Clear Text (experimental) calls the built-in `Windows.Media.Ocr`
  engine through C++/WinRT projection headers from the Windows SDK and links
  `windowsapp.lib`. The OCR engine and its language data are Windows
  components on the user's machine; the Windows fallback redistributes no
  Windows OCR model or language pack and copies no C++/WinRT source into the
  repository. C++/WinRT headers are used as provided by the installed SDK.
- License source: Windows SDK / Visual Studio / OS redistribution terms
- Notes: these platform APIs are not copied into the repository as standalone
  third-party source code. When `windeployqt` stages `D3Dcompiler_47.dll`, the
  top-level SBOM adds the file's actual version as a Microsoft redistributable
  package governed by the applicable Microsoft terms.

## Android Companion Dependencies (closed-test preparation)

The standalone `android/` Gradle project uses GPL-3.0-compatible Apache-2.0
runtime libraries: AndroidX Core KTX 1.19.1, Activity Compose 1.13.0,
Lifecycle Compose 2.11.0, Compose UI/Foundation 1.12.1 (BOM 2026.09.00),
Material 3 1.4.0, Kotlin standard library and transitive coroutines.
Exact direct pins are in `android/gradle/libs.versions.toml`; dependency
versions and installed tools are recorded in `android/BUILDING.md`.
Upstreams: [AndroidX](https://android.googlesource.com/platform/frameworks/support/),
[Kotlin](https://github.com/JetBrains/kotlin), and
[kotlinx.coroutines](https://github.com/Kotlin/kotlinx.coroutines).

AGP 9.4.1, Kotlin/Compose compiler 2.4.21, the Gradle 9.8.1 wrapper jar,
and Compose preview tooling are Apache-2.0 development dependencies. The
generated wrapper jar is checked against its official upstream checksum.
JUnit 4.13.2 (EPL-1.0) and its Hamcrest dependency (BSD-3-Clause) are JVM
test-only dependencies; they are not included in the application APK.
Their test-only use does not add an EPL runtime component to the GPL app.

Temurin JDK 21 uses GPL-2.0 with the Classpath exception. The Android SDK,
NDK and CMake are separately installed development tools governed by their own
vendor licenses/notices, not copied into the Windows bundle. The Windows bundle
does contain the source-built adb runtime and its dependency DLLs; see
[Android Debug Bridge (source-built runtime)](#android-debug-bridge-source-built-runtime).
The release gate now checks 65 resolved runtime entries against 64 reviewed
unique artifacts and verifies the original license/NOTICE assets in both APK
and AAB. Official POMs, original notices, artifact hashes and review tools are
retained in [`android/licenses/`](../android/licenses/README.md). Guava's
inherited license and AndroidX graphics-path's native source/build inputs are
reviewed explicitly. The latter retains separate source/blob and ELF dependency
evidence; this is not a reproducible-build claim for Google's native binary.
Offline Open-source licenses includes these notices and OkuFlow's GPL text.
Changed dependencies or notices fail validation until reviewed again. The Windows
release script does not ship the APK. Contributions to the companion follow the
repository's GPL-3.0 plus commercial contribution terms.

The phone engine uses Android platform Camera2/MediaCodec/LocalSocket APIs;
the desktop receiver uses the Windows system H.264 decoder, an optional installed
Windows HEVC decoder/extension, and the existing Qt Network dependency. Windows
codec extensions remain separately licensed and are never downloaded or bundled
by this receiver. No new third-party library or codec binary is bundled.
The adb protobuf reader is original code implementing the public AOSP field
contract; no generated protobuf source/runtime is imported. Python's standard
library provides the optional probe and fake-server tests.

`tests/data/phone-camera.h264` and `tests/data/phone-camera.hevc` are original
64×48 gray-scene fixtures generated locally with separately installed
FFmpeg/libx264 and FFmpeg/libx265, not third-party footage.
The generation command is recorded in `tests/README.md`; the fixture follows
the project's contribution/license terms. Neither generation tool is redistributed.

## Android Debug Bridge (source-built runtime)
- Upstream source: [android-tools 37.0.0](https://github.com/nmeum/android-tools/releases/tag/37.0.0),
  which includes AOSP adb and its source dependencies; Windows USB DLL source:
  [MSYS2 adbwinapi](https://github.com/msys2/adbwinapi).
- Distribution version: `37.0.0-okuflow.1`, adb protocol 41. Inputs, patches
  and exact development-library packages are SHA-256 pinned in
  `scripts/adb-source/pins.json`. The reviewed output manifest is pinned in
  `scripts/adb-source/distribution.json`.
- Build: GCC 16.2.0 / MSYS2 UCRT64 for adb, MSVC 2022 for both USB DLLs.
  Only adb and the USB DLL targets are built. Wireless mDNS is disabled.
  This is an unsigned OkuFlow source build; no Google SDK binary is copied.
- Bundled runtime: `platform-tools/adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`
  and their complete transitive dependency DLLs. `NOTICE.txt`,
  `source.properties` and `adb-build.json` accompany them. Fastboot, sqlite3,
  filesystem tools and Android build tools are not deployed.
- Source/relinking materials: `sources/adb-37.0.0-okuflow-source.zip` contains
  complete original adb/libusb/BoringSSL and Windows USB sources, all applied
  patches, exact dependency package archives with headers/import libraries and
  runtime DLLs, and rebuild instructions/scripts. Full notices are retained in
  `licenses/adb/`. Source archives also contain other Android tools; their
  presence does not mean those executables or code are linked into adb.

| Component | License | Use / notice |
| --- | --- | --- |
| AOSP adb and Windows USB API | Apache-2.0 | Separate executable and USB DLLs; `AOSP_ADB_NOTICE.txt`, `APACHE_2_0.txt` |
| libusb 1.0.29, AOSP commit `e392f3b7f7c3550c665793db2eea37a95c3bb105` | LGPL-2.1-or-later | Static; `LIBUSB_LGPL_2_1.txt`, `LIBUSB_NOTICE.txt`; full corresponding source/relinking ZIP |
| BoringSSL snapshot in the pinned source archive | Composite upstream license (`LicenseRef-BoringSSL` in SBOM) | Static; complete `BORINGSSL_LICENSE.txt` retained and extracted into SBOM |
| Abseil 20260526.0-1 | Apache-2.0 | DLLs; `abseil-cpp/` |
| Protobuf 36.2-1 | BSD-3-Clause | DLLs; `protobuf/` |
| utf8_range, Protobuf 36.2 snapshot | MIT | DLL; `UTF8_RANGE_LICENSE.txt` |
| Brotli 1.2.0-1 | MIT | DLLs; `brotli/` |
| LZ4 1.10.0-1 | BSD-2-Clause | DLL; `lz4/LICENSE` |
| Zstandard 1.5.7-2 | BSD-3-Clause | DLL; `zstd/` |
| zlib 1.3.2-2 | Zlib | DLL; `zlib/` |
| fmt 12.2.0-1 | MIT | Headers; `fmt/` |
| GCC libgcc/libstdc++ 16.2.0-5 | GPL-3.0-or-later WITH GCC-exception-3.1 | DLLs; full `libgcc/` and `libstdc++/` notices and exception text |
| MinGW winpthreads 14.0.0.r426.g4564ee4b5-1 | MIT AND BSD-3-Clause | DLL; complete `libwinpthread/` notices |
| MinGW CRT/headers | Component terms in retained original notices | Toolchain runtime/header material; complete `crt/` and `headers/` notices |
| MSYS2 Windows recipe/patches | BSD-3-Clause | Applied build adaptation; `MSYS2_RECIPE_LICENSE.txt` |

- LGPL static-link distribution follows [LGPL 2.1 section 6(a)](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html):
  the included corresponding source and relinking inputs allow modifying libusb
  and rebuilding/relinking adb. Modification of the library and reverse
  engineering for debugging those modifications are permitted. OkuFlow's
  commercial terms do not limit these rights. Redistributors must preserve
  the complete notices and source/relinking package, not omit it from the ZIP.
- The [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html)
  applies to eligible compilation using the GCC runtime; its exact installed
  text is retained with the GPL text. These runtimes are not classified as
  unrestricted Apache/MIT code.
- adb is an independent program, not linked into `oku_flow.exe`; its licenses
  remain independent in both GPL and commercial OkuFlow distributions.
- Staging validates the reviewed manifest and every runtime/source file; SDK
  directories are never an automatic packaging fallback. See
  [adb_distribution.md](adb_distribution.md) for build/relink and ownership rules.
- The Android APK and development SDK remain separately installed/distributed.

## Not Currently Bundled

`tests/uct_book_crop_test_support.hpp` contains a test-only 46x36 lossless RGB
crop from the owner's supplied local book frame, used for the authorized source
ink regression. It introduces no third-party code, font, model or runtime and
is not an application asset or release payload. Source identity, crop bounds
and exact pixel digest are recorded in
[`uct_color_startup_repair.md`](uct_color_startup_repair.md#small-upright-source-ink-2026-10-22-development).

- OpenCV DNN
- TensorRT
- external AI model weights
- NVIDIA Video Effects runtime, models, and installers
- OpenAI Codex CLI binaries and installer/bootstrap files

`OKUFLOW_ENABLE_TEXT_SR` adds only the dynamic Maxine adapter built from the
MIT header snapshot. It does not add a link-time or redistribution dependency
on the proprietary runtime.

## Optional External Tools And Services
- MSI tooling executes the WiX apphost directly from the already reviewed
  4.0.6 NuGet archive after complete extracted-tree byte verification. No
  separately resolved NuGet tool, new WiX version or shipped component is
  introduced. CUDA metadata and notice staging use CMake's effective compiler
  and linked static runtime facts; the selected toolkit's exact license bytes
  must match the configured manifest.
- WiX Toolset 4.0.6 is a developer-only MSI compiler under MS-RL, pinned by
  NuGet SHA-256 in `scripts/windows-installer/tools.json`. The unmodified exact
  upstream license is retained as `scripts/windows-installer/WIX_LICENSE.txt`;
  source: <https://github.com/wixtoolset/wix/tree/v4.0.6>. Its reciprocal source
  requirements apply to distribution of covered WiX files. OkuFlow uses
  first-party WiX authoring and standard Windows Installer actions only;
  no WiX executables, extensions, custom-action DLLs or runtime are shipped
  inside the MSI or application. Do not add such components without a new
  redistribution/license review. Microsoft's SDK MakeAppx is likewise a
  separately installed development tool, not a redistributed application file.
- OBS Studio (GPL-2.0-or-later, <https://obsproject.com/>) is an optional
  separately installed lecture-test tool. `scripts/lecture_camera.ps1`
  prepares a private local portable copy, including its bundled obs-websocket
  plugin. Neither OBS nor its binaries/plugins ship in OkuFlow bundles.
- DroidCam Virtual Output 0.2.2 (GPL-2.0,
  <https://github.com/dev47apps/droidcam-obs-virtual-output>) is extracted from
  a SHA-256-verified official package into the private OBS copy. The separately
  installed modern DroidCam Video kernel driver makes that output visible to
  Media Foundation. The script does not install/relicense that driver. Neither
  driver, plugin source, installer, nor plugin binaries ship in OkuFlow bundles.
- FFmpeg is an optional separately installed tool used by yt-dlp to remux
  YouTube HLS downloads. These tooling binaries are not bundled with OkuFlow.
- The optional default lecture sample is Sanjay Sarma / MIT OpenCourseWare,
  2.003J Dynamics and Control I, Fall 2007, Lecture 2: The spider on a Frisbee
  problem, under CC BY-NC-SA 4.0. It is downloaded to the user's local scratch
  directory with an adjacent attribution/source record and remains excluded
  from git and OkuFlow bundles. Its license is separate from OkuFlow's dual
  license; see [`lecture_camera.md`](lecture_camera.md) and
  <https://ocw.mit.edu/pages/privacy-and-terms-of-use/>.
- `yt-dlp` (<https://github.com/yt-dlp/yt-dlp>) is an optional external
  downloader for replacement YouTube sources; no downloader or Python package
  is bundled. The default publisher download needs neither.
- OpenAI-compatible VLM services may be used at runtime through user-supplied endpoint credentials, but no hosted model or service SDK is bundled here.
- OpenAI Codex CLI may be launched as an optional external `codex app-server`
  process. At the user's request, Setup Assistant may download an exact pinned
  copy of OpenAI's official Windows bootstrap script, verify its SHA-256, and
  run it with prompts disabled. That upstream bootstrap independently verifies
  the selected official release package against OpenAI's checksum manifest.
  OkuFlow does not bundle Codex source, binaries, model weights, installer, or
  an OpenAI SDK. Codex authentication, service access, usage limits, updates,
  and licensing remain governed by the user's Codex installation and OpenAI
  terms.

## Microsoft Visual C++ Windows runtime

The Windows ZIP retains Microsoft's unmodified signed x64 redistributable
14.44.35211.0 and its exact embedded English EULA as
`licenses/MICROSOFT_VC_RUNTIME_LICENSE.rtf`. Version, installer and EULA
SHA-256 pins live in `scripts/vc-runtime/pins.json`. The build uses the
licensed Visual Studio 2022 redistributable; Microsoft's redistribution
terms apply independently of OkuFlow's dual license. The native launcher
uses the static CRT (/MT); the Qt application uses a system installation.
No app-local CRT DLLs are copied. The launcher requests the user's vendor
license/UAC approval only when installation is needed; it never forces a
restart or downgrades a supported installed version.

[Microsoft redistribution guidance](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)
and [runtime license](https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/).
See [Windows runtime setup](windows_runtime_setup.md) for staging, provenance
and remaining clean-VM acceptance. The license text and installer must be
reviewed together whenever pins are refreshed.

The agent-control compatibility profile contains first-party factual records
from local Codex 0.160.1 offline feature/schema generation. It does not vendor a
Codex SDK, generated implementation or model. The owner-installed CLI remains
a separate prerequisite; no new third-party runtime is introduced by this gate.

## Development-only MCP interoperability fixture

The bounded JSON/schema parser, Qt-free stdio bridge, Windows named-pipe
security glue and application endpoint are first-party OkuFlow code. They add
no MCP SDK or third-party runtime to the Windows package. The bridge uses
Windows APIs, bcrypt and the existing Microsoft static CRT redistribution.

The opt-in independent-client fixture pins `@modelcontextprotocol/sdk` 1.32.1
and its exact MIT notice in `tests/mcp-sdk/`; the lock file records its
MIT/ISC/BSD development-only dependencies. Install this fixture outside the
checkout with lifecycle scripts disabled. It is never bundled into OkuFlow,
Android artifacts or installer payloads. Passing it proves interoperability
with a private synthetic application, not a live provider or camera.

The original PaddleOCR Apache license is marked `-text` in `.gitattributes`,
so Windows checkout conversion cannot alter the reviewed upstream bytes.

`tests/data/long-gop-h264.mp4` is an original 192×128 synthetic test-pattern
fixture generated locally with the separately installed FFmpeg/libx264 tools.
It follows the project's contribution/license terms; no tool or codec library
is bundled. The reproducible command and reviewed byte hash are recorded in
`tests/README.md`. The fixture is test-only, outside application payloads.

`tests/data/uct-scene-cut-h264.mp4` is another original first-party fixture:
one second of red followed by two seconds of blue, generated with the owner's
separately installed FFmpeg/libx264. It has no third-party footage, audio or
fonts, follows the project's contribution/license terms, and is test-only.
Its command and exact reviewed hash are recorded in `tests/README.md`; neither
the tool nor a codec library is bundled with the application.

`tests/data/paced-audio-h264.mp4` is an original first-party synthetic test
pattern and 440 Hz sine tone, generated with the owner's separately installed
FFmpeg/libx264 and native FFmpeg AAC encoder. It contains no third-party footage,
music or fonts, follows the project's contribution/license terms, and is
test-only. Generation, exact bytes and its silent use are recorded in
`docs/video_audio_preroll_repair.md`; no generation tool or codec library is
bundled with the application.
