import re
import argparse


def process_log_file(input_filename, output_filename):
    processed_data = []
    unique_addresses = set()  # Set to track unique hex values    
    # Pattern explanation:
    # ^PC:10        -> Matches lines starting with PC:10
    # .*?           -> Non-greedy match for anything in between
    # (read|write): -> Matches either 'read:' or 'write:'
    # (0x[0-9a-fA-F]+) -> Captures the hex address
    pattern = r'^PC:10.*?(?:read|write):(0x[0-9a-fA-F]+)'
    
    try:
        with open(input_filename, 'r') as file:
            for line in file:
                line = line.strip()
                match = re.search(pattern, line)
                
                # Only keep the line if it matches PC:10 AND has a read/write hex
                if match:
                    # Store a tuple: (integer_value_of_hex, original_line)
                    hex_val = int(match.group(1), 16)
                    processed_data.append((hex_val, line))
                    unique_addresses.add(hex_val)    
        # Sort by the first element of the tuple (the integer hex value)
        processed_data.sort(key=lambda x: x[0])
        
        # Write the sorted lines to the output file
        with open(output_filename, 'w') as out_file:
            for _, original_line in processed_data:
                out_file.write(original_line + '\n')
        
        print("Processing complete.")
        print(f"Total 'PC:10' lines found: {len(processed_data)}")
        print(f"Unique addresses found: {len(unique_addresses)}")
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
