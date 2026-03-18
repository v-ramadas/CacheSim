#include "cachesim.h"
#include "predictor.h"
#include "packet.h"

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
uint64_t WARMUP_INSTS = 2500000;

enum TraceFormat {
    CHAMPSIM,
    ADDRESSES,
};

#ifdef MULTI_LEVEL
void access_multi_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet,
            SparsityPredictor* predictor, uint64_t address, uint64_t pc, bool is_read, uint64_t inst_count) {
    access_packet->clear();
    eviction_packet->clear();
    fill_packet->clear();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache[0]->get_block_size(cache[0]->get_set_idx(access_packet->address));
    access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
    fill_packet->address = access_packet->address;

    access_packet->pc = pc;
    fill_packet->pc = pc;
    eviction_packet->pc = pc;

    int num_levels = cache.size();
    int hit_at_level = num_levels;
    bool hit = false;
    bool is_sparse = false;
    // Check for hits
    for (int i = 0; i < num_levels; i++) {

        if (access_packet->is_sparse) {
            access_packet->size = cache[i]->get_block_size(cache[i]->get_set_idx(access_packet->address));
            access_packet->blocks.clear();
            access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
            access_packet->blocks.push_back(access_packet->aligned_address);
        }

        hit = cache[i]->try_hit(access_packet);
        //fmt::print("Level {}: Accessing {:#x} ({}) for {}\n", i, access_packet->address, hit ? "HIT" : "MISS", is_read ? "READ" : "WRITE");

        if (hit) {
            hit_at_level = i;
            break;
        }

        if (inst_count >= WARMUP_INSTS) {
            is_sparse = predictor->predict(access_packet);
        }
        
        access_packet->is_sparse = is_sparse;
        if (!access_packet->is_sparse) {
            access_packet->size = CACHELINE_SIZE;
            access_packet->blocks.clear();
            access_packet->aligned_address = align_address(access_packet->address, CACHELINE_SIZE);
        }

    }

    if (hit && access_packet->is_sparse && (predictor->get_footprint(access_packet) < 2)) {
        if (cachesim::DEBUG)
            fmt::print("Hit at level {}, address {:#x}. Access is to a sparse block, so nothing else to do\n", hit_at_level, access_packet->address);
        return;
    }
    
    bool needs_invalidate = false;
    if (hit) {
        if (access_packet->is_sparse && !cache[0]->get_is_sectored()) {
            fill_packet->size = access_packet->size;
            eviction_packet->size = access_packet->size;
            needs_invalidate = true;
        } else {
            fill_packet->size = CACHELINE_SIZE;
            eviction_packet->size = CACHELINE_SIZE;
            needs_invalidate = true;
        }
    } else {
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        needs_invalidate = false;
    }

    fill_packet->address = access_packet->address;

    if (hit_at_level != 0) {
        if (needs_invalidate) {
            fill_packet->blocks = cache[hit_at_level]->handle_invalidate(fill_packet);
            fill_packet->address = fill_packet->blocks[0];
            cache[0]->handle_fill_blocks(fill_packet, eviction_packet, 0);
        } else {
            cache[0]->handle_fill_line(fill_packet, eviction_packet, 0);
        }
        auto prev_cache_block_size = cache[0]->get_block_size(cache[0]->get_set_idx(fill_packet->address));
        auto curr_cache_block_size = prev_cache_block_size;
        for (int i = 1; i < num_levels; i++) {
            curr_cache_block_size = cache[i]->get_block_size(cache[i]->get_set_idx(fill_packet->address));

            if (prev_cache_block_size != curr_cache_block_size) {
                resize_packet(eviction_packet, curr_cache_block_size);       
            }

            if (eviction_packet->blocks.size() > 0) {
                bool is_sparse = false;
                predictor->insert(eviction_packet);
                predictor->update(eviction_packet);
                if (inst_count >= WARMUP_INSTS) {
                    is_sparse = predictor->predict(access_packet);
                }
                fill_packet->is_sparse = is_sparse;
                fill_packet->address = eviction_packet->address;
                fill_packet->aligned_address = eviction_packet->aligned_address;
                fill_packet->blocks = eviction_packet->blocks;
                fill_packet->footprint = eviction_packet->footprint;
                cache[i]->handle_fill_blocks(fill_packet, eviction_packet, 1);
                //fmt::print("Level {}: Filled line {:#x}\n", i, fill_packet->address);

            } else {
                break;
            }
            prev_cache_block_size = curr_cache_block_size;
        }
    }
}
#else
template <typename T>
void access_single_level(Cache<T> *cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet,
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
    fill_packet->is_sparse = true;
    fill_packet->pc = pc;
    eviction_packet->size = CACHELINE_SIZE;
    bool hit = cache->try_hit(access_packet);
    //fmt::print("Accessing {:#x} ({}) for {}\n", access_packet->address, hit ? "HIT" : "MISS", is_read ? "READ" : "WRITE");
    if (!hit) {
        cache->handle_fill_line(fill_packet, eviction_packet, 0);
    }
    return;
}
#endif
//void histogram(const ooo_model_instr inst, std::map<uint64_t, uint64_t> &count, const uint64_t histogram_granularity, const uint64_t block_size) {
//    for (auto& smem:inst.source_memory) {
//        auto size = histogram_granularity;
//        auto cacheline_size = block_size;
//        auto cacheline = (smem.to<uint64_t>()/cacheline_size)*cacheline_size;
//
//        auto aligned_addr = (cacheline/size)*size;
//        if (count.find(aligned_addr) == count.end()) {
//            count.insert(std::pair<uint64_t, uint64_t>(aligned_addr, 1));
//        } else {
//            count[aligned_addr]++;
//        }
//    }
//}

