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

uint64_t Set::make_tag(uint64_t address) const {
    if (address == UINT64_MAX)
        return INVALID_TAG;
    uint64_t index_shift = champsim::msl::lg2(CACHELINE_SIZE) + champsim::msl::lg2(cache->get_num_sets());
    uint64_t sector_bits = champsim::msl::lg2(CACHELINE_SIZE) - champsim::msl::lg2(block_size);
    uint64_t sector = (address >> champsim::msl::lg2(block_size)) & ((1ULL << sector_bits) - 1);
    return ((address >> index_shift) << sector_bits) | sector;
}

void Set::set_way_address(uint64_t way_idx, uint64_t address) {
    // A tag only identifies a block within its own set, and only at block
    // granularity - both hold for every address a Set stores today.
    assert(address == UINT64_MAX || align_address(address, block_size) == address);
    assert(address == UINT64_MAX ||
           ((address >> champsim::msl::lg2(CACHELINE_SIZE)) & (cache->get_num_sets() - 1)) == set_idx);
    bdi_release(way_idx);
    ways[way_idx] = address;
    tags[way_idx] = make_tag(address);
    if (bdi_active() && tags[way_idx] != INVALID_TAG)
        bdi_encode(way_idx, true);
}

uint64_t Set::find_way(uint64_t aligned_address) const {
    auto tag = make_tag(aligned_address);
    if (tag == INVALID_TAG)
        return tags.size();
    uint64_t way_idx;
    if (bdi_active()) {
        way_idx = find_way_bdi(tag);
    } else {
        way_idx = std::distance(tags.begin(), std::find(tags.begin(), tags.end(), tag));
    }
#ifdef TAG_CHECK
    // Verification build: the lookup must find exactly what a full-tag lookup
    // and an address lookup find.
    auto by_tag = std::distance(tags.begin(), std::find(tags.begin(), tags.end(), tag));
    auto by_addr = std::distance(ways.begin(), std::find(ways.begin(), ways.end(), aligned_address));
    assert((uint64_t)by_tag == way_idx);
    assert((uint64_t)by_addr == way_idx);
#endif
    return way_idx;
}

uint64_t Set::sector_bits() const {
    return champsim::msl::lg2(CACHELINE_SIZE) - champsim::msl::lg2(block_size);
}

static bool bdi_fits(int64_t delta) {
    int64_t half = 1LL << (cachesim::TAG_BDI_DELTA - 1);
    return delta >= -half && delta < half;
}

void Set::bdi_reset() {
    bdi_bases.assign(cachesim::TAG_BDI_BASES, INVALID_TAG);
    bdi_refs.assign(cachesim::TAG_BDI_BASES, 0);
    bdi_base.assign(num_ways, -1);
    bdi_delta.assign(num_ways, 0);
    bdi_sector.assign(num_ways, 0);
}

void Set::bdi_release(uint64_t way_idx) {
    if (way_idx >= bdi_base.size() || bdi_base[way_idx] < 0)
        return;
    auto k = bdi_base[way_idx];
    assert(bdi_refs[k] > 0);
    if (cachesim::DEBUG_TAG_BDI)
        fmt::print("[{}] BDI release  set {} way {:3} addr {:#x} base {} refs {} -> {}{}\n", cachesim::instCount, set_idx, way_idx,
            ways[way_idx], k, bdi_refs[k], bdi_refs[k] - 1, bdi_refs[k] == 1 ? " (base freed)" : "");
    if (--bdi_refs[k] == 0) {
        bdi_bases[k] = INVALID_TAG;
        bdi_stat_frees++;
    }
    bdi_base[way_idx] = -1;
}

// Encode way_idx's tag against this set's bases: first base whose signed
// TAG_BDI_DELTA-bit window holds the line tag; else install the line tag in a
// free slot; else overflow (kept with its full tag, bdi_base = -1).
void Set::bdi_encode(uint64_t way_idx, bool count) {
    assert(bdi_base[way_idx] < 0 && tags[way_idx] != INVALID_TAG);
    uint64_t line = tags[way_idx] >> sector_bits();
    int32_t k = -1;
    for (size_t i = 0; i < bdi_bases.size() && k < 0; i++)
        if (bdi_bases[i] != INVALID_TAG && bdi_fits((int64_t)(line - bdi_bases[i])))
            k = (int32_t)i;
    if (k < 0) {
        for (size_t i = 0; i < bdi_bases.size() && k < 0; i++)
            if (bdi_bases[i] == INVALID_TAG) {
                k = (int32_t)i;
                bdi_bases[i] = line;
                bdi_stat_installs++;   // always: every install is later matched by a free
                if (cachesim::DEBUG_TAG_BDI)
                    fmt::print("[{}] BDI install  set {} base {} = line {:#x} (window {:#x}..{:#x})\n", cachesim::instCount, set_idx, i, line,
                        line - (1ULL << (cachesim::TAG_BDI_DELTA - 1)), line + (1ULL << (cachesim::TAG_BDI_DELTA - 1)) - 1);
            }
    }
    if (k < 0) {
        if (count) bdi_stat_overflow++;
        // With the admission constraint, bdi_reserve() guarantees a base fits
        // or a slot is free before every fill.
        assert(cachesim::TAG_BDI_UNCONSTRAINED);
        return;
    }
    bdi_base[way_idx] = k;
    bdi_delta[way_idx] = (int64_t)(line - bdi_bases[k]);
    bdi_sector[way_idx] = (uint8_t)(tags[way_idx] & ((1ULL << sector_bits()) - 1));
    bdi_refs[k]++;
    if (count) bdi_stat_encoded++;
    if (cachesim::DEBUG_TAG_BDI)
        fmt::print("[{}] BDI encode   set {} way {:3} addr {:#x} tag {:#x} = line {:#x} sector {} -> base {} ({:#x}) delta {:+} refs {}\n",
            cachesim::instCount, set_idx, way_idx, ways[way_idx], tags[way_idx], line, (unsigned)bdi_sector[way_idx],
            k, bdi_bases[k], bdi_delta[way_idx], bdi_refs[k]);
#ifdef TAG_CHECK
    assert((((bdi_bases[k] + (uint64_t)bdi_delta[way_idx]) << sector_bits()) | bdi_sector[way_idx]) == tags[way_idx]);
#endif
}

