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

void DRRIP::init_counter(bool is_sparse) {
}

void DRRIP::hit_update(uint64_t way_idx) {
    counter[way_idx] = 0;
}

void DRRIP::fill_update(uint64_t way_idx, bool is_sparse) {
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

void DRRIP::print() {
    fmt::print("DRRIP\n");
    std::fflush(stdout);
}
