#include "cachesim.h"
#include "msl/bits.h"
#include <cassert>


bool Set::get_footprint(uint64_t way_idx, uint64_t word_idx) {
    uint64_t idx = way_idx*(block_size/8) + word_idx;
    return footprint[idx];
}



void Set::set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed) {
    uint64_t idx = way_idx*(block_size/8) + word_idx;
    footprint[idx] = accessed;
    return;
}

void Set::fill_way(PacketPtr /*packet*/, uint64_t /*way_idx*/) {
}

void Set::fill_packet(PacketPtr packet, uint64_t way_idx) {
    packet->pc = pc[way_idx];
    packet->next_reuse = next_reuse[way_idx];
    packet->serviced_from_llc = std::max(packet->serviced_from_llc, serviced_from_llc[way_idx]);
    packet->is_hub_node = is_hub_node[way_idx];
    packet->l1_hits = std::max(packet->l1_hits, way_hits[way_idx]);
    packet->degree = std::max(packet->degree, degree[way_idx]);
    if (valid[way_idx]) {
        packet->block_degrees.push_back(degree[way_idx]);
        packet->block_serviced_from_llc.push_back(serviced_from_llc[way_idx]);
    }
    packet->avg_degree = avg_degree[way_idx];
}

void Set::invalidate_way(uint64_t way_idx) {
    ways[way_idx] = UINT64_MAX;
    valid[way_idx] = false;
    dirty[way_idx] = false;

    pc[way_idx] = UINT64_MAX;
    serviced_from_llc[way_idx] = 0;
    is_hub_node[way_idx] = false;
    next_reuse[way_idx] = UINT64_MAX;
    way_hits[way_idx] = 0;
    degree[way_idx] = 0;
    avg_degree[way_idx] = 0.0f;
    repl_counter->evict(way_idx);
}

bool Set::try_hit(PacketPtr packet) {
    bool hit = true;
    uint64_t hit_counter = UINT64_MAX;
    std::vector<uint64_t> way_idx_list;
    for (const auto block: packet->blocks) {
        auto aligned_address = align_address(block, block_size);
        auto way = std::find(ways.begin(), ways.end(), aligned_address);
        auto way_idx = std::distance(ways.begin(), way);

        bool block_hit = (way != ways.end());// && (valid[way_idx] == true);
        hit &= block_hit;

        if (cachesim::DEBUG || cachesim::LLC_DEBUG)
            fmt::print("{} level {} hit {} align_address {:#x} address {:#x} set {} way {} pc {} size {} degree {} avg_degree {:4f} is_hub_node {} repl_counter {}\n",
                    __func__, level, (hit) ? "HIT" : "MISS", packet->aligned_address, aligned_address, set_idx, way_idx, pc[way_idx],
                    packet->blocks.size(), packet->degree, packet->avg_degree, (packet->degree > uint64_t(packet->avg_degree)), repl_counter->get_counter_value(way_idx));
        if (block_hit) {
            if (repl_counter->get_counter_value(way_idx) < hit_counter) hit_counter = repl_counter->get_counter_value(way_idx);
            way_idx_list.push_back(way_idx);
        } else {
            //break;
        }
    }

    accesses++;

    if (hit) {
        if (level == 0 || cache->get_insertion_policy() != InsertionPolicy::EXCLUSIVE)
            repl_counter->init_counter(packet);
        for (auto way_idx: way_idx_list) {
            if (!packet->is_read) {
                dirty[way_idx] = true;
            }
            auto word_idx = (packet->address - align_address(packet->address, block_size)) >> 3;
            //fmt::print("Aligned Address {:#x} Address {:#x} word_idx {}\n", packet->aligned_address, packet->address, word_idx);

            set_footprint(way_idx, word_idx, true);
            //TODO: TEMP
            packet->l1_hits = way_hits[way_idx]+1;
            repl_counter->hit_update(packet, way_idx);
            if (next_reuse[way_idx] != UINT64_MAX) next_reuse[way_idx] = packet->next_reuse;

            way_hits[way_idx]++;
            degree[way_idx] = std::max(degree[way_idx], packet->degree);

            if (ways[way_idx] == align_address(packet->address, block_size)) {
                serviced_from_llc[way_idx]++;
                //cache->update_data_var_hub_hits((degree[way_idx] > avg_degree[way_idx]), serviced_from_llc[way_idx]);
            }
        }

        hits++;
    } else {
        cache->incr_partial_misses(packet->blocks.size() - way_idx_list.size());
        switch(get_dueling_type()) {
            case SetDuelingType::Leader64:
                cache->decr_psel();
                if (cachesim::DEBUG)
                    fmt::print("decr psel {} set {} num_ways {} aligned_address {:#x} address {:#x} pc {:#x} size {} block_size {} num_hits {}\n", cache->get_psel(), set_idx, num_ways,  packet->aligned_address, packet->address, packet->pc, packet->size, block_size,  way_idx_list.size());
                break;
            case SetDuelingType::Leader8:
                cache->incr_psel();
                if (cachesim::DEBUG)
                  fmt::print("incr psel {} set {} num_ways {} aligned_address {:#x} address {:#x} pc {:#x} size {} block_size {} num_hits {}\n", cache->get_psel(), set_idx, num_ways,  packet->aligned_address, packet->address, packet->pc, packet->size, block_size,  way_idx_list.size());
                break;
            default:
                break;
        }

    }

    return hit;
}



