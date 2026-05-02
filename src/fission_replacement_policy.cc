#include "fission_replacement_policy.h"
#include <fmt/core.h>

void Fission::init_counter(PacketPtr packet) {
    if (diff < maxRRPV)
        std::transform(counter.cbegin(), std::next(counter.cend()), counter.begin(), [_diff = diff, _maxRRPV = maxRRPV](auto x) { 
                if (x >= _maxRRPV) return x;
                uint64_t val = x + _diff;
                if (val <= _maxRRPV) return val;
                else return _maxRRPV;
        });

    diff = UINT64_MAX;
    global_clock++;

}

void Fission::hit_update(uint64_t way_idx) {
//    counter[way_idx] = 0;
}

void Fission::fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed) {
    if (packet->serviced_from_llc == true) {
        counter[way_idx] = 0;
    } else {
        counter[way_idx] = maxRRPV-1;
    }

    if (packet->is_sparse) {
        low_priority[way_idx] = true;
    } else {
        low_priority[way_idx] = false;
    }

    //reuse_probability[way_idx] = packet->reuse_probability;
}

uint64_t Fission::get_eviction_candidate(bool is_low_priority=false) {
    
    // Lambda to encapsulate the comparison logic for reuse
    auto is_better_candidate = [&](uint64_t current_idx, uint64_t best_idx, bool compare_reuse=false) {
        if (counter[current_idx] > maxRRPV) return false;
        if (counter[current_idx] > counter[best_idx]) return true;
        if (compare_reuse && (counter[current_idx] == counter[best_idx])) {
            return reuse_probability[current_idx] < reuse_probability[best_idx];
        }
        return false;
    };


    // Step 1: If requested, try searching ONLY low_priority entries
    uint64_t candidate_idx = num_ways; // Initialize with invalid index
    for (uint64_t idx = 0; idx < num_ways; idx++) {
        if (low_priority[idx] && counter[idx] <= maxRRPV) {
            if (is_better_candidate(idx, candidate_idx, false)) {
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

            if (is_better_candidate(idx, candidate_idx, true)) {
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

uint64_t Fission::get_reserved_eviction_candidate(bool is_low_priority = false) {
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

void Fission::evict(uint64_t way_idx) {
    counter[way_idx] = UINT_MAX;
}

uint64_t Fission::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t Fission::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void Fission::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

bool Fission::can_insert(PacketPtr packet) {
    return true;
    if (!packet->is_sparse) return true;

    // Lambda to encapsulate the comparison logic for reuse
    auto is_better_candidate = [&](uint64_t current_idx, uint64_t best_idx, bool compare_reuse=false) {
        if (counter[current_idx] > maxRRPV) return false;
        if (counter[current_idx] > counter[best_idx]) return true;
        if (compare_reuse && (counter[current_idx] == counter[best_idx])) {
            return reuse_probability[current_idx] < reuse_probability[best_idx];
        }
        return false;
    };
    
    // Step 1: If requested, try searching ONLY low_priority entries
    uint64_t candidate_idx = num_ways; // Initialize with invalid index
    for (uint64_t idx = 0; idx < num_ways; idx++) {
        if (low_priority[idx] && counter[idx] <= maxRRPV) {
            if (is_better_candidate(idx, candidate_idx, false)) {
                candidate_idx = idx;
                break;
            }
        }
    }

    if (candidate_idx < num_ways) {
        return true;
    } else {
        return false;
    }

}
