#!/bin/bash
# Compress every *.log file in a directory, several files at a time.
#
# Usage: tests/compress_traces.sh [options] <dir>
#
#   --format F   xz | gz                       (default: xz; smallest, read by TraceFile)
#   --level N    compression level             (default: 6)
#   --jobs N     files compressed at once      (default: 8)
#   --threads N  threads per file              (default: nproc / jobs)
#   --delete     remove each original after its compressed copy verifies
#   --force      overwrite existing compressed files (default: skip them)
#   -h, --help   show this help
#
# Writes <name>.xz / <name>.gz (for <name>.log) next to each original, via
# a .tmp file so an interrupted run never leaves a truncated archive behind.
# Originals are kept unless --delete is given.
#
# Example: tests/compress_traces.sh --jobs 9 ../pin/pin/traces/hubs/pr_spmv

set -euo pipefail

usage() { sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
die()   { echo "error: $*" >&2; exit 1; }

FORMAT=xz LEVEL=6 JOBS=8 THREADS="" DELETE=0 FORCE=0 DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --format)  FORMAT="${2:?}";  shift 2 ;;
        --level)   LEVEL="${2:?}";   shift 2 ;;
        --jobs)    JOBS="${2:?}";    shift 2 ;;
        --threads) THREADS="${2:?}"; shift 2 ;;
        --delete)  DELETE=1; shift ;;
        --force)   FORCE=1;  shift ;;
        -h|--help) usage 0 ;;
        -*)        echo "error: unknown option '$1'" >&2; usage ;;
        *)         [ -z "$DIR" ] || die "only one directory allowed"; DIR="$1"; shift ;;
    esac
done
[ -n "$DIR" ] || usage
[ -d "$DIR" ] || die "not a directory: $DIR"
THREADS="${THREADS:-$(( $(nproc) / JOBS > 0 ? $(nproc) / JOBS : 1 ))}"

case "$FORMAT" in
    xz) command -v xz >/dev/null || die "xz not installed"
        COMPRESS="xz -$LEVEL -T$THREADS -c"; TEST="xz -t" ;;
    gz) if command -v pigz >/dev/null; then COMPRESS="pigz -$LEVEL -p $THREADS -c"; TEST="pigz -t"
        else COMPRESS="gzip -$LEVEL -c"; TEST="gzip -t"; fi ;;
    *)  die "unknown --format '$FORMAT' (xz|gz)" ;;
esac

compress_one() {
    local src="$1" dst="${1%.log}.$FORMAT"
    if [ -e "$dst" ] && [ "$FORCE" -eq 0 ]; then
        echo "skip  $src (exists)"; return 0
    fi
    if $COMPRESS < "$src" > "$dst.tmp" && $TEST "$dst.tmp"; then
        mv -f "$dst.tmp" "$dst"
        [ "$DELETE" -eq 1 ] && rm -f "$src"
        echo "done  $src -> $(du -h "$dst" | cut -f1)"
    else
        rm -f "$dst.tmp"
        echo "FAIL  $src" >&2; return 1
    fi
}
export -f compress_one
export FORMAT COMPRESS TEST DELETE FORCE

shopt -s nullglob
files=("$DIR"/*.log)
[ ${#files[@]} -gt 0 ] || die "no *.log files in $DIR"
echo "compressing ${#files[@]} files with '$COMPRESS', $JOBS at a time"
printf '%s\0' "${files[@]}" | xargs -0 -n 1 -P "$JOBS" bash -c 'compress_one "$1"' _
