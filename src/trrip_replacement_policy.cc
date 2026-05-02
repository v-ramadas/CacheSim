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
//    counter[way_idx] = 0;
}

void TRRIP::fill_update(uint64_t way_idx, PacketPtr packet, bool was_accessed) {
    if (packet->serviced_from_llc == true) {
        counter[way_idx] = 0;
    } else {
        counter[way_idx] = denseRRPV - 1;
    }
    insertion_clock[way_idx] = global_clock;
}

uint64_t TRRIP::get_eviction_candidate(bool is_low_priority = false) {
    auto candidate_idx = 0;
    auto candidate = counter[candidate_idx];
    for (uint64_t idx = 0; idx < num_ways; idx++) {
        candidate = counter[candidate_idx];
        auto way = counter[idx];

        if (way > maxRRPV) continue;
        if (candidate > maxRRPV) {
            candidate_idx = idx;
            continue;
        }

        if (way > candidate) candidate_idx = idx;
        else if (way == candidate) {
            if (insertion_clock[idx] < insertion_clock[candidate_idx]) {
                candidate_idx = idx;
            }
        }
    }

    diff = std::min(diff, maxRRPV - candidate);
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
        else if (way == candidate) {
            if (insertion_clock[idx] < insertion_clock[candidate_idx]) {
                candidate_idx = idx;
            }
        }
    }

    diff = std::min(diff, maxRRPV - candidate);
    return candidate_idx;
}

void TRRIP::evict(uint64_t way_idx) {
    counter[way_idx] = UINT_MAX;
    insertion_clock[way_idx] = UINT_MAX;
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
