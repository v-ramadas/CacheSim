#!/bin/bash
#ways_list=("4" "8" "12" "16" "32" "48" "64" "96" "128" "192" "256" "384" "512")
ways_list=("16" "32" "48" "64")
for ways in "${ways_list[@]}"; do
    # Run the simulation
    # Note: 'time' output usually goes to stderr
    echo "Ways: ${ways}"
    time bin/cachesim_multi --num-cache-sets ${1} \
                          --num-cache-ways "${ways}" \
                          --cache-block-size ${2} \
                          --trace "${3}" \
                          --trace-format addresses \
                          --replacement-policy "${4}" \
                          --iters 10 \
                          --warmup-instructions 25000000
done
