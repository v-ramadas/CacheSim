#ifndef __UTILS_H__
#define __UTILS_H__

#include <stdint.h>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include "performance_model.h"
#include "ghost_cache.h"

const uint64_t CACHELINE_SIZE = 64;

// PSEL: the original mechanism - a single saturating +-1 counter on Leader64
// vs Leader8 misses, decision re-checked live every access past DUELING_PERIOD.
// ZTEST: cumulative + windowed conditional-binomial z-test on Leader64 vs
// Leader8 miss *counts* (assumes ~equal exposure between the two leader
// groups; doesn't need per-side access counts). ZTEST_RATIO: the same
// cumulative + windowed structure, but a two-proportion z-test on miss
// *rates* (misses/accesses per side) - corrects for any exposure imbalance,
// at the cost of tracking access counts too. Both z-test modes lock in
// permanently once confidently favored.
enum class DuelingMode {
    PSEL,
    ZTEST,
    ZTEST_RATIO,
};

// Which words of an L1D victim an Amoeba-Cache LLC stores, always as one
// block: SPAN = from the first to the last touched word, untouched words in
// between included (the paper's single contiguous range per insertion);
// LINE = the whole 64B line (fixed granularity).
enum class AmoebaInsert {
    SPAN,
    LINE,
};

// What an Amoeba L1D fetches around the demanded word on a miss (always one
// contiguous run, limited to words the source holds and the L1D does not):
// HISTORY = within the span the line used in its previous L1 residency (the
// whole line on its first touch); LINE = the whole line; ORACLE = within the
// span a recorded run says this residency of the line uses.
enum class L1AmoebaFetch {
    HISTORY,
    LINE,
    ORACLE,
};

namespace cachesim {
    extern uint64_t instCount;
    extern uint64_t prevInstCount;

    extern uint64_t BLOCK_SIZE;

    extern bool DEBUG;
    extern bool DEBUG_ALL;
    extern bool L1_DEBUG;
    extern bool REPLACEMENT_POLICY_DEBUG;
    extern bool LLC_DEBUG;
    extern bool NO_ISO_AREA;
    extern bool GEN_STATS;
    extern bool dropBlocks;
    extern bool useMemSignature;
    extern bool isoArea;
    extern bool ENABLE_PREDICTOR;
    extern uint64_t START_DEBUG;
    extern uint64_t END_DEBUG;

    extern uint64_t WARMUP_INSTRUCTIONS;

    extern bool SET_DUELING;
    extern int NUM_DUELS;
    extern uint64_t PSEL_MAX;
    extern uint64_t PSEL_THRESHOLD;
    // PSEL mode: half-width of the band around PSEL_THRESHOLD in which the
    // 64B/8B decision is kept as is (default PSEL_MAX / 8; 0 = plain threshold).
    extern uint64_t PSEL_HYSTERESIS;
    extern uint64_t DUELING_PERIOD;

    extern DuelingMode DUELING_MODE;
    extern bool LOG_DUELING_METRICS;

    extern uint64_t CONF_EPOCH;
    extern uint64_t CONF_MAX;
    extern uint64_t CONF_MARGIN;
    extern double Z_THRESHOLD;

    extern uint64_t Z_WINDOW_SIZE;
    extern double WINDOW_Z_THRESHOLD;

    extern PerformanceModel performanceModel;

    extern bool USE_GHOST_CACHE;
    extern uint64_t GHOST_CACHE_SIZE;
    extern GhostCache ghostCache;
    extern bool GHOST_CACHE_DEBUG;
    extern uint64_t GHOST_CACHE_MIN_SERVICED;

    // L1D geometry (--l1-sets / --l1-ways; each way is a 64B line with 8B sectors).
    extern uint64_t L1_NUM_SETS;
    extern uint64_t L1_NUM_WAYS;

    // Optional sectored L2 between L1D and LLC (--l2).
    extern bool L2_ENABLED;
    extern uint64_t L2_NUM_SETS;
    extern uint64_t L2_NUM_WAYS;

