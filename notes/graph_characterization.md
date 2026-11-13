# Which graphs gain from hub-aware 8B caching, and why (2026-10-15)

Data: every policy run of 2026-10-13..15 (LLC 2MB 1024x32; 2-level = L1D 128KB -> LLC, 3-level = L1D 32KB ->
L2 256KB -> LLC; random ordering; traces in `pin/pin/traces_roi/hubs/<app>/`). "Gain" = change in LLC MPKI vs
SRRIP-64B (negative = fewer misses). "Area-fair" = HRU / PHRU / hub_oracle (static 8B or psel dueling);
HRUpp / PHRUpp are excluded from area-fair claims because they skip iso-area and keep 256 ways at 8B.
2-level and 3-level gave the same gains within ~1 point everywhere, so the conclusions hold for both.

## 1. The predictor: 8B uplift (not skew, not average degree)

Computed from the graph file alone (`gapbs_traces/inputs/<G>/<G>.random.sg`; `.sg` layout = bool directed,
int64 arcs, int64 nodes, int64 offsets[n+1], int32 neighbors..., then the in-edge arrays if directed).
PageRank-style kernels gather a 4B value per source vertex once per out-edge. In random ID order a 64B line
holds 16 unrelated vertices, an 8B block holds 2. For each graph:

- capture@64B = share of gathers a perfect cache of the hottest 32,768 64B lines (2MB) could serve
- capture@8B = same for the hottest 163,840 8B blocks (iso-area 8B LLC, 1024 x 160)
- **8B uplift = capture@8B - capture@64B**

Correlation with pr_spmv HRU-8B gain vs SRRIP (2-level, 12 graphs): 8B uplift **r = -0.77**; fraction of
vertices that fit in 2MB r = 0.40; top-1% edge share (skew) r = -0.21; vertex count r = -0.21; average
degree r = 0.01.

| Graph | Type | Vertices | Avg deg | Max deg | Top-1% edge share | Capture@64B | @8B | 8B uplift | pr_spmv HRU | hub_oracle | HRUpp |
|---|---|---|---|---|---|---|---|---|---|---|---|
| sx-stackoverflow | Q&A, dir | 2.60M | 13.4 | 38,147 | 51% | 65% | 82% | +16 pp | -41% | -43% | -56% |
| soc-LiveJournal1 | social, dir | 4.85M | 14.1 | 20,292 | 18% | 24% | 37% | +12 pp | -9% | -10% | -19% |
| com-LiveJournal | social | 4.00M | 17.3 | 14,815 | 17% | 28% | 39% | +11 pp | -8% | -9% | -20% |
| cit-Patents | citation, dir | 3.77M | 4.4 | 770 | 12% | 25% | 33% | +8 pp | -6% | -7% | -17% |
| soc-Pokec | social, dir | 1.63M | 18.8 | 8,763 | 10% | 46% | 53% | +7 pp | -6% | -8% | -33% |
| com-Orkut | social | 3.07M | 76.3 | 33,313 | 12% | 28% | 35% | +6 pp | -4% | -5% | -16% |
| as-Skitter | internet | 1.70M | 13.1 | 35,455 | 33% | 59% | 66% | +6 pp | -16% | -18% | -39% |
| wiki-topcats | hyperlink, dir | 1.79M | 15.9 | 3,907 | 14% | 44% | 49% | +5 pp | -5% | -7% | -26% |
| com-Youtube | social | 1.13M | 5.3 | 28,754 | 36% | 75% | 78% | +3 pp | -16% | -22% | -30% |
| wiki-Talk | comms, dir | 2.39M | 2.1 | 100,022 | 96% | 98% | 100% | +2 pp | +4% | +4% | +1% |
| web-Google | web, dir | 0.92M | 5.6 | 456 | 6% | 68% | 66% | -2 pp | +7% | +5% | -42% |
| web-BerkStan | web, dir | 0.69M | 11.1 | 249 | 10% | 86% | 80% | -6 pp | +8% | +6% | -24% |

(hollywood-2009 1.14M vertices, avg 98.9, 8B uplift +8 pp; ogbn-products 2.45M, avg 50.5, +7 pp - bfs only.)

## 2. Four graph classes

1. **Hub-concentrated, hot set ~ LLC size** (sx-stackoverflow, as-Skitter, com-Youtube): max degree >= ~28K,
   top 1% of vertices hold 33-51% of edges, 1-2.6M vertices. 64B lines waste space on cold neighbours in
   random order; 8B packs the hubs. Big wins (-16% to -41% area-fair).
2. **Large, moderately skewed** (both LiveJournals, com-Orkut, soc-Pokec, cit-Patents, wiki-topcats): flatter
   tails (top 1% = 10-18%), even a perfect 8B cache captures only 33-53%. Capacity-bound: modest area-fair gains
   (-4% to -10%); HRUpp's -16% to -33% is mostly its extra capacity.
