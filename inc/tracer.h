#ifndef __TRACER_H__
#define __TRACER_H__

// Trace input: one reader per trace format, all producing the same TraceRecord, which a Core turns into
// instruction counts, barriers and memory accesses (core.cc). Formats (--trace-format):
//   champsim      ChampSim binary traces (ChampSim's tracereader): one record per instruction, its loads then stores
//   addresses     text "PC:<pc> <read|write>:0x<addr> [next_reuse degree avg_degree]", one instruction per line
//   instructions  text "instCount:N,PC:0x..,Read|Write:0x..,NextReuse:..,NodeDegree:..,PCAvgDegree:.." (GAP Pin
//                 traces), one access per line, N = the program's running instruction count
//   pin           live: CacheSim runs the program under Pin with our ROI tracer and reads its records while it runs
//                 (pin_input.h); same records as instructions traces before hub post-processing
// Text traces may also hold "BARRIER" / "BARRIER:<n>" lines (multicore barriers).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class TraceFormat {
    CHAMPSIM,
    ADDRESSES,
    INSTRUCTIONS,
    PIN,
};

struct TraceAccess {
    uint64_t address = 0, pc = 0;
    bool is_read = true;
    uint64_t next_reuse = UINT64_MAX;   // trace's NextReuse (UINT64_MAX = none)
    uint64_t degree = 0;
    float avg_degree = 0.0f;
};

// What one step of a trace yields. The reader only parses; the core applies it (retire, issue, barriers, hooks).
struct TraceRecord {
    enum class Count { NONE, ADD, AT };
    bool text_line = false;             // one text line was consumed: sets TRACE_LINE, the debug window applies
    bool barrier = false;               // a barrier line
    bool barrier_has_id = false;
    uint64_t barrier_id = 0;
    Count count = Count::NONE;          // ADD: retire `instructions`; AT: the trace's running count is `instructions`
    uint64_t instructions = 0;
    bool next_reuse_from_now = false;   // addresses format: NextReuse is relative to the count after retiring
    bool oracle_hooks = false;          // instructions format: feed P-OPT / Belady before issuing
    bool reset_packets = false;         // champsim: clear the core's packets before issuing
    std::vector<TraceAccess> accesses;  // in issue order
    std::string line;                   // text formats: the line (error messages)

    void clear();
};

class TraceReader {
  public:
    virtual ~TraceReader() = default;
    virtual bool ok() const = 0;                 // opened successfully
    virtual bool next(TraceRecord& rec) = 0;     // false at the end of the trace
};

// One pass over `path`. core_id: ChampSim's reader takes the cpu number.
std::unique_ptr<TraceReader> open_trace(TraceFormat format, const std::string& path, uint32_t core_id);

// Line parsers (also used by the oracle prescans, which read instructions-format traces directly).
// Return the number of fields parsed, as sscanf does (fields after the first unparsable one keep their defaults).
struct InstructionsLine {
    uint64_t inst = 0, pc = 0, address = 0, next_reuse = UINT64_MAX, degree = 0;
    float avg_degree = 0.0f;
    char action[16] = {0};
    bool is_read() const;                        // action == "Read"
};
int parse_instructions_line(const std::string& line, InstructionsLine& out);   // 7 fields when complete
// Just the access of an instructions-format line (count, PC, action, address): true when all four parse.
bool parse_instructions_access(const std::string& line, uint64_t& inst, uint64_t& pc, char (&action)[16],
                               uint64_t& address);

struct AddressesLine {
    uint64_t pc = 0, address = 0, next_reuse = UINT64_MAX, degree = 0;
    float avg_degree = 0.0f;
    char action[16] = {0};
    bool is_read() const;                        // action == "read"
};
int parse_addresses_line(const std::string& line, AddressesLine& out);         // 6 fields when complete

// "BARRIER" or "BARRIER:<n>": true for a barrier line; has_id/id say whether a number was given.
bool parse_barrier_line(const std::string& line, bool& has_id, uint64_t& id);

#endif
