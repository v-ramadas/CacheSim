#include "grasp_replacement_policy.h"
#include "trace_file.h"
#include <fmt/core.h>
#include <unordered_map>

// One pass over the trace: each LLC block's degree is the highest NodeDegree
// of any access to it. Sorted by degree, the degree at the LLC-size mark is
// the high-reuse threshold and at twice that the moderate-reuse threshold
// (GRASP's first and second LLC-sized spans of the property array). Ties at a
// mark are included, so a tier can exceed the LLC size.
void GRASP::compute_thresholds() {
    thresholds_ready = true;
    TraceFile trace(trace_path);
    if (!trace.is_open() || llc_bytes == 0) {
        fmt::print(stderr, "GRASP: cannot read trace {} (llc_bytes {}); all labelled data is low reuse\n", trace_path, llc_bytes);
        return;
    }
    uint64_t shift = (uint64_t)log2(cachesim::BLOCK_SIZE);
    std::unordered_map<uint64_t, uint64_t> block_degree;
    std::string line;
    char action[16];
    uint64_t inst, pc, address, next_reuse, degree;
    float avg_degree;
    while (trace.getline(line)) {
        if (std::sscanf(line.c_str(), "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx,NextReuse:%lu,NodeDegree:%lu,PCAvgDegree:%f",
                        &inst, &pc, action, &address, &next_reuse, &degree, &avg_degree) != 7)
            continue;
        // Skip unlabelled accesses and the tracer's scalar labels, whose
        // degree equals the PC average (about the node count).
        if (degree == 0 || (float)degree == avg_degree)
            continue;
        auto& d = block_degree[address >> shift];
        if (degree > d) d = degree;
    }
    std::vector<uint64_t> degrees;
    degrees.reserve(block_degree.size());
    for (auto& kv : block_degree) degrees.push_back(kv.second);
    std::sort(degrees.begin(), degrees.end(), std::greater<uint64_t>());
    uint64_t llc_blocks = llc_bytes / cachesim::BLOCK_SIZE;
    auto at = [&](uint64_t n) { return degrees.empty() ? UINT64_MAX : degrees[std::min(n, (uint64_t)degrees.size()) - 1]; };
    high_degree = at(llc_blocks);
    mod_degree = at(2 * llc_blocks);
    fmt::print("GRASP: {} labelled {}B blocks, LLC {} blocks -> high-reuse degree >= {}, moderate >= {}\n",
        degrees.size(), cachesim::BLOCK_SIZE, llc_blocks, high_degree, mod_degree);
}

void GRASP::print_stats() {
    uint64_t tot = stat_fills[0] + stat_fills[1] + stat_fills[2] + stat_fills[3];
    auto pct = [tot](uint64_t v) { return tot ? 100.0 * v / tot : 0.0; };
    fmt::print("GRASP thresholds high {} moderate {} | fills {} high {} ({:.2f}%) moderate {} ({:.2f}%) low {} ({:.2f}%) default {} ({:.2f}%)\n",
        high_degree, mod_degree, tot,
        stat_fills[(int)Hint::High], pct(stat_fills[(int)Hint::High]),
        stat_fills[(int)Hint::Moderate], pct(stat_fills[(int)Hint::Moderate]),
        stat_fills[(int)Hint::Low], pct(stat_fills[(int)Hint::Low]),
        stat_fills[(int)Hint::Default], pct(stat_fills[(int)Hint::Default]));
}

GRASP::Hint GRASP::classify(uint64_t degree) const {
    if (degree == 0) return Hint::Default;
    if (degree >= high_degree) return Hint::High;
    if (degree >= mod_degree) return Hint::Moderate;
    return Hint::Low;
}

uint64_t GRASP::block_degree(PacketPtr packet, uint64_t block_idx) const {
    // A 64B way gets one block but keeps the 8 per-sector degrees: hint the
    // line by its highest-degree vertex (as hub_oracle does).
    if (packet->blocks.size() == 1 && packet->block_degrees.size() > 1)
        return *std::max_element(packet->block_degrees.begin(), packet->block_degrees.end());
    if (block_idx < packet->block_degrees.size())
        return packet->block_degrees[block_idx];
    return packet->degree;
}

void GRASP::hit_update(PacketPtr packet, uint64_t way_idx) {
    switch (classify(packet->degree)) {
        case Hint::Moderate:
        case Hint::Low:
            if (counter[way_idx] > 0 && counter[way_idx] <= maxRRPV) counter[way_idx]--;
            break;
        default:
            counter[way_idx] = 0;
            break;
    }
}

void GRASP::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool /*was_accessed*/) {
    Hint hint = classify(block_degree(packet, block_idx));
    stat_fills[(int)hint]++;
    uint64_t serviced = (block_idx < packet->block_serviced_from_llc.size())
                            ? packet->block_serviced_from_llc[block_idx] : packet->serviced_from_llc;
    if (packet->blocks.size() == 1) serviced = packet->serviced_from_llc;
    switch (hint) {
        case Hint::High:
            counter[way_idx] = 0;
            break;
        case Hint::Moderate:
        case Hint::Low: {
            uint64_t insert = (hint == Hint::Moderate) ? maxRRPV - 1 : maxRRPV;
            counter[way_idx] = insert - std::min(insert, serviced);
            break;
        }
        case Hint::Default:
            counter[way_idx] = (packet->serviced_from_llc > 0) ? 0 : denseRRPV - 1;
            break;
    }
}
