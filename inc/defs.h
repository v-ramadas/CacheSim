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
#include <string>
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
#include "popt_replacement_policy.h"
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
    SHIPPC,
    SHIPMem,
    Belady,
    HRRIP,
    Hawkeye,
    HubHawkeye,
    GRASP,
    POPT,
    HubOracle,
    Amoeba,
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
        case ReplacementPolicy::SHIPMem:
            return new SHIPMem(set_idx, num_ways, level);
        case ReplacementPolicy::SHIPPC:
            return new SHIPPC(set_idx, num_ways, level);
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
        case ReplacementPolicy::POPT:
            return new POPT(set_idx, num_ways, level);
        case ReplacementPolicy::HubOracle:
            return new HubOracle(set_idx, num_ways, level);
        // Amoeba-Cache replaces whole blocks in LRU order itself (Set::amoeba_*);
        // the per-way LRU state only tracks insertion and hits.
        case ReplacementPolicy::Amoeba:
            return new LRU(set_idx, num_ways, level);

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
    // A sector-granular victim (one entry per 8B sector) going into 16B/32B
    // blocks: each block is present if any of its sectors is, and takes the
    // largest degree, LLC service count and previous LLC counter of its
    // sectors (UINT64_MAX = none is ignored), so the per-block
    // arrays line up with blocks. (64B keeps the per-sector arrays as before;
    // hub_oracle reads them.)
    if (block_size < CACHELINE_SIZE && packet->blocks.size() == CACHELINE_SIZE/8 && num_blocks < packet->blocks.size()) {
        auto per = packet->blocks.size() / num_blocks;
        auto aligned_address = align_address(packet->address, packet->size);
        std::vector<uint64_t> blocks(num_blocks, UINT64_MAX), degrees(num_blocks, 0), serviced(num_blocks, 0);
        std::vector<uint64_t> prev(num_blocks, UINT64_MAX);
        for (uint64_t idx = 0; idx < num_blocks; ++idx) {
            for (uint64_t s = idx*per; s < (idx+1)*per; ++s) {
                if (packet->blocks[s] != UINT64_MAX) blocks[idx] = aligned_address + idx*block_size;
                if (s < packet->block_degrees.size()) degrees[idx] = std::max(degrees[idx], packet->block_degrees[s]);
                if (s < packet->block_serviced_from_llc.size()) serviced[idx] = std::max(serviced[idx], packet->block_serviced_from_llc[s]);
                if (s < packet->block_prev_counter.size() && packet->block_prev_counter[s] != UINT64_MAX)
                    prev[idx] = (prev[idx] == UINT64_MAX) ? packet->block_prev_counter[s] : std::max(prev[idx], packet->block_prev_counter[s]);
            }
        }
        packet->blocks = blocks;
        packet->block_degrees = degrees;
        packet->block_serviced_from_llc = serviced;
        packet->block_prev_counter = prev;
        return;
    }
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
        if (cachesim::AMOEBA && cachesim::AMOEBA_WORDS > 0)
            return cachesim::AMOEBA_WORDS;
        if (!cachesim::isoArea) {
            get_total_cache_size(num_sets, reference_ways * (CACHELINE_SIZE/block_size), block_size, false);
            return (reference_ways * (CACHELINE_SIZE/block_size));
        }
        if (cachesim::AMOEBA) {
            // Amoeba-Cache: the set is a uniform array of 64-bit words (tags
            // and data alike), each with a T? (is-tag) and a V? (in-use) bit,
            // so a set gets budget / (64 + 2) words; "ways" are those words.
            uint64_t budget_bits_per_set = (uint64_t)(target_budget_bytes * 8) / num_sets;
            return budget_bits_per_set / (64 + 2);
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

// GAP .sg/.wsg headers of the graphs in gapbs_traces/inputs (every ordering
// and the weighted copies share one header), so policies that need a graph's
// size (P-OPT) can look it up by name instead of reading the file.
struct GraphInfo {
    const char* name;
    bool directed;
    uint64_t num_arcs;
    uint64_t num_nodes;
};

inline constexpr GraphInfo GRAPH_INFO[] = {
    {"as-Skitter",       false,  22190596, 1696415},
    {"cit-Patents",      true,   16518947, 3774768},
    {"com-LiveJournal",  false,  69362378, 3997962},
    {"com-Orkut",        false, 234370166, 3072627},
    {"com-Youtube",      false,   5975248, 1134890},
    {"flickr-growth",    true,   33140017, 2302926},
    {"higgs-twitter",    true,   14855819,  456627},
    {"hollywood-2009",   false, 112751422, 1139905},
    {"in-2004",          true,   16539643, 1382908},
    {"indochina-2004",   true,  191606827, 7414865},
    {"kron21",           false,  63537182, 2097150},
    {"ogbn-products",    false, 123718024, 2449029},
    {"soc-LiveJournal1", true,   68475391, 4847571},
    {"soc-Pokec",        true,   30622564, 1632804},
    {"sx-stackoverflow", true,   34875684, 2601977},
    {"web-BerkStan",     true,    7600595,  685230},
    {"web-Google",       true,    5105039,  916428},
    {"wiki-Talk",        true,    5021410, 2394385},
    {"wiki-topcats",     true,   28508141, 1791489},
};

// The graph a trace or graph file is named after: its basename up to the
// first '.' (e.g. .../kron21.hubsort.xz -> kron21). nullptr if not listed.
inline const GraphInfo* find_graph_info(const std::string& path) {
    auto base = path.substr(path.find_last_of('/') + 1);
    base = base.substr(0, base.find('.'));
    for (const auto& g : GRAPH_INFO)
        if (base == g.name) return &g;
    return nullptr;
}

#endif
