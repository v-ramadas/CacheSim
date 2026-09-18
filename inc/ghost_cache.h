#ifndef __GHOST_CACHE_H__
#define __GHOST_CACHE_H__

#include <cstdint>
#include <list>
#include <unordered_map>
#include <fmt/core.h>

// Small address-only structure that remembers lines evicted from the LLC's
// main sets - it holds no data, just tracks that an address was recently
// resident (a "ghost" of the evicted line), which is why it's named this
// way rather than "victim cache" (a real victim cache would also hold the
// evicted data). If a line is missed again shortly after being evicted,
// that's direct proof it was evicted too early - a signal PHRU's local
// (block_serviced_from_llc) heuristic can never produce, since by
// definition the line is gone before that proof exists. See
// notes/phru_hub_heuristic_quality.md for why this is needed: ~93-99% of
// PHRU's false negatives are lines touched exactly once and evicted before
// any revisit, with zero local signal available at the point of eviction.
//
// FIFO replacement, O(1) insert/lookup/removal via the standard LRU-cache
// pattern: a doubly-linked list (`order`, front = oldest, back = newest)
// holding the actual FIFO order, plus a hashmap (`present`) from address to
// that address's iterator into the list. `std::list` iterators stay valid
// across insertions/erasures of *other* elements, so a hit can erase its
// own node directly - O(1), no stale entries ever left behind, and
// order.size() == present.size() always holds exactly.
//
// (An earlier version used a hashmap + append-only deque with lazy
// deletion, tolerating stale entries until they aged out naturally, to
// avoid a std::deque's O(n) mid-queue erase. Measured on web-BerkStan: 69%
// of HRU's trim-time evictions and 73% of PHRU's were of already-dead
// entries, meaning the nominal capacity() overstated the real look-back
// window by roughly 3x. This version has no such gap - the counted
// evictions below are always real capacity pressure.)
//
// (A two-tier probationary/protected split was also tried - fresh
// evictions below a promotion threshold in one FIFO, evictions at/above it
// straight into a second FIFO with its own capacity. Measured on
// as-Skitter: it helped HRU at 8B (best result of anything tried) but hurt
// both policies at 64B and PHRU at 8B, and doubling total capacity didn't
// fix 64B - the rigid capacity split just isn't a reliable win across
// configs. Discarded in favor of this single-queue design.)
class GhostCache {
    std::list<uint64_t> order;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> present;
    // Diagnostic only: the LLC-side serviced_from_llc[way_idx] count
    // recorded at insert time for each currently-tracked address, so
    // insert()/eviction can report what serviced_from_llc value the line
    // had when it entered the ghost cache. Also doubles as the eviction
    // priority key below.
    std::unordered_map<uint64_t, uint64_t> serviced_from_llc_at_insert;
    size_t capacity_ = 0;
    // Mirrors Cache<T>::is_broken_down - see breakdown()/merge() below for
    // why this guard is load-bearing, not just bookkeeping.
    bool is_broken_down_ = false;

    public:
    uint64_t debug_insert_count = 0;
    uint64_t debug_hit_count = 0;
    uint64_t debug_lookup_count = 0;
    uint64_t debug_eviction_count = 0;
    uint64_t debug_insert_rejected_count = 0;
    // Plain instance flag rather than reading a cachesim:: global directly -
    // this header is included by utils.h, so it can't include utils.h back
    // to see cachesim::GHOST_CACHE_DEBUG without a circular include. Set
    // externally (main.cc sets cachesim::ghostCache.debug alongside resize()).
    bool debug = false;

    void resize(size_t capacity) {
        capacity_ = capacity;
        order.clear();
        present.clear();
        serviced_from_llc_at_insert.clear();
        is_broken_down_ = false;
    }

    size_t capacity() const { return capacity_; }

    // Value-aware eviction: when full, displace the EARLIEST (oldest)
    // entry whose recorded serviced_from_llc is <= this new entry's value,
    // rather than blindly evicting the oldest regardless of value (plain
    // FIFO). Measured on as-Skitter: ~77% of both inserted and
    // FIFO-aged-out entries were serviced_from_llc==1, the weakest
    // qualifying signal, with FIFO giving them the exact same residency
    // time as a serviced_from_llc==712 entry - this policy instead lets
    // a new entry only evict something at least as weak as itself,
    // protecting strongly-reused lines from eviction by weaker newcomers.
    // If no such entry exists (every resident entry is stronger than the
    // new one), the new entry is dropped instead of forcing out something
    // more valuable - capacity pressure is resolved by rejecting the
    // weaker side, not always the older side.
    void insert(uint64_t address, uint64_t serviced_from_llc = 0) {
        if (capacity_ == 0) return;
        auto it = present.find(address);
        if (it != present.end()) {
            // Already tracked - refresh its position to most-recent rather
            // than leaving two entries for the same address.
            order.erase(it->second);
            present.erase(it);
            serviced_from_llc_at_insert.erase(address);
        }
        if (order.size() >= capacity_) {
            auto victim = order.end();
            for (auto oit = order.begin(); oit != order.end(); ++oit) {
                auto sit = serviced_from_llc_at_insert.find(*oit);
                uint64_t existing_value = (sit != serviced_from_llc_at_insert.end()) ? sit->second : 0;
                if (existing_value <= serviced_from_llc) {
                    victim = oit;
                    break;
                }
            }
            if (victim == order.end()) {
                debug_insert_rejected_count++;
                if (debug) fmt::print("GhostCache insert REJECTED address={:#x}, serviced_from_llc={} - every resident entry has a higher value\n", address, serviced_from_llc);
                return;
            }
            uint64_t evicted = *victim;
            order.erase(victim);
            present.erase(evicted);
            debug_eviction_count++;
            uint64_t evicted_serviced_from_llc = 0;
            auto sit = serviced_from_llc_at_insert.find(evicted);
            if (sit != serviced_from_llc_at_insert.end()) {
                evicted_serviced_from_llc = sit->second;
                serviced_from_llc_at_insert.erase(sit);
            }
            if (debug) fmt::print("GhostCache evict address={:#x}, serviced_from_llc={}\n", evicted, evicted_serviced_from_llc);
        }
        debug_insert_count++;
        if (debug) fmt::print("GhostCache insert address={:#x}, serviced_from_llc={}, cache entries={} cache capacity={}\n", address, serviced_from_llc, order.size()+1, capacity_);
        order.push_back(address);
        present[address] = std::prev(order.end());
        serviced_from_llc_at_insert[address] = serviced_from_llc;
    }

