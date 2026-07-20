#include "cachesim.h"

uint64_t cachesim::instCount = 0;
uint64_t cachesim::prevInstCount = 0;

bool cachesim::DEBUG = false;
bool cachesim::L1_DEBUG = false;
bool cachesim::LLC_DEBUG = false;
bool cachesim::REPLACEMENT_POLICY_DEBUG = false;
bool cachesim::NO_ISO_AREA = false;
bool cachesim::GEN_STATS = false;
bool cachesim::ENABLE_PREDICTOR = false;
uint64_t g_block_size = CACHELINE_SIZE;
//TODO: Figure out a good value
uint64_t cachesim::WARMUP_INSTRUCTIONS = 0;//2*16*2048;
bool cachesim::dropBlocks = false;
bool cachesim::useMemSignature = false;
bool cachesim::isoArea = false;
uint64_t cachesim::DEBUG_INSTRUCTIONS = 10000000;

bool performance::DETAILED_DRAM = false;

PerformanceModel cachesim::performanceModel;

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
    uint64_t block_size = CACHELINE_SIZE;
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
    app.add_option("--cache-block-size", block_size, "Cache block size");

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
        {"fission", ReplacementPolicy::Fission},
        {"distillation", ReplacementPolicy::Distillation},
        {"hawkeye", ReplacementPolicy::Hawkeye}
    }));
    app.add_option("--insertion-policy", insertion_policy, "Cache insertion policy")->transform(CLI::CheckedTransformer(std::map<std::string, InsertionPolicy>{
        {"exclusive", InsertionPolicy::EXCLUSIVE},
    }));
    app.add_flag("--debug", cachesim::DEBUG, "Enable debug mode");
    app.add_flag("--debug-llc", cachesim::LLC_DEBUG, "Enable debug mode");
    app.add_flag("--debug-l1", cachesim::L1_DEBUG, "Enable debug mode");
    app.add_flag("--debug-replacement-policy", cachesim::REPLACEMENT_POLICY_DEBUG, "Enable debug mode");
    app.add_flag("--no-iso-area", cachesim::NO_ISO_AREA, "Disable iso-area mode");
    app.add_option("--iters", num_iters, "Number of iterations");
    app.add_option("--warmup-instructions", cachesim::WARMUP_INSTRUCTIONS, "Warmup instruction count");
    app.add_flag("--gen-stats", cachesim::GEN_STATS, "Disable iso-area mode");
    app.add_flag("--enable-predictor", cachesim::ENABLE_PREDICTOR, "Enable predictor (not implemented)");
    app.add_flag("--detailed-dram", performance::DETAILED_DRAM, "Enable detailed DRAM timing model");

    CLI11_PARSE(app, argc, argv);
    g_block_size = block_size;

    switch(replacement_policy) {
        case ReplacementPolicy::Fission:
            cachesim::dropBlocks = true;
            break;
        case ReplacementPolicy::Distillation:
            cachesim::dropBlocks = false;
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
        default:
            cachesim::dropBlocks = false;
            break;
    }

    llc_num_ways = get_iso_area_cache(llc_num_sets, llc_num_ways, block_size);
    
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
    if (block_size == CACHELINE_SIZE)
        cache[0] = new Cache<Set>("L1D", 128, 16, block_size, 0, false, ReplacementPolicy::LRU, insertion_policy);
    else
        cache[0] = new Cache<SectoredSet>("L1D", 128, 16, block_size, 0, true, ReplacementPolicy::LRU, insertion_policy);
    cache[1] = new Cache<Set>("LLC", llc_num_sets, llc_num_ways, block_size, 1, false, replacement_policy, insertion_policy);
    if (cachesim::ENABLE_PREDICTOR) predictor->enable();
    else predictor->disable();
    if (cachesim::useMemSignature)
        predictor->set_mem_signature();
    else
        predictor->set_pc_signature();
#else
    cache.resize(1);
    cache[0] = new Cache<Set>("L1D", llc_num_sets, llc_num_ways, block_size, 0, false, replacement_policy, insertion_policy);
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

    for (auto cache_inst: cache)
        cache_inst->print_stats(cachesim::instCount, tracename);
    if (cachesim::GEN_STATS)
        predictor->print_stats();
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
