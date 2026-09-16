#include "defs.h"

#include "cachesim.h"
#include "performance_model.h"
#include "msl/bits.h"
#include <cassert>
#include <random>
#include <algorithm>
#include <cmath>

template<typename T>
Cache<T>::Cache():
        NAME("DefaultCache"),
        num_sets(1024),
        level(0),
        is_sectored(false),
        insertion_policy(InsertionPolicy::EXCLUSIVE) {
   PSEL = 0;
   num_ways = 16;
   for (uint64_t i = 0; i < num_sets; ++i) {
       sets[i] = std::make_unique<T>(this, num_ways, 64, i, ReplacementPolicy::LRU, level);
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
    PSEL = cachesim::PSEL_MAX;
    auto iso_area_num_ways = get_iso_area_cache(num_sets, num_ways, block_size);
    if (is_sectored) {
        assert(block_size != CACHELINE_SIZE);
    } else {
        num_ways = num_ways;
    }

    //uint64_t num_set_chunks = num_sets/(2*cachesim::NUM_DUELS);

    if (is_sectored) {
        for (uint64_t i = 0; i < num_sets; ++i) {
            assert(level == 0);
            sets[i] = std::make_unique<T>(this, num_ways, block_size, i, repl_policy, level);
        }
    } else {
        if (cachesim::SET_DUELING) { 
            uint64_t num_leaders = 2*cachesim::NUM_DUELS;
            uint64_t half_leaders = num_leaders/2;

            std::vector<uint64_t> indices(num_sets);
            std::vector<bool> is_leader64(num_sets, false);
            std::vector<bool> is_leader8(num_sets, false);

            for (uint64_t i =0; i < num_sets; ++i) indices[i] = i;
            std::mt19937 rng(1337);
            std::shuffle(indices.begin(), indices.end(), rng);
            for (uint64_t i = 0; i < num_leaders; i++) {
                uint64_t idx = indices[i];
                if (i < half_leaders) {
                    is_leader64[idx] = true;
                } else {
                    is_leader8[idx] = true;
                }
            }

            for (uint64_t i = 0; i < num_sets; i++) {
                //if ((i%num_set_chunks == 0) && (((i/num_set_chunks)%2) == 0)) {
                if (is_leader64[i]) {
                    sets[i] = std::make_unique<T>(this, num_ways, CACHELINE_SIZE, i, repl_policy, level);
                    sets[i]->set_dueling_type(SetDuelingType::Leader64);
                //} else if ((i%num_set_chunks == 0) && (((i/num_set_chunks)%2) == 1)) {
                } else if (is_leader8[i]) {
                    sets[i] = std::make_unique<T>(this, iso_area_num_ways, 8, i, repl_policy, level);
                    sets[i]->set_dueling_type(SetDuelingType::Leader8);
                } else {
                    sets[i] = std::make_unique<T>(this, num_ways, CACHELINE_SIZE, i, repl_policy, level);
                }
            }
        } else {
            for (uint64_t i = 0; i < num_sets; ++i) {
                sets[i] = std::make_unique<T>(this, iso_area_num_ways, block_size, i, repl_policy, level);
            }
        }

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
void Cache<T>::print_mpki_curve(uint64_t instCount) {

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
        double mpki = ((double)(miss_count) / instCount)*1000;

        fmt::print("{} | {} | {} | {:f}\n", num_sets*(way+1)*(sets[0]->get_block_size()), cumulative_hits, miss_count, mpki);
      }
}

template<typename T>
void Cache<T>::print_stats(uint64_t instCount, std::string tracename) {
    auto mpki = (((float)(get_misses()))/instCount)*1000;
    auto miss_rate = ((float)(get_misses()))/(get_hits() + get_misses());
    fmt::print(" Trace File {}, Instruction count: {} cache size: {} KB\n", tracename, (float)(instCount),
            num_sets*num_ways*get_block_size(0)/1024);
    fmt::print("hits: {} miss: {} accesses: {} read_hits: {} read_misses: {} write_hits: {} write_misses: {}\n",
         get_hits(), get_misses(), get_accesses(),
         get_read_hits(), get_read_misses(),
         get_write_hits(), get_write_misses());
    fmt::print("Miss Rate {:4f}\n", miss_rate);
    fmt::print("MPKI {:10f}\n", mpki);
    fmt::print("PSEL {}\n", get_psel());
    fmt::print("Num Cycles {}\n", PerformanceModel::getCycles());
    if (get_evictions() > 0) {
        fmt::print("Utilization {:4f} \n", 100*(float)(get_num_blocks_used())/(get_evictions()*(get_block_size(0)/8)));
    } else {
        fmt::print("Utilization undefined (No evictions)\n");
    }
//    fmt::print("Partial Misses ");
//    auto partial_misses = get_partial_misses();
//    for (long unsigned idx = 0; idx < partial_misses.size(); idx++) {
//        fmt::print("{}:{} ", idx, partial_misses[idx]);
//    }
//    fmt::print("\n");
//
//    for (auto& [pc, misses]: data_var_misses) {
//        fmt::print("PC {:#x} Misses {}\n", pc, misses);
//    }
//
//    for (auto& [pc, hits]: data_var_hits) {
//        fmt::print("PC {:#x} Hits {}\n", pc, hits);
//    }
//
//    for (auto& [pc, hist]: data_var_invalidations) {
//        for (auto& [footprint, count]: hist)
//            fmt::print("PC {:#x} Density {} Invalidations {}\n", pc, footprint, count);
//;
//    }
//
//    for (auto& [pc, hist]: data_var_evictions) {
//        for (auto& [footprint, count]: hist)
//            fmt::print("PC {:#x} Density {} Evictions {}\n", pc, footprint, count);
//    }

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

//    if (!is_sectored && cachesim::instCount % 500000 == 0) {
//        fmt::print("Inst {} MPKI {:4f}\n",
//                cachesim::instCount, (((float)(get_misses()))/cachesim::instCount)*1000);
//
//    }

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
    eviction_packet->is_high_reuse = fill_packet->is_high_reuse;

    if (is_sectored)
        fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    else
        fill_packet->aligned_address = align_address(fill_packet->address, block_size);

    if (fill_packet->size > fill_packet->blocks.size()*block_size)
        populate_line(fill_packet);
    populate_blocks(fill_packet);

    if (fill_packet->blocks.size() == 0) {
        return;
    }

    eviction_packet->aligned_address = fill_packet->aligned_address;
    if (eviction_packet->size == 0)
        eviction_packet->size = fill_packet->size;//fill_packet->blocks.size()*sets[set_idx]->get_block_size();
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
    auto block_size = sets[set_idx]->get_block_size();

    if (is_sectored)
        fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    else
        fill_packet->aligned_address = align_address(fill_packet->address, block_size);

    // Populate the vector
    populate_line(fill_packet);
    if (eviction_packet->size == 0)
        eviction_packet->size = fill_packet->size;//fill_packet->blocks.size()*sets[set_idx]->get_block_size();
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
        fmt::print("Level {} Inserted address {:#x} @ set {} Footprint {:#x}\n", level, fill_packet->aligned_address, set_idx, fill_packet->footprint);
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
    invalidate_packet.is_high_reuse = packet->is_high_reuse;
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

        //update_data_var_invalidations(packet->pc,
        //    count_footprint(packet->footprint));

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
    } else if (fill_packet->blocks.size() < num_blocks) {
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
    //auto is_high_reuse = fill_packet->is_high_reuse;
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
        //auto was_accessed = (fill_packet->footprint >> idx) & 0x1;
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

#ifdef MULTI_LEVEL
void access_multi_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            SparsityPredictor* predictor, uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree) {
    access_packet->clear();
    eviction_packet->clear();
    fill_packet->clear();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache[0]->get_block_size(cache[0]->get_set_idx(access_packet->address));
    access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
    access_packet->pc = pc;
    access_packet->next_reuse = next_reuse;
    access_packet->degree = degree;
    access_packet->avg_degree = avg_degree;

    *fill_packet = *access_packet;
    fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    *eviction_packet = *fill_packet;
    *invalidation_packet = *fill_packet;

    int num_levels = cache.size();
    int hit_at_level = num_levels;
    bool hit = false;

    if (cachesim::SET_DUELING) {
        if (cache[num_levels-1]->should_breakdown()) {
            cache[num_levels-1]->breakdown(cachesim::BLOCK_SIZE);
        } else if (cache[num_levels-1]->should_merge()) {
            cache[num_levels-1]->merge(CACHELINE_SIZE);
        }
    }

    PerformanceModel::processCPU(cachesim::instCount - cachesim::prevInstCount);
    cachesim::prevInstCount = cachesim::instCount;
    // Check for hits
    for (int i = 0; i < num_levels; i++) {
        access_packet->size = cache[i]->get_block_size(cache[i]->get_set_idx(access_packet->address));
        //access_packet->aligned_address = align_address(access_packet->aligned_address, access_packet->size);
        hit = cache[i]->try_hit(access_packet);

        if (hit) {
            hit_at_level = i;
            break;
        }

        if (access_packet->is_high_reuse) {
            access_packet->clear_blocks();
            access_packet->aligned_address = align_address(access_packet->address, CACHELINE_SIZE);
        }

    }

    if (hit) {
        eviction_packet->footprint = 0;
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        invalidation_packet->size = CACHELINE_SIZE;
    } else {
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        invalidation_packet->size = CACHELINE_SIZE;
    }

    if (hit_at_level != 0) {
        if (hit) {
            cache[hit_at_level]->handle_invalidate(invalidation_packet);
            *fill_packet = *invalidation_packet;
            fill_packet->footprint |= invalidation_packet->footprint;
            fill_packet->serviced_from_llc += 1;
            cache[0]->handle_fill_blocks(fill_packet, eviction_packet, 0);
        } else {
            cache[1]->handle_invalidate(invalidation_packet);
            if (invalidation_packet->blocks.size() != 0)
                *fill_packet = *invalidation_packet;

            cache[0]->handle_invalidate(invalidation_packet);
            if (invalidation_packet->blocks.size() != 0)
                *fill_packet = *invalidation_packet;

            fill_packet->degree = access_packet->degree;
            fill_packet->avg_degree = access_packet->avg_degree;
            cache[0]->handle_fill_line(fill_packet, eviction_packet, 0);
        }

        auto prev_cache_block_size = cache[0]->get_block_size(cache[0]->get_set_idx(eviction_packet->address));
        auto curr_cache_block_size = prev_cache_block_size;
        for (int i = 1; i < num_levels; i++) {
            curr_cache_block_size = cache[i]->get_block_size(cache[i]->get_set_idx(eviction_packet->address));

            if (prev_cache_block_size != curr_cache_block_size) {
                resize_packet(eviction_packet, curr_cache_block_size);       
            }


            fill_packet->l1_hits = eviction_packet->l1_hits;
            if (fill_packet->l1_hits == 0) fill_packet->l1_hits = 1;
            // Train on data movement from LLC -> L1D
            if (eviction_packet->blocks.size() > 0) {
                predictor->update_footprint(eviction_packet);
                predictor->update_access(fill_packet);
                if (cachesim::instCount >= cachesim::WARMUP_INSTRUCTIONS) {
                    // Predict for data movement from L1D -> LLC
                    fill_packet->is_high_reuse = predictor->predict(eviction_packet);
                }

                *fill_packet = *eviction_packet;
                fill_packet->reuse_probability = predictor->get_reuse_probability(eviction_packet);

                fill_packet->shct_value = predictor->get_shct_value(fill_packet);

                if (predictor->get_reuse_distance(eviction_packet) <= 1) {
                    fill_packet->reuse_distance = UINT64_MAX;
                } else {
                    fill_packet->reuse_distance = cachesim::instCount + predictor->get_reuse_distance(eviction_packet);
                }
                fill_packet->next_reuse = eviction_packet->next_reuse;
                eviction_packet->clear();
                cache[i]->handle_fill_blocks(fill_packet, eviction_packet, 1);

                if (eviction_packet->blocks.size() > 0) {
                    predictor->update_eviction(eviction_packet);
                }
            } else {
                break;
            }
            eviction_packet->clear();
            eviction_packet->clear_pc();
            fill_packet->clear();
            fill_packet->clear_pc();
            prev_cache_block_size = curr_cache_block_size;
        }
    }

    switch(hit_at_level) {
        case 0:
            // L1 hit - We assume the CPU has an IPC of 1 on cache hit
            // and is already accounted for
            //PerformanceModel::processL1D(fill_packet->address, true, false);
            break;
        case 1:
            // L1 miss
            //PerformanceModel::processL1D(fill_packet->address, false, false);
            PerformanceModel::processLLC(fill_packet->address, true, false);
            // L1 fill
            //PerformanceModel::processL1D(fill_packet->address, false, true);
            break;
        default:
            // L1 miss
            //PerformanceModel::processL1D(fill_packet->address, false, false);
            // LLC miss
            PerformanceModel::processLLC(fill_packet->address, false, false);
            // Memory access
            PerformanceModel::processMemory(fill_packet->address);
            // L1 fill
            //PerformanceModel::processL1D(fill_packet->address, false, true);
            break;
    }
}
#else
void access_single_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            uint64_t pc, uint64_t address, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree) {

    assert(cache.size() ==  1);
    access_packet->clear();
    eviction_packet->clear();
    fill_packet->clear();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache[0]->get_block_size(cache[0]->get_set_idx(access_packet->address));
    access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
    access_packet->pc = pc;
    access_packet->next_reuse = next_reuse;
    access_packet->degree = degree;
    access_packet->avg_degree = avg_degree;

    *fill_packet = *access_packet;
    fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    fill_packet->size = CACHELINE_SIZE;
    *eviction_packet = *fill_packet;
    *invalidation_packet = *fill_packet;

    bool hit = cache[0]->try_hit(access_packet);
    if (!hit) {
        cache[0]->handle_fill_line(fill_packet, eviction_packet, 0);
    }
    return;
}
#endif

void useAddressTrace(std::vector<BaseCache*> cache, const std::string& filename, [[maybe_unused]]SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters) {
    while (num_iters > 0) {
        std::ifstream file(filename);

        if (!file.is_open()) {
            std::cerr << "Error: Could not open the file!" << std::endl;
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            uint64_t pc = 0;
            uint64_t address = 0;
            uint64_t next_reuse = UINT64_MAX;
            uint64_t degree = 0;
            float avg_degree = 0.0f;
            int parsed_count = 0;
            char action[16];
            if (cachesim::DEBUG_ALL || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {

                if (cachesim::instCount >  cachesim::END_DEBUG) {
                    break;
                }
                if (cachesim::instCount >= cachesim::START_DEBUG) {
                    cachesim::DEBUG = true;
                }
            }

            parsed_count = std::sscanf(line.c_str(), "PC:%lu %15[^:]:0x%lx %lu %lu %f", &pc, action, &address, &next_reuse, &degree, &avg_degree);
            if (parsed_count >= 3) {
                try {
                    cachesim::instCount++;
                    if (next_reuse != UINT64_MAX) next_reuse += cachesim::instCount;
                    // Extract from the start of "0x" to the end of the line
                    bool is_read = (strcmp(action, "read") == 0) ? true : false;
#ifdef MULTI_LEVEL
                    access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        predictor, address, pc, is_read, next_reuse, degree, avg_degree);
#else
                    access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        pc, address, is_read, next_reuse, degree, avg_degree);
#endif

                } catch (const std::exception& e) {
                    std::cerr << "Conversion error on line: " << line << " -> " <<e.what() << std::endl;
                }
            }
        }
        file.close();
        --num_iters;
    }
    return;
}

