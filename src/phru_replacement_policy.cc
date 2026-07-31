#include "phru_replacement_policy.h"
#include <fmt/core.h>


void PHRU::init_counter(PacketPtr /*packet*/) {
    lru_counter++;
    mru_counter+=num_ways;
}

void PHRU::hit_update(PacketPtr packet, uint64_t way_idx) {   
    auto is_hub_node = (packet->block_serviced_from_llc[way_idx] >= hub_threshold);
    if (low_priority[way_idx] && is_hub_node) {
        low_priority[way_idx] = false;
    }

    if (!low_priority[way_idx]) {
        counter[way_idx] = mru_counter;
    } else {
        counter[way_idx] = lru_counter;
    }
}

void PHRU::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    auto is_hub_node = (packet->block_serviced_from_llc[block_idx] >= hub_threshold);
    if (packet->serviced_from_llc > 0) {
        if (is_hub_node && was_accessed) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
        } else if (is_hub_node && !was_accessed) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
        } else if (was_accessed) {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
        } else {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
        }
    } else {
        counter[way_idx] = lru_counter;
        low_priority[way_idx] = true;
    }
}

uint64_t PHRU::get_eviction_candidate(bool /*is_low_priority = false*/) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t PHRU::get_reserved_eviction_candidate(bool /*is_low_priority = false*/) {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void PHRU::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
    low_priority[way_idx] = true;
}

uint64_t PHRU::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t PHRU::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void PHRU::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

bool PHRU::can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) {
    return true;
}

template<typename VecType>
void PHRU::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool /*incr*/) {
    vec.resize(prev_num_ways*scale_factor);
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            vec[baseIdx + j] = val;
        }
    }
}

void PHRU::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    assert(old_block_size != new_block_size);
    uint64_t scale_factor = old_block_size/new_block_size;
    uint64_t new_num_ways = num_ways*scale_factor;

    // First, resize all the vectors to break down their contents
    breakdown(counter, num_ways, scale_factor, false);
    breakdown(low_priority, num_ways, scale_factor, false);

    num_ways = new_num_ways;
}

template<typename VecType>
void PHRU::contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks) {
    assert(way_idx + num_blocks <= vec.size());
    auto start_it = vec.begin() + way_idx;
    auto end_it = vec.begin() + (way_idx + num_blocks);
    vec.erase(start_it, end_it);
}

void PHRU::set_contract(uint64_t way_idx, uint64_t size) {
    // First, resize all the vectors to break down their contents
    contract(counter, way_idx, size);
    contract(low_priority, way_idx, size);
    num_ways -= size;
}
