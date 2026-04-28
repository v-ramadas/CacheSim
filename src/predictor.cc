#include "cachesim.h"
#include "predictor.h"

bool SparsityPredictor::predict(PacketPtr packet) {
    if (!_enable) return false;
    bool is_sparse = false;
    if (history.find(packet->pc) == history.end()) {
        return false; // default to dense
    }

    if (std::get<0>(history[packet->pc]) < warmup_accesses) {
        return false; // default to dense during warmup
    }
    
    float reuse_ratio = (float)(std::get<1>(history[packet->pc]))/(float)(std::get<0>(history[packet->pc]));
    is_sparse = (reuse_ratio < threshold);
   if (cachesim::DEBUG)
        fmt::print("PredictorModel: Predicting PC {:#x} address {:#x} as {} (reuse ratio {:4f})\n", packet->pc, packet->address, is_sparse ? "sparse" : "dense", reuse_ratio);
    return is_sparse;
}

void SparsityPredictor::update(PacketPtr packet) {
    if (!_enable) return;

    auto pc = packet->pc;
    if (history.find(pc) == history.end()) {
        history[packet->pc] = std::make_pair(1, 0); // default footprint
        if (cachesim::DEBUG)
            fmt::print("PredictorModel: Inserting PC {:#x} with default_footprint {}\n", packet->pc, default_footprint);
    }

    //auto footprint = __builtin_popcountll(packet->footprint);
    auto serviced_from_llc = packet->serviced_from_llc;
    auto accesses = std::get<0>(history[pc]);

    //auto old_footprint = std::get<1>(history[pc]);
    //std::get<1>(history[pc]) = (old_footprint*accesses + footprint)/(accesses+1); // update footprint
    if (serviced_from_llc) {
        std::get<1>(history[pc]) = std::get<1>(history[pc]) + 1;
    }
    std::get<0>(history[pc]) = accesses + 1; // update access count
    if (cachesim::DEBUG) {
   //     fmt::print("PredictorModel: Updating PC {:#x} with accesses {}, old_footprint {}, footprint of access {} new_footprint {}\n",
   //             packet->pc, std::get<0>(history[pc]), old_footprint, footprint, std::get<1>(history[pc]));
    }

}

uint64_t SparsityPredictor::get_footprint(PacketPtr packet) {
    if (!_enable) return 0xff;

    auto footprint = 0xff;
//    if (history.find(packet->pc) == history.end()) {
//        return 0xff; // default to dense
//    }
//
//    if (std::get<0>(history[packet->pc]) < warmup_accesses) {
//        return 0xff; // default to dense during warmup
//    }
//    
//    footprint = (std::get<1>(history[packet->pc]));
    return footprint;
}

float SparsityPredictor::get_reuse_probability(PacketPtr packet) {
    if (!_enable) return 0;

    if(history.find(packet->pc) == history.end()) {
        return 0;
    }

    return ((float)(std::get<1>(history[packet->pc]))/(float)(std::get<0>(history[packet->pc])));

}

void SparsityPredictor::print_reuse_probability() {
    for (auto it = history.begin(); it != history.end(); it++) {
        auto reuse = ((float)(std::get<1>(it->second)))/(float)(std::get<0>(it->second));

        fmt::print("PC {:#x} Predictor Reuse Probabiltiy {:4f}\n", it->first, reuse);
    }
                
}
