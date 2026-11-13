// P-OPT transpose (push) mode: see the PushArray comment in inc/popt_replacement_policy.h.
#include "popt_replacement_policy.h"
#include "popt_matrix_build.h"
#include "trace_file.h"
#include "performance_model.h"
#include <fmt/core.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unistd.h>

namespace {
constexpr uint64_t kEpochs = 256, kSub = 128;   // as the HPCA'21 code
constexpr size_t kMaxPairs = 4096;

std::string exe_dir_push() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
}

// Every pair kept would exceed memory on long traces: keep a uniform sample (halve and double the stride when full).
struct PairSample {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    uint64_t stride = 1, seen = 0;
    void add(uint64_t c, uint64_t q) {
        if (seen++ % stride) return;
        if (v.size() == kMaxPairs) {
            for (size_t i = 0; i < kMaxPairs / 2; i++) v[i] = v[2 * i];
            v.resize(kMaxPairs / 2);
            stride *= 2;
            if ((seen - 1) % stride) return;
        }
        v.push_back({c, q});
    }
};
}   // namespace

bool POPT::load_graph(const std::string& base) {
    std::string g = base.substr(0, base.find_last_of('.'));
    std::vector<std::string> paths;
    if (!graph_dir.empty()) { paths.push_back(graph_dir + "/" + base + ".sg"); paths.push_back(graph_dir + "/" + g + "/" + base + ".sg"); }
    paths.push_back("../gapbs_traces/inputs/" + g + "/" + base + ".sg");
    paths.push_back(exe_dir_push() + "/../../gapbs_traces/inputs/" + g + "/" + base + ".sg");
    paths.push_back("data/graphs/" + base + ".sg");
    for (const auto& p : paths) {
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        bool directed = false;
        int64_t m = 0, n = 0;
        f.read(reinterpret_cast<char*>(&directed), sizeof(bool));
        f.read(reinterpret_cast<char*>(&m), sizeof(m));
        f.read(reinterpret_cast<char*>(&n), sizeof(n));
        if (!f || n <= 0 || m < 0) continue;
        g_n = n; g_m = m; g_directed = directed;
        for (int d = 0; d < (directed ? 2 : 1); d++) {
            g_off[d].resize(n + 1);
            g_nb[d].resize(m);
            f.read(reinterpret_cast<char*>(g_off[d].data()), (n + 1) * sizeof(int64_t));
            f.read(reinterpret_cast<char*>(g_nb[d].data()), m * sizeof(int32_t));
        }
        if (!f || (uint64_t)g_off[0][n] != g_m) {
            fmt::print(stderr, "POPT: truncated graph {}\n", p);
            return false;
        }
        fmt::print("POPT: transpose mode graph {} ({}, {} vertices, {} arcs)\n", p, directed ? "directed" : "undirected", n, m);
        return true;
    }
    fmt::print("POPT: transpose mode needs the graph {}.sg (--popt-graph-dir); not found\n", base);
    return false;
}

