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
#include <cassert>

const uint64_t CACHELINE_SIZE = 64;
namespace cachesim {
    extern bool DEBUG;
};

enum InsertionPolicy {
    EXCLUSIVE,
};

struct Sector {
    std::vector<uint64_t> sectors;
    std::vector<bool> valid;
    std::vector<bool> dirty;
    uint64_t num_blocks;

    Sector(uint64_t num_blocks):
        num_blocks(num_blocks)
    {
        sectors.resize(num_blocks, UINT64_MAX);
        valid.resize(num_blocks, false);
        dirty.resize(num_blocks, false);
    }

    Sector(const Sector& other) noexcept : 
        sectors(other.sectors),
        valid(other.valid),
        dirty(other.dirty),
        num_blocks(other.num_blocks)
    {
    }

    Sector& operator=(const Sector& other) noexcept{
        if (this != &other) {
           sectors = other.sectors;
           valid = other.valid;
           dirty = other.dirty;
        }
        num_blocks = other.num_blocks;
        return *this;
    }
    ~Sector() = default;

    void invalidate() {
        std::fill(sectors.begin(), sectors.end(), UINT64_MAX);
        std::fill(valid.begin(), valid.end(), false);
        std::fill(dirty.begin(), dirty.end(), false);
    }
};



class CacheSet {

    protected:
    uint64_t num_ways;
    std::vector<uint64_t> ways;
    std::vector<uint64_t> lru;
    std::vector<bool> valid;
    std::vector<uint64_t> distance_counts;
    std::vector<bool> dirty;
    std::vector<bool> footprint;
    std::vector<uint64_t> pc;
    bool do_mrc = true;

    uint64_t block_size = 64;
    uint64_t set_idx = 1;
    uint64_t num_blocks;
    uint64_t num_lines;
    uint64_t level = 0;

    uint64_t lru_counter = 0;
    uint64_t mru_counter = 0;

    public:
    uint64_t reuse_dist = 0;
    uint64_t accesses = 0;
    uint64_t hits = 0;

    public:
    CacheSet() {}
    CacheSet(uint64_t _num_ways, uint64_t blk_size, uint64_t _set_idx, uint64_t _level) {
        num_ways = _num_ways;
        block_size = blk_size;
        set_idx = _set_idx;
        level = _level;
        num_blocks = CACHELINE_SIZE/block_size;
        num_lines = num_ways/num_blocks;
        ways.resize(num_ways, UINT64_MAX);
        lru.resize(num_ways, 0);
        valid.resize(num_ways, false);
        dirty.resize(num_ways, false);
        pc.resize(num_ways, UINT64_MAX);
        distance_counts.resize(num_ways, 0);
        footprint.resize(num_ways*block_size/8, false);
    }

    ~CacheSet() {
    }

    bool try_hit(PacketPtr packet);
    void handle_fill(PacketPtr packet);
    void handle_evict(PacketPtr eviction_packet);
    uint64_t handle_invalidate(PacketPtr packet);

    uint64_t get_block_size() const { return block_size; }
    uint64_t get_distance_count(uint64_t way) const { return distance_counts[way]; }
    bool get_do_mrc() {return do_mrc;}
    void set_do_mrc(bool mrc) {do_mrc = mrc;}

    bool get_footprint(uint64_t way_idx, uint64_t word_idx);
    void set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed);

    const std::vector<uint64_t>& get_ways() const {return ways;}
    bool get_valid(uint64_t idx) const {return valid[idx];}
};

class SectoredCacheSet: public CacheSet {
    protected:
    std::vector<Sector> way_sectors;

    public:
    SectoredCacheSet() {}
    SectoredCacheSet(uint64_t _num_ways, uint64_t blk_size, uint64_t _set_idx, uint64_t _level) {
        num_ways = _num_ways;
        block_size = blk_size;
        set_idx = _set_idx;
        level = _level;
        num_blocks = CACHELINE_SIZE/block_size;
        num_lines = num_ways/num_blocks;
        ways.resize(num_ways, UINT64_MAX);
        way_sectors.assign(num_ways, Sector(num_blocks));
        lru.resize(num_ways, 0);
        valid.resize(num_ways, false);
        dirty.resize(num_ways, false);
        pc.resize(num_ways, UINT64_MAX);
        distance_counts.resize(num_ways, 0);
        footprint.resize(num_ways*block_size, false);
    }
    ~SectoredCacheSet() {}
    bool try_hit(PacketPtr packet);
    void handle_fill(PacketPtr packet);
    void handle_evict(PacketPtr eviction_packet);
    Sector handle_invalidate(PacketPtr packet);
    uint64_t get_block_size() const { return block_size; }
    bool get_footprint(uint64_t way_idx, uint64_t word_idx);
    void set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed);

    const std::vector<uint64_t>& get_ways() const {return ways;}
    bool get_valid(uint64_t idx) const {return valid[idx];}

};

