# PSEL live-threshold mode vs ZTEST, current working-tree state (2026-09-21)

## What was actually compared

The working tree (uncommitted) currently has `should_merge()` hardcoded to
`return false;` unconditionally (`inc/cachesim.h`) — this applies to **every**
`DuelingMode`, not just PSEL, so this comparison is breakdown-decision-only;
neither mode ever merges back in this state. It also has a rewritten PSEL
branch of `update_dueling_confidence()` (`src/cachesim.cc`): the old
quiet-epoch-streak mechanism (N consecutive settled `CONF_EPOCH`s before
locking) is gone. It's replaced with a simple live check gated only by a
`DUELING_PERIOD` warm-up: once `instCount >= DUELING_PERIOD`,
`duel_locked = (PSEL < PSEL_THRESHOLD)` is recomputed on *every* access (not
truly "decide once and freeze" despite the intent) — but since merge is
disabled globally, the first time it evaluates true still permanently
converts the set via `breakdown()`, so in practice it behaves as one-shot.

## Command (matches the existing `hru_graduated_hub_priority.md` convention,
just with the current PSEL_MAX/THRESHOLD defaults from `main.cc`)

```
./bin/cachesim_multi --trace <trace>.random.log --trace-format instructions \
  --config configs/default.cfg --num-cache-sets 1024 --num-cache-ways 32 \
  --cache-block-size 8 --num-duels 64 --psel-max 4096 --psel-threshold 2048 \
  --replacement-policy hru --dueling-period 1000000 --conf-epoch 10000 \
  --conf-max 1 --set-dueling --dueling-mode {ztest,psel}
```

Traces: `as-Skitter`, `cit-Patents`, `web-BerkStan` (known true-negative),
`sx-stackoverflow` — the same 4-trace set used in prior set-dueling sessions.

## Results (LLC/whole-cache final MPKI)

| Trace | ztest MPKI | psel MPKI | Δ (psel−ztest) | Broke down? (ztest / psel) |
|---|---|---|---|---|
| as-Skitter | 37.221455 | 37.220867 | −0.0006 | yes @ 27.98M / yes @ 34.36M |
| cit-Patents | 53.187256 | 53.255821 | +0.0686 | yes @ 36.38M / yes @ 49.50M |
| web-BerkStan | 17.991753 | 17.991753 | 0.0000 | no / no |
| sx-stackoverflow | 26.212118 | 26.195749 | −0.0164 | yes @ 16.61M / yes @ 14.87M |

No `Merging at` lines appear in any of the 8 logs (expected — merge is
hardcoded off).

## Takeaway

On this 4-trace set, the new live-PSEL mechanism lands within ~0.07 MPKI
(≤0.3% relative) of ztest on every trace — essentially a wash. More
notably, it correctly stays at 64B on `web-BerkStan`, matching ztest's
correct true-negative behavior. That's a meaningful difference from the
*old* PSEL quiet-epoch-streak mechanism, which (per `set_dueling` skill
notes) false-positived on `web-BerkStan` in 18/18 previously-tested
`conf_max`/threshold combinations. This one hasn't reproduced that failure
mode, at least at `psel-max=4096`/`psel-threshold=2048`.

Breakdown timing differs (psel fires later on 2 of 3 true positives, earlier
on the third), but with traces this long (270-470M dynamic instructions) the
timing difference washes out in total MPKI. It could matter more on shorter
runs or if merge-back is ever re-enabled.

**Not tested here**: `web-Google` and `wiki-topcats` (the other two
established true-negative traces) — worth adding before trusting this beyond
web-BerkStan, since that's exactly where the old PSEL mechanism's false
positives were least consistent (16/18, not 18/18).

Raw logs: `CacheSim/sweep_psel_oneshot_vs_ztest/*.log`, driver timing in
`driver.log` in the same directory.
