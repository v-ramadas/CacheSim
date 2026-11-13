# Base+Delta tags in a repurposed 64B set: layout, lookup, fill, area

Status: design, not implemented (2026-11-11). Built up as Amoeba-Cache (MICRO'12) Fig. 4 / Fig. 5-style figures.
Geometry: 1024 sets x 32 ways (2 MB), 8 bases, 10-bit delta.

## Idea

A 64B set's own SRAMs are reused unchanged in 8B mode, through three levels of naming:

| level | lives in | names | per set |
|---|---|---|---|
| base | tag entries 0-7 | a 48b line tag; window of 1024 lines (64 KB) around it | 8 |
| line entry | tag entries 8-31, packed | one 64B line: base id + delta | 82 |
| block | data array: 12b metadata (metadata columns) + 64b word (data columns) | one 8B word: sector, pointer to its line entry, data | 216 |

All resident words of one 64B line share one line entry; that sharing is the tag saving. The sector is stored per
block, not in the line entry (otherwise every word needs its own line entry and only 72 blocks fit).

## Baseline (sectored 64B set)

- Tag array: 32 x 48b tag = 1,536b.
- Data array: 32 ways x 8 sectors x (V 1 + D 1 + 64b) = 16,896b. Valid and dirty are per-8B-sector state, so they
  sit with the data words, not in the tag entry. Physically the 512 V/D columns are grouped at the end of the row.
- Total 18,432b per set. The simulator's `get_total_cache_size` counts 48b tag + 8 sector-valid bits per way
  (18,176b, no dirty); adding dirty does not change its results (BDI 216, full-tag 160).
- Replacement state (RRPV/LRU) is not counted on either side.

## Layout (split: metadata columns + data columns)

```
 ADDRESS SPACE                                     ADDRESS (64-bit, 8B blocks)
                                                    63                16 15       6 5     3 2  0
         base_k-512        base_k        +511      ┌────────────────────┬──────────┬───────┬────┐
  ──────┬───────────────────┬─────────────┬───     │  line tag L (48b)  │ set(10b) │sector │byte│
        │░░░░░░░░░░░░░░░░░░░│░░░░░░░░░░░░░│        └────────────────────┴──────────┴───────┴────┘
        └─ window of one base: 1024 lines ┘            L = base[id] + delta          S
           = 64 KB of memory

 ════════════════════════════════════════════ ONE SET (x1024) ═════════════════════════════════════

 TAG ARRAY ROW   32 entries x 48b = 1,536b
 ┌──────┬──────┬─────┬──────┬──────────────────┬──────────────────┬─────┬──────────────────┐
 │ b0   │ b1   │ ... │ b7   │ entry 8          │ entry 9          │ ... │ entry 31         │
 │ 48b  │ 48b  │     │ 48b  │ ln0 │ ln1 │ ln2 │l│n3 │ ln4 │ ln5 │l│n6 │ │ ... │  ln80 │ ln81 │…│
 └──────┴──────┴─────┴──────┴──────────────────┴──────────────────┴─────┴──────────────────┘
  └──── 8 BASES ─────┘      └──────── LINE-ENTRY POOL: 24 x 48b = 1,152b ─────────────────┘
   one 48b line tag each     82 x 14b = 1,148b (4b spare); entries cross 48b borders

     base  b_k  = │      line tag (48b)      │            in use = some valid ln_j has id k
     line  ln_j = │V│ id(3) │ delta(10) │      14b      L = base[id] + delta

 DATA ARRAY ROW  16,896 columns
 64B mode: ┌──────────── data, ways 0-31: 32 x 512b ─────────────────┬─ V/D, ways 0-31: 512b ─┐
           │ way0 │ way1 │ ...                       │ way27 │...│w31│ V D V D ...            │
           └──────────────────────────────────────────────────────────────────────────────────┘
 8B mode:  ┌──── DATA COLUMNS: ways 0-26 = 13,824b ─────────┬── METADATA COLUMNS: 3,072b ──────┐
           │ w0│w1│w2│...  216 x 64b words                │ meta0 │ meta1 │ ... │ meta215 │ … │
           │ block i = way i/8, word i%8 (64b aligned)      │ 216 x 12b = 2,592b (480b spare) │
           └────────────────────────────────────────────────┴─────────────────────────────────┘
             normal column muxing: read 1 word               every column sensed: read all 216

     meta_i = │V│D│ sector(3) │ ptr(7) │   12b        data_i = │ 64b │  at way i/8, word i%8

 ═════════════════════════════════════════ EXAMPLE CONTENTS ═══════════════════════════════════════

   b0  = 0x7F3A1200        b1 = 0x7F3A9C40        b2..b7 unused

   ln0 = (1, b0,   +3) ─► line A = 0x7F3A1203 ◄──┬── meta0 = (1, ln0, sector 5)   data0 = word 5 of A
                                                 ├── meta1 = (1, ln0, sector 6)   data1 = word 6 of A
                                                 └── meta2 = (1, ln0, sector 7)   data2 = word 7 of A
   ln1 = (1, b0, +250) ─► line C = 0x7F3A12FA ◄───── meta3 = (1, ln1, sector 2)   data3 = word 2 of C
   ln2 = (1, b1,  -17) ─► line B = 0x7F3A9C2F ◄───── meta4 = (1, ln2, sector 0)   data4 = word 0 of B
   ln3..ln81: V = 0                                  meta5..meta215: V = 0

 ═══════════════════════════════════════════ BIT BUDGET / SET ═════════════════════════════════════

                      64B baseline (sectored)            8B Base+Delta
   tag array          32 x 48b tag       =  1,536      8 x 48b + 82 x 14b      =  1,532 (+4 spare)
   data array         32 x 8 x (V+D+64b) = 16,896      216 x 64b + 216 x 12b   = 16,416 (+480 spare)
   capacity           32 x 64B = 2 KB                  216 x 8B = 1.69 KB (84.4%)
   limits             32 lines                         8 bases, 82 lines, 216 words
```

Caption (Amoeba style). Left: a base is a 48b line tag covering a window of 1024 lines (64 KB). Right: the set index
comes from the 64B-line bits, so all 8 words of a line map to one set; L = line tag, S = sector. Set: tag entries
0-7 hold 8 bases; entries 8-31 hold 82 packed 14b line entries (V, base id, delta). The data array is split: the
data columns of ways 0-26 hold 216 aligned 8B words; the remaining 3,072 columns (data of ways 27-31 plus the V/D
columns) hold 216 packed 12b metadata fields (V, D, sector, pointer to a line entry). A word's line tag is
base[id] + delta; the words of a line share one line entry.

## Lookup (Amoeba Fig. 5 equivalent)

```
 ADDR ┌────────────────────┬──────────┬────────┬──────┐
      │  L  line tag (48b) │ set(10b) │ S (3b) │ byte │
      └─────────┬──────────┴────┬─────┴───┬────┴──────┘
                │               │ ❶       │
                │               ▼         │
                │        ┌─────────────┐  │
                │        │ row decoder │──┼────────────────────────────┐ same set row
                │        └──────┬──────┘  │                            │
                │               ▼         │                            ▼
                │   TAG ARRAY ROW (1,536b)│               METADATA COLUMNS (3,072b)
                │   ┌──────────────┬──────┴───────────────┐ ┌─────────────────────────────┐
                │   │ b0 … b7      │ ln0 … ln81           │ │ meta0 … meta215 (12b each)  │
                │   └──────┬───────┴──────────┬───────────┘ └──────────────┬──────────────┘
                │          │ 8 x 48b          │ 82 x (V,id,delta)          │ 216 x (V,D,S,ptr)
   ❷            ▼          ▼                  │                            │
   ┌──────────────────────────────────┐       │                            │
   │ 8 SUBTRACTORS (48b), in parallel │       │                            │
   │   d_k = L - b_k                  │       │                            │
   │   fits_k = -512 <= d_k < 512     │       │                            │
   │   (bits 47..9 of d_k all 0/all 1)│       │                            │
   └───────────────┬──────────────────┘       │                            │
                   │ 8 x (fits_k, d_k[9:0])   │                            │
   ❸               ▼                          ▼                            │
   ┌──────────────────────────────────────────────────────────┐            │
   │ 82 LINE MATCHERS, in parallel                            │            │
   │   ln_j hit = V_j & fits[id_j] & (delta_j == d[id_j])     │            │
   │   each: 8:1 mux (by id_j) + 10b compare                  │            │
   └───────────────┬──────────────────────────────────────────┘            │
                   │ hit_ln[81:0]  (at most one-hot: one entry per line)   │
                   ▼                                                       │
            ┌──────────────┐   none ──► MISS (line not resident)           │
            │ 82->7 encoder│                                               │
            └──────┬───────┘                                               │
                   │ j (7b)                         S (3b)                 │
   ❹               ▼                                  ▼                    ▼
   ┌─────────────────────────────────────────────────────────────────────────────────┐
   │ 216 BLOCK MATCHERS, in parallel                                                 │
   │   blk_i hit = V_i & (ptr_i == j) & (sector_i == S)      7b + 3b compare each    │
   └───────────────┬─────────────────────────────────────────────────────────────────┘
                   │ hit_blk[215:0] -> encode i (8b)         none ──► MISS (line resident,
   ❺               ▼                                                     word not resident)
   ┌──────────────────────────────────────────────┐
   │ DATA COLUMNS: read way i/8, word i%8 (64b)   │──► 8B data out   (write hit: data in, set D_i)
   └──────────────────────────────────────────────┘

   critical path:  ❶ decode ─► tag row + metadata read ─► ❷ 48b subtract + window ─► ❸ 8:1 mux +
                   10b cmp ─► 82->7 encode ─► ❹ 7b+3b cmp ─► encode i ─► ❺ one-word data read
```

Steps:
1. The set index (10 bits, from the 64B-line address) raises one wordline; the tag row (8 bases, 82 line entries)
   and the metadata columns (216 x 12b) are read together.
2. 8 x 48b subtractors: d_k = L - b_k; base k fits if d_k is in the 10-bit window (bits 47-9 all 0 or all 1).
3. 82 line matchers: each muxes its base's (fits, d) by its id and compares its 10b delta. At most one hits (one
   entry per line); encoded to a 7b index j. None -> miss.
