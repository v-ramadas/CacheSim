#include "ship_replacement_policy.h"

uint64_t SHIPBase::fold(uint64_t key) {
    // Fold the key into 14 bits.
    key ^= key >> 14;
    key ^= key >> 28;
    return key % SHCT_ENTRIES;
}

uint64_t SHIPPC::make_signature(PacketPtr packet, uint64_t /*block_idx*/) const {
    return fold(packet->pc);
}

uint64_t SHIPMem::make_signature(PacketPtr packet, uint64_t block_idx) const {
    uint64_t addr = (block_idx < packet->blocks.size() && packet->blocks[block_idx] != UINT64_MAX)
                        ? packet->blocks[block_idx] : packet->address;
    return fold(addr >> REGION_BITS);
}

void SHIPBase::hit_update(PacketPtr packet, uint64_t way_idx) {
    SRRIP::hit_update(packet, way_idx);
    if (tracked[way_idx]) {
        if (shct[signature[way_idx]] < SHCT_MAX) shct[signature[way_idx]]++;
        reused[way_idx] = true;
    }
}

void SHIPBase::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    uint64_t sig = make_signature(packet, block_idx);
    signature[way_idx] = sig;
    reused[way_idx] = false;
    tracked[way_idx] = true;
    if (packet->from_ghost_cache || packet->serviced_from_llc > 0)
        SRRIP::fill_update(way_idx, block_idx, packet, was_accessed);   // RRPV 0
    else
        counter[way_idx] = (shct[sig] == 0) ? maxRRPV : maxRRPV - 1;
}

void SHIPBase::evict(uint64_t way_idx) {
    // A line that leaves without being reused trains its signature as dead.
    // (A hit sets reused before the line is invalidated and moves to L1D.)
    if (tracked[way_idx] && !reused[way_idx] && shct[signature[way_idx]] > 0)
        shct[signature[way_idx]]--;
    tracked[way_idx] = false;
    SRRIP::evict(way_idx);
}

void SHIPBase::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    uint64_t new_num_ways = num_ways * (old_block_size / new_block_size);
    SRRIP::set_breakdown(old_block_size, new_block_size);
    signature.assign(new_num_ways, 0);
    reused.assign(new_num_ways, false);
    tracked.assign(new_num_ways, false);
}

void SHIPBase::set_contract(uint64_t way_idx, uint64_t size) {
    SRRIP::set_contract(way_idx, size);
    signature.erase(signature.begin() + way_idx, signature.begin() + way_idx + size);
    reused.erase(reused.begin() + way_idx, reused.begin() + way_idx + size);
    tracked.erase(tracked.begin() + way_idx, tracked.begin() + way_idx + size);
}

void SHIPBase::set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways) {
    SRRIP::set_merge(old_block_size, new_block_size, new_num_ways);
    signature.assign(new_num_ways, 0);
    reused.assign(new_num_ways, false);
    tracked.assign(new_num_ways, false);
}
