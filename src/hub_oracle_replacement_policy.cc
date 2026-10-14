#include "hub_oracle_replacement_policy.h"
#include "defs.h"
#include <fmt/core.h>


void HubOracle::init_counter(PacketPtr /*packet*/) {
    lru_counter++;
    mru_counter += num_ways;
}

void HubOracle::print_stats() {
    fmt::print("hub_oracle: fills {} pinned {} (reused lines {}, first insertion {})\n",
        fills, pins_reused + pins_first, pins_reused, pins_first);
}

void HubOracle::hit_update(PacketPtr packet, uint64_t way_idx) {
    // On a hit the packet is the access itself, so this is the accessed
    // node's degree - the same test as HRU's is_hub_node.
    if (low_priority[way_idx] && packet->degree > uint64_t(packet->avg_degree))
        low_priority[way_idx] = false;
    counter[way_idx] = low_priority[way_idx] ? lru_counter : mru_counter;
}

void HubOracle::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool /*was_accessed*/) {
    bool hub;
    if (packet->blocks.size() == 1 && packet->block_degrees.size() > 1) {
        // One LLC block covers the whole line (64B sets): resize_packet()
        // shrinks `blocks` to 1 but leaves the 8 per-sector degrees. Nearly
        // every 64B line holds some hub vertex, so pinning on first insertion
        // floods the cache; use HRU's rule instead - pin only a reused line
        // whose hottest sector is a hub.
        uint64_t degree = *std::max_element(packet->block_degrees.begin(), packet->block_degrees.end());
        hub = packet->serviced_from_llc > 0 && degree > uint64_t(packet->avg_degree);
    } else {
        hub = packet->block_degrees[block_idx] > uint64_t(packet->avg_degree);
    }
    hub |= packet->from_ghost_cache;
    counter[way_idx] = hub ? mru_counter : lru_counter;
    low_priority[way_idx] = !hub;
    if (level == 1) {
        fills++;
        if (hub) (packet->serviced_from_llc > 0 ? pins_reused : pins_first)++;
    }
}

uint64_t HubOracle::get_eviction_candidate(bool /*is_low_priority*/) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    return std::distance(counter.begin(), way);
}

uint64_t HubOracle::get_reserved_eviction_candidate(bool /*is_low_priority*/) {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    return std::distance(counter.begin(), way);
}

void HubOracle::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
    low_priority[way_idx] = true;
}

uint64_t HubOracle::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t HubOracle::count_distance(uint64_t threshold) {
    return std::count_if(counter.begin(), counter.end(), [threshold](uint64_t n) { return n > threshold; });
}

void HubOracle::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

bool HubOracle::can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) {
    return true;
}

template<typename VecType>
void HubOracle::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor) {
    vec.resize(prev_num_ways*scale_factor);
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            vec[baseIdx + j] = val;
        }
    }
}

void HubOracle::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    assert(old_block_size != new_block_size);
    uint64_t scale_factor = old_block_size/new_block_size;
    breakdown(counter, num_ways, scale_factor);
    breakdown(low_priority, num_ways, scale_factor);
    num_ways *= scale_factor;
    // Same as PHRU/HRU: new LRU stamps must not be older than every existing line.
    lru_counter = mru_counter;
}

template<typename VecType>
void HubOracle::contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks) {
    assert(way_idx + num_blocks <= vec.size());
    vec.erase(vec.begin() + way_idx, vec.begin() + (way_idx + num_blocks));
}

void HubOracle::set_contract(uint64_t way_idx, uint64_t size) {
    contract(counter, way_idx, size);
    contract(low_priority, way_idx, size);
    num_ways -= size;
}

void HubOracle::set_merge(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/, uint64_t new_num_ways) {
    counter.assign(new_num_ways, max_counter);
    low_priority.assign(new_num_ways, true);
    num_ways = new_num_ways;
    lru_counter = mru_counter;
}
