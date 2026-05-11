#ifndef __UTILS_H__
#define __UTILS_H__

#include <stdint.h>
#include <vector>
struct Packet {
    uint64_t pc;
    uint64_t address;
    uint64_t aligned_address;
    uint64_t size;
    std::vector<uint64_t> blocks;
    bool is_read;
    bool is_low_reuse;
    bool is_hub_node;
    uint64_t footprint;
    //std::vector<uint64_t> block_accesses;
    uint64_t serviced_from_llc;
    uint64_t l1_hits = 0;
    float reuse_probability = 0.0;
    uint64_t reuse_distance = 0;
    uint64_t next_reuse=0;
    uint64_t shct_value=0;

    Packet() {
        clear_pc();
        clear();
        //block_accesses.resize(8, 0);
    }
    void clear() {
        //pc = 0;
        address = 0;
        aligned_address = 0;
        size = 0;
        blocks.clear();
        is_read = false;
        is_low_reuse = false;
        is_hub_node = true;
        serviced_from_llc = 0;
        footprint = 0;
        reuse_probability = 0.0;
        reuse_distance = 0;
        next_reuse = 0;
        l1_hits = 0;
        //block_accesses.clear();
    }


    void clear_pc() {
        pc = 0;
    }

    void clear_address() {
        address = 0;
        aligned_address = 0;
        size = 0;
        blocks.clear();
    }

    void clear_metadata() {
        is_read = false;
        is_low_reuse = false;
        is_hub_node = true;
        footprint = 0;
    }
};

typedef Packet *PacketPtr;

#endif
