#include "defs.h"

#include "cachesim.h"
#include "msl/bits.h"
#include <cassert>

template<typename T>
Cache<T>::Cache():
        NAME("DefaultCache"),
        num_sets(1024),
        level(0),
        is_sectored(false),
        insertion_policy(InsertionPolicy::EXCLUSIVE) {
   num_ways = 16;
   for (uint64_t i = 0; i < num_sets; ++i) {
       sets[i] = new T(this, num_ways, 64, i, ReplacementPolicy::LRU, level);
   }
   partial_misses.resize(CACHELINE_SIZE/64+1, 0);
}

template<typename T>
Cache<T>::Cache(std::string name, uint64_t _num_sets, uint64_t _num_ways, uint64_t block_size, uint64_t level, bool is_sectored, ReplacementPolicy repl_policy, InsertionPolicy policy):
        NAME(name),
        num_sets(_num_sets),
        num_ways(_num_ways),
        level(level),
        is_sectored(is_sectored),
        insertion_policy(policy)
{

    if (is_sectored) {
        assert(block_size != CACHELINE_SIZE);
    } else {
        num_ways = num_ways;//*CACHELINE_SIZE/block_size;
    }
    for (uint64_t i = 0; i < num_sets; ++i) {
        sets[i] = new T(this, num_ways, block_size, i, repl_policy, level);
    }

    partial_misses.resize(CACHELINE_SIZE/block_size+1, 0);
}


template<typename T>
uint64_t Cache<T>::get_set_idx(uint64_t address) const {
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
            cumulative_hits += sets[set]->get_distance_count(way);
        }

        uint64_t miss_count = total_accesses - cumulative_hits;
        double mpki = ((double)(miss_count) / inst_count)*1000;

        fmt::print("{} | {} | {} | {:f}\n", num_sets*(way+1)*(sets[0]->get_block_size()), cumulative_hits, miss_count, mpki);
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
        fmt::print("{}:{} ", idx, partial_misses[idx]);
    }
    fmt::print("\n");

    for (auto& [pc, misses]: data_var_misses) {
        fmt::print("PC {:#x} Misses {}\n", pc, misses);
    }

    for (auto& [pc, hits]: data_var_hits) {
        fmt::print("PC {:#x} Hits {}\n", pc, hits);
    }

    for (auto& [pc, hist]: data_var_invalidations) {
        for (auto& [footprint, count]: hist)
            fmt::print("PC {:#x} Density {} Invalidations {}\n", pc, footprint, count);
;
    }

    for (auto& [pc, hist]: data_var_evictions) {
        for (auto& [footprint, count]: hist)
            fmt::print("PC {:#x} Density {} Evictions {}\n", pc, footprint, count);
    }

    if (cachesim::GEN_STATS) {
//        for (auto& [is_hub, map]: data_var_hub_hits) {
//            for (auto& [reuse, count]: map)
//                fmt::print("hub_hits: Hub {} Serviced From LLC {} Count {}\n", (is_hub? "True": "False"), reuse, count);
//        }

        for (auto& [is_hub, map]: data_var_hub_evictions) {
            for (auto& [reuse, count]: map)
                fmt::print("hub_evictions: Hub {} Serviced From LLC {} Count {}\n", (is_hub? "True": "False"), reuse, count);
        }

        for (auto& [is_hub, map]: data_var_eviction_reuse) {
            for (auto& [reuse, count]: map)
                fmt::print("eviction_reuse: Hub {} Next Reuse {:#x} Count {}\n", (is_hub? "True": "False"), reuse, count);
        }

        for (auto& [fill_pc, map]: data_var_pc_evictions1) {
            for (auto& [eviction_pc, count]: map)
                fmt::print("Eviction Candidate: Fill PC {} Eviction PC {} Count {}\n", fill_pc, eviction_pc, count);
        }
        for (auto& [fill_pc, map]: data_var_pc_evictions2) {
            for (auto& [eviction_pc, count]: map)
                fmt::print("Eviction Candidate: Fill PC {} Hub {} Count {}\n", fill_pc, (eviction_pc? "True": "False"), count);
        }
        for (auto& [fill_pc, map]: data_var_pc_evictions3) {
            for (auto& [eviction_pc, degree]: map)
                fmt::print("Eviction Candidate: Fill PC {} Eviction PC {} Degree {}\n", fill_pc, eviction_pc, (float)(degree)/data_var_pc_evictions1[fill_pc][eviction_pc]);
        }


    }
}

