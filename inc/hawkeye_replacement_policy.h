#ifndef __HAWKEYE_REPLACEMENT_POLICY_H__
#define __HAWKEYE_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"
#include <map>
#include <math.h>


#define MAX_SHCT 31
#define SHCT_SIZE_BITS 11
#define SHCT_SIZE (1 << SHCT_SIZE_BITS)
// OPTgen history: OPTGEN_QUANTA_PER_WAY x associativity time quanta (the paper's
// 8x; 128 for a 16-way set). The set timer wraps at TIMER_TO_VECTOR x that.
#define OPTGEN_QUANTA_PER_WAY 8
#define TIMER_TO_VECTOR 8
struct AddrInfo {
    uint64_t addr;
    uint32_t last_quanta;
    uint64_t PC;
    bool prefetched;
    uint32_t lru;

    void init(unsigned int /*curr_quanta*/)
    {
        last_quanta = 0;
        PC = 0;
        prefetched = false;
        lru = 0;
    }

    void update(unsigned int curr_quanta, uint64_t _pc, bool /*prediction*/)
    {
        last_quanta = curr_quanta;
        PC = _pc;
    }

    void mark_prefetch()
    {
        prefetched = true;
    }
};

struct OPTgen {
    std::vector<uint64_t> liveness_history;

    uint64_t num_cache;
    uint64_t num_dont_cache;
    uint64_t access;

    uint64_t CACHE_SIZE;

    // vector_size: time quanta of history.
    void init(uint64_t size, uint64_t vector_size) {
        num_cache = 0;
        num_dont_cache = 0;
        access = 0;
        CACHE_SIZE = size;
        liveness_history.resize(vector_size, 0);
    }

    void add_access(uint64_t curr_quanta) {
        access++;
        liveness_history[curr_quanta] = 0;
    }

    void add_prefetch(uint64_t curr_quanta) {
        liveness_history[curr_quanta] = 0;
    }

    bool should_cache(uint64_t curr_quanta, uint64_t last_quanta) {
        bool is_cache = true;
        unsigned int i = last_quanta;
        while (i != curr_quanta) {
            if(liveness_history[i] >= CACHE_SIZE) {
                is_cache = false;
                break;
            }
            i = (i+1) % liveness_history.size();
        }

        //if ((is_cache) && (last_quanta != curr_quanta))
        if ((is_cache)) {
            i = last_quanta;
            while (i != curr_quanta) {
                liveness_history[i]++;
                i = (i+1) % liveness_history.size();
            }
            assert(i == curr_quanta);
        }

        if (is_cache) num_cache++;
        else num_dont_cache++;

        return is_cache;    
    }

    uint64_t get_num_opt_hits() {
        return num_cache;
        uint64_t num_opt_misses = access - num_cache;
        return num_opt_misses;
    }
};


class HawkeyePCPredictor {
    std::map<uint64_t, short unsigned int > SHCT;

    public:

    static uint64_t CRC(uint64_t _blockAddress) {
        static const unsigned long long crcPolynomial = 3988292384ULL;
        unsigned long long _returnVal = _blockAddress;
        for( unsigned int i = 0; i < 32; i++ )
            _returnVal = ( ( _returnVal & 1 ) == 1 ) ? ( ( _returnVal >> 1 ) ^ crcPolynomial ) : ( _returnVal >> 1 );
        return _returnVal;
    }

    void increment (uint64_t pc) {
        uint64_t signature = CRC(pc) % SHCT_SIZE;
        if(SHCT.find(signature) == SHCT.end())
            SHCT[signature] = (1+MAX_SHCT)/2;

        SHCT[signature] = (SHCT[signature] < MAX_SHCT) ? (SHCT[signature]+1) : MAX_SHCT;

    }

    void decrement (uint64_t pc) {
        uint64_t signature = CRC(pc) % SHCT_SIZE;
        if(SHCT.find(signature) == SHCT.end())
            SHCT[signature] = (1+MAX_SHCT)/2;
        if(SHCT[signature] != 0)
            SHCT[signature] = SHCT[signature]-1;
    }

    bool get_prediction (uint64_t pc) {
        uint64_t signature = CRC(pc) % SHCT_SIZE;
        if(SHCT.find(signature) != SHCT.end() && SHCT[signature] < ((MAX_SHCT+1)/2))
            return false;
        return true;
    }
};

class Hawkeye : public BasePolicy {
    protected:
    uint64_t maxRRPV;
    uint64_t mytimer;
    uint64_t num_sets;   // of the owning cache: picks the sampled sets
    OPTgen optgen;
    std::vector<uint64_t> counter;
    std::vector<uint64_t> signatures;

    // Sample Cache
    inline static uint64_t sampled_cache_size;
    inline static uint64_t sampler_ways;
    inline static uint64_t sampler_sets;
    inline static std::vector<std::map<uint64_t, AddrInfo>> addr_history;

    inline static HawkeyePCPredictor* predictor;
    std::vector<bool> low_priority;

    // Block size this policy's sets hold (LLC: --cache-block-size; L1D: 64B).
    // The sampler / OPTgen are keyed on blocks of this size.
    uint64_t block_size;
    uint64_t block_shift() const { return (uint64_t)log2(block_size); }

