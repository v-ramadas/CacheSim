#include "core.h"

#include "belady_replacement_policy.h"
#include "popt_replacement_policy.h"
#include "performance_model.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iostream>

Core::Core(uint32_t id, std::string trace, TraceFormat format, std::vector<BaseCache*> caches,
           SparsityPredictor* predictor, uint64_t num_iters, uint32_t app)
    : id_(id), app_(app), trace_(std::move(trace)), format_(format), caches_(std::move(caches)), predictor_(predictor),
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
    fmt::print("Core {}: instructions {} cycles {} IPC {:.4f} accesses {} barriers {} barrier_wait_cycles {}", id_,
               instructions_, cycles_, cycles_ ? (double)instructions_ / cycles_ : 0.0, accesses_, barriers_,
               barrier_wait_cycles_);
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
    if (!finished_ && debug_schedule)
        fmt::print("SCHED finish core {} clock {}\n", id_, cycles_);
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

bool Core::barrier_line(const std::string& line) {
    if (line.rfind("BARRIER", 0) != 0)
        return false;
    unsigned long long id = 0;
    barrier_id_ = std::sscanf(line.c_str(), "BARRIER:%llu", &id) == 1 ? id : barriers_ + 1;
    at_barrier_ = true;
    if (debug_schedule)
        fmt::print("SCHED arrive core {} barrier {} clock {}\n", id_, barrier_id_, cycles_);
    return true;
}

void Core::release_barrier(uint64_t clock) {
    if (clock > cycles_) {
        barrier_wait_cycles_ += clock - cycles_;
        cycles_ = clock;
    }
    at_barrier_ = false;
    ++barriers_;
}

