import mmap
import os
import argparse

parser = argparse.ArgumentParser()
parser.add_argument("--log", help="Path to input log", default="", type=str)
parser.add_argument("--out", help="Path to output log", default="", type=str)
args=parser.parse_args()

def process_log_mmap(input_file, output_file):
    # 1. Open the file and map it
    with open(input_file, 'r+b') as f:
        # Create a memory map of the file
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        
        # 2. Get line offsets to process backwards
        # We find all newline positions
        line_starts = [0]
        pos = mm.find(b'\n')
        while pos != -1:
            line_starts.append(pos + 1)
            pos = mm.find(b'\n', pos + 1)
        
        # Remove empty last entry if file ends with newline
        if line_starts[-1] == mm.size():
            line_starts.pop()
        
        # We need a way to store distances in order. 
        # Using a list of distances corresponding to each line index.
        num_lines = len(line_starts)
        distances = [None] * num_lines
        next_occurrence = {} # address -> line_index
        
        # 3. Process backwards to fill the distances
        for i in range(num_lines - 1, -1, -1):
            start = line_starts[i]
            # Find the end of the line
            end = mm.find(b'\n', start)
            if end == -1: end = mm.size()
            
            # Extract line and parse
            line = mm[start:end].decode('utf-8').strip()
            if not line:
                continue
                
            try:
                parts = line.split()
                # Parse: PC:num  op:addr
                # Use split(':', 1) to ensure we only split at the first colon for the address
                # Example: "read:0x7fd4..." -> ["read", "0x7fd4..."]
                op_addr_part = parts[1].split(':', 1)
                addr_hex = op_addr_part[1]
                raw_addr = int(addr_hex, 16)
                
                # Align to 64 bytes (0x40)
                #aligned_addr = raw_addr & ~0x3F
                aligned_addr = raw_addr & ~0x7

                # Calculate distance
                if aligned_addr in next_occurrence:
                    distances[i] = next_occurrence[aligned_addr] - i
                else:
                    distances[i] = "inf"
                
                # Update map
                next_occurrence[aligned_addr] = i
                
            except (ValueError, IndexError):
                distances[i] = "N/A"
        
        mm.close()

    # 4. Write results to the output file
    # We read the original file again line by line to preserve formatting
    # or iterate through the lines we already parsed.
    with open(input_file, 'r') as fin, open(output_file, 'w') as fout:
        for i, line in enumerate(fin):
            dist = distances[i]
            fout.write(f"{line.strip()}\t{dist}\n")

# Usage
input_file = args.log
#outfile_name = os.path.basename(input_file).replace('.log', '.belady.log')
#output_file = os.path.join(os.path.dirname(input_file), outfile_name)
output_file = os.path.join(args.out, os.path.basename(input_file))
print("Storing in output file", output_file)
process_log_mmap(input_file, output_file)
