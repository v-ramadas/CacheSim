#!/usr/bin/python3

import os
import argparse
import re
import numpy as np
from scipy.stats import entropy, gmean
from utils import *
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter, MultipleLocator

parser = argparse.ArgumentParser()
parser.add_argument("--dir", help="Path to results dir", default="", type=str)
parser.add_argument("--no-pc-breakdown", help="Do not breakdown stats by PC", action="store_true")
parser.add_argument("--graph-type", help="Choose the graph type from [hubsort, random, pr_spmv]", default="pr_spmv", type=str)
parser.add_argument("--num-sets", help="Number of sets", default=2048, type=int)
args = parser.parse_args()


graph_type = args.graph_type
if graph_type == "pr_spmv":
    graph_type = "default"
num_sets = args.num_sets
expt_list = ["baseline", "blkSize_64", "blkSize_8"]
graph_list = [
        "as-Skitter",
        "cit-Patents",
        "com-LiveJournal",
        "com-Youtube",
        "roadNet-CA",
        "soc-LiveJournal1",
        "sx-stackoverflow",
        "web-BerkStan",
        "web-Google",
        "wiki-topcats"
        ]
cache = {}
for expt in expt_list:
    if expt not in cache.keys():
        llc_blk_size = 64
        if expt == "blkSize_8":
            llc_blk_size = 8
        cache[expt] = {"L1D": CacheStats("L1D", 0, 64), "LLC": CacheStats("LLC", 0, llc_blk_size)}
    expt_dir = os.path.join(args.dir, expt)
    result_log_list = os.listdir(expt_dir)
    for graph in graph_list:
        for cache_type in ["L1D", "LLC"]:
            if graph not in cache[expt][cache_type].mpki_dict.keys():
                cache[expt][cache_type].mpki_dict[graph] = []
                cache[expt][cache_type].size_dict[graph] = []
        result_log = ".".join([graph, args.graph_type, "sweep"])
        if result_log not in result_log_list:
            continue

        file = open(os.path.join(expt_dir, result_log))
        cache_type = "LLC"
        for line in file:
            line_type = cache[expt][cache_type].get_line_type(line)
            if (line_type == "INIT"):
                num_ways = cache[expt][cache_type].get_int_value(line)
                cache[expt]["L1D"].size_dict[graph].append(num_sets*float(num_ways)*64/1024/1024)
                cache[expt]["LLC"].size_dict[graph].append(num_sets*float(num_ways)*64/1024/1024)

            elif line_type == "Level":
                if cache_type == "L1D":
                    cache_type = "LLC"
                else:
                    cache_type = "L1D"
            elif line_type == "MPKI":
                mpki = cache[expt][cache_type].get_float_value(line)
                if mpki is None:
                    continue
                cache[expt][cache_type].mpki_dict[graph].append(mpki)
#            elif line_type == "Utilization":
#                utilization = cache[expt][cache_type].get_float_value(line)
            elif "PC" in line_type:
                pc = cache[expt][cache_type].get_pc(line)
                if line_type == "PC Miss":
                    misses = cache[expt][cache_type].get_int_value(line, pc)
                elif line_type == "PC Hits":
                    hits = cache[expt][cache_type].get_int_value(line, pc)
                elif line_type == "PC Eviction":
                    evictions = cache[expt][cache_type].get_int_value(line, pc)
                elif line_type == "PC Density":
                    line_density_counts = cache[expt][cache_type].get_int_value(line, pc)

plot_dir = os.path.join(args.dir, "plots")
label_map = {
    "baseline": "Baseline (64B)",
    "blkSize_64": "No Streaming Data Stores (64B Blocks)",
    "blkSize_8": "No Streaming Data Stores (8B Blocks)"
}
if not os.path.exists(plot_dir):
    os.makedirs(plot_dir)
for graph in graph_list:
    for cache_type in ["L1D", "LLC"]:
        plt.figure(figsize=(10, 6))
        ax=plt.gca()
        max_ylim = 0
        for expt in expt_list:
            y_values = cache[expt][cache_type].mpki_dict[graph]
            x_values = cache[expt][cache_type].size_dict[graph]
            ax.set_xticks(x_values)
            if len(y_values) == 0:
                continue
            if max_ylim < max(y_values):
                max_ylim = max(y_values)
            plt.plot(x_values, y_values, label=label_map[expt], marker='o', markersize=4)
        plt.title(f"{graph} {cache_type} MPKI - {graph_type.title()} Node Order")
        plt.xlabel("Cache Size (MB)")
        plt.ylabel("MPKI Value")
        plt.xscale("log", base=2)
        ymin, ymax = ax.dataLim.intervaly
        if ymax <= 0:
            plt.close()
            continue
        plt.ylim(0, max_ylim*1.2)
        plt.legend()
        plt.grid(True, linestyle='--', alpha=0.7)
        formatter = ScalarFormatter()
        formatter.set_scientific(False) # Force scientific notation off
        ax.xaxis.set_major_formatter(formatter)
        #ax.yaxis.set_major_locator(MultipleLocator(round(max_ylim/10, -2)))
        # Calculation Logic for the Text Box
        stats_lines = []
        # Ensure baseline exists to compare against
        base_y = np.array(cache['baseline'][cache_type].mpki_dict[graph])
        base_x = np.array(cache['baseline'][cache_type].size_dict[graph])
        
        for other_expt in ['blkSize_8', 'blkSize_64']:
            if other_expt in cache and graph in cache[other_expt][cache_type].mpki_dict:
                other_y = np.array(cache[other_expt][cache_type].mpki_dict[graph])
                
                # Calculate absolute difference
                diffs = (base_y - other_y)/base_y*100
                if len(diffs) == 0:
                    continue
                max_idx = np.argmax(diffs)
                min_idx = np.argmin(diffs)
                
                stats_lines.append(f"--- {label_map[other_expt]} vs Baseline ---")
                stats_lines.append(f"Best MPKI Reduction: {diffs[max_idx]:.3f}% at {base_x[max_idx]}MB")
                stats_lines.append(f"Worst MPKI Reduction: {diffs[min_idx]:.3f}% at {base_x[min_idx]}MB")
                stats_lines.append("") # Spacer line
        
        if stats_lines:
            report_text = "\n".join(stats_lines).strip()
            # transform=ax.transAxes means (0,0) is bottom-left and (1,1) is top-right
            plt.text(0.02, 0.02, report_text, transform=ax.transAxes, 
                     horizontalalignment='left', verticalalignment='bottom', fontsize=9,
                     bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.3))
        # Adjust layout and show/save
        plt.tight_layout()
        plt.savefig(os.path.join(plot_dir, f"{graph}_{graph_type}_{cache_type}_mpki.png"))
