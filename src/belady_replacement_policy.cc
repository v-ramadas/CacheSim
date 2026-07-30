#include "belady_replacement_policy.h"
#include <fmt/core.h>

void Belady::init_counter(PacketPtr /*packet*/) {
}

void Belady::hit_update(PacketPtr /*packet*/, uint64_t /*way_idx*/) {
}

void Belady::fill_update(uint64_t way_idx, uint64_t /*block_idx*/, PacketPtr packet, bool /*was_accessed*/) {
    counter[way_idx] = packet->next_reuse;
}

uint64_t Belady::get_eviction_candidate(bool /*is_low_priority=false*/) {
    auto way = std::max_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;

}

uint64_t Belady::get_reserved_eviction_candidate(bool /*is_low_priority = false*/) {
    assert(reserved_ways != 0);
    auto way = std::max_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

void Belady::evict(uint64_t way_idx) {
    counter[way_idx] = 0;
}

uint64_t Belady::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t Belady::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void Belady::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
