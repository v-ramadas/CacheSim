#include "cachesim.h"
#include "msl/bits.h"
#include <cassert>

CacheSet::CacheSet() {
}

CacheSet::CacheSet(uint64_t way, uint64_t blk_size, uint64_t set_id, uint64_t level):
    num_ways(way),
    block_size(blk_size),
    set_idx(set_id),
    level(level)
{
    num_blocks = CACHELINE_SIZE/block_size;
    ways.resize(num_ways, UINT64_MAX);
    lru.resize(num_ways, 0);
    valid.resize(num_ways, false);
    dirty.resize(num_ways, false);
    distance_counts.resize(num_ways, 0);
}

Cache::Cache():
    NAME("DefaultCache"),
    num_sets(1024),
    level(0),
    insertion_policy(EXCLUSIVE)
{
    num_ways = 16;
    for (uint64_t i = 0; i < num_sets; ++i) {
        sets[i] = CacheSet(num_ways, 64, i, level);
    }
    partial_misses.resize(CACHELINE_SIZE/64, 0);
}

Cache::Cache(std::string name, uint64_t set, uint64_t ways, uint64_t blk_size, uint64_t level,
            InsertionPolicy policy): 
    NAME(name),
    num_sets(set),
    level(level),
    insertion_policy(policy)
{
    num_ways = ways*CACHELINE_SIZE/blk_size;
    for (uint64_t i = 0; i < num_sets; ++i) {
        sets[i] = CacheSet(num_ways, blk_size, i, level);
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
    auto way_idx = std::distance(ways.begin(), way);

    auto hit = (way != ways.end());// && (valid[way_idx] == true);
    accesses++;
    if (hit) {
        const uint64_t hit_counter = lru[way_idx];
        if (do_mrc) {
            uint64_t distance = std::count_if(
                lru.begin(), lru.end(),
                [hit_counter](uint64_t n) {
                        return n > hit_counter;}
            );
            distance_counts[distance]++;
            reuse_dist += distance;
        }
        
        if (is_read) {
            //global_counter2 += num_ways;
            lru[way_idx] = global_counter1++;
        } else {
            lru[way_idx] = global_counter1++;
            dirty[way_idx] = true;
        }
        
        hits++;
    }

    if (cachesim::DEBUG) {
        fmt::print("{} level {} address {:#x} hit {} set {} way {}\n", __func__, level, address, (hit) ? "HIT" : "MISS", set_idx, way_idx);
    }
    return hit;
}

uint32_t CacheSet::handle_miss(uint64_t address) {
    auto block_address = align_address(address, CACHELINE_SIZE);
    uint64_t num_blocks_touched = 0;
    std::vector<uint64_t> ways_to_fill;
    std::vector<uint64_t> address_to_fill;
    uint32_t partial_misses = 0;
    while (num_blocks_touched < num_blocks) {
        auto try_hit = std::find(ways.begin(), ways.end(), block_address);

        if (try_hit != ways.end()) {
            num_blocks_touched++;
            block_address += block_size;
            continue;
        }

        partial_misses++;
        num_blocks_touched++;
    }

    return partial_misses;
}

uint32_t CacheSet::handle_fill(std::vector<uint64_t> blocks) {
    //auto fill_address = align_address(address, CACHELINE_SIZE);
    auto way = valid.begin();
    global_counter1++;
    uint32_t partial_misses = 0;
    for (auto block_address: blocks) {
        auto try_hit = std::find(ways.begin(), ways.end(), block_address);
        if (try_hit != ways.end()) {
            // Block already present, don't update LRU
            continue;
        }
        partial_misses++;
        way = std::find(way, valid.end(), false);
        auto way_idx = std::distance(valid.begin(), way);
        valid[way_idx] = true;
        lru[way_idx] = global_counter1;
        ways[way_idx] = block_address;

        if (cachesim::DEBUG)
            fmt::print("Level {} Inserting address {:#x} @ set {} way {} valid {}\n", level, block_address, set_idx, way_idx, (uint32_t)valid[way_idx]);
    }
    return partial_misses;
}

std::vector<uint64_t> CacheSet::handle_evict(uint64_t num_blocks_to_evict) {
    uint64_t num_blocks_evicted = 0;
    bool eviction_needed = ((uint64_t)(std::count(valid.begin(), valid.end(), false)) <= num_blocks_to_evict);
    if (!eviction_needed) {
        if (cachesim::DEBUG)
            fmt::print("Level {} No eviction needed for set {} because there are {} invalid ways\n", level, set_idx, std::count(valid.begin(), valid.end(), false));
        return {};
    }
    
    std::vector<uint64_t> evicted_ways = {};
    while (num_blocks_evicted < num_blocks_to_evict) {
        auto lru_way = std::min_element(lru.begin(), lru.end());
        auto way_idx = std::distance(lru.begin(), lru_way);

        if (cachesim::DEBUG)//&& valid[way_idx])
            fmt::print("Level {} Evicted set {} way {} address {:#x} dirty {} valid {}\n", level, set_idx, way_idx, ways[way_idx], (uint32_t)dirty[way_idx], (uint32_t)valid[way_idx]);
 
        if (dirty[way_idx]) {
            dirty[way_idx] = false;
        }
        if (valid[way_idx]) {
            evicted_ways.push_back(ways[way_idx]);
        }
        valid[way_idx] = false;
        lru[way_idx] = UINT64_MAX;
        ways[way_idx] = UINT64_MAX;
        num_blocks_evicted++;       
    }
    return evicted_ways;
}

void CacheSet::handle_invalidate(uint64_t address) {
    auto try_hit = std::find(ways.begin(), ways.end(), address);
    if (try_hit != ways.end()) {
        auto way_idx = std::distance(ways.begin(), try_hit);
        valid[way_idx] = false;
        dirty[way_idx] = false;
        lru[way_idx] = UINT64_MAX;
        ways[way_idx] = UINT64_MAX;
        if (cachesim::DEBUG) {
            fmt::print("Level {} Invalidated address {:#x} @ set {} way {} because of line promotion to higher level\n", level, address, set_idx, way_idx);
        }
    }

}

bool Cache::can_insert_at_level(int level) {
    if (insertion_policy == EXCLUSIVE) {
        return level == 0;
    } else {
        return true;
    }
}

bool Cache::try_hit(uint64_t address, bool is_read) {
    auto set_idx = get_set_idx(address);
    auto aligned_address = align_address(address, sets[set_idx].get_block_size());
    auto hit = sets[set_idx].try_hit(aligned_address, is_read);
    if (hit) {
        hits++;
        if (is_read) read_hits++;
        else write_hits++;
    } else {
        misses++;
        if (is_read) read_misses++;
        else write_misses++;
    }
    return hit;
}
void Cache::handle_fill(std::vector<uint64_t> blocks, uint64_t num_blocks_to_fill,int level = 0) {
    if (can_insert_at_level(level) == false) {
        return;
    }

    if (blocks.size() == 0) {
        if (cachesim::DEBUG)
            fmt::print("Level {} No blocks to fill since block list is empty\n", level);
        return;
    }
    auto set_idx = get_set_idx(blocks[0]);
    auto block_size = sets[set_idx].get_block_size();
    if (blocks.size() != num_blocks_to_fill) {
        fmt::print("{} Not enoughblocks provided. Expected {} blocks to fill for address {:#x} at set {}\n", __func__, blocks.size(), num_blocks_to_fill, blocks[0], set_idx);
        blocks.resize(num_blocks_to_fill);
        auto aligned_address = blocks[0];
        // Populate the vector
        for (uint64_t i = 1; i < num_blocks_to_fill; i++) {
            blocks[i] = aligned_address + i*block_size;
        }
    }
    auto pmisses = sets[set_idx].handle_fill(blocks);
    partial_misses[pmisses - 1];
 
    return;
}

void Cache::handle_fill(uint64_t address, int level = 0) {
    if (can_insert_at_level(level) == false) {
        return;
    }
    auto set_idx = get_set_idx(address);
    auto aligned_address = align_address(address, sets[set_idx].get_block_size());
    std::vector<uint64_t> blocks;
    auto block_size = sets[set_idx].get_block_size();
    auto num_blocks = CACHELINE_SIZE/sets[set_idx].get_block_size();
    blocks.resize(num_blocks);
    // Populate the vector
    for (uint64_t i = 0; i < num_blocks; i++) {
        blocks[i] = aligned_address + i*block_size;
    }
    auto pmisses = sets[set_idx].handle_fill(blocks);
    partial_misses[pmisses-1]++;

    if (cachesim::DEBUG)
        fmt::print("Level {} Inserted address {:#x} @ set {}\n", level, address, set_idx);
    return;
}

std::vector<uint64_t> Cache::handle_evict(uint64_t address, uint64_t size, int level = 0) {
    if (can_insert_at_level(level) == false) {
        return {};
    }  
    auto set_idx = get_set_idx(address);
    auto num_blocks_to_evict = size/sets[set_idx].get_block_size();
    return sets[set_idx].handle_evict(num_blocks_to_evict);
}

void Cache::handle_invalidate(uint64_t address) {
    auto set_idx = get_set_idx(address);
    sets[set_idx].handle_invalidate(address);
    return;
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
