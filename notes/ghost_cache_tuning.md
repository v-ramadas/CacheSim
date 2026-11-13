# Ghost cache: architecture, bugs found, and tuning results

This documents a long iterative session building on the initial ghost cache
work (see `inc/ghost_cache.h`'s own header comment and
`phru_hub_heuristic_quality.md` for why the mechanism exists at all: PHRU's
local `block_serviced_from_llc` heuristic structurally cannot detect a line
that gets evicted and then re-missed shortly after, since the line's own
metadata is gone by the time that proof exists). This note is about
everything discovered *after* that initial design: where the mechanism was
subtly wrong, what fixed it, what was tried and discarded, and what
actually moves MPKI.

## What the mechanism does, current state

Two independent hook points, both gated on `cachesim::USE_GHOST_CACHE`:

1. **Insertion** - `Set::handle_evict()` (`src/set.cc`). When the LLC evicts
   a line with `serviced_from_llc[way_idx]` above a threshold (see
   "Insertion threshold" below), its address goes into the ghost cache.
2. **Consumption** - `SectoredSet::handle_fill()` (`src/sectored_set.cc`).
   When L1D installs a line on a genuine miss, the *actual requested
   address* (not some unrelated address) is checked against the ghost
   cache. A hit means this exact line was evicted from the LLC too early -
   direct, deterministic proof, not a noisy local proxy. On a hit,
   `way_sector->block_serviced_from_llc[sector_idx]` is incremented
   directly on the persisted L1D state, so it survives until this line is
   itself evicted from L1D and flows back into the LLC via
   `Set::handle_fill()`, which is where PHRU/HRU's replacement decision
   actually reads it.
3. **Stale-entry purge** - `Set::try_hit()` (`src/set.cc`). When an address
   is confirmed resident in the LLC (a genuine hit), any leftover ghost
   cache entry for it is purged via `GhostCache::remove()` - it can't
   simultaneously be "recently evicted" and "currently resident," so a
   stale entry here would taint a later eviction's classification.

## Bug #1: the original mechanism checked the wrong address

The mechanism as first built put the consumption check inside
`Set::handle_fill()` (the LLC's own fill), which in this architecture is
**only ever reached via the backfill loop in `access_multi_level()` that
pushes whatever L1D evicted into the LLC** - not the address that actually
missed. On a genuine miss, the fresh line goes into L1D directly; the LLC
only ever receives new content as a side effect of L1D's own eviction.  So
the "ghost cache hit" check was answering "was *this unrelated L1D victim*
recently evicted from the LLC," not "was the line the CPU just asked for
recently evicted too early" - the two addresses are usually completely
different.

Fix: move the check to `SectoredSet::handle_fill()` (L1D's own fill, where
`block_address` for the exactly-requested sub-block genuinely is the
missed address), and remove the old check from `Set::handle_fill()`
entirely. See git history / `src/set.cc` and `src/sectored_set.cc` for the
current, correct locations.

**Open, unexplained puzzle**: the *old, address-mismatched* mechanism
empirically produced *bigger* MPKI wins than the corrected one, at least at
64B (old: PHRU -0.14 to -0.37 MPKI across 4 traces; new/correct, before the
threshold-tuning work below: 0 improvement at 64B in every case tested).
Leading theory, unverified: the old check was inadvertently acting as a
proxy for "L1D is under eviction pressure from a hot working set," which
correlates with something useful, rather than genuinely detecting early
eviction. Worth revisiting if anyone has time.

## Bug #2: `is_hub_node`/reserved-ways is a dead end at 64B in this config

Verified empirically (byte-identical MPKI with ghost cache on vs off, in
every 64B config, despite the ghost cache clearly registering hits and
shifting the PHRU confusion matrix): with reserved-way partitioning off
(the default in all these tests), `is_hub_node[way_idx]` in `Set` only
feeds back into eviction decisions through the reserved-ways promotion path
in `Set::handle_evict()`, which is unreachable when `reserved_ways == 0`.
The *real* lever for both HRU and PHRU is `packet->serviced_from_llc > 0`
gating their `fill_update()` counter/low_priority assignment - that's what
actually needs the ghost cache's persistence to work, and it does (see
below).