void Set::bdi_rebuild() {
    bdi_reset();
    if (!bdi_active())
        return;
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++)
        if (tags[way_idx] != INVALID_TAG)
            bdi_encode(way_idx, false);
}

// Hardware view: subtract the incoming line tag from every base in parallel,
// keep the bases whose delta fits, then match (base id, delta, sector) across
// the ways; overflow ways are matched on their full tag. Returns the lowest
// matching way, like the plain tag search.
uint64_t Set::find_way_bdi(uint64_t tag) const {
    uint64_t line = tag >> sector_bits();
    uint8_t sector = (uint8_t)(tag & ((1ULL << sector_bits()) - 1));
    int64_t deltas[64];
    bool fits[64];
    assert(bdi_bases.size() <= 64);
    for (size_t k = 0; k < bdi_bases.size(); k++) {
        deltas[k] = (int64_t)(line - bdi_bases[k]);
        fits[k] = bdi_bases[k] != INVALID_TAG && bdi_fits(deltas[k]);
    }
    uint64_t found = tags.size();
    for (uint64_t way_idx = 0; way_idx < tags.size() && found == tags.size(); way_idx++) {
        auto k = bdi_base[way_idx];
        if (k >= 0) {
            if (fits[k] && bdi_delta[way_idx] == deltas[k] && bdi_sector[way_idx] == sector)
                found = way_idx;
        } else if (tags[way_idx] == tag) {
            found = way_idx;
        }
    }
    if (cachesim::DEBUG_TAG_BDI) {
        std::string cand;
        for (size_t k = 0; k < bdi_bases.size(); k++)
            if (fits[k]) cand += fmt::format(" b{}:{:+}", k, deltas[k]);
        if (found < tags.size())
            fmt::print("[{}] BDI lookup   set {} tag {:#x} = line {:#x} sector {} | fitting bases{} -> HIT way {} (base {} delta {:+})\n",
                cachesim::instCount, set_idx, tag, line, (unsigned)sector, cand.empty() ? " none" : cand, found, bdi_base[found], bdi_delta[found]);
        else
            fmt::print("[{}] BDI lookup   set {} tag {:#x} = line {:#x} sector {} | fitting bases{} -> MISS\n",
                cachesim::instCount, set_idx, tag, line, (unsigned)sector, cand.empty() ? " none" : cand);
    }
    return found;
}

uint64_t Set::bdi_reserve(PacketPtr fill_packet, uint64_t& used_words) {
    if (!bdi_active() || cachesim::TAG_BDI_UNCONSTRAINED || fill_packet->address == UINT64_MAX)
        return 0;
    // A fill packet carries the blocks of one 64B line, so one line tag.
    uint64_t line = make_tag(align_address(fill_packet->address, block_size)) >> sector_bits();
    int32_t victim = -1;
    for (size_t k = 0; k < bdi_bases.size(); k++) {
        if (bdi_bases[k] == INVALID_TAG || bdi_fits((int64_t)(line - bdi_bases[k])))
            return 0;   // fits a base, or a free slot will take it
        if (victim < 0 || bdi_refs[k] < bdi_refs[victim])
            victim = (int32_t)k;
    }
    // No base fits and no slot is free: replace the base with the fewest users
    // (lowest slot on a tie) by evicting all of its ways. The last release
    // frees the slot, so the fill's first block installs the new base.
    bdi_stat_replacements++;
    uint64_t evicted = 0;
    if (cachesim::DEBUG_TAG_BDI)
        fmt::print("[{}] BDI replace  set {} line {:#x} fits no base: replacing base {} ({:#x}, {} users)\n",
            cachesim::instCount, set_idx, line, victim, bdi_bases[victim], bdi_refs[victim]);
    for (uint64_t way_idx = 0; way_idx < num_ways && bdi_bases[victim] != INVALID_TAG; way_idx++) {
        if (bdi_base[way_idx] != victim) continue;
        Packet scratch;   // own packet: a whole base can exceed the 64-bit footprint of one
        evict_way_into(&scratch, way_idx, 0, 1);
        used_words += count_footprint(scratch.footprint);
        evicted++;
    }
    assert(bdi_bases[victim] == INVALID_TAG);
    bdi_stat_replaced_blocks += evicted;
    return evicted;
}

void Set::invalidate_all_ways() {
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++) {
        invalidate_way(way_idx);
        for (uint64_t word_idx = 0; word_idx < block_size / 8; word_idx++)
            set_footprint(way_idx, word_idx, false);
    }
}

