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

inline float bytes_to_mb(uint64_t size) {
    return (float)(size)/(1024.0*1024.0);
}

inline float get_total_cache_size(uint64_t num_sets, uint64_t num_ways, uint64_t block_size) {
    // 1. Calculate Data Storage Size (in bytes)
    float data_size = num_ways * num_sets * block_size;

    // 2. Calculate Tag Size in bits per block
    // Tag = 64 - log2(block_size) - log2(num_sets)
    uint64_t tag_bits_per_block = 64 - std::log2(block_size) - std::log2(num_sets);

    // 3. Calculate Total Tag Storage Size
    // Total blocks = num_sets * num_ways
    uint64_t total_blocks = num_sets * num_ways;
    uint64_t total_tag_bits = total_blocks * tag_bits_per_block;

    // Convert tag storage from bits to bytes (rounding up if necessary, though it usually divides evenly)
    float tag_size = (total_tag_bits + 7) / 8; 

    //fmt::print("Cache Size Calculator. Num Sets {} Num Ways {} Block Size {} Data Cache Size {:4f}MB Tag Storage {:4f}MB Total Size {:4f}MB\n",
    //        num_sets, num_ways, block_size, bytes_to_mb(data_size), bytes_to_mb(tag_size), bytes_to_mb(data_size + tag_size));
    return data_size + tag_size;
}

inline uint64_t get_iso_area_cache(uint64_t num_sets, uint64_t reference_ways, uint64_t block_size) {
    // 1. Calculate the budget baseline using the 64-byte block cache
    float target_budget_bytes = get_total_cache_size(num_sets, reference_ways, 64);
    
    fmt::print("Target Budget ({} sets, {} ways): {:4f} MB\n",
            num_sets, reference_ways, bytes_to_mb(target_budget_bytes));

    // 2. Incrementally search for the highest number of ways for the 8-byte block cache
    if (block_size != CACHELINE_SIZE) {
        if (!cachesim::isoArea) {
            return (reference_ways * (CACHELINE_SIZE/block_size));
        }
        uint64_t current_ways = 1;
        float new_size;
        while (true) {
            new_size = get_total_cache_size(num_sets, current_ways, block_size);
            
            // If the next step exceeds the budget, the previous step was our max
            if (new_size > target_budget_bytes) {
                current_ways--;
                break;
            }
            
            current_ways++;
        }

        current_ways = ((current_ways + 8) / 8) * 8;
        new_size = get_total_cache_size(num_sets, current_ways, block_size);
        fmt::print("New Budget ({} sets, {} ways): {:4f} MB\n",
                num_sets, current_ways, bytes_to_mb(new_size));

        return current_ways;
    } else {
        return reference_ways;
    }
}

#endif
