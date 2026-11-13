# Running P-OPT on another cluster (all 7 kernels)

This assumes you already know how to run CacheSim (traces, `--config`, LLC geometry, output parsing). It covers only
what P-OPT adds. Kernels: **pr, pr_spmv, cc_sv, bfs, sssp, bc, cc (Afforest)**. tc is not evaluated.

## Code version

**Not committed yet.** The code is branch `performance-model` at local commit `07b9f74` (not pushed) plus uncommitted
changes (transpose mode). Until it is committed and pushed, copy the whole `CacheSim/` source tree (without `data/`,
see below) and rebuild:

```
cd CacheSim && make release          # ChampSim at ../ChampSim, or make CHAMPSIM_DIR=/abs/path/to/ChampSim
make popt_matrix                     # only if you need to build matrices there
```

New files the build needs: `src/popt_push.cc`, `inc/popt_matrix_build.h`. Do not use `3d86eb0` or older: it uses the
old (pre-HPCA) matrix encoding, `.wd` matrices and a coarse 8B mode, none of which exist any more.

## Two modes, picked automatically per trace

| kernels | mode | P-OPT state comes from | needs |
|---|---|---|---|
| pr, pr_spmv, cc_sv | **pull** (HPCA'21 P-OPT as published) | a Rereference Matrix file | `.popt` file(s) |
| bfs, sssp, bc, cc | **transpose** (our extension: same methodology, not in the paper) | matrices built in memory from the graph's transpose at startup | the graph `.sg` |

The simulator first looks for a matching `.popt` matrix and checks that the trace's gather sequence matches it. If
there is no matrix, or it does not match, it switches to transpose mode, which needs the `.sg` graph. If both fail,
P-OPT runs as plain SRRIP (see "Checking that P-OPT was actually active"). Label transpose-mode results as
"P-OPT (transpose extension)", not as the paper's P-OPT.

## What to transfer

| item | source on the original machine | needed for |
|---|---|---|
| simulator source | `CacheSim/` (see Code version) | all |
| timing config | `CacheSim/configs/default.cfg` | all |
| traces | `pin/pin/traces_roi/hubs/<kernel>/<G>.<order>.xz` | all. **Do not rename**; keep one directory per kernel |
| Rereference Matrices | `CacheSim/data/popt/` (see table below) | pr, pr_spmv, cc_sv |
| graphs | `gapbs_traces/inputs/<G>/<G>.<order>.sg` (~320 MB sx-stackoverflow, ~270 MB kron21 each) | bfs, sssp, bc, cc |

Traces exist for sx-stackoverflow and kron21 (`default`, `hubsort`, `random`) for every kernel, except pr_spmv
(sx-stackoverflow only).

### Matrix files (pull kernels)

The matrix name follows the trace name. For `<dir>/<G>.<order>.xz`, the file is
`<G>.<order>[.out][.l8][.a<B>].popt`:

- `.out` = out-neighbour matrix (cc_sv's hook pass); none = in-neighbour (pr, pr_spmv).
- `.l8` = 8B matrix lines, for `--cache-block-size 8`. None = 64B.
- `.a<B>` = the traced array starts B bytes into a 64B block, so the matrix lines follow the block grid. It depends on
  the trace's memory layout, not on the graph.

What the existing traces need:

| kernel | sx-stackoverflow (all 3 orders) | kron21 (all 3 orders) |
|---|---|---|
| pr, 64B | `.a32.popt` | `.popt` |
| pr_spmv, 64B | `.a32.popt` (same file as pr) | (no trace) |
| cc_sv, 64B | `.out.a48.popt`; exists for `default` only | `.out.popt` (offset not verified; see below) |
| pr / pr_spmv, 8B | `.l8.popt` | `.l8.popt` |
| cc_sv, 8B | not built (`.out.l8.popt`) | not built |

Simplest: copy every `sx-stackoverflow.*.popt` and `kron21.*.popt`, about 125 MB per graph and order (skip `*.log`
and `*.bm.popt`, which are no longer used). A matrix does not depend on the LLC size, so one file serves every
`--num-cache-sets`.

**Missing matrix for the trace's offset:** the run **exits 1** with
`POPT: irregData starts B B into a 64B block; build <path> with tools/popt_matrix.cc --align-offset B ...`. Build
exactly what it names, then rerun:

```
bin/popt_matrix ../gapbs_traces/inputs/<G>/<G>.<order>.sg data/popt/<G>.<order>.a<B>.popt --align-offset <B>
#   cc_sv: add --neigh out and name it <G>.<order>.out.a<B>.popt
#   8B:    add --line-size 8 and name it <G>.<order>[.out].l8.popt (4B elements at offset 32/48 are 8B-aligned)
```

This is expected for the cc_sv `hubsort`/`random` traces of sx-stackoverflow (only `default.out.a48` exists) and
possibly for kron21 cc_sv. Each build needs the `.sg` graph.

## Command line

```
cachesim_multi --trace <dir>/<kernel>/<G>.<order>.xz --trace-format instructions --config default.cfg \
    --num-cache-sets 1024 --num-cache-ways 32 --cache-block-size 64 \
    --replacement-policy popt --popt-matrix-dir /abs/path/to/popt --popt-graph-dir /abs/path/to/inputs
```

- `--replacement-policy popt`: required.
- `--popt-matrix-dir`: absolute path. Otherwise only `./data/popt` and `<bin>/../data/popt` are searched.
- `--popt-graph-dir`: absolute path. Either a flat directory of `<G>.<order>.sg`, or the `gapbs_traces/inputs` layout
  `<dir>/<G>/<G>.<order>.sg`. Otherwise only `../gapbs_traces/inputs/<G>/`, `<bin>/../../gapbs_traces/inputs/<G>/`
  and `data/graphs/` are searched. Pass both flags for every kernel; the unused one is ignored.
- Leave `--popt-elem-size` unset (default 4). `--popt-line-size` no longer exists.
- `--cache-block-size 8` is P-OPT at 8B. Pull kernels need the `.l8` matrix. Transpose mode builds 8B matrices
  itself, but that path has **not been run yet**. At 8B the matrix has 8x the lines, so it reserves about 8x the ways
  (sx-stackoverflow pr: 10 of 32 ways at 8 MB; at 2 MB it needs 40 of 32 and exits with
  `popt: rereference matrix needs N of M LLC ways`).

## Checking that P-OPT was actually active

A run that falls back to SRRIP still exits 0, so check every output:

- **Pull mode active:** `POPT: matrix <path> | ...` and `POPT: reserving N of 32 LLC ways ...` at the top. At the end:
  `POPT evictions ...`, `POPT gathers ...`, `POPT currDstID ...`, `POPT next-ref engine ...`.
- **Transpose mode active:** `POPT: transpose mode graph <path> ...`, one `POPT: transpose mode irregular array ...`
  line per array, `POPT: reserving N of 32 ...`. At the end: `POPT (transpose mode) evictions ...`,
  `POPT (transpose mode) clock updates ...`, `POPT next-ref engine ...`.
- **Inactive (SRRIP):** `POPT inactive (no matching Rereference Matrix): SRRIP, no reserved ways` at the end. For any of
  the 7 kernels this is a setup error. For a transpose kernel, look for
  `POPT: transpose mode needs the graph <G>.<order>.sg (--popt-graph-dir); not found`.
- Exit 1 with `irregData starts ...`: build the matrix it names (above). Exit 1 with
  `popt: rereference matrix needs N of M LLC ways`: the LLC is too small for the matrix.
- `POPT: irregData starts at ..., B B into a 64B block: matrix lines follow the block grid` is normal.

## What P-OPT does here (for interpreting results)

- **Faithful to HPCA'21 (paper + POPT-CacheSim-HPCA21 code):** 256 epochs, 128 sub-epochs, two pinned columns
  (current and next epoch), Algorithm 2 next references, the HPCA tie-break (first tied way at max RRPV), and
  `reserved ways = ceil(2 x matrix lines / way bytes)`.
  - At 2 MB/32 ways (64 KB ways), sx-stackoverflow reserves 5 ways and kron21 4. At 8 MB it is 2 and 1.
  - The HPCA paper's 24 MB LLC has 1.5 MB ways, so it reserved 1 way. That is the same formula, not a different
    design.
- **Known differences from the HPCA code, not changed:**
  - The base policy is SRRIP, where HPCA uses DRRIP.
  - Outside the annotated loop, HPCA runs DRRIP over all ways. Here the reserved ways stay reserved for the whole run.
- **currDstID** (the clock) is inferred from the trace's destination stream, because the traces have no
  `PIN_UpdateRegIndex` markers.
- **Transpose mode:**
  - The clock is the vertex being processed, recovered from the neighbour-array index.
  - Each array read as `x[nb[k]]` gets its own matrix. bc has two (path_counts 8B and deltas 4B), hence 15 ways at
    2 MB.
  - When the clock jumps by more than one epoch (frontier-driven kernels), both columns are re-streamed and charged
    as stop-the-world cycles. For sssp and bc this dominates `Num Cycles` (about 10x SRRIP), so **compare MPKI, and
    report cycles separately with that caveat**.
- The streaming cost is in the `POPT gathers`/`clock updates` lines and the per-replacement matrix reads are in
  `POPT next-ref engine`. Both are included in `Num Cycles`.

## Memory and time (sx-stackoverflow.default, 2 MB, 64B)

- Pull mode: a normal run plus the matrix file size (~40 MB).
- Transpose mode: 0.5-0.75 GB peak RSS, because it loads the graph and both CSR directions. Wall time is 2.5-16 min;
  sssp and bc are the slowest.
- 8B pull runs are the slowest overall, because each eviction scans ~200 ways.

## Reference results to validate the setup

1024 sets x 32 ways (2 MB) unless noted, 64B blocks, `configs/default.cfg`, LLC MPKI. The same code and inputs should
reproduce these exactly.

| kernel | graph.order | config | SRRIP | P-OPT | ways reserved |
|---|---|---|---|---|---|
| pr | sx-stackoverflow.default | 2 MB | 58.44 | 44.625824 | 5 |
| pr | sx-stackoverflow.hubsort | 2 MB | 32.20 | 28.136484 | 5 |
| pr | sx-stackoverflow.random | 2 MB | 79.82 | 58.079464 | 5 |
| pr | sx-stackoverflow.default | 8 MB | 20.46 | 14.940630 | 2 |
| pr | sx-stackoverflow.hubsort | 8 MB | 20.02 | 14.828747 | 2 |
| pr | sx-stackoverflow.random | 8 MB | 25.42 | 17.132816 | 2 |
| pr | sx-stackoverflow.hubsort | 8 MB, 8B blocks | 22.16 | 22.932846 | 10 |
| pr | kron21.default | 2 MB | 76.54 | 61.593586 | 4 |
| pr | kron21.hubsort | 2 MB | 28.82 | 26.344528 | 4 |
| pr | kron21.random | 2 MB | 76.74 | 61.764629 | 4 |
| pr_spmv | sx-stackoverflow.default | 2 MB | | 21.886534 | 5 |
| cc_sv | sx-stackoverflow.default | 2 MB | | 43.787613 | 5 |
| sssp | sx-stackoverflow.default | 2 MB | 45.145821 | 42.222416 | 5 |
| bc | sx-stackoverflow.default | 2 MB | 48.794781 | 50.632954 | 15 |
| bfs | sx-stackoverflow.default | 2 MB | 17.617508 | 17.325144 | 6 |
| cc | sx-stackoverflow.default | 2 MB | 23.028521 | 24.530416 | 5 |

Exact checks (`Num Cycles`, P-OPT):

| run | `Num Cycles` |
|---|---|
| pr sx-stackoverflow.default 2 MB | 1768027086 |
| pr_spmv | 1710162008 |
| cc_sv | 2426808624 |
| sssp | 47809014820 |
| bc | 34744302129 |
| bfs | 496910508 |
| cc | 854989490 |

A different value most likely means a different trace, matrix, graph or code version. These numbers replace the ones
in older versions of this note, which used the pre-HPCA encoding (e.g. pr sx default 50.35).
