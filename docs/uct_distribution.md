# Portable pinned UCT payload

`scripts/stage_uct_payload.ps1` stages the exact private Windows x64 inventory
in `scripts/uct/distribution.json`. It performs no downloads, preparation,
conversion, engine construction, inference, corpus review or accuracy checks.
The app prefers BF16 for both selected GPU roles with same-tier FP32 fallback;
CPU recognition remains FP32. Model errors and BF16/FP32 disagreement are not
startup, packaging or precision-selection gates. Saved tiers and tiny N1 service
remain the application's responsibility.

The inventory includes all six original FP32 graphs, original configurations
(including recognition dictionaries) and model cards, all six converted BF16
graphs, the original Apache-2.0 model license, the pinned BF16 catalog and exact
first-party converter source. The generated `MODIFICATIONS.txt` names every
original/converted graph, configuration, upstream revision and converter hash.
The converter is retained as source; staging and the application never run it.

CPU ONNX Runtime 1.31.0 retains `LICENSE`, `ThirdPartyNotices.txt` and
`Privacy.md`. The private TensorRT 11.4.0.106 CUDA12.9 subset contains only
`nvinfer_11.dll`, `nvonnxparser_11.dll`,
`nvinfer_builder_resource_sm75_11.dll`,
`nvinfer_builder_resource_sm86_11.dll`,
`nvinfer_builder_resource_sm89_11.dll`,
`nvinfer_builder_resource_sm120_11.dll` and
`nvinfer_builder_resource_ptx_11.dll`, together with unchanged
`doc/Acknowledgements.txt`, `doc/README.txt` and the full versioned agreement.
The parent's exact `PROVENANCE.json` notice snapshot is retained beside them.
The agreement is retained verbatim from the
[versioned NVIDIA source](https://docs.nvidia.com/deeplearning/tensorrt/11.4.0/reference/sla.html)
as `third_party/tensorrt/LICENSE-11.4.html` (61,120 bytes,
SHA-256 `ee9cc052ec5f03616f1bd9777cea89dda1b8f5413e7a1948b6f31b64773746c4`).
The parent reviewed Supplement 2's distributable runtime `.so`/`.dll` terms;
these files stay inside the private application runtime. Package license IDs
are recorded explicitly, including NVIDIA `LicenseRef` identifiers.

CUDA adds only the known matching NVRTC 12.9.86 and builtins DLLs from the
12.9.2 toolkit plus its unchanged `LICENSE`. These exact three hashes were
available in the existing candidate inventory and match the selected local
files. The toolkit license lists NVRTC and its builtins. No additional CUDA
runtime dependency is established by this inventory. The application has its
separately built static CUDA runtime.

This is the desktop GPU inventory: native Turing, consumer/workstation Ampere,
Ada and Blackwell resources, plus PTX. Server-only SM80/90/100 builder DLLs are
not bundled; a separately installed full SDK remains an explicit runtime
override. This payload does not claim fresh-build support on those server GPUs.
All six model tiers/roles remain included.

No SDK headers, static/import libraries, SDK tools, `trtexec`, unlisted DLL
expansions, Python environments, archives, plans, timing caches, model review
receipts or golden corpora are staged.

## Integration API

Supply physical, absolute local directories. `ModelsRoot` is the directory
**containing** `uct/`, not `uct/` itself. `CudaRoot` contains `bin/` and `LICENSE`;
`TensorRtRoot` contains `bin/` and `doc/`; `RuntimeRoot` contains `det/`, `rec/`
and `cpu/`. Repository inputs resolve from the script location.

```powershell
$receipt = & "$repo/scripts/stage_uct_payload.ps1" `
    -BundleRoot $freshBundle `
    -RuntimeRoot $physicalOcrRuntime `
    -TensorRtRoot $physicalTensorRtSdk `
    -ModelsRoot $convertedModelsParent `
    -CudaRoot $physicalCudaToolkit
# Before signing/metadata generation, retain receipt seals in release evidence.
$receipt.ReceiptPath
$receipt.ReceiptSha256
$receipt.SourceSealSha256
$receipt.StagedSealSha256
$receipt.DistributionSha256
```

`BundleRoot` must already exist outside the runtime, TensorRT, models and CUDA
input roots. The tracked `repo/dist/OkuFlow.staging-<guid>` transaction is
supported: the repository root may contain staging because only closed,
explicitly pinned repository files are selected. Every repository source must
lie outside both owned destination subtrees, and an input file cannot overlap
an output. A repository inside the bundle does not qualify for this exception.
`app/` may
already contain other freshly staged runtime files. Both `app/ocr` and
`app/models` must be absent; an existing empty directory also causes refusal.
The script creates and owns those two subtrees for this attempt. LocalAppData's
OCR development path is a junction on the setup machine: pass its physical
target rather than the junction. No machine-specific path is in the manifest.

Every selected file has an exact relative source/destination, byte length,
SHA-256 and package identity. The per-file ceiling is 512 MiB and the whole
payload ceiling is 3 GiB. Before creating its targets the helper verifies all
selected inputs, catalog parity and path safety. Paths refuse traversal,
drive-relative/UNC/device aliases, alternate streams, reparse points in any
ancestor, reserved Windows names, unsafe components and trailing dot/space
aliases. Sources must be regular files. Source/destination roots cannot overlap
except for the restricted repository-parent case described above.

Copies use a bounded stream from a read handle that denies writers/deletion,
hash the source before and after copying, and use `CreateNew` for every output.
Staged bytes are checked from the destination handle and rehashed with all
sources before the final receipt. There is no deletion, replacement, overwrite
or automatic retry. Failure preserves the entire partial generation; select a
fresh bundle for a repaired attempt. These checks assume an owned staging
directory without a hostile process concurrently replacing directory ancestors;
they do not implement a filesystem sandbox or retain native directory locks.

`app/ocr/payload-receipt.json` lists portable source-root labels, relative paths,
package/license IDs and exact source/staged hashes. Generated manifest and
modification-notice bytes are included. `payload-receipt.sha256` is written last
and hashes the receipt's exact UTF-8 bytes. Seals are content digests, not
signatures or independent approval. A failure has no valid final marker. The
parent must verify these receipt hashes and all listed installed bytes again
when generating its complete release manifest/SBOM and signing/publishing; the
helper's receipt cannot attest to subsequent edits. No absolute private source
path is included in the distributed receipt.

## Validation and remaining acceptance

The author performed read-only hashing of all six converted graphs and the
small source catalogs/converter. All matched their existing pins. The four ORT
files and the six selected TensorRT archive files matched exact entries streamed
from the existing cached ZIPs. The complete ZIPs were **not rehashed** in that
review; their archive hashes remain declarations from `pins.json`, rather than
fresh whole-archive authentication. CUDA files matched the prior candidate pins
and current installed bytes; its original installer archive was not verified.

The model-free fixture is:

```powershell
pwsh -NoProfile -File scripts/test_uct_payload.ps1
```

It reads only tracked catalogs, converter and compiled pin headers for parity,
then uses tiny synthetic files in a unique LocalAppData evidence folder. It
covers exact role/tier pin binding, notices, seals, overwrite/new-only refusal,
containment/aliases, streams/reparse points, regular files, size bounds,
metadata-preserving mutation and preserved partial failures without a commit
marker. All fixtures are serial with a 120-second budget and retain their
evidence. They do not load real SDKs/models or start GUI/GPU processes.

Once the tracked batch starts OCR staging, a later failure preserves the fresh
staging generation and prints its path. It does not discard partial copies.
Metadata inventories all of the owned `app/models` tree, rejects injected
package fields on generated receipts and derives their SPDX owner from the
closed generated-path mapping.

Root's direct validation passed all ten staging fixture groups and all seven
metadata fixture groups. The first metadata attempt correctly refused a fixture
whose total was null; the repaired fixture computes the explicit integer total.
Root also repaired dropped SPDX package ownership and added a per-file package
relationship assertion. The failed attempt is retained separately. These are
model-free script results, not the tracked full release matrix.

Independent R1 source review identified generated-package injection, incomplete
models-tree inventory, failed-stage deletion and two missing runtime inputs.
All were repaired; R2 reported no P1/P2 findings. The expanded seven metadata
groups then passed in 19.18 seconds. A separate real copy verified 44 pinned
inputs plus generated notices/receipts (1,469,398,280 payload bytes) and five
package records in 25.12 seconds. This private copy does not contain a compiled
application and is not an installer or published bundle.

The new CTest cases are `uct_payload_contract` and
`uct_bundle_metadata_contract`, both serial with a 120-second limit and exact
script/catalog/header runtime inputs.

The first cold runtime check using only the staged SM75/PTX subset failed:
TensorRT's builder explicitly requested SM89 on the Ada test GPU. The failure
and confirmed retirement are retained. The repaired desktop inventory adds
SM86/89/120, and raises only the total staging ceiling to 3 GiB. All ten
model-free staging cases passed again; a fresh 47-file copy and metadata check
verified 2,394,527,371 bytes in 31.95 seconds. The full cached TensorRT archive
also freshly matched its independent `pins.json` hash, and all three new DLLs
matched their exact archive entries. Cold runtime repair validation is pending.

Actual cold TensorRT builds, the SM75
resource and PTX fallback behavior, dynamic runtime dependency closure, full
shipping CPU/CUDA/native/GUI gates, release manifest/SBOM integration and clean
disposable-VM installer acceptance remain separate parent work. This change
alone establishes no public release, hardware acceptance or recognition quality
claim. The parent also owns shared documentation and build-script integration.
