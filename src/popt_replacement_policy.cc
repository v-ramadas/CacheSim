#include "popt_replacement_policy.h"
#include "trace_file.h"
#include "defs.h"
#include <fmt/core.h>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <unistd.h>
#include "performance_model.h"

bool POPT::load(const std::string& path, bool with_matrix) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    Header h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || std::memcmp(h.magic, "POPTRM2", 8) != 0) {
        fmt::print(stderr, "POPT: {} is not a version-2 Rereference Matrix (rebuild with tools/popt_matrix.cc)\n", path);
        return false;
    }
    std::vector<int32_t> check(h.num_check);
    f.read(reinterpret_cast<char*>(check.data()), h.num_check * sizeof(int32_t));
    std::vector<uint64_t> st(h.num_epochs * h.num_sub + 1);
    f.read(reinterpret_cast<char*>(st.data()), st.size() * sizeof(uint64_t));
    if (with_matrix) {
        matrix.resize(h.num_epochs * h.num_lines);
        f.read(reinterpret_cast<char*>(matrix.data()), matrix.size());
    }
    if (!f) {
        fmt::print(stderr, "POPT: truncated matrix file {}\n", path);
        return false;
    }
    hdr = h;
    starts = std::move(st);
    // Keep the check list for setup() in the matrix vector's place only when not loading the matrix.
    if (!with_matrix) {
        matrix.assign(reinterpret_cast<uint8_t*>(check.data()), reinterpret_cast<uint8_t*>(check.data() + check.size()));
    }
    return true;
}

static std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
}

