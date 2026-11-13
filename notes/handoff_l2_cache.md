# Handoff: add a sectored L2 between L1D and LLC

**Status: implemented and committed** on top of HEAD `defc81c`.
Committed as `538fe15` (2026-10-15; also adds `--l1-sets`/`--l1-ways` and
`assert(level != CacheLevel::LLC)` for sectored caches). The plan below
is kept for reference; this section records what was built and where it differs from the plan.

- Usage: `--l2 [--l2-sets 1024] [--l2-ways 16]` (1MB default). `l2_latency` config key: 11 in `configs/default.cfg` (2026-10-16; was an implicit 10), fallback 11 when absent.
- Levels are now the `CacheLevel` enum (`L1D`, `L2`, `LLC` in `inc/utils.h`) everywhere a numeric level was
  used (`Cache`, `Set`, `BasePolicy`, all policy constructors, `create_policy`); `handle_fill_blocks/line`
  lost their unused numeric `level` argument. "Is LLC" is `level == CacheLevel::LLC`.
- Stats: each block is preceded by `Cache <name>`; printed L1D, LLC, L2 so the 2nd MPKI line is still the LLC.
- **Correction to the plan:** the victim cascade was *not* N-level generic - it cleared `eviction_packet` at
  the end of every iteration, so L2 victims never reached the LLC (first validation: LLC hit rate 0, every
  policy identical). Fixed by not clearing it between iterations.
- **Pre-existing bug found:** `SectoredSet::try_hit` indexes per-way `dirty[]`/`degree[]` by sector. Kept for
  the L1D (it feeds HRU's `low_prio_only` via the eviction packet's degree; fixing it changes all goldens);
  the L2 indexes the hit way (the sector indexing overflowed a 1-way L2 under ASAN).
- Also: `Cache<SectoredSet>::handle_invalidate` leaves the caller's packet with address/size 0 on a miss;
  the miss path now restores both before each level.

Validation (pr_spmv random traces, `traces/hubs/pr_spmv`):
- `--l2` off: `baseline --full` 27/27 PASS; `dueling` 18 PASS + the 6 already-stale entries with unchanged
  current-code values -> bit-identical to before.
- `--l2` on: ASAN clean (cit-Patents HRU psel with breakdown/merge; web-Google HRU-8B + ghost cache; LRU-64B;
  hub_oracle psel with a 1-set x 1-way L2). Invariants hold on all 22 runs (L2 accesses = L1D misses,
  LLC accesses = L2 misses).
- Tiny L2 (1 line) reproduces no-L2 LLC MPKI: as-Skitter LRU-64B 44.853 vs golden 44.854, web-BerkStan
  HRU-8B 23.353 vs 23.353, as-Skitter HRU psel 36.280 vs 36.280.

LLC MPKI, 2MB LLC, no L2 -> 1MB L2 -> 4MB L2:

| Trace | LRU-64B | HRU-8B | HRU psel |
|---|---|---|---|
| as-Skitter | 44.854 -> 35.778 -> 15.985 | 35.812 -> 30.368 -> 17.044 | 36.280 -> 30.832 -> 12.508 |
| web-BerkStan | 25.174 -> 10.029 -> 6.104 | 23.353 -> 15.964 -> 6.109 | 17.992 -> 7.070 -> 6.164 |

With a 1MB L2 the LLC sees an L2-filtered stream and the policy ranking changes: on web-BerkStan HRU-8B
(15.96) is now worse than LRU-64B (10.03); HRU psel (7.07) is best.


Written 2026-10-14. Goal: `cachesim_multi` hierarchy L1D -> **L2 (sectored)** -> LLC, off by default so
every existing result and golden value is unchanged when the L2 is disabled. Read the `cachesim`
skill (simulator internals) and `cachesim-results` skill (how to compare results) first.

## Repo state at handoff

- HEAD `138891b` (configs + compress script), `c2eba0c` (.gitignore), `5e302cb` (hub_oracle policy).
- **Working tree still contains the changes saved in `patches/07_hub_stats_phru_knobs_first_insertion.patch`**
  (hub stats, dead PHRU knobs, first-insertion bitmap/PC-reuse; see
  `notes/hub_stats_phru_knobs_first_insertion_patch.md`). Decide first: `git apply -R` that patch to start
  from a clean HEAD (recommended), rebuild, and confirm `run_regression.py baseline --no-build` passes.
  `notes/` and `patches/` are untracked - copy patch 07 somewhere safe before any `git clean`.
