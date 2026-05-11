#!/bin/bash

# Check if a directory argument was provided
if [ -z "$1" ]; then
    echo "Usage: $0 <parent_directory>"
    exit 1
fi

# Iterate through every item in the provided directory
cache_size_list=("1" "2" "4" "6" "8")
for subdir in "$1"/*/; do
    # Remove the trailing slash for cleaner logging/passing
    subdir=${subdir%/}

    echo "Processing: $subdir"

    # Run your commands
    for cache_size in "${cache_size_list[@]}"; do
        python3 cache_size_analysis.py --dir "$subdir" --graph random --cache-size ${cache_size}
        python3 cache_size_analysis.py --dir "$subdir" --graph hubsort --cache-size ${cache_size}
    done
done
