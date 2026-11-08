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
uint64_t cachesim::PSEL_HYSTERESIS = UINT64_MAX;   // unset: PSEL_MAX / 8
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
bool cachesim::AMOEBA = false;
AmoebaInsert cachesim::AMOEBA_INSERT = AmoebaInsert::SPAN;
uint64_t cachesim::AMOEBA_WORDS = 0;
bool cachesim::AMOEBA_FREE_TAGS = false;
bool cachesim::L1_AMOEBA = false;
L1AmoebaFetch cachesim::L1_AMOEBA_FETCH = L1AmoebaFetch::HISTORY;
bool cachesim::L1_AMOEBA_KEEP_LLC = false;
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
        {"popt", ReplacementPolicy::POPT},
        {"amoeba", ReplacementPolicy::Amoeba}
    }));
    app.add_option("--insertion-policy", insertion_policy, "Cache insertion policy")->transform(CLI::CheckedTransformer(std::map<std::string, InsertionPolicy>{
        {"exclusive", InsertionPolicy::EXCLUSIVE},
    }));
    app.add_flag("--debug", cachesim::DEBUG_ALL, "Enable debug mode");
    app.add_flag("--debug-llc", cachesim::LLC_DEBUG, "Enable debug mode");
    app.add_flag("--debug-l1", cachesim::L1_DEBUG, "Enable debug mode");
    app.add_flag("--debug-replacement-policy", cachesim::REPLACEMENT_POLICY_DEBUG, "Enable debug mode");
    app.add_flag("--no-iso-area", cachesim::NO_ISO_AREA, "Give a smaller-block LLC 64/block-size times the ways instead of sizing it iso-area with the 64B LLC (iso-area is the default for every policy except hrupp and phrupp, which never are)");
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
    app.add_option("--psel-hysteresis", cachesim::PSEL_HYSTERESIS, "PSEL mode: break down only below threshold - H, merge back only at or above threshold + H (default psel-max / 8; 0 = plain threshold)");
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
    app.add_option("--tag-bdi-bases", cachesim::TAG_BDI_BASES, "Base+Delta LLC tag compression: base tags per set (default 8; 0 = off, uncompressed 51-bit tags). Applies to iso-area 8B sets");
    app.add_option("--tag-bdi-delta", cachesim::TAG_BDI_DELTA, "Base+Delta tag compression: signed delta bits per block (default 10)");
    app.add_flag("--tag-bdi-unconstrained", cachesim::TAG_BDI_UNCONSTRAINED, "Base+Delta tags: keep a tag that fits no base in full (step 2, capacity-only bound) instead of replacing the base with the fewest users");
    app.add_flag("--debug-tag-bdi", cachesim::DEBUG_TAG_BDI, "Print every Base+Delta tag encode, lookup, release and base replacement (LLC)");
    app.add_option("--amoeba-insert", cachesim::AMOEBA_INSERT, "amoeba: words of an L1D victim to store as one block - span (first to last touched word, default), line (whole line)")->transform(CLI::CheckedTransformer(std::map<std::string, AmoebaInsert>{
        {"span", AmoebaInsert::SPAN},
        {"line", AmoebaInsert::LINE},
    }));
    app.add_option("--amoeba-words", cachesim::AMOEBA_WORDS, "amoeba (debug): words per set, overriding the iso-area / naive size");
    app.add_flag("--amoeba-free-tags", cachesim::AMOEBA_FREE_TAGS, "amoeba (debug): blocks take no tag word, only their data words");
    app.add_flag("--l1-amoeba", cachesim::L1_AMOEBA, "cachesim_multi: make the L1D an Amoeba-Cache (8B words, iso-area with the 64B L1D); a miss fetches the words the line used in its previous L1 residency (the whole line on its first touch), the rest of the line is dropped. Not with --l2, set dueling or an amoeba LLC");
    app.add_option("--l1-amoeba-fetch", cachesim::L1_AMOEBA_FETCH, "--l1-amoeba: what a miss fetches around the demanded word - history (span used in the line's previous L1 residency, default), line (whole line), oracle (span recorded for this residency, needs --l1-amoeba-oracle)")->transform(CLI::CheckedTransformer(std::map<std::string, L1AmoebaFetch>{
        {"history", L1AmoebaFetch::HISTORY},
        {"line", L1AmoebaFetch::LINE},
        {"oracle", L1AmoebaFetch::ORACLE},
    }));
    app.add_flag("--l1-amoeba-keep-llc", cachesim::L1_AMOEBA_KEEP_LLC, "--l1-amoeba: words of a line the L1D does not take stay in / go into the LLC as a partial line instead of being dropped (breaks line-level exclusivity)");
    std::string l1_amoeba_record, l1_amoeba_oracle;
    app.add_option("--l1-amoeba-record", l1_amoeba_record, "--l1-amoeba: write every L1 residency's used-word mask per line to this file (input for --l1-amoeba-oracle)");
    app.add_option("--l1-amoeba-oracle", l1_amoeba_oracle, "--l1-amoeba-fetch oracle: per-residency used-word masks recorded by --l1-amoeba-record");
    app.add_option("--popt-elem-size", POPT::elem_size, "popt: bytes per element of the irregularly accessed array (default 4; must match the matrix)");
    app.add_option("--popt-matrix-dir", POPT::matrix_dir, "popt: directory holding <G>.<order>[.wd].popt Rereference Matrices (tools/popt_matrix.cc); also searched: data/popt, <bin>/../data/popt");


    CLI11_PARSE(app, argc, argv);
    if (cachesim::PSEL_HYSTERESIS == UINT64_MAX)
        cachesim::PSEL_HYSTERESIS = cachesim::PSEL_MAX / 8;

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

    // Smaller-block LLCs are sized iso-area with the 64B LLC unless
    // --no-iso-area; HRUpp and PHRUpp are the non-iso-area bounds and turn it
    // off below.
    cachesim::isoArea = !cachesim::NO_ISO_AREA;
    switch(replacement_policy) {
        case ReplacementPolicy::LRU:
            cachesim::SET_DUELING = false;
            break;
        case ReplacementPolicy::HRRIP:
            cachesim::dropBlocks = true;
            break;
        case ReplacementPolicy::HRU:
            cachesim::dropBlocks = true;
            break;
        case ReplacementPolicy::HRUpp:
            cachesim::dropBlocks = true;
            cachesim::isoArea = false;
            break;
        case ReplacementPolicy::PHRUpp:
            cachesim::dropBlocks = false;
            cachesim::isoArea = false;
            break;
        case ReplacementPolicy::PHRU:
        case ReplacementPolicy::HubOracle:
            cachesim::dropBlocks = true;
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
        case ReplacementPolicy::Amoeba:
            // Amoeba sets keep their own tag words, so Base+Delta tags are off;
            // sets are sized from the 64B budget unless --no-iso-area.
            if (cachesim::BLOCK_SIZE != 8) {
                fmt::print(stderr, "amoeba models a word-granularity LLC; use --cache-block-size 8\n");
                return 1;
            }
            cachesim::AMOEBA = true;
            cachesim::TAG_BDI_BASES = 0;
            cachesim::SET_DUELING = false;
            break;
        default:
            cachesim::dropBlocks = false;
            break;
    }

    if (cachesim::BLOCK_SIZE == 64) {
        cachesim::SET_DUELING = false;
    }

    if (cachesim::TAG_BDI_BASES > 0 && cachesim::isoArea && cachesim::BLOCK_SIZE < CACHELINE_SIZE) {
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
    if (cachesim::L1_AMOEBA) {
        if (cachesim::L2_ENABLED || cachesim::SET_DUELING || cachesim::AMOEBA) {
            fmt::print(stderr, "--l1-amoeba supports two levels only, without set dueling or an amoeba LLC\n");
            return 1;
        }
        cache.push_back(new Cache<Set>("L1D", cachesim::L1_NUM_SETS, cachesim::L1_NUM_WAYS, 8, CacheLevel::L1D, false, ReplacementPolicy::Amoeba, insertion_policy));
        if (cachesim::L1_AMOEBA_FETCH == L1AmoebaFetch::ORACLE && !Set::load_l1_oracle(l1_amoeba_oracle)) {
            fmt::print(stderr, "--l1-amoeba-fetch oracle: cannot read --l1-amoeba-oracle '{}'\n", l1_amoeba_oracle);
            return 1;
        }
        Set::am_l1_recording = !l1_amoeba_record.empty();
        fmt::print("Amoeba L1D: {} sets x {} words (iso-area with {} x 64B ways)\n", cachesim::L1_NUM_SETS,
            get_amoeba_words(cachesim::L1_NUM_SETS, cachesim::L1_NUM_WAYS), cachesim::L1_NUM_WAYS);
    } else {
        cache.push_back(new Cache<SectoredSet>("L1D", cachesim::L1_NUM_SETS, cachesim::L1_NUM_WAYS, 8, CacheLevel::L1D, true, ReplacementPolicy::LRU, insertion_policy));
    }
    if (cachesim::L2_ENABLED) {
        cache.push_back(new Cache<SectoredSet>("L2", cachesim::L2_NUM_SETS, cachesim::L2_NUM_WAYS, 8, CacheLevel::L2, true, ReplacementPolicy::LRU, insertion_policy));
    }
    GRASP::trace_path = tracename;
    GRASP::llc_bytes = llc_num_sets * llc_num_ways * CACHELINE_SIZE;
    if (replacement_policy == ReplacementPolicy::POPT) {
        // P-OPT gives up LLC ways for two epochs of its rereference matrix, taken from the 64B-equivalent ways
        // before iso-area sizing. Sub-64B blocks use the same 64B-granularity matrix (coarse next references).
        if (!POPT::setup(tracename))
            return 1;
        uint64_t reserved = POPT::active ? POPT::reserved_ways(llc_num_sets, CACHELINE_SIZE) : 0;
        if (reserved >= llc_num_ways) {
            fmt::print(stderr, "popt: rereference matrix needs {} of {} LLC ways\n", reserved, llc_num_ways);
            return 1;
        }
        if (POPT::active)
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
    if (replacement_policy == ReplacementPolicy::Belady) {
        // OPT needs the future. 64B blocks: the trace's per-64B NextReuse (carried with each line) is exact. Smaller
        // blocks: per-word next uses are not in the trace, so build the 8B next-use oracle (instruction traces).
        Belady::block_size = cachesim::BLOCK_SIZE;
        if (trace_format == TraceFormat::INSTRUCTIONS && cachesim::BLOCK_SIZE < CACHELINE_SIZE && !Belady::prescan(tracename)) {
            fmt::print(stderr, "Belady: cannot read trace {} for the next-use oracle\n", tracename);
            return 1;
        }
    }
    if (trace_format == TraceFormat::ADDRESSES) {
        useAddressTrace(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, invalidation_packet, num_iters);
    } else if (trace_format == TraceFormat::INSTRUCTIONS) {
        useInstructionTrace(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, invalidation_packet, num_iters);
    } else {
        useChampsimTrace(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, invalidation_packet, num_iters);
    }

    // Print L1D, then the LLC, then any middle level (L2), so the LLC stays the
    // 2nd stats block - every results parser takes the 2nd MPKI line as the LLC.
    std::vector<std::string> level_names;   // print_pc_hub_stats prints after the caches are deleted
    for (auto c: cache) level_names.push_back(c->get_name());
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
    if (replacement_policy == ReplacementPolicy::Belady)
        Belady::print_stats();
    if (replacement_policy == ReplacementPolicy::LRU && cachesim::BLOCK_SIZE < CACHELINE_SIZE)
        LRU::print_stats();
    if (replacement_policy == ReplacementPolicy::Amoeba)
        Set::print_amoeba_stats();
    if (cachesim::L1_AMOEBA)
        Set::print_l1_amoeba_stats();
    print_pc_hub_stats(level_names, PC_HUB_TOP_N);
    if (cachesim::L1_AMOEBA && !l1_amoeba_record.empty() && !Set::save_l1_record(l1_amoeba_record))
        fmt::print(stderr, "--l1-amoeba-record: cannot write '{}'\n", l1_amoeba_record);
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