void useInstructionTrace(std::vector<BaseCache*> cache, const std::string& filename, [[maybe_unused]]SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters) {
    while (num_iters > 0) {
        std::ifstream file(filename);

        if (!file.is_open()) {
            std::cerr << "Error: Could not open the file!" << std::endl;
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            uint64_t pc = 0;
            uint64_t address = 0;
            uint64_t next_reuse = UINT64_MAX;
            uint64_t degree = 0;
            float avg_degree = 0.0f;
            int parsed_count = 0;
            char action[16];
            if (cachesim::DEBUG_ALL || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {

                if (cachesim::instCount >  cachesim::END_DEBUG) {
                    break;
                }
                if (cachesim::instCount >= cachesim::START_DEBUG) {
                    cachesim::DEBUG = true;
                }
            }

            parsed_count = std::sscanf(line.c_str(), 
                           "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx,NextReuse:%lu,NodeDegree:%lu,PCAvgDegree:%f", 
                           &cachesim::instCount, &pc, action, &address, &next_reuse, &degree, &avg_degree);
            if (parsed_count >= 3) {
                try {
                    // Extract from the start of "0x" to the end of the line
                    bool is_read = (strcmp(action, "Read") == 0) ? true : false;
#ifdef MULTI_LEVEL
                    access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        predictor, address, pc, is_read, next_reuse, degree, avg_degree);
#else
                    access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        pc, address, is_read, next_reuse, degree, avg_degree);
#endif
                } catch (const std::exception& e) {
                    std::cerr << "Conversion error on line: " << line << " -> " <<e.what() << std::endl;
                }
            }
        }
        file.close();
        --num_iters;
    }
    return;
}

