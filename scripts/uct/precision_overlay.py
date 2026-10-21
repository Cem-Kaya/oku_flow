"""Unchanged source image plus SVG overlays from exact paired OCR receipts."""
import argparse
import html
import json
from pathlib import Path

from precision_report import edits, envelope, iou, load, words


def annotations(reference,candidate):
    a=[w for w in words(reference) if w["disposition"]==0]
    all_b=words(candidate)
    b=[w for w in all_b if w["disposition"]==0]
    _,script=edits([w["text"] for w in a],[w["text"] for w in b])
    output=[]
    for edit in script:
        if edit["kind"]=="equal" or edit["a"] is None:continue
        word=a[edit["a"]]
        overlaps=sorted([(iou(envelope(word),envelope(other)),index,other) for index,other in enumerate(all_b)],reverse=True)
        best=overlaps[0] if overlaps else (0,None,None)
        if any(overlap>=.5 and other["text"]==word["text"] and other["disposition"]==0 for overlap,_,other in overlaps):
            # Reading-order alignment can mark an existing occurrence deleted.
            # Do not mislabel that case as an undetected word.
            continue
        if edit["kind"]=="changed":
            kind="changed";after=edit["after"]
        elif best[0]>=.3 and best[2]["disposition"]==0:
            kind="changed";after=best[2]["text"]
        elif best[0]>=.3 and best[2]["text"]==word["text"]:
            kind="suppressed";after=best[2]["text"]+f' (disposition {best[2]["disposition"]})'
        else:
            kind="omitted";after="no corresponding candidate word"
        output.append({"id":len(output)+1,"kind":kind,"before":word["text"],"after":after,
                       "bounds":word["bounds"],"baselineWord":word,"alignedEdit":edit,
                       "bestSpatialOutput":best[2],"bestSpatialIou":best[0]})
    return output


