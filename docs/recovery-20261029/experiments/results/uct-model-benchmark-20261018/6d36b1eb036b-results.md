# One-time UCT model benchmark — 2026-10-18

Measured on NVIDIA RTX 4090 (24 GB), driver 617.42, AMD Ryzen 9 5900X, Windows. Source commit `5b85bfcea0ca01e108401b518bdcdfbfb304bd06`; MSVC 14.44.35207 / CUDA 12.9.86 / Qt 6.12.0 / TensorRT 11.4.0.106 / ONNX Runtime CPU 1.31.0.

## Result

Both medium networks are practical on this RTX 4090: warmed end-to-end recognition medians are 12.7–34.4 ms on these four images, versus 9.1–23.6 ms for the current tiny-detector/small-recognizer pair. The medium pair is approximately 40–46% slower here and uses roughly 912 MiB of observed process GPU memory versus 554 MiB. This is recognition latency, not render FPS or camera-to-photon latency.

The initial forward-order pass had shorter warmup and followed new-engine compilation. Its larger-model apparent speedup did not survive the uniformly warmed reverse-order comparison. Keep those original files as order/clock evidence, not as the headline comparison. No GPU clock, power or desktop preference was changed.

## Warm GPU measurements

Each cell is median / p95 milliseconds; 31 samples per input, at least 2 seconds of warmup before each input, separate process per pairing, all engines cached. GPU model order: medium/medium, medium/small, tiny/medium, tiny/small.

| Detector / recognizer | MIT title | Clean board | Low-resolution board | Dense page | Sampled GPU process MiB |
|---|---:|---:|---:|---:|---:|
| Current: tiny / small | 16.2 / 20.5 | 9.1 / 18.3 | 9.1 / 10.2 | 23.6 / 29.4 | 554 |
| Tiny / medium | 18.8 / 21.1 | 10.5 / 13.3 | 11.4 / 13.6 | 30.3 / 36.1 | 658 |
| Medium / small | 21.4 / 26.4 | 11.7 / 13.3 | 11.5 / 14.1 | 26.0 / 32.2 | 806 |
| Medium / medium | 23.0 / 25.7 | 12.7 / 14.6 | 12.7 / 13.6 | 34.4 / 40.8 | 912 |

Memory uses Windows PDH GPU Process Memory Dedicated Usage for only the benchmark PID, sampled after each timed OCR call. Values include CUDA/TensorRT context, engines, working buffers and owned input; they are observed steady-run peaks, not a guarantee of absolute allocation peaks. Empty CUDA-context baselines varied around 244–336 MiB between processes. Global GPU memory counters are diagnostic only because other desktop GPU work remains active. No inference is delegated to another GPU.

## CPU fallback

Production ONNX Runtime CPU settings: 2 intra-op threads, 1 inter-op thread, sequential execution. Nine measured calls per input, three warmups. CPU runs are not a GTX 1070 measurement; the CUDA context created by this instrumentation is outside the timed CPU calls.

| Pairing | MIT title ms | Clean board ms | Low-resolution board ms | Dense page ms |
|---|---:|---:|---:|---:|
| Current: tiny / small | 383.3 | 243.8 | 248.4 | 848.5 |
| Medium / medium | 2318.1 | 1392.1 | 1381.8 | 2576.5 |

Medium CPU latency was approximately 3–6 times the current pair. Retain a lightweight CPU fallback rather than assuming the GPU-quality tier belongs on every processor.

## Recognition observations

- All four GPU pairings returned the literal `I` in `Dynamics and Control I` for the MIT frame. The overlay omission is therefore the independent high-risk-token filter, not a missing dictionary entry on this frame.
- Medium detection also recovered the three small upper-left logo lines (`Massachusetts / Institute of / Technology`) that tiny detection missed. It therefore processes more recognized lines on this image (8 versus 5); these timings include that real workload difference.
- Neither medium nor small recognition read the very small lower logo subtitle faithfully. Both still made I/l or spacing mistakes on controlled inputs. These four images are not a representative accuracy benchmark and establish no general accuracy ranking.

## Exact raw GPU output

### tiny-small

