#include "cachesim.h"
#include "msl/bits.h"
#include <cassert>

bool SectoredSet::get_footprint(uint64_t way_idx, uint64_t word_idx) {
    uint64_t idx = way_idx*(block_size) + word_idx;
    return footprint[idx];
}

void SectoredSet::set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed) {
    uint64_t idx = way_idx*(block_size) + word_idx;
    footprint[idx] = accessed;
    return;
}

void SectoredSet::fill_way(PacketPtr packet, uint64_t way_idx) {
}

void SectoredSet::fill_packet(PacketPtr packet, uint64_t way_idx) {
        packet->pc = pc[way_idx];
        packet->next_reuse = next_reuse[way_idx];
        packet->serviced_from_llc = serviced_from_llc[way_idx];
        packet->is_hub_node = is_hub_node[way_idx];
        packet->l1_hits = way_hits[way_idx];
        packet->aligned_address = align_address(packet->address, CACHELINE_SIZE);
        packet->is_hub_node = is_hub_node[way_idx];
        packet->degree = degree[way_idx];
        packet->avg_degree = avg_degree[way_idx];

        packet->blocks = way_sectors[way_idx].sectors;
        packet->block_degrees = way_sectors[way_idx].degree;
        packet->llc_counter_values = way_sectors[way_idx].llc_counter_values;
}

void SectoredSet::invalidate_way(uint64_t way_idx) {
    ways[way_idx] = UINT64_MAX;
    valid[way_idx] = false;
    serviced_from_llc[way_idx] = 0;
    is_hub_node[way_idx] = false;
    dirty[way_idx] = false;
    pc[way_idx] = UINT64_MAX;
    next_reuse[way_idx] = UINT64_MAX;
    way_hits[way_idx] = 0;
    degree[way_idx] = 0;
    avg_degree[way_idx] = 0.0f;
    way_sectors[way_idx].invalidate();
    repl_counter->evict(way_idx);
}


bool SectoredSet::try_hit(PacketPtr packet) {
    bool hit = false;
    bool sector_hit = false;
    uint64_t hit_counter = UINT64_MAX;
    std::vector<uint64_t> sector_idx_list;
    auto way = std::find(ways.begin(), ways.end(), packet->aligned_address);
    auto way_idx = std::distance(ways.begin(), way);
    auto way_sector = &way_sectors[way_idx];
    hit = (way != ways.end());
    
    if (hit) {
        hit = (std::find(way_sector->sectors.begin(), way_sector->sectors.end(), align_address(packet->address, block_size)) != way_sector->sectors.end());
        for (const auto block: packet->blocks) {
            auto sector = std::find(way_sector->sectors.begin(), way_sector->sectors.end(), block);
            auto sector_idx = std::distance(way_sector->sectors.begin(), sector);
            //if (repl_counter[way_idx] < hit_counter) hit_counter = repl_counter[way_idx];
    
            sector_hit = (sector != way_sector->sectors.end());// && (valid[way_idx] == true);
    
            if (sector_hit) {
                sector_idx_list.push_back(sector_idx);
            } else {
                //break;
            }
    
        }
        if (cachesim::DEBUG || cachesim::L1_DEBUG)
            fmt::print("{} level {} hit {} pc {:#x} address {:#x} set {} way {} size {} degree {} avg_degree {:4f}\n",
                    __func__, level, (hit) ? "HIT" : "MISS", packet->pc, packet->address, set_idx, way_idx, packet->blocks.size(), packet->degree, packet->avg_degree);
    } else {
        if (cachesim::DEBUG || cachesim::L1_DEBUG)
            fmt::print("{} level {} hit MISS pc {:#x} address {:#x} sector_address {:#x} set {} way {} no sectors available size {} degree {} avg_degree {:4f}\n",
                    __func__, level, packet->pc, packet->aligned_address, packet->address, set_idx, way_idx, packet->size, packet->degree, packet->avg_degree);
    }

    cache->incr_partial_misses(packet->blocks.size() - sector_idx_list.size());
    accesses++;

    if (hit) {
        repl_counter->init_counter(packet);

        repl_counter->hit_update(packet, way_idx);
        if (next_reuse[way_idx] != UINT64_MAX) next_reuse[way_idx] = packet->next_reuse;

        for (auto sector_idx: sector_idx_list) {
            if (!packet->is_read) {
                dirty[sector_idx] = true;
            }
            if (degree[sector_idx] < packet->degree)
                degree[sector_idx] = packet->degree;

        }
        auto word_idx = (packet->address - packet->aligned_address) >> 3;
        set_footprint(way_idx, word_idx, true);
        way_hits[way_idx]++;
        //set_block_accesses(way_idx, word_idx, 
        //        get_block_accesses(way_idx, word_idx)+1);
        hits++;
    }

    return hit;
}

