#ifndef __CACHESIM_H__
#define __CACHESIM_H__

#include "defs.h"
#include "utils.h"

class BaseCache;

class Set {
    protected:
    BaseCache* cache;
    uint64_t num_ways;
    std::vector<uint64_t> ways;
    BasePolicy* repl_counter;
    std::vector<bool> valid;
    std::vector<uint64_t> serviced_from_llc;
    std::vector<bool> is_hub_node;
    std::vector<uint64_t> distance_counts;
    std::vector<bool> dirty;
    std::vector<bool> footprint;
    std::vector<uint64_t> pc;
    std::vector<uint64_t> next_reuse;
    std::vector<uint64_t> way_hits;
    std::vector<uint64_t> degree;
    std::vector<float> avg_degree;

    uint64_t block_size = 64;
    uint64_t set_idx = 1;
    uint64_t num_blocks;
    uint64_t num_lines;
    uint64_t level = 0;

    uint64_t bits_per_block;
    uint64_t bitmask;

    public:
    uint64_t reuse_dist = 0;
    uint64_t accesses = 0;
    uint64_t hits = 0;

    public:
    Set() {}
    Set(BaseCache* p, uint64_t _num_ways, uint64_t blk_size, uint64_t _set_idx, ReplacementPolicy policy, uint64_t _level) {
        cache = p;
        num_ways = _num_ways;
        block_size = blk_size;
        set_idx = _set_idx;
        level = _level;
        num_blocks = CACHELINE_SIZE/block_size;
        num_lines = num_ways/num_blocks;
        bits_per_block = block_size/8;
        bitmask = (1ULL << bits_per_block) - 1;
        ways.resize(num_ways, UINT64_MAX);
        repl_counter = create_policy(policy, set_idx, num_ways, level);
        assert(repl_counter != nullptr);
        valid.resize(num_ways, false);
        serviced_from_llc.resize(num_ways, 0);
        is_hub_node.resize(num_ways, false);
        dirty.resize(num_ways, false);
        pc.resize(num_ways, UINT64_MAX);
        distance_counts.resize(num_ways, 0);
        footprint.resize(num_ways*block_size/8, false);
        next_reuse.resize(num_ways, 0);
        way_hits.resize(num_ways, 0);
        degree.resize(num_ways, 0);
        avg_degree.resize(num_ways, 0.0f);
    }

    ~Set() {
        delete repl_counter;
    }

    void fill_way(PacketPtr packet, uint64_t way_idx);
    void fill_packet(PacketPtr packet, uint64_t way_idx);
    void invalidate_way(uint64_t way_idx);

    bool try_hit(PacketPtr packet);
    void handle_fill(PacketPtr packet);
    void handle_evict(PacketPtr eviction_packet);
    void handle_invalidate(PacketPtr packet, uint64_t block_num);

    uint64_t get_block_size() const { return block_size; }
    uint64_t get_distance_count(uint64_t way) const { return distance_counts[way]; }

    bool get_footprint(uint64_t way_idx, uint64_t word_idx);
    void set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed);
    uint64_t get_block_accesses(uint64_t way_idx, uint64_t word_idx);
    void set_block_accesses(uint64_t way_idx, uint64_t word_idx, uint64_t value);

    const std::vector<uint64_t>& get_ways() const {return ways;}
    bool get_valid(uint64_t idx) const {return valid[idx];}
    uint64_t get_serviced_from_llc(uint64_t idx) const {return serviced_from_llc[idx];}
    uint64_t get_num_invalid() const { return (uint64_t)(std::count(valid.begin(), valid.end(), false));}
    BasePolicy* get_replacement_policy() const { return repl_counter; }

    bool is_eviction_needed(uint64_t num_ways) const {
        return (get_num_invalid() < num_ways);
    }
};


struct Sector {
    std::vector<uint64_t> sectors;
    std::vector<bool> valid;
    std::vector<bool> dirty;
    std::vector<uint64_t> degree;
    std::vector<float> avg_degree;
    std::vector<uint64_t> block_serviced_from_llc;
    uint64_t num_blocks;

