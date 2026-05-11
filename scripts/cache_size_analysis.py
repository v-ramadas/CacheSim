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
parser.add_argument("--cache-size", help="Cache size in MB", default=2, type=int)
args = parser.parse_args()


graph_type = args.graph_type
if graph_type == "pr_spmv":
    graph_type = "default"
num_sets = args.num_sets
expt_list = ["lru_64", "srrip_64", "srrip_8", "ship_64", "ship_8", "prrip_8", 
                "belady_64"]
graph_list = [
        "as-Skitter",
        "cit-Patents",
        "com-LiveJournal",
        "com-Youtube",
        "sx-stackoverflow",
        "web-BerkStan",
        "web-Google",
        "wiki-topcats"
        ]
cache = {}
for expt in expt_list:
    if expt not in cache.keys():
        llc_blk_size = 64
        if "8" in expt:
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
        reached_size = False
        for line in file:
            line_type = cache[expt][cache_type].get_line_type(line)
            if (line_type == "INIT"):
                num_ways = cache[expt][cache_type].get_int_value(line)
                size = num_sets*float(num_ways)*64/1024/1024
                if (size != float(args.cache_size)):
                    reached_size = False
                    continue
                cache[expt]["L1D"].size_dict[graph].append(size)
                cache[expt]["LLC"].size_dict[graph].append(size)
                reached_size = True

            elif reached_size == False:
                continue
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
if not os.path.exists(plot_dir):
    os.makedirs(plot_dir)
label_map = {
    "lru_64": "LRU (64B)",
    "srrip_64": "SRRIP (64B)",
    "srrip_8": "SRRIP (8B)",
    "ship_64": "SHiP Mem (64B)",
    "ship_8": "SHiP Mem (8B)",
    "prrip_8": "Ours (8B)",
    "belady_64": "Belady's Optimal",
}

# Define the order you want the bars to appear in
expt_order = ["lru_64", "srrip_64", "srrip_8", "ship_64", "ship_8", "prrip_8", "belady_64"]
baseline_key = "lru_64"

theme_64 = plt.cm.GnBu(np.linspace(0.4, 0.9, 5)) # 4 policies for 64B
theme_8  = plt.cm.OrRd(np.linspace(0.4, 0.9, 5)) # 4 policies for 8B
for cache_type in ["L1D", "LLC"]:
    plt.figure(figsize=(20, 9)) # Widened slightly for the extra tick
    ax = plt.gca()

    # X-axis will be graph_list + the Geomean entry
    all_x_labels = graph_list + ["GEOMEAN"]
    n_ticks = len(all_x_labels)
    n_expts = len(expt_order)
    
    total_group_width = 0.85
    bar_width = total_group_width / n_expts
    indices = np.arange(n_ticks)

    idx_64, idx_8 = 0, 0

    for i, expt in enumerate(expt_order):
        normalized_values = []
        
        # 1. Collect normalized values for all standard graphs
        for graph in graph_list:
            try:
                base_val = cache[baseline_key][cache_type].mpki_dict[graph][0]
                current_val = cache[expt][cache_type].mpki_dict[graph][0]
                normalized_values.append(current_val / base_val if base_val > 0 else 1.0)
            except (KeyError, IndexError):
                normalized_values.append(1.0) # Assume no change if data missing

        # 2. Calculate Geomean for this experiment and append it
        # Note: We use np.array to ensure gmean works correctly
        experiment_geomean = gmean(normalized_values)
        normalized_values.append(experiment_geomean)

        # 3. Positioning and Color
        offset = (i * bar_width) - (total_group_width / 2) + (bar_width / 2)
        
        if '64' in expt:
            color = theme_64[idx_64]
            idx_64 += 1
        else:
            color = theme_8[idx_8]
            idx_8 += 1
        
        # 4. Plot the bars
        bars = ax.bar(indices + offset, normalized_values, width=bar_width, 
                      label=label_map.get(expt, expt), color=color, 
                      edgecolor='black', alpha=0.9)
        
        # ADD HEIGHT LABELS TO ALL BARS
        ax.bar_label(bars, 
                     fmt='%.2f', 
                     padding=3, 
                     rotation=90, 
                     fontsize=15, 
                     fontweight='bold')

    # --- Formatting ---
    ax.axhline(1.0, color='black', linestyle='-', linewidth=2, alpha=0.6)
    
    # Visual separator for the Geomean section
    ax.axvline(len(graph_list) - 0.5, color='gray', linestyle='--', alpha=0.5)

    ax.set_xticks(indices)
    ax.set_xticklabels(all_x_labels, rotation=25, ha='right', fontsize=10)
    
    # Bold the Geomean label specifically
    labels = ax.get_xticklabels()
    labels[-1].set_fontweight('bold')
    labels[-1].set_color('darkblue')

    ax.set_ylabel("Normalized MPKI (Lower is Better)", fontsize=14, fontweight='bold')
    ax.set_title(f"Normalized MPKI with Aggregate Geomean\n{cache_type} - {args.cache_size}MB Cache", fontsize=14)
    
    ax.grid(axis='y', linestyle='--', alpha=0.4)
    ax.set_axisbelow(True)
    ax.legend(title="Replacement Policy", bbox_to_anchor=(1.02, 1), loc='upper left', prop={'size': 15}) # Size and weight of the legend text)
    
    plt.tight_layout()
    save_path = os.path.join(plot_dir, f"all_graphs_type_{args.graph_type}_{cache_type}_{args.cache_size}MB_geomean.png")
    plt.savefig(save_path)
    plt.close()