void Set::bdi_resident(uint64_t& encoded, uint64_t& overflow) const {
    if (!bdi_active())   // 64B sets (leaders, merged followers) have no bases
        return;
    for (uint64_t way_idx = 0; way_idx < tags.size(); way_idx++) {
        if (tags[way_idx] == INVALID_TAG) continue;
        if (bdi_base[way_idx] >= 0) encoded++; else overflow++;
    }
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
        packet->block_prev_counter.push_back(repl_counter->get_counter_value(way_idx));
    }
    packet->avg_degree = avg_degree[way_idx];
}

void Set::invalidate_way(uint64_t way_idx) {
    if (amoeba_active())
        amoeba_release_way(way_idx);
    set_way_address(way_idx, UINT64_MAX);
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
        auto way_idx = find_way(aligned_address);

        bool block_hit = (way_idx < tags.size());// && (valid[way_idx] == true);
        hit &= block_hit;

        //if (cachesim::DEBUG || cachesim::LLC_DEBUG)
            //fmt::print("{} level {} hit {} align_address {:#x} address {:#x} set {} way {} pc {} size {} degree {} avg_degree {:4f} is_hub_node {}\n",
            //        __func__, level, (hit) ? "HIT" : "MISS", packet->aligned_address, aligned_address, set_idx, way_idx, packet->pc,
            //        packet->blocks.size(), packet->degree, packet->avg_degree, (packet->degree > uint64_t(packet->avg_degree)));
        if (block_hit) {
            if (repl_counter->get_counter_value(way_idx) < hit_counter) hit_counter = repl_counter->get_counter_value(way_idx);
            way_idx_list.push_back(way_idx);
        } else {
            //break;
        }
    }

    accesses++;
    cache->record_leader_access(get_dueling_type(), hit);

    if (hit) {
        if (level == CacheLevel::L1D || cache->get_insertion_policy() != InsertionPolicy::EXCLUSIVE)
            repl_counter->init_counter(packet);
        // Amoeba-Cache: LRU over blocks, so a hit makes the block most recent
        // (an exclusive LLC moves the line out on a hit, so this only matters
        // for the single-level cache).
        if (amoeba_active())
            for (auto way_idx : way_idx_list)
                if (am_way_block[way_idx] >= 0) am_blocks[am_way_block[way_idx]].stamp = ++am_clock;
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

            if (tags[way_idx] == make_tag(align_address(packet->address, block_size))) {
                serviced_from_llc[way_idx]++;
            }
        }

        hits++;
    } else {
        if (amoeba_active()) {
            for (const auto block : packet->blocks) {
                auto aligned = align_address(block, block_size);
                if (find_way(aligned) < tags.size()) continue;
                auto f = am_fate.find(aligned);
                if (f == am_fate.end()) am_miss_never++;
                else if (f->second == 1) am_miss_dropped++;
                else am_miss_evicted++;
                break;   // classify the access by its first missing word
            }
        }
        // Miss: how many blocks of the requested 64B line this set still holds
        // (all of a line's blocks map to the same set). Always 0 at 64B.
        uint64_t line = align_address(packet->address, CACHELINE_SIZE), present = 0;
        for (uint64_t b = 0; b < CACHELINE_SIZE / block_size; b++)
            present += find_way(line + b * block_size) < tags.size();
        cache->incr_partial_misses(present);
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
        if (level == CacheLevel::L1D) {
            was_accessed = (block_address == align_address(packet->address, block_size)); 
        } else {
            // The footprint has one bit per 8B sector of the line, by position,
            // ORed over the block's whole lifetime. blocks[] may have been
            // compacted (empty, already-present or dropped blocks removed), so
            // index it by this block's own first sector, not by block_idx. A
            // block counts as accessed if any of its sectors was touched.
            uint64_t first_sector = (block_address % CACHELINE_SIZE) / 8;
            was_accessed = ((packet->footprint >> first_sector) & bitmask) != 0;
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
            set_way_address(way_idx, block_address);
            if (amoeba_active())
                amoeba_attach_way(way_idx, block_address);
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

        // The packet footprint has one bit per 8B sector of the line; this
        // block covers sectors (block_address % 64)/8 .. + bits_per_block - 1.
        if (block_address != UINT64_MAX) {
            uint64_t first_sector = (block_address % CACHELINE_SIZE) / 8;
            for (uint64_t word_idx = 0; word_idx < bits_per_block; word_idx++) {
                if ((packet->footprint >> (first_sector + word_idx)) & 1)
                    set_footprint(way_idx, word_idx, true);
            }
#ifndef MULTI_LEVEL
            // Single-level fills are demand misses with no footprint: the
            // demanded word is used, as SectoredSet marks it in the L1D.
            if (block_address == align_address(packet->address, block_size))
                set_footprint(way_idx, (packet->address - block_address) >> 3, true);
#endif
        }

        if (cachesim::DEBUG || cachesim::LLC_DEBUG)
            fmt::print("{}: Level {} Inserting address {:#x} @ set {} way {} blk_size {} packet size {} total ways {} valid {} repl_counter {} pc {:#x} block_idx {} was_accessed {} set_footprint {} serviced_from_llc {} packet footprint {:#x} degree {} avg_degree {:4f} is_hub_node {}\n",
                    cachesim::instCount, level, ways[way_idx], set_idx, way_idx, block_size, packet->size, num_ways, (uint32_t)valid[way_idx], repl_counter->get_counter_value(way_idx), pc[way_idx], block_idx, was_accessed, get_footprint(way_idx, 0), serviced_from_llc[way_idx],
                    packet->footprint, packet->degree, packet->avg_degree, packet->degree > uint64_t(packet->avg_degree));

        block_idx++;
    }
    return;
}