void Set::handle_fill(PacketPtr packet) {
    auto way = valid.begin();
    repl_counter->init_counter(packet);
    uint64_t block_idx = 0;

    for (const auto block_address: packet->blocks) {
        bool was_accessed;
        if (level == 0) {
            was_accessed = (block_address == align_address(packet->address, block_size)); 
        } else {
            was_accessed = ((packet->footprint >> (block_idx*bits_per_block)&bitmask) == bitmask);
        }
        if (packet->serviced_from_llc > 0) {
            packet->is_hub_node = packet->block_degrees[block_idx] > uint64_t(packet->avg_degree);
            //packet->is_hub_node = packet->degree > uint64_t(packet->avg_degree);
        } else {
            packet->is_hub_node = packet->degree > uint64_t(packet->avg_degree);
        }
        //packet->address = block_address;
        way = std::find(way, valid.end(), false);
        auto way_idx = std::distance(valid.begin(), way);

        //if (packet->serviced_from_llc > 0) {
        //    fmt::print("Filling block address {:#x} from set {} block {} at level {}. was_accessed {} packet degree {} block degree {} block list size {} serviced_from_llc {}\n", block_address, set_idx, way_idx, level, was_accessed, packet->degree, packet->block_degrees[block_idx], packet->block_degrees.size(), packet->serviced_from_llc);
        //}

        repl_counter->fill_update(way_idx, block_idx, packet, was_accessed);
        if (block_address != UINT64_MAX) {
            ways[way_idx] = block_address;
        }
        valid[way_idx] = true;
        pc[way_idx] = packet->pc;
        next_reuse[way_idx] = packet->next_reuse;
        serviced_from_llc[way_idx] += packet->block_serviced_from_llc[block_idx];
        //serviced_from_llc[way_idx] += packet->serviced_from_llc;
        is_hub_node[way_idx] = packet->is_hub_node;
        way_hits[way_idx] = packet->l1_hits;
        if (degree[way_idx] < packet->block_degrees[block_idx])
            degree[way_idx] = packet->block_degrees[block_idx];
        avg_degree[way_idx] = packet->avg_degree;

        for (uint64_t word_idx = 0; word_idx < bits_per_block; word_idx++) {
            bool was_accessed = ((packet->footprint >> (word_idx*bits_per_block)&bitmask) == bitmask);
            if (was_accessed) {
                set_footprint(way_idx, word_idx, true);
            }
        }

        if (cachesim::DEBUG || cachesim::LLC_DEBUG)
            fmt::print("Level {} Inserting address {:#x} @ set {} way {} blk_size {} packet size {} total ways {} valid {} repl_counter {} pc {:#x} block_idx {} was_accessed {} set_footprint {} serviced_from_llc {} packet footprint {:#x} degree {} avg_degree {:4f} is_hub_node {}\n",
                    level, ways[way_idx], set_idx, way_idx, block_size, packet->size, num_ways, (uint32_t)valid[way_idx], repl_counter->get_counter_value(way_idx), pc[way_idx], block_idx, was_accessed, get_footprint(way_idx, 0), serviced_from_llc[way_idx],
                    packet->footprint, packet->degree, packet->avg_degree, packet->degree > uint64_t(packet->avg_degree));

        block_idx++;
    }
    return;
}



