"""CPU-only graph/quantization contract tests; no real model or GPU required."""
import copy
import unittest

import ml_dtypes
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper
from onnx.reference import ReferenceEvaluator

from precision_models import audit, convert_int4, convert_linear, reduced_tensor
from precision_qdq import eligible, quantize


def model(k=64, n=16):
    weights = np.linspace(-1, 1, k*n, dtype=np.float32).reshape(k, n)
    graph = helper.make_graph([
        helper.make_node("MatMul", ["x", "w"], ["linear"], name="linear"),
        helper.make_node("Softmax", ["linear"], ["y"], axis=-1, name="sensitive")],
        "fixture", [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, k])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, n])],
        [numpy_helper.from_array(weights, "w")])
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 11)]), weights


class PrecisionTests(unittest.TestCase):
    def test_fp16_preserves_interfaces_and_sensitive_operator(self):
        source, _ = model()
        before = source.SerializeToString()
        result, receipt = convert_linear(source, "fp16-linear")
        self.assertEqual(before, source.SerializeToString())
        self.assertEqual(audit(source)["io"], audit(result)["io"])
        self.assertEqual(len(receipt["convertedOperators"]), 1)
        softmax = next(n for n in result.graph.node if n.op_type == "Softmax")
        self.assertEqual(list(softmax.input), ["linear"])
        self.assertEqual(next(n for n in result.graph.node if "linear" in n.output).op_type, "Cast")

    def test_fp16_real_arithmetic_diff_is_bounded_on_fixture(self):
        source, _ = model()
        result, _ = convert_linear(source, "fp16-linear")
        x = np.linspace(-.5, .5, 64, dtype=np.float32).reshape(1, 64)
        expected = ReferenceEvaluator(source).run(None, {"x": x})[0]
        actual = ReferenceEvaluator(result).run(None, {"x": x})[0]
        np.testing.assert_allclose(expected, actual, rtol=.01, atol=.001)

    def test_bf16_opset_is_converted_and_fp32_io_kept(self):
        source, _ = model()
        result, _ = convert_linear(source, "bf16-linear")
        self.assertEqual(result.opset_import[0].version, 22)
        self.assertEqual(audit(source)["io"], audit(result)["io"])
        self.assertIn("BFLOAT16", audit(result)["tensorCounts"])

    def test_bf16_ties_to_even(self):
        values = np.array([0x3f808000, 0x3f818000], dtype=np.uint32).view(np.float32)
        result = reduced_tensor(numpy_helper.from_array(values, "w"), "low", "bf16-linear")
        np.testing.assert_array_equal(np.frombuffer(result.raw_data, dtype="<u2"), [0x3f80, 0x3f82])

    def test_fp16_overflow_refuses_silent_clipping(self):
        source, _ = model()
        source.graph.initializer[0].CopyFrom(numpy_helper.from_array(np.full((64,16),1e20,dtype=np.float32),"w"))
        with self.assertRaisesRegex(ValueError,"overflow"):
            convert_linear(source,"fp16-linear")

    def test_nonfinite_weights_refused(self):
        with self.assertRaisesRegex(ValueError,"Non-finite"):
            reduced_tensor(numpy_helper.from_array(np.array([np.nan],np.float32),"w"),"low","bf16-linear")

    def test_shared_weight_alias_retains_other_fp32_users(self):
        source, weights = model()
        source.graph.node.insert(0, helper.make_node("Identity", ["w"], ["alias"]))
        source.graph.node[1].input[1] = "alias"
        result, _ = convert_linear(source,"fp16-linear")
        self.assertTrue(any(t.name=="w" and t.data_type==TensorProto.FLOAT for t in result.graph.initializer))
        self.assertEqual(list(next(n for n in result.graph.node if n.op_type=="MatMul").input)[1],"__okuflow_precision_weight_0")

    def test_int4_packing_scales_and_error_bound(self):
        source, weights = model()
        result, receipt = convert_int4(source)
        packed = next(t for t in result.graph.initializer if t.data_type==TensorProto.INT4)
        scales = numpy_helper.to_array(next(t for t in result.graph.initializer if t.name.endswith("_scale")))
        raw = np.frombuffer(packed.raw_data,np.uint8)
        q = np.stack([raw&15,raw>>4],axis=1).reshape(-1).astype(np.int8)
        q[q>=8]-=16
        restored=q.reshape(1,64,16)*scales[:,None,:]
        self.assertLessEqual(float(np.max(np.abs(restored.reshape(weights.shape)-weights))),float(scales.max()/2)+1e-6)
        self.assertEqual(len(receipt["convertedOperators"]),1)
        self.assertEqual(audit(source)["io"],audit(result)["io"])
        self.assertFalse(any(t.name=="w" for t in result.graph.initializer))

    def test_int4_incompatible_k_refused(self):
        source,_=model(63)
        with self.assertRaisesRegex(ValueError,"No compatible"):
            convert_int4(source)

    def test_int8_and_fp8_have_signed_qdq_and_fp32_io(self):
        for precision,kind in [("int8-qdq",TensorProto.INT8),("fp8-qdq",TensorProto.FLOAT8E4M3FN)]:
            source,_=model()
            result,receipt=quantize(source,precision,{"x":1.,"linear":12.})
            self.assertIn(kind,[t.data_type for t in result.graph.initializer])
            self.assertEqual(audit(source)["io"],audit(result)["io"])
            self.assertEqual(sum(n.op_type=="QuantizeLinear" for n in result.graph.node),2)
            self.assertEqual(sum(n.op_type=="DequantizeLinear" for n in result.graph.node),3)
            self.assertEqual(len(receipt["convertedOperators"]),1)

    def test_fp8_non_aligned_weight_is_left_fp32(self):
        source,_=model(64,17)
        selected,skipped=eligible(source,"fp8-qdq")
        self.assertFalse(selected)
        self.assertEqual(len(skipped),1)

    def test_missing_calibration_ranges_refused(self):
        source,_=model()
        with self.assertRaises(KeyError):
            quantize(source,"int8-qdq",{})

    def test_nonfinite_calibration_ranges_refused(self):
        source,_=model()
        with self.assertRaisesRegex(ValueError,"Invalid calibration"):
            quantize(source,"fp8-qdq",{"x":float("nan"),"linear":1.})

    def test_reserved_namespace_refused(self):
        source,_=model()
        source.graph.node[0].output[0]="__okuflow_precision_collision"
        with self.assertRaisesRegex(ValueError,"namespace"):
            convert_linear(source,"fp16-linear")


if __name__=="__main__":
    unittest.main()
