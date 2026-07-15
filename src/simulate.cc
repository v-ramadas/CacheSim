#include "simulate.h"


#ifdef MULTI_LEVEL
void access_multi_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            SparsityPredictor* predictor, uint64_t address, uint64_t pc, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree) {

    access_packet->clear();
    eviction_packet->clear();
    fill_packet->clear();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache[0]->get_block_size(cache[0]->get_set_idx(access_packet->address));
    access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
    access_packet->pc = pc;
    access_packet->next_reuse = next_reuse;
    access_packet->degree = degree;
    access_packet->avg_degree = avg_degree;

    *fill_packet = *access_packet;
    fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    *eviction_packet = *fill_packet;
    *invalidation_packet = *fill_packet;

    int num_levels = cache.size();
    int hit_at_level = num_levels;
    bool hit = false;

    // Check for hits
    for (int i = 0; i < num_levels; i++) {

        hit = cache[i]->try_hit(access_packet);

        if (hit) {
            hit_at_level = i;
            break;
        }

        if (access_packet->is_high_reuse) {
            access_packet->clear_blocks();
            access_packet->aligned_address = align_address(access_packet->address, CACHELINE_SIZE);
        }

    }

    if (hit) {
        eviction_packet->footprint = 0;
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        invalidation_packet->size = CACHELINE_SIZE;
    } else {
        fill_packet->size = CACHELINE_SIZE;
        eviction_packet->size = CACHELINE_SIZE;
        invalidation_packet->size = CACHELINE_SIZE;
    }

    if (hit_at_level != 0) {
        if (hit) {
            cache[hit_at_level]->handle_invalidate(invalidation_packet);
            *fill_packet = *invalidation_packet;
            fill_packet->footprint |= invalidation_packet->footprint;
            fill_packet->serviced_from_llc += 1;
            cache[0]->handle_fill_blocks(fill_packet, eviction_packet, 0);
        } else {
            cache[1]->handle_invalidate(invalidation_packet);
            if (invalidation_packet->blocks.size() != 0)
                *fill_packet = *invalidation_packet;

            cache[0]->handle_invalidate(invalidation_packet);
            if (invalidation_packet->blocks.size() != 0)
                *fill_packet = *invalidation_packet;

            fill_packet->degree = access_packet->degree;
            fill_packet->avg_degree = access_packet->avg_degree;
            cache[0]->handle_fill_line(fill_packet, eviction_packet, 0);
        }

        auto prev_cache_block_size = cache[0]->get_block_size(cache[0]->get_set_idx(fill_packet->address));
        auto curr_cache_block_size = prev_cache_block_size;
        for (int i = 1; i < num_levels; i++) {
            curr_cache_block_size = cache[i]->get_block_size(cache[i]->get_set_idx(fill_packet->address));

            if (prev_cache_block_size != curr_cache_block_size) {
                resize_packet(eviction_packet, curr_cache_block_size);       
            }


            fill_packet->l1_hits = eviction_packet->l1_hits;
            if (fill_packet->l1_hits == 0) fill_packet->l1_hits = 1;
            // Train on data movement from LLC -> L1D
            if (eviction_packet->blocks.size() > 0) {
                predictor->update_footprint(eviction_packet);
                predictor->update_access(fill_packet);
                bool is_high_reuse = false;
                //bool is_hub_node = true;
                if (cachesim::inst_count >= cachesim::WARMUP_INSTRUCTIONS) {
                    // Predict for data movement from L1D -> LLC
                    fill_packet->is_high_reuse = predictor->predict(eviction_packet);
                }

                *fill_packet = *eviction_packet;
                fill_packet->reuse_probability = predictor->get_reuse_probability(eviction_packet);

                fill_packet->shct_value = predictor->get_shct_value(fill_packet);

                if (predictor->get_reuse_distance(eviction_packet) <= 1) {
                    fill_packet->reuse_distance = UINT64_MAX;
                } else {
                    fill_packet->reuse_distance = cachesim::inst_count + predictor->get_reuse_distance(eviction_packet);
                }
                fill_packet->next_reuse = eviction_packet->next_reuse;
                eviction_packet->clear();
                cache[i]->handle_fill_blocks(fill_packet, eviction_packet, 1);

                if (eviction_packet->blocks.size() > 0) {
                    predictor->update_eviction(eviction_packet);
                }
            } else {
                break;
            }
            eviction_packet->clear();
            eviction_packet->clear_pc();
            fill_packet->clear();
            fill_packet->clear_pc();
            prev_cache_block_size = curr_cache_block_size;
        }
    }
}
#else
void access_single_level(std::vector<BaseCache*> &cache,
            PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet,
            uint64_t pc, uint64_t address, bool is_read, uint64_t next_reuse, uint64_t degree, float avg_degree) {

    assert(cache.size() ==  1);
    access_packet->clear();
    eviction_packet->clear();
    fill_packet->clear();
    access_packet->address = address;
    access_packet->is_read = is_read;
    access_packet->size = cache[0]->get_block_size(cache[0]->get_set_idx(access_packet->address));
    access_packet->aligned_address = align_address(access_packet->address, access_packet->size);
    access_packet->pc = pc;
    access_packet->next_reuse = next_reuse;
    access_packet->degree = degree;
    access_packet->avg_degree = avg_degree;

    *fill_packet = *access_packet;
    fill_packet->aligned_address = align_address(fill_packet->address, CACHELINE_SIZE);
    fill_packet->size = CACHELINE_SIZE;
    *eviction_packet = *fill_packet;
    *invalidation_packet = *fill_packet;

    bool hit = cache[0]->try_hit(access_packet);
    if (!hit) {
        cache[0]->handle_fill_line(fill_packet, eviction_packet, 0);
    }
    return;
}
#endif

void useAddressTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters) {
    while (num_iters > 0) {
        std::ifstream file(filename);

        if (!file.is_open()) {
            std::cerr << "Error: Could not open the file!" << std::endl;
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            uint64_t pc = 0;
            uint64_t address = 0;
            uint64_t next_reuse = UINT64_MAX;
            uint64_t degree = 0;
            float avg_degree = 0.0f;
            int parsed_count = 0;
            char action[16];
            if (cachesim::DEBUG || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {
                if (cachesim::inst_count > cachesim::DEBUG_INSTRUCTIONS) {
                    break;
                }
            }

            parsed_count = std::sscanf(line.c_str(), "PC:%lu %15[^:]:0x%lx %lu %lu %f", &pc, action, &address, &next_reuse, &degree, &avg_degree);
            if (parsed_count >= 3) {
                try {
                    cachesim::inst_count++;
                    if (next_reuse != UINT64_MAX) next_reuse += cachesim::inst_count;
                    // Extract from the start of "0x" to the end of the line
                    bool is_read = (strcmp(action, "read") == 0) ? true : false;
#ifdef MULTI_LEVEL
                    access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        predictor, address, pc, is_read, next_reuse, degree, avg_degree);
#else
                    access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        pc, address, is_read, next_reuse, degree, avg_degree);
#endif

                } catch (const std::exception& e) {
                    std::cerr << "Conversion error on line: " << line << " -> " <<e.what() << std::endl;
                }
            }
        }
        file.close();
        --num_iters;
    }
    return;
}

void useInstructionTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters) {
    while (num_iters > 0) {
        std::ifstream file(filename);

        if (!file.is_open()) {
            std::cerr << "Error: Could not open the file!" << std::endl;
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            uint64_t pc = 0;
            uint64_t address = 0;
            uint64_t next_reuse = UINT64_MAX;
            uint64_t degree = 0;
            float avg_degree = 0.0f;
            int parsed_count = 0;
            char action[16];
            if (cachesim::DEBUG || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {
                if (cachesim::inst_count > cachesim::DEBUG_INSTRUCTIONS) {
                    break;
                }
            }

            parsed_count = std::sscanf(line.c_str(), 
                           "instCount:%lu,PC:0x%lx,%15[^:]:0x%lx,NextReuse:%lu,NodeDegree:%lu,PCAvgDegree:%f", 
                           &cachesim::inst_count, &pc, action, &address, &next_reuse, &degree, &avg_degree);
            if (parsed_count >= 3) {
                try {
                    // Extract from the start of "0x" to the end of the line
                    bool is_read = (strcmp(action, "Read") == 0) ? true : false;
#ifdef MULTI_LEVEL
                    access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        predictor, address, pc, is_read, next_reuse, degree, avg_degree);
#else
                    access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        pc, address, is_read, next_reuse, degree, avg_degree);
#endif
                } catch (const std::exception& e) {
                    std::cerr << "Conversion error on line: " << line << " -> " <<e.what() << std::endl;
                }
            }
        }
        file.close();
        --num_iters;
    }
    return;
}

void useChampsimTrace(std::vector<BaseCache*> cache, const std::string& filename, SparsityPredictor* predictor, PacketPtr access_packet, PacketPtr eviction_packet, PacketPtr fill_packet, PacketPtr invalidation_packet, uint64_t num_iters) {
        champsim::tracereader trace(get_tracereader(filename, 0, false, false));
        while (!trace.eof()) {
            if (cachesim::DEBUG || cachesim::L1_DEBUG || cachesim::LLC_DEBUG || cachesim::REPLACEMENT_POLICY_DEBUG) {
                if (cachesim::inst_count > 5000000) {
                    break;
                }
            }
            cachesim::inst_count++;
            auto inst = trace();
            access_packet->clear();
            eviction_packet->clear();
            fill_packet->clear();
            invalidation_packet->clear();
#ifdef MULTI_LEVEL
            for (auto& smem:inst.source_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                    predictor,smem.to<uint64_t>(), inst.ip.to<uint64_t>(), true,
                    0, 0, 0.0);
            }
            for (auto& dmem:inst.destination_memory) {
                access_multi_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                    predictor, dmem.to<uint64_t>(), inst.ip.to<uint64_t>(), false,
                    0, 0, 0.0);
            }
#else
            for (auto& smem:inst.source_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        inst.ip.to<uint64_t>(), smem.to<uint64_t>(), true, 0, 0, 0.0);
            }
            for (auto& dmem:inst.destination_memory) {
                access_single_level(cache, access_packet, eviction_packet, fill_packet, invalidation_packet,
                        inst.ip.to<uint64_t>(), dmem.to<uint64_t>(), false, 0, 0, 0.0);
            }
#endif
        }
}

