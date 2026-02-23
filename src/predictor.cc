#include "cachesim.h"
#include "predictor.h"

bool SparsityPredictor::predict(PacketPtr packet) {
    if (history.find(packet->pc) == history.end()) {
        return false; // default to dense
    }

    if (std::get<0>(history[packet->pc]) < warmup_accesses) {
        return false; // default to dense during warmup
    }
    
    auto is_sparse = (std::get<1>(history[packet->pc]) < threshold);
    if (cachesim::DEBUG) {
        fmt::print("Predicting PC {:#x} address {:#x} as {} (footprint {:4f})\n", packet->pc, packet->address, is_sparse ? "sparse" : "dense", std::get<1>(history[packet->pc]));
    }
    return is_sparse;
}

void SparsityPredictor::insert(PacketPtr packet) {
    if (pc_map.find(packet->address) == pc_map.end()) {
        pc_map[packet->address] = packet->pc;
    }
    if (history.find(packet->pc) == history.end()) {
        history[packet->pc] = std::make_pair(0, default_footprint); // default footprint
    }
}

void SparsityPredictor::update(PacketPtr packet) {
    auto pc = pc_map[packet->address];
    pc_map.erase(packet->address); // clear mapping after update
    auto footprint = __builtin_popcountll(packet->footprint);
    auto accesses = std::get<0>(history[pc]);
    if (accesses > warmup_accesses) {
        return; // do not update after warmup
    }
    auto old_footprint = std::get<1>(history[pc]);
    std::get<1>(history[pc]) = (old_footprint*accesses + footprint)/(accesses+1); // update footprint
    std::get<0>(history[pc]) = accesses + 1; // update access count
}