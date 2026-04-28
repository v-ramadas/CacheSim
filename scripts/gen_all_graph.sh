#!/bin/bash
python3 all_workload_analysis.py --dir ${1} --graph random
python3 all_workload_analysis.py --dir ${1} --graph hubsort
