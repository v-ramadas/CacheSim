#include "pin_input.h"

#include <fmt/core.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>

namespace fs = std::filesystem;

namespace {

// One parsed line: as the instructions reader sees it (fields: count, then optionally PC/action/address).
struct LiveRec {
    uint64_t inst = 0, pc = 0, address = 0;
    uint8_t parsed = 0;      // fields parsed, as parse_instructions_line returns (capped at 3)
    bool is_read = true;
};

// Records of one application thread, filled by its reader thread, drained by its core.
struct Stream {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<LiveRec>> blocks;   // filled blocks, in order
    bool eof = false;
    uint64_t records = 0;                      // total pushed
    std::thread reader;
    std::string fifo;
};

struct Session {
    PinConfig cfg;
    std::string dir;
    bool own_dir = false;
    pid_t pid = -1;
    std::atomic<bool> exited{false};
    int status = 0;
    std::thread waiter;
    std::vector<std::unique_ptr<Stream>> streams;
};

std::unique_ptr<Session> S;

constexpr size_t BLOCK = 4096;

void read_fifo(Stream& st, uint32_t c) {
    int fd = ::open(st.fifo.c_str(), O_RDWR | O_NONBLOCK);   // O_RDWR: never blocks, and the writer never blocks on open
    if (fd < 0) {
        fmt::print(stderr, "pin input: cannot open {}: {}\n", st.fifo, std::strerror(errno));
        std::lock_guard<std::mutex> g(st.m);
        st.eof = true;
        st.cv.notify_all();
        return;
    }
    ::fcntl(fd, F_SETPIPE_SZ, 1 << 20);
    std::FILE* dump = nullptr;
    if (!S->cfg.dump.empty()) {
        std::string p = fmt::format("{}/trace.t{}.log", S->cfg.dump, c);
        dump = std::fopen(p.c_str(), "w");
        if (!dump)
            fmt::print(stderr, "pin input: cannot write {}\n", p);
    }
    std::vector<char> buf(1 << 20);
    std::string partial;
    std::vector<LiveRec> block;
    block.reserve(BLOCK);
    auto push = [&](bool final) {
        if (block.empty() && !final)
            return;
        std::lock_guard<std::mutex> g(st.m);
        if (!block.empty()) {
            st.records += block.size();
            st.blocks.push_back(std::move(block));
            block = {};
            block.reserve(BLOCK);
        }
        if (final)
            st.eof = true;
        st.cv.notify_all();
    };
    auto line_done = [&](const std::string& line) {
        if (dump) {
            std::fwrite(line.data(), 1, line.size(), dump);
            std::fputc('\n', dump);
        }
        InstructionsLine l;
        int n = parse_instructions_line(line, l);
        LiveRec r;
        r.parsed = (uint8_t)(n < 0 ? 0 : (n > 3 ? 3 : n));
        r.inst = l.inst;
        r.pc = l.pc;
        r.address = l.address;
        r.is_read = l.is_read();
        block.push_back(r);
        if (block.size() >= BLOCK)
            push(false);
    };
    for (;;) {
        const bool exited = S->exited.load();     // read before draining: data written before exit is in the pipe
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            size_t start = 0;
            for (ssize_t i = 0; i < n; ++i)
                if (buf[i] == '\n') {
                    partial.append(buf.data() + start, i - start);
                    line_done(partial);
                    partial.clear();
                    start = i + 1;
                }
            partial.append(buf.data() + start, n - start);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            fmt::print(stderr, "pin input: read {}: {}\n", st.fifo, std::strerror(errno));
            break;
        }
        if (exited)
            break;                                  // Pin has exited and the pipe is empty
        push(false);                                // hand over what we have while waiting
        pollfd p{fd, POLLIN, 0};
        ::poll(&p, 1, 50);
    }
    if (!partial.empty())
        line_done(partial);                         // a last line without newline
    ::close(fd);
    if (dump)
        std::fclose(dump);
    push(true);
}

class PinReader : public TraceReader {
  public:
    explicit PinReader(Stream& st) : st_(st) {}
    bool ok() const override { return true; }
    bool next(TraceRecord& rec) override {
        while (pos_ >= cur_.size()) {
            std::unique_lock<std::mutex> g(st_.m);
            st_.cv.wait(g, [&] { return !st_.blocks.empty() || st_.eof; });
            if (st_.blocks.empty())
                return false;
            cur_ = std::move(st_.blocks.front());
            st_.blocks.pop_front();
            pos_ = 0;
        }
        const LiveRec& r = cur_[pos_++];
        // As the instructions reader (tracer.cc) treats the same line.
        rec.clear();
        rec.text_line = true;
        if (r.parsed >= 1) {
            rec.count = TraceRecord::Count::AT;
            rec.instructions = r.inst;
        }
        if (r.parsed >= 3) {
            rec.oracle_hooks = true;
            rec.accesses.push_back({r.address, r.pc, r.is_read, UINT64_MAX, 0, 0.0f});
        }
        return true;
    }

