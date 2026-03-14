#include "cachesim.h"
#include "msl/bits.h"
#include <cassert>

uint64_t align_address(uint64_t address, uint64_t align_size) {
    auto aligned_address = (address/align_size)*align_size;
    return aligned_address;
}

template<typename T>
uint64_t Cache<T>::get_set_idx(uint64_t address) {
    uint64_t offset_bits = champsim::msl::lg2(CACHELINE_SIZE);
    uint64_t set_bits = champsim::msl::lg2(num_sets);
    uint64_t set_idx = (address >> offset_bits) & ((0x1 << set_bits) - 1);
    assert(set_idx < num_sets);
    return set_idx;
}

template<typename T>
void Cache<T>::print_mpki_curve(uint64_t inst_count) {

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

template<typename T>
void Cache<T>::print_stats(uint64_t inst_count, std::string tracename) {
    auto mpki = (((float)(get_misses()))/inst_count)*1000;
    auto miss_rate = ((float)(get_misses()))/(get_hits() + get_misses());
    fmt::print(" Trace File {}, Instruction count: {} cache size: {} KB\n", tracename, (float)(inst_count),
            num_sets*num_ways*get_block_size(0)/1024);
    fmt::print("hits: {} miss: {} accesses: {} read_hits: {} read_misses: {} write_hits: {} write_misses: {}\n",
         get_hits(), get_misses(), get_accesses(),
         get_read_hits(), get_read_misses(),
         get_write_hits(), get_write_misses());
    fmt::print("Miss Rate {:4f}\n", miss_rate);
    fmt::print("MPKI {:10f}\n", mpki);
    if (get_evictions() > 0) {
        fmt::print("Utilization {:4f} \n", 100*(float)(get_num_blocks_used())/(get_evictions()*(get_block_size(0)/8)));
    } else {
        fmt::print("Utilization undefined (No evictions)\n");
    }
    fmt::print("Partial Misses ");
    auto partial_misses = get_partial_misses();
    for (long unsigned idx = 0; idx < partial_misses.size(); idx++) {
        fmt::print("{}:{} ", idx+1, partial_misses[idx]);
    }
    fmt::print("\n");

    if (get_do_mrc()) {
        print_mpki_curve(inst_count);
    }

    //print_reuse_distance();
    //print_histogram(page_count, get_block_size(0));


}

bool CacheSet::get_footprint(uint64_t way_idx, uint64_t word_idx) {
    uint64_t idx = way_idx*(block_size/8) + word_idx;
    return footprint[idx];
}

bool SectoredCacheSet::get_footprint(uint64_t way_idx, uint64_t word_idx) {
    uint64_t idx = way_idx*(block_size) + word_idx;
    return footprint[idx];
}

void CacheSet::set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed) {
    uint64_t idx = way_idx*(block_size/8) + word_idx;
    footprint[idx] = accessed;
    return;
}

void SectoredCacheSet::set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed) {
    uint64_t idx = way_idx*(block_size) + word_idx;
    footprint[idx] = accessed;
    return;
}

bool CacheSet::try_hit(PacketPtr packet) {
    bool hit = true;
    uint64_t hit_counter = UINT64_MAX;
    std::vector<uint64_t> way_idx_list;
    for (const auto block: packet->blocks) {
        auto way = std::find(ways.begin(), ways.end(), block);
        auto way_idx = std::distance(ways.begin(), way);
        if (lru[way_idx] < hit_counter) hit_counter = lru[way_idx];

        hit &= (way != ways.end());// && (valid[way_idx] == true);

        if (cachesim::DEBUG)
            fmt::print("{} level {} hit {} address {:#x} set {} way {} size {}\n", __func__, level, (hit) ? "HIT" : "MISS", block, set_idx, way_idx, packet->blocks.size());
        if (!hit) {
            break;
        }

        way_idx_list.push_back(way_idx);
    }

    accesses++;

    if (hit) {
        mru_counter += num_ways;
        if (do_mrc) {
            uint64_t distance = std::count_if(
                lru.begin(), lru.end(),
                [hit_counter](uint64_t n) {
                    return n > hit_counter;}
        );
            distance_counts[distance]++;
            reuse_dist += distance;
        }
        for (auto way_idx: way_idx_list) {
            if (!packet->is_read) {
                dirty[way_idx] = true;
            }
            auto word_idx = (packet->address - packet->aligned_address) >> 3;
            set_footprint(way_idx, word_idx, true);
            lru[way_idx] = mru_counter;
        }

        hits++;
    }

    return hit;
}