    // Base+Delta LLC tag compression (--tag-bdi-bases B, --tag-bdi-delta d):
    // get_iso_area_cache() sizes 8B sets as if each block stored a (base id,
    // d-bit delta, sector) tag against B per-set base tags, and Set stores and
    // looks up tags in that form. Default B=8, d=10; --tag-bdi-bases 0 turns it
    // off (uncompressed tags).
    extern uint64_t TAG_BDI_BASES;
    extern uint64_t TAG_BDI_DELTA;
    // Step 2 behaviour (no admission constraint: a tag that fits no base is
    // kept in full) instead of step 3's base replacement.
    extern bool TAG_BDI_UNCONSTRAINED;
    // Print every Base+Delta encode / lookup / release / base replacement.
    extern bool DEBUG_TAG_BDI;
    extern bool DEBUG_TIMING;   // --debug-timing: per-access event times at each level
    // Amoeba-Cache LLC (--replacement-policy amoeba, 8B blocks): each set is an
    // array of 64-bit words holding variable-size blocks, each a tag word plus
    // a contiguous run of data words from one 64B line (Kumar et al., MICRO
    // 2012). AMOEBA_INSERT picks which words of an L1D victim are stored.
    extern bool AMOEBA;
    extern AmoebaInsert AMOEBA_INSERT;
    // Debug knobs: words per set (0 = from the iso-area budget), and blocks
    // that take no tag word (tags cost nothing; data words only).
    extern uint64_t AMOEBA_WORDS;
    extern bool AMOEBA_FREE_TAGS;
    // Amoeba-Cache as the L1D (--l1-amoeba, multi-level build): the L1D is an
    // Amoeba word array sized iso-area with the 64B L1D; on a miss it fetches
    // the words the line used in its previous L1 residency (all of it on the
    // first touch); the line's other words are dropped.
    extern bool L1_AMOEBA;
    extern L1AmoebaFetch L1_AMOEBA_FETCH;
    // --l1-amoeba-keep-llc: no word is lost - on an LLC hit the words the L1D
    // does not take stay in the LLC, and on a memory fill they go into it, as
    // a partial line (line-level exclusivity no longer holds; a word is still
    // in one level only).
    extern bool L1_AMOEBA_KEEP_LLC;
    // 0-based index of the trace line being simulated (the units of NextReuse).
    extern uint64_t TRACE_LINE;
};

enum class InsertionPolicy {
    EXCLUSIVE,
    INCLUSIVE,
};

// Position of a cache in the hierarchy. The LLC is always CacheLevel::LLC,
// whether or not the optional L2 is present.
enum class CacheLevel {
    L1D,
    L2,
    LLC,
};

inline const char* to_string(CacheLevel level) {
    switch (level) {
        case CacheLevel::L1D: return "L1D";
        case CacheLevel::L2:  return "L2";
        case CacheLevel::LLC: return "LLC";
    }
    return "?";
}

// Lets fmt print a CacheLevel with "{}".
inline const char* format_as(CacheLevel level) { return to_string(level); }

// Address spaces. Each core runs one application (app id). The physical address
// is the app id in bits 48-63 plus the low 48 bits of the virtual address
// (Sniper's va2pa without page randomization): threads of one application share
// addresses, different applications never do. Application 0 keeps its virtual
// addresses unchanged, so a single-application run is not affected. No page
// table or TLB is modeled.
constexpr unsigned APP_SHIFT = 48;
constexpr uint64_t VA_MASK = (uint64_t(1) << APP_SHIFT) - 1;
inline uint64_t translate(uint64_t vaddr, uint32_t app = 0) {
    return app == 0 ? vaddr : (uint64_t(app) << APP_SHIFT) | (vaddr & VA_MASK);
}
// Virtual address of a physical address made by translate() (user-space virtual addresses fit in 48 bits).
inline uint64_t virtual_of(uint64_t paddr) {
    return paddr == UINT64_MAX || (paddr >> APP_SHIFT) == 0 ? paddr : paddr & VA_MASK;
}

