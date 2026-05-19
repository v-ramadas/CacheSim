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
    uint64_t serviced_from_llc;
    uint64_t l1_hits = 0;
    float reuse_probability = 0.0;
    uint64_t reuse_distance = 0;
    uint64_t next_reuse=0;
    uint64_t shct_value=0;
    uint64_t degree=0;
    float avg_degree=0.0f;

    Packet() {
        clear_pc();
        clear();
    }

    void clear() {
        address = 0;
        aligned_address = 0;
        size = 0;
        blocks.clear();
        is_read = false;
        is_low_reuse = false;
        is_hub_node = false;
        serviced_from_llc = 0;
        footprint = 0;
        reuse_probability = 0.0;
        reuse_distance = 0;
        next_reuse = 0;
        l1_hits = 0;
        degree = 0;
        avg_degree = 0.0f;
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
        is_hub_node = false;
        footprint = 0;
    }

    Packet& operator=(const Packet &packet) {
        pc = packet.pc;
        address = packet.address;
        aligned_address = packet.aligned_address;
        size = packet.size;
        blocks = packet.blocks;
        is_read = packet.is_read;
        is_low_reuse = packet.is_low_reuse;
        is_hub_node = packet.is_hub_node;
        serviced_from_llc = packet.serviced_from_llc;
        footprint = packet.footprint;
        reuse_probability = packet.reuse_probability;
        next_reuse = packet.next_reuse;
        l1_hits = packet.l1_hits;
        shct_value = packet.shct_value;
        degree = packet.degree;
        avg_degree = packet.avg_degree;

        return *this;
    }
};

typedef Packet *PacketPtr;

#endif