void useChampsimTrace(std::vector<BaseCache*> cache, const std::string& filename, [[maybe_unused]]SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters) {
    while (num_iters > 0) {
        champsim::tracereader trace(get_tracereader(filename, 0, false, false));
        while (!trace.eof()) {
            if (cachesim::DEBUG || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {
                if (cachesim::instCount > 5000000) {
                    break;
                }
            }
            cachesim::instCount++;
            auto inst = trace();
            access_packet->clear();
            eviction_packet->clear();
            fill_packet->clear();
            invalidation_packet->clear();
#ifdef MULTI_LEVEL
            for (auto& smem:inst.source_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                    predictor,smem.to<uint64_t>(), inst.ip.to<uint64_t>(), true,
                    0, 0, 0.0);
            }
            for (auto& dmem:inst.destination_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                    predictor, dmem.to<uint64_t>(), inst.ip.to<uint64_t>(), false,
                    0, 0, 0.0);
            }
#else
            for (auto& smem:inst.source_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        inst.ip.to<uint64_t>(), smem.to<uint64_t>(), true, 0, 0, 0.0);
            }
            for (auto& dmem:inst.destination_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        inst.ip.to<uint64_t>(), dmem.to<uint64_t>(), false, 0, 0, 0.0);
            }
#endif
        }
        --num_iters;
    }
    return;
}

