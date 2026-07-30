#ifndef __DRRIP_REPLACEMENT_POLICY_H__
#define __DRRIP_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"
#include <algorithm>
#include <random>
#include <utility>

class DRRIP : public BasePolicy {
    const uint64_t maxRRPV ;
    uint64_t denseRRPV;
    uint64_t sparseRRPV;
    uint64_t diff = UINT64_MAX;

    static constexpr std::size_t NUM_POLICY = 2;
    static constexpr std::size_t SDM_SIZE = 32;
    static constexpr uint64_t BIP_MAX = 32;
    static constexpr uint64_t PSEL_WIDTH = 10;
    static constexpr uint64_t PSEL_MAX = (1 << PSEL_WIDTH);

    uint64_t bip_counter;
    uint64_t PSEL = 0;

    public:
    DRRIP() : maxRRPV(3) {}
    DRRIP(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level) :
        maxRRPV(std::bit_floor(_num_ways)-1) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        counter.resize(num_ways, maxRRPV);
        level = _level;
        denseRRPV = maxRRPV;
        sparseRRPV = maxRRPV;
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

    void update_bip(uint64_t way_idx, PacketPtr packet);

    void update_srrip(uint64_t way_idx, PacketPtr packet);

    void inc_psel() {
        if (PSEL != PSEL_MAX)
            PSEL++;
    }

    void dec_psel(){
        if (PSEL != 0)
            PSEL--;
    }

    void repartition_ways(uint64_t num_ways_to_reserve);

    uint64_t get_reserved_ways() {return reserved_ways;}

    bool can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) { return true;}

};

#endif
