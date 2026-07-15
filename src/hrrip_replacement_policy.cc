#include "hrrip_replacement_policy.h"
#include <fmt/core.h>

void HRRIP::init_counter(PacketPtr packet) {
    if (diff < maxRRPV)
        std::transform(counter.cbegin(), std::next(counter.cend()), low_priority.cbegin(), counter.begin(), [_diff = diff, _maxRRPV = maxRRPV](auto x, bool is_low_priority) { 
            uint64_t cur_diff = (is_low_priority) ? std::min((uint64_t)1, _diff) : _diff;    
            uint64_t val = x + cur_diff;
                if (val <= _maxRRPV) return val;
                else return _maxRRPV;
        });

    diff = UINT64_MAX;
    global_clock++;

}

void HRRIP::hit_update(PacketPtr packet, uint64_t way_idx) {
    auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    auto old_counter_value = counter[way_idx];
    if ((int)(packet->degree) < packet->avg_degree/2) {
        //counter[way_idx] = std::min(maxRRPV - 1, (uint64_t)((float)(packet->avg_degree/packet->degree)*maxRRPV));
        if (counter[way_idx] > 0) counter[way_idx]--;
        else counter[way_idx] = 0;
    } else if (packet->degree == 2) {
        counter[way_idx] = 0;
    } else {
        counter[way_idx] = 0;
    }
    if (cachesim::DEBUG ||cachesim::REPLACEMENT_POLICY_DEBUG)
        fmt::print("Hit Update. PC {:#x} address {:#x} way {} hub {} degree {} l1_hits {} repl_policy_old {} repl_policy_new {}\n", packet->pc, packet->address, way_idx, is_hub_node,
        packet->degree, packet->l1_hits, old_counter_value, counter[way_idx]);
}

void HRRIP::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    //auto packet_reuse_probability = packet->reuse_probability;
    double packet_reuse_probability = (double)(__builtin_popcountll(packet->footprint))/8.0d;
    auto is_hub_node = (packet->degree > uint64_t(packet->avg_degree));
    if (packet->serviced_from_llc > 0) {
        if (is_hub_node)
            counter[way_idx] = 0;
        else if ((int)(packet->degree) < packet->avg_degree/2)
            counter[way_idx] = std::max(0UL, maxRRPV - packet->l1_hits);
        else
            counter[way_idx] = 0;
            //counter[way_idx] = (int)((1.0d - packet_reuse_probability)*maxRRPV);
    } else {
        if (is_hub_node && was_accessed) {
            counter[way_idx] =  0;
            //counter[way_idx] = (int)((float)(packet->avg_degree/packet->degree)*maxRRPV);
        } else if (is_hub_node && !was_accessed) {
            counter[way_idx] = maxRRPV-2;
        } else { 
            counter[way_idx] = maxRRPV-1;
            //counter[way_idx] = std::min(maxRRPV - 1,(uint64_t)((float)(packet->avg_degree/packet->degree)*maxRRPV));
        }
    }

    if (is_hub_node && was_accessed) {
        //low_priority[way_idx] = true;
        //if (packet->blocks.size() < 8)
        //fmt::print("HRRIP Node. PC {:#x} address {:#x} way {} size {} density {} footprint {:#x} l1_hits {}\n", packet->pc, packet->address, way_idx, packet->blocks.size(), __builtin_popcountll(packet->footprint), packet->footprint, packet->l1_hits); 
    }
    if (cachesim::DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG)
        fmt::print("Fill Update. PC {:#x} address {:#x} way {} hub {} degree {} l1_hits {} repl_policy_new {}\n", packet->pc, packet->address, way_idx, is_hub_node,
        packet->degree, packet->l1_hits, counter[way_idx]);
}

uint64_t HRRIP::get_eviction_candidate(bool is_low_priority=false) {
    
    // Lambda to encapsulate the comparison logic for reuse
    auto is_better_candidate = [&](uint64_t current_idx, uint64_t best_idx, bool compare_priority=false) {
        if (counter[current_idx] > maxRRPV) return false;
        if (counter[current_idx] > counter[best_idx]) return true;
        if (compare_priority && (counter[current_idx] == counter[best_idx])) {
            if (low_priority[best_idx] == true) return false;
            if (low_priority[best_idx] == false && low_priority[current_idx] == true) return true;
            else return false;
        }
        return false;
    };


    // Step 1: If requested, try searching ONLY low_priority entries
    uint64_t candidate_idx = num_ways; // Initialize with invalid index
    for (uint64_t idx = 0; idx < num_ways; idx++) {
        if (low_priority[idx] && counter[idx] <= maxRRPV) {
            if (is_better_candidate(idx, candidate_idx, true)) {
                candidate_idx = idx;
            }
        }
    }

    // Step 2: Fallback (if no low_priority found OR is_low_priority was false)
    if (candidate_idx >= num_ways) {
        candidate_idx = 0; 
        for (uint64_t idx = 0; idx < num_ways; idx++) {
            if (counter[idx] > maxRRPV) continue;

            // Handle initial candidate validity
            if (counter[candidate_idx] > maxRRPV) {
                candidate_idx = idx;
                continue;
            }

            if (is_better_candidate(idx, candidate_idx, false)) {
                candidate_idx = idx;
            }
        }
    }

    assert(candidate_idx < num_ways);

    // Update diff based on the final candidate found
    if (counter[candidate_idx] < maxRRPV) {
        diff = std::min(diff, maxRRPV - counter[candidate_idx]);
    }

    return candidate_idx;
}

uint64_t HRRIP::get_reserved_eviction_candidate(bool is_low_priority = false) {
    assert(reserved_ways != 0);
    auto candidate_idx = 0;
    auto candidate = counter[candidate_idx];
    for (uint64_t idx = num_ways; idx < num_ways+reserved_ways; idx++) {
        candidate = counter[candidate_idx];
        auto way = counter[idx];

        if (way > maxRRPV) continue;
        if (candidate > maxRRPV) {
            candidate_idx = idx;
            continue;
        }

        if (way > candidate) candidate_idx = idx;
    }

    diff = std::min(diff, maxRRPV - candidate);
    return candidate_idx;
}

void HRRIP::evict(uint64_t way_idx) {
    if (cachesim::DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG)
        fmt::print("Evicting way {} counter value {}\n",
        way_idx, counter[way_idx]);
    counter[way_idx] = UINT_MAX;
    low_priority[way_idx] = false;
}

uint64_t HRRIP::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t HRRIP::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void HRRIP::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

bool HRRIP::can_insert(PacketPtr packet, uint64_t idx) {
    return true;
    if (packet->degree == 0) return false;
    else return true;
    //if (packet->serviced_from_llc == 0) return true;
    auto was_accessed = ((packet->footprint >> idx)&0x1 == 0x1);
    auto is_hub_node = (packet->degree > int(packet->avg_degree));
    if (is_hub_node && was_accessed) return true;
    if (is_hub_node && !was_accessed) return false;
    else if (!packet->is_high_reuse) return false;
    else return true;
}

