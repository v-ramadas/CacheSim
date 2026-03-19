#include "srrip_replacement_policy.h"
#include <fmt/core.h>


void SRRIP::init_counter(bool is_sparse) {
}

void SRRIP::hit_update(uint64_t way_idx) {
    counter[way_idx] = 0;
}

void SRRIP::fill_update(uint64_t way_idx, bool is_sparse) {
    if (is_sparse) {
        counter[way_idx] = sparseRRPV - 3;
    } else {
        counter[way_idx] = denseRRPV - 1;
    }
}

uint64_t SRRIP::get_eviction_candidate() {
    auto way = std::max_element(std::begin(counter), std::end(counter));
    uint64_t way_idx = std::distance(std::begin(counter), way);
    std::transform(std::cbegin(counter), std::cend(counter), std::begin(counter), [diff = maxRRPV - *way](auto x) { return x + diff; });

    return way_idx;
}

void SRRIP::evict(uint64_t way_idx) {
    counter[way_idx] = maxRRPV-1;
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
    fmt::print("SRRIP\n");
    std::fflush(stdout);
}
