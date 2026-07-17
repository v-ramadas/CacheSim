#!/bin/bash

blk_size_list=("64" "8")
app_list=("pr_spmv")
replacement_policy_list=("lru" "srrip" "hru" "hrupp" "phru" "phrupp")
set_list=("1024" "2048" "4096")
# This will exit and print the error if $1 is empty or unset
expt_name=${1:? "Usage: $0 <experiment_name>"}
config=../configs/default.cfg
for num_sets in "${set_list[@]}"; do
    for blk_size in "${blk_size_list[@]}"; do
        for app in "${app_list[@]}"; do
            for policy in "${replacement_policy_list[@]}"; do
                out_dir=results/$(date +%m_%d)/${expt_name}/${app}/num_sets_${num_sets}/${policy}_${blk_size}/
                trace_dir=../../pin/pin/traces/hubs/${app}
                mkdir -p ${out_dir}
                echo "Submitting batch for App: $app, Policy: $policy, Blk: $blk_size"
                rm -f "$out_dir"/*.sweep
                condor_submit out_dir=$out_dir trace_dir=$trace_dir blk_size=$blk_size num_sets=$num_sets replacement_policy=$policy config_file=$config cachesim_multi_perf.sub
            done
        done
    done
done