// One victim way leaves the set: footprint packed into packet at position
// num_blocks_evicted, metadata merged into packet, ghost cache / stats
// updated, way invalidated. Shared by handle_evict() and base replacement.
void Set::record_reuse(uint64_t degree, float avg_degree, uint64_t llc_hits) {
    int d = degree == 0 ? 0 : std::min<int>(REUSE_DEG_BUCKETS - 1, 1 + (63 - __builtin_clzll(degree)));
    int r = llc_hits < 4 ? (int)llc_hits : std::min<int>(REUSE_BUCKETS - 1, 1 + (63 - __builtin_clzll(llc_hits)));
    reuse_hist[d][r]++;
    reuse_hits_by_deg[d] += llc_hits;
    int c = degree == 0 ? 2 : (degree > (uint64_t)avg_degree ? 1 : 0);
    reuse_blocks[c]++;
    reuse_reused[c] += llc_hits > 0;
    reuse_hits[c] += llc_hits;
}

void Set::record_reuse_residents() const {
    if (level != CacheLevel::LLC) return;
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++)
        if (valid[way_idx])
            record_reuse(degree[way_idx], avg_degree[way_idx], serviced_from_llc[way_idx]);
}

void Set::print_reuse_by_degree(const char* name) {
    static const char* reuse_label[REUSE_BUCKETS] = {"0", "1", "2", "3", "4-7", "8-15", "16-31", "32+"};
    fmt::print("{} LLC reuse by degree (one record per block lifetime; reuse = times serviced from LLC) columns:", name);
    for (auto l : reuse_label) fmt::print(" {}", l);
    fmt::print("\n");
    for (int d = 0; d < REUSE_DEG_BUCKETS; d++) {
        uint64_t n = 0;
        for (int r = 0; r < REUSE_BUCKETS; r++) n += reuse_hist[d][r];
        if (n == 0) continue;
        std::string range = d == 0 ? "unlabelled" : d == REUSE_DEG_BUCKETS - 1 ? fmt::format("[{},inf)", 1ULL << (d - 1))
                                                  : fmt::format("[{},{})", 1ULL << (d - 1), 1ULL << d);
        fmt::print("{} reuse degree {} blocks {} mean_reuse {:.3f} reused {:.4f} |", name, range, n,
                   (double)reuse_hits_by_deg[d] / n, 1.0 - (double)reuse_hist[d][0] / n);
        for (int r = 0; r < REUSE_BUCKETS; r++) fmt::print(" {}", reuse_hist[d][r]);
        fmt::print("\n");
    }
    static const char* cls[3] = {"non-hub", "hub", "unlabelled"};
    uint64_t all_hits = reuse_hits[0] + reuse_hits[1] + reuse_hits[2];
    for (int c = 0; c < 3; c++)
        fmt::print("{} reuse {}: blocks {} reused(>=1) {} ({:.4f}) mean_reuse {:.3f} share_of_llc_hits {:.4f}\n", name, cls[c],
                   reuse_blocks[c], reuse_reused[c], reuse_blocks[c] ? (double)reuse_reused[c] / reuse_blocks[c] : 0.0,
                   reuse_blocks[c] ? (double)reuse_hits[c] / reuse_blocks[c] : 0.0, all_hits ? (double)reuse_hits[c] / all_hits : 0.0);
    // PHRU's rule, scored over labelled blocks: predict hub if reused at least once.
    uint64_t tp = reuse_reused[1], fp = reuse_reused[0], fn = reuse_blocks[1] - reuse_reused[1];
    fmt::print("{} reuse rule 'reused >= 1 => hub' (labelled blocks): precision {:.4f} recall {:.4f}\n", name,
               tp + fp ? (double)tp / (tp + fp) : 0.0, tp + fn ? (double)tp / (tp + fn) : 0.0);
}

void Set::retire_way(uint64_t way_idx) {
    if (level != CacheLevel::LLC || !valid[way_idx])
        return;
    record_reuse(degree[way_idx], avg_degree[way_idx], serviced_from_llc[way_idx]);
    for (uint64_t word_idx = 0; word_idx < block_size/8; word_idx++) {
        life_pieces++;
        life_touched += get_footprint(way_idx, word_idx);
    }
    record_line_used(way_idx);
}

void Set::record_line_used(uint64_t way_idx) {
    if (block_size != CACHELINE_SIZE || !valid[way_idx])
        return;
    int used = 0;
    for (uint64_t word_idx = 0; word_idx < CACHELINE_SIZE/8; word_idx++)
        used += get_footprint(way_idx, word_idx);
    line_used_hist[used]++;
}

void Set::record_line_used_residents() {
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++)
        record_line_used(way_idx);
}

void Set::count_resident(uint64_t& capacity, uint64_t& valid_pieces, uint64_t& touched) const {
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++) {
        capacity += block_size/8;
        if (!valid[way_idx]) continue;
        valid_pieces += block_size/8;
        for (uint64_t word_idx = 0; word_idx < block_size/8; word_idx++)
            touched += footprint[way_idx*(block_size/8) + word_idx];
    }
}

