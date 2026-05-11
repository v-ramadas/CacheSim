#!/bin/bash

export CORES=64
export SCRIPT_PATH="/nobackup.1/vishnu/adaptive-cache/CacheSim/scripts/memory_stats2.py"
export INPUT_BASE="/nobackup.1/vishnu/adaptive-cache/CacheSim/traces/gap/belady"
export OUTPUT_BASE="/nobackup.1/vishnu/adaptive-cache/CacheSim/scripts/logs"

mkdir -p "$OUTPUT_BASE"

# --- THE COMMAND ---
# We use ::: to pass arguments to Parallel, which is cleaner than piping
find "$INPUT_BASE" -type f -name "*.log" | \
parallel -j "$CORES" --progress --joblog logs/parallel_job_history.log \
'FULL_PATH={}; 
 FILENAME=$(basename $FULL_PATH); 
 DIRNAME=$(basename $(dirname $FULL_PATH)); 
 python3 "$SCRIPT_PATH" -i "$FULL_PATH" -o "$OUTPUT_BASE/${DIRNAME}.${FILENAME}"'

echo "-------------------------------------------------------"
echo "Processing complete. Check the 'logs/' folder for results."
echo "View logs/parallel_job_history.log for task status."
