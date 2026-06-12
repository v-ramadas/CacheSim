#include "drrip_replacement_policy.h"
#include <fmt/core.h>

void DRRIP::init_counter(PacketPtr packet) {
    if (diff < maxRRPV)
        std::transform(counter.cbegin(), std::next(counter.cend()), counter.begin(), [_diff = diff, _maxRRPV = maxRRPV](auto x) { 
                uint64_t val = x + _diff;
                if (val <= _maxRRPV) return val;
                else return _maxRRPV;
        });

    diff = UINT64_MAX;
    global_clock++;
}

void DRRIP::hit_update(PacketPtr packet, uint64_t way_idx) {
//    counter[way_idx] = 0;
}

void DRRIP::update_bip(uint64_t way_idx, PacketPtr packet) {
    if (packet->serviced_from_llc == true) {
        counter[way_idx] = 0;
    } else {
        counter[way_idx] = maxRRPV - 1;
        bip_counter++;
        if (bip_counter == BIP_MAX) {
            bip_counter = 0;
            counter[way_idx] = maxRRPV - 2;
        }
    }
}

void DRRIP::update_srrip(uint64_t way_idx, PacketPtr packet) {
    if (packet->serviced_from_llc == true) {
        counter[way_idx] = 0;
    } else {
        counter[way_idx] = maxRRPV - 1;
    }
}

void DRRIP::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    auto begin = 0;
    auto end = NUM_POLICY*SDM_SIZE;
    if (set_idx > end) { // follower sets
        if (PSEL > (PSEL_MAX/2)) { // follow BIP
            update_bip(way_idx, packet);
        } else { // follow SRRIP
            update_srrip(way_idx, packet);
        }
    } else if (set_idx < SDM_SIZE) { // leader 0: BIP
        dec_psel();
        update_bip(way_idx, packet);
    } else {
        inc_psel();
        update_srrip(way_idx, packet);
    }
}


uint64_t DRRIP::get_eviction_candidate(bool is_low_priority = false) {
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
    }

    diff = std::min(diff, maxRRPV - candidate);
    return candidate_idx;
}

uint64_t DRRIP::get_reserved_eviction_candidate(bool is_low_priority = false) {
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

void DRRIP::evict(uint64_t way_idx) {
    counter[way_idx] = UINT_MAX;
}

uint64_t DRRIP::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t DRRIP::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void DRRIP::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}