void Set::evict_way_into(PacketPtr packet, uint64_t way_idx, uint64_t num_blocks_evicted, uint64_t num_blocks_to_evict) {
    retire_way(way_idx);   // an LLC victim leaves the hierarchy
#ifndef MULTI_LEVEL
    record_line_used(way_idx);   // single-level: the line's lifetime ends here
#endif
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
            set_way_address(dist_way_idx, ways[way_idx]);
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
            fmt::print("{}: Level {} Evicted set {} way {} address {:#x} dirty {} valid {} repl_counter {} pc {:#x} num_blocks_to_evict {} packet footprint {:#x} word_idx {} get_footprint {} serviced_from_llc {} num_blocks_evicted {} degree {} avg_degree {:4f} repl_counter {}\n",
                cachesim::instCount, level, set_idx, way_idx, ways[way_idx], (uint32_t)dirty[way_idx], (uint32_t)valid[way_idx], repl_counter->get_counter_value(way_idx),
                pc[way_idx], num_blocks_to_evict, packet->footprint,
                i, get_footprint(way_idx, i), serviced_from_llc[way_idx], num_blocks_evicted,
                degree[way_idx], avg_degree[way_idx], repl_counter->get_counter_value(way_idx));

        set_footprint(way_idx, i, false);
        //set_block_accesses(way_idx, i, 0);
    }

    //fmt::print("PC {} way {}\n", pc[way_idx], way_idx);
    fill_packet(packet, way_idx);


    // Capture the evicted line's address before invalidate_way() clears
    // it, so a fast re-miss on this same address can be caught as a
    // "ghost cache hit" (see inc/ghost_cache.h). LLC only (level 1) -
    // PHRU/PHRUpp are LLC-level policies, and L1D evictions aren't what
    // this is meant to catch. Only store lines with
    // serviced_from_llc[way_idx] > GHOST_CACHE_MIN_SERVICED - i.e. this
    // exact line has already round-tripped through LLC more than that
    // many times, not just the bare minimum. Measured on as-Skitter:
    // ~77% of evictions that qualified under a threshold of 0 were
    // exactly 1, the weakest possible signal, and raising the
    // threshold to 1 helped PHRU substantially (fixed a persistent 8B
    // regression) but hurt HRU slightly - the two policies want
    // different bars here, hence CLI-tunable rather than hardcoded.
    if (cachesim::USE_GHOST_CACHE && level == CacheLevel::LLC && valid[way_idx] && serviced_from_llc[way_idx] > cachesim::GHOST_CACHE_MIN_SERVICED) {
        cachesim::ghostCache.insert(ways[way_idx], serviced_from_llc[way_idx]);
    }

    invalidate_way(way_idx);
}

bool Set::amoeba_keep(PacketPtr fill_packet, uint64_t block_address) const {
    uint64_t sector = (block_address % CACHELINE_SIZE) / 8;
    uint64_t fp = fill_packet->footprint & 0xff;
    if (fp == 0 && level != CacheLevel::LLC && fill_packet->address != UINT64_MAX)
        fp = 1ULL << ((fill_packet->address % CACHELINE_SIZE) / 8);
    bool keep = true;
    if (fp != 0 && cachesim::AMOEBA_INSERT == AmoebaInsert::SPAN) {
        uint64_t first = __builtin_ctzll(fp), last = 63 - __builtin_clzll(fp);
        keep = sector >= first && sector <= last;
    }
    if (!keep) {
        am_stat_dropped++;
        am_fate[block_address] = 1;
    }
    return keep;
}

uint64_t Set::amoeba_reserve(PacketPtr fill_packet, PacketPtr eviction_packet) {
    // The words to store form one contiguous range of one line (span or
    // whole line), so this fill is a single Amoeba-Block.
    uint32_t first_sector = UINT32_MAX, last_sector = 0, count = 0;
    for (auto addr : fill_packet->blocks) {
        if (addr == UINT64_MAX) continue;
        uint32_t s = (addr % CACHELINE_SIZE) / 8;
        first_sector = std::min(first_sector, s);
        last_sector = std::max(last_sector, s);
        count++;
    }
    assert(count > 0 && last_sector - first_sector + 1 == count);
    const std::pair<uint32_t, uint32_t> ranges[] = {{first_sector, count}};   // (start, len)
    auto find_free = [this](uint32_t need) -> int64_t {   // first fit
        uint32_t run = 0;
        for (uint64_t i = 0; i < am_word.size(); i++) {
            if (am_word[i] >= 0) { run = 0; continue; }
            if (++run == need) return (int64_t)(i + 1 - need);
        }
        return -1;
    };
    am_pending.clear();
    am_stat_fills++;
    uint64_t evicted = 0;
    const uint64_t first_stamp = am_clock + 1;   // blocks of this fill are never victims
    for (auto [start, len] : ranges) {
        const uint32_t tag_words = cachesim::AMOEBA_FREE_TAGS ? 0 : 1;
        uint32_t need = len + tag_words;         // tag word + data words
        int64_t pos;
        while ((pos = find_free(need)) < 0) {
            uint64_t free_words = std::count(am_word.begin(), am_word.end(), -1);
            int32_t victim = -1;
            for (int32_t id = 0; id < (int32_t)am_blocks.size(); id++) {
                const auto& b = am_blocks[id];
                if (b.stamp != 0 && b.stamp < first_stamp && (victim < 0 || b.stamp < am_blocks[victim].stamp))
                    victim = id;
            }
            assert(victim >= 0 && !am_blocks[victim].ways.empty());
            if (free_words >= need)
                am_stat_frag_evictions++;   // enough free words, just not contiguous
            am_stat_evicted_blocks++;
            auto victim_ways = am_blocks[victim].ways;   // released (and freed) as they go
            for (auto w : victim_ways) {
                am_fate[ways[w]] = 2;
                evict_way_into(eviction_packet, w, std::min<uint64_t>(evicted++, 63), 0);
            }
        }
        int32_t id;
        if (!am_free_ids.empty()) { id = am_free_ids.back(); am_free_ids.pop_back(); }
        else { id = am_blocks.size(); am_blocks.emplace_back(); }
        auto& b = am_blocks[id];
        b.start = start; b.len = len; b.pos = pos; b.live = 0; b.stamp = ++am_clock; b.ways.clear();
        for (uint32_t i = 0; i < need; i++) am_word[pos + i] = id;   // tag (if any), then data
        am_pending.push_back(id);
        am_stat_blocks++;
        am_stat_words += len;
    }
    return evicted;
}

