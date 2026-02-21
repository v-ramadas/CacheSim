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
    footprint.resize(num_ways*block_size/8, false);
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

bool CacheSet::get_footprint(uint64_t way_idx, uint64_t word_idx) {
    uint64_t idx = way_idx*(block_size/8) + word_idx;
    return footprint[idx];
}

void CacheSet::set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed) {
    uint64_t idx = way_idx*(block_size/8) + word_idx;
    footprint[idx] = accessed;
    return;
}

void CacheSet::populate_fill_packet(PacketPtr fill_packet) {
    if (fill_packet->blocks.size() != 0) {
        for (auto block: fill_packet->blocks) {
            auto try_hit = std::find(ways.begin(), ways.end(), block);
            if (try_hit != ways.end()) {
                auto way_idx = std::distance(ways.begin(), try_hit);
                assert(valid[way_idx] == true);
                if (cachesim::DEBUG)
                    fmt::print("Block address {:#x} already present in set {} way {}. Removing fill packet\n", block, set_idx, way_idx);
                fill_packet->blocks.erase(std::remove(fill_packet->blocks.begin(), fill_packet->blocks.end(), block), fill_packet->blocks.end());
            }
        }
        return;
    } else {
        for (uint64_t address = fill_packet->aligned_address; address < fill_packet->aligned_address + CACHELINE_SIZE; address += block_size) {
            auto try_hit = std::find(ways.begin(), ways.end(), address);
            if (try_hit != ways.end()) {
                auto way_idx = std::distance(ways.begin(), try_hit);
                assert(valid[way_idx] == true);
                fmt::print("Block address {:#x} already present in set {} way {}. Not adding to fill packet\n", address, set_idx, way_idx);
                continue;
            }
            fill_packet->blocks.push_back(address);
       }
    }
    return;
}

bool CacheSet::try_hit(PacketPtr packet) {
    bool hit = true;

    for (auto block: packet->blocks) {
        auto way = std::find(ways.begin(), ways.end(), block);
        hit &= (way != ways.end());// && (valid[way_idx] == true);
        if (!hit) {
            break;
        }
    }

    accesses++;
    auto way_idx = std::distance(ways.begin(), std::find(ways.begin(), ways.end(), packet->aligned_address));

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
    
        lru[way_idx] = global_counter1++;
        if (!packet->is_read) {
            dirty[way_idx] = true;
        }
        auto word_idx = (packet->aligned_address - align_address(packet->aligned_address, block_size)) >> 3;
        set_footprint(way_idx, word_idx, true);
        hits++;
    }

    if (cachesim::DEBUG) {
        fmt::print("{} level {} address {:#x} hit {} set {} way {} size {}\n", __func__, level, packet->address, (hit) ? "HIT" : "MISS", set_idx, way_idx, packet->blocks.size());
    }

    return hit;
}

void CacheSet::handle_fill(PacketPtr packet) {
    //auto fill_address = align_address(address, CACHELINE_SIZE);
    auto way = valid.begin();
    global_counter1++;
    for (auto block_address: packet->blocks) {
        way = std::find(way, valid.end(), false);
        auto way_idx = std::distance(valid.begin(), way);
        valid[way_idx] = true;
        lru[way_idx] = global_counter1;
        ways[way_idx] = block_address;
        if (block_address == packet->address) {
            lru[way_idx] = global_counter1+1;
            auto word_idx = (block_address - packet->address) >> 3;
            set_footprint(way_idx, word_idx, true);
        }

        if (cachesim::DEBUG)
            fmt::print("Level {} Inserting address {:#x} @ set {} way {} valid {}\n", level, block_address, set_idx, way_idx, (uint32_t)valid[way_idx]);
    }
    global_counter1++;
    return;
}

void CacheSet::handle_evict(PacketPtr packet) {
    uint64_t num_blocks_evicted = 0;
    uint64_t num_blocks_to_evict = packet->size/block_size;
    uint64_t num_invalid_blocks = (uint64_t)(std::count(valid.begin(), valid.end(), false));
    bool eviction_needed = (num_invalid_blocks < num_blocks_to_evict);
    if (!eviction_needed) {
        if (cachesim::DEBUG)
            fmt::print("Level {} No eviction needed for set {} because there are {} invalid ways\n", level, set_idx, std::count(valid.begin(), valid.end(), false));
        return;
    }

    num_blocks_to_evict -= num_invalid_blocks;
    std::vector<uint64_t> evicted_ways = {};
    while (num_blocks_evicted < num_blocks_to_evict) {
        auto lru_way = std::min_element(lru.begin(), lru.end());
        auto way_idx = std::distance(lru.begin(), lru_way);

        if (dirty[way_idx]) {
            dirty[way_idx] = false;
        }
        if (valid[way_idx]) {
            packet->blocks.push_back(ways[way_idx]);
        }

        for (uint64_t i = 0; i < block_size/8; ++i) {
            packet->footprint += get_footprint(way_idx, i);
            set_footprint(way_idx, i, false);
        }

        if (cachesim::DEBUG)//&& valid[way_idx])
            fmt::print("Level {} Evicted set {} way {} address {:#x} dirty {} valid {}\n",
                level, set_idx, way_idx, ways[way_idx], (uint32_t)dirty[way_idx], (uint32_t)valid[way_idx]);

        valid[way_idx] = false;
        lru[way_idx] = UINT64_MAX;
        ways[way_idx] = UINT64_MAX;
        num_blocks_evicted++;
    }

    if (cachesim::DEBUG)
        fmt::print("Level {} Num invalid blocks {} Evicted blocks {} Evicted Line Footprint {}\n",
            level, num_invalid_blocks, num_blocks_evicted, packet->footprint);

    return;
}

