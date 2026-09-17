#include "phru_replacement_policy.h"
#include "defs.h"
#include <fmt/core.h>


void PHRU::init_counter(PacketPtr /*packet*/) {
    lru_counter++;
    mru_counter+=num_ways;
}

void PHRU::record_heuristic(bool predicted_hub, bool actual_hub) {
    if (predicted_hub && actual_hub) heur_tp++;
    else if (predicted_hub && !actual_hub) heur_fp++;
    else if (!predicted_hub && actual_hub) heur_fn++;
    else heur_tn++;
}

void PHRU::print_heuristic_stats() {
    uint64_t total = heur_tp + heur_fp + heur_fn + heur_tn;
    if (total == 0) return;
    double precision = (heur_tp + heur_fp) > 0 ? (double)heur_tp / (heur_tp + heur_fp) : 0.0;
    double recall = (heur_tp + heur_fn) > 0 ? (double)heur_tp / (heur_tp + heur_fn) : 0.0;
    double accuracy = (double)(heur_tp + heur_tn) / total;
    fmt::print("PHRU heuristic quality: TP {} FP {} FN {} TN {} | precision {:4f} recall {:4f} accuracy {:4f}\n",
        heur_tp, heur_fp, heur_fn, heur_tn, precision, recall, accuracy);

    uint64_t fn_total = 0;
    for (auto v : fn_l1hits_hist) fn_total += v;
    uint64_t tn_total = 0;
    for (auto v : tn_l1hits_hist) tn_total += v;
    if (fn_total == 0 && tn_total == 0) return;

    fmt::print("PHRU FN pattern (fill-time misses only, {} of them): both_zero {} ({:4f}) l1hits_gt_footprint {} ({:4f})\n",
        fn_total, fn_both_zero, fn_total ? (double)fn_both_zero/fn_total : 0.0, fn_l1hits_gt_footprint, fn_total ? (double)fn_l1hits_gt_footprint/fn_total : 0.0);
    fmt::print("PHRU FN l1_hits histogram [0,1,2,3,4-7,8-15,16-31,32+]: {} {} {} {} {} {} {} {}\n",
        fn_l1hits_hist[0], fn_l1hits_hist[1], fn_l1hits_hist[2], fn_l1hits_hist[3],
        fn_l1hits_hist[4], fn_l1hits_hist[5], fn_l1hits_hist[6], fn_l1hits_hist[7]);
    fmt::print("PHRU FN footprint_count histogram [0,1,2,3,4,5,6,7,8]: {} {} {} {} {} {} {} {} {}\n",
        fn_footprint_hist[0], fn_footprint_hist[1], fn_footprint_hist[2], fn_footprint_hist[3], fn_footprint_hist[4],
        fn_footprint_hist[5], fn_footprint_hist[6], fn_footprint_hist[7], fn_footprint_hist[8]);

    fmt::print("PHRU TN pattern (fill-time misses only, {} of them): both_zero {} ({:4f}) l1hits_gt_footprint {} ({:4f})\n",
        tn_total, tn_both_zero, tn_total ? (double)tn_both_zero/tn_total : 0.0, tn_l1hits_gt_footprint, tn_total ? (double)tn_l1hits_gt_footprint/tn_total : 0.0);
    fmt::print("PHRU TN l1_hits histogram [0,1,2,3,4-7,8-15,16-31,32+]: {} {} {} {} {} {} {} {}\n",
        tn_l1hits_hist[0], tn_l1hits_hist[1], tn_l1hits_hist[2], tn_l1hits_hist[3],
        tn_l1hits_hist[4], tn_l1hits_hist[5], tn_l1hits_hist[6], tn_l1hits_hist[7]);
    fmt::print("PHRU TN footprint_count histogram [0,1,2,3,4,5,6,7,8]: {} {} {} {} {} {} {} {} {}\n",
        tn_footprint_hist[0], tn_footprint_hist[1], tn_footprint_hist[2], tn_footprint_hist[3], tn_footprint_hist[4],
        tn_footprint_hist[5], tn_footprint_hist[6], tn_footprint_hist[7], tn_footprint_hist[8]);
}

void PHRU::record_pattern(bool is_fn, uint64_t l1_hits, uint64_t footprint_count) {
    uint64_t& both_zero = is_fn ? fn_both_zero : tn_both_zero;
    uint64_t& gt_footprint = is_fn ? fn_l1hits_gt_footprint : tn_l1hits_gt_footprint;
    uint64_t* l1hits_hist = is_fn ? fn_l1hits_hist : tn_l1hits_hist;
    uint64_t* footprint_hist = is_fn ? fn_footprint_hist : tn_footprint_hist;

    if (l1_hits == 0 && footprint_count == 0) both_zero++;
    if (l1_hits > footprint_count) gt_footprint++;

    uint64_t l1_bucket;
    if (l1_hits <= 3) l1_bucket = l1_hits;
    else if (l1_hits < 8) l1_bucket = 4;
    else if (l1_hits < 16) l1_bucket = 5;
    else if (l1_hits < 32) l1_bucket = 6;
    else l1_bucket = 7;
    l1hits_hist[l1_bucket]++;

    footprint_hist[std::min(footprint_count, (uint64_t)8)]++;
}

