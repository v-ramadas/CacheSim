#include "drrip_replacement_policy.h"

DRRIP::SetType DRRIP::set_type() const {
    uint64_t offset = set_idx & 31, constituency = (set_idx >> 5) & 31;
    if (offset == constituency) return SetType::SRRIPLeader;
    if (offset == (~constituency & 31)) return SetType::BRRIPLeader;
    return SetType::Follower;
}

void DRRIP::insert_brrip(uint64_t way_idx, PacketPtr packet) {
    if (packet->from_ghost_cache || packet->serviced_from_llc > 0) {
        counter[way_idx] = 0;
        return;
    }
    counter[way_idx] = maxRRPV;
    if (++bip_counter == BIP_MAX) {
        bip_counter = 0;
        counter[way_idx] = maxRRPV - 1;
    }
}

void DRRIP::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    switch (set_type()) {
        case SetType::SRRIPLeader:
            if (PSEL < PSEL_MAX) PSEL++;
            SRRIP::fill_update(way_idx, block_idx, packet, was_accessed);
            break;
        case SetType::BRRIPLeader:
            if (PSEL > 0) PSEL--;
            insert_brrip(way_idx, packet);
            break;
        case SetType::Follower:
            if (PSEL > (PSEL_MAX + 1) / 2)
                insert_brrip(way_idx, packet);
            else
                SRRIP::fill_update(way_idx, block_idx, packet, was_accessed);
            break;
    }
}
