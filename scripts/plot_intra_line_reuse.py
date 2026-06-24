import argparse
import glob
import os
import re
import matplotlib.pyplot as plt

def main():
    # 1. Set up argparse to get the directory location and plotting preference
    parser = argparse.ArgumentParser(
        description="Parse log files and plot cacheline vs neighbor counts."
    )
    parser.add_argument(
        "--log-dir", 
        type=str, 
        help="Path to the directory containing the log files"
    )
    parser.add_argument(
        "--split-by-graph",
        action="store_true",
        help="If set, generates a separate plot for every unique graph + order combination. "
             "Otherwise, generates 3 plots (one per order) containing all graphs."
    )
    args = parser.parse_args()

    # Verify if the directory exists
    if not os.path.isdir(args.log_dir):
        print(f"Error: The directory '{args.log_dir}' does not exist.")
        return

    # 2. Find all log files in the specified directory
    search_path = os.path.join(args.log_dir, "*.log")
    log_files = glob.glob(search_path)
    out_dir = "plots"
    if not os.path.exists(out_dir):
        os.makedirs(out_dir)

    if not log_files:
        print(f"No .log files found in directory: {args.log_dir}")
        return

    # Regex patterns for filename and log content
    file_pattern = re.compile(r"^(.+)\.(hubsort|default|random)\.log$")
    content_pattern = re.compile(r"Cacheline\s+(\d+),\s+neighbor\s+count\s+(\d+)")

    # Data structure to hold data: data[order][graph_name] = {'x': [...], 'y': [...]}
    data = {
        "hubsort": {},
        "default": {},
        "random": {}
    }

    # 3. Parse each log file
    for file_path in log_files:
        filename = os.path.basename(file_path)
        match_file = file_pattern.match(filename)
        
        if not match_file:
            continue
            
        graph_name, order = match_file.groups()
        
        cachelines = []
        neighbor_counts = []
        
        with open(file_path, "r", encoding="utf-8") as f:
            for line in f:
                match_content = content_pattern.search(line)
                if match_content:
                    cachelines.append(int(match_content.group(1)))
                    neighbor_counts.append(int(match_content.group(2)))
                    
        if cachelines:
            data[order][graph_name] = {
                "x": cachelines,
                "y": neighbor_counts
            }

    # 4. Plotting logic based on the boolean flag
    if not args.split_by_graph:
        # Scenario A: Default behavior (Three plots total, grouped by order)
        for order in ["hubsort", "default", "random"]:
            order_data = data[order]
            if not order_data:
                continue
                
            fig, ax = plt.subplots(figsize=(11, 6))
            
            for graph_name, coords in sorted(order_data.items()):
                ax.scatter(coords["x"], coords["y"], alpha=0.6, label=graph_name, s=40)
                
            ax.set_xlabel("Cacheline Number")
            ax.set_ylabel("Neighbor Count")
            ax.set_title(f"Intra-Cacheline Neighbor Count ({order.capitalize()} Order)")
            ax.grid(True, linestyle="--", alpha=0.5)
            ax.legend(bbox_to_anchor=(1.02, 1), loc="upper left", title="Graph Names")
            ax.set_ylim(0, 250)
            fig.tight_layout()
            output_filename = f"cacheline_neighbor_{order}.png"
            plt.savefig(os.path.join(out_dir, output_filename), dpi=300)
            plt.close(fig)
            print(f"Generated grouped plot: '{output_filename}'")
            
    else:
        # Scenario B: Split behavior (Individual plot for each graph x order combination)
        for order, graphs in data.items():
            for graph_name, coords in graphs.items():
                fig, ax = plt.subplots(figsize=(10, 6))
                
                # Plotting just this single combination
                ax.scatter(coords["x"], coords["y"], alpha=0.7, color="tab:blue", s=40)
                
                ax.set_xlabel("Cacheline Number")
                ax.set_ylabel("Neighbor Count")
                ax.set_title(f"Intra-Cacheline Neighbor Count {graph_name} - {order.capitalize()} Order")
                ax.grid(True, linestyle="--", alpha=0.5)
                ax.set_ylim(0, 250)
           
                fig.tight_layout()
                # Sanitize filename string just in case graph names have weird characters
                safe_graph_name = "".join(c for c in graph_name if c.isalnum() or c in ("-", "_", "."))
                output_filename = f"cacheline_{safe_graph_name}_{order}.png"
                
                plt.savefig(os.path.join(out_dir, output_filename), dpi=300)
                plt.close(fig)
                print(f"Generated individual plot: '{output_filename}'")

if __name__ == "__main__":
    main()
