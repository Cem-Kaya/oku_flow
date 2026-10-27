#pragma once
// Exact immutable upstream model inventory; reconciled against scripts/uct/download-models.json.
namespace okuflow::uct::installation {
inline constexpr char kDownloadCatalog[] = R"models({
  "schema": 1,
  "models": [
    {
      "id": "ocr-det-tiny",
      "name": "PP-OCRv6 tiny",
      "kind": "det",
      "tier": "tiny",
      "directory": "det",
      "revision": "2ba1506c0380b8f0b03dd142459aac66d4421f6c",
      "format": "onnx",
      "files": [
        {
          "name": "inference.onnx",
          "bytes": 1780590,
          "sha256": "193bab7a04fca699a6c82e6abb5b81bdb28177f0abd4062552b04908dafb19f8",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_det_onnx/resolve/2ba1506c0380b8f0b03dd142459aac66d4421f6c/inference.onnx"
        },
        {
          "name": "inference.yml",
          "bytes": 883,
          "sha256": "3ac018be6f97499a08faa3bbdeb33640968d9307f6736d152902747a9f259593",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_det_onnx/resolve/2ba1506c0380b8f0b03dd142459aac66d4421f6c/inference.yml"
        },
        {
          "name": "README.md",
          "bytes": 16064,
          "sha256": "f48628931a5dc994ce5c67e32c94d71b4a7e96b8c5c1b00663284999c3c566bc",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_det_onnx/resolve/2ba1506c0380b8f0b03dd142459aac66d4421f6c/README.md"
        }
      ]
    },
    {
      "id": "ocr-rec-small",
      "name": "PP-OCRv6 small",
      "kind": "rec",
      "tier": "small",
      "directory": "rec",
      "revision": "b8f84f0b80c529de40b4fbb3544b84fa7233a513",
      "format": "onnx",
      "files": [
        {
          "name": "inference.onnx",
          "bytes": 21159378,
          "sha256": "5435fd747c9e0efe15a96d0b378d5bd157e9492ed8fd80edf08f30d02fa24634",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx/resolve/b8f84f0b80c529de40b4fbb3544b84fa7233a513/inference.onnx"
        },
        {
          "name": "inference.yml",
          "bytes": 150579,
          "sha256": "ab078671bb49f06228eadccd34f1bb501e157f7a047095ffb943ba81512c77d1",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx/resolve/b8f84f0b80c529de40b4fbb3544b84fa7233a513/inference.yml"
        },
        {
          "name": "README.md",
          "bytes": 16585,
          "sha256": "7d46dbd183ac09bd5a2e6b4577dcb29240751b1c00eb6813eca308d529f86806",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx/resolve/b8f84f0b80c529de40b4fbb3544b84fa7233a513/README.md"
        }
      ]
    },
    {
      "id": "ocr-det-medium",
      "name": "PP-OCRv6 medium",
      "kind": "det",
      "tier": "medium",
      "directory": "det/medium",
      "revision": "61323801669c338b7891481ec7bac61ce31b576a",
      "format": "onnx",
      "files": [
        {
          "name": "inference.onnx",
          "bytes": 62032837,
          "sha256": "eb13b44b25bb36f89528b68720af8a61d9cf381176107f465db1757b65d086e1",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_det_onnx/resolve/61323801669c338b7891481ec7bac61ce31b576a/inference.onnx"
        },
        {
          "name": "inference.yml",
          "bytes": 886,
          "sha256": "7298d5ead546584af2504d03355f881ac7a7bc0eb1e282d3e159277c1d0af871",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_det_onnx/resolve/61323801669c338b7891481ec7bac61ce31b576a/inference.yml"
        },
        {
          "name": "README.md",
          "bytes": 16089,
          "sha256": "3046e3aab0194a2291bb3941c93b980c2b3a938a24a5be88354968f6d6187ac8",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_det_onnx/resolve/61323801669c338b7891481ec7bac61ce31b576a/README.md"
        }
      ]
    },
    {
      "id": "ocr-rec-medium",
      "name": "PP-OCRv6 medium",
      "kind": "rec",
      "tier": "medium",
      "directory": "rec/medium",
      "revision": "50c7eacafc52fa7bcf4194e8cd08e46f8558504b",
      "format": "onnx",
      "files": [
        {
          "name": "inference.onnx",
          "bytes": 76554979,
          "sha256": "9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_rec_onnx/resolve/50c7eacafc52fa7bcf4194e8cd08e46f8558504b/inference.onnx"
        },
        {
          "name": "inference.yml",
          "bytes": 150580,
          "sha256": "991b700facf5b50a7de193468207d5f4255b538dde0d312ae3b7c7a9b6873129",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_rec_onnx/resolve/50c7eacafc52fa7bcf4194e8cd08e46f8558504b/inference.yml"
        },
        {
          "name": "README.md",
          "bytes": 16587,
          "sha256": "ebce8d28436623ecab4952e24935aed86b3f8ecaf8f8736b92d5544f60fae1e9",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_rec_onnx/resolve/50c7eacafc52fa7bcf4194e8cd08e46f8558504b/README.md"
        }
      ]
    },
    {
      "id": "ocr-det-small",
      "name": "PP-OCRv6 small",
      "kind": "det",
      "tier": "small",
      "directory": "det/small",
      "revision": "28fe5895c24fd108c19eb3e8479f4ab385fbfc62",
      "format": "onnx",
      "files": [
        {
          "name": "inference.onnx",
          "bytes": 9880512,
          "sha256": "d73e0058b7a8086bbd57f3d10b8bcd4ff95363f67e06e2762b5e814fe9c9410e",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx/resolve/28fe5895c24fd108c19eb3e8479f4ab385fbfc62/inference.onnx"
        },
        {
          "name": "inference.yml",
          "bytes": 885,
          "sha256": "193f435274bf9f0b5f71a929bbfbcf148282df7e633b34e7c373e8f44741b516",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx/resolve/28fe5895c24fd108c19eb3e8479f4ab385fbfc62/inference.yml"
        },
        {
          "name": "README.md",
          "bytes": 16076,
          "sha256": "e9403667f0f92055d8ce9e73a2beb1002fc489d9342697db74039eb7178bb5fb",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx/resolve/28fe5895c24fd108c19eb3e8479f4ab385fbfc62/README.md"
        }
      ]
    },
    {
      "id": "ocr-rec-tiny",
      "name": "PP-OCRv6 tiny",
      "kind": "rec",
      "tier": "tiny",
      "directory": "rec/tiny",
      "revision": "2612ab37152ae0a677521bae4e1e3d4fb4cf7c30",
      "format": "onnx",
      "files": [
        {
          "name": "inference.onnx",
          "bytes": 4462639,
          "sha256": "9ef676d6ed3c88256a2d92c640c44f25b0c40947e111b14b8be8f594091563e6",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_rec_onnx/resolve/2612ab37152ae0a677521bae4e1e3d4fb4cf7c30/inference.onnx"
        },
        {
          "name": "inference.yml",
          "bytes": 55571,
          "sha256": "66170210bad538e83fff3c4a3867e547d6bf20b50d64b20347c4b913f3034ea1",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_rec_onnx/resolve/2612ab37152ae0a677521bae4e1e3d4fb4cf7c30/inference.yml"
        },
        {
          "name": "README.md",
          "bytes": 16600,
          "sha256": "e83dd03e515a2d064eaba8ec82248f21dbf6a0813550bafe4c70ee2f6e8fdf2a",
          "url": "https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_rec_onnx/resolve/2612ab37152ae0a677521bae4e1e3d4fb4cf7c30/README.md"
        }
      ]
    }
  ]
}
)models";
}
