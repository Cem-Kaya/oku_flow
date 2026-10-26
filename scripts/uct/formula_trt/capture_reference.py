"""Capture exact native Paddle inputs and token outputs for backend validation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import sys

assert platform.system() == "Windows"
argp = argparse.ArgumentParser()
argp.add_argument("model")
argp.add_argument('--work-dir',type=Path,required=True)
argp.add_argument('--assets',type=Path,required=True)
args = argp.parse_args()
root=args.work_dir.resolve(); assets=args.assets.resolve()
from model_pins import verify_model
verify_model(assets/"models",args.model)
os.environ["PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK"] = "True"
os.environ["PADDLE_PDX_CACHE_HOME"] = str(root / "paddlex-cache")
dirs = [Path(sys.prefix) / "Lib/site-packages/paddle/libs"]
dirs += list((Path(sys.prefix) / "Lib/site-packages/nvidia").glob("*/bin"))
handles = [os.add_dll_directory(str(p)) for p in dirs if p.is_dir()]
import numpy as np
import paddle
from PIL import Image
from paddleocr import FormulaRecognition

target = root / "reference" / args.model
target.mkdir(parents=True, exist_ok=True)
if (target/'reference.json').exists(): raise FileExistsError('preserve previous reference capture')
manifest = json.loads((assets / "inputs.json").read_text(encoding='utf-8'))
model = FormulaRecognition(model_name=args.model, model_dir=str(assets / "models" / args.model),
                           device="gpu:0", use_tensorrt=False, enable_mkldnn=False, cpu_threads=1)
original = model.paddlex_predictor.infer
captured = {}
def capture(*a, **kw):
    x = kw.get("x", a[0] if a else None)
    assert len(x) == 1 and x[0].dtype == np.float32
    out = original(*a, **kw)
    paddle.device.synchronize()
    captured["x"] = np.array(x[0], copy=True)
    captured["tokens"] = np.array(out[0], copy=True)
    return out
model.paddlex_predictor.infer = capture
rows = []
for item in manifest:
    if not (item["name"].endswith("normalized") or item["name"] == "official-complex"):
        continue
    image = np.asarray(Image.open(assets / "inputs" / item["path"]).convert("RGB"))[:, :, ::-1].copy()
    output = list(model.predict(input=image, batch_size=1))
    assert len(output) == 1
    prefix = target / item["name"]
    np.save(str(prefix) + "-input.npy", captured["x"])
    np.save(str(prefix) + "-tokens.npy", captured["tokens"])
    row = {"input": item["name"], "latex": output[0]["rec_formula"],
           "shape": list(captured["x"].shape), "tokensShape": list(captured["tokens"].shape),
           "inputSha256": hashlib.sha256(captured["x"].tobytes()).hexdigest(),
           "tokensSha256": hashlib.sha256(captured["tokens"].tobytes()).hexdigest()}
    rows.append(row)
    print(json.dumps(row), flush=True)
(target / "reference.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