    Sector(uint64_t num_blocks):
        num_blocks(num_blocks)
    {
        sectors.resize(num_blocks, UINT64_MAX);
        valid.resize(num_blocks, false);
        dirty.resize(num_blocks, false);
        degree.resize(num_blocks, 0);
        avg_degree.resize(num_blocks, 0.0f);
        block_serviced_from_llc.resize(num_blocks, 0);
    }

    Sector(const Sector& other) noexcept : 
        sectors(other.sectors),
        valid(other.valid),
        dirty(other.dirty),
        degree(other.degree),
        avg_degree(other.avg_degree),
        block_serviced_from_llc(other.block_serviced_from_llc),
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
        std::fill(degree.begin(), degree.end(), 0);
        std::fill(avg_degree.begin(), avg_degree.end(), 0.0f);
        std::fill(block_serviced_from_llc.begin(), block_serviced_from_llc.end(), 0);
    }
};

class SectoredSet: public Set {
    protected:
    BaseCache* cache;
    std::vector<Sector> way_sectors;

    public:
    SectoredSet() {}
    SectoredSet(BaseCache* p, uint64_t _num_ways, uint64_t blk_size, uint64_t _set_idx, ReplacementPolicy policy, uint64_t _level) {
        cache = p;
        num_ways = _num_ways;
        block_size = blk_size;
        set_idx = _set_idx;
        level = _level;
        bits_per_block = block_size/8;
        bitmask = (1ULL << bits_per_block) - 1;
        num_blocks = CACHELINE_SIZE/block_size;
        num_lines = num_ways/num_blocks;
        ways.resize(num_ways, UINT64_MAX);
        way_sectors.assign(num_ways, Sector(num_blocks));
        repl_counter = create_policy(policy, set_idx, num_ways, level);
        assert(repl_counter != nullptr);
        valid.resize(num_ways, false);
        serviced_from_llc.resize(num_ways, 0);
        is_hub_node.resize(num_ways, 0);
        dirty.resize(num_ways, false);
        pc.resize(num_ways, UINT64_MAX);
        distance_counts.resize(num_ways, 0);
        footprint.resize(num_ways*block_size, false);
        next_reuse.resize(num_ways, 0);
        way_hits.resize(num_ways, 0);
        degree.resize(num_ways, 0);
        avg_degree.resize(num_ways, 0.0f);

    }

    ~SectoredSet() {
        delete repl_counter;
    }

    void fill_way(PacketPtr packet, uint64_t way_idx);
    void fill_packet(PacketPtr packet, uint64_t way_idx);
    void invalidate_way(uint64_t way_idx);

    bool try_hit(PacketPtr packet);
    void handle_fill(PacketPtr packet);
    void handle_evict(PacketPtr eviction_packet);
    void handle_invalidate(PacketPtr packet, uint64_t block_num);
    uint64_t get_block_size() const { return block_size; }
    bool get_footprint(uint64_t way_idx, uint64_t word_idx);
    void set_footprint(uint64_t way_idx, uint64_t word_idx, bool accessed);
    uint64_t get_block_accesses(uint64_t way_idx, uint64_t word_idx);
    void set_block_accesses(uint64_t way_idx, uint64_t word_idx, uint64_t value);

    const std::vector<uint64_t>& get_ways() const {return ways;}
    bool get_valid(uint64_t idx) const {return valid[idx];}
    uint64_t get_serviced_from_llc(uint64_t idx) const {return serviced_from_llc[idx];}
    uint64_t get_num_invalid() const { return (uint64_t)(std::count(valid.begin(), valid.end(), false));}

    bool is_eviction_needed(uint64_t num_ways) const {
        return (get_num_invalid() < num_ways);
    }
};



