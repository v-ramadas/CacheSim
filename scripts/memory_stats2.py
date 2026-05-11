import re
import os
import argparse
from utils import *

block_size = 8
cacheline_size = 64
num_sets = 2048
cacheline_map = {}

def process_log_file(input_filename, output_filename):
    # Pattern explanation:
    # ^PC:10        -> Matches lines starting with PC:10
    # .*?           -> Non-greedy match for anything in between
    # (read|write): -> Matches either 'read:' or 'write:'
    # (0x[0-9a-fA-F]+) -> Captures the hex address
    pattern = r"PC:(\d+).*?(0x[a-fA-F0-9]+)"
    
    try:
        with open(input_filename, 'r') as file:
            for line in file:
                #line = line.strip()
                match = re.search(pattern, line)
                
                # Only keep the line if it matches PC:10 AND has a read/write hex
                if match:
                    pc = int(match.group(1))
                    # Store a tuple: (integer_value_of_hex, original_line)
                    block_address = LineStats.align_address(int(match.group(2), 16), block_size)
                    line_address = LineStats.align_address(int(match.group(2), 16), cacheline_size)
                    if line_address not in cacheline_map.keys():
                        cacheline_map[line_address] = LineStats(line_address, pc, block_size, num_sets)
                    cacheline_map[line_address].accesses = cacheline_map[line_address].accesses+1
                    cacheline_map[line_address].set_footprint(block_address)

        # Sort by the first element of the tuple (the integer hex value)
        
        # Write the sorted lines to the output file
        with open(output_filename, 'w') as out_file:
            for key in cacheline_map.keys():
                out_file.write(f"0x{hex(key)}\tPC {hex(cacheline_map[key].pc)}\tAccesses:{cacheline_map[key].accesses}\tFootprint:{cacheline_map[key].footprint}\n")
        
        print("Processing complete.")
        print(f"Sorted output saved to: {output_filename}")

    except FileNotFoundError:
        print(f"Error: '{input_filename}' not found.")
    except Exception as e:
        print(f"An error occurred: {e}")
# Run the function
parser = argparse.ArgumentParser()
parser.add_argument("-i", help="Path to input file", default="", type=str)
parser.add_argument("-o", help="Path to output file", default="", type=str)
args = parser.parse_args()
process_log_file(args.i, args.o)
