#include "hub_hawkeye_replacement_policy.h"
#include <fmt/core.h>

void HubHawkeye::print_stats() {
    uint64_t pos = stat_tp + stat_fp, hubs = stat_tp + stat_fn, tot = stat_tp + stat_fp + stat_fn + stat_tn;
    fmt::print("HubHawkeye fills {} | PC friendly {} ({:.2f}%) | block veto {} ({:.2f}% of PC-friendly) | final friendly {} ({:.2f}%)\n",
        stat_fills, stat_pc_friendly, stat_fills ? 100.0 * stat_pc_friendly / stat_fills : 0.0,
        stat_block_veto, stat_pc_friendly ? 100.0 * stat_block_veto / stat_pc_friendly : 0.0,
        stat_friendly, stat_fills ? 100.0 * stat_friendly / stat_fills : 0.0);
    fmt::print("HubHawkeye prediction vs degree hub label: TP {} FP {} FN {} TN {} | precision {:.4f} recall {:.4f} accuracy {:.4f}\n",
        stat_tp, stat_fp, stat_fn, stat_tn, pos ? (double)stat_tp / pos : 0.0, hubs ? (double)stat_tp / hubs : 0.0,
        tot ? (double)(stat_tp + stat_tn) / tot : 0.0);
    if (block_predictor) {
        auto& t = *block_predictor;
        fmt::print("HubHawkeye block table: {} entries ({} sets x {} ways, {} b/entry) = {:.2f} KB | Hawkeye predictor budget "
                   "{:.2f} KB (SHCT {:.2f} + sampler {:.2f} + OPTgen {:.2f}) -> HubHawkeye total {:.2f} KB = {:.2f}x Hawkeye\n",
            t.num_entries(), t.num_sets(), HawkeyeBlockPredictor::WAYS, HawkeyeBlockPredictor::ENTRY_BITS, t.storage_bits() / 8192.0,
            hawkeye_budget.total() / 8192.0, hawkeye_budget.shct / 8192.0, hawkeye_budget.sampler / 8192.0, hawkeye_budget.optgen / 8192.0,
            (hawkeye_budget.total() + t.storage_bits()) / 8192.0,
            hawkeye_budget.total() ? (double)(hawkeye_budget.total() + t.storage_bits()) / hawkeye_budget.total() : 0.0);
        fmt::print("HubHawkeye block counters: {} valid | lookups+training hits {} misses {} | allocations {} evictions {}\n",
            t.num_tracked(), t.hits, t.misses, t.allocations, t.evictions);
    }
}

void HubHawkeye::train_sampled(uint64_t paddr, bool friendly) {
    if (friendly)
        block_predictor->increment(paddr, block_shift());
    else
        block_predictor->decrement(paddr, block_shift());
}

bool HubHawkeye::predict(uint64_t way_idx, uint64_t block_idx, uint64_t paddr, PacketPtr packet, bool pc_prediction) {
    // Exclusive-hierarchy reuse signal: this block is being refilled after the
    // LLC serviced it.
    bool block_serviced = (block_idx < packet->block_serviced_from_llc.size())
                              ? packet->block_serviced_from_llc[block_idx] > 0 : packet->serviced_from_llc > 0;
    if (block_serviced)
        block_predictor->increment(paddr, block_shift());
    bool prediction = pc_prediction && block_predictor->get_prediction(paddr, block_shift());
    block_addrs[way_idx] = paddr;

    stat_fills++;
    if (pc_prediction) stat_pc_friendly++;
    if (pc_prediction && !prediction) stat_block_veto++;
    if (prediction) stat_friendly++;
    if (prediction && packet->is_hub_node) stat_tp++;
    else if (prediction) stat_fp++;
    else if (packet->is_hub_node) stat_fn++;
    else stat_tn++;
    return prediction;
}

void HubHawkeye::lru_evicted(uint64_t way_idx) {
    // Like the PC, detrained only when a line it kept is evicted unreused
    // (maxRRPV evictions were already predicted averse); unlike the PC, in all sets.
    if (block_addrs[way_idx] != UINT64_MAX)
        block_predictor->decrement(block_addrs[way_idx], block_shift());
}

void HubHawkeye::evict(uint64_t way_idx) {
    Hawkeye::evict(way_idx);
    block_addrs[way_idx] = UINT64_MAX;
}

void HubHawkeye::set_breakdown(uint64_t old_block_size, uint64_t new_block_size) {
    uint64_t prev_num_ways = num_ways;
    Hawkeye::set_breakdown(old_block_size, new_block_size);
    breakdown(block_addrs, prev_num_ways, old_block_size / new_block_size, false);
}

void HubHawkeye::set_contract(uint64_t way_idx, uint64_t size) {
    Hawkeye::set_contract(way_idx, size);
    contract(block_addrs, way_idx, size);
}

void HubHawkeye::set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways) {
    Hawkeye::set_merge(old_block_size, new_block_size, new_num_ways);
    block_addrs.assign(new_num_ways, UINT64_MAX);
}
