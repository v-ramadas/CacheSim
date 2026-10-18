// Standalone test: do the tags of an 8B-block LLC compress with a Base+Delta
// (BDI-style) scheme?
//
// Model (approximation of CacheSim's 2-level setup, not bit-exact):
//   - L1D: l1_sets x l1_ways 64B lines, LRU; only L1 misses reach the LLC.
//   - LLC: sets x ways 8B blocks, LRU, inserted on every LLC miss.
//     set index = line address & (sets-1); line tag = addr >> (6 + log2 sets);
//     sector = (addr >> 3) & 7  (same split as get_total_cache_size: 51 tag bits).
// Every snap_every LLC accesses, every set's resident blocks are compressed:
//   scheme: up to B base tags per set; a block is covered if
//   line_tag - base fits in a signed d-bit delta; entry = (base id, delta, sector).
//   - "online": bases picked in insertion order (oldest first), a block that
//     fits no base becomes a new base while < B bases exist (BDI's
//     "first uncompressible element becomes the base").
//   - "offline": greedy max-coverage choice of B windows of width 2^d (upper bound
//     for B bases).
// Correctness check: every online-covered block is encoded and decoded back;
// any mismatch is counted and makes the program exit non-zero.
//
// Build: g++ -O2 -std=c++20 -o tag_bdi_test tag_bdi_test.cc
// Run:   ./tag_bdi_test <trace.xz|trace.log> [sets ways l1_sets l1_ways snap_every]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct Way { uint64_t blk = UINT64_MAX; uint64_t ts = 0; };

struct Cache {
    uint64_t sets, ways, set_bits, line_shift;  // line_shift: 6 for 64B lines, 3 for 8B blocks
    std::vector<Way> w;
    std::unordered_map<uint64_t, uint32_t> where;  // block -> way slot (global index)
    uint64_t clock = 0;
    Cache(uint64_t s, uint64_t ways_, uint64_t shift) : sets(s), ways(ways_), line_shift(shift), w(s * ways_) {
        set_bits = 0; while ((1ULL << set_bits) < s) set_bits++;
        where.reserve(s * ways_ * 2);
    }
    uint64_t set_of(uint64_t addr) const { return (addr >> 6) & (sets - 1); }
    // returns true on hit; inserts on miss (LRU victim)
    bool access(uint64_t addr) {
        uint64_t blk = addr >> line_shift;
        ++clock;
        auto it = where.find(blk);
        if (it != where.end()) { w[it->second].ts = clock; return true; }
        uint64_t base = set_of(addr) * ways, victim = base;
        for (uint64_t i = base; i < base + ways; i++) {
            if (w[i].blk == UINT64_MAX) { victim = i; break; }
            if (w[i].ts < w[victim].ts) victim = i;
        }
        if (w[victim].blk != UINT64_MAX) where.erase(w[victim].blk);
        w[victim] = {blk, clock};
        where[blk] = (uint32_t)victim;
        return false;
    }
};

static const int DS[] = {4, 6, 8, 10, 12, 16};
static const int BS[] = {1, 2, 4, 8};
static const int ND = sizeof(DS) / sizeof(DS[0]), NB = sizeof(BS) / sizeof(BS[0]);

struct Stats {
    uint64_t snaps = 0, set_snaps = 0, blocks = 0, distinct_lines = 0, roundtrip_bad = 0, roundtrip_ok = 0;
    uint64_t online[ND][NB] = {}, offline[ND][NB] = {};
    uint64_t max_tag = 0;
};

static bool fits(int64_t delta, int d) {
    int64_t lo = -(1LL << (d - 1)), hi = (1LL << (d - 1)) - 1;
    return delta >= lo && delta <= hi;
}