4. 216 block matchers on the metadata: ptr == j and sector == S. At most one hits; encoded to i.
5. One 64b word is read from the data columns (way i/8, word i%8), like a normal way read with a word column mux;
   a write hit also sets D_i in the metadata.

vs Amoeba Fig. 5: Amoeba finds tags via the T? bitmap and routes adjacent words to N/2 comparators (50b region ==,
3b range), reading the whole row. Ours has fixed positions (no bitmap, no routing muxes), shared 8 x 48b
subtractions, then 82 x 10b and 216 x 10b compares (two levels: line entry -> block), and reads only one data word.

Two misses:
- Line miss (step 3): no line entry. Fill allocates a line entry (and maybe a base). Known from the tag row alone.
- Word miss (step 4): line present, word not. Fill reuses line entry j; needs only a block slot.

Timing: with the split layout the lookup is serial by construction: tag row + metadata (wide read), logic, then a
one-word data read. CACTI (22nm lstp): tag row 1.64 ns, metadata 2.34 ns, data word 3.56 ns -> about
2.34 + logic + 3.56 = ~5.9 ns + logic, vs the 64B cache's 5.36 ns (sequential). The earlier packed layout could
overlap the data read but needs the whole 16,896b row sensed (see Area).

## Fill / eviction

A fill needs up to three resources, coarse to fine: a base (8), a line entry (82), block slots (216). A miss only
acquires what it lacks.

