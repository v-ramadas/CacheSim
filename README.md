# CacheSim

A trace-driven cache simulator for studying LLC block size (64B vs 8B) and replacement policies on graph workloads
(GAP benchmarks traced with Pin). It models an exclusive hierarchy with a small timing model, and reports miss counts,
MPKI and cycles per level.

## Build

```
make            # release, debug and asan builds of both binaries (six binaries in bin/)
make release    # bin/cachesim and bin/cachesim_multi only
make popt_matrix   # bin/popt_matrix, the P-OPT matrix builder (tools/README.md)
make clean
```

The build expects ChampSim's trace reader at `../ChampSim` (`CHAMPSIM_DIR` in the Makefile). Override it with
`make CHAMPSIM_DIR=/path/to/ChampSim` when building from another directory.

| binary | hierarchy |
|---|---|
| `bin/cachesim` | the LLC alone; every trace access goes to it |
| `bin/cachesim_multi` | L1D (sectored, 128 sets x 16 ways of 64B lines with 8B sectors = 128 KB) + LLC, exclusive; `--l2` adds a sectored 1 MB L2 in between |

`_debug` (-O0) and `_asan` (AddressSanitizer) variants of both binaries are also built.

## Quick start

```
bin/cachesim_multi --trace <trace.xz> --trace-format instructions --config configs/default.cfg \
    --num-cache-sets 1024 --num-cache-ways 32 --cache-block-size 8 --replacement-policy srrip
```

`--num-cache-sets` and `--num-cache-ways` describe the 64B-equivalent LLC: 1024 x 32 is 2 MB, and 4096 x 32 is
8 MB. Smaller blocks are sized from that (see iso-area below).

## Traces

`--trace-format instructions` is the format the GAP traces under `pin/pin/traces_roi/hubs/<app>/<G>.<order>.xz` use.
There is one access per line:

```
instCount:2,PC:0x5575223c649c,Read:0x7fff52514278,NextReuse:4,NodeDegree:0,PCAvgDegree:0.00
```

- `NextReuse`: the line index of the next access to the same 64B block (-1 = never).
- `NodeDegree`, `PCAvgDegree`: the degree of the vertex touched, and the PC's average degree. These drive the hub
  labels.

The annotations come from `pin/pin/scripts/hubs.cpp`. `addresses` and `champsim` formats are also accepted. Traces
may be `.xz`-compressed.

## Configuration

`--config configs/default.cfg` sets core, cache-latency and DRAM parameters for the timing model. `configs/intel.cfg`
is an alternative.

### Iso-area 8B LLC

A sub-64B LLC holds more, smaller blocks in the same area. Each block needs its own tag, so extra tag storage eats
into the capacity. With 8B blocks and a 1024 x 32 LLC, the default and the related options give:

| mode | 8B ways per set |
|---|---|
| default: iso-area with Base+Delta tag compression (`--tag-bdi-bases 8 --tag-bdi-delta 10`) | 216 |
| `--tag-bdi-bases 0`: iso-area, uncompressed tags | 160 |
| `--no-iso-area`: no tag cost, 64/block-size x the ways | 256 |

`hrupp` and `phrupp` are never iso-area. They are non-iso-area upper bounds of `hru` and `phru`, not separate
algorithms.

## Replacement policies (`--replacement-policy`)