class BaseCache {
    public:
    virtual ~BaseCache() = default;
    virtual uint64_t get_set_idx(uint64_t address) const = 0;
    virtual bool try_hit(PacketPtr packet) = 0;
    virtual void handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0) = 0;
    virtual void handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0) = 0;
    virtual void handle_evict(PacketPtr access_packet, PacketPtr eviction_packet) = 0;
    virtual void handle_invalidate(PacketPtr packet) = 0;
    virtual void populate_line(PacketPtr packet) = 0;
    virtual void populate_blocks(PacketPtr packet) = 0;

    virtual bool can_insert_at_level(int level) = 0;

    virtual void update_data_var_hub_hits(bool is_hub, uint64_t serviced_from_llc) = 0;
    virtual void update_data_var_hub_evictions(bool is_hub, uint64_t serviced_from_llc) = 0;
    virtual void update_data_var_eviction_reuse(bool is_hub, uint64_t reuse) = 0;
    virtual void update_data_var_pc_evictions1(uint64_t fill_pc, uint64_t eviction_pc) = 0;
    virtual void update_data_var_pc_evictions2(uint64_t fill_pc, bool is_hub) = 0;
    virtual void update_data_var_pc_evictions3(uint64_t fill_pc, uint64_t eviction_pc, uint64_t degree) = 0;
    virtual void update_data_var_invalidations(uint64_t pc, uint64_t footprint) = 0;

    virtual void update_data_var_evictions(uint64_t pc, uint64_t footprint) = 0;
    virtual void update_data_var_block_accesses(uint64_t pc, std::vector<uint64_t> footprint) = 0;

    virtual void update_data_var_hits(uint64_t pc) = 0;

    virtual void print_mpki_curve(uint64_t inst_count) = 0;
    virtual void print_stats(uint64_t inst_count, std::string tracename) = 0;
    virtual void print_reuse_distance() = 0;

    virtual InsertionPolicy get_insertion_policy() const = 0;
    virtual uint64_t get_block_size(uint64_t set_idx) const = 0;
    virtual bool get_is_sectored() = 0;

    virtual std::string get_name() const = 0;
    virtual uint64_t get_hits() const = 0; 
    virtual uint64_t get_misses() const = 0; 
    virtual uint64_t get_accesses() const = 0; 
    virtual uint64_t get_read_hits() const = 0; 
    virtual uint64_t get_read_misses() const = 0; 
    virtual uint64_t get_write_hits() const = 0; 
    virtual uint64_t get_write_misses() const = 0; 
    virtual uint64_t get_evictions() const = 0; 
    virtual uint64_t get_num_blocks_used() const = 0; 
    virtual void incr_partial_misses(uint64_t num_misses) = 0;
    virtual std::vector<uint64_t> get_partial_misses() const = 0; 
    virtual const std::vector<uint64_t>& get_ways(uint64_t set_idx) const = 0; 

    virtual bool is_eviction_needed(PacketPtr packet) const = 0;
};

template<typename T>
class Cache: public BaseCache {
    std::string NAME;
    uint64_t num_sets;
    uint64_t num_ways;
    uint64_t total_accesses = 0;
    uint64_t level = 0;
    std::unordered_map<uint64_t, T*> sets;
    const bool is_sectored;
    const InsertionPolicy insertion_policy = InsertionPolicy::EXCLUSIVE;
    //Stats
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t read_hits = 0;
    uint64_t read_misses = 0;
    uint64_t write_hits = 0;
    uint64_t write_misses = 0;
    uint64_t evictions = 0;
    uint64_t invalidations = 0;
    uint64_t num_blocks_used = 0;
    std::vector<uint64_t> partial_misses;


    // Workload Behavior
    std::unordered_map<uint64_t, uint64_t> data_var_misses;
    std::unordered_map<uint64_t, std::map<uint64_t, uint64_t>> data_var_invalidations;
    std::unordered_map<bool, std::map<uint64_t, uint64_t>> data_var_hub_hits;
    std::unordered_map<bool, std::map<uint64_t, uint64_t>> data_var_hub_evictions;
    std::unordered_map<bool, std::map<int64_t, uint64_t>> data_var_eviction_reuse;
    std::unordered_map<uint64_t, std::map<uint64_t, uint64_t>> data_var_pc_evictions1;
    std::unordered_map<uint64_t, std::map<bool, uint64_t>> data_var_pc_evictions2;
    std::unordered_map<uint64_t, std::map<uint64_t, uint64_t>> data_var_pc_evictions3;
    std::map<uint64_t, std::map<uint64_t, uint64_t>> data_var_evictions;
    std::map<uint64_t, std::map<uint64_t, uint64_t>> data_var_block_accesses;
    std::map<uint64_t, uint64_t> data_var_hits;