template<typename T>
void Cache<T>::incr_psel() {
    psel_changes_this_epoch++;
    if (PSEL < cachesim::PSEL_MAX) {
        PSEL++;
    }
    // PSEL only ever moves by +-1, so landing exactly on PSEL_THRESHOLD via
    // an increment means it crossed up from PSEL_THRESHOLD - 1 (favoring
    // breakdown) to favoring 64B. PSEL_THRESHOLD is never == PSEL_MAX, so
    // this can't misfire while saturated at the top.
    if (PSEL == cachesim::PSEL_THRESHOLD) {
        psel_switches_this_epoch++;
    }
}

template<typename T>
void Cache<T>::decr_psel() {
    psel_changes_this_epoch++;
    if (PSEL > 0) {
        PSEL--;
    }
    // Symmetric to incr_psel(): landing exactly on PSEL_THRESHOLD - 1 via a
    // decrement means it crossed down from PSEL_THRESHOLD (favoring 64B) to
    // favoring breakdown. PSEL_THRESHOLD is never 0, so this can't misfire
    // while saturated at the bottom.
    if (PSEL == cachesim::PSEL_THRESHOLD - 1) {
        psel_switches_this_epoch++;
    }
}

// Conditional-binomial z-score: given mis64 + mis8 total misses observed
// over what's assumed to be equal exposure (leader64 and leader8 have the
// same number of sets and see near-identical traffic in practice, measured
// within ~0.1-2.6% across a 9-trace suite), test whether the split between
// them deviates from 50/50. Used by DuelingMode::ZTEST - doesn't need
// per-side access counts, only misses.
static double binomial_z(uint64_t mis64, uint64_t mis8) {
    uint64_t total = mis64 + mis8;
    if (total == 0)
        return 0.0;
    return ((double)mis64 - (double)mis8) / std::sqrt((double)total);
}

