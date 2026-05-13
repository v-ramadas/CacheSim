#include "cachesim.h"
#include "predictor.h"
#include "utils.h"
#include "victim_buffer.h"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <CLI/CLI.hpp>
#include <list>

#include "tracereader.h"

bool cachesim::DEBUG = false;
//TODO: Figure out a good value
uint64_t WARMUP_INSTS = 0;//2*16*2048;
bool cachesim::dropBlocks = false;
bool cachesim::useVictimBuffer = false;
bool cachesim::useMemSignature = false;
enum TraceFormat {
    CHAMPSIM,
    ADDRESSES,
};

#ifdef MULTI_LEVEL
void access_multi_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            SparsityPredictor* predictor, uint64_t address, uint64_t pc, bool is_read, uint64_t inst_count, uint64_t next_reuse) {
    access_packet->clear();
    eviction_packet->clear();
    fill_packet->clear();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache[0]->get_block_size(cache[0]->get_set_idx(access_packet->address));
    access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
    access_packet->pc = pc;
    access_packet->next_reuse = next_reuse;

//    fill_packet->address = access_packet->address;
//    fill_packet->pc = pc;
    *fill_packet = *access_packet;
    fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    *eviction_packet = *fill_packet;
    *invalidation_packet = *fill_packet;

//    eviction_packet->pc = pc;

//    invalidation_packet->address = fill_packet->address;
//    invalidation_packet->aligned_address = fill_packet->aligned_address;

    int num_levels = cache.size();
    int hit_at_level = num_levels;
    bool hit = false;

    // Check for hits
    for (int i = 0; i < num_levels; i++) {

//        if (access_packet->is_low_reuse) {
//            access_packet->size = cache[i]->get_block_size(cache[i]->get_set_idx(access_packet->address));
//            access_packet->blocks.clear();
//            access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
//            access_packet->blocks.push_back(access_packet->aligned_address);
//        }

        hit = cache[i]->try_hit(access_packet);

        if (hit) {
            hit_at_level = i;
            break;
        }

        if (!access_packet->is_low_reuse) {
            access_packet->blocks.clear();
            access_packet->aligned_address = align_address(access_packet->address, CACHELINE_SIZE);
        }

    }

    bool needs_invalidate = false;
    if (hit) {
        fill_packet->serviced_from_llc = 1;
        eviction_packet->footprint = 0;
        //if (access_packet->is_low_reuse && !cache[0]->get_is_sectored()) {
        //    fill_packet->size = access_packet->size;
        //    eviction_packet->size = access_packet->size;
        //    invalidation_packet->size = access_packet->size;
        //    needs_invalidate = true;
        //} else {
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        invalidation_packet->size = CACHELINE_SIZE;
        needs_invalidate = true;
        //}
    } else {
        fill_packet->serviced_from_llc = 0;
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        invalidation_packet->size = CACHELINE_SIZE;
        needs_invalidate = false;
    }

    //fill_packet->address = access_packet->address;
    //fill_packet->aligned_address = access_packet->aligned_address;
    //fill_packet->next_reuse = next_reuse;
    if (hit_at_level != 0) {
        if (hit) {
            fill_packet->blocks = cache[hit_at_level]->handle_invalidate(fill_packet);
            cache[0]->handle_invalidate(invalidation_packet);
            fill_packet->footprint |= invalidation_packet->footprint;
            fill_packet->serviced_from_llc += 1;
            cache[0]->handle_fill_blocks(fill_packet, eviction_packet, 0);
        } else {
            cache[1]->handle_invalidate(invalidation_packet);
            cache[0]->handle_invalidate(invalidation_packet);
            fill_packet->footprint = invalidation_packet->footprint;
            cache[0]->handle_fill_line(fill_packet, eviction_packet, 0);
        }

        auto prev_cache_block_size = cache[0]->get_block_size(cache[0]->get_set_idx(fill_packet->address));
        auto curr_cache_block_size = prev_cache_block_size;
        for (int i = 1; i < num_levels; i++) {
            curr_cache_block_size = cache[i]->get_block_size(cache[i]->get_set_idx(fill_packet->address));

            if (prev_cache_block_size != curr_cache_block_size) {
                resize_packet(eviction_packet, curr_cache_block_size);       
            }


            // Train on data movement from LLC -> L1D
            //predictor->update_footprint(eviction_packet);
            predictor->update_access(fill_packet);
            if (eviction_packet->blocks.size() > 0) {
                bool is_low_reuse = false;
                bool is_hub_node = true;
                if (inst_count >= WARMUP_INSTS) {
                    // Predict for data movement from L1D -> LLC
                    is_low_reuse = predictor->predict(eviction_packet);
                    is_hub_node = predictor->is_hub_node(eviction_packet);
                }

                *fill_packet = *eviction_packet;
                fill_packet->reuse_probability = predictor->get_reuse_probability(eviction_packet);

                fill_packet->shct_value = predictor->get_shct_value(fill_packet);

                if (predictor->get_reuse_distance(eviction_packet) <= 1) {
                    fill_packet->reuse_distance = UINT64_MAX;
                } else {
                    fill_packet->reuse_distance = inst_count + predictor->get_reuse_distance(eviction_packet);
                }
                fill_packet->next_reuse = eviction_packet->next_reuse;
                eviction_packet->clear();
                cache[i]->handle_fill_blocks(fill_packet, eviction_packet, 1);
                if (eviction_packet->blocks.size() > 0) {
                    predictor->update_eviction(eviction_packet);
                }
            } else {
                break;
            }
            eviction_packet->clear();
            eviction_packet->clear_pc();
            fill_packet->clear();
            fill_packet->clear_pc();
            prev_cache_block_size = curr_cache_block_size;
        }
    }
}
#else
template <typename T>
void access_single_level(Cache<T> *cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            uint64_t pc, uint64_t address, bool is_read) {
    access_packet->clear_address();
    eviction_packet->clear_address();
    fill_packet->clear_address();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache->get_block_size(cache->get_set_idx(access_packet->address));
    access_packet->pc = pc;
    fill_packet->address = access_packet->address;
    fill_packet->size = CACHELINE_SIZE;
    fill_packet->is_low_reuse = true;
    fill_packet->pc = pc;
    eviction_packet->size = CACHELINE_SIZE;
    bool hit = cache->try_hit(access_packet);
    if (!hit) {
        cache->handle_fill_line(fill_packet, eviction_packet, 0);
    }
    return;
}
#endif

