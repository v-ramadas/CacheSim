// Builds P-OPT's Rereference Matrix (Balaji et al., HPCA 2021, Sec. IV-V)
// for a pull PageRank traversal of a GAP serialized graph (.sg), the
// preprocessing step the paper runs before execution.
//
// Irregular data: srcData = outgoing_contrib[v], elem_size bytes per vertex,
// read for every in-neighbour v of the destination u being processed (the
// outer loop runs u = 0 .. n-1; one gather per in-arc). Epochs are
// num_epochs equal ranges of destination vertices (EpochSize =
// ceil(n/num_epochs)), each split into 128 sub-epochs (SubEpochSize =
// ceil(EpochSize/128)), as the HPCA'21 code (builder.h makeOffsetMatrix, llc.cpp numSubEpochs = numEpochs/2).
// One byte per (epoch, srcData line), Fig. 6 format:
//   MSB 1: line not referenced in the epoch; low 7 bits = distance (in
//          epochs) to its next referencing epoch, 127 = none (sentinel).
//   MSB 0: line referenced in the epoch; low 7 bits = sub-epoch of its last
//          reference in the epoch. In the last epoch a referenced line stores 127
//          (as the HPCA'21 code: it stays "to be referenced" for the whole epoch).
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
// As the HPCA'21 builder, only the neighbour reads count as references (pr's Gauss-Seidel write of srcData[u] at
// the destination is not encoded).
//
// --align-offset B (default 0): the traced array starts B bytes into a line_size block. The HPCA'21 code allocates
// irregData line-aligned, so each cache block holds exactly one matrix line; with B != 0 the lines follow the cache
// block grid of the real array instead of starting at vertex 0 (line 0 holds the vertices of the first, partial
// block). B is stored in the header (align_offset). Write to <G>.<order>[.variant][.l<N>].a<B>.popt.
//
// --neigh out: the traversal reads srcData[v] for the OUT-neighbours v of each destination u (cc_sv's hook pass:
// for u: for v in out_neigh(u): comp[v]); default in (pull PageRank: in_neigh). Write to <G>.<order>.out.popt.
//
// --elem-bits 1: srcData is a bitmap (1 bit per vertex, 512 vertices per 64B line; bfs bottom-up steps read the
// frontier bitmap for in_neigh(u)); the header stores elem_size 0. Write to <G>.<order>.bm.popt.
//
// --line-size N (default 64): bytes of srcData per matrix line (8, 16, 32 or 64). 8 gives one entry per 8B block,
// for a sub-64B LLC; the matrix (and the LLC space P-OPT pins for it) grows by 64/N. Write to
// <G>.<order>[.variant].l<N>.popt (no suffix for 64).
//
// Usage: popt_matrix <graph.sg> <out.popt> [--neigh in|out] [--elem-size N (4) | --elem-bits 1]
//        [--epochs N (256)] [--line-size N (64)] [--align-offset B (0)]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include "../inc/popt_matrix_build.h"

static constexpr uint32_t kSub = 128;
static constexpr uint32_t kCheck = 1024;

struct PoptHeader {
    char magic[8];            // "POPTRM3\0" (3: 128 sub-epochs and the HPCA'21 last-epoch rule)
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
    uint64_t align_offset;    // bytes into a line_size block where the array starts (formerly writes_at_dst, always 0)
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <graph.sg> <out.popt> [--neigh in|out] [--elem-bits 1] "
                             "[--elem-size N] [--epochs N] [--line-size N] [--align-offset B]\n", argv[0]);
        return 1;
    }
    uint64_t elem_size = 4, num_epochs = 256, line_size = 64, align_offset = 0;
    bool neigh_out = false, bitmap = false;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--neigh") && i + 1 < argc) {
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
        else if (!std::strcmp(argv[i], "--line-size") && i + 1 < argc) line_size = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--align-offset") && i + 1 < argc) align_offset = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
    }
    if (line_size != 8 && line_size != 16 && line_size != 32 && line_size != 64) {
        std::fprintf(stderr, "--line-size must be 8, 16, 32 or 64\n");
        return 1;
    }
    if (align_offset >= line_size || (!bitmap && align_offset % elem_size)) {
        std::fprintf(stderr, "--align-offset must be below --line-size and a multiple of --elem-size\n");
        return 1;
    }
    if (num_epochs < 2 || num_epochs > 1024 || elem_size == 0 || (!bitmap && line_size % elem_size)) {
        std::fprintf(stderr, "bad --epochs / --elem-size (must divide --line-size)\n");
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
    std::memcpy(h.magic, "POPTRM3", 8);
    h.num_nodes = n;
    h.num_arcs = m;
    h.elem_size = bitmap ? 0 : elem_size;
    h.line_size = line_size;
    h.num_epochs = num_epochs;
    h.num_sub = kSub;
    h.epoch_size = (n + num_epochs - 1) / num_epochs;
    h.sub_size = (h.epoch_size + kSub - 1) / kSub;
    const uint64_t per_line = bitmap ? line_size * 8 : line_size / elem_size;   // vertices per matrix line
    const uint64_t off_v = bitmap ? align_offset * 8 : align_offset / elem_size;   // vertices before line 0's first
    h.num_lines = (n + off_v + per_line - 1) / per_line;
    h.num_check = m < kCheck ? m : kCheck;
    h.align_offset = align_offset;

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

    // Fig. 6 entries (inc/popt_matrix_build.h, shared with the simulator's transpose mode).
    std::vector<uint8_t> matrix;
    popt_build_matrix(n, off.data(), neigh.data(), num_epochs, kSub, h.epoch_size, h.sub_size, per_line, off_v,
                      h.num_lines, matrix);

    std::ofstream o(argv[2], std::ios::binary);
    o.write(reinterpret_cast<const char*>(&h), sizeof(h));
    o.write(reinterpret_cast<const char*>(neigh.data()), h.num_check * sizeof(int32_t));
    o.write(reinterpret_cast<const char*>(starts.data()), starts.size() * sizeof(uint64_t));
    o.write(reinterpret_cast<const char*>(matrix.data()), matrix.size());
    if (bitmap) {
        // Trailer (bitmap matrices only): every kSampleStride-th neighbour of the traversal's neighbour array, so the
        // simulator can validate partial traversals (bfs bottom-up skips visited vertices) beyond the prefix.
        constexpr uint64_t kSampleStride = 16;
        std::vector<int32_t> samples;
        for (uint64_t k = 0; k < m; k += kSampleStride) samples.push_back(neigh[k]);
        uint64_t stride = kSampleStride, count = samples.size();
        o.write("POPTSMP1", 8);
        o.write(reinterpret_cast<const char*>(&stride), sizeof(stride));
        o.write(reinterpret_cast<const char*>(&count), sizeof(count));
        o.write(reinterpret_cast<const char*>(samples.data()), count * sizeof(int32_t));
    }
    if (!o) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("%s%s%s: %s, %lu vertices, %lu arcs, %lu lines of %lu B (offset %lu, %lu B/elem, 0 = bit), %lu epochs of %lu vertices (%u sub-epochs of %lu) -> %s\n",
        neigh_out ? "[out-neighbours] " : "", bitmap ? "[bitmap] " : "", argv[1], directed ? "directed" : "undirected", n, m, h.num_lines, line_size, align_offset, h.elem_size, num_epochs, h.epoch_size,
        kSub, h.sub_size, argv[2]);
    return 0;
}
