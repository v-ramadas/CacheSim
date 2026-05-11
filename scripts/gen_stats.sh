#!/bin/bash

# Create the output directory if it doesn't exist
mkdir -p logs

# Navigate to the base trace directory
BASE_DIR="../traces/gap/belady"

# Loop through each subdirectory (the <dir> variable)
for d in "$BASE_DIR"/*/; do
    # Remove trailing slash to get the directory name
    dir_name=$(basename "$d")

    # Loop through the files to find the <graph> name
    # We look for .hubsort.log files to extract the graph prefix
    for file in "$d"*.hubsort.log; do
        # Check if file exists to avoid errors in empty dirs
        [ -e "$file" ] || continue

        # Extract the <graph> name (everything before .hubsort.log)
        filename=$(basename "$file")
        graph_name=${filename%.hubsort.log}

        echo "Processing $dir_name / $graph_name..."

        # Run the hubsort command
        python memory_stats2.py --i "$d$graph_name.hubsort.log" \
                                -o "logs/$dir_name.$graph_name.hubsort.log"

        python memory_stats2.py --i "$d$graph_name.random.log" \
                                    -o "logs/$dir_name.$graph_name.random.log"
    done
done

echo "Done!"
