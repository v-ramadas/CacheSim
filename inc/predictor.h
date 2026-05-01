#ifndef __PREDICTOR_H__
#define __PREDICTOR_H__

#include "packet.h"
#include <stdint.h>
#include <cassert>
#include <map>

struct PredictorMetadata {
    uint64_t pc;
    uint64_t accesses = 0;
    uint64_t reuses = 0;
    uint64_t evictions = 0;
    uint64_t footprint = 0xff;
    double reuse_distance = 0;
    uint64_t last_accessed = 0;

    double get_reuse_probability() {
        return (double)(reuses)/(double)(accesses);
    }

    void print() {
        fmt::print("Accesses {} Reuses {} Reuse Probability {:4f} Evictions {} Footprint {} Reuse Distance {}",
                accesses, reuses, get_reuse_probability(), evictions, footprint, reuse_distance);
    }

    PredictorMetadata(uint64_t _pc):
        pc(_pc) {}
};

class SparsityPredictor {
    private:
        bool _enable;
        double threshold;
        uint64_t size;
        float default_footprint;
        uint64_t warmup_accesses;
        uint64_t accesses=0;
//        std::map<uint64_t, std::pair<uint64_t, float>> history;
        std::map<uint64_t, PredictorMetadata*> history;

    public:
        SparsityPredictor(double threshold, uint64_t size,
                float default_footprint, uint64_t warmup_accesses):
            threshold(threshold), size(size),
            default_footprint(default_footprint), warmup_accesses(warmup_accesses) {
                assert(threshold > 0 && threshold <= 1);
                assert(default_footprint > 0 && default_footprint <= 8);
                disable();
        }

        ~SparsityPredictor() {
            for (auto it = history.begin(); it != history.end(); it++) {
                delete it->second;
            }
        }
        
        bool predict(PacketPtr packet);
        uint64_t get_footprint(PacketPtr packet);
        double get_reuse_probability(PacketPtr packet);
        uint64_t get_reuse_distance(PacketPtr packet);
        void print_stats();
        void update_access(PacketPtr packet);
        void update_eviction(PacketPtr packet);
        void clear() {
            history.clear();
        }
        void enable() {_enable = true;}
        void disable() {_enable = false;}
};
#endif
