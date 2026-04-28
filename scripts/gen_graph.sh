#!/bin/bash
python3 workload_analysis.py --dir ${1} --graph random
python3 workload_analysis.py --dir ${1} --graph hubsort
