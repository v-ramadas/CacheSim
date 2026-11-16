#ifndef __PIN_INPUT_H__
#define __PIN_INPUT_H__

// Live Pin input (--trace-format pin): CacheSim runs the program under Pin with our ROI tracer
// (pin/pin/gap_roi_tracer_mt.cpp) and reads its records while it runs, instead of from trace files.
//
//   - One FIFO per core, <dir>/trace.t<c>.log: the tracer writes thread c's records to trace.t<c>.log (Pin thread
//     ids in creation order, 0 = main thread), so core c simulates application thread c. Threads beyond the core
//     count write ordinary files there and are not simulated (reported at the end).
//   - Same record format as the stored traces before hub post-processing: instCount:N,PC:0x..,Read|Write:0x..
//     (no NextReuse / NodeDegree / PCAvgDegree: oracles needing them are not available live).
//   - One reader thread per FIFO drains it into an in-memory queue, so Pin never blocks on a pipe CacheSim is not
//     reading (no deadlock when the scheduler waits for one thread while another runs ahead or waits at an OpenMP
//     barrier). Cores pop records in order, so results do not depend on when data arrives.
//   - The program runs with OMP_NUM_THREADS = number of cores, and without ASLR unless --pin-aslr.
//   - --pin-dump DIR also writes the received records to DIR/trace.t<c>.log: replaying them as an instructions
//     trace gives the same results (stage-1 validation), and they can be post-processed like any trace.

#include "tracer.h"

#include <cstdint>
#include <memory>
#include <string>

struct PinConfig {
    std::string cmd;                 // program and arguments (shell-style quoting)
    std::string pin_root;            // Pin kit (default $PIN_ROOT)
    std::string tool;                // pintool .so (default: ../pin/pin/obj-intel64/gap_roi_tracer_mt.so next to CacheSim)
    std::string roi;                 // "<start>,<end>": ROI marker offsets from _init (as gen_traces.sh), or "magic":
                                     // Sniper magic instructions (SimRoiStart/SimRoiEnd) in the program (-roiMagic 1)
    std::string bmk;                 // image name substring the tracer instruments (default: program path)
    std::string tool_args;           // extra tracer knobs, e.g. "-roiOnce 1"
    std::string dir;                 // FIFO directory (default: a fresh temporary one, removed at the end)
    std::string dump;                // also save the received records here
    bool aslr = false;               // keep address-space randomization for the program
};

namespace pin_input {
// Start Pin and the reader threads for `cores` application threads. Returns false (with a message) on error.
bool start(const PinConfig& cfg, uint32_t cores, const std::string& cachesim_bin);
// After the simulation: wait for Pin, report its exit and the tracer's summary, clean up. Returns Pin's success.
bool finish();
// Abort: kill Pin if still running, clean up.
void abort();
// Reader for core c (open_trace calls it).
std::unique_ptr<TraceReader> reader(uint32_t core);
}

#endif