3. **Flat web graphs that nearly fit** (web-Google, web-BerkStan): no out-degree hubs (max 249-456), <1M vertices,
   64B already captures 68-86%. Iso-area 8B loses capacity for nothing: hub policies +5% to +8% *worse* than SRRIP.
4. **Extreme star** (wiki-Talk): avg degree 2.1, 1% of vertices hold 96% of edges; the tiny hub set fits at any
   granularity (98% at 64B). Nothing to win.

Rule: hub-aware 8B caching helps when the graph is skewed *and* its hot set is comparable to the LLC; it fails
for small flat graphs and for star graphs whose hubs already fit.

## 3. Per kernel: is "skew and hub size" the explanation?

3-level runs; best area-fair gain vs SRRIP; negative r = the factor predicts bigger gains.

| Kernel | Graphs | r(skew alone, gain) | r(8B uplift, gain) | Gain range vs SRRIP | Graphs with gain > 5% |
|---|---|---|---|---|---|
| pr | 7 | -0.20 | **-0.78** | -41% .. -6% | 7/7 |
| pr_spmv_clean | 7 | -0.18 | **-0.77** | -42% .. -5% | 7/7 |
| sssp | 6 | +0.06 | **-0.89** | -21% .. -6% | 6/6 |
| cc | 7 | +0.50 | -0.64 | -15% .. -5% | 6/7 |
| cc_sv | 7 | -0.43 | -0.63 | -24% .. -6% | 7/7 |
| bc | 7 | +0.42 | -0.48 | -16% .. -2% | 4/7 |
| bfs | 9 | +0.34 | +0.01 | -15% .. +1% | 2/9 |

- **Skew alone predicts nothing** for any kernel (wiki-Talk is the most skewed graph and gains least;
  sx-stackoverflow gains most at 51% skew because its hot set ~ LLC size).
- **Skew x hub-set size relative to the LLC** (8B uplift) explains pr, pr_spmv, sssp well and cc, cc_sv
  fairly well - kernels that gather a per-vertex value once per edge.
- **bc**: weak trend; its gains come mostly from hub_oracle and peak on com-Orkut, which this measure ranks low.
- **bfs**: not explained (r ~ 0); see section 4.

Defensible statement: *For pr, sssp, cc and cc_sv, the benefit of hub-aware fine-grained caching is set by how
much of the hub working set fits in the LLC - the interaction of degree skew and hub-set size relative to cache
capacity, not skew alone. bc follows weakly; bfs is dominated by compulsory misses and largely insensitive to
graph structure.*

Caveats: 6-9 graphs per kernel (one outlier moves r a lot); 8B uplift is a static popularity bound from
out-degree, closest to pr's gather pattern.

Reproduce: `graphchar.py` logic is in section 1 (offsets -> out-degree -> group 16 / 2 consecutive IDs ->
sort -> top-K share).

## 4. bfs: why only cit-Patents and com-Orkut gain

Hypothesis (user): high-degree vertices are re-accessed *across* BFS levels, so retaining them in the LLC
helps. **Supported, with a refinement: it only matters where BFS spends real time in top-down steps.**

GAP's bfs is direction-optimizing. The relevant accesses (3-level traces, random ordering; PCs are offsets in
`gapbs/bfs`, `_init` = 0x3000):
- PC `...c06` (0x5c06): `TDStep` `curr_val = parent[v]; if (curr_val < 0)` (bfs.cc:78-79) - top-down gather;
  a hub is checked once per frontier neighbour, across many levels.
- PC `...a33`: `BUStep` `front.get_bit(v)` - bottom-up frontier bitmap (~0.2MB, always resident).

Long-gap reuse = reuse of the same 64B line >= 10K trace records later (proxy for inter-level; the trace has no
level markers).

| Graph | Top-down parent[] share of accesses | TD long-gap reuses / 1K instr | of those on hubs | TD parent[] footprint | Best area-fair vs SRRIP | hub_oracle vs SRRIP |
|---|---|---|---|---|---|---|
| cit-Patents | 16.7% | **30.8** | 68% | 8.8MB | **-9.4%** | -9.4% |
| com-Orkut | 6.2% | **13.2** | 76% | 11.7MB | **-14.5%** | -8.3% |
| hollywood-2009 | 3.4% | 4.3 | 61% | 4.2MB | -3.4% | +4.7% |
| ogbn-products | 3.1% | 4.0 | 69% | 9.0MB | +0.9% | +15.4% |
| soc-Pokec | 0.5% | 0.6 | 60% | 4.3MB | -3.2% | +10.9% |
| sx-stackoverflow | 0.4% | 0.5 | 51% | 5.7MB | -1.5% | +16.6% |
| as-Skitter | 0.2% | 0.4 | 50% | 3.2MB | -0.3% | +12.6% |
| com-Youtube | 0.1% | 0.2 | 48% | 1.2MB | -0.3% | +4.1% |
| wiki-Talk | 0.2% | 0.1 | 50% | 3.1MB | -2.4% | +7.2% |

