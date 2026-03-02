#ifndef __MRC_H__
#define __MRC_H__

#include <algorithm>
#include <list>
#include <vector>
#include <cstdint>

class MRC {
    // MRC
    std::list <uint64_t> lru_stack;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> stack_pos;
    std::vector<uint64_t> distance_counts;
    uint64_t max_size;
    uint64_t total_accesses;

    public:
    MRC(): max_size(0), total_accesses(0) {}
    MRC(uint64_t size): max_size(size), total_accesses(0) {
      // MRC Data Structrues
      distance_counts.resize(max_size);
      std::fill(distance_counts.begin(), distance_counts.end(), 0);
    }

    void handle_fill(uint64_t address, uint64_t num_blocks, uint64_t block_size);
    void insert_block(uint64_t address);
    void handle_hit(std::unordered_map<uint64_t, std::list<uint64_t>::iterator>::iterator it_map);
    bool mattson_stack_distance_algorithm(uint64_t address, uint64_t block_size);
    void print_mpki_curve(uint64_t inst_count) const;
};

#endif