bool SectoredCacheSet::try_hit(PacketPtr packet) {
    bool hit = true;
    uint64_t hit_counter = UINT64_MAX;
    std::vector<uint64_t> sector_idx_list;
    auto way = std::find(ways.begin(), ways.end(), packet->aligned_address);
    auto way_idx = std::distance(ways.begin(), way);
    auto way_sector = &way_sectors[way_idx];
    hit &= (way != ways.end());

    if (hit) {
        for (const auto block: packet->blocks) {
            auto sector = std::find(way_sector->sectors.begin(), way_sector->sectors.end(), block);
            auto sector_idx = std::distance(way_sector->sectors.begin(), sector);
            //if (lru[way_idx] < hit_counter) hit_counter = lru[way_idx];
    
            hit &= (sector != way_sector->sectors.end());// && (valid[way_idx] == true);
    
            if (cachesim::DEBUG)
                fmt::print("{} level {} hit {} address {:#x} set {} way {} sector {} size {}\n", __func__, level, (hit) ? "HIT" : "MISS", block, set_idx, way_idx, sector_idx, packet->blocks.size());
            if (!hit) {
                break;
            }
    
            sector_idx_list.push_back(sector_idx);
        }
    } else {
        if (cachesim::DEBUG)
            fmt::print("{} level {} hit MISS address {:#x} sector_address {:#x} set {} way {} no sectors available size {}\n", __func__, level, packet->aligned_address, packet->address, set_idx, way_idx, packet->size);
    }

    accesses++;

    if (hit) {
        mru_counter += num_ways;
        if (do_mrc) {
            uint64_t distance = std::count_if(
                lru.begin(), lru.end(),
                [hit_counter](uint64_t n) {
                    return n > hit_counter;}
        );
            distance_counts[distance]++;
            reuse_dist += distance;
        }
        lru[way_idx] = mru_counter;
        for (auto sector_idx: sector_idx_list) {
            if (!packet->is_read) {
                dirty[sector_idx] = true;
            }
        }
        auto word_idx = (packet->address - packet->aligned_address) >> 3;
        set_footprint(way_idx, word_idx, true);

        hits++;
    }

    return hit;
}

void CacheSet::handle_fill(PacketPtr packet) {
    auto way = valid.begin();
    mru_counter+=num_ways;
    if (packet->is_sparse) {
        lru_counter++/;
    }
    uint64_t block_idx = 0;
    for (const auto block_address: packet->blocks) {
        way = std::find(way, valid.end(), false);
        auto way_idx = std::distance(valid.begin(), way);
        if (packet->is_sparse) {
            lru[way_idx] = lru_counter;
        } else {
            lru[way_idx] = mru_counter;
        }
        if (block_address != UINT64_MAX) {
            ways[way_idx] = block_address;
        }
        valid[way_idx] = true;
        pc[way_idx] = packet->pc;
//        if (align_address(packet->address, block_size) == block_address) {
        if ((packet->footprint >> block_idx) & 0x1) {
            lru[way_idx] = mru_counter;
            //auto word_idx = (packet->address - block_address) >> 3;
            set_footprint(way_idx, block_idx, true);
        }
        block_idx++;
        if (cachesim::DEBUG)
            fmt::print("Level {} Inserting address {:#x} @ set {} way {} valid {} lru {}\n", level, ways[way_idx], set_idx, way_idx, (uint32_t)valid[way_idx], lru[way_idx]);
    }
    return;
}