## Bug #3: catastrophic runaway memory growth with set dueling

`GhostCache::breakdown()`/`merge()` exist to keep the ghost cache's stored
addresses and capacity correctly scaled when set dueling changes the LLC's
block size at runtime (`Cache<T>::breakdown()`/`merge()`, called from
`access_multi_level()`). The caller's `should_breakdown()`/`should_merge()`
return a **sticky** flag (`duel_locked`) that reads `true` on *every single
access* for the rest of the run once dueling favors one side, not just once
at the transition. `Cache<T>::breakdown()` itself has an internal
`is_broken_down` guard making repeated calls a safe no-op - but
`GhostCache::breakdown()` originally didn't, so it was getting called (and
re-multiplying `capacity_` by the scale factor) on every subsequent access.

Measured: capacity went from 2048 to **~490GB of RSS in about 11 minutes**
(2048 * 8^N explodes fast) before the run was killed by hand. Machine
memory dropped to 35GB free before the process was stopped - a real risk to
the whole box, not just the simulation.

Fix: `GhostCache` now has its own `is_broken_down_` guard, exactly mirroring
`Cache<T>`'s contract - `breakdown()` and `merge()` are each genuine no-ops
after the first call following a real transition. Verified via:
- A single-transition case (as-Skitter, 8B + ztest dueling): breakdown
  fires exactly once, final capacity is the expected `16384 * 8 = 131072`,
  memory stays in double-digit MB throughout.
- A heavy flip-flop case (cit-Patents, same config): ~24 full
  breakdown/merge cycles over the trace, still lands on the correct
  `131072` with zero drift.

**When testing any new code path that touches set dueling + a global
resource like this, use `ulimit -v <bytes>` as a safety net** and watch
`ps aux` memory for the first 20-30 seconds before trusting a long
unattended run. This bug would not have been caught by a quick sanity
check on a short trace segment - it needed the full sticky-flag behavior to
manifest, which only shows up once dueling actually locks in.

## Bug #4 (methodology, not code): concurrent same-file trace reads corrupt results

Running two `cachesim_multi` processes that read the *same* trace file
concurrently on this machine's `/nobackup.1` mount produces silently wrong
(non-deterministic, non-reproducible) results - confirmed by running the
identical command twice back-to-back in isolation (always reproduces
exactly) vs. once concurrently with another process reading the same file
(produced a different, wrong number). Root cause not fully diagnosed
(suspected I/O contention on the NFS-style mount), but the practical rule
is: **parallelize across different trace files freely, never run two
processes against the same trace file at the same time.**

## Ghost cache eviction policy: FIFO -> value-aware -> (two-tier, discarded)

Measured on as-Skitter: under plain FIFO, ~77% of both inserted and
FIFO-aged-out entries had `serviced_from_llc == 1` (mean inserted value
8.28, mean evicted value 6.27) - FIFO gives a barely-reused line the exact
same residency time as one that was reused hundreds of times.

