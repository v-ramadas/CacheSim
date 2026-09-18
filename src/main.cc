#include "cachesim.h"

uint64_t cachesim::instCount = 0;
uint64_t cachesim::prevInstCount = 0;

uint64_t cachesim::BLOCK_SIZE = CACHELINE_SIZE;

bool cachesim::DEBUG = false;
bool cachesim::DEBUG_ALL = false;
bool cachesim::L1_DEBUG = false;
bool cachesim::LLC_DEBUG = false;
bool cachesim::REPLACEMENT_POLICY_DEBUG = false;
bool cachesim::NO_ISO_AREA = false;
bool cachesim::GEN_STATS = false;
bool cachesim::ENABLE_PREDICTOR = false;
//TODO: Figure out a good value
uint64_t cachesim::WARMUP_INSTRUCTIONS = 0;//2*16*2048;
bool cachesim::dropBlocks = false;
bool cachesim::useMemSignature = false;
bool cachesim::isoArea = false;
uint64_t cachesim::START_DEBUG = 0;
uint64_t cachesim::END_DEBUG = 10000000;

bool performance::DETAILED_DRAM = false;

bool cachesim::SET_DUELING = false;
int cachesim::NUM_DUELS = 1;
uint64_t cachesim::PSEL_MAX = 64;
uint64_t cachesim::PSEL_THRESHOLD = cachesim::PSEL_MAX >> 2;
uint64_t cachesim::DUELING_PERIOD = 10000000;

DuelingMode cachesim::DUELING_MODE = DuelingMode::ZTEST;
bool cachesim::LOG_DUELING_METRICS = false;

uint64_t cachesim::CONF_EPOCH = 200000;
uint64_t cachesim::CONF_MAX = 8;
uint64_t cachesim::CONF_MARGIN = cachesim::PSEL_THRESHOLD;
double cachesim::Z_THRESHOLD = 2.0;

// Validated against the 9-trace pr_spmv suite at CONF_EPOCH=10000: smaller
// windows (e.g. 20 checkpoints) have too little data to be safe at any
// reasonable threshold (false-locked on web-BerkStan even at z=7.0). 1600
// checkpoints / z=2.5 was the smallest safe combination found - 3 of 4
// known-negative traces stay safe, and the 5 true-positive traces converge
// noticeably faster than the cumulative-only test.
uint64_t cachesim::Z_WINDOW_SIZE = 1600;
double cachesim::WINDOW_Z_THRESHOLD = 2.5;

PerformanceModel cachesim::performanceModel;

bool cachesim::USE_GHOST_CACHE = false;
uint64_t cachesim::GHOST_CACHE_SIZE = 32;
GhostCache cachesim::ghostCache;

enum class TraceFormat {
    CHAMPSIM,
    ADDRESSES,
    INSTRUCTIONS,
};

