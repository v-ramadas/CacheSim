#!/usr/bin/env python3
"""Multicore scheduler and barrier tests on small hand-written traces.

Writes instruction-format traces to a temporary directory, runs bin/cachesim_multi with --debug-schedule and
checks the SCHED log and the per-core statistics:
  1. barriers: a waiting core is not stepped until every core has arrived; on release every waiting core's clock
     becomes the latest arrival, and barrier_wait_cycles = sum of (release clock - own arrival clock);
  2. a single core: barrier lines change nothing (cache statistics identical without them);
  3. a core that finishes its trace counts as arrived (no deadlock);
  4. cores waiting at different barrier ids stop the run with an error;
  5. clock skew is measured after every step over running cores only;
  6. scheduling: a waiting or finished core never runs, and a stepped core is never ahead of a runnable core
     that the rule would have picked (checked as: after every step, the next core stepped is the first core in
     id order after the current one whose clock is not ahead, or the current core if all others are ahead).
Usage: python3 tests/multicore/run_tests.py [--binary bin/cachesim_multi]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def access(n, addr, write=False):
    kind = "Write" if write else "Read"
    return f"instCount:{n},PC:0x401000,{kind}:{addr:#x},NextReuse:0,NodeDegree:0,PCAvgDegree:0.00"


def trace(path, lines):
    path.write_text("\n".join(lines) + "\n")
    return str(path)


def run(binary, args):
    cmd = [str(binary), "--config", str(REPO / "configs/two_level.yaml"), "--trace-format", "instructions",
           "--cache-block-size", "64", "--replacement-policy", "srrip"] + args
    return subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)


def sched_events(out):
    ev = []
    for line in out.splitlines():
        m = re.match(r"SCHED step core (\d+) clock (\d+)", line)
        if m:
            ev.append(("step", int(m[1]), int(m[2])))
            continue
        m = re.match(r"SCHED arrive core (\d+) barrier (\d+) clock (\d+)", line)
        if m:
            ev.append(("arrive", int(m[1]), int(m[2]), int(m[3])))
            continue
        m = re.match(r"SCHED release barrier (\d+) clock (\d+)", line)
        if m:
            ev.append(("release", int(m[1]), int(m[2])))
    return ev


def core_stats(out):
    st = {}
    for m in re.finditer(r"^Core (\d+): instructions (\d+) cycles (\d+) .*?barriers (\d+) barrier_wait_cycles (\d+)",
                         out, re.M):
        st[int(m[1])] = dict(inst=int(m[2]), cycles=int(m[3]), barriers=int(m[4]), wait=int(m[5]))
    return st


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f": {detail}" if detail and not cond else ""))
    return cond


def test_barriers(binary, d):
    print("barriers: fast core (L1 hits) and slow core (misses), two barriers")
    fast = [access(i, 0x10000000) for i in range(1, 5)] + ["BARRIER:1"] + \
           [access(i, 0x10000000) for i in range(5, 7)] + ["BARRIER:2"] + [access(7, 0x10000000)]
    slow = [access(i, 0x20000000 + i * 0x100000) for i in range(1, 5)] + ["BARRIER:1"] + \
           [access(i, 0x20000000 + i * 0x100000) for i in range(5, 7)] + ["BARRIER:2"] + \
           [access(7, 0x20000000 + 7 * 0x100000)]
    r = run(binary, ["--num-cores", "2", "--traces", trace(d / "fast.txt", fast), trace(d / "slow.txt", slow),
                     "--debug-schedule"])
    ok = check("run succeeds", r.returncode == 0, r.stderr[-500:])
    ev = sched_events(r.stdout)
    st = core_stats(r.stdout)
    sysline = next((l for l in r.stdout.splitlines() if l.startswith("System: cycles")), "")
    ok &= check("2 barriers released", sysline.endswith("barriers 2"), sysline)
    # Largest gap between running cores: after the first step core 0 is at 131 (a miss) while core 1 is still at 0;
    # later gaps are 130 (core 1 at 262 vs core 0 at 132). Core 0's 390 cycles at barrier 1 are waiting, not skew.
    ok &= check("max clock skew 131 (running cores only)", "max clock skew 131 cycles" in sysline, sysline)
    waiting, arrive_clock, expected_wait = set(), {}, {0: 0, 1: 0}
    clock = {0: 0, 1: 0}
    stepped_while_waiting = False
    release_ok = True
    for k, e in enumerate(ev):
        if e[0] == "step":
            # The step that reads the BARRIER line logs its arrival first, then itself: not a step while waiting.
            own_arrival = k > 0 and ev[k - 1][0] == "arrive" and ev[k - 1][1] == e[1]
            if e[1] in waiting and not own_arrival:
                stepped_while_waiting = True
            clock[e[1]] = e[2]
        elif e[0] == "arrive":
            waiting.add(e[1])
            arrive_clock[e[1]] = e[3]
        elif e[0] == "release":
            release_ok &= waiting == {0, 1} and e[2] == max(arrive_clock.values())
            for c in waiting:
                expected_wait[c] += e[2] - arrive_clock[c]
                clock[c] = e[2]
            waiting.clear()
    ok &= check("no core is stepped while waiting", not stepped_while_waiting)
    ok &= check("release only when both arrived, at the latest arrival clock", release_ok)
    ok &= check("barrier_wait_cycles = sum(release - arrival)",
                all(st.get(c, {}).get("wait") == expected_wait[c] for c in (0, 1)), f"{st} vs {expected_wait}")
    ok &= check("each core passed 2 barriers", all(st.get(c, {}).get("barriers") == 2 for c in (0, 1)), str(st))
    ok &= check("fast core waited, slow core did not", expected_wait[0] > 0 and expected_wait[1] == 0,
                str(expected_wait))
    return ok


def test_rule(binary, d):
    print("scheduling rule: next core = first after current (id order) whose clock is not ahead")
    traces = []
    for c in range(3):
        lines = [access(i, 0x10000000 * (c + 1) + (i * 0x40000 if c else 0)) for i in range(1, 40)]
        traces.append(trace(d / f"rule{c}.txt", lines))
    r = run(binary, ["--num-cores", "3", "--traces", *traces, "--debug-schedule"])
    ok = check("run succeeds", r.returncode == 0, r.stderr[-500:])
    steps = [e for e in sched_events(r.stdout) if e[0] == "step"]
    total = {c: 40 for c in range(3)}   # steps per core: 39 records + the step that finds the end of the trace
    done = {c: 0 for c in range(3)}
    clock = {c: 0 for c in range(3)}
    good = True
    for k, e in enumerate(steps[:-1]):
        cur = e[1]
        clock[cur] = e[2]
        done[cur] += 1
        runnable = [c for c in range(3) if done[c] < total[c]]
        nxt = steps[k + 1][1]
        expect = None
        for j in range(1, 3):
            cand = (cur + j) % 3
            if cand in runnable and clock[cand] <= clock[cur]:
                expect = cand
                break
        if expect is None:
            expect = cur if cur in runnable else min(runnable, key=lambda c: (clock[c], c))
        if nxt != expect:
            good = False
            print(f"    step {k}: core {cur} clock {clock[cur]}, next {nxt}, expected {expect}, clocks {clock}")
            break
    ok &= check("every switch follows the rule", good)
    return ok


def test_single_core(binary, d):
    print("single core: barrier lines change nothing")
    body = [access(i, 0x10000000 + (i % 7) * 0x40) for i in range(1, 30)]
    with_b = body[:10] + ["BARRIER:1"] + body[10:20] + ["BARRIER:2"] + body[20:]
    a = run(binary, ["--trace", trace(d / "plain.txt", body)])
    b = run(binary, ["--trace", trace(d / "barriers.txt", with_b)])
    strip = lambda s: [l for l in s.splitlines() if "Trace File" not in l]
    return check("identical statistics", a.returncode == 0 and strip(a.stdout) == strip(b.stdout))


def test_finished_counts_as_arrived(binary, d):
    print("finished core counts as arrived")
    short = [access(i, 0x10000000) for i in range(1, 3)]
    long_ = [access(i, 0x20000000) for i in range(1, 5)] + ["BARRIER:1"] + [access(i, 0x20000000) for i in range(5, 8)]
    r = run(binary, ["--num-cores", "2", "--traces", trace(d / "short.txt", short), trace(d / "long.txt", long_)])
    st = core_stats(r.stdout)
    return check("completes, core 1 passes its barrier", r.returncode == 0 and st.get(1, {}).get("barriers") == 1,
                 r.stderr[-300:] + str(st))


def test_mismatch(binary, d):
    print("mismatched barrier ids stop the run")
    a = [access(1, 0x10000000), "BARRIER:1", access(2, 0x10000000)]
    b = [access(1, 0x20000000), "BARRIER:2", access(2, 0x20000000)]
    r = run(binary, ["--num-cores", "2", "--traces", trace(d / "a.txt", a), trace(d / "b.txt", b)])
    return check("exit 1 with a barrier error", r.returncode == 1 and "different barriers" in r.stderr, r.stderr[-300:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(REPO / "bin/cachesim_multi"))
    args = ap.parse_args()
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for t in (test_barriers, test_rule, test_single_core, test_finished_counts_as_arrived, test_mismatch):
            ok &= t(args.binary, d)
    print("ALL PASS" if ok else "FAILURES")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