  private:
    Stream& st_;
    std::vector<LiveRec> cur_;
    size_t pos_ = 0;
};

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty())
        return out;
    wordexp_t w;
    if (wordexp(s.c_str(), &w, WRDE_NOCMD) != 0) {
        fmt::print(stderr, "pin input: cannot parse '{}'\n", s);
        return out;
    }
    for (size_t i = 0; i < w.we_wordc; ++i)
        out.emplace_back(w.we_wordv[i]);
    wordfree(&w);
    return out;
}

void cleanup_dir() {
    if (!S)
        return;
    for (auto& st : S->streams)
        ::unlink(st->fifo.c_str());
    if (S->own_dir) {
        std::error_code ec;
        fs::remove_all(S->dir, ec);
    }
}

}  // namespace

namespace pin_input {

bool start(const PinConfig& cfg, uint32_t cores, const std::string& cachesim_bin) {
    S = std::make_unique<Session>();
    S->cfg = cfg;
    auto& c = S->cfg;
    if (c.cmd.empty()) {
        fmt::print(stderr, "--trace-format pin needs --pin-cmd \"<program> <args>\"\n");
        return false;
    }
    if (c.pin_root.empty())
        if (const char* e = std::getenv("PIN_ROOT")) c.pin_root = e;
    if (c.pin_root.empty() || !fs::exists(c.pin_root + "/pin")) {
        fmt::print(stderr, "pin input: no Pin kit (give --pin-root or set PIN_ROOT)\n");
        return false;
    }
    if (c.tool.empty()) {
        fs::path bin = fs::absolute(cachesim_bin).parent_path();   // CacheSim/bin
        c.tool = (bin / "../../pin/pin/obj-intel64/gap_roi_tracer_mt.so").lexically_normal().string();
    }
    if (!fs::exists(c.tool)) {
        fmt::print(stderr, "pin input: no pintool {} (give --pin-tool)\n", c.tool);
        return false;
    }
    const bool magic = c.roi == "magic";
    auto comma = c.roi.find(',');
    if (!magic && comma == std::string::npos) {
        fmt::print(stderr, "pin input: --pin-roi <start>,<end> (marker offsets from _init, as in gen_traces.sh) or magic\n");
        return false;
    }
    std::vector<std::string> prog = split_words(c.cmd);
    if (prog.empty())
        return false;
    // Pin runs in the FIFO directory (so its command line, which reaches the program's address space, is the same
    // every run: "-o trace.log"). Relative paths in the command that exist here become absolute.
    for (auto& w : prog)
        if (!w.empty() && w[0] != '/' && w[0] != '-' && fs::exists(w))
            w = fs::absolute(w).lexically_normal().string();
    if (c.bmk.empty())
        c.bmk = prog[0];
    if (!c.dump.empty())
        fs::create_directories(c.dump);
    // FIFO directory.
    if (c.dir.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        std::string tmpl = std::string(tmp ? tmp : "/tmp") + "/cachesim-pin-XXXXXX";
        std::vector<char> t(tmpl.begin(), tmpl.end());
        t.push_back('\0');
        if (!::mkdtemp(t.data())) {
            fmt::print(stderr, "pin input: mkdtemp {}: {}\n", tmpl, std::strerror(errno));
            return false;
        }
        S->dir = t.data();
        S->own_dir = true;
    } else {
        S->dir = fs::absolute(c.dir).lexically_normal().string();
        fs::create_directories(S->dir);
    }
    for (uint32_t i = 0; i < cores; ++i) {
        auto st = std::make_unique<Stream>();
        st->fifo = fmt::format("{}/trace.t{}.log", S->dir, i);
        ::unlink(st->fifo.c_str());
        if (::mkfifo(st->fifo.c_str(), 0600) != 0) {
            fmt::print(stderr, "pin input: mkfifo {}: {}\n", st->fifo, std::strerror(errno));
            cleanup_dir();
            return false;
        }
        S->streams.push_back(std::move(st));
    }
    // Pin command line, as scripts/gen_traces.sh runs the multithreaded tracer.
    std::vector<std::string> argv = {fs::absolute(c.pin_root + "/pin").string(), "-t", fs::absolute(c.tool).string(),
                                     "-o", "trace.log", "-s", "0",
                                     "-t", "1000000000000", "-bmkName", c.bmk};
    if (magic) {   // ROI from Sniper magic instructions (sim_api.h SimRoiStart / SimRoiEnd) in the program
        argv.push_back("-roiMagic");
        argv.push_back("1");
    } else {
        for (const char* k : {"-roiStart", "-roiEnd"}) argv.push_back(k);
        argv.insert(argv.end() - 1, c.roi.substr(0, comma));
        argv.push_back(c.roi.substr(comma + 1));
    }
    for (auto& w : split_words(c.tool_args)) argv.push_back(w);
    argv.push_back("--");
    for (auto& w : prog) argv.push_back(w);
    const std::string log = S->dir + "/pin.log";
    std::string shown;
    for (auto& a : argv) shown += (shown.empty() ? "" : " ") + a;
    fmt::print("Pin input: OMP_NUM_THREADS={} {}{}\n", cores, shown, c.aslr ? "" : " (ASLR off)");
    fmt::print("Pin input: FIFOs and program output in {} (Pin runs there)\n", S->dir);
    std::fflush(stdout);

    pid_t pid = ::fork();
    if (pid < 0) {
        fmt::print(stderr, "pin input: fork: {}\n", std::strerror(errno));
        cleanup_dir();
        return false;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        if (!c.aslr)
            ::personality(ADDR_NO_RANDOMIZE);
        ::setenv("OMP_NUM_THREADS", std::to_string(cores).c_str(), 1);
        if (::chdir(S->dir.c_str()) != 0)
            ::_exit(126);
        int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            ::close(fd);
        }
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        ::execv(a[0], a.data());
        std::perror("execv pin");
        ::_exit(127);
    }
    S->pid = pid;
    S->waiter = std::thread([] {
        int st = 0;
        while (::waitpid(S->pid, &st, 0) < 0 && errno == EINTR) {}
        S->status = st;
        S->exited.store(true);
    });
    for (uint32_t i = 0; i < cores; ++i) {
        Stream* st = S->streams[i].get();
        st->reader = std::thread([st, i] { read_fifo(*st, i); });
    }
    return true;
}

