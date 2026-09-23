#!/usr/bin/env python3
"""CacheSim regression / performance test harness.

Three run modes:
  baseline    - rerun HRU-8B/PHRU-8B (no dueling), exact-match diff against
                golden_values.json's static_baselines. lru_64b/srrip_64b are
                pure reference data in that file - this script never runs them.
  dueling     - rerun HRU-8B/PHRU-8B under both the psel and ztest presets,
                exact-match diff against golden_values.json's dueling_baselines.
  performance - rerun HRU-8B/HRUpp-8B/PHRU-8B/PHRUpp-8B (dueling preset
                configurable via --dueling-preset, default psel), report MPKI
                gain % and speedup vs LRU-64B/SRRIP-64B (static, never rerun)
                and vs each policy's own performance_history checkpoint. Not a
                pass/fail mode - it's a report.

Every policy in `performance` mode is compared only against the universal
LRU/SRRIP anchors and against its own history - never against a sibling
policy (e.g. HRU is never diffed against HRUpp) - because HRUpp/PHRUpp don't
get iso-area treatment by default at 8B the way HRU/PHRU do (see README), so a
direct HRU-vs-HRUpp comparison would silently compare unequal capacities.
"""

import argparse
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]  # .../CacheSim
BINARY = REPO_ROOT / "bin" / "cachesim_multi"
TRACE_DIR = Path("/nobackup.1/vishnu/adaptive-cache/pin/pin/traces/hubs/pr_spmv")
GOLDEN_FILE = Path(__file__).resolve().parent / "golden_values.json"

DEFAULT_TRACES = ["as-Skitter", "web-BerkStan", "sx-stackoverflow", "cit-Patents"]
FULL_TRACES = DEFAULT_TRACES + [
    "com-LiveJournal", "com-Youtube", "soc-LiveJournal1", "web-Google", "wiki-topcats",
]

BASE_ARGS = [
    "--trace-format", "instructions",
    "--config", "configs/default.cfg",
    "--num-cache-sets", "1024",
    "--num-cache-ways", "32",
]

# Exact flags for "use set-dueling" (psel) / "use ztest" - see
# ../../../../.claude memory feedback_cachesim_dueling_shorthand.md. Keep this
# in sync with that memory file if the convention ever changes.
DUELING_PRESETS = {
    "psel": [
        "--cache-block-size", "8", "--set-dueling",
        "--dueling-mode", "psel", "--dueling-period", "1000000",
    ],
    "ztest": [
        "--cache-block-size", "8", "--set-dueling",
        "--dueling-mode", "ztest", "--conf-margin", "1", "--conf-epoch", "10000",
    ],
}


def cfg_no_dueling(policy, block_size=8):
    return ["--cache-block-size", str(block_size), "--replacement-policy", policy]


def cfg_dueling(policy, preset):
    return ["--replacement-policy", policy] + DUELING_PRESETS[preset]


def cfg_64b(policy):
    return ["--cache-block-size", "64", "--replacement-policy", policy]


BASELINE_RERUN_CONFIGS = {
    "hru_8b_no_dueling": cfg_no_dueling("hru"),
    "phru_8b_no_dueling": cfg_no_dueling("phru"),
}

DUELING_CONFIGS = {
    "hru_8b_psel": cfg_dueling("hru", "psel"),
    "hru_8b_ztest": cfg_dueling("hru", "ztest"),
    "phru_8b_psel": cfg_dueling("phru", "psel"),
    "phru_8b_ztest": cfg_dueling("phru", "ztest"),
}

PERFORMANCE_POLICIES = ["hru", "hrupp", "phru", "phrupp"]

MPKI_RE = re.compile(r"^MPKI\s+([\d.]+)", re.MULTILINE)
CYCLES_RE = re.compile(r"^Num Cycles\s+(\d+)", re.MULTILINE)