r(TD parent[] long-gap reuses per 1K instr, best area-fair gain) = **-0.71** (9 graphs).

- On the 5 bottom-up-dominated graphs (top-down <= 0.5% of accesses) the long-gap reuse is mostly the frontier
  bitmap (59-78% via `...a33`), which fits under any policy - nothing to win.
- Threshold: hollywood-2009 / ogbn-products have some top-down reuse (~4 per 1K instr) but a 4-9MB TD footprint
  and no gain; the two winners are 3-7x higher.
- Why hub_oracle hurts bfs: it pins every high-degree line regardless of phase; in bottom-up-dominated runs those
  pins are rarely reused across levels and waste capacity (+4% to +17%).
- Things that did NOT separate winners from losers: non-compulsory share of LRU misses (winners 34-55%,
  losers 37-73%), share of long-gap reuse overall, its concentration on the top 32K lines (60-83% everywhere).
- Caveats: cit-Patents' BFS is tiny (9.5M ROI instructions, ~216K reachable vertices of the DAG); 9 graphs.

### 4a. Same-level vs cross-level top-down reuse (follow-up)

Each BFS is TD -> one BU stretch -> TD (cit-Patents: TD only, 0 BU steps). Re-reads of a `parent[]` line (PC `...c06`)
were split by whether a BU stretch ran between the two reads (certainly different levels):

| Graph | Long-gap re-reads inside a TD run / 1K instr | Re-reads across the BU stretch / 1K instr | Best area-fair vs SRRIP |
|---|---|---|---|
| cit-Patents | 30.75 | 0.00 | -9.4% |
| com-Orkut | 13.14 | 0.06 | -14.5% |
| hollywood-2009 | 4.20 | 0.05 | -3.4% |
| ogbn-products | 3.72 | 0.20 | +0.9% |
| soc-Pokec / sx-so / as-Skitter / com-Youtube / wiki-Talk | 0.09-0.49 | 0.00-0.07 | -3.2% .. -0.3% |

- The "across the BU stretch" reuse is negligible everywhere (<=0.2/1K) and all >=1M records apart (beyond any LLC).
- The useful reuse is 10K-1M records apart *within a TD run*; it can't be split into same-level vs adjacent-level
  without level markers (a TD run spans several levels). Within one large level a hub is re-checked once per
  frontier neighbour, and in random order those checks are already 10K-1M records apart.
- Conclusion so far: the gain tracks the volume of mid-gap TD `parent[]` reuse, not a same-depth vs cross-depth
  property. A per-level split needs a level marker in the tracer (e.g. emit a record at each TDStep call).

- Follow-up with miss curves, Belady OPT bounds, per-level TD footprints and bitmap capacity:
  `bfs_cross_level_reuse.md` (2026-10-15).


## 5. All 28 graphs: skew, hub size, 64B vs 8B (2026-10-15)

Same method as section 1, extended to every graph in `gapbs_traces/inputs/` (random ordering, out-degree as the
gather weight). Hub = out-degree > average (HRU's rule). "Hubs packed" = their 4 B entries side by side (and share
of the property array); "as 64B lines" = the lines their entries touch in random order. Capture = share of gathers a
perfect 2MB cache of the hottest 32,768 64B lines / 163,840 iso-area 8B blocks serves. Script and raw metrics:
`pin/pin/analysis/graph_summary/` (`graph_summary.py`, `<G>.txt`, `summary.md` has the full size/skew/hub tables).

