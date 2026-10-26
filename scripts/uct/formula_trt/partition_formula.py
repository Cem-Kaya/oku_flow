"""Native TensorRT formula partition: hoist invariant cross-attention KV, no If scopes."""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import onnx
from onnx import helper, TensorProto
import numpy as np


def node_uses(node):
    used = {s for s in node.input if s}
    for a in node.attribute:
        if a.type == onnx.AttributeProto.GRAPH:
            defined = {v.name for v in a.g.input} | {o for n in a.g.node for o in n.output}
            used |= {s for n in a.g.node for s in node_uses(n) if s not in defined}
    return used


def needed_nodes(nodes, outputs):
    wanted, selected = set(outputs), []
    for n in reversed(nodes):
        if set(n.output) & wanted:
            selected.append(n)
            wanted |= node_uses(n)
    return list(reversed(selected))


def model_graph(source, nodes, inputs, outputs, name):
    g = helper.make_graph(deepcopy(nodes), name, deepcopy(inputs), deepcopy(outputs))
    m = helper.make_model(g, opset_imports=deepcopy(source.opset_import), producer_name='OkuFlow formula loop partition')
    m.ir_version = source.ir_version
    onnx.checker.check_model(m)
    return m


def lower_branches(body):
    lowered, cross_outputs, proof = [], [], []
    for n in body.node:
        if n.op_type != 'If':
            lowered.append(deepcopy(n)); continue
        branches = {a.name: a.g for a in n.attribute}
        yes, no = branches['then_branch'], branches['else_branch']
        if len(n.output) == 2 and all(q.op_type == 'Identity' for q in yes.node):
            # Identity-only cached cross-attention branch, computed branch is
            # proved invariant by the caller before the rewrite is accepted.
            assert len(yes.node) == 2 and all(o.type.tensor_type.elem_type == TensorProto.FLOAT and len(o.type.tensor_type.shape.dim) == 4 for o in no.output)
            assert not any(q.op_type in ('If','Loop','RandomNormal','RandomUniform','Dropout') for q in no.node)
            lowered.extend(deepcopy(list(no.node)))
            for original, result in zip(no.output, n.output):
                lowered.append(helper.make_node('Identity', [original.name], [result]))
            cross_outputs.extend(n.output)
            proof.append({'node':n.name, 'rewrite':'compute cross-attention from invariant encoder projection; reshape in decoder', 'outputs':list(n.output)})
        else:
            assert len(n.output) == 1
            assert all(q.op_type in ('Identity','Constant','Mul','Add') for g in (yes,no) for q in g.node)
            for g in (yes,no):
                lowered.extend(deepcopy(list(g.node)))
            # Broadcast-compatible single-element scalar controls preserve values.
            if yes.output[0].type.tensor_type.elem_type == TensorProto.BOOL:
                value = next(a.t for q in yes.node for a in q.attribute if q.op_type=='Constant' and a.name=='value')
                assert bool(onnx.numpy_helper.to_array(value))
                lowered.append(helper.make_node('Or',[n.input[0],no.output[0].name],list(n.output)))
            else:
                lowered.append(helper.make_node('Where', [n.input[0], yes.output[0].name, no.output[0].name], list(n.output)))
            proof.append({'node':n.name, 'rewrite':'pure scalar branches to Where'})
    return lowered, cross_outputs, proof


