#include "defs.h"

#include "cachesim.h"
#include "popt_replacement_policy.h"
#include "belady_replacement_policy.h"
#include "performance_model.h"
#include "msl/bits.h"
#include "trace_file.h"
#include <cassert>
#include <random>
#include <algorithm>
#include <cmath>
#include <unordered_map>

template<typename T>
Cache<T>::Cache():
        NAME("DefaultCache"),
        num_sets(1024),
        level(CacheLevel::L1D),
        is_sectored(false),
        insertion_policy(InsertionPolicy::EXCLUSIVE) {
   PSEL = 0;
   num_ways = 16;
   for (uint64_t i = 0; i < num_sets; ++i) {
       sets[i] = std::make_unique<T>(this, num_ways, 64, i, ReplacementPolicy::LRU, level);
   }
   partial_misses.resize(CACHELINE_SIZE/8+1, 0);   // index = blocks of the line present on a miss (0..8)
}

template<typename T>
Cache<T>::Cache(std::string name, uint64_t _num_sets, uint64_t _num_ways, uint64_t block_size, CacheLevel level, bool is_sectored, ReplacementPolicy repl_policy, InsertionPolicy policy):
        NAME(name),
        num_sets(_num_sets),
        num_ways(_num_ways),
        level(level),
        is_sectored(is_sectored),
        insertion_policy(policy)
{
    PSEL = cachesim::PSEL_THRESHOLD;
    auto iso_area_num_ways = get_iso_area_cache(num_sets, num_ways, block_size);
    if (level == CacheLevel::L1D && cachesim::L1_AMOEBA)
        iso_area_num_ways = get_amoeba_words(num_sets, num_ways);   // Amoeba L1D, 8B words
    if (is_sectored) {
        assert(block_size != CACHELINE_SIZE);
    } else {
        num_ways = num_ways;
    }

    //uint64_t num_set_chunks = num_sets/(2*cachesim::NUM_DUELS);

    if (is_sectored) {
        // Sectored caches: the L1D and the optional L2.
        assert(level != CacheLevel::LLC);
        for (uint64_t i = 0; i < num_sets; ++i) {
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

    partial_misses.resize(CACHELINE_SIZE/8+1, 0);   // index = blocks of the line present on a miss (0..8)
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
    fmt::print("Cache {}\n", NAME);
    fmt::print(" Trace File {}, Instruction count: {} cache size: {} KB\n", tracename, (float)(instCount),
            num_sets*num_ways*get_block_size(0)/1024);
    // Upper levels report MPKI only; every other stat is for the last level
    // (the LLC in the multi-level build, the one cache otherwise).
#ifdef MULTI_LEVEL
    const bool last_level = level == CacheLevel::LLC;
#else
    const bool last_level = true;
#endif
    if (!last_level) {
        fmt::print("MPKI {:10f}\n", mpki);
        return;
    }
    fmt::print("hits: {} miss: {} accesses: {} read_hits: {} read_misses: {} write_hits: {} write_misses: {}\n",
         get_hits(), get_misses(), get_accesses(),
         get_read_hits(), get_read_misses(),
         get_write_hits(), get_write_misses());
    fmt::print("Miss Rate {:4f}\n", miss_rate);
    fmt::print("MPKI {:10f}\n", mpki);
    fmt::print("PSEL {}\n", get_psel());
    fmt::print("Num Cycles {}\n", PerformanceModel::getCycles());
    if constexpr (std::is_same_v<T, Set>) {
        if (cachesim::TAG_BDI_BASES > 0 && cachesim::isoArea && cachesim::BLOCK_SIZE < CACHELINE_SIZE) {
            uint64_t enc = 0, ovf = 0;
            for (const auto& [idx, s] : sets) s->bdi_resident(enc, ovf);
            auto fills = Set::bdi_stat_encoded + Set::bdi_stat_overflow;
            fmt::print("Tag BDI bases {} delta {}: fills encoded {} overflow {} ({:.4f}% of fills) | base installs {} frees {} | base replacements {} blocks evicted by them {} ({:.4f}% of fills) | resident 8B blocks encoded {} overflow {} ({:.4f}%)\n",
                cachesim::TAG_BDI_BASES, cachesim::TAG_BDI_DELTA, Set::bdi_stat_encoded, Set::bdi_stat_overflow,
                fills ? 100.0 * Set::bdi_stat_overflow / fills : 0.0, Set::bdi_stat_installs, Set::bdi_stat_frees,
                Set::bdi_stat_replacements, Set::bdi_stat_replaced_blocks,
                fills ? 100.0 * Set::bdi_stat_replaced_blocks / fills : 0.0,
                enc, ovf, (enc + ovf) ? 100.0 * ovf / (enc + ovf) : 0.0);
        }
    }
    // Per residency at this level: share of 8B words used while resident (the
    // demanded word or a hit). Running totals over the blocks evicted or
    // invalidated (moved up) from this level, plus, for non-sectored caches,
    // the blocks still resident at the end (their residency ends with the
    // run), so a working set that fits is not judged by its few evictions
    // alone; both parts are also printed on their own. Single-level only:
    // the multi-level LLC prints Line lifetime utilization below instead.
    if (level != CacheLevel::LLC) {
        uint64_t resident = 0, resident_used = 0;
        if constexpr (std::is_same_v<T, Set>) {
            uint64_t capacity = 0;
            for (const auto& [idx, s] : sets) s->count_resident(capacity, resident, resident_used);
        }
        auto pct = [](uint64_t used, uint64_t words) { return words ? 100.0 * used / words : 0.0; };
        uint64_t evicted_used = get_num_blocks_used();
        fmt::print("Utilization {:.4f}% of words used | evicted words {} used {} ({:.4f}%) | "
                   "resident at end words {} used {} ({:.4f}%)\n",
            pct(evicted_used + resident_used, evicted_words + resident), evicted_words, evicted_used,
            pct(evicted_used, evicted_words), resident, resident_used, pct(resident_used, resident));
    }
    if constexpr (std::is_same_v<T, Set>) {
        if (!sets.empty() && sets.begin()->second->amoeba_active()) {
            uint64_t capacity = 0, valid_pieces = 0, touched = 0, am_blocks = 0, am_data = 0;
            for (const auto& [idx, s] : sets) {
                s->count_resident(capacity, valid_pieces, touched);
                s->amoeba_resident(am_blocks, am_data);
            }
            uint64_t tag_words = cachesim::AMOEBA_FREE_TAGS ? 0 : am_blocks;
            fmt::print("Amoeba LLC at end: {} blocks ({:.2f}/set, {:.3f} data words/block) | words: data {} tag {} free {} of {}\n",
                am_blocks, (double)am_blocks / sets.size(), am_blocks ? (double)am_data / am_blocks : 0.0,
                am_data, tag_words, capacity - am_data - tag_words, capacity);
        }
        if (level == CacheLevel::LLC) {
            fmt::print("Line lifetime utilization {:4f} | touched {} of {} 8B pieces that left the hierarchy\n",
                Set::life_pieces ? 100.0 * Set::life_touched / Set::life_pieces : 0.0, Set::life_touched, Set::life_pieces);
            uint64_t capacity = 0, valid_pieces = 0, touched = 0;
            for (const auto& [idx, s] : sets) s->count_resident(capacity, valid_pieces, touched);
            if (!Set::reuse_residents_recorded) {   // lifetimes still open at the end count as ending now
                for (const auto& [idx, s] : sets) s->record_reuse_residents();
                Set::reuse_residents_recorded = true;
            }
            fmt::print("LLC touched capacity at end {:4f} | touched {} valid {} capacity {} 8B pieces ({:4f}% valid)\n",
                capacity ? 100.0 * touched / capacity : 0.0, touched, valid_pieces, capacity,
                capacity ? 100.0 * valid_pieces / capacity : 0.0);
        }
    }
    if constexpr (std::is_same_v<T, Set>) {
#ifndef MULTI_LEVEL
        for (const auto& [idx, s] : sets) s->record_line_used_residents();   // print_stats runs once
        const char* population = "evicted + resident at end";
#else
        const char* population = "left the hierarchy";
#endif
        uint64_t lines = 0, used = 0;
        for (int k = 0; k <= 8; k++) {
            lines += Set::line_used_hist[k];
            used += k * Set::line_used_hist[k];
        }
        if (lines == 0) {
            fmt::print("Used words per 64B line: n/a (no 64B blocks)\n");
        } else {
            fmt::print("Used words per 64B line ({}):", population);
            for (int k = 0; k <= 8; k++) fmt::print(" {}:{}", k, Set::line_used_hist[k]);
            fmt::print(" | lines {} mean {:.4f} of 8\n", lines, (double)used / lines);
        }
    }
    if constexpr (std::is_same_v<T, Set>) {
        // Cache efficiency: live word-time over capacity x run length. The
        // stays still open end now (print_stats runs once, after the trace).
        uint64_t capacity_words = 0;
        for (const auto& [idx, s] : sets) {
            s->eff_close_residents();
            capacity_words += s->get_ways().size() * s->get_block_size() / 8;
        }
        double total = (double)capacity_words * instCount;
        uint64_t dead = Set::eff_dead_after_use + Set::eff_dead_unused;
        uint64_t occupied = Set::eff_live + dead;
        auto pct = [](double x, double of) { return of > 0 ? 100.0 * x / of : 0.0; };
        fmt::print("Cache efficiency {:.4f}% (live word-time / capacity x time) | occupied {:.4f}% of capacity, "
                   "live {:.4f}% of occupied | dead: never used {:.4f}%, after last use {:.4f}% of occupied\n",
            pct(Set::eff_live, total), pct(occupied, total), pct(Set::eff_live, occupied),
            pct(Set::eff_dead_unused, occupied), pct(Set::eff_dead_after_use, occupied));
    }
    // Misses by how many 8B-or-larger blocks of the requested 64B line were
    // already present at this level (64B LLC: always 0).
    fmt::print("Partial Misses (blocks of the 64B line present on a miss) ");
    auto partial_misses = get_partial_misses();
    for (long unsigned idx = 0; idx < partial_misses.size(); idx++) {
        fmt::print("{}:{} ", idx, partial_misses[idx]);
    }
    fmt::print("\n");
}

template<typename T>
bool Cache<T>::can_insert_at_level(CacheLevel level) {
    if (insertion_policy == InsertionPolicy::EXCLUSIVE) {
        return level == CacheLevel::L1D;
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
        packet->block_prev_counter.resize(num_blocks, UINT64_MAX);
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
    } else {
        misses++;
        if (packet->is_read) read_misses++;
        else write_misses++;
    }


//    if (!is_sectored && cachesim::instCount % 500000 == 0) {
//        fmt::print("Inst {} MPKI {:4f}\n",
//                cachesim::instCount, (((float)(get_misses()))/cachesim::instCount)*1000);
//
//    }

    return hit;
}

template<typename T>
void Cache<T>::handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet) {
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
    if constexpr (std::is_same_v<T, Set>) {
        // Base+Delta tags: free a base for this line first if none fits; the
        // regular victim selection below then evicts only what is still needed.
        uint64_t used_words = 0;
        auto bdi_evicted = sets[set_idx]->bdi_reserve(fill_packet, used_words);
        evictions += bdi_evicted;
        evicted_words += bdi_evicted * (sets[set_idx]->get_block_size() / 8);
        num_blocks_used += used_words;
        // Amoeba-Cache: make contiguous room for each block of this fill; the
        // evicted words land in eviction_packet and are counted below.
        if (sets[set_idx]->amoeba_active())
            sets[set_idx]->amoeba_reserve(fill_packet, eviction_packet);
    }
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
    evicted_words += eviction_packet->blocks.size() * (sets[set_idx]->get_block_size() / 8);
    sets[set_idx]->handle_fill(fill_packet);

    idx++;
    if (cachesim::DEBUG)
        fmt::print("Level {} Inserted address {:#x} @ set {} Footprint {:#x}\n", level, fill_packet->address, set_idx, fill_packet->footprint);

    fill_packet->clear_blocks();
    return;
}

template<typename T>
void Cache<T>::handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet) {
//    if (fill_packet->block_accesses.size() == 0) {
//        fill_packet->block_accesses.resize(8, 0);
//    }
    auto set_idx = get_set_idx(fill_packet->address);

    // The fill covers the whole 64B line holding the access, at any block
    // size: aligning to a smaller block would start the window mid-line and
    // spill blocks into the next line, which belongs to another set.
    fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);

    // Populate the vector
    populate_line(fill_packet);
    if constexpr (std::is_same_v<T, Set>) {
        // Below 64B a line can be partly resident: drop the blocks already
        // present (and any the policy or Amoeba declines), as
        // handle_fill_blocks does, so no block is filled twice.
        populate_blocks(fill_packet);
    }
    if (eviction_packet->size == 0)
        eviction_packet->size = fill_packet->size;//fill_packet->blocks.size()*sets[set_idx]->get_block_size();
    eviction_packet->clear_blocks();
    eviction_packet->footprint = 0;
    eviction_packet->aligned_address = fill_packet->aligned_address;
    if constexpr (std::is_same_v<T, Set>) {
        // Base+Delta tags: free a base for this line first if none fits, as
        // handle_fill_blocks does; without it a line too far from all of a
        // full set of bases has no base to encode against.
        uint64_t used_words = 0;
        auto bdi_evicted = sets[set_idx]->bdi_reserve(fill_packet, used_words);
        evictions += bdi_evicted;
        evicted_words += bdi_evicted * (sets[set_idx]->get_block_size() / 8);
        num_blocks_used += used_words;
        if (sets[set_idx]->amoeba_active())
            sets[set_idx]->amoeba_reserve(fill_packet, eviction_packet);
    }
    sets[set_idx]->handle_evict(eviction_packet);
    num_blocks_used += count_footprint(eviction_packet->footprint);
    evictions+=eviction_packet->blocks.size();
    evicted_words += eviction_packet->blocks.size() * (sets[set_idx]->get_block_size() / 8);
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
    if (eviction_packet->blocks.size() != 0) {
        evictions++;
        evicted_words += eviction_packet->blocks.size() * (sets[set_idx]->get_block_size() / 8);
    }
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

    auto invalidated = std::count_if(invalidate_packet.blocks.begin(), invalidate_packet.blocks.end(),
            [](uint64_t addr){return (addr != UINT64_MAX);});
    if constexpr (std::is_same_v<T, Set>) {
        // 16B/32B blocks moving up: L1D holds 8B sectors, so hand it one entry
        // per sector, each with its block's degree and LLC service count.
        if (block_size > 8 && block_size < CACHELINE_SIZE) {
            uint64_t per = block_size / 8;
            std::vector<uint64_t> blocks(CACHELINE_SIZE/8, UINT64_MAX), degrees(CACHELINE_SIZE/8, 0), serviced(CACHELINE_SIZE/8, 0);
            std::vector<uint64_t> prev(CACHELINE_SIZE/8, UINT64_MAX);
            for (uint64_t b = 0; b < invalidate_packet.blocks.size(); b++) {
                if (invalidate_packet.blocks[b] == UINT64_MAX) continue;
                for (uint64_t s = 0; s < per; s++) {
                    blocks[b*per + s] = invalidate_packet.blocks[b] + 8*s;
                    degrees[b*per + s] = invalidate_packet.block_degrees[b];
                    serviced[b*per + s] = invalidate_packet.block_serviced_from_llc[b];
                    prev[b*per + s] = invalidate_packet.block_prev_counter[b];
                }
            }
            invalidate_packet.blocks = blocks;
            invalidate_packet.block_degrees = degrees;
            invalidate_packet.block_serviced_from_llc = serviced;
            invalidate_packet.block_prev_counter = prev;
        }
    }
    *packet = invalidate_packet;
    num_blocks_used += count_footprint(invalidate_packet.footprint);

    evictions += invalidated;
    evicted_words += invalidated * (block_size / 8);
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
        fill_packet->block_prev_counter.resize(num_blocks, UINT64_MAX);
    } else if (fill_packet->blocks.size() < num_blocks) {
        fill_packet->blocks.resize(num_blocks, UINT64_MAX);
        fill_packet->block_degrees.resize(num_blocks, 0);
        fill_packet->block_serviced_from_llc.resize(num_blocks, 0);
        fill_packet->block_prev_counter.resize(num_blocks, UINT64_MAX);
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
            fill_packet->block_prev_counter[block_idx] = UINT64_MAX;
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
                if (index < (decltype(index))fill_packet->block_prev_counter.size())
                    fill_packet->block_prev_counter.erase(fill_packet->block_prev_counter.begin() + index);
            }
            idx++;
            continue;
        }
        auto set_idx = get_set_idx(fill_packet->address);
        const auto& ways = get_ways(set_idx);
        //auto was_accessed = (fill_packet->footprint >> idx) & 0x1;
        auto block_size = sets[set_idx]->get_block_size();
        // Presence check. Set (LLC) matches on its per-way tags; an address that
        // isn't block-aligned never equalled a stored (aligned) address, so it
        // stays a miss. SectoredSet (L1D/L2) keeps the address lookup.
        uint64_t found_way = ways.size();
        if constexpr (std::is_same_v<T, Set>) {
            if (align_address(*it, block_size) == *it)
                found_way = sets[set_idx]->find_way(*it);
#ifdef TAG_CHECK
            assert(found_way == (uint64_t)std::distance(ways.begin(), std::find(ways.begin(), ways.end(), *it)));
#endif
        } else {
            found_way = std::distance(ways.begin(), std::find(ways.begin(), ways.end(), *it));
        }
        auto try_hit = ways.begin() + found_way;
        auto dropBlock = false;
        if (cachesim::dropBlocks && (block_size < CACHELINE_SIZE) && !sets[set_idx]->get_replacement_policy()->can_insert(fill_packet, idx)) {
            dropBlock = true;
        }
        if constexpr (std::is_same_v<T, Set>) {
            // Amoeba-Cache stores only the words its insertion mode selects.
            if (try_hit == ways.end() && sets[set_idx]->amoeba_active() && !sets[set_idx]->amoeba_keep(fill_packet, *it))
                dropBlock = true;
        }

        if (try_hit != ways.end()) {
            auto way_idx = std::distance(ways.begin(), try_hit);
            if constexpr (std::is_same_v<T, Set>) {
                // Another part of a line the LLC already holds (from an Amoeba
                // L1D, which evicts partial lines): merge it into that way.
                if (sets[set_idx]->partial_lines())
                    sets[set_idx]->merge_partial_line(way_idx, fill_packet->present_mask, fill_packet->footprint & 0xff);
            }
            if (cachesim::DEBUG && try_hit != ways.end())
                fmt::print("Block address {:#x} already present in set {} way {}. Removing fill packet\n", *it, set_idx, way_idx);
            //assert(valid[way_idx] == true);
            auto index = std::distance(fill_packet->blocks.begin(), it);
            fill_packet->blocks.erase(it);
            fill_packet->block_degrees.erase(fill_packet->block_degrees.begin() + index);
            fill_packet->block_serviced_from_llc.erase(fill_packet->block_serviced_from_llc.begin() + index);
            if (index < (decltype(index))fill_packet->block_prev_counter.size())
                fill_packet->block_prev_counter.erase(fill_packet->block_prev_counter.begin() + index);
        } else if (dropBlock){
            if (level == CacheLevel::LLC) {   // never inserted: its line lifetime ends here
                auto index = std::distance(fill_packet->blocks.begin(), it);
                Set::record_reuse(fill_packet->block_degrees[index], fill_packet->avg_degree,
                                  fill_packet->block_serviced_from_llc[index]);
                Set::life_pieces++;
                Set::life_touched += (fill_packet->footprint >> ((*it % CACHELINE_SIZE) / 8)) & 1;
            }
            auto index = std::distance(fill_packet->blocks.begin(), it);
            fill_packet->blocks.erase(it);
            fill_packet->block_degrees.erase(fill_packet->block_degrees.begin() + index);
            fill_packet->block_serviced_from_llc.erase(fill_packet->block_serviced_from_llc.begin() + index);
            if (index < (decltype(index))fill_packet->block_prev_counter.size())
                fill_packet->block_prev_counter.erase(fill_packet->block_prev_counter.begin() + index);
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
// An Amoeba L1D evicts whole blocks, possibly of several lines, into one
// packet (8B words; footprint bit k = k-th word evicted). Regroup them into
// one 64B-line packet per line - word-indexed blocks, footprint and per-block
// arrays, and the words present - for the levels below.
static std::vector<Packet> split_amoeba_victims(const Packet& ev) {
    std::vector<Packet> out;
    for (size_t k = 0; k < ev.blocks.size(); k++) {
        uint64_t b = ev.blocks[k];
        if (b == UINT64_MAX) continue;
        uint64_t line = align_address(b, CACHELINE_SIZE);
        auto it = std::find_if(out.begin(), out.end(), [line](const Packet& p) { return p.address == line; });
        if (it == out.end()) {
            Packet p = ev;
            p.address = p.aligned_address = line;
            p.size = CACHELINE_SIZE;
            p.footprint = 0;
            p.present_mask = 0;
            p.serviced_from_llc = 0;
            p.blocks.assign(CACHELINE_SIZE / 8, UINT64_MAX);
            p.block_degrees.assign(CACHELINE_SIZE / 8, 0);
            p.block_serviced_from_llc.assign(CACHELINE_SIZE / 8, 0);
            p.block_prev_counter.assign(CACHELINE_SIZE / 8, UINT64_MAX);
            out.push_back(p);
            it = out.end() - 1;
        }
        uint32_t w = (b % CACHELINE_SIZE) / 8;
        it->blocks[w] = b;
        it->present_mask |= 1u << w;
        if (k < 64 && ((ev.footprint >> k) & 1)) it->footprint |= 1ull << w;
        if (k < ev.block_degrees.size()) it->block_degrees[w] = ev.block_degrees[k];
        if (k < ev.block_serviced_from_llc.size()) {
            it->block_serviced_from_llc[w] = ev.block_serviced_from_llc[k];
            it->serviced_from_llc = std::max(it->serviced_from_llc, ev.block_serviced_from_llc[k]);
        }
        if (k < ev.block_prev_counter.size()) it->block_prev_counter[w] = ev.block_prev_counter[k];
    }
    return out;
}

// --l1-amoeba-keep-llc: the part of a line the Amoeba L1D did not take, as a
// packet that puts it into the LLC as a partial line.
static Packet amoeba_rest_of_line(const Packet& access, uint8_t words, uint64_t footprint, uint64_t serviced) {
    Packet p;
    p.pc = access.pc;
    p.degree = access.degree;
    p.avg_degree = access.avg_degree;
    p.next_reuse = access.next_reuse;
    p.address = p.aligned_address = align_address(access.address, CACHELINE_SIZE);
    p.size = CACHELINE_SIZE;
    p.present_mask = words;
    p.footprint = footprint & words;
    p.serviced_from_llc = serviced;
    for (uint64_t w = 0; w < CACHELINE_SIZE / 8; w++) {
        p.blocks.push_back(((words >> w) & 1) ? p.address + 8 * w : UINT64_MAX);
        p.block_degrees.push_back(0);
        p.block_serviced_from_llc.push_back(serviced);
        p.block_prev_counter.push_back(UINT64_MAX);
    }
    return p;
}

int access_multi_level(std::vector<BaseCache*> &cache,
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
            if (cachesim::USE_GHOST_CACHE) {
                cachesim::ghostCache.breakdown(CACHELINE_SIZE, cachesim::BLOCK_SIZE);
            }
        } else if (cache[num_levels-1]->should_merge()) {
            cache[num_levels-1]->merge(CACHELINE_SIZE);
            if (cachesim::USE_GHOST_CACHE) {
                cachesim::ghostCache.merge(cachesim::BLOCK_SIZE, CACHELINE_SIZE);
            }
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
        Packet rest_of_line;            // --l1-amoeba-keep-llc only
        bool keep_rest = false;
        if (hit) {
            cache[hit_at_level]->handle_invalidate(invalidation_packet);
            *fill_packet = *invalidation_packet;
            fill_packet->footprint |= invalidation_packet->footprint;
            // serviced_from_llc counts LLC round trips only, not L2 hits.
            if (cache[hit_at_level]->get_level() == CacheLevel::LLC)
                fill_packet->serviced_from_llc += 1;
            // The line's next use is this access's NextReuse, not the one
            // stored with it (which was this access).
            fill_packet->next_reuse = access_packet->next_reuse;
            if (cachesim::L1_AMOEBA) {
                // The Amoeba L1D takes the line's words it predicts (of those
                // the LLC copy holds) around the demanded one; the rest of the
                // line is dropped.
                const uint64_t line = align_address(access_packet->address, CACHELINE_SIZE);
                fill_packet->address = access_packet->address;
                fill_packet->size = CACHELINE_SIZE;
                fill_packet->clear_blocks();
                for (uint64_t w = 0; w < CACHELINE_SIZE / 8; w++) {
                    fill_packet->blocks.push_back(line + 8 * w);
                    fill_packet->block_degrees.push_back(line + 8 * w == align_address(access_packet->address, 8) ? access_packet->degree : 0);
                    fill_packet->block_serviced_from_llc.push_back(fill_packet->serviced_from_llc);
                    fill_packet->block_prev_counter.push_back(UINT64_MAX);
                }
                const uint8_t src_words = fill_packet->present_mask;
                const uint64_t src_footprint = fill_packet->footprint, src_serviced = fill_packet->serviced_from_llc;
                cache[0]->handle_fill_line(fill_packet, eviction_packet);
                if (cachesim::L1_AMOEBA_KEEP_LLC) {
                    uint8_t rest = src_words & ~Set::am_l1_last_line_words;
                    if (rest) {
                        rest_of_line = amoeba_rest_of_line(*access_packet, rest, src_footprint, src_serviced);
                        keep_rest = true;
                    }
                }
            } else {
                cache[0]->handle_fill_blocks(fill_packet, eviction_packet);
            }
        } else {
            // Pull any partial copy of the line out of every level, LLC first
            // and L1D last, so the line is refilled into L1D whole. A miss in a
            // sectored cache clears the packet, so restore the line's address
            // and size before each level.
            const uint64_t line_address = invalidation_packet->address;
            const uint64_t line_size = invalidation_packet->size;
            uint64_t line_footprint = 0;   // touched sectors of every partial copy
            // An Amoeba L1D keeps the words of the line it already holds and
            // fetches only around the missing one, so it is not invalidated.
            const int lowest = cachesim::L1_AMOEBA ? 1 : 0;
            for (int i = num_levels - 1; i >= lowest; i--) {
                invalidation_packet->address = line_address;
                invalidation_packet->size = line_size;
                cache[i]->handle_invalidate(invalidation_packet);
                line_footprint |= invalidation_packet->footprint;
                if (invalidation_packet->blocks.size() != 0)
                    *fill_packet = *invalidation_packet;
            }
            fill_packet->footprint |= line_footprint;

            fill_packet->degree = access_packet->degree;
            fill_packet->avg_degree = access_packet->avg_degree;
            // Copying an invalidation packet above clears next_reuse.
            fill_packet->next_reuse = access_packet->next_reuse;
            if (cachesim::L1_AMOEBA) {   // from memory: every word is available
                fill_packet->address = access_packet->address;
                fill_packet->present_mask = 0xff;
                fill_packet->clear_blocks();
            }
            cache[0]->handle_fill_line(fill_packet, eviction_packet);
            if (cachesim::L1_AMOEBA && cachesim::L1_AMOEBA_KEEP_LLC) {
                uint8_t rest = 0xff & ~Set::am_l1_last_line_words;
                if (rest) {
                    rest_of_line = amoeba_rest_of_line(*access_packet, rest, 0, 0);
                    keep_rest = true;
                }
            }
        }

        // Push the L1D's victim down through the levels below.
        auto push_down = [&]() {
            auto prev_cache_block_size = cache[0]->get_block_size(cache[0]->get_set_idx(eviction_packet->address));
            auto curr_cache_block_size = prev_cache_block_size;
            for (int i = 1; i < num_levels; i++) {
                curr_cache_block_size = cache[i]->get_block_size(cache[i]->get_set_idx(eviction_packet->address));

                if (prev_cache_block_size != curr_cache_block_size) {
                    resize_packet(eviction_packet, curr_cache_block_size);       
                }


                fill_packet->l1_hits = eviction_packet->l1_hits;
                if (fill_packet->l1_hits == 0) fill_packet->l1_hits = 1;
                if (eviction_packet->blocks.size() > 0 && cache[i]->get_level() != CacheLevel::LLC) {
                    // L1D victim into the L2: move it as is. The predictor only
                    // trains on, and predicts for, fills into the LLC.
                    *fill_packet = *eviction_packet;
                    eviction_packet->clear();
                    cache[i]->handle_fill_blocks(fill_packet, eviction_packet);
                } else if (eviction_packet->blocks.size() > 0) {
                    // Train on data movement from LLC -> L1D
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
                    cache[i]->handle_fill_blocks(fill_packet, eviction_packet);

                    if (eviction_packet->blocks.size() > 0) {
                        predictor->update_eviction(eviction_packet);
                    }
                } else {
                    break;
                }
                // eviction_packet now holds this level's victim, which the next
                // iteration fills into the level below - do not clear it here.
                fill_packet->clear();
                fill_packet->clear_pc();
                prev_cache_block_size = curr_cache_block_size;
            }
        };
        if (cachesim::L1_AMOEBA && eviction_packet->blocks.size() > 0) {
            // One push per victim line (an Amoeba L1D can evict several).
            for (const auto& victim : split_amoeba_victims(*eviction_packet)) {
                *eviction_packet = victim;
                push_down();
            }
        } else {
            push_down();
        }
        if (keep_rest) {
            Set::am_l1_kept_lines++;
            Set::am_l1_kept_words += __builtin_popcount(rest_of_line.present_mask);
            *eviction_packet = rest_of_line;
            push_down();
        }
    }

    count_pc_hub(pc, degree, avg_degree, hit_at_level);

    // L1D hit: we assume the CPU has an IPC of 1 on cache hit, already
    // accounted for in processCPU. Otherwise charge each level the access
    // reached: L2 (if present), then LLC, then memory.
    if (hit_at_level != 0) {
        const bool served_by_l2 = hit_at_level < num_levels && cache[hit_at_level]->get_level() == CacheLevel::L2;
        const bool served_by_llc = hit_at_level < num_levels && cache[hit_at_level]->get_level() == CacheLevel::LLC;
        if (num_levels > 2)
            PerformanceModel::processL2(fill_packet->address, served_by_l2, false);
        uint64_t dram_cycles = 0;
        if (served_by_llc) {
            PerformanceModel::processLLC(fill_packet->address, true, false);
        } else if (!served_by_l2) {
            PerformanceModel::processLLC(fill_packet->address, false, false);
            uint64_t before = PerformanceModel::getCycles();
            PerformanceModel::processMemory(fill_packet->address);
            dram_cycles = PerformanceModel::getCycles() - before;
        }
        // P-OPT: the next-ref engine's reads of the pinned columns for this access's LLC replacements. The
        // exclusive LLC is filled only by L1D victims (writebacks), so the engine is off the critical path:
        // counted, not charged (paper V-C hides it even on demand fills, behind the DRAM fetch).
        (void)dram_cycles;
        if (POPT::active) POPT::charge_engine(dram_cycles, true);
    }
    return hit_at_level;
}
#else
int access_single_level(std::vector<BaseCache*> &cache,
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
        cache[0]->handle_fill_line(fill_packet, eviction_packet);
    }
    count_pc_hub(pc, degree, avg_degree, hit ? 0 : 1);
    return hit ? 0 : 1;
}
#endif

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
// signal currently favors (true = favor breakdown, false = favor 64B), for
// both PSEL and the z-test modes below. should_merge() reads it directly -
// once the cache is already broken down (is_broken_down), a reversion to
// duel_locked == false is what triggers merge() back to 64B.
template<typename T>
void Cache<T>::update_dueling_confidence() {
    if (cachesim::DUELING_MODE == DuelingMode::PSEL) {
        // Hysteresis band [THRESHOLD - H, THRESHOLD + H): break down only
        // below it, merge back only at or above it, and otherwise keep the
        // current state, so PSEL hovering near the threshold does not flip
        // (and flush) the follower sets. H = 0 is the plain threshold check.
        if (cachesim::instCount >= cachesim::DUELING_PERIOD) {
            const uint64_t h = cachesim::PSEL_HYSTERESIS;
            if (!duel_locked && PSEL + h < cachesim::PSEL_THRESHOLD)
                duel_locked = true;
            else if (duel_locked && PSEL >= cachesim::PSEL_THRESHOLD + h)
                duel_locked = false;
        } else {
            duel_locked = false;
        }
        return;
    }

    //ZTEST CONFIDENCE
    if (cachesim::instCount - conf_last_check < cachesim::CONF_EPOCH)
        return;
    conf_last_check = cachesim::instCount;

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

namespace {
constexpr int PC_HUB_MAX_LEVELS = 3;
struct PcHubCounts {
    uint64_t acc[3] = {};                       // regular, hub, non-hub
    uint64_t miss[3][PC_HUB_MAX_LEVELS] = {};   // [class][level]
};
std::unordered_map<uint64_t, PcHubCounts> pc_hub;
}

void count_pc_hub(uint64_t pc, uint64_t degree, float avg_degree, int hit_at_level) {
    int c = avg_degree == 0.0f ? 0 : (degree > uint64_t(avg_degree) ? 1 : 2);
    auto& e = pc_hub[pc];
    e.acc[c]++;
    for (int l = 0; l < std::min(hit_at_level, PC_HUB_MAX_LEVELS); l++)
        e.miss[c][l]++;
}

void print_pc_hub_stats(const std::vector<std::string>& level_names, uint64_t top_n) {
    static const char* names[3] = {"regular", "hub", "non-hub"};
    const int levels = std::min<int>(level_names.size(), PC_HUB_MAX_LEVELS);
    std::vector<std::pair<uint64_t, PcHubCounts>> v(pc_hub.begin(), pc_hub.end());
    auto total = [](const PcHubCounts& e) { return e.acc[0] + e.acc[1] + e.acc[2]; };
    std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return total(a.second) > total(b.second); });
    PcHubCounts all, rest;
    uint64_t grand = 0;
    for (const auto& [pc, e] : v) grand += total(e);
    auto add = [&](PcHubCounts& d, const PcHubCounts& e) {
        for (int c = 0; c < 3; c++) {
            d.acc[c] += e.acc[c];
            for (int l = 0; l < PC_HUB_MAX_LEVELS; l++) d.miss[c][l] += e.miss[c][l];
        }
    };
    auto line = [&](const std::string& label, const PcHubCounts& e) {
        uint64_t t = total(e);
        std::string out = fmt::format("{} accesses {} ({:.1f}% of all)", label, t, grand ? 100.0 * t / grand : 0.0);
        for (int c = 0; c < 3; c++) {
            if (!e.acc[c]) continue;
            out += fmt::format(" | {} {} ({:.1f}%)", names[c], e.acc[c], 100.0 * e.acc[c] / t);
            for (int l = 0; l < levels; l++)
                out += fmt::format(" {} miss {}", level_names[l], e.miss[c][l]);
        }
        fmt::print("{}\n", out);
    };
    fmt::print("PC hub stats: {} PCs, top {} by accesses (regular = PCAvgDegree 0, hub = NodeDegree > PCAvgDegree)\n",
               v.size(), std::min<uint64_t>(top_n, v.size()));
    for (size_t i = 0; i < v.size(); i++) {
        add(all, v[i].second);
        if (i < top_n) line(fmt::format("PC {:#x}", v[i].first), v[i].second);
        else add(rest, v[i].second);
    }
    if (v.size() > top_n) line(fmt::format("PC other ({} PCs)", v.size() - top_n), rest);
    line("PC all", all);
}
