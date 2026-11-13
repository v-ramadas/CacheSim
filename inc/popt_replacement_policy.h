#ifndef __POPT_REPLACEMENT_POLICY_H__
#define __POPT_REPLACEMENT_POLICY_H__

#include "srrip_replacement_policy.h"
#include <string>
#include <vector>
#include <cstdlib>

// P-OPT (Balaji, Crago, Jaleel, Lucia, HPCA 2021) on SRRIP, following the HPCA'21 code (POPT-CacheSim-HPCA21).
//
// Two ways of getting the Rereference Matrix and the clock (currDstID, the vertex being processed):
// - Pull mode (vertex-ordered traversals, as the paper: pr, pr_spmv, cc_sv's hook pass): matrix files built offline
//   by tools/popt_matrix.cc (<G>.<order>[.out][.l<N>][.a<B>].popt); setup() finds the gather PC by matching the
//   trace against the file's neighbour prefix; the clock comes from the per-destination stream.
// - Transpose mode (runtime-ordered traversals: sssp, bc, bfs, Afforest cc; src/popt_push.cc): matrices built in
//   memory from the graph (.sg), one per irregular array; the clock comes from each neighbour-array read and may
//   jump, re-streaming the pinned columns.
//
// Matrix (inc/popt_matrix_build.h): one byte per (irregular-array line, epoch), 256 epochs of ceil(n/256) vertices,
// Fig. 6 format (MSB 1: not referenced this epoch, low 7 bits = epochs to the next reference, 127 = none; MSB 0:
// referenced this epoch, low 7 bits = sub-epoch of the last reference, 128 sub-epochs; 127 throughout the last
// epoch). Matrix lines follow the LLC's block grid: line size = block size, offset = where the array starts.
//
// Only the current and next epoch columns are visible to the policy (the columns P-OPT pins in reserved LLC ways;
// main takes the reserved capacity out of the LLC via reserved_ways()). Streaming them in is charged as
// stop-the-world cycles.
//
// Eviction (P-OPT's next-ref engine, llc.cpp): the first way not holding irregular data; otherwise the way whose
// next reference (Algorithm 2) is farthest, ties as the HPCA'21 code. Insertion and hits are SRRIP.
class POPT : public SRRIP {
    std::vector<uint64_t> line_addr;   // block address held by each way (UINT64_MAX = none)

    struct Header {
        char magic[8];
        uint64_t num_nodes, num_arcs, elem_size, line_size, num_epochs, num_sub, epoch_size, sub_size, num_lines,
                 num_check, align_offset;   // align_offset: bytes into a block where irregData starts
    };
    inline static Header hdr{};
    inline static std::vector<uint64_t> starts;   // first gather of each (epoch, sub-epoch) block, + num_arcs
    inline static std::vector<uint8_t> matrix;    // num_epochs columns of num_lines bytes (memory copy)
    inline static const uint8_t* cur_col = nullptr;   // pinned current-epoch column
    inline static const uint8_t* nxt_col = nullptr;   // pinned next-epoch column (nullptr in the last epoch)
    inline static uint64_t block = 0;             // current (epoch, sub-epoch) block = epoch * num_sub + sub
    inline static uint64_t gathers = 0;
    // currDstID (paper: update_index at the start of each destination): a per-destination stream - the earliest
    // PC with one access per vertex, constant stride, starting before the first gather (GAP: the CSR offset read
    // of in_neigh(u)) - gives u exactly. Gather counting (via starts[]) cross-checks it; it drives the epochs only
    // if no such stream exists.
    inline static uint64_t dst_pc = 0, dst_total = 0, dst_count = 0, gather_reads = 0;
    inline static uint64_t gblock = 0;            // block implied by the gather count (cross-check)
    // P-OPT runs only over the pull loop (reference code: UpdateIrregRanges around it): from the first destination
    // to the end of both streams; elsewhere eviction is plain SRRIP.
    inline static bool in_window = false, window_done = false;
    inline static uint64_t stat_block_mismatch = 0, stat_evict_outside = 0;
    static void set_block(uint64_t new_block);
    inline static uint64_t irreg_base = 0, irreg_bound = 0, gather_pc = 0;
    inline static uint64_t stat_evict_stream = 0, stat_evict_opt = 0, stat_evict_tie = 0;
    inline static uint64_t stat_streamed_lines = 0, stat_epochs = 0;
    inline static uint64_t stat_ref_hist[4] = {};   // next refs at OPT evictions: 0, 1, 2..126, 127
    // Next-ref engine (paper V-C): each OPT replacement reads the pinned columns' 64B blocks holding the eviction
    // set's entries (current column; next column too when Algorithm 2 falls through). Reads of this access are
    // charged by charge_engine() after the access, hidden behind the access's own DRAM fetch.
    inline static uint64_t pending_reads = 0;
    inline static uint64_t stat_matrix_reads = 0, stat_engine_cycles = 0, stat_exposed_cycles = 0;
    inline static uint64_t stat_replacements_dram = 0, stat_replacements_nodram = 0, stat_replacements_wb = 0;
    // Streaming engine (paper V-D): stop-the-world transfer of a column (num_lines bytes) at peak DRAM bandwidth,
    // at the start (current + next column) and at every epoch boundary (the new next column).
    inline static bool streamed_initial = false;
    inline static uint64_t stat_stream_cycles = 0;
    static void stream_columns(uint64_t columns);

