#ifndef __SIMULATE_H__
#define __SIMULATE_H__

#include "cachesim.h"
#include "predictor.h"
#include "utils.h"
#include "victim_buffer.h"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <CLI/CLI.hpp>
#include <list>

#include "tracereader.h"

#ifdef MULTI_LEVEL
void access_multi_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            SparsityPredictor* predictor, uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree);
#else
void access_single_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            uint64_t pc, uint64_t address, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree);
#endif

void useAddressTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters);

void useInstructionTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters);

void useChampsimTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters);

#endif
