# UCT offline payload boundary (development, unstaged)

This source-only helper inventories the selected BF16 **model**, original medium
detector/recognizer assets, conversion source and candidate native dependencies.
It does not prepare or distribute an engine. The owner's choice is local engine
construction and golden checking on each eligible user's GPU. This checkout
enables no public factory, downloads nothing and changes no release option.

`scripts/uct/offline_payload.py` is first-party Python 3.10+ standard-library code.
Its three explicit actions are `inventory` (read-only facts), `validate` (read-only
reviewed inventory validation), and `stage` (future new-only private staging).
**All three and the fixtures remain unrun in this task.** The only execution here
was bounded read-only source/file/PE metadata and selected SHA-256 inventory.
No build, test, GPU call, inference, engine preparation or payload copy was run.

The separate public boundary at `faa7fbfe7b87983802c881411a856328bc873dfd`
was read in `t3-2a9e2016`; it was not cherry-picked or edited. Its production
corpus catalog was empty and no active public factory existed. This helper
does not turn that state into readiness. The golden-asset owner supplies and
reviews the actual eight crops and truth; no duplicate corpus is created here.

## Exact candidate inventory

[`offline-payload-candidate.json`](../scripts/uct/offline-payload-candidate.json)
is the exact machine-readable allowlist, with source-root aliases, relative
source/destination paths, byte lengths, full SHA-256 and PE imports. It is
**development-unstaged**, with unresolved blockers, not an approved distribution
manifest. No accepted reviews, model acceptance, corpus approval, trusted
signature, engine or ready receipt are supplied. Nothing discovers an owner's
SDK/cache or derives approval from an installed file.

Destinations below are relative to a new private `app/` fragment:

| Payload | Exact selection |
| --- | --- |
| Original detector | `models/uct/det/medium/{inference.onnx,inference.yml,README.md}` |
| Original recognizer and dictionary | `models/uct/rec/medium/{inference.onnx,inference.yml,README.md}`; the dictionary is inside the exact config |
| Converted graph | `models/uct/rec/medium/bf16-linear/inference.onnx`, 38,456,512 bytes, `ecd8ffc9…` |
| Conversion source | `sources/uct/precision_models.py`, 13,934 bytes, `da22b96f…`; retained, never executed |
| Model license | `models/uct/MODEL_LICENSES.txt`, exact existing `third_party/paddleocr/LICENSE` |
| TensorRT inference/builder factory | `uct-runtime/tensorrt-11.4.0.106/bin/nvinfer_11.dll`, 412,735,600 bytes |
| ONNX parser | Same `bin/nvonnxparser_11.dll`, 2,241,648 bytes |
| Generic builder resource candidate | Same `bin/nvinfer_builder_resource_ptx_11.dll`, 505,629,808 bytes |
| CUDA runtime compilation | `uct-runtime/cuda-12.9.2/bin/nvrtc64_120_0.dll`, 89,832,960 bytes, and `nvrtc-builtins64_129.dll`, 7,217,664 bytes |
| TensorRT notices | Complete original `doc/Acknowledgements.txt` and `doc/README.txt` under `licenses/uct/` |
| NVRTC terms | Exact selected toolkit `LICENSE` under `licenses/uct/NVIDIA_CUDA_NVRTC_LICENSE.txt` |
| Still-required reviewed files | Sanitized `PROVENANCE.json`, prominent `MODIFICATIONS.txt`, frozen full TensorRT agreement, versioned golden manifest/NOTICE and eight exact `.gray8` files |

There are 17 fixed files plus five required reviewed files and eight corpus
files: **30 payload files**, followed by one private inventory receipt. The
recognizer's original FP32 graph is required by the existing `LoadPaddleAssets`
and public `ReadModel` contract; the converted graph alone is insufficient.
No tiny/small CPU model or ONNX Runtime DLL is silently added. CPU fallback
payload scope remains a separate integration decision; built-in Windows OCR
uses the user's installed Windows components.

