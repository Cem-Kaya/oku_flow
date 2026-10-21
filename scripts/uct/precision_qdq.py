"""Finite CPU calibration and explicit symmetric INT8/FP8 ONNX Q/DQ.

Consumes native, original-pixel reference tensors exported by the opt-in
precision diagnostic. No CUDA provider, ModelOpt dependency or GPU is loaded.
Run only in the granted serial block; all data stays in the private work root.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import time

import ml_dtypes
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

from precision_models import audit, sha256, write_new


def eligible(model, precision):
    weights = {t.name: t for t in model.graph.initializer}
    aliases = {n.output[0]: n.input[0] for n in model.graph.node if n.op_type == "Identity"}
    def tensor(name):
        seen = set()
        while name in aliases and name not in seen:
            seen.add(name)
            name = aliases[name]
        return weights.get(name)
    selected, skipped = [], []
    for index, node in enumerate(model.graph.node):
        if node.domain or node.op_type not in {"Conv", "MatMul"}:
            continue
        w = tensor(node.input[1])
        if w is None:
            skipped.append({"name": node.name, "reason": "dynamic MatMul operands left FP32"})
            continue
        if precision == "fp8-qdq":
            group = next((a.i for a in node.attribute if a.name == "group"), 1)
            if any(d % 16 for d in w.dims[:2]) or group != 1:
                skipped.append({"name": node.name, "reason": "conservative FP8 channel alignment/group requirement"})
                continue
        selected.append((index, node, w))
    return selected, skipped


def calibrate(model, samples, role, output):
    import onnxruntime as ort
    # Calibrate the superset once; both INT8 and FP8 consume the same ranges.
    selected, _ = eligible(model, "int8-qdq")
    names = sorted({name for _, n, _ in selected for name in [n.input[0], n.output[0]]})
    augmented = copy.deepcopy(model)
    del augmented.graph.output[:]
    augmented.graph.output.extend(helper.make_tensor_value_info(name, TensorProto.FLOAT, None) for name in names)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    options.inter_op_num_threads = 1
    options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    session = ort.InferenceSession(augmented.SerializeToString(), options, providers=["CPUExecutionProvider"])
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise ValueError("Calibration must use CPUExecutionProvider only")
    ranges = {name: 0.0 for name in names}
    receipts = []
    chosen = [s for s in samples if s["role"] == role]
    limit = 8 if role == "det" else 16
    if not 4 <= len(chosen) <= limit:
        raise ValueError(f"Require 4-{limit} explicit {role} calibration tensors")
    for sample in chosen:
        path = Path(sample["path"])
        if sha256(path) != sample["sha256"] or sample["dtype"] != "little-endian-float32":
            raise ValueError("Calibration tensor identity/dtype mismatch")
        shape = sample["shape"]
        if len(shape) != 4 or shape[:2] != [1, 3] or max(shape[2:]) > 1024:
            raise ValueError("Calibration shape outside native contract")
        data = np.fromfile(path, dtype="<f4").reshape(shape)
        if not np.isfinite(data).all():
            raise ValueError("Nonfinite calibration input")
        start = time.perf_counter()
        values = session.run(names, {session.get_inputs()[0].name: data})
        for name, value in zip(names, values):
            if not np.isfinite(value).all():
                raise ValueError(f"Nonfinite calibration activation: {name}")
            ranges[name] = max(ranges[name], float(np.max(np.abs(value))))
        receipts.append({"tensorSha256": sample["sha256"], "sourceSha256": sample["sourceSha256"],
                         "seconds": time.perf_counter() - start})
        del values
    record = {"schema": 1, "method": "symmetric per-tensor max-absolute; no percentile tuning or evaluation feedback",
              "provider": session.get_providers(), "onnxruntime": ort.__version__, "ranges": ranges, "samples": receipts}
    write_new(output, json.dumps(record, indent=2).encode())
    return ranges


def quantize(model, precision, ranges):
    if precision not in {"int8-qdq", "fp8-qdq"}:
        raise ValueError("Explicit INT8/FP8 QDQ selection required")
    original_io = audit(model)["io"]
    result = onnx.version_converter.convert_version(model, 19)
    selected, skipped = eligible(result, precision)
    if not selected:
        raise ValueError("No compatible QDQ operations; no precision substitution is allowed")
    selected = {index: (node, weight) for index, node, weight in selected}
    kind = TensorProto.INT8 if precision == "int8-qdq" else TensorProto.FLOAT8E4M3FN
    maximum = 127.0 if kind == TensorProto.INT8 else 448.0
    prefix = "__okuflow_qdq_"
    if any(x.startswith(prefix) for n in result.graph.node for x in n.output):
        raise ValueError("Reserved QDQ namespace already exists")
    nodes, changed = [], []
    def scale(name, amax):
        if not np.isfinite(amax) or amax < 0:
            raise ValueError(f"Invalid calibration range: {name}")
        value = np.float32(amax / maximum) if amax else np.float32(1)
        if value <= 0:
            raise ValueError("Unrepresentable calibration scale")
        sname, zname = name + "_scale", name + "_zero"
        result.graph.initializer.append(numpy_helper.from_array(np.asarray(value), sname))
        result.graph.initializer.append(onnx.TensorProto(name=zname, data_type=kind, raw_data=b"\0"))
        return sname, zname, value
    def qdq(input_name, name, amax, destination=None):
        sname, zname, _ = scale(name, amax)
        qname, dqname = name + "_q", destination or name + "_dq"
        nodes.extend([helper.make_node("QuantizeLinear", [input_name, sname, zname], [qname], name=qname),
                      helper.make_node("DequantizeLinear", [qname, sname, zname], [dqname], name=name + "_dequantize")])
        return dqname
    for index, node in enumerate(result.graph.node):
        if index not in selected:
            nodes.append(node)
            continue
        _, weight = selected[index]
        converted = copy.deepcopy(node)
        name = f"{prefix}{index}"
        converted.input[0] = qdq(node.input[0], name + "_activation", ranges[node.input[0]])
        values = numpy_helper.to_array(weight).astype(np.float32)
        sname, zname, value = scale(name + "_weight", float(np.max(np.abs(values))))
        if kind == TensorProto.INT8:
            qvalues = np.clip(np.rint(values/value), -128, 127).astype(np.int8)
        else:
            qvalues = np.clip(values/value, -448, 448).astype(ml_dtypes.float8_e4m3fn)
        qname, dqname = name + "_weight_q", name + "_weight_dq"
        result.graph.initializer.append(onnx.TensorProto(name=qname, data_type=kind, dims=weight.dims,
                                                       raw_data=qvalues.tobytes()))
        nodes.append(helper.make_node("DequantizeLinear", [qname, sname, zname], [dqname], name=dqname))
        converted.input[1] = dqname
        raw_output = name + "_raw_output"
        converted.output[0] = raw_output
        nodes.append(converted)
        qdq(raw_output, name + "_output", ranges[node.output[0]], node.output[0])
        changed.append({"name": node.name, "operator": node.op_type})
    del result.graph.node[:]
    result.graph.node.extend(nodes)
    # Keep type annotations, shapes, original FP32 I/O and sensitive operators.
    onnx.checker.check_model(result, full_check=True)
    if audit(result)["io"] != original_io:
        raise ValueError("QDQ changed interface")
    return result, {"convertedOperators": changed, "skippedOperators": skipped,
                    "strategy": "signed symmetric QDQ linear islands, calibrated FP32 dequantization; other operators FP32"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--evaluation-audit", type=Path, required=True)
    parser.add_argument("--role", choices=["det", "rec"], required=True)
    parser.add_argument("--precision", choices=["int8-qdq", "fp8-qdq"], required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    if sha256(args.original) != args.source_sha256:
        raise ValueError("Original graph hash mismatch")
    inputs = json.loads(args.inputs.read_text(encoding="utf-8-sig"))
    evaluation = json.loads(args.evaluation_audit.read_text(encoding="utf-8-sig"))
    evaluation_hashes = {s["sha256"].lower() for s in evaluation["sources"]}
    if not evaluation_hashes or any(s["sourceSha256"].lower() in evaluation_hashes for s in inputs["samples"]):
        raise ValueError("Calibration/evaluation source overlap or missing evaluation identity")
    model = onnx.load(args.original, load_external_data=False)
    parent = args.output_root / "models" / args.precision / args.source_sha256
    if parent.exists():
        raise ValueError(f"Preparation already attempted: {parent}")
    parent.mkdir(parents=True)
    start = time.perf_counter()
    try:
        cache = args.output_root / "calibration" / args.source_sha256 / sha256(args.inputs)
        cache.mkdir(parents=True, exist_ok=True)
        cache_file = cache / "ranges.json"
        ranges = json.loads(cache_file.read_text())["ranges"] if cache_file.exists() else calibrate(model, inputs["samples"], args.role, cache_file)
        result, details = quantize(model, args.precision, ranges)
        data = result.SerializeToString()
        digest = hashlib.sha256(data).hexdigest()
        path = parent / f"{digest}.onnx"
        write_new(path, data)
        record = {"schema": 1, "role": args.role, "tier": "medium", "precision": args.precision,
                  "sourceSha256": args.source_sha256, "sha256": digest, "path": str(path),
                  "audit": audit(result), "conversion": details, "calibrationInputsSha256": sha256(args.inputs),
                  "rangesSha256": sha256(cache_file), "evaluationAuditSha256": sha256(args.evaluation_audit),
                  "preparationSeconds": time.perf_counter()-start}
        write_new(parent / "manifest.json", json.dumps(record, indent=2).encode())
        print(parent / "manifest.json")
    except Exception as error:
        write_new(parent / "failed.json", json.dumps({"error": str(error), "seconds": time.perf_counter()-start}, indent=2).encode())
        raise


if __name__ == "__main__":
    main()