bool POPT::setup_push(const std::string& trace_path, const std::string& base) {
    if (!load_graph(base)) return false;
    const uint64_t n = g_n, m = g_m;
    // Pass 1: per-PC read counts, +4/+8 strides and address range.
    struct Stat { uint64_t reads = 0, s4 = 0, s8 = 0, last = 0, rmin = UINT64_MAX, rmax = 0; };
    std::unordered_map<uint64_t, Stat> st;
    std::string line;
    char action[16];
    uint64_t inst, pc, addr;
    {
        TraceFile trace(trace_path);
        if (!trace.is_open()) return false;
        while (trace.getline(line)) {
            if (std::sscanf(line.c_str(), "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx", &inst, &pc, action, &addr) != 4) continue;
            if (strcmp(action, "Read") != 0) continue;
            auto& s = st[pc];
            if (s.reads) { if (addr - s.last == 4) s.s4++; else if (addr - s.last == 8) s.s8++; }
            s.last = addr; s.reads++;
            s.rmin = std::min(s.rmin, addr); s.rmax = std::max(s.rmax, addr);
        }
    }
    // Neighbour-array candidates: sequential (+4 / +8) for at least 3% of their reads, span within m entries.
    struct Cand { uint64_t pc, stride; Stat s; };
    std::vector<Cand> cands;
    std::vector<uint64_t> qpcs;
    for (const auto& [p, s] : st) {
        if (s.reads < 1000) continue;
        qpcs.push_back(p);
        uint64_t stride = s.s8 > s.s4 ? 8 : 4, seq = std::max(s.s4, s.s8);
        if (seq * 100 < 3 * s.reads || s.rmax - s.rmin >= m * stride) continue;
        cands.push_back({p, stride, s});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.s.reads > b.s.reads; });
    if (cands.size() > 6) cands.resize(6);
    if (cands.empty()) return false;
    // Pass 2: for each candidate P and each other PC Q, (P's read, Q's first read after it) within 8 trace lines.
    std::vector<std::unordered_map<uint64_t, PairSample>> pairs(cands.size());
    {
        std::vector<uint64_t> last_c(cands.size(), 0), last_line(cands.size(), 0), seq(cands.size(), 0);
        std::vector<std::unordered_map<uint64_t, uint64_t>> used(cands.size());
        std::unordered_map<uint64_t, int> cidx;
        for (size_t i = 0; i < cands.size(); i++) cidx[cands[i].pc] = (int)i;
        std::unordered_map<uint64_t, bool> isq;
        for (auto p : qpcs) isq[p] = true;
        TraceFile trace(trace_path);
        if (!trace.is_open()) return false;
        uint64_t li = 0;
        while (trace.getline(line)) {
            li++;
            if (std::sscanf(line.c_str(), "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx", &inst, &pc, action, &addr) != 4) continue;
            if (strcmp(action, "Read") != 0) continue;
            auto ci = cidx.find(pc);
            if (ci != cidx.end()) { int i = ci->second; last_c[i] = addr; last_line[i] = li; seq[i]++; }
            if (!isq.count(pc)) continue;
            for (size_t i = 0; i < cands.size(); i++) {
                if (cands[i].pc == pc || !seq[i] || li - last_line[i] > 8) continue;
                auto& u = used[i][pc];
                if (u == seq[i]) continue;
                u = seq[i];
                pairs[i][pc].add(last_c[i], addr);
            }
        }
    }
    hdr = Header{};
    hdr.num_nodes = n; hdr.num_arcs = m; hdr.num_epochs = kEpochs; hdr.num_sub = kSub;
    hdr.epoch_size = (n + kEpochs - 1) / kEpochs; hdr.sub_size = (hdr.epoch_size + kSub - 1) / kSub;
    hdr.line_size = cachesim::BLOCK_SIZE;
    // Element address of neighbour v: base + elem * v, or base + 8 * (v / 64) for a frontier bitmap (elem 0).
    auto eaddr = [](uint64_t elem, int32_t v) { return elem ? elem * (uint64_t)v : 8 * ((uint64_t)v >> 6); };
    // Bases implied by the pairs for neighbour-array base nbase: (base, count) clusters, largest first.
    auto clusters = [&](const std::vector<std::pair<uint64_t, uint64_t>>& pv, uint64_t nbase, uint64_t stride, int csr,
                        uint64_t elem) {
        std::unordered_map<uint64_t, uint64_t> h;
        for (auto [c, q] : pv) {
            if (c < nbase || (c - nbase) % stride) continue;
            uint64_t k = (c - nbase) / stride;
            if (k >= m) continue;
            h[q - eaddr(elem, g_nb[csr][k])]++;
        }
        std::vector<std::pair<uint64_t, uint64_t>> v(h.begin(), h.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        return v;
    };
    const uint64_t elems[3] = {4, 8, 0};
    for (size_t i = 0; i < cands.size(); i++) {
        const auto& c = cands[i];
        std::vector<std::pair<uint64_t, const PairSample*>> qs;
        for (const auto& [q, ps] : pairs[i]) if (ps.v.size() >= 256) qs.push_back({q, &ps});
        std::sort(qs.begin(), qs.end(), [](auto& a, auto& b) { return a.second->seen > b.second->seen; });
        if (qs.size() > 4) qs.resize(4);
        bool found = false;
        for (int csr = 0; csr < (g_directed ? 2 : 1) && !found; csr++) {
            for (auto [q, ps] : qs) {
                const auto& pv = ps->v;
                for (uint64_t elem : elems) {
                    // Search the neighbour-array base: from the lowest read down to the lowest base that keeps every
                    // read inside the m entries; the first 8 pairs must agree on one array base (a bitmap's pairs too:
                    // consecutive pairs fall in the same traversal step).
                    uint64_t c0 = pv[0].first;
                    uint64_t hi = c.s.rmin - ((c.s.rmin - c0 % c.stride) % c.stride);
                    uint64_t lo = c.s.rmax >= c.stride * (m - 1) ? c.s.rmax - c.stride * (m - 1) : 0;
                    for (uint64_t nbase = hi; nbase + c.stride > lo && nbase >= lo && !found; nbase -= c.stride) {
                        uint64_t k0 = (c0 - nbase) / c.stride;
                        if (k0 >= m) break;
                        uint64_t a = pv[0].second - eaddr(elem, g_nb[csr][k0]);
                        bool ok = true;
                        for (size_t j = 1; j < 8 && j < pv.size() && ok; j++) {
                            uint64_t k = (pv[j].first - nbase) / c.stride;
                            ok = pv[j].first >= nbase && k < m && pv[j].second == a + eaddr(elem, g_nb[csr][k]);
                        }
                        if (!ok) { if (nbase < c.stride) break; continue; }
                        auto cl = clusters(pv, nbase, c.stride, csr, elem);
                        uint64_t top = cl[0].second + (elem == 0 && cl.size() > 1 ? cl[1].second : 0);
                        if (top * 10 >= pv.size() * 9) {
                            pnas.push_back({c.pc, nbase, c.stride, c.s.reads, csr});
                            found = true;
                        }
                        if (nbase < c.stride) break;
                    }
                    if (found) break;
                }
                if (found) break;
            }
        }
        if (!found) continue;
        // Arrays read as x[nb[k]] after this neighbour array: every Q whose pairs agree (>= 90%) on one base (two
        // for a bitmap: GAP swaps the frontier bitmaps every step).
        const auto& na = pnas.back();
        for (const auto& [q, ps] : pairs[i]) {
            if (ps.v.size() < 256) continue;
            for (uint64_t elem : elems) {
                auto cl = clusters(ps.v, na.base, na.stride, na.csr, elem);
                if (cl.empty()) continue;
                bool two = elem == 0 && cl.size() > 1 && cl[1].second * 10 >= ps.v.size();
                uint64_t top = cl[0].second + (two ? cl[1].second : 0);
                if (top * 10 < ps.v.size() * 9) continue;
                for (int j = 0; j < (two ? 2 : 1); j++) {
                    uint64_t b = cl[j].first;
                    bool dup = false;
                    for (const auto& a : parrs) dup |= a.base == b && a.elem == elem;
                    if (dup) continue;
                    PushArray a;
                    a.base = b; a.elem = elem; a.csr = na.csr; a.pc = q;
                    a.bound = b + (elem ? n * elem : (n + 63) / 64 * 8);
                    parrs.push_back(a);
                }
                break;
            }
        }
    }
    if (pnas.empty() || parrs.empty()) {
        pnas.clear(); parrs.clear();
        hdr = Header{};
        return false;
    }
    // One matrix per array, on the array's own block grid (the HPCA'21 code allocates arrays block-aligned).
    const uint64_t L = cachesim::BLOCK_SIZE;
    for (auto& a : parrs) {
        a.align = a.base % L;
        a.per_line = a.elem ? L / a.elem : L * 8;
        if (a.elem && (a.align % a.elem || L % a.elem)) {
            fmt::print(stderr, "POPT: array {:#x} ({}B elements) is not element-aligned to the {}B blocks\n", a.base, a.elem, L);
            return false;
        }
        a.off_v = a.elem ? a.align / a.elem : a.align * 8;
        a.num_lines = (n + a.off_v + a.per_line - 1) / a.per_line;
        popt_build_matrix(n, g_off[a.csr].data(), g_nb[a.csr].data(), kEpochs, kSub, hdr.epoch_size, hdr.sub_size,
                          a.per_line, a.off_v, a.num_lines, a.matrix);
        a.cur = a.matrix.data();
        a.nxt = a.matrix.data() + a.num_lines;
        hdr.num_lines += a.num_lines;
    }
    push_mode = active = true;
    push_block = 0;
    for (const auto& na : pnas)
        fmt::print("POPT: transpose mode neighbour array PC {:#x} at {:#x} ({}B entries, {} CSR, {} reads) sets currDstID\n",
            na.pc, na.base, na.stride, na.csr ? "in" : "out", na.reads);
    for (const auto& a : parrs)
        fmt::print("POPT: transpose mode irregular array {:#x}..{:#x} ({}, {} CSR, PC {:#x}, {}B into a {}B block) | matrix "
                   "{} lines of {} vertices\n", a.base, a.bound, a.elem ? fmt::format("{}B elements", a.elem) : "frontier bitmap",
            a.csr ? "in" : "out", a.pc, a.align, L, a.num_lines, a.per_line);
    fmt::print("POPT: transpose mode | {} epochs of {} vertices, {} sub-epochs of {} | pinned columns {} B\n", kEpochs,
        hdr.epoch_size, kSub, hdr.sub_size, 2 * hdr.num_lines);
    return true;
}

