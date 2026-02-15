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
#include "packet.h"

const uint64_t CACHELINE_SIZE = 64;
namespace cachesim {
    extern bool DEBUG;
};

enum InsertionPolicy {
    EXCLUSIVE,
};

class CacheSet {
    uint64_t num_ways;
    std::vector<uint64_t> ways;
    std::vector<uint64_t> lru;
    std::vector<bool> valid;
    std::vector<uint64_t> distance_counts;
    std::vector<bool> dirty;
    std::vector<bool> footprint;

    bool do_mrc = true;

    uint64_t block_size = 64;
    uint64_t set_idx = 1;
    uint64_t num_blocks;
    uint64_t level = 0;

    uint64_t global_counter1 = 1;
    uint64_t global_counter2 = 1;

    public:
    uint64_t reuse_dist = 0;
    uint64_t accesses = 0;
    uint64_t hits = 0;

    public:
    CacheSet();
    CacheSet(uint64_t num_ways, uint64_t blk_size, uint64_t set_idx, uint64_t level);
    ~CacheSet() {}

    bool try_hit(PacketPtr packet);
    uint32_t handle_fill(PacketPtr packet);
    void handle_evict(PacketPtr eviction_packet);
    void handle_invalidate(PacketPtr packet);

    uint64_t get_block_size() const { return block_size; }
    uint64_t get_distance_count(uint64_t way) const { return distance_counts[way]; }
    bool get_do_mrc() {return do_mrc;}
    void set_do_mrc(bool mrc) {do_mrc = mrc;}

    bool get_footprint(uint64_t way_idx, uint64_t word_idx);
    void set_footprint(uint64_t way_idx, uint64_t word_idx);
};

class Cache {
    std::string NAME;
    uint64_t num_sets;
    uint64_t num_ways;
    uint64_t total_accesses = 0;
    uint64_t level = 0;
    std::unordered_map<uint64_t, CacheSet> sets;

    MRC mrc;
    const InsertionPolicy insertion_policy = EXCLUSIVE;
    //Stats
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t read_hits = 0;
    uint64_t read_misses = 0;
    uint64_t write_hits = 0;
    uint64_t write_misses = 0;
    uint64_t evictions = 0;
    uint64_t num_blocks_used = 0;
    std::vector<uint64_t> partial_misses;
    
    public:
    Cache();
    Cache(std::string name, uint64_t num_sets, uint64_t num_ways, uint64_t block_size, uint64_t level, InsertionPolicy policy=EXCLUSIVE);
    ~Cache() {}
    uint64_t get_set_idx(uint64_t address);
    bool try_hit(PacketPtr packet);
    void handle_fill(PacketPtr packet, uint64_t num_blocks_to_fill, int level);
    void handle_fill(PacketPtr packet, int level);
    void handle_evict(PacketPtr access_packet, PacketPtr eviction_packet, int level);
    void handle_invalidate(PacketPtr packet);

    bool can_insert_at_level(int level);

    MRC* get_mrc() {return &mrc;};
    void print_mpki_curve(uint64_t inst_count);
    void print_reuse_distance();

    InsertionPolicy get_insertion_policy() const { return insertion_policy; }
    uint64_t get_block_size(uint64_t set_idx) { return sets[set_idx].get_block_size(); }
    bool get_do_mrc() {return sets[0].get_do_mrc();}

    //Stats
    uint64_t get_hits() const { return hits; }
    uint64_t get_misses() const { return misses; }
    uint64_t get_accesses() const { return hits+misses; }
    uint64_t get_read_hits() const { return read_hits; }
    uint64_t get_read_misses() const { return read_misses; }
    uint64_t get_write_hits() const { return write_hits; }
    uint64_t get_write_misses() const { return write_misses; }
    uint64_t get_evictions() const {return evictions; }
    uint64_t get_num_blocks_used() const {return num_blocks_used; }
    std::vector<uint64_t> get_partial_misses() const {return partial_misses;}

    void set_do_mrc(bool mrc) {
        for (auto& set: sets) {
            set.second.set_do_mrc(mrc);
        }
    }
};

uint64_t align_address(uint64_t address, uint64_t align_size);

#endif
