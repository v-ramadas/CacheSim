# HRU graduated hub priority — experiment log

Status as of 2026-09-17: **reverted**. `inc/hru_replacement_policy.h` and
`src/hru_replacement_policy.cc` are back to the binary hub/non-hub scheme
(git commit `90f362f`). This file records four attempts at making hub
priority graduated (scale with degree) instead of binary, why each one
failed or partially worked, and what's left to try.

## The idea

HRU protects "hub" lines (graph nodes with `degree > avg_degree`) by
promoting them to a high priority value on hit/fill, versus plain-LRU
treatment for non-hub lines. That split is binary: a degree-100,000
supernode and a degree-51 node that both barely clear `avg_degree` get
identical treatment. The goal was to make the boost scale with *how much*
of a hub a line is.

## The mechanism you have to respect

`HRU::init_counter()` does:
```cpp
lru_counter++;
mru_counter += num_ways;
```
every access. So after `N` accesses, `mru_counter == num_ways * lru_counter`
**exactly** — it is not a fixed offset above `lru_counter`, it is a
*multiplicatively scaled version of the same ever-growing clock*. A hub's
protection isn't "survive one extra LRU cycle" — once touched, its
snapshotted `mru_counter` value stays far ahead of any future non-hub
`lru_counter` snapshot for a long time (roughly proportional to how late in
the run it was touched), because the gap between the two clocks only ever
widens. This is easy to miss and it is the reason attempts 1–3 below failed.

## Attempts and results

Baseline (binary hub/non-hub, current committed HRU), LLC MPKI, 1024×32-way,
`--dueling-mode ztest --conf-epoch 10000 --conf-max 1 --psel-threshold 16`:

| Trace | 64B | 8B+dueling |
|---|---|---|
| as-Skitter | 37.50 | 37.22 |
| web-BerkStan | 17.64 | 17.99 |
| sx-stackoverflow | 33.28 | 26.21 |
| cit-Patents | (not run) | (not run) |

### Attempt 1 — raw ratio, additive
```cpp
counter[way] = lru_counter + min(degree/avg_degree, 4.0) * num_ways;
```
Regressed everywhere (+0.5 to +2.2 MPKI). Root cause understood only in
hindsight (see attempt 4): adding a small bounded term to `lru_counter`
is negligible next to the real `mru_counter` scale, so this just weakens
hub protection versus the original scheme while looking superficially
"graduated."

At the time, the regression was misattributed to `degree/avg_degree`
saturating near-instantly because `avg_degree` is tiny (~4-6) on these
graphs, making nearly every hub hit the ratio cap and collapse back
toward a blunter version of the binary scheme.

### Attempt 2 — log-scale ratio, additive
```cpp
counter[way] = lru_counter + min(log2(degree+1)/log2(avg_degree+1), 4.0) * num_ways;
```
Worse than attempt 1 (+1.5 to +7.4 MPKI vs baseline). Compressing the ratio
into log-space was meant to fix attempt 1's saturation, but since the whole
additive approach was the actual problem, this just produced smaller boost
magnitudes on average — under-protecting hubs even more than attempt 1.

### Attempt 3 — max-normalized ratio, additive
```cpp
// max_degree_seen tracked as a running max, updated in init_counter()
counter[way] = lru_counter + (degree/max_degree_seen) * num_ways;
```
Worst of the three additive attempts (+3.0 to +16.6 MPKI vs baseline).
Deliberately tested the "opposite direction" (ratio bounded to `[0,1]`
instead of `>= 1`), on the hypothesis that attempts 1-2 were over-boosting.
It made things worse, which is what falsified that hypothesis and pointed
at the additive-vs-multiplicative distinction instead: `degree/max_degree_seen`
is usually far below 1 for most hub-classified nodes, giving *even less*
protection than attempts 1-2, confirming boost magnitude (not direction)
was the axis that mattered, and that all three were on the wrong side of
"enough."