**Value-aware eviction** (current design, in `GhostCache::insert()`): when
full, evict the *earliest* entry whose recorded `serviced_from_llc` is `<=`
the new entry's value, rather than always the oldest. If no such entry
exists (everything resident is stronger than the newcomer), the newcomer is
rejected instead of forcing out something more valuable. Verified this
correctly skews evictions weaker than insertions (evicted mean dropped to
5.24 at 64B / 1.66 at 8B, vs FIFO's symmetric ~6-8; evicted "% at value 1"
rose to 79-87% vs FIFO's ~77%) - i.e. it's doing exactly what it's supposed
to.

**Two-tier probationary/protected split** (tried, discarded): fresh
evictions below a promotion threshold go into one FIFO, evictions at/above
it go straight into a second FIFO with its own fixed capacity, so
"proven" entries can't be starved by newcomers but also can't become
immortal (unlike a pure value-based single queue, where a very strong entry
can only ever be displaced by an equal-or-stronger one). Measured on
as-Skitter: helped HRU at 8B (best result of anything tried at the time,
before the min-serviced-threshold sweep below), but hurt both policies at
64B and PHRU at 8B; doubling total capacity (2048 -> 4096) didn't fix the
64B case. The rigid capacity split just isn't a reliable win across
configs - discarded in favor of keeping the single value-aware queue.

**Dynamic/AIMD insertion threshold** (tried, discarded): rather than a
fixed `--ghost-cache-min-serviced`, adjust it every N instructions based on
the ghost cache's own eviction rate (raise on high pressure, lower on low
pressure). Measured on as-Skitter (HRU/PHRU x 64B/8B, all 4 conditions):
**converged to threshold 0 in every single case**, after exactly one
adjustment, regardless of which fixed threshold was actually best for that
condition - for PHRU this locked in the *worse* of its two options every
time. Root cause: a threshold above 0 naturally shows low occupancy in the
first epoch (fewer things qualify, so the cache fills slowly), which the
low-watermark rule always misreads as "capacity going unused, loosen up,"
before the stricter threshold has had any chance to show its actual value.
**Eviction pressure is a proxy for occupancy, not for MPKI - the two are
not the same thing, and this design conflates them.** Discarded.

## Insertion threshold: the real, validated win