// Two-proportion z-score: how many standard errors apart are miss rates
// mis64/acc64 and mis8/acc8, under a pooled-variance null of "no
// difference"? Used by DuelingMode::ZTEST_RATIO - corrects for any exposure
// imbalance between the two sides, at the cost of needing access counts too.
static double two_proportion_z(uint64_t acc64, uint64_t mis64, uint64_t acc8, uint64_t mis8) {
    if (acc64 == 0 || acc8 == 0)
        return 0.0;
    double p64 = (double)mis64 / (double)acc64;
    double p8 = (double)mis8 / (double)acc8;
    double pooled = (double)(mis64 + mis8) / (double)(acc64 + acc8);
    double se = std::sqrt(pooled * (1.0 - pooled) * (1.0 / (double)acc64 + 1.0 / (double)acc8));
    return (se > 0.0) ? (p64 - p8) / se : 0.0;
}

// Logs one CSV row every CONF_EPOCH instructions (same cadence as the real
// decision checkpoints - update_dueling_confidence() always runs first, see
// should_breakdown(), so the z-scores below reflect this checkpoint's
// freshly-updated state, not the previous one). Gated behind
// --log-dueling-metrics; a no-op otherwise. Logs both z-test variants
// (miss-count and miss-ratio) regardless of which one is actually driving
// the decision, so they can be compared side by side.
template<typename T>
void Cache<T>::log_dueling_metrics() {
    if (!cachesim::LOG_DUELING_METRICS)
        return;

    if (cachesim::instCount - metrics_log_last < cachesim::CONF_EPOCH)
        return;
    metrics_log_last = cachesim::instCount;

    // All three MPKI figures share the same denominator (instCount), so
    // they're directly comparable in absolute terms - leader64/leader8 MPKI
    // will naturally look much smaller than the whole-cache figure since
    // leader sets are only a small fraction (2*NUM_DUELS out of num_sets) of
    // the cache, not because misses are rarer there.
    double cache_mpki = ((double)get_misses() / (double)cachesim::instCount) * 1000.0;
    double leader64_mpki = ((double)leader64_misses / (double)cachesim::instCount) * 1000.0;
    double leader8_mpki = ((double)leader8_misses / (double)cachesim::instCount) * 1000.0;
    double cum_z_miss = binomial_z(leader64_misses, leader8_misses);
    double window_z_miss = binomial_z(window_leader64_misses, window_leader8_misses);
    double cum_z_ratio = two_proportion_z(leader64_accesses, leader64_misses, leader8_accesses, leader8_misses);
    double window_z_ratio = two_proportion_z(window_leader64_accesses, window_leader64_misses,
                                              window_leader8_accesses, window_leader8_misses);

    fmt::print("METRIC,{},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{},{}\n",
        cachesim::instCount, PSEL, cache_mpki, leader64_mpki, leader8_mpki,
        cum_z_miss, window_z_miss, cum_z_ratio, window_z_ratio, duel_locked ? 1 : 0,
        psel_switches_last_epoch);
}

