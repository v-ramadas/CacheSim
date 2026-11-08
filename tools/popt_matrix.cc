// Builds P-OPT's Rereference Matrix (Balaji et al., HPCA 2021, Sec. IV-V)
// for a pull PageRank traversal of a GAP serialized graph (.sg), the
// preprocessing step the paper runs before execution.
//
// Irregular data: srcData = outgoing_contrib[v], elem_size bytes per vertex,
// read for every in-neighbour v of the destination u being processed (the
// outer loop runs u = 0 .. n-1; one gather per in-arc). Epochs are
// num_epochs equal ranges of destination vertices (EpochSize =
// ceil(n/num_epochs)), each split into 127 sub-epochs (SubEpochSize =
// ceil(EpochSize/127)). One byte per (epoch, srcData line), Fig. 6 format:
//   MSB 1: line not referenced in the epoch; low 7 bits = distance (in
//          epochs) to its next referencing epoch, 127 = none (sentinel).
//   MSB 0: line referenced in the epoch; low 7 bits = sub-epoch of its last
//          reference in the epoch.
//
// Output (little-endian):
//   header   PoptHeader below
//   check    first min(num_arcs, kCheck) in-neighbours in traversal order
//            (int32), so the simulator can check a trace against this graph
//   starts   (num_epochs * kSub + 1) uint64: index of the first gather (in-arc
//            in traversal order) of each (epoch, sub-epoch) block; the last
//            entry is num_arcs
//   matrix   num_epochs columns of num_lines bytes (column e = epoch e)
//
// pr (Gauss-Seidel, PageRankPullGS) also writes srcData[u] while processing destination u: with --writes-at-dst
// that write counts as a reference to u's line in u's sub-epoch (pr_spmv only reads srcData; leave it off).
//
// --neigh out: the traversal reads srcData[v] for the OUT-neighbours v of each destination u (cc_sv's hook pass:
// for u: for v in out_neigh(u): comp[v]); default in (pull PageRank: in_neigh). Write to <G>.<order>.out.popt.
//
// --elem-bits 1: srcData is a bitmap (1 bit per vertex, 512 vertices per 64B line; bfs bottom-up steps read the
// frontier bitmap for in_neigh(u)); the header stores elem_size 0. Write to <G>.<order>.bm.popt.
//
// Usage: popt_matrix <graph.sg> <out.popt> [--writes-at-dst] [--neigh in|out] [--elem-size N (4) | --elem-bits 1]
//        [--epochs N (256)]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

static constexpr uint32_t kSub = 127;
static constexpr uint32_t kCheck = 1024;
static constexpr uint32_t kLineSize = 64;

