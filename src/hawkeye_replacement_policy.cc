#include "hawkeye_replacement_policy.h"
#include <fmt/core.h>

//uint64_t Hawkeye::sampled_cache_size = 2800;
//uint64_t Hawkeye::sampler_ways = 8;
//uint64_t Hawkeye::samper_sets = (Hawkeye::sampled_cache_size/Hawkeye::sampler_ways);

void Hawkeye::init_counter(PacketPtr packet) {
}

void Hawkeye::hit_update(PacketPtr packet, uint64_t way_idx) {
    if (level == 0) {
        uint64_t paddr = (packet->address >> 6) << 6;

        //If we are sampling, OPTgen will only see accesses from sampled sets
        if(is_sampled_set(set_idx)) {
        //The current timestep 
            uint64_t curr_quanta = mytimer % OPTGEN_VECTOR_SIZE;
            
            uint32_t sampler_set = (paddr >> 6) % sampler_sets; 
            uint64_t sampler_tag = HawkeyePCPredictor::CRC(paddr >> 12) % 256;
            assert(sampler_set < sampler_sets);
            
            // This line has been used before. Since the right end of a usage interval is always 
            //a demand, ignore prefetches
            if((addr_history[sampler_set].find(sampler_tag) != addr_history[sampler_set].end())) {
                unsigned int curr_timer = mytimer;
                if(curr_timer < addr_history[sampler_set][sampler_tag].last_quanta)
                   curr_timer = curr_timer + TIMER_SIZE;
                bool wrap =  ((curr_timer - addr_history[sampler_set][sampler_tag].last_quanta) > OPTGEN_VECTOR_SIZE);
                uint64_t last_quanta = addr_history[sampler_set][sampler_tag].last_quanta % OPTGEN_VECTOR_SIZE;
                //and for prefetch hits, we train the last prefetch trigger PC
                predictor->increment(addr_history[sampler_set][sampler_tag].PC);
                //Some maintenance operations for OPTgen
                optgen.add_access(curr_quanta);
                update_addr_history_lru(sampler_set, addr_history[sampler_set][sampler_tag].lru);
            } else if(addr_history[sampler_set].find(sampler_tag) == addr_history[sampler_set].end()) {
                // This is the first time we are seeing this line
                // Find a victim from the sampled cache if we are sampling
                if(addr_history[sampler_set].size() == sampler_ways) 
                    replace_addr_history_element(sampler_set);

                assert(addr_history[sampler_set].size() < sampler_ways);
                //Initialize a new entry in the sampler
                addr_history[sampler_set][sampler_tag].init(curr_quanta);
                //If it's a prefetch, mark the prefetched bit;
                optgen.add_access(curr_quanta);
                update_addr_history_lru(sampler_set, sampler_ways-1);
            }
            // Get Hawkeye's prediction for this line
            bool new_prediction = predictor->get_prediction (packet->pc);
            // Update the sampler with the timestamp, PC and our prediction
            // For prefetches, the PC will represent the trigger PC
            addr_history[sampler_set][sampler_tag].update(mytimer, packet->pc, new_prediction);
            addr_history[sampler_set][sampler_tag].lru = 0;
            //Increment the set timer
            mytimer = (mytimer+1) % TIMER_SIZE;
        }
        counter[way_idx] = 0;
    }
}

