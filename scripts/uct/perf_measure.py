"""Same-binary (or build-versus-build) OCR worker timing and output comparison.

Runs the opt-in `optionalCorpusTiming` slot of `uct_paddle_tests` once per arm
and cohort. Cohort 0 walks the scenes forward with the arms in the given order;
cohort 1 walks them in reverse with the arms reversed (A B / B A). Every
process makes one first read and `--passes` warm reads per scene, so two
cohorts of five passes give ten warm reads per arm and scene.

Reported per scene and arm: the median of the pooled warm scheduler-worker
times, each cohort's own median (look at these before trusting a pooled one),
the first read in each process, and whether the complete ordered output
(text, order, dispositions, geometry, confidence, colours) is byte-identical
between arms. Scenes with a `.truth.txt` are also scored against the words
that were drawn. Nothing here measures camera, decoder, presenter, paint or
accessibility latency.

Engines are never prepared: point --localappdata at a cache that already
holds the plans. Evidence stays outside the repository.

    python scripts/uct/perf_measure.py --exe <uct_paddle_tests.exe> --corpus <dir> --out <new dir> \
        --arm base= --arm candidate=OKUFLOW_UCT_SWITCH=1 --qt-bin <dir> --tensorrt-root <dir> \
        --cuda-bin <dir> --runtime-root <dir> --localappdata <dir> [--lease <gpu_serial.py>]
"""
import argparse
import collections
import hashlib
import json
import math
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def parse_arm(text):
    name, _, rest = text.partition("=")
    switches = {}
    for item in filter(None, rest.split(";")):
        key, _, value = item.partition("=")
        if not key.startswith("OKUFLOW_UCT_"):
            sys.exit(f"Arm switch {key!r} must be an OKUFLOW_UCT_ variable.")
        switches[key] = value
    return name, switches


def gpu_state():
    query = "utilization.gpu,clocks.current.graphics,power.draw,memory.used,display_active,driver_version,name"
    try:
        line = subprocess.run(["nvidia-smi", f"--query-gpu={query}", "--format=csv,noheader"],
                              capture_output=True, text=True, timeout=20).stdout.strip().splitlines()[0]
        return dict(zip(query.split(","), (part.strip() for part in line.split(","))))
    except Exception as error:  # diagnostics only
        return {"error": str(error)}


def environment(arguments, switches, extra):
    env = {key: value for key, value in os.environ.items() if not key.startswith("OKUFLOW_UCT_")}
    env.update({
        "OKUFLOW_UCT_ROOT": str(arguments.runtime_root),
        "OKUFLOW_UCT_TENSORRT_ROOT": str(arguments.tensorrt_root),
        "OKUFLOW_UCT_CUDA_BIN": str(arguments.cuda_bin),
        "LOCALAPPDATA": str(arguments.localappdata),
        "QT_QPA_PLATFORM": "offscreen",
        "QT_FORCE_STDERR_LOGGING": "1",
        "QTEST_FUNCTION_TIMEOUT": "3600000",
        "PATH": os.pathsep.join([str(arguments.qt_bin), str(arguments.tensorrt_root / "bin"), str(arguments.cuda_bin),
                                 os.environ.get("PATH", "")]),
    })
    env.update(switches)
    env.update(extra)
    return env


def run_process(arguments, exe, switches, scenes, evidence, log):
    extra = {
        "OKUFLOW_UCT_CORPUS_TIMING": "1",
        "OKUFLOW_UCT_CORPUS_IMAGES": ";".join(str(scene) for scene in scenes),
        "OKUFLOW_UCT_CORPUS_PASSES": str(arguments.passes),
        "OKUFLOW_UCT_CORPUS_EVIDENCE": str(evidence),
        "OKUFLOW_UCT_CORPUS_PROFILE": "1" if arguments.profile else "0",
        "OKUFLOW_UCT_CORPUS_STAMP": arguments.stamp,
        "OKUFLOW_UCT_CORPUS_GAP_MS": str(arguments.gap_ms),
    }
    command = [str(exe), "optionalCorpusTiming"]
    if arguments.lease:
        command = [sys.executable, str(arguments.lease)] + command
    env = environment(arguments, switches, extra)
    waits = 0
    while True:
        before = gpu_state()
        with open(log, "ab") as stream:
            code = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=3600).returncode
        if code != 75:
            break
        waits += 1  # the shared GPU lease was busy
        time.sleep(5)
    if code != 0 or not evidence.exists():
        sys.exit(f"Timing process failed with exit {code}; kept {log}")
    report = json.loads(evidence.read_text(encoding="utf-8"))
    report["gpuBefore"] = before
    report["leaseWaits"] = waits
    return report


