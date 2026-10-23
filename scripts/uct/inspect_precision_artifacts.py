"""Read-only graph/plan inventory using an already installed ONNX interpreter.

No inference, shape inference, conversion, checker, TensorRT/CUDA import or
download. A dynamic declaration is NOT evidence of executable width support.
Receipts use exclusive creation; do not overwrite a previous failed inspection.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

LIMIT = 256 * 1024 * 1024


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def graph_inventory(path: Path, expected: str) -> dict:
    import onnx  # Explicit private, existing tool. No package installation.
    from onnx import numpy_helper

    if not 0 < path.stat().st_size < LIMIT:
        raise ValueError("Graph exceeds runtime bounds")
    raw = path.read_bytes()
    actual = hashlib.sha256(raw).hexdigest()
    if actual != expected:
        raise ValueError(f"Source graph mismatch: {path}: {actual}")
    model = onnx.load_model_from_string(raw)
    graph = model.graph
    producers = {name: node for node in graph.node for name in node.output}
    constants = {tensor.name: tensor for tensor in graph.initializer}
    for node in graph.node:
        if node.op_type == "Constant":
            for attr in node.attribute:
                if attr.name == "value" and attr.type == onnx.AttributeProto.TENSOR:
                    constants[node.output[0]] = attr.t
    external = [tensor.name for tensor in constants.values()
                if tensor.data_location == onnx.TensorProto.EXTERNAL or tensor.external_data]
    if external:
        raise ValueError(f"External tensor data requires a separate audit: {external[:8]}")

    def interface(value):
        tensor = value.type.tensor_type
        return {"name": value.name, "type": onnx.TensorProto.DataType.Name(tensor.elem_type),
                "shape": [dim.dim_param or dim.dim_value for dim in tensor.shape.dim]}

    def shape_chain(name, seen=(), depth=0):
        # Report all dependencies of each reshape's shape input. A depth cap is
        # explicit unknown evidence, never treated as a successful shape proof.
        if name in seen or depth >= 24:
            return {"tensor": name, "unresolved": "cycle/depth-bound"}
        if name in constants:
            tensor = constants[name]
            count = 1
            for dim in tensor.dims:
                count *= dim
            return {"tensor": name, "constantShape": list(tensor.dims),
                    "value": numpy_helper.to_array(tensor).tolist() if count <= 32 else "too-large"}
        node = producers.get(name)
        if node is None:
            return {"tensor": name, "unresolved": "graph-input-or-unknown"}
        if node.op_type == "Shape":
            return {"tensor": name, "op": "Shape", "of": list(node.input)}
        return {"tensor": name, "op": node.op_type, "inputs": [
            shape_chain(source, (*seen, name), depth + 1) for source in node.input if source]}

    return {"path": str(path.resolve()), "sha256": actual, "bytes": len(raw),
            "onnxVersion": onnx.__version__, "opsets": [{"domain": o.domain, "version": o.version}
                                                          for o in model.opset_import],
            "inputs": [interface(v) for v in graph.input], "outputs": [interface(v) for v in graph.output],
            "reshapeShapeDependencies": [{"node": n.name, "shape": shape_chain(n.input[1])}
                                          for n in graph.node if n.op_type == "Reshape" and len(n.input) > 1],
            "tail": [{"op": n.op_type, "name": n.name, "inputs": list(n.input), "outputs": list(n.output)}
                     for n in list(graph.node)[-12:]],
            "executionVerifiedWidths": [], "supportVerdict": "declaration-and-dependency-inspection-only"}


def plan_inventory(path: Path) -> dict:
    if not 32 < path.stat().st_size < LIMIT:
        raise ValueError("Plan exceeds runtime bounds")
    with path.open("rb") as source:
        header = source.read(32)
        whole = hashlib.sha256(header)
        payload = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            whole.update(chunk)
            payload.update(chunk)
    return {"path": str(path.resolve()), "bytes": path.stat().st_size,
            "sha256": whole.hexdigest(), "payloadSha256": payload.hexdigest(),
            "payloadDigestMatches": payload.digest() == header,
            "deserialized": False, "approval": "not-established-by-inventory"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--expect-sha256", required=True)
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.output.is_absolute() or not args.output.parent.is_dir():
        parser.error("Use an existing absolute private output parent")
    # Reserve before parsing: a first inspection failure is retained, not lost.
    with args.output.open("x", encoding="utf-8", newline="\n") as target:
        result = {"schema": 1, "operation": "read-only-source-inspection", "inference": False,
                  "preparation": False, "toolSha256": digest(Path(__file__)),
                  "interpreter": str(Path(sys.executable).resolve()),
                  "interpreterSha256": digest(Path(sys.executable))}
        try:
            result["graph"] = graph_inventory(args.graph,args.expect_sha256)
            if args.plan:
                result["plan"] = plan_inventory(args.plan)
            result["outcome"] = "inspected"
        except Exception as error:
            result.update(outcome="failed", error=f"{type(error).__name__}: {error}")
            raise
        finally:
            json.dump(result,target,ensure_ascii=False,indent=2)
            target.write("\n")
    print(args.output)


if __name__ == "__main__":
    main()
