#ifndef __PACKET_H_
#define _PACKET_H_

struct Packet {
    uint64_t pc;
    uint64_t address;
    uint64_t aligned_address;
    uint64_t size;
    std::vector<uint64_t> blocks;
    bool is_read;
    bool is_sparse;
    uint64_t footprint;

    Packet() {}
    void clear() {
        pc = 0;
        address = 0;
        aligned_address = 0;
        size = 0;
        blocks.clear();
        is_read = false;
        is_sparse = false;
        footprint = 0;
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