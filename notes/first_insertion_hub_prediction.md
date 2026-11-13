# First-insertion hub prediction for PHRU (2026-09-30)

Setup for every number here: `cachesim_multi`, LLC 1024 sets x 32 ways
(64B-equivalent) at 8B blocks, no set dueling, pr_spmv `.random` traces,
4 standard graphs. "MPKI" is always the LLC MPKI (second MPKI line).

## 0. Background

- Two-level exclusive hierarchy: a line lives in L1D *or* the LLC.
- **HRU** pins "hub" blocks (degree > average, read from the trace - oracle
  information) at MRU. **PHRU** tries to do the same with runtime signals only,
  mainly `serviced_from_llc` (times a line/block came back to L1 from the LLC).
- Both only decide for lines that have already been reused
  (`serviced_from_llc > 0`). A line entering the LLC for the first time goes
  in at LRU.
- Goal: close the PHRU->HRU MPKI gap with runtime information only.
- PHRU logs a confusion matrix against the true degree: TP = pinned a true hub,
  FP = pinned a non-hub, FN = missed a true hub, TN = correctly left unpinned.

## 1. Demote not-accessed hub pins - no effect, removed

Change: a hub-classified block of a reused line that was never accessed in L1
got stamp `mru_counter - K*num_ways` instead of `mru_counter` (K = 1, 4, 16; PHRU
and PHRUpp). Largest MPKI change 0.04% (web-BerkStan PHRU 29.098 -> 29.086);
precision/recall moved in the 4th decimal.

Why:
1. The hub decision is made before the stamp, so TP/FP counts can't change -
   only how long a pinned block stays.
2. `mru_counter` grows by `num_ways` per set access, so `mru - num_ways` is the
   previous access's stamp: one position lower among pinned lines.
3. `was_accessed` does not separate hubs from non-hubs among pins (HUBSTAT 6):

| | as-Skitter | web-BerkStan | sx-stackoverflow | cit-Patents |
|---|---|---|---|---|
| accessed pins that are true hubs | 71% | 72% | 80% | 77% |
| not-accessed pins that are true hubs | 74% | 72% | 85% | 80% |

Correction: `phru_hub_heuristic_quality.md`'s precision of 0.87-0.98 is at 64B.
At 8B PHRU precision is 0.62-0.83, so FPs matter as well as missed hubs.

## 2. Where PHRU loses

| Trace | All hub fills | First-insertion hubs (never-reused line) | Missed hubs on reused lines | FP pins |
|---|---|---|---|---|
| as-Skitter | 33.2M | 1.64M (4.9%) | 3.24M (9.7%) | 10.6M |
| web-BerkStan | 20.7M | 0.15M (0.7%) | 1.08M (5.2%) | 7.7M |
| sx-stackoverflow | 49.6M | 2.16M (4.4%) | 2.53M (5.1%) | 9.0M |
| cit-Patents | 18.4M | 4.83M (26.3%) | 3.63M (19.8%) | 2.6M |

HRU also inserts first-time lines at LRU, so first-insertion hubs are missed by
both policies. FP pins outnumber all missed hubs on 3 of 4 traces.

## 3. Oracle ceilings (diagnostic, true degree)

`--phru-oracle`: `fix_fn` / `fix_fp` / `both` correct missed hubs / FP pins /
both on reused lines. `first` (new) pins exactly the true hubs of never-reused
lines. `all` (new) = `both` + `first`.

| Trace | PHRU | HRU | fix_fn | fix_fp | both | first | all |
|---|---|---|---|---|---|---|---|
| as-Skitter | 38.85 | 35.81 | 37.13 | 35.44 | 35.81 | 36.84 | 35.05 |
| web-BerkStan | 29.10 | 23.35 | 25.21 | 25.82 | 23.35 | 28.50 | 22.89 |
| sx-stackoverflow | 25.17 | 23.11 | 24.37 | 23.16 | 23.11 | 23.23 | 22.40 |
| cit-Patents | 53.93 | 52.83 | 53.20 | 52.55 | 52.83 | 52.17 | 52.24 |

- `both` is bit-for-bit HRU: the whole PHRU->HRU gap is classification on
  reused lines; victim selection contributes nothing.
- `first` is a separate lever beyond HRU: closes 66% (as-Skitter) / 10%
  (web-BerkStan) / 94% (sx-stackoverflow) of the gap, and beats HRU on
  cit-Patents. `all` beats HRU on every trace.
