"""Replace a proven binary Loop state's integer masking with exact Boolean ops."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import onnx
from onnx import helper, numpy_helper, TensorProto
import numpy as np

def rewrite(model):
    changes = []
    def visit(graph, ancestors):
        outer = {**ancestors, **{o: n for n in graph.node for o in n.output}}
        for loop in graph.node:
            for attr in loop.attribute:
                if attr.type != onnx.AttributeProto.GRAPH:
                    continue
                body = attr.g
                if loop.op_type != "Loop" or attr.name != "body":
                    visit(body, outer)
                    continue
                producer = {o: n for n in body.node for o in n.output}
                counts = Counter(s for n in body.node for s in n.input)
                def initial_binary(name, depth=0):
                    if depth > 20:
                        return False
                    n = outer.get(name)
                    if n is None:
                        return False
                    if n.op_type == "Constant":
                        vals = [numpy_helper.to_array(a.t) for a in n.attribute if a.name == "value"]
                        return bool(vals) and bool(np.all((vals[0] == 0) | (vals[0] == 1)))
                    if n.op_type in ("Identity", "Cast", "Expand", "Transpose", "Reshape"):
                        return initial_binary(n.input[0], depth + 1)
                    return False
                def bool_value(name, depth=0):
                    if depth > 20:
                        return False
                    n = producer.get(name) or outer.get(name)
                    if n is None:
                        return False
                    if n.op_type == "Constant":
                        return any(a.name == "value" and a.t.data_type == TensorProto.BOOL for a in n.attribute)
                    if n.op_type == "Cast":
                        dtype = next(a.i for a in n.attribute if a.name == "to")
                        return dtype == TensorProto.BOOL or bool_value(n.input[0], depth + 1)
                    if n.op_type in ("Identity", "Transpose", "Reshape", "Expand"):
                        return bool_value(n.input[0], depth + 1)
                    return n.op_type in ("Equal", "Less", "Greater", "Not", "And", "Or")
                def forwards(name, goal):
                    for _ in range(20):
                        if name == goal:
                            return True
                        n = producer.get(name)
                        if n is None or n.op_type != "Identity":
                            return False
                        name = n.input[0]
                    return False
                replacements, remove = {}, set()
                for idx, n in enumerate(body.node):
                    if n.op_type != "BitwiseAnd":
                        continue
                    a, b = n.input
                    neg = producer.get(b)
                    if neg is None or neg.op_type != "BitwiseNot":
                        raise ValueError("unrecognized integer bitmask pattern")
                    positions = [i for i, v in enumerate(body.input) if v.name == a]
                    if len(positions) != 1 or positions[0] < 2:
                        raise ValueError("mask must be a Loop-carried binary state")
                    pos = positions[0]
                    if not (initial_binary(loop.input[pos]) and
                            bool_value(neg.input[0]) and
                            forwards(body.output[pos - 1].name, n.output[0]) and
                            counts[b] == 1):
                        raise ValueError("binary mask invariant could not be proven")
                    prefix = "okuflow_binary_mask_" + str(idx)
                    replacements[n.name] = [
                        helper.make_node("Cast", [a], [prefix + "_a"], to=TensorProto.BOOL),
                        helper.make_node("Cast", [neg.input[0]], [prefix + "_b"], to=TensorProto.BOOL),
                        helper.make_node("Not", [prefix + "_b"], [prefix + "_not"]),
                        helper.make_node("And", [prefix + "_a", prefix + "_not"], [prefix + "_and"]),
                        helper.make_node("Cast", [prefix + "_and"], list(n.output), to=TensorProto.INT64),
                    ]
                    remove.add(neg.name)
                    changes.append({"node": n.name, "loop": loop.name,
                                    "stateInput": a, "stateIndex": pos,
                                    "proof": "initial binary constant propagated through unary shape/cast ops; feedback is exactly this mask; other operand is a cast Boolean"})
                nodes = []
                for n in body.node:
                    if n.name in remove:
                        continue
                    nodes.extend(replacements.get(n.name, [n]))
                del body.node[:]
                body.node.extend(nodes)
                visit(body, outer)
    visit(model.graph, {})
    onnx.checker.check_model(model)
    return changes

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("source")
    p.add_argument("destination")
    args = p.parse_args()
    source, dest = Path(args.source), Path(args.destination)
    model = onnx.load(source)
    changes = rewrite(model)
    assert changes, "expected an explicit verified transformation"
    onnx.save(model, dest)
    receipt = {"sourceSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
               "resultSha256": hashlib.sha256(dest.read_bytes()).hexdigest(), "changes": changes}
    dest.with_suffix(".rewrite.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    print(json.dumps(receipt, indent=2))
