#ifndef __PLRU_REPLACEMENT_POLICY_H__
#define __PLRU_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"

class PLRU : public BasePolicy {
    uint64_t max_counter = UINT64_MAX;
    uint64_t mru_counter = 0;
    uint64_t lru_counter = 0;

    public:
    PLRU() {}
    PLRU(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, UINT64_MAX);
        level = _level;
        reserved_ways = 0;
    }
    
    void init_counter(PacketPtr packet);

    void hit_update(uint64_t way_idx);

    void fill_update(uint64_t way_idx, PacketPtr packet);

    uint64_t get_eviction_candidate();

    uint64_t get_reserved_eviction_candidate();

    void evict(uint64_t way_idx);

    uint64_t get_counter_value(uint64_t way_idx);

    uint64_t count_distance(uint64_t threshold);

    uint64_t get_reserved_ways() {return reserved_ways;}
};

#endif
