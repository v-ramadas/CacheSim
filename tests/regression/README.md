# CacheSim regression / performance harness

Every invocation runs `make release` first (building both `cachesim`/`cachesim_multi`
release binaries) unless you pass `--no-build`, so the binary under test always
matches the current working tree. A failed build aborts the run before anything
is simulated.

`run_regression.py` has three modes. All of them compile down to
`bin/cachesim_multi` invocations against the 4 standard traces (as-Skitter,
web-BerkStan, sx-stackoverflow, cit-Patents) by default; pass `--full` to use
all 9.

## Modes

**`baseline`** - reruns HRU-8B and PHRU-8B (no set-dueling), exact-match diffs
against `golden_values.json`'s `static_baselines`. `lru_64b` and `srrip_64b`
are pure reference data in that file and are never rerun by this script.

**`dueling`** - reruns HRU-8B and PHRU-8B under both the `psel` and `ztest`
presets (exact flags below), exact-match diffs against `dueling_baselines`.

**`performance`** - reruns HRU-8B, HRUpp-8B, PHRU-8B, PHRUpp-8B (dueling
preset via `--dueling-preset`, default `psel`), and reports MPKI gain % and
speedup (`reference_cycles / candidate_cycles`) against two things per policy:
the universal `lru_64b`/`srrip_64b` anchors, and that policy's own
`performance_history` checkpoint. Not pass/fail - it's a report.

Pass `--update` to any mode to write the newly-measured values into
`golden_values.json` (regression modes overwrite the diffed baseline;
performance mode writes/advances the `performance_history` checkpoint). Do
this only when a difference is a deliberate, understood change - not to make
a failing run "pass."

## Dueling presets (exact flags - keep in sync with the memory file
`feedback_cachesim_dueling_shorthand.md` if this ever changes)

```
psel:  --cache-block-size 8 --set-dueling --dueling-mode psel  --dueling-period 1000000
ztest: --cache-block-size 8 --set-dueling --dueling-mode ztest --conf-margin 1 --conf-epoch 10000
```

## Known caveat: HRUpp/PHRUpp iso-area gap

`HRU` and `PHRU` set `isoArea=true` at 8B (`src/main.cc`'s replacement-policy
switch), giving them the real tag-cost-aware way count (160 ways for our
standard 1024x32 config). `HRUpp` does not set `isoArea`, and `PHRUpp` isn't
in that switch at all - both fall back to the naive, tag-free 256-way scale
unless a future change adds them to the switch or passes
`--iso-area-ratio 0.625` explicitly.

This is why `performance` mode never compares HRU against HRUpp (or PHRU
against PHRUpp) directly - only each policy against the universal
LRU/SRRIP anchors and against its own history. A direct cross-policy
comparison would silently be comparing unequal physical capacities. If that
comparison is ever needed, fix the capacity gap first (either via the switch
statement or `--iso-area-ratio`).

## Parsing convention

Every level (L1D, then LLC) prints its own `MPKI` and `Num Cycles` line
(`src/cachesim.cc`). This script always takes the **second** occurrence of
each (the LLC) - matches the `grep -E "MPKI"` pattern used throughout manual
testing of this codebase.

## Golden file format

```json
{
  "traces": [...],
  "static_baselines": {
    "lru_64b": {"<trace>": {"mpki": ..., "cycles": ...}},
    "srrip_64b": {...},
    "hru_8b_no_dueling": {...},
    "phru_8b_no_dueling": {...}
  },
  "dueling_baselines": {
    "hru_8b_psel": {...}, "hru_8b_ztest": {...},
    "phru_8b_psel": {...}, "phru_8b_ztest": {...}
  },
  "performance_history": {
    "hru_8b_psel": {...}, "hrupp_8b_psel": {...},
    "phru_8b_psel": {...}, "phrupp_8b_psel": {...}
  }
}
```