static void analyse(const Cache& llc, Stats& st) {
    st.snaps++;
    struct B { uint64_t tag, sector, ts; };
    std::vector<B> v; v.reserve(llc.ways);
    for (uint64_t s = 0; s < llc.sets; s++) {
        v.clear();
        for (uint64_t i = s * llc.ways; i < (s + 1) * llc.ways; i++) {
            if (llc.w[i].blk == UINT64_MAX) continue;
            uint64_t addr = llc.w[i].blk << 3;
            v.push_back({addr >> (6 + llc.set_bits), (addr >> 3) & 7, llc.w[i].ts});
        }
        if (v.empty()) continue;
        st.set_snaps++; st.blocks += v.size();
        std::sort(v.begin(), v.end(), [](const B& a, const B& b) { return a.ts < b.ts; });
        // distinct line tags
        std::vector<uint64_t> tags; for (auto& b : v) { tags.push_back(b.tag); st.max_tag = std::max(st.max_tag, b.tag); }
        std::vector<uint64_t> uniq = tags; std::sort(uniq.begin(), uniq.end());
        std::vector<std::pair<uint64_t, uint64_t>> cnt;  // (tag, count)
        for (auto t : uniq) { if (cnt.empty() || cnt.back().first != t) cnt.push_back({t, 0}); cnt.back().second++; }
        st.distinct_lines += cnt.size();
        for (int di = 0; di < ND; di++) {
            int d = DS[di];
            for (int bi = 0; bi < NB; bi++) {
                int Bmax = BS[bi];
                // online greedy (insertion order) + encode/decode round trip
                std::vector<uint64_t> bases;
                uint64_t covered = 0;
                for (auto& b : v) {
                    int id = -1;
                    for (size_t k = 0; k < bases.size(); k++)
                        if (fits((int64_t)(b.tag - bases[k]), d)) { id = (int)k; break; }
                    if (id < 0 && (int)bases.size() < Bmax) { bases.push_back(b.tag); id = (int)bases.size() - 1; }
                    if (id < 0) continue;
                    covered++;
                    // encode: (id, delta truncated to d bits two's complement, sector); decode back
                    int64_t delta = (int64_t)(b.tag - bases[id]);
                    uint64_t field = (uint64_t)delta & ((1ULL << d) - 1);
                    int64_t back = (field & (1ULL << (d - 1))) ? (int64_t)(field | ~((1ULL << d) - 1)) : (int64_t)field;
                    uint64_t tag2 = bases[id] + (uint64_t)back;
                    if (tag2 != b.tag) st.roundtrip_bad++; else st.roundtrip_ok++;
                }
                st.online[di][bi] += covered;
                // offline greedy max coverage over windows of width 2^d
                std::vector<std::pair<uint64_t, uint64_t>> rem = cnt;
                uint64_t width = 1ULL << d, cov = 0;
                for (int k = 0; k < Bmax && !rem.empty(); k++) {
                    uint64_t best = 0, sum = 0; size_t bl = 0, br = 0, l = 0;
                    for (size_t r = 0; r < rem.size(); r++) {
                        sum += rem[r].second;
                        while (rem[r].first - rem[l].first >= width) sum -= rem[l++].second;
                        if (sum > best) { best = sum; bl = l; br = r; }
                    }
                    cov += best;
                    rem.erase(rem.begin() + bl, rem.begin() + br + 1);
                }
                st.offline[di][bi] += cov;
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s trace [sets ways l1_sets l1_ways snap_every]\n", argv[0]); return 2; }
    std::string path = argv[1];
    uint64_t sets = argc > 2 ? atoll(argv[2]) : 1024, ways = argc > 3 ? atoll(argv[3]) : 256;
    uint64_t l1s = argc > 4 ? atoll(argv[4]) : 128, l1w = argc > 5 ? atoll(argv[5]) : 16;
    uint64_t snap_every = argc > 6 ? atoll(argv[6]) : 2000000;
    std::string cmd = (path.size() > 3 && path.substr(path.size() - 3) == ".xz") ? "xz -dc '" + path + "'" : "cat '" + path + "'";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) { perror("popen"); return 2; }
    Cache l1(l1s, l1w, 6), llc(sets, ways, 3);
    Stats st;
    char line[512]; uint64_t llc_acc = 0, recs = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "instCount:", 10) != 0) continue;
        char* p = strstr(line, ",Read:0x"); if (!p) p = strstr(line, ",Write:0x");
        if (!p) continue;
        p = strchr(p, 'x') + 1;
        uint64_t addr = strtoull(p, nullptr, 16);
        recs++;
        if (l1.access(addr)) continue;
        llc.access(addr);
        if (++llc_acc % snap_every == 0) analyse(llc, st);
    }
    pclose(f);
    if (st.snaps == 0) analyse(llc, st);
    int tag_bits = 0; while ((1ULL << tag_bits) <= st.max_tag) tag_bits++;
    printf("trace %s\nrecords %lu llc_accesses %lu snapshots %lu set_snapshots %lu\n", path.c_str(), recs, llc_acc, st.snaps, st.set_snaps);
    printf("resident blocks per set %.1f  distinct 64B lines per set %.1f  blocks per line %.2f  max line-tag bits %d\n",
           (double)st.blocks / st.set_snaps, (double)st.distinct_lines / st.set_snaps, (double)st.blocks / st.distinct_lines, tag_bits);
    printf("roundtrip ok %lu bad %lu\n", st.roundtrip_ok, st.roundtrip_bad);
    printf("coverage = share of resident blocks a (B bases, d-bit delta) scheme can hold; online / offline-bound\n");
    printf("%4s", "d");
    for (int bi = 0; bi < NB; bi++) printf(" | B=%d online  offline", BS[bi]);
    printf("\n");
    for (int di = 0; di < ND; di++) {
        printf("%4d", DS[di]);
        for (int bi = 0; bi < NB; bi++)
            printf(" |   %6.1f%%  %6.1f%%", 100.0 * st.online[di][bi] / st.blocks, 100.0 * st.offline[di][bi] / st.blocks);
        printf("\n");
    }
    return st.roundtrip_bad ? 1 : 0;
}