- `bin/` binaries were built from the working tree (with patch 07), not from HEAD.
- 6 of 16 HRU/PHRU dueling goldens are stale independent of this work (`cachesim-results` skill, section 3).

## What the code does today (facts the plan depends on)

| Fact | Where |
|---|---|
| Hierarchy is a vector: `cache[0]` = L1D `Cache<SectoredSet>("L1D", 128, 16, 8, level 0, sectored, LRU)`, `cache[1]` = LLC `Cache<Set>` | `src/main.cc:197-201` |
| L1D real capacity is **128KB** (128 sets x 16 lines x 64B, 8B sectors); it *prints* 16KB (sets x ways x 8B). Size the L2 against 128KB | `inc/cachesim.h` SectoredSet ctor (`way_sectors.assign(num_ways, Sector(8))`), `src/cachesim.cc:134` |
| Sectored caches are only allowed at level 0: `assert(level == 0)` | `src/cachesim.cc:48` |
| Exclusive hierarchy: `can_insert_at_level` is true only for level 0; a hit at level k>0 invalidates the line there and fills L1 | `src/cachesim.cc:209-211`, `access_multi_level` |
| `access_multi_level` probe loop and victim cascade (`for i = 1..num_levels-1`: fill level i with level i-1's victim, take level i's victim) are already N-level generic | `src/cachesim.cc` ~l.566 / ~l.617 |
| Hard-coded 2-level spots: miss path invalidates partial blocks from `cache[1]` then `cache[0]`; perf-model `switch(hit_at_level)` treats 1 = LLC hit, default = memory | `access_multi_level` |
| `fill_packet->serviced_from_llc += 1` on a hit at **any** level > 0 - this is HRU/PHRU's core reuse signal | `access_multi_level` hit path |
| SparsityPredictor trains on every level-boundary victim inside the cascade loop ("LLC -> L1D") | cascade loop |
| `level == 1` is used to mean "the LLC": ghost-cache insertion, hub_oracle stats (and HUBSTAT hooks in patch 07) | `src/set.cc:273`, `src/hub_oracle_replacement_policy.cc:40` |
| `level == 0` means "the L1": hit-time policy update for exclusive caches, `was_accessed` derivation, Hawkeye hit path | `src/set.cc:79,134`, `src/hawkeye_replacement_policy.cc:12` |
| SectoredSet fill assumes it is the L1: ghost-cache lookup on the requested address, and marks the sector matching `packet->address` as accessed | `src/sectored_set.cc:172,182` |
| Dueling / breakdown operate on `cache[num_levels-1]` (the LLC) | top of `access_multi_level` |
| Perf model: `processCPU`, `processL1D`, `processLLC`, `processMemory`; config has `l1i_latency`, `l1d_latency`, `llc_*`, no L2 keys | `inc/performance_model.h:189-195`, `configs/default.cfg` |
| Stats header has no cache name; every parser takes the **2nd** `MPKI`/`Num Cycles`/`hits:` line as the LLC | `src/cachesim.cc:134`, `tests/regression/run_regression.py` (`MPKI_RE`, index 1), skills/notes recipes |

## Design decisions (recommendation first)

1. **Inclusion: exclusive across all three levels** (recommended - reuses the existing hit/invalidate/
   cascade logic). A line lives in exactly one of L1D/L2/LLC; an LLC hit moves the line to L1D (skipping
   L2); L1D victims fill L2, L2 victims fill LLC, LLC victims leave. Alternative: non-inclusive L2 (keep
   the L2 copy on an L2 hit) - closer to real Intel parts but needs new invalidation/duplicate handling.
2. **Geometry default:** 1024 sets x 16 ways x 64B lines, 8B sectors = 1MB (must be well above the 128KB
   L1D), LRU. CLI: `--l2` (enable), `--l2-sets`, `--l2-ways`, `--l2-policy` (default lru).
3. **Reuse counters:** increment `serviced_from_llc` only when the hit is in the LLC
   (`hit_at_level == num_levels-1`); add `serviced_from_l2` (per line and per sector, mirroring
   `block_serviced_from_llc`) if L2 reuse is wanted as a separate signal. Otherwise HRU/PHRU would see L2
   hits as LLC reuse and their behaviour silently changes.
4. **Predictor / ghost cache boundary:** train the SparsityPredictor and insert into the ghost cache only
   at the L2 -> LLC boundary and on LLC eviction; ghost lookup only on L1D fills.
5. **Footprint propagation:** an L2 victim carries the OR of its L1D-era and L2-era footprints, so the
   LLC's `was_accessed` still means "this sector was used while above the LLC". `l1_hits` (from
   `way_hits`) should accumulate across L1D and L2 residencies or be split into `l1_hits`/`l2_hits`.