    static bool load(const std::string& path, bool with_matrix);
    static void enter_epoch(uint64_t epoch);
    static uint64_t find_next_ref(uint64_t line_id, std::vector<uint64_t>* reads = nullptr);
    // Next ref of a 64B cache line address: the nearest over the matrix lines it overlaps (two when irregData
    // does not start on a 64B boundary).
    uint64_t next_ref_of(uint64_t addr, std::vector<uint64_t>* reads) const;

    // ---- Transpose (push) mode: kernels whose traversal order is decided at run time (sssp, bc, bfs, Afforest cc).
    // The paper's methodology applied as is: each irregular array x indexed by a neighbour v gets a Rereference
    // Matrix built from the transpose of the traversal (processing u references x[v] for v in nbrs(u)), with u, the
    // vertex being processed, as the clock (currDstID). u is not visited in id order here, so the clock jumps: a
    // change of epoch other than e -> e+1 streams both pinned columns in again (stop-the-world, charged).
    // Detection (setup_push): the simulator loads the graph (.sg, --popt-graph-dir) and finds, from the trace,
    // the neighbour-array reads (base, entry size, CSR direction) and the arrays read with x[nb[k]] right after
    // them (base, element size or frontier bitmap); each array's matrix is built in memory
    // (inc/popt_matrix_build.h) at its own element size and block offset.
    struct PushArray {
        uint64_t base = 0, bound = 0, elem = 0;   // elem 0: frontier bitmap (1 bit per vertex, 8B words)
        uint64_t align = 0, per_line = 0, off_v = 0, num_lines = 0, pc = 0;
        int csr = 0;                              // 0: out-neighbour CSR, 1: in-neighbour CSR
        std::vector<uint8_t> matrix;
        const uint8_t* cur = nullptr;
        const uint8_t* nxt = nullptr;
    };
    struct PushNA { uint64_t pc = 0, base = 0, stride = 0, reads = 0; int csr = 0; };
    inline static bool push_mode = false;
    inline static std::vector<PushArray> parrs;
    inline static std::vector<PushNA> pnas;
    inline static std::vector<int64_t> g_off[2];
    inline static std::vector<int32_t> g_nb[2];
    inline static uint64_t g_n = 0, g_m = 0;
    inline static bool g_directed = false;
    inline static uint64_t push_block = 0;        // clock: epoch * num_sub + sub-epoch of the vertex being processed
    inline static uint64_t stat_push_updates = 0, stat_push_epoch_steps = 0, stat_push_epoch_jumps = 0;
    static bool load_graph(const std::string& base);
    static bool setup_push(const std::string& trace_path, const std::string& base);
    static void on_access_push(uint64_t pc, uint64_t addr);
    static void push_set_block(uint64_t new_block);
    static void push_stream(uint64_t columns);
    static void push_print_stats();
    static int push_array_of(uint64_t addr) {
        for (size_t i = 0; i < parrs.size(); i++)
            if (addr != UINT64_MAX && addr + cachesim::BLOCK_SIZE > parrs[i].base && addr < parrs[i].bound) return (int)i;
        return -1;
    }
    static uint64_t push_next_ref(int arr, uint64_t addr, std::vector<uint64_t>* reads);
    uint64_t push_eviction_candidate();

