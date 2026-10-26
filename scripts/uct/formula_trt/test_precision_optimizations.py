import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
import numpy as np
import onnx
from onnx import helper as h, TensorProto as T, numpy_helper
from precision_variants import convert_bf16, convert_bf16_wide
from execution_optimizations import EncoderReplay, DecoderBindings


def model_fixture():
    vi=lambda n,t,shape:h.make_tensor_value_info(n,t,shape)
    nodes=[h.make_node('Constant',[],['w'],value=numpy_helper.from_array(np.eye(2,dtype=np.float32))),h.make_node('MatMul',['x','w'],['product']),h.make_node('Add',['product','x'],['residual']),h.make_node('Softmax',['residual'],['answer'],axis=-1),h.make_node('Add',['counter','counter'],['nextCounter'])]
    return h.make_model(h.make_graph(nodes,'precision',[vi('x',T.FLOAT,[1,2]),vi('counter',T.FLOAT,[1])],[vi('answer',T.FLOAT,[1,2]),vi('nextCounter',T.FLOAT,[1])]),opset_imports=[h.make_opsetid('',18)],ir_version=10)


class PrecisionTests(unittest.TestCase):
    def test_linear_bf16_preserves_io_and_stable_ops(self):
        original=model_fixture();converted,receipt=convert_bf16(original)
        self.assertEqual([v.SerializeToString() for v in original.graph.input],[v.SerializeToString() for v in converted.graph.input])
        self.assertEqual([v.SerializeToString() for v in original.graph.output],[v.SerializeToString() for v in converted.graph.output])
        typed=onnx.shape_inference.infer_shapes(converted)
        types={v.name:v.type.tensor_type.elem_type for v in typed.graph.value_info}
        matmul=next(n for n in typed.graph.node if n.op_type=='MatMul')
        self.assertEqual(types[matmul.output[0]],T.BFLOAT16)
        self.assertEqual(types['residual'],T.FLOAT)
        self.assertEqual(len(receipt['operators']),1)
    def test_wide_bf16_keeps_softmax_and_control_fp32(self):
        converted,receipt=convert_bf16_wide(model_fixture())
        typed=onnx.shape_inference.infer_shapes(converted)
        types={v.name:v.type.tensor_type.elem_type for v in list(typed.graph.value_info)+list(typed.graph.input)+list(typed.graph.output)}
        softmax=next(n for n in typed.graph.node if n.op_type=='Softmax')
        counter=next(n for n in typed.graph.node if 'nextCounter' in n.output)
        self.assertEqual(types[softmax.input[0]],T.FLOAT)
        self.assertEqual(types[counter.output[0]],T.FLOAT)
        self.assertGreater(len(receipt['operators']),1)
    def test_nonfinite_weight_refused(self):
        m=model_fixture();m.graph.node[0].attribute[0].t.CopyFrom(numpy_helper.from_array(np.full((2,2),np.nan,dtype=np.float32)))
        with self.assertRaisesRegex(ValueError,'Non-finite'): convert_bf16(m)


class BindingTests(unittest.TestCase):
    def test_each_bank_has_its_own_bindings_and_updates_changed_shape(self):
        class Context:
            def __init__(self): self.calls=[]
            def set_input_shape(self,n,v): self.calls.append(('shape',n,v));return True
            def set_tensor_address(self,n,v): self.calls.append(('address',n,v));return True
            def set_output_allocator(self,n,v): self.calls.append(('allocator',n,v));return True
            def execute_async_v3(self,s): self.calls.append(('execute',s));return True
        contexts=[Context(),Context()];alloc=[{'out':object()},{'out':object()}]
        bindings=DecoderBindings(contexts,alloc)
        for bank in (0,1,0,1):
            bindings.set_input_shape('x',(1,3));bindings.set_tensor_address('x',100+bank);bindings.set_tensor_address('out',200+bank);bindings.set_output_allocator('out',alloc[bank]['out']);self.assertTrue(bindings.execute_async_v3(7))
        for c in contexts: self.assertEqual(sum(v[0]=='address' for v in c.calls),2)
        self.assertGreater(bindings.skipped,0)
        bindings.set_input_shape('x',(1,6));bindings.execute_async_v3(7)
        self.assertEqual(contexts[1].calls[-2],('shape','x',(1,6)))
    def test_encoder_warms_before_capture_then_replays(self):
        calls=[]
        class Context:
            def execute_async_v3(self,s): calls.append('execute');return True
        class CUDA:
            cudaStreamCaptureMode=SimpleNamespace(cudaStreamCaptureModeThreadLocal=1)
            def cudaStreamSynchronize(self,s): calls.append('sync')
            def cudaStreamBeginCapture(self,s,m): calls.append('begin')
            def cudaStreamEndCapture(self,s): calls.append('end');return 'graph'
            def cudaGraphInstantiate(self,g,f): calls.append('instantiate');return 'exec'
            def cudaGraphLaunch(self,g,s): calls.append('launch')
            def cudaGraphExecDestroy(self,g): calls.append('destroy-exec')
            def cudaGraphDestroy(self,g): calls.append('destroy-graph')
        replay=EncoderReplay(Context(),SimpleNamespace(cu=CUDA(),ck=lambda v:v))
        replay.execute_async_v3(7);replay.execute_async_v3(7);replay.destroy()
        self.assertEqual(calls,['execute','sync','begin','execute','end','instantiate','launch','destroy-exec','destroy-graph'])


if __name__=='__main__': unittest.main()
