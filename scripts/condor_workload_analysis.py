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
expt_list_64 = ["baseline_64", "blkSize_64"]
expt_list_8 = ["baseline_8", "blkSize_8"]

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
for expt in expt_list_64 + expt_list_8:
    if expt not in cache.keys():
        llc_blk_size = 64
        if expt == "blkSize_8" or expt == "baseline_8":
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
    "baseline": "Baseline (64B, no ServicedFromLLC Bit)",
    "blkSize_64": "64B Blocks With ServicedFromLLC Bit Used",
    "blkSize_8": "8B Blocks With ServicedFromLLC Bit Used"
}

label_map_all = {
    "baseline_64": "64B: Baseline Replacement (No Predictor)",
#    "reserve_64": "64B: Reserve One Way for Streaming Data",
    "blkSize_64": "64B: Use Predictor Based Replacement",
    "baseline_8": "8B: Baseline Replacement (No Predictor)",
#    "reserve_8": "8B: Reserve One Way for Streaming Data",
    "blkSize_8": "8B: Use Predictor Based Replacement"
}

if not os.path.exists(plot_dir):
    os.makedirs(plot_dir)
for graph in graph_list:
    for cache_type in ["L1D", "LLC"]:
        plt.figure(figsize=(10, 6))
        ax=plt.gca()
        max_ylim = 0
        for expt in expt_list_64 + expt_list_8:
            y_values = cache[expt][cache_type].mpki_dict[graph]
            x_values = cache[expt][cache_type].size_dict[graph]
            ax.set_xticks(x_values)
            if len(y_values) == 0:
                continue
            if max_ylim < max(y_values):
                max_ylim = max(y_values)
            plt.plot(x_values, y_values, label=label_map_all[expt], marker='o', markersize=4)
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
        base_y = np.array(cache['baseline_64'][cache_type].mpki_dict[graph])
        base_x = np.array(cache['baseline_64'][cache_type].size_dict[graph])
        
        for other_expt in ['blkSize_64', 'baseline_8', 'blkSize_8']:
            if other_expt in cache and graph in cache[other_expt][cache_type].mpki_dict:
                other_y = np.array(cache[other_expt][cache_type].mpki_dict[graph])
                
                # Calculate absolute difference
                diffs = (base_y - other_y)/base_y*100
                if len(diffs) == 0:
                    continue
                max_idx = np.argmax(diffs)
                min_idx = np.argmin(diffs)
                
                stats_lines.append(f"--- {label_map_all[other_expt]} vs Baseline ---")
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


for graph in graph_list:
    for cache_type in ["L1D", "LLC"]:
        plt.figure(figsize=(12, 7))
        ax = plt.gca()
        
        # 1. Prepare Data
        base_y = np.array(cache['baseline_64'][cache_type].mpki_dict[graph])
        x_values = cache['baseline_64'][cache_type].size_dict[graph]
        
        # We only care about the comparison experiments
        comp_expts = ['baseline_8', 'blkSize_64', 'blkSize_8']
        
        # Width of a bar and positions
        n_groups = len(x_values)
        index = np.arange(n_groups)
        bar_width = 0.2
        
        # 2. Plotting the bars
        for i, expt in enumerate(comp_expts):
            if expt not in cache: continue
            
            other_y = np.array(cache[expt][cache_type].mpki_dict[graph])
            
            # Calculate % Change: (Base - New) / Base * 100 
            # Note: A positive value here means an improvement (reduction in MPKI)
            percent_change = (base_y - other_y) / base_y * 100
            
            # Offset each experiment's bars
            bars = plt.bar(index + (i * bar_width), percent_change, bar_width, 
                    label=f"{label_map_all[expt]} % Reduction",
                    alpha=0.8)
            ax.bar_label(bars, 
                         padding=3, 
                         fmt='%.1f%%', 
                         fontsize=9, 
                         rotation=90 if n_groups > 5 else 0)

        for x in index[1:]:
            # Position the line slightly to the left of the next index
            plt.axvline(x - (bar_width / 2), color='gray', linestyle=':', alpha=0.3)
        
        # 3. Formatting
        plt.title(f"% MPKI Reduction vs Baseline: {graph} ({cache_type})")
        plt.xlabel("Cache Size (MB)")
        plt.ylabel("% Reduction (Higher is Better)")
        
        # Set X-ticks to the middle of the groups
        plt.xticks(index + bar_width / 2, x_values)
        
        # Add a horizontal line at 0 for reference
        plt.axhline(0, color='black', linewidth=0.8, linestyle='-')
        plt.ylim(-50.0, 50.0)
        plt.legend()
        plt.grid(True, axis='y', linestyle='--', alpha=0.6)
        
        # Optional: Add text labels on top of bars for exact values
        plt.tight_layout()
        
        # Save with a distinct filename
        save_path = os.path.join(plot_dir, f"{graph}_{graph_type}_{cache_type}_pct_change.png")
        plt.savefig(save_path)
        plt.close()