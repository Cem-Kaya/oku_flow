"""Bind actual TensorRT tokens to a source image for an offline formula overlay."""
import argparse
import hashlib
import json
from pathlib import Path
from tokenizers import Tokenizer
from model_pins import verify_model


def make_request(prediction,model_root,assets,output):
    prediction=Path(prediction);assets=Path(assets);output=Path(output)
    if output.exists(): raise FileExistsError(output)
    result=json.loads(prediction.read_text(encoding='utf-8'))
    if not result.get('completed'): raise ValueError('incomplete model execution')
    model=result['model'];verify_model(model_root,model)
    config=json.loads((Path(model_root)/model/'config.json').read_text(encoding='utf-8'))
    tokenizer=Tokenizer.from_str(json.dumps(config['PostProcess']['character_dict']['fast_tokenizer_file']))
    inputs=json.loads((assets/'inputs.json').read_text(encoding='utf-8'))
    regions=[];source=None;source_hash=None
    for case in result['cases']:
        match=[v for v in inputs if v['name']==case['case'] and 'cropLTRB' in v]
        if not match: continue
        item=match[0]
        if source is None: source=item['source'];source_hash=item['sourceSha256']
        if item['source']!=source or item['sourceSha256']!=source_hash: raise ValueError('mixed source images')
        tokens=case['tokens']
        if len(tokens)!=1 or len(tokens[0])>4096: raise ValueError('invalid token shape')
        ids=tokens[0]
        if 2 not in ids: raise ValueError('missing model EOS')
        ids=ids[:ids.index(2)+1]
        latex=tokenizer.decode(ids,skip_special_tokens=True)
        if not latex or len(latex)>8192: raise ValueError('invalid formula length')
        regions.append({'case':case['case'],'sourceLTRB':item['cropLTRB'],'latex':latex,'tokens':ids,'tokensEqualPaddle':case['allTokensEqualPaddle']})
    if not regions: raise ValueError('no source-bound formula regions')
    if hashlib.sha256(Path(source).read_bytes()).hexdigest()!=source_hash: raise ValueError('source image changed')
    request={'version':1,'model':model,'precision':result['precision'],'source':source,'sourceSha256':source_hash,'prediction':str(prediction),'predictionSha256':hashlib.sha256(prediction.read_bytes()).hexdigest(),'placement':'whole source formula regions; no per-symbol source geometry','regions':regions}
    output.write_text(json.dumps(request,indent=2),encoding='utf-8')
    return request

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('prediction',type=Path);p.add_argument('--model-root',type=Path,required=True);p.add_argument('--assets',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(json.dumps(make_request(a.prediction,a.model_root,a.assets,a.output),indent=2))
