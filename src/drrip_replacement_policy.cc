#include "drrip_replacement_policy.h"
#include <fmt/core.h>

void DRRIP::update_bip(uint64_t way_idx) {
    counter[way_idx] = maxRRPV;
    bip_counter++;
    if (bip_counter == BIP_MAX) {
        bip_counter = 0;
        counter[way_idx] = maxRRPV - 1;
    }
}

void DRRIP::init_counter(PacketPtr packet) {
}

void DRRIP::hit_update(uint64_t way_idx) {
    counter[way_idx] = 0;
}

void DRRIP::fill_update(uint64_t way_idx, PacketPtr packet) {
    auto leader_idx = NUM_POLICY*SDM_SIZE;
    if (set_idx > NUM_POLICY*SDM_SIZE) { // follower sets
        if (PSEL > (PSEL_WIDTH/2)) { // follow BIP
            update_bip(way_idx);
        } else { // follow SRRIP
            counter[way_idx] = maxRRPV - 1;
        }
    } else if (set_idx == 0) { // leader 0: BIP
        dec_psel();
        update_bip(way_idx);
    } else {
        inc_psel();
        counter[way_idx] = maxRRPV - 1;
    }
}


uint64_t DRRIP::get_eviction_candidate() {
    auto way = std::max_element(std::begin(counter), std::end(counter));
    uint64_t way_idx = std::distance(std::begin(counter), way);
    std::transform(std::cbegin(counter), std::cend(counter), std::begin(counter), [diff = maxRRPV - *way](auto x) { return x + diff; });

    return way_idx;
}

uint64_t DRRIP::get_reserved_eviction_candidate() {
//    assert(reserved_ways == 0);
//    auto candidate_idx = 0;
//    auto candidate = counter[candidate_idx];
//    for (uint64_t idx = num_ways; idx < num_ways+reserved_ways; idx++) {
//        candidate = counter[candidate_idx];
//        auto way = counter[idx];
//
//        if (way > maxRRPV) continue;
//        if (candidate > maxRRPV) {
//            candidate_idx = idx;
//            continue;
//        }
//
//        if (way > candidate) candidate_idx = idx;
//        else if (way == candidate) {
//            if (insertion_clock[idx] < insertion_clock[candidate_idx]) {
//                candidate_idx = idx;
//            }
//        }
//    }
//
//    diff = std::min(diff, maxRRPV - candidate);
//    return candidate_idx;
}

void DRRIP::evict(uint64_t way_idx) {
    counter[way_idx] = maxRRPV;
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
