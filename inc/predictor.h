#ifndef __PREDICTOR_H__
#define __PREDICTOR_H__

#include "packet.h"
#include <stdint.h>
#include <cassert>
#include <map>
class SparsityPredictor {
    private:
        bool _enable;
        float threshold;
        uint64_t size;
        float default_footprint;
        uint64_t warmup_accesses;
        std::map<uint64_t, std::pair<uint64_t, float>> history;
        std::map<uint64_t, uint64_t> pc_map;

    public:
        SparsityPredictor(uint64_t threshold, uint64_t size,
                float default_footprint, uint64_t warmup_accesses):
            threshold(threshold), size(size),
            default_footprint(default_footprint), warmup_accesses(warmup_accesses) {
                assert(threshold > 0 && threshold <= 8);
                assert(default_footprint > 0 && default_footprint <= 8);
                disable();
            }
        
        bool predict(PacketPtr packet);
        void insert(PacketPtr packet);
        void update(PacketPtr packet);
        void clear() {
            history.clear();
        }
        void enable() {_enable = true;}
        void disable() {_enable = false;}
};
#endif