def words(output):
    return [word["text"] for line in output["lines"] for word in line if word["disposition"] == 0]


def all_words(output):
    return [(word["text"], word["disposition"]) for line in output["lines"] for word in line]


def truth_score(truth_path, recognized):
    truth = truth_path.read_text(encoding="utf-8").split()
    remaining = collections.Counter(truth)
    matched = 0
    for word in recognized:
        if remaining[word] > 0:
            remaining[word] -= 1
            matched += 1
    return {"truthWords": len(truth), "exactMatched": matched, "recall": matched / len(truth) if truth else None,
            "unmatchedRecognized": len(recognized) - matched}


def multiset_difference(base, candidate):
    left, right = collections.Counter(base), collections.Counter(candidate)
    return sum((left - right).values()), sum((right - left).values())


def geometry_delta(base, candidate):
    """Largest box-edge change when the ordered text and dispositions agree; otherwise None."""
    a = [word for line in base["lines"] for word in line]
    b = [word for line in candidate["lines"] for word in line]
    if [(w["text"], w["disposition"]) for w in a] != [(w["text"], w["disposition"]) for w in b]:
        return None
    return max((abs(x - y) for wa, wb in zip(a, b) for x, y in zip(wa["bounds"], wb["bounds"])), default=0.0)


def summarize(arguments, arms, records, manifest):
    scenes = [entry["name"] for entry in manifest]
    truths = {entry["name"]: entry.get("truth") for entry in manifest}
    # Reports are matched to scenes by file, not by name: a manifest may name a scene differently.
    files = {entry["name"]: (arguments.corpus / entry["file"]).resolve() for entry in manifest}
    table, ratios, first_ratios = [], [], []
    base = arms[0][0]
    for scene in scenes:
        row = {"scene": scene, "arms": {}}
        for name, _ in arms:
            warm, cohort_medians, firsts, shas, repeat = [], [], [], set(), True
            output = None
            for record in records:
                if record["arm"] != name:
                    continue
                entry = next(item for item in record["report"]["scenes"] if Path(item["path"]).resolve() == files[scene])
                values = [read["workerMs"] for read in entry["reads"] if read["pass"] > 0]
                warm += values
                cohort_medians.append(statistics.median(values))
                firsts.append(entry["reads"][0]["workerMs"])
                shas.add(entry["firstOutputSha256"])
                repeat = repeat and entry["repeatedOutputsIdentical"]
                output = entry["firstOutput"]
            arm = {"warmMedianMs": statistics.median(warm), "warmMinMs": min(warm), "warmMaxMs": max(warm),
                   "warmReads": len(warm), "cohortMedianMs": cohort_medians, "firstReadMs": firsts,
                   "firstReadMedianMs": statistics.median(firsts), "outputSha256": sorted(shas),
                   "deterministic": repeat and len(shas) == 1, "words": len(all_words(output)),
                   "candidateWords": len(words(output)), "_output": output}
            if truths[scene]:
                arm["truth"] = truth_score(arguments.corpus / truths[scene], words(output))
            row["arms"][name] = arm
        reference = row["arms"][base]
        for name, _ in arms[1:]:
            arm = row["arms"][name]
            lost, gained = multiset_difference(all_words(reference["_output"]), all_words(arm["_output"]))
            arm["versusBase"] = {
                "warmSpeedup": reference["warmMedianMs"] / arm["warmMedianMs"],
                "firstReadSpeedup": reference["firstReadMedianMs"] / arm["firstReadMedianMs"],
                "outputIdentical": reference["outputSha256"] == arm["outputSha256"],
                "wordsLost": lost, "wordsGained": gained,
                "maxBoxEdgeDeltaPx": geometry_delta(reference["_output"], arm["_output"])}
        table.append(row)
    for row in table:
        for arm in row["arms"].values():
            arm.pop("_output")
    geometric = {}
    for name, _ in arms[1:]:
        warm = [row["arms"][name]["versusBase"]["warmSpeedup"] for row in table]
        first = [row["arms"][name]["versusBase"]["firstReadSpeedup"] for row in table]
        geometric[name] = {"warm": math.exp(sum(map(math.log, warm)) / len(warm)),
                           "firstRead": math.exp(sum(map(math.log, first)) / len(first)),
                           "scenesIdentical": sum(row["arms"][name]["versusBase"]["outputIdentical"] for row in table),
                           "scenes": len(table)}
    return {"base": base, "scenes": table, "geometricMean": geometric}