void POPT::push_stream(uint64_t columns) {
    double cpu_mhz = PerformanceModel::getParam("cpu_frequency");
    double mem_mhz = PerformanceModel::getParam("memory_frequency");
    double bytes_per_us = (double)PerformanceModel::getParam("memory_data_rate") * PerformanceModel::getParam("memory_channel_width")
                        * std::max<uint64_t>(1, PerformanceModel::getParam("memory_channels"));
    stat_streamed_lines += columns * ((hdr.num_lines + 63) / 64);
    if (cpu_mhz <= 0 || mem_mhz <= 0 || bytes_per_us <= 0) return;
    uint64_t latency = (uint64_t)((PerformanceModel::getParam("memory_tRCD") + PerformanceModel::getParam("memory_tCAS")) * cpu_mhz / mem_mhz);
    uint64_t transfer = (uint64_t)std::ceil(columns * hdr.num_lines * cpu_mhz / bytes_per_us);
    PerformanceModel::addCycles(latency + transfer);
    stat_stream_cycles += latency + transfer;
}

void POPT::push_set_block(uint64_t new_block) {
    uint64_t e_old = push_block / kSub, e_new = new_block / kSub;
    push_block = new_block;
    if (e_new == e_old) return;
    for (auto& a : parrs) {
        a.cur = a.matrix.data() + e_new * a.num_lines;
        a.nxt = e_new + 1 < kEpochs ? a.matrix.data() + (e_new + 1) * a.num_lines : nullptr;
    }
    stat_epochs++;
    if (e_new == e_old + 1) {   // the pinned next column becomes current; the new next column is streamed in
        stat_push_epoch_steps++;
        if (e_new + 1 < kEpochs) push_stream(1);
    } else {                    // the clock jumped: both columns are streamed in again
        stat_push_epoch_jumps++;
        push_stream(e_new + 1 < kEpochs ? 2 : 1);
    }
}