6. **Perf model:** add `processL2(address, is_hit, is_fill)` and `l2_latency` (+ queue sizes if modeled)
   in `configs/*.cfg` with a default when the key is absent (suggest l2_latency ~10, between l1d 5 and
   llc 20 in `default.cfg`).
7. **Stats output:** add the cache name to each stats header and print the L2 block **after** the LLC
   block, so "2nd MPKI line = LLC" stays true for existing parsers; then move parsers to name-based
   selection at leisure.

## Implementation steps

1. **Clean start.** Revert patch 07 (or keep it consciously), rebuild, run `baseline` regression.
2. **Construction + identity.** `main.cc`: parse the L2 options; when `--l2`, build
   `{L1D, L2, LLC}` with `Cache<SectoredSet>("L2", sets, ways, 8, level 1, sectored, policy)` and LLC at
   level 2. `cachesim.cc:48`: allow sectored at any level. Add `bool is_llc` (or `is_last_level`) and
   `std::string name` to `BaseCache`/`Set`; pass them down so sets and policies can ask "am I the LLC".
3. **Replace numeric LLC checks** (`level == 1` -> `is_llc`): `set.cc:273` (ghost insert),
   `hub_oracle_replacement_policy.cc:40` (stats), HUBSTAT hooks if patch 07 is kept, and any policy code
   that counts stats at `level == 1`. Keep `level == 0` checks as "is L1D" (correct as is). Grep again:
   `grep -nE 'level\s*(==|!=|>|<)\s*[0-9]' src inc`.
4. **`access_multi_level`.**
   - Miss path: invalidate partial blocks from every level below L1D (loop from the LLC down to L2, then
     L1D) and merge them into `fill_packet`, instead of the hard-coded `cache[1]` / `cache[0]`.
   - Hit path: generic already; apply decision 3 for the reuse counters.
   - Cascade loop: keep as is (it already chains L1D -> L2 -> LLC); move predictor training/prediction
     into the iteration that fills the LLC (`i == num_levels-1`).
   - Perf-model switch: L1D hit free; L2 hit -> `processL2(hit)`; LLC hit -> `processL2(miss)` +
     `processLLC(hit)`; memory -> `processL2(miss)` + `processLLC(miss)` + `processMemory`.
5. **SectoredSet as a non-L1 level** (`src/sectored_set.cc`): gate the ghost lookup (l.172) and the
   requested-address sector marking (l.182) on `level == 0`; at L2 fills use only `packet->footprint`;
   make L2 `handle_evict` emit footprint, `l1_hits`, `block_serviced_from_llc` (and new l2 counters) for
   the LLC fill.
6. **Perf model + config** (decision 6), defaults so old configs still parse.
7. **Stats** (decision 7). Also report L2 hits/misses and LLC accesses per kilo-instruction.
8. **Docs:** update the `cachesim` skill (hierarchy section, data-movement section) and
   `tests/regression/README.md` if new configs are added.

## Validation

1. `--l2` off: `baseline --full` and `dueling` regressions identical to before the change (bit-exact,
   modulo the 6 already-stale dueling goldens).
2. ASAN build, `--l2` on, with set dueling, on a trace that breaks down and merges often
   (cit-Patents psel had 37 breakdowns / 36 merges).
3. Invariants (add a debug-only checker or count): a line resident in at most one level; L1D misses =
   L2 hits + L2 misses; L2 misses = LLC lookups; LLC misses unchanged in definition.
4. Sanity extremes: a tiny L2 (1 set x 1 way) should give LLC misses close to the no-L2 run; an L2 as large
   as the LLC should cut LLC lookups sharply.
5. Results check: pr_spmv random as-Skitter / web-BerkStan, LRU-64B and HRU-8B, L2 off vs on; LLC MPKI
   should fall with L2 on and the HRU-vs-LRU gap may change because the LLC now sees an L2-filtered stream.

## Open questions for the user

- Exclusive vs non-inclusive L2 (decision 1)?
- L2 size/associativity and latency to model (decision 2, 6)?
- Should HRU/PHRU treat L2 reuse as a hub signal (decision 3), and should L2 hits vote in set dueling
  (today only LLC-level leader-set misses move PSEL)?

## Effort estimate

Construction/identity/stats/perf model: ~1 day. Access-path generalisation + SectoredSet-at-L2: ~1 day.
Validation (regressions, ASAN, invariants, sanity runs): ~0.5-1 day.
