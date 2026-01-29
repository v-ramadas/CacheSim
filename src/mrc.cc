#include "mrc.h"
#include "cachesim.h"
#include <cassert>

void MRC::handle_fill(uint64_t address, uint64_t num_blocks, uint64_t block_size) {
  auto aligned_address = align_address(address, CACHELINE_SIZE) + (num_blocks - 1)*block_size;
  auto num_fills = num_blocks;
  while(num_fills > 0) {
    insert_block(aligned_address);
    aligned_address -= block_size;
    num_fills--;
  }
}


void MRC::insert_block(uint64_t address) {
      assert(stack_pos.find(address) == stack_pos.end());

      // MISS: The address is not in the stack (a compulsory miss for all cache sizes).
      // This is treated as a distance of 'infinity' for the MRC.

      // The size of the current stack before insertion is the maximum distance observed so far.
      // A compulsory miss contributes to the miss count for all cache sizes > current stack size.

      // distance is the size of the stack
      uint64_t distance = lru_stack.size();

      if (distance == max_size) {
          auto evicted_address = lru_stack.back();
          lru_stack.pop_back();
          stack_pos.erase(evicted_address);
      }

      // distance_counts[distance] accounts for the misses (stack distance is distance + 1, or 'infinity')
      // This miss should be counted against the capacity *one greater* than the current stack size.
      // However, for the standard Mattson algorithm implementation, a miss *introduces* a new element.
      // The stack distance is usually defined as $d$, where a hit is counted for all cache sizes $\ge d$.
      // A miss has a stack distance equal to the current stack size $S$ plus one (if we consider a max capacity of $S+1$).
      // Let's use the definition where distance $d$ means a hit for cache size $d$.

      lru_stack.push_front(address);
      stack_pos[address] = lru_stack.begin();

      // The distance count for 'infinity' is implied.
}

void MRC::handle_hit(std::unordered_map<uint64_t, std::list<uint64_t>::iterator>::iterator it_map) {
      // HIT: Calculate stack distance, record in histogram, and update LRU stack.
      // The list iterator to the element's current position
      std::list<uint64_t>::iterator it_list = it_map->second;
      // Calculate the stack distance (depth)
      // The distance is the number of *unique* elements in the stack above the requested element.
      // distance = number of elements from the front of the list up to, but not including, *it_list.
      uint64_t distance = 0;
      for (auto it = lru_stack.begin(); it != it_list; ++it) {
          distance++;
      }

      // distance_counts is 0-indexed, so distance 1 corresponds to index 0, etc.
      if (distance >= (uint64_t)distance_counts.size()) {
          distance_counts.resize(distance + 1, 0);
      }
      distance_counts[distance]++;

      // Update LRU stack: Move the accessed element to the front (Most Recently Used).
      lru_stack.splice(lru_stack.begin(), lru_stack, it_list);
      
      // The iterator in the map is still valid and now points to the front (lru_stack.begin()).
      it_map->second = lru_stack.begin();
}

bool MRC::mattson_stack_distance_algorithm(uint64_t address, uint64_t block_size) {
  total_accesses++;
  auto critical_block = align_address(address, block_size);
  auto it_map = stack_pos.find(critical_block);
  if (it_map != stack_pos.end()) {
      handle_hit(it_map);
  } else {
    handle_fill(address, CACHELINE_SIZE/block_size, block_size);
  }

  return true;
}

void MRC::print_mpki_curve(uint64_t inst_count) const {

      fmt::print ( "\n--- MPKI Curve (MpkiC) ---\n");
      fmt::print ( "Cache Size (d) | Hit Count | Miss Count | MPKI\n");
      fmt::print ( "--------------------------------------------------\n");

      long long cumulative_hits = 0;
      int max_cache_size = lru_stack.size();

      // Total Misses for a size d = Total Accesses - Cumulative Hits up to size d.
      // Stack distance d means a hit in a cache of size d.
      for (int d = 1; d <= max_cache_size; ++d) {
          // distance_counts is 0-indexed, so distance d (size d) is at index d-1
          if (d - 1 < (int)distance_counts.size()) {
              cumulative_hits += distance_counts[d - 1];
          }

          long long miss_count = total_accesses - cumulative_hits;
          double mpki = ((double)(miss_count) / inst_count)*1000;

          if ((d > 0) && ((d & (d-1)) == 0)) {
              fmt::print("{} | {} | {} | {:f}\n", d, cumulative_hits, miss_count, mpki);
          }
      }

      // Add the 'infinite' cache size entry:
      // Cache Size = unique elements (stack_pos.size()), which results in only compulsory misses.
      // Total unique addresses: all accesses hit eventually, so miss count is just compulsory misses.
      long long unique_accesses = stack_pos.size();

      // This is complex, so for simplicity in the basic algorithm presentation, we stop at max_cache_size,
      // which covers all hits that occurred in the trace.

      // A full MRC for all sizes up to the number of unique elements is often computed.
      // For a size equal to the total number of unique elements, all non-compulsory misses are hits.
      // Let's print the maximum, which is the total unique elements in the trace.
      // The miss count at this size is the number of accesses that were a "first-time" access (compulsory misses).
      long long final_hit_count = cumulative_hits;
      long long final_miss_count = total_accesses - final_hit_count;
      double final_mpki = ((double)(final_miss_count) / inst_count)*1000;

      fmt::print("{} | {} | {} | {:f}\n", unique_accesses, final_hit_count, final_miss_count, final_mpki);
      fmt::print ( "--------------------------------------------------\n");
}
