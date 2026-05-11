#include "lru_replacement_policy.h"
#include <fmt/core.h>


void LRU::init_counter(PacketPtr packet) {
    mru_counter += num_ways;
    if (packet->is_low_reuse) {
        lru_counter++;
        mru_counter++;
    }
}

void LRU::hit_update(uint64_t way_idx) {
    counter[way_idx] = mru_counter;
}

void LRU::fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed) {
    if (packet->is_low_reuse) {
        counter[way_idx] = lru_counter;
    } else {
        counter[way_idx] = mru_counter;
    }
}

uint64_t LRU::get_eviction_candidate(bool is_low_priority = false) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t LRU::get_reserved_eviction_candidate(bool is_low_priority = false) {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void LRU::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
}

uint64_t LRU::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t LRU::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void LRU::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
