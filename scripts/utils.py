import matplotlib.pyplot as plt
import numpy as np
import collections
import re

pc_map = {
        0: "All_Accesses",
        1: "Loop Variable",
        2: "outgoing_contrib reads",
        3: "scores",
        4: "outgoing_contrib writes",
        5: "out_degree",
        6: "out_degree",
        10: "hub node reads",
        11: "non-hub node reads",
        }


class CacheStats:
    def __init__(self, name: str, level: int, block_size: int):
        self.name = name
        self.level = level
        self.block_size = block_size
        self.mpki_dict = {}
        self.size_dict = {}
        return

    def get_line_type(self, line: str):
        if "PC" in line:
            if "Misses" in line:
                return "PC Miss"
            elif "Hits" in line:
                return "PC Hit"
            elif "Evictions" in line:
                return "PC Eviction"
            elif "Density" in line:
                return "PC Density"
            elif "Reuse" in line:
                return "PC Reuse Probability"
        elif "Ways:" in line:
            return "INIT"
        elif "MPKI" in line:
            return "MPKI"
        elif "Utilization" in line:
            return "Utilization"
        elif "Partial Misses" in line:
            return "Partial Misses"
        elif "Trace File" in line:
            return "Level"
        else:
            return "Ignore"

    def get_pc(self, line: str):
        match = re.search(r'PC\s+(0x[\da-fA-F]+)', line)
        return int(match.group(1), 16)

    def get_int_value(self, line: str, pc: int = 0):
        if pc == 0:
            match = re.search(r'\d+', line)
            return int(match.group())
        else:
            pattern = r'PC\s+(0x[\da-fA-F]+)\s+(\w+)\s+(\d+)'
            match = re.search(pattern, line)
            if int(match.group(1), 16) == pc:
                return int(match.group(3))


    def get_float_value(self, line: str, pc: int = 0):
        if pc == 0:
            match = re.search(r'\d+\.?\d*', line)
            if match is None:
                return None
        else:
            pattern = r'PC\s+(0x[\da-fA-F]+)\s+(\w+)\s+(\d+\.?\d*)'
            match = re.search(pattern, line)
            if match is None:
                return None
            if int(match.group(1), 16) == pc:
                return int(match.group(3))
        return float(match.group())

class LineStats:
    def __init__(self, address: int, pc: int, block_size: int, num_sets: int):
        self.address = address
        self.set_idx = (address >> 6)%num_sets
        self.line_size = 64
        self.pc = pc
        self.block_size = block_size
        self.num_blocks = int(self.line_size/self.block_size)
        self.num_block_bits = self.num_blocks
        self.footprint = [0]*self.num_blocks
        self.accesses = 0
        self.last_access_time = 0
        self.reuse_dist = 0
        return

    def count_density(self):
        return self.footprint.count(1)

    def align_address(address, size):
        mask = ~(size - 1)
        return address & mask

    def get_word_idx(self, address):
        word_idx = (address - self.address) >> (self.block_size.bit_length() - 1)
        if word_idx < 0 or word_idx >= self.num_blocks:
            print(f"Incorrect address {hex(address)} to cacheline {hex(self.address)}")
        return word_idx

    def set_footprint(self, address):
        self.footprint[self.get_word_idx(address)] = 1

    def clear_footprint(self, address):
        self.footprint[self.get_word_idx(address)] = 0

    def get_footprint(self, address):
        return self.footprint[self.get_word_idx(address)]

    def increment_accesses(self):
        self.accesses += 1

    def set_last_access_time(self, time):
        self.reuse_dist += (time - self.last_access_time)
        self.last_access_time = time

    def get_average_reuse_dist(self):
        return self.reuse_dist/self.accesses


def line_footprint_histogram(cacheline_map, graph_name, ds, save_fig=False):
    densities = [obj.count_density() for obj in cacheline_map.values()]

    # 2. Setup the plot
    plt.figure(figsize=(8, 5))
    
    # 3. Create the histogram
    # Bins are set from 0.5 to 8.5 to center bars over integers 1-8
    counts, bins, patches = plt.hist(densities, bins=np.arange(0.5, 9.5), rwidth=0.8, color='teal', edgecolor='black')
    # Print y-values on bars
    for count, bin_edge in zip(counts, bins):
        if count > 0:
            plt.text(bin_edge + 0.5, count + 0.05, str(int(count)), ha='center', va='bottom')

    # 4. Formatting
    plt.title(f'{graph_name}: {ds} Cacheline Densities With Infinite Cache Size', fontsize=14)
    plt.xlabel('Line Density Value', fontsize=12)
    plt.ylabel('Number of Cachelines', fontsize=12)
    plt.xticks(range(1, 9))
    plt.grid(axis='y', alpha=0.3)
    
    # Show the plot
    if save_fig:
        plt.savefig(f'{graph_name}_{ds}_line_densities.png')
    else:
        plt.show()