    // Returns true (and removes the entry) if address was found - a "ghost
    // cache hit" is a one-shot signal, not a persistent membership test,
    // matching how a real victim cache reinserts the line into the main
    // array on a hit rather than leaving a copy behind.
    bool contains_and_remove(uint64_t address) {
        debug_lookup_count++;
        auto it = present.find(address);
        bool found = (it != present.end());
        if (!found) return false;
        if (debug) fmt::print("GhostCache lookup address={:#x} hit={}\n", address, found);
        order.erase(it->second);
        present.erase(it);
        serviced_from_llc_at_insert.erase(address);
        debug_hit_count++;
        return true;
    }

    // Silent purge - removes the entry if present, not counted as a
    // lookup/hit (this isn't "the CPU asked for this and we served it",
    // it's cleanup of a now-stale/inconsistent entry). No-op if not
    // present. Used when an address is confirmed resident in the LLC (a
    // genuine hit) - it can't simultaneously be a "recently evicted" line,
    // so any leftover ghost cache entry for it is stale and shouldn't
    // survive to taint a later eviction's classification.
    void remove(uint64_t address) {
        auto it = present.find(address);
        if (it == present.end()) return;
        if (debug) fmt::print("GhostCache remove because of hit address={:#x}\n", address);
        order.erase(it->second);
        present.erase(it);
        serviced_from_llc_at_insert.erase(address);
    }

    // Mirrors the replacement policies' own set_breakdown(): called once,
    // globally, when set dueling transitions the LLC's follower sets from
    // old_block_size down to the finer new_block_size. Every existing
    // old_block_size-aligned address is replaced by scale_factor
    // new_block_size-aligned addresses spanning the same cacheline, since
    // an address stored here is only ever compared against lookups aligned
    // to the LLC's CURRENT block size - left at the old granularity, every
    // existing entry would silently stop matching anything the moment the
    // LLC's block size changes. Capacity scales by the same factor, since
    // each old-granularity line now needs scale_factor tag slots to cover
    // the same look-back window in terms of actual cachelines.
    //
    // The is_broken_down_ guard is NOT optional bookkeeping: the caller's
    // should_breakdown() returns a sticky "duel_locked" flag that reads
    // true on every single access for the rest of the run once dueling
    // favors the finer block size, not just once at the transition -
    // Cache<T>::breakdown() itself is safe to call repeatedly because it
    // has this exact same guard internally, but this class didn't, so
    // every subsequent access re-multiplied capacity_ by scale_factor.
    // Measured: capacity went from 2048 to ~490GB of RSS in about a dozen
    // accesses (2048 * 8^N explodes fast) before the run was killed. This
    // guard is what makes the call idempotent per transition, matching
    // Cache<T>::breakdown()'s own contract.
    void breakdown(uint64_t old_block_size, uint64_t new_block_size) {
        if (is_broken_down_) return;
        is_broken_down_ = true;
        uint64_t scale_factor = old_block_size / new_block_size;
        capacity_ *= scale_factor;

        std::list<uint64_t> new_order;
        std::unordered_map<uint64_t, std::list<uint64_t>::iterator> new_present;
        std::unordered_map<uint64_t, uint64_t> new_serviced_from_llc_at_insert;
        for (uint64_t address : order) {
            uint64_t original_value = serviced_from_llc_at_insert.count(address) ? serviced_from_llc_at_insert[address] : 0;
            for (uint64_t offset = 0; offset < old_block_size; offset += new_block_size) {
                uint64_t sub_address = address + offset;
                new_order.push_back(sub_address);
                new_present[sub_address] = std::prev(new_order.end());
                new_serviced_from_llc_at_insert[sub_address] = original_value;
            }
        }
        order = std::move(new_order);
        present = std::move(new_present);
        serviced_from_llc_at_insert = std::move(new_serviced_from_llc_at_insert);
    }

    // Inverse of breakdown(): called once, globally, when set dueling
    // reverts follower sets back from old_block_size (fine) to
    // new_block_size (coarse). Mirrors Set::set_merge()'s own approach
    // rather than breakdown()'s: there's no sound way to recombine
    // scale_factor sub-addresses back into one entry (they may never have
    // all coexisted - some could already have been individually evicted or
    // consumed by a hit), so instead of trying to preserve state, this
    // just clears everything and restores capacity to what it was before
    // the matching breakdown(). Guarded by the same is_broken_down_ flag
    // for the same reason breakdown() needs it: should_merge() is also
    // sticky (true on every access once dueling reverts, not just once),
    // so without the guard capacity_ would keep dividing by scale_factor
    // on every subsequent access until it collapsed to 0.
    void merge(uint64_t old_block_size, uint64_t new_block_size) {
        if (!is_broken_down_) return;
        is_broken_down_ = false;
        uint64_t scale_factor = new_block_size / old_block_size;
        capacity_ /= scale_factor;
        order.clear();
        present.clear();
        serviced_from_llc_at_insert.clear();
    }
};

#endif