    void update_data_var_hub_hits(bool is_hub, uint64_t serviced_from_llc) {
//        if (data_var_hub_hits.find(is_hub) == data_var_hub_hits.end()) {
//            data_var_hub_hits[is_hub] = std::map<uint64_t, uint64_t>();
//        }
//
//        if (data_var_hub_hits[is_hub].find(serviced_from_llc) == data_var_hub_hits[is_hub].end()) {
//            data_var_hub_hits[is_hub][serviced_from_llc] = 1;
//        } else {
//            data_var_hub_hits[is_hub][serviced_from_llc] += 1;
//        }
    }

    void update_data_var_hub_evictions(bool is_hub, uint64_t serviced_from_llc) {
//        if (data_var_hub_evictions.find(is_hub) == data_var_hub_evictions.end()) {
//            data_var_hub_evictions[is_hub] = std::map<uint64_t, uint64_t>();
//        }
//
//        if (data_var_hub_evictions[is_hub].find(serviced_from_llc) == data_var_hub_evictions[is_hub].end()) {
//            data_var_hub_evictions[is_hub][serviced_from_llc] = 1;
//        } else {
//            data_var_hub_evictions[is_hub][serviced_from_llc] += 1;
//        }
    }

    void update_data_var_eviction_reuse(bool is_hub, uint64_t reuse) {
        if (reuse == UINT64_MAX) return;
        if (reuse == 0) return;
        auto next_reuse = reuse - cachesim::inst_count;
        next_reuse = next_reuse & ~(0x8 - 1);//0x7f;
        if (next_reuse > 0x3200) return;
        if (data_var_eviction_reuse.find(is_hub) == data_var_eviction_reuse.end()) {
            data_var_eviction_reuse[is_hub] = std::map<int64_t, uint64_t>();
        }

        if (data_var_eviction_reuse[is_hub].find(next_reuse) == data_var_eviction_reuse[is_hub].end()) {
            data_var_eviction_reuse[is_hub][next_reuse] = 1;
        } else {
            data_var_eviction_reuse[is_hub][next_reuse] += 1;
        }
    }


    void update_data_var_pc_evictions1(uint64_t fill_pc, uint64_t eviction_pc) {
//        if (data_var_pc_evictions1.find(fill_pc) == data_var_pc_evictions1.end()) {
//            data_var_pc_evictions1[fill_pc] = std::map<uint64_t, uint64_t>();
//        }
//
//        if (data_var_pc_evictions1[fill_pc].find(eviction_pc) == data_var_pc_evictions1[fill_pc].end()) {
//            data_var_pc_evictions1[fill_pc][eviction_pc] = 1;
//        } else {
//            data_var_pc_evictions1[fill_pc][eviction_pc] += 1;
//        }
    }

    void update_data_var_pc_evictions2(uint64_t fill_pc, bool is_hub) {
//        if (data_var_pc_evictions2.find(fill_pc) == data_var_pc_evictions2.end()) {
//            data_var_pc_evictions2[fill_pc] = std::map<bool, uint64_t>();
//        }
//
//        if (data_var_pc_evictions2[fill_pc].find(is_hub) == data_var_pc_evictions2[fill_pc].end()) {
//            data_var_pc_evictions2[fill_pc][is_hub] = 1;
//        } else {
//            data_var_pc_evictions2[fill_pc][is_hub] += 1;
//        }
    }

    void update_data_var_pc_evictions3(uint64_t fill_pc, uint64_t eviction_pc, uint64_t degree) {
//        if (data_var_pc_evictions3.find(fill_pc) == data_var_pc_evictions3.end()) {
//            data_var_pc_evictions3[fill_pc] = std::map<uint64_t, uint64_t>();
//        }
//
//        if (data_var_pc_evictions3[fill_pc].find(eviction_pc) == data_var_pc_evictions3[fill_pc].end()) {
//            data_var_pc_evictions3[fill_pc][eviction_pc] = degree;
//        } else {
//            data_var_pc_evictions3[fill_pc][eviction_pc] += degree;
//        }
    }