def page(source, width, height, items, baseline, candidate, recognized=None):
    escape=lambda value:html.escape(str(value),quote=True)
    script_items=json.dumps(items,ensure_ascii=True).replace('<','\\u003c').replace('>','\\u003e').replace('&','\\u0026')
    recognized=recognized or []
    rects=[]
    for word in recognized:
        x,y,w,h=word['bounds']
        rects.append(f'<g data-recognized="1"><title>BF16 output: {escape(word["text"])}</title><rect x="{x}" y="{y}" width="{w}" height="{h}" fill="#19c46a" fill-opacity=".12" stroke="#13b961" stroke-width="2.5"/></g>')
    for item in items:
        x,y,w,h=item["bounds"];color="#ff304f" if item["kind"]!="changed" else "#ffbd19"
        title=escape(f'{item["id"]}: {item["before"]} → {item["after"]}')
        rects.append(f'<g data-id="{item["id"]}"><title>{title}</title><rect x="{x-2}" y="{y-2}" width="{w+4}" height="{h+4}" fill="{color}" fill-opacity=".13" stroke="white" stroke-width="7"/><rect x="{x-2}" y="{y-2}" width="{w+4}" height="{h+4}" fill="none" stroke="{color}" stroke-width="4"/></g>')
    picture=f'<div class="picture"><img src="{escape(source)}" alt="Unchanged book evaluation frame"><svg viewBox="0 0 {width} {height}" aria-label="OCR differences">'+''.join(rects)+'</svg></div>'
    buttons=''.join(f'<button data-id="{x["id"]}" class="{x["kind"]}">{x["id"]}. {escape(x["before"])} → {escape(x["after"])}</button>' for x in items)
    return '''<!doctype html><meta charset="utf-8"><style>
body{font:16px var(--font-sans,system-ui);color:var(--foreground,#20232a);margin:0;line-height:1.5}p{margin:0 0 12px}.legend{display:flex;gap:18px;flex-wrap:wrap}.red{color:#f64b63}.amber{color:#c98b00}.picture{position:relative}.picture img{display:block;width:100%;height:auto}.picture svg{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}.zoom{position:relative;overflow:hidden;aspect-ratio:600/220;border:2px solid var(--border,#777);margin:12px 0}.zoomInner{position:absolute}.buttons{display:flex;gap:6px;flex-wrap:wrap;margin:12px 0}button{font:inherit;border-radius:6px;padding:7px 10px;cursor:pointer;color:var(--foreground,#222);background:var(--secondary,#eee);border:2px solid #ffbd19}button.omitted,button.suppressed{border-color:#ff304f}button[aria-pressed=true]{outline:3px solid var(--primary,#3979ff)}details{overflow-wrap:anywhere}#label{font-weight:650}svg g{transition:opacity .1s}
</style><p><strong>Book: FP32 baseline versus BF16 recognition</strong></p>
<p class="legend"><span style="color:#13b961">■ Green: every BF16 candidate word</span><span class="red">■ Red: baseline word omitted or suppressed</span><span class="amber">■ Amber: changed reading</span></p>
<p>Source pixels are unchanged. These are differences from the baseline, not verified ground-truth errors. Select a word to inspect its original image region.</p>'''+picture+'<div class="buttons">'+buttons+'</div><p id="label"></p><div class="zoom"><div class="zoomInner">'+picture+'''</div></div><details><summary>Exact receipts and interpretation</summary><p>Reading-order edits are cross-checked against spatial occurrences so a reordered existing word is not called omitted. Red also includes a baseline word that the variant suppresses; details retain the actual disposition. Added words and every raw output remain in the JSON report.</p><p>Baseline: <code>'''+escape(baseline)+'</code><br>BF16: <code>'+escape(candidate)+'''</code></p></details><script>
const items='''+script_items+f';const W={width},H={height};'+'''
function select(id){const item=items.find(x=>x.id===id);if(!item)return;const [x,y,w,h]=item.bounds;const cw=Math.min(W,600),ch=Math.min(H,220),cx=Math.max(0,Math.min(W-cw,x+w/2-cw/2)),cy=Math.max(0,Math.min(H-ch,y+h/2-ch/2));const inner=document.querySelector('.zoomInner');inner.style.width=(W/cw*100)+'%';inner.style.left=(-cx/cw*100)+'%';inner.style.top=(-cy/ch*100)+'%';document.querySelector('.zoom').style.aspectRatio=cw+'/'+ch;document.querySelector('#label').textContent=item.before+' → '+item.after;document.querySelectorAll('button').forEach(b=>b.setAttribute('aria-pressed',Number(b.dataset.id)===id));document.querySelectorAll('svg g').forEach(g=>g.style.opacity=g.dataset.recognized==='1'||Number(g.dataset.id)===id?'1':'.35');}
document.querySelectorAll('button').forEach(b=>b.onclick=()=>select(Number(b.dataset.id)));if(items.length)select(items[0].id);window.addEventListener('load',()=>console.log(JSON.stringify({annotations:items.length,images:[...document.images].map(i=>({loaded:i.complete,width:i.naturalWidth,height:i.naturalHeight}))})));
</script>'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report",type=Path,required=True)
    parser.add_argument("--source-name",default="book_001.png")
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    report=load(args.report)
    record=next(x for x in report["comparisons"] if Path(x["source"]["path"]).name==args.source_name and
                x["recognizer"]=="bf16-linear" and x["detector"]=="fp32" and not x["frozen"] and x["passNumber"]==1)
    baseline=load(Path(record["baselineReceipt"]));candidate=load(Path(record["candidateReceipt"]))
    if baseline["result"]["sourcePixelDigest"]!=candidate["result"]["sourcePixelDigest"]:
        raise ValueError("Source pixel identity mismatch")
    items=annotations(baseline,candidate)
    recognized=[word for word in words(candidate) if word['disposition']==0]
    args.output.mkdir(parents=True,exist_ok=False)
    receipt={"source":record["source"],"baselineReceipt":record["baselineReceipt"],"candidateReceipt":record["candidateReceipt"],
             "annotations":items,"recognized":recognized,"notGroundTruth":True,"pixelsModified":False}
    (args.output/"annotations.json").write_text(json.dumps(receipt,indent=2),encoding="utf-8")
    (args.output/"overlay.html").write_text(page(record["source"]["path"],record["source"]["width"],record["source"]["height"],
        items,record["baselineReceipt"],record["candidateReceipt"],recognized),encoding="utf-8")
    print(args.output/"overlay.html")


if __name__=="__main__":main()