| Graph | Gini | Top-1% edges | Property array | Hubs packed (share) | Hubs as 64B lines (share) | 2MB capture @64B | @8B iso | 8B uplift |
|---|---|---|---|---|---|---|---|---|
| kron23 | 0.94 | 57% | 32.0 MB | 3.4 MB (10%) | 26.6 MB (83%) | 48% | 71% | +23 pp |
| sx-stackoverflow | 0.89 | 51% | 9.9 MB | 1.1 MB (11%) | 8.4 MB (85%) | 65% | 82% | +16 pp |
| flickr-growth | 0.90 | 52% | 8.8 MB | 0.9 MB (10%) | 7.2 MB (82%) | 70% | 85% | +15 pp |
| kron21 | 0.92 | 52% | 8.0 MB | 0.8 MB (10%) | 6.5 MB (81%) | 73% | 87% | +15 pp |
| ljournal-2008 | 0.74 | 19% | 20.5 MB | 4.9 MB (24%) | 20.2 MB (99%) | 24% | 37% | +14 pp |
| soc-LiveJournal1 | 0.72 | 18% | 18.5 MB | 4.5 MB (24%) | 18.3 MB (99%) | 24% | 37% | +12 pp |
| com-LiveJournal | 0.69 | 17% | 15.2 MB | 3.9 MB (26%) | 15.1 MB (99%) | 28% | 39% | +11 pp |
| enwiki-2013 | 0.56 | 14% | 16.1 MB | 4.1 MB (26%) | 15.9 MB (99%) | 24% | 32% | +8 pp |
| cit-Patents | 0.68 | 12% | 14.4 MB | 5.1 MB (35%) | 14.4 MB (100%) | 25% | 33% | +8 pp |
| hollywood-2009 | 0.75 | 22% | 4.3 MB | 0.8 MB (19%) | 4.2 MB (96%) | 71% | 78% | +8 pp |
| ogbn-products | 0.62 | 13% | 9.3 MB | 2.9 MB (31%) | 9.3 MB (100%) | 35% | 42% | +7 pp |
| soc-Pokec | 0.64 | 10% | 6.2 MB | 2.0 MB (32%) | 6.2 MB (100%) | 46% | 53% | +7 pp |
| com-Orkut | 0.56 | 12% | 11.7 MB | 3.6 MB (31%) | 11.7 MB (100%) | 28% | 35% | +6 pp |
| as-Skitter | 0.70 | 33% | 6.5 MB | 1.1 MB (18%) | 6.2 MB (96%) | 59% | 66% | +6 pp |
| ogbl-citation2 | 0.56 | 10% | 11.2 MB | 3.5 MB (32%) | 11.1 MB (100%) | 28% | 34% | +6 pp |
| in-2004 | 0.72 | 26% | 5.3 MB | 1.4 MB (27%) | 5.2 MB (99%) | 62% | 67% | +5 pp |
| indochina-2004 | 0.74 | 36% | 28.3 MB | 7.5 MB (26%) | 28.1 MB (99%) | 37% | 42% | +5 pp |
| wiki-topcats | 0.55 | 14% | 6.8 MB | 1.9 MB (28%) | 6.8 MB (100%) | 44% | 49% | +5 pp |
| uk-2002 | 0.62 | 11% | 70.7 MB | 24.4 MB (35%) | 70.6 MB (100%) | 7% | 12% | +4 pp |
| com-Youtube | 0.71 | 36% | 4.3 MB | 0.6 MB (15%) | 4.0 MB (92%) | 74% | 78% | +3 pp |
| wiki-Talk | 1.00 | 96% | 9.1 MB | 0.2 MB (2%) | 2.5 MB (27%) | 98% | 100% | +2 pp |
| road-USA | 0.21 | 2% | 91.3 MB | 45.5 MB (50%) | 91.3 MB (100%) | 3% | 2% | -0 pp |
| urand23 | 0.10 | 2% | 32.0 MB | 16.8 MB (52%) | 32.0 MB (100%) | 7% | 5% | -2 pp |
| web-Google | 0.57 | 6% | 3.5 MB | 1.3 MB (38%) | 3.5 MB (100%) | 68% | 66% | -2 pp |
| higgs-twitter | 0.61 | 10% | 1.7 MB | 0.5 MB (29%) | 1.7 MB (100%) | 100% | 94% | -6 pp |
| web-BerkStan | 0.57 | 10% | 2.6 MB | 0.7 MB (25%) | 2.6 MB (99%) | 86% | 80% | -6 pp |
| roadNet-CA | 0.19 | 2% | 7.5 MB | 5.5 MB (73%) | 7.5 MB (100%) | 30% | 22% | -8 pp |
| urand21 | 0.10 | 2% | 8.0 MB | 4.2 MB (52%) | 8.0 MB (100%) | 26% | 19% | -8 pp |

Reading:
- **Biggest uplift (+15..+23 pp)**: very skewed (Gini >= 0.89, top 1% hold > 50% of edges). Hubs packed are only
  ~10% of the array (0.8-3.4MB, ~LLC size), but scattered in random order they touch 81-85% of the 64B lines, so 64B
  wastes most of its capacity and 8B recovers it. kron23, flickr-growth and kron21 (not yet simulated) join
  sx-stackoverflow (pr HRU -41%) here.
- **Moderate skew (LiveJournal family, cit-Patents, com-Orkut, indochina-2004)**: hubs are 24-35% of the array and
  touch 96-100% of its 64B lines. 8B still helps (+5..+14 pp), but the packed hub set (4-24MB) exceeds the LLC, so
  capture tops out at 33-42% (uk-2002: 12%).
- **Most skewed is not best**: wiki-Talk's hubs are 0.2MB and fit at either granularity (64B already 98%).
- **8B loses (-2..-8 pp)** where there is nothing to pack: flat graphs (urand, road: "hubs" are half the vertices)
  and graphs that already fit (higgs-twitter, web-Google, web-BerkStan: 64B captures 66-100%). Iso-area 8B has
  ~37% less data capacity.
