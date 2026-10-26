"""Explicit formula linear precision variants; stable math and interfaces stay FP32."""
import argparse
from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import onnx
from onnx import helper, TensorProto

SPEC=importlib.util.spec_from_file_location('uct_precision',Path(__file__).parents[1]/'precision_models.py')
PRECISION=importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(PRECISION)


def convert_bf16(model):
    result=onnx.version_converter.convert_version(deepcopy(model),22)
    inferred=onnx.shape_inference.infer_shapes(result)
    types={v.name:v.type.tensor_type.elem_type for v in list(inferred.graph.input)+list(inferred.graph.value_info)+list(inferred.graph.output)}
    constants={t.name:t for t in result.graph.initializer}
    constants.update({n.output[0]:a.t for n in result.graph.node if n.op_type=='Constant' for a in n.attribute if a.name=='value'})
    types.update({n:t.data_type for n,t in constants.items()})
    aliases={n.output[0]:n.input[0] for n in result.graph.node if n.op_type=='Identity'}
    def constant(name):
        seen=set()
        while name in aliases and name not in seen:
            seen.add(name);name=aliases[name]
        return constants.get(name)
    prefix='__okuflow_formula_bf16_'
    if any(o.startswith(prefix) for n in result.graph.node for o in n.output): raise ValueError('already converted')
    nodes=[];weights={};changes=[]
    for index,node in enumerate(result.graph.node):
        if node.domain or node.op_type not in ('Conv','MatMul','Gemm'):
            nodes.append(node);continue
        converted=deepcopy(node)
        for slot,name in enumerate(node.input):
            if not name: continue
            tensor=constant(name)
            if tensor is not None:
                if tensor.data_type!=TensorProto.FLOAT: raise ValueError('non-FP32 linear weight')
                key=hashlib.sha256(tensor.SerializeToString()).hexdigest()
                if key not in weights:
                    low_name=prefix+'weight_'+str(len(weights))
                    low=PRECISION.reduced_tensor(tensor,low_name,'bf16-linear')
                    nodes.append(helper.make_node('Constant',[],[low_name],value=low))
                    weights[key]=low_name
                converted.input[slot]=weights[key]
            else:
                if types.get(name)!=TensorProto.FLOAT: raise ValueError('unproven float linear operand '+name)
                low_name=f'{prefix}{index}_input_{slot}'
                nodes.append(helper.make_node('Cast',[name],[low_name],to=TensorProto.BFLOAT16))
                converted.input[slot]=low_name
        low_out=f'{prefix}{index}_output';converted.output[0]=low_out
        nodes += [converted,helper.make_node('Cast',[low_out],list(node.output),to=TensorProto.FLOAT)]
        changes.append({'name':node.name,'op':node.op_type})
    # Drop dead FP32 weights rather than retaining duplicate full-size tensors.
    live={v.name for v in result.graph.output}; kept=[]
    for node in reversed(nodes):
        if set(node.output)&live:
            kept.append(node);live.update(node.input)
    del result.graph.node[:];result.graph.node.extend(reversed(kept))
    initializers=[v for v in result.graph.initializer if v.name in live]
    del result.graph.initializer[:];result.graph.initializer.extend(initializers)
    del result.graph.value_info[:]
    if not changes: raise ValueError('no BF16 arithmetic')
    onnx.checker.check_model(result,full_check=True)
    return result,{'operators':changes,'reducedWeights':len(weights),'interfaces':'unchanged FP32/integer/bool','stableMath':'normalization, Softmax, residual/control remain FP32'}


