#!/bin/bash

# Define the base directory where your folders reside
dir="../traces/gap/${1}"
output_dir="../traces/gap/belady_8/${1}"
        
# Create output directory if it doesn't exist
mkdir -p "$output_dir"
        
# Loop through all log files in the current subdirectory
#for log_file in "$dir"/*.log; do
#    echo "Processing: $log_file"
#    # Execute the python command
#    python3 belady.py --log "$log_file" --out "$output_dir"
#done
parallel python3 belady.py --log {} --out "$output_dir" ::: "$dir"/*.log
echo "Batch processing complete."