void SectoredSet::handle_fill(PacketPtr packet) {
    auto way = std::find(valid.begin(), valid.end(), false);
    auto way_idx = std::distance(valid.begin(), way);
    auto hit = false;
    if (way_idx == num_ways) {
        hit = (std::find(ways.begin(), ways.end(), packet->aligned_address) != ways.end());
    }
    assert(hit || (way_idx < num_ways));
    repl_counter->init_counter(packet);
    valid[way_idx] = true;
    ways[way_idx] = packet->aligned_address;
    //TODO: Is this required?
    pc[way_idx] = packet->pc;
    next_reuse[way_idx] = packet->next_reuse;
    serviced_from_llc[way_idx] += packet->serviced_from_llc;
    if (!is_hub_node[way_idx])
        is_hub_node[way_idx] = packet->is_hub_node;
    if (degree[way_idx] < packet->degree)
        degree[way_idx] = packet->degree;
    avg_degree[way_idx] = packet->avg_degree;

    repl_counter->fill_update(way_idx, 0, packet, true);
    auto way_sector = &way_sectors[way_idx];

    uint64_t sector_idx = 0;
    for (const auto block_address: packet->blocks) {
        if (packet->blocks.size() > 8) {
            packet->print();
            fmt::print("block {:#x}\n",block_address);
        }
        if (sector_idx <0 || sector_idx >= 8)
            fmt::print("sector {} length {}\n", sector_idx, packet->blocks.size());
        assert(sector_idx >=0 && sector_idx < 8);
        if (way_sector->valid.size() != 8) {
            fmt::print("{}, {}\n", sector_idx, way_sector->valid.size());
        }
        assert(way_sector->valid.size() == 8);
        if (way_sector->valid[sector_idx]) {
            assert(way_sector->sectors[sector_idx] == block_address);
            sector_idx++;
            continue;
        }
        if (block_address != UINT64_MAX) {
            way_sector->sectors[sector_idx] = block_address;
            way_sector->valid[sector_idx] = true;
            if (packet->block_degrees.size() == 0) {
                way_sector->degree[sector_idx] = packet->degree;
            } else {
                way_sector->degree[sector_idx] = packet->block_degrees[sector_idx];
                way_sector->llc_counter_values[sector_idx] = packet->llc_counter_values[sector_idx];
            }
            way_sector->avg_degree[sector_idx] = packet->avg_degree;
            //fmt::print("Level 0 Inserting address {:#x} sector {} counter {} serviced_from_llc {} vector size {} val {}\n",
             //       way_sectors[way_idx].sectors[sector_idx], sector_idx, way_sectors[way_idx].llc_counter_values[sector_idx], serviced_from_llc[way_idx], packet->llc_counter_values.size(), packet->llc_counter_values[sector_idx]);
        }
        auto was_accessed = ((packet->footprint >> sector_idx*bits_per_block)&bitmask == bitmask);
        was_accessed |= (align_address(packet->address, block_size) == block_address);
        for (auto word_idx = sector_idx; word_idx < sector_idx + bits_per_block; word_idx++) {
            if (was_accessed) {
                set_footprint(way_idx, word_idx, true);
            }
        }
        sector_idx++;
    }
    return;
}

