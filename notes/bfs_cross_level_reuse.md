# BFS: where the reuse is, and which of it a cache can use (2026-10-15)

Question (user): does BFS only benefit graphs whose vertices are hit from several BFS levels, because each level's
walk re-touches them? Companion to `graph_characterization.md` section 4 (policy runs; bfs gains only on cit-Patents
-9.4% and com-Orkut -14.5% vs SRRIP, r = -0.71 with top-down `parent[]` long-gap reuse).

Scripts and raw outputs: `pin/pin/analysis/bfs_xlevel/`. Traces: `pin/pin/traces_roi/hubs/bfs/<G>.<order>.xz`
(full DOBFS ROI, `gap_roi_tracer`). Sources: `pin/pin/analysis/bfs_sources.tsv`. LLC = 2MB unless noted;
miss curves are fully associative LRU (`mrc`), "OPT" = Belady MIN with bypass, fully associative, 64B lines.

## Summary

1. **Full BFS has plenty of reuse beyond 2MB**: LRU-64B misses are 1.3-4.6x the compulsory (first-touch) misses.
   The single-step traces (bfs_bu, bfs_bupk) hid this: they have 0-4% avoidable misses because they exclude
   cross-level reuse by construction.
2. **But almost none of it is capturable**: even OPT removes only 6-17% of LRU's misses on the large graphs
   (cit-Patents 27-35%, a small trace). Most cross-level reuse spans a whole bottom-up step (15-80MB).
3. **63-99% of the avoidable misses come from bottom-up steps** re-scanning still-unvisited vertices
   (`parent[u]`, index, neighbor-list prefix every BU level) - a looping scan over mostly low-degree, late-found
   vertices. Hub-aware policies target the wrong lines; this is why bottom-up-dominated graphs gain 0-3%.
