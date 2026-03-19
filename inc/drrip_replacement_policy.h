#ifndef __DRRIP_REPLACEMENT_POLICY_H__
#define __DRRIP_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"

class DRRIP : public BasePolicy {
    static constexpr uint64_t maxRRPV = 3;
    static constexpr std::size_t NUM_POLICY = 2;
    static constexpr std::size_t SDM_SIZE = 32;
    static constexpr uint64_t BIP_MAX = 32;
    static constexpr uint64_t PSEL_WIDTH = 10;
    static constexpr uint64_t PSEL_MAX = (1 << PSEL_WIDTH);

    uint64_t bip_counter;
    uint64_t PSEL = 0;

    public:
    DRRIP() {}
    DRRIP(uint64_t _set_idx, uint64_t _num_ways) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, maxRRPV);
    }
    
    void init_counter(bool is_sparse);

    void hit_update(uint64_t way_idx);

    void fill_update(uint64_t way_idx, bool is_sparse);

    uint64_t get_eviction_candidate();

    void evict(uint64_t way_idx);

    uint64_t get_counter_value(uint64_t way_idx);

    uint64_t count_distance(uint64_t threshold);

    void print();

    void update_bip(uint64_t way_idx);

    void inc_psel() {
        if (PSEL != PSEL_MAX)
            PSEL++;
    }

    void dec_psel(){
        if (PSEL != 0)
            PSEL--;
    }

};

#endif
