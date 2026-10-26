"""Compile a specified partition once with the installed native TensorRT SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import time
assert platform.system() == "Windows"
argp = argparse.ArgumentParser()
argp.add_argument('model')
argp.add_argument('part', choices=['encoder','decoder'])
argp.add_argument('--work-dir',type=Path,required=True)
argp.add_argument('--sdk',type=Path,required=True)
argp.add_argument('--cuda-bin',type=Path,required=True)
argp.add_argument('--partition',default='partition-v2')
argp.add_argument('--optimization-level',type=int,choices=range(6),default=2)
args=argp.parse_args()
root=args.work_dir.resolve(); sdk=args.sdk.resolve(); cuda=args.cuda_bin.resolve()
os.environ['PATH']=os.pathsep.join((str(sdk/'bin'),str(cuda),os.environ.get('PATH','')))
handles=[os.add_dll_directory(str(p)) for p in (sdk/'bin',cuda)]
import tensorrt as trt
directory = root / args.partition / args.model
source = directory / (args.part + ".onnx")
destination = directory / (args.part + ".plan")
assert not destination.exists(), "preserve prepared plans; choose a new explicit destination"
meta = json.loads((directory / "partition.json").read_text(encoding='utf-8'))
log = trt.Logger(trt.Logger.INFO)
builder = trt.Builder(log)
net = builder.create_network()
parser = trt.OnnxParser(net, log)
assert parser.parse_from_file(str(source)), [str(parser.get_error(i)) for i in range(parser.num_errors)]
config = builder.create_builder_config()
config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 512 * 1024 * 1024)
config.builder_optimization_level = args.optimization_level
policy=meta.get('precisionPolicy',{'linear':'fp32','tf32':False})
if policy['tf32']: config.set_flag(trt.BuilderFlag.TF32)
else: config.clear_flag(trt.BuilderFlag.TF32)
precision_label=('BF16 neural weights/activations; FP32 norm/Softmax/control/interfaces' if policy.get('strategy')=='bf16-wide' else ('BF16 linear islands; FP32 interfaces/stable math' if policy['linear']=='bf16' else 'FP32 graph')) + ('; TF32 permitted' if policy['tf32'] else '; TF32 disabled')
config.profiling_verbosity = trt.ProfilingVerbosity.DETAILED
profile = builder.create_optimization_profile()
profiles = {}
for index in range(net.num_inputs):
    tensor = net.get_input(index)
    if args.part == "encoder":
        shape = tuple(1 if d == -1 else d for d in tensor.shape)
        spec = {"min": shape, "opt": shape, "max": shape}
    else:
        spec = meta["profiles"][tensor.name]
    print("profile", tensor.name, spec, flush=True)
    profile.set_shape(tensor.name, spec["min"], spec["opt"], spec["max"])
    assert [tuple(v) for v in profile.get_shape(tensor.name)] == [tuple(spec[k]) for k in ("min", "opt", "max")]
    profiles[tensor.name] = spec
config.add_optimization_profile(profile)
started = time.perf_counter()
plan = builder.build_serialized_network(net, config)
elapsed = time.perf_counter() - started
assert plan is not None, "TensorRT build failed; no Paddle/ORT fallback"
destination.write_bytes(bytes(plan))
receipt = {"model": args.model, "part": args.part, "trtVersion": trt.__version__,
           "precision": precision_label, "precisionPolicy":policy, "buildSeconds": elapsed, "optimizationLevel":args.optimization_level,
           "sourceSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
           "planSha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
           "profiles": profiles, "layers": net.num_layers}
destination.with_suffix(".plan.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
print(json.dumps(receipt, indent=2), flush=True)
