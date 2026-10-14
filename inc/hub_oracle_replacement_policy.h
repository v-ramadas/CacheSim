#ifndef __HUB_ORACLE_REPLACEMENT_POLICY_H__
#define __HUB_ORACLE_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"

// Oracle upper bound for hub-aware insertion (--replacement-policy hub_oracle).
// PHRU's structure (two clocks, global-min victim, iso-area) but every hub
// decision uses the trace's degree: a block is pinned at MRU iff
// block_degree > avg_degree, on every LLC fill - including the first
// insertion of a line that has never been reused, which HRU and PHRU both
// leave at LRU. Equivalent to `phru --phru-oracle all`, except the hit path
// also uses the true degree. Needs a trace with NodeDegree/PCAvgDegree.
// See notes/first_insertion_hub_prediction.md.
class HubOracle : public BasePolicy {
    uint64_t max_counter = UINT64_MAX;
    uint64_t mru_counter = 0;
    uint64_t lru_counter = 0;
    std::vector<bool> low_priority;

    inline static uint64_t pins_reused = 0;   // pinned blocks of lines with serviced_from_llc > 0
    inline static uint64_t pins_first = 0;    // pinned blocks of never-reused lines
    inline static uint64_t fills = 0;

    public:
    static void print_stats();

    HubOracle() {}
    HubOracle(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, max_counter);
        low_priority.resize(num_ways, true);
        level = _level;
        reserved_ways = 0;
    }

    void init_counter(PacketPtr packet);

    void hit_update(PacketPtr packet, uint64_t way_idx);

    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);

    uint64_t get_eviction_candidate(bool is_low_priority);

    uint64_t get_reserved_eviction_candidate(bool is_low_priority);

    void evict(uint64_t way_idx);

    uint64_t get_counter_value(uint64_t way_idx);

    uint64_t count_distance(uint64_t threshold);

    void repartition_ways(uint64_t ways_to_reserve);

    uint64_t get_reserved_ways() {return reserved_ways;}

    bool can_insert(PacketPtr packet, uint64_t idx);

    void set_breakdown(uint64_t old_block_size, uint64_t new_block_size);

    template<typename VecType>
    void breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor);

    void set_contract(uint64_t way_idx, uint64_t size);

    void set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways);

    template<typename VecType>
    void contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks);
};

#endif
