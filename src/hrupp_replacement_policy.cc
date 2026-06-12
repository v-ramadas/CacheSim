#include "hrupp_replacement_policy.h"
#include <fmt/core.h>


void HRUpp::init_counter(PacketPtr packet) {
    auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    lru_counter++;
    mru_counter+=num_ways;
}

void HRUpp::hit_update(PacketPtr packet, uint64_t way_idx) {   
    auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    if (low_priority[way_idx] && is_hub_node) {
        low_priority[way_idx] = false;
    }

    if (!low_priority[way_idx]) {
        counter[way_idx] += mru_counter;
        if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Hub Node Hit. PC {:#x} address {:#x} set {} way {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx,
                packet->degree, packet->l1_hits, counter[way_idx]);
    } else {
        if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Non-Hub Node Hit. PC {:#x} address {:#x} set {} way {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx,
                packet->degree, packet->l1_hits, counter[way_idx]);
        counter[way_idx] += lru_counter;
    }
}

void HRUpp::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    if (packet->serviced_from_llc > 0) {
        if (is_hub_node) {
            counter[way_idx] = (packet->l1_hits * mru_counter);
            low_priority[way_idx] = false;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Hub Fill Update. PC {:#x} address {:#x} set {} way {} hub {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx, is_hub_node,
                packet->degree, packet->l1_hits, counter[way_idx]);
        } else {
            counter[way_idx] = (packet->l1_hits * lru_counter);
            low_priority[way_idx] = true;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Serviced From LLC earlier. Non-Hub Fill Update. PC {:#x} address {:#x} set {} way {} hub {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx, is_hub_node,
                packet->degree, packet->l1_hits, counter[way_idx]);
        }
    } else {if (is_hub_node) {// && g_block_size < CACHELINE_SIZE) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Hub Fill Update. PC {:#x} address {:#x} set {} way {} hub {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx, is_hub_node,
                packet->degree, packet->l1_hits, counter[way_idx]);
        } else {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
            if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Non-Hub Fill Update. PC {:#x} address {:#x} set {} way {} hub {} degree {} l1_hits {} repl_policy {}\n", packet->pc, packet->address, set_idx, way_idx, is_hub_node,
                packet->degree, packet->l1_hits, counter[way_idx]);
        }
    }
}

uint64_t HRUpp::get_eviction_candidate(bool is_low_priority = false) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
                fmt::print("Evicting set {} way {} counter {}\n", set_idx, way_idx, counter[way_idx]);
    return way_idx;
}

uint64_t HRUpp::get_reserved_eviction_candidate(bool is_low_priority = false) {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void HRUpp::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
    low_priority[way_idx] = true;
}

uint64_t HRUpp::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t HRUpp::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void HRUpp::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