def build_binary():
    print("=== building (make release) ===")
    result = subprocess.run(
        ["make", "release"], cwd=REPO_ROOT, capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(result.stdout[-4000:])
        print(result.stderr[-4000:])
        raise RuntimeError("make release failed - see build output above")
    if not BINARY.exists():
        raise RuntimeError(f"make release succeeded but {BINARY} is still missing")
    print("build OK\n")


def run_one(trace, extra_args):
    trace_path = TRACE_DIR / f"{trace}.random.log"
    cmd = [str(BINARY), "--trace", str(trace_path)] + BASE_ARGS + extra_args
    result = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=1800)
    mpkis = MPKI_RE.findall(result.stdout)
    cycles = CYCLES_RE.findall(result.stdout)
    if len(mpkis) < 2 or len(cycles) < 2:
        raise RuntimeError(
            f"Could not parse MPKI/Num Cycles (LLC) for trace={trace}, cmd={' '.join(cmd)}\n"
            f"--- stdout tail ---\n{result.stdout[-2000:]}\n"
            f"--- stderr tail ---\n{result.stderr[-2000:]}"
        )
    # Every level (L1D, then LLC) prints its own MPKI/Num Cycles line - the
    # second occurrence of each is the LLC, which is what we track.
    return {"mpki": float(mpkis[1]), "cycles": int(cycles[1])}


def run_config_all_traces(extra_args, traces):
    """Runs one config across multiple traces in parallel - safe because each
    trace is a different file; never call this twice concurrently for the
    same config (that would run two processes against the same trace file)."""
    results = {}
    with ThreadPoolExecutor(max_workers=len(traces)) as ex:
        futures = {ex.submit(run_one, t, extra_args): t for t in traces}
        for fut, trace in futures.items():
            results[trace] = fut.result()
    return results


def load_golden():
    if not GOLDEN_FILE.exists():
        return {
            "traces": DEFAULT_TRACES,
            "static_baselines": {},
            "dueling_baselines": {},
            "performance_history": {},
        }
    return json.loads(GOLDEN_FILE.read_text())


def save_golden(data):
    GOLDEN_FILE.write_text(json.dumps(data, indent=2) + "\n")


def compare(trace, golden, measured):
    if golden is None:
        return f"  [NO GOLDEN] {trace}: measured mpki={measured['mpki']} cycles={measured['cycles']} (nothing to compare against - run with --update to establish it)"
    mpki_match = golden["mpki"] == measured["mpki"]
    cycles_match = golden["cycles"] == measured["cycles"]
    if mpki_match and cycles_match:
        return f"  [PASS] {trace}: mpki={measured['mpki']} cycles={measured['cycles']}"
    lines = [f"  [FAIL] {trace}:"]
    if not mpki_match:
        delta = (measured["mpki"] - golden["mpki"]) / golden["mpki"] * 100
        lines.append(f"    mpki:   golden={golden['mpki']}  measured={measured['mpki']}  ({delta:+.4f}%)")
    if not cycles_match:
        delta = (measured["cycles"] - golden["cycles"]) / golden["cycles"] * 100
        lines.append(f"    cycles: golden={golden['cycles']}  measured={measured['cycles']}  ({delta:+.4f}%)")
    return "\n".join(lines)


def mpki_gain_pct(reference, candidate):
    return (reference - candidate) / reference * 100


def speedup(reference_cycles, candidate_cycles):
    return reference_cycles / candidate_cycles


def mode_baseline(golden, traces, do_update):
    print("=== baseline regression: HRU-8B, PHRU-8B (no dueling) ===")
    print("(lru_64b / srrip_64b are read-only reference values - not rerun)")
    any_fail = False
    for name, args in BASELINE_RERUN_CONFIGS.items():
        print(f"\n-- {name} --")
        measured = run_config_all_traces(args, traces)
        for trace in traces:
            g = golden.get("static_baselines", {}).get(name, {}).get(trace)
            report = compare(trace, g, measured[trace])
            print(report)
            if "[FAIL]" in report:
                any_fail = True
        if do_update:
            golden.setdefault("static_baselines", {}).setdefault(name, {})
            for trace in traces:
                golden["static_baselines"][name][trace] = measured[trace]
    if do_update:
        save_golden(golden)
        print("\nGolden file updated.")
    return not any_fail