void POPT::on_access_push(uint64_t pc, uint64_t addr) {
    if (!streamed_initial) { streamed_initial = true; push_stream(2); }
    for (const auto& na : pnas) {
        if (pc != na.pc) continue;
        if (addr < na.base) return;
        uint64_t k = (addr - na.base) / na.stride;
        if (k >= g_m) return;
        const auto& off = g_off[na.csr];
        uint64_t u = std::upper_bound(off.begin(), off.end(), (int64_t)k) - off.begin() - 1;
        stat_push_updates++;
        uint64_t e = u / hdr.epoch_size;
        push_set_block(e * kSub + (u - e * hdr.epoch_size) / hdr.sub_size);
        return;
    }
}

uint64_t POPT::push_next_ref(int arr, uint64_t addr, std::vector<uint64_t>* reads) {
    // Algorithm 2 on this array's pinned columns; the block is one matrix line (its first element's).
    const auto& a = parrs[arr];
    uint64_t line = std::min<uint64_t>((addr - (a.base - a.align)) / hdr.line_size, a.num_lines - 1);
    uint64_t tag = (uint64_t)arr << 48;
    if (reads) reads->push_back(tag | (line / 64));
    uint8_t cur = a.cur[line];
    if (cur & 0x80) return cur & 0x7F;
    if (push_block % kSub <= (uint64_t)(cur & 0x7F)) return 0;
    if (a.nxt == nullptr) return 127;
    if (reads) reads->push_back(tag | (line / 64) | (1ULL << 63));
    uint8_t nxt = a.nxt[line];
    return (nxt & 0x80) ? std::min<uint64_t>(1 + (nxt & 0x7F), 127) : 1;
}