void Set::amoeba_attach_way(uint64_t way_idx, uint64_t block_address) {
    uint32_t sector = (block_address % CACHELINE_SIZE) / 8;
    for (auto id : am_pending) {
        auto& b = am_blocks[id];
        if (sector < b.start || sector >= b.start + b.len) continue;
        b.ways.push_back(way_idx);
        b.live++;
        am_way_block[way_idx] = id;
        am_way_pos[way_idx] = b.pos + (cachesim::AMOEBA_FREE_TAGS ? 0 : 1) + (sector - b.start);
        assert(am_word[am_way_pos[way_idx]] == id);
        return;
    }
    assert(false && "amoeba: filled word has no reserved block");
}

void Set::amoeba_release_way(uint64_t way_idx) {
    int32_t id = am_way_block[way_idx];
    if (id < 0) return;
    auto& b = am_blocks[id];
    assert(am_word[am_way_pos[way_idx]] == id);
    am_word[am_way_pos[way_idx]] = -1;
    b.ways.erase(std::find(b.ways.begin(), b.ways.end(), (uint32_t)way_idx));
    am_way_block[way_idx] = -1;
    if (--b.live == 0) {   // last data word gone: free the tag word too
        if (!cachesim::AMOEBA_FREE_TAGS)
            am_word[b.pos] = -1;
        b.stamp = 0;
        am_free_ids.push_back(id);
    }
}

void Set::amoeba_resident(uint64_t& blocks, uint64_t& data_words) const {
    for (const auto& b : am_blocks) {
        if (b.stamp == 0) continue;
        blocks++;
        data_words += b.live;
    }
}

void Set::print_amoeba_stats() {
    fmt::print("Amoeba: fills {} blocks inserted {} ({:.3f} words/block) | words dropped at fill {} | blocks evicted {} of which with enough free words but none contiguous {} ({:.2f}%)\n",
        am_stat_fills, am_stat_blocks, am_stat_blocks ? (double)am_stat_words / am_stat_blocks : 0.0, am_stat_dropped,
        am_stat_evicted_blocks, am_stat_frag_evictions,
        am_stat_evicted_blocks ? 100.0 * am_stat_frag_evictions / am_stat_evicted_blocks : 0.0);
    uint64_t misses = am_miss_dropped + am_miss_evicted + am_miss_never;
    auto pct = [misses](uint64_t v) { return misses ? 100.0 * v / misses : 0.0; };
    fmt::print("Amoeba LLC misses {}: word dropped at fill {} ({:.2f}%) | word evicted {} ({:.2f}%) | word never in LLC {} ({:.2f}%)\n",
        misses, am_miss_dropped, pct(am_miss_dropped), am_miss_evicted, pct(am_miss_evicted), am_miss_never, pct(am_miss_never));
}

void Set::handle_evict(PacketPtr packet) {
    // Amoeba-Cache sets made room for exactly this fill in amoeba_reserve().
    if (amoeba_active())
        return;
    uint64_t num_blocks_evicted = 0;
    auto num_invalid_blocks = get_num_invalid();
    uint64_t num_blocks_to_evict = packet->size/block_size;
    if (cachesim::DEBUG || cachesim::LLC_DEBUG)
        fmt::print("{}: Level {} Num invalid blocks {} Evicted blocks {} packet size {}\n",
            cachesim::instCount, level, num_invalid_blocks, num_blocks_evicted, packet->size);

    if (!is_eviction_needed(num_blocks_to_evict)) {
        if (cachesim::DEBUG || cachesim::LLC_DEBUG)
            fmt::print("{}: Level {} No eviction needed for set {} because there are {} invalid ways\n", 
                    cachesim::instCount, level, set_idx, std::count(valid.begin(), valid.end(), false));
        return;
    }

    num_blocks_to_evict -= num_invalid_blocks;
    while (num_blocks_evicted < num_blocks_to_evict) {
        uint64_t way_idx = num_ways;
        way_idx = repl_counter->get_eviction_candidate(packet->degree < (uint64_t)packet->avg_degree);

        evict_way_into(packet, way_idx, num_blocks_evicted, num_blocks_to_evict);
        num_blocks_evicted++;

    }



    if (cachesim::DEBUG || cachesim::LLC_DEBUG)
        fmt::print("{}: Level {} Num invalid blocks {} evicted blocks {} evicted line footprint {:#x}\n",
            cachesim::instCount, level, num_invalid_blocks, num_blocks_evicted, packet->footprint);

    return;
}

