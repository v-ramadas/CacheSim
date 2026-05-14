#ifndef __LFU_REPLACEMENT_POLICY_H__
#define __LFU_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"

class LFU : public BasePolicy {
    uint64_t min_counter = 0;
    public:

    LFU() {}

    LFU(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, min_counter);
        level = _level;
        reserved_ways = 0;
    }
    
    void init_counter(PacketPtr packet);

    void hit_update(uint64_t way_idx);

    void fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed);

    uint64_t get_eviction_candidate(bool is_low_priority);

    uint64_t get_reserved_eviction_candidate(bool is_low_priority);

    void evict(uint64_t way_idx);

    uint64_t get_counter_value(uint64_t way_idx);

    uint64_t count_distance(uint64_t threshold);

    void repartition_ways(uint64_t ways_to_reserve);

    uint64_t get_reserved_ways() {return reserved_ways;}

    bool can_insert(PacketPtr packet, uint64_t idx) { return true;}

};

#endif