void CacheSet::handle_invalidate(PacketPtr packet) {
    auto try_hit = std::find(ways.begin(), ways.end(), packet->address);
    if (try_hit != ways.end()) {
        auto way_idx = std::distance(ways.begin(), try_hit);
        valid[way_idx] = false;
        dirty[way_idx] = false;
        lru[way_idx] = UINT64_MAX;
        ways[way_idx] = UINT64_MAX;
        if (cachesim::DEBUG) {
            fmt::print("Level {} Invalidated address {:#x} @ set {} way {} because of line promotion to higher level\n", level, packet->address, set_idx, way_idx);
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

bool Cache::try_hit(PacketPtr packet) {
    auto set_idx = get_set_idx(packet->address);
    auto block_size = sets[set_idx].get_block_size();
    auto num_blocks = packet->size/block_size;

    assert(num_blocks > 0);

    if (packet->blocks.size() != num_blocks) {
        packet->aligned_address = align_address(packet->address, sets[set_idx].get_block_size());
        packet->blocks.clear();
        packet->blocks.resize(num_blocks);
        packet->blocks[0] = packet->aligned_address;
        // Populate the vector
        for (uint64_t i = 0; i < num_blocks; i++) {
           packet->blocks[i] = align_address(packet->aligned_address, packet->size) + i*block_size;
        }
    }

    auto hit = sets[set_idx].try_hit(packet);
    if (hit) {
        hits++;
        if (packet->is_read) read_hits++;
        else write_hits++;
    } else {
        misses++;
        if (packet->is_read) read_misses++;
        else write_misses++;
    }
    return hit;
}

void Cache::handle_fill(PacketPtr packet, uint64_t num_blocks_to_fill, int level = 0) {
    if (can_insert_at_level(level) == false) {
        return;
    }

    if (packet->blocks.size() == 0) {
        if (cachesim::DEBUG)
            fmt::print("Level {} No blocks to fill since block list is empty\n", level);
        return;
    }
    auto set_idx = get_set_idx(packet->blocks[0]);
    auto block_size = sets[set_idx].get_block_size();
    if (packet->blocks.size() != num_blocks_to_fill) {
        fmt::print("{} Not enoughblocks provided. Expected {} blocks to fill for address {:#x} at set {}\n", __func__, packet->blocks.size(), num_blocks_to_fill, packet->blocks[0], set_idx);
        packet->blocks.resize(num_blocks_to_fill);
        packet->aligned_address = packet->blocks[0];
        // Populate the vector
        for (uint64_t i = 1; i < num_blocks_to_fill; i++) {
           packet->blocks[i] = packet->aligned_address + i*block_size;
        }
    }
    sets[set_idx].handle_fill(packet);
    partial_misses[packet->blocks.size()-1]++;

    return;
}

void Cache::handle_fill(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0) {
    if (can_insert_at_level(level) == false) {
        return;
    }
    auto set_idx = get_set_idx(fill_packet->address);
    std::vector<uint64_t> blocks;
    fill_packet->aligned_address = align_address(fill_packet->address, fill_packet->size);
    // Populate the vector
    sets[set_idx].populate_fill_packet(fill_packet);
    eviction_packet->size = fill_packet->blocks.size()*sets[set_idx].get_block_size();
    eviction_packet->blocks.clear();
    sets[set_idx].handle_evict(eviction_packet);
    num_blocks_used += eviction_packet->footprint;
    if (eviction_packet->blocks.size() != 0) evictions++;
    sets[set_idx].handle_fill(fill_packet);
    partial_misses[fill_packet->blocks.size()-1]++;
    fill_packet->blocks.clear();
    if (cachesim::DEBUG)
        fmt::print("Level {} Inserted address {:#x} @ set {}\n", level, fill_packet->address, set_idx);
    return;
}

void Cache::handle_evict(PacketPtr access_packet, PacketPtr eviction_packet, int level = 0) {
    if (can_insert_at_level(level) == false) {
        return;
    }  
    auto set_idx = get_set_idx(access_packet->address);
    sets[set_idx].handle_evict(eviction_packet);
    num_blocks_used += eviction_packet->footprint;
    if (eviction_packet->blocks.size() != 0) evictions++;
    return;
}

void Cache::handle_invalidate(PacketPtr packet) {
    auto set_idx = get_set_idx(packet->address);
    sets[set_idx].handle_invalidate(packet);
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