void Set::handle_invalidate(PacketPtr packet, uint64_t block_num) {
    // ways only ever hold block-aligned addresses, so an unaligned (or invalid)
    // packet->address never matched the old exact-address lookup; keep that.
    uint64_t way_idx = tags.size();
    if (packet->address != UINT64_MAX && align_address(packet->address, block_size) == packet->address)
        way_idx = find_way(packet->address);
    uint64_t inv_address = UINT64_MAX;
    if (way_idx < tags.size()) {
        //serviced_from_llc[way_idx]++;
        inv_address = ways[way_idx];
        //auto previous_footprint = packet->footprint;
        for (uint64_t word_idx = 0; word_idx < bits_per_block; word_idx++)
            packet->footprint |= (uint64_t)get_footprint(way_idx, word_idx) << (block_num*bits_per_block + word_idx);

        fill_packet(packet, way_idx);
        invalidate_way(way_idx);
        for (uint64_t word_idx = 0; word_idx < bits_per_block; word_idx++)
            set_footprint(way_idx, word_idx, false);

        if (cachesim::DEBUG || cachesim::LLC_DEBUG) {
            fmt::print("{}: Level {} Invalidated address {:#x} @ set {} way {} footprint {:#x} because of line promotion to higher level\n", 
                    cachesim::instCount, level, packet->address, set_idx, way_idx, packet->footprint);
        }
        packet->blocks.push_back(inv_address);
    } else {
        packet->blocks.push_back(UINT64_MAX);
        packet->block_degrees.push_back(0);
        packet->block_serviced_from_llc.push_back(0);
        packet->block_prev_counter.push_back(UINT64_MAX);
    }
}

template<typename VecType>
void Set::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool incr) {
    vec.resize(prev_num_ways*scale_factor);
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            if (incr) {
                //Need the following if condition to remove a compile time -Wbool-compare warning
                if constexpr (std::is_same_v<VecType, bool>) {
                    vec[baseIdx + j] = val;
                } else {
                    if (val == UINT64_MAX) {
                        vec[baseIdx + j] = val;
                    }
                    else {
                        vec[baseIdx + j] = val + (block_size/scale_factor)*j;
                    }
                }
            } else {
                vec[baseIdx + j] = val;
            }
        }
    }
}

template<typename VecType>
void Set::contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks) {
    assert(way_idx + num_blocks <= vec.size());
    auto start_it = vec.begin() + way_idx;
    auto end_it = vec.begin() + (way_idx + num_blocks);
    vec.erase(start_it, end_it);
}

void Set::set_breakdown(uint64_t new_block_size) {
    assert(new_block_size < block_size);
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++)
        retire_way(way_idx);   // the set is emptied below

    // Second, evict ways if iso area set needs to convert ways to tags
    uint64_t scale_factor = block_size/new_block_size;
    uint64_t new_num_ways = num_ways*scale_factor;
    auto iso_area_new_num_ways = get_iso_area_cache(cache->get_num_sets(), num_ways, new_block_size);
    auto num_blocks_to_evict = std::ceil((new_num_ways - iso_area_new_num_ways)/scale_factor);

    
    //for (int way_idx = 0; way_idx < num_ways; way_idx++) {
    //    fmt::print("Set Contents. set {} way {} before breakdown. addr {:#x} repl_counter {} next_reuse {} degree {} avg_degree {:4f}\n",
    //            set_idx, way_idx, ways[way_idx], repl_counter->get_counter_value(way_idx), next_reuse[way_idx], degree[way_idx], avg_degree[way_idx]);
    //}
#if 0
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++) {
        repl_counter->evict(way_idx);
        ways[way_idx] = UINT64_MAX;
        valid[way_idx] = false;
        serviced_from_llc[way_idx] = 0;
        is_hub_node[way_idx] = false;
        distance_counts[way_idx] = 0;
        dirty[way_idx] = false;
        footprint[way_idx] = 0;
        pc[way_idx] = UINT64_MAX;
        next_reuse[way_idx] = 0;
        way_hits[way_idx] = 0;
        degree[way_idx] = 0;
        avg_degree[way_idx] = 0;
    }
