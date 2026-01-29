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

void try_read(const ooo_model_instr inst, Cache *cache, const int64_t block_size) {
    for (auto& smem:inst.source_memory) {
        cache->try_hit(smem.to<uint64_t>(), true);
        if (false)
            cache->get_mrc()->mattson_stack_distance_algorithm(smem.to<uint64_t>(), block_size);
    }
}

void try_write(const ooo_model_instr inst, Cache *cache, const int64_t block_size) {
    for (auto& dmem:inst.destination_memory) {
        cache->try_hit(dmem.to<uint64_t>(), false);
        if (false)
            cache->get_mrc()->mattson_stack_distance_algorithm(dmem.to<uint64_t>(), block_size);
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

int main(int argc, char** argv) {

    CLI::App app{"CacheSim"};
    std::string tracename;
    uint64_t num_sets;
    uint64_t num_ways;
    uint64_t block_size = CACHELINE_SIZE;
    bool do_mrc = false;
    app.add_option("--trace", tracename, "Path to input trace file")->required()->expected(1)->check(CLI::ExistingFile);
    app.add_option("--num-cache-sets", num_sets, "Number of sets in cache")->required();
    app.add_option("--num-cache-ways", num_ways, "Number of ways in cache")->required();
    app.add_option("--cache-block-size", block_size, "Cache block size");
    app.add_flag("--mrc", do_mrc, "Perform MRC Analysis");

    CLI11_PARSE(app, argc, argv);

    Cache* cache = new Cache("L1D", num_sets, num_ways, block_size);
    champsim::tracereader trace(get_tracereader(tracename, 0, false, false));
    uint64_t inst_count = 0;
 
    std::map<uint64_t, uint64_t> page_count;
    while (!trace.eof()) {
        inst_count++;
        auto inst = trace();

        //histogram(inst, page_count, 64, block_size);

        try_read(inst, cache, block_size);
        try_write(inst, cache, block_size);
    }

    auto mpki = (((float)(cache->get_misses()))/inst_count)*1000;
    auto miss_rate = ((float)(cache->get_misses()))/(cache->get_hits() + cache->get_misses());
    fmt::print(" Trace File {}, Instruction count: {} cache size: {} KB hits: {} miss: {} accesses: {}\n", tracename, (float)(inst_count),
            num_sets*num_ways*block_size/1024, cache->get_hits(), cache->get_misses(), cache->get_accesses());
    fmt::print("miss rate {:4f}\n", miss_rate);
    fmt::print("MPKI {:10f}\n", mpki);
    fmt::print("Partial misses ");
    auto partial_misses = cache->get_partial_misses();
    for (long unsigned idx = 0; idx < partial_misses.size(); idx++) {
        fmt::print("{}:{} ", idx+1, partial_misses[idx]);
    }
    fmt::print("\n");

    cache->print_mpki_curve(inst_count);

    //cache->print_reuse_distance();
    //print_histogram(page_count, block_size);
    delete cache;
    return 0;
}