- Tuned ghost cache (`--ghost-cache-min-serviced 4`, 8B, from
  `ghost_cache_tuning.md`) recovers 0.47 / 0.60 / 0.15 / 0.10 MPKI of `first`'s
  2.01 / 0.60 / 1.95 / 1.76 headroom.
- Concurrent runs on the same `.xz` trace reproduced isolated runs bit for bit
  (the old same-file concurrency bug in `ghost_cache_tuning.md` does not affect
  `.xz` traces).

## 4. Constraints from earlier attempts

- Bloom filter of addresses "evicted after reuse", never forgets: net-negative
  at every size to 64Mi bits. The predictor must forget (recency window).
- Thrash counter that decays a slot on any unrelated miss: too contended to
  reach threshold 2. Age globally and periodically.
- Ghost cache works because it is recency-bounded, but stores full tags: the
  tuned 2048-line config is 16K 8B tags, ~100KB.

## 5. Design: tagless windowed re-miss bitmap (`inc/hub_bitmap.h`)

A first-inserted hub has usually missed to memory before, in an earlier
residency. "This block missed to memory again recently" is the ghost-cache
condition with a longer horizon, in bits instead of tags.

- Train: requested 8B block of every access that misses both levels.
- Query: at that miss, all 8 blocks of the line are probed *before* the miss is
  recorded. Hardware: a per-sector bit set at L1 install and carried to the
  LLC fill; the simulator keeps positive blocks in a side set.
- `bits` mode: two N-bit generations, hub = `cur | prev`; every W events
  `prev = cur`, `cur` cleared. `counter` mode: 2-bit counters halved every W
  events, hub = counter >= T.
- Measurement only; MPKI identical to PHRU in every run.

## 6. Step 1: required accuracy (noisy `first` oracle)

Pin a true hub with probability r (recall), a non-hub with probability q (FP
rate); % of `first`'s MPKI gain kept (negative = worse than PHRU). Cell = r = 1 / 0.5 / 0.25.

| Trace | FP 0 | 1% | 2% | 5% |
|---|---|---|---|---|
| as-Skitter | 100 / 76 / 52% | 71 / 47 / 20% | 46 / 20 / -10% | -17 / -52 / -86% |
| web-BerkStan | 100 / 57 / 30% | 26 / -18 / -46% | -58 / -102 / -127% | -320 / -363 / -400% |
| sx-stackoverflow | 100 / 91 / 76% | 54 / 43 / 22% | 19 / 3 / -22% | -69 / -103 / -143% |
| cit-Patents | 100 / 120 / 100% | 72 / 80 / 55% | 47 / 42 / 13% | -20 / -58 / -98% |

Recall is cheap; FP rate is expensive. Above ~1-2% FP (under 1% on
web-BerkStan) the result is worse than PHRU. Target: FP <= ~1%.

## 7. Step 2: bitmap quality, ungated

