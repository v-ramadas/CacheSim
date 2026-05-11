#include "lfu_replacement_policy.h"
#include <fmt/core.h>


void LFU::init_counter(PacketPtr packet) {
}

void LFU::hit_update(uint64_t way_idx) {
    counter[way_idx]++;
}

void LFU::fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed) {
    counter[way_idx] = packet->l1_hits;
}

uint64_t LFU::get_eviction_candidate(bool is_low_priority = false) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t LFU::get_reserved_eviction_candidate(bool is_low_priority = false) {
    assert(reserved_ways != 0);
    auto way = std::max_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void LFU::evict(uint64_t way_idx) {
    counter[way_idx] = UINT64_MAX;
}

uint64_t LFU::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t LFU::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void LFU::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