```
 MISS on word S of line L        (the fill brings k words of line L; k = 1..8)
   │
   ├─ step ❸ found line entry j? ── yes ─► WORD MISS ──────────────────────────────┐
   │                                                                               │
   no: LINE MISS                                                                   │
   │                                                                               │
   ▼                                                                               │
 ┌─ A. BASE ─────────────────────────────────────────────────────────────────┐     │
 │ some fits_k (from step ❷)?        yes ─► use base k, delta = d_k          │     │
 │ no: some base free?               yes ─► b_k := L, delta = 0              │     │
 │     (free_k = no valid ln has id k: 8 OR-reductions over 82 id fields)    │     │
 │ no: BASE REPLACEMENT                                                      │     │
 │     pick base v (rule: see below)                                         │     │
 │     evict every ln with id v, and every block pointing to those ln        │     │
 │     b_v := L, delta = 0                                                   │     │
 └──────────────────────────────┬────────────────────────────────────────────┘     │
                                ▼                                                  │
 ┌─ B. LINE ENTRY ───────────────────────────────────────────────────────────┐     │
 │ some ln with V = 0?               yes ─► take it                          │     │
 │ no: LINE EVICTION                                                         │     │
 │     pick line entry v (rule: see below)                                   │     │
 │     evict every block with ptr == v (≤ 8 blocks)                          │     │
 │ write ln_j := (V=1, id, delta)                                            │     │
 └──────────────────────────────┬────────────────────────────────────────────┘     │
                                ▼                                                  ▼
 ┌─ C. BLOCK SLOTS (k of them) ──────────────────────────────────────────────────────┐
 │ each: some blk with V = 0?        yes ─► take it                                  │
 │       no: BLOCK EVICTION ─► victim by the replacement policy (RRIP over 216)      │
 │ write meta := (V=1, D=0, sector, ptr = j);  data word := fill data                │
 └────────────────────────────────────┬──────────────────────────────────────────────┘
                                      ▼
                                    done

 EVICTING ONE BLOCK i (used by steps A, B, C)
   D_i = 1 ?  ─► write back 8B to address  (b[id] + delta) · set · sector_i
   V_i := 0
   last block of its line?  ─► ln_ptr_i.V := 0      (OR over 216 "ptr == ptr_i" compares,
                                                     the same comparators as lookup step ❹)
   last line of its base?   ─► base becomes free    (derived, nothing to write)
```