- Skew here is out-degree; follower graphs (higgs-twitter, flickr-growth) carry most of their skew in in-degree
  (higgs max in-degree 51K vs out 1,259), so this understates them for in-edge gathers.

## 5. bfs vs bc on the same graph: higgs-twitter and kron21 (2026-10-15)

New graphs, random order, 3-level, PHRU psel vs SRRIP: bfs higgs-twitter **-11.6%**, kron21 +2.0%; bc higgs-twitter
-0.4%, kron21 **-9.9%** (best area-fair bc kron21: hub_oracle -27.5%, HRU -25.8%). flickr-growth / in-2004: bfs -0.7% /
-2.5%, bc -1.3% / -1.8% (bc flickr-growth hub_oracle -14.1%).

What each kernel reuses (PCs map to `Brandes` in `gapbs/bc`; bfs schedule from replaying GAP's DOBFS rule, alpha 15 /
beta 18, from the traced source):
- bfs: top-down `parent[v]` (PC `...c06`) only; bottom-up reuses just the frontier bitmap.
- bc: Brandes forward pass is plain top-down (no direction switch, walks every reachable edge); the long-gap reuse is
  the backward pass gathering `deltas[v]` (4B, PC `...727`) and `path_counts[v]` (8B, `...72d`) once per predecessor -
  a pr-style gather (61-84% of bc's long-gap reuse on all 4 graphs).

| | higgs-twitter | kron21 | flickr-growth | in-2004 |
|---|---|---|---|---|
| Vertices / avg deg / max deg | 457K / 32.5 / 1,259 | 2.1M / 30.3 / 102,440 | 2.3M / 14.4 / 26,367 | 1.4M / 12.0 / 7,752 |
| Top-1% edge share | 10% | 52% | 52% | 26% |
| bfs top-down share of edge work | **52%** | 0.4% | 1.0% | 5.4% |
| bfs TD `parent[]` long-gap re-reads / 1K instr | **12.5** | 0.02 | 0.25 | 1.16 |
| bfs TD `parent[]` footprint | **1.8MB** | 0.7MB | 2.7MB | 5.5MB |
| bc backward-gather long-gap reuses / 1K instr | 13.5 | **30.0** | 24.3 | 12.6 |
| bc: share of long-gap reuse on above-average-degree vertices | 45% | **68%** | 43% | 17% |
| 8B uplift (reachable gathers: capture @64B -> @8B) | 100% -> 99% (-1 pp) | **73% -> 87% (+15 pp)** | 70% -> 84% (+15 pp) | 81% -> 85% (+4 pp) |

- higgs-twitter bfs wins without hubs: small graph with a broad middle -> 4 TD levels before switching and 6 after;
  half the edge work is TD `parent[]` checks, and the whole `parent[]` (1.8MB) ~ LLC, so keeping it resident against
  streaming neighbour lists pays. bc on higgs gathers ~7MB of flat-degree data: no subset worth keeping (uplift -1 pp).
- kron21 bfs loses: hubs blow the frontier up (10K -> 890K at level 3), BFS goes bottom-up at once (99.6% of edge
  work). kron21 bc wins: never switches direction, walks all 63.5M edges, most hub-concentrated graph measured.
- flickr-growth bc has the same uplift as kron21 but only hub_oracle wins: HRU/PHRU don't find its hubs (as with bc on
  com-Orkut). Lower hub share of reuse (43% vs 68%) is consistent with that.
- Rule: hubs help bc (and pr/sssp/cc; gain follows 8B uplift); hubs *hurt* bfs (they trigger bottom-up). bfs gains only
  when top-down steps dominate and their `parent[]` working set is near LLC size.
- Not separating: share of long-gap reuse on the top 32K 64B lines (bc higgs 46% > kron21 35%) - at 64B in random
  order hub data shares lines with cold vertices, which is exactly what 8B fixes.
- Default / hubsort orders not run for these graphs yet (traces exist in `traces_roi`).

### 5a. Same graphs in default and hubsort order (3-level, best area-fair vs SRRIP in that order)

| | bc random | bc default | bc hubsort | bfs random | bfs default | bfs hubsort |
|---|---|---|---|---|---|---|
| kron21 | -27.5% (SRRIP 29.8) | -27.7% (29.8) | -8.8% (16.1) | +0.5% | +0.2% | -0.1% |
| flickr-growth | -14.1% (39.3) | -4.2% (20.1) | -2.8% (20.7) | -0.7% | -0.8% | -1.1% |
| higgs-twitter | -1.9% (16.2) | +4.9% (10.7) | +0.2% (12.1) | -11.6% (17.5) | -1.8% (13.7) | -4.8% (14.2) |
| in-2004 | -2.6% (38.1) | +0.8% (8.2) | -0.5% (10.4) | -2.5% | -0.8% | -1.8% |

