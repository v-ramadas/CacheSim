#ifndef __SHIP_REPLACEMENT_POLICY_H__
#define __SHIP_REPLACEMENT_POLICY_H__

#include "srrip_replacement_policy.h"

// SHiP (Wu et al., MICRO 2011) on SRRIP's 2-bit RRPV. Each way remembers
// the signature of the access that inserted it and whether it was
// reused. A shared SHCT of 3-bit saturating counters is incremented when a
// line is hit and decremented when a line is evicted without reuse. A fill
// whose signature's counter is 0 is predicted dead and inserted at maxRRPV,
// otherwise at maxRRPV-1. In the exclusive hierarchy an LLC hit moves the line
// to L1D, so training happens in hit_update; lines re-inserted after an LLC
// hit go to RRPV 0, as in SRRIP. Counters start at 1 (weakly reused) so cold
// signatures are not all predicted dead.
//
// SHIPBase holds the shared mechanism; the variants differ only in the
// signature: SHIPPC (SHiP-PC) uses the inserting PC, SHIPMem (SHiP-Mem) the
// line's 16KB memory region.
class SHIPBase : public SRRIP {
    static constexpr uint64_t SHCT_ENTRIES = 16384;
    static constexpr uint8_t SHCT_MAX = 7;
    inline static std::vector<uint8_t> shct = std::vector<uint8_t>(SHCT_ENTRIES, 1);

    std::vector<uint64_t> signature;   // SHCT index of the inserting access
    std::vector<bool> reused;
    std::vector<bool> tracked;          // way holds a line inserted by fill_update

    protected:
    static uint64_t fold(uint64_t key);   // into an SHCT index
    // SHCT index for the block being filled.
    virtual uint64_t make_signature(PacketPtr packet, uint64_t block_idx) const = 0;

    public:
    SHIPBase(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : SRRIP(_set_idx, _num_ways, _level) {
        signature.resize(num_ways, 0);
        reused.resize(num_ways, false);
        tracked.resize(num_ways, false);
    }

    void hit_update(PacketPtr packet, uint64_t way_idx);
    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);
    void evict(uint64_t way_idx);
    void set_breakdown(uint64_t old_block_size, uint64_t new_block_size);
    void set_contract(uint64_t way_idx, uint64_t size);
    void set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways);
};

class SHIPPC : public SHIPBase {
    protected:
    uint64_t make_signature(PacketPtr packet, uint64_t block_idx) const override;
    public:
    SHIPPC(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : SHIPBase(_set_idx, _num_ways, _level) {}
};

class SHIPMem : public SHIPBase {
    static constexpr uint64_t REGION_BITS = 14;   // 16KB regions
    protected:
    uint64_t make_signature(PacketPtr packet, uint64_t block_idx) const override;
    public:
    SHIPMem(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : SHIPBase(_set_idx, _num_ways, _level) {}
};

#endif
