"""Offline receipts -> inspectable HTML/JSON; baseline agreement, not accuracy."""
import argparse
from collections import Counter, defaultdict
import hashlib
import html
import json
from pathlib import Path
import statistics


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def words(record):
    return [word for line in record["result"]["output"]["lines"] for word in line]


def edits(a, b):
    """Exact Levenshtein edit script in reading order (case/punctuation kept)."""
    if len(a)*len(b)>4_000_000:
        raise ValueError("Word comparison bound exceeded")
    rows=[list(range(len(b)+1))]
    for i,x in enumerate(a,1):
        row=[i]
        for j,y in enumerate(b,1):
            row.append(min(rows[-1][j]+1,row[-1]+1,rows[-1][j-1]+(x!=y)))
        rows.append(row)
    result=[];i=len(a);j=len(b)
    while i or j:
        if i and j and rows[i][j]==rows[i-1][j-1]+(a[i-1]!=b[j-1]):
            result.append({"kind":"equal" if a[i-1]==b[j-1] else "changed","a":i-1,"b":j-1,
                           "before":a[i-1],"after":b[j-1]});i-=1;j-=1
        elif i and rows[i][j]==rows[i-1][j]+1:
            result.append({"kind":"dropped","a":i-1,"b":None,"before":a[i-1],"after":""});i-=1
        else:
            result.append({"kind":"added","a":None,"b":j-1,"before":"","after":b[j-1]});j-=1
    return rows[-1][-1],list(reversed(result))


def envelope(word):
    r=word.get("protectedBounds")
    return r if r and r[2]>0 and r[3]>0 else word["bounds"]


def iou(a,b):
    shared=max(0,min(a[0]+a[2],b[0]+b[2])-max(a[0],b[0]))*max(0,min(a[1]+a[3],b[1]+b[3])-max(a[1],b[1]))
    area=a[2]*a[3]+b[2]*b[3]-shared
    return shared/area if area else 0


def geometry(a,b):
    pairs=sorted([(-iou(envelope(x),envelope(y)),i,j) for i,x in enumerate(a) for j,y in enumerate(b)
                  if x["text"]==y["text"] and iou(envelope(x),envelope(y))>=.5])
    used_a=set();used_b=set();result=[]
    for negative,i,j in pairs:
        if i in used_a or j in used_b:continue
        used_a.add(i);used_b.add(j)
        x,y=a[i],b[j];r=x["bounds"];s=y["bounds"]
        drift=max(abs(r[0]-s[0]),abs(r[1]-s[1]),abs(r[0]+r[2]-s[0]-s[2]),abs(r[1]+r[3]-s[1]-s[3]))
        result.append({"a":i,"b":j,"text":x["text"],"edgePixels":drift,
                       "confidenceDelta":y["confidence"]-x["confidence"],
                       "dispositionBefore":x["disposition"],"dispositionAfter":y["disposition"]})
    return {"pairs":result,"unmatchedA":sorted(set(range(len(a)))-used_a),"unmatchedB":sorted(set(range(len(b)))-used_b),
            "maxCommonWordEdgePixels":max([p["edgePixels"] for p in result],default=None),
            "orderDescents":sum(x["b"]>y["b"] for x,y in zip(sorted(result,key=lambda p:p["a"]),sorted(result,key=lambda p:p["a"])[1:])),
            "rule":"Exact text, greedy one-to-one protected-envelope IoU>=0.5; unmatched occurrences remain visible."}


