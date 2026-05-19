#ifndef __PREDICTOR_H__
#define __PREDICTOR_H__

#include "utils.h"
#include <stdint.h>
#include <cassert>
#include <map>

struct PredictorMetadata {
    uint64_t pc;
    uint64_t accesses = 0;
    uint64_t reuses = 0;
    uint64_t evictions = 0;
    uint64_t footprint = 0xff;
    uint64_t num_unique_blocks_accessed = 0;
    double reuse_distance = 0;
    uint64_t last_accessed = 0;
    uint64_t serviced_from_llc = 0;
    std::vector<uint64_t> footprint_stats = std::vector<uint64_t>(8, 0);

    ~PredictorMetadata() {
        footprint_stats.clear();
    }

    double get_reuse_probability() {
        if (accesses == 0)
            return 0.0;
        return (double)(reuses)/(double)(accesses);
    }

    void print() {
        fmt::print("Accesses {} Reuses {} Reuse Probability {:4f} Evictions {} Footprint {} Reuse Distance {} Num LLC Promotions Per Eviction {:4f}",
                accesses, reuses, get_reuse_probability(), evictions, footprint, reuse_distance, (float)(serviced_from_llc)/(float)(evictions));
    }

    void print_footprint() {
         for (auto idx = 0; idx < footprint_stats.size(); idx++)
            fmt::print("PC {:#x} num blocks touched {:#x} count {}\n",
                    pc, idx+1, footprint_stats[idx]);
    }


    PredictorMetadata(uint64_t _pc):
        pc(_pc) {}
};

class SparsityPredictor {
    private:
        bool _enable;
        bool mem_signature = false;
        uint64_t mem_region_size = 1*64;
        double threshold;
        uint64_t size;
        uint64_t warmup_accesses;
        uint64_t accesses=0;
        std::map<uint64_t, PredictorMetadata*> history;
        std::map<uint64_t, uint64_t> SHCT;
        std::map<uint64_t, std::map<uint64_t, uint64_t>> Fission;
        std::map<uint64_t, uint64_t> fission_count;

    public:
        SparsityPredictor(double threshold, uint64_t size,
                uint64_t warmup_accesses):
            threshold(threshold), size(size),
            warmup_accesses(warmup_accesses) {
                assert(threshold > 0 && threshold <= 1);
                disable();
        }

        ~SparsityPredictor() {
            for (auto it = history.begin(); it != history.end(); it++) {
                it->second->footprint_stats.clear();
                delete it->second;
            }
        }
        
        bool predict(PacketPtr packet);
        bool predict_footprint_basic(PacketPtr packet, uint64_t signature);
        bool predict_reuse_probability(PacketPtr packet, uint64_t signature);
        bool is_hub_node(PacketPtr packet);
        uint64_t get_footprint(PacketPtr packet);
        double get_reuse_probability(PacketPtr packet);
        uint64_t get_reuse_distance(PacketPtr packet);
        uint64_t get_shct_value(PacketPtr packet);
        void print_stats();
        void update_access(PacketPtr packet);
        void update_footprint(PacketPtr packet);
        void update_eviction(PacketPtr packet);
        void clear() {
            history.clear();
        }
        void enable() {_enable = true;}
        void disable() {_enable = false;}
        void set_pc_signature() { mem_signature = false; }
        void set_mem_signature() { mem_signature = true; }
};
#endif