Collateral evictions (a plain cache evicts 1 block per fill):

| event | blocks evicted | when |
|---|---|---|
| block eviction | 1 | block slots full (normal) |
| line eviction | 1-8 (all words of the victim line) | 82 line entries full |
| base replacement | all blocks under the victim base | 8 bases full and none covers L |

The simulator models the first and last today (`bdi_reserve`; 0 base replacements on pr). Line eviction is new
with this layout and is the cost of the 82-line cap.

Victim rules to decide:
1. Base: the simulator uses fewest blocks (8 counts over 216 ptrs: costly). Cheaper: (a) fewest line entries
   (8 popcounts over 82 ids), or (b) the base of the replacement policy's victim block.
2. Line: (a) the line of the RRIP victim block (no new state), or (b) the line with the fewest resident words
   (frees an entry losing the least data; suits 1-word-per-line graph traces).
3. Order A -> B -> C: base and line evictions can free block slots.

Write-back rebuilds the address from b[id] + delta, set and sector, all in the tag row already read for the lookup.

## Area (2026-11-11)

Reference: CACTI 7, 22nm, itrs-lstp, 2 MB / 32-way / 64B cache = 2.24 (data) + 0.16 (tag) = **2.40 mm²**
(`cacti/runs_bdi/c64.out`).

Logic (hand estimate, NAND2-equivalents; one copy per lookup port/bank, shared by all 1024 sets):

