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

Paced runs (`--gap-ms`: the application reads on timers, not back to back)
also keep every read's time in order, because the effect of GPU power
management develops over seconds; the median of the reads from
`--settled-from` on; and, with `--gpu-sample-ms`, the GPU power state, clocks,
utilization and power draw sampled by `nvidia-smi` (read-only queries) while
each process runs.

Engines are never prepared: point --localappdata at a cache that already
holds the plans. Evidence stays outside the repository.

    python scripts/uct/perf_measure.py --exe <uct_paddle_tests.exe> --corpus <dir> --out <new dir> \
        --arm base= --arm candidate=OKUFLOW_UCT_SWITCH=1 --qt-bin <dir> --tensorrt-root <dir> \
        --cuda-bin <dir> --runtime-root <dir> --localappdata <dir> [--lease <gpu_serial.py>]
"""
import argparse
import collections
import datetime
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


SMI_FIELDS = ("timestamp", "pstate", "clocks.gr", "clocks.mem", "utilization.gpu", "power.draw")
SMI_MAX_BYTES = 64 * 1024 * 1024  # bound for one process's sample file


def start_sampler(interval_ms, path):
    """Start a read-only `nvidia-smi` sampler; returns (process, file) or None."""
    if not interval_ms:
        return None
    stream = open(path, "wb")
    try:
        process = subprocess.Popen(["nvidia-smi", f"--query-gpu={','.join(SMI_FIELDS)}",
                                    "--format=csv,noheader,nounits", "-lms", str(interval_ms)],
                                   stdout=stream, stderr=subprocess.DEVNULL)
    except OSError:
        stream.close()
        return None
    return process, stream


def stop_sampler(sampler):
    if not sampler:
        return
    process, stream = sampler
    process.terminate()  # only the sampler this script started
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)
    stream.close()


def read_samples(path):
    """Parse sampler rows into dicts with a Unix-millisecond time; malformed rows are skipped."""
    samples = []
    if not path.exists():
        return samples
    with open(path, "rb") as stream:
        data = stream.read(SMI_MAX_BYTES)
    for line in data.decode("utf-8", "replace").splitlines():
        parts = [part.strip() for part in line.split(",")]
        if len(parts) != len(SMI_FIELDS):
            continue
        try:
            stamp = datetime.datetime.strptime(parts[0], "%Y/%m/%d %H:%M:%S.%f")
            samples.append({"unixMs": stamp.timestamp() * 1000.0, "pstate": parts[1],
                            "clockMHz": float(parts[2]), "memClockMHz": float(parts[3]),
                            "utilization": float(parts[4]), "powerW": float(parts[5])})
        except ValueError:
            continue
    return samples


def sample_summary(samples):
    if not samples:
        return None
    states = collections.Counter(sample["pstate"] for sample in samples)
    return {"samples": len(samples), "pstates": dict(sorted(states.items())),
            "clockMHzMedian": statistics.median(s["clockMHz"] for s in samples),
            "clockMHzMin": min(s["clockMHz"] for s in samples),
            "memClockMHzMedian": statistics.median(s["memClockMHz"] for s in samples),
            "utilizationMean": statistics.fmean(s["utilization"] for s in samples),
            "powerWMean": statistics.fmean(s["powerW"] for s in samples),
            "powerWMax": max(s["powerW"] for s in samples)}


def attach_samples(report, samples):
    """Summarize the samples inside the timed reads; tag each read with the latest earlier sample."""
    reads = [read for scene in report["scenes"] for read in scene["reads"] if read.get("submittedUnixMs")]
    if not reads or not samples:
        report["gpuSamples"] = sample_summary(samples)
        return
    first = min(read["submittedUnixMs"] for read in reads)
    end = max(read["submittedUnixMs"] + read["submitToCallbackMs"] for read in reads)
    report["gpuSamples"] = sample_summary([s for s in samples if first <= s["unixMs"] <= end])
    report["gpuSampleWindowUnixMs"] = [first, end]
    tail = report.get("tail") or {}
    if tail.get("startUnixMs"):
        # The idle tail after the last read, split at the keep-warm time-out.
        timeout = (report.get("keepWarm") or {}).get("idleTimeoutMs", 0)
        split = end + timeout + 500
        report["tailSamples"] = {
            "beforeTimeout": sample_summary([s for s in samples if end < s["unixMs"] <= split]),
            "afterTimeout": sample_summary([s for s in samples if split < s["unixMs"] <= tail["endUnixMs"]])}
    for read in reads:
        before = [sample for sample in samples if sample["unixMs"] <= read["submittedUnixMs"]]
        if before:
            latest = before[-1]
            read["gpuBefore"] = {"pstate": latest["pstate"], "clockMHz": latest["clockMHz"],
                                 "ageMs": read["submittedUnixMs"] - latest["unixMs"]}


def run_process(arguments, exe, switches, scenes, evidence, log):
    extra = {
        "OKUFLOW_UCT_CORPUS_TIMING": "1",
        "OKUFLOW_UCT_CORPUS_IMAGES": ";".join(str(scene) for scene in scenes),
        "OKUFLOW_UCT_CORPUS_PASSES": str(arguments.passes),
        "OKUFLOW_UCT_CORPUS_EVIDENCE": str(evidence),
        "OKUFLOW_UCT_CORPUS_PROFILE": "1" if arguments.profile else "0",
        "OKUFLOW_UCT_CORPUS_STAMP": arguments.stamp,
        "OKUFLOW_UCT_CORPUS_GAP_MS": str(arguments.gap_ms),
        "OKUFLOW_UCT_CORPUS_FRAME_LOAD": arguments.frame_load or "",
        "OKUFLOW_UCT_CORPUS_TAIL_MS": str(arguments.tail_ms),
    }
    command = [str(exe), "optionalCorpusTiming"]
    if arguments.lease:
        command = [sys.executable, str(arguments.lease)] + command
    env = environment(arguments, switches, extra)
    waits = 0
    smi = evidence.with_suffix(".smi.csv")
    while True:
        before = gpu_state()
        if smi.exists():
            smi.unlink()  # a lease wait discards that attempt's samples
        sampler = start_sampler(arguments.gpu_sample_ms, smi)
        try:
            with open(log, "ab") as stream:
                code = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                      timeout=3600).returncode
        finally:
            stop_sampler(sampler)
        if code != 75:
            break
        waits += 1  # the shared GPU lease was busy
        time.sleep(5)
    if code != 0 or not evidence.exists():
        sys.exit(f"Timing process failed with exit {code}; kept {log}")
    report = json.loads(evidence.read_text(encoding="utf-8"))
    report["gpuBefore"] = before
    report["leaseWaits"] = waits
    if arguments.gpu_sample_ms:
        attach_samples(report, read_samples(smi))
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
            settled, cohort_settled, series = [], [], []
            output = None
            for record in records:
                if record["arm"] != name:
                    continue
                entry = next(item for item in record["report"]["scenes"] if Path(item["path"]).resolve() == files[scene])
                values = [read["workerMs"] for read in entry["reads"] if read["pass"] > 0]
                warm += values
                cohort_medians.append(statistics.median(values))
                late = [read["workerMs"] for read in entry["reads"] if read["pass"] >= arguments.settled_from]
                if late:
                    settled += late
                    cohort_settled.append(statistics.median(late))
                # Every read in order, first read included, with the GPU state sampled just before it.
                series.append({"cohort": record["cohort"],
                               "workerMs": [round(read["workerMs"], 2) for read in entry["reads"]],
                               "gpuBefore": [read.get("gpuBefore", {}).get("pstate") for read in entry["reads"]]})
                firsts.append(entry["reads"][0]["workerMs"])
                shas.add(entry["firstOutputSha256"])
                repeat = repeat and entry["repeatedOutputsIdentical"]
                output = entry["firstOutput"]
            arm = {"warmMedianMs": statistics.median(warm), "warmMinMs": min(warm), "warmMaxMs": max(warm),
                   "warmReads": len(warm), "cohortMedianMs": cohort_medians, "series": series,
                   "settledMedianMs": statistics.median(settled) if settled else None,
                   "settledCohortMedianMs": cohort_settled, "firstReadMs": firsts,
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
    return {"base": base, "gapMs": arguments.gap_ms, "settledFrom": arguments.settled_from,
            "scenes": table, "geometricMean": geometric}


def markdown(summary, arms, records):
    names = [name for name, _ in arms]
    lines = [f"Gap before each read: {summary['gapMs']} ms. Settled median: reads with pass >= "
             f"{summary['settledFrom']} (pass 0 is the scene's first read).\n",
             "| Scene | " + " | ".join(f"{name} warm ms (cohorts)" for name in names) + " | " +
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
    lines += ["\n## Settled medians\n",
              "| Scene | " + " | ".join(f"{name} settled ms (cohorts)" for name in names) + " |",
              "|---|" + "---|" * len(names)]
    for row in summary["scenes"]:
        cells = [row["scene"]]
        for name in names:
            arm = row["arms"][name]
            value = arm["settledMedianMs"]
            cells.append("-" if value is None else
                         f"{value:.2f} ({', '.join(f'{v:.2f}' for v in arm['settledCohortMedianMs'])})")
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("\n## Per-read series\n\nMilliseconds, first read first; with GPU sampling, the P-state "
                 "sampled last before each read.\n")
    for row in summary["scenes"]:
        for name in names:
            for item in row["arms"][name]["series"]:
                values = [f"{ms:g}" + (f"/{state}" if state else "")
                          for ms, state in zip(item["workerMs"], item["gpuBefore"])]
                lines.append(f"- {row['scene']} {name} cohort {item['cohort']}: " + " ".join(values))
    tails = [record for record in records if record["report"].get("tail")]
    if tails:
        lines += ["\n## Idle tail after the last read\n",
                  "| Cohort | Arm | Armed at start / end | Keep-warm launches after reads / at end | Time-outs | "
                  "Power W before / after time-out | P-states after time-out |", "|---|---|---|---|---|---|---|"]
        for record in tails:
            report = record["report"]
            tail, keep = report["tail"], report.get("keepWarm", {})
            samples = report.get("tailSamples") or {}
            before, after = samples.get("beforeTimeout"), samples.get("afterTimeout")
            power = " / ".join("-" if value is None else f"{value['powerWMean']:.1f}" for value in (before, after))
            states = " ".join(f"{key}:{count}" for key, count in (after or {}).get("pstates", {}).items()) or "-"
            lines.append(f"| {record['cohort']} | {record['arm']} | {tail.get('armedAtStart')} / {tail.get('armedAtEnd')} | "
                         f"{keep.get('afterReads', {}).get('launches')} / {tail.get('stats', {}).get('launches')} | "
                         f"{tail.get('stats', {}).get('timeouts')} | {power} | {states} |")
    keeps = [record for record in records if (record["report"].get("keepWarm") or {}).get("mode")]
    if keeps:
        lines += ["\n## Keep-warm activity during the reads\n",
                  "| Cohort | Arm | Launches | Previous still running | Not ready | Spin requested ms |",
                  "|---|---|---|---|---|---|"]
        for record in keeps:
            value = record["report"]["keepWarm"]["afterReads"]
            lines.append(f"| {record['cohort']} | {record['arm']} | {value['launches']:.0f} | {value['pending']:.0f} | "
                         f"{value['notReady']:.0f} | {value['busyMs']:.1f} |")
    sampled = [record for record in records if record["report"].get("gpuSamples")]
    if sampled:
        lines += ["\n## GPU state during the timed reads (nvidia-smi samples)\n",
                  "| Cohort | Arm | Samples | P-states | Clock MHz median (min) | Memory MHz median | "
                  "Utilization mean % | Power mean (max) W |",
                  "|---|---|---|---|---|---|---|---|"]
        for record in sampled:
            value = record["report"]["gpuSamples"]
            states = " ".join(f"{key}:{count}" for key, count in value["pstates"].items())
            lines.append(f"| {record['cohort']} | {record['arm']} | {value['samples']} | {states} | "
                         f"{value['clockMHzMedian']:.0f} ({value['clockMHzMin']:.0f}) | "
                         f"{value['memClockMHzMedian']:.0f} | {value['utilizationMean']:.1f} | "
                         f"{value['powerWMean']:.1f} ({value['powerWMax']:.1f}) |")
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
    parser.add_argument("--gap-ms", type=int, default=0, help="sleep before each read (application pacing)")
    parser.add_argument("--settled-from", type=int, default=5,
                        help="median of the reads from this pass on (pass 0 is the first read)")
    parser.add_argument("--gpu-sample-ms", type=int, default=0,
                        help="sample GPU state with nvidia-smi at this interval (50-1000 ms; off by default)")
    parser.add_argument("--frame-load", help="emulated per-frame video GPU work during the reads, fps:passes")
    parser.add_argument("--tail-ms", type=int, default=0,
                        help="idle time after the last read, to show keep-warm stopping (0-20000)")
    parser.add_argument("--qt-bin", required=True, type=Path)
    parser.add_argument("--tensorrt-root", required=True, type=Path)
    parser.add_argument("--cuda-bin", required=True, type=Path)
    parser.add_argument("--runtime-root", required=True, type=Path)
    parser.add_argument("--localappdata", required=True, type=Path)
    parser.add_argument("--lease", type=Path, help="shared GPU lease wrapper; exit 75 is retried")
    arguments = parser.parse_args()
    if arguments.gpu_sample_ms and not 50 <= arguments.gpu_sample_ms <= 1000:
        sys.exit("--gpu-sample-ms must be 0 (off) or 50 to 1000.")
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
                "gapMs": arguments.gap_ms, "settledFrom": arguments.settled_from,
                "gpuSampleMs": arguments.gpu_sample_ms, "frameLoad": arguments.frame_load,
                "tailMs": arguments.tail_ms, "scenes": manifest, "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
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
    text = markdown(summary, arms, records)
    (out / "summary.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
