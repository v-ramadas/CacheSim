#include "belady_replacement_policy.h"
#include "trace_file.h"
#include <fmt/core.h>
#include <cstdio>

// ---------------------------------------------------------------- next-use oracle
static inline uint64_t align_block(uint64_t a, uint64_t size) { return a / size * size; }

bool Belady::prescan(const std::string& trace_path) {
    // Same line numbering as useInstructionTrace (TRACE_LINE = index of every getline line).
    TraceFile trace(trace_path);
    if (!trace.is_open()) return false;
    std::string line;
    char action[16];
    uint64_t inst, pc, addr;
    uint64_t idx = 0;
    while (trace.getline(line)) {
        oracle.next_line.append(1, BeladyOracle::NONE);
        if (std::sscanf(line.c_str(), "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx", &inst, &pc, action, &addr) == 4) {
            uint64_t s = oracle.slot(addr >> 3, true);
            if (oracle.last[s] == BeladyOracle::NONE) {                            // first access
                oracle.next_use[s] = (uint32_t)idx;
                oracle.words++;
            }
            else oracle.next_line[oracle.last[s]] = (uint32_t)idx;
            oracle.last[s] = (uint32_t)idx;
        }
        idx++;
    }
    uint64_t prescan_peak = oracle.bytes();
    if (idx >= UINT32_MAX) {
        fmt::print(stderr, "Belady: trace has {} lines, more than the oracle's 32-bit line indices\n", idx);
        return false;
    }
    oracle.last.clear();   // prescan-only
    oracle_active = true;
    fmt::print("Belady: prescan peak {:.2f} GB\n", prescan_peak / double(1ULL << 30));
    fmt::print("Belady: next-use oracle from {} trace lines, {} distinct 8B words in {} 4KB pages ({:.2f} GB)\n",
        idx, oracle.words, oracle.pages, oracle.bytes() / double(1ULL << 30));
    return true;
}

uint64_t Belady::block_next_use(uint64_t block_addr) {
    // Earliest next access to any 8B word of the block (UINT64_MAX: never again).
    uint64_t best = UINT64_MAX;
    for (uint64_t w = block_addr >> 3; w < (block_addr + block_size) >> 3; w++) {
        uint64_t s = oracle.slot(w, false);
        if (s != UINT64_MAX && oracle.next_use[s] != BeladyOracle::NONE) best = std::min<uint64_t>(best, oracle.next_use[s]);
    }
    return best;
}

void Belady::print_stats() {
    uint64_t tot = stat_bypassed + stat_inserted;
    fmt::print("Belady (OPT, next use from {}): blocks offered to the LLC {} | inserted {} | bypassed {} ({:.2f}%)\n",
        oracle_active ? "the 8B oracle" : "the trace's 64B NextReuse", tot, stat_inserted, stat_bypassed,
        tot ? 100.0 * stat_bypassed / tot : 0.0);
}

// ---------------------------------------------------------------- policy
void Belady::init_counter(PacketPtr /*packet*/) {
}

uint64_t Belady::way_next_use(uint64_t w) {
    if (oracle_active && counter[w] <= cachesim::TRACE_LINE) counter[w] = block_next_use(way_addr[w]);
    return counter[w];
}

void Belady::hit_update(PacketPtr packet, uint64_t way_idx) {
    // The block's next use is now the one after this access (the stored one was this access).
    counter[way_idx] = oracle_active ? block_next_use(way_addr[way_idx]) : packet->next_reuse;
}

void Belady::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool /*was_accessed*/) {
    uint64_t addr = (block_idx < packet->blocks.size() && packet->blocks[block_idx] != UINT64_MAX)
                        ? packet->blocks[block_idx] : align_block(packet->address, block_size);
    way_addr[way_idx] = align_block(addr, block_size);
    if (!oracle_active) counter[way_idx] = packet->next_reuse;
    else if (way_addr[way_idx] == pending_addr && pending_line == cachesim::TRACE_LINE) counter[way_idx] = pending_nu;
    else counter[way_idx] = block_next_use(way_addr[way_idx]);
    pending_addr = UINT64_MAX;
    stat_inserted++;
}

void Belady::begin_fill(PacketPtr /*packet*/) {
    fill_set.clear();
    fill_free = 0;
    for (uint64_t w = 0; w < num_ways; w++) {
        if (way_addr[w] == UINT64_MAX) fill_free++;
        else fill_set.push_back(way_next_use(w));
    }
}

bool Belady::bypass(PacketPtr packet, uint64_t block_addr) {
    if (fill_free) { fill_free--; return false; }          // free way: inserting never hurts
    // Oracle (sub-64B blocks): the block's exact next use; otherwise the trace's per-64B NextReuse the fill carries
    // (refreshed by the L1D on every hit, so it is the line's next use after its last access).
    uint64_t nu = packet->next_reuse;
    if (oracle_active) {
        pending_addr = align_block(block_addr, block_size);
        nu = pending_nu = block_next_use(pending_addr);
        pending_line = cachesim::TRACE_LINE;
    }
    auto far = std::max_element(fill_set.begin(), fill_set.end());
    if (far == fill_set.end() || nu >= *far) {             // it would be the farthest: OPT does not keep it
        stat_bypassed++;
        return true;
    }
    *far = nu;                                             // kept; the farthest resident goes instead
    return false;
}

uint64_t Belady::get_eviction_candidate(bool /*is_low_priority=false*/) {
    {
        uint64_t victim = 0, far = 0;
        bool any = false;
        for (uint64_t w = 0; w < num_ways; w++) {
            if (way_addr[w] == UINT64_MAX) continue;
            uint64_t nu = way_next_use(w);
            if (!any || nu > far) { far = nu; victim = w; any = true; }
        }
        if (any) return victim;
    }
    auto way = std::max_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t Belady::get_reserved_eviction_candidate(bool /*is_low_priority = false*/) {
    assert(reserved_ways != 0);
    auto way = std::max_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

void Belady::evict(uint64_t way_idx) {
    counter[way_idx] = 0;
    way_addr[way_idx] = UINT64_MAX;
}

uint64_t Belady::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t Belady::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void Belady::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

template<typename VecType>
void Belady::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool /*incr*/) {
    vec.resize(prev_num_ways*scale_factor);
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            vec[baseIdx + j] = val;
        }
    }
}

void Belady::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    assert(old_block_size != new_block_size);
    uint64_t scale_factor = old_block_size/new_block_size;
    uint64_t new_num_ways = num_ways*scale_factor;

    breakdown(counter, num_ways, scale_factor, false);
    way_addr.assign(new_num_ways, UINT64_MAX);   // Belady never duels; keep the vector sized

    num_ways = new_num_ways;
}

template<typename VecType>
void Belady::contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks) {
    assert(way_idx + num_blocks <= vec.size());
    auto start_it = vec.begin() + way_idx;
    auto end_it = vec.begin() + (way_idx + num_blocks);
    vec.erase(start_it, end_it);
}

void Belady::set_contract(uint64_t way_idx, uint64_t size) {
    contract(counter, way_idx, size);
    contract(way_addr, way_idx, size);
    num_ways -= size;
}

void Belady::set_merge(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/, uint64_t new_num_ways) {
    // No sound way to merge per-way next-reuse state across a group of small
    // blocks, so reset it for the new (larger-block) geometry instead.
    counter.assign(new_num_ways, 0);
    way_addr.assign(new_num_ways, UINT64_MAX);
    num_ways = new_num_ways;
}