    void update_data_var_invalidations(uint64_t pc, uint64_t _footprint) {
//    if (data_var_invalidations.find(pc) == data_var_invalidations.end()) {
//        std::map<uint64_t, uint64_t> invalidations_hist;
//        invalidations_hist[0]=0;
//        invalidations_hist[1]=0;
//        invalidations_hist[2]=0;
//        invalidations_hist[3]=0;
//        invalidations_hist[4]=0;
//        invalidations_hist[5]=0;
//        invalidations_hist[6]=0;
//        invalidations_hist[7]=0;
//    
//        data_var_invalidations[pc] = invalidations_hist;
//    }
//    data_var_invalidations[pc][_footprint] += 1;
}

    void update_data_var_evictions(uint64_t pc, uint64_t _footprint) {
//        if (data_var_evictions.find(pc) == data_var_evictions.end()) {
//            std::map<uint64_t, uint64_t> footprint_hist;
//            footprint_hist[0]=0;
//            footprint_hist[1]=0;
//            footprint_hist[2]=0;
//            footprint_hist[3]=0;
//            footprint_hist[4]=0;
//            footprint_hist[5]=0;
//            footprint_hist[6]=0;
//            footprint_hist[7]=0;
//
//            data_var_evictions[pc] = footprint_hist;
//        }
//        data_var_evictions[pc][_footprint] += 1;
    }

    void update_data_var_block_accesses(uint64_t pc, std::vector<uint64_t> _footprint) {
        if (data_var_block_accesses.find(pc) == data_var_block_accesses.end()) {
            std::map<uint64_t, uint64_t> footprint_hist;
            for (int i = 0; i < 8; i++)
                footprint_hist[i] = _footprint[i];

            data_var_block_accesses[pc] = footprint_hist;
        } else {
            for (int i = 0; i < 8; i++)
                data_var_block_accesses[pc][i] += _footprint[i];
        }
    }

    void update_data_var_hits(uint64_t pc) {
        if (data_var_hits.find(pc) == data_var_hits.end()) {
            data_var_hits[pc] = 0;
        }
        data_var_hits[pc]++;
    }

    
    public:
    Cache();

    Cache(std::string name, uint64_t _num_sets, uint64_t _num_ways, uint64_t block_size, uint64_t level, bool is_sectored, ReplacementPolicy repl_policy, InsertionPolicy policy=InsertionPolicy::EXCLUSIVE);

    std::string get_name() const {return NAME;}
    uint64_t get_set_idx(uint64_t address) const;
    bool try_hit(PacketPtr packet);
    void handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0);
    void handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet, int level = 0);
    void handle_evict(PacketPtr access_packet, PacketPtr eviction_packet);
    void handle_invalidate(PacketPtr packet);
    void populate_line(PacketPtr packet);
    void populate_blocks(PacketPtr packet);

    bool is_eviction_needed(PacketPtr packet) const;
    bool can_insert_at_level(int level);

    void print_mpki_curve(uint64_t inst_count);
    void print_stats(uint64_t inst_count, std::string tracename);
    void print_reuse_distance();

    InsertionPolicy get_insertion_policy() const { return insertion_policy; }
    uint64_t get_block_size(uint64_t set_idx) const { return sets.at(set_idx)->get_block_size(); }
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
    uint64_t get_invalidations() const {return invalidations; }
    uint64_t get_num_blocks_used() const {return num_blocks_used; }
    void incr_partial_misses(uint64_t num_misses) { partial_misses[num_misses]++; }
    std::vector<uint64_t> get_partial_misses() const {return partial_misses;}
    const std::vector<uint64_t>& get_ways(uint64_t set_idx) const {return sets.at(set_idx)->get_ways();}
};


//class Set;
//class SectoredSet;
#endif
