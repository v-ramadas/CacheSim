# PHRU hub-detection heuristic — quality investigation

Status as of 2026-09-17: **first genuine, broad win of the whole
investigation found** - a small victim cache, gated on `serviced_from_llc >
0` at insertion time (see "Attempt 5, take 2" below), closes a mean 70.8% of
the PHRU-to-HRU MPKI gap across the full 9-trace suite, improves *both*
PHRU and HRU on 9/9 traces, and on 3/9 traces PHRU+victim-cache actually
**beats HRU's oracle-degree baseline outright**. Enabled via
`--use-victim-cache --victim-cache-size 2048` (off by default, zero cost
when disabled). Also fixed one pre-existing, unrelated bug along the way
(see below) that did not change any MPKI results.

Diagnostic instrumentation (confusion matrix + FN/TN pattern histograms)
added and kept in `inc/phru_replacement_policy.h` /
`src/phru_replacement_policy.cc` (`record_heuristic`, `print_heuristic_stats`,
`record_pattern`) plus call sites in `main.cc`, gated on
`replacement_policy == PHRU`/`PHRUpp`. None of it affects replacement
behavior - it's pure measurement, safe to leave in. Same instrumentation was
also added to `PHRUpp` (identical policy, only differs in `main.cc`'s
`isoArea` handling for 8B configs).

The victim cache itself was also wired into `HRU` (`hru_replacement_policy.cc`),
not just `PHRU`/`PHRUpp` - see "Attempt 5, take 2" for why it helps HRU too
despite HRU already having oracle-perfect hub knowledge.

## The question