def convert_bf16_wide(model):
    """Continuous neural BF16 paths; explicit FP32 stable/control math boundaries."""
    result=onnx.version_converter.convert_version(deepcopy(model),22)
    inferred=onnx.shape_inference.infer_shapes(result)
    info={v.name:v for v in list(inferred.graph.input)+list(inferred.graph.value_info)+list(inferred.graph.output)}
    original_constants={t.name:t for t in result.graph.initializer}
    original_constants.update({n.output[0]:a.t for n in result.graph.node if n.op_type=='Constant' for a in n.attribute if a.name=='value'})
    types={n:v.type.tensor_type.elem_type for n,v in info.items()}
    types.update({n:t.data_type for n,t in original_constants.items()})
    safe={'Conv','MatMul','Gemm','Add','Sub','Mul','Relu','Gelu','Erf','Tanh','Identity','Reshape','Transpose','Flatten','Concat','Split','Slice','Gather','Squeeze','Unsqueeze','Expand','Pad','MaxPool','AveragePool','GlobalAveragePool','Resize'}
    no_float_cast={'Shape','Size'}
    prefix='__okuflow_formula_wide_bf16_'
    if any(o.startswith(prefix) for n in result.graph.node for o in n.output): raise ValueError('already converted')
    current={v.name:(v.name,v.type.tensor_type.elem_type) for v in result.graph.input}
    current.update({n:(n,t.data_type) for n,t in original_constants.items()})
    nodes=[];casts={};changes=[]
    def operand(name,target):
        actual,kind=current.get(name,(name,types.get(name)))
        if kind!=TensorProto.FLOAT and kind!=TensorProto.BFLOAT16: return actual
        if kind==target: return actual
        key=(actual,target)
        if key not in casts:
            dest=prefix+'cast_'+str(len(casts))
            if name in original_constants and target==TensorProto.BFLOAT16:
                low=PRECISION.reduced_tensor(original_constants[name],dest,'bf16-linear')
                nodes.append(helper.make_node('Constant',[],[dest],value=low))
            else: nodes.append(helper.make_node('Cast',[actual],[dest],to=target))
            casts[key]=dest
        return casts[key]
    for index,node in enumerate(result.graph.node):
        if node.op_type=='Constant': nodes.append(deepcopy(node));continue
        floating=[o for o in node.output if types.get(o)==TensorProto.FLOAT]
        ranks=[len(info[o].type.tensor_type.shape.dim) for o in floating if o in info]
        supports=any('tensor(bfloat16)' in c.allowed_type_strs for c in onnx.defs.get_schema(node.op_type,22,node.domain).type_constraints)
        low=bool(floating and ranks and max(ranks)>=2 and node.op_type in safe and supports and not node.domain)
        if node.op_type in ('Conv','MatMul','Gemm') and not low: raise ValueError('linear type proof failed '+node.name)
        copied=deepcopy(node)
        for i,name in enumerate(node.input):
            if not name: continue
            copied.input[i]=current.get(name,(name,None))[0] if node.op_type in no_float_cast else operand(name,TensorProto.BFLOAT16 if low else TensorProto.FLOAT)
        for i,name in enumerate(node.output):
            if types.get(name)==TensorProto.FLOAT:
                target=TensorProto.BFLOAT16 if low else TensorProto.FLOAT
                actual=prefix+'value_'+str(index)+'_'+str(i) if low else name
                copied.output[i]=actual;current[name]=(actual,target)
            else: current[name]=(name,types.get(name))
        nodes.append(copied)
        if low: changes.append({'name':node.name,'op':node.op_type})
    for output in result.graph.output:
        actual,kind=current[output.name]
        if actual!=output.name:
            nodes.append(helper.make_node('Cast',[actual],[output.name],to=output.type.tensor_type.elem_type))
    live={v.name for v in result.graph.output};kept=[]
    for n in reversed(nodes):
        if set(n.output)&live: kept.append(n);live.update(n.input)
    del result.graph.node[:];result.graph.node.extend(reversed(kept))
    kept_initial=[t for t in result.graph.initializer if t.name in live]
    del result.graph.initializer[:];result.graph.initializer.extend(kept_initial)
    del result.graph.value_info[:]
    onnx.checker.check_model(result,full_check=True)
    return result,{'operators':changes,'casts':len(casts),'interfaces':'unchanged','stableMath':'FP32 normalization/Softmax/control; BF16 neural residuals and activations'}


def prepare(source,destination,precision):
    source=Path(source);destination=Path(destination)
    if destination.exists(): raise FileExistsError(destination)
    destination.mkdir(parents=True)
    meta=json.loads((source/'partition.json').read_text(encoding='utf-8'))
    meta['sourcePartitionOracleKineticTokensEqual']=meta.pop('oracleKineticTokensEqual')
    meta['sourcePartitionSha256']=hashlib.sha256((source/'partition.json').read_bytes()).hexdigest()
    meta['precisionPolicy']={'linear':'bf16' if precision.startswith('bf16') else 'fp32','tf32':precision=='tf32','strategy':precision}
    receipt={'precision':precision,'source':str(source),'graphs':{}}
    for part in ('encoder','decoder'):
        original=source/(part+'.onnx');out=destination/(part+'.onnx')
        if precision.startswith('bf16'):
            graph,detail=(convert_bf16_wide if precision=='bf16-wide' else convert_bf16)(onnx.load(original));onnx.save(graph,out)
        else: shutil.copyfile(original,out);detail={'operators':'unchanged; TF32 is a builder permission'}
        receipt['graphs'][part]={'sourceSha256':hashlib.sha256(original.read_bytes()).hexdigest(),'resultSha256':hashlib.sha256(out.read_bytes()).hexdigest(),**detail}
    shutil.copyfile(source/'initial-states.npz',destination/'initial-states.npz')
    (destination/'partition.json').write_text(json.dumps(meta,indent=2),encoding='utf-8')
    (destination/'precision-variant.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps({'model':meta['model'],'precision':precision,'convertedOperators':{p:len(v['operators']) if isinstance(v.get('operators'),list) else 0 for p,v in receipt['graphs'].items()}},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('destination',type=Path);p.add_argument('--precision',choices=['bf16','bf16-wide','tf32'],required=True);a=p.parse_args();prepare(a.source,a.destination,a.precision)
