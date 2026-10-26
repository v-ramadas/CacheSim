#include "popt_replacement_policy.h"
#include "trace_file.h"
#include "defs.h"
#include <fmt/core.h>

bool POPT::prescan(const std::string& trace_path) {
    // Vertex count from the GRAPH_INFO table (defs.h), keyed by the graph the
    // trace is named after.
    const GraphInfo* info = find_graph_info(trace_path);
    if (!info) {
        fmt::print(stderr, "POPT: no GRAPH_INFO entry (defs.h) for the graph of trace '{}'\n", trace_path);
        return false;
    }
    num_vertices = info->num_nodes;
    irregular_lines = (num_vertices * elem_size + CACHELINE_SIZE - 1) / CACHELINE_SIZE;

    TraceFile trace(trace_path);
    std::string line;
    while (trace.is_open() && trace.getline(line))
        trace_lines++;
    fmt::print("POPT: {} trace lines -> {} per epoch; {} vertices x {}B -> {} irregular 64B lines\n",
        trace_lines, std::max<uint64_t>(1, trace_lines / NUM_EPOCHS), num_vertices, elem_size, irregular_lines);
    return true;
}

uint64_t POPT::reserved_ways(uint64_t num_sets, uint64_t line_size) {
    uint64_t way_bytes = num_sets * line_size;
    return (2 * irregular_lines + way_bytes - 1) / way_bytes;
}

void POPT::print_stats() {
    uint64_t tot = stat_evict_regular + stat_evict_opt;
    fmt::print("POPT evictions {} | unlabelled first {} ({:.2f}%) | farthest epoch {} ({:.2f}%), of which RRIP tie-break {}\n",
        tot, stat_evict_regular, tot ? 100.0 * stat_evict_regular / tot : 0.0,
        stat_evict_opt, tot ? 100.0 * stat_evict_opt / tot : 0.0, stat_evict_tie);
}

uint64_t POPT::epoch_distance(uint64_t next) const {
    if (next == UINT64_MAX) return MAX_REREF;   // no later reference
    uint64_t epoch_len = std::max<uint64_t>(1, trace_lines / NUM_EPOCHS);
    uint64_t now = cachesim::TRACE_LINE / epoch_len, then = next / epoch_len;
    if (then <= now) return 0;                  // referenced again this epoch
    return std::min(then - now, MAX_REREF);
}

void POPT::hit_update(PacketPtr packet, uint64_t way_idx) {
    SRRIP::hit_update(packet, way_idx);
    next_use[way_idx] = packet->next_reuse;
}

void POPT::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    SRRIP::fill_update(way_idx, block_idx, packet, was_accessed);
    next_use[way_idx] = packet->next_reuse;
    // A 64B way gets one block but keeps the 8 per-sector degrees.
    uint64_t degree = packet->degree;
    for (auto d : packet->block_degrees) degree = std::max(degree, d);
    irregular[way_idx] = degree > 0;
}

uint64_t POPT::get_eviction_candidate(bool /*is_low_priority*/) {
    // Data that is not irregular goes first (first such way, as P-OPT does).
    for (uint64_t way = 0; way < num_ways; way++) {
        if (!irregular[way]) {
            stat_evict_regular++;
            return way;
        }
    }
    // Farthest next reference in epochs.
    std::vector<uint64_t> dist(num_ways);
    uint64_t max_dist = 0;
    for (uint64_t way = 0; way < num_ways; way++) {
        dist[way] = epoch_distance(next_use[way]);
        max_dist = std::max(max_dist, dist[way]);
    }
    stat_evict_opt++;
    uint64_t tied = std::count(dist.begin(), dist.end(), max_dist);
    if (tied > 1) stat_evict_tie++;
    // Ties: the tied way at the distant RRPV, aging only the tied ways.
    while (true) {
        for (uint64_t way = 0; way < num_ways; way++)
            if (dist[way] == max_dist && counter[way] >= maxRRPV)
                return way;
        for (uint64_t way = 0; way < num_ways; way++)
            if (dist[way] == max_dist && counter[way] < maxRRPV)
                counter[way]++;
    }
}

void POPT::evict(uint64_t way_idx) {
    SRRIP::evict(way_idx);
    next_use[way_idx] = UINT64_MAX;
    irregular[way_idx] = false;
}