struct Packet {
    uint64_t pc;
    uint64_t address;            // physical address = translate(vaddr, app): caches index and tag with this
    uint64_t vaddr = 0;          // virtual address, as in the trace; kept in step by set_address()
    // Timestamp (core cycles) of this packet's next event. A demand access starts with the issuing core's clock
    // (its L1D access); each level it misses advances it by that level's lookup cycles, so before a lookup it is
    // the time of that lookup; when the access completes it is the time the data returns (= the core's clock).
    // Fill, eviction and invalidation packets copy the access's value but are not timed yet.
    uint64_t event_time = 0;
    uint64_t aligned_address;
    uint64_t size;
    std::vector<uint64_t> blocks;
    bool is_read;
    bool is_high_reuse;
    bool is_hub_node;
    uint64_t footprint;
    uint64_t serviced_from_llc;
    uint64_t l1_hits = 0;
    float reuse_probability = 0.0;
    uint64_t reuse_distance = 0;
    uint64_t next_reuse=0;
    uint64_t shct_value=0;
    uint64_t degree=0;
    std::vector<uint64_t> block_degrees;
    float avg_degree=0.0f;
    std::vector<uint64_t> block_serviced_from_llc;
    // Per block: its LLC replacement counter when it last left the LLC for
    // L1D (UINT64_MAX = none), carried through L1D/L2 back to the LLC fill.
    std::vector<uint64_t> block_prev_counter;
    // Words of the 64B line this packet's data holds (bit = 8B word); below
    // all-ones only with --l1-amoeba, whose L1D keeps and evicts partial lines.
    uint8_t present_mask = 0xff;
    bool from_ghost_cache = false;

    Packet() {
        clear_pc();
        clear();
    }
    
    Packet(const Packet &packet) = default;
     ~Packet() = default;

    // Set the (physical) address and its virtual address together.
    void set_address(uint64_t paddr) {
        address = paddr;
        vaddr = virtual_of(paddr);
    }

    void clear() {
        address = 0;
        vaddr = 0;
        aligned_address = 0;
        size = 0;
        blocks.clear();
        is_read = false;
        is_high_reuse = false;
        is_hub_node = false;
        serviced_from_llc = 0;
        footprint = 0;
        reuse_probability = 0.0;
        reuse_distance = 0;
        next_reuse = 0;
        l1_hits = 0;
        degree = 0;
        block_degrees.clear();
        avg_degree = 0.0f;
        block_serviced_from_llc.clear();
        block_prev_counter.clear();
        present_mask = 0xff;
        from_ghost_cache = false;
    }

    void clear_pc() {
        pc = 0;
    }

    void clear_address() {
        address = 0;
        vaddr = 0;
        aligned_address = 0;
        size = 0;
        blocks.clear();
        block_serviced_from_llc.clear();
        block_prev_counter.clear();
    }
    
    void clear_blocks() {
        blocks.clear();
        block_degrees.clear();
        block_serviced_from_llc.clear();
        block_prev_counter.clear();
    }

    void clear_metadata() {
        is_read = false;
        is_high_reuse = false;
        is_hub_node = false;
        footprint = 0;
    }

    void print() {
        fmt::print("Packet pc {:#x} address {:#x} vaddr {:#x} aligned_address {:#x} size {} is_read {} is_high_reuse {} is_hub_node {} footprint {:#x} serviced_from_llc {} reuse_probability {} reuse_distance {} next_reuse {} l1_hits {} degree {} avg_degree {:4f}\n",
            pc, address, vaddr, aligned_address, size, is_read, is_high_reuse, is_hub_node, footprint, serviced_from_llc, reuse_probability, reuse_distance, next_reuse, l1_hits, degree, avg_degree);
    }

    Packet& operator=(const Packet &packet) {
        pc = packet.pc;
        address = packet.address;
        vaddr = packet.vaddr;
        event_time = packet.event_time;
        aligned_address = packet.aligned_address;
        size = packet.size;
        blocks = packet.blocks;
        is_read = packet.is_read;
        is_high_reuse = packet.is_high_reuse;
        is_hub_node = packet.is_hub_node;
        serviced_from_llc = packet.serviced_from_llc;
        footprint = packet.footprint;
        reuse_probability = packet.reuse_probability;
        next_reuse = packet.next_reuse;
        l1_hits = packet.l1_hits;
        shct_value = packet.shct_value;
        degree = packet.degree;
        block_degrees = packet.block_degrees;
        avg_degree = packet.avg_degree;
        block_serviced_from_llc = packet.block_serviced_from_llc;
        block_prev_counter = packet.block_prev_counter;
        present_mask = packet.present_mask;
        from_ghost_cache = packet.from_ghost_cache;

        return *this;
    }
};

typedef Packet *PacketPtr;

#endif
