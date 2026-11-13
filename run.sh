#!/bin/bash

./bin/cachesim_multi \
    --trace ${1} \
    --trace-format instructions \
    --config configs/default.cfg \
    --num-cache-sets 1024 --num-cache-ways 32 \
    --cache-block-size ${2} \
    --num-duels 64 \
    --psel-max 4096 \
    --psel-threshold 2048 \
    --replacement-policy ${3} \
    --dueling-period 1000000 \
    --conf-epoch 10000 \
    --conf-max 1 \
    --set-dueling \
    --dueling-mode ztest
