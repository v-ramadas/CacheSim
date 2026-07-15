#include "cachesim.h"
#include "predictor.h"
#include <cmath>

bool SparsityPredictor::predict(PacketPtr packet) {
    if (!_enable) return false;
    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }
    if (history.find(signature) == history.end()) {
        return false; // default to dense
    }

    if (history[signature]->accesses < warmup_accesses) {
        return false; // default to dense during warmup
    }
    
    //return predict_footprint_basic(packet, signature);
    //return predict_reuse_probability(packet, signature);
    if (signature == 0x55dea38e87be && SHCT.find(signature) != SHCT.end()) {
        return (SHCT[signature] >= 1000);
    } else {
        return false;
    }
}

bool SparsityPredictor::predict_footprint_basic(PacketPtr packet, uint64_t signature) {
    if (packet->footprint == 0xff) return true;
    else return false;
}

bool SparsityPredictor::predict_reuse_probability(PacketPtr packet, uint64_t signature) {
    double reuse_ratio = history[signature]->get_reuse_probability();

    double logSum = 0.0;
    int count = 0;
    for (const auto& [key, value] : history) {
        if (value->get_reuse_probability() <= 0.0) {
            continue;
        }
        logSum += std::log(value->get_reuse_probability());  // natural log
        count++;
    }
    double geomean = std::exp(logSum/ count);

    bool is_high_reuse = (reuse_ratio >= geomean);
//    if (geomean < 0.2)
//    fmt::print("PC {:#x} reuse_ratio {:4f} geomean {:4f}\n", packet->pc, reuse_ratio, geomean);
    return is_high_reuse;
}

bool SparsityPredictor::is_hub_node(PacketPtr packet) {
    if (!_enable) return true;
    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }
    auto addr_region_signature = align_address(packet->address, mem_region_size);
    if (Fission[signature].find(addr_region_signature) == Fission[signature].end()) {
        return false;
    }

    float avg_fission_count = (float)(fission_count[signature])/Fission[signature].size();
    if (Fission[signature][addr_region_signature] > avg_fission_count) {
        if (packet->l1_hits > 5*__builtin_popcountll(packet->footprint)) {
//        fmt::print("Is a Hub Node! PC {:#x} addr {:#x}. Accesses {} avg accesses {} signature {:#x} l1_hits {} footprint {}\n", signature, packet->address, Fission[signature][addr_region_signature], avg_fission_count, addr_region_signature, packet->l1_hits, __builtin_popcountll(packet->footprint));
            return true;}
        else return false;
    } else {
//        fmt::print("Not a Hub Node! PC {:#x} addr {:#x}. Accesses {} avg accesses {} signature {:#x}\n", signature, packet->address, Fission[signature][addr_region_signature], avg_fission_count, addr_region_signature);
        return false;
    }
}


void SparsityPredictor::update_access(PacketPtr packet) {
    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }
    auto serviced_from_llc = packet->serviced_from_llc;
    //SHIP metadata
    if (SHCT.find(signature) == SHCT.end()) {
        SHCT[signature] = 0;
    }

    if (serviced_from_llc) {
        SHCT[signature]++;
    }

    auto addr_region_signature = align_address(packet->aligned_address, mem_region_size);
    if (Fission.find(signature) == Fission.end()) {
        Fission[signature] = std::map<uint64_t, uint64_t>();
        fission_count[signature] = 0;
    }
    if (Fission[signature].find(addr_region_signature) == Fission[signature].end()) {
        Fission[signature][addr_region_signature] = 0;
    }
    Fission[signature][addr_region_signature]+=packet->l1_hits;
    fission_count[signature]+=packet->l1_hits;

    if (!_enable) return;

    if (history.find(signature) == history.end()) {
        history[signature] = new PredictorMetadata(signature);
    }

    auto footprint = __builtin_popcountll(packet->footprint);
    auto signature_accesses = history[signature]->accesses;
    auto old_footprint = history[signature]->footprint;
    auto delta_access = double(accesses - history[signature]->last_accessed);
    auto mem_region = align_address(packet->address, CACHELINE_SIZE);
    if (history[signature]->last_accessed == 0) {
        history[signature]->reuse_distance = delta_access/16.0;
    } else if (history[signature]->reuse_distance <= delta_access) {
        history[signature]->reuse_distance += std::min(1.0, delta_access/8.0);
    } else {
        history[signature]->reuse_distance -= std::min(1.0, delta_access/8.0);
    }
    history[signature]->last_accessed = accesses;

    if (serviced_from_llc) {
        history[signature]->reuses++;
        history[signature]->footprint = (old_footprint*signature_accesses + footprint)/(signature_accesses+1); // update footprint
    }
    history[signature]->accesses++; // update access count
    accesses++;

}