std::unique_ptr<TraceReader> reader(uint32_t core) {
    if (!S || core >= S->streams.size())
        return nullptr;
    return std::make_unique<PinReader>(*S->streams[core]);
}

bool finish() {
    if (!S)
        return true;
    for (auto& st : S->streams)
        if (st->reader.joinable()) st->reader.join();
    if (S->waiter.joinable()) S->waiter.join();
    bool ok = WIFEXITED(S->status) && WEXITSTATUS(S->status) == 0;
    fmt::print("Pin input: Pin {}", WIFEXITED(S->status) ? fmt::format("exited {}", WEXITSTATUS(S->status))
                                                          : fmt::format("killed by signal {}", WTERMSIG(S->status)));
    for (size_t i = 0; i < S->streams.size(); ++i)
        fmt::print("{} t{} {} records", i ? "," : " |", i, S->streams[i]->records);
    fmt::print("\n");
    // The tracer's own summary (per-thread ROI instructions, total, write errors).
    std::ifstream in(S->dir + "/pin.log");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("Thread ", 0) == 0 || line.rfind("ROI instructions executed", 0) == 0 ||
            line.find("ERROR") != std::string::npos)
            fmt::print("Pin input: {}\n", line);
    // Threads the tracer saw beyond the simulated cores wrote ordinary files.
    for (uint32_t t = (uint32_t)S->streams.size(); t < 1024; ++t) {
        std::string p = fmt::format("{}/trace.t{}.log", S->dir, t);
        if (!fs::exists(p))
            break;
        fmt::print("Pin input: WARNING application thread {} ran but there are only {} cores: not simulated\n", t,
                   S->streams.size());
        ok = false;
    }
    if (!ok)
        fmt::print("Pin input: see {}/pin.log{}\n", S->dir, S->own_dir ? " (kept)" : "");
    else
        cleanup_dir();
    if (!ok)
        for (auto& st : S->streams) ::unlink(st->fifo.c_str());
    S.reset();
    return ok;
}

void abort() {
    if (!S)
        return;
    if (S->pid > 0 && !S->exited.load())
        ::kill(-S->pid, SIGKILL);
    for (auto& st : S->streams)
        if (st->reader.joinable()) st->reader.join();
    if (S->waiter.joinable()) S->waiter.join();
    cleanup_dir();
    S.reset();
}

}  // namespace pin_input
