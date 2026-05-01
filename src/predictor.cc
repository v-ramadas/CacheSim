#include "cachesim.h"
#include "predictor.h"

bool SparsityPredictor::predict(PacketPtr packet) {
    if (!_enable) return false;
    bool is_sparse = false;
    if (history.find(packet->pc) == history.end()) {
        return false; // default to dense
    }

    if (history[packet->pc]->accesses < warmup_accesses) {
        return false; // default to dense during warmup
    }
    
    double reuse_ratio = history[packet->pc]->get_reuse_probability();
    is_sparse = (reuse_ratio < threshold);
    return is_sparse;
}

void SparsityPredictor::update_access(PacketPtr packet) {
    if (!_enable) return;

    auto pc = packet->pc;
    if (history.find(pc) == history.end()) {
        history[packet->pc] = new PredictorMetadata(packet->pc);
        if (cachesim::DEBUG)
            fmt::print("PredictorModel: Inserting PC {:#x} with default_footprint {}\n", packet->pc, default_footprint);
    }

    auto footprint = __builtin_popcountll(packet->footprint);
    auto serviced_from_llc = packet->serviced_from_llc;
    auto pc_accesses = history[pc]->accesses;
    auto old_footprint = history[pc]->footprint;
    if (serviced_from_llc) {
        history[pc]->reuses++;
        history[pc]->footprint = (old_footprint*pc_accesses + footprint)/(pc_accesses+1); // update footprint
        auto delta_access = double(accesses - history[pc]->last_accessed);
        if (history[pc]->last_accessed == 0) {
            history[pc]->reuse_distance = delta_access;
        } else if (history[pc]->reuse_distance <= delta_access) {
            history[pc]->reuse_distance += std::min(1.0, delta_access/16.0);
        } else {
            history[pc]->reuse_distance -= std::min(1.0, delta_access/16.0);
        }
        history[pc]->last_accessed = accesses;
    }
    history[pc]->accesses++; // update access count
    accesses++;

}

void SparsityPredictor::update_eviction(PacketPtr packet) {
    if (!_enable) return;

    auto pc = packet->pc;
    if (history.find(pc) == history.end()) {
        history[packet->pc] = new PredictorMetadata(packet->pc);
        if (cachesim::DEBUG)
            fmt::print("PredictorModel: Inserting PC {:#x} with default_footprint {}\n", packet->pc, default_footprint);
    }

    history[pc]->evictions++; // update access count
}

uint64_t SparsityPredictor::get_footprint(PacketPtr packet) {
    if (!_enable) return 0xff;

    if (history.find(packet->pc) == history.end()) {
        return 0xff; // default to dense
    }

    if (history[packet->pc]->accesses < warmup_accesses) {
        return 0xff; // default to dense during warmup
    }
    
    return history[packet->pc]->footprint;
}

double SparsityPredictor::get_reuse_probability(PacketPtr packet) {
    if (!_enable) return 0;

    if(history.find(packet->pc) == history.end()) {
        return 0;
    }

    return history[packet->pc]->get_reuse_probability();

}

uint64_t SparsityPredictor::get_reuse_distance(PacketPtr packet) {
    if (!_enable) return 0;

    if(history.find(packet->pc) == history.end()) {
        return UINT64_MAX;
    }

    return history[packet->pc]->reuse_distance;

}


void SparsityPredictor::print_stats() {
    for (auto it = history.begin(); it != history.end(); it++) {
        fmt::print("PC {:#x} Predictor Stats ", it->first);
        it->second->print();
        fmt::print("\n");
    }
                
}
