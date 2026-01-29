#include "cachesim.h"

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

void access(std::vector<Cache*> cache, uint64_t access_address, bool is_read) {
    std::vector<uint64_t> eviction_buffer = {};
    int num_levels = cache.size();
    int hit_at_level = num_levels;
    bool hit = false;
    // Check for hits
    for (int i = 0; i < num_levels; i++) {
        hit = cache[i]->try_hit(access_address, is_read);
        if (hit) {
            hit_at_level = i;
            break;
        }
    }
    
    if (hit) {
        // If Hit, handle here
        std::vector<uint64_t> downgrade_address = {};
        auto set_idx = cache[0]->get_set_idx(access_address);
        if (hit_at_level != 0) {
            downgrade_address = cache[0]->handle_evict(access_address, cache[0]->get_block_size(set_idx), 0);
            std::vector<uint64_t> blocks = {align_address(access_address, cache[0]->get_block_size(set_idx))};
            cache[0]->handle_fill(blocks, blocks.size(), 0);
        }
        for (int i = 1; i < num_levels; i++) {
            cache[i]->handle_invalidate(access_address);
            if (downgrade_address.size() > 0) {
                auto evicted_address = cache[i]->handle_evict(access_address, cache[i]->get_block_size(set_idx), 0);
                cache[i]->handle_fill(downgrade_address, downgrade_address.size(), 0);
                downgrade_address = evicted_address;
            } else {
                break;
            }
        }
    } else {
        auto downgrade_address = cache[0]->handle_evict(access_address, CACHELINE_SIZE, 0);
        cache[0]->handle_fill(access_address, 0);
        for (int i = 1; i < num_levels; i++) {
            if (downgrade_address.size() > 0) {
                auto evicted_address = cache[i]->handle_evict(access_address, CACHELINE_SIZE, 0);
                cache[i]->handle_fill(downgrade_address, downgrade_address.size(), 0);
                downgrade_address = evicted_address;
            } else {
                break;
            }
        }
    }
}


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

void print_stats(Cache* cache, uint64_t inst_count, std::string tracename, uint64_t llc_num_sets, uint64_t llc_num_ways, uint64_t block_size) {
    auto mpki = (((float)(cache->get_misses()))/inst_count)*1000;
    auto miss_rate = ((float)(cache->get_misses()))/(cache->get_hits() + cache->get_misses());
    fmt::print(" Trace File {}, Instruction count: {} cache size: {} KB\n", tracename, (float)(inst_count),
            llc_num_sets*llc_num_ways*block_size/1024);
    fmt::print("hits: {} miss: {} accesses: {} read_hits: {} read_misses: {} write_hits: {} write_misses: {}\n",
         cache->get_hits(), cache->get_misses(), cache->get_accesses(),
         cache->get_read_hits(), cache->get_read_misses(),
         cache->get_write_hits(), cache->get_write_misses());
    fmt::print("miss rate {:4f}\n", miss_rate);
    fmt::print("MPKI {:10f}\n", mpki);
    fmt::print("Partial misses ");
    auto partial_misses = cache->get_partial_misses();
    for (long unsigned idx = 0; idx < partial_misses.size(); idx++) {
        fmt::print("{}:{} ", idx+1, partial_misses[idx]);
    }
    fmt::print("\n");

    if (cache->get_do_mrc()) {
        cache->print_mpki_curve(inst_count);
    }

    //cache->print_reuse_distance();
    //print_histogram(page_count, block_size);

}

int main(int argc, char** argv) {

    CLI::App app{"CacheSim"};
    std::string tracename;
    uint64_t llc_num_sets;
    uint64_t llc_num_ways;
    uint64_t block_size = CACHELINE_SIZE;
    InsertionPolicy insertion_policy = EXCLUSIVE;
    app.add_option("--trace", tracename, "Path to input trace file")->required()->expected(1)->check(CLI::ExistingFile);
    app.add_option("--num-cache-sets", llc_num_sets, "Number of sets in cache")->required();
    app.add_option("--num-cache-ways", llc_num_ways, "Number of ways in cache")->required();
    app.add_option("--cache-block-size", block_size, "Cache block size");
    app.add_option("--insertion-policy", insertion_policy, "Cache insertion policy")->transform(CLI::CheckedTransformer(std::map<std::string, InsertionPolicy>{
        {"exclusive", InsertionPolicy::EXCLUSIVE},
    }));
    app.add_flag("--debug", cachesim::DEBUG, "Enable debug mode");
    CLI11_PARSE(app, argc, argv);

#ifdef MULTI_LEVEL
    std::vector<Cache*> cache;
    cache.push_back(new Cache("L1D", 128, 16, block_size, 0, insertion_policy));
    cache.push_back(new Cache("LLC", llc_num_sets, llc_num_ways, block_size, 1, insertion_policy));
    cache[0]->set_do_mrc(false);
#else
    Cache* cache = new Cache("L1D", llc_num_sets, llc_num_ways, block_size, 0);
#endif
    champsim::tracereader trace(get_tracereader(tracename, 0, false, false));
    uint64_t inst_count = 0;
 
    std::map<uint64_t, uint64_t> page_count;
    while (!trace.eof()) {
        if (cachesim::DEBUG) {
            if (inst_count > 5000000) {
                break;
            }
        }
        inst_count++;
        auto inst = trace();
#ifdef MULTI_LEVEL
        for (auto& smem:inst.source_memory) {
            access(cache, smem.to<uint64_t>(), true);
        }
        
        for (auto& dmem:inst.destination_memory) {
            access(cache, dmem.to<uint64_t>(), false);
        }
#else
        for (auto& smem:inst.source_memory) {
            bool hit = cache->try_hit(smem.to<uint64_t>(), true);
            if (!hit) {
                cache->handle_evict(smem.to<uint64_t>(), CACHELINE_SIZE, 0);                
                cache->handle_fill(smem.to<uint64_t>(), 0);
            }
        }
        for (auto& dmem:inst.destination_memory) {
            bool hit = cache->try_hit(dmem.to<uint64_t>(), false);
            if (!hit) {
                cache->handle_evict(dmem.to<uint64_t>(), CACHELINE_SIZE, 0);
                cache->handle_fill(dmem.to<uint64_t>(), 0);
            }
        }
#endif
    }

#ifdef MULTI_LEVEL
    print_stats(cache[0], inst_count, tracename, 256, 16, block_size);
    print_stats(cache[1], inst_count, tracename, llc_num_sets, llc_num_ways, block_size);
    for (auto level : cache) {
        delete level;
    } 
    cache.clear();
#else
    print_stats(cache, inst_count, tracename, llc_num_sets, llc_num_ways, block_size);
    delete cache;
#endif
    return 0;
}
