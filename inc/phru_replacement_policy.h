#ifndef __PHRU_REPLACEMENT_POLICY_H__
#define __PHRU_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"

class PHRU : public BasePolicy {
    uint64_t max_counter = UINT64_MAX;
    uint64_t mru_counter = 0;
    uint64_t lru_counter = 0;
    uint64_t hub_threshold = 1;
    std::vector<bool> low_priority;
    bool is_way_full = false;
    bool is_low_priority_present = true;

    // Instrumentation only - measures how well the LLC-reuse heuristic
    // (block_serviced_from_llc >= hub_threshold) tracks the true,
    // degree-based hub classification HRU has direct access to. Shared
    // across all per-set PHRU instances (one per set) since we want one
    // confusion matrix for the whole run, not 1024 tiny ones.
    inline static uint64_t heur_tp = 0; // heuristic says hub, degree says hub
    inline static uint64_t heur_fp = 0; // heuristic says hub, degree says not
    inline static uint64_t heur_fn = 0; // heuristic says not, degree says hub
    inline static uint64_t heur_tn = 0; // heuristic says not, degree says not
    static void record_heuristic(bool predicted_hub, bool actual_hub);

    // Diagnostic: for false negatives AND true negatives (fill-time misses
    // only, where l1_hits/footprint are actually meaningful - see
    // fill_update()), histogram l1_hits and footprint_count to look for an
    // exploitable pattern instead of guessing at one. TN is tracked
    // alongside FN specifically to check whether "touched once, l1_hits=0,
    // footprint=1" is a hub-specific signature or just what most lines look
    // like regardless of true hub status - if TN's distribution matches
    // FN's, that profile can't discriminate anything by construction.
    inline static uint64_t fn_both_zero = 0;
    inline static uint64_t fn_l1hits_gt_footprint = 0;
    inline static uint64_t fn_l1hits_hist[8] = {0}; // buckets: 0,1,2,3,4-7,8-15,16-31,32+
    inline static uint64_t fn_footprint_hist[9] = {0}; // buckets: exactly 0..8
    inline static uint64_t tn_both_zero = 0;
    inline static uint64_t tn_l1hits_gt_footprint = 0;
    inline static uint64_t tn_l1hits_hist[8] = {0};
    inline static uint64_t tn_footprint_hist[9] = {0};
    static void record_pattern(bool is_fn, uint64_t l1_hits, uint64_t footprint_count);

    public:
    static void print_heuristic_stats();

    PHRU() {}
    PHRU(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level) {
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

    void set_breakdown(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/);

    template<typename VecType>
    void breakdown(std::vector<VecType>& /*vec*/, uint64_t /*prev_num_ways*/, uint64_t /*scale_factor*/, bool /*incr*/);

    void set_contract(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/);

    void set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways);

    template<typename VecType>
    void contract(std::vector<VecType>& /*vec*/, uint64_t /*prev_num_ways*/, uint64_t /*scale_factor*/);
};

#endif
