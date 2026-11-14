#ifndef __CORE_H__
#define __CORE_H__

// A core reads its trace one record at a time and issues each memory access
// to its view of the hierarchy: its private levels (L1D, optional L2) followed
// by the shared LLC. Each core has a unique id (0 .. num_cores-1).
//
// Per-core accounting (B1): each core counts its own instructions, cycles and
// demand accesses / misses per level of its view. cachesim::instCount is the
// sum over all cores (the count shared structures such as set dueling see);
// PerformanceModel's cycle counter is the sum of all cores' cycles, and a core's
// cycles are what that counter gained while it was stepping.
//
// Current limit: at most one core runs a trace; the others are idle. P-OPT and
// Belady oracles and the DRAM model's notion of time are still global.

#include "cachesim.h"
#include "trace_file.h"
#include "tracereader.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class TraceFormat {
    CHAMPSIM,
    ADDRESSES,
    INSTRUCTIONS,
};

class Core {
  public:
    // caches: this core's view, top to bottom ([L1D, (L2), LLC] or the single
    // level of the single-level build). trace empty = idle core.
    Core(uint32_t id, std::string trace, TraceFormat format, std::vector<BaseCache*> caches,
         SparsityPredictor* predictor, uint64_t num_iters);

    uint32_t id() const { return id_; }
    bool active() const { return !trace_.empty(); }
    const std::string& trace() const { return trace_; }
    std::vector<BaseCache*>& caches() { return caches_; }

    // Read one trace record (one line, or one ChampSim instruction) and issue
    // its accesses. Returns false once every iteration of the trace is done.
    bool step();
    void run() { while (step()) {} }

    // Per-core statistics.
    uint64_t instructions() const { return instructions_; }
    uint64_t cycles() const { return cycles_; }
    uint64_t accesses() const { return accesses_; }
    // Demand accesses that missed level i of this core's view (i = 0 is the L1D).
    uint64_t misses(size_t level) const { return level < misses_.size() ? misses_[level] : 0; }
    void print_stats() const;

  private:
    enum class Read { RECORD, END_OF_PASS };

    bool open_pass();
    Read step_addresses();
    Read step_instructions();
    Read step_champsim();
    void retire(uint64_t n);                     // count n instructions on this core and in the global total
    bool debug_stop();   // END_DEBUG reached (text traces); also raises cachesim::DEBUG at START_DEBUG
    void issue(uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree);

    uint32_t id_;
    std::string trace_;
    TraceFormat format_;
    std::vector<BaseCache*> caches_;
    SparsityPredictor* predictor_;
    uint64_t iters_left_;

    bool pass_open_ = false;
    uint64_t line_idx_ = 0;                          // NextReuse indexes lines within one pass
    std::unique_ptr<TraceFile> text_;                // addresses / instructions formats
    std::optional<champsim::tracereader> champsim_;  // champsim format

    uint64_t instructions_ = 0;
    uint64_t cycles_ = 0;
    uint64_t accesses_ = 0;
    std::vector<uint64_t> misses_;
    std::vector<std::string> level_names_;           // kept: main deletes the caches before stats print
    uint64_t pass_base_ = 0;                         // instructions format: count before this pass

    Packet access_packet_, eviction_packet_, fill_packet_, invalidation_packet_;
};

#endif