### Attempt 4 — raw ratio, multiplicative (the fix)
```cpp
counter[way] = lru_counter * min(degree/avg_degree, 4.0) * num_ways;
// i.e. hub_boost() returns the multiplier, applied via `*` not `+`
```
This preserves the real `mru_counter == num_ways * lru_counter` relationship
and just lets the multiplier scale with degree instead of being pinned at
`num_ways` for every hub. Results:

| Trace | Cfg | Before | Attempt 4 | Δ |
|---|---|---|---|---|
| as-Skitter | 64B | 37.50 | 36.60 | **-0.90** |
| as-Skitter | 8B+duel | 37.22 | 38.08 | +0.86 |
| web-BerkStan | 64B | 17.64 | 16.95 | **-0.69** |
| web-BerkStan | 8B+duel | 17.99 | 17.44 | **-0.55** |
| sx-stackoverflow | 64B | 33.28 | 32.89 | **-0.39** |
| sx-stackoverflow | 8B+duel | 26.21 | 34.58 | **+8.37** |
| cit-Patents | 64B | n/a | 52.87 | — |
| cit-Patents | 8B+duel | n/a | 53.71 | — |

**At plain 64B (no set-dueling), this is a consistent, real win** on all
three traces tested — the only attempt of four that beat baseline anywhere.

**Combined with 8B set-dueling it's a mixed bag**, and on sx-stackoverflow —
HRU's best dueling win by far under the binary scheme (33.28 -> 26.21, a
7-point drop from breakdown alone) — it's a severe regression that more
than erases that win.

## Leading hypothesis for the dueling regression (untested)

`HRU::set_breakdown()` and `HRU::set_merge()` both do `lru_counter =
mru_counter;` to fast-forward the slow clock at a breakdown/merge event, so
newly-resized ways don't look artificially ancient next to existing
mru_counter-scale hub values. That rebase was calibrated for a world where
`mru_counter` was only ever used as a *snapshot value* assigned directly to
`counter[way]`. Now that hub priority is `lru_counter * multiplier`,
jumping `lru_counter` itself up to `mru_counter`'s scale means every
subsequent hub assignment in that set becomes enormous (`mru_counter_scale
* multiplier`, not just `mru_counter_scale`), likely swamping the set and
explaining why sx-stackoverflow - the trace where breakdown/merge actually
fires a lot under ztest - is the one that blows up.

**Next step if you pick this back up:** try decoupling the rebase from the
multiplicative scheme, e.g. rebase to `mru_counter / num_ways` (an estimate
of what plain `lru_counter` "should" be at that point) instead of
`mru_counter` directly, or track a separate rebase reference that doesn't
get multiplied. Re-run the same 4-trace comparison (as-Skitter,
web-BerkStan, sx-stackoverflow, cit-Patents; 64B baseline +
`--cache-block-size 8 --set-dueling --dueling-mode ztest --conf-epoch 10000
--conf-max 1 --psel-threshold 16 --num-duels 64 --psel-max 64`) against the
attempt-4 numbers above.

## Traces and command used

```
TRACE_DIR=/nobackup.1/vishnu/adaptive-cache/pin/pin/traces/hubs/pr_spmv
./bin/cachesim_multi --trace $TRACE_DIR/<trace>.random.log --trace-format instructions \
  --config configs/default.cfg --num-cache-sets 1024 --num-cache-ways 32 \
  --cache-block-size 64 --replacement-policy hru                    # 64B baseline

./bin/cachesim_multi --trace $TRACE_DIR/<trace>.random.log --trace-format instructions \
  --config configs/default.cfg --num-cache-sets 1024 --num-cache-ways 32 \
  --cache-block-size 8 --num-duels 64 --psel-max 64 --psel-threshold 16 \
  --replacement-policy hru --dueling-period 1000000 --conf-epoch 10000 \
  --conf-max 1 --set-dueling --dueling-mode ztest                   # 8B + dueling
```
