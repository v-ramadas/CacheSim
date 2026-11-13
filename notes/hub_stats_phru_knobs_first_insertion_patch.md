# Patch 07: hub stats, PHRU experiment knobs, first-insertion predictor (2026-10-14)

`patches/07_hub_stats_phru_knobs_first_insertion.patch` holds every CacheSim source change that was
left uncommitted after `5e302cb Add hub oracle policy` and `138891b Add performance-model configs and
trace compression script`. It is plain `git diff` output (6 modified files + 5 new files):

```
git apply patches/07_hub_stats_phru_knobs_first_insertion.patch      # restore the changes on HEAD
git apply -R patches/07_hub_stats_phru_knobs_first_insertion.patch   # remove them from a tree that has them
```

Verified when written: applies cleanly to HEAD `138891b`, and `git apply -R --check` against the working
tree succeeds (the patch is exactly the uncommitted state). The working tree with the patch applied built
on 2026-10-14 (`make` 15:02-15:09).

## What is in it

| Group | Purpose | Files | State |
|---|---|---|---|
| Hub statistics (`--hub-stats`) | HUBSTAT lines: hub occupancy at each LLC insertion, hub vs non-hub victims at each eviction | new `inc/hub_stats.h`, `src/hub_stats.cc`; `handle_evict(packet, incoming)` signature in `inc/cachesim.h`, `src/sectored_set.cc`; call in `src/cachesim.cc`; hooks in `src/set.cc`; flag/print in `src/main.cc`, `inc/utils.h` | Works for LLC insertion/eviction. HUBSTAT 6-8 (PHRU fill diagnostics) are empty: their callers lived in `phru_replacement_policy.cc` |
| PHRU experiment knobs | ~25 `PHRU_*` options: threshold + adaptive modes, promote-accessed, line threshold/ratio, lease, random demotion, `--phru-oracle` (fix_fn/fix_fp/both/first/all, recall/fp noise), address-keyed reuse history | `inc/utils.h`, `src/main.cc`; new `inc/reuse_history.h` + train hooks in `src/set.cc` | **Dead**: parsed but unused. The PHRU code that read them was reverted to HEAD on 2026-10-14 14:56 and is not recoverable from git (no stash/reflog/unreachable copy) |
| First-insertion predictor measurement | `--hub-bitmap*` (tagless windowed re-miss bitmap), `--pc-reuse-*` (per-PC LLC reuse rate + gate) | new `inc/hub_bitmap.h`, `inc/pc_reuse.h`; hooks in `src/cachesim.cc`; options/print in `src/main.cc`, `inc/utils.h` | **Half-dead**: trains, but its scoring (`hubBitmap.consume`, `pcReuse.record_first_fill`) was in PHRU. `--hub-bitmap` now grows its pending set without bound |
| hub_oracle leftover | `HubOracle::print_stats()` call at exit (2 lines) | `src/main.cc` | Not in `5e302cb`; the policy works without it, it only prints the fills/pinned summary |

## Why it was not committed

The PHRU-knob and first-insertion groups expose options that silently do nothing (or leak) at HEAD. The results they produced are recorded in
`phru_hub_heuristic_quality.md`, `ghost_cache_tuning.md` and `first_insertion_hub_prediction.md`.
If PHRU experiments resume, re-implement the consumers in `phru_replacement_policy.cc` first (the notes
describe each mechanism and its result), then apply this patch for the plumbing, or take only the hub
stats group. The hub_oracle `print_stats` hunk can be committed on its own.
