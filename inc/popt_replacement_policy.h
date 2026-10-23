#ifndef __POPT_REPLACEMENT_POLICY_H__
#define __POPT_REPLACEMENT_POLICY_H__

#include "srrip_replacement_policy.h"
#include <string>

// Approximation of P-OPT (Balaji, Crago, Jaleel, Lucia, HPCA 2021), 64B LLC.
// P-OPT evicts the irregularly accessed line whose next reference is farthest
// away, using a "rereference matrix" quantized to 256 epochs of the traversal
// (distance in epochs, 0 = referenced again this epoch, capped at 127), and
// reserves LLC capacity for two epochs of that matrix. Here the next
// reference comes from the trace's NextReuse field (absolute trace-line
// index of the next access to the same 64B block), quantized to 256 equal
// epochs of trace lines, so this is P-OPT with exact rather than CSR-derived
// rereference information. Lines with a degree label stand in for P-OPT's
// irregular data. As in the paper, the matrix spans
// numLines = numVertices * elemSize / 64 lines; numVertices comes from the
// graph's GAP .sg header (--popt-graph), elemSize from --popt-elem-size.
//
// Eviction (P-OPT's getReplacementIndex): first a way holding unlabelled
// data; otherwise the largest epoch distance, ties broken by RRIP (aging only
// the tied ways). Insertion and hits follow SRRIP.
class POPT : public SRRIP {
    static constexpr uint64_t NUM_EPOCHS = 256;
    static constexpr uint64_t MAX_REREF = 127;

    std::vector<uint64_t> next_use;    // absolute trace line of the next access
    std::vector<bool> irregular;       // way holds degree-labelled data

    uint64_t epoch_distance(uint64_t next) const;

    inline static uint64_t stat_evict_regular = 0, stat_evict_opt = 0, stat_evict_tie = 0;

    public:
    // Set by main from --popt-graph / --popt-elem-size.
    inline static std::string graph_path;
    inline static uint64_t elem_size = 4;
    // Set by prescan(): trace length in lines; irregular-array lines from the
    // graph header.
    inline static uint64_t trace_lines = 0;
    inline static uint64_t num_vertices = 0;
    inline static uint64_t irregular_lines = 0;
    // Reads the graph header and makes one pass over the trace before the LLC
    // is built. Returns false if the graph header cannot be read.
    static bool prescan(const std::string& trace_path);
    // Ways P-OPT reserves per set for two epochs of the rereference matrix
    // (one byte per irregular line per epoch).
    static uint64_t reserved_ways(uint64_t num_sets, uint64_t line_size);
    static void print_stats();

    POPT(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : SRRIP(_set_idx, _num_ways, _level) {
        next_use.resize(num_ways, UINT64_MAX);
        irregular.resize(num_ways, false);
    }

    void hit_update(PacketPtr packet, uint64_t way_idx);
    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);
    uint64_t get_eviction_candidate(bool is_low_priority);
    void evict(uint64_t way_idx);
};

#endif
