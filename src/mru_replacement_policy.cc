#include "mru_replacement_policy.h"
#include <fmt/core.h>


void MRU::init_counter(PacketPtr packet) {
    mru_counter += num_ways;
    if (packet->is_sparse) {
        lru_counter++;
        mru_counter++;
    }
}

void MRU::hit_update(uint64_t way_idx) {
    counter[way_idx] = mru_counter;
}

void MRU::fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed) {
    if (packet->is_sparse) {
        counter[way_idx] = lru_counter;
    } else {
        counter[way_idx] = mru_counter;
    }
}

uint64_t MRU::get_eviction_candidate(bool is_low_priority = false) {
    auto way = std::max_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t MRU::get_reserved_eviction_candidate(bool is_low_priority = false) {
    assert(reserved_ways != 0);
    auto way = std::max_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void MRU::evict(uint64_t way_idx) {
    counter[way_idx] = min_counter;
}

uint64_t MRU::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t MRU::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void MRU::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
