#ifndef __PACKET_H__
#define __PACKET_H__

#include <stdint.h>
#include <vector>
struct Packet {
    uint64_t pc;
    uint64_t address;
    uint64_t aligned_address;
    uint64_t size;
    std::vector<uint64_t> blocks;
    bool is_read;
    bool is_sparse;
    uint64_t footprint;
    bool serviced_from_llc;
    float reuse_probability = 0.0;
    uint64_t reuse_distance = 0;
    uint64_t next_reuse=0;

    Packet() {
        clear_pc();
        clear();
    }
    void clear() {
        //pc = 0;
        address = 0;
        aligned_address = 0;
        size = 0;
        blocks.clear();
        is_read = false;
        is_sparse = false;
        serviced_from_llc = false;
        footprint = 0;
        reuse_probability = 0.0;
        reuse_distance = 0;
        next_reuse = 0;
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
        is_sparse = false;
        footprint = 0;
    }
};

typedef Packet *PacketPtr;

#endif
