#ifndef __PRRIP_REPLACEMENT_POLICY_H__
#define __PRRIP_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"
#include <climits>

class PRRIP : public BasePolicy {
    const uint64_t maxRRPV;;
    uint64_t denseRRPV;
    uint64_t sparseRRPV;
    std::vector<bool> low_priority;
    std::vector<float> reuse_probability;
    uint64_t diff = UINT64_MAX;

    public:
    PRRIP(): maxRRPV(3) {}

    PRRIP(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level):
        maxRRPV(std::bit_floor(_num_ways)-1) {
//        maxRRPV(3) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, UINT_MAX);
        insertion_clock.resize(num_ways, 0);
        low_priority.resize(num_ways, false);
        reuse_probability.resize(num_ways, 0.0);
        denseRRPV = maxRRPV;
        sparseRRPV = maxRRPV;
        level = _level;
        global_clock = 0;
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

    void repartition_ways(uint64_t num_ways_to_reserve);

    uint64_t get_reserved_ways() {return reserved_ways;}

    bool can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) {return true;}

    void set_breakdown(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/) {}

    void set_contract(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/) {}
};

#endif