void Hawkeye::fill_update(uint64_t way_idx, uint64_t block_idx, PacketPtr packet, bool was_accessed) {
    uint64_t paddr = (packet->address >> 6) << 6;

    //Ignore writebacks
    //if (!packet->is_read) {
    //    return;
    //}


    //If we are sampling, OPTgen will only see accesses from sampled sets
    if(is_sampled_set(set_idx)) {
        //The current timestep 
        uint64_t curr_quanta = mytimer % OPTGEN_VECTOR_SIZE;

        uint32_t sampler_set = (paddr >> 6) % sampler_sets; 
        uint64_t sampler_tag = HawkeyePCPredictor::CRC(paddr >> 12) % 256;
        assert(sampler_set < sampler_sets);

        // This line has been used before. Since the right end of a usage interval is always 
        //a demand, ignore prefetches
        if((addr_history[sampler_set].find(sampler_tag) != addr_history[sampler_set].end())) {
            unsigned int curr_timer = mytimer;
            if(curr_timer < addr_history[sampler_set][sampler_tag].last_quanta)
               curr_timer = curr_timer + TIMER_SIZE;
            bool wrap =  ((curr_timer - addr_history[sampler_set][sampler_tag].last_quanta) > OPTGEN_VECTOR_SIZE);
            uint64_t last_quanta = addr_history[sampler_set][sampler_tag].last_quanta % OPTGEN_VECTOR_SIZE;
            //and for prefetch hits, we train the last prefetch trigger PC
            if(packet->serviced_from_llc > 0 || ( !wrap && optgen.should_cache(curr_quanta, last_quanta))) {
                predictor->increment(addr_history[sampler_set][sampler_tag].PC);
            } else {
                predictor->decrement(addr_history[sampler_set][sampler_tag].PC);
            }
            //Some maintenance operations for OPTgen
            optgen.add_access(curr_quanta);
            update_addr_history_lru(sampler_set, addr_history[sampler_set][sampler_tag].lru);
        } else if(addr_history[sampler_set].find(sampler_tag) == addr_history[sampler_set].end()) {
            // This is the first time we are seeing this line
            // Find a victim from the sampled cache if we are sampling
            if(addr_history[sampler_set].size() == sampler_ways) 
                replace_addr_history_element(sampler_set);

            assert(addr_history[sampler_set].size() < sampler_ways);
            //Initialize a new entry in the sampler
            addr_history[sampler_set][sampler_tag].init(curr_quanta);
            //If it's a prefetch, mark the prefetched bit;
            optgen.add_access(curr_quanta);
            update_addr_history_lru(sampler_set, sampler_ways-1);
        }

        // Get Hawkeye's prediction for this line
        bool new_prediction = predictor->get_prediction (packet->pc);
        // Update the sampler with the timestamp, PC and our prediction
        // For prefetches, the PC will represent the trigger PC
        addr_history[sampler_set][sampler_tag].update(mytimer, packet->pc, new_prediction);
        addr_history[sampler_set][sampler_tag].lru = 0;
        //Increment the set timer
        mytimer = (mytimer+1) % TIMER_SIZE;
    }

    bool new_prediction = predictor->get_prediction (packet->pc);

    signatures[way_idx] = packet->pc;

    //Set RRIP values and age cache-friendly line
    if(packet->serviced_from_llc == 0 && !new_prediction) {
        counter[way_idx] = maxRRPV;
    } else {
        counter[way_idx] = 0;
        if (packet->serviced_from_llc == 0) {
            bool saturated = false;
            for(uint32_t i=0; i < num_ways; i++)
                if (counter[i] == maxRRPV-1)
                    saturated = true;

            //Age all the cache-friendly  lines
            for(uint32_t i=0; i < num_ways; i++) {
                if (!saturated && counter[i] < maxRRPV-1)
                    counter[i]++;
            }
        }
        counter[way_idx] = 0;
    }
}

uint64_t Hawkeye::get_eviction_candidate(bool is_low_priority = false) {
    // look for the maxRRPV line
    for (uint64_t idx = 0; idx < num_ways; idx++) {
        if (counter[idx] == maxRRPV)
            return idx;
    }

    //If we cannot find a cache-averse line, we evict the oldest cache-friendly line
    uint64_t max_rrip = 0;
    int64_t lru_victim = -1;

    for (uint32_t idx = 0; idx < num_ways; idx++)
    {
        if (counter[idx] >= max_rrip && counter[idx] != UINT64_MAX)
        {
            max_rrip = counter[idx];
            lru_victim = idx;
        }
    }

    assert (lru_victim != -1);
    //The predictor is trained negatively on LRU evictions
    if( is_sampled_set(set_idx) )
    {
        predictor->decrement(signatures[lru_victim]);
    }
    return lru_victim;

    // WE SHOULD NOT REACH HERE
    assert(0);
    return 0;
}

uint64_t Hawkeye::get_reserved_eviction_candidate(bool is_low_priority = false) {
    assert(reserved_ways != 0);
    return 0;
}

void Hawkeye::evict(uint64_t way_idx) {
    counter[way_idx] = UINT64_MAX;
}

uint64_t Hawkeye::get_counter_value(uint64_t way_idx) {
    return counter[way_idx];
}

uint64_t Hawkeye::count_distance(uint64_t threshold) {
    uint64_t distance = std::count_if(
        counter.begin(), counter.end(),
        [threshold](uint64_t n) {
            return n > threshold;}
    );
    return distance;
}

void Hawkeye::repartition_ways(uint64_t num_ways_to_reserve) {
    num_ways -= num_ways_to_reserve;
    reserved_ways = num_ways_to_reserve;
}

void Hawkeye::replace_addr_history_element(uint64_t sampler_set)
{
    uint64_t lru_addr = 0;
    
    for(std::map<uint64_t, AddrInfo>::iterator it=addr_history[sampler_set].begin(); it != addr_history[sampler_set].end(); it++)
    {
   //     uint64_t timer = (it->second).last_quanta;

        if((it->second).lru == (sampler_ways-1))
        {
            //lru_time =  (it->second).last_quanta;
            lru_addr = it->first;
            break;
        }
    }

    addr_history[sampler_set].erase(lru_addr);
}

// called on every cache hit and cache fill
void Hawkeye::update_addr_history_lru(uint64_t sampler_set, uint64_t curr_lru)
{
    for(std::map<uint64_t, AddrInfo>::iterator it=addr_history[sampler_set].begin(); it != addr_history[sampler_set].end(); it++)
    {
        if((it->second).lru < curr_lru)
        {
            (it->second).lru++;
            assert((it->second).lru < sampler_ways); 
        }
    }
}

