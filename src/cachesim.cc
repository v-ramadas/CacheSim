#include "cachesim.h"
#include "msl/bits.h"
#include <cassert>

CacheSet::CacheSet() {
}

CacheSet::CacheSet(uint64_t way, uint64_t blk_size, uint64_t set_id):
    num_ways(way),
    block_size(blk_size),
    set_idx(set_id)
{
    num_blocks = CACHELINE_SIZE/block_size;
    ways.resize(num_ways, 0);
    lru.resize(num_ways, 0);
    valid.resize(num_ways, false);
    distance_counts.resize(num_ways, 0);
}

Cache::Cache() {
}

Cache::Cache(std::string name, uint64_t set, uint64_t ways, uint64_t blk_size): 
    NAME(name),
    num_sets(set)
{
    num_ways = ways*CACHELINE_SIZE/blk_size;
    for (uint64_t i = 0; i < num_sets; ++i) {
        sets[i] = CacheSet(num_ways, blk_size, i);
    }
    partial_misses.resize(CACHELINE_SIZE/blk_size, 0);
}

uint64_t align_address(uint64_t address, uint64_t align_size) {
    auto aligned_address = (address/align_size)*align_size;
    return aligned_address;
}

uint64_t Cache::get_set_idx(uint64_t address) {
    uint64_t offset_bits = champsim::msl::lg2(CACHELINE_SIZE);
    uint64_t set_bits = champsim::msl::lg2(num_sets);
    uint64_t set_idx = (address >> offset_bits) & ((0x1 << set_bits) - 1);
    assert(set_idx < num_sets);
    return set_idx;
}

void Cache::print_mpki_curve(uint64_t inst_count) {

      fmt::print ( "\n--- MPKI Curve (MpkiC) ---\n");
      fmt::print ( "Cache Size (d) | Hit Count | Miss Count | MPKI\n");
      fmt::print ( "--------------------------------------------------\n");

      uint64_t cumulative_hits = 0;
      uint64_t total_accesses = get_accesses();

      // Total Misses for a size d = Total Accesses - Cumulative Hits up to size d.
      // Stack distance d means a hit in a cache of size d.
      for (uint64_t way = 0; way < num_ways; ++way) {
        for (uint64_t set = 0; set < num_sets; ++set) {
            cumulative_hits += sets[set].get_distance_count(way);
        }

        uint64_t miss_count = total_accesses - cumulative_hits;
        double mpki = ((double)(miss_count) / inst_count)*1000;

        fmt::print("{} | {} | {} | {:f}\n", num_sets*(way+1)*(sets[0].get_block_size()), cumulative_hits, miss_count, mpki);
      }
}

bool CacheSet::try_hit(uint64_t address, bool is_read) {
    auto way = std::find(ways.begin(), ways.end(), address);
    auto hit = (way != ways.end());
    auto way_idx = std::distance(ways.begin(), way);
    accesses++;
    if (hit) {
        const uint64_t hit_counter = lru[way_idx];
        uint64_t distance = std::count_if(
            lru.begin(), lru.end(),
            [hit_counter](uint64_t n) {
                        return n > hit_counter;}
        );
        distance_counts[distance]++;
        reuse_dist += distance;
        if (is_read) {
            //global_counter2 += num_ways;
            lru[way_idx] = global_counter1++;
            hits++;
        }
    }

    if (DEBUG)
        fmt::print("{} hit {} way {}\n", __func__, (hit) ? "HIT" : "MISS", way_idx);

    return hit;
}

uint32_t CacheSet::handle_fill(uint64_t address) {
    auto fill_address = align_address(address, CACHELINE_SIZE);
    uint64_t num_fills = 0;
    std::vector<uint64_t> ways_to_fill;
    std::vector<uint64_t> address_to_fill;
    auto way = valid.begin();
    uint32_t partial_misses = 0;
    while (num_fills < num_blocks) {
        auto try_hit = std::find(ways.begin(), ways.end(), fill_address);

        if (try_hit != ways.end()) {
            num_fills++;
            fill_address += block_size;
            continue;
        }

        partial_misses++;
        way = std::find(way, valid.end(), false);
        auto needs_eviction = (way == valid.end());
        if (!needs_eviction) {
            auto way_idx = std::distance(valid.begin(), way);
            ways_to_fill.push_back(way_idx);
            address_to_fill.push_back(fill_address);
            valid[way_idx] = true;
        } else {
            auto lru_way = std::min_element(lru.begin(), lru.end());
            auto way_idx = std::distance(lru.begin(), lru_way);
            lru[way_idx] = UINT64_MAX;

            if (DEBUG)
                fmt::print("Evicting set {} way {} address {:#x}\n", set_idx, way_idx, ways[way_idx]);
            ways_to_fill.push_back(way_idx);
            address_to_fill.push_back(fill_address);
            valid[way_idx] = true;
        }
        fill_address += block_size;
        num_fills++;
    }

    global_counter1++;
    for (uint32_t idx = 0; idx < ways_to_fill.size(); ++idx) {
        auto way_idx = ways_to_fill[idx];
        ways[way_idx] = address_to_fill[idx];

        if (address_to_fill[idx] == address) {
            //global_counter2 += num_ways;
            lru[way_idx] = global_counter1;
        } else {
            lru[way_idx] = global_counter1;
        }

        if (DEBUG)
            fmt::print("Inserting address {:#x} @ set {} way {}\n", address_to_fill[idx], set_idx, way_idx);
    }

    return partial_misses;
}

bool Cache::try_hit(uint64_t address, bool is_read) {
    auto set_idx = get_set_idx(address);

    if (DEBUG)
        fmt::print("{} address {:#x} set_idx {} ", __func__, address, set_idx);
    auto aligned_address = align_address(address, sets[set_idx].get_block_size());
    if (sets[set_idx].try_hit(aligned_address, is_read)) {
        hits++;
        return true;
    } else {
        auto pmisses = sets[set_idx].handle_fill(aligned_address);
        misses++;
        partial_misses[pmisses-1]++;
        return false;
    }
}

void Cache::print_reuse_distance() {
    auto set_idx = 0;
    fmt::print("Reuse Distance\nSet Idx | Accumulated Reuse Distance | Hits | Accesses | Avg Reuse Distance Per Hit | Avg Reuse Distance Per Access\n");
    for (auto set: sets) {
        fmt::print("{} | {} | {} | {} | {:.4f} | {:.4f}\n",
                set.first, set.second.reuse_dist, set.second.hits, set.second.accesses, (float)(set.second.reuse_dist)/set.second.hits, (float)(set.second.reuse_dist)/set.second.accesses); 
        set_idx++;
    }
}
