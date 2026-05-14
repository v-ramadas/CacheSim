#include "fission_replacement_policy.h"
#include <fmt/core.h>

void Fission::init_counter(PacketPtr packet) {
}

void Fission::hit_update(uint64_t way_idx) {
}

void Fission::fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed) {
    counter[way_idx] = packet->reuse_distance;
}

uint64_t Fission::get_eviction_candidate(bool is_low_priority=false) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    if (counter[way_idx] == 1)
        return way_idx;

    way = std::max_element(counter.begin(), std::next(counter.begin(), num_ways));
    way_idx = std::distance(counter.begin(), way);
    return way_idx;

}

uint64_t Fission::get_reserved_eviction_candidate(bool is_low_priority = false) {
    assert(reserved_ways != 0);
    auto way = std::max_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

void Fission::evict(uint64_t way_idx) {
    std::transform(counter.cbegin(), std::next(counter.cend()), counter.begin(), [](auto x) {
            if (x > 1)
                return x-1;
            });
    counter[way_idx] = 0;
}

uint64_t Fission::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t Fission::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void Fission::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