The converter hash and exact original configs/cards/revisions are retained.
The decoded dictionary has 18,710 entries including the blank and appended
space, hash `f1d7cde7…`, independently confirmed by the read-only reviewer.
Conversion provenance identifies opset-22 BF16 linear islands, mixed
BF16/FP16/FP32 arithmetic and FLOAT I/O. The helper requires exact sanitized
provenance matching the catalog. It never copies the private conversion manifest
with workstation paths. A modification notice and all upstream notices must be
reviewed with the exact files; Apache section 4 describes retention and modified
file identification. The project additionally requires the converter source.
[Apache-2.0 section 4](https://www.apache.org/licenses/LICENSE-2.0)

Explicit exclusions include all `.plan`, `.engine`, timing caches, local ready
receipts, owner approval manifests/caches, unrelated models, rejected detector
variants, headers/import/static libraries, Python bindings/wheels, samples,
tests, `trtexec.exe`, compilers/profilers and SDK archives. Plugins, lean/dispatch
runtimes, SM-specific resources, `.alt.dll`, cuBLAS/cuDNN/nvJitLink, cudart DLLs,
Windows system DLLs, NVIDIA driver files, fonts and private media are excluded.
Adding one requires a reviewed catalog/source change, never a wildcard copy.

## Actual local observations and unresolved ABI/dependency facts

The inspected optional roots were the existing documented LocalAppData
`OkuFlow/ocr-runtime`, `ocr-deps/TensorRT-11.4.0.106-cuda12.9/TensorRT-11.4.0.106`
and `toolchains/cuda-12.9.2/toolkit`, physically below
`Z:/Projects/OkuFlow/local-work-data/`. The converted graph came from the existing
`uct-quantization-20261022` artifact root; converter source was read at archived
`t3-a299a57f/scripts/uct/precision_models.py`. These are inventory provenance,
not compiled paths or automatic staging defaults. Only selected files were
hashed; the pinned SDK archive was not rehashed or extracted.

All five selected DLLs are PE32+ AMD64 (`0x8664`, optional magic `0x20b`).
`nvinfer` imports ntdll/kernel32/user32/shell32/ole32/advapi32; the parser imports
kernel32/dbghelp; PTX resource and NVRTC builtins import kernel32; NVRTC imports
user32/kernel32/advapi32/ole32/shell32. Delay-import tables were empty and no
direct CUDA DLL imports appeared. Read-only binary string observations by the
independent reviewer name NVRTC inside nvinfer and builtins inside NVRTC.
These facts support the candidate selection but **do not prove dynamic closure**.
The SDK/archive identity supplies the `11.4.0.106` version claim; the inspected
TensorRT DLLs exposed no FileVersion resource. No export was invoked and no
loaded runtime version was measured in this source-only task.

The source loader selects a matching SM resource, otherwise PTX. PTX-only is
the smallest source-supported candidate for per-user construction, not proof
of successful graph compilation on every compute-major >=8 device. The local
SM89 resource was separately observed as 267,857,008 bytes,
SHA-256 `47a92f598f96abadbc6076fe07e4abab15aff1a14867a3ecbd1d7996f845d731`;
it is excluded from this candidate. No toolkit/resource upgrade is authorized.

Application build facts describe statically linked cudart. The separately
observed `cudart64_12.dll` is 583,680 bytes,
`760c38928bbe5759f7b31ed6692599eb7ec83cedd5702e84c2b72028a89837e1`;
it is not established as necessary here and is excluded. A PE table cannot
prove static linkage inside an NVIDIA DLL. Exact dynamic load closure, target
GPU/driver support and controlled engine construction/deserialization remain
unaccepted. ABI11 eligibility is distinct from a successfully usable plan.

The current versioned NVIDIA support matrix describes CUDA13.x ZIP packages
and CUDA12.x wheels; the existing pinned/local archive identifies a CUDA12.9
Windows ZIP. Reconcile the exact archive's provenance before distribution;
do not silently replace the pin or install a different SDK.
[TensorRT 11.4 support matrix](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/getting-started/support-matrix.html)

## Redistribution review, still open

TensorRT 11.4 SLA section 1.1(3) limits distribution to identified portions;
section 1.2 sets application functionality/access and distribution conditions
and reserves unidentified developer tools for internal use. Supplement section
2 names runtime `.so`/`.dll` files. This supports nvinfer's runtime category,
but does not explicitly classify the parser and builder resources by basename.
Their exact distribution classification remains unresolved. Apache headers do
not license the proprietary DLLs. Preserve the complete acknowledgements and
review a frozen full agreement; the local README links to mutable online terms.
The GPL NVIDIA exception does not waive these conditions.
[TensorRT 11.4 agreement](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/reference/sla.html)

CUDA 12.9.2 supplement section 2.2/Attachment A identifies NVRTC, its builtins
and cudart, including versioned/architecture filenames; the general distribution
conditions still apply. This is separate from TensorRT permission. Retain the
selected component's exact notice. The 67,876-byte assembled toolkit license
matches the NVRTC package (`72c22161…`); the separately observed cudart package
license is 63,522 bytes, `213f2f1cc4bfecd53eb1579edc89d6e3a024ddd2f77413b69b40e2bf3c114d84`.
They are different original texts. Do not replace the application's existing
build-facts CUDA notice with the added NVRTC notice or infer permission for
unlisted CUDA DLLs.
[CUDA 12.9.2 EULA](https://docs.nvidia.com/cuda/archive/12.9.2/eula/index.html)
NVRTC and its builtins must come from the same toolkit.
[NVRTC builtins](https://docs.nvidia.com/cuda/archive/12.9.2/nvrtc/index.html#nvrtc-builtins-library)

These are source/inventory findings, not legal clearance or advice that all
redistribution obligations are satisfied. No new NVIDIA license text or binary
was vendored. The already tracked Apache text remains byte-preserved by its
existing `-text` attribute. Future staging copies reviewed notices byte for byte.

## Review manifest and future private staging

The separately supplied distribution JSON must match
[`offline-payload.schema.json`](../scripts/uct/offline-payload.schema.json) and
the stricter Python semantic checks. No accepted example is provided.

- Caller explicitly supplies its independently reviewed raw SHA-256. There is
  no default trusted hash, signature assertion or trust derived from the
  manifest's own contents. This is a caller-controlled local review pin, not
  cryptographic signer authentication or protection against malicious same-user
  edits. A future release caller must pin it in separately reviewed source.
- Manifest binds catalog SHA-256, model version, actual corpus version/hash and
  exact file records. All 17 fixed records must match the catalog, all five
  reviewed files must exist, and exactly eight crop records must match the
  corpus. Extra models/engines/tools cannot be admitted by another file entry.
- Four bounded private JSON reviews (`redistribution`, `model`, `corpus`,
  `runtime-closure`) must say accepted and bind this catalog, ordered file-list
  digest, graph and corpus. Each review's own length/hash is checked. Their
  narrative must cite concrete evidence; the helper does not manufacture a
  verdict or verify the truth of a human/legal assertion. These review files
  remain private and are not copied into the app fragment.
- Model review preserves original failed comparison evidence and explicitly
  records the owner's model choice and its limits. Corpus review must establish
  owned source, notices and independently checked truth, and match the actual
  compiled production corpus pin in the integrated source. Offline validation
  checks bytes/format, not the binary's compiled pin or real OCR output.
- The reviewed corpus must use the exact public model/policy/encoding and eight
  unique bounded GRAY8 height48 crops. Sample text/tags/width/hash are checked;
  no truth is generated, normalized or derived from the candidate recognizer.
- Root aliases must be explicitly supplied as existing physical local paths:
  `models`, `variant`, `converter`, `repo`, `tensorrt`, `cuda`, `review`, `corpus`.
  No environment/default cache/PATH lookup is used. Reject UNC, symlinks,
  junctions at any ancestor, hardlinked files, ADS, device names and traversal.
- `inventory` hashes only the fixed allowlist and reads PE metadata without
  loading DLLs. Its status always says candidate-only. Matching bytes and a
  zero exit status do not clear its printed redistribution/corpus blockers.
- `validate` performs no writes. `stage` requires the same complete validation
  before exclusively creating a nonexistent `app` folder under an existing
  private parent, disjoint from every source root. It copies only those 30
  files, checks source/copy hashes and exact destination inventory, and writes
  `uct-offline-payload.json` with `private-staging-complete-not-release` status.
  It never overwrites/reuses/deletes an existing tree. Failure preserves partial
  files and an incomplete marker; no retry-until-pass or rollback sweep exists.

JSON inputs/reviews are at most 1 MiB, corpus manifest at most 64 KiB, each
payload at most 600 MiB, total at most 2 GiB and inventory at most 64 files.
Hashes stream in 1 MiB chunks; PE section/import counts are bounded. This is
offline maintenance code, never run on the UI thread.

## Separate release integration seam

No tracked release, SBOM, installer, runtime `Network`, factory, CMake, test
impact or MCP catalog file was changed. The helper makes a private **fragment**,
not an application bundle; it intentionally refuses an existing `app/` folder.
The composition lead owns these separate steps before public inclusion:

1. Supply reviewed distribution/evidence pins and real corpus approval, including
   compiled pin parity. Pin the source/model/converter provenance and notices.
2. Admit only the verified fragment into a new owned release generation, with
   collision/reparse refusal. Integrate after `organize_release_bundle.ps1`
   has selected the nested layout, before `generate_release_metadata.ps1`.
   Do not copy a whole SDK or reuse a deployed app folder. A separate explicit
   merger is required because this helper never merges existing directories.
3. Pass application model asset root `app/models`, normal model root
   `app/models/uct`, TensorRT root `app/uct-runtime/tensorrt-11.4.0.106` and CUDA
   dependency bin `app/uct-runtime/cuda-12.9.2/bin` through the assigned loader
   integration. Current automatic discovery does not select this layout.
4. Extend release metadata with distinct PaddleOCR source/converted model,
   first-party converter, golden corpus, TensorRT and CUDA NVRTC packages;
   retain hashes, licenses, modification/source relationships and extracted
   exact agreement/notices. Current metadata enumerates files, but has no UCT
   package semantics; its static-cudart package cannot represent added NVRTC.
   Bind reviewed/staged/published inventories and signing/installer inclusion.
5. Preserve all build-facts seals, required capability gates, complete tracked
   shipping/CPU/CUDA/bundle gates, signing and disposable installer acceptance.
   No dirty-preview, staging receipt or review verdict substitutes for them.

## Unrun finite model-free validation proposal

This is a proposal for a later **root START**, not a grant. Maximum three serial
jobs, **300 seconds total** (60 + 180 + 60), one CPU process at a time, no GUI,
GPU, SDK/model payload reads, real staging, cache mutation or installation:

1. 60 s: compile the two Python source strings with Python's builtin `compile`
   under `python -B` (no pyc), and parse the two JSON source files strictly.
2. 180 s: run `python -B scripts/uct/test_offline_payload.py --evidence-root
   <explicit-existing-private-physical-parent> -v`. Eleven model-free tests
   cover pins, path/JSON/hash/PE refusal, missing reviews/corpus, unlisted engine,
   duplicate inventory, no-clobber tiny fixture copying and preserved failures.
   The test-only PE bytes are not a loadable SDK. No readable golden corpus or
   accepted legal/model evidence is fabricated. Keep every attempt directory.
3. 60 s: reconcile candidate model identity with the integrated public boundary,
   schema/semantic field names, exact changed-file inventory and canonical LF
   patch. This is source-only review, not execution acceptance of the real SDK.

Root records timeout/exit/stdout/stderr and stops at the first unexpected failure;
preserve the original attempt before diagnosing source changes. Do not consume
the reserved merge CPU slot or append jobs automatically. Real dependency,
GPU golden/runtime/lifetime, multilingual quality, native desktop and release
acceptance require distinct future grants.

Independent source/license inventory round task ID:
`node:delegated-task:command%3Amcp%3A3d67d461-6eb4-4143-84d1-67c74676eb9b%3Adelegate-task%3Auct-offline-payload-license-inventory-20261023-r1`.
It confirmed selected bytes/imports/dictionary/notices and the open gates. It
finished before these new source files existed, so **it did not review this
implementation**. Its initial missing `prepare_uct.ps1` lookup and unavailable
pinned publisher web pages were retained as review limitations. This task's
read-only wrong-filename/wildcard lookups were corrected without any execution
or test failure being erased; no validation pass is claimed.
Two source-edit patch attempts also refused unmatched context atomically before
the corrected patches were applied; those original tool failures remain in the
thread. They were edit failures, not executed fixture outcomes.
