# ML Text Super-Resolution Options — Pre-Trained Models Only (2026-07)

Research report for filling the `OKUFLOW_ENABLE_TEXT_SR` integration boundary
(`cmake/ProjectOptions.cmake`, adapter stub around `src/cuda/cuda_interop.cpp:1653`)
with a **pre-trained, downloadable** model. Constraints applied to every option:

- **No training.** Weights must be downloadable today (GitHub release, HuggingFace, vendor SDK).
- **License of code AND weights** must be compatible with OkuFlow's GPL-3.0 +
  commercial dual license → commercial redistribution required. Apache-2.0 / MIT /
  BSD = good. No-license repos, CC-BY-NC, research-only, "contact us" = **blocked**.
- **Latency budget:** ~640×360 ROI, 2×–4× upscale, **< 8 ms** on an RTX 30/40-series
  *laptop* GPU, TensorRT FP16 (INT8 optional).
- **Deployment:** ONNX → TensorRT engine preferred (we are already CUDA-resident);
  ONNX Runtime + DirectML noted as the future non-NVIDIA path; vendor SDK acceptable.
- **Text suitability:** glyph edges, stroke contrast, no ringing/hallucination.

Latency figures marked *(est.)* are derived from parameter counts / FLOPs scaled to
640×360 input, assuming TensorRT FP16 on an RTX 3060 Laptop GPU (~10.7 TFLOPS FP32,
realistically ~15–20 TFLOPS sustained FP16 tensor throughput on small convnets).
Figures marked *(vendor)* or *(published)* are measured numbers from the source.
Treat all *(est.)* numbers as ±2× until we benchmark on real hardware.

---

## 1. TL;DR table

| Option | Params / size | Est. latency @640×360→2× (TRT FP16, 3060-class) | Weights license | Commercial OK? | Text quality expectation | Integration effort |
|---|---|---|---|---|---|---|
| **NVIDIA Maxine VFX SuperRes** | SDK, models via NVIDIA installer (~400 MB) | **3.28 ms @2060, 1.54 ms @3080 for exactly 360p→720p** *(vendor)* → ~2–3 ms on 3060 laptop | NVIDIA SLA (proprietary, commercial use + end-user redistributable installer) | **Yes, with SLA review** | Good: artifact reduction + detail enhancement, tuned for real video | Low-medium (CUDA-buffer native API, no TRT plumbing needed) |
| **Real-ESRGAN compact `realesr-general-x4v3`** | 1.21 M (SRVGG, 32×64ch convs) + wdn denoise pair | ~25–45 ms *(est.)* — **over budget at full ROI**; ~7–12 ms at 320×180 deep-zoom ROI | BSD-3-Clause (repo release v0.2.5.0) | **Yes** | Very good on camera text; denoise blend is valuable | Medium (ONNX → TRT, engine cache) |
| **`realesr-animevideov3`** (same family, 16 convs) | ~0.6 M | ~12–20 ms *(est.)* — borderline, OK on 40-series / smaller ROI | BSD-3-Clause | **Yes** | Good on glyphs (line-art bias helps text) | Medium |
| **RLFN (NTIRE 2022 runtime winner)** | 0.317 M, 19.7 GFLOPs @256² | ~4–7 ms *(est.)* (69 GFLOPs @640×360) | Apache-2.0 (bytedance/RLFN, weights in repo) | **Yes** | Good general SR; not text-specialized | Medium (ONNX → TRT) |
| **SPAN (NTIRE 2024 winner, 2026 baseline)** | 0.48 M | ~5–9 ms *(est.)* — borderline | Apache-2.0 (weights via Google Drive) | **Yes** | Good; slightly better PSNR than RLFN | Medium |
| **ESPCN / FSRCNN (classic tiny)** | 12–25 K | **< 1 ms** *(est.)* | Apache-2.0 (TF re-impls: fannymonori/TF-ESPCN, Saafke/FSRCNN_Tensorflow) | **Yes** | Mild improvement over bicubic; no hallucination risk | Low (tiny ONNX; could even hand-port to CUDA) |
| **Anime4K CNN shaders (M/L/UL)** | 2.7 K (M) – ~100 K (UL) | **< 0.5–2 ms** *(est.; runs ~1–3 ms @720p on mid GPUs as GLSL)* | **MIT** (shaders = the weights) | **Yes** | Surprisingly good on glyphs/strokes (line-art CNN); no ringing | Low-medium (port GLSL weights to CUDA kernels — no runtime dep at all) |
| **OpenVINO `text-image-super-resolution-0001`** | **0.003 M**, 1.379 GFLOPs @360×640, ×3 | **< 0.5 ms** *(est.)* | Apache-2.0 (Open Model Zoo) | **Yes** | Text-specific (scanned docs); mild, safe | Medium (IR→ONNX conversion friction) |
| **RTX Video SDK (VSR as SDK)** | in-driver models, thin SDK | real-time by design (browser VSR does 1080p→4K @60fps on 3060) | NVIDIA SDK license (developer account; attribution clauses) | **Yes, with SLA review** | Good deblock/edge restore; tuned for compressed video, not glyph-aware | Low (native **D3D12** support — zero interop for our presenter) |
| SwinIR-light / SwinIR / HAT | 0.9 M–20 M transformer | **~150 ms – seconds** (SwinIR-S: 474.7 ms @256² V100 FP32 *(published)*) | Apache-2.0 | Yes (license fine) | Excellent quality — irrelevant, can't hit budget | **Rejected: too slow** |
| TextZoom line: TSRN / TBSRN | ~3–5 M + recognizer | 16×64 word crops only — N/A for live ROI | **No license** (TextZoom, FudanOCR repos) | **BLOCKED** | Best-in-class on word crops | Rejected |
| TATT / TPGSR | MIT code | word-crop + recognizer-prior architecture, N/A live | MIT code; full SR checkpoints not published, recognizer weights external, TextZoom-trained | **Blocked in practice** | — | Rejected |
| DLSS / Streamline | — | — | NVIDIA (redistributable) | — | — | **Rejected: requires motion vectors + depth + jittered rendering** |
| waifu2x / realsr ncnn-Vulkan | MIT code; models MIT (waifu2x) / Apache-2.0 (RealSR) | RRDB-class models too slow; runtime is Vulkan | MIT / Apache-2.0 | Yes | OK | Rejected: wrong runtime for our D3D12/CUDA stack |