void SparsityPredictor::update_footprint(PacketPtr packet) {
    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }

    if (history.find(signature) == history.end()) {
        history[signature] = new PredictorMetadata(signature);
    }

    auto footprint = __builtin_popcountll(packet->footprint);

    //fmt::print("Footprint {} size {}\n", footprint - 1, history[signature]->footprint_stats.size());
    if (footprint == 0) {
        // No eviction
        return;
    }
    history[signature]->footprint_stats.at(footprint-1)++;
//    if (signature == 6)
//    fmt::print("PC {} addr {:#x} footprint {:#x} density {} count {}\n", signature, packet->address, packet->footprint, footprint, history[signature]->footprint_stats[footprint-1]);

}

void SparsityPredictor::update_eviction(PacketPtr packet) {
    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }
    //SHIP metadata
    if (SHCT.find(signature) == SHCT.end()) {
        SHCT[signature] = 0;
    } else if (!packet->serviced_from_llc) {
        if (SHCT[signature] > 0) {
            SHCT[signature]--;
        }
    }

    if (!_enable) return;

    if (history.find(signature) == history.end()) {
        history[signature] = new PredictorMetadata(signature);
    }

    history[signature]->evictions++; // update access count
    history[signature]->serviced_from_llc += packet->serviced_from_llc;
}

uint64_t SparsityPredictor::get_footprint(PacketPtr packet) {
    if (!_enable) return 0xff;
    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }

    if (history.find(signature) == history.end()) {
        return 0xff; // default to dense
    }

    if (history[signature]->accesses < warmup_accesses) {
        return 0xff; // default to dense during warmup
    }
    
    return history[signature]->footprint;
}

double SparsityPredictor::get_reuse_probability(PacketPtr packet) {
    if (!_enable) return 0;

    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }

    if(history.find(signature) == history.end()) {
        return 0;
    }

//    return history[signature]->get_reuse_probability();

    auto footprint = __builtin_popcountll(packet->footprint);
    if (footprint == 0 || footprint == 8) {
        // No eviction
        return 0.0d;
    }

    if (packet->serviced_from_llc <= 1) {
        return 1.0d;
    } else {
        return (double)(footprint)/8.0d;
    }


    double p_less = 0.0d, p_equal = 0.0d, p_greater = 0.0d;
    for (auto idx = 0; idx < history[signature]->footprint_stats.size(); idx++) {
        auto footprint_count = history[signature]->footprint_stats[idx];
        if (idx < footprint-1) {
            p_less += footprint_count; 
        } else if (idx == footprint-1) {
            p_equal = footprint_count;
        } else {
            p_greater += footprint_count;
        }
    }
    p_less /= history[signature]->accesses;
    p_equal /= history[signature]->accesses;
    p_greater /= history[signature]->accesses;

    if (p_equal == 1.0d) return 0.0d;
    else if (p_greater == 0.0d) return 0.0d;
    else if (p_less == 0.0d) return 1.0d;
    else if (p_equal >= p_less + p_greater) return p_equal + p_greater;
    else if (p_less >= p_equal + p_greater) return p_less + p_equal;//0.0d;
    else return p_equal+p_greater;//1.0d;


}

uint64_t SparsityPredictor::get_reuse_distance(PacketPtr packet) {
    if (!_enable) return UINT64_MAX;

    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }

    if(history.find(signature) == history.end()) {
        return UINT64_MAX;
    }

    return history[signature]->reuse_distance;

}

uint64_t SparsityPredictor::get_shct_value(PacketPtr packet) {
    if (!_enable) return UINT64_MAX;

    uint64_t signature;
    if (mem_signature) {
        signature = align_address(packet->address, mem_region_size);
    } else {
        signature = packet->pc;
    }

    if(history.find(signature) == history.end()) {
        return UINT64_MAX;
    }

    return history[signature]->reuse_distance;

}

void SparsityPredictor::print_stats() {
    for (auto it = history.begin(); it != history.end(); it++) {
        fmt::print("Signature {:#x} Predictor Stats ", it->first);
        it->second->print();
        fmt::print("\n");
        //it->second->print_footprint();
        fmt::print(" SHCT value {}\n", SHCT[it->first]);
//        it->second->print_shct();
    }
                
}