#ifdef MULTI_LEVEL
void useLogFile(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t &inst_count, uint64_t num_iters) {
#else
template<typename T>
void useLogFile(Cache<T>* cache, const std::string& filename, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t &inst_count, uint64_t num_iters) {
#endif
    while (num_iters > 0) {
        std::ifstream file(filename);

        if (!file.is_open()) {
            std::cerr << "Error: Could not open the file!" << std::endl;
            return;
        }

        std::string line;
        std::string delimiter = "0x";
        while (std::getline(file, line)) {
            uint64_t pc;
            uint64_t address;
            uint64_t next_reuse = UINT64_MAX;
            int parsed_count = 0;
            char action[16];
            if (cachesim::DEBUG) {
                if (inst_count > 2500000) {
                    break;
                }
            }

            parsed_count = std::sscanf(line.c_str(), "PC:%lu %15[^:]:0x%lx %lu", &pc, action, &address, &next_reuse);
            if (parsed_count >= 3) {
                try {
                    inst_count++;
                    if (next_reuse != UINT64_MAX) next_reuse += inst_count;
                    //std::cout << "address 0x" << std::hex << address << " reuse " << std::dec << next_reuse << std::endl;
                    // Extract from the start of "0x" to the end of the line
                    bool is_read = (strcmp(action, "read") == 0) ? true : false;
#ifdef MULTI_LEVEL
                    access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        predictor, address, pc, is_read, inst_count, next_reuse);
#else
                    access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet, pc,address, is_read);
#endif

                } catch (const std::exception& e) {
                    std::cerr << "Conversion error on line: " << line << " -> " <<e.what() << std::endl;
                }
            }
        }
        file.close();
        --num_iters;
    }
    return;
}

