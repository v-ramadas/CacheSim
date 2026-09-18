#ifndef __GHOST_CACHE_H__
#define __GHOST_CACHE_H__

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>

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
// FIFO replacement, O(1) insert/lookup via a hashmap + lazy-deletion queue
// (a naive per-lookup linear scan over `entries` is O(capacity) and doesn't
// scale - measured tens of billions of comparisons at capacity 8192 on a
// single trace). `present` maps address -> the sequence number of its most
// recent insertion; `fifo` is an append-only log of (address, seq) pairs.
// On eviction we pop the front and only actually remove it from `present`
// if its sequence number still matches (i.e. it wasn't already removed by a
// hit, or superseded by a more recent re-insertion) - otherwise it's a
// stale log entry and we just drop it.
class GhostCache {
    std::unordered_map<uint64_t, uint64_t> present;
    std::deque<std::pair<uint64_t, uint64_t>> fifo;
    uint64_t next_seq = 0;
    size_t capacity_ = 0;

    public:
    uint64_t debug_insert_count = 0;
    uint64_t debug_hit_count = 0;
    uint64_t debug_lookup_count = 0;

    void resize(size_t capacity) {
        capacity_ = capacity;
        present.clear();
        fifo.clear();
        next_seq = 0;
    }

    size_t capacity() const { return capacity_; }

    void insert(uint64_t address) {
        if (capacity_ == 0) return;
        debug_insert_count++;
        uint64_t seq = next_seq++;
        present[address] = seq; // overwrite: dedups to the most recent insertion
        fifo.push_back({address, seq});
        while (fifo.size() > capacity_) {
            const auto& [addr, s] = fifo.front();
            auto it = present.find(addr);
            if (it != present.end() && it->second == s) {
                present.erase(it);
            }
            fifo.pop_front();
        }
    }

    // Returns true (and removes the entry) if address was found - a "ghost
    // cache hit" is a one-shot signal, not a persistent membership test,
    // matching how a real victim cache reinserts the line into the main
    // array on a hit rather than leaving a copy behind. The stale (address,
    // seq) pair left in `fifo` is harmless: it'll be skipped via the
    // sequence-number check whenever it's eventually popped.
    bool contains_and_remove(uint64_t address) {
        debug_lookup_count++;
        auto it = present.find(address);
        if (it == present.end()) return false;
        present.erase(it);
        debug_hit_count++;
        return true;
    }
};

#endif