class BaseCache {
    public:
    virtual ~BaseCache() = default;
    virtual uint64_t get_set_idx(uint64_t address) = 0;
    virtual bool try_hit(PacketPtr packet) = 0;
    virtual void handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0) = 0;
    virtual void handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0) = 0;
    virtual void handle_evict(PacketPtr access_packet, PacketPtr eviction_packet, int level = 0) = 0;
    virtual std::vector<uint64_t> handle_invalidate(PacketPtr packet) = 0;
    virtual void populate_fill_packet(PacketPtr packet) = 0;

    virtual bool can_insert_at_level(int level) = 0;

    virtual MRC* get_mrc() = 0;
    virtual void print_mpki_curve(uint64_t inst_count) = 0;
    virtual void print_stats(uint64_t inst_count, std::string tracename) = 0;
    virtual void print_reuse_distance() = 0;

    virtual InsertionPolicy get_insertion_policy() const = 0;
    virtual uint64_t get_block_size(uint64_t set_idx) = 0;
    virtual bool get_do_mrc() = 0;
    virtual bool get_is_sectored() = 0;

    virtual uint64_t get_hits() const = 0; 
    virtual uint64_t get_misses() const = 0; 
    virtual uint64_t get_accesses() const = 0; 
    virtual uint64_t get_read_hits() const = 0; 
    virtual uint64_t get_read_misses() const = 0; 
    virtual uint64_t get_write_hits() const = 0; 
    virtual uint64_t get_write_misses() const = 0; 
    virtual uint64_t get_evictions() const = 0; 
    virtual uint64_t get_num_blocks_used() const = 0; 
    virtual std::vector<uint64_t> get_partial_misses() const = 0; 
    virtual const std::vector<uint64_t>& get_ways(uint64_t set_idx) const = 0; 

    virtual void set_do_mrc(bool mrc)  = 0;
};

template<typename T>
class Cache: public BaseCache {
    std::string NAME;
    uint64_t num_sets;
    uint64_t num_ways;
    uint64_t total_accesses = 0;
    uint64_t level = 0;
    std::unordered_map<uint64_t, T> sets;
    const bool is_sectored;
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
    Cache():
        NAME("DefaultCache"),
        num_sets(1024),
        level(0),
        is_sectored(false),
        insertion_policy(EXCLUSIVE) {
        num_ways = 16;
        for (uint64_t i = 0; i < num_sets; ++i) {
            sets[i] = T(num_ways, 64, i, level);
        }
        partial_misses.resize(CACHELINE_SIZE/64, 0);
    }

    Cache(std::string name, uint64_t _num_sets, uint64_t _num_ways, uint64_t block_size, uint64_t level, bool is_sectored, InsertionPolicy policy=EXCLUSIVE):
        NAME(name),
        num_sets(_num_sets),
        num_ways(_num_ways),
        level(level),
        is_sectored(is_sectored),
        insertion_policy(policy)
    {
    
        if (is_sectored) {
            assert(block_size != CACHELINE_SIZE);
        } else {
            num_ways = num_ways*CACHELINE_SIZE/block_size;
        }
        for (uint64_t i = 0; i < num_sets; ++i) {
                sets[i] = T(num_ways, block_size, i, level);
        }
    
        partial_misses.resize(CACHELINE_SIZE/block_size, 0);
    }

    uint64_t get_set_idx(uint64_t address);
    bool try_hit(PacketPtr packet);
    void handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0);
    void handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0);
    void handle_evict(PacketPtr access_packet, PacketPtr eviction_packet, int level = 0);
    std::vector<uint64_t> handle_invalidate(PacketPtr packet);
    void populate_fill_packet(PacketPtr packet);

    bool can_insert_at_level(int level);

    MRC* get_mrc() {return &mrc;};
    void print_mpki_curve(uint64_t inst_count);
    void print_stats(uint64_t inst_count, std::string tracename);
    void print_reuse_distance();

    InsertionPolicy get_insertion_policy() const { return insertion_policy; }
    uint64_t get_block_size(uint64_t set_idx) { return sets[set_idx].get_block_size(); }
    bool get_do_mrc() {return sets[0].get_do_mrc();}
    bool get_is_sectored() {return is_sectored;}

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
    const std::vector<uint64_t>& get_ways(uint64_t set_idx) const {return sets.at(set_idx).get_ways();}

    void set_do_mrc(bool mrc) {
        for (auto& set: sets) {
            set.second.set_do_mrc(mrc);
        }
    }
};

uint64_t align_address(uint64_t address, uint64_t align_size);

uint64_t count_footprint(uint64_t footprint);

void resize_packet(PacketPtr packet, uint64_t block_size);
#endif