bool POPT::setup(const std::string& trace_path) {
    // <G>.<order> from the trace name (<dir>/<G>.<order>.xz)
    std::string base = trace_path.substr(trace_path.find_last_of('/') + 1);
    for (const char* ext : {".xz", ".gz", ".txt"})
        if (base.size() > strlen(ext) && base.compare(base.size() - strlen(ext), strlen(ext), ext) == 0)
            base.resize(base.size() - strlen(ext));
    std::vector<std::string> dirs;
    if (!matrix_dir.empty()) dirs.push_back(matrix_dir);
    dirs.push_back("data/popt");
    dirs.push_back(exe_dir() + "/../data/popt");
    // Variants: in-neighbour traversal (pull PageRank; .wd adds pr's write at the destination) and out-neighbour
    // traversal (.out: cc_sv's hook pass). The one whose neighbour prefix matches the trace's gathers is used.
    std::string plain, wd, out;
    for (const auto& d : dirs) {
        if (plain.empty() && std::ifstream(d + "/" + base + ".popt")) plain = d + "/" + base + ".popt";
        if (wd.empty() && std::ifstream(d + "/" + base + ".wd.popt")) wd = d + "/" + base + ".wd.popt";
        if (out.empty() && std::ifstream(d + "/" + base + ".out.popt")) out = d + "/" + base + ".out.popt";
    }
    if (plain.empty() && wd.empty() && out.empty()) {
        fmt::print(stderr, "POPT: no Rereference Matrix {0}.popt / {0}.wd.popt / {0}.out.popt in --popt-matrix-dir, data/popt or "
                   "<bin>/../data/popt\n", base);
        return false;
    }
    auto read_check = [&](const std::string& path, std::vector<int32_t>& chk) {
        if (path.empty() || !load(path, false)) return false;
        if (hdr.elem_size != elem_size) {
            fmt::print(stderr, "POPT: {} elem_size {} != --popt-elem-size {}\n", path, hdr.elem_size, elem_size);
            return false;
        }
        chk.assign(hdr.num_check, 0);
        std::memcpy(chk.data(), matrix.data(), hdr.num_check * sizeof(int32_t));
        matrix.clear();
        return true;
    };
    std::vector<int32_t> check_in, check_out;
    bool have_in = read_check(plain.empty() ? wd : plain, check_in);
    bool have_out = read_check(out, check_out);
    if (!have_in && !have_out) return false;

    // Prescan: reads per PC (+ the first num_check read addresses), writes per PC with their address range.
    struct PcInfo {
        uint64_t reads = 0, writes = 0, wmin = UINT64_MAX, wmax = 0; std::vector<uint64_t> first;
        uint64_t first_idx = UINT64_MAX, prev = 0, stride = 0; bool stride_ok = true;   // destination-stream detection
    };
    uint64_t line_idx = 0;
    std::map<uint64_t, PcInfo> pcs;
    TraceFile trace(trace_path);
    if (!trace.is_open()) {
        fmt::print(stderr, "POPT: cannot read trace {}\n", trace_path);
        return false;
    }
    std::string line;
    char action[16];
    uint64_t inst, pc, addr;
    while (trace.getline(line)) {
        line_idx++;
        if (std::sscanf(line.c_str(), "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx", &inst, &pc, action, &addr) != 4) continue;
        auto& p = pcs[pc];
        uint64_t n_before = p.reads + p.writes;
        if (n_before == 0) p.first_idx = line_idx;
        else if (n_before == 1) { p.stride = addr - p.prev; p.stride_ok = addr > p.prev; }
        else if (p.stride_ok && addr - p.prev != p.stride) p.stride_ok = false;
        p.prev = addr;
        if (strcmp(action, "Read") == 0) {
            p.reads++;
            if (p.first.size() < hdr.num_check) p.first.push_back(addr);
        } else {
            p.writes++;
            p.wmin = std::min(p.wmin, addr);
            p.wmax = std::max(p.wmax, addr);
        }
    }
    // Gather PC: num_arcs reads (a few fewer allowed: older traces lose the last gathers at the ROI end marker)
    // whose first accesses are irreg_base + elem_size * neighbour, in traversal order - tried against the
    // in-neighbour prefix, then the out-neighbour prefix.
    constexpr uint64_t kRoiSlack = 16;
    gather_reads = 0;
    auto detect = [&](const std::vector<int32_t>& check) {
        for (const auto& [p, info] : pcs) {
            if (info.reads > hdr.num_arcs || info.reads + kRoiSlack < hdr.num_arcs) continue;
            if (check.empty() || info.first.size() < check.size()) continue;
            uint64_t b = info.first[0] - elem_size * (uint64_t)check[0];
            bool ok = true;
            for (uint64_t i = 0; i < check.size() && ok; i++)
                ok = info.first[i] == b + elem_size * (uint64_t)check[i];
            if (ok && info.reads > gather_reads) { gather_pc = p; irreg_base = b; gather_reads = info.reads; }
        }
        return irreg_base != 0;
    };
    bool via_out = false;
    if (!(have_in && detect(check_in)) && have_out && detect(check_out)) via_out = true;
    if (irreg_base && gather_reads != hdr.num_arcs)
        fmt::print("POPT: warning: gather PC has {} reads, {} fewer than the graph's {} arcs (truncated at the ROI end)\n",
            gather_reads, hdr.num_arcs - gather_reads, hdr.num_arcs);
    if (!irreg_base) {
        fmt::print(stderr, "POPT: no read PC in {} matches the graph's gather sequence ({} arcs, first {} in-neighbours) - "
                   "the trace and the matrix's graph / vertex order differ\n", trace_path, hdr.num_arcs, hdr.num_check);
        return false;
    }
    irreg_bound = irreg_base + hdr.num_nodes * elem_size;
    // Destination stream: one access per vertex (a few fewer allowed at an ROI end), constant positive stride,
    // starting before the first gather; the earliest such PC.
    uint64_t gather_first = pcs[gather_pc].first_idx, best_first = UINT64_MAX;
    for (const auto& [p, info] : pcs) {
        uint64_t n = info.reads + info.writes;
        if (p == gather_pc || n > hdr.num_nodes || n + kRoiSlack < hdr.num_nodes || !info.stride_ok) continue;
        if (info.first_idx > gather_first || info.first_idx >= best_first) continue;
        best_first = info.first_idx; dst_pc = p; dst_total = n;
    }
    if (dst_pc)
        fmt::print("POPT: destination stream PC {:#x} ({} accesses, stride {}B) sets currDstID; gathers cross-check it\n",
            dst_pc, dst_total, pcs[dst_pc].stride);
    else
        fmt::print("POPT: warning: no per-destination stream found; currDstID follows the gather count (sub-epoch "
                   "granularity, lags across destinations without in-edges)\n");
    if (irreg_base % CACHELINE_SIZE)
        fmt::print("POPT: note: irregData starts at {:#x}, not 64B-aligned; a cache line's next ref is the nearest of the "
                   "two matrix lines it overlaps\n", irreg_base);
    // pr (Gauss-Seidel) writes irregData once per vertex while processing it.
    bool writes = false;
    for (const auto& [p, info] : pcs)
        if (info.writes == hdr.num_nodes && info.wmin >= irreg_base && info.wmax < irreg_bound) writes = true;
    const std::string& chosen = via_out ? out : writes ? wd : plain;
    if (via_out && writes) {
        fmt::print(stderr, "POPT: out-neighbour traversal with per-vertex irregData writes is not supported\n");
        return false;
    }
    if (chosen.empty()) {
        fmt::print(stderr, "POPT: trace {} irregData writes per vertex; needs {}.{}popt (tools/popt_matrix.cc{})\n",
            writes ? "has" : "has no", base, writes ? "wd." : "", writes ? " --writes-at-dst" : "");
        return false;
    }
    if (!load(chosen, true)) return false;
    if ((hdr.writes_at_dst != 0) != writes) {
        fmt::print(stderr, "POPT: {} writes_at_dst={} does not match the trace\n", chosen, hdr.writes_at_dst);
        return false;
    }
    active = true;
    block = 0;
    gathers = 0;
    cur_col = &matrix[0];
    nxt_col = hdr.num_epochs > 1 ? &matrix[hdr.num_lines] : nullptr;
    stat_streamed_lines = 2 * ((hdr.num_lines + 63) / 64);   // the first two columns
    fmt::print("POPT: matrix {} | {} vertices, {} arcs, {} x {}B irregData lines | {} epochs of {} vertices, {} sub-epochs of {} | "
               "gather PC {:#x}, irregData {:#x}..{:#x}{}\n",
        chosen, hdr.num_nodes, hdr.num_arcs, hdr.num_lines, hdr.elem_size, hdr.num_epochs, hdr.epoch_size, hdr.num_sub,
        hdr.sub_size, gather_pc, irreg_base, irreg_bound, writes ? " (+ writes at dst)" : "");
    return true;
}