template<typename T>
void Cache<T>::record_leader_access(SetDuelingType type, bool hit) {
    switch (type) {
        case SetDuelingType::Leader64:
            leader64_accesses++;
            if (!hit) leader64_misses++;
            break;
        case SetDuelingType::Leader8:
            leader8_accesses++;
            if (!hit) leader8_misses++;
            break;
        default:
            break;
    }
}

// Confidence estimator for set dueling (DuelingMode::ZTEST / ZTEST_RATIO only
// - should_breakdown() short-circuits to the original PSEL check in
// DuelingMode::PSEL and never calls this). Every CONF_EPOCH instructions,
// two independent checks run, using binomial_z (ZTEST) or two_proportion_z
// (ZTEST_RATIO) depending on cachesim::DUELING_MODE:
//
// 1. Cumulative: z-test on Leader64 vs Leader8 misses since the start of the
//    run - a side with too few misses naturally produces a small |z|
//    instead of needing a separate hand-tuned minimum-sample gate.
// 2. Windowed: the same test, but restricted to only the last
//    Z_WINDOW_SIZE checkpoints. The cumulative test is slow to react to a
//    genuine late phase change (it has to outweigh the entire accumulated
//    history first); this reacts on its own timescale instead, at the cost
//    of losing the cumulative test's "old noise can no longer hurt you"
//    property - hence its own, separately tunable WINDOW_Z_THRESHOLD.
//
// Either check reaching CONF_MAX consecutive passing checkpoints locks in
// the decision; a checkpoint that doesn't pass resets *that check's* streak
// to zero (the other check's streak is independent). Past DUELING_PERIOD
// both checks apply against a much lower bar (one confirming checkpoint),
// so a workload that never builds full confidence still gets a decision
// eventually, but never from one raw access read.
//
// duel_locked is a two-way latch: it tracks whichever way the statistical
// signal currently favors, so a genuine later reversal (checked and found to
// never occur across as-Skitter, web-BerkStan, web-Google, sx-stackoverflow)
// would still be reflected here. Note breakdown() itself still only latches
// one-way via is_broken_down - once the cache has physically broken down its
// sets into 8B blocks it stays that way regardless of what duel_locked does
// afterward.
template<typename T>
void Cache<T>::update_dueling_confidence() {
    if (cachesim::instCount - conf_last_check < cachesim::CONF_EPOCH)
        return;
    conf_last_check = cachesim::instCount;

    if (cachesim::DUELING_MODE == DuelingMode::PSEL) {
        if (psel_switches_this_epoch == 0 && psel_changes_this_epoch > 0) {
            if (psel_quiet_streak < cachesim::CONF_MAX)
                psel_quiet_streak++;
        } else {
            psel_quiet_streak = 0;
        }
        psel_switches_last_epoch = psel_switches_this_epoch;
        psel_switches_this_epoch = 0;
        psel_changes_this_epoch = 0;

        if ((psel_quiet_streak >= cachesim::CONF_MAX) && (PSEL < cachesim::PSEL_THRESHOLD)) {
            duel_locked = true;
        }
        return;
    }

    bool use_ratio = (cachesim::DUELING_MODE == DuelingMode::ZTEST_RATIO);

    // Cumulative check.
    double z_cumulative = use_ratio
        ? two_proportion_z(leader64_accesses, leader64_misses, leader8_accesses, leader8_misses)
        : binomial_z(leader64_misses, leader8_misses);
    bool favors_cumulative = z_cumulative > cachesim::Z_THRESHOLD;
    if (favors_cumulative) {
        if (conf_counter < cachesim::CONF_MAX)
            conf_counter++;
    } else {
        conf_counter = 0;
    }

    // Windowed check: record this checkpoint's deltas, evict the oldest
    // entry once the window is full, maintain running windowed sums. Always
    // tracks all four (access64, miss64, access8, miss8) regardless of mode,
    // since log_dueling_metrics() reports both z-test variants for
    // comparison either way.
    std::array<uint64_t, 4> delta = {
        leader64_accesses - window_snapshot_leader64_accesses,
        leader64_misses - window_snapshot_leader64_misses,
        leader8_accesses - window_snapshot_leader8_accesses,
        leader8_misses - window_snapshot_leader8_misses,
    };
    window_snapshot_leader64_accesses = leader64_accesses;
    window_snapshot_leader64_misses = leader64_misses;
    window_snapshot_leader8_accesses = leader8_accesses;
    window_snapshot_leader8_misses = leader8_misses;

    window_buffer.push_back(delta);
    window_leader64_accesses += delta[0];
    window_leader64_misses += delta[1];
    window_leader8_accesses += delta[2];
    window_leader8_misses += delta[3];
    if (window_buffer.size() > cachesim::Z_WINDOW_SIZE) {
        const auto& oldest = window_buffer.front();
        window_leader64_accesses -= oldest[0];
        window_leader64_misses -= oldest[1];
        window_leader8_accesses -= oldest[2];
        window_leader8_misses -= oldest[3];
        window_buffer.pop_front();
    }

    double z_window = use_ratio
        ? two_proportion_z(window_leader64_accesses, window_leader64_misses,
                            window_leader8_accesses, window_leader8_misses)
        : binomial_z(window_leader64_misses, window_leader8_misses);
    bool favors_window = z_window > cachesim::WINDOW_Z_THRESHOLD;
    if (favors_window) {
        if (window_conf_counter < cachesim::CONF_MAX)
            window_conf_counter++;
    } else {
        window_conf_counter = 0;
    }

    //uint64_t effective_conf_max = (cachesim::instCount >= cachesim::DUELING_PERIOD) ? 1 : cachesim::CONF_MAX;
    duel_locked = (conf_counter >= cachesim::CONF_MAX || window_conf_counter >= cachesim::CONF_MAX);
}

template<typename T>
void Cache<T>::breakdown(uint64_t block_size) {
    if (is_broken_down == true)
        return;
    fmt::print("Breaking down at {}. MPKI {:4f}\n", cachesim::instCount, (((float)(get_misses()))/cachesim::instCount)*1000);
;
    for (uint64_t i = 0; i < num_sets; i++) {
        if (sets[i]->get_dueling_type() == SetDuelingType::Follower) {
            sets[i]->set_breakdown(block_size);    
        }
    }
    fmt::print("Breakdown done\n");
    is_broken_down = true;
}

template<typename T>
void Cache<T>::merge(uint64_t block_size) {
    if (is_broken_down == false)
        return;
    fmt::print("Merging at {}. MPKI {:4f}\n", cachesim::instCount, (((float)(get_misses()))/cachesim::instCount)*1000);
    for (uint64_t i = 0; i < num_sets; i++) {
        if (sets[i]->get_dueling_type() == SetDuelingType::Follower) {
            sets[i]->set_merge(block_size);
        }
    }
    fmt::print("Merge done\n");
    is_broken_down = false;
}
