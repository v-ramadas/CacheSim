#ifndef __HUB_HAWKEYE_REPLACEMENT_POLICY_H__
#define __HUB_HAWKEYE_REPLACEMENT_POLICY_H__

#include "hawkeye_replacement_policy.h"
#include <unordered_map>

// HubHawkeye's block predictor: one saturating counter per block address (no
// hashing, so no aliasing), same range, initial value and threshold as the
// SHCT. A fill is predicted cache-friendly only if its PC (SHCT) AND its block
// are friendly.
class HawkeyeBlockPredictor {
    std::unordered_map<uint64_t, uint8_t> table;   // block number -> counter

    uint8_t& counter(uint64_t block_addr, uint64_t block_shift) {
        return table.try_emplace(block_addr >> block_shift, (1 + MAX_SHCT) / 2).first->second;
    }

    public:
    void increment(uint64_t block_addr, uint64_t block_shift) {
        auto& c = counter(block_addr, block_shift);
        if (c < MAX_SHCT) c++;
    }
    void decrement(uint64_t block_addr, uint64_t block_shift) {
        auto& c = counter(block_addr, block_shift);
        if (c > 0) c--;
    }
    bool get_prediction(uint64_t block_addr, uint64_t block_shift) {
        return counter(block_addr, block_shift) >= (MAX_SHCT + 1) / 2;
    }
    size_t num_tracked() const { return table.size(); }
};

// Hawkeye plus a per-block-address predictor, so it can learn which data
// (hub vertices) to keep rather than only which load PCs reuse. A fill is
// cache-friendly only if the PC (SHCT) and the block are both friendly
// (PC-gated). The block table is trained
//  - in sampled sets, with OPTgen's label for the sampled block (as the PC);
//  - in all sets, up when a block is refilled after being serviced from the
//    LLC, down when a cache-friendly line is evicted as the LRU victim.
class HubHawkeye : public Hawkeye {
    inline static HawkeyeBlockPredictor* block_predictor;
    std::vector<uint64_t> block_addrs;   // block address held by each way

    inline static uint64_t stat_fills = 0, stat_pc_friendly = 0, stat_block_veto = 0, stat_friendly = 0;
    inline static uint64_t stat_tp = 0, stat_fp = 0, stat_fn = 0, stat_tn = 0;

    void train_sampled(uint64_t paddr, bool friendly) override;
    bool predict(uint64_t way_idx, uint64_t block_idx, uint64_t paddr, PacketPtr packet, bool pc_prediction) override;
    void lru_evicted(uint64_t way_idx) override;

    public:
    static void print_stats();
    HubHawkeye(uint64_t _set_idx, uint64_t _num_ways, CacheLevel _level) : Hawkeye(_set_idx, _num_ways, _level) {
        block_addrs.resize(num_ways, UINT64_MAX);
        if (block_predictor == nullptr) block_predictor = new HawkeyeBlockPredictor();
    }

    void evict(uint64_t way_idx) override;
    void set_breakdown(uint64_t old_block_size, uint64_t new_block_size) override;
    void set_contract(uint64_t way_idx, uint64_t size) override;
    void set_merge(uint64_t old_block_size, uint64_t new_block_size, uint64_t new_num_ways) override;
};

#endif