void SectoredCacheSet::handle_fill(PacketPtr packet) {
    auto way = std::find(valid.begin(), valid.end(), false);
    auto way_idx = std::distance(valid.begin(), way);
    mru_counter+=num_ways;
    auto sector_idx = 0;
    lru[way_idx] = mru_counter;
    valid[way_idx] = true;
    ways[way_idx] = packet->aligned_address;
    pc[way_idx] = packet->pc;
    auto way_sector = &way_sectors[way_idx];
    for (const auto block_address: packet->blocks) {
        if (block_address != UINT64_MAX) {
            way_sector->sectors[sector_idx] = block_address;
            way_sector->valid[sector_idx] = true;
        }
        if (align_address(packet->address, block_size) == block_address) {
            auto word_idx = (packet->address - packet->aligned_address) >> 3;
            set_footprint(way_idx, word_idx, true);
        }
        if (cachesim::DEBUG)
            fmt::print("Level {} Inserting address {:#x} @ set {} way {} sector {} valid {} lru {}\n", level, way_sectors[way_idx].sectors[sector_idx], set_idx, way_idx, sector_idx, (uint32_t)valid[way_idx], lru[way_idx]);

        sector_idx++;
    }
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
            packet->footprint |= get_footprint(way_idx, i) << (i + num_blocks_evicted*(block_size/8));
            set_footprint(way_idx, i, false);
        }

        if (cachesim::DEBUG)//&& valid[way_idx])
            fmt::print("Level {} Evicted set {} way {} address {:#x} dirty {} valid {} lru {}\n",
                level, set_idx, way_idx, ways[way_idx], (uint32_t)dirty[way_idx], (uint32_t)valid[way_idx], lru[way_idx]);


        valid[way_idx] = false;
        lru[way_idx] = UINT64_MAX;
        ways[way_idx] = UINT64_MAX;
        packet->pc = pc[way_idx];
        pc[way_idx] = UINT64_MAX;
        num_blocks_evicted++;
    }

    if (cachesim::DEBUG)
        fmt::print("Level {} Num invalid blocks {} Evicted blocks {} Evicted Line Footprint {:#x}\n",
            level, num_invalid_blocks, num_blocks_evicted, packet->footprint);

    return;
}

void SectoredCacheSet::handle_evict(PacketPtr packet) {
    uint64_t num_invalid_blocks = (uint64_t)(std::count(valid.begin(), valid.end(), false));
    auto try_hit = std::find(ways.begin(), ways.end(), packet->aligned_address);
    bool hit = (try_hit != ways.end());
    bool eviction_needed = (num_invalid_blocks == 0) && (hit != true);
    if (!eviction_needed) {
        if (hit) {
            if (cachesim::DEBUG)
                fmt::print("Level {} No eviction needed for set {} because some sectors are already present at way {}\n", level, set_idx, std::distance(ways.begin(), try_hit));

        } else {
            if (cachesim::DEBUG)
                fmt::print("Level {} No eviction needed for set {} because there are {} invalid ways\n", level, set_idx, std::count(valid.begin(), valid.end(), false));
        }
        return;
    }

    auto lru_way = std::min_element(lru.begin(), lru.end());
    auto way_idx = std::distance(lru.begin(), lru_way);

    if (dirty[way_idx]) {
        dirty[way_idx] = false;
    }

    for (uint64_t sector_idx = 0; sector_idx < num_blocks; sector_idx++) {
        //if (way_sectors[way_idx].valid[sector_idx]) {
            packet->blocks.push_back(way_sectors[way_idx].sectors[sector_idx]);
            way_sectors[way_idx].sectors[sector_idx] = UINT64_MAX;
            way_sectors[way_idx].valid[sector_idx] = false;
            way_sectors[way_idx].dirty[sector_idx] = false;
        //}
    }


    for (uint64_t i = 0; i < CACHELINE_SIZE/8; ++i) {
        packet->footprint |= get_footprint(way_idx, i) << i;
        set_footprint(way_idx, i, false);
    }

    if (cachesim::DEBUG)//&& valid[way_idx])
        fmt::print("Level {} Evicted set {} way {} address {:#x} dirty {} valid {} lru {}\n",
            level, set_idx, way_idx, ways[way_idx], (uint32_t)dirty[way_idx], (uint32_t)valid[way_idx], lru[way_idx]);


    valid[way_idx] = false;
    lru[way_idx] = UINT64_MAX;
    ways[way_idx] = UINT64_MAX;
    packet->pc = pc[way_idx];
    pc[way_idx] = UINT64_MAX;
   

    if (cachesim::DEBUG)
        fmt::print("Level {} Num invalid blocks {} Evicted Line Footprint {:#x}\n",
            level, num_invalid_blocks, packet->footprint);

    return;
}