template<typename T>
bool Cache<T>::can_insert_at_level(int level) {
    if (insertion_policy == InsertionPolicy::EXCLUSIVE) {
        return level == 0;
    } else {
        return true;
    }
}

template<typename T>
bool Cache<T>::try_hit(PacketPtr packet) {
    auto set_idx = get_set_idx(packet->address);
    auto block_size = sets[set_idx]->get_block_size();
    auto num_blocks = packet->size/block_size;
    if (is_sectored)
        packet->aligned_address = align_address(packet->address, CACHELINE_SIZE);
    else
        packet->aligned_address = align_address(packet->address, block_size);
    assert(num_blocks > 0);
    if (packet->blocks.size() != num_blocks) {
        packet->clear_blocks();
        packet->blocks.resize(num_blocks);
        packet->block_degrees.resize(num_blocks);
        packet->block_serviced_from_llc.resize(num_blocks);
        packet->blocks[0] = packet->aligned_address;
        // Populate the vector
        for (uint64_t i = 0; i < num_blocks; i++) {
           packet->blocks[i] = align_address(packet->address, packet->size) + i*block_size;
        }
    }

    auto hit = sets[set_idx]->try_hit(packet);
    if (hit) {
        hits++;
        if (packet->is_read) read_hits++;
        else write_hits++;
        update_data_var_hits(packet->pc);
    } else {
        misses++;
        if (packet->is_read) read_misses++;
        else write_misses++;
    }

    if (!hit) {
        if (data_var_misses.find(packet->pc) == data_var_misses.end())
            data_var_misses[packet->pc] = 1;
        else
            data_var_misses[packet->pc]++;
    }
    return hit;
}

template<typename T>
void Cache<T>::handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet, int level) {
//    if (fill_packet->block_accesses.size() == 0) {
//        fill_packet->block_accesses.resize(8, 0);
//    }

    if (fill_packet->blocks.size() == 0) {
        if (cachesim::DEBUG)
            fmt::print("Level {} No blocks to fill since block list is empty\n", level);
        return;
    }

    uint64_t idx = 0;
    auto set_idx = get_set_idx(fill_packet->address);
    auto block_size = sets[set_idx]->get_block_size();
    eviction_packet->is_low_reuse = fill_packet->is_low_reuse;

    if (is_sectored)
        fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    else
        fill_packet->aligned_address = align_address(fill_packet->address, block_size);

    populate_blocks(fill_packet);
    if (fill_packet->blocks.size() == 0) {
        return;
    }

    eviction_packet->aligned_address = fill_packet->aligned_address;
    eviction_packet->size = fill_packet->blocks.size()*sets[set_idx]->get_block_size();
    eviction_packet->clear_blocks();
    eviction_packet->footprint = 0;
    sets[set_idx]->handle_evict(eviction_packet);
    if (eviction_packet->blocks.size() == 0) {
        eviction_packet->address = UINT64_MAX;
    } else {
        eviction_packet->address = *(std::find_if(eviction_packet->blocks.begin(),
            eviction_packet->blocks.end(), [](uint64_t n) {
                return n != UINT64_MAX;
            }));
    }
    eviction_packet->aligned_address = align_address(eviction_packet->address, eviction_packet->size);
    num_blocks_used += count_footprint(eviction_packet->footprint);
    evictions+=eviction_packet->blocks.size();
    sets[set_idx]->handle_fill(fill_packet);
    idx++;
    if (cachesim::DEBUG)
        fmt::print("Level {} Inserted address {:#x} @ set {} Footprint {:#x}\n", level, fill_packet->address, set_idx, fill_packet->footprint);

    fill_packet->clear_blocks();
    return;
}