`Set::handle_evict()`'s insertion gate (`serviced_from_llc[way_idx] >
threshold`) is now CLI-tunable via `--ghost-cache-min-serviced` (default
1). This single knob turned out to matter far more than the eviction
policy refinements above.

**HRU wants the lowest threshold (0).** It improves monotonically as the
threshold *decreases* toward 0 across every trace/block-size tested - HRU
benefits from tracking everything, including weak single-reuse lines, and
lets the value-aware eviction policy sort out what's actually worth
keeping.

**PHRU wants a much higher threshold, and the two block sizes disagree on
how high.** On as-Skitter, sweeping 0/1/2/4/8:
- 64B: monotonically improves at every value tried (0: 37.766445, 1:
  37.580215, 2: 37.521812, 4: 37.499920, 8: 37.465054/best-so-far) - hasn't
  found where it turns yet.
- 8B: improves through 4 (0: 38.928314/worst, 1: 38.425411, 2: 38.397923,
  4: 38.378025/best), then **reverses at 8** (38.410702, worse than 4).

So there is no single universal "best" threshold even for one policy - it's
block-size-dependent. `--ghost-cache-min-serviced 4` was chosen as a
practical default that's solidly past the early gains on both block sizes
without yet risking the 8B reversal, and was validated as the real
recommendation:

**Validated across all 4 standard traces (PHRU, threshold=4 vs baseline,
clean build, both block sizes) - improves in all 8 conditions:**

| Trace | Block | Baseline | Threshold=4 | Δ |
|---|---|---|---|---|
| web-BerkStan | 64B | 18.067221 | 17.691597 | -0.375624 |
| web-BerkStan | 8B | 29.098145 | 28.499249 | -0.598896 |
| as-Skitter | 64B | 38.031700 | 37.499920 | -0.531780 |
| as-Skitter | 8B | 38.851341 | 38.378025 | -0.473316 |
| sx-stackoverflow | 64B | 33.512009 | 32.968826 | -0.543183 |
| sx-stackoverflow | 8B | 25.173746 | 25.020292 | -0.153454 |
| cit-Patents | 64B | 53.404224 | 53.188255 | -0.215969 |
| cit-Patents | 8B | 53.927601 | 53.825405 | -0.102196 |

This is the largest, most consistent PHRU win found in the whole session -
bigger and more reliable than any eviction-policy change alone. **If
picking one thing to carry forward from this session, it's this: for PHRU,
raise `--ghost-cache-min-serviced` well above the default of 1 (4 is a
solid starting point); for HRU, leave it at 0.**

## Capacity scaling for block size

A ghost cache entry is one address tag at the LLC's *current* block size.
At 8B blocks, the same cacheline needs 8 tag slots instead of 1 to cover
the same look-back window in terms of actual cachelines. `main.cc` scales
`--ghost-cache-size` by `CACHELINE_SIZE / cachesim::BLOCK_SIZE` at init, so
the CLI flag keeps meaning "cachelines tracked" regardless of configured
block size (1x at 64B, 8x at 8B). `GhostCache::breakdown()`/`merge()` apply
the same scaling for a *dynamic* mid-run transition (see Bug #3).

## Brainstormed, not implemented: smarter threshold selection

Since a single fixed threshold doesn't suit both policies (or even both
block sizes for one policy), a few directions were discussed for making it
adaptive rather than hand-tuned:

1. **Set-dueling-style A/B arbitration** - reuse the existing PSEL/z-test
   infrastructure (`should_breakdown()`/`should_merge()`'s underlying
   machinery) to duel between two threshold candidates. The wrinkle: that
   infrastructure's power comes from *simultaneous* comparison via
   disjoint **sets** (Leader64/Leader8/Follower), and the ghost cache is a
   single global structure with no per-set concept - a naive port would
   need to duel in *time* (alternate thresholds epoch-by-epoch) instead of
   *space*, which reintroduces a confound the set-based version doesn't
   have (trace-phase changes get conflated with the threshold's actual
   effect). A closer, truer port would partition by **address hash**
   instead of set index - small sampled "leader" sub-populations of
   addresses, each with their own tiny shadow ghost cache and threshold,
   compared via the same PSEL/z-test math, with the winner applied to the
   real (follower) population. Not implemented.
2. **AIMD on the ghost cache's own eviction rate** - implemented and
   discarded; see above. The proxy-vs-objective mismatch is the core
   lesson: anything that tunes toward "the cache looks appropriately full"
   rather than directly toward MPKI risks converging somewhere that feels
   balanced but isn't actually optimal, and the failure mode isn't a small
   miss - it converged to the *same wrong* answer in every tested
   condition, not to noise.
3. **Policy-keyed static default** - simplest fix for the concrete finding
   above (HRU wants 0, PHRU wants a much higher value): key the default
   off `replacement_policy` instead of hardcoding one number, unless the
   user overrides via CLI. Not adaptive, but directly encodes the one
   thing we're actually confident about. Cheapest option if someone wants
   a quick correctness improvement without more experimentation.

## Practical notes for whoever picks this up next

- Standard invocation for these traces: `--trace
  /nobackup.1/vishnu/adaptive-cache/pin/pin/traces/hubs/pr_spmv/<Name>.random.log
  --trace-format instructions --config configs/default.cfg
  --num-cache-sets 1024 --num-cache-ways 32 --cache-block-size <64|8>
  --replacement-policy <hru|phru> --use-ghost-cache --ghost-cache-size
  2048 [--ghost-cache-min-serviced N]`.
- `--debug-ghost-cache` prints every insert/evict/lookup/remove event with
  its `serviced_from_llc` value - useful for exactly this kind of
  distribution analysis, but combine with `--debug` only if you want the
  run capped at `--debug-end` (default 10M) instructions, since `--debug`
  alone bounds runtime; `--debug-ghost-cache` alone does not and will run
  the full trace (still fine - ghost cache events are far sparser than
  full per-instruction debug output).
- The 4 "standard" traces used for faster iteration: as-Skitter,
  web-BerkStan, sx-stackoverflow, cit-Patents (cit-Patents is a good one
  for exercising set-dueling flip-flops, per Bug #3 above - it changed
  block size ~24 times over one run under ztest dueling, while as-Skitter
  only ever broke down once).
- Baselines (`--use-ghost-cache` omitted) are completely unaffected by any
  ghost-cache-internal change - safe to reuse previously-measured baseline
  numbers rather than rerunning them, as long as nothing outside the
  `cachesim::USE_GHOST_CACHE`-gated code paths changed.