void Set::handle_evict(PacketPtr packet) {
    uint64_t num_blocks_evicted = 0;
    auto num_invalid_blocks = get_num_invalid();
    uint64_t num_blocks_to_evict = packet->size/block_size;
    if (cachesim::DEBUG || cachesim::LLC_DEBUG)
        fmt::print("Level {} Num invalid blocks {} Evicted blocks {} packet size {}\n",
            level, num_invalid_blocks, num_blocks_evicted, packet->size);

    if (!is_eviction_needed(num_blocks_to_evict)) {
        if (cachesim::DEBUG || cachesim::LLC_DEBUG)
            fmt::print("Level {} No eviction needed for set {} because there are {} invalid ways\n", level, set_idx, std::count(valid.begin(), valid.end(), false));
        return;
    }


    num_blocks_to_evict -= num_invalid_blocks;
    while (num_blocks_evicted < num_blocks_to_evict) {
        uint64_t way_idx = num_ways;
        way_idx = repl_counter->get_eviction_candidate(packet->degree < (uint64_t)packet->avg_degree);

        if (dirty[way_idx]) {
            dirty[way_idx] = false;
        }

        if (valid[way_idx]) {
            packet->blocks.push_back(ways[way_idx]);
        }

        if (repl_counter->get_reserved_ways() != 0) {
            if (get_footprint(way_idx, 0) == 1 && is_hub_node[way_idx]) {
                auto num_reserved_ways = repl_counter->get_reserved_ways();
                auto way = std::find(valid.begin()+num_ways-num_reserved_ways, valid.end(), false);
                uint64_t dist_way_idx = num_ways-num_reserved_ways;
                if (way != valid.end()) {
                    dist_way_idx = std::distance(valid.begin(), way);
                } else {
                    dist_way_idx = repl_counter->get_reserved_eviction_candidate(false);

                }
                ways[dist_way_idx] = ways[way_idx];
                pc[dist_way_idx] = pc[way_idx];
                valid[dist_way_idx] = true;
                serviced_from_llc[dist_way_idx] = serviced_from_llc[way_idx];
                is_hub_node[dist_way_idx] = is_hub_node[way_idx];
                way_hits[dist_way_idx] = way_hits[way_idx];
                set_footprint(dist_way_idx, 0, true);
                Packet pkt;
                pkt.serviced_from_llc = serviced_from_llc[dist_way_idx];
                pkt.is_hub_node = is_hub_node[way_idx];
                repl_counter->fill_update(dist_way_idx, 0, &pkt, true);
            }
        }

        for (uint64_t i = 0; i < block_size/8; ++i) {
            packet->footprint |= get_footprint(way_idx, i) << (i + num_blocks_evicted*(block_size/8));
            if (cachesim::DEBUG || cachesim::LLC_DEBUG)
                fmt::print("Level {} Evicted set {} way {} address {:#x} dirty {} valid {} repl_counter {} pc {:#x} num_blocks_to_evict {} packet footprint {:#x} word_idx {} get_footprint {} serviced_from_llc {} num_blocks_evicted {} degree {} avg_degree {:4f} repl_counter {}\n",
                    level, set_idx, way_idx, ways[way_idx], (uint32_t)dirty[way_idx], (uint32_t)valid[way_idx], repl_counter->get_counter_value(way_idx),
                    pc[way_idx], num_blocks_to_evict, packet->footprint,
                    i, get_footprint(way_idx, i), serviced_from_llc[way_idx], num_blocks_evicted,
                    degree[way_idx], avg_degree[way_idx], repl_counter->get_counter_value(way_idx));

            set_footprint(way_idx, i, false);
            //set_block_accesses(way_idx, i, 0);
        }

        //fmt::print("PC {} way {}\n", pc[way_idx], way_idx);
        fill_packet(packet, way_idx);

        //if (degree[way_idx] != 0)
        //    fmt::print("Degree {} Avg {:4f}\n", degree[way_idx], avg_degree[way_idx]);
        cache->update_data_var_hub_evictions((degree[way_idx] > avg_degree[way_idx]), serviced_from_llc[way_idx]);

        invalidate_way(way_idx);
        num_blocks_evicted++;
        //cache->update_data_var_evictions(packet->pc,
        //    count_footprint(packet->footprint)-count_footprint(previous_footprint));

    }



    if (cachesim::DEBUG || cachesim::LLC_DEBUG)
        fmt::print("Level {} Num invalid blocks {} evicted blocks {} evicted line footprint {:#x}\n",
            level, num_invalid_blocks, num_blocks_evicted, packet->footprint);

    return;
}

void Set::handle_invalidate(PacketPtr packet, uint64_t block_num) {
    auto try_hit = std::find(ways.begin(), ways.end(), packet->address);
    uint64_t inv_address = UINT64_MAX;
    if (try_hit != ways.end()) {
        auto way_idx = std::distance(ways.begin(), try_hit);
        //serviced_from_llc[way_idx]++;
        inv_address = ways[way_idx];
        //auto previous_footprint = packet->footprint;
        packet->footprint |= (bitmask*get_footprint(way_idx, 0)) << (block_num*bits_per_block);

        fill_packet(packet, way_idx);
        invalidate_way(way_idx);
        set_footprint(way_idx, 0, false);

        //cache->update_data_var_invalidations(packet->pc,
        //    count_footprint(packet->footprint)-count_footprint(previous_footprint));
        if (cachesim::DEBUG || cachesim::LLC_DEBUG) {
            fmt::print("Level {} Invalidated address {:#x} @ set {} way {} footprint {:#x} because of line promotion to higher level\n", level, packet->address, set_idx, way_idx, packet->footprint);
        }
        packet->blocks.push_back(inv_address);
    } else {
        packet->blocks.push_back(UINT64_MAX);
        packet->block_degrees.push_back(0);
        packet->block_serviced_from_llc.push_back(0);
    }
}

template<typename VecType>
void Set::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool incr) {
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            if (incr)
                vec[baseIdx + j] = val + (block_size/scale_factor)*j;
            else
                vec[baseIdx + j] = val;
        }
    }
}

void Set::set_breakdown(uint64_t new_block_size) {
    assert(new_block_size < block_size);

    uint64_t scale_factor = block_size/new_block_size;
    uint64_t new_num_ways = num_ways*scale_factor;
    ways.resize(new_num_ways);

    breakdown(ways, num_ways, scale_factor, true);
    num_ways = new_num_ways;
    block_size = new_block_size;
}
