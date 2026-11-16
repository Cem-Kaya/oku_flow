"""GPU power-management characterization for paced OCR reads (rounds 13 and 14).

Runs the opt-in `optionalGpuClockProbe` slot of `uct_paddle_tests` once, with
read-only `nvidia-smi` sampling, and attaches each phase's samples (power
state, clocks, utilization, power draw) to the phase the slot measured with its
in-kernel clock meter. Phases: `idle` (nothing runs), `stepdown` (clock after
an idle gap that follows a busy period), `stepup` (clock under continuous load
from a low state), `duty` (clock under periodic load of a given busy fraction),
`frames` (emulated per-frame video work), and from round 14 `climb` (which load
raises the clock fastest from the lowest state), `stall` (how long cudaMalloc,
cudaFree and pinned allocations on another thread wait while keep-warm spins
run) and `interrupt` (how fast a raised stop flag ends a running and a queued
spin). No GPU or system setting is
changed. Evidence stays outside the repository.

    python scripts/uct/gpu_clock_probe.py --exe <uct_paddle_tests.exe> --out <new dir> \\
        --qt-bin <dir> --cuda-bin <dir> [--plan idle,stepdown,stepup,duty,frames,climb,stall,interrupt] [--lease <gpu_serial.py>]
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_measure  # noqa: E402  (sampler helpers)

ROOT = Path(__file__).resolve().parents[2]


def phase_samples(phase, samples):
    window = [s for s in samples if phase["startUnixMs"] <= s["unixMs"] <= phase["endUnixMs"]]
    return perf_measure.sample_summary(window)


def describe(phase):
    name = phase["name"]
    if name == "stepdown":
        return f"gap {phase['gapMs']} ms (repeat {phase['repeat']})"
    if name == "stepup":
        return f"from idle {phase['idleMs']} ms, {phase['load']} (repeat {phase['repeat']})"
    if name == "duty":
        nap = f", napping {phase['sleepNs'] / 1000:g} us" if phase.get("sleepNs") else ""
        return (f"period {phase['periodMs']:g} ms, busy {phase['busyFraction'] * 100:g}%, "
                f"{phase['blocks']} block(s){nap}, from {phase['from']}")
    if name == "frames":
        return (f"{phase['fps']:g} fps, {phase['passes']} passes, from {phase['from']}, "
                f"median frame GPU {phase['medianFrameGpuMs']:.3f} ms")
    if name == "climb":
        return f"{phase['load']} from idle {phase['idleMs']} ms (repeat {phase['repeat']})"
    if name == "stall":
        return f"spins of {phase['busyMs']:g} ms ({phase['launches']} launches)" if phase["busyMs"] else "no spins (control)"
    if name == "interrupt":
        return "napping warp" if phase["sleepNs"] else "spinning warp"
    return name


def spread(values):
    values = sorted(v for v in values if v >= 0)
    if not values:
        return "-"
    return f"median {statistics.median(values):.3g}, max {values[-1]:.3g}"


def first_at_or_above(trajectory, mhz):
    for at, value in trajectory:
        if value >= mhz:
            return at
    return None


def markdown(report):
    lines = [f"GPU: {report['gpu']}; plan {report['plan']}.\n",
             "| Phase | Clock (meter) | P-states (samples) | Clock MHz median | Utilization % | Power W mean |",
             "|---|---|---|---|---|---|"]
    for phase in report["phases"]:
        smi = phase.get("samples")
        if phase["name"] == "stepdown":
            meter = f"{phase['firstMHz']:.0f} first, {phase['tenthMHz']:.0f} tenth"
        elif phase["name"] == "stall":
            meter = "; ".join(f"{key[4:-2]} {spread(phase[key])} ms" for key in
                              ("cudaMallocMs", "cudaFreeMs", "cudaMallocHostMs", "cudaFreeHostMs"))
        elif phase["name"] == "interrupt":
            meter = f"stop to both spins done: {spread(phase['stopUs'])} us"
        elif phase["name"] in ("stepup", "duty", "frames", "climb"):
            values = [value for _, value in phase["trajectory"]]
            high = 0.95 * max(values) if values else 0
            reach = first_at_or_above(phase["trajectory"], 2400) if values else None
            meter = (f"start {values[0]:.0f}, end {values[-1]:.0f}, min {min(values):.0f}"
                     + (f", >=2400 at {reach:.0f} ms" if reach is not None else ", never >=2400")) if values else "-"
        else:
            meter = "-"
        if smi:
            states = " ".join(f"{key}:{count}" for key, count in smi["pstates"].items())
            lines.append(f"| {phase['name']}: {describe(phase)} | {meter} | {states} | {smi['clockMHzMedian']:.0f} | "
                         f"{smi['utilizationMean']:.1f} | {smi['powerWMean']:.1f} |")
        else:
            lines.append(f"| {phase['name']}: {describe(phase)} | {meter} | - | - | - | - |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--qt-bin", required=True, type=Path)
    parser.add_argument("--cuda-bin", required=True, type=Path)
    parser.add_argument("--plan", default="idle,stepdown,stepup,duty,frames",
                        help="comma list of idle, stepdown, stepup, duty, refine, frames, climb, stall, interrupt")
    parser.add_argument("--sample-ms", type=int, default=50, help="nvidia-smi interval, 50 to 1000 ms")
    parser.add_argument("--lease", type=Path, help="shared GPU lease wrapper; exit 75 is retried")
    arguments = parser.parse_args()
    if not 50 <= arguments.sample_ms <= 1000:
        sys.exit("--sample-ms must be 50 to 1000.")
    out = arguments.out.resolve()
    if ROOT in out.parents or out == ROOT:
        sys.exit("Refusing to write evidence inside the repository.")
    out.mkdir(parents=True, exist_ok=False)
    evidence = out / "probe.json"
    env = {key: value for key, value in os.environ.items() if not key.startswith("OKUFLOW_UCT_")}
    env.update({"OKUFLOW_UCT_CLOCK_PROBE": "1", "OKUFLOW_UCT_CLOCK_PROBE_EVIDENCE": str(evidence),
                "OKUFLOW_UCT_CLOCK_PROBE_PLAN": arguments.plan, "QT_QPA_PLATFORM": "offscreen",
                "QTEST_FUNCTION_TIMEOUT": "3600000",
                "PATH": os.pathsep.join([str(arguments.qt_bin), str(arguments.cuda_bin), os.environ.get("PATH", "")])})
    command = [str(arguments.exe), "optionalGpuClockProbe"]
    if arguments.lease:
        command = [sys.executable, str(arguments.lease)] + command
    smi = out / "probe.smi.csv"
    waits = 0
    while True:
        before = perf_measure.gpu_state()
        if smi.exists():
            smi.unlink()
        sampler = perf_measure.start_sampler(arguments.sample_ms, smi)
        try:
            with open(out / "probe.log", "ab") as stream:
                code = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=3600).returncode
        finally:
            perf_measure.stop_sampler(sampler)
        if code != 75:
            break
        waits += 1
        time.sleep(5)
    if code != 0 or not evidence.exists():
        sys.exit(f"Probe failed with exit {code}; see {out / 'probe.log'}")
    report = json.loads(evidence.read_text(encoding="utf-8"))
    samples = perf_measure.read_samples(smi)
    for phase in report["phases"]:
        phase["samples"] = phase_samples(phase, samples)
    report["gpuBefore"] = before
    report["leaseWaits"] = waits
    report["sampleMs"] = arguments.sample_ms
    (out / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    text = markdown(report)
    (out / "summary.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