//void print_histogram(std::map<uint64_t, uint64_t> &count, const uint64_t block_size) {
//    fmt::print("Histogram\n");
//    std::map<uint64_t, uint64_t> freq;
//    uint64_t sum = 0;
//    uint64_t num = 0;
//    for (auto const&it : count) {
//        if (freq.find(it.second) == freq.end()) {
//            freq.insert(std::pair<uint64_t, uint64_t>(it.second, 1));
//        } else {
//            freq[it.second]++;
//        }        
//        num++;
//        sum += it.second;
//    }
//    for (auto const& it : count) {
//        fmt::print("Accesses {:#x}, Count {}\n", it.first, it.second);
//    }
//     fmt::print("Average reuse {:4f}\n", ((float)sum)/num);
//    fmt::print("Footprint: {}\n", count.size()*block_size);
//}

#ifdef MULTI_LEVEL
void useLogFile(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, uint64_t &inst_count) {
#else
template<typename T>
void useLogFile(Cache<T>* cache, const std::string& filename, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, uint64_t &inst_count) {
#endif
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
        char action[16];
        if (cachesim::DEBUG) {
            if (inst_count > 25000000) {
                break;
            }
        }

        if (std::sscanf(line.c_str(), "PC:%lu %15[^:]:0x%lx", &pc, action, &address) == 3) {
            try {
                inst_count++;
                // Extract from the start of "0x" to the end of the line
                bool is_read = (strcmp(action, "read") == 0) ? true : false;
//                if (pc != 2 /*&& pc != 4*/) continue;
#ifdef MULTI_LEVEL
                access_multi_level(cache, access_packet, eviction_packet, fill_packet,
                    predictor, address, pc, is_read, inst_count);
#else
                access_single_level(cache, access_packet, eviction_packet, fill_packet, pc,address, is_read);
#endif

            } catch (const std::exception& e) {
                std::cerr << "Conversion error on line: " << line << " -> " <<e.what() << std::endl;
            }
        }
    }
    file.close();
    return;
}

int main(int argc, char** argv) {

    CLI::App app{"CacheSim"};
    std::string tracename;
    TraceFormat trace_format = CHAMPSIM;
    uint64_t llc_num_sets;
    uint64_t llc_num_ways;
    uint64_t block_size = CACHELINE_SIZE;
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
    }));
    app.add_option("--insertion-policy", insertion_policy, "Cache insertion policy")->transform(CLI::CheckedTransformer(std::map<std::string, InsertionPolicy>{
        {"exclusive", InsertionPolicy::EXCLUSIVE},
    }));
    app.add_flag("--debug", cachesim::DEBUG, "Enable debug mode");
    CLI11_PARSE(app, argc, argv);


#ifdef MULTI_LEVEL
    std::vector<BaseCache*> cache;
    cache.resize(2);
    if (block_size == CACHELINE_SIZE)
        cache[0] = new Cache<CacheSet>("L1D", 128, 16, block_size, 0, false, replacement_policy, insertion_policy);
    else
        cache[0] = new Cache<SectoredCacheSet>("L1D", 128, 16, block_size, 0, true, replacement_policy, insertion_policy);
    cache[1] = new Cache<CacheSet>("LLC", llc_num_sets, llc_num_ways, block_size, 1, false, replacement_policy, insertion_policy);
    cache[0]->set_do_mrc(false);
    cache[1]->set_do_mrc(false);
    SparsityPredictor* predictor = new SparsityPredictor(4, 1024, 8, 81920);
    if (block_size < CACHELINE_SIZE) predictor->enable();
    else predictor->disable();

#else
    Cache<CacheSet>* cache = new Cache<CacheSet>("L1D", llc_num_sets, llc_num_ways, block_size, 0, false, replacement_policy, insertion_policy);
    cache->set_do_mrc(false);
#endif
    uint64_t inst_count = 0;

    std::map<uint64_t, uint64_t> page_count;
    Packet* access_packet = new Packet();
    Packet* eviction_packet = new Packet();
    Packet* fill_packet = new Packet();
    if (trace_format == TraceFormat::ADDRESSES) {
#ifdef MULTI_LEVEL
        useLogFile(cache, tracename, predictor, access_packet, eviction_packet, fill_packet, inst_count);
#else
        useLogFile(cache, tracename, access_packet, eviction_packet, fill_packet, inst_count);
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
#ifdef MULTI_LEVEL
            for (auto& smem:inst.source_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet,
                    predictor,smem.to<uint64_t>(), inst.ip.to<uint64_t>(), true, inst_count);
            }
            for (auto& dmem:inst.destination_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet,
                    predictor, dmem.to<uint64_t>(), inst.ip.to<uint64_t>(), false, inst_count);
            }
#else
            for (auto& smem:inst.source_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet,
                        inst.ip.to<uint64_t>(), smem.to<uint64_t>(), true);
            }
            for (auto& dmem:inst.destination_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet, 
                        inst.ip.to<uint64_t>(), dmem.to<uint64_t>(), false);
            }
#endif
        }
    }

#ifdef MULTI_LEVEL
    for (auto cache_inst: cache)
        cache_inst->print_stats(inst_count, tracename);
//    print_stats(cache[0], inst_count, tracename, 128, 16, CACHELINE_SIZE);
//    print_stats(cache[1], inst_count, tracename, llc_num_sets, llc_num_ways, block_size);
    cache.clear();
    delete predictor;
#else
    cache->print_stats(inst_count, tracename);
//    print_stats(cache, inst_count, tracename, llc_num_sets, llc_num_ways, block_size);
    delete cache;
#endif
    delete access_packet;
    delete eviction_packet;
    delete fill_packet;
    return 0;
}