uint64_t POPT::push_eviction_candidate() {
    for (uint64_t way = 0; way < num_ways; way++)
        if (line_addr[way] != UINT64_MAX && push_array_of(line_addr[way]) < 0) {
            stat_evict_stream++;
            return way;
        }
    std::vector<uint64_t> dist(num_ways, UINT64_MAX), reads;
    uint64_t max_dist = 0;
    for (uint64_t way = 0; way < num_ways; way++) {
        if (line_addr[way] == UINT64_MAX) continue;
        dist[way] = push_next_ref(push_array_of(line_addr[way]), line_addr[way], &reads);
        max_dist = std::max(max_dist, dist[way]);
    }
    std::sort(reads.begin(), reads.end());
    uint64_t distinct = std::unique(reads.begin(), reads.end()) - reads.begin();
    pending_reads += distinct;
    stat_matrix_reads += distinct;
    stat_evict_opt++;
    stat_ref_hist[max_dist == 0 ? 0 : max_dist == 1 ? 1 : max_dist < 127 ? 2 : 3]++;
    if (std::count(dist.begin(), dist.end(), max_dist) > 1) stat_evict_tie++;
    // Ties exactly as the HPCA'21 code (see get_eviction_candidate).
    for (uint64_t way = 0; way < num_ways; way++)
        if (dist[way] == max_dist && counter[way] >= maxRRPV) return way;
    if (max_dist > 0)
        for (uint64_t way = 0; way < num_ways; way++)
            if (dist[way] == max_dist) return way;
    while (true) {
        for (uint64_t way = 0; way < num_ways; way++)
            if (dist[way] == max_dist && counter[way] < maxRRPV) counter[way]++;
        for (uint64_t way = 0; way < num_ways; way++)
            if (dist[way] == max_dist && counter[way] >= maxRRPV) return way;
    }
}

void POPT::push_print_stats() {
    uint64_t tot = stat_evict_stream + stat_evict_opt;
    fmt::print("POPT (transpose mode) evictions {} | non-irregData first {} ({:.2f}%) | farthest next-ref {} ({:.2f}%), of "
               "which SRRIP tie-break {} | next-ref at OPT evictions 0:{} 1:{} 2-126:{} 127+:{}\n",
        tot, stat_evict_stream, tot ? 100.0 * stat_evict_stream / tot : 0.0, stat_evict_opt,
        tot ? 100.0 * stat_evict_opt / tot : 0.0, stat_evict_tie,
        stat_ref_hist[0], stat_ref_hist[1], stat_ref_hist[2], stat_ref_hist[3]);
    fmt::print("POPT (transpose mode) clock updates {} | epoch changes {} (to the next epoch {}, jumps {}) | Rereference "
               "Matrix lines streamed {} ({:.2f} MB) = {} stop-the-world cycles\n", stat_push_updates, stat_epochs,
        stat_push_epoch_steps, stat_push_epoch_jumps, stat_streamed_lines, stat_streamed_lines * 64.0 / (1 << 20),
        stat_stream_cycles);
    fmt::print("POPT next-ref engine: {} pinned-column block reads ({:.2f} per OPT replacement) = {} cycles, {} exposed "
               "| accesses with OPT replacements: {} on writeback fills (off the critical path), {} demand fills with a "
               "DRAM fetch, {} without\n",
        stat_matrix_reads, stat_evict_opt ? (double)stat_matrix_reads / stat_evict_opt : 0.0, stat_engine_cycles,
        stat_exposed_cycles, stat_replacements_wb, stat_replacements_dram, stat_replacements_nodram);
}
