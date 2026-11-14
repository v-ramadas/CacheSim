#!/usr/bin/env python3
"""Multicore scheduler, barrier and invariant tests.

Small hand-written traces (seconds): every multi-core run is replayed from its --debug-schedule log and checked
against the scheduling rule and the barrier semantics (see replay()). With --real, also smoke runs on real traces
(minutes): set dueling on 2 cores, 8 cores, invariants.

Usage: python3 tests/multicore/run_tests.py [--real]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REAL = Path("/nobackup.1/vishnu/adaptive-cache/pin/pin/traces_roi/hubs")
BIN = REPO / "bin/cachesim_multi"
BIN_SINGLE = REPO / "bin/cachesim"
FAILS = []


# ------------------------------------------------------------------------------------------------ helpers
def acc(n, addr, write=False, fmt="instructions"):
    kind = "Write" if write else "Read"
    if fmt == "addresses":
        return f"PC:4198400 {kind.lower()}:{addr:#x} 0 0 0.00"
    return f"instCount:{n},PC:0x401000,{kind}:{addr:#x},NextReuse:0,NodeDegree:0,PCAvgDegree:0.00"


def hits(n0, n1, addr, fmt="instructions"):          # accesses n0..n1-1 to one line (hits after the first)
    return [acc(i, addr, fmt=fmt) for i in range(n0, n1)]


def misses(n0, n1, base, fmt="instructions"):        # accesses n0..n1-1 to lines 1 MB apart (all miss)
    return [acc(i, base + i * 0x100000, fmt=fmt) for i in range(n0, n1)]


class Run:
    counter = 0
    scheduler = "min_clock"        # scheduler for runs that do not name one (main() also reruns with the other)

    def __init__(self, d, traces, args=(), binary=BIN, fmt="instructions", cores=None, config="two_level",
                 scheduler=None):
        scheduler = scheduler or Run.scheduler
        self.scheduler = next((args[i + 1] for i in range(len(args) - 1) if args[i] == "--scheduler"), scheduler)
        if "--scheduler" not in args:
            args = [*args, "--scheduler", scheduler]
        self.paths = []
        for lines in traces:
            if lines is None:
                self.paths.append("none")
                continue
            Run.counter += 1
            p = d / f"t{Run.counter}.txt"
            p.write_text("\n".join(lines) + "\n")
            self.paths.append(str(p))
        n = cores or len(traces)
        cmd = [str(binary), "--config", str(REPO / f"configs/{config}.yaml"), "--trace-format", fmt,
               "--cache-block-size", "64", "--replacement-policy", "srrip", "--num-cores", str(n),
               "--traces", *self.paths, "--debug-schedule", *args]
        self.p = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
        self.out, self.err, self.rc = self.p.stdout, self.p.stderr, self.p.returncode
        self.events = []
        for line in self.out.splitlines():
            for kind, pat in (("step", r"SCHED step core (\d+) clock (\d+)"),
                              ("arrive", r"SCHED arrive core (\d+) barrier (\d+) clock (\d+)"),
                              ("finish", r"SCHED finish core (\d+) clock (\d+)"),
                              ("release", r"SCHED release barrier (\d+) clock (\d+)")):
                m = re.match(pat, line)
                if m:
                    self.events.append((kind, *map(int, m.groups())))
                    break
        self.cores = {}
        for m in re.finditer(r"^Core (\d+): instructions (\d+) cycles (\d+) IPC \S+ accesses (\d+) barriers (\d+) "
                             r"barrier_wait_cycles (\d+)(.*)$", self.out, re.M):
            self.cores[int(m[1])] = dict(inst=int(m[2]), cycles=int(m[3]), acc=int(m[4]), barriers=int(m[5]),
                                         wait=int(m[6]), rest=m[7])
        self.system = next((l for l in self.out.splitlines() if l.startswith("System: cycles")), "")
        self.inv = next((l for l in self.out.splitlines() if l.startswith("Invariants:")), "")


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + ("" if cond or not detail else f": {detail}"))
    if not cond:
        FAILS.append(name)
    return cond


def replay(r, n, mode="round_robin_skip"):
    """Re-derive every scheduling decision from the log. Returns (ok, message, waits per core).

    State: clock per core, waiting (arrived, not released), finished. After each step of core `prev`:
      - if another core is runnable with clock <= clock[prev], the next step is the first such core after prev
        in id order (wrapping);
      - else, if prev is runnable, prev steps again;
      - else the runnable core with the smallest clock (lowest id on ties);
      - if no core is runnable, a release must come first (all live cores waiting, one barrier id, clock = latest
        arrival; finished cores count as arrived) and the next step is the lowest-id runnable core.
    No core steps while waiting or finished. (The step that reads a BARRIER line or finds the end of the trace
    logs its arrive/finish line first; that state applies after the step.)
    """
    clock = {c: 0 for c in range(n)}
    waiting, finished, arrive = set(), set(), {}
    waits = {c: 0 for c in range(n)}
    active = {c for c in range(n) if r.paths[c] != "none"}
    released, prev = False, None
    # A core's arrive/finish line is printed inside the step that read it, before that step's own "step" line:
    # hold it until that step has been checked (the scheduler chose the step before the core arrived/finished).
    pending = []

    def apply(ev):
        if ev[0] == "arrive":
            waiting.add(ev[1])
            arrive[ev[1]] = (ev[2], ev[3])
        else:
            finished.add(ev[1])

    for k, e in enumerate(r.events):
        kind = e[0]
        if kind in ("arrive", "finish"):
            nxt = r.events[k + 1] if k + 1 < len(r.events) else None
            if nxt is not None and nxt[0] == "step" and nxt[1] == e[1]:
                pending.append(e)
            else:
                apply(e)
        elif kind == "release":
            bid, rel = e[1], e[2]
            live = active - finished
            if waiting != live:
                return False, f"release of barrier {bid} with waiting {sorted(waiting)} but live {sorted(live)}", waits
            if any(arrive[c][0] != bid for c in waiting):
                return False, f"release of barrier {bid}: ids {[arrive[c][0] for c in waiting]}", waits
            if rel != max(arrive[c][1] for c in waiting):
                return False, f"release clock {rel} != latest arrival {max(arrive[c][1] for c in waiting)}", waits
            for c in waiting:
                waits[c] += rel - arrive[c][1]
                clock[c] = rel
            waiting.clear()
            released = True
        elif kind == "step":
            cur, clk = e[1], e[2]
            if cur in waiting or cur in finished:
                return False, f"event {k}: core {cur} stepped while waiting/finished", waits
            if prev is not None:
                runnable = [c for c in sorted(active) if c not in waiting and c not in finished]
                if mode == "min_clock":
                    exp = min(runnable, key=lambda c: (clock[c], c), default=None)
                elif released:
                    exp = runnable[0] if runnable else None
                else:
                    exp = None
                    for j in range(1, n):
                        cand = (prev + j) % n
                        if cand in runnable and clock[cand] <= clock[prev]:
                            exp = cand
                            break
                    if exp is None:
                        exp = prev if prev in runnable else min(runnable, key=lambda c: (clock[c], c), default=None)
                if exp != cur:
                    return False, f"event {k}: core {cur} stepped, rule says {exp} (prev {prev}, clocks {clock}, " \
                                  f"waiting {sorted(waiting)}, finished {sorted(finished)})", waits
            released = False
            clock[cur] = clk
            prev = cur
            for ev in pending:
                apply(ev)
            pending.clear()
    return True, "", waits


def common_checks(r, n, barriers_expected=None, mode=None):
    check("run succeeds", r.rc == 0, r.err[-400:])
    good, msg, waits = replay(r, n, mode or r.scheduler)
    check("every decision follows the rule and the barrier semantics", good, msg)
    check("barrier_wait_cycles match the log",
          all(r.cores.get(c, {}).get("wait", 0) == waits[c] for c in range(n)),
          f"{ {c: r.cores.get(c, {}).get('wait') for c in range(n)} } vs {waits}")
    if barriers_expected is not None:
        check(f"{barriers_expected} barrier releases", r.system.endswith(f"barriers {barriers_expected}"), r.system)


# ------------------------------------------------------------------------------------------------ small tests
def t_two_core_barriers(d):
    print("2 cores, fast/slow, two barriers")
    fast = hits(1, 5, 0x10000000) + ["BARRIER:1"] + hits(5, 7, 0x10000000) + ["BARRIER:2"] + hits(7, 8, 0x10000000)
    slow = misses(1, 5, 0x20000000) + ["BARRIER:1"] + misses(5, 7, 0x20000000) + ["BARRIER:2"] + \
        misses(7, 8, 0x20000000)
    r = Run(d, [fast, slow])
    common_checks(r, 2, 2)
    # Running-core gap peaks after the first step: core 0 at 131 (a miss) while core 1 is still at 0.
    check("max clock skew 131", "max clock skew 131 cycles" in r.system, r.system)
    check("fast core waited, slow core did not", r.cores[0]["wait"] > 0 and r.cores[1]["wait"] == 0, str(r.cores))
    order = next((l for l in r.out.splitlines() if l.startswith("System: LLC arrival order")), "")
    # With two cores the rule always runs the core that is behind, so LLC arrivals are in time order.
    check("2 cores: LLC demand accesses arrive in time order", "out of time order 0 " in order, order)


def t_three_core_barriers(d):
    print("3 cores, different speeds, two barriers; lowest id runs first after a release")
    a = hits(1, 4, 0x10000000) + ["BARRIER:1"] + hits(4, 9, 0x10000000) + ["BARRIER:2"] + hits(9, 10, 0x10000000)
    b = misses(1, 6, 0x20000000) + ["BARRIER:1"] + misses(6, 7, 0x20000000) + ["BARRIER:2"] + \
        misses(7, 9, 0x20000000)
    c = misses(1, 3, 0x40000000) + hits(3, 6, 0x50000000) + ["BARRIER:1"] + misses(6, 9, 0x40000000) + ["BARRIER:2"]
    r = Run(d, [a, b, c])
    common_checks(r, 3, 2)
    after = [r.events[k + 1] for k, e in enumerate(r.events[:-1]) if e[0] == "release"]
    check("after each release the lowest-id core steps first", all(e[0] == "step" and e[1] == 0 for e in after),
          str(after))


def t_rule_with_idle_core(d):
    print("4 cores with an idle core (core 1); no barriers")
    tr = [misses(1, 30, 0x10000000), None, hits(1, 40, 0x30000000),
          misses(1, 10, 0x40000000) + hits(10, 30, 0x48000000)]
    r = Run(d, tr)
    common_checks(r, 4, 0)
    check("idle core counts zero instructions", r.cores.get(1, {}).get("inst") == 0, str(r.cores.get(1)))


def t_finish_while_others_wait(d):
    print("a core finishes after the others are already waiting")
    waiters = [hits(1, 3, 0x10000000) + ["BARRIER:1"] + hits(3, 5, 0x10000000),
               hits(1, 3, 0x20000000) + ["BARRIER:1"] + hits(3, 5, 0x20000000)]
    straggler = misses(1, 8, 0x40000000)                       # never reaches the barrier: its trace ends
    r = Run(d, waiters + [straggler])
    common_checks(r, 3, 1)
    fin = next((e for e in r.events if e[0] == "finish" and e[1] == 2), None)
    rel = next((e for e in r.events if e[0] == "release"), None)
    check("release comes after the straggler finishes", fin is not None and rel is not None and
          r.events.index(fin) < r.events.index(rel), f"{fin} {rel}")
    check("release clock = latest waiting arrival (the finished core's clock is not used)",
          rel is not None and fin is not None and rel[2] < fin[2], f"release {rel}, finish {fin}")


def t_finish_before_barrier(d):
    print("a core finishes before the others reach a barrier")
    r = Run(d, [hits(1, 3, 0x10000000), misses(1, 5, 0x20000000) + ["BARRIER:1"] + misses(5, 8, 0x20000000)])
    common_checks(r, 2, 1)
    check("core 1 passes its barrier", r.cores.get(1, {}).get("barriers") == 1, str(r.cores))


def t_marker_forms(d):
    print("BARRIER without id, as first and last line, and back to back")
    a = ["BARRIER"] + hits(1, 3, 0x10000000) + ["BARRIER", "BARRIER"] + hits(3, 5, 0x10000000) + ["BARRIER"]
    b = ["BARRIER"] + misses(1, 3, 0x20000000) + ["BARRIER", "BARRIER"] + misses(3, 5, 0x20000000) + ["BARRIER"]
    r = Run(d, [a, b])
    common_checks(r, 2, 4)
    ids = [e[2] for e in r.events if e[0] == "arrive" and e[1] == 0]
    check("automatic ids count 1, 2, 3, 4", ids == [1, 2, 3, 4], str(ids))


def t_iters(d):
    print("--iters 2: barrier ids repeat each pass")
    a = hits(1, 3, 0x10000000) + ["BARRIER:1"] + hits(3, 5, 0x10000000)
    b = misses(1, 3, 0x20000000) + ["BARRIER:1"] + misses(3, 5, 0x20000000)
    r = Run(d, [a, b], ["--iters", "2"])
    common_checks(r, 2, 2)
    check("each core passed 2 barriers and counted 8 instructions",
          all(r.cores.get(c, {}).get("barriers") == 2 and r.cores.get(c, {}).get("inst") == 8 for c in (0, 1)),
          str(r.cores))


def t_addresses_format(d):
    print("addresses trace format with barriers")
    a = hits(1, 4, 0x10000000, "addresses") + ["BARRIER:1"] + hits(4, 6, 0x10000000, "addresses")
    b = misses(1, 4, 0x20000000, "addresses") + ["BARRIER:1"] + misses(4, 6, 0x20000000, "addresses")
    r = Run(d, [a, b], fmt="addresses")
    common_checks(r, 2, 1)
    check("5 accesses per core", all(r.cores.get(c, {}).get("acc") == 5 for c in (0, 1)), str(r.cores))


def t_single_core(d):
    print("1 core: barrier lines change nothing")
    body = [acc(i, 0x10000000 + (i % 7) * 0x40) for i in range(1, 30)]
    with_b = body[:10] + ["BARRIER:1"] + body[10:20] + ["BARRIER:2"] + body[20:]
    strip = lambda s: [l for l in s.splitlines() if "Trace File" not in l and not l.startswith("SCHED")]
    a, b = Run(d, [body], cores=1), Run(d, [with_b], cores=1)
    check("identical statistics", a.rc == 0 and b.rc == 0 and strip(a.out) == strip(b.out))


def t_mismatch(d):
    print("mismatched barrier ids stop the run")
    r = Run(d, [hits(1, 2, 0x10000000) + ["BARRIER:1"] + hits(2, 3, 0x10000000),
                hits(1, 2, 0x20000000) + ["BARRIER:2"] + hits(2, 3, 0x20000000)])
    check("exit 1 with a barrier error", r.rc == 1 and "different barriers" in r.err, r.err[-300:])


def t_determinism(d):
    print("determinism: the same run twice")
    tr = [misses(1, 20, 0x10000000) + ["BARRIER:1"] + hits(20, 40, 0x18000000),
          hits(1, 30, 0x20000000) + ["BARRIER:1"] + misses(30, 35, 0x20000000)]
    a, b = Run(d, tr, ["--check-invariants", "5"]), Run(d, tr, ["--check-invariants", "5"])
    strip = lambda s: [l for l in s.splitlines() if "Trace File" not in l and "runs " not in l]
    check("identical output", a.rc == 0 and strip(a.out) == strip(b.out))


def t_invariants(d):
    print("invariants: separate programs vs one program without coherence")
    line = 0x10000000
    tr = [[acc(1, line), acc(2, line + 8)] + misses(3, 10, 0x30000000),
          [acc(1, line + 16)] + misses(2, 12, 0x50000000)]
    sep = Run(d, tr, ["--check-invariants", "1"])
    shared = Run(d, tr, ["--check-invariants", "1", "--core-apps", "0", "0"])
    zero = lambda inv: all(int(x) == 0 for x in re.findall(r"(?:cache|LLC|core|levels) (\d+)", inv))
    check("separate programs: all invariant counts 0", sep.rc == 0 and sep.inv != "" and zero(sep.inv), sep.inv)
    m = re.search(r"private levels (\d+)", shared.inv)
    check("one program: the shared line shows up in both cores' private caches",
          shared.rc == 0 and m is not None and int(m[1]) > 0, shared.inv)
    check("one program: never a word twice in one cache", "one cache 0 " in shared.inv, shared.inv)


def t_single_level_build(d):
    print("single-level build (bin/cachesim): no timing model, barriers release at clock 0")
    tr = [hits(1, 4, 0x10000000) + ["BARRIER:1"] + hits(4, 6, 0x10000000),
          misses(1, 4, 0x20000000) + ["BARRIER:1"] + misses(4, 6, 0x20000000)]
    r = Run(d, tr, binary=BIN_SINGLE, config="default")
    common_checks(r, 2, 1)


def t_event_times(d):
    print("packet event times: per-level lookup times, data-back time = core clock")
    tiny = [acc(1, 0x10000000), acc(2, 0x10000000), acc(3, 0x10000040), acc(4, 0x10000000)]
    cmd = [str(BIN), "--config", str(REPO / "configs/three_level.yaml"), "--trace-format", "instructions",
           "--cache-block-size", "64", "--replacement-policy", "srrip", "--debug-timing"]
    path = d / "tiny_time.txt"
    path.write_text("\n".join(tiny) + "\n")
    out = subprocess.run(cmd + ["--trace", str(path)], capture_output=True, text=True, cwd=REPO)
    got = [l for l in out.stdout.splitlines() if l.startswith("TIME")]
    # miss: L1D t, L2 t+1, LLC t+1+5, memory t+1+5+9, back t+15+121; L1D hit: back t+1
    want = ["TIME 0x10000000 L1D @0", "TIME 0x10000000 L2 @1", "TIME 0x10000000 LLC @6",
            "TIME 0x10000000 memory @15", "TIME 0x10000000 done @136",
            "TIME 0x10000000 L1D @136", "TIME 0x10000000 done @137",
            "TIME 0x10000040 L1D @137", "TIME 0x10000040 L2 @138", "TIME 0x10000040 LLC @143",
            "TIME 0x10000040 memory @152", "TIME 0x10000040 done @273",
            "TIME 0x10000000 L1D @273", "TIME 0x10000000 done @274"]
    check("TIME lines follow the per-level charges", got == want, "\n".join(got))
    check("no completion-time mismatch reported", "mismatch" not in out.stderr, out.stderr[-300:])
    tr = [misses(1, 20, 0x10000000) + ["BARRIER:1"] + hits(20, 40, 0x18000000),
          hits(1, 30, 0x20000000) + ["BARRIER:1"] + misses(30, 35, 0x20000000)]
    r = Run(d, tr, config="three_level")
    check("2 cores with a barrier: no completion-time mismatch", r.rc == 0 and "mismatch" not in r.err, r.err[-300:])


def t_min_clock(d):
    print("min_clock: LLC arrivals in time order with 4 active cores")
    a = hits(1, 4, 0x10000000) + ["BARRIER:1"] + hits(4, 9, 0x10000000) + ["BARRIER:2"] + hits(9, 10, 0x10000000)
    b = misses(1, 6, 0x20000000) + ["BARRIER:1"] + misses(6, 7, 0x20000000) + ["BARRIER:2"] + \
        misses(7, 9, 0x20000000)
    c = misses(1, 3, 0x40000000) + hits(3, 6, 0x50000000) + ["BARRIER:1"] + misses(6, 9, 0x40000000) + ["BARRIER:2"]
    r = Run(d, [a, b, c], ["--scheduler", "min_clock"])
    common_checks(r, 3, 2)
    tr = [misses(1, 30, 0x10000000), None, hits(1, 40, 0x30000000),
          misses(1, 10, 0x40000000) + hits(10, 30, 0x48000000), misses(1, 25, 0x60000000)]
    r = Run(d, tr, ["--scheduler", "min_clock"], config="three_level")
    common_checks(r, 5, 0)
    order = next((l for l in r.out.splitlines() if l.startswith("System: LLC arrival order")), "")
    check("4 active cores: LLC demand accesses arrive in time order", "out of time order 0 " in order, order)
    r = Run(d, tr, ["--scheduler", "round_robin_skip"], config="three_level")
    order = next((l for l in r.out.splitlines() if l.startswith("System: LLC arrival order")), "")
    print(f"    (same traces, round_robin_skip: {order[len('System: LLC arrival order: '):]})")


# ------------------------------------------------------------------------------------------------ real traces
def real_run(args, label):
    cmd = [str(BIN), "--config", str(REPO / "configs/three_level.yaml"), "--check-invariants", "4000000", *args]
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    out = p.stdout
    llc = re.search(r"^Cache LLC\n.*\nhits: \d+ miss: (\d+)", out, re.M)
    per_core = sum(int(x) for x in re.findall(r"LLC misses (\d+)", out))
    inv = next((l for l in out.splitlines() if l.startswith("Invariants:")), "")
    print(f"  {label}: " + next((l for l in out.splitlines() if l.startswith("System: cycles")), ""))
    check(f"{label}: completes", p.returncode == 0, p.stderr[-300:])
    check(f"{label}: no completion-time mismatch", "mismatch" not in p.stderr, p.stderr[-300:])
    check(f"{label}: per-core LLC misses sum to the LLC's", llc is not None and int(llc[1]) == per_core,
          f"{llc[1] if llc else None} vs {per_core}")
    zero = all(int(x) == 0 for x in re.findall(r"(?:cache|LLC|core|levels) (\d+)", inv))
    check(f"{label}: invariant counts all 0", inv != "" and zero, inv)


def real_tests():
    t1, t2 = str(REAL / "pr/web-BerkStan.default.xz"), str(REAL / "pr_spmv_clean/web-BerkStan.default.xz")
    print("real traces (minutes)")
    real_run(["--num-cores", "2", "--traces", t1, t2, "--cache-block-size", "8", "--replacement-policy", "phru",
              "--set-dueling", "--dueling-mode", "ztest", "--conf-margin", "1"], "2 programs, PHRU 8B ztest dueling")
    real_run(["--num-cores", "8", "--traces", *([t1, t2] * 4), "--cache-block-size", "64",
              "--replacement-policy", "srrip"], "8 programs, SRRIP 64B")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", action="store_true", help="also run the real-trace smoke tests (minutes)")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for t in (t_two_core_barriers, t_three_core_barriers, t_rule_with_idle_core, t_finish_while_others_wait,
                  t_finish_before_barrier, t_marker_forms, t_iters, t_addresses_format, t_single_core, t_mismatch,
                  t_determinism, t_invariants, t_single_level_build, t_event_times, t_min_clock):
            t(d)
        print("--- again with --scheduler round_robin_skip")
        Run.scheduler = "round_robin_skip"
        for t in (t_two_core_barriers, t_three_core_barriers, t_rule_with_idle_core, t_finish_while_others_wait,
                  t_finish_before_barrier, t_marker_forms, t_iters, t_addresses_format, t_single_level_build):
            t(d)
        Run.scheduler = "min_clock"
    if args.real:
        real_tests()
    print("ALL PASS" if not FAILS else f"FAILURES ({len(FAILS)}): " + "; ".join(FAILS))
    sys.exit(0 if not FAILS else 1)


if __name__ == "__main__":
    main()
