#include "lru_replacement_policy.h"
#include <fmt/core.h>


void LRU::init_counter(PacketPtr /*packet*/) {
    lru_counter++;
}

void LRU::hit_update(PacketPtr /*packet*/, uint64_t way_idx) {
    counter[way_idx] = lru_counter;
}

void LRU::fill_update(uint64_t way_idx, uint64_t /*block_idx*/, PacketPtr /*packet*/, bool /*was_accessed*/) {
    counter[way_idx] = lru_counter;
}

uint64_t LRU::get_eviction_candidate(bool /*is_low_priority = false*/) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t LRU::get_reserved_eviction_candidate(bool /*is_low_priority = false*/) {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void LRU::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
    low_priority[way_idx] = false;
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

template<typename VecType>
void LRU::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool /*incr*/) {
    vec.resize(prev_num_ways*scale_factor);
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            vec[baseIdx + j] = val;
        }
    }
}

void LRU::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    assert(old_block_size != new_block_size);
    uint64_t scale_factor = old_block_size/new_block_size;
    uint64_t new_num_ways = num_ways*scale_factor;

    breakdown(counter, num_ways, scale_factor, false);
    breakdown(low_priority, num_ways, scale_factor, false);

    num_ways = new_num_ways;
}

template<typename VecType>
void LRU::contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks) {
    assert(way_idx + num_blocks <= vec.size());
    auto start_it = vec.begin() + way_idx;
    auto end_it = vec.begin() + (way_idx + num_blocks);
    vec.erase(start_it, end_it);
}

void LRU::set_contract(uint64_t way_idx, uint64_t size) {
    contract(counter, way_idx, size);
    contract(low_priority, way_idx, size);
    num_ways -= size;
}

void LRU::set_merge(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/, uint64_t new_num_ways) {
    // No sound way to merge per-way LRU state across a group of small
    // blocks, so reset it for the new (larger-block) geometry instead, same
    // defaults as a fresh construction.
    counter.assign(new_num_ways, max_counter);
    low_priority.assign(new_num_ways, false);
    num_ways = new_num_ways;
}
