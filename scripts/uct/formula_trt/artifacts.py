"""Bind a prepared native formula runtime to exact compiler receipts and files."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''): h.update(block)
    return h.hexdigest()


def write_seal(directory):
    directory=Path(directory)
    target=directory/'runtime-seal.json'
    if target.exists(): raise FileExistsError(target)
    meta=json.loads((directory/'partition.json').read_text(encoding='utf-8'))
    if not (meta.get('oracleKineticTokensEqual') or meta.get('sourcePartitionOracleKineticTokensEqual')): raise ValueError('reference partition has not matched Paddle')
    required=['partition.json','initial-states.npz']
    if 'precisionPolicy' in meta:
        variant=json.loads((directory/'precision-variant.json').read_text(encoding='utf-8'))
        for part,entry in variant['graphs'].items():
            if digest(directory/(part+'.onnx'))!=entry['resultSha256']: raise ValueError('precision graph changed')
        required.append('precision-variant.json')
    for part in ('encoder','decoder'):
        receipt=json.loads((directory/(part+'.plan.json')).read_text(encoding='utf-8'))
        if receipt['model']!=meta['model'] or receipt['part']!=part: raise ValueError('wrong engine identity')
        for key,suffix in [('sourceSha256','.onnx'),('planSha256','.plan')]:
            if receipt[key]!=digest(directory/(part+suffix)): raise ValueError('engine/source differs from compiler receipt')
        if part=='decoder' and receipt['profiles']!=meta['profiles']: raise ValueError('decoder profiles changed since compilation')
        required += [part+'.onnx',part+'.plan',part+'.plan.json']
    seal={'version':1,'model':meta['model'],'files':{name:digest(directory/name) for name in required},'scope':'Offline content binding; not a signature or external-model quality acceptance.'}
    with target.open('x',encoding='utf-8') as stream: json.dump(seal,stream,indent=2)
    return seal


def verify_seal(directory):
    directory=Path(directory)
    seal=json.loads((directory/'runtime-seal.json').read_text(encoding='utf-8'))
    expected={'partition.json','initial-states.npz'}|{p+s for p in ('encoder','decoder') for s in ('.onnx','.plan','.plan.json')}
    if (directory/'precision-variant.json').exists(): expected.add('precision-variant.json')
    if seal.get('version')!=1 or set(seal['files'])!=expected: raise ValueError('incomplete runtime seal')
    for name,expected_hash in seal['files'].items():
        if digest(directory/name)!=expected_hash: raise ValueError('runtime artifact changed: '+name)
    return seal


if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('directory',type=Path); p.add_argument('--verify',action='store_true'); a=p.parse_args()
    print(json.dumps(verify_seal(a.directory) if a.verify else write_seal(a.directory),indent=2))
