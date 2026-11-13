#!/bin/bash

# Check if a directory argument was provided
if [ -z "$1" ]; then
    echo "Usage: $0 <parent_directory>"
    exit 1
fi

# Iterate through every item in the provided directory
set_list=("1024" "2048" "4096")
way_list=("16" "32" "48" "64")
for num_sets in "${set_list[@]}"; do
    # Remove the trailing slash for cleaner logging/passing
    subdir=${1}/num_sets_${num_sets}

    echo "Processing: $subdir"

    # Run your commands
    for num_ways in "${way_list[@]}"; do
        python3 new_analysis.py --dir "$subdir" --graph random --num-sets ${num_sets} --num-ways ${num_ways}
        python3 new_analysis.py --dir "$subdir" --graph hubsort --num-sets ${num_sets}  --num-ways ${num_ways}
        python3 new_analysis.py --dir "$subdir" --graph default --num-sets ${num_sets}  --num-ways ${num_ways}
    done
done