#for graph in graph_list:
#    for cache_type in ["L1D", "LLC"]:
#        plt.figure(figsize=(12, 7))
#        ax = plt.gca()
#        
#        # 1. Prepare Data
#        base_y = np.array(cache['lru_64'][cache_type].mpki_dict[graph])
#        x_values = cache['lru_64'][cache_type].size_dict[graph]
#        
#        # We only care about the comparison experiments
#        comp_expts = ['lru_8', 'srrip_64', 'srrip_8', "prrip_64", "prrip_8", "belady_64", "belady_8"]
#        # Width of a bar and positions
#        n_groups = len(x_values)
#        index = np.arange(n_groups)
#        bar_width = 0.2
#        
#        # 2. Plotting the bars
#        for i, expt in enumerate(comp_expts):
#            if expt not in cache: continue
#            
#            other_y = np.array(cache[expt][cache_type].mpki_dict[graph])
#            
#            # Calculate % Change: (Base - New) / Base * 100 
#            # Note: A positive value here means an improvement (reduction in MPKI)
#            percent_change = (base_y - other_y) / base_y * 100
#            
#            # Offset each experiment's bars
#            bars = plt.bar(index + (i * bar_width), percent_change, bar_width, 
#                    label=f"{label_map[expt]} % Reduction",
#                    alpha=0.8)
#            ax.bar_label(bars, 
#                         padding=3, 
#                         fmt='%.1f%%', 
#                         fontsize=9, 
#                         rotation=90 if n_groups > 5 else 0)
#
#        for x in index[1:]:
#            # Position the line slightly to the left of the next index
#            plt.axvline(x - (bar_width / 2), color='gray', linestyle=':', alpha=0.3)
#        
#        # 3. Formatting
#        plt.title(f"% MPKI Reduction vs LRU: {graph} ({cache_type})")
#        plt.xlabel("Cache Size (MB)")
#        plt.ylabel("% Reduction (Higher is Better)")
#        
#        # Set X-ticks to the middle of the groups
#        plt.xticks(index + bar_width / 2, x_values)
#        
#        # Add a horizontal line at 0 for reference
#        plt.axhline(0, color='black', linewidth=0.8, linestyle='-')
#        plt.ylim(-50.0, 50.0)
#        plt.legend()
#        plt.grid(True, axis='y', linestyle='--', alpha=0.6)
#        
#        # Optional: Add text labels on top of bars for exact values
#        plt.tight_layout()
#        
#        # Save with a distinct filename
#        save_path = os.path.join(plot_dir, f"{graph}_{graph_type}_{cache_type}_pct_change.png")
#        plt.savefig(save_path)
#        plt.close()
