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
#include "phru_replacement_policy.h"
#include "phrupp_replacement_policy.h"
#include "srrip_replacement_policy.h"
#include "drrip_replacement_policy.h"
#include "prrip_replacement_policy.h"
#include "ship_replacement_policy.h"
#include "belady_replacement_policy.h"
#include "hrrip_replacement_policy.h"
#include "hawkeye_replacement_policy.h"
#include "hub_hawkeye_replacement_policy.h"
#include "grasp_replacement_policy.h"
#include "hub_oracle_replacement_policy.h"

enum class ReplacementPolicy {
    LRU,
    HRU,
    HRUpp,
    PHRU,
    PHRUpp,
    SRRIP,
    DRRIP,
    PRRIP,
    SHIP,
    Belady,
    HRRIP,
    Hawkeye,
    HubHawkeye,
    GRASP,
    HubOracle,
};

inline BasePolicy* create_policy(ReplacementPolicy policy, uint64_t set_idx, uint64_t num_ways, CacheLevel level) {
    switch (policy) {
        case ReplacementPolicy::LRU:
            return new LRU(set_idx, num_ways, level);
        case ReplacementPolicy::HRU:
            return new HRU(set_idx, num_ways, level);
        case ReplacementPolicy::HRUpp:
            return new HRUpp(set_idx, num_ways, level);
        case ReplacementPolicy::PHRU:
            return new PHRU(set_idx, num_ways, level);
        case ReplacementPolicy::PHRUpp:
            return new PHRUpp(set_idx, num_ways, level);
        case ReplacementPolicy::SRRIP:
            return new SRRIP(set_idx, num_ways, level);
        case ReplacementPolicy::DRRIP:
            return new DRRIP(set_idx, num_ways, level);
        case ReplacementPolicy::PRRIP:
            return new PRRIP(set_idx, num_ways, level);
        case ReplacementPolicy::SHIP:
            return new SHIP(set_idx, num_ways, level);
        case ReplacementPolicy::Belady:
            return new Belady(set_idx, num_ways, level);
        case ReplacementPolicy::HRRIP:
            return new HRRIP(set_idx, num_ways, level);
        case ReplacementPolicy::Hawkeye:
            return new Hawkeye(set_idx, num_ways, level);
        case ReplacementPolicy::HubHawkeye:
            return new HubHawkeye(set_idx, num_ways, level);
        case ReplacementPolicy::GRASP:
            return new GRASP(set_idx, num_ways, level);
        case ReplacementPolicy::HubOracle:
            return new HubOracle(set_idx, num_ways, level);

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
    if (num_blocks == 0) num_blocks = 1;
    if (packet->blocks.size() > 0) {
        if (packet->blocks.size() != num_blocks) {
            packet->blocks.resize(num_blocks);
            auto aligned_address = 
                align_address(packet->address, packet->size);
            for (uint64_t idx = 0; idx < num_blocks; ++idx) {
                packet->blocks[idx] = aligned_address + idx*block_size;
            }
        } else if (packet->blocks.size() > num_blocks) {
            packet->blocks.clear();
            packet->blocks.resize(num_blocks);
            auto aligned_address = 
                align_address(packet->address, packet->size);
            for (uint64_t idx = 0; idx < num_blocks; ++idx) {
                packet->blocks[idx] = aligned_address + idx*block_size;
            }
        }
    }
}


inline float bytes_to_mb(uint64_t size) {
    return (float)(size)/(1024.0*1024.0);
}

inline float get_total_cache_size(uint64_t num_sets, uint64_t num_ways, uint64_t block_size, bool print=false) {
    // 1. Calculate Data Storage Size (in bytes)
    float data_size = num_ways * num_sets * block_size;

    // 2. Calculate Tag Size in bits per block
    // Tag = 64 - log2(block_size) - log2(num_sets)
    uint64_t tag_bits_per_block = 64 - std::log2(block_size) - std::log2(num_sets);

    // 3. Calculate Total Tag Storage Size
    // Total blocks = num_sets * num_ways
    uint64_t total_blocks = num_sets * num_ways;
    uint64_t total_tag_bits = total_blocks * tag_bits_per_block;

    // Sectored with 8B sectors: one valid bit per 8B unit (8 per 64B line, 1 per 8B block).
    total_tag_bits += total_blocks * (block_size / 8);

    // Convert tag storage from bits to bytes (rounding up if necessary, though it usually divides evenly)
    float tag_size = (total_tag_bits + 7) / 8;

    //fmt::print("Cache Size Calculator. Num Sets {} Num Ways {} Block Size {} Data Cache Size {:4f}MB Tag Storage {:4f}MB Total Size {:4f}MB\n",
    //        num_sets, num_ways, block_size, bytes_to_mb(data_size), bytes_to_mb(tag_size), bytes_to_mb(data_size + tag_size));
    if (print)
        fmt::print("Data Size {} Tag Size {}\n", bytes_to_mb(data_size), bytes_to_mb(tag_size));
    return data_size + tag_size;
}

inline uint64_t get_iso_area_cache(uint64_t num_sets, uint64_t reference_ways, uint64_t block_size) {
    // 1. Calculate the budget baseline using the 64-byte block cache
    float target_budget_bytes = get_total_cache_size(num_sets, reference_ways, 64, false);
    
    //fmt::print("Target Budget ({} sets, {} ways): {:4f} MB\n",
    //        num_sets, reference_ways, bytes_to_mb(target_budget_bytes));

    // 2. Incrementally search for the highest number of ways for the 8-byte block cache
    if (block_size != CACHELINE_SIZE) {
        if (!cachesim::isoArea) {
            get_total_cache_size(num_sets, reference_ways * (CACHELINE_SIZE/block_size), block_size, false);
            return (reference_ways * (CACHELINE_SIZE/block_size));
        }
        if (cachesim::TAG_BDI_BASES > 0) {
            // Base+Delta tag compression (capacity only, --tag-bdi-bases B
            // --tag-bdi-delta d): per set, B base line tags of
            // 64 - log2(64) - log2(num_sets) bits; per block, valid (1) + base
            // id (ceil log2 B) + signed delta (d) + sector (log2(64/block_size))
            // tag bits instead of a full 64 - log2(block_size) - log2(num_sets).
            uint64_t sets_bits = (uint64_t)std::log2(num_sets);
            uint64_t id_bits = 0;
            while ((1ULL << id_bits) < cachesim::TAG_BDI_BASES) id_bits++;
            uint64_t sector_bits = (uint64_t)std::log2(CACHELINE_SIZE / block_size);
            uint64_t base_bits = cachesim::TAG_BDI_BASES * (64 - (uint64_t)std::log2(CACHELINE_SIZE) - sets_bits);
            uint64_t block_bits = block_size * 8 + 1 + id_bits + cachesim::TAG_BDI_DELTA + sector_bits;
            // target_budget_bytes already includes the baseline's 8 sector-valid
            // bits per 64B way (get_total_cache_size); block_bits includes this
            // scheme's 1 valid bit per 8B block.
            uint64_t budget_bits_per_set = (uint64_t)(target_budget_bytes * 8) / num_sets;
            assert(budget_bits_per_set > base_bits);
            uint64_t ways = (budget_bits_per_set - base_bits) / block_bits;
            // Round down to a multiple of 8 (a whole number of 64B lines), so the
            // compressed set never exceeds the budget.
            return (ways / 8) * 8;
        }
        uint64_t current_ways = 1;
        float new_size;
        while (true) {
            new_size = get_total_cache_size(num_sets, current_ways, block_size, false);
            
            // If the next step exceeds the budget, the previous step was our max
            if (new_size > target_budget_bytes) {
                current_ways--;
                break;
            }
            
            current_ways++;
        }

        current_ways = ((current_ways + 8) / 8) * 8;
        new_size = get_total_cache_size(num_sets, current_ways, block_size, false);
        //fmt::print("New Budget ({} sets, {} ways): {:4f} MB\n",
        //        num_sets, current_ways, bytes_to_mb(new_size));

        return current_ways;
    } else {
        return reference_ways;
    }
}

// Inverse of get_iso_area_cache: current config is the smaller-block cache
// (e.g. 8B), we want the largest new_block_size (e.g. 64B) way count that
// fits the same total (data+tag) byte budget. Because tag bits/block shrink
// as block_size grows, this can come out slightly larger than the naive
// current_ways / (new_block_size/current_block_size) division would give -
// hence the same incremental search get_iso_area_cache uses, not a divide.
inline uint64_t get_iso_area_cache_merge(uint64_t num_sets, uint64_t current_ways,
                                          uint64_t current_block_size, uint64_t new_block_size) {
    float target_budget_bytes = get_total_cache_size(num_sets, current_ways, current_block_size, false);

    if (!cachesim::isoArea) {
        uint64_t scale_factor = new_block_size / current_block_size;
        return current_ways / scale_factor;
    }

    uint64_t ways = 1;
    float new_size;
    while (true) {
        new_size = get_total_cache_size(num_sets, ways, new_block_size, false);
        if (new_size > target_budget_bytes) {
            ways--;
            break;
        }
        ways++;
    }
    ways = ((ways + 8) / 8) * 8;
    new_size = get_total_cache_size(num_sets, ways, new_block_size, false);
    return ways;
}

#endif