Scored on every 8B block entering the LLC on a never-reused line. Recall is
over accessed blocks; FP rate = flagged non-hubs / all non-hub fills (same
denominator as step 1's q); precision = flagged blocks that are hubs.
Never-accessed blocks of never-reused lines are essentially never true hubs
(2.4K of 26M on as-Skitter; likely a labelling artifact - the trace only
carries degree for accessed addresses), so predictions only cover accessed
blocks.

Window 0.5x the LLC's 8B-block capacity (81,920 misses). Cell = recall / FP rate / precision.

| Size | as-Skitter | web-BerkStan | sx-stackoverflow | cit-Patents |
|---|---|---|---|---|
| 32KB | 0.68 / 21.6% / 0.11 | 0.65 / 36.5% / 0.04 | 0.71 / 24.2% / 0.10 | 0.63 / 13.8% / 0.23 |
| 128KB | 0.42 / 7.6% / 0.18 | 0.50 / 12.2% / 0.08 | 0.48 / 8.4% / 0.18 | 0.30 / 4.9% / 0.29 |
| 2MB (control) | 0.30 / 1.05% / 0.53 | 0.44 / 1.4% / 0.40 | 0.37 / 0.9% / 0.61 | 0.14 / 0.56% / 0.63 |
| hub base rate | 0.09 | 0.03 | 0.08 | 0.22 |

- Hash collisions dominate at practical sizes (FP ~ 2x fraction of bits set).
  At 32KB precision is barely above the base rate.
- The signal is real: at 2MB precision is 3-14x the base rate at ~1% FP, but
  that is as much storage as the LLC.
- Longer windows raise recall and FP together; counter T=2 cuts FP only by
  cutting recall; training on LLC hits too was slightly worse.
- One-hash storage at 1% collision FP costs ~100 bits per tracked miss - worse
  than tags.

## 8. PC gating: only one PC reuses hub data

`inc/pc_reuse.h`: per-PC reuse rate = LLC hits / LLC lookups, trained on every
L1D miss; a reuse PC has rate >= theta.

| Trace | Hub PC `...7be` rate | Streaming PCs (`...7b8`, `...790`, `...7ef`) | Hubs from other PCs |
|---|---|---|---|
| as-Skitter | 0.53 | <= 0.002 | 0 |
| web-BerkStan | 0.67 | <= 0.006 | 26 fills |
| sx-stackoverflow | 0.74 | <= 0.002 | 43 fills |
| cit-Patents | 0.22 | 0.000 | 0 |

The streaming PCs produce 79-98% of accessed never-reused non-hub fills.
cit-Patents' hub PC is only 0.22, so theta >= 0.2 excludes it; theta 0.02-0.05
is safe, and a relative threshold would be more robust.

Gated oracle (`--phru-oracle first --pc-reuse-gate theta`): `first`, but only
for blocks from reuse PCs.

| Trace | PHRU | first | gated theta=0.02 | gated theta=0.1 |
|---|---|---|---|---|
| as-Skitter | 38.851 | 36.844 | 36.848 | 36.864 |
| web-BerkStan | 29.098 | 28.503 | 28.503 | 28.512 |
| sx-stackoverflow | 25.174 | 23.226 | 23.229 | 23.236 |
| cit-Patents | 53.928 | 52.166 | 52.172 | 52.311 |

The gate is lossless and removes most non-hub candidates: within the hub PC,
34-61% of fills are hubs instead of 3-22%.

PC-gated bitmap (theta 0.1, window 0.5x). Cell = recall / FP rate / precision.

| Size | as-Skitter | web-BerkStan | sx-stackoverflow | cit-Patents |
|---|---|---|---|---|
| 8KB | 0.95 / 4.8% / 0.44 | 0.82 / 2.0% / 0.46 | 0.96 / 4.1% / 0.47 | 0.93 / 4.0% / 0.61 |
| 32KB | 0.68 / 3.1% / 0.47 | 0.63 / 1.4% / 0.49 | 0.72 / 2.6% / 0.52 | 0.61 / 2.5% / 0.62 |
| 128KB | 0.44 / 1.5% / 0.54 | 0.51 / 0.9% / 0.54 | 0.53 / 1.1% / 0.64 | 0.30 / 1.0% / 0.66 |

- FP falls 4-8x at every size; precision rises to 0.43-0.66.
- 8KB is ~90-100% full, i.e. effectively "pin every accessed first-insertion
  block from the hub PC" - no bitmap needed for that rule.
- Only 128KB reaches ~1% FP on all four traces (recall 0.30-0.53).

## 9. Status and next steps

Current design: per-PC reuse filter + optional windowed single-hash re-miss
bitmap, applied to accessed blocks on first insertion. (The old
never-forgetting Bloom filter is dead; today's bitmap is effectively a one-hash
generational Bloom filter; a multi-hash generational Bloom filter is proposed,
not built.)

Caveats: step 1 spreads FPs over all non-hubs, while gated FPs are accessed
hub-PC blocks (probably more valuable), so the FP-tolerance mapping is likely
pessimistic. The hub PC's rate is measured under the running policy. All
results: 8B, no dueling, 1024x32, 4 traces.

Next:
1. Let the gated predictor pin and measure MPKI: PC gate only, + 32KB bitmap,
   + 128KB bitmap, theta 0.02; vs PHRU, ghost cache, ghost + predictor, HRU.
2. Rerun step 1 with FPs only on accessed hub-PC non-hubs.
3. If collisions still limit it, a multi-hash Bloom filter per generation.
4. Relative theta; check all 9 traces.

## Code (all off by default)

| Addition | Where | Flags |
|---|---|---|
| First-insertion oracles + noise | PHRU fill path | `--phru-oracle first\|all`, `--phru-oracle-recall`, `--phru-oracle-fp` |
| Re-miss bitmap (measurement only) | `inc/hub_bitmap.h` | `--hub-bitmap`, `--hub-bitmap-mode/-bits/-window/-threshold/-train` |
| Per-PC reuse table + gate | `inc/pc_reuse.h` | `--pc-reuse-stats`, `--pc-reuse-gate`, `--pc-reuse-min-lookups`, `--pc-reuse-halve` |

Hooks in `src/cachesim.cc`, `src/phru_replacement_policy.cc`, `src/main.cc`,
`inc/utils.h`. Baseline regression passed 8/8 after the oracle + bitmap code;
not rerun after the PC gate was added.

## 10. `hub_oracle` policy (added later the same day)

`--replacement-policy hub_oracle` (`{inc,src}/hub_oracle_replacement_policy.*`)
turns `phru --phru-oracle all` into a standalone oracle policy:

- 8B sets: pin at MRU iff the block is a true hub, on every fill (reused or
  first insertion). Static 8B MPKI is bit-identical to `phru --phru-oracle all`.
- 64B sets: pin only a reused line whose hottest sector is a hub (HRU's rule).
  Static 64B MPKI is bit-identical to HRU.
- Hit path uses the accessed node's true degree (irrelevant at the exclusive
  LLC, where a hit is invalidated immediately).

Why the 64B rule: at a 64B fill, `resize_packet()` shrinks `blocks` to 1 but
leaves 8 per-sector degrees, so `block_degrees[0]` is sector 0 only. Static 64B
with that sector-0 test was worse than HRU (as-Skitter 38.94 vs 37.50,
sx-stackoverflow 35.40 vs 33.28, cit-Patents 54.54 vs 53.21). Using the line's
hottest sector on every fill was worse still (40.37 / 38.28 / 54.93): nearly
every 64B line contains a hub vertex, so first-insertion pinning at 64B floods
the cache. First-insertion pinning is an 8B-granularity idea.

Also found: `populate_line()` gives every non-requested sector degree 0
(`//TODO: Fix`). That is why never-accessed blocks never looked like hubs in
sections 7-8 - a labelling artifact, not graph structure.

LLC MPKI (8B static / 64B static / dueling):

| Trace | hub_oracle 8B | HRU 8B | hub_oracle 64B | HRU 64B | hub_oracle ztest | HRU ztest | hub_oracle psel | HRU psel |
|---|---|---|---|---|---|---|---|---|
| as-Skitter | 35.053 | 35.812 | 37.500 | 37.500 | 35.810 | 37.221 | 35.343 | 37.221 |
| web-BerkStan | 22.893 | 23.353 | 17.639 | 17.639 | 17.962 | 17.992 | 17.962 | 17.992 |
| sx-stackoverflow | 22.402 | 23.114 | 33.276 | 33.276 | 23.267 | 26.212 | 23.164 | 26.196 |
| cit-Patents | 52.244 | 52.826 | 53.208 | 53.208 | 52.376 | 53.187 | 52.354 | 53.256 |

`hub_oracle` beats HRU on all four traces, static 8B and under both dueling
modes. ASAN clean through 37 breakdowns / 36 merges (cit-Patents psel) and
as-Skitter ztest.

**Measurement pitfall:** in dueling runs, "Breaking down at ... MPKI x" and
"Merging at ... MPKI x" lines also contain "MPKI". Grepping the second line
containing "MPKI" picks one of those (or the L1D line) instead of the LLC. Match
lines that *start* with `MPKI` (as `run_regression.py` does) and take the
second one.

**Regression status (2026-09-30):** `hub_oracle` goldens added:
`hub_oracle_8b_no_dueling` (9 traces) and `hub_oracle_8b_psel` / `_ztest`
(4 traces). `baseline --full` passes for HRU and PHRU on all 9 traces. The
**HRU/PHRU dueling goldens were already stale before this session**:
`dueling` fails on 6 of 16 entries, e.g. `hru_8b_psel` sx-stackoverflow golden
26.195749 vs measured 24.143385. It reproduces run alone (not concurrency),
ASAN gives the identical number (no layout-dependent UB), and the Sep 23
`bin/cachesim_multi_debug` (built before today and before the Sep 24 edits to
`cachesim.h` / `set.cc` / `sectored_set.cc`) also gives 24.143385 /
1855187218 cycles. The goldens (Sep 22) predate some Sep 22-23 change. Not
refreshed - needs a deliberate `dueling --update`.
Also: merges do happen under dueling now (`Merging at ...` lines, up to 36 per
run on cit-Patents psel), so the cachesim skill's "`should_merge()` is
hardcoded false" is out of date.
