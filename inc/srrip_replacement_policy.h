#ifndef __SRRIP_REPLACEMENT_POLICY_H__
#define __SRRIP_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"
#include <climits>

class SRRIP : public BasePolicy {
    const uint64_t maxRRPV;;
    uint64_t denseRRPV;
    uint64_t sparseRRPV;

    uint64_t diff = UINT64_MAX;

    public:
    SRRIP(): maxRRPV(3) {}

    SRRIP(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level):
//        maxRRPV(((1ul) << ((64) - (__builtin_clz(_num_ways)))-1)) {
        maxRRPV(_num_ways-1) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, maxRRPV);
        insertion_clock.resize(num_ways, 0);
        denseRRPV = maxRRPV;
        sparseRRPV = maxRRPV;
        level = _level;
        global_clock = 0;
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

    void repartition_ways(uint64_t num_ways_to_reserve);

    uint64_t get_reserved_ways() {return reserved_ways;}
};

#endif
