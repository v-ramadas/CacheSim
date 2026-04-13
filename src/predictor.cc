#include "cachesim.h"
#include "predictor.h"

bool SparsityPredictor::predict(PacketPtr packet) {
    if (!_enable) return false;
    bool is_sparse = false;
//    if (history.find(packet->pc) == history.end()) {
//        return false; // default to dense
//    }
//
//    if (std::get<0>(history[packet->pc]) < warmup_accesses) {
//        return false; // default to dense during warmup
//    }
//    
//    auto is_sparse = (std::get<1>(history[packet->pc]) < threshold);
    if (cachesim::DEBUG)
        fmt::print("PredictorModel: Predicting PC {:#x} address {:#x} as {} (footprint {:4f})\n", packet->pc, packet->address, is_sparse ? "sparse" : "dense", std::get<1>(history[packet->pc]));
    return is_sparse;
}

void SparsityPredictor::insert(PacketPtr packet) {
    if (!_enable) return;

//    if (history.find(packet->pc) == history.end()) {
//        history[packet->pc] = std::make_pair(1, default_footprint); // default footprint
//        if (cachesim::DEBUG)
//            fmt::print("PredictorModel: Inserting PC {:#x} with default_footprint {}\n", packet->pc, default_footprint);
//    }
}

void SparsityPredictor::update(PacketPtr packet) {
    if (!_enable) return;

//    auto pc = packet->pc;
//    auto footprint = __builtin_popcountll(packet->footprint);
//    auto accesses = std::get<0>(history[pc]);
//
//    auto old_footprint = std::get<1>(history[pc]);
//    std::get<1>(history[pc]) = (old_footprint*accesses + footprint)/(accesses+1); // update footprint
//    std::get<0>(history[pc]) = accesses + 1; // update access count
//    if (cachesim::DEBUG)
//        fmt::print("PredictorModel: Updating PC {:#x} with accesses {}, old_footprint {}, footprint of access {} new_footprint {}\n",
//                packet->pc, std::get<0>(history[pc]), old_footprint, footprint, std::get<1>(history[pc]));

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