def compare(reference, candidate, frozen):
    if reference["result"]["sourcePixelDigest"]!=candidate["result"]["sourcePixelDigest"]:
        raise ValueError("Different GPU source-pixel fingerprints")
    a,b=words(reference),words(candidate)
    all_distance,all_edits=edits([w["text"] for w in a],[w["text"] for w in b])
    ac=[w["text"] for w in a if w["disposition"]==0];bc=[w["text"] for w in b if w["disposition"]==0]
    distance,candidate_edits=edits(ac,bc)
    crop_equal=reference["profile"]["orderedCropDescriptorSha256"]==candidate["profile"]["orderedCropDescriptorSha256"]
    if frozen and (not crop_equal or candidate["profile"]["detectorPasses"]!=0 or reference["profile"]["detectorPasses"]!=0):
        raise ValueError("Frozen crop equality or zero timed detection failed")
    for record in [reference,candidate]:
        if record["meters"]["imageEgressCalls"]!=0 or not record["result"]["deviceInput"] or not record["result"]["output"]["ok"]:
            raise ValueError("Invalid resident result/egress")
        if record["profile"]["batchCapacity"]!=8 or record["profile"]["recognizerWidthPolicy"]!="fixed1024":
            raise ValueError("Batch/width policy changed")
    return {"allWordEdits":all_distance,"allWordDenominator":len(a),"allWordScript":all_edits,
            "candidateEdits":distance,"candidateDenominator":len(ac),"candidateScript":candidate_edits,
            "candidateDifferencePercent":100*distance/len(ac) if ac else None,
            "withinOwnerFivePercent":bool(ac) and distance*20<=len(ac),
            "dispositionsBefore":dict(Counter(w["disposition"] for w in a)),
            "dispositionsAfter":dict(Counter(w["disposition"] for w in b)),
            "orderedCropsEqual":crop_equal,"geometry":geometry(a,b),
            "notAccuracy":True}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs",nargs="+",type=Path)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    reports=[];statistics_rows=[];receipts=[];contended_runs=[]
    for run in args.runs:
        invocation=load(run/"invocation.json");process=load(run/"process.json");raw=run/"raw"
        if invocation.get("contended",False):contended_runs.append(str(run))
        summary=load(raw/"summary.json");manifest=load(raw/"manifest.json");sources=load(raw/"sources.json")
        if process["exitCode"] or not summary["complete"]:
            raise ValueError("Incomplete/failed timing invocation; preserve and report it separately")
        if load(raw/"baseline-before.json")!=load(raw/"baseline-after.json"):
            raise ValueError("Baseline cache changed")
        if any(not b["allObservedStateClosuresCompleted"] or b["cleanupFailed"] for b in summary["blocks"]):
            raise ValueError("Missing exact State retirement")
        configs=summary["configurations"]
        if configs[0]["detector"]!="fp32" or configs[0]["recognizer"]!="fp32":
            raise ValueError("First configuration must be the baseline")
        records={};metrics=defaultdict(lambda:defaultdict(list))
        for index,config in enumerate(configs):
            folder=raw/f"configuration-{index}"
            if not load(folder/"cache.json")["unchanged"]:raise ValueError("Variant cache changed")
            jobs=[(path,load(path)) for path in sorted((folder/"jobs").glob("*.json"))]
            timed=[(path,job) for path,job in jobs if job["kind"]=="timed"]
            if len(timed)!=len(sources["sources"])*invocation["passes"]:raise ValueError("Missing/duplicate timed work")
            for path,job in timed:
                key=(index,job["source"],job["pass"])
                if key in records:raise ValueError("Duplicate timed key")
                records[key]=(path,job)
                for metric,value in job["metrics"].items():metrics[(index,job["source"])][metric].append(value)
                if index:
                    basepath,baseline=records[(0,job["source"],job["pass"])]
                    report=compare(baseline,job,invocation["frozen"])
                    report.update(run=str(run),configuration=index,detector=config["detector"],recognizer=config["recognizer"],
                                  source=sources["sources"][job["source"]],passNumber=job["pass"],
                                  baselineReceipt=str(basepath),candidateReceipt=str(path),frozen=invocation["frozen"])
                    reports.append(report)
            for source in range(len(sources["sources"])):
                row={"run":str(run),"frozen":invocation["frozen"],"source":sources["sources"][source]["path"],
                     "configuration":index,"detector":config["detector"],"recognizer":config["recognizer"],
                     "metrics":{name:{"n":len(values),"median":statistics.median(values),"min":min(values),"max":max(values)}
                                for name,values in metrics[(index,source)].items()}}
                statistics_rows.append(row)
        receipts.append({"run":str(run),"process":process,"manifest":manifest,"summary":summary})
    output={"notAccuracy":True,"contendedRuns":contended_runs,
            "comparisons":reports,"statistics":statistics_rows,"receipts":receipts}
    (args.output/"report.json").write_text(json.dumps(output,indent=2),encoding="utf-8")
    escape=lambda v:html.escape(str(v),quote=True)
    content=['<!doctype html><meta charset="utf-8"><title>UCT precision experiment</title><style>body{font:17px system-ui;max-width:1200px;margin:32px auto;padding:16px;line-height:1.5}table{border-collapse:collapse;width:100%}td,th{padding:8px;border:1px solid #bbb;text-align:left}del,.dropped{background:#ffdad8;color:#650000}ins,.added{background:#d3f6d9;color:#034016;text-decoration:none}.changed{background:#ffe7a4}summary{cursor:pointer;margin-top:18px}code{overflow-wrap:anywhere}</style><h1>UCT precision experiment</h1><p>Same-source agreement, not ground-truth accuracy. Fixed1024 / batch8. Warmup and startup excluded. Three finite observations describe these frames, not tail latency, native FPS, or another GPU. INT4 is weight-only, with higher-precision arithmetic.</p>']
    if contended_runs:
        content.append('<p><strong>CONTENDED timing:</strong> '+escape(len(contended_runs))+
                       ' invocation(s) explicitly permitted concurrent GPU clients. Per-configuration GPU utilization, memory, power and process snapshots are retained in raw receipts. These are not unloaded or isolated measurements.</p>')
    content.append('<h2>Worker latency and stage medians</h2><table><tr><th>Source</th><th>Detector / recognizer</th><th>Path</th><th>Worker ms</th><th>Detector ms</th><th>Recognizer ms</th><th>Free VRAM after job</th></tr>')
    for row in statistics_rows:
        m=row["metrics"]
        value=lambda key:round(m[key]["median"],3) if key in m else 'unavailable'
        content.append('<tr>'+''.join('<td>'+escape(v)+'</td>' for v in [Path(row["source"]).name,row["detector"]+' / '+row["recognizer"],"frozen" if row["frozen"] else "full",value("schedulerWorkerMs"),value("stage.detectorMs"),value("stage.recognizerMs"),value("cudaFreeBytesAfterJob")])+'</tr>')
    content.append('</table><p>Device-wide free VRAM is approximate, not process-exclusive or a peak. Exact engine context and plan sizes are in engine receipts; peak process working set is in process receipts.</p><h2>Every paired word comparison</h2>')
    for report in reports:
        title=f'{Path(report["source"]["path"]).name} · {report["detector"]}/{report["recognizer"]} · pass {report["passNumber"]} · {"frozen" if report["frozen"] else "full"}'
        content.append('<details><summary>'+escape(title)+f' · {report["candidateEdits"]}/{report["candidateDenominator"]} candidate edits ({report["candidateDifferencePercent"]:.3f}%)</summary>')
        for edit in report["allWordScript"]:
            if edit["kind"]=="equal":content.append(escape(edit["before"])+" ")
            else:content.append('<span class="'+edit["kind"]+'"><del>'+escape(edit["before"])+'</del> <ins>'+escape(edit["after"])+'</ins></span> ')
        content.append('<p>Disposition counts: '+escape(report["dispositionsBefore"])+' → '+escape(report["dispositionsAfter"])+'. Common exact-text occurrence maximum edge drift: '+escape(report["geometry"]["maxCommonWordEdgePixels"])+'.</p><p>Raw: <code>'+escape(report["baselineReceipt"])+'</code><br><code>'+escape(report["candidateReceipt"])+'</code></p></details>')
    (args.output/"report.html").write_text(''.join(content),encoding="utf-8")
    print(args.output/"report.html")


if __name__=="__main__":main()