HRU knows a node's true structural hub-ness directly (`degree > avg_degree`,
from oracle trace annotations). PHRU is meant to approximate that without
cheating, via a purely local, observable heuristic: `is_hub_node =
block_serviced_from_llc[block] >= hub_threshold` (`hub_threshold = 1`, i.e.
"this exact block has been serviced from LLC at least once before" - a proxy
for "has this line round-tripped through eviction and refill, implying it's
worth keeping"). Goal: measure how good that heuristic actually is against
the ground truth HRU gets for free, and see if a better *simple* one exists.

## How ground truth was obtained for measurement

`Set::handle_fill()` already computes true `degree > avg_degree` into
`packet->is_hub_node` right before calling any policy's `fill_update()` -
this is universal, not HRU-specific, so no new plumbing was needed to get
ground truth at fill time. At hit time `packet->is_hub_node` is *not* kept
fresh (stays `false` from `packet->clear()`), so ground truth there is
computed directly as `packet->degree > (uint64_t)packet->avg_degree`.

## Baseline heuristic quality (`block_serviced_from_llc >= 1`)

64B, 1024x32-way, LLC MPKI:

| Trace | Precision | Recall | Accuracy | PHRU MPKI | HRU MPKI | Gap |
|---|---|---|---|---|---|---|
| as-Skitter | 0.919 | 0.658 | 0.697 | 38.03 | 37.50 | +0.53 |
| web-BerkStan | 0.869 | 0.818 | 0.761 | 18.07 | 17.64 | +0.43 |
| sx-stackoverflow | 0.977 | 0.690 | 0.719 | 33.51 | 33.28 | +0.24 |
| cit-Patents | 0.902 | 0.338 | 0.533 | 53.40 | 53.21 | +0.20 |

Precision is consistently high (0.87-0.98) - when the heuristic says hub,
it's almost always right. **Recall is the bottleneck**, badly so on
cit-Patents (0.34).

## A structural gate bug found along the way

`PHRU::fill_update()`'s entire `is_hub_node`-driven branching was nested
inside `if (packet->serviced_from_llc > 0)` (the line-level aggregate, only
nonzero if this line already round-tripped through this cache once before).
For the majority of ordinary fills that's false, so `is_hub_node` - however
computed - was silently ignored and the `else` branch always fired
(`counter = lru_counter; low_priority = true`). Confirmed by an experiment
where changing the heuristic formula changed the recorded confusion matrix
but left MPKI byte-identical to baseline. All four branches inside the `if`
collapsed to the same two outcomes anyway (`was_accessed` never
differentiated anything), so removing the gate is a strict bug fix, not a
redesign - but every attempt below that combined the gate fix with an
extended heuristic made things *worse*, meaning the gate had been acting as
an (accidental) conservative default that mattered.

## Three heuristic-extension attempts, all failed

Same 64B, 4-trace setup. All numbers are LLC MPKI (baseline PHRU: 38.03 /
18.07 / 33.51 / 53.40; HRU: 37.50 / 17.64 / 33.28 / 53.21):

1. **`l1_hits >= 1` OR'd into `hit_update` only, gate untouched.**
   `packet->l1_hits` (set from this cache's own `way_hits[]` on every hit,
   carried forward across eviction via `Set::fill_packet()`) is guaranteed
   `>= 1` on literally every hit by construction, so this made
   `hit_update`'s hub check trivially always-true. Confusion matrix moved
   (recall +0.06 to +0.12 on 3/4 traces), but **MPKI stayed byte-identical
   on every trace** - `low_priority` only ever transitions non-hub->hub once
   and popular lines already got there "eventually" via
   `block_serviced_from_llc` before it mattered for an eviction decision.

2. **Footprint-based, combined, with the gate fix**: `is_hub_node =
   (block_serviced_from_llc>=1) || (l1_hits > footprint_count) ||
   (footprint_count < 8)`. Result: 38.03->44.85, 18.07->25.17,
   33.51->42.79, 53.40->56.47 - badly worse on every trace, and
   as-Skitter's new MPKI (44.853634) is an exact digit match to plain LRU's
   baseline. `footprint_count < 8` ("line not fully touched") is true for
   almost every fill regardless of hub-ness in this workload (a 64B line
   rarely gets all 8 words touched before eviction either way), so it
   swamped the classifier - precision fell to 0.63-0.83, nearly everything
   got classified hub, and PHRU's replacement collapsed to plain LRU.

3. **Isolated `l1_hits > footprint_count` only, gate fix kept, "line not
   touched" clause dropped.** Result: 38.03->50.36, 18.07->32.96,
   33.51->45.06, 53.40->63.06 - worse than attempt 2, worst of everything
   tried. Recall crashed to as low as 0.07 (cit-Patents). This condition
   only fires when the exact same 8B word gets hit more than once in a
   residency - a true hub visited repeatedly through *different* fields each
   time never satisfies strict `>`, so it's a much narrower/weaker signal
   than it looked on paper.

## False-negative pattern analysis (why recall is capped)

Reverted the decision logic to original (confirmed via exact MPKI + exact
confusion-matrix match to baseline), then histogrammed `l1_hits` and
`count_footprint(footprint)` specifically for cases the *original* heuristic
gets wrong or right, fill-time only (where these fields are meaningful -
carried forward from the evicted line's prior residency via
`Set::fill_packet()`; not meaningful at hit time).

**Dominant false-negative pattern, 93-99% of all FN across all 4 traces:
`l1_hits == 0` and `footprint_count == 1`.** These are true hubs (by degree)
touched exactly once - filled, then evicted - before any revisit. There is
no local signal to exploit: from the cache's point of view a line that gets
one shot before eviction is indistinguishable from a genuine one-off cold
access. No per-residency-local counter (LLC-reuse count, hit count,
footprint density) can ever fix this, because the information needed - "this
address is a hub globally" - doesn't survive the eviction that already
happened.

A small tail (1.75%-3.6% of FN, closely matching `l1hits_gt_footprint`) does
show real local reuse (`l1_hits` 8-31, full footprint) and is genuinely
recoverable in principle - this is exactly what attempts 1/3 above targeted.

## False-negative vs. true-negative comparison (why the tail isn't usable either)

Added the same histograms for TN (correctly-classified non-hubs) and
compared fraction with `l1_hits >= 8`:

| Trace | FN with l1_hits>=8 | TN with l1_hits>=8 | Ratio |
|---|---|---|---|
| as-Skitter | 1.75% (99,793) | 27.4% (1,604,997) | ~16:1 |
| web-BerkStan | 3.6% | 50.0% | ~21:1 |
| sx-stackoverflow | 1.6% | 38.3% | ~24:1 |
| cit-Patents | 0.2% | 27.3% | ~200:1 |

**This confirms, rather than contradicts, why attempts 1 and 3 failed.** The
small recoverable FN tail (heavy local reuse) shares its exact signature
with a population of *true non-hubs* that's 16-200x larger. Any threshold on
`l1_hits`/`footprint_count` that catches the missed hubs also reclassifies
a much bigger population of genuinely-non-hub, heavily-locally-reused lines
(e.g. dense/sequential structures like CSR row-pointer scans) as false
positives - exactly the precision collapse observed empirically. This is a
real class-overlap problem in the data, not a signal that was just applied
wrong.

## Attempt 4: `footprint_count == 1`, analytically predicted then tested

`footprint_count == 1` is the single dominant value in both the FN and TN
footprint histograms above (e.g. as-Skitter: 97.2% of FN, 71.8% of TN) - it's
nearly the entire population attempt 2's `footprint_count < 8` clause
covered, minus the negligible 2-7 buckets. Before running anything, computed
the exact resulting confusion matrix analytically from already-collected
histogram counts (`new_TP = old_TP + FN_footprint1`, `new_FP = old_FP +
TN_footprint1`, etc.) - this needed no new simulation, just arithmetic on
data already in hand:

| Trace | Predicted P/R/A | Actual P/R/A | MPKI: baseline -> actual | HRU |
|---|---|---|---|---|
| as-Skitter | 0.794/0.884/0.738 | 0.744/0.891/0.705 | 38.03 -> **42.73** | 37.50 |
| web-BerkStan | 0.835/0.884/0.772 | 0.795/0.885/0.742 | 18.07 -> **19.24** | 17.64 |
| sx-stackoverflow | 0.896/0.886/0.813 | 0.877/0.898/0.809 | 33.51 -> **40.80** | 33.28 |
| cit-Patents | 0.723/0.934/0.718 | 0.687/0.944/0.693 | 53.40 -> **55.54** | 53.21 |

The analytical prediction held up well - actual precision/recall tracked the
predicted values closely (small deltas expected, since changing the
heuristic changes which lines get evicted, which feeds back into the
footprint/l1_hits values themselves in a way the static histogram estimate
can't capture). **MPKI regressed on every trace anyway**, sx-stackoverflow
worst (+7.29, erasing most of breakdown's own win on that trace). Reverted;
decision logic re-verified to match original baseline exactly (MPKI and
confusion matrix digit-for-digit).

This is the fourth distinct variant to fail (l1_hits-in-hit_update: neutral;
footprint<8 combined: bad; l1_hits>footprint isolated: worse; footprint==1:
also worse), and the pattern is now clear: **better classification metrics
(precision/recall/accuracy) do not reliably predict better MPKI in this
system.** The original single-signal heuristic, despite weak recall,
apparently protects the *right* lines - ones that already proved reuse value
via a completed evict-refill round trip. Every attempt to broaden the
criteria ends up also protecting a much larger population of low-value
first-touch lines, diluting capacity rather than concentrating it.

## Conclusion

Local, per-residency signals (`block_serviced_from_llc`, `l1_hits`,
`footprint`) appear to have a hard ceiling near where the original heuristic
already sits. The dominant recall-limiting failure mode (single-touch, no
revisit before eviction) has no local signal by construction. The smaller
recoverable slice overlaps too heavily with genuine non-hubs to be usable
via any simple threshold/combination tried.

**What would actually be needed:** a signal that survives eviction - keyed
on address or PC, persisting across a line's lifetime rather than reset each
residency - e.g. a SHiP-style signature-history table (this codebase already
has one for `SHIP`/`SparsityPredictor::shct_value`, wired into `Packet` but
unused by PHRU). That's a materially bigger change (new persistent global
state) than anything tried here.

## Attempt 5: small victim cache (`--use-victim-cache`)

Of the brainstormed "signal that survives eviction" directions, this is the
one that was actually built: a small buffer (`inc/victim_cache.h`) that
catches addresses evicted from the LLC's main sets. If a fresh miss hits an
address still sitting in the buffer, that's direct, deterministic proof the
line was evicted too early - unlike every local signal tried in attempts
1-4, it can't be confused with an unrelated non-hub line's local access
pattern, since it requires the *exact same address* to reappear.

New CLI flags: `--use-victim-cache` (off by default, zero behavior change
when unset) and `--victim-cache-size <N>` (default 32). Wired in as: `Set::
handle_evict()` inserts the evicted address (LLC level only, i.e. `level ==
1`) right before `invalidate_way()` clears it; `Set::handle_fill()` checks
each incoming block address against the victim cache and sets a new
`packet->from_victim_cache` field, consumed in `PHRU`/`PHRUpp::fill_update()`
as an unconditional (ungated) hub signal - unlike the `block_serviced_from_llc`
clause, which stays behind the original `serviced_from_llc > 0` gate, since
every attempt to remove that gate for a *local* signal made things worse
(see attempts 2-4). Also added identically to `PHRUpp`.

### Results (take 1, ungated): full 9-trace suite, 64B, LLC MPKI

| Trace | HRU | PHRU | +VC (512) | +VC (2048) |
|---|---|---|---|---|
| as-Skitter | 37.50 | 38.03 | 38.03 | 38.21 |
| cit-Patents | 53.21 | 53.40 | 53.40 | 53.37 |
| com-LiveJournal | 62.61 | 62.83 | 62.83 | 62.91 |
| com-Youtube | 23.82 | 24.29 | 24.29 | 24.37 |
| soc-LiveJournal1 | 63.83 | 64.02 | 64.02 | 64.08 |
| sx-stackoverflow | 33.28 | 33.51 | 33.51 | 33.69 |
| web-BerkStan | 17.64 | 18.07 | 18.06 | 18.19 |
| web-Google | 29.09 | 29.33 | 29.33 | 29.44 |
| wiki-topcats | 49.91 | 50.42 | 50.42 | 50.56 |

Victim-cache hit counts at size 512 were tiny on every trace except
web-BerkStan (5,950 hits vs. 1-231 elsewhere - e.g. com-LiveJournal has 174M
trace lines but only 4 hits registered). A *global* 512-entry buffer is
simply too small a fraction of eviction traffic once trace size scales up;
web-BerkStan is the smallest trace tested (21M lines), so 512 entries
represents proportionally far more coverage there than on traces 5-9x
larger. At size 2048, hits scale into the tens-to-hundreds of thousands
across every trace, but MPKI gets *worse* on 8 of 9 - the same
precision-loss pattern as attempts 1-4. Only cit-Patents shows a negligible
improvement (53.40 -> 53.37) at 2048.

**Verdict at the time: did not meaningfully close the PHRU-to-HRU gap at
either size.** A global victim cache sized to matter for large traces
(thousands of entries) caught too much noise along with the signal, for the
same underlying reason as every local heuristic in attempts 1-4: a large
population of genuinely-non-hub lines gets caught by any threshold loose
enough to catch a meaningful fraction of missed hubs.

## Attempt 5, take 2: gate insertion on `serviced_from_llc > 0` — the fix that worked

The problem with the plain victim cache wasn't its existence, it was what it
stored: every LLC eviction, unconditionally, including the ~93-99%
single-touch cold lines identified earlier in this file that will never be
revisited soon regardless of buffer size. At a fixed small capacity, those
cold evictions constantly flush out the rare genuinely-useful entries before
they get a chance to be hit.

The fix: only `insert()` an evicted line into the victim cache if
`serviced_from_llc[way_idx] > 0` (`Set`'s own per-way tracking - this line
has already round-tripped through LLC at least once before this eviction,
i.e. it's not a first-time compulsory fill). One line changed in
`Set::handle_evict()`. Effect on web-BerkStan at capacity 512: insert count
dropped 92% (1,880,019 -> 144,931) and **hit rate jumped from 0.3% to
38%** (5,950/1,880,019 -> 55,568/144,931) - the buffer now holds almost
nothing but already-proven candidates, so far fewer of its limited slots are
wasted on lines that were never going to be revisited anyway.

This also reversed the size trend from take 1: previously bigger capacity
made things *worse* (more noise let in); now bigger capacity helps *up to
where the qualifying population saturates it* (~2048-8192 on web-BerkStan,
where insert counts plateau and hit rate approaches 95-99%), then flattens
out or drifts slightly worse beyond that (32768 was marginally worse than
2048/8192 - diminishing returns, not the earlier noise-driven regression).
2048 was chosen as the size for the full-suite run below.

**Also wired into `HRU`** (`src/hru_replacement_policy.cc`), not just
`PHRU`/`PHRUpp`: `hit_update()` OR's `packet->from_victim_cache` into the
promotion check, and `fill_update()` gets an unconditional branch ahead of
the `serviced_from_llc > 0` gate, mirroring PHRU exactly. HRU already has
oracle-perfect hub knowledge (`packet->is_hub_node` from true degree), so a
priori this looked likely to be redundant - but it isn't: a genuine hub can
still get evicted due to counter/priority dynamics unrelated to
misclassification (e.g. a cold-start race, or simply losing out to an
even-more-recently-touched way), and the victim cache gives it a fast path
back to hub-level priority the moment it's re-referenced, regardless of why
it left. The effect is smaller than for PHRU (HRU has much less headroom to
begin with) but real and consistent.

### Full 9-trace results, 64B, victim-cache-size 2048, LLC MPKI

| Trace | HRU | HRU+VC | PHRU | PHRU+VC | HRU Δ | PHRU gap closed |
|---|---|---|---|---|---|---|
| as-Skitter | 37.500 | 37.410 | 38.032 | 37.809 | −0.090 | 41.9% |
| cit-Patents | 53.208 | 53.098 | 53.404 | 53.307 | −0.110 | 49.5% |
| com-LiveJournal | 62.608 | 62.404 | 62.832 | 62.568 | −0.204 | **117.9%** |
| com-Youtube | 23.818 | 23.763 | 24.293 | 24.138 | −0.056 | 32.8% |
| soc-LiveJournal1 | 63.831 | 63.635 | 64.023 | 63.776 | −0.196 | **128.9%** |
| sx-stackoverflow | 33.276 | 33.064 | 33.512 | 33.217 | −0.213 | **125.2%** |
| web-BerkStan | 17.639 | 17.593 | 18.067 | 17.874 | −0.045 | 45.1% |
| web-Google | 29.091 | 29.023 | 29.331 | 29.207 | −0.068 | 51.6% |
| wiki-topcats | 49.908 | 49.798 | 50.415 | 50.188 | −0.110 | 44.7% |

"Gap closed" = `(PHRU_baseline - PHRU_VC) / (PHRU_baseline - HRU_baseline)`;
values over 100% mean PHRU+VC actually beat HRU's own baseline.

**HRU improved on 9/9 traces. PHRU improved on 9/9 traces. Mean gap closed:
70.8%.** On three traces (com-LiveJournal, soc-LiveJournal1,
sx-stackoverflow) PHRU+victim-cache **outperforms HRU's oracle-degree
baseline outright** - a heuristic built from purely observable, local +
one-shot-persistent signals beating the version with perfect structural
knowledge of the graph. This is consistent with the fifth-attempt
mechanism being genuinely orthogonal to degree-based classification (it
answers "was this evicted too early", not "is this a hub"), so it helps
even a policy that already gets the hub question right.

**Verdict: this is the fix that works.** Recommend as the default going
forward for both `HRU` and `PHRU`/`PHRUpp` when the extra small persistent
structure is acceptable. `--victim-cache-size 2048` was the size used for
this table, but see the size sweep below - **2048 turned out to be
significantly undersized as a default.**

### Size sweep, full 9-trace suite: 1024 / 2048 / 4096 / 8192

Raw results in `results/06_victim_cache_size_sweep_9trace.txt`. Mean PHRU
gap-closed by size, averaged over all 9 traces:

| Size | Mean gap closed |
|---:|---:|
| 1024 | 40.3% |
| 2048 | 70.8% |
| 4096 | **97.8%** |
| 8192 | **99.4%** |

At 8192, PHRU+VC's average MPKI is essentially indistinguishable from HRU's
own baseline. Per-trace, **6 of 9 traces are still monotonically improving
all the way through 8192** (cit-Patents, com-LiveJournal, soc-LiveJournal1,
sx-stackoverflow's HRU side, web-Google, wiki-topcats) - capacity hasn't
saturated yet at this range for the larger/denser traces. Only `web-BerkStan`
consistently peaks around 4096 and gives a little back at 8192 (HRU there
even dips slightly below its own no-victim-cache baseline: 17.639 -> 17.645
at size 8192) - it's real, but trace-specific, not a general trend. An
earlier, narrower sweep run on `web-BerkStan` alone had suggested 4096 was
the global sweet spot and 8192 a regression; the full-suite sweep shows that
conclusion doesn't generalize - it was an artifact of testing only the one
(smallest, ~106M-instruction) trace where the buffer saturates soonest.

The same 3 traces that beat HRU's baseline outright at size 2048
(com-LiveJournal, soc-LiveJournal1, sx-stackoverflow) still do at 4096/8192
- larger capacity mostly helps close the *remaining* gap on the other 6
traces rather than creating new outright wins.

**Updated recommendation: `--victim-cache-size 8192` (or 4096 if the extra
table size at 8192 isn't worth it, since the two sizes are close - 97.8% vs
99.4% mean gap closed).** Sizes beyond 8192 haven't been tested; given most
traces are still improving at 8192, a larger size may help further.

### A second pre-existing bug, found via ASAN while verifying this (unrelated, MPKI-neutral)

`PHRU::hit_update()`/`PHRUpp::hit_update()` read `packet->block_serviced_
from_llc[way_idx]` - but that vector is sized by `num_blocks` (sub-blocks
per line, e.g. **1** at plain 64B) in `Cache<T>::try_hit()`, while `way_idx`
is a *way* index (up to `num_ways-1` = 31 for a 32-way set). Any hit at a
way other than 0 read out of bounds - confirmed via ASAN on baseline PHRU
with `--use-victim-cache` completely absent, so this predates and is
unrelated to the victim cache work. Fixed by using `packet->serviced_from_llc`
(the scalar aggregate) instead - exactly what `fill_update()`'s own outer
gate already uses for the equivalent whole-line check.

**Important: this fix did not change MPKI at all** (verified digit-for-digit
identical before/after on web-BerkStan), only the diagnostic precision/
recall/accuracy numbers (recall dropped from ~0.82 to ~0.54 once the
out-of-bounds reads stopped accidentally counting extra true positives).
`fill_update()`'s classification was never buggy (always correctly indexed
by `block_idx`), and it's what actually determines `low_priority` in the
cases that matter for eviction - `hit_update()`'s promotion logic rarely
changes a real outcome. **All MPKI-based conclusions and tables earlier in
this file (attempts 1-4, and take 1 of attempt 5) are unaffected by this
fix** - they were never reading the buggy field for their MPKI numbers.

### Implementation lesson: O(n) doesn't scale

First `VictimCache` implementation used a flat vector with linear-scan
lookup (`O(capacity)` per lookup). At capacity 8192 with ~7.8M lookups on a
single mid-size trace, that's tens of billions of comparisons - it never
finished in reasonable time and had to be killed mid-run on the largest
traces before switching to a `std::unordered_map` (address -> insertion
sequence number) plus a lazy-deletion FIFO deque, giving O(1) insert/lookup
regardless of capacity. Runtime became flat (~30s regardless of size on
web-BerkStan) after the fix. Worth remembering for any similar
persistent-structure idea.

### New CLI flags

`--use-victim-cache` (off by default, zero behavior change when unset) and
`--victim-cache-size <N>` (default 32 - the full-suite run above overrode
this to 2048 explicitly).

## Reproduction

```
TRACE_DIR=/nobackup.1/vishnu/adaptive-cache/pin/pin/traces/hubs/pr_spmv
./bin/cachesim_multi --trace $TRACE_DIR/<trace>.random.log --trace-format instructions \
  --config configs/default.cfg --num-cache-sets 1024 --num-cache-ways 32 \
  --cache-block-size 64 --replacement-policy phru
# prints "PHRU heuristic quality: ..." and "PHRU FN/TN pattern: ..." lines
# after the normal stats block.

# with the victim cache (recommended size, works for both phru and hru):
./bin/cachesim_multi --trace $TRACE_DIR/<trace>.random.log --trace-format instructions \
  --config configs/default.cfg --num-cache-sets 1024 --num-cache-ways 32 \
  --cache-block-size 64 --replacement-policy phru \
  --use-victim-cache --victim-cache-size 2048
# also prints "Victim cache: capacity ... inserts ... lookups ... hits ..."
```

## Attempt: idea 4, Bloom filter of "seen before" addresses

Implemented in isolation (clean baseline, no victim-cache code present) as
`inc/bloom_filter.h` - a standard bit-array Bloom filter (double hashing,
splitmix64-derived hash functions, configurable bits/hash-count). Same
integration points as the victim cache: on LLC eviction, insert the evicted
address if `serviced_from_llc[way_idx] > 0` (same cold-line gate that made
the victim cache work); on the next fill of any address, check
`possibly_contains()` and if true, treat it exactly like a victim-cache hit
(promote to MRU, clear `low_priority`) in `HRU`/`PHRU`/`PHRUpp`'s
`fill_update()`/`hit_update()`. Saved as `patches/04_bloom_filter.patch`
(applied against the same clean baseline as patch 06, mutually exclusive
with it - both touch the same lines in `fill_update`/`hit_update`).

Motivation: unlike the victim cache's bounded FIFO (which only remembers a
handful of the most recent evictions), a Bloom filter's capacity is
probabilistic rather than exact, so in principle it can also catch addresses
that recur much later in the trace, long after they've fallen out of any
small FIFO's window.

ASAN-clean (no memory errors) on `web-BerkStan` for both PHRU+filter and
HRU+filter.

### Result: net negative, and not a sizing problem

Swept `--bloom-filter-bits` from 64Ki up to 64Mi (1024x range) on PHRU,
`web-BerkStan` (baseline MPKI, no filter: **18.067221**):

| bits       | MPKI     | inserts | lookups | hits    | hit rate |
|-----------:|---------:|--------:|--------:|--------:|---------:|
| 65,536     | 22.656149| 1,134,236| 7,839,433| 6,838,820| 87.2% |
| 262,144    | 19.221346| 898,217 | 7,839,433| 6,253,387| 79.8% |
| 1,048,576  | 18.864780| 873,567 | 7,839,433| 6,200,542| 79.1% |
| 4,194,304  | 18.854794| 872,921 | 7,839,433| 6,198,809| 79.1% |
| 16,777,216 | 18.854671| 872,911 | 7,839,433| 6,198,797| 79.1% |
| 67,108,864 | 18.854671| 872,911 | 7,839,433| 6,198,797| 79.1% |

At small sizes the filter is badly saturated (87% hit rate against ~1.1M
gated insertions into only 64Ki bits, i.e. almost every lookup false-positives)
- same "unfiltered noise" failure mode as every earlier ungated local
heuristic. But growing the filter 1024x (16Mi -> 64Mi bits gives bit-for-bit
identical results) reaches the **false-positive-free asymptote**: at that
point the filter is behaviorally equivalent to an oracle "was this address
ever LLC-evicted-with-reuse before" test, and it still plateaus at
**18.855 MPKI - 4.4% worse than not having the filter at all.**

Confirmed on HRU too (same trace): baseline **17.638653** vs. **18.854897**
with the filter at 4Mi bits (near-asymptote) - an even larger relative
regression, and HRU+filter converges to almost the same absolute MPKI as
PHRU+filter, suggesting the "unconditionally promote to MRU on any bloom hit"
branch dominates behavior once it fires ~870K+ times, regardless of the
underlying policy's own hub logic.

**This rules out capacity/false-positive-rate as the problem.** The
mechanism itself is the problem: "this address was evicted from LLC at some
point in the past, with at least one reuse before eviction" is a
fundamentally different (and worse) signal than the victim cache's "this
address was evicted from LLC *very recently*". A Bloom filter never forgets,
so once an address crosses the `serviced_from_llc > 0` bar even once, it is
marked hub-like *forever* - including on a re-fill that happens naturally,
much later, as part of ordinary sparse reuse in a huge graph trace, with no
actual thrashing involved. The victim cache's small FIFO capacity is not a
limitation to work around - it's the load-bearing part of why it works: it
only fires within a bounded recency window, which is what actually
distinguishes "evicted too early, thrashing" from "legitimately infrequent
reuse."

**Conclusion: idea 4 does not help, at any filter size.** Confirmed on the
full 9-trace suite too (`results/04_bloom_filter_9trace.txt`, filter size
16Mi bits): **worse than baseline on 9/9 traces for both HRU and PHRU**,
e.g. as-Skitter HRU 37.500 -> 40.302, PHRU 38.032 -> 40.287; every other
trace shows the same 0.6-2.9 MPKI regression. The implementation and patch
are kept available (`patches/04_bloom_filter.patch`) in case a different
insertion/consultation policy is worth revisiting later (e.g. periodic
decay/reset of the filter to reintroduce a recency window), but as specified
in the original idea, it is not a viable heuristic.

## Attempt: idea 5, persistent thrash counter

Implemented in isolation (clean baseline, no victim-cache or bloom-filter
code present) as `inc/thrash_counter.h`. Two parts: (1) the exact same small
bounded recency FIFO as the victim cache, gated the same way
(`serviced_from_llc[way_idx] > 0` on LLC eviction), used only to detect a
"fast re-miss" event; (2) a fixed-size, address-hashed table of saturating
counters that *persists* for the whole run - a fast re-miss increments the
counter for that address's hashed slot, an ordinary (non-recency) miss
decays it, and crossing a threshold is treated as a hub-like signal exactly
like `from_victim_cache`/`from_bloom_filter`. The intent was for the
persistent counter to "remember" repeated thrashing at an address beyond
what a single FIFO slot could hold, requiring corroborating evidence
(threshold > 1) before acting - a middle ground between the victim cache
(acts on one fast re-miss, forgets fast) and the Bloom filter (never
forgets, acts on one occurrence forever).

ASAN-clean on `web-BerkStan` for both PHRU+counter and HRU+counter (default
params: recency=32, table=4096, max=3, threshold=2).

### Threshold sensitivity: >=2 kills the signal

At the default threshold=2, the counter essentially never fires:

| threshold | recency | fast_remiss | positive_signals | MPKI (PHRU, baseline 18.067221) |
|---:|---:|---:|---:|---:|
| 2 | 32  | 984   | 0   | 18.067221 (no change) |
| 2 | 128 | 13,623| 24  | 18.067144 |
| 2 | 512 | 50,317| 348 | 18.068861 (slightly worse) |
| 3 | 32/128/512 | - | 0-2 | 18.067221 (no change) |
| 1 | 32  | 970   | 970 | 18.065832 |
| 1 | 128 | 13,656| 13,675 | 18.052408 |
| 1 | 512 | 55,641| 56,061 | 17.989082 |

With only 4096 hashed slots and millions of ordinary misses, any given slot
gets decremented by unrelated addresses' misses far more often than it gets
incremented by a real repeat-thrash event at the *same* address - so a
counter almost never survives long enough to reach 2. Requiring
corroborating evidence turned out to be self-defeating: the persistent table
is far too contended for "wait for a second occurrence" to ever pay off.
**Only threshold=1 produces a usable signal**, at which point every
increment is immediately read back as positive - i.e. the "persistence"
degenerates to "was this address just marked", the same thing the victim
cache already does more precisely (FIFO membership, no decay noise).

### Recency/table-size sweep at threshold=1 (PHRU, `web-BerkStan`)

| recency | table  | MPKI     |
|---:|---:|---:|
| 32   | 4096  | 18.065832 |
| 128  | 4096  | 18.052408 |
| 512  | 4096  | 17.989082 |
| 2048 | 4096  | 17.884251 |
| 2048 | 16384 | 17.878019 |
| 2048 | 65536 | **17.874472** |
| 8192 | 4096  | 17.914417 |
| 8192 | 16384 | 17.902554 |
| 8192 | 65536 | 17.881117 |

Best found: `recency=2048, table=65536, threshold=1, max=3`. Recency=2048
matches the victim cache's own winning size exactly; larger tables reduce
hash-collision noise but the improvement is small once collisions are
already rare. HRU at this config: 17.638653 -> 17.598602.

**These numbers are, to within ~0.005 MPKI, identical to the victim cache's
own numbers on this trace** (PHRU+VC: 17.874, HRU+VC: 17.593 - see the table
above). That is not a coincidence: at threshold=1 the persistent counter's
value is irrelevant except "is it zero or not right after this event",
which is exactly the victim cache's membership test, just approximated
through a lossy hash table instead of an exact FIFO with consume-on-hit.

### Full 9-trace results, threshold=1/recency=2048/table=65536, LLC MPKI

| Trace | HRU | HRU+TC | HRU+VC | PHRU | PHRU+TC | PHRU+VC |
|---|---|---|---|---|---|---|
| as-Skitter | 37.500 | 37.412 | 37.410 | 38.032 | 37.809 | 37.809 |
| cit-Patents | 53.208 | 53.100 | 53.098 | 53.404 | 53.310 | 53.307 |
| com-LiveJournal | 62.608 | 62.403 | 62.404 | 62.832 | 62.569 | 62.568 |
| com-Youtube | 23.818 | 23.764 | 23.763 | 24.293 | 24.140 | 24.138 |
| soc-LiveJournal1 | 63.831 | 63.634 | 63.635 | 64.023 | 63.779 | 63.776 |
| sx-stackoverflow | 33.276 | 33.065 | 33.064 | 33.512 | 33.221 | 33.217 |
| web-BerkStan | 17.639 | 17.599 | 17.593 | 18.067 | 17.874 | 17.874 |
| web-Google | 29.091 | 29.025 | 29.023 | 29.331 | 29.206 | 29.207 |
| wiki-topcats | 49.908 | 49.804 | 49.798 | 50.415 | 50.189 | 50.188 |

Full results in `results/05_thrash_counter_9trace.txt`. HRU improved 9/9,
PHRU improved 9/9 (same as the victim cache) - **mean gap closed: 70.1%**,
against the victim cache's 70.8% on the identical suite. Every per-trace
number matches its victim-cache counterpart to within ~0.005 MPKI, well
inside noise from the hash-collision approximation.

**Conclusion: idea 5, tuned to actually engage, is not an independent
mechanism - it converges to the victim cache.** The "persistence" and
"repeated evidence" framing in the original idea doesn't survive contact
with a realistically-sized table: any threshold above 1 is starved of
signal by collision-driven decay, and at threshold 1 the counter carries no
more information than plain recency-FIFO membership. It is a strictly
worse implementation of idea 6 (extra hash-collision noise, more tunables to
get wrong, marginally worse numbers on most traces) with no compensating
benefit. Patch kept available (`patches/05_thrash_counter.patch`) for
completeness, but **idea 6's victim cache remains the one to use** - it is
simpler, slightly more accurate, and has no threshold/table-size tuning
surface to get wrong.