4. **"Hit from several levels" is not the discriminator.** In undirected graphs a vertex can be probed from at most
   3 levels (neighbors' levels differ by <= 1), yet com-Orkut (undirected) is the biggest bfs winner. What matters
   is how much top-down `parent[v]` probing hits the same lines while the footprint is a few times the LLC:
   - com-Orkut: one TD level probes the **whole 11.7MB parent[] array, 44 probes per 64B line** (within-level hub
     reuse).
   - cit-Patents: all 22 levels TD; levels of 1.8-3.6MB (~LLC size) re-probe 38% of the previous TD level's
     lines (cross-level reuse - the case the hypothesis describes).
5. **Prediction**: indochina-2004 (being traced) should be the strongest bfs case: 15 TD levels > 2MB, largest
   25.8MB at 26 probes/line, 67% of lines re-probed by the next TD level. in-2004 is a smaller version.
6. **Bitmap capacity**: a 2MB LLC holds a 1-bit/vertex bitmap of 16.8M vertices (8.4M with both front and next;
   ~4-8M once it competes with BU streams; iso-area 8B LLC ~1.25MB -> ~10M / ~5M). Every simulated graph's
   frontier bitmap is <= 0.6MB, so BU bitmap reuse never misses. Bitmap capacity misses need ~8M+ vertices:
   indochina-2004 (7.4M, 0.93MB/bitmap), kron23/urand23 (8.4M, 1.0MB) are at the edge; uk-2002 (18.5M, 2.3MB),
   road-USA (23.9M, 3.0MB) are past it.

## 1. How many levels touch each vertex (graph only) - `level_spread.py`

TD at level d probes `parent[v]` for every out-neighbor v of a level-d vertex, so v is probed at the distinct levels
of its reached in-neighbors. Undirected: <= 3 levels. Directed: in-neighbors can sit at any level >= d(v)-1.

| Graph | Dir | Levels | Probes on vertices touched at >= 4 levels | Probe-weighted mean levels | Top 1% in-degree: share at >= 4 levels |
|---|---|---|---|---|---|
| com-Youtube, as-Skitter, com-LiveJournal, hollywood-2009, ogbn-products | no | 9-25 | 0% | 2.57-2.74 | 0% |
| wiki-Talk | yes | 8 | 10.4% | 1.88 | 9% |
| soc-LiveJournal1 | yes | 15 | 18.3% | 3.00 | 37% |
| sx-stackoverflow | yes | 9 | 25.4% | 2.95 | 54% |
| cit-Patents | yes | 22 | 25.4% | 2.55 | 36% |
| soc-Pokec | yes | 13 | 27.6% | 3.17 | 62% |
| web-Google | yes | 33 | 43.6% | 4.02 | 92% |
| web-BerkStan | yes | 577 | 59.2% | 11.67 | 69% (max 270 levels) |
| wiki-topcats | yes | 256 | 73.1% | 6.38 | 99% (max 255 levels) |

(This counts possible TD probes over all levels; DOBFS runs many levels bottom-up, see section 4.)

## 2. Miss decomposition from the per-step logical profiles - `xlevel_logical.py`

Input: `gapbs_traces/reuse/bfs/*.csv` (reuse_tracker.h; older runs with different sources than bfs_sources.tsv -
cit-Patents/web-Google default reach almost nothing there). Per step: compulsory = first touch in the whole BFS,
cross-level = first touch in this step of a line used in an earlier step, within-level = re-touch inside the step at
distance >= 2MB. For non-trivial runs, **cross-level is 34-75% of all misses of a step-flushed cache and
within-level is 0-2.4%** (com-LiveJournal random/hubsort the largest). Cross-level lines by array:
`in_neigh` 25-59%, `parent` 17-42%, `in_index` 17-41% - i.e. bottom-up re-scans.

## 3. Full-trace miss curves and OPT - `run_full_mrc.sh`, `mrc_hub.cpp`, `opt.cpp`, `summarize.py`

2MB, 64B lines, fully associative (random / hubsort / default):

| Graph | LRU misses / compulsory (random) | OPT saves vs LRU (random / hubsort / default) | Avoidable LRU misses from BU PCs | from TD `...c06` | Avoidable on HRU-rule hubs |
|---|---|---|---|---|---|
| com-Youtube | 3.71x | 9.9 / 12.5 / 11.3% | 92% | 1.5% | 64% |
| as-Skitter | 2.65x | 5.5 / 6.8 / 6.0% | 89% | 3.2% | 74% |
| hollywood-2009 | 1.60x | 11.6 / 7.8 / 8.1% | 76% | 21% | 56% |
| ogbn-products | 1.92x | 9.3 / 9.0 / 9.1% | 63% | 23% | 60% |
| com-LiveJournal | 2.41x | 9.6 / 9.6 / 9.3% | 66% | 19% | 59% |
| wiki-Talk | 2.34x | 7.6 / 6.7 / 7.1% | 99% | - | 59% |
| cit-Patents | 1.54x | 27.0 / 32.7 / 35.0% | ~2% | 76% | 47% |
| sx-stackoverflow | 2.09x | 6.0 / 8.1 / 6.9% | 96% | 3.1% | 69% |
| soc-LiveJournal1 | 2.66x | 9.8 / 10.8 / 9.9% | 86% | 13% | 68% |
| soc-Pokec | 2.11x | 6.2 / 6.9 / 6.9% | 97% | 2.6% | 73% |
| web-Google | 3.13x | 15.6 / 15.3 / 15.9% | 81% | 13% | 62% |
| wiki-topcats | 2.68x | 17.3 / 15.8 / 15.8% | 67% | 30% | 59% |
| web-BerkStan | 4.62x | 13.3 / 10.5 / 10.0% | 88% | 7.2% | 66% |

- PCs are page offsets in `gapbs/bfs`. BU = `...9fd`, `...a0b`, `...a29`, `...a0f`, `...a33` (inside the BUStep
  range 0x29b2-0x2a70 used for bfs_bu); `...a33` = `front.get_bit` (bitmap) is a tiny share of avoidable misses.
  TD `...c06` = `parent[v]` probe (bfs.cc:78-79). `...bf9` / `...bd6` are other TD/conversion accesses.
- "HRU-rule hub" = NodeDegree > PCAvgDegree from the hubs annotation (an address reused more than its PC's
  average), not graph degree.
- 8B lines: at 2MB, 8B blocks give 3.7-5.9x the misses of 64B (neighbor-list streams need spatial locality); any
  bfs gain must come from 64B replacement decisions.
- Notes on other findings: hub_oracle hurts bfs by pinning hubs in BU-dominated runs
  (`graph_characterization.md` section 4). Long-gap reuse in BU runs is mostly the bitmap, which hits; the
  avoidable misses are the re-scan PCs above.

## 4. TDStep: size of each level's parent[] probes - `td_footprint.py`

Replays DOBFS's direction choice exactly (alpha 15, beta 18, scout_count rules of bfs.cc) on exact BFS levels.
Validated: total TD probes == trace accesses of PC `...c06` (com-Youtube 38,869; com-LiveJournal 1,522,296;
cit-Patents 506,156; hollywood-2009 1,087,304; ogbn-products 3,730,237; web-Google 761,463; as-Skitter 110,316).
Random ordering (16 unrelated vertices per 64B line). Per-level tables: `bfs_xlevel/td_out/<G>.random.txt`.

| Graph | parent[] | TD levels | TD probes (% arcs) | Biggest TD level: probes | footprint | probes / line | TD levels > 2MB | lines re-probed by next TD level |
|---|---|---|---|---|---|---|---|---|
| indochina-2004 | 28.3MB | 33 | 47.2M (24.7%) | 10.9M | 25.8MB | 26 | 15 | 67% |
| soc-LiveJournal1 | 18.5MB | 11 | 1.85M (2.7%) | 1.65M | 15.1MB | 6.7 | 2 | 21% |
| com-LiveJournal | 15.3MB | 12 | 1.52M (2.2%) | 0.74M | 12.4MB | 3.7 | 2 | 40% |
| com-Orkut | 11.7MB | 5 | 8.51M (3.6%) | 8.44M | 11.7MB | 44 | 2 | 23% |
| urand21 | 8.0MB | 5 | 0.98M (1.5%) | 0.95M | 8.0MB | 7.3 | 1 | 17% |
| ogbn-products | 9.3MB | 22 | 3.73M (3.0%) | 1.55M | 7.7MB | 12 | 3 | 47% |
| wiki-topcats | 6.8MB | 252 | 2.05M (7.2%) | 1.67M | 6.7MB | 15 | 2 | 49% |
| sx-stackoverflow | 9.9MB | 6 | 0.18M (0.5%) | 0.16M | 5.3MB | 1.9 | 1 | 9% |
| in-2004 | 5.3MB | 38 | 2.88M (17.4%) | 0.63M | 4.3MB | 8.9 | 4 | 60% |
| hollywood-2009 | 4.3MB | 6 | 1.09M (1.0%) | 1.05M | 4.1MB | 15 | 1 | 13% |
| soc-Pokec | 6.2MB | 9 | 0.21M (0.7%) | 0.18M | 3.8MB | 2.9 | 1 | 18% |
| cit-Patents | 14.4MB | 22 (all) | 0.51M (3.1%) | 0.10M | 3.6MB | 1.6 | 4 | 38% |
| wiki-Talk | 9.1MB | 5 | 0.07M (1.5%) | 0.07M | 3.1MB | 1.8 | 1 | 0.4% |
| web-Google | 3.5MB | 29 | 0.76M (14.9%) | 0.24M | 3.0MB | 4.9 | 2 | 57% |
| as-Skitter | 6.5MB | 18 | 0.11M (0.5%) | 0.08M | 2.5MB | 2.0 | 1 | 14% |
| web-BerkStan | 2.6MB | 570 | 1.73M (22.7%) | 0.55M | 2.3MB | 15 | 2 | 60% |
| higgs-twitter | 1.7MB | 10 | 1.05M (7.0%) | 0.78M | 1.7MB | 7.4 | 0 | 57% |
| flickr-growth | 8.8MB | 12 | 0.10M (0.3%) | 0.06M | 1.5MB | 2.4 | 0 | 11% |
| com-Youtube | 4.3MB | 10 | 0.04M (0.7%) | 0.03M | 0.9MB | 1.0 | 0 | 8% |
| kron21 | 8.0MB | 4 | 0.01M (0.0%) | 0.01M | 0.6MB | 1.0 | 0 | 1% |

Shape: early TD levels are tiny; the level just before the TD->BU switch (scout_count > edges_to_check/15) is the
big one (2-26MB, often most of parent[]); after BU, TD resumes on the shrinking tail, which on deep graphs
(indochina, in-2004, web-*, wiki-topcats, cit-Patents) is long and carries 38-67% of lines level to level.

Reading against the policy runs: winners need TD probing that is intense per line over a footprint of ~1-6x the
LLC (com-Orkut within-level, cit-Patents cross-level). Thin probing over a big footprint (com-LiveJournal, 3.7
probes/line over 12MB) or tiny TD (com-Youtube, as-Skitter, sx-stackoverflow, kron21, wiki-Talk) gains nothing;
hollywood-2009 / ogbn-products (12-15 probes/line, 4-8MB, but TD only 1-3% of arcs) gained nothing.

## 4b. Why cit-Patents and com-Orkut gain - `td_concentration.py`

All TD `parent[v]` probes of the run as one stream (random ordering, 2MB, fully associative, probe stream alone so
LRU is optimistic; probe order within a level approximated by sorted frontier order):

| Graph | TD share of accesses | TD misses as share of all LRU misses | LRU-64B hit on TD probes | Best static 64B (top 32,768 lines) | Best static 8B iso-area (top 163,840 blocks) | Probes on top-1% degree | Bound: TD headroom as share of all misses | Simulated gain vs SRRIP |
|---|---|---|---|---|---|---|---|---|
| cit-Patents | 16.7% | **38%** | 46.7% | 58.8% | 90.9% | 0.3% | **32%** | -9.4% |
| com-Orkut | 6.2% | **48%** | 38.3% | 55.7% | 78.0% | 37.5% | **31%** | -14.5% |
| hollywood-2009 | 3.4% | 10% | 79.8% | 88.3% | 98.0% | 20.4% | 9% | -3.4% |
| soc-LiveJournal1 | 1.7% | 13% | 38.7% | 52.5% | 77.1% | 18.8% | 8% | (not run) |
| ogbn-products | 3.1% | 12% | 77.2% | 68.8% | 89.9% | 8.3% | 7% | +0.9% |
| com-LiveJournal | 1.8% | 13% | 39.1% | 46.2% | 66.2% | 6.0% | 6% | (not run) |
| sx-stackoverflow | 0.4% | 4% | 28.2% | 59.2% | 100% | 12.8% | 4% | -1.5% |
| as-Skitter | 0.2% | 2% | 49.5% | 82.7% | 100% | 7.7% | 2% | -0.3% |

Both winners need two things at once: TD probes are ~40-50% of all LLC misses (others: 2-13%), and LRU-64B leaves
40-44 points of TD hit rate on the table. The ways they get there differ:
- **cit-Patents** never switches to bottom-up (the reachable DAG is ~216K of 3.77M vertices, so scout_count never
  exceeds edges_to_check/15): the whole BFS is a pr-like random gather on `parent[]`, 8.8MB over 22 levels, each
  level 1.8-3.6MB with 38% of lines re-probed next level. The probes are *not* degree-hub-concentrated (0.3% on
  the top-1% degree vertices); the win is granularity: a random-order 64B line holds 16 vertices of which 1-2 are
  probed, so iso-area 8B blocks could serve 91% vs 59% for 64B, and retaining blocks across levels pays.
- **com-Orkut** (avg degree 76) does one TD level just before switching to bottom-up that probes the whole 11.7MB
  `parent[]` 8.4M times (44 per line); a vertex is probed once per frontier neighbor, so probes follow degree
  (37.5% on the top 1%) - the same hub gather as pr. Hub-aware 8B retention: 78% vs LRU 38%.
- Everyone else is bottom-up-dominated: even perfect TD caching moves <= 9% of all misses (hollywood-2009 has
  enough TD but LRU already hits 80% on its 4.2MB footprint).

## 5. Open checks

- Policy runs (LRU/SRRIP-64B, HRU/PHRU/hub_oracle 8B) on full bfs for the deep directed graphs (wiki-topcats,
  web-Google, web-BerkStan) and the LiveJournals: if level spread mattered by itself these should gain like
  cit-Patents; the TD-intensity explanation predicts small gains. Traces exist.
- indochina-2004 / in-2004 bfs once traced (predicted winners), and com-Orkut level spread / OPT (not run here).
- Caveats: cit-Patents' BFS reaches only ~216K vertices (tiny trace); fully associative bounds ignore set conflicts;
  section 2 uses older BFS sources.