- **MIT title, 320×240**: 2.003J/1.053J / Dynamics and Control I / Fall 2007 / MITOPENCOURSEWARE / MISSOICHUSETIS LNSIAIUTE OP TECHNULOGY
- **Clean board, 1280×720**: Dynamics and Control I / 2.003J/1.053J / Fall 2007
- **Low-resolution board, 1280×720**: Dynamics and Control I / 2.003J/1.053J / Fall 2007
- **Dense page, 1920×1080**: INTRODUCTION TO DYNAMICS AND CONTROL I / A clear view helps every student follow the lecture. / Mass and acceleration determine the resulting force. / Read the measurements carefully before calculating. / The initial position is 12.5 and the velocity is 42. / Compare cases I, Il and III with experiment 1070. / Numbers: 0123456789 Symbols: + - = () / % / Temperature 25.0 degrees; frequency 60 Hz. / A steady camera preserves detail across the page. / Use the original image when a character is uncertain. / Printed text stays sharp while the viewport moves. / MAGNIFIED PRINTED TEXT READS CLEARLY TODAY

### tiny-medium

- **MIT title, 320×240**: 2.003J/1.053J / Dynamics and Control I / Fall 2007 / MITOPENCOURSEWARE / MISSOCHUSETIS INSTRTUTE UP TECHNULOGY
- **Clean board, 1280×720**: Dynamics and Control I / 2.003J/1.053J / Fall 2007
- **Low-resolution board, 1280×720**: Dynamics and Control I / 2.003J/1.053J / Fall 2007
- **Dense page, 1920×1080**: INTRODUCTION TO DYNAMICS AND CONTROL I / A clear view helps every student follow the lecture. / Mass and acceleration determine the resulting force. / Read the measurements carefully before calculating. / The initial position is 12.5 and the velocity is 42. / Compare cases I, Il and IIl with experiment 1070. / Numbers: 0123456789 Symbols: + - = ( ) / % / Temperature 25.0 degrees; frequency 60 Hz. / A steady camera preserves detail across the page. / Use the original image when a character is uncertain. / Printed text stays sharp while the viewport moves. / MAGNIFIED PRINTED TEXT READS CLEARLY TODAY

### medium-small

- **MIT title, 320×240**: Massachusetts / Institute of / Technology / 2.003J/1.053J / Dynamics and Control I / Fall 2007 / MITOPENCOURSEWARE / MISSOCHUSETIS LNSIATUTE OP TECHNOLOGY
- **Clean board, 1280×720**: Dynamics and Control / 2.003J/1.053J / Fall 2007
- **Low-resolution board, 1280×720**: Dynamics and Control I / 2.003J/1.053J / Fall 2007
- **Dense page, 1920×1080**: INTRODUCTION TO DYNAMICS AND CONTROL I / A clear view helps every student follow the lecture. / Mass and acceleration determine the resulting force. / Read the measurements carefully before calculating. / The initial position is 12.5 and the velocity is 42. / Compare cases I, Il and III with experiment 1070. / Numbers: 0123456789 Symbols: + - = ( ) / % / Temperature 25.0 degrees; frequency 60 Hz. / A steady camera preserves detail across the page. / Use the original image when a character is uncertain. / Printed text stays sharp while the viewport moves. / MAGNIFIED PRINTED TEXT READS CLEARLY TODAY

### medium-medium

- **MIT title, 320×240**: Massachusetts / Institute of / Technology / 2.003J/1.053J / Dynamics and Control I / Fall 2007 / MITOPENCOURSEWARE / MOSSOCHUSETIS INSTETUTE OP TECHNOLOGY
- **Clean board, 1280×720**: Dynamics and Control I / 2.003J/1.053J / Fall 2007
- **Low-resolution board, 1280×720**: Dynamics and Control I / 2.003J/1.053J / Fall 2007
- **Dense page, 1920×1080**: INTRODUCTION TO DYNAMICS AND CONTROL I / A clear view helps every student follow the lecture. / Mass and acceleration determine the resulting force. / Read the measurements carefully before calculating. / The initial position is 12.5 and the velocity is 42. / Compare cases I, Il and III with experiment 1070. / Numbers: 0123456789 Symbols: + - = ( ) / % / Temperature 25.0 degrees; frequency 60 Hz. / A steady camera preserves detail across the page. / Use the original image when a character is uncertain. / Printed text stays sharp while the viewport moves. / MAGNIFIED PRINTED TEXT READS CLEARLY TODAY

