"""CPU-only contracts; no model, CUDA, network, app, or owner settings needed."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
import onnx
from onnx import helper as h, TensorProto as T
from onnx.reference import ReferenceEvaluator
from artifacts import digest, write_seal, verify_seal
from rewrite_mask import rewrite
from partition_formula import lower_branches


def mask_model(initial=1,feedback_identity=True,extra_not_use=False):
    vi=lambda n,t,d:h.make_tensor_value_info(n,t,d)
    scalar=lambda n,t,v:h.make_node('Constant',[],[n],name=n,value=h.make_tensor(n,t,[],[v]))
    nodes=[scalar('trip',T.INT64,1),scalar('go',T.BOOL,True),scalar('initial',T.INT64,initial)]
    body_nodes=[scalar('b',T.BOOL,True),h.make_node('Cast',['b'],['b64'],to=T.INT64),h.make_node('BitwiseNot',['b64'],['neg'],name='neg'),h.make_node('BitwiseAnd',['mask','neg'],['next'],name='and')]
    if extra_not_use: body_nodes.append(h.make_node('Identity',['neg'],['another']))
    feedback='next' if feedback_identity else 'mask'
    body=h.make_graph(body_nodes,'body',[vi('iter',T.INT64,[]),vi('condition',T.BOOL,[]),vi('mask',T.INT64,[])],[vi('condition',T.BOOL,[]),vi(feedback,T.INT64,[])])
    nodes.append(h.make_node('Loop',['trip','go','initial'],['out'],body=body,name='loop'))
    model=h.make_model(h.make_graph(nodes,'test',[],[vi('out',T.INT64,[])]),opset_imports=[h.make_opsetid('',18)])
    model.ir_version=10
    return model


class FormulaContracts(unittest.TestCase):
    def test_binary_mask_exact_and_proven(self):
        model=mask_model(); original=deepcopy(model); changes=rewrite(model)
        np.testing.assert_array_equal(ReferenceEvaluator(original).run(None,{})[0],ReferenceEvaluator(model).run(None,{})[0])
        self.assertEqual(len(changes),1)
        body=next(a.g for a in model.graph.node[-1].attribute if a.name=='body')
        self.assertNotIn('BitwiseAnd',[n.op_type for n in body.node])
        for a in (0,1):
            for b in (0,1): self.assertEqual(a & ~b,int(bool(a) and not bool(b)))
    def test_mask_rejects_nonbinary_initial(self):
        with self.assertRaisesRegex(ValueError,'could not be proven'): rewrite(mask_model(initial=2))
    def test_mask_rejects_different_feedback(self):
        with self.assertRaisesRegex(ValueError,'could not be proven'): rewrite(mask_model(feedback_identity=False))
    def test_mask_rejects_shared_integer_not(self):
        with self.assertRaisesRegex(ValueError,'could not be proven'): rewrite(mask_model(extra_not_use=True))
    def test_boolean_scalar_if_is_or(self):
        vi=lambda n:h.make_tensor_value_info(n,T.BOOL,[])
        yes=h.make_graph([h.make_node('Constant',[],['true'],value=h.make_tensor('t',T.BOOL,[],[True]))],'yes',[],[vi('true')])
        no=h.make_graph([h.make_node('Identity',['old'],['no'])],'no',[],[vi('no')])
        graph=h.make_graph([h.make_node('If',['test'],['out'],then_branch=yes,else_branch=no)],'body',[],[])
        nodes,_,_=lower_branches(graph)
        self.assertEqual(nodes[-1].op_type,'Or')
        for test in (False,True):
            for old in (False,True): self.assertEqual(True if test else old,test or old)
    def test_branch_lowering_refuses_nonpure_scalar_branch(self):
        vi=lambda n:h.make_tensor_value_info(n,T.FLOAT,[])
        yes=h.make_graph([h.make_node('Div',['a','b'],['x'])],'yes',[],[vi('x')])
        no=h.make_graph([h.make_node('Identity',['a'],['y'])],'no',[],[vi('y')])
        body=h.make_graph([h.make_node('If',['test'],['out'],then_branch=yes,else_branch=no)],'body',[],[])
        with self.assertRaises(AssertionError): lower_branches(body)
    def test_runtime_seal_binds_engine_source_state_and_profiles(self):
        with tempfile.TemporaryDirectory(prefix='okuflow-formula-contract-') as folder:
            r=Path(folder); meta={'model':'test','oracleKineticTokensEqual':True,'profiles':{'state':{'max':[8]}}}
            (r/'partition.json').write_text(json.dumps(meta)); (r/'initial-states.npz').write_bytes(b'private states')
            for part in ('encoder','decoder'):
                (r/(part+'.onnx')).write_bytes(b'original onnx'); (r/(part+'.plan')).write_bytes(b'actual plan')
                receipt={'model':'test','part':part,'profiles':meta['profiles'],'sourceSha256':digest(r/(part+'.onnx')),'planSha256':digest(r/(part+'.plan'))}
                (r/(part+'.plan.json')).write_text(json.dumps(receipt))
            write_seal(r); verify_seal(r)
            with self.assertRaises(FileExistsError): write_seal(r)
            (r/'initial-states.npz').write_bytes(b'modified')
            with self.assertRaisesRegex(ValueError,'changed'): verify_seal(r)
    def test_seal_refuses_wrong_engine_identity(self):
        with tempfile.TemporaryDirectory(prefix='okuflow-formula-contract-') as folder:
            r=Path(folder); (r/'partition.json').write_text(json.dumps({'model':'test','oracleKineticTokensEqual':True,'profiles':{}}))
            (r/'initial-states.npz').write_bytes(b'')
            (r/'encoder.plan.json').write_text(json.dumps({'model':'other','part':'encoder'}))
            with self.assertRaisesRegex(ValueError,'identity'): write_seal(r)


if __name__=='__main__': unittest.main()