def mode_dueling(golden, traces, do_update):
    print("=== set-dueling regression: HRU-8B, PHRU-8B x {psel, ztest} ===")
    any_fail = False
    for name, args in DUELING_CONFIGS.items():
        print(f"\n-- {name} --")
        measured = run_config_all_traces(args, traces)
        for trace in traces:
            g = golden.get("dueling_baselines", {}).get(name, {}).get(trace)
            report = compare(trace, g, measured[trace])
            print(report)
            if "[FAIL]" in report:
                any_fail = True
        if do_update:
            golden.setdefault("dueling_baselines", {}).setdefault(name, {})
            for trace in traces:
                golden["dueling_baselines"][name][trace] = measured[trace]
    if do_update:
        save_golden(golden)
        print("\nGolden file updated.")
    return not any_fail


def mode_performance(golden, traces, dueling_preset, do_update):
    print(f"=== performance study: HRU/HRUpp/PHRU/PHRUpp @ 8B ({dueling_preset}) ===")
    lru = golden.get("static_baselines", {}).get("lru_64b", {})
    srrip = golden.get("static_baselines", {}).get("srrip_64b", {})
    for policy in PERFORMANCE_POLICIES:
        config_name = f"{policy}_8b_{dueling_preset}"
        print(f"\n-- {policy} ({config_name}) --")
        measured = run_config_all_traces(cfg_dueling(policy, dueling_preset), traces)
        history = golden.get("performance_history", {}).get(config_name, {})
        for trace in traces:
            m = measured[trace]
            row = [f"  {trace}: mpki={m['mpki']} cycles={m['cycles']}"]
            if trace in lru:
                row.append(
                    f"    vs LRU-64B:     mpki_gain={mpki_gain_pct(lru[trace]['mpki'], m['mpki']):+.2f}%"
                    f"  speedup={speedup(lru[trace]['cycles'], m['cycles']):.3f}x"
                )
            if trace in srrip:
                row.append(
                    f"    vs SRRIP-64B:   mpki_gain={mpki_gain_pct(srrip[trace]['mpki'], m['mpki']):+.2f}%"
                    f"  speedup={speedup(srrip[trace]['cycles'], m['cycles']):.3f}x"
                )
            if trace in history:
                h = history[trace]
                row.append(
                    f"    vs own history: mpki_gain={mpki_gain_pct(h['mpki'], m['mpki']):+.2f}%"
                    f"  speedup={speedup(h['cycles'], m['cycles']):.3f}x"
                )
            else:
                row.append("    vs own history: [no checkpoint yet - run with --update to establish one]")
            print("\n".join(row))
        if do_update:
            golden.setdefault("performance_history", {}).setdefault(config_name, {})
            for trace in traces:
                golden["performance_history"][config_name][trace] = measured[trace]
    if do_update:
        save_golden(golden)
        print("\nGolden file updated (performance_history checkpoints).")
    return True


def main():
    parser = argparse.ArgumentParser(description="CacheSim regression / performance test harness")
    parser.add_argument("mode", choices=["baseline", "dueling", "performance"])
    parser.add_argument("--full", action="store_true", help="Use all 9 traces instead of the default 4")
    parser.add_argument("--update", action="store_true", help="Write newly-measured values into golden_values.json")
    parser.add_argument(
        "--dueling-preset", choices=["psel", "ztest"], default="psel",
        help="Dueling preset for performance mode (default: psel)",
    )
    parser.add_argument(
        "--no-build", action="store_true",
        help="Skip the 'make release' build step and use whatever binary is already there",
    )
    args = parser.parse_args()

    if not args.no_build:
        build_binary()

    golden = load_golden()
    traces = FULL_TRACES if args.full else DEFAULT_TRACES

    if args.mode == "baseline":
        ok = mode_baseline(golden, traces, args.update)
    elif args.mode == "dueling":
        ok = mode_dueling(golden, traces, args.update)
    else:
        ok = mode_performance(golden, traces, args.dueling_preset, args.update)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
