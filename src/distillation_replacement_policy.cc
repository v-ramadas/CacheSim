#include "distillation_replacement_policy.h"
#include <fmt/core.h>

void Distillation::init_counter(PacketPtr /*packet*/) {
    if (diff < maxRRPV)
        std::transform(counter.cbegin(), std::next(counter.cend()), counter.begin(), [_diff = diff, _maxRRPV = maxRRPV](auto x) { 
                uint64_t val = x + _diff;
                if (val <= _maxRRPV) return val;
                else return _maxRRPV;
        });

    diff = UINT64_MAX;
    global_clock++;

}

void Distillation::hit_update(PacketPtr /*packet*/, uint64_t way_idx) {
    counter[way_idx] = 0;
}

void Distillation::fill_update(uint64_t way_idx, uint64_t /*block_idx*/, PacketPtr packet, bool was_accessed) {
    //auto packet_reuse_probability = packet->reuse_probability;
    double packet_reuse_probability = (double)(__builtin_popcountll(packet->footprint))/8.0d;
    if (packet->serviced_from_llc > 0) {
        if (packet->is_high_reuse) {
            counter[way_idx] = maxRRPV-1;
        } else if (was_accessed) { 
            counter[way_idx] = 0;
        } else {
            counter[way_idx] = (int)((1.0d - packet_reuse_probability)*(maxRRPV-1));
        }
    } else {
        if ((!packet->is_high_reuse && packet->is_hub_node))
            counter[way_idx] = 0;//maxRRPV - 2;
        else
            counter[way_idx] = maxRRPV-1;
    }

    if (packet->is_hub_node) {
        //if (packet->blocks.size() < 8)
        //fmt::print("Hub Node. PC {:#x} address {:#x} way {} size {} density {} footprint {:#x} l1_hits {}\n", packet->pc, packet->address, way_idx, packet->blocks.size(), __builtin_popcountll(packet->footprint), packet->footprint, packet->l1_hits); 
    }

    if (!packet->is_high_reuse && packet->is_hub_node) {
        low_priority[way_idx] = false;
    } else {
        low_priority[way_idx] = true;
    }
}

uint64_t Distillation::get_eviction_candidate(bool /*is_low_priority=false*/) {
    
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

uint64_t Distillation::get_reserved_eviction_candidate(bool /*is_low_priority = false*/) {
    assert(reserved_ways != 0);
    auto candidate_idx = num_ways;
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

void Distillation::evict(uint64_t way_idx) {
    counter[way_idx] = UINT_MAX;
}

uint64_t Distillation::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t Distillation::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void Distillation::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

bool Distillation::can_insert(PacketPtr packet, uint64_t idx) {
//    return true;
    auto was_accessed = (((packet->footprint >> idx)&0x1) == 0x1);
    if (packet->is_high_reuse) { fmt::print("Skip\n");return false; }
    if (packet->is_hub_node && !was_accessed) return false;
    else return true;
}

