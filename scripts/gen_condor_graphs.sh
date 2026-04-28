#!/bin/bash

# Check if a directory argument was provided
if [ -z "$1" ]; then
    echo "Usage: $0 <parent_directory>"
    exit 1
fi

# Iterate through every item in the provided directory
for subdir in "$1"/*/; do
    # Remove the trailing slash for cleaner logging/passing
    subdir=${subdir%/}

    echo "Processing: $subdir"

    # Run your commands
    python3 condor_workload_analysis.py --dir "$subdir" --graph random
    python3 condor_workload_analysis.py --dir "$subdir" --graph hubsort
done
