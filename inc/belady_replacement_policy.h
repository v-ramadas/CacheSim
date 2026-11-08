#ifndef __BELADY_REPLACEMENT_POLICY_H__
#define __BELADY_REPLACEMENT_POLICY_H__

#include "replacement_policy.h"
#include <climits>
#include <string>
#include <vector>

// Belady's MIN (OPT) for the LLC.
//
// Next use: for 64B blocks, the trace's per-64B NextReuse, carried with the line (the L1D refreshes it on every hit)
// and kept per way. For smaller blocks, the next-use oracle (built by prescan() from the trace, at 8B granularity):
// a block's next use is the earliest future access to any of its 8B words. prescan() stores, per trace line, the
// next line touching the same 8B word, and per word its first access; on_access() (called for every trace access
// before it is simulated) moves the accessed word's next use to the line after this one. Without the oracle
// (non-instruction traces), the trace's per-64B NextReuse kept per way is used.
//
// Each way keeps its block's next use (counter). With the oracle it is looked up when the block is inserted and
// refreshed lazily: a block's next use changes only when that access happens, so a stored value <= TRACE_LINE (the
// line being simulated) is stale and is looked up again; any later value is still exact. Decisions therefore match
// a lookup of every way at every decision, at one lookup per inserted (or re-referenced) block.
//
// Oracle storage: a radix table over 8B words (1GB regions -> 4KB pages -> 512 words), 4B per word of every
// touched page, plus 4B per trace line.
struct BeladyOracle {
    static constexpr uint64_t PAGE_WORDS = 512, PAGE_SHIFT = 9;           // 4KB pages of 8B words
    static constexpr uint64_t MID_BITS = 18, TOP_ENTRIES = 1ULL << 20;    // 1GB regions; 2^50 B address space
    static constexpr uint32_t NONE = UINT32_MAX;
    std::vector<uint32_t> next_line;   // per trace line: next line touching the same 8B word (NONE = none)
    std::vector<uint32_t> top;         // region -> mid table index (NONE = untouched)
    std::vector<uint32_t> mid;         // mid tables of 2^MID_BITS pages: page -> leaf index (NONE = untouched)
    std::vector<uint32_t> next_use;    // leaves of PAGE_WORDS words: line of the word's next access (prescan: first)
    std::vector<uint32_t> last;        // prescan only, parallel to next_use: last line seen
    uint64_t pages = 0, words = 0;
    // Index of a word in next_use; UINT64_MAX if its page was never touched (insert=false).
    uint64_t slot(uint64_t word, bool insert) {
        uint64_t page = word >> PAGE_SHIFT;
        uint64_t region = page >> MID_BITS;
        if (region >= TOP_ENTRIES) return UINT64_MAX;
        if (top.empty()) { if (!insert) return UINT64_MAX; top.assign(TOP_ENTRIES, NONE); }
        uint32_t m = top[region];
        if (m == NONE) {
            if (!insert) return UINT64_MAX;
            m = top[region] = (uint32_t)(mid.size() >> MID_BITS);
            mid.resize(mid.size() + (1ULL << MID_BITS), NONE);
        }
        uint32_t& leaf = mid[((uint64_t)m << MID_BITS) + (page & ((1ULL << MID_BITS) - 1))];
        if (leaf == NONE) {
            if (!insert) return UINT64_MAX;
            leaf = (uint32_t)pages++;
            next_use.resize(pages * PAGE_WORDS, NONE);
            last.resize(pages * PAGE_WORDS, NONE);
        }
        return (uint64_t)leaf * PAGE_WORDS + (word & (PAGE_WORDS - 1));
    }
    uint64_t bytes() const {
        return (next_line.capacity() + top.capacity() + mid.capacity() + next_use.capacity() + last.capacity()) * 4;
    }
};

// OPT inserts only what it keeps: on a fill, if the set has no free way, a block is inserted only if its next use
// is earlier than the farthest of the blocks it competes with (residents and the fill's blocks already kept);
// otherwise it is bypassed (dropped). Evictions take the resident with the farthest next use.
class Belady : public BasePolicy {
    // Per-way block address (UINT64_MAX = invalid) for oracle lookups.
    std::vector<uint64_t> way_addr;
    // Bypass state of the current fill: next uses of the blocks the set would hold (residents, then kept fill
    // blocks replacing the farthest), and the free ways left.
    std::vector<uint64_t> fill_set;
    uint64_t fill_free = 0;
    // Next use of the block bypass() last evaluated, reused by fill_update() when it is inserted.
    uint64_t pending_addr = UINT64_MAX, pending_nu = 0, pending_line = UINT64_MAX;
    // Next use of the block in valid way w (oracle: refreshed if stale).
    uint64_t way_next_use(uint64_t w);

    inline static BeladyOracle oracle;
    static uint64_t block_next_use(uint64_t block_addr);

    public:
    inline static bool oracle_active = false;
    inline static uint64_t block_size = 64;           // LLC block size (set by main)
    inline static uint64_t stat_bypassed = 0, stat_inserted = 0;
    // Builds the next-use oracle from an instruction trace; false if it cannot read it.
    static bool prescan(const std::string& trace_path);
    static void on_access(uint64_t address) {
        uint64_t s = oracle.slot(address >> 3, false);
        if (s != UINT64_MAX) oracle.next_use[s] = oracle.next_line[cachesim::TRACE_LINE];
    }
    static void print_stats();

    Belady() {}

    Belady(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) {
        set_idx = _set_idx;
        num_ways = _num_ways;
        // counter = next-use trace line of the way's block; invalid ways hold 0 so they are never picked as a
        // victim (Set::handle_evict asks for candidates even when some ways are already free).
        counter.resize(num_ways, 0);
        way_addr.resize(num_ways, UINT64_MAX);
        insertion_clock.resize(num_ways, 0);
        level = _level;
        reserved_ways = 0;
    }
    
    void init_counter(PacketPtr packet);

    void hit_update(PacketPtr packet, uint64_t way_idx);

    void fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed);

    uint64_t get_eviction_candidate(bool is_low_priority);

    uint64_t get_reserved_eviction_candidate(bool is_low_priority);

    void evict(uint64_t way_idx);

    void invalidate(uint64_t way_idx) { counter[way_idx] = 0; way_addr[way_idx] = UINT64_MAX; }

    void begin_fill(PacketPtr packet) override;
    bool bypass(PacketPtr packet, uint64_t block_addr) override;
    bool evict_only_kept() const override { return true; }

    uint64_t get_counter_value(uint64_t way_idx);

    uint64_t count_distance(uint64_t threshold);

    void repartition_ways(uint64_t num_ways_to_reserve);

    uint64_t get_reserved_ways() {return reserved_ways;}

    bool can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) {return true;}

    void set_breakdown(uint64_t old_block_size, uint64_t new_block_size);

    template<typename VecType>
    void breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool incr);

    void set_contract(uint64_t way_idx, uint64_t size);

    void set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways);

    template<typename VecType>
    void contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks);
};

#endif
