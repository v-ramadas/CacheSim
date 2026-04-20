#include "plru_replacement_policy.h"
#include <fmt/core.h>


void PLRU::init_counter(PacketPtr packet) {
    mru_counter += num_ways;
    if (packet->is_sparse) {
        lru_counter++;
        mru_counter++;
    }
}

void PLRU::hit_update(uint64_t way_idx) {
    counter[way_idx] = mru_counter;
}

void PLRU::fill_update(uint64_t way_idx, PacketPtr packet) {
    if (packet->is_sparse) {
        counter[way_idx] = lru_counter;
    } else {
        counter[way_idx] = mru_counter;
    }
}

uint64_t PLRU::get_eviction_candidate() {
    auto way = std::min_element(counter.begin(), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t PLRU::get_reserved_eviction_candidate() {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void PLRU::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
}

uint64_t PLRU::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t PLRU::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}