| block | count | GE each | total GE |
|---|---|---|---|
| 48b subtractor + window detect | 8 | ~780 | ~6.2K |
| line matcher: 8:1 mux x 11b + 10b compare | 82 | ~210 | ~17.2K |
| 82 -> 7 encoder | 1 | | ~0.3K |
| block matcher: 7b + 3b compare | 216 | ~34 | ~7.3K |
| fill: free-base detect, free-slot priority encoders, base popcounts, write-back adder | | | ~7.5K |
| total | | | **~38K GE** |

At ~0.4 µm²/GE (22nm, with layout overhead): ~0.015 mm², ~0.6% of the LLC (x number of banks with their own
port). For reference the baseline's 32 x 48b comparators are ~4.5K GE. The split layout needs no 216:1 x 64b
word select (one word is read from the data columns with the normal column mux); the packed layout needed one
(~37K GE, replacing the baseline's ~43K GE 32:1 x 512b way mux).

Arrays (CACTI runs `cacti/runs_bdi/gen_split.py`, outputs `split_*.out`, `packed_fullrow*.out`):

| design | arrays | area (mm²) | read energy / access (nJ) |
|---|---|---|---|
| 64B cache (baseline) | tag + data | 2.40 | 0.19 |
| split layout | tag row 1,536b (0.199) + metadata 3,072b full-width (0.519) + data 13,824b one-word (1.725) | **2.44 (+1.8%)** | 0.44 (0.092 + 0.315 + 0.032) |
| packed layout (first draft) | data row 16,896b read full-width | 19.9 (data alone) | 12.1 |

- Split layout total with logic: ~2.46 mm², about +2.5% over the 64B cache.
- The packed layout (block metadata interleaved with data, whole row sensed) is impractical per CACTI (~8x the
  cache, 7% array efficiency): every column needs a sense amp and the full row is routed out.
- Energy: the split lookup reads ~2.3x the 64B cache's per-access energy, dominated by the 3,072b metadata read.
- RAM-mode CACTI models the tag row as a full-width RAM (0.199 mm² vs 0.165 for the cache's tag array), so the
  +1.8% is slightly pessimistic.

## Choices and variants

- **Valid bits kept** (chosen, 2026-11-11): 82 line entries, 216 blocks (exactly the simulator's 216). Free slot =
  V 0. Bases have no room for V (48b used); "base in use" is derived from the line entries' ids. No refcounts stored.
- **No valid bits** (footnote variant): line entry 13b, metadata 11b with reserved `ptr = 127` = empty; a line entry
  is live iff some block points to it (a stale entry can only match a lookup that then finds no block = correct
  miss). Gives 88 line entries and 224 blocks.
- **Split vs packed** (chosen: split): packed 76b blocks crossing way borders (first draft) need a full-row read;
  splitting metadata (3,072 columns) from data (216 aligned words) keeps the same bit counts and reads one word.
- 222 blocks also fit with valid bits; 216 rounds down to a multiple of 8 as the simulator does.
- Rejected: sector in the line entry (one entry per word -> 1,152 / 16b = 72 blocks).
- Earlier sketch (2026-11-04, superseded): bases + 17b per-block tags in the tag array + data ways 27-31, ~208
  blocks once RRPV + dirty counted. Its CACTI energy estimate is in `cacti/runs_bdi/energy_summary.txt`.

## Open items

1. **82-line cap per set**: filling 216 blocks needs >= 2.6 resident words per line on average. Graph traces with
   1 word per line (non-hub vertices, sssp/bc frontiers) could hit the cap first and fall toward 82 blocks. Measure
   distinct lines per set (and line-eviction rate) in the simulator before relying on 216.
2. Simulator model: currently 216 blocks with no line cap; decide whether to model the cap, line evictions and the
   victim rules above (and 224 for the no-valid variant).
3. Energy model: update `cacti/runs_bdi/energy.py` for the split lookup (0.44 nJ per lookup, one-word data access).
4. Parallel vs serial (tag_bdi_followups task 1): the split layout makes the lookup serial; latency ~5.9 ns + logic
   vs 5.36 ns.
