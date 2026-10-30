#ifndef __CACHESIM_H__
#define __CACHESIM_H__

#include "defs.h"
#include "utils.h"
#include "predictor.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <numeric>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <CLI/CLI.hpp>
#include <list>
#include <unordered_map>

#include "tracereader.h"

enum class SetDuelingType {
    Follower,
    Leader64,
    Leader8,
};

class BaseCache;

class Set {
    protected:
    BaseCache* cache;
    uint64_t num_ways;
    uint64_t original_num_ways = 0;
    std::vector<uint64_t> ways;
    // Per-way tag: the address bits above the set index, plus the sector bits
    // when block_size < 64 - i.e. the 64 - log2(block_size) - log2(num_sets)
    // bits get_total_cache_size() charges. Kept in lockstep with ways (set via
    // set_way_address()); lookups match on tags. INVALID_TAG for empty ways.
    static constexpr uint64_t INVALID_TAG = UINT64_MAX;
    std::vector<uint64_t> tags;
    // Base+Delta tag encoding (--tag-bdi-bases B, iso-area 8B sets only). Per
    // set: up to B base line tags (bdi_bases, INVALID_TAG = free slot) and how
    // many ways use each (bdi_refs). Per way: base id, signed delta and sector,
    // so that tag = ((bdi_bases[id] + delta) << sector_bits) | sector. A block
    // whose line tag fits no base while all B slots are in use keeps its full
    // tag ("overflow", bdi_base = -1): this step stores and looks up tags in
    // compressed form but does not yet constrain admission.
    std::vector<uint64_t> bdi_bases;
    std::vector<uint32_t> bdi_refs;
    std::vector<int32_t> bdi_base;
    std::vector<int64_t> bdi_delta;
    std::vector<uint8_t> bdi_sector;
    std::unique_ptr<BasePolicy> repl_counter;
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
    CacheLevel level = CacheLevel::L1D;

    uint64_t bits_per_block;
    uint64_t bitmask;

    SetDuelingType set_dueler = SetDuelingType::Follower;

