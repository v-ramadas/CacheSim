#ifndef __SRRIP_REPLACEMENT_POLICY_H__
#define __SRRIP_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"

class SRRIP : public BasePolicy {
    const uint64_t maxRRPV;;
    uint64_t denseRRPV;
    uint64_t sparseRRPV;

    public:
    SRRIP(): maxRRPV(3) {}

    SRRIP(uint64_t _set_idx, uint64_t _num_ways):
        maxRRPV(_num_ways) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, maxRRPV);
        denseRRPV = maxRRPV;
        sparseRRPV = maxRRPV/2;
    }
    
    void init_counter(bool is_sparse);

    void hit_update(uint64_t way_idx);

    void fill_update(uint64_t way_idx, bool is_sparse);

    uint64_t get_eviction_candidate();

    void evict(uint64_t way_idx);

    uint64_t get_counter_value(uint64_t way_idx);

    uint64_t count_distance(uint64_t threshold);

    void print();

};

#endif
