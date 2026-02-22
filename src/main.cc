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

void access_multi_level(std::vector<Cache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet,
            uint64_t address, bool is_read) {
    access_packet->clear_address();
    eviction_packet->clear_address();
    fill_packet->clear_address();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache[0]->get_block_size(cache[0]->get_set_idx(access_packet->address));
    fill_packet->address = access_packet->address;
    int num_levels = cache.size();
    int hit_at_level = num_levels;
    bool hit = false;
    // Check for hits
    for (int i = 0; i < num_levels; i++) {
        hit = cache[i]->try_hit(access_packet);
        if (hit) {
            hit_at_level = i;
            break;
        }
    }
    
    bool needs_invalidate = false;
    if (hit) {
        fill_packet->size = access_packet->size;
        eviction_packet->size = access_packet->size;
        needs_invalidate = true;
    } else {
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        fill_packet->address = access_packet->address;
        needs_invalidate = false;
    }

    if (hit_at_level != 0) {
        cache[0]->handle_fill_line(fill_packet, eviction_packet, 0);
        for (int i = 1; i < num_levels; i++) {
            if (needs_invalidate)
                cache[i]->handle_invalidate(fill_packet);

            if (eviction_packet->blocks.size() > 0) {
                fill_packet->blocks = eviction_packet->blocks;
                cache[i]->handle_fill_blocks(fill_packet, eviction_packet, 0);
            } else {
                break;
            }
        }
    }
}

void access_single_level(Cache *cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet,
            uint64_t address, bool is_read) {
    access_packet->clear_address();
    eviction_packet->clear_address();
    fill_packet->clear_address();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache->get_block_size(cache->get_set_idx(access_packet->address));
    fill_packet->address = access_packet->address;
    fill_packet->size = CACHELINE_SIZE;
    eviction_packet->size = CACHELINE_SIZE;
    bool hit = cache->try_hit(access_packet);
    if (!hit) {
        //cache->handle_evict(access_packet, eviction_packet, 0);                
        cache->handle_fill_line(fill_packet, eviction_packet, 0);
    }
    return;
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

void print_stats(Cache* cache, uint64_t inst_count, std::string tracename, uint64_t num_sets, uint64_t num_ways, uint64_t block_size) {
    auto mpki = (((float)(cache->get_misses()))/inst_count)*1000;
    auto miss_rate = ((float)(cache->get_misses()))/(cache->get_hits() + cache->get_misses());
    fmt::print(" Trace File {}, Instruction count: {} cache size: {} KB\n", tracename, (float)(inst_count),
            num_sets*num_ways*block_size/1024);
    fmt::print("hits: {} miss: {} accesses: {} read_hits: {} read_misses: {} write_hits: {} write_misses: {}\n",
         cache->get_hits(), cache->get_misses(), cache->get_accesses(),
         cache->get_read_hits(), cache->get_read_misses(),
         cache->get_write_hits(), cache->get_write_misses());
    fmt::print("Miss Rate {:4f}\n", miss_rate);
    fmt::print("MPKI {:10f}\n", mpki);
    if (cache->get_evictions() > 0) {
        fmt::print("Utilization {:4f} \n", 100*(float)(cache->get_num_blocks_used())/(cache->get_evictions()*(block_size/8)));
    } else {
        fmt::print("Utilization undefined (No evictions)\n");
    }
    fmt::print("Partial Misses ");
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
    cache.resize(2);
    cache[0] = new Cache("L1D", 256, 16, block_size, 0, insertion_policy);
    cache[1] = new Cache("LLC", llc_num_sets, llc_num_ways, block_size, 1, insertion_policy);
    cache[0]->set_do_mrc(false);
#else
    Cache* cache = new Cache("L1D", llc_num_sets, llc_num_ways, block_size, 0, insertion_policy);
#endif
    champsim::tracereader trace(get_tracereader(tracename, 0, false, false));
    uint64_t inst_count = 0;
 
    std::map<uint64_t, uint64_t> page_count;
    Packet* access_packet = new Packet();
    Packet* eviction_packet = new Packet();
    Packet* fill_packet = new Packet();

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
        access_packet->pc = inst.ip.to<uint64_t>();
        eviction_packet->pc = inst.ip.to<uint64_t>();
        fill_packet->pc = inst.ip.to<uint64_t>();
#ifdef MULTI_LEVEL
        for (auto& smem:inst.source_memory) {
            access_multi_level(cache, access_packet, eviction_packet, fill_packet, smem.to<uint64_t>(), true);
        }
        for (auto& dmem:inst.destination_memory) {
            access_multi_level(cache, access_packet, eviction_packet, fill_packet, dmem.to<uint64_t>(), false);
        }
#else
        for (auto& smem:inst.source_memory) {
            access_single_level(cache, access_packet, eviction_packet, fill_packet, smem.to<uint64_t>(), true);
        }
        for (auto& dmem:inst.destination_memory) {
            access_single_level(cache, access_packet, eviction_packet, fill_packet, dmem.to<uint64_t>(), false);
        }
#endif
    }

#ifdef MULTI_LEVEL
    print_stats(cache[0], inst_count, tracename, 128, 16, block_size);
    print_stats(cache[1], inst_count, tracename, llc_num_sets, llc_num_ways, block_size);
    cache.clear();
#else
    print_stats(cache, inst_count, tracename, llc_num_sets, llc_num_ways, block_size);
    delete cache;
#endif
    delete access_packet;
    delete eviction_packet;
    delete fill_packet;
    return 0;
}