---

## 2. Per-option detail

### 2.1 NVIDIA Maxine Video Effects SDK — SuperRes / Upscale  ⭐ primary candidate

- **Repo (headers/samples, MIT):** <https://github.com/NVIDIA-Maxine/Maxine-VFX-SDK> — latest release v0.7.6 (2025-06); no deprecation notice. Runtime + models come from the SDK installer on the [NVIDIA Maxine End-user Redistributables page](https://catalog.ngc.nvidia.com/orgs/nvidia/maxine/collections/maxine_vfx_sdk_collection_ga/-) / NGC.
- **License:** SDK API headers and proxy-loader source are MIT. The **binaries + models are under the NVIDIA Software License Agreement** (+ Product-Specific Terms for NVIDIA AI Products) — proprietary but explicitly designed for commercial apps: NVIDIA documents that a developer "can package the runtime dependencies into the application or require application users to use the SDK installer". Action item: read the current SLA once before shipping; this is *not* an open-weights option, but it is a shippable one. (Precedent: OBS plugins/StreamFX ship against it.)
- **Performance (vendor-published, exactly our workload):** SuperRes 2× **360p→720p: 3.28 ms on RTX 2060, 1.54 ms on RTX 3080, 0.505 ms on RTX 4090** — <https://docs.nvidia.com/maxine/vfx/WindowsVFXSDK/PerformanceReference.html>. A 3060 laptop lands ~2–3 ms → comfortably inside the 8 ms budget. Scales 4/3×, 1.5×, 2×, 3×, 4×; two modes (0 = conservative, 1 = strong enhancement). The separate cheap **Upscale** effect (edge-adaptive, non-DNN) is sub-ms and duplicates what NIS already gives us — ignore it.
- **Deployment on our stack:** best-in-class fit. `NvVFX_Run` consumes `NvCVImage`, which wraps **CUDA device buffers directly** — our pipeline is already CUDA-resident, so no D3D12 interop, no ONNX, no TensorRT plumbing, no engine build. Effect load ≈ seconds at startup (its own TRT engines are pre-baked per-arch in the installer). GPU support: Turing/Ampere/Ada/Blackwell w/ Tensor Cores, driver ≥ 521.98, Windows 10+.
- **Gotchas:** ~400 MB runtime (either bundled per SLA or user-installed via NVIDIA's installer — recommend the latter); NVIDIA-only (acceptable today — the whole pipeline is CUDA — but it dies with a future DirectML backend); models are a black box (no INT8/tuning control); quality mode 1 can over-enhance noise — expose our blend-strength slider on top.

### 2.2 Real-ESRGAN family — the open-weights quality option

- **Repo:** <https://github.com/xinntao/Real-ESRGAN> — code **BSD-3-Clause**; weights published as GitHub release assets under the same repo/license (Qualcomm AI Hub also redistributes `Real-ESRGAN-General-x4v3` and lists the model license as BSD-3-Clause, corroborating that the weights are treated as BSD).
- **Weights (release v0.2.5.0):**
  [realesr-general-x4v3.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth),
  [realesr-general-wdn-x4v3.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-wdn-x4v3.pth) (denoise pair — interpolate state dicts for a runtime denoise-strength knob),
  [realesr-animevideov3.pth](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-animevideov3.pth).
  Architecture: `SRVGGNetCompact` (num_feat=64, PReLU; 32 convs for general-x4v3 ≈ **1.21 M params**, 16 convs for animevideov3 ≈ 0.6 M).
- **Latency honesty:** compute scales with LR pixel count: general-x4v3 ≈ 2.4 MFLOPs/px → ~545 GFLOPs at 640×360 → **~25–45 ms TRT FP16 on a 3060 laptop — over budget**. BUT: at high magnification the visible ROI shrinks (4× digital zoom of a 720p feed = 320×180 source region → ~136 GFLOPs → **~7–12 ms**). So general-x4v3 is viable *only* as a "deep zoom" tier, engaged when zoom ≥ ~3×. `animevideov3` (16 convs) halves cost and its line-art bias actually suits glyphs; borderline at full ROI, fine on 40-series.
- **Deployment:** clean ONNX export (community exports exist: [phineas-pta/RealESRGAN-trt-win](https://github.com/phineas-pta/RealESRGAN-trt-win), [Qualcomm AI Hub ONNX](https://huggingface.co/qualcomm/Real-ESRGAN-General-x4v3)); dynamic-shape TRT engine, FP16 is lossless in practice; INT8 needs calibration (use synthetic text crops). RRDB variants (`RealESRGAN_x4plus`, `x2plus`, 16.7 M) are offline-only — hundreds of ms; do not ship for live video.
- **Gotchas:** GAN-trained → can hallucinate stroke detail on very low-res text; mitigate with the wdn blend and our strength slider. Trained on DIV2K/Flickr2K etc. — the *weights* are what we redistribute, released under BSD-3 by the authors; training-data provenance is their statement, not our liability, but worth noting in THIRD_PARTY_LICENSES.md.

### 2.3 NTIRE efficient-SR class: RLFN and SPAN — the open real-time sweet spot

- **RLFN:** <https://github.com/bytedance/RLFN> — **Apache-2.0**, pretrained ×4 checkpoint in-repo. 0.317 M params, 19.7 GFLOPs @256×256 LR, 27.11 ms PyTorch FP32 (challenge V100 measurement). Scaled to 640×360: ~69 GFLOPs → **~4–7 ms TRT FP16 on 3060 laptop** *(est.)* — inside budget. Plain conv topology (its whole point is TRT/mobile-friendly ops; it won the NTIRE 2022 *runtime* track).
- **SPAN:** <https://github.com/hongyuanyu/SPAN> — **Apache-2.0**, checkpoints via Google Drive links in README. 0.48 M params; won NTIRE 2024 overall+runtime, and is the *baseline* of the NTIRE 2026 ESR challenge. ~5–9 ms *(est.)* — borderline; prefer RLFN if we need headroom, SPAN if we want the extra PSNR.
- **Hunting ground for newer drops:** [NTIRE2025_ESR](https://github.com/Amazingren/NTIRE2025_ESR) (MIT, "checkpoints and models of all the solutions are uploaded", baseline EFDN: 0.276 M / 16.69 ms FP32) and [NTIRE2026_ESR](https://github.com/Amazingren/NTIRE2026_ESR). Caveat: the umbrella repo is MIT but **verify the individual team's LICENSE before adopting any specific 2025/2026 entry** — several teams attach their own terms.
- **Deployment:** textbook ONNX → TRT; all-conv (RLFN) exports cleanly. ×4 nets with a ×2 need: run ×4 and downsample, or use their ×2/×3 checkpoints where published.
- **Text suitability:** general-purpose SR trained on DIV2K-class data — crisper than bicubic/NIS, no glyph awareness, minimal hallucination (PSNR-oriented, not GAN). A good "honest sharpness" tier.

### 2.4 ESPCN / FSRCNN — the guaranteed-fast floor

- **Weights:** OpenCV `dnn_superres`-compatible pretrained graphs: [fannymonori/TF-ESPCN](https://github.com/fannymonori/TF-ESPCN) (**Apache-2.0**), [Saafke/FSRCNN_Tensorflow](https://github.com/Saafke/FSRCNN_Tensorflow) (**Apache-2.0**); ONNX Model Zoo also ships an ESPCN-style [super-resolution-10](https://huggingface.co/onnxmodelzoo/super-resolution-10) (Apache-2.0).
- 12–25 K params, ~11 GFLOPs @640×360 → **< 1 ms**; ×2/×3/×4 variants.
- Quality: modest — sharper than bicubic, clearly below RLFN; zero hallucination. Honestly close to what NIS/FSR-EASU+RCAS already deliver, so this only earns its place as an SR *scale* stage (real pixel synthesis at >2× zoom), not as an enhancement stage.
- Deployment: trivially small ONNX; could even be re-implemented as a single CUDA kernel (no TRT dependency at all).

### 2.5 Anime4K — MIT stroke/line-art CNNs, the "no-runtime" option

- **Repo:** <https://github.com/bloc97/Anime4K> — **MIT**, including the shader files, which *embed the trained weights as GLSL constants* — i.e. the weights are unambiguously MIT. Variants S/M/L/VL/UL (`Anime4K_Upscale_CNN_x2_M` ≈ **2 741 params**; UL ≈ tens of K). Published performance: ~1–3 ms at 720p→1440p on mid-range GPUs; RTX 3070 Ti-class finishes UL within 3 ms.
- **Why it's on a text list:** these CNNs are trained for line art — high-contrast strokes on flat backgrounds — which is structurally identical to glyphs on slides/whiteboards. Community use on subtitle/text overlays is a known strength; combined with `Restore_CNN` passes it de-rings and thickens strokes without GAN hallucination.
- **Deployment:** no ONNX, no TRT, no DLL: port the (tiny) conv stacks from GLSL to CUDA kernels next to our existing NIS/FSR launches. [Magpie](https://github.com/Blinue/Magpie) (GPL-3.0) already ports them to DirectCompute HLSL — a useful reference (port from the MIT upstream, not from Magpie, to keep the commercial arm clean). This is also the only ML option that survives a future non-NVIDIA backend *for free* (compute shader).
- **Gotchas:** ×2 only per pass (chain twice for ×4); quality ceiling below RLFN/Real-ESRGAN on photographic content; several variants must be A/B-tested on real slide footage.

### 2.6 OpenVINO Open Model Zoo — Apache-2.0, includes a true text-SR model

- **Models:** [text-image-super-resolution-0001](https://docs.openvino.ai/2023.3/omz_models_model_text_image_super_resolution_0001.html) (×3, grayscale, **0.003 M params, 1.379 GFLOPs @360×640** — designed for scanned text), [single-image-super-resolution-1032/1033](https://docs.openvino.ai/2023.3/omz_models_model_single_image_super_resolution_1033.html) (×4/×3 attention-based, ~0.03 M). All Open Model Zoo intel-models are **Apache-2.0**.
- **Gotchas:** distributed as OpenVINO IR, not ONNX — needs conversion (openvino2onnx tooling / re-export) before TRT or DirectML; trained on flatbed-scan degradation, not camera blur+noise, so expect it to under-perform on our input; grayscale-only (fine after our binarization/luma stages, wrong before them). Worth a cheap experiment because it's the only *permissively licensed text-specialized* SR network found.

### 2.7 RTX Video SDK (RTX VSR for applications) — the driver-level "free win", now real

- **What changed:** RTX VSR began as a driver feature reachable only through the **D3D11 VideoProcessor** private extension (documented by [mpc VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer/wiki/Super-Resolution); Moonlight integrated it the same way). Since then NVIDIA productized it as the **[RTX Video SDK](https://developer.nvidia.com/rtx-video-sdk)** with effects: Super Resolution, artifact reduction, SDR→HDR. Officially supports **DirectX 11, DirectX 12, Vulkan** (CUDA "coming soon"), Windows 10/11 x64 — the D3D12 support removes the interop tax that made the old D3D11-VP route unattractive for our D3D12 presenter ([announcement blog](https://developer.nvidia.com/blog/enhancing-low-resolution-sdr-video-with-the-nvidia-rtx-video-sdk)).
- **Performance:** driver VSR upscales 1080p→4K at 60 fps on 3060-class hardware; our 640×360→720p/1080p ROI is far smaller — well within budget. Models live in the driver/SDK installer (driver ≥ 537.42 for VSR; RTX 20-series and up).
- **License:** SDK download behind an NVIDIA developer account; governed by the NVIDIA SDK license family (attribution/branding clauses apply — e.g. NVIDIA marks in about-box; RTX-SDK license questions: nvidia-rtx-license-questions@nvidia.com). Commercial apps (DaVinci Resolve, Filmora, VLC integrations) ship it, so redistribution is clearly workable — but as with Maxine, **read the current agreement before the commercial release**.
- **Gotchas:** tuned for *compressed video* (deblocking + edge reconstruction), not glyph geometry — on clean camera text it behaves like a good adaptive sharpener; NVIDIA-only; since our frames sit in CUDA memory, feeding the D3D12 path means CUDA↔D3D12 sharing of the ROI texture (we already do CUDA→D3D12 for present, so the plumbing exists). Positioning: cheap "enhance" toggle, not the text-SR endgame.

### 2.8 ONNX Runtime + DirectML — the portability insurance (runtime, not a model)

- Whatever open model we pick (RLFN / ESPCN / Real-ESRGAN compact), keep the ONNX file as the single source of truth. Primary path: **TensorRT engine built and cached at first run**. Secondary path (future non-NVIDIA roadmap): **ONNX Runtime with the DirectML EP** — `onnxruntime.dll` (~15–25 MB) + `DirectML.dll` (~15–50 MB) versus TensorRT's heavyweight footprint (`nvinfer` ~200 MB + `nvinfer_builder_resource` ~0.7–1 GB in TRT 10.x; the *lean/dispatch* runtime with pre-built version-compatible engines cuts this to ~tens of MB but locks engine/runtime versions).
- Practical recommendation: for RLFN/ESPCN-class models, use the **TensorRT C++ API directly** (we already link CUDA) with an on-disk engine cache keyed by `(model hash, GPU compute capability, driver, TRT version)`; fall back to ORT+DirectML later, same ONNX asset. For Anime4K-class, skip runtimes entirely (hand CUDA kernels).

### 2.9 Scene-text-SR research line (TextZoom models) — verified and blocked

- **TSRN** ([TextZoom repo](https://github.com/WenjiaWang0312/TextZoom)): **no LICENSE file** → all-rights-reserved by default → **blocked** for redistribution.
- **TBSRN** (Scene Text Telescope, in [FudanVI/FudanOCR](https://github.com/FudanVI/FudanOCR)): **no LICENSE file** → **blocked**.
- **TATT** ([mjq11302010044/TATT](https://github.com/mjq11302010044/TATT), CVPR 2022) and **TPGSR** ([mjq11302010044/TPGSR](https://github.com/mjq11302010044/TPGSR)): code is **MIT**, but the READMEs only link *external recognizer* weights (ASTER/MORAN/CRNN); full SR checkpoints are not reliably published, and the models are trained on TextZoom.
- **Architectural mismatch regardless of license:** this entire line consumes *cropped, recognizer-aligned word images* (TextZoom LR crops are 16×64 → 32×128) and several need a text recognizer in the loop. They do not apply to a live 640×360 mixed-content ROI without a detection+crop+paste pipeline that would blow the latency budget and flicker. Newer entries (TATT successors, diffusion-based TextSR/PEAN 2024–2025, StyleSRN ICCV 2025) are even heavier (diffusion = seconds). **Conclusion: mine this literature for degradation/eval ideas, not for shippable weights.**
- HuggingFace sweep (2025–2026) found no permissively-licensed *real-time* text-SR release; text-SR activity has moved to diffusion/OCR-guided models (e.g. TextSR, arXiv 2505.23119) that are off-budget by orders of magnitude.

---

## 3. RECOMMENDATION

**Primary: NVIDIA Maxine VFX SuperRes** — the only option with *vendor-measured* latency on exactly our workload (360p→720p 2×: 3.28 ms on an RTX 2060; expect ~2–3 ms on a 3060 laptop), pre-trained, maintained (v0.7.6, 2025), consumes CUDA buffers natively (zero interop with our pipeline), and is commercially shippable under the NVIDIA SLA (headers MIT). Cost: proprietary models, ~400 MB runtime (have users run NVIDIA's installer), NVIDIA-only. One SLA read-through is the only open action.

**Open-weights alternative / second tier: RLFN (Apache-2.0)** — 0.317 M params, ~4–7 ms est. at 640×360 ×4 via TensorRT FP16; the best license-quality-latency balance among genuinely open models. Ship the ONNX in-repo, build+cache the TRT engine at first enable. Add **`realesr-general-x4v3` (BSD-3)** as a "deep zoom" tier only (ROI ≤ ~320×180, where it fits the budget and its hallucination risk is offset by the wdn denoise blend).

**Lightweight fallback (and future non-NVIDIA path): Anime4K Upscale/Restore CNN (MIT)** — 3–100 K params ported from GLSL to CUDA kernels beside our NIS/FSR launches; sub-millisecond, no runtime DLLs, line-art bias suits glyphs, and trivially portable to compute shaders later.

**Driver-level free win: RTX Video SDK** — now app-integrable with native **D3D12** support; expose as a cheap "Video Enhance" toggle on the presented frame. Worth a 1-day spike; not the text-SR endgame.

### Integration sketch (behind `OKUFLOW_ENABLE_TEXT_SR=ON`)

1. New `TextSrAdapter` interface in the CUDA pipeline at the existing stub
   (`cuda_interop.cpp` ~line 1653), running **on the zoomed ROI only**, before
   black-white/binarization stages, replacing NIS/FSR as the *scaler* when active
   (never run ML-SR + NIS together — pick per settings).
2. Backends: `MaxineSrAdapter` (NvVFX, CUDA buffers in/out) and `TrtSrAdapter`
   (TensorRT C++ API; ONNX asset in `models/`; engine cached under
   `%LOCALAPPDATA%/OkuFlow/trt-cache/<model>-<sm>-<driver>-<trt>.engine`;
   build ≈ seconds–1 min, do it async with a status toast, fall back to NIS
   until ready). FP16 default; INT8 later via calibration on synthetic text
   crops (see §5.1) — optional, FP16 already fits budget for RLFN-class.
3. **Blend by strength**: `out = lerp(bicubic(roi), sr(roi), strength)` with the
   existing slider; auto-disable above latency watchdog threshold (reuse the
   pipeline's timing infra) so a slow GPU degrades to NIS instead of dropping frames.
4. Tiering by zoom factor: zoom < 2× → no ML; 2–3× → RLFN/Maxine 2×;
   ≥ 3× → 4× model on the (now small) ROI. ROI padding of ~8 px to avoid edge
   artifacts at the crop border; reuse SR output across frames when the ROI is
   static (stabilized) to save power.
5. `THIRD_PARTY_LICENSES.md`: add model entries (BSD-3 Real-ESRGAN /
   Apache-2.0 RLFN / MIT Anime4K; Maxine per SLA notice). Keep GPL build able to
   compile with only the open backends so the GPL arm never depends on
   proprietary bits.

---

## 4. Explicitly rejected options

- **SwinIR / SwinIR-light / HAT:** 474.7 ms @256×256 (V100, FP32, published) for SwinIR-S; even ×10 TRT-FP16 optimism leaves ~150 ms+ at 640×360 — 20–50× over budget. Transformer window ops also convert poorly to TRT. Quality is irrelevant at that latency.
- **DLSS / Streamline:** DLSS-SR requires depth buffer, screen-space motion vectors and jittered low-res *rendering* ([Streamline programming guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)) — it is a temporal reconstruction for rendered scenes, not applicable to camera frames. Disqualified on mechanism, not license.
- **TSRN / TBSRN (TextZoom line):** repos publish **no license** → weights not redistributable; plus word-crop architecture unusable on live ROI.
- **TATT / TPGSR:** MIT code but no shippable full checkpoints + recognizer-in-the-loop word-crop design; latency and flicker would be unacceptable.
- **Diffusion text-SR (TextSR 2025, PEAN, StyleSRN):** seconds per frame; research-only in practice.
- **waifu2x / realsr ncnn-Vulkan ports:** fine licenses (MIT/Apache-2.0) but a Vulkan/ncnn runtime is a third GPU stack alongside CUDA+D3D12; their strong models are RRDB-class (too slow) and their fast models add nothing over RLFN via TRT.
- **RealESRGAN_x4plus / x2plus (RRDB 16.7 M):** ~0.5 s/frame class — offline tool, not live video.
- **OpenVINO *runtime* on NVIDIA:** the OMZ *models* are useful (see §2.6) but the OpenVINO runtime targets Intel silicon; on our stack it would run CPU-side. Convert the models out instead.
- **Old D3D11-VP RTX VSR hack (mpc-style private GUIDs):** superseded by the official RTX Video SDK with native D3D12; no reason to touch undocumented extensions.
- **"Upscaler-Ultra"-style HF re-uploads (Apache-tagged Real-ESRGAN derivatives):** provenance unverifiable; use the upstream BSD-3 originals instead.

---

## 5. Last resort: train our own (kept open deliberately)

Ranked strictly **after** every viable pre-trained option above, per project owner.
Recorded so the door stays open if all pre-trained routes prove insufficient on
real classroom footage.

### 5.1 Synthetic-data retraining — the clean path

Train a small, permissively-licensed architecture (ESPCN/FSRCNN code Apache-2.0,
RLFN Apache-2.0, or our own ~0.3 M-param convnet) on **synthetic text pairs**:

- **HR generation:** render random text (fonts under OFL/Apache — Google Fonts corpus,
  Noto for multi-script coverage; random sizes, weights, colors, rotations, mild
  perspective) over varied backgrounds (paper texture, slide gradients,
  whiteboard sheen, photos).
- **LR degradation:** camera-realistic pipeline — anisotropic Gaussian + motion +
  defocus blur kernels, Poisson-Gaussian sensor noise, JPEG/H.264 compression,
  downsampling with random kernels. This is exactly Real-ESRGAN's published
  degradation model, whose **BSD-3 code we can reuse directly**; synthetic text
  data is standard practice in text recognition (MJSynth/SynthText lineage), so
  the approach is well-trodden.
- **Effort estimate:** dataset generator ~1–2 days of scripting; ESPCN-class
  training converges in hours on one consumer GPU; an RLFN-class model is
  **a weekend of GPU time** on a single RTX 4090-class card. No dataset license
  issues, unlimited data, degradation tuned to *our* cameras (we can even mix in
  a few hundred real OkuFlow frame captures as validation).
- **Decisive advantage:** the resulting weights are **ours outright** — cleanly
  dual-licensable under GPL-3.0 + commercial with zero third-party terms, and a
  text-specialized model at RLFN latency would likely beat every general-purpose
  option above on glyphs.

### 5.2 Open-dataset retraining — mostly *not* commercial-OK (verified)

- **DIV2K:** the [official page](https://data.vision.ee.ethz.ch/cvl/DIV2K/) states the
  dataset is "made available for academic **research purposes only**"; image
  copyright remains with original owners → **blocked** for the commercial arm.
- **Flickr2K:** no explicit license identifiable; scraped via Flickr API; treated
  community-wide as non-commercial research data → **blocked**.
- **TextZoom:** repo publishes **no license**, and the data derives from RealSR and
  SR-RAW (both academic releases) → **blocked**.
- Net: the standard SR training corpora are all research-encumbered. If real
  photographic HR data is ever needed, use sources with explicit commercial-use
  terms (e.g. self-captured footage, public-domain/CC0 image sets) — or simply
  prefer §5.1, which needs none of them.

### 5.3 Distillation from a restricted teacher — NOT recommended

Distilling (training our student on the *outputs* of) a model whose weights are
under CC-BY-NC / research-only / API ToS is **legally risky and rejected**:
several model and service licenses explicitly prohibit using outputs to train
other models, and even where the copyright status of model outputs is unsettled,
the restriction binds **contractually** through the license accepted at download.
Only acceptable with teachers whose licenses permit output reuse — e.g.
Real-ESRGAN (BSD-3) or RLFN (Apache-2.0) as teachers is fine, but those are
exactly the models we could ship directly, so distillation buys nothing there.
If §5.1 happens, use permissive teachers or none.

---

## Sources

- [xinntao/Real-ESRGAN (BSD-3-Clause)](https://github.com/xinntao/Real-ESRGAN) · [release v0.2.5.0 assets](https://github.com/xinntao/Real-ESRGAN/releases) · [Qualcomm AI Hub: Real-ESRGAN-General-x4v3 (BSD-3 model license)](https://huggingface.co/qualcomm/Real-ESRGAN-General-x4v3) · [SRVGGNetCompact arch](https://github.com/xinntao/Real-ESRGAN/blob/master/realesrgan/archs/srvgg_arch.py)
- [bytedance/RLFN (Apache-2.0, weights in repo)](https://github.com/bytedance/RLFN) · [RLFN paper](https://arxiv.org/abs/2205.07514)
- [hongyuanyu/SPAN (Apache-2.0)](https://github.com/hongyuanyu/SPAN) · [SPAN paper](https://arxiv.org/abs/2311.12770)
- [NTIRE 2025 ESR report](https://arxiv.org/abs/2504.10686) · [NTIRE2025_ESR repo (MIT)](https://github.com/Amazingren/NTIRE2025_ESR) · [NTIRE2026_ESR repo](https://github.com/Amazingren/NTIRE2026_ESR) · [NTIRE 2026 ESR report](https://arxiv.org/pdf/2604.03198)
- [fannymonori/TF-ESPCN (Apache-2.0)](https://github.com/fannymonori/TF-ESPCN) · [Saafke/FSRCNN_Tensorflow (Apache-2.0)](https://github.com/Saafke/FSRCNN_Tensorflow) · [ONNX model zoo super-resolution-10](https://huggingface.co/onnxmodelzoo/super-resolution-10)
- [bloc97/Anime4K (MIT)](https://github.com/bloc97/Anime4K) · [Upscale shaders wiki](https://github.com/bloc97/Anime4K/wiki/Upscale-Shaders) · [Magpie (GPL-3.0 reference port)](https://github.com/Blinue/Magpie)
- [NVIDIA-Maxine/Maxine-VFX-SDK (MIT headers; SLA runtime)](https://github.com/NVIDIA-Maxine/Maxine-VFX-SDK) · [Maxine VFX performance reference (SuperRes ms table)](https://docs.nvidia.com/maxine/vfx/WindowsVFXSDK/PerformanceReference.html) · [Maxine VFX SuperRes docs](https://docs.nvidia.com/maxine/vfx/1.1.0/Filters/SuperResolution.html) · [NGC Maxine Windows VFX SDK](https://catalog.ngc.nvidia.com/orgs/nvidia/maxine/collections/maxine_vfx_sdk_collection_ga/-)
- [RTX Video SDK](https://developer.nvidia.com/rtx-video-sdk) · [RTX Video SDK announcement (DX11/DX12/Vulkan)](https://developer.nvidia.com/blog/enhancing-low-resolution-sdr-video-with-the-nvidia-rtx-video-sdk) · [RTX Video FAQ](https://nvidia.custhelp.com/app/answers/detail/a_id/5448/~/rtx-video-faq) · [mpc VideoRenderer D3D11-VP VSR notes](https://github.com/Aleksoid1978/VideoRenderer/wiki/Super-Resolution) · [NVIDIA RTX SDKs license](https://developer.nvidia.com/downloads/nvidia-rtx-sdks-license-23jan2023pdf)
- [Streamline DLSS programming guide (depth+mvec requirement)](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
- [OpenVINO OMZ text-image-super-resolution-0001](https://docs.openvino.ai/2023.3/omz_models_model_text_image_super_resolution_0001.html) · [single-image-super-resolution-1033](https://docs.openvino.ai/2023.3/omz_models_model_single_image_super_resolution_1033.html) (OMZ models Apache-2.0)
- [WenjiaWang0312/TextZoom (no license)](https://github.com/WenjiaWang0312/TextZoom) · [FudanVI/FudanOCR (no license)](https://github.com/FudanVI/FudanOCR) · [mjq11302010044/TATT (MIT)](https://github.com/mjq11302010044/TATT) · [mjq11302010044/TPGSR (MIT)](https://github.com/mjq11302010044/TPGSR) · [TextZoom paper](https://arxiv.org/abs/2005.03341) · [TextSR diffusion 2025](https://arxiv.org/abs/2505.23119)
- [nihui/waifu2x-ncnn-vulkan (MIT)](https://github.com/nihui/waifu2x-ncnn-vulkan) · [nihui/realsr-ncnn-vulkan (MIT)](https://github.com/nihui/realsr-ncnn-vulkan) · [Tencent/Real-SR (Apache-2.0)](https://github.com/Tencent/Real-SR) · [jixiaozhong/RealSR (Apache-2.0)](https://github.com/jixiaozhong/RealSR)
- [DIV2K dataset ("academic research purpose only")](https://data.vision.ee.ethz.ch/cvl/DIV2K/) · [SwinIR-S latency data point (V100 0.4747 s @256²)](https://arxiv.org/pdf/2211.11436) · [NVIDIAGameWorks/NVIDIAImageScaling — NIS (MIT, already integrated)](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling)

*(License SPDX identifiers for GitHub repos verified 2026-07-22 via the GitHub API;
NVIDIA SLA terms summarized from NVIDIA docs — re-read the current agreement text
before the commercial release.)*

---

## Maxine SLA verification (full license texts read, 2026-07-22)

**Verdict: SAFE-WITH-CONDITIONS — but only via the legacy 0.7.6 path.**

Two license regimes coexist:
- **Legacy Windows VFX SDK 0.7.6** (2021 SDK EULA + Maxine Supplement; GitHub
  headers/samples are MIT): free commercial use, **bundling DLLs+models with the
  app expressly permitted** (Supplement §2: everything but the A/V samples is
  distributable), free public end-user installers also allowed (the OBS/Elgato
  route). Shipped versions survive termination (EULA §6.4: "Your prior
  distributions ... are not affected by the termination").
- **New "VFX SDK Core" 1.2.x (NGC, 2026)**: production use requires paid
  **NVIDIA AI Enterprise**; the free Developer Program access is explicitly
  "non-production". 30-day termination-for-convenience with destroy-all-copies
  and no prior-distribution carve-out. Do not build on this.

**Binding conditions for OkuFlow:**
1. **Anti-copyleft clause (EULA §2.5 / NVSLA §8.8):** the SDK must never become
   subject to an open-source license → NVIDIA bits must NOT ship inside the
   GPL-3.0-distributed build. Architecture: SuperRes is an optional
   dynamically-loaded plugin; NVIDIA runtime ships only with the commercial
   build, or GPL users run NVIDIA's free installer themselves.
2. **Mandatory attribution** (Supplement §3.1): NVIDIA Maxine branding per
   guidelines must appear in the app.
3. **NVIDIA-GPU-only** (Supplement §1) — acceptable because it's an optional
   enhancement; the core magnifier must never depend on it.
4. **Sunset track:** pin to 0.7.6-era binaries; assume no future updates outside
   AI Enterprise.

Given the conditions and sunset risk: **RLFN (Apache-2.0) stays the default
primary; Maxine 0.7.6 is the optional premium tier for the commercial build.**
Key sources: Maxine_SDK_License_1Apr2021_updated.pdf (developer.nvidia.com),
NVIDIA-Software-License-Agreement-2026.5.7.pdf, PST-for-AI-Products-2025.02.28,
ngc.nvidia.com maxine collection pages, nvidia.com/broadcast-sdk-resources,
github.com/NVIDIA-Maxine/Maxine-VFX-SDK (MIT).
