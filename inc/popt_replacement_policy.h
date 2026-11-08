#ifndef __POPT_REPLACEMENT_POLICY_H__
#define __POPT_REPLACEMENT_POLICY_H__

#include "srrip_replacement_policy.h"
#include <string>
#include <vector>

// P-OPT (Balaji, Crago, Jaleel, Lucia, HPCA 2021) for pull PageRank traces (pr, pr_spmv), 64B LLC, on SRRIP.
//
// Rereference Matrix: built offline from the graph (tools/popt_matrix.cc -> <G>.<order>[.wd].popt): one byte per
// (irregData line, epoch), 256 epochs of EpochSize = ceil(n/256) destination vertices, Fig. 6 format (MSB 1: not
// referenced this epoch, low 7 bits = epochs to the next reference, 127 = none; MSB 0: referenced this epoch, low
// 7 bits = sub-epoch of the last reference, 127 sub-epochs). irregData = outgoing_contrib, read once per in-arc
// (the gather); the .wd matrix also counts pr's (Gauss-Seidel) write of outgoing_contrib[u] at destination u.
//
// Only the current and next epoch columns are visible to the policy (the columns P-OPT pins in reserved LLC ways;
// the reserved capacity is taken out of the LLC by main via reserved_ways()). At each epoch boundary the next
// column becomes current and column epoch+1 is streamed in from memory (counted in stat_streamed_lines, not as
// demand misses).
//
// currDstID (P-OPT's update_index): the trace has no vertex ids, so the (epoch, sub-epoch) of the destination
// being processed is tracked by counting gathers - the matrix file holds the index of the first gather of every
// (epoch, sub-epoch) block. setup() finds the gather PC (the read PC with exactly num_arcs accesses whose first
// accesses follow the graph's in-neighbour order) and irreg_base (first gather address - elem_size * first
// in-neighbour), checks the first 1024 gathers against the graph, and picks the .wd matrix if and only if the
// trace writes one element per vertex inside irregData.
//
// Eviction (P-OPT's next-ref engine): the first way not holding irregData (address outside
// [irreg_base, irreg_bound)); otherwise the way whose next reference (Algorithm 2) is farthest, ties broken by
// SRRIP among the tied ways. Insertion and hits are SRRIP.
class POPT : public SRRIP {
    std::vector<uint64_t> line_addr;   // block address held by each way (UINT64_MAX = none)

    struct Header {
        char magic[8];
        uint64_t num_nodes, num_arcs, elem_size, line_size, num_epochs, num_sub, epoch_size, sub_size, num_lines,
                 num_check, writes_at_dst;
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
    // Pass mode (bfs bottom-up steps, <G>.<order>.bm.popt): several traversals per trace, each a partial pull over
    // the frontier BITMAP (1 bit per vertex; unvisited u only, early break). dst_pc (parent[u], once per vertex per
    // step) restarts the epochs at u = 0 and opens the window, closed at u = n-1. na_pc reads in_neigh(u)'s
    // neighbour array, bm_pc the bitmap word of each neighbour; the pair is validated against the matrix's neighbour
    // prefix. GAP swaps front/next after each step, so two bitmaps alternate: the step's bitmap sets irreg_base (as
    // software would update P-OPT's irreg range registers).
    inline static bool pass_mode = false;
    inline static uint64_t na_pc = 0, bm_pc = 0, na_base = 0, bm_bytes = 0, last_na = 0;
    inline static uint64_t bm_base[2] = {0, 0};
    inline static bool bm_known[2] = {false, false};
    inline static std::vector<int32_t> check_prefix;
    inline static std::vector<int32_t> samples;     // every sample_stride-th neighbour (bitmap matrices)
    inline static uint64_t sample_stride = 0;
    static bool load_samples(const std::string& path);
    // Neighbour id of neighbour-array index k when known (prefix or sample), else -1.
    static int64_t known_neighbour(uint64_t k) {
        if (k < check_prefix.size()) return check_prefix[k];
        if (sample_stride && k % sample_stride == 0 && k / sample_stride < samples.size()) return samples[k / sample_stride];
        return -1;
    }
    inline static uint64_t stat_passes = 0, stat_bm_refined = 0, stat_bm_mismatch = 0;
    static void new_pass();
    static void on_access_pass(uint64_t pc, uint64_t addr, bool is_read);

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

    // A 64B line holds irregData if it overlaps [irreg_base, irreg_bound).
    bool is_irreg(uint64_t addr) const { return addr != UINT64_MAX && addr + cachesim::BLOCK_SIZE > irreg_base && addr < irreg_bound; }

    public:
    inline static bool active = false;
    inline static uint64_t elem_size = 4;     // --popt-elem-size: must match the matrix
    inline static std::string matrix_dir;     // --popt-matrix-dir

    // Finds and validates the matrix for this trace and prescans the trace (gather PC, irreg range, pr writes).
    static bool setup(const std::string& trace_path);
    // Called for every trace access before it is simulated: the destination stream sets currDstID; gathers
    // cross-check it (or drive it, without a destination stream).
    static void on_access(uint64_t pc, uint64_t addr, bool is_read) {
        if (pass_mode) { on_access_pass(pc, addr, is_read); return; }
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
