"""Reject unpinned development models/exports before preparing a formula engine."""
import argparse
import json
from pathlib import Path
from artifacts import digest


def verify_model(model_root, model_name):
    catalog=json.loads(Path(__file__).with_name('models.json').read_text(encoding='utf-8'))
    rows=[r for r in catalog if r['name']==model_name]
    if len(rows)!=1: raise ValueError('unreviewed formula model')
    row=rows[0]
    for f in row['files']:
        p=Path(model_root)/model_name/f['file']
        if p.stat().st_size!=f['bytes'] or digest(p)!=f['sha256']: raise ValueError('model file differs from pin: '+str(p))
    return row


def verify_export(work_dir,model_row):
    r=Path(work_dir)/'onnx'; name=model_row['name']
    raw=r/(name+'.onnx'); masked=r/(name+'.trt-mask.onnx')
    receipt=json.loads(masked.with_suffix('.rewrite.json').read_text(encoding='utf-8'))
    if digest(raw)!=model_row['nativeExportSha256']: raise ValueError('ONNX export differs from pinned native export')
    if receipt['sourceSha256']!=digest(raw) or receipt['resultSha256']!=digest(masked): raise ValueError('mask rewrite receipt mismatch')
    return receipt


if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('model'); p.add_argument('--model-root',type=Path,required=True); a=p.parse_args()
    print(json.dumps(verify_model(a.model_root,a.model),indent=2))