uint64_t CacheSet::handle_invalidate(PacketPtr packet) {
    auto try_hit = std::find(ways.begin(), ways.end(), packet->address);
    uint64_t inv_address = UINT64_MAX;
    if (try_hit != ways.end()) {
        auto way_idx = std::distance(ways.begin(), try_hit);
        inv_address = ways[way_idx];
        valid[way_idx] = false;
        dirty[way_idx] = false;
        lru[way_idx] = UINT64_MAX;
        ways[way_idx] = UINT64_MAX;
        packet->pc = pc[way_idx];
        pc[way_idx] = UINT64_MAX;
        if (cachesim::DEBUG) {
            fmt::print("Level {} Invalidated address {:#x} @ set {} way {} because of line promotion to higher level\n", level, packet->address, set_idx, way_idx);
        }
    }
    return inv_address;
}

Sector SectoredCacheSet::handle_invalidate(PacketPtr packet) {
    auto try_hit = std::find(ways.begin(), ways.end(), packet->aligned_address);
    Sector inv_sector(num_blocks);
    if (try_hit != ways.end()) {
        auto way_idx = std::distance(ways.begin(), try_hit);
        valid[way_idx] = false;
        dirty[way_idx] = false;
        lru[way_idx] = UINT64_MAX;
        ways[way_idx] = UINT64_MAX;
        inv_sector = way_sectors[way_idx];
        way_sectors[way_idx].invalidate();
        packet->pc = pc[way_idx];
        pc[way_idx] = UINT64_MAX;
        if (cachesim::DEBUG) {
            fmt::print("Level {} Invalidated address {:#x} @ set {} way {} because of line promotion to higher level\n", level, packet->address, set_idx, way_idx);
        }
    }
    return inv_sector;
}

template<typename T>
bool Cache<T>::can_insert_at_level(int level) {
    if (insertion_policy == EXCLUSIVE) {
        return level == 0;
    } else {
        return true;
    }
}