struct PoptHeader {
    char magic[8];            // "POPTRM2\0"
    uint64_t num_nodes;
    uint64_t num_arcs;
    uint64_t elem_size;
    uint64_t line_size;
    uint64_t num_epochs;
    uint64_t num_sub;
    uint64_t epoch_size;
    uint64_t sub_size;
    uint64_t num_lines;
    uint64_t num_check;
    uint64_t writes_at_dst;   // 1: srcData[u] is also referenced while processing u (pr Gauss-Seidel)
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <graph.sg> <out.popt> [--writes-at-dst] [--elem-size N] [--epochs N]\n", argv[0]);
        return 1;
    }
    uint64_t elem_size = 4, num_epochs = 256;
    bool writes_at_dst = false, neigh_out = false, bitmap = false;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--writes-at-dst")) writes_at_dst = true;
        else if (!std::strcmp(argv[i], "--neigh") && i + 1 < argc) {
            const char* d = argv[++i];
            if (!std::strcmp(d, "out")) neigh_out = true;
            else if (std::strcmp(d, "in")) { std::fprintf(stderr, "--neigh in|out\n"); return 1; }
        }
        else if (!std::strcmp(argv[i], "--elem-bits") && i + 1 < argc) {
            if (std::strcmp(argv[++i], "1")) { std::fprintf(stderr, "--elem-bits supports 1 (bitmap)\n"); return 1; }
            bitmap = true;
        }
        else if (!std::strcmp(argv[i], "--elem-size") && i + 1 < argc) elem_size = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--epochs") && i + 1 < argc) num_epochs = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
    }
    if (num_epochs < 2 || num_epochs > 1024 || elem_size == 0 || kLineSize % elem_size) {
        std::fprintf(stderr, "bad --epochs / --elem-size\n");
        return 1;
    }

    // GAP serialized graph (reader.h ReadSerializedGraph): bool directed,
    // int64 num_edges, int64 num_nodes, out offsets (n+1 int64), out neighbours
    // (num_edges int32), then for directed graphs the in (inverse) offsets and
    // neighbours. Undirected graphs store both directions once: in == out.
    std::ifstream f(argv[1], std::ios::binary);
    bool directed = false;
    int64_t num_edges = 0, num_nodes = 0;
    f.read(reinterpret_cast<char*>(&directed), sizeof(bool));
    f.read(reinterpret_cast<char*>(&num_edges), sizeof(num_edges));
    f.read(reinterpret_cast<char*>(&num_nodes), sizeof(num_nodes));
    if (!f || num_nodes <= 0 || num_edges < 0) {
        std::fprintf(stderr, "cannot read a GAP .sg header from %s\n", argv[1]);
        return 1;
    }
    const uint64_t n = num_nodes, m = num_edges;
    std::vector<int64_t> off(n + 1);
    std::vector<int32_t> neigh(m);
    f.read(reinterpret_cast<char*>(off.data()), (n + 1) * sizeof(int64_t));
    f.read(reinterpret_cast<char*>(neigh.data()), m * sizeof(int32_t));
    if (directed && !neigh_out) {   // replace with the inverse (in-neighbour) CSR
        f.read(reinterpret_cast<char*>(off.data()), (n + 1) * sizeof(int64_t));
        f.read(reinterpret_cast<char*>(neigh.data()), m * sizeof(int32_t));
    }
    if (!f || (uint64_t)off[n] != m) {
        std::fprintf(stderr, "truncated or inconsistent graph %s (offsets end %ld, edges %lu)\n", argv[1], (long)off[n], m);
        return 1;
    }

    PoptHeader h{};
    std::memcpy(h.magic, "POPTRM2", 8);
    h.num_nodes = n;
    h.num_arcs = m;
    h.elem_size = bitmap ? 0 : elem_size;
    h.line_size = kLineSize;
    h.num_epochs = num_epochs;
    h.num_sub = kSub;
    h.epoch_size = (n + num_epochs - 1) / num_epochs;
    h.sub_size = (h.epoch_size + kSub - 1) / kSub;
    const uint64_t per_line = bitmap ? kLineSize * 8 : kLineSize / elem_size;   // vertices per 64B line
    h.num_lines = (n + per_line - 1) / per_line;
    h.num_check = m < kCheck ? m : kCheck;
    h.writes_at_dst = writes_at_dst;

    // First gather of each (epoch, sub-epoch) block of destination vertices.
    std::vector<uint64_t> starts(num_epochs * kSub + 1);
    for (uint64_t e = 0; e < num_epochs; e++) {
        uint64_t epoch_begin = e * h.epoch_size, epoch_end = std::min<uint64_t>(n, (e + 1) * h.epoch_size);
        for (uint64_t s = 0; s < kSub; s++) {
            uint64_t u = std::min<uint64_t>(epoch_begin + s * h.sub_size, epoch_end);
            starts[e * kSub + s] = off[std::min<uint64_t>(u, n)];
        }
    }
    starts[num_epochs * kSub] = m;

    // Last referencing sub-epoch of each line in each epoch (-1 = none),
    // then the Fig. 6 entries.
    std::vector<int8_t> last(num_epochs * h.num_lines, -1);
    for (uint64_t u = 0; u < n; u++) {
        uint64_t e = u / h.epoch_size;
        int8_t s = (int8_t)((u - e * h.epoch_size) / h.sub_size);
        for (int64_t k = off[u]; k < off[u + 1]; k++)
            last[e * h.num_lines + (uint64_t)neigh[k] / per_line] = s;   // u ascends: the last write is the last reference
        if (writes_at_dst)
            last[e * h.num_lines + u / per_line] = s;                    // srcData[u] written after u's gathers
    }
    std::vector<uint8_t> matrix(num_epochs * h.num_lines);
    for (uint64_t c = 0; c < h.num_lines; c++) {
        uint64_t next = UINT64_MAX;   // next referencing epoch at or after e
        for (uint64_t e = num_epochs; e-- > 0;) {
            int8_t s = last[e * h.num_lines + c];
            if (s >= 0) {
                matrix[e * h.num_lines + c] = (uint8_t)s;   // MSB 0
                next = e;
            } else {
                uint64_t dist = next == UINT64_MAX ? 127 : std::min<uint64_t>(127, next - e);
                matrix[e * h.num_lines + c] = (uint8_t)(0x80 | dist);
            }
        }
    }

    std::ofstream o(argv[2], std::ios::binary);
    o.write(reinterpret_cast<const char*>(&h), sizeof(h));
    o.write(reinterpret_cast<const char*>(neigh.data()), h.num_check * sizeof(int32_t));
    o.write(reinterpret_cast<const char*>(starts.data()), starts.size() * sizeof(uint64_t));
    o.write(reinterpret_cast<const char*>(matrix.data()), matrix.size());
    if (!o) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("%s%s%s%s: %s, %lu vertices, %lu arcs, %lu lines (%lu B/elem, 0 = bit), %lu epochs of %lu vertices (%u sub-epochs of %lu) -> %s\n",
        writes_at_dst ? "[writes-at-dst] " : "", neigh_out ? "[out-neighbours] " : "", bitmap ? "[bitmap] " : "", argv[1], directed ? "directed" : "undirected", n, m, h.num_lines, h.elem_size, num_epochs, h.epoch_size,
        kSub, h.sub_size, argv[2]);
    return 0;
}
