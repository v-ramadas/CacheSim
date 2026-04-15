#!/usr/bin/python3

import os
import argparse
import re
import numpy as np
from scipy.stats import entropy, gmean
from utils import *

parser = argparse.ArgumentParser()

parser.add_argument("--log", help="Path to log file", default="", type=str)
parser.add_argument("--graph", help="Graph", default="web-Google", type=str)
parser.add_argument("--no-pc-breakdown", help="Do not breakdown stats by PC", action="store_true")
parser.add_argument("--block-size", help="Block Size", default=64, type=int)
parser.add_argument("--save-fig", help="Save figure", action="store_true")
args = parser.parse_args()

total_cachelines = []
total_blocks = []
block_size = 8
num_sets = 2048
file = open(args.log)
cacheline_map = {}
address_stream = []
line_num = 0
for line in file:
    pattern = r"PC:(\d+).*?(0x[a-fA-F0-9]+)"
    match = re.search(pattern, line)
    if match:
        pc = int(match.group(1))
        if pc == 1:
            continue
        if pc == 4:
            continue
        if pc == 5:
            pc = 6
        if args.no_pc_breakdown:
            pc = 0

        line_num += 1
        address = int(match.group(2), 16)
        address_stream.append(address)
        cacheline = LineStats.align_address(address, args.block_size)
        block = LineStats.align_address(address, block_size)
        total_cachelines.append(cacheline)
        total_blocks.append(block)
        if cacheline not in cacheline_map.keys():
            cacheline_map[cacheline] = LineStats(cacheline, pc, 8, num_sets)
        cacheline_map[cacheline].set_footprint(address)
        cacheline_map[cacheline].increment_accesses()
        cacheline_map[cacheline].set_last_access_time(line_num)
        #if pc != cacheline_map[cacheline].pc and pc == 10:
        #    cacheline_map[cacheline].pc = pc


unique_cachelines, line_counts = np.unique(total_cachelines, return_counts = True)
unique_blocks, block_counts = np.unique(total_blocks, return_counts = True)
print(f"Compulsory Miss traffic: {len(unique_cachelines)*args.block_size/(1024*1024)} MB")

line_probs = line_counts/line_counts.sum()
print(f"Global Entropy: {entropy(line_probs, base=2)}")
block_probs = block_counts/block_counts.sum()
print(f"Local Entropy: {entropy(block_probs, base=2)}")
cacheline_pc_map = {}
for cacheline in cacheline_map.keys():
    pc = cacheline_map[cacheline].pc
    if pc not in cacheline_pc_map.keys():
        cacheline_pc_map[pc] = {}
    cacheline_pc_map[pc][cacheline] = cacheline_map[cacheline]
for pc in cacheline_pc_map.keys():
    arith_reuse_dist = 0
    geo_reuse_dist = []
    num_accesses = 0
    for addr in cacheline_pc_map[pc].values():
        arith_reuse_dist += addr.get_average_reuse_dist()
        geo_reuse_dist.append(addr.get_average_reuse_dist())
    arith_reuse_dist /= len(cacheline_pc_map[pc])
    print(f"PC {pc} Average Arithmetic Reuse Distance {arith_reuse_dist}")
    print(f"PC {pc} Average Geomean Reuse Distance {gmean(geo_reuse_dist)}")
    #print(f"PC {pc} Sparsity Locality Score (SLS) {sls}")
    line_footprint_histogram(cacheline_pc_map[pc], args.graph, pc_map[pc], args.save_fig)
