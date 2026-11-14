#!/bin/bash
# Build and run the Sniper queue-model differential test (Sniper sources from ../../../snipersim, unchanged).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SNIPER=${SNIPER:-$HERE/../../../snipersim/common}
CHAMPSIM=${CHAMPSIM:-$HERE/../../../ChampSim}
OUT=${OUT:-$(mktemp -d)}
SRC="$SNIPER/performance_model/queue_model.cc $SNIPER/performance_model/queue_model_history_list.cc
     $SNIPER/performance_model/queue_model_windowed_mg1.cc $SNIPER/performance_model/queue_model_basic.cc
     $SNIPER/performance_model/queue_model_contention.cc $SNIPER/performance_model/contention_model.cc
     $SNIPER/misc/modulo_num.cc"
g++ -std=c++17 -O2 -w -I"$HERE/stubs" -I"$SNIPER/performance_model" -I"$SNIPER/misc" \
    -I"$CHAMPSIM/vcpkg_installed/x64-linux/include" -I"$HERE/../../inc" \
    $SRC "$HERE/diff_test.cc" "$HERE/log_stub.cc" "$HERE/../../src/sniper_dram.cc" \
    -L"$CHAMPSIM/vcpkg_installed/x64-linux/lib" -lfmt -o "$OUT/sniper_queue_diff"
"$OUT/sniper_queue_diff"
