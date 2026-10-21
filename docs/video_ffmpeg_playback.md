# Optional embedded selected-video playback (development)

This saved-file prototype links to public FFmpeg 7.1.1 APIs at runtime and
explicitly chooses BSD-2-Clause dav1d 1.5.1 for AV1. It runs inside OkuFlow's
existing selected-video worker. Media Foundation remains the default and the
open-time fallback. Qt 6.12's separate FFmpeg 9 DLLs do not supply dav1d and are
neither overwritten nor substituted. No executable is launched per frame, no
file is transcoded, and the owner's installed full GPL FFmpeg is not deployed.

## Build and opt in

Use PowerShell 7 with an existing private MSYS2 UCRT64 GCC 16.2.0 toolchain
(the source-built adb toolchain is suitable), Python with venv/pip, Windows tar,
MSVC 2022 and Qt 6.12.0. The recipe downloads only hash-pinned sources and
private build tools, verifies reused source bytes against the archives, and
builds shared libraries without programs, encoders, muxers, filters, devices,
network protocols, hardware acceleration, GPL or nonfree components. Decoders,
demuxers and parsers come from the upstream library inventory rather than an
application-maintained codec workaround list. Codec/format coverage still needs
actual acceptance; this is not a claim that every video can play.

```powershell
pwsh -NoProfile -File scripts/prepare_ffmpeg_playback.ps1
$sdk = "$env:LOCALAPPDATA/OkuFlow/embedded-video-sdk/sdk"
$build = "$env:LOCALAPPDATA/OkuFlow/embedded-video-prototype-build"
# In the documented VS2022 x64 developer environment:
cmake --preset msvc-cpu -B $build "-DOKUFLOW_FFMPEG_SDK_ROOT=$sdk"
cmake --build $build --target oku_flow video_playback_tests video_audio_output_tests
$env:OKUFLOW_VIDEO_BACKEND = 'ffmpeg'
$env:OKUFLOW_FFMPEG_RUNTIME = "$sdk/bin"
# Launch only your independently built app in an authorized desktop session.
```

Both an explicit SDK at configure time and the runtime environment selection
are required. No SDK means a compiled native fallback stub. Missing/incompatible
DLLs or unsupported open-time inputs retain the refusal reason and reopen an
independent cursor on the **same held backing**, then use Media Foundation.
One successful runtime identity, or its initialization failure, is retained per
process. Change runtime identity by normally restarting an authorized test app.
`decoderBackend` identifies actual use separately from `backendReason`.