template<typename T>
bool Cache<T>::try_hit(PacketPtr packet) {
    auto set_idx = get_set_idx(packet->address);
    auto block_size = sets[set_idx].get_block_size();
    auto num_blocks = packet->size/block_size;
    if (is_sectored)
        packet->aligned_address = align_address(packet->address, CACHELINE_SIZE);
    else
        packet->aligned_address = align_address(packet->address, block_size);
    assert(num_blocks > 0);
    if (packet->blocks.size() != num_blocks) {
        packet->blocks.clear();
        packet->blocks.resize(num_blocks);
        packet->blocks[0] = packet->aligned_address;
        // Populate the vector
        for (uint64_t i = 0; i < num_blocks; i++) {
           packet->blocks[i] = align_address(packet->address, packet->size) + i*block_size;
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

template<typename T>
void Cache<T>::handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet, int level) {
//    if (can_insert_at_level(level) == false) {
//        return;
 //   }

    if (fill_packet->blocks.size() == 0) {
        if (cachesim::DEBUG)
            fmt::print("Level {} No blocks to fill since block list is empty\n", level);
        return;
    }
    Packet fill_block_packet;
    //fill_block_packet.blocks.resize(1);
    //fill_block_packet.is_sparse = fill_packet->is_sparse;
    //uint32_t block_misses = 0;
    uint64_t idx = 0;
//    for (const auto block: fill_packet->blocks) {

    auto set_idx = get_set_idx(fill_packet->blocks[0]);
    auto block_size = sets[set_idx].get_block_size();
    //fill_packet.address = block;
    //fill_packet.size = block_size;
    if (is_sectored)
        fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    else
        fill_packet->aligned_address = align_address(fill_packet->address, block_size);
        //fill_block_packet.blocks[0] = block;
        //fill_block_packet.footprint = (fill_packet->footprint >> idx);
        //auto was_accessed = fill_block_packet.footprint & 0x1;
        //if (fill_packet->is_sparse && !was_accessed) {
        //    if (cachesim::DEBUG)
        //        fmt::print("Block address {:#x} in set {} not accessed in sparse line. Footprint {} block idx {}. Dropping it\n", block, set_idx, fill_block_packet.footprint, idx);
        //    idx++;
        //    continue;
        //}
    populate_fill_packet(fill_packet);
    if (fill_packet->blocks.size() == 0) {
            //idx++;
            //continue;
        return;
    }
    eviction_packet->size = fill_packet->blocks.size()*sets[set_idx].get_block_size();
    eviction_packet->blocks.clear();
    eviction_packet->footprint = 0;
    sets[set_idx].handle_evict(eviction_packet);
    num_blocks_used += count_footprint(eviction_packet->footprint);
    evictions+=eviction_packet->blocks.size();
    sets[set_idx].handle_fill(fill_packet);
    //block_misses++;
    idx++;
    if (cachesim::DEBUG)
        fmt::print("Level {} Inserted address {:#x} @ set {}\n", level, fill_packet->address, set_idx);

//    }
    //if (block_misses > 0)
    //    partial_misses[block_misses-1]++;
    fill_packet->blocks.clear();
    return;
}

template<typename T>
void Cache<T>::handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet, int level) {
//    if (can_insert_at_level(level) == false) {
//        return;
//    }
    auto set_idx = get_set_idx(fill_packet->address);
    fill_packet->aligned_address = align_address(fill_packet->address, fill_packet->size);
    // Populate the vector
    populate_fill_packet(fill_packet);
    eviction_packet->size = fill_packet->size;
    eviction_packet->blocks.clear();
    eviction_packet->footprint = 0;
    sets[set_idx].handle_evict(eviction_packet);
    num_blocks_used += count_footprint(eviction_packet->footprint);
    evictions+=eviction_packet->blocks.size();
    sets[set_idx].handle_fill(fill_packet);
    partial_misses[fill_packet->blocks.size()-1]++;
    fill_packet->blocks.clear();
    if (cachesim::DEBUG)
        fmt::print("Level {} Inserted address {:#x} @ set {}\n", level, fill_packet->address, set_idx);
    return;
}

template<typename T>
void Cache<T>::handle_evict(PacketPtr access_packet, PacketPtr eviction_packet, int level) {
//    if (can_insert_at_level(level) == false) {
//        return;
//    }  
    auto set_idx = get_set_idx(access_packet->address);
    sets[set_idx].handle_evict(eviction_packet);
    num_blocks_used += count_footprint(eviction_packet->footprint);
    if (eviction_packet->blocks.size() != 0) evictions++;
    return;
}

template<typename T>
std::vector<uint64_t> Cache<T>::handle_invalidate(PacketPtr packet) {
    Packet invalidate_packet;
    invalidate_packet.is_sparse = packet->is_sparse;
    auto set_idx = get_set_idx(packet->address);
    auto block_size = sets[set_idx].get_block_size();
    auto aligned_address = align_address(packet->address, packet->size);
    std::vector<uint64_t> inv_address;
    auto num_blocks = packet->size/block_size;
    if constexpr (std::is_same_v<T, SectoredCacheSet>) {
        invalidate_packet.blocks.resize(num_blocks);
        invalidate_packet.address = packet->address;
        invalidate_packet.size = packet->size;
        invalidate_packet.aligned_address = align_address(invalidate_packet.address, CACHELINE_SIZE);
         auto address = sets[set_idx].handle_invalidate(&invalidate_packet);
        inv_address = address.sectors;
    } else {
        invalidate_packet.blocks.resize(1);
        inv_address.resize(num_blocks);
        for (uint64_t block = 0; block < num_blocks; block ++) {
            invalidate_packet.address = aligned_address + block*block_size;
            invalidate_packet.size = block_size;
            invalidate_packet.aligned_address = align_address(invalidate_packet.address, block_size);
            auto address = sets[set_idx].handle_invalidate(&invalidate_packet);
            inv_address[block] = address;
        }
    }

    return inv_address;
}

template<typename T>
void Cache<T>::print_reuse_distance() {
    auto set_idx = 0;
    fmt::print("Reuse Distance\nSet Idx | Accumulated Reuse Distance | Hits | Accesses | Avg Reuse Distance Per Hit | Avg Reuse Distance Per Access\n");
    for (const auto &set: sets) {
        fmt::print("{} | {} | {} | {} | {:.4f} | {:.4f}\n",
                set.first, set.second.reuse_dist, set.second.hits, set.second.accesses, (float)(set.second.reuse_dist)/set.second.hits, (float)(set.second.reuse_dist)/set.second.accesses); 
        set_idx++;
    }
}

template<typename T>
void Cache<T>::populate_fill_packet(PacketPtr fill_packet) {
    if (fill_packet->blocks.size() != 0)  {
        auto is_sparse = fill_packet->is_sparse;
        int idx = 0;
        for ( auto it = fill_packet->blocks.begin(); it != fill_packet->blocks.end();) {
            if (*it == UINT64_MAX) {
                if (is_sectored) {
                    ++it;
                } else {
                    fill_packet->blocks.erase(it);
                }
                idx++;
                continue;
            }
            auto set_idx = get_set_idx(fill_packet->address);
            auto ways = get_ways(set_idx);
            //auto was_accessed = (fill_packet->footprint >> idx) & 0x1;
            auto try_hit = std::find(ways.begin(), ways.end(), *it);
            if (try_hit != ways.end()) {// || (is_sparse && !was_accessed)) {
                auto way_idx = std::distance(ways.begin(), try_hit);
                if (cachesim::DEBUG && try_hit != ways.end())
                    fmt::print("Block address {:#x} already present in set {} way {}. Removing fill packet\n", *it, set_idx, way_idx);
//                else if (cachesim::DEBUG && (is_sparse && !was_accessed))
//                    fmt::print("Block address {:#x} in set {} way {} unneeded in sparse line. Footprint {} block idx {}. Removing from fill packet\n", *it, set_idx, way_idx, fill_packet->footprint, idx);
                //assert(valid[way_idx] == true);
                fill_packet->blocks.erase(it);
            } else {
                ++it;
            }
            idx++;
        }
        return;
    } else {
        auto set_idx = get_set_idx(fill_packet->address);
        auto ways = get_ways(set_idx);
        auto block_size = sets[set_idx].get_block_size();
        for (uint64_t address = fill_packet->aligned_address; address < fill_packet->aligned_address + fill_packet->size; address += block_size) {
            auto try_hit = std::find(ways.begin(), ways.end(), address);
            if (try_hit != ways.end()) {
                auto way_idx = std::distance(ways.begin(), try_hit);
                assert(sets[set_idx].get_valid(way_idx) == true);
                if (cachesim::DEBUG)
                    fmt::print("Block address {:#x} already present in set {} way {}. Not adding to fill packet\n", address, set_idx, way_idx);
                continue;
            }
            fill_packet->blocks.push_back(address);
       }
    }
    return;
}

uint64_t count_footprint(uint64_t footprint) {
    return __builtin_popcountll(footprint);
}

void resize_packet(PacketPtr packet, uint64_t block_size) {
    auto num_blocks = packet->size/block_size;
    if (packet->blocks.size() > 0 &&
            packet->blocks.size() != num_blocks) {
        packet->blocks.resize(num_blocks);
        auto aligned_address = 
            align_address(packet->blocks[0], packet->size);
        for (uint64_t idx = 0; idx < block_size; ++idx) {
            packet->blocks[idx] = aligned_address + idx*block_size;
        }
    }
}

// Explicitly tell the compiler to generate code for these specific template types
template class Cache<CacheSet>;
template class Cache<SectoredCacheSet>;
