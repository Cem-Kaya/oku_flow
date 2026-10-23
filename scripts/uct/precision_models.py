"""Offline pinned-ONNX audit and explicit linear-island precision conversion.

No TensorRT/CUDA imports, network access or inference. Install ONNX/Numpy only
in a private environment. Outputs are immutable, content-addressed artifacts.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import time

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_new(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(value)


def audit(model: onnx.ModelProto) -> dict:
    tensors = list(model.graph.initializer)
    for node in model.graph.node:
        for attribute in node.attribute:
            if attribute.type == onnx.AttributeProto.TENSOR:
                tensors.append(attribute.t)
            elif attribute.type == onnx.AttributeProto.GRAPH:
                raise ValueError("Subgraphs require a separate precision audit")
    counts, elements = Counter(), Counter()
    for tensor in tensors:
        kind = TensorProto.DataType.Name(tensor.data_type)
        counts[kind] += 1
        elements[kind] += int(np.prod(tensor.dims, dtype=np.int64))
        if tensor.data_location == TensorProto.EXTERNAL:
            raise ValueError("External tensor data is not allowed")
    return {
        "tensorCounts": dict(counts), "tensorElements": dict(elements),
        "operators": dict(Counter(n.op_type for n in model.graph.node)),
        "opsets": [{"domain": o.domain, "version": o.version} for o in model.opset_import],
        "io": [{"name": v.name, "type": TensorProto.DataType.Name(v.type.tensor_type.elem_type),
                "shape": [d.dim_param or d.dim_value for d in v.type.tensor_type.shape.dim]}
               for v in list(model.graph.input) + list(model.graph.output)],
    }


def reduced_tensor(tensor: onnx.TensorProto, name: str, precision: str) -> onnx.TensorProto:
    values = numpy_helper.to_array(tensor).astype(np.float32)
    if not np.isfinite(values).all():
        raise ValueError(f"Non-finite source weight {tensor.name}")
    if precision == "fp16-linear":
        with np.errstate(over="ignore"):
            reduced = values.astype(np.float16)
        if not np.isfinite(reduced).all():
            raise ValueError(f"FP16 weight overflow: {tensor.name}; no silent clipping")
        return numpy_helper.from_array(reduced, name)
    # IEEE round-to-nearest ties-to-even, retaining FP32's exponent width.
    bits = values.view(np.uint32)
    reduced = ((bits + np.uint32(0x7fff) + ((bits >> 16) & 1)) >> 16).astype("<u2")
    output = onnx.TensorProto(name=name, data_type=TensorProto.BFLOAT16, dims=tensor.dims)
    output.raw_data = reduced.tobytes()
    return output


def convert_linear(model: onnx.ModelProto, precision: str) -> tuple[onnx.ModelProto, dict]:
    if precision not in {"fp16-linear", "bf16-linear"}:
        raise ValueError("Explicit fp16-linear or bf16-linear selection required")
    original = audit(model)
    if any(v["type"] != "FLOAT" for v in original["io"]):
        raise ValueError("Expected original FP32 interfaces")
    if any(t.data_type != TensorProto.FLOAT for t in model.graph.initializer
           if t.data_type in {TensorProto.FLOAT16, TensorProto.BFLOAT16, TensorProto.INT8}):
        raise ValueError("Only original unquantized models may be converted")
    result = onnx.ModelProto()
    result.CopyFrom(model)
    if precision == "bf16-linear":
        # ONNX Conv first permits BF16 in schema 22. The official converter
        # preserves changed operator signatures; never just relabel an opset.
        result = onnx.version_converter.convert_version(result, 22)
    low_type = TensorProto.FLOAT16 if precision == "fp16-linear" else TensorProto.BFLOAT16
    tensors = {t.name: t for t in result.graph.initializer}
    aliases = {n.output[0]: n.input[0] for n in result.graph.node if n.op_type == "Identity"}
    def constant(name: str):
        seen = set()
        while name in aliases and name not in seen:
            seen.add(name)
            name = aliases[name]
        return tensors.get(name)
    existing = {x for n in result.graph.node for x in list(n.input) + list(n.output)} | set(tensors)
    prefix = "__okuflow_precision_"
    if any(name.startswith(prefix) for name in existing):
        raise ValueError("Reserved conversion namespace already exists")
    nodes, weights, changed = [], {}, []
    for index, node in enumerate(result.graph.node):
        if node.domain or node.op_type not in {"Conv", "MatMul", "Gemm"}:
            nodes.append(node)
            continue
        if len(node.output) != 1:
            raise ValueError("Unexpected linear operator output arity")
        converted = onnx.NodeProto()
        converted.CopyFrom(node)
        for slot, name in enumerate(node.input):
            if not name:
                continue
            tensor = constant(name)
            if tensor is not None:
                if tensor.data_type != TensorProto.FLOAT:
                    raise ValueError("A linear operand is not FP32")
                if tensor.name not in weights:
                    low_name = f"{prefix}weight_{len(weights)}"
                    low = reduced_tensor(tensor, low_name, precision)
                    result.graph.initializer.append(low)
                    weights[tensor.name] = low_name
                converted.input[slot] = weights[tensor.name]
            else:
                low_name = f"{prefix}{index}_input_{slot}"
                nodes.append(helper.make_node("Cast", [name], [low_name], to=low_type,
                                              name=low_name))
                converted.input[slot] = low_name
        low_output = f"{prefix}{index}_output"
        converted.output[0] = low_output
        nodes.append(converted)
        nodes.append(helper.make_node("Cast", [low_output], list(node.output), to=TensorProto.FLOAT,
                                      name=low_output + "_fp32"))
        changed.append({"index": index, "name": node.name, "op": node.op_type})
    del result.graph.node[:]
    result.graph.node.extend(nodes)
    # Keep original constants if other FP32 operators/Identity aliases use them.
    used = {name for node in nodes for name in node.input} | {v.name for v in result.graph.input}
    kept = [tensor for tensor in result.graph.initializer if tensor.name in used]
    del result.graph.initializer[:]
    result.graph.initializer.extend(kept)
    if not changed:
        raise ValueError("No eligible linear operations")
    onnx.checker.check_model(result, full_check=True)
    if audit(result)["io"] != original["io"]:
        raise ValueError("Conversion changed an interface shape/type")
    return result, {"convertedOperators": changed, "reducedWeightTensors": len(weights),
                    "strategy": "explicit lower-type Conv/MatMul/Gemm islands; FP32 interfaces and other operators"}


def convert_int4(model: onnx.ModelProto) -> tuple[onnx.ModelProto, dict]:
    """Symmetric block-64 RTN, weight-only MatMul; FP32 activation/compute.

    No activation calibration is needed for this deliberately simple WoQ
    candidate. This is not activation INT4 or NVFP4, and no speedup is assumed.
    """
    original = audit(model)
    result = onnx.version_converter.convert_version(model, 21)
    tensors = {t.name: t for t in result.graph.initializer}
    aliases = {n.output[0]: n.input[0] for n in result.graph.node if n.op_type == "Identity"}
    nodes, changed = [], []
    prefix = "__okuflow_int4_"
    if any(x.startswith(prefix) for n in result.graph.node for x in n.output):
        raise ValueError("Reserved INT4 namespace already exists")
    for index, node in enumerate(result.graph.node):
        name = node.input[1] if node.op_type == "MatMul" and not node.domain else ""
        seen = set()
        while name in aliases and name not in seen:
            seen.add(name)
            name = aliases[name]
        tensor = tensors.get(name)
        if tensor is None or tensor.data_type != TensorProto.FLOAT or len(tensor.dims) != 2 or tensor.dims[0] % 64:
            nodes.append(node)
            continue
        values = numpy_helper.to_array(tensor)
        if not np.isfinite(values).all():
            raise ValueError("Non-finite INT4 weight")
        k, n = values.shape
        blocks = values.reshape(k//64, 64, n)
        scales = np.max(np.abs(blocks), axis=1).astype(np.float32) / np.float32(7)
        scales[scales == 0] = 1
        quantized = np.clip(np.rint(blocks/scales[:, None, :]), -8, 7).astype(np.int8).reshape(-1)
        nibbles = (quantized.astype(np.uint8) & 15)
        if nibbles.size % 2:
            nibbles = np.append(nibbles, np.uint8(0))
        packed = nibbles[0::2] | (nibbles[1::2] << 4)
        qname, sname, dqname = (f"{prefix}{index}_{suffix}" for suffix in ("weight", "scale", "dequantized"))
        qweight = onnx.TensorProto(name=qname, data_type=TensorProto.INT4, dims=tensor.dims, raw_data=packed.tobytes())
        result.graph.initializer.extend([qweight, numpy_helper.from_array(scales, sname)])
        nodes.append(helper.make_node("DequantizeLinear", [qname, sname], [dqname],
                                      axis=0, block_size=64, name=dqname))
        converted = onnx.NodeProto()
        converted.CopyFrom(node)
        converted.input[1] = dqname
        nodes.append(converted)
        changed.append({"name": node.name, "shape": [k, n], "blockAxis": 0, "blockSize": 64})
    if not changed:
        raise ValueError("No compatible constant block-64 MatMul weights")
    # Remove only dead computation, including original weight Identity aliases.
    live = {v.name for v in result.graph.output}
    kept_nodes = []
    for node in reversed(nodes):
        if any(x in live for x in node.output):
            kept_nodes.append(node)
            live.update(node.input)
    del result.graph.node[:]
    result.graph.node.extend(reversed(kept_nodes))
    kept_weights = [t for t in result.graph.initializer if t.name in live]
    del result.graph.initializer[:]
    result.graph.initializer.extend(kept_weights)
    onnx.checker.check_model(result, full_check=True)
    if audit(result)["io"] != original["io"]:
        raise ValueError("INT4 conversion changed graph interfaces")
    return result, {"convertedOperators": changed, "strategy": "INT4 weight-only symmetric block64 RTN; FP32 activation/compute"}


def pinned_models(runtime: Path, pins: Path):
    for pin in json.loads(pins.read_text(encoding="utf-8"))["models"]:
        path = runtime / pin["directory"] / "inference.onnx"
        digest = sha256(path)
        if digest != pin["files"]["inference.onnx"]:
            raise ValueError(f"Pinned original mismatch: {path}")
        model = onnx.load(path, load_external_data=False)
        yield pin, path, digest, model


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["audit", "convert"])
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--pins", type=Path, default=Path(__file__).with_name("pins.json"))
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--precision", choices=["fp16-linear", "bf16-linear", "int4-woq"])
    parser.add_argument("--roles", nargs="+", choices=["det", "rec"])
    args = parser.parse_args()
    if not args.output_root.is_absolute():
        parser.error("An absolute private output root is required")
    records = []
    for pin, path, digest, model in pinned_models(args.runtime, args.pins):
        record = {"role": pin["kind"], "tier": pin["tier"], "path": str(path),
                  "sourceSha256": digest, "sha256": digest, "precision": "fp32", "audit": audit(model)}
        if args.action == "convert":
            if not args.precision or not args.roles:
                parser.error("Choose --precision and --roles explicitly")
            if pin["tier"] != "medium" or pin["kind"] not in args.roles:
                continue
            parent = args.output_root / "models" / args.precision / digest
            if parent.exists():
                raise ValueError(f"Preparation already attempted; preserve it: {parent}")
            parent.mkdir(parents=True)
            start = time.perf_counter()
            try:
                converted, details = convert_int4(model) if args.precision == "int4-woq" else convert_linear(model, args.precision)
                data = converted.SerializeToString()
                converted_hash = hashlib.sha256(data).hexdigest()
                output = parent / f"{converted_hash}.onnx"
                write_new(output, data)
                record.update(path=str(output), sha256=converted_hash, precision=args.precision,
                              audit=audit(converted), conversion=details,
                              conversionSeconds=time.perf_counter() - start)
                write_new(parent / "manifest.json", json.dumps(record, indent=2).encode())
            except Exception as error:
                write_new(parent / "failed.json", json.dumps({"sourceSha256": digest,
                          "error": str(error), "elapsedSeconds": time.perf_counter()-start}, indent=2).encode())
                raise
        records.append(record)
    output = args.output_root / ("original-audit.json" if args.action == "audit" else
                                f"{args.precision}-{'-'.join(args.roles)}.json")
    write_new(output, json.dumps({"schema": 1, "onnx": onnx.__version__, "numpy": np.__version__,
                                 "records": records}, indent=2).encode())
    print(output)


if __name__ == "__main__":
    main()