void POPT::stream_columns(uint64_t columns) {
    // One DRAM access latency (tRCD + tCAS) plus the bytes at peak bandwidth
    // (data rate x channel width x channels), in CPU cycles.
    double cpu_mhz = PerformanceModel::getParam("cpu_frequency");
    double mem_mhz = PerformanceModel::getParam("memory_frequency");
    double bytes_per_us = (double)PerformanceModel::getParam("memory_data_rate") * PerformanceModel::getParam("memory_channel_width")
                        * std::max<uint64_t>(1, PerformanceModel::getParam("memory_channels"));   // MT/s x B = B/us
    if (cpu_mhz <= 0 || mem_mhz <= 0 || bytes_per_us <= 0) return;
    uint64_t latency = (uint64_t)((PerformanceModel::getParam("memory_tRCD") + PerformanceModel::getParam("memory_tCAS")) * cpu_mhz / mem_mhz);
    uint64_t transfer = (uint64_t)std::ceil(columns * hdr.num_lines * cpu_mhz / bytes_per_us);
    PerformanceModel::addCycles(latency + transfer);
    stat_stream_cycles += latency + transfer;
}

void POPT::set_block(uint64_t new_block) {
    // Advance only (destinations ascend); stream a column at every epoch boundary crossed.
    if (new_block <= block) return;
    for (uint64_t e = block / hdr.num_sub; e < new_block / hdr.num_sub; e++) enter_epoch(e + 1);
    block = new_block;
}

