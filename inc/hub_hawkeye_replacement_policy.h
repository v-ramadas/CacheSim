#ifndef __HUB_HAWKEYE_REPLACEMENT_POLICY_H__
#define __HUB_HAWKEYE_REPLACEMENT_POLICY_H__

#include "hawkeye_replacement_policy.h"
#include <algorithm>
#include <vector>

// HubHawkeye's block predictor: a bounded, set-associative table of saturating counters keyed by block
// address, sized at construction to at most the storage of Hawkeye's own predictor state (SHCT + sampler +
// OPTgen; Hawkeye::predictor_budget_bits), so HubHawkeye uses at most 2x Hawkeye's budget. Counters have the
// SHCT's range, initial value and threshold. Entry = TAG_BITS partial tag (from the CRC hash, as the Hawkeye
// sampler) + 5-bit counter + valid + log2(WAYS) LRU bits. Lookups never allocate - a block with no entry
// predicts friendly, as a fresh counter would; training (increment / decrement) allocates, evicting the LRU
// entry. With a table large enough never to evict, this matches an unbounded per-block table exactly.
class HawkeyeBlockPredictor {
    public:
    static constexpr uint64_t WAYS = 16, TAG_BITS = 12;
    static constexpr uint64_t ENTRY_BITS = TAG_BITS + 5 + 1 + 4;   // tag + counter + valid + LRU (log2 WAYS)

    private:
    struct Entry { bool valid = false; uint16_t tag = 0; uint8_t ctr = 0; uint64_t lru = 0; };
    std::vector<Entry> table;
    uint64_t sets, clock = 0;

    Entry* find(uint64_t block_number, bool allocate) {
        uint64_t h = HawkeyePCPredictor::CRC(block_number);
        uint64_t set = h % sets;
        uint16_t tag = (h / sets) & ((1ULL << TAG_BITS) - 1);
        Entry* row = &table[set * WAYS];
        Entry* victim = row;
        for (uint64_t w = 0; w < WAYS; w++) {
            if (row[w].valid && row[w].tag == tag) {
                row[w].lru = ++clock;
                hits++;
                return &row[w];
            }
            if (!row[w].valid || (victim->valid && row[w].lru < victim->lru))
                victim = &row[w];
        }
        misses++;
        if (!allocate)
            return nullptr;
        if (victim->valid) evictions++;
        allocations++;
        *victim = Entry{true, tag, (uint8_t)((1 + MAX_SHCT) / 2), ++clock};
        return victim;
    }

    public:
    uint64_t hits = 0, misses = 0, allocations = 0, evictions = 0;
    explicit HawkeyeBlockPredictor(uint64_t entries) : sets(std::max<uint64_t>(1, entries / WAYS)) {
        table.resize(sets * WAYS);
    }
    uint64_t num_entries() const { return sets * WAYS; }
    uint64_t num_sets() const { return sets; }
    uint64_t storage_bits() const { return num_entries() * ENTRY_BITS; }

    void increment(uint64_t block_addr, uint64_t block_shift) {
        auto* e = find(block_addr >> block_shift, true);
        if (e->ctr < MAX_SHCT) e->ctr++;
    }
    void decrement(uint64_t block_addr, uint64_t block_shift) {
        auto* e = find(block_addr >> block_shift, true);
        if (e->ctr > 0) e->ctr--;
    }
    bool get_prediction(uint64_t block_addr, uint64_t block_shift) {
        auto* e = find(block_addr >> block_shift, false);
        return e == nullptr || e->ctr >= (MAX_SHCT + 1) / 2;
    }
    size_t num_tracked() const {
        return std::count_if(table.begin(), table.end(), [](const Entry& e) { return e.valid; });
    }
};

// Hawkeye plus a per-block-address predictor, so it can learn which data
// (hub vertices) to keep rather than only which load PCs reuse. A fill is
// cache-friendly only if the PC (SHCT) and the block are both friendly
// (PC-gated). The block table is trained
//  - in sampled sets, with OPTgen's label for the sampled block (as the PC);
//  - in all sets, up when a block is refilled after being serviced from the
//    LLC, down when a cache-friendly line is evicted as the LRU victim.
class HubHawkeye : public Hawkeye {
    inline static HawkeyeBlockPredictor* block_predictor;
    inline static Budget hawkeye_budget{};   // Hawkeye's predictor bits for this cache (the table's cap)
    std::vector<uint64_t> block_addrs;   // block address held by each way

    inline static uint64_t stat_fills = 0, stat_pc_friendly = 0, stat_block_veto = 0, stat_friendly = 0;
    inline static uint64_t stat_tp = 0, stat_fp = 0, stat_fn = 0, stat_tn = 0;

    void train_sampled(uint64_t paddr, bool friendly) override;
    bool predict(uint64_t way_idx, uint64_t block_idx, uint64_t paddr, PacketPtr packet, bool pc_prediction) override;
    void lru_evicted(uint64_t way_idx) override;

    public:
    static void print_stats();
    HubHawkeye(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level, uint64_t _num_sets) : Hawkeye(_set_idx, _num_ways, _level, _num_sets) {
        block_addrs.resize(num_ways, UINT64_MAX);
        if (block_predictor == nullptr) {
            hawkeye_budget = predictor_budget_bits();
            uint64_t entries = hawkeye_budget.total() / HawkeyeBlockPredictor::ENTRY_BITS;
            block_predictor = new HawkeyeBlockPredictor(entries / HawkeyeBlockPredictor::WAYS * HawkeyeBlockPredictor::WAYS);
        }
    }

    void evict(uint64_t way_idx) override;
    void set_breakdown(uint64_t old_block_size, uint64_t new_block_size) override;
    void set_contract(uint64_t way_idx, uint64_t size) override;
    void set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways) override;
};

#endif