void PHRU::hit_update(PacketPtr packet, uint64_t way_idx) {
    auto is_hub_node = (packet->block_serviced_from_llc[way_idx] >= hub_threshold);
    record_heuristic(is_hub_node, packet->degree > (uint64_t)packet->avg_degree);
    if (low_priority[way_idx] && is_hub_node) {
        low_priority[way_idx] = false;
    }

    if (!low_priority[way_idx]) {
        counter[way_idx] = mru_counter;
    } else {
        counter[way_idx] = lru_counter;
    }
}

void PHRU::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    auto is_hub_node = (packet->block_serviced_from_llc[block_idx] >= hub_threshold);
    record_heuristic(is_hub_node, packet->is_hub_node);
    // Diagnostic only, doesn't affect replacement: when the heuristic
    // correctly/incorrectly says non-hub, log l1_hits/footprint_count so we
    // can compare the FN distribution against the TN distribution - if
    // they match, that profile can't be discriminating anything.
    if (!is_hub_node) {
        record_pattern(packet->is_hub_node, packet->l1_hits, count_footprint(packet->footprint));
    }
    if (packet->serviced_from_llc > 0) {
        if (is_hub_node && was_accessed) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
        } else if (is_hub_node && !was_accessed) {
            counter[way_idx] = mru_counter;
            low_priority[way_idx] = false;
        } else if (was_accessed) {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
        } else {
            counter[way_idx] = lru_counter;
            low_priority[way_idx] = true;
        }
    } else {
        counter[way_idx] = lru_counter;
        low_priority[way_idx] = true;
    }
}

uint64_t PHRU::get_eviction_candidate(bool /*is_low_priority = false*/) {
    auto way = std::min_element(counter.begin(), std::next(counter.begin(), num_ways));
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}

uint64_t PHRU::get_reserved_eviction_candidate(bool /*is_low_priority = false*/) {
    assert(reserved_ways != 0);
    auto way = std::min_element(std::next(counter.begin(), num_ways), counter.end());
    uint64_t way_idx = std::distance(counter.begin(), way);
    return way_idx;
}


void PHRU::evict(uint64_t way_idx) {
    counter[way_idx] = max_counter;
    low_priority[way_idx] = true;
}

uint64_t PHRU::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t PHRU::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void PHRU::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

bool PHRU::can_insert(PacketPtr /*packet*/, uint64_t /*idx*/) {
    return true;
}

template<typename VecType>
void PHRU::breakdown(std::vector<VecType>& vec, uint64_t prev_num_ways, uint64_t scale_factor, bool /*incr*/) {
    vec.resize(prev_num_ways*scale_factor);
    for (size_t i = prev_num_ways; i-- > 0; ) {
        VecType val = vec[i];
        size_t baseIdx = i * scale_factor;
        for (size_t j = 0; j < scale_factor; ++j) {
            vec[baseIdx + j] = val;
        }
    }
}

void PHRU::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    assert(old_block_size != new_block_size);
    uint64_t scale_factor = old_block_size/new_block_size;
    uint64_t new_num_ways = num_ways*scale_factor;

    // First, resize all the vectors to break down their contents
    breakdown(counter, num_ways, scale_factor, false);
    breakdown(low_priority, num_ways, scale_factor, false);

    num_ways = new_num_ways;
    lru_counter = mru_counter;
}

template<typename VecType>
void PHRU::contract(std::vector<VecType>& vec, uint64_t way_idx, uint64_t num_blocks) {
    assert(way_idx + num_blocks <= vec.size());
    auto start_it = vec.begin() + way_idx;
    auto end_it = vec.begin() + (way_idx + num_blocks);
    vec.erase(start_it, end_it);
}

void PHRU::set_contract(uint64_t way_idx, uint64_t size) {
    // First, resize all the vectors to break down their contents
    contract(counter, way_idx, size);
    contract(low_priority, way_idx, size);
    num_ways -= size;
}

void PHRU::set_merge(uint64_t /*old_block_size*/, uint64_t /*new_block_size*/, uint64_t new_num_ways) {
    counter.assign(new_num_ways, max_counter);
    low_priority.assign(new_num_ways, true);
    num_ways = new_num_ways;
    lru_counter = mru_counter;
}
