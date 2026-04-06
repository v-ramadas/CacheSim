#include "srrip_replacement_policy.h"
#include <fmt/core.h>

void SRRIP::init_counter(PacketPtr packet) {
    if (diff < maxRRPV)
        std::transform(std::cbegin(counter), std::cend(counter), std::begin(counter), [_diff = diff, _maxRRPV = maxRRPV](auto x) { 
                uint64_t val = x + _diff;
                if (val <= _maxRRPV) return val;
                else return _maxRRPV;
        });

    diff = UINT64_MAX;
    global_clock++;

}

void SRRIP::hit_update(uint64_t way_idx) {
    counter[way_idx] = 0;
}

void SRRIP::fill_update(uint64_t way_idx, PacketPtr packet) {
    if (packet->pc != 10 && level != 0) {
        counter[way_idx] = denseRRPV - 1;
    } else {
        counter[way_idx] = sparseRRPV - 1;
    }
    insertion_clock[way_idx] = global_clock;
}

uint64_t SRRIP::get_eviction_candidate() {
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

void SRRIP::evict(uint64_t way_idx) {
    counter[way_idx] = maxRRPV;
    insertion_clock[way_idx] = UINT_MAX;
}

uint64_t SRRIP::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t SRRIP::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void SRRIP::print() {
}
