#include "core.h"

#include "belady_replacement_policy.h"
#include "popt_replacement_policy.h"
#include "performance_model.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iostream>

Core::Core(uint32_t id, std::string trace, TraceFormat format, std::vector<BaseCache*> caches,
           SparsityPredictor* predictor, uint64_t num_iters)
    : id_(id), trace_(std::move(trace)), format_(format), caches_(std::move(caches)), predictor_(predictor),
      iters_left_(trace_.empty() ? 0 : num_iters), misses_(caches_.size(), 0) {
    for (auto* c : caches_)
        level_names_.push_back(c->get_name());
}

void Core::retire(uint64_t n) {
    instructions_ += n;
    cachesim::instCount += n;
}

void Core::print_stats() const {
    auto mpki = [&](uint64_t m) { return instructions_ ? 1000.0 * m / instructions_ : 0.0; };
    fmt::print("Core {}: instructions {} cycles {} IPC {:.4f} accesses {}", id_, instructions_, cycles_,
               cycles_ ? (double)instructions_ / cycles_ : 0.0, accesses_);
    for (size_t i = 0; i < caches_.size(); ++i)
        fmt::print(" | {} misses {} MPKI {:.6f}", level_names_[i], misses_[i], mpki(misses_[i]));
    fmt::print("\n");
}

bool Core::step() {
    // This core's cycles: what the (global) timing model gains while it steps.
    const uint64_t cycles_before = PerformanceModel::getCycles();
    struct Charge {
        uint64_t before; uint64_t& acc;
        ~Charge() { acc += PerformanceModel::getCycles() - before; }
    } charge{cycles_before, cycles_};
    while (iters_left_ > 0) {
        if (!pass_open_ && !open_pass()) {
            iters_left_ = 0;
            finished_ = true;
            return false;
        }
        Read r = format_ == TraceFormat::ADDRESSES    ? step_addresses()
               : format_ == TraceFormat::INSTRUCTIONS ? step_instructions()
                                                      : step_champsim();
        if (r == Read::RECORD)
            return true;
        // End of one pass over the trace (or the debug window ended it): the next
        // iteration reopens the trace from the start.
        pass_open_ = false;
        text_.reset();
        champsim_.reset();
        --iters_left_;
    }
    finished_ = true;
    return false;
}

bool Core::open_pass() {
    line_idx_ = 0;
    pass_base_ = instructions_;   // a later pass continues the count
    if (format_ == TraceFormat::CHAMPSIM) {
        champsim_.emplace(get_tracereader(trace_, static_cast<uint8_t>(id_), false, false));
    } else {
        text_ = std::make_unique<TraceFile>(trace_);
        if (!text_->is_open()) {
            std::cerr << "Error: Could not open the file!" << std::endl;
            text_.reset();
            return false;
        }
    }
    pass_open_ = true;
    return true;
}

