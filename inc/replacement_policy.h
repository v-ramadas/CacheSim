#ifndef __REPLACEMENT_POLICY_H__
#define __REPLACEMENT_POLICY_H__

#include <algorithm>
#include <vector>
#include <cstdint>
#include "utils.h"
#include <cassert>
#include <bit>

class BasePolicy {
    protected:
    std::vector<uint64_t> counter;
    std::vector<uint64_t> insertion_clock;
    uint64_t set_idx;
    uint64_t num_ways;
    uint64_t reserved_ways;
    uint64_t level;
    uint64_t global_clock;

    public:
    BasePolicy() = default;
    virtual ~BasePolicy() = default;
    virtual void init_counter(PacketPtr packet) = 0;
    virtual void hit_update(PacketPtr packet, uint64_t way_idx) = 0;
    virtual void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed = true) = 0;
    virtual uint64_t get_eviction_candidate(bool) = 0;
    virtual uint64_t get_reserved_eviction_candidate(bool) = 0;
    virtual void evict(uint64_t way_idx) = 0;
    virtual uint64_t get_counter_value(uint64_t way_idx) = 0;
    virtual uint64_t count_distance(uint64_t threshold) = 0;
    virtual void repartition_ways(uint64_t num_ways_to_reserve) = 0;
    virtual uint64_t get_reserved_ways() = 0;
    virtual bool can_insert(PacketPtr packet, uint64_t idx) = 0;
};

#endif