(SRRIP-64B LLC MPKI in parentheses.) Raw: `results_2026-10-15/bfsbc4_orders/summary.md`.
- A good vertex order removes most of what 8B buys: random order wastes 64B lines on cold neighbours; default
  (crawl/community) order or hubsort packs the hot data, cutting SRRIP's own MPKI by up to ~2x (bc flickr 39 -> 20,
  bc kron21 30 -> 16 with hubsort). The remaining policy gains are small, except bc kron21.
- kron21's default order behaves like random (same SRRIP MPKI and gains); hubsort is what packs its hubs.
- higgs-twitter's bfs win is mostly a random-order effect (-11.6% -> -1.8% default / -4.8% hubsort): with locality,
  `parent[]` lines are denser and SRRIP already keeps them.
- Hubsort is worse than default for SRRIP on the non-synthetic graphs (it breaks community locality for non-hubs).

## 6. Re-test with all 14 graphs (2026-10-16)

Random order, 3-level, best area-fair vs SRRIP. Added graphs: higgs-twitter, flickr-growth, in-2004, kron21 (bc, bfs,
pr, sssp) and urand21 (all 7 apps). Scripts / data: `results_2026-10-15/uplift_all.py` (8B uplift for all graphs),
`results_2026-10-15/hypothesis.py` (gain table + correlations).

| Kernel | graphs | r(8B uplift, best gain) | r(uplift, PHRU psel) | r(skew, best) | outlier | r without it |
|---|---|---|---|---|---|---|
| pr | 12 | **-0.85** | -0.86 | -0.49 | (higgs-twitter fits: +7% at -6 pp) | |
| pr_spmv_clean | 8 | -0.73 | -0.76 | -0.30 | | |
| sssp | 12 | -0.65 | -0.51 | -0.29 | higgs-twitter (-15% at uplift -6 pp) | -0.77 |
| cc_sv | 8 | -0.75 | -0.83 | -0.54 | | |
| bc | 12 | -0.68 | -0.33 | -0.19 | | |
| cc | 8 | +0.29 | +0.26 | +0.60 | urand21 (-20% hub_oracle, -7% PHRU psel) | -0.64 |
| bfs | 14 | (uplift not the metric) | | | | |

bfs vs top-down `parent[]` long-gap re-reads / 1K instr: r = **-0.73** over 14 graphs (was -0.71 over 9). urand21:
6.5 / 1K (between hollywood-2009 and com-Orkut) but only -2.2% - its TD reuse is uniform over an 8.4MB footprint.

Verdict:
- Confirmed for pr, pr_spmv_clean, sssp, cc_sv, bc: 8B uplift (skew x hot-set size vs LLC) predicts the gain; skew
  alone is always weaker. New graphs fit: kron21 / flickr-growth (+15 pp) give the largest gains measured (pr -51% /
  -46%, sssp -42% / -27%); urand21 (no hubs, -8 pp) gives ~nothing (-1% to -3%).
- bc: the trend holds for the best policy (mostly hub_oracle) but not for PHRU psel (r -0.33) - online predictors
  miss bc's hubs.
- bfs refinement: top-down reuse must also be concentrated (hubs: cit-Patents, com-Orkut 68-76% of TD reuse) or have
  a footprint ~ LLC size (higgs-twitter 1.8MB). Uniform TD reuse over a large footprint (urand21) gains nothing.
- Unexplained: (1) sssp on higgs-twitter -15% with negative uplift - small graph, likely the same "array ~ LLC" effect
  as its bfs, not measured; (2) cc on urand21 -20% (hub_oracle) on a graph with no hubs - not a hub effect; needs a
  per-PC look at cc's (Afforest) accesses.
- Ordering (section 5a + pr/sssp): gains shrink sharply in default / hubsort order, except where default order is
  random-like (kron21).

## 7. Reuse analysis for cc, cc_sv, sssp (2026-10-17)

Same method as sections 4-5 for bfs/bc (`results_2026-10-15/why/reuse.py`), applied to every random-order cc / cc_sv /
sssp trace (19 graphs each) and joined with the section 6 gains (3-level, best area-fair and PHRU psel vs SRRIP-64B).
Code and full tables: `pin/pin/analysis/kernel_reuse/` (`reuse_scan.cpp` = reuse.py in C++ with full PCs, `pcmap.sh`
maps PCs to source via the byte-identical -g builds in `analysis/bfs_dbg/` (cc_sv_g added), `trace_uplift.cpp`,
`summarize.py` -> `summary.md`). urand21 (cc -20%, sssp -3%) cannot be examined: its traces/inputs were deleted 2026-10-16.

Long-gap reuse = re-touch of a 64B line >= 10K trace records later. "Gather uplift (trace)" = section 1's 8B uplift but
measured on the kernel's own per-vertex gather PCs in the trace (hottest 2MB at 64B vs iso-area 8B) instead of modelled
from out-degree. Hub = NodeDegree > PCAvgDegree.