def markdown(summary, arms):
    names = [name for name, _ in arms]
    lines = ["| Scene | " + " | ".join(f"{name} warm ms (cohorts)" for name in names) + " | " +
             " | ".join(f"{name} first ms" for name in names) + " | speedup warm / first | output | words lost/gained | truth recall |",
             "|---|" + "---|" * (2 * len(names) + 4)]
    for row in summary["scenes"]:
        cells = [row["scene"]]
        for name in names:
            arm = row["arms"][name]
            cells.append(f"{arm['warmMedianMs']:.2f} ({', '.join(f'{v:.2f}' for v in arm['cohortMedianMs'])})")
        for name in names:
            cells.append(f"{row['arms'][name]['firstReadMedianMs']:.2f}")
        last = row["arms"][names[-1]]
        versus = last.get("versusBase")
        if versus:
            cells += [f"{versus['warmSpeedup']:.3f} / {versus['firstReadSpeedup']:.3f}",
                      "identical" if versus["outputIdentical"] else "DIFFERS",
                      f"{versus['wordsLost']}/{versus['wordsGained']}"]
        else:
            cells += ["", "", ""]
        recalls = [row["arms"][name].get("truth", {}).get("recall") for name in names]
        cells.append(" / ".join("-" if value is None else f"{value:.4f}" for value in recalls))
        lines.append("| " + " | ".join(cells) + " |")
    for name, value in summary["geometricMean"].items():
        lines.append(f"\nGeometric mean {name} versus {summary['base']}: warm {value['warm']:.3f}x, first read "
                     f"{value['firstRead']:.3f}x; identical output on {value['scenesIdentical']}/{value['scenes']} scenes.")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--arm-exe", action="append", default=[], help="name=path: a different build for one arm")
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--arm", action="append", required=True, help="name=KEY=VALUE;KEY=VALUE (first arm is the base)")
    parser.add_argument("--scenes", help="comma-separated scene names (default: all)")
    parser.add_argument("--passes", type=int, default=5)
    parser.add_argument("--cohorts", type=int, default=2)
    parser.add_argument("--profile", action="store_true", help="CUDA stage events (changes timing; never mix with timing runs)")
    parser.add_argument("--stamp", default="camera", choices=("camera", "media"))
    parser.add_argument("--gap-ms", type=int, default=0)
    parser.add_argument("--qt-bin", required=True, type=Path)
    parser.add_argument("--tensorrt-root", required=True, type=Path)
    parser.add_argument("--cuda-bin", required=True, type=Path)
    parser.add_argument("--runtime-root", required=True, type=Path)
    parser.add_argument("--localappdata", required=True, type=Path)
    parser.add_argument("--lease", type=Path, help="shared GPU lease wrapper; exit 75 is retried")
    arguments = parser.parse_args()
    out = arguments.out.resolve()
    if ROOT in out.parents or out == ROOT:
        sys.exit("Refusing to write evidence inside the repository.")
    out.mkdir(parents=True, exist_ok=False)
    arms = [parse_arm(text) for text in arguments.arm]
    executables = dict(item.split("=", 1) for item in arguments.arm_exe)
    manifest = json.loads((arguments.corpus / "manifest.json").read_text(encoding="utf-8"))["scenes"]
    if arguments.scenes:
        wanted = arguments.scenes.split(",")
        manifest = [entry for entry in manifest if entry["name"] in wanted]
    paths = [arguments.corpus / entry["file"] for entry in manifest]
    identity = {"arms": {name: switches for name, switches in arms}, "passes": arguments.passes,
                "cohorts": arguments.cohorts, "profile": arguments.profile, "stamp": arguments.stamp,
                "gapMs": arguments.gap_ms, "scenes": manifest, "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "executables": {}}
    for name, _ in arms:
        exe = Path(executables.get(name, arguments.exe))
        identity["executables"][name] = {"path": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    (out / "identity.json").write_text(json.dumps(identity, indent=2), encoding="utf-8")
    records = []
    for cohort in range(arguments.cohorts):
        order = arms if cohort % 2 == 0 else arms[::-1]
        scenes = paths if cohort % 2 == 0 else paths[::-1]
        for name, switches in order:
            evidence = out / f"cohort{cohort}-{name}.json"
            report = run_process(arguments, Path(executables.get(name, arguments.exe)), switches, scenes, evidence,
                                 out / f"cohort{cohort}-{name}.log")
            records.append({"cohort": cohort, "arm": name, "report": report})
            print(f"cohort {cohort} arm {name}: startup {report['startupFirstResultMs']:.0f} ms", flush=True)
    summary = summarize(arguments, arms, records, manifest)
    summary["gpuAfter"] = gpu_state()
    summary["gpuBefore"] = [record["report"]["gpuBefore"] for record in records]
    (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    text = markdown(summary, arms)
    (out / "summary.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