def partition(args):
    root = args.work_dir.resolve()
    outdir = root / args.partition / args.model
    if (outdir/'partition.json').exists(): raise FileExistsError('preserve prepared partition; use a new --partition name')
    outdir.mkdir(parents=True, exist_ok=True)
    from model_pins import verify_model, verify_export
    verify_export(root,verify_model(args.model_root,args.model))
    source = root / 'onnx' / (args.model + '.trt-mask.onnx')
    m = onnx.load(source)
    m.graph.input[0].type.tensor_type.shape.dim[0].dim_value = 1
    m = onnx.shape_inference.infer_shapes(m, data_prop=True)
    loop = next(n for n in m.graph.node if n.op_type == 'Loop')
    body = next(a.g for a in loop.attribute if a.name == 'body')
    global_nodes = list(m.graph.node)[:list(m.graph.node).index(loop)]
    global_producer = {o:n for n in global_nodes for o in n.output}
    types = {v.name:deepcopy(v) for g in (m.graph,body) for v in list(g.input)+list(g.value_info)+list(g.output)}
    for n in body.node:
        for a in n.attribute:
            if a.type == onnx.AttributeProto.GRAPH:
                types.update({v.name:deepcopy(v) for v in a.g.output})
    nodes, cross_outputs, proof = lower_branches(body)
    invariant = set(global_producer) - {v.name for v in body.input}
    invariant_nodes = []
    for n in nodes:
        if node_uses(n) <= invariant:
            invariant_nodes.append(n)
            invariant.update(n.output)
    # Cross-KV recomputation is retained until its batch-shape projection is hoisted.
    invariant_ids = {id(n) for n in invariant_nodes}
    final_state_index = list(loop.output).index(m.graph.output[0].name) + 2
    wanted = {body.output[0].name,body.output[final_state_index-1].name}
    while True:
        selected = needed_nodes(nodes, wanted)
        used = {s for n in selected for s in node_uses(n)}
        state_indices = [i for i in range(2,len(body.input)) if body.input[i].name in used]
        extended = wanted | {body.output[i-1].name for i in state_indices}
        if extended == wanted: break
        wanted = extended
    assert final_state_index in state_indices
    assert body.input[0].name not in used and body.input[1].name not in used
    dynamic_nodes = [n for n in selected if id(n) not in invariant_ids]
    dynamic_used = {s for n in dynamic_nodes for s in node_uses(n)}
    inv_producer = {**global_producer, **{o:n for n in invariant_nodes for o in n.output}}
    constants, invariant_names = [], []
    for name in sorted(dynamic_used & invariant):
        n = inv_producer[name]
        if n.op_type == 'Constant':
            if n not in constants: constants.append(n)
        else: invariant_names.append(name)
    all_encoder_nodes = global_nodes + invariant_nodes
    producer = {o:n for n in all_encoder_nodes for o in n.output}
    def depends_on_pixels(name, seen=None):
        seen = set() if seen is None else seen
        if name in seen: return False
        seen.add(name)
        if name == m.graph.input[0].name: return True
        n = producer[name]
        if n.op_type in ('Constant','Shape'): return False
        return any(depends_on_pixels(s,seen) for s in n.input if s)
    def value_info(name):
        if name in types: return types[name]
        n = producer[name]
        if n.op_type == 'Constant':
            dtype = next(a.t.data_type for a in n.attribute if a.name == 'value')
        elif n.op_type == 'Cast':
            dtype = next(a.i for a in n.attribute if a.name == 'to')
        else:
            dtype = value_info(n.input[0]).type.tensor_type.elem_type
        types[name] = helper.make_tensor_value_info(name,dtype,[])
        return types[name]
    for name in invariant_names: value_info(name)
    initial_names = [loop.input[i] for i in state_indices]
    assert not any(depends_on_pixels(s) for s in initial_names)
    prelude = model_graph(m,needed_nodes(all_encoder_nodes,invariant_names+initial_names),list(m.graph.input),[types[n] for n in invariant_names+initial_names],'formula_preparation')
    onnx.save(prelude,outdir/'prelude-reference.onnx')
    import onnxruntime as ort
    options=ort.SessionOptions(); options.intra_op_num_threads=8; options.log_severity_level=3
    pre=ort.InferenceSession(prelude.SerializeToString(),options,providers=['CPUExecutionProvider'])
    x=np.load(root/'reference'/args.model/'chalk-kinetic-normalized-input.npy')
    values=dict(zip([o.name for o in pre.get_outputs()],pre.run(None,{m.graph.input[0].name:x})))
    for n in list(invariant_names):
        if not depends_on_pixels(n):
            constants.append(helper.make_node('Constant',[],[n],value=onnx.numpy_helper.from_array(values[n],n)))
            invariant_names.remove(n)
    for n in invariant_names:
        types[n]=helper.make_tensor_value_info(n,helper.np_dtype_to_tensor_dtype(values[n].dtype),list(values[n].shape))
    encoder = model_graph(m,needed_nodes(all_encoder_nodes,invariant_names),list(m.graph.input),[types[n] for n in invariant_names],'formula_encoder_with_cross_kv')
    onnx.save(encoder,outdir/'encoder.onnx')
    inputs = [deepcopy(body.input[i]) for i in state_indices]+[deepcopy(types[n]) for n in invariant_names]
    outputs = [deepcopy(body.output[0])]+[deepcopy(body.output[i-1]) for i in state_indices]
    # Fixed rank-one controls avoid the exporter's rank-zero/rank-one ambiguity.
    for v in inputs+outputs:
        if all(d.HasField('dim_value') and d.dim_value==1 for d in v.type.tensor_type.shape.dim):
            v.type.tensor_type.shape.ClearField('dim')
            v.type.tensor_type.shape.dim.add().dim_value = 1
    decoder = model_graph(m,constants+dynamic_nodes,inputs,outputs,'formula_decoder_iteration')
    assert not any(n.op_type in ('If','Loop') for n in decoder.graph.node)
    onnx.save(decoder,outdir/'decoder-reference.onnx')
    dec = ort.InferenceSession(decoder.SerializeToString(),options,providers=['CPUExecutionProvider'])
    states={body.input[i].name:values[loop.input[i]] for i in state_indices}
    states={n:a.reshape([1]) if a.ndim==0 else a for n,a in states.items()}
    np.savez(outdir/'initial-states.npz',**states)
    invariants={n:values[n] for n in invariant_names}
    for v in encoder.graph.output:
        a=values[v.name]
        v.type.CopyFrom(helper.make_tensor_value_info(v.name,helper.np_dtype_to_tensor_dtype(a.dtype),list(a.shape)).type)
    onnx.save(encoder,outdir/'encoder.onnx')
    state_outputs={body.input[i].name:body.output[i-1].name for i in state_indices}
    feed0={**states,**invariants}
    def step(feed): return dict(zip([o.name for o in dec.get_outputs()],dec.run(None,feed)))
    v1=step(feed0)
    feed1={**{n:v1[out] for n,out in state_outputs.items()},**invariants}
    v2=step(feed1)
    config=json.loads((args.model_root/args.model/'config.json').read_text(encoding='utf-8'))
    max_seq=next(op['UniMERNetLabelEncode']['max_seq_len'] for op in config['PreProcess']['transform_ops'] if 'UniMERNetLabelEncode' in op)
    profiles={}
    for vi in decoder.graph.input:
        n=vi.name
        a,b,c=feed0[n].shape,feed1[n].shape,(v2[state_outputs[n]].shape if n in states else feed0[n].shape)
        low,opt,high=[],[],[]
        for av,bv,cv in zip(a,b,c):
            if cv>bv:
                low.append(min(av,bv)); opt.append(bv+(cv-bv)*15); high.append(max_seq+8)
            else:
                low.append(min(av,bv,cv)); opt.append(max(av,bv,cv)); high.append(max(av,bv,cv))
        profiles[n]={'min':low,'opt':opt,'max':high,'initialShape':list(a),'dtype':str(feed0[n].dtype)}
        vi.type.CopyFrom(helper.make_tensor_value_info(n,helper.np_dtype_to_tensor_dtype(feed0[n].dtype),[lo if lo==hi else f'state_{len(profiles)}_{axis}' for axis,(lo,hi) in enumerate(zip(low,high))]).type)
    for k,vi in enumerate(decoder.graph.output):
        vi.type.CopyFrom(helper.make_tensor_value_info(vi.name,helper.np_dtype_to_tensor_dtype(v1[vi.name].dtype),[f'out_{k}_{a}' for a in range(v1[vi.name].ndim)]).type)
    onnx.save(decoder,outdir/'decoder.onnx')
    metadata={'model':args.model,'sourceSha256':hashlib.sha256(source.read_bytes()).hexdigest(),'profiles':profiles,'invariants':invariant_names,'conditionOutput':body.output[0].name,'states':[{'index':i,'input':body.input[i].name,'output':body.output[i-1].name} for i in state_indices],'finalTokenOutput':body.output[final_state_index-1].name,'maxSequenceLength':max_seq,'rewrites':proof,'scope':'Batch one; image and KV stay in GPU memory at runtime. CPU ORT is a preparation-only oracle.'}
    feed=dict(feed0)
    for iteration in range(max_seq+8):
        got=step(feed)
        if not bool(got[metadata['conditionOutput']]): break
        feed={**{n:got[out] for n,out in state_outputs.items()},**invariants}
    else: raise RuntimeError('reference loop exceeded model bound')
    actual=got[metadata['finalTokenOutput']]
    expected=np.load(root/'reference'/args.model/'chalk-kinetic-normalized-tokens.npy')
    metadata.update(oracleKineticIterations=iteration+1,oracleKineticTokenShape=list(actual.shape),oracleKineticTokensEqual=bool(np.array_equal(actual,expected)))
    assert metadata['oracleKineticTokensEqual'], 'partition changed tokens'
    (outdir/'partition.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    np.save(outdir/'oracle-kinetic-tokens.npy',actual)
    print(json.dumps(metadata,indent=2),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('model')
    p.add_argument('--work-dir',type=Path,required=True)
    p.add_argument('--model-root',type=Path,required=True)
    p.add_argument('--partition',default='partition-v2')
    partition(p.parse_args())
