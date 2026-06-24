import mmap
import os
import argparse

parser = argparse.ArgumentParser()
parser.add_argument("--log", help="Path to input log", default="", type=str)
parser.add_argument("--out", help="Path to output log", default="", type=str)
args=parser.parse_args()

def process_graph_trace(input_file, output_file):
    with open(input_file, 'r+b') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) 

        line_starts = [0]
        pos = mm.find(b'\n')
        while pos != -1:
            line_starts.append(pos + 1)
            pos = mm.find(b'\n', pos + 1) 

        if line_starts[-1] == mm.size():
            line_starts.pop() 

        num_lines = len(line_starts)
        
        # State Tracking
        next_occurrence = {}     # address -> line_index
        address_reuse_count = {} # address -> total remaining reuses
        
        # PC Degree Metrics Tracking
        pc_total_reuses = {}     # pc -> total cumulative reuses
        pc_unique_nodes = {}     # pc -> set of unique addresses it touched
        
        line_pc_map = [None] * num_lines
        line_address_reuses = [None] * num_lines

        # Pass 1: Backward pass to collect Node Degrees and PC Metrics
        for i in range(num_lines - 1, -1, -1):
            start = line_starts[i]
            end = mm.find(b'\n', start)
            if end == -1: end = mm.size() 

            line = mm[start:end].decode('utf-8').strip()
            if not line: continue 

            try:
                parts = line.split()
                pc_hex = parts[0].split(':', 1)[1]
                line_pc_map[i] = pc_hex
                
                op_addr_part = parts[1].split(':', 1)
                addr_hex = op_addr_part[1]
                raw_addr = int(addr_hex, 16) 

                # Align to 4 bytes (Proxy for Node ID boundaries)
                aligned_addr = raw_addr & ~0x03 

                if aligned_addr in next_occurrence:
                    address_reuse_count[aligned_addr] += 1
                else:
                    address_reuse_count[aligned_addr] = 0 
                
                current_degree_val = address_reuse_count[aligned_addr]
                line_address_reuses[i] = current_degree_val
                
                # Setup PC Graph structural tracking
                if pc_hex not in pc_unique_nodes:
                    pc_unique_nodes[pc_hex] = set()
                
                # Track the node this PC is interacting with
                pc_unique_nodes[pc_hex].add(aligned_addr)
                
                # If it's a valid reuse, add to the total structural edges processed by this PC
                if current_degree_val > 0:
                    pc_total_reuses[pc_hex] = pc_total_reuses.get(pc_hex, 0) + 1

                next_occurrence[aligned_addr] = i 

            except (ValueError, IndexError):
                line_address_reuses[i] = "N/A"

        # Calculate Average Degree per PC: Total Reuses / Unique Nodes Accessed
        pc_avg_degree = {}
        for pc in pc_unique_nodes:
            total_edges = pc_total_reuses.get(pc, 0)
            unique_nodes = len(pc_unique_nodes[pc])
            # Average Degree = structural reuses divided by unique node entities seen
            pc_avg_degree[pc] = total_edges / unique_nodes if unique_nodes > 0 else 0.0

        # Pass 2: Write out with Graph Analytics columns appended
        with open(output_file, 'w') as out:
            for i in range(num_lines):
                start = line_starts[i]
                end = mm.find(b'\n', start)
                if end == -1: end = mm.size()
                
                original_line = mm[start:end].decode('utf-8').rstrip('\r\n')
                if not original_line.strip():
                    out.write(original_line + "\n")
                    continue
                
                pc = line_pc_map[i]
                node_degree = line_address_reuses[i]
                
                if pc in pc_avg_degree:
                    avg_degree = f"{pc_avg_degree[pc]:.2f}"
                else:
                    avg_degree = "N/A"
                
                # Appends: [Original Trace] \t [Node Degree Proxy] \t [PC Avg Degree Proxy]
                out.write(f"{original_line}\t{node_degree}\t{avg_degree}\n")

        mm.close()

input_file = args.log
#outfile_name = os.path.basename(input_file).replace('.log', '.belady.log')
#output_file = os.path.join(os.path.dirname(input_file), outfile_name)
output_file = os.path.join(args.out, os.path.basename(input_file))
print("Storing in output file", output_file)
process_graph_trace(input_file, output_file)
