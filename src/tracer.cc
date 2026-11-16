#include "tracer.h"

#include "pin_input.h"
#include "trace_file.h"
#include "tracereader.h"

#include <cstdio>
#include <cstring>
#include <optional>

void TraceRecord::clear() {
    text_line = barrier = barrier_has_id = false;
    barrier_id = 0;
    count = Count::NONE;
    instructions = 0;
    oracle_hooks = reset_packets = false;
    accesses.clear();
    line.clear();
}

bool InstructionsLine::is_read() const { return std::strcmp(action, "Read") == 0; }

int parse_instructions_line(const std::string& line, InstructionsLine& o) {
    return std::sscanf(line.c_str(), "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx,NextReuse:%lu,NodeDegree:%lu,PCAvgDegree:%f",
                       &o.inst, &o.pc, o.action, &o.address, &o.next_reuse, &o.degree, &o.avg_degree);
}

bool parse_instructions_access(const std::string& line, uint64_t& inst, uint64_t& pc, char (&action)[16],
                               uint64_t& address) {
    return std::sscanf(line.c_str(), "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx", &inst, &pc, action, &address) == 4;
}


bool parse_barrier_line(const std::string& line, bool& has_id, uint64_t& id) {
    if (line.rfind("BARRIER", 0) != 0)
        return false;
    unsigned long long n = 0;
    has_id = std::sscanf(line.c_str(), "BARRIER:%llu", &n) == 1;
    id = has_id ? n : 0;
    return true;
}

namespace {

// Instructions-format text traces: one record per line.
class TextReader : public TraceReader {
  public:
    explicit TextReader(const std::string& path) : file_(path) {}
    bool ok() const override { return file_.is_open(); }
    bool next(TraceRecord& rec) override {
        rec.clear();
        if (!file_.getline(rec.line))
            return false;
        rec.text_line = true;
        if (parse_barrier_line(rec.line, rec.barrier_has_id, rec.barrier_id)) {
            rec.barrier = true;
            return true;
        }
        // The line carries the program's running instruction count; an access needs PC, action and address.
        InstructionsLine l;
        int parsed = parse_instructions_line(rec.line, l);
        if (parsed >= 1) {
            rec.count = TraceRecord::Count::AT;
            rec.instructions = l.inst;
        }
        if (parsed >= 3) {
            rec.oracle_hooks = true;
            rec.accesses.push_back({l.address, l.pc, l.is_read(), l.next_reuse, l.degree, l.avg_degree});
        }
        return true;
    }

  private:
    TraceFile file_;
};

// ChampSim binary traces: one record per instruction, its source (load) then destination (store) addresses.
class ChampSimReader : public TraceReader {
  public:
    ChampSimReader(const std::string& path, uint32_t core_id)
        : reader_(get_tracereader(path, static_cast<uint8_t>(core_id), false, false)) {}
    bool ok() const override { return true; }   // the ChampSim reader aborts itself on an unreadable file
    bool next(TraceRecord& rec) override {
        rec.clear();
        if (reader_.eof())
            return false;
        auto inst = reader_();
        rec.count = TraceRecord::Count::ADD;
        rec.instructions = 1;
        rec.reset_packets = true;
        const uint64_t pc = inst.ip.to<uint64_t>();
        for (auto& smem : inst.source_memory)
            rec.accesses.push_back({smem.to<uint64_t>(), pc, true, 0, 0, 0.0f});
        for (auto& dmem : inst.destination_memory)
            rec.accesses.push_back({dmem.to<uint64_t>(), pc, false, 0, 0, 0.0f});
        return true;
    }
    bool eof() { return reader_.eof(); }

  private:
    champsim::tracereader reader_;
};

}  // namespace

std::unique_ptr<TraceReader> open_trace(TraceFormat format, const std::string& path, uint32_t core_id) {
    switch (format) {
        case TraceFormat::CHAMPSIM:
            return std::make_unique<ChampSimReader>(path, core_id);
        case TraceFormat::INSTRUCTIONS:
            return std::make_unique<TextReader>(path);
        case TraceFormat::PIN:
            return pin_input::reader(core_id);   // live: the path is only a label
    }
    return nullptr;
}
