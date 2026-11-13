#!/bin/bash

./bin/cachesim_multi \
    --trace ${1} \
    --trace-format instructions \
    --config configs/default.cfg \
    --num-cache-sets 1024 --num-cache-ways 32 \
    --cache-block-size ${2} \
    --replacement-policy ${3} \
#    --use-ghost-cache --ghost-cache-size 256 \
    #--debug-ghost-cache