int main(int argc, char** argv) {

    CLI::App app{"CacheSim"};
    std::string tracename;
    TraceFormat trace_format = CHAMPSIM;
    uint64_t llc_num_sets;
    uint64_t llc_num_ways;
    uint64_t block_size = CACHELINE_SIZE;
    uint64_t num_iters = 1;
    ReplacementPolicy replacement_policy = ReplacementPolicy::LRU;
    InsertionPolicy insertion_policy = InsertionPolicy::EXCLUSIVE;
    app.add_option("--trace", tracename, "Path to input trace file")->required()->expected(1)->check(CLI::ExistingFile);
    app.add_option("--trace-format", trace_format, "Trace format")->transform(CLI::CheckedTransformer(std::map<std::string, TraceFormat>{
        {"champsim", TraceFormat::CHAMPSIM},
        {"addresses", TraceFormat::ADDRESSES},
    }));
    app.add_option("--num-cache-sets", llc_num_sets, "Number of sets in cache")->required();
    app.add_option("--num-cache-ways", llc_num_ways, "Number of ways in cache")->required();
    app.add_option("--cache-block-size", block_size, "Cache block size");

    app.add_option("--replacement-policy", replacement_policy, "Cache replacement policy")->transform(CLI::CheckedTransformer(std::map<std::string, ReplacementPolicy>{
        {"lru", ReplacementPolicy::LRU},
        {"lfu", ReplacementPolicy::LFU},
        {"srrip", ReplacementPolicy::SRRIP},
        {"drrip", ReplacementPolicy::DRRIP},
        {"trrip", ReplacementPolicy::TRRIP},
        {"prrip", ReplacementPolicy::PRRIP},
        {"ship", ReplacementPolicy::SHIP},
        {"belady", ReplacementPolicy::Belady},
        {"fission", ReplacementPolicy::Fission}
    }));
    app.add_option("--insertion-policy", insertion_policy, "Cache insertion policy")->transform(CLI::CheckedTransformer(std::map<std::string, InsertionPolicy>{
        {"exclusive", InsertionPolicy::EXCLUSIVE},
    }));
    app.add_flag("--debug", cachesim::DEBUG, "Enable debug mode");
    app.add_option("--iters", num_iters, "Number of iterations");
    app.add_option("--warmup-instructions", WARMUP_INSTS, "Warmup instruction count");

    CLI11_PARSE(app, argc, argv);

    switch(replacement_policy) {
        case ReplacementPolicy::Fission:
            cachesim::dropBlocks = false;
            cachesim::useVictimBuffer = true;
            break;
        case ReplacementPolicy::SHIP:
            cachesim::useMemSignature = true;
            break;
        default:
            cachesim::dropBlocks = false;
            break;
    }
#ifdef MULTI_LEVEL
    std::vector<BaseCache*> cache;
    cache.resize(2);
    if (block_size == CACHELINE_SIZE)
        cache[0] = new Cache<CacheSet>("L1D", 128, 16, block_size, 0, false, ReplacementPolicy::LRU, insertion_policy);
    else
        cache[0] = new Cache<SectoredCacheSet>("L1D", 128, 16, block_size, 0, true, ReplacementPolicy::LRU, insertion_policy);
    cache[1] = new Cache<CacheSet>("LLC", llc_num_sets, llc_num_ways, block_size, 1, false, replacement_policy, insertion_policy);
    cache[0]->set_do_mrc(false);
    cache[1]->set_do_mrc(false);
    SparsityPredictor* predictor = new SparsityPredictor(0.4, 1024, WARMUP_INSTS);
    //if (block_size < CACHELINE_SIZE) predictor->enable();
    //else predictor->disable();
    predictor->enable();
    if (cachesim::useMemSignature)
        predictor->set_mem_signature();
    else
        predictor->set_pc_signature();
#else
    Cache<CacheSet>* cache = new Cache<CacheSet>("L1D", llc_num_sets, llc_num_ways, block_size, 0, false, replacement_policy, insertion_policy);
    cache->set_do_mrc(false);
#endif
    uint64_t inst_count = 0;

    std::map<uint64_t, uint64_t> page_count;
    Packet* access_packet = new Packet();
    Packet* eviction_packet = new Packet();
    Packet* fill_packet = new Packet();
    Packet* invalidation_packet = new Packet();
    if (trace_format == TraceFormat::ADDRESSES) {
#ifdef MULTI_LEVEL
        useLogFile(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, invalidation_packet, inst_count, num_iters);
#else
        useLogFile(cache, tracename, access_packet, eviction_packet, fill_packet, invalidation_packet, inst_count, num_iters);
#endif
    } else {
        champsim::tracereader trace(get_tracereader(tracename, 0, false, false));
        while (!trace.eof()) {
            if (cachesim::DEBUG) {
                if (inst_count > 2500000) {
                    break;
                }
            }
            inst_count++;
            auto inst = trace();
            access_packet->clear();
            eviction_packet->clear();
            fill_packet->clear();
            invalidation_packet->clear();
#ifdef MULTI_LEVEL
            for (auto& smem:inst.source_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                    predictor,smem.to<uint64_t>(), inst.ip.to<uint64_t>(), true, inst_count,
                    0);
            }
            for (auto& dmem:inst.destination_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                    predictor, dmem.to<uint64_t>(), inst.ip.to<uint64_t>(), false, inst_count,
                    0);
            }
#else
            for (auto& smem:inst.source_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        inst.ip.to<uint64_t>(), smem.to<uint64_t>(), true);
            }
            for (auto& dmem:inst.destination_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        inst.ip.to<uint64_t>(), dmem.to<uint64_t>(), false);
            }
#endif
        }
    }

#ifdef MULTI_LEVEL
    for (auto cache_inst: cache)
        cache_inst->print_stats(inst_count, tracename);
//    if (!cachesim::useMemSignature)
        predictor->print_stats();
    cache.clear();
    delete predictor;
#else
    cache->print_stats(inst_count, tracename);
    delete cache;
#endif
    delete access_packet;
    delete eviction_packet;
    delete fill_packet;
    delete invalidation_packet;
    return 0;
}
