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
int cachesim::NUM_DUELS = 64;
uint64_t cachesim::PSEL_MAX = 4096;
uint64_t cachesim::PSEL_THRESHOLD = cachesim::PSEL_MAX >> 1;
uint64_t cachesim::DUELING_PERIOD = 1000000;

DuelingMode cachesim::DUELING_MODE = DuelingMode::PSEL;
bool cachesim::LOG_DUELING_METRICS = false;

uint64_t cachesim::CONF_EPOCH = 10000;
uint64_t cachesim::CONF_MAX = 1;
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
bool cachesim::GHOST_CACHE_DEBUG = false;
uint64_t cachesim::GHOST_CACHE_MIN_SERVICED = 1;
uint64_t cachesim::L1_NUM_SETS = 128;
uint64_t cachesim::L1_NUM_WAYS = 16;
bool cachesim::L2_ENABLED = false;
uint64_t cachesim::TAG_BDI_BASES = 8;
uint64_t cachesim::TAG_BDI_DELTA = 10;
bool cachesim::TAG_BDI_UNCONSTRAINED = false;
bool cachesim::DEBUG_TAG_BDI = false;
uint64_t cachesim::TRACE_LINE = 0;
uint64_t cachesim::L2_NUM_SETS = 1024;
uint64_t cachesim::L2_NUM_WAYS = 16;

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
        {"hub_oracle", ReplacementPolicy::HubOracle},
        {"srrip", ReplacementPolicy::SRRIP},
        {"drrip", ReplacementPolicy::DRRIP},
        {"prrip", ReplacementPolicy::PRRIP},
        {"ship_pc", ReplacementPolicy::SHIPPC},
        {"ship_mem", ReplacementPolicy::SHIPMem},
        {"belady", ReplacementPolicy::Belady},
        {"hrrip", ReplacementPolicy::HRRIP},
        {"hawkeye", ReplacementPolicy::Hawkeye},
        {"hub_hawkeye", ReplacementPolicy::HubHawkeye},
        {"grasp", ReplacementPolicy::GRASP},
        {"popt", ReplacementPolicy::POPT}
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
    app.add_flag("--debug-ghost-cache", cachesim::GHOST_CACHE_DEBUG, "Print a line for every ghost cache insert/lookup/eviction (only used if --use-ghost-cache is set)");
    app.add_option("--ghost-cache-min-serviced", cachesim::GHOST_CACHE_MIN_SERVICED, "A line is only inserted into the ghost cache on LLC eviction if serviced_from_llc exceeds this (only used if --use-ghost-cache is set); default 1 (i.e. requires serviced_from_llc > 1)");
    app.add_option("--l1-sets", cachesim::L1_NUM_SETS, "L1D sets (default 128)");
    app.add_option("--l1-ways", cachesim::L1_NUM_WAYS, "L1D ways, each a 64B line with 8B sectors (default 16 -> 128KB; it prints as sets x ways x 8B)");
    app.add_flag("--l2", cachesim::L2_ENABLED, "Add a sectored L2 (64B lines, 8B sectors, LRU) between L1D and LLC; exclusive like the rest of the hierarchy");
    app.add_option("--l2-sets", cachesim::L2_NUM_SETS, "L2 sets (only used with --l2; default 1024)");
    app.add_option("--l2-ways", cachesim::L2_NUM_WAYS, "L2 ways, each a 64B line (only used with --l2; default 16 -> 1MB)");
    app.add_option("--tag-bdi-bases", cachesim::TAG_BDI_BASES, "Base+Delta LLC tag compression: base tags per set (default 8; 0 = off, uncompressed 51-bit tags). Applies to iso-area 8B sets (HRU/PHRU/hub_oracle)");
    app.add_option("--tag-bdi-delta", cachesim::TAG_BDI_DELTA, "Base+Delta tag compression: signed delta bits per block (default 10)");
    app.add_flag("--tag-bdi-unconstrained", cachesim::TAG_BDI_UNCONSTRAINED, "Base+Delta tags: keep a tag that fits no base in full (step 2, capacity-only bound) instead of replacing the base with the fewest users");
    app.add_flag("--debug-tag-bdi", cachesim::DEBUG_TAG_BDI, "Print every Base+Delta tag encode, lookup, release and base replacement (LLC)");
    app.add_option("--popt-graph", POPT::graph_path, "popt: the trace's graph as a GAP .sg file; its vertex count sizes the rereference matrix");
    app.add_option("--popt-elem-size", POPT::elem_size, "popt: bytes per element of the irregularly accessed array (default 4)");


    CLI11_PARSE(app, argc, argv);

    if (cachesim::USE_GHOST_CACHE) {
        // A ghost cache entry is one address tag at the LLC's CURRENT block
        // size. At 8B blocks, the same cacheline needs 8 tag slots instead
        // of 1 to cover the same look-back window in terms of actual
        // cachelines - scale capacity by CACHELINE_SIZE/BLOCK_SIZE so
        // --ghost-cache-size keeps meaning "cachelines tracked" regardless
        // of block size.
        uint64_t scale_factor = CACHELINE_SIZE / cachesim::BLOCK_SIZE;
        cachesim::ghostCache.resize(cachesim::GHOST_CACHE_SIZE * scale_factor);
        cachesim::ghostCache.debug = cachesim::GHOST_CACHE_DEBUG;
    }

    switch(replacement_policy) {
        case ReplacementPolicy::LRU:
            cachesim::SET_DUELING = false;
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
        case ReplacementPolicy::HubOracle:
            cachesim::dropBlocks = true;
            cachesim::isoArea = !cachesim::NO_ISO_AREA;
            break;
        case ReplacementPolicy::Belady:
            cachesim::SET_DUELING = false;
            break;
        case ReplacementPolicy::Hawkeye:
        case ReplacementPolicy::HubHawkeye:
        case ReplacementPolicy::GRASP:
        case ReplacementPolicy::POPT:
            cachesim::SET_DUELING = false;
            break;
        default:
            cachesim::dropBlocks = false;
            break;
    }

    if (cachesim::BLOCK_SIZE == 64) {
        cachesim::SET_DUELING = false;
    }

    if (cachesim::TAG_BDI_BASES > 0 && cachesim::isoArea) {
        fmt::print("Tag BDI ({}): bases {} delta {} bits -> {}B LLC sets get {} ways (uncompressed iso-area would give {})\n",
            cachesim::TAG_BDI_UNCONSTRAINED ? "unconstrained" : "base replacement", cachesim::TAG_BDI_BASES, cachesim::TAG_BDI_DELTA, cachesim::BLOCK_SIZE,
            get_iso_area_cache(llc_num_sets, llc_num_ways, cachesim::BLOCK_SIZE),
            [&]{ auto b = cachesim::TAG_BDI_BASES; cachesim::TAG_BDI_BASES = 0; auto w = get_iso_area_cache(llc_num_sets, llc_num_ways, cachesim::BLOCK_SIZE); cachesim::TAG_BDI_BASES = b; return w; }());
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
    //cache[0] = new Cache<Set>("L1D", 128, 16, cachesim::BLOCK_SIZE, 0, false, ReplacementPolicy::LRU, insertion_policy);
    cache.push_back(new Cache<SectoredSet>("L1D", cachesim::L1_NUM_SETS, cachesim::L1_NUM_WAYS, 8, CacheLevel::L1D, true, ReplacementPolicy::LRU, insertion_policy));
    if (cachesim::L2_ENABLED) {
        cache.push_back(new Cache<SectoredSet>("L2", cachesim::L2_NUM_SETS, cachesim::L2_NUM_WAYS, 8, CacheLevel::L2, true, ReplacementPolicy::LRU, insertion_policy));
    }
    GRASP::trace_path = tracename;
    GRASP::llc_bytes = llc_num_sets * llc_num_ways * CACHELINE_SIZE;
    if (replacement_policy == ReplacementPolicy::POPT) {
        // P-OPT gives up LLC ways for two epochs of its rereference matrix.
        if (cachesim::BLOCK_SIZE != CACHELINE_SIZE) {
            fmt::print(stderr, "popt models a 64B LLC; use --cache-block-size 64\n");
            return 1;
        }
        if (!POPT::prescan(tracename))
            return 1;
        uint64_t reserved = POPT::reserved_ways(llc_num_sets, CACHELINE_SIZE);
        if (reserved >= llc_num_ways) {
            fmt::print(stderr, "popt: rereference matrix needs {} of {} LLC ways\n", reserved, llc_num_ways);
            return 1;
        }
        fmt::print("POPT: reserving {} of {} LLC ways for the rereference matrix\n", reserved, llc_num_ways);
        llc_num_ways -= reserved;
    }
    cache.push_back(new Cache<Set>("LLC", llc_num_sets, llc_num_ways, cachesim::BLOCK_SIZE, CacheLevel::LLC, false, replacement_policy, insertion_policy));
    if (cachesim::ENABLE_PREDICTOR) predictor->enable();
    else predictor->disable();
    if (cachesim::useMemSignature)
        predictor->set_mem_signature();
    else
        predictor->set_pc_signature();
#else
    cache.resize(1);
    cache[0] = new Cache<Set>("L1D", llc_num_sets, llc_num_ways, cachesim::BLOCK_SIZE, CacheLevel::L1D, false, replacement_policy, insertion_policy);
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

    // Print L1D, then the LLC, then any middle level (L2), so the LLC stays the
    // 2nd stats block - every results parser takes the 2nd MPKI line as the LLC.
    std::vector<BaseCache*> print_order{cache.front()};
    if (cache.size() > 1) print_order.push_back(cache.back());
    for (size_t i = 1; i + 1 < cache.size(); ++i) print_order.push_back(cache[i]);
    for (auto cache_inst: print_order) {
        cache_inst->print_stats(cachesim::instCount, tracename);
        delete cache_inst;
    }
    if (cachesim::GEN_STATS)
        predictor->print_stats();
    if (replacement_policy == ReplacementPolicy::HRU)
        Set::print_reuse_by_degree("HRU");
    if (replacement_policy == ReplacementPolicy::PHRU)
        PHRU::print_heuristic_stats();
    if (replacement_policy == ReplacementPolicy::PHRUpp)
        PHRUpp::print_heuristic_stats();
    if (replacement_policy == ReplacementPolicy::HubHawkeye)
        HubHawkeye::print_stats();
    if (replacement_policy == ReplacementPolicy::GRASP)
        GRASP::print_stats();
    if (replacement_policy == ReplacementPolicy::POPT)
        POPT::print_stats();
    if (cachesim::USE_GHOST_CACHE)
        fmt::print("Ghost cache: capacity {} inserts {} lookups {} hits {} evictions {} rejected {}\n",
            cachesim::ghostCache.capacity(), cachesim::ghostCache.debug_insert_count,
            cachesim::ghostCache.debug_lookup_count, cachesim::ghostCache.debug_hit_count,
            cachesim::ghostCache.debug_eviction_count, cachesim::ghostCache.debug_insert_rejected_count);

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
