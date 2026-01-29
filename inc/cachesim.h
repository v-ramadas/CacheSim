#ifndef __CACHESIM_H__
#define __CACHESIM_H__

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <list>
#include "mrc.h"

const uint64_t CACHELINE_SIZE = 64;
#define DEBUG false

class CacheSet {
    uint64_t num_ways;
    std::vector<uint64_t> ways;
    std::vector<uint64_t> lru;
    std::vector<bool> valid;
    std::vector<uint64_t> distance_counts;

    uint64_t block_size = 64;
    uint64_t set_idx = 1;
    uint64_t num_blocks;

    uint64_t global_counter1 = 1;
    uint64_t global_counter2 = 1;

    public:
    uint64_t reuse_dist = 0;
    uint64_t accesses = 0;
    uint64_t hits = 0;

    public:
    CacheSet();
    CacheSet(uint64_t num_ways, uint64_t blk_size, uint64_t set_idx);
    ~CacheSet() {}

    bool try_hit(uint64_t address, bool is_read);
    uint32_t handle_fill(uint64_t address);

    uint64_t get_block_size() const { return block_size; }
    uint64_t get_distance_count(uint64_t way) const { return distance_counts[way]; }
};

class Cache {
    std::string NAME;
    uint64_t num_sets;
    uint64_t num_ways;
    uint64_t total_accesses = 0;
    std::unordered_map<uint64_t, CacheSet> sets;

    MRC mrc;

    //Stats
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;
    std::vector<uint64_t> partial_misses;
    
    public:
    Cache();
    Cache(std::string name, uint64_t num_sets, uint64_t num_ways, uint64_t block_size);
    ~Cache() {}
    uint64_t get_set_idx(uint64_t address);
    bool try_hit(uint64_t address, bool is_read);
    MRC* get_mrc() {return &mrc;};
    void print_mpki_curve(uint64_t inst_count);
    void print_reuse_distance();

    //Stats
    uint64_t get_hits() const { return hits; }
    uint64_t get_misses() const { return misses; }
    uint64_t get_accesses() const { return hits+misses; }
    std::vector<uint64_t> get_partial_misses() const {return partial_misses;}
};

uint64_t align_address(uint64_t address, uint64_t align_size);

#endif
