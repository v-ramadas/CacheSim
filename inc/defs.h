#ifndef __DEFS_H__
#define __DEFS_H__

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>
#include <map>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <list>
#include "utils.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include "replacement_policy.h"

#include "lru_replacement_policy.h"
#include "hru_replacement_policy.h"
#include "hrupp_replacement_policy.h"
#include "srrip_replacement_policy.h"
#include "drrip_replacement_policy.h"
#include "prrip_replacement_policy.h"
#include "ship_replacement_policy.h"
#include "fission_replacement_policy.h"
#include "distillation_replacement_policy.h"
#include "belady_replacement_policy.h"
#include "hrrip_replacement_policy.h"


enum class ReplacementPolicy {
    LRU,
    HRU,
    HRUpp,
    SRRIP,
    DRRIP,
    PRRIP,
    SHIP,
    Belady,
    Distillation,
    Fission,
    HRRIP,
};

inline BasePolicy* create_policy(ReplacementPolicy policy, uint64_t set_idx, uint64_t num_ways, uint64_t level) {
    switch (policy) {
        case ReplacementPolicy::LRU:
            return new LRU(set_idx, num_ways, level);
        case ReplacementPolicy::HRU:
            return new HRU(set_idx, num_ways, level);
        case ReplacementPolicy::HRUpp:
            return new HRUpp(set_idx, num_ways, level);
        case ReplacementPolicy::SRRIP:
            return new SRRIP(set_idx, num_ways, level);
        case ReplacementPolicy::DRRIP:
            return new DRRIP(set_idx, num_ways, level);
        case ReplacementPolicy::PRRIP:
            return new PRRIP(set_idx, num_ways, level);
        case ReplacementPolicy::SHIP:
            return new SHIP(set_idx, num_ways, level);
        case ReplacementPolicy::Fission:
            return new Fission(set_idx, num_ways, level);
        case ReplacementPolicy::Distillation:
            return new Distillation(set_idx, num_ways, level);
        case ReplacementPolicy::Belady:
            return new Belady(set_idx, num_ways, level);
        case ReplacementPolicy::HRRIP:
            return new HRRIP(set_idx, num_ways, level);

        default:
            return nullptr;
    }
}

inline uint64_t align_address(uint64_t address, uint64_t align_size) {
    auto aligned_address = (address/align_size)*align_size;
    return aligned_address;
}

inline uint64_t count_footprint(uint64_t footprint) {
    return __builtin_popcountll(footprint);
}

inline void resize_packet(PacketPtr packet, uint64_t block_size) {
    auto num_blocks = packet->size/block_size;
    if (packet->blocks.size() > 0 &&
            packet->blocks.size() != num_blocks) {
        packet->blocks.resize(num_blocks);
        auto aligned_address = 
            align_address(packet->address, packet->size);
        for (uint64_t idx = 0; idx < block_size; ++idx) {
            packet->blocks[idx] = aligned_address + idx*block_size;
        }
    }
}


#endif
