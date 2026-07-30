#ifndef __HAWKEYE_REPLACEMENT_POLICY_H__
#define __HAWKEYE_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"
#include <map>
#include <math.h>


#define MAX_SHCT 31
#define SHCT_SIZE_BITS 11
#define SHCT_SIZE (1 << SHCT_SIZE_BITS)
#define OPTGEN_VECTOR_SIZE 128
#define TIMER_SIZE 1024;
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

    void init(uint64_t size) {
        num_cache = 0;
        num_dont_cache = 0;
        access = 0;
        CACHE_SIZE = size;
        liveness_history.resize(OPTGEN_VECTOR_SIZE, 0);
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
    uint64_t maxRRPV;
    uint64_t mytimer;
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
    public:
    Hawkeye() {}
    Hawkeye(uint64_t _set_idx, uint64_t _num_ways, uint64_t _level) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        maxRRPV = 7;
        counter.resize(num_ways, maxRRPV);
        signatures.resize(num_ways, 0);
        level = _level;
        reserved_ways = 0;
        mytimer = 0;
        optgen.init(num_ways - 2);

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


    inline uint64_t bitmask(uint64_t l) {
        return (((l) == 64) ? (uint64_t)(-1ULL) : ((1ULL << (l))-1ULL));
    }

    inline uint64_t bits(uint64_t x, uint64_t i, uint64_t l) {
        return (((x) >> (i)) & bitmask(l));
    }

    inline bool is_sampled_set(uint64_t set_idx) {
        return (bits(set_idx, 0 , 6) == bits(set_idx, ((uint64_t)log2(/*num_sets*/ 1024) - 6), 6));
    }

};

#endif
