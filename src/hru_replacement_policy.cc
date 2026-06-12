#include "hru_replacement_policy.h"
#include <fmt/core.h>


void HRU::init_counter(PacketPtr packet) {
    auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    lru_counter++;
    mru_counter+=num_ways;
}

void HRU::hit_update(PacketPtr packet, uint64_t way_idx) {   
    auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    if (low_priority[way_idx] && is_hub_node) {
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
    auto is_hub_line = (packet->degree > uint64_t(packet->avg_degree));
    if (packet->serviced_from_llc > 0) {
        if (packet->is_hub_node && was_accessed) {
            counter[way_idx] = packet->llc_counter_values[block_idx];
            low_priority[way_idx] = false;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Hub Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {} block idx {} size of vector {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx], block_idx, packet->llc_counter_values.size());
        } else if (packet->is_hub_node && !was_accessed) {
            counter[way_idx] = packet->llc_counter_values[block_idx];
            low_priority[way_idx] = false;
        } else if (was_accessed) {
            low_priority[way_idx] = true;
            counter[way_idx] = packet->llc_counter_values[block_idx];
           if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Non-Hub but Accessed Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {} block idx {} size of vector {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx], block_idx, packet->llc_counter_values.size());
        } else if (!was_accessed) {
            counter[way_idx] = packet->llc_counter_values[block_idx];
           if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Non-Hub and Unaccessed Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {} block idx {} size of vector {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx], block_idx, packet->llc_counter_values.size());

        } else {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
        }
    } else {
        if (packet->is_hub_node && was_accessed) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Hub Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} hub {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx, packet->is_hub_node,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx]);
        } else {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Non-Hub Fill Update. PC {:#x} address {:#x} block_address {:#x} set {} way {} hub {} degree {} l1_hits {} was_accessed {} footprint {:#x} repl_policy {}\n", packet->pc, packet->address, packet->blocks[block_idx], set_idx, way_idx, packet->is_hub_node,
                packet->degree, packet->l1_hits, was_accessed, packet->footprint, counter[way_idx]);
        }
    }
}

uint64_t HRU::get_eviction_candidate(bool is_low_priority = false) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t HRU::get_reserved_eviction_candidate(bool is_low_priority = false) {
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

bool HRU::can_insert(PacketPtr packet, uint64_t idx) {
    return true;
    //if (packet->serviced_from_llc <= 2) return true;
    //auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    //auto was_accessed = ((packet->footprint >> idx)&0x1 == 0x1);
    //if (is_hub_node && !was_accessed) { 
    //    return false; 
    //} else {
    //    return true;
    //}
}
