#ifndef __DRRIP_REPLACEMENT_POLICY_H__
#define __DRRIP_REPLACEMENT_POLICY_H__

#include "srrip_replacement_policy.h"

// DRRIP (Jaleel et al., ISCA 2010) on SRRIP's 2-bit RRPV: set dueling between
// SRRIP (insert at maxRRPV-1) and BRRIP (insert at maxRRPV, at maxRRPV-1 once
// every BIP_MAX fills). 32 SRRIP and 32 BRRIP leader sets are spread over the
// cache by constituency (low 5 bits of the set index vs the next 5 bits); a
// miss in an SRRIP leader increments the shared PSEL, a miss in a BRRIP leader
// decrements it, and followers use BRRIP while PSEL is above its midpoint.
// Lines re-inserted after an LLC hit go to RRPV 0, as in SRRIP.
class DRRIP : public SRRIP {
    static constexpr uint64_t BIP_MAX = 32;
    static constexpr uint64_t PSEL_MAX = (1 << 10) - 1;
    inline static uint64_t PSEL = (PSEL_MAX + 1) / 2;
    inline static uint64_t bip_counter = 0;

    enum class SetType { SRRIPLeader, BRRIPLeader, Follower };
    SetType set_type() const;
    void insert_brrip(uint64_t way_idx, PacketPtr packet);

    public:
    DRRIP(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : SRRIP(_set_idx, _num_ways, _level) {
        split_reused_8b = false;
    }

    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);
};

#endif