    public:
    uint64_t reuse_dist = 0;
    uint64_t accesses = 0;
    uint64_t hits = 0;
    void set_dueling_type(SetDuelingType type) {
        set_dueler = type;
    }
    SetDuelingType get_dueling_type() {
        return set_dueler;
    }
    template<typename VecType>
    void breakdown(std::vector<VecType>& vec, uint64_t block_size, uint64_t scale_factor, bool incr);
    template<typename VecType>
    void contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks);
    void set_breakdown(uint64_t block_size);
    void set_merge(uint64_t new_block_size);

    public:
    Set() {}
    Set(BaseCache* p, uint64_t _num_ways, uint64_t blk_size, uint64_t _set_idx, ReplacementPolicy policy, CacheLevel _level) {
        cache = p;
        num_ways = _num_ways;
        original_num_ways = _num_ways;
        block_size = blk_size;
        set_idx = _set_idx;
        level = _level;
        num_blocks = CACHELINE_SIZE/block_size;
        num_lines = num_ways/num_blocks;
        bits_per_block = block_size/8;
        bitmask = (1ULL << bits_per_block) - 1;
        ways.resize(num_ways, UINT64_MAX);
        tags.resize(num_ways, INVALID_TAG);
        bdi_reset();
        repl_counter = std::unique_ptr<BasePolicy>(create_policy(policy, set_idx, num_ways, level));
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
        if (amoeba_active()) {
            am_word.assign(num_ways, -1);
            am_way_block.assign(num_ways, -1);
            am_way_pos.assign(num_ways, 0);
        }
    }

    ~Set() {
    }

    uint64_t make_tag(uint64_t address) const;
    void set_way_address(uint64_t way_idx, uint64_t address);
    uint64_t find_way(uint64_t aligned_address) const;  // tags.size() if absent

    // Base+Delta tag encoding (see bdi_bases above).
    bool bdi_active() const {
        return cachesim::TAG_BDI_BASES > 0 && cachesim::isoArea && block_size < CACHELINE_SIZE;
    }
    uint64_t sector_bits() const;
    void bdi_reset();                          // all ways unencoded, all base slots free
    void bdi_release(uint64_t way_idx);        // drop way's base reference
    void bdi_encode(uint64_t way_idx, bool count);
    void bdi_rebuild();                        // re-encode every way (after breakdown)
    uint64_t find_way_bdi(uint64_t tag) const; // lookup through the bases
    void bdi_resident(uint64_t& encoded, uint64_t& overflow) const;
    // Admission constraint (on unless --tag-bdi-unconstrained): before a fill,
    // make sure its line tag fits a base or a base slot is free; otherwise
    // evict every way of the base with the fewest users. Returns the number of
    // blocks evicted and adds their used footprint words to used_words.
    uint64_t bdi_reserve(PacketPtr fill_packet, uint64_t& used_words);
    void invalidate_all_ways();
    // Line-lifetime utilization (LLC only): 8B pieces leaving the hierarchy -
    // evicted to memory, dropped at fill, or discarded by a breakdown/merge -
    // and how many of them were touched. A piece's footprint bit is ORed over
    // all its L1<->LLC trips, so each piece is counted once per lifetime.
    inline static uint64_t life_pieces = 0;
    inline static uint64_t life_touched = 0;
    void retire_way(uint64_t way_idx);
    // Reuse by degree (LLC only): one record per block when its lifetime ends
    // (evicted, dropped at fill, or resident at end of run). Reuse = times it
    // was serviced from the LLC. Degree rows are power-of-two buckets
    // (0 = unlabelled, then [1,2), [2,4), ..., [2^20, inf)); reuse columns
    // are 0, 1, 2, 3, 4-7, 8-15, 16-31, 32+.
    static constexpr int REUSE_DEG_BUCKETS = 22, REUSE_BUCKETS = 8;
    inline static uint64_t reuse_hist[REUSE_DEG_BUCKETS][REUSE_BUCKETS] = {};
    inline static uint64_t reuse_hits_by_deg[REUSE_DEG_BUCKETS] = {};
    // [0] non-hub, [1] hub (degree > avg_degree, as is_hub_node), [2] unlabelled
    inline static uint64_t reuse_blocks[3] = {}, reuse_reused[3] = {}, reuse_hits[3] = {};
    inline static bool reuse_residents_recorded = false;
    static void record_reuse(uint64_t degree, float avg_degree, uint64_t llc_hits);
    void record_reuse_residents() const;
    static void print_reuse_by_degree(const char* name);
    // End-of-run LLC contents in 8B units: capacity, valid, touched.
    void count_resident(uint64_t& capacity, uint64_t& valid_pieces, uint64_t& touched) const;
    inline static uint64_t bdi_stat_encoded = 0;
    inline static uint64_t bdi_stat_overflow = 0;
    inline static uint64_t bdi_stat_installs = 0;
    inline static uint64_t bdi_stat_frees = 0;
    inline static uint64_t bdi_stat_replacements = 0;
    inline static uint64_t bdi_stat_replaced_blocks = 0;

    void evict_way_into(PacketPtr packet, uint64_t way_idx, uint64_t num_blocks_evicted, uint64_t num_blocks_to_evict);

    // Amoeba-Cache (LLC, --replacement-policy amoeba). The set's num_ways are
    // its physical 64-bit words. An Amoeba-Block is a tag word followed by a
    // contiguous run of data words from one 64B line; each data word is also
    // one Set way (8B block), so hits, invalidations and stats are unchanged.
    // am_word is the T?/V? state: -1 for a free word, else the id of the block
    // whose tag or data occupies it. Blocks are placed first-fit and replaced
    // whole, in LRU (insertion) order, until the incoming run fits.
    struct AmoebaBlock {
        uint32_t start = 0, len = 0;   // first word in the line, # data words
        uint32_t pos = 0;              // tag word; data at pos+1 .. pos+len
        uint32_t live = 0;             // data words still valid
        uint64_t stamp = 0;            // LRU order (0 = free entry)
        std::vector<uint32_t> ways;    // Set ways holding its data words
    };
    std::vector<int32_t> am_word;
    std::vector<AmoebaBlock> am_blocks;
    std::vector<int32_t> am_free_ids;
    std::vector<int32_t> am_way_block;   // per way: owning block, -1 if none
    std::vector<uint32_t> am_way_pos;    // per way: its data word
    std::vector<int32_t> am_pending;     // blocks allocated for the current fill
    uint64_t am_clock = 0;
    bool amoeba_active() const {
#ifdef MULTI_LEVEL
        return cachesim::AMOEBA && level == CacheLevel::LLC && block_size == 8;
#else
        return cachesim::AMOEBA && block_size == 8;   // the single cache
#endif
    }
    // Whether the block at block_address of an incoming L1D victim is stored.
    // In the single-level build the fill is a demand miss with no footprint,
    // so the demanded word counts as the touched one.
    bool amoeba_keep(PacketPtr fill_packet, uint64_t block_address) const;
    // Before a fill: allocate one block for the fill's words (one contiguous
    // range), evicting whole blocks into eviction_packet until it fits.
    // Returns the number of 8B blocks evicted.
    uint64_t amoeba_reserve(PacketPtr fill_packet, PacketPtr eviction_packet);
    void amoeba_attach_way(uint64_t way_idx, uint64_t block_address);
    void amoeba_release_way(uint64_t way_idx);
    void amoeba_resident(uint64_t& blocks, uint64_t& data_words) const;
    inline static uint64_t am_stat_fills = 0, am_stat_blocks = 0, am_stat_words = 0;
    inline static uint64_t am_stat_dropped = 0, am_stat_evicted_blocks = 0, am_stat_frag_evictions = 0;
    // Why an LLC miss's word was not in the LLC: the last way it left - dropped
    // at fill (1) or evicted (2); absent = never left the hierarchy before.
    inline static std::unordered_map<uint64_t, uint8_t> am_fate;
    inline static uint64_t am_miss_dropped = 0, am_miss_evicted = 0, am_miss_never = 0;
    static void print_amoeba_stats();

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
    BasePolicy* get_replacement_policy() const { return repl_counter.get(); }

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
    std::vector<uint64_t> block_prev_counter;
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
        block_prev_counter.resize(num_blocks, UINT64_MAX);
    }

    Sector(const Sector& other) noexcept : 
        sectors(other.sectors),
        valid(other.valid),
        dirty(other.dirty),
        degree(other.degree),
        avg_degree(other.avg_degree),
        block_serviced_from_llc(other.block_serviced_from_llc),
        block_prev_counter(other.block_prev_counter),
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
        std::fill(block_prev_counter.begin(), block_prev_counter.end(), UINT64_MAX);
    }
};

