#include "hru_replacement_policy.h"
#include <fmt/core.h>


void HRU::init_counter(PacketPtr /*packet*/) {
    lru_counter++;
    mru_counter+=num_ways;
}

void HRU::hit_update(PacketPtr packet, uint64_t way_idx) {   
    if (low_priority[way_idx] && packet->is_hub_node) {
        low_priority[way_idx] = false;
    }

    if (!low_priority[way_idx]) {
        counter[way_idx] = mru_counter;
        if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Hub Node Hit. PC {:#x} address {:#x} set {} way {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx,
                packet->degree, packet->l1_hits, counter[way_idx]);
    } else {
        counter[way_idx] = lru_counter;
        if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Non-Hub Node Hit. PC {:#x} address {:#x} set {} way {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx,
                packet->degree, packet->l1_hits, counter[way_idx]);
    }
}

void HRU::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    if (packet->serviced_from_llc > 0) {
        //auto counter_value = packet->block_serviced_from_llc[block_idx];
        if (packet->is_hub_node && was_accessed) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Hub Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {} block idx {} serviced_from_llc {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx], block_idx, packet->block_serviced_from_llc[block_idx]);
        } else if (packet->is_hub_node && !was_accessed) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
        } else if (was_accessed) {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
           if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Non-Hub but Accessed Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {} block idx {} size of vector {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx], block_idx, packet->block_serviced_from_llc.size());
        } else if (!was_accessed) {
            counter[way_idx] = lru_counter;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Non-Hub and Unaccessed Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {} block idx {} size of vector {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx], block_idx, packet->block_serviced_from_llc.size());
        } else {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
        }
    } else {
//        if (packet->is_hub_node && was_accessed) {
//            counter[way_idx] = mru_counter;
//            low_priority[way_idx] = false;
//            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
//                fmt::print("Hub Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} hub {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx, packet->is_hub_node,
//                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx]);
//        } else {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Non-Hub Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} hub {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx, packet->is_hub_node,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx]);
//        }
    }
    if (std::count(counter.begin(), counter.end(), max_counter) == 0) {
        is_way_full = true;
    } else {
        is_way_full = false;
    }
    if (std::count(low_priority.begin(), low_priority.end(), true) == 0) {
        is_low_priority_present = false;
    } else {
        is_low_priority_present = true;
    }

}

uint64_t HRU::get_eviction_candidate(bool is_low_priority = false) {
    auto way = counter.end();
    for (uint64_t i = 0; i < num_ways; ++i) {
        if (is_low_priority && !low_priority[i]) continue;
        if (way == counter.end() || counter[i] < *way) {
            way = counter.begin() + i;
        }
    }

    if (*way == max_counter) {
        way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    }

    if (way == counter.end()) {
        way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    }

    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t HRU::get_reserved_eviction_candidate(bool /*is_low_priority = false*/) {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void HRU::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
    low_priority[way_idx] = true;
}

uint64_t HRU::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t HRU::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void HRU::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

bool HRU::can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) {
    return true;
    //if (packet->pc == 0xa) return true;
    //if (!is_way_full) return true;
    //else if (packet->pc != 0xa) return false;
    //if (is_low_priority_present) return true;
    //if (packet->degree < (uint64_t)packet->avg_degree) return false;
    return true;   
}

template<typename VecType>
void HRU::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool /*incr*/) {
    vec.resize(prev_num_ways*scale_factor);
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            vec[baseIdx + j] = val;
        }
    }
}

void HRU::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    assert(old_block_size != new_block_size);
    uint64_t scale_factor = old_block_size/new_block_size;
    uint64_t new_num_ways = num_ways*scale_factor;

    // First, resize all the vectors to break down their contents
    breakdown(counter, num_ways, scale_factor, false);
    breakdown(low_priority, num_ways, scale_factor, false);

    num_ways = new_num_ways;
}

template<typename VecType>
void HRU::contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks) {
    assert(way_idx + num_blocks <= vec.size());
    auto start_it = vec.begin() + way_idx;
    auto end_it = vec.begin() + (way_idx + num_blocks);
    vec.erase(start_it, end_it);
}

void HRU::set_contract(uint64_t way_idx, uint64_t size) {
    // First, resize all the vectors to break down their contents
    contract(counter, way_idx, size);
    contract(low_priority, way_idx, size);
    num_ways -= size;
}