template<typename T>
void Cache<T>::handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet, int level) {
//    if (fill_packet->block_accesses.size() == 0) {
//        fill_packet->block_accesses.resize(8, 0);
//    }
    auto set_idx = get_set_idx(fill_packet->address);
    fill_packet->aligned_address = align_address(fill_packet->address, fill_packet->size);
    // Populate the vector
    populate_line(fill_packet);
    eviction_packet->size = fill_packet->blocks.size()*sets[set_idx]->get_block_size();
    eviction_packet->clear_blocks();
    eviction_packet->footprint = 0;
    eviction_packet->aligned_address = fill_packet->aligned_address;
    sets[set_idx]->handle_evict(eviction_packet);
    num_blocks_used += count_footprint(eviction_packet->footprint);
    evictions+=eviction_packet->blocks.size();
    if (eviction_packet->blocks.size() == 0) {
        eviction_packet->address = UINT64_MAX;
    } else {
        eviction_packet->address = *(std::find_if(eviction_packet->blocks.begin(),
            eviction_packet->blocks.end(), [](uint64_t n) {
                return n != UINT64_MAX;
            }));
    }
    eviction_packet->aligned_address = align_address(eviction_packet->address, eviction_packet->size);
    sets[set_idx]->handle_fill(fill_packet);
    //partial_misses[fill_packet->blocks.size()-1]++;
    fill_packet->clear_blocks();
    if (cachesim::DEBUG)
        fmt::print("Level {} Inserted address {:#x} @ set {} Footprint {:#x}\n", level, fill_packet->address, set_idx, fill_packet->footprint);
    return;
}

template<typename T>
void Cache<T>::handle_evict(PacketPtr access_packet, PacketPtr eviction_packet) {
    auto set_idx = get_set_idx(access_packet->address);
    sets[set_idx]->handle_evict(eviction_packet);
    num_blocks_used += count_footprint(eviction_packet->footprint);
    if (eviction_packet->blocks.size() != 0) evictions++;
    return;
}

template<typename T>
void Cache<T>::handle_invalidate(PacketPtr packet) {
//    if (packet->block_accesses.size() == 0) {
//        packet->block_accesses.resize(8, 0);
//    }
    Packet invalidate_packet = *packet;
    invalidate_packet.clear();
    invalidate_packet.is_low_reuse = packet->is_low_reuse;
    auto set_idx = get_set_idx(packet->address);
    auto block_size = sets[set_idx]->get_block_size();
    auto aligned_address = align_address(packet->address, packet->size);
    auto num_blocks = packet->size/block_size;
    if constexpr (std::is_same_v<T, SectoredSet>) {
        //invalidate_packet.blocks.resize(num_blocks);
        //invalidate_packet.block_degrees.resize(num_blocks);
        //invalidate_packet.block_serviced_from_llc.resize(num_blocks);
        invalidate_packet.address = packet->address;
        invalidate_packet.size = packet->size;
        invalidate_packet.aligned_address = align_address(invalidate_packet.address, CACHELINE_SIZE);
        sets[set_idx]->handle_invalidate(&invalidate_packet, 0);
    } else {
        //invalidate_packet.blocks.resize(1);
        for (uint64_t block = 0; block < num_blocks; block ++) {
            invalidate_packet.address = aligned_address + block*block_size;
            invalidate_packet.size = block_size;
            sets[set_idx]->handle_invalidate(&invalidate_packet, block);
            //fmt::print("Invalidated address {:#x} block {}, serviced_from_llc {}\n", address, block, invalidate_packet.serviced_from_llc);
        }
        invalidate_packet.address = packet->address;
        invalidate_packet.size = packet->size;
    }

    *packet = invalidate_packet;
    num_blocks_used += count_footprint(invalidate_packet.footprint);
//        fmt::print("Aligned Address {:#x} Footprint {:#x}\n", aligned_address, packet->footprint);

        update_data_var_invalidations(packet->pc,
            count_footprint(packet->footprint));

    evictions+=std::count_if(invalidate_packet.blocks.begin(), invalidate_packet.blocks.end(),
            [](uint64_t addr){return (addr != UINT64_MAX);});
}

template<typename T>
void Cache<T>::print_reuse_distance() {
    auto set_idx = 0;
    fmt::print("Reuse Distance\nSet Idx | Accumulated Reuse Distance | Hits | Accesses | Avg Reuse Distance Per Hit | Avg Reuse Distance Per Access\n");
    for (const auto &set: sets) {
        fmt::print("{} | {} | {} | {} | {:.4f} | {:.4f}\n",
                set.first, set.second->reuse_dist, set.second->hits, set.second->accesses, (float)(set.second->reuse_dist)/set.second->hits, (float)(set.second->reuse_dist)/set.second->accesses); 
        set_idx++;
    }
}

