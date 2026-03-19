#ifndef __REPLACEMENT_POLICY_H__
#define __REPLACEMENT_POLICY_H__

#include <algorithm>
#include <vector>
#include <cstdint>
#include "packet.h"

enum class ReplacementPolicy {
    LRU,
    SRRIP,
    DRRIP,
};

class BasePolicy {
    protected:
    std::vector<uint64_t> counter;
    std::vector<uint64_t> insertion_clock;
    uint64_t set_idx;
    uint64_t num_ways;
    uint64_t level;
    uint64_t global_clock;

    public:
    BasePolicy() = default;
    virtual ~BasePolicy() = default;
    virtual void init_counter(PacketPtr packet) = 0;
    virtual void hit_update(uint64_t way_idx) = 0;
    virtual void fill_update(uint64_t way_idx, PacketPtr packet) = 0;
    virtual uint64_t get_eviction_candidate() = 0;
    virtual void evict(uint64_t way_idx) = 0;
    virtual uint64_t get_counter_value(uint64_t way_idx) = 0;
    virtual uint64_t count_distance(uint64_t threshold) = 0;
    virtual void print() = 0;
};

BasePolicy* create_policy(ReplacementPolicy policy, uint64_t set_idx, uint64_t num_ways, uint64_t level);
#endif