void POPT::enter_epoch(uint64_t epoch) {
    // The pinned next column becomes current; column epoch+1 is streamed into the freed reserved space.
    stat_epochs++;
    cur_col = &matrix[epoch * hdr.num_lines];
    if (epoch + 1 < hdr.num_epochs) {
        nxt_col = &matrix[(epoch + 1) * hdr.num_lines];
        stat_streamed_lines += (hdr.num_lines + 63) / 64;
        stream_columns(1);
    } else {
        nxt_col = nullptr;
    }
}

uint64_t POPT::find_next_ref(uint64_t line_id, std::vector<uint64_t>* reads) {
    // Algorithm 2, with the current sub-epoch known directly from the gather count. reads collects the pinned
    // columns' 64B blocks touched (current column: block line_id/64; next column: tagged with bit 63).
    if (reads) reads->push_back(line_id / 64);
    uint8_t cur = cur_col[line_id];
    if (cur & 0x80) return cur & 0x7F;
    uint64_t curr_sub = block % hdr.num_sub;
    if (curr_sub <= (uint64_t)(cur & 0x7F)) return 0;
    if (nxt_col == nullptr) return 127;               // last epoch: no further reference this traversal
    if (reads) reads->push_back((line_id / 64) | (1ULL << 63));
    uint8_t nxt = nxt_col[line_id];
    return (nxt & 0x80) ? std::min<uint64_t>(1 + (nxt & 0x7F), 127) : 1;   // 127 = none (sentinel), as the reference code
}

uint64_t POPT::next_ref_of(uint64_t addr, std::vector<uint64_t>* reads) const {
    // Matrix lines are 64B chunks of irregData counted from irreg_base; a cache line overlaps one of them, or two
    // when irreg_base is not 64B-aligned. The line is next needed when any of its elements is.
    uint64_t lo = addr <= irreg_base ? 0 : (addr - irreg_base) / CACHELINE_SIZE;
    uint64_t hi = std::min<uint64_t>((addr + CACHELINE_SIZE - 1 - irreg_base) / CACHELINE_SIZE, hdr.num_lines - 1);
    uint64_t best = find_next_ref(lo, reads);
    for (uint64_t l = lo + 1; l <= hi; l++) best = std::min(best, find_next_ref(l, reads));
    return best;
}

void POPT::charge_engine(uint64_t dram_cycles, bool writeback) {
    if (pending_reads == 0) return;
    // One extra read of the reserved ways in the bank the request is already at: the bank cycle time, not a
    // load-to-use latency. The paper's Table I has bank cycle 7 vs load-to-use 21 cycles; the same 1/3 ratio is
    // applied to our llc_latency (20 -> 7 cycles). Reads are pipelined (paper V-C), so they add up per read.
    uint64_t per_read = (PerformanceModel::getParam("llc_latency") + 2) / 3;
    uint64_t engine = pending_reads * per_read;
    uint64_t exposed = writeback ? 0 : (engine > dram_cycles ? engine - dram_cycles : 0);
    PerformanceModel::addCycles(exposed);
    stat_engine_cycles += engine;
    stat_exposed_cycles += exposed;
    (writeback ? stat_replacements_wb : dram_cycles ? stat_replacements_dram : stat_replacements_nodram)++;
    pending_reads = 0;
}

uint64_t POPT::reserved_ways(uint64_t num_sets, uint64_t line_size) {
    uint64_t way_bytes = num_sets * line_size;
    return (2 * hdr.num_lines + way_bytes - 1) / way_bytes;
}