bool Core::debug_stop() {
    if (cachesim::DEBUG_ALL || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {
        if (cachesim::instCount > cachesim::END_DEBUG)
            return true;
        if (cachesim::instCount >= cachesim::START_DEBUG)
            cachesim::DEBUG = true;
    }
    return false;
}

void Core::issue(uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree) {
    int hit_at_level =
#ifdef MULTI_LEVEL
    access_multi_level(caches_, &access_packet_, &eviction_packet_, &fill_packet_, &invalidation_packet_,
                       predictor_, address, pc, is_read, next_reuse, degree, avg_degree);
#else
    access_single_level(caches_, &access_packet_, &eviction_packet_, &fill_packet_, &invalidation_packet_,
                        pc, address, is_read, next_reuse, degree, avg_degree);
#endif
    ++accesses_;
    for (int i = 0; i < hit_at_level && i < (int)misses_.size(); ++i)
        ++misses_[i];
}

// "PC:<pc> <read|write>:0x<addr> [next_reuse degree avg_degree]"; every line is one instruction.
Core::Read Core::step_addresses() {
    std::string line;
    if (!text_->getline(line))
        return Read::END_OF_PASS;
    cachesim::TRACE_LINE = line_idx_++;
    if (debug_stop())
        return Read::END_OF_PASS;
    uint64_t pc = 0, address = 0, next_reuse = UINT64_MAX, degree = 0;
    float avg_degree = 0.0f;
    char action[16];
    int parsed = std::sscanf(line.c_str(), "PC:%lu %15[^:]:0x%lx %lu %lu %f", &pc, action, &address, &next_reuse,
                             &degree, &avg_degree);
    if (parsed >= 3) {
        try {
            retire(1);
            if (next_reuse != UINT64_MAX)
                next_reuse += cachesim::instCount;
            issue(address, pc, strcmp(action, "read") == 0, next_reuse, degree, avg_degree);
        } catch (const std::exception& e) {
            std::cerr << "Conversion error on line: " << line << " -> " << e.what() << std::endl;
        }
    }
    return Read::RECORD;
}

// "instCount:N,PC:0x..,Read|Write:0x..,NextReuse:..,NodeDegree:..,PCAvgDegree:.." (GAP Pin traces).
Core::Read Core::step_instructions() {
    std::string line;
    if (!text_->getline(line))
        return Read::END_OF_PASS;
    cachesim::TRACE_LINE = line_idx_++;
    if (debug_stop())
        return Read::END_OF_PASS;
    uint64_t pc = 0, address = 0, next_reuse = UINT64_MAX, degree = 0, trace_count = 0;
    float avg_degree = 0.0f;
    char action[16];
    int parsed = std::sscanf(line.c_str(),
                             "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx,NextReuse:%lu,NodeDegree:%lu,PCAvgDegree:%f",
                             &trace_count, &pc, action, &address, &next_reuse, &degree, &avg_degree);
    if (parsed >= 1) {
        // The trace carries the program's instruction count, which only increases. A smaller value is a
        // corrupted line: the program's or tracer's own output was written into the middle of a record
        // ("instCount:19999998 Another 100 million instructions gone by..." for 199999987), so such a line
        // carries no valid access. Keep the largest count seen: this core = earlier passes + that value.
        uint64_t now = pass_base_ + trace_count;
        if (now > instructions_)
            retire(now - instructions_);
    }
    if (parsed >= 3) {
        try {
            bool is_read = strcmp(action, "Read") == 0;
            if (POPT::active) POPT::on_access(pc, address, is_read);   // gathers advance P-OPT's currDstID
            if (Belady::oracle_active) Belady::on_access(address);      // next-use oracle
            issue(address, pc, is_read, next_reuse, degree, avg_degree);
        } catch (const std::exception& e) {
            std::cerr << "Conversion error on line: " << line << " -> " << e.what() << std::endl;
        }
    }
    return Read::RECORD;
}

// One ChampSim instruction: its loads, then its stores.
Core::Read Core::step_champsim() {
    if (champsim_->eof())
        return Read::END_OF_PASS;
    if (cachesim::DEBUG || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {
        if (cachesim::instCount > 5000000)
            return Read::END_OF_PASS;
    }
    retire(1);
    auto inst = (*champsim_)();
    access_packet_.clear();
    eviction_packet_.clear();
    fill_packet_.clear();
    invalidation_packet_.clear();
    uint64_t pc = inst.ip.to<uint64_t>();
    for (auto& smem : inst.source_memory)
        issue(smem.to<uint64_t>(), pc, true, 0, 0, 0.0f);
    for (auto& dmem : inst.destination_memory)
        issue(dmem.to<uint64_t>(), pc, false, 0, 0, 0.0f);
    return Read::RECORD;
}

ScheduleStats run_cores(std::vector<Core>& cores) {
    ScheduleStats st;
    const size_t n = cores.size();
    size_t cur = n;
    for (size_t c = 0; c < n && cur == n; ++c)
        if (cores[c].runnable())
            cur = c;
    while (cur < n) {
        cores[cur].step();
        ++st.steps;

        // Next core: the first one after cur (in id order, wrapping) that is not ahead of cur.
        size_t next = n;
        for (size_t k = 1; k < n; ++k) {
            size_t d = (cur + k) % n;
            if (cores[d].runnable() && cores[d].cycles() <= cores[cur].cycles()) {
                next = d;
                break;
            }
        }
        if (next == n) {
            if (cores[cur].runnable()) {
                next = cur;                       // every other core is ahead: keep going
            } else {
                for (size_t d = 0; d < n; ++d)    // cur finished: the runnable core furthest behind
                    if (cores[d].runnable() && (next == n || cores[d].cycles() < cores[next].cycles()))
                        next = d;
            }
        }
        if (next != cur && next < n)
            ++st.switches;
        cur = next;

        if (n > 1 && (st.steps & 1023) == 0) {
            uint64_t lo = UINT64_MAX, hi = 0;
            for (const auto& c : cores)
                if (c.runnable()) {
                    lo = std::min(lo, c.cycles());
                    hi = std::max(hi, c.cycles());
                }
            if (hi >= lo)
                st.max_skew = std::max(st.max_skew, hi - lo);
        }
    }
    return st;
}
