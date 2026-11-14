// Differential test: Sniper's own queue-model sources (compiled unchanged from ../../../snipersim with the stubs in
// stubs/) against CacheSim's port in src/sniper_dram.cc. Identical request streams must give identical queue
// delays, request counts and analytical-model counts for every queue type and setting, and identical
// ComponentBandwidth rounding and fixed-latency conversion. Build and run: tests/sniper_queue/run.sh
#include "queue_model.h"
#include "contention_model.h"
#include "subsecond_time.h"
#include "simulator.h"

#include "../../inc/sniper_dram.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static StubSim g_sim;
StubSim* Sim() { return &g_sim; }

struct Req { uint64_t t, proc, global; };

static int failures = 0, checks = 0;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        if (failures < 20) std::printf("  MISMATCH: %s\n", what.c_str());
    }
}

// Request streams (times in fs). proc = transfer time of 64B at the configured bandwidth, or mixed sizes.
static std::vector<Req> stream(const std::string& kind, uint64_t proc, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::vector<Req> v;
    const int n = 20000;
    if (kind == "in_order") {             // arrivals faster than service at times: queues build
        uint64_t t = 1000000;
        for (int i = 0; i < n; i++) {
            t += std::exponential_distribution<double>(1.0 / (proc * 0.9))(rng);
            v.push_back({t, proc, t > 2000000000 ? t - 2000000000 : 0});
        }
    } else if (kind == "skewed_cores") {  // 8 cores whose clocks drift apart: out-of-order arrivals
        std::vector<uint64_t> clk(8, 1000000);
        for (int i = 0; i < n; i++) {
            int c = rng() % 8;
            clk[c] += std::exponential_distribution<double>(1.0 / (proc * 6.0))(rng);
            uint64_t lo = *std::min_element(clk.begin(), clk.end());
            v.push_back({clk[c], proc, lo});
        }
    } else if (kind == "bursts") {        // groups of requests at the same instant
        uint64_t t = 1000000;
        for (int i = 0; i < n; i++) {
            if (i % 16 == 0) t += proc * (8 + rng() % 40);
            v.push_back({t, proc, t > 500000000 ? t - 500000000 : 0});
        }
    } else if (kind == "grid") {          // arrivals and sizes on a grid of proc/4: exact interval boundaries
        uint64_t t = 1000000 / proc * proc;
        for (int i = 0; i < n; i++) {
            t += (proc / 4) * (rng() % 9);
            uint64_t p = (proc / 4) * (1 + rng() % 8);
            uint64_t back = (proc / 4) * (rng() % 64);          // some requests arrive earlier than the last
            uint64_t at = (i % 5 == 0 && t > back) ? t - back : t;
            v.push_back({at, p, t > 400000000 ? t - 400000000 : 0});
        }
    } else if (kind == "mixed_light") {   // light load, varied sizes: windowed_mg1 not saturated
        uint64_t t = 1000000;
        for (int i = 0; i < n; i++) {
            t += std::exponential_distribution<double>(1.0 / (proc * 20.0))(rng);
            uint64_t p = 1 + rng() % (2 * proc);
            v.push_back({t, p, t > 1500000000 ? t - 1500000000 : 0});
        }
    } else if (kind == "mixed_sizes") {   // processing times vary (8B..256B transfers)
        uint64_t t = 1000000;
        for (int i = 0; i < n; i++) {
            t += std::exponential_distribution<double>(1.0 / proc)(rng);
            uint64_t p = proc / 8 * (1 + rng() % 32);
            v.push_back({t, p, t > 3000000000ULL ? t - 3000000000ULL : 0});
        }
    }
    return v;
}

static void set_cfg(const sniper::Config& c) {
    auto& m = g_sim.cfg.v;
    m["queue_model/basic/moving_avg_enabled"] = c.basic_ma_enabled ? "true" : "false";
    m["queue_model/basic/moving_avg_window_size"] = std::to_string(c.basic_ma_window);
    m["queue_model/basic/moving_avg_type"] = c.basic_ma_type;
    m["queue_model/history_list/analytical_model_enabled"] = c.history_analytical ? "true" : "false";
    m["queue_model/history_list/max_list_size"] = std::to_string(c.history_max_list_size);
    m["queue_model/windowed_mg1/window_size"] = std::to_string(c.mg1_window_ns);
}