## Preparation and model storage

Actual downloaded ONNX weights: current detector 1,780,590 bytes, current recognizer 21,159,378 bytes; medium detector 62,032,837 bytes and medium recognizer 76,554,979 bytes (138.59 MB combined). Published Paddle-native sizes differ from ONNX export sizes.

New medium recognizer first preparation took 98.4 seconds (the tiny detector was already cached); new medium detector took 59.7 seconds (the small recognizer was already cached). Both-medium reuse then took 3.8 seconds in the first pass. Final cached startup for each pairing is retained in its JSON. Preparation includes loading and compilation; it is not pure kernel time. Existing engines were copied into a separate benchmark cache and never deleted or rebuilt to obtain another cold measurement.

Medium detector: `PaddlePaddle/PP-OCRv6_medium_det_onnx`, revision `61323801669c338b7891481ec7bac61ce31b576a`; SHA-256 `eb13b44b25bb36f89528b68720af8a61d9cf381176107f465db1757b65d086e1`.

Medium recognizer: `PaddlePaddle/PP-OCRv6_medium_rec_onnx`, revision `50c7eacafc52fa7bcf4194e8cd08e46f8558504b`; SHA-256 `9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba`.

Both upstream model cards declare Apache-2.0. Complete cards, configs, upstream metadata, exact hashes and the original PaddleOCR license are retained privately. Nothing is bundled or published.

## Method and limits

- This is an isolated Release C++ harness using the production TensorRT/CPU implementations, CUDA kernels and AnalysisPool from the source commit. Only two private source copies differ: model/config SHA expectations come from the reviewed per-profile manifest, and TensorRT cache location points to the private benchmark directory. No production guard or model pin was changed.
- Identical pixel hashes were checked across every measured pairing and CPU/GPU mode. The MIT image is the existing local lecture at 5 seconds, 320×240. The three other inputs are generated with registered Arial; the low-resolution board is the clean board downsampled to 320×180 then upscaled. Input upload and creation of the owned GPU snapshot happen before the timer.
- Timed GPU work includes production normalization, detection, connected regions, rectification, per-region recognition, CTC reduction, compact metadata synchronization and string construction. Neural tensors remain device resident. Float32 graphs use TensorRT default math/tactics, with no explicit FP16/INT8 conversion. Detector long edge is capped at 960; recognition crops are 48×1024; 256 MiB build workspace, builder optimization level 2, no auxiliary streams.
- CPU timing includes its production preprocessing, detection, recognition, output copying and decoding.
- Camera capture, display presentation, UCT validation/temporal acceptance, glyph layout/paint, final reference/color preparation and native GUI behavior are outside this benchmark. No claim of optical accuracy, 120 FPS rendering or older-GPU performance follows.
- Windows GPU clocks remain unlocked and other desktop processes remain active. Values are observations on this machine, not isolated lab results. Original forward pass, per-sample timings, memory diagnostics, console logs and manifests are preserved.
- The first private build failed due to a quoted trailing Windows path and VS discovery environment; `build.txt` is retained. `build-repaired.txt` and `build-warm.txt` show successful corrected builds. No project test gates were rerun for this offline, private benchmark.
- Registered Arial rendered the synthetic inputs. Qt emitted its existing offscreen missing-default-font-directory notice; no native window was opened.

## Evidence inventory

`gpu-warm-*.json` are the final warmed GPU comparison; `gpu-*.json` without `warm` retain the initial forward pass; `cpu-*.json` retain CPU fallback. `run-manifest.json`, `run-warm-manifest.json`, `profiles.json`, `download-manifest.jsonl`, `source-provenance.jsonl` and build logs bind the inputs and executable. `summary.json` is derived; this report and the inline view use the final warm GPU pass.

The owner app, its configured OCR files, settings, private OBS feed and phone display were not changed. The repository working tree remains clean.