What carries the long-gap reuse (com-Youtube; similar on all graphs):
- **cc_sv**: `comp[v]` per edge (cc_sv.cc:65) 82%, root check `comp[high_comp]` (:70) 11% (94% on hubs), shortcut
  `comp[comp[n]]` (:78) 5%. 95-100% of long-gap reuse is on these gather PCs on every graph - a pr-style gather.
- **sssp**: `dist[v]` in RelaxEdges (sssp.cc:72-74) 56% (64% hubs), bucket re-check `dist[u]` (:111) 15%, CSR index
  and neighbor lists re-read (13% each; vertices are relaxed more than once). Gather PCs carry 58-97%.
- **cc (Afforest)**: `comp[v]` in Link (cc.cc:43) 46%, Compress root lookups `comp[comp[n]]` (:62) 21-26%, Link tree
  walks `comp[high]` (:47) 5%, plus neighbor index / list starts re-read in every sampling round. Gather PCs carry only
  49-73% (9-28% on wiki-Talk, web-BerkStan, in-2004, indochina-2004).

**cc** (simulated graphs; all graphs in `summary.md`)

| Graph | Best area-fair vs SRRIP | PHRU psel | Degree uplift | Gather uplift (trace): @64B -> @8B | Gather data (8B blocks) | Long-gap reuse / 1K instr | of it on gather PCs | on hubs | on top 32K lines |
|---|---|---|---|---|---|---|---|---|---|
| sx-stackoverflow | -14.7% | -4.7% | +16 pp | 60% -> 63% (+3 pp) | 9.9 MB | 33.8 | 52% | 54% | 49% |
| com-Orkut | -14.1% | -7.5% | +6 pp | 69% -> 76% (+7 pp) | 11.7 MB | 40.8 | 61% | 45% | 44% |
| soc-Pokec | -11.6% | -8.8% | +7 pp | 66% -> 68% (+2 pp) | 6.2 MB | 43.5 | 61% | 50% | 48% |
| com-Youtube | -10.7% | -10.7% | +3 pp | 80% -> 78% (-2 pp) | 4.3 MB | 30.3 | 72% | 42% | 65% |
| as-Skitter | -8.6% | -8.6% | +6 pp | 74% -> 76% (+1 pp) | 6.5 MB | 33.5 | 68% | 43% | 53% |
| cit-Patents | -7.8% | -7.3% | +8 pp | 41% -> 40% (-1 pp) | 14.4 MB | 51.5 | 49% | 57% | 40% |
| wiki-Talk | -4.8% | -4.8% | +2 pp | 29% -> 22% (-7 pp) | 9.1 MB | 14.2 | 9% | 56% | 67% |

**cc_sv** (simulated graphs; all graphs in `summary.md`)

| Graph | Best area-fair vs SRRIP | PHRU psel | Degree uplift | Gather uplift (trace): @64B -> @8B | Gather data (8B blocks) | Long-gap reuse / 1K instr | of it on gather PCs | on hubs | on top 32K lines |
|---|---|---|---|---|---|---|---|---|---|
| sx-stackoverflow | -24.5% | -19.9% | +16 pp | 71% -> 78% (+8 pp) | 9.9 MB | 67.9 | 99% | 78% | 51% |
| as-Skitter | -14.4% | -9.7% | +6 pp | 70% -> 73% (+3 pp) | 6.5 MB | 73.6 | 99% | 70% | 57% |
| com-Youtube | -14.1% | -10.1% | +3 pp | 80% -> 80% (-0 pp) | 4.3 MB | 59.7 | 98% | 70% | 73% |
| wiki-Talk | -11.2% | -6.7% | +2 pp | 62% -> 62% (-0 pp) | 9.1 MB | 40.0 | 95% | 50% | 47% |
| soc-Pokec | -7.8% | -7.0% | +7 pp | 70% -> 73% (+3 pp) | 6.2 MB | 74.5 | 99% | 79% | 51% |
| cit-Patents | -7.4% | -6.9% | +8 pp | 49% -> 50% (+2 pp) | 14.4 MB | 76.5 | 98% | 74% | 41% |
| com-Orkut | -5.6% | -5.1% | +6 pp | 61% -> 65% (+3 pp) | 11.7 MB | 77.3 | 100% | 73% | 32% |

**sssp** (simulated graphs; all graphs in `summary.md`)