    // OPTgen history and timer, sized from the set's associativity in 64B
    // lines (OPTGEN_QUANTA_PER_WAY x ways, timer TIMER_TO_VECTOR x that). The
    // window is the same at any block size: num_ways counts blocks, so it is
    // scaled back to 64B ways.
    uint64_t optgen_vector_size;
    uint64_t timer_size;

    // Extension hooks (no-ops here, so Hawkeye itself is unchanged); HubHawkeye
    // overrides them. paddr is the block address the way holds.
    // OPTgen's label for a sampled block, at the point the PC is trained.
    virtual void train_sampled(uint64_t /*paddr*/, bool /*friendly*/) {}
    // Final insertion prediction given the PC's (SHCT) prediction.
    virtual bool predict(uint64_t /*way_idx*/, uint64_t /*block_idx*/, uint64_t /*paddr*/, PacketPtr /*packet*/, bool pc_prediction) { return pc_prediction; }
    // A cache-friendly line was evicted as the LRU victim (all sets).
    virtual void lru_evicted(uint64_t /*way_idx*/) {}

    public:
    Hawkeye() {}
    Hawkeye(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level, uint64_t _num_sets) {
        set_idx = _set_idx;
        num_sets = _num_sets;
        num_ways = _num_ways;
        maxRRPV = 3;
        counter.resize(num_ways, maxRRPV);
        signatures.resize(num_ways, 0);
        level = _level;
        block_size = (_level == CacheLevel::LLC) ? cachesim::BLOCK_SIZE : CACHELINE_SIZE;
        reserved_ways = 0;
        mytimer = 0;
        optgen_vector_size = OPTGEN_QUANTA_PER_WAY * num_ways * block_size / CACHELINE_SIZE;
        timer_size = TIMER_TO_VECTOR * optgen_vector_size;
        optgen.init(num_ways - 2, optgen_vector_size);

        sampled_cache_size = 2800;
        sampler_ways = 8;
        sampler_sets = (Hawkeye::sampled_cache_size/Hawkeye::sampler_ways);

        addr_history.resize(sampler_sets);
        predictor = new HawkeyePCPredictor();
    }
    
    void init_counter(PacketPtr packet);

    void hit_update(PacketPtr packet, uint64_t way_idx);

    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);

    uint64_t get_eviction_candidate(bool is_low_priority);

    uint64_t get_reserved_eviction_candidate(bool is_low_priority);

    void evict(uint64_t way_idx);

    uint64_t get_counter_value(uint64_t way_idx);

    uint64_t count_distance(uint64_t threshold);

    void repartition_ways(uint64_t ways_to_reserve);

    uint64_t get_reserved_ways() {return reserved_ways;}

    bool can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) { return true;}

    void replace_addr_history_element(uint64_t sampler_set);
    void update_addr_history_lru(uint64_t sampler_set, uint64_t curr_lru);


    // Storage of Hawkeye's predictor state in hardware bits, as this code sizes it (per-line RRPV and PC
    // signature are not counted: they are per-way state every Hawkeye variant carries).
    //   SHCT    : SHCT_SIZE x 5-bit counters (0..MAX_SHCT)
    //   sampler : sampled_cache_size entries x (8-bit tag (CRC % 256) + last_quanta (log2 timer_size) +
    //             SHCT_SIZE_BITS PC signature + log2(sampler_ways) LRU + prefetched + valid)
    //   OPTgen  : sampled sets (num_sets/64) x optgen_vector_size occupancy counters of log2(num_ways - 1) bits
    struct Budget { uint64_t shct, sampler, optgen; uint64_t total() const { return shct + sampler + optgen; } };
    static uint64_t ceil_log2(uint64_t x) { uint64_t b = 0; while ((1ULL << b) < x) b++; return b; }
    Budget predictor_budget_bits() const {
        Budget b;
        b.shct = SHCT_SIZE * ceil_log2(MAX_SHCT + 1);
        b.sampler = sampled_cache_size * (8 + ceil_log2(timer_size) + SHCT_SIZE_BITS + ceil_log2(sampler_ways) + 1 + 1);
        uint64_t sampled_sets = num_sets <= 64 ? num_sets : num_sets / 64;
        b.optgen = sampled_sets * optgen_vector_size * ceil_log2(num_ways - 1);
        return b;
    }

    inline uint64_t bitmask(uint64_t l) {
        return (((l) == 64) ? (uint64_t)(-1ULL) : ((1ULL << (l))-1ULL));
    }

    inline uint64_t bits(uint64_t x, uint64_t i, uint64_t l) {
        return (((x) >> (i)) & bitmask(l));
    }

    // Sampled sets: the low 6 index bits equal the top 6 of the cache's own index (ChampSim's Hawkeye rule).
    // The fields overlap below 4096 sets, so this samples num_sets/64 sets (1024 -> 16, 4096 -> 64); a cache
    // with 64 sets or fewer samples every set. (Was hardcoded to a 1024-set index: same count, but at 2048/4096
    // sets it ignored the top index bits and picked a different subset.)
    inline bool is_sampled_set(uint64_t set_idx) {
        uint64_t lg = (uint64_t)log2(num_sets);
        if (lg <= 6) return true;
        return (bits(set_idx, 0 , 6) == bits(set_idx, lg - 6, 6));
    }

    void set_breakdown(uint64_t old_block_size, uint64_t new_block_size);

    template<typename VecType>
    void breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool incr);

    void set_contract(uint64_t way_idx, uint64_t size);

    void set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways);

    template<typename VecType>
    void contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks);
};

#endif
