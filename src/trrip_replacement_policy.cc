#include "trrip_replacement_policy.h"
#include <fmt/core.h>

void TRRIP::init_counter(PacketPtr packet) {
    if (diff < maxRRPV)
        std::transform(counter.cbegin(), std::next(counter.cend()), counter.begin(), [_diff = diff, _maxRRPV = maxRRPV](auto x) { 
                uint64_t val = x + _diff;
                if (val <= _maxRRPV) return val;
                else return _maxRRPV;
        });

    diff = UINT64_MAX;
    global_clock++;

}

void TRRIP::hit_update(uint64_t way_idx) {
    counter[way_idx] = 0;
}

void TRRIP::fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed) {
    auto packet_reuse_probability = packet->reuse_probability;
    //fmt::print("set {} way {} address {:#x} l1_hits {} footprint {:#x}\n",
    //        set_idx, way_idx, packet->address, packet->l1_hits, packet->footprint);
    if (packet->serviced_from_llc > 0) {
        if (packet->is_low_reuse) {
            counter[way_idx] = maxRRPV-1;
        } else if (was_accessed) { 
            counter[way_idx] = 0;
        } else if (!packet->is_hub_node) {
            counter[way_idx] = maxRRPV;
        } else {
            counter[way_idx] = (int)((1.0d - packet_reuse_probability)*maxRRPV);
        }
    } else {
        if (!packet->is_hub_node) {
            counter[way_idx] = maxRRPV-1;
        } else {
            counter[way_idx] = maxRRPV-2;
        }
    }

}

uint64_t TRRIP::get_eviction_candidate(bool is_low_priority=false) {
    
    // Lambda to encapsulate the comparison logic for reuse
    auto is_better_candidate = [&](uint64_t current_idx, uint64_t best_idx, bool compare_reuse=false) {
        if (counter[current_idx] > maxRRPV) return false;
        if (counter[current_idx] > counter[best_idx]) return true;
        if (compare_reuse && (counter[current_idx] == counter[best_idx])) {
            return reuse_probability[current_idx] < reuse_probability[best_idx];
        }
        return false;
    };


    uint64_t candidate_idx = 0; 
    for (uint64_t idx = 0; idx < num_ways; idx++) {
        if (counter[idx] > maxRRPV) continue;

        // Handle initial candidate validity
        if (counter[candidate_idx] > maxRRPV) {
            candidate_idx = idx;
            continue;
        }

        if (is_better_candidate(idx, candidate_idx, true)) {
            candidate_idx = idx;
        }
    }

    assert(candidate_idx < num_ways);

    // Update diff based on the final candidate found
    if (counter[candidate_idx] < maxRRPV) {
        diff = std::min(diff, maxRRPV - counter[candidate_idx]);
    }

    return candidate_idx;
}

uint64_t TRRIP::get_reserved_eviction_candidate(bool is_low_priority = false) {
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

void TRRIP::evict(uint64_t way_idx) {
    counter[way_idx] = UINT_MAX;
}

uint64_t TRRIP::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t TRRIP::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void TRRIP::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