| Graph | Best area-fair vs SRRIP | PHRU psel | Degree uplift | Gather uplift (trace): @64B -> @8B | Gather data (8B blocks) | Long-gap reuse / 1K instr | of it on gather PCs | on hubs | on top 32K lines |
|---|---|---|---|---|---|---|---|---|---|
| kron21 | -42.4% | -29.2% | +15 pp | 72% -> 86% (+14 pp) | 6.7 MB | 59.1 | 96% | 83% | 68% |
| flickr-growth | -27.0% | -20.7% | +15 pp | 67% -> 80% (+13 pp) | 8.7 MB | 54.8 | 89% | 71% | 58% |
| sx-stackoverflow | -21.5% | -15.3% | +16 pp | 48% -> 62% (+14 pp) | 9.8 MB | 60.7 | 90% | 65% | 42% |
| com-Orkut | -15.9% | -9.0% | +6 pp | 28% -> 35% (+6 pp) | 11.7 MB | 53.8 | 97% | 67% | 27% |
| higgs-twitter | -15.2% | -15.2% | -6 pp | 100% -> 98% (-2 pp) | 1.7 MB | 40.6 | 93% | 68% | 94% |
| com-Youtube | -10.8% | -9.7% | +3 pp | 70% -> 70% (+0 pp) | 4.3 MB | 54.5 | 71% | 37% | 45% |
| soc-Pokec | -10.7% | -5.1% | +7 pp | 46% -> 53% (+7 pp) | 6.2 MB | 54.9 | 91% | 65% | 40% |
| as-Skitter | -8.7% | -3.0% | +6 pp | 57% -> 62% (+5 pp) | 6.5 MB | 42.8 | 82% | 43% | 37% |
| cit-Patents | -8.1% | -1.7% | +8 pp | 51% -> 87% (+36 pp) | 1.6 MB | 34.3 | 87% | 38% | 49% |
| wiki-Talk | -6.2% | -5.1% | +2 pp | 38% -> 38% (-0 pp) | 9.1 MB | 54.4 | 74% | 25% | 28% |
| in-2004 | -5.6% | -5.6% | +5 pp | 77% -> 80% (+3 pp) | 5.3 MB | 19.2 | 58% | 12% | 26% |

Correlation with gain (r best / r PHRU psel; negative = predicts larger gains):

| Kernel | graphs | degree uplift | gather uplift (trace) | hub share | top-32K share | long-gap / 1K instr |
|---|---|---|---|---|---|---|
| cc_sv | 7 | -0.61 / -0.78 | -0.61 / -0.76 | -0.11 / -0.32 | -0.47 / -0.35 | +0.21 / +0.02 |
| sssp | 11 | -0.62 / -0.46 | -0.20 / -0.00 | -0.79 / -0.72 | -0.50 / -0.60 | -0.54 / -0.48 |
| sssp without cit-Patents, higgs-twitter | 9 | -0.84 / -0.82 | -0.83 / -0.78 | -0.81 / -0.70 | -0.87 / -0.88 | -0.51 / -0.46 |
| cc | 7 | -0.63 / +0.43 | -0.87 / -0.14 | +0.32 / +0.76 | +0.45 / -0.06 | -0.41 / -0.33 |

Verdict:
- **cc_sv is pr's twin**: virtually all long-gap reuse is the `comp[]` gather, 70-87% on hubs. Gains follow uplift
  (degree or trace, r ~ -0.6 best, -0.8 PHRU), as for pr. The long-gap rate (40-77 per 1K) predicts
  nothing. Not explained by uplift: com-Youtube (-14%) and wiki-Talk (-11%) at ~0 pp.
- **sssp is also a gather kernel** once two special cases are set aside; then every measure predicts the gain
  (r -0.81..-0.87):
  - higgs-twitter (-15% at -2 pp uplift; the open question in section 6): its whole `dist[]` is 1.7MB < LLC and 94%
    of long-gap reuse falls on the hottest 32K lines - the gain is keeping the array resident against streaming
    neighbor lists, not 8B packing. Same mechanism as its bfs win.
  - cit-Patents (+36 pp trace uplift, only -8%): sssp reaches ~216K vertices; the gather touches 1.6MB and the
    kernel has the lowest gather intensity, so there is little to win.
  - Hub share is the best single predictor for sssp (r -0.79 even with both): dist[v] is read once per in-edge.
- **cc (Afforest) is not a clean gather kernel**: sampling links only each vertex's first two neighbors, so gathers are
  not degree-proportional - the trace-measured uplift is small everywhere (-7..+7 pp) even where the degree model
  says +16 pp (sx-stackoverflow: +3 pp measured). Root lookups in Compress and per-round index re-reads carry a third
  to half of the long-gap reuse. Trace uplift tracks the best policy (r -0.87, mostly hub_oracle) but not PHRU psel;
  with 7 graphs and gains spanning only -5..-15%, cc is only weakly explained. (Section 6's r = +0.29 included
  urand21; without it -0.64 - consistent.)
- Overall: for the gather kernels (pr, pr_spmv, cc_sv, sssp, bc) the benefit is set by how much hot per-vertex data
  8B can pack into the LLC; the exceptions are graphs whose whole per-vertex array fits (higgs-twitter) or whose
  traversal touches little of the graph (cit-Patents sssp/bfs).