The SDK root, source/build/tool caches, exact configuration, original notices,
runtime import/hash inventory and `okuflow-ffmpeg-sdk.json` stay under
LocalAppData. The recipe never installs a system package, changes Qt, launches
OkuFlow or touches a release bundle. GNU make/diffutils and Meson are build-only.
See [third-party terms](THIRD_PARTY_LICENSES.md#optional-embedded-ffmpegdav1d-development-sdk).
The source recipe is reproducible from pinned inputs; bit-identical compiler
output across machines has not been established.

## Authority, I/O and bounded ownership

The adapter implements only the synchronous SourceReader operations used by
`VideoPlayback`. A private COM interface arms deadlines/cancellation; foreign
Windows objects are identified with QueryInterface, never C++ RTTI. There is
one serial worker, the existing process-wide retirement permit and one latest
replacement request. Open, seek, neighbor, End and audio-aligned Resume retain
the repaired progress window/absolute cap described in
[the baseline repair](video_audio_preroll_repair.md). Custom AVIO checks the
request and deadline before/after each held-stream read, before seeks and
between decoder operations. A blocked filesystem/library/native call cannot
be forcibly canceled; its existing worker/state remain owned, without a UI join
or a replacement decoder on another worker.

Video and audio use two independent cursors cloned from the exact stable file.
Each has one demuxer/decoder, AVFrame and AVPacket, so sparse/VFR video cannot
accumulate a queue of compressed audio (or vice versa). File bytes never pass
through an ambient URL opener: protocols are disabled and secondary `io_open`
requests are refused. Reads use 64 KiB AVIO storage, probing/index limits are
8 MiB, streams are capped at eight, packets are checked at 64 MiB, and each
decode step has a finite 65,536-packet guard plus the armed wall deadline.
The packet check occurs after upstream allocation; these are not hard caps on
all demuxer/codec internal memory. Persistent and metadata-probe decoder
threads are limited to four,
dimensions to 16,777,216 pixels and audio decoded samples to 131,072.

The existing adaptive 96–256 MiB staging/immutable-pixel quota includes each
BGRA output and consumer lease. Preroll before an ordinary seek target avoids
BGRA conversion/allocation. Codec reference pictures, threads, demuxer metadata
and audio conversion are additional internal allocations, just as native
decoder internals are outside the pixel quota. Report process memory separately.
End currently converts/copies every candidate and is slower than an ordinary
targeted seek. There is no unbounded raw-frame history or retired decoder list.

The video stream start PTS defines the normalized media origin for **both**
tracks. The selected sample retains actual normalized PTS/duration, stream ID
and existing issuance seal. PCM stays interleaved signed 16-bit at its source
rate/layout. Selected-cursor trimming, generation revocation, the 200 ms
high-water, 500 ms/2 MiB/32-packet queues, consumed audio clock, pause, mute and
the existing output volume path stay owned by the production player/output.
No microphone or audio endpoint is created by the new fixtures.

Opening retains the exact request generation supplied by the worker; a request
that was superseded before admission cannot adopt its successor's cancellation
ticket. The video demuxer's successfully read stream inventory decides whether
an audio cursor is required. Any failure opening an existing audio track
refuses the adapter, preserving native fallback instead of silently dropping
the track. Exhausting the container probe's 1,024 top-level or child boxes also
refuses admission; an unfinished scan is not proof of a non-fragmented file.

CPU BGRA follows the existing upload/presentation path. It receives the same
selected frame lease used by GPU effects, UCT, annotation and save operations;
the adapter never creates a second presentation or OCR authority. Resident
FFmpeg GPU decoding, GPU transfer performance and actual UCT recognition were
not implemented or measured. Existing native GPU quarantine rules are untouched.

## Explicit limitations

`OpenArchive`, including finalized archive facades, always uses Media Foundation
and its producer-fenced prefix validation. Growing recordings, Waiting/resume
and writer mutation ownership are not claimed as FFmpeg support. A finalized
ordinary file selected with `OpenFile` is eligible. Saved **fragmented MP4** also
uses native fallback: a strict regression found different Windows/FFmpeg PTS
precision for identical tail samples (for example 40,333,000 versus 40,333,333
ticks). A bounded held-stream box probe checks `moof`/`moov/mvex`; it does not
infer fragmentation from a codec, filename or media duration. The original
strict failure is retained rather than rounding timestamps or relaxing equality.
Resolving that source timeline difference is required before enabling it.
Inputs lacking header stream metadata, secondary resources such as playlists,
arbitrary display transforms and runtime format changes
are unsupported. Open-time refusal can fall back; a later decode failure keeps
the previous selected image and reports failure, without silently changing
decoder/sample authority mid-request.

Swscale converts the signaled YUV matrix/range to opaque BGRA. It does **not**
perform PQ/HLG transfer conversion, wide-gamut display color management or HDR
tone mapping. The private PQ/BT.2020 experiment below compares code-value
images only. It does not certify faithful HDR rendering. Keep the backend
opt-in until color, sound and hardware/native acceptance are complete.

## Finite validation and measurements

The parent review added two refusal regressions. Before repair, both failed:
a fragmented container hidden behind more than 1,024 boxes bypassed the probe,
and a failed audio cursor was accepted as silent video despite known audio
metadata. The original failing gate is retained under
`LocalAppData/OkuFlow/ffmpeg-parent-review-20261021/`. These are separate from
the child's finite seek measurements below.

Parent review also found a shared native/embedded Pause/Resume race: the worker
could consume restart before its held-frame rewind intent, then leave a pending
decoder reposition at EOS with no restart left to perform it. Both flags now
transfer in one locked handoff, and End publication checks for a newer Resume.
The original native EOS failure and the subsequent disk-full gate remain
preserved; after-change validation is recorded separately from the child's
earlier successful prototype run. The four-second fixture wait is unchanged.

After these parent repairs, seven focused SDK/PCH-ON cases passed: native
playback, embedded playback (including both refusal regressions), 4K retained
leases, archive fallback, missing-runtime fallback, audio output and shared-source
equivalence. A separate no-SDK/PCH-OFF build passed native playback, audio output
and shared-source equivalence. The respective ledgers are
`DB3D2EF7538D86BAEBF3845B83331B087F93A11D6FD045E68DAF5A37F3FE60B7`
and `D39910BF5E77D1DA3E9020B5E236B43A934E9E7509A4A2BC0C60544DE5C30670`
under `LocalAppData/OkuFlow/test-evidence/`. Fresh gates used the current runtime
after resume refused a changed PATH/codec-opt-in environment; the original
unfinished disk-full records remain in history. Preprocessing outputs from that
failure are preserved separately in the SDK build. These are SCOPED background
checks, not full matrix, native GUI, audible-device or release acceptance.

The SDK-enabled configuration registers desktop-free codec cases for actual
embedded decoding, missing-runtime native fallback, growing-archive native
fallback and 4K retained-lease pressure. The production-reader regressions
cover scene cuts/real neighbors, latest-seek supersession, exact frame issuance,
same-frame reuse, PCM floors/pacing/continuity, callback cancellation, expired
I/O deadlines and final lease retirement. Run the tracked gate serially:

```powershell
pwsh -NoProfile -File scripts/run_test_gate.ps1 -BuildDirectory $build -ExcludeGui `
  -Test video_playback_contract,video_playback_embedded_contract,video_playback_embedded_missing_runtime,video_playback_embedded_archive_fallback,video_playback_embedded_4k,video_audio_output_contract
```

For a one-time exact-source comparison, `scripts/compare_video_backends.ps1`
requires an explicit test executable, file and private runtime. It acquires the
global tracked-runner lease, refuses active parent gates, writes source/exe/DLL
hashes and separate original stdout/stderr, and allows at most 180 seconds.
Only its own diagnostic child may be stopped at that deadline. `-Mode Compare`
requires strict normalized PTS/sample identity at 6 s and 23 s and reports RGB
differences; `-Mode Playback` exercises supersession, three seconds of silent
PCM/video and actual End. It never starts a GUI or an audio endpoint.

The final-adapter comparison on the owner's exact 89.5 s 2288×1080 10-bit
AV1/PQ/BT.2020/AAC clip measured:

| Target | Native CPU | Embedded dav1d CPU | Selected normalized PTS | RGB mean absolute difference / PSNR |
| --- | --- | --- | --- | --- |
| 6 s | 2,533.502 ms | 514.569 ms | 60,000,000 ticks, both | 0.901002 / 46.5364 dB |
| 23 s | 9,677.088 ms | 1,968.665 ms | 230,000,000 ticks, both | 0.917745 / 46.5712 dB |

Both used 181/691 video reads and copied one selected frame. Maximum channel
difference was 10/255. Peak accounted pixel bytes were 29,652,480 native and
49,420,800 embedded while comparison images were deliberately retained; the
combined diagnostic process peaked at 281,407,488 working-set bytes. These are
one-run warm-host worker-publication measurements, not a latency distribution,
display latency, total decoder memory bound or release benchmark. Private raw
receipts are under `%LOCALAPPDATA%/OkuFlow/embedded-video-evidence/`.

The separate `private-av1-playback-accepted` receipt measured 388.719/1,477.310 ms
seeks, 455 ms unmuted-Resume preroll (PCM remained silent in the fixture),
619,324 PCM bytes over three seconds, 269.8669 ms maximum delivery lead and
**one 100-ns tick** maximum PCM boundary difference. Actual End took
22,090.619 ms / 2,685 reads at PTS 89.4333333 s. Peak process working set was
189,603,840 bytes. Supersession, exact PCM floor/generation, continuity and
endpoint assertions passed. Earlier receipts are retained. The measured
executable hashes identify those runs; a later native-only diagnostic-format
regression is validated separately. Do not substitute the old external
executable probe for these integrated paths.

The final SDK-enabled CPU application build and seven-case tracked headless
gate passed with PCH ON. A separate no-SDK/PCH-OFF playback build, native
contract and shared-source equivalence check passed. `agent_check.ps1`
correctly reported FULL required for CMake infrastructure; no duplicate full
matrix was run. Exact SDK gate ledger:
`test-evidence/57AA218A782D1FE3FF2E6C96AD67BF538F9C3ACFA202B82B20B9E831C9029321/ledger-v2.json`;
no-SDK ledger:
`test-evidence/635BD48592CB8FDB19CF76567F486556ED6A5D9FB6D2EBD35EC70E58CCA9D987/ledger-v2.json`,
both under LocalAppData/OkuFlow. Original failed attempts remain archived.

Original failed receipts are preserved, including an initial foreign-COM RTTI
identification defect repaired with QueryInterface. Local passing checks are
SCOPED, not the complete Windows release/CPU/CUDA/bundle matrix. No native GUI,
browser, audio endpoint, OCR model preparation, owner app or parent artifact was
used. Public deployment requires exact source/config/notice/SBOM and applicable
LGPL replacement/relinking review in addition to normal release acceptance.

Primary API references: [FFmpeg licensing/checklist](https://ffmpeg.org/legal.html),
[7.1 custom AVIO](https://ffmpeg.org/doxygen/7.1/avio_8h.html),
[7.1 send/receive API](https://ffmpeg.org/doxygen/7.1/group__lavc__encdec.html),
[dav1d source and license](https://code.videolan.org/videolan/dav1d/-/tree/1.5.1).

## Integration inventory

The prototype is integrated into the development branch. Configure the optional
SDK only in a fresh private build, using the commands above. Omit both opt-ins
to keep normal native behavior. The complete tracked matrix and existing release
gates remain required before publishing; focused private builds do not replace
the owner's running artifact.

Exact changed files:

```text
CHANGELOG.md
agents.md
cmake/CMakeLists.txt
cmake/FfmpegPlayback.cmake
docs/README.md
docs/THIRD_PARTY_LICENSES.md
docs/code_reference.md
docs/developer_guide.md
docs/hardcoded_paths.md
docs/video_ffmpeg_playback.md
include/okuflow/common/README.md
include/okuflow/common/video_playback.hpp
scripts/compare_video_backends.ps1
scripts/ffmpeg/pins.json
scripts/prepare_ffmpeg_playback.ps1
src/common/README.md
src/common/video_ffmpeg_reader.cpp
src/common/video_ffmpeg_reader.hpp
src/common/video_playback.cpp
tests/README.md
tests/data/embedded-pool-4k.mp4
tests/video_playback.cmake
tests/video_playback_tests.cpp
```