class SectoredSet: public Set {
    protected:
    BaseCache* cache;
    std::vector<Sector> way_sectors;

    public:
    SectoredSet() {}
    SectoredSet(BaseCache* p, uint64_t _num_ways, uint64_t blk_size, uint64_t _set_idx, ReplacementPolicy policy, CacheLevel _level) {
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
        repl_counter = std::unique_ptr<BasePolicy>(create_policy(policy, set_idx, num_ways, level));
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
    virtual void handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet) = 0;
    virtual void handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet) = 0;
    virtual void handle_evict(PacketPtr access_packet, PacketPtr eviction_packet) = 0;
    virtual void handle_invalidate(PacketPtr packet) = 0;
    virtual void populate_line(PacketPtr packet) = 0;
    virtual void populate_blocks(PacketPtr packet) = 0;
    virtual void breakdown(uint64_t block_size) = 0;
    virtual void merge(uint64_t block_size) = 0;

    virtual bool can_insert_at_level(CacheLevel level) = 0;
    virtual CacheLevel get_level() const = 0;
    virtual void incr_psel() = 0;
    virtual void decr_psel() = 0;
    virtual void record_leader_access(SetDuelingType type, bool hit) = 0;

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
    virtual uint64_t get_psel() const = 0;
    virtual uint64_t get_num_sets() const = 0;
    virtual bool should_breakdown() = 0;
    virtual bool should_merge() = 0;
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
    CacheLevel level = CacheLevel::L1D;
    std::unordered_map<uint64_t, std::unique_ptr<T>> sets;
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
    uint64_t evicted_words = 0;   // 8B words in the evicted/invalidated blocks (Utilization denominator)
    std::vector<uint64_t> partial_misses;
    uint64_t PSEL;
    bool is_broken_down = false;

    // Set-dueling confidence estimator (DuelingMode::ZTEST / ZTEST_RATIO):
    // every CONF_EPOCH instructions, compute a z-score comparing Leader64 vs
    // Leader8 behavior - ZTEST uses a conditional-binomial test on miss
    // *counts* directly (assumes ~equal exposure between the two leader
    // groups, measured within ~0.1-2.6% across a 9-trace suite; doesn't need
    // per-side access counts), ZTEST_RATIO uses a two-proportion test on miss
    // *rates* (misses/accesses per side, corrects for any exposure imbalance
    // at the cost of tracking accesses too). Both modes share the same
    // checkpoint/streak/lock machinery below - only which z-score feeds it
    // differs (see update_dueling_confidence()). Only CONF_MAX consecutive
    // checkpoints where z > Z_THRESHOLD build confidence; anything else
    // resets the streak to zero. A second, independent check runs the same
    // test over only the last Z_WINDOW_SIZE checkpoints (recent-only, not
    // all-time) - this exists because the cumulative test is slow to react
    // to a genuine late phase change (it has to outweigh the entire
    // accumulated history first); the windowed test reacts on its own
    // timescale instead. Either path reaching its streak requirement locks
    // in the decision. Past DUELING_PERIOD both checks apply against a much
    // lower bar (one confirming checkpoint), so a workload that never builds
    // full confidence still gets a decision eventually - but never from a
    // single raw access read.
    //
    // DuelingMode::PSEL uses its own quiet-epoch streak instead of a z-score
    // (see the psel_quiet_streak fields below) but shares the same
    // duel_locked flag and two-way latch semantics.
    //
    // duel_locked is a two-way latch for every mode: true means the
    // statistics currently favor breakdown, false means they favor 64B.
    // should_merge() reads it directly, so a genuine reversal on an
    // already-broken-down cache triggers a merge back to 64B, no matter how
    // far into the run it happens.
    uint64_t conf_last_check = 0;
    uint64_t conf_counter = 0;
    uint64_t window_conf_counter = 0;
    bool duel_locked = false;

    uint64_t merge_streak = 0;

    // DuelingMode::PSEL confidence estimator, handled inside
    // update_dueling_confidence() alongside the z-test branch rather than as
    // a separate function: a "switch" is PSEL crossing PSEL_THRESHOLD - going
    // from >= threshold to < threshold (favors breakdown) or from < threshold
    // back up to >= threshold (favors 64B) - counted in incr_psel()/
    // decr_psel() by comparing PSEL's value before and after the +-1 step.
    // This is a coarser, rarer event than a raw increment/decrement reversal:
    // PSEL can reverse direction many times per epoch while staying on the
    // same side of the threshold the whole time, and none of that counts as
    // a switch here. Every CONF_EPOCH instructions, a checkpoint counts
    // toward psel_quiet_streak only if zero threshold-crossings happened
    // *and* at least one incr_psel()/decr_psel() call happened during that
    // epoch - without the latter, an epoch with no accesses to the leader
    // sets at all (e.g. cold start) is indistinguishable from zero-switches,
    // and would otherwise look identical to a genuinely settled epoch. Any
    // crossing, or a completely idle epoch, resets the streak to zero.
    // CONF_MAX consecutive quiet checkpoints latches duel_locked - true if
    // PSEL settled below PSEL_THRESHOLD, false if it settled at or above it
    // (see update_dueling_confidence()) - same as the z-test's own streak
    // requirement.
    uint64_t psel_switches_this_epoch = 0;
    uint64_t psel_changes_this_epoch = 0;
    // Snapshot of psel_switches_this_epoch taken right before
    // update_dueling_confidence() resets it for the next epoch, so
    // log_dueling_metrics() (called right after) can still report the just-
    // completed epoch's switch count instead of always seeing 0.
    uint64_t psel_switches_last_epoch = 0;
    uint64_t psel_quiet_streak = 0;

    uint64_t leader64_accesses = 0;
    uint64_t leader64_misses = 0;
    uint64_t leader8_accesses = 0;
    uint64_t leader8_misses = 0;

    // Windowed check: ring buffer of the last Z_WINDOW_SIZE checkpoints'
    // per-checkpoint deltas (access64, miss64, access8, miss8), plus running
    // sums over that window so the windowed counts don't need to be
    // recomputed from scratch each time.
    std::deque<std::array<uint64_t, 4>> window_buffer;
    uint64_t window_leader64_accesses = 0;
    uint64_t window_leader64_misses = 0;
    uint64_t window_leader8_accesses = 0;
    uint64_t window_leader8_misses = 0;
    uint64_t window_snapshot_leader64_accesses = 0;
    uint64_t window_snapshot_leader64_misses = 0;
    uint64_t window_snapshot_leader8_accesses = 0;
    uint64_t window_snapshot_leader8_misses = 0;

    // Investigative instrumentation: periodically dump PSEL, whole-cache MPKI,
    // and the cumulative/windowed z-scores, independent of DuelingMode, so
    // the full trajectory is visible even after a decision is made and even
    // in PSEL mode. Gated behind --log-dueling-metrics; off by default.
    uint64_t metrics_log_last = 0;

    void incr_psel();
    void decr_psel();
    void record_leader_access(SetDuelingType type, bool hit);
    void update_dueling_confidence();
    void log_dueling_metrics();



    
    public:
    Cache();

    Cache(std::string name, uint64_t _num_sets, uint64_t _num_ways, uint64_t block_size, CacheLevel level, bool is_sectored, ReplacementPolicy repl_policy, InsertionPolicy policy=InsertionPolicy::EXCLUSIVE);

    std::string get_name() const {return NAME;}
    uint64_t get_set_idx(uint64_t address) const;
    bool try_hit(PacketPtr packet);
    void handle_fill_blocks(PacketPtr fill_packet, PacketPtr eviction_packet);
    void handle_fill_line(PacketPtr fill_packet, PacketPtr eviction_packet);
    void handle_evict(PacketPtr access_packet, PacketPtr eviction_packet);
    void handle_invalidate(PacketPtr packet);
    void populate_line(PacketPtr packet);
    void populate_blocks(PacketPtr packet);
    void breakdown(uint64_t block_size);
    void merge(uint64_t block_size);

    bool is_eviction_needed(PacketPtr packet) const;
    bool can_insert_at_level(CacheLevel level);
    CacheLevel get_level() const { return level; }

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
    uint64_t get_psel() const { return PSEL; }
    uint64_t get_num_sets() const { return num_sets;}
    bool should_breakdown() {
        // update_dueling_confidence() now handles PSEL's own quiet-epoch
        // streak alongside the z-test branch, setting duel_locked either
        // way, so this just reads the one shared flag regardless of mode.
        update_dueling_confidence();
        log_dueling_metrics();
        return duel_locked;
    }

    bool should_merge() {
        // should_breakdown() is always called first each access (see
        // access_multi_level()) and refreshes duel_locked for this
        // checkpoint, so this only needs the two conditions: never merge a
        // cache that isn't broken down, and otherwise merge back to 64B once
        // the statistics have reverted to favoring it.
        if (!is_broken_down)
            return false;
#ifdef NO_MERGE
        // Experiment build: once broken down, stay broken down.
        return false;
#endif
        return !duel_locked;
    }

    void incr_partial_misses(uint64_t num_misses) { partial_misses[num_misses]++; }
    std::vector<uint64_t> get_partial_misses() const {return partial_misses;}
    const std::vector<uint64_t>& get_ways(uint64_t set_idx) const {return sets.at(set_idx)->get_ways();}
};

#ifdef MULTI_LEVEL
void access_multi_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            SparsityPredictor* predictor, uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree);
#else
void access_single_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            uint64_t pc, uint64_t address, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree);
#endif

void useAddressTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters);

void useInstructionTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters);

void useChampsimTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters);

#endif