| name | policy |
|---|---|
| `lru`, `srrip`, `drrip`, `prrip` | baselines (SRRIP: accessed blocks inserted at 0, untouched sectors at maxRRPV-1) |
| `ship_pc`, `ship_mem` | SHiP with a PC or memory-region signature |
| `hawkeye` | Hawkeye (OPTgen on num_sets/64 sampled sets + per-PC predictor) |
| `hub_hawkeye` | Hawkeye + a per-block-address predictor, capped at Hawkeye's predictor budget (2x total) |
| `grasp` | GRASP (software-marked hub range gets priority) |
| `hru`, `phru`, `hrrip`, `hub_oracle` | hub-aware policies from this project (`hub_oracle` uses the trace's degree labels) |
| `hrupp`, `phrupp` | non-iso-area upper bounds of `hru` / `phru` |
| `belady` | Belady's MIN (see below) |
| `popt` | P-OPT (HPCA'21) from a Rereference Matrix (see below) |
| `amoeba` | Amoeba-Cache (variable-size blocks; `--amoeba-*`, `--l1-amoeba*`) |

### Belady

- **64B:** the next use comes from the trace's `NextReuse`.
- **Sub-64B blocks (instruction traces):** the next use comes from an exact 8B next-use oracle. A prescan reads
  the trace once before simulating. No flag is needed.
- **No bypass (MIN):** every block is inserted; the victim is the resident with the farthest next use.
- **Cost:** the oracle needs about 4 B per trace line plus 4 B per touched word.

| trace (8B) | peak memory | run time |
|---|---|---|
| pr sx-stackoverflow | 0.65 GB | about 2.5x a 64B Belady run |
| pr kron21 | 0.88 GB | |
| sssp com-Orkut | 8.2 GB | prescan alone about 18 min; ask for more than 8 GB on the cluster |

### P-OPT

P-OPT (HPCA'21) following the HPCA'21 code (`POPT-CacheSim-HPCA21`): same matrix (128 sub-epochs, 127 throughout
the last epoch), Algorithm 2, tie-break and one matrix line per block. `currDstID` (the vertex being processed) is
inferred from the trace, which has no `PIN_UpdateRegIndex`. Two modes, chosen per trace:

| mode | kernels | matrices | clock |
|---|---|---|---|
| pull (the paper's case) | pr, pr_spmv, cc_sv | files from `make popt_matrix` (`tools/README.md`), looked up in `--popt-matrix-dir`, then `data/popt`, then `<bin>/../data/popt` | per-destination stream, vertices in id order |
| transpose | sssp, bc, bfs (top-down and bottom-up steps), Afforest cc | built in memory from the graph (`<G>.<order>.sg`, `--popt-graph-dir`, default `../gapbs_traces/inputs/<G>/`), one per irregular array the trace reads as `x[neighbour]` | each neighbour-array read; the clock jumps, and a jump re-streams the pinned columns (charged) |

The transpose mode applies the paper's methodology (matrix = compressed transpose, clock = vertex being processed)
to kernels the paper did not evaluate: label its results that way. A trace neither mode recognises falls back to
SRRIP (`POPT inactive`).

- **Alignment:** matrix lines follow the block grid of the traced array. In pull mode an array that starts B bytes
  into a block needs `<G>.<order>[.variant].a<B>.popt` (`popt_matrix --align-offset B`); the run stops and prints the
  builder command if it is missing. The transpose mode handles offsets itself.
- **Block size:** the matrix line is the LLC block. A sub-64B LLC needs `--line-size <block>` matrices
  (`.l8.popt`) in pull mode; without them P-OPT is inactive. An 8B matrix is 8x larger (2 bytes pinned per 8B of the
  array), so it does not fit a 2 MB LLC for sx-stackoverflow or kron21 (`popt: rereference matrix needs N of M LLC ways`).
- `--popt-elem-size` must match the pull-mode matrix.

## Common options

| option | meaning |
|---|---|
| `--set-dueling`, `--dueling-mode psel\|ztest\|ztest-ratio`, `--dueling-period`, `--psel-*`, `--conf-*`, `--z-threshold`, `--window-*` | adaptive 64B/8B set dueling (off by default) |
| `--l1-sets`, `--l1-ways` | L1D geometry (default 128 x 16) |
| `--l2`, `--l2-sets`, `--l2-ways` | add the sectored L2 (default 1024 x 16 = 1 MB) |
| `--tag-bdi-bases`, `--tag-bdi-delta`, `--tag-bdi-unconstrained` | Base+Delta tag compression for 8B sets |
| `--no-iso-area` | size small-block LLCs without tag cost |
| `--use-ghost-cache`, `--ghost-cache-size`, `--ghost-cache-min-serviced` | ghost cache of LLC victims (PHRU/PHRUpp) |
| `--warmup-instructions`, `--iters` | warmup and iteration count |
| `--detailed-dram` | detailed DRAM timing model |
| `--debug`, `--debug-llc`, `--debug-l1`, `--debug-replacement-policy`, `--debug-start`, `--debug-end` | debug prints |

Run `bin/cachesim_multi --help` for the full list.

## Output

Each level prints a `Cache L1D` / `Cache LLC` block (plus `Cache L2` with `--l2`) with hits, misses, `MPKI` and
`Num Cycles`. Parse by the level header, not by position. A per-PC table of hub, non-hub and regular accesses and
misses per level is always printed. Policy-specific statistics follow, for example
`Belady (MIN, ...)`, `HubHawkeye ...` or P-OPT's epoch and engine counters.

## Regression tests

```
python3 tests/regression/run_regression.py baseline      # HRU/PHRU/hub_oracle at 8B, exact match vs golden values
python3 tests/regression/run_regression.py dueling       # same policies under the psel and ztest presets
python3 tests/regression/run_regression.py performance   # MPKI / speedup report, not pass/fail
```

Each mode rebuilds first and compares exact MPKI and cycles against `tests/regression/golden_values.json`. See
`tests/regression/README.md`. Use `--update` only for a deliberate, understood change.

## Layout

| path | contents |
|---|---|
| `inc/`, `src/` | simulator: `cachesim.cc` (hierarchy, fills, evictions, dueling), `set.cc` / `sectored_set.cc`, `*_replacement_policy.*` |
| `configs/` | timing-model configs |
| `tools/` | offline helpers (`popt_matrix`); see `tools/README.md` |
| `tests/regression/` | regression harness and golden values |
| `tests/tag_compression/` | Base+Delta tag tests |
| `scripts/` | analysis and plotting scripts |
| `data/popt/` | Rereference Matrices (gitignored) |
