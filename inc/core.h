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
// Barriers: a text trace line "BARRIER:<n>" (or "BARRIER") marks the thread
// entering barrier n. The core stops being scheduled until every core still
// running has reached the same barrier; then all are released with their clocks
// set to the latest arrival (the difference counts as barrier-wait cycles).
//
// Several cores may run traces; run_cores() interleaves them (see below). No
// coherence yet: cores touching the same line keep duplicate copies. P-OPT,
// Belady and the L1D Amoeba mode need a single active core; the DRAM model's
// notion of time is still the global cycle counter.

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
    // app: the application (address space) this core's trace belongs to; see translate() in utils.h.
    Core(uint32_t id, std::string trace, TraceFormat format, std::vector<BaseCache*> caches,
         SparsityPredictor* predictor, uint64_t num_iters, uint32_t app = 0);

    uint32_t id() const { return id_; }
    uint32_t app() const { return app_; }
    bool active() const { return !trace_.empty(); }
    bool runnable() const { return active() && !finished_ && !at_barrier_; }   // can be stepped now
    bool finished() const { return finished_; }
    bool at_barrier() const { return at_barrier_; }
    uint64_t barrier_id() const { return barrier_id_; }
    // Leave the barrier at time `clock` (the latest arrival): the wait is added to this core's cycles.
    void release_barrier(uint64_t clock);
    uint64_t barriers() const { return barriers_; }
    uint64_t barrier_wait_cycles() const { return barrier_wait_cycles_; }
    static inline bool debug_schedule = false;     // --debug-schedule: log every step, arrival and release
    const std::string& trace() const { return trace_; }
    std::vector<BaseCache*>& caches() { return caches_; }

    // Read one trace record (one line, or one ChampSim instruction) and issue
    // its accesses. Returns false once every iteration of the trace is done.
    bool step();                                   // also marks the core finished when it returns false
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
    bool debug_stop();
    bool barrier_line(const std::string& line);  // "BARRIER[:n]": enter the barrier   // END_DEBUG reached (text traces); also raises cachesim::DEBUG at START_DEBUG
    void issue(uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree);

    uint32_t id_;
    uint32_t app_;
    std::string trace_;
    TraceFormat format_;
    std::vector<BaseCache*> caches_;
    SparsityPredictor* predictor_;
    uint64_t iters_left_;

    bool finished_ = false;
    bool at_barrier_ = false;
    uint64_t barrier_id_ = 0;
    uint64_t barriers_ = 0;
    uint64_t barrier_wait_cycles_ = 0;
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

// Interleave the cores. After every step (one trace record: one memory access,
// or one ChampSim instruction) control moves to the next core in id order
// (wrapping around) whose clock is not ahead of the current core's; cores that
// are ahead are skipped. If every other core is ahead, the current core has the
// smallest clock and keeps running. The core furthest behind is therefore
// reached within num_cores - 1 switches, which bounds how far clocks drift
// apart. A finished or waiting core hands over to the runnable core with the
// smallest clock. When no core is runnable but some wait at a barrier, they are
// released (finished cores count as arrived).
struct ScheduleStats {
    uint64_t steps = 0;
    uint64_t switches = 0;
    uint64_t max_skew = 0;       // largest gap between running cores' clocks, measured after every step
    uint64_t skew_sum = 0;       // sum of that gap over the steps with two or more running cores
    uint64_t skew_samples = 0;   // number of such steps
    double mean_skew() const { return skew_samples ? (double)skew_sum / skew_samples : 0.0; }
    uint64_t barriers = 0;       // barrier releases
    std::string error;           // set when the run cannot continue (barrier mismatch)
};
ScheduleStats run_cores(std::vector<Core>& cores);

#endif