void Core::issue(uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree) {
    // Into this core's address space: the hierarchy sees physical addresses, and PCs are tagged the
    // same way so PC-indexed predictors keep different applications apart (identity for app 0).
    address = translate(address, app_);
    pc = translate(pc, app_);
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
    if (barrier_line(line))
        return Read::RECORD;
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
    if (barrier_line(line))
        return Read::RECORD;
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

InvariantCounts check_invariants(const std::vector<Core>& cores, uint64_t step, bool report_first) {
    InvariantCounts r;
    if (cores.empty() || cachesim::AMOEBA || cachesim::L1_AMOEBA)
        return r;
    // Holder of a word: core * 16 + private level, or SHARED for the last level (shared by all cores).
    constexpr uint32_t SHARED = UINT32_MAX;
    std::vector<std::pair<uint64_t, uint32_t>> held;
    std::vector<std::string> names;
    std::vector<uint64_t> words;
    static bool reported_dup = false, reported_excl = false, reported_l1l2 = false;
    auto add = [&](const BaseCache* c, uint32_t holder) {
        words.clear();
        c->collect_words(words);
        std::sort(words.begin(), words.end());
        for (size_t i = 1; i < words.size(); ++i)
            if (words[i] == words[i - 1]) {
                ++r.dup_in_cache;
                if (report_first && !reported_dup) {
                    reported_dup = true;
                    fmt::print("INVARIANT step {}: word {:#x} held twice in {}\n", step, words[i], c->get_name());
                }
            }
        words.erase(std::unique(words.begin(), words.end()), words.end());
        for (uint64_t w : words)
            held.emplace_back(w, holder);
    };
    const auto& shared_view = cores.front().caches();
    add(shared_view.back(), SHARED);
    for (const auto& core : cores)
        for (size_t i = 0; i + 1 < core.caches().size(); ++i)
            add(core.caches()[i], core.id() * 16 + static_cast<uint32_t>(i));
    std::sort(held.begin(), held.end());
    auto holder_name = [&](uint32_t h) {
        return h == SHARED ? shared_view.back()->get_name() : cores[h / 16].caches()[h % 16]->get_name();
    };
    for (size_t i = 0; i < held.size();) {
        size_t j = i;
        while (j < held.size() && held[j].first == held[i].first)
            ++j;
        bool shared = false, l1l2 = false;
        uint32_t first_core = UINT32_MAX;
        bool two_cores = false, any_private = false;
        for (size_t k = i; k < j; ++k) {
            uint32_t h = held[k].second;
            if (h == SHARED) { shared = true; continue; }
            any_private = true;
            uint32_t core = h / 16;
            if (first_core == UINT32_MAX) first_core = core;
            else if (core != first_core) two_cores = true;
            if (k > i && held[k - 1].second != SHARED && held[k - 1].second / 16 == core)
                l1l2 = true;
        }
        auto list = [&]() {
            std::string s;
            for (size_t k = i; k < j; ++k)
                s += (k > i ? ", " : "") + holder_name(held[k].second);
            return s;
        };
        if (shared && any_private) {
            ++r.private_and_llc;
            if (report_first && !reported_excl) {
                reported_excl = true;
                fmt::print("INVARIANT step {}: word {:#x} in a private level and the LLC: {}\n", step, held[i].first, list());
            }
        }
        if (l1l2) {
            ++r.l1_and_l2;
            if (report_first && !reported_l1l2) {
                reported_l1l2 = true;
                fmt::print("INVARIANT step {}: word {:#x} in two levels of one core: {}\n", step, held[i].first, list());
            }
        }
        if (two_cores)
            ++r.cross_core;
        i = j;
    }
    return r;
}

ScheduleStats run_cores(std::vector<Core>& cores, uint64_t check_every, InvariantStats* inv) {
    ScheduleStats st;
    auto check = [&](bool force) {
        if (!inv || check_every == 0 || (!force && st.steps % check_every != 0))
            return;
        InvariantCounts c = check_invariants(cores, st.steps, true);
        ++inv->checks;
        inv->max.dup_in_cache = std::max(inv->max.dup_in_cache, c.dup_in_cache);
        inv->max.private_and_llc = std::max(inv->max.private_and_llc, c.private_and_llc);
        inv->max.l1_and_l2 = std::max(inv->max.l1_and_l2, c.l1_and_l2);
        inv->max.cross_core = std::max(inv->max.cross_core, c.cross_core);
    };
    const size_t n = cores.size();
    size_t cur = n;
    for (size_t c = 0; c < n && cur == n; ++c)
        if (cores[c].runnable())
            cur = c;
    // No core can run: release the cores waiting at a barrier (finished cores count as arrived). Returns the
    // core to continue with, or n when nothing is left.
    auto release = [&]() -> size_t {
        uint64_t latest = 0, id = 0;
        bool any = false;
        for (const auto& c : cores)
            if (c.at_barrier()) {
                if (any && c.barrier_id() != id) {
                    st.error = fmt::format("cores wait at different barriers ({} and {}): the traces do not belong "
                                           "to one run", id, c.barrier_id());
                    return n;
                }
                any = true;
                id = c.barrier_id();
                latest = std::max(latest, c.cycles());
            }
        if (!any)
            return n;
        ++st.barriers;
        if (Core::debug_schedule)
            fmt::print("SCHED release barrier {} clock {}\n", id, latest);
        for (auto& c : cores)
            if (c.at_barrier())
                c.release_barrier(latest);
        for (size_t d = 0; d < n; ++d)
            if (cores[d].runnable())
                return d;
        return n;
    };
    if (cur == n)
        cur = release();
    while (cur < n) {
        cores[cur].step();
        ++st.steps;
        if (Core::debug_schedule)
            fmt::print("SCHED step core {} clock {}\n", cur, cores[cur].cycles());
        // Clock skew: the gap between the running cores' clocks (waiting and finished cores excluded; waiting
        // is reported as barrier_wait_cycles). Only the stepped core's clock moved, so measuring after every
        // step sees every peak.
        if (n > 1) {
            uint64_t lo = UINT64_MAX, hi = 0, running = 0;
            for (const auto& c : cores)
                if (c.runnable()) {
                    lo = std::min(lo, c.cycles());
                    hi = std::max(hi, c.cycles());
                    ++running;
                }
            if (running > 1) {
                st.max_skew = std::max(st.max_skew, hi - lo);
                st.skew_sum += hi - lo;
                ++st.skew_samples;
            }
        }

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
                for (size_t d = 0; d < n; ++d)    // cur finished or waits: the runnable core furthest behind
                    if (cores[d].runnable() && (next == n || cores[d].cycles() < cores[next].cycles()))
                        next = d;
                if (next == n)
                    next = release();             // nobody can run: everyone left is at the barrier
            }
        }
        if (next != cur && next < n)
            ++st.switches;
        cur = next;
        check(false);

    }
    check(true);
    return st;
}