void POPT::print_stats() {
    uint64_t tot = stat_evict_stream + stat_evict_opt;
    fmt::print("POPT evictions {} | non-irregData first {} ({:.2f}%) | farthest next-ref {} ({:.2f}%), of which SRRIP tie-break {} | "
               "next-ref at OPT evictions 0:{} 1:{} 2-126:{} 127+:{}\n",
        tot, stat_evict_stream, tot ? 100.0 * stat_evict_stream / tot : 0.0, stat_evict_opt,
        tot ? 100.0 * stat_evict_opt / tot : 0.0, stat_evict_tie,
        stat_ref_hist[0], stat_ref_hist[1], stat_ref_hist[2], stat_ref_hist[3]);
    fmt::print("POPT gathers {} of {} arcs | epochs entered {} | Rereference Matrix lines streamed {} ({:.2f} MB) = {} "
               "stop-the-world cycles\n", gathers, hdr.num_arcs, stat_epochs, stat_streamed_lines,
        stat_streamed_lines * 64.0 / (1 << 20), stat_stream_cycles);
    uint64_t reps = stat_replacements_dram + stat_replacements_nodram;
    fmt::print("POPT currDstID: destination stream {} ({} of {} vertices) | gathers in a different (epoch, sub-epoch) "
               "than the destination stream: {} | evictions outside the pull loop (SRRIP): {}\n",
        dst_pc ? fmt::format("{:#x}", dst_pc) : std::string("none (gather count)"), dst_count, hdr.num_nodes,
        stat_block_mismatch, stat_evict_outside);
    fmt::print("POPT next-ref engine: {} pinned-column block reads ({:.2f} per OPT replacement) = {} cycles, {} exposed "
               "| accesses with OPT replacements: {} on writeback fills (off the critical path), {} demand fills with a "
               "DRAM fetch, {} without\n",
        stat_matrix_reads, stat_evict_opt ? (double)stat_matrix_reads / stat_evict_opt : 0.0, stat_engine_cycles,
        stat_exposed_cycles, stat_replacements_wb, stat_replacements_dram, stat_replacements_nodram);
    (void)reps;
}

void POPT::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    SRRIP::fill_update(way_idx, block_idx, packet, was_accessed);
    line_addr[way_idx] = align_address(packet->address, CACHELINE_SIZE);
}

uint64_t POPT::get_eviction_candidate(bool is_low_priority) {
    // Outside the pull loop P-OPT is off: plain SRRIP (the reserved ways stay reserved).
    if (!in_window) {
        stat_evict_outside++;
        return SRRIP::get_eviction_candidate(is_low_priority);
    }
    // Data outside irregData (streaming) goes first: the first such way.
    for (uint64_t way = 0; way < num_ways; way++) {
        if (!is_irreg(line_addr[way])) {
            stat_evict_stream++;
            return way;
        }
    }
    // All irregData: farthest next reference from the pinned columns.
    std::vector<uint64_t> dist(num_ways), reads;
    uint64_t max_dist = 0;
    for (uint64_t way = 0; way < num_ways; way++) {
        dist[way] = next_ref_of(line_addr[way], &reads);
        max_dist = std::max(max_dist, dist[way]);
    }
    // Distinct pinned-column blocks read by the next-ref engine for this replacement.
    std::sort(reads.begin(), reads.end());
    uint64_t distinct = std::unique(reads.begin(), reads.end()) - reads.begin();
    pending_reads += distinct;
    stat_matrix_reads += distinct;
    stat_evict_opt++;
    stat_ref_hist[max_dist == 0 ? 0 : max_dist == 1 ? 1 : max_dist < 127 ? 2 : 3]++;
    if (std::count(dist.begin(), dist.end(), max_dist) > 1) stat_evict_tie++;
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
    line_addr[way_idx] = UINT64_MAX;
}