int main(int argc, char** argv) {

    CLI::App app{"CacheSim"};
    std::string tracename;
    std::string configFile = "configs/default.cfg";
    TraceFormat trace_format = TraceFormat::CHAMPSIM;
    uint64_t llc_num_sets;
    uint64_t llc_num_ways;
    uint64_t num_iters = 1;
    ReplacementPolicy replacement_policy = ReplacementPolicy::LRU;
    InsertionPolicy insertion_policy = InsertionPolicy::EXCLUSIVE;
    app.add_option("--trace", tracename, "Path to input trace file")->required()->expected(1)->check(CLI::ExistingFile);
    app.add_option("--config", configFile, "Path to config file")->required()->expected(1)->check(CLI::ExistingFile);
    app.add_option("--trace-format", trace_format, "Trace format")->transform(CLI::CheckedTransformer(std::map<std::string, TraceFormat>{
        {"champsim", TraceFormat::CHAMPSIM},
        {"addresses", TraceFormat::ADDRESSES},
        {"instructions", TraceFormat::INSTRUCTIONS},
    }));
    app.add_option("--num-cache-sets", llc_num_sets, "Number of sets in cache")->required();
    app.add_option("--num-cache-ways", llc_num_ways, "Number of ways in cache")->required();
    app.add_option("--cache-block-size", cachesim::BLOCK_SIZE, "Cache block size");

    app.add_option("--replacement-policy", replacement_policy, "Cache replacement policy")->transform(CLI::CheckedTransformer(std::map<std::string, ReplacementPolicy>{
        {"lru", ReplacementPolicy::LRU},
        {"hru", ReplacementPolicy::HRU},
        {"hrupp", ReplacementPolicy::HRUpp},
        {"phru", ReplacementPolicy::PHRU},
        {"phrupp", ReplacementPolicy::PHRUpp},
        {"srrip", ReplacementPolicy::SRRIP},
        {"drrip", ReplacementPolicy::DRRIP},
        {"prrip", ReplacementPolicy::PRRIP},
        {"ship", ReplacementPolicy::SHIP},
        {"belady", ReplacementPolicy::Belady},
        {"hrrip", ReplacementPolicy::HRRIP},
        {"hawkeye", ReplacementPolicy::Hawkeye}
    }));
    app.add_option("--insertion-policy", insertion_policy, "Cache insertion policy")->transform(CLI::CheckedTransformer(std::map<std::string, InsertionPolicy>{
        {"exclusive", InsertionPolicy::EXCLUSIVE},
    }));
    app.add_flag("--debug", cachesim::DEBUG_ALL, "Enable debug mode");
    app.add_flag("--debug-llc", cachesim::LLC_DEBUG, "Enable debug mode");
    app.add_flag("--debug-l1", cachesim::L1_DEBUG, "Enable debug mode");
    app.add_flag("--debug-replacement-policy", cachesim::REPLACEMENT_POLICY_DEBUG, "Enable debug mode");
    app.add_flag("--no-iso-area", cachesim::NO_ISO_AREA, "Disable iso-area mode");
    app.add_option("--debug-start", cachesim::START_DEBUG, "Start debugging here");
    app.add_option("--debug-end", cachesim::END_DEBUG, "End debugging here");
    app.add_option("--iters", num_iters, "Number of iterations");
    app.add_option("--warmup-instructions", cachesim::WARMUP_INSTRUCTIONS, "Warmup instruction count");
    app.add_flag("--gen-stats", cachesim::GEN_STATS, "Disable iso-area mode");
    app.add_flag("--enable-predictor", cachesim::ENABLE_PREDICTOR, "Enable predictor (not implemented)");
    app.add_flag("--detailed-dram", performance::DETAILED_DRAM, "Enable detailed DRAM timing model");
    app.add_flag("--set-dueling", cachesim::SET_DUELING, "Enable set dueling");
    app.add_option("--num-duels", cachesim::NUM_DUELS, "Max Value of PSEL");
    app.add_option("--psel-max", cachesim::PSEL_MAX, "Max Value of PSEL");
    app.add_option("--psel-threshold", cachesim::PSEL_THRESHOLD, "PSEL Threshold value");
    app.add_option("--dueling-period", cachesim::DUELING_PERIOD, "Hard cap (instructions) on set dueling before a winner is forced");
    app.add_option("--dueling-mode", cachesim::DUELING_MODE, "Set-dueling decision mechanism: psel (original, live-checked PSEL threshold), ztest (cumulative+windowed conditional-binomial z-test on miss counts), or ztest-ratio (same, but a two-proportion z-test on miss rates)")->transform(CLI::CheckedTransformer(std::map<std::string, DuelingMode>{
        {"psel", DuelingMode::PSEL},
        {"ztest", DuelingMode::ZTEST},
        {"ztest-ratio", DuelingMode::ZTEST_RATIO},
    }));
    app.add_option("--conf-epoch", cachesim::CONF_EPOCH, "Instructions between set-dueling confidence checkpoints");
    app.add_option("--conf-max", cachesim::CONF_MAX, "Consecutive agreeing checkpoints required to lock a set-dueling winner");
    app.add_option("--conf-margin", cachesim::CONF_MARGIN, "Distance from the PSEL midpoint required for a checkpoint to count as confident (unused by the current decision rule, kept for compatibility)");
    app.add_option("--z-threshold", cachesim::Z_THRESHOLD, "Two-proportion z-score required (cumulative Leader64 vs Leader8 miss rate) for a checkpoint to count as confidently favoring breakdown (ztest mode only)");
    app.add_option("--window-size", cachesim::Z_WINDOW_SIZE, "Number of recent checkpoints used for the windowed z-test (ztest mode only)");
    app.add_option("--window-z-threshold", cachesim::WINDOW_Z_THRESHOLD, "Two-proportion z-score required over the recent window for a checkpoint to count as confidently favoring breakdown (ztest mode only)");
    app.add_flag("--log-dueling-metrics", cachesim::LOG_DUELING_METRICS, "Print a METRIC,... CSV line every conf-epoch instructions with PSEL, cache MPKI, and cumulative/windowed Leader64 vs Leader8 miss rates");
    app.add_flag("--use-ghost-cache", cachesim::USE_GHOST_CACHE, "Enable a small ghost cache that catches lines evicted from the LLC, so PHRU/PHRUpp can detect a line evicted too early via a fast re-miss instead of relying only on block_serviced_from_llc");
    app.add_option("--ghost-cache-size", cachesim::GHOST_CACHE_SIZE, "Ghost cache capacity in entries (only used if --use-ghost-cache is set)");


    CLI11_PARSE(app, argc, argv);

    if (cachesim::USE_GHOST_CACHE) {
        cachesim::ghostCache.resize(cachesim::GHOST_CACHE_SIZE);
    }

    switch(replacement_policy) {
        case ReplacementPolicy::LRU:
            cachesim::SET_DUELING = false;
            break;
        case ReplacementPolicy::SHIP:
            cachesim::useMemSignature = true;
            break;
        case ReplacementPolicy::HRRIP:
            cachesim::dropBlocks = true;
            break;
        case ReplacementPolicy::HRU:
            cachesim::dropBlocks = true;
            cachesim::isoArea = !cachesim::NO_ISO_AREA;
            break;
        case ReplacementPolicy::HRUpp:
            cachesim::dropBlocks = true;
            break;
        case ReplacementPolicy::PHRU:
            cachesim::dropBlocks = true;
            cachesim::isoArea = !cachesim::NO_ISO_AREA;
            break;
        case ReplacementPolicy::Belady:
            cachesim::SET_DUELING = false;
            break;
        case ReplacementPolicy::Hawkeye:
            cachesim::SET_DUELING = false;
            break;
        default:
            cachesim::dropBlocks = false;
            break;
    }

    if (cachesim::BLOCK_SIZE == 64) {
        cachesim::SET_DUELING = false;
    }

    try {
        PerformanceModel::populateModel(configFile);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    std::vector<BaseCache*> cache;
    SparsityPredictor* predictor = new SparsityPredictor(0.4, 1024, cachesim::WARMUP_INSTRUCTIONS);
#ifdef MULTI_LEVEL
    cache.resize(2);
    //cache[0] = new Cache<Set>("L1D", 128, 16, cachesim::BLOCK_SIZE, 0, false, ReplacementPolicy::LRU, insertion_policy);
    cache[0] = new Cache<SectoredSet>("L1D", 128, 16, 8, 0, true, ReplacementPolicy::LRU, insertion_policy);
    cache[1] = new Cache<Set>("LLC", llc_num_sets, llc_num_ways, cachesim::BLOCK_SIZE, 1, false, replacement_policy, insertion_policy);
    if (cachesim::ENABLE_PREDICTOR) predictor->enable();
    else predictor->disable();
    if (cachesim::useMemSignature)
        predictor->set_mem_signature();
    else
        predictor->set_pc_signature();
#else
    cache.resize(1);
    cache[0] = new Cache<Set>("L1D", llc_num_sets, llc_num_ways, cachesim::BLOCK_SIZE, 0, false, replacement_policy, insertion_policy);
#endif

    std::map<uint64_t, uint64_t> page_count;
    Packet* access_packet = new Packet();
    Packet* eviction_packet = new Packet();
    Packet* fill_packet = new Packet();
    Packet* invalidation_packet = new Packet();
    if (trace_format == TraceFormat::ADDRESSES) {
        useAddressTrace(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, invalidation_packet, num_iters);
    } else if (trace_format == TraceFormat::INSTRUCTIONS) {
        useInstructionTrace(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, invalidation_packet, num_iters);
    } else {
        useChampsimTrace(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, invalidation_packet, num_iters);
    }

    for (auto cache_inst: cache) {
        cache_inst->print_stats(cachesim::instCount, tracename);
        delete cache_inst;
    }
    if (cachesim::GEN_STATS)
        predictor->print_stats();
    if (replacement_policy == ReplacementPolicy::PHRU)
        PHRU::print_heuristic_stats();
    if (replacement_policy == ReplacementPolicy::PHRUpp)
        PHRUpp::print_heuristic_stats();
    if (cachesim::USE_GHOST_CACHE)
        fmt::print("Ghost cache: capacity {} inserts {} lookups {} hits {}\n",
            cachesim::ghostCache.capacity(), cachesim::ghostCache.debug_insert_count,
            cachesim::ghostCache.debug_lookup_count, cachesim::ghostCache.debug_hit_count);

    cache.clear();
#ifdef MULTI_LEVEL
    delete predictor;
#endif
    delete access_packet;
    delete eviction_packet;
    delete fill_packet;
    delete invalidation_packet;
    return 0;
}