template<typename T>
void Cache<T>::populate_line(PacketPtr fill_packet) {
    auto set_idx = get_set_idx(fill_packet->address);
    uint64_t num_blocks = fill_packet->size/sets.at(set_idx)->get_block_size();
    if (fill_packet->blocks.size() == 0) {
        fill_packet->blocks.resize(num_blocks, UINT64_MAX);
        fill_packet->block_degrees.resize(num_blocks, 0);
        fill_packet->block_serviced_from_llc.resize(num_blocks, 0);
    }
    assert(fill_packet->blocks.size() == num_blocks);
    auto ways = get_ways(set_idx);
    auto block_size = sets[set_idx]->get_block_size();
    for (uint64_t block_idx = 0; block_idx < fill_packet->size/sets.at(set_idx)->get_block_size(); block_idx++) {
        auto address = fill_packet->aligned_address + (block_idx*block_size);
        if (fill_packet->blocks[block_idx] != UINT64_MAX) {

            if (cachesim::DEBUG)
                fmt::print("Block address {:#x} already present in set {}. Not adding to fill packet\n", address, set_idx);
            continue;
        } else {
            fill_packet->blocks[block_idx] = address;
            if (address == align_address(fill_packet->address, block_size))
                fill_packet->block_degrees[block_idx] = fill_packet->degree;
            else
            //TODO: Fix
                fill_packet->block_degrees[block_idx] = 0;
            fill_packet->block_serviced_from_llc[block_idx] = 0;
        }
    }
    return;
}

template<typename T>
void Cache<T>::populate_blocks(PacketPtr fill_packet) {
    assert(fill_packet->blocks.size() != 0);
    //auto is_low_reuse = fill_packet->is_low_reuse;
    int idx = 0;
    for ( auto it = fill_packet->blocks.begin(); it != fill_packet->blocks.end();) {
        if (*it == UINT64_MAX) {
            if (is_sectored) {
                ++it;
            } else {
                auto index = std::distance(fill_packet->blocks.begin(), it);

                fill_packet->blocks.erase(it);
                fill_packet->block_degrees.erase(fill_packet->block_degrees.begin() + index);
                fill_packet->block_serviced_from_llc.erase(fill_packet->block_serviced_from_llc.begin() + index);
            }
            idx++;
            continue;
        }
        auto set_idx = get_set_idx(fill_packet->address);
        auto ways = get_ways(set_idx);
        auto was_accessed = (fill_packet->footprint >> idx) & 0x1;
        auto try_hit = std::find(ways.begin(), ways.end(), *it);
        auto block_size = sets[set_idx]->get_block_size();
        auto dropBlock = false;
        if (cachesim::dropBlocks && (block_size < CACHELINE_SIZE) && !sets[set_idx]->get_replacement_policy()->can_insert(fill_packet, idx)) {
            dropBlock = true;
        }

        if (try_hit != ways.end()) {
            auto way_idx = std::distance(ways.begin(), try_hit);
            if (cachesim::DEBUG && try_hit != ways.end())
                fmt::print("Block address {:#x} already present in set {} way {}. Removing fill packet\n", *it, set_idx, way_idx);
            //assert(valid[way_idx] == true);
            auto index = std::distance(fill_packet->blocks.begin(), it);
            fill_packet->blocks.erase(it);
            fill_packet->block_degrees.erase(fill_packet->block_degrees.begin() + index);
            fill_packet->block_serviced_from_llc.erase(fill_packet->block_serviced_from_llc.begin() + index);
        } else if (dropBlock){
            auto index = std::distance(fill_packet->blocks.begin(), it);
            fill_packet->blocks.erase(it);
            fill_packet->block_degrees.erase(fill_packet->block_degrees.begin() + index);
            fill_packet->block_serviced_from_llc.erase(fill_packet->block_serviced_from_llc.begin() + index);
        } else {
            ++it;
        }
        idx++;
    }
    return;
}

template<typename T>
bool Cache<T>::is_eviction_needed(PacketPtr packet) const {
    auto set_idx = get_set_idx(packet->address);
    uint64_t num_blocks = packet->size/sets.at(set_idx)->get_block_size();
    return sets.at(set_idx)->is_eviction_needed(num_blocks);
}

// Explicitly tell the compiler to generate code for these specific template types
template class Cache<Set>;
template class Cache<SectoredSet>;

