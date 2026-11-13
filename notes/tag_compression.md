# LLC tag compression under iso-area (8B block size)

## Background

Earlier work (see `phru_hub_heuristic_quality.md`, `ghost_cache_tuning.md`)
tuned the ghost cache's insertion threshold and eviction policy. This note
covers a separate, independent investigation: the iso-area capacity penalty
itself, and whether region-based tag compression can recover it. This is
about `inc/defs.h`'s `get_total_cache_size()`/`get_iso_area_cache()` - not
the ghost cache.

## The core finding

`get_iso_area_cache()` (`inc/defs.h:138`) is not theoretical - it is *live*
in every HRU/PHRU run in this codebase, at cache-construction time
(`src/cachesim.cc:36`, used unconditionally for every plain non-dueling
run) and again in `Set::set_breakdown()` (`src/set.cc:361`) during dynamic
set-dueling transitions. `cachesim::isoArea` is `true` for HRU/PHRU by
default (`src/main.cc:161-171`, unless `--no-iso-area` is passed), so every
HRU/PHRU run at `--cache-block-size 8` this whole project has been running
with an iso-area-adjusted way count, not the raw `--num-cache-ways` CLI
value.

For `S=1024 sets, W=32 ways` (this project's standard config): the 64B
reference budget is 2,293,760 bytes. At 8B, uncompressed tags need 51
bits/block, giving an iso-area way count of **160** (0.625x the reference
data capacity - see the iso-area derivation earlier in this session).

## Region-based tag compression (prototype, `--use-tag-compression`)

Instead of a full 51-bit tag per 8B block, store a pointer into a small
region buffer (`log2(N)` bits) plus the bits between the region boundary
and the set-index/offset boundary. Sized initially using SHiP-Mem's
published parameters (Wu et al., MICRO 2011): N=16384 regions, R=16KB
region size - **but see the caveat below, this citation is weaker than it
looks.** Implemented in `get_total_cache_size()` (`inc/defs.h`), gated by
`cachesim::USE_TAG_COMPRESSION` / `--use-tag-compression`, with
`--tag-compression-region-size` and `--tag-compression-region-entries`
also CLI-tunable. This is an **idealized** prototype - assumes the region
buffer never overflows/aliases (see "Explicitly out of scope" in the
implementation plan). No real region-tracking mechanics exist yet.

With N=16384/R=16KB: compressed tag = 15 bits (vs 51 uncompressed), region
buffer = 102,400 bytes one-time. Iso-area way count recovers to **224**
(0.875x reference capacity, vs 0.625x uncompressed).

## Full 9-trace validation (HRU and PHRU, 8B, with vs without `--use-tag-compression`)

18/18 conditions improved. No regressions.

| Trace | HRU without | HRU with | Δ% | PHRU without | PHRU with | Δ% |
|---|---|---|---|---|---|---|
| as-Skitter | 35.811722 | 28.770170 | -19.7% | 38.851341 | 33.600925 | -13.5% |
| cit-Patents | 52.826366 | 48.669910 | -7.9% | 53.927601 | 50.447571 | -6.5% |
| com-LiveJournal | 62.004913 | 56.729210 | -8.5% | 60.425259 | 55.588970 | -8.0% |
| com-Youtube | 23.570461 | 20.740204 | -12.0% | 27.428656 | 23.166740 | -15.5% |
| soc-LiveJournal1 | 62.638016 | 57.754047 | -7.8% | 60.865982 | 56.335037 | -7.4% |
| sx-stackoverflow | 23.114246 | 18.389425 | -20.4% | 25.173746 | 21.279865 | -15.5% |
| web-BerkStan | 23.353212 | 18.616201 | -20.3% | 29.098145 | 18.952887 | -34.9% |
| web-Google | 34.760399 | 23.506939 | -32.4% | 38.338264 | 28.897190 | -24.6% |
| wiki-topcats | 51.953449 | 43.989250 | -15.3% | 53.135056 | 46.482643 | -12.5% |

**Mean relative improvement: ~15.7% across all 18 conditions.** This is a
capacity lever, not a replacement-policy lever - far bigger effect size
than anything from the ghost cache threshold tuning (which moved MPKI by
tenths, not multi-point percentages). Confirms recovering iso-area capacity
matters more than any local eviction heuristic once the LLC is
capacity-starved at fine block sizes.

Combined with the ghost cache (`--ghost-cache-size 256
--ghost-cache-min-serviced 4 --use-tag-compression` together, PHRU,
as-Skitter): the two mechanisms **stack** (33.244404 combined vs 33.600925
tag-compression-alone vs 38.378025 ghost-cache-alone vs 38.851341
baseline) - tag compression does the heavy lifting, ghost cache adds a
smaller independent improvement on top. Not redundant.

## Open, unresolved methodological weakness

The N=16384 region buffer size was borrowed from SHiP-Mem's SHCT entry
count, but this is a weaker justification than it first appears:

- SHiP-Mem's SHCT stores a 3-bit **lossy confidence counter** and
  tolerates aliasing (two regions sharing a hash bucket just costs a worse
  replacement-policy guess). Our region buffer must be **lossless** - an
  aliased entry corrupts the compressed tag reconstruction. So the 16384
  entry count answers "how many buckets keep aliasing rate acceptable for
  a confidence predictor," not "how many distinct regions does this
  workload's actual working set touch at once" - a different question,
  answered for a different structure's different failure mode.
- SHiP-Mem's SHCT itself costs only 16384 x 3 bits = 6,144 bytes (6KB) -
  16.7x cheaper than our region buffer's 16384 x 50 bits = 102,400 bytes
  (100KB), because our entries must hold the full region address, not a
  saturating counter.
- **Not yet done**: instrumenting the region buffer against the actual
  traces to count real distinct-regions-in-flight, to check whether 16384
  is actually enough for these graph workloads (large, scattered adjacency
  arrays are exactly the access pattern likely to touch more than 16384
  regions simultaneously, unlike the SPEC/desktop workloads SHiP was
  validated on).

## An area-neutral construction: reclaiming existing tag ways

A cleaner way to size a *real* (non-idealized, SHiP-Mem-faithful, 3-bit
lossy-counter) SHCT is to repurpose area the baseline 64B cache already
has, rather than adding new area:

- One 64B tag entry = 48 bits (`64 - log2(64) - log2(1024)`).
- "One tag way, reclaimed across all 1024 sets" = 1024 x 48 bits =
  6,144 bytes (6KB) - **exactly** the size of one real, literature-faithful
  SHiP-Mem SHCT (16384 entries x 3-bit counters = 6,144 bytes). Reclaiming
  one way's worth of tag storage from the baseline design buys exactly one
  full-size real SHCT, at zero net new area.
- Reclaiming two tag ways (12,288 bytes) buys two such SHCTs.
- Address-space "coverage" (best case, no aliasing): one SHCT with 16384
  entries x 16KB/region = 256MB; two SHCTs = 512MB. Whether 256-512MB is
  enough to cover these graph traces' actual working sets without aliasing
  is unverified - this is the same open question as above, just framed in
  physical-memory terms instead of entry-count terms.

## Physical layout: how to actually fit compressed tags into the array

Requirement that makes this hard: at 64B, tag storage must stay *exactly*
48 bits/way (unmodified baseline behavior). At 8B compressed (15-bit tag,
16384-entry region buffer), the validated 224-slot/set design needs
`224x15=3,360` tag bits/set - more than double the original `32x48=1,536`.
A single fixed-width SRAM macro can't be 48 bits wide in one mode and
physically wider in another for free; the extra bits have to come from
somewhere real. Three approaches, increasing disruption to the baseline:

1. **Pack multiple compressed tags into the existing 48-bit field, touch
   nothing else.** `floor(48/15)=3` compressed tags fit per original tag
   slot (3 bits wasted/way) -> `32x3=96` slots/set. Zero tag-SRAM-width
   change, only decode logic changes. Ceiling of 96 slots, well short of
   224 - simplest, but leaves most of the capacity benefit unrealized.
2. **Mode-muxed donor ways**: repurpose a few ways' *data* SRAM as extra
   tag bits, but only in 8B-compressed mode. Need `3,360-1,536=1,824` more
   tag bits; 4 ways' worth of data (`4x512=2,048` bits) covers it. In 64B
   mode those 4 ways' cells are ordinary data (wired to the data path); in
   8B mode a mux reroutes them to compressed-tag compare logic instead. Working breakdown that hits exactly 224: 28 "normal" ways each keep
   their own repacked 48-bit field (3 local tags) + their 512-bit data area
   (8 possible 8B chunks, only 3 of which have a *local* tag -> 5 "spare"
   chunks/way, 28x5=140 total); the 4 donor ways contribute
   `4x(3+34)=148` tags (3 from their own repacked field + 34 from their
   512 repurposed data bits), of which 140 get statically pre-wired at
   design time to point at those 140 spare chunks. `84 local + 140 remote
   = 224`, matching the validated MPKI-tested number exactly. This is the
   only approach of the three that reaches the full iso-area result, but
   every way is no longer identical (complicates replacement-policy
   bookkeeping, which normally assumes uniform ways), and needs real
   mux/routing design work.
3. **Keep approach 1's simple packing (96 slots), put the region buffer in
   its own separate macro** rather than trying to fit it in the 1024x32
   array at all - architecturally the most natural fit anyway, since the
   region buffer is global/CAM-like, not per-set.

### Approach 2's access path, worked through in detail (this is where the real complexity is)

**Hit path**: split address into `[50-bit region#][1 within-region
bit][10-bit index][3-bit offset]` -> do ONE associative (CAM-style) lookup
on the *global* region buffer keyed by the region number, get back a
14-bit pointer `P_req` -> compare `[P_req, within-region-bit]` against all
224 stored pairs in the target set in parallel (same principle as an
ordinary N-way compare, just N=224) -> on match at slot `k`, `k`'s
physical data location is fixed by construction (local way's own data, or
a pre-wired normal-way spare chunk if `k` is a remote/donor-hosted slot) -
no extra lookup needed. The region-buffer lookup is a new *serial* stage
before the per-set compare can even start, unlike an ordinary cache's pure
parallel compare.

**Miss path, two structurally different cases**:
- *Region-buffer miss* (region# not tracked at all): unambiguous miss,
  fetch from DRAM - but before installing with a compressed tag you need a
  region-buffer slot. If full, you must evict an existing region entry,
  and **every cached line anywhere in all 1024 sets currently pointing at
  that entry's index becomes silently wrong the instant you reassign it**.
  This is not a local, per-set problem - it's global. No cheap solution
  exists yet; options are (a) per-entry reference counting (refuse to
  evict anything in use - can deadlock allocation if everything's pinned),
  (b) a reverse map from region-buffer index -> every line using it (a
  real structure of its own, exactly the piece marked "explicitly out of
  scope" in the implementation plan), or (c) accept a correctness risk and
  rely on a secondary verification tag (which eats back some of the bits
  compression was meant to save).
- *Region-buffer hit, no per-set match*: ordinary miss - fetch, pick a
  victim among the 224 via the normal replacement policy, install.

**This region-buffer-eviction hazard is the honest reason the ~15.7%
9-trace MPKI win is an idealized upper bound, not a delivered result** -
closing it is the real remaining work, not the bit-packing arithmetic.

### Quick reference: capacity recovered vs. compressed-tag width (no region-buffer overhead assumed, i.e. compression scheme unspecified)

| Bits/8B-block tag | Iso-area ways | Data capacity vs. 64B reference |
|---|---|---|
| 51 (uncompressed) | 160 | 0.625x |
| 32 | 192 | 0.75x |
| 15 (+16384-entry region buffer, 102,400-byte fixed cost) | 224 | 0.875x |

Formula used: `total(N) = N*1024*8 + (1024*N*tag_bits)/8 [+ region_buffer_bytes]`,
solved against the fixed 2,293,760-byte 64B-reference budget, then rounded
up to the nearest multiple of 8 (matching `get_iso_area_cache()`'s own
search+round behavior exactly - verified this reproduces the real 160 and
224 values printed by the simulator).