    // A 64B line holds irregData if it overlaps [irreg_base, irreg_bound).
    bool is_irreg(uint64_t addr) const { return addr != UINT64_MAX && addr + cachesim::BLOCK_SIZE > irreg_base && addr < irreg_bound; }   // block overlaps irregData   // HPCA'21: block start in range

    public:
    inline static bool active = false;
    inline static uint64_t elem_size = 4;     // --popt-elem-size: must match the matrix
    inline static std::string matrix_dir;     // --popt-matrix-dir
    inline static std::string graph_dir;      // --popt-graph-dir (transpose mode: <G>.<order>.sg)

    // Finds and validates the matrix for this trace and prescans the trace (gather PC, irreg range, pr writes).
    static bool setup(const std::string& trace_path);
    // Called for every trace access before it is simulated: the destination stream sets currDstID; gathers
    // cross-check it (or drive it, without a destination stream).
    static void on_access(uint64_t pc, uint64_t addr, bool is_read) {
        if (push_mode) { if (is_read) on_access_push(pc, addr); return; }
        if (!streamed_initial) { streamed_initial = true; stream_columns(hdr.num_epochs > 1 ? 2 : 1); }
        if (dst_pc && pc == dst_pc) {
            uint64_t u = dst_count++;
            if (!window_done) in_window = true;
            if (u < hdr.num_nodes) {
                uint64_t e = u / hdr.epoch_size;
                set_block(e * hdr.num_sub + (u - e * hdr.epoch_size) / hdr.sub_size);
            }
        }
        if (pc == gather_pc && is_read) {
            uint64_t last_block = starts.size() - 2;
            while (gblock < last_block && gathers >= starts[gblock + 1]) gblock++;   // gather's (epoch, sub-epoch)
            if (!dst_pc) {
                if (!window_done) in_window = true;
                set_block(gblock);
            } else if (gblock != block) {
                stat_block_mismatch++;
            }
            gathers++;
        }
        if (in_window && gathers >= gather_reads && (!dst_pc || dst_count >= dst_total)) {
            in_window = false;
            window_done = true;
        }
    }
    static uint64_t num_lines() { return hdr.num_lines; }
    // After each access: account the next-ref engine's matrix reads. A replacement made while filling a writeback
    // (an L1D victim into the exclusive LLC) is off the critical path: reads and engine cycles are counted, no
    // cycles are added. A demand fill adds what the access's own DRAM fetch does not hide.
    static void charge_engine(uint64_t dram_cycles, bool writeback);
    // Ways P-OPT reserves per set for the current and next epoch columns (num_lines bytes each).
    static uint64_t reserved_ways(uint64_t num_sets, uint64_t line_size);
    static void print_stats();

    POPT(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : SRRIP(_set_idx, _num_ways, _level) {
        line_addr.resize(num_ways, UINT64_MAX);
    }

    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);
    uint64_t get_eviction_candidate(bool is_low_priority);
    void evict(uint64_t way_idx);
    void invalidate(uint64_t way_idx) { SRRIP::invalidate(way_idx); line_addr[way_idx] = UINT64_MAX; }   // emptied without an eviction
};

#endif