static void compare_queue(const std::string& type, const sniper::Config& cfg, const std::string& kind, uint64_t proc,
                          const std::string& label) {
    set_cfg(cfg);
    QueueModel* theirs = QueueModel::create("q", 0, String(type.c_str()), SubsecondTime::FS(proc));
    auto ours = sniper::QueueModel::create(type, proc, cfg);
    auto reqs = stream(kind, proc, 12345);
    int before = failures;
    uint64_t sum = 0;
    for (size_t i = 0; i < reqs.size(); i++) {
        g_sim.skew.t = SubsecondTime::FS(reqs[i].global);
        sniper::set_global_time(reqs[i].global);
        uint64_t a = theirs->computeQueueDelay(SubsecondTime::FS(reqs[i].t), SubsecondTime::FS(reqs[i].proc)).getFS();
        uint64_t b = ours->computeQueueDelay(reqs[i].t, reqs[i].proc);
        sum += b;
        if (a != b) {
            check(false, label + " request " + std::to_string(i) + ": sniper " + std::to_string(a) + " ours " +
                             std::to_string(b));
            break;
        }
        ++checks;
    }
    std::printf("  [%s] %-58s %zu requests, mean delay %.3f ns\n", failures == before ? "PASS" : "FAIL", label.c_str(),
                reqs.size(), (double)sum / reqs.size() / 1e6);
    delete theirs;
}

int main() {
    std::printf("Sniper queue models vs CacheSim port (identical delays required)\n");
    const uint64_t proc = ComponentBandwidth(8 * 5.0).getRoundedLatency(8 * 64).getFS();   // base.cfg 5 GB/s
    for (std::string kind : {"in_order", "skewed_cores", "bursts", "mixed_sizes", "grid", "mixed_light"}) {
        sniper::Config c;
        for (uint32_t max_list : {100u, 10u}) {
            for (bool analytical : {true, false}) {
                c = sniper::Config();
                c.history_max_list_size = max_list;
                c.history_analytical = analytical;
                compare_queue("history_list", c, kind, proc,
                              "history_list max " + std::to_string(max_list) + (analytical ? " analytical" : " exact") +
                                  " / " + kind);
            }
        }
        for (uint64_t w : {1000ULL, 100ULL}) {
            c = sniper::Config();
            c.mg1_window_ns = w;
            compare_queue("windowed_mg1", c, kind, proc, "windowed_mg1 window " + std::to_string(w) + " ns / " + kind);
        }
        for (std::string ma : {"arithmetic_mean", "geometric_mean", "median"}) {
            for (uint32_t win : {1024u, 3u}) {
                c = sniper::Config();
                c.basic_ma_type = ma;
                c.basic_ma_window = win;
                compare_queue("basic", c, kind, proc, "basic " + ma + " window " + std::to_string(win) + " / " + kind);
            }
        }
        c = sniper::Config();
        c.basic_ma_enabled = false;
        compare_queue("basic", c, kind, proc, "basic no moving average / " + kind);
        compare_queue("contention", sniper::Config(), kind, proc, "contention / " + kind);
    }

    std::printf("ComponentBandwidth rounding and fixed latency (DramPerfModelConstant constructor expressions)\n");
    int before = failures;
    for (double bw : {5.0, 7.6, 12.8, 25.6, 3.3, 1.1, 19.2, 6.4}) {
        for (uint64_t bytes : {8ULL, 64ULL, 128ULL}) {
            uint64_t a = ComponentBandwidth(8 * bw).getRoundedLatency(8 * bytes).getFS();
            uint64_t b = sniper::ComponentBandwidth(static_cast<float>(8 * bw)).getRoundedLatency(8 * bytes);
            check(a == b, "bandwidth " + std::to_string(bw) + " GB/s, " + std::to_string(bytes) + "B");
        }
    }
    for (double lat : {100.0, 45.0, 30.25, 66.6, 0.0, 12.5}) {
        uint64_t a = (SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(lat))).getFS();
        sniper::Config c;
        c.latency_ns = lat;
        c.queue_enabled = false;
        sniper::DramController d(c, 64);
        uint64_t proc64 = ComponentBandwidth(8 * c.per_controller_bandwidth).getRoundedLatency(8 * 64).getFS();
        uint64_t b = d.getAccessLatency(5000000, 64) - proc64;
        check(a == b, "latency " + std::to_string(lat) + " ns: sniper " + std::to_string(a) + " ours " + std::to_string(b));
    }
    std::printf("  [%s] 24 bandwidth/size cases, 6 latencies\n", failures == before ? "PASS" : "FAIL");

    std::printf("%s: %d checks, %d mismatches\n", failures ? "FAILURES" : "ALL PASS", checks, failures);
    return failures ? 1 : 0;
}