void SectoredSet::handle_evict(PacketPtr packet) {
    auto try_hit = std::find(ways.begin(), ways.end(), packet->aligned_address);
    auto num_invalid_blocks = get_num_invalid();
    bool hit = (try_hit != ways.end());
    bool eviction_needed = (num_invalid_blocks == 0) && (hit != true);
    packet->footprint = 0;
    packet->degree = 0;
    packet->avg_degree = 0.0f;
    if (!eviction_needed) {
        if (hit) {
            if (cachesim::DEBUG || cachesim::L1_DEBUG)
                fmt::print("Level {} No eviction needed for set {} because some sectors of address {:#x} aligned address {:#x} are already present at way {}\n",
                        level, set_idx, packet->address,  packet->aligned_address, std::distance(ways.begin(), try_hit));

        } else {
            if (cachesim::DEBUG || cachesim::L1_DEBUG)
                fmt::print("Level {} No eviction needed for set {} because there are {} invalid ways\n", level, set_idx, get_num_invalid());
        }
        return;
    }

    auto way_idx = repl_counter->get_eviction_candidate(false);

    if (dirty[way_idx]) {
        dirty[way_idx] = false;
    }

    for (uint64_t sector_idx = 0; sector_idx < num_blocks; sector_idx++) {
        //if (way_sectors[way_idx].valid[sector_idx]) {
            packet->blocks.push_back(way_sectors[way_idx].sectors[sector_idx]);
            packet->block_degrees.push_back(way_sectors[way_idx].degree[sector_idx]);
            packet->llc_counter_values.push_back(way_sectors[way_idx].llc_counter_values[sector_idx]);
            //fmt::print("Level 0 Evicting address {:#x} sector {} counter {} serviced_from_llc {} vector size {} val {}\n",
//                    way_sectors[way_idx].sectors[sector_idx], sector_idx, way_sectors[way_idx].llc_counter_values[sector_idx], serviced_from_llc[way_idx], packet->llc_counter_values.size(), packet->llc_counter_values[sector_idx]);


            way_sectors[way_idx].sectors[sector_idx] = UINT64_MAX;
            way_sectors[way_idx].valid[sector_idx] = false;
            way_sectors[way_idx].dirty[sector_idx] = false;
        //}
    }


    for (uint64_t sector_idx = 0; sector_idx < CACHELINE_SIZE/8; ++sector_idx) {
        //packet->footprint |= get_footprint(way_idx, sector_idx) << sector_idx
        packet->footprint |= (bitmask*get_footprint(way_idx, sector_idx) << sector_idx*bits_per_block);
        set_footprint(way_idx, sector_idx, false);

    }

    if (cachesim::DEBUG || cachesim::L1_DEBUG)
        fmt::print("Level {} Evicted set {} way {} address {:#x} dirty {} valid {} repl_counter {} pc {:#x} degree {} avg_degree {:4f}\n",
            level, set_idx, way_idx, ways[way_idx], (uint32_t)dirty[way_idx], (uint32_t)valid[way_idx], repl_counter->get_counter_value(way_idx),
            pc[way_idx], degree[way_idx], avg_degree[way_idx]);


    packet->pc = pc[way_idx];
    packet->next_reuse = next_reuse[way_idx];
    packet->serviced_from_llc = std::max(packet->serviced_from_llc, serviced_from_llc[way_idx]);
    packet->is_hub_node = is_hub_node[way_idx];
    packet->l1_hits = way_hits[way_idx];
    packet->degree = std::max(packet->degree, degree[way_idx]);
    packet->avg_degree = std::max(packet->avg_degree, avg_degree[way_idx]);

    invalidate_way(way_idx);

    cache->update_data_var_utilization(packet->pc, packet->blocks.size(),
            count_footprint(packet->footprint));
    cache->update_data_var_footprint(packet->pc, count_footprint(packet->footprint));

    if (cachesim::DEBUG || cachesim::L1_DEBUG)
        fmt::print("Level {} Num invalid blocks {} evicted line footprint {:#x} serviced_from_llc {}\n",
            level, num_invalid_blocks, packet->footprint, packet->serviced_from_llc);

    return;
}

void SectoredSet::handle_invalidate(PacketPtr packet, uint64_t block_num) {
    auto try_hit = std::find(ways.begin(), ways.end(), packet->aligned_address);
    bool hit = (try_hit != ways.end());
    //Sector inv_sector(num_blocks);
    uint64_t inv_address;
    if (hit) {
        auto way_idx = std::distance(ways.begin(), try_hit);
        auto previous_footprint = packet->footprint;
        for (uint64_t sector_idx = 0; sector_idx < num_blocks; sector_idx++) {
            packet->footprint |= (bitmask*get_footprint(way_idx, sector_idx)) << (block_num*bits_per_block);
            set_footprint(way_idx, sector_idx, false);
        }
        fill_packet(packet, way_idx);
        way_sectors[way_idx].invalidate();
        invalidate_way(way_idx);
        //cache->update_data_var_utilization(packet->pc, 1,
        //    count_footprint(packet->footprint)-count_footprint(previous_footprint));
        //cache->update_data_var_footprint(packet->pc,
        //    count_footprint(packet->footprint)-count_footprint(previous_footprint));

        if (cachesim::DEBUG || cachesim::L1_DEBUG) {
            fmt::print("Level {} Invalidated address {:#x} @ set {} way {} footprint {:#x} serviced_from_llc {} because of line promotion to higher level\n", level, packet->address, set_idx, way_idx, packet->footprint, packet->serviced_from_llc);
        }
    } else {
        packet->clear();
    }
}


