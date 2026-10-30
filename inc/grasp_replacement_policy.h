#ifndef __GRASP_REPLACEMENT_POLICY_H__
#define __GRASP_REPLACEMENT_POLICY_H__

#include "srrip_replacement_policy.h"
#include <string>

// Approximation of GRASP (Faldu, Diamond, Grot, HPCA 2020) without graph
// reordering. GRASP marks the first LLC-sized span of a (reordered) property
// array as high reuse and the next LLC-sized span as moderate reuse; here the
// same split comes from degree thresholds computed from the trace once, before
// simulation (compute_thresholds): the LLC-size worth of highest-degree
// blocks, then the next LLC-size worth. Data with no degree label is "default"
// and gets SRRIP unchanged.
//
// In the exclusive hierarchy an LLC hit is a re-insertion of a block the LLC
// serviced, so GRASP's hit policy is applied there:
//   high:     insert 0,          hit -> 0
//   moderate: insert maxRRPV-1,  hit -> one step closer to 0 per LLC service
//   low:      insert maxRRPV,    hit -> one step closer to 0 per LLC service
//   default:  SRRIP (insert maxRRPV-1, hit -> 0)
// With 8B LLC blocks the hint is per 8B block, and a block that was not
// touched during its L1 residency is inserted at maxRRPV regardless of hint.
class GRASP : public SRRIP {
    enum class Hint { Default, High, Moderate, Low };
    Hint classify(uint64_t degree) const;
    uint64_t block_degree(PacketPtr packet, uint64_t block_idx) const;

    inline static uint64_t stat_fills[4] = {0, 0, 0, 0};
    inline static uint64_t stat_untouched = 0;   // untouched 8B blocks inserted at maxRRPV

    // Degree thresholds of the high / moderate reuse tiers.
    inline static uint64_t high_degree = UINT64_MAX;
    inline static uint64_t mod_degree = UINT64_MAX;
    inline static bool thresholds_ready = false;
    static void compute_thresholds();

    public:
    // Set by main before the LLC is built: the trace to scan and the LLC's
    // data capacity in bytes.
    inline static std::string trace_path;
    inline static uint64_t llc_bytes = 0;

    static void print_stats();
    GRASP(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : SRRIP(_set_idx, _num_ways, _level) {
        if (_level == CacheLevel::LLC && !thresholds_ready)
            compute_thresholds();
    }

    void hit_update(PacketPtr packet, uint64_t way_idx);
    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);
};

#endif