#endif
    while (num_blocks_to_evict) {
#if 1
        uint64_t way_idx = num_ways;
        auto way = std::find(valid.begin(), valid.end(), false);
        if (way != valid.end()) {
            way_idx = std::distance(valid.begin(), way);
        } else {
            way_idx = repl_counter->get_eviction_candidate(false);
        }
        way_idx = std::distance(degree.begin(), std::min(degree.begin(), degree.end()));
#endif
#if 0
        uint64_t way_idx = num_ways;
        auto way = std::find(degree.begin(), degree.end(), 0);
        if (way != degree.end()) {
            way_idx = std::distance(degree.begin(), way);
        } else {
            way_idx = std::distance(degree.begin(), std::min_element(degree.begin(), degree.end()));
        }
#endif
        if (dirty[way_idx]) {
            dirty[way_idx] = false;
        }
        repl_counter->evict(way_idx);
        if (iso_area_new_num_ways != new_num_ways) {
            contract(ways, way_idx, 1);
            contract(tags, way_idx, 1);
            contract(valid, way_idx, 1);
            contract(serviced_from_llc, way_idx, 1);
            contract(is_hub_node, way_idx, 1);
            contract(distance_counts, way_idx, 1);
            contract(dirty, way_idx, 1);
            contract(footprint, way_idx, 1);
            contract(pc, way_idx, 1);
            contract(next_reuse, way_idx, 1);
            contract(way_hits, way_idx, 1);
            contract(degree, way_idx, 1);
            contract(avg_degree, way_idx, 1);
            get_replacement_policy()->set_contract(way_idx, 1);
            num_ways--;
        }

        num_blocks_to_evict --;
    }
    assert(num_ways*scale_factor == iso_area_new_num_ways);
    // First, resize all the vectors and break down their contents
    breakdown(ways, num_ways, scale_factor, true);
    breakdown(valid, num_ways, scale_factor, false);
    breakdown(serviced_from_llc, num_ways, scale_factor, false);
    breakdown(is_hub_node, num_ways, scale_factor, false);
    breakdown(distance_counts, num_ways, scale_factor, false);
    breakdown(dirty, num_ways, scale_factor, false);
    breakdown(footprint, num_ways, scale_factor, false);
    breakdown(pc, num_ways, scale_factor, false);
    breakdown(next_reuse, num_ways, scale_factor, false);
    breakdown(way_hits, num_ways, scale_factor, false);
    breakdown(degree, num_ways, scale_factor, false);
    breakdown(avg_degree, num_ways, scale_factor, false);
    num_ways = iso_area_new_num_ways;
    get_replacement_policy()->set_breakdown(block_size, new_block_size);

//    for (int way_idx = 0; way_idx < num_ways; way_idx++) {
//        fmt::print("Set Contents. set {} way {} after breakdown. addr {:#x} repl_counter {} next_reuse {} degree {} avg_degree {:4f}\n",
//                set_idx, way_idx, ways[way_idx], repl_counter->get_counter_value(way_idx), next_reuse[way_idx], degree[way_idx], avg_degree[way_idx]);
//
//    }

    block_size = new_block_size;
    num_blocks = CACHELINE_SIZE/block_size;
    num_lines = num_ways/num_blocks;
    bits_per_block = block_size/8;
    bitmask = (1ULL << bits_per_block) - 1;

    // Tags depend on block_size (sector bits), so rebuild them from the
    // broken-down ways now that block_size is the new one.
    tags.resize(num_ways);
    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++)
        tags[way_idx] = make_tag(ways[way_idx]);
    // Breaking down starts the set empty, as merge already does: the 8B copies
    // of the old 64B lines are mostly never-used sectors carrying their line's
    // replacement state, and keeping them measurably hurt. Not re-encoding
    // first also matters with Base+Delta tags: the old lines can span more
    // address regions than there are bases.
    bdi_reset();             // per-way state sized for the new way count, nothing encoded
    invalidate_all_ways();
    bdi_reset();
}

// Inverse of set_breakdown: merge small blocks back into fewer, larger ones.
// There's no sound way to decide which group of small blocks becomes one
// coherent big block (they may not even all be valid), so instead of trying
// to preserve data the way set_breakdown does, invalidate everything and
// resize back to original_num_ways - the way count this set had before it
// was ever broken down - rather than recomputing an iso-area target from the
// *current* (already rounded) way count. get_iso_area_cache_merge() rounds
// its result up to a multiple of 8, same as get_iso_area_cache() does going
// the other way; composing the two isn't idempotent, so doing that on every
// merge made each breakdown+merge round trip inflate the way count by ~8
// (confirmed by direct calculation: 32 -> 160 -> 40 -> 200 -> 48 -> ...),
// eventually corrupting the heap after enough repeated cycles. Restoring the
// literal original value sidesteps the asymmetry entirely.
// footprint is contracted/reset 1-per-way here, mirroring the same
// (pre-existing) 1-per-way handling set_breakdown itself uses, even though
// its true size is num_ways*(block_size/8) - not something this change
// introduces or fixes.
void Set::set_merge(uint64_t new_block_size) {
    assert(new_block_size > block_size);

    uint64_t new_num_ways = original_num_ways;

    for (uint64_t way_idx = 0; way_idx < num_ways; way_idx++) {
        retire_way(way_idx);   // merge discards every block
        repl_counter->evict(way_idx);
    }

    ways.assign(new_num_ways, UINT64_MAX);
    tags.assign(new_num_ways, INVALID_TAG);
    valid.assign(new_num_ways, false);
    serviced_from_llc.assign(new_num_ways, 0);
    is_hub_node.assign(new_num_ways, false);
    distance_counts.assign(new_num_ways, 0);
    dirty.assign(new_num_ways, false);
    footprint.assign(new_num_ways * new_block_size / 8, false);   // block_size/8 words per way
    pc.assign(new_num_ways, UINT64_MAX);
    next_reuse.assign(new_num_ways, 0);
    way_hits.assign(new_num_ways, 0);
    degree.assign(new_num_ways, 0);
    avg_degree.assign(new_num_ways, 0);

    get_replacement_policy()->set_merge(block_size, new_block_size, new_num_ways);

    num_ways = new_num_ways;
    block_size = new_block_size;
    num_blocks = CACHELINE_SIZE/block_size;
    num_lines = num_ways/num_blocks;
    bits_per_block = block_size/8;
    bitmask = (1ULL << bits_per_block) - 1;
    bdi_reset();
}
