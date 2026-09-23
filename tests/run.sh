#!/bin/bash
# Run one CacheSim (cachesim_multi) simulation.
# Replaces the old top-level run.sh (dueling) and run_baseline.sh (no dueling).
#
# Usage: tests/run.sh --trace T [options] [-- extra cachesim flags...]
#
# Options (any order):
#   --trace T        required. A path to a trace file, or a bare graph name
#                    (e.g. as-Skitter) resolved to <trace-dir>/<name>.<order>.log
#   --block-size N   LLC block size in bytes              (default: 8)
#   --policy P       LLC replacement policy               (default: hru)
#                    lru hru hrupp phru phrupp srrip drrip prrip ship belady hrrip hawkeye
#   --mode M         none | ztest | ztest-ratio | psel    (default: ztest)
#                    none = baseline run without set dueling
#   --sets N         LLC sets                             (default: 1024)
#   --ways N         LLC ways, 64B-equivalent             (default: 32)
#   --workload W     trace subdirectory under traces/hubs (default: pr_spmv)
#   --trace-dir D    trace directory; overrides --workload
#                    (default: ../pin/pin/traces/hubs/<workload>)
#   --order O        trace ordering suffix: random | default | hubsort (default: random)
#   -h, --help       show this help
#
# Anything after `--` goes straight to cachesim_multi, after the defaults,
# so it overrides them. For example: -- --conf-max 5 --log-dueling-metrics
#
# Examples:
#   tests/run.sh --trace as-Skitter                            # hru, 8B, ztest dueling
#   tests/run.sh --policy phru --mode none --trace web-BerkStan   # baseline
#   tests/run.sh --trace cit-Patents --mode psel -- --log-dueling-metrics
#   tests/run.sh --trace some-graph --workload bfs --sets 2048 --ways 16
#
# Works from any directory. Uses the binary as-is; after editing inc/ or
# src/, rebuild first with `make cachesim_multi`.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/bin/cachesim_multi"

usage() { sed -n '2,33p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
die()   { echo "error: $*" >&2; exit 1; }

TRACE="" BLOCK_SIZE=8 POLICY=hru MODE=ztest SETS=1024 WAYS=32
WORKLOAD=pr_spmv TRACE_DIR="" ORDER=random

while [ $# -gt 0 ]; do
    case "$1" in
        --trace)      TRACE="${2:?--trace needs a value}";           shift 2 ;;
        --block-size) BLOCK_SIZE="${2:?--block-size needs a value}"; shift 2 ;;
        --policy)     POLICY="${2:?--policy needs a value}";         shift 2 ;;
        --mode)       MODE="${2:?--mode needs a value}";             shift 2 ;;
        --sets)       SETS="${2:?--sets needs a value}";             shift 2 ;;
        --ways)       WAYS="${2:?--ways needs a value}";             shift 2 ;;
        --workload)   WORKLOAD="${2:?--workload needs a value}";     shift 2 ;;
        --trace-dir)  TRACE_DIR="${2:?--trace-dir needs a value}";   shift 2 ;;
        --order)      ORDER="${2:?--order needs a value}";           shift 2 ;;
        -h|--help)    usage 0 ;;
        --)           shift; break ;;
        *)            echo "error: unknown option '$1' (pass cachesim flags after --)" >&2; usage ;;
    esac
done

[ -n "$TRACE" ] || { echo "error: --trace is required" >&2; usage; }
TRACE_DIR="${TRACE_DIR:-$ROOT/../pin/pin/traces/hubs/$WORKLOAD}"
[ -f "$TRACE" ] || TRACE="$TRACE_DIR/$TRACE.$ORDER.log"
[ -f "$TRACE" ] || die "trace not found: $TRACE"
[ -x "$BIN" ]   || die "$BIN missing - run 'make cachesim_multi'"

ARGS=(
    --trace "$TRACE" --trace-format instructions
    --config "$ROOT/configs/default.cfg"
    --num-cache-sets "$SETS" --num-cache-ways "$WAYS"
    --cache-block-size "$BLOCK_SIZE"
    --replacement-policy "$POLICY"
)

case "$MODE" in
    none) ;;
    ztest|ztest-ratio|psel)
        ARGS+=(
            --set-dueling --dueling-mode "$MODE"
            --num-duels 64 --psel-max 4096 --psel-threshold 2048
            --dueling-period 1000000
            --conf-epoch 10000 --conf-max 1
        ) ;;
    *) die "unknown --mode '$MODE' (none|ztest|ztest-ratio|psel)" ;;
esac

exec "$BIN" "${ARGS[@]}" "$@"
