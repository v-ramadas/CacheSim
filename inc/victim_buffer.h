#ifndef __VICTIM_BUFFER_H__
#define __VICTIM_BUFFER_H__

#include <stdint.h>
#include <vector>
#include "utils.h"

struct VictimBufferEntry {
    bool valid = false;
    Packet packet;
    uint64_t bitmask;
    uint64_t bits_per_block;
    uint64_t block_size;
};

struct VictimBuffer {
    std::map<uint64_t, VictimBufferEntry> entries;

    void insert(PacketPtr packet, bool valid, uint64_t blockSize) {
        assert(entries.find(packet->aligned_address) == entries.end());

        VictimBufferEntry entry;
        entry.valid = valid;
        entry.packet = *packet;
        entries[packet->aligned_address] = entry;
        entry.block_size = blockSize;
        entry.bits_per_block = entry.block_size/8;
        entry.bitmask = (1ULL << entry.bits_per_block) - 1;

    }
    void insertPartialMiss(PacketPtr packet, uint64_t blockSize) {
        insert(packet, false, blockSize);
    }
    void insertEviction(PacketPtr packet, uint64_t blockSize) {
        insert(packet, true, blockSize);
    }

    PacketPtr fillReturn(PacketPtr packet) {
        auto entry = entries.find(packet->aligned_address);
        if (entry == entries.end()) {
            return packet;
        }

        auto block_idx = (packet->address - packet->aligned_address) >> entry->second.bits_per_block;
        packet->footprint = entry->second.packet.footprint | (entry->second.bitmask << block_idx);
        return packet;
    }
};

#endif
