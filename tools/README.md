# tools/

Offline helpers for CacheSim. They are separate programs, not part of the simulator.

## popt_matrix: P-OPT Rereference Matrix builder

`popt_matrix.cc` builds the Rereference Matrix that P-OPT (Balaji et al., HPCA 2021) needs
(`--replacement-policy popt`). It reads a GAP serialized graph (`.sg`) and writes one matrix file per graph,
vertex ordering and kernel variant. This is a one-time step for each graph, ordering and variant. The matrix does
not depend on the cache configuration.

### Build

```
make popt_matrix        # -> bin/popt_matrix (not built by the default `make`)
```

### Usage

```
bin/popt_matrix <graph.sg> <out.popt> [--writes-at-dst] [--neigh in|out] [--elem-bits 1] [--elem-size N] [--epochs N] [--line-size N]
```

| option | default | meaning |
|---|---|---|
| `--elem-size N` | 4 | bytes per element of the irregularly read array; must divide 64 |
| `--epochs N` | 256 | number of epochs (2-1024); each epoch is split into 127 sub-epochs |
| `--line-size N` | 64 | bytes of the array per matrix line: 8, 16, 32 or 64. 8 gives one entry per 8B block, for an 8B LLC; the matrix, and the LLC space it pins, grows by 64/N |
| `--writes-at-dst` | off | also count pr's (Gauss-Seidel) write of `outgoing_contrib[u]` at destination u |
| `--neigh in\|out` | `in` | neighbours read per destination: `in` = pull PageRank, `out` = cc_sv's hook pass |
| `--elem-bits 1` | off | the irregular array is a bitmap (1 bit per vertex), as in bfs bottom-up steps; also stores every 16th neighbour so the simulator can validate the trace |

### Variants and file names

The simulator finds a matrix from the trace name. For a trace `<dir>/<G>.<order>.xz` it looks for
`<G>.<order>[.variant].popt`. It searches `--popt-matrix-dir` first, then `data/popt`, then `<bin>/../data/popt`.

| kernel (trace dir) | file | builder flags |
|---|---|---|
| pr_spmv (read-only pull) | `<G>.<order>.popt` | none |
| pr (Gauss-Seidel pull) | `<G>.<order>.wd.popt` | `--writes-at-dst` |
| cc_sv (hook pass) | `<G>.<order>.out.popt` | `--neigh out` |
| bfs (bottom-up steps) | `<G>.<order>.bm.popt` | `--elem-bits 1` |

Matrices with `--line-size N` other than 64 add `.l<N>` before `.popt`, e.g. `<G>.<order>.wd.l8.popt`.

The simulator validates each trace against the matrix and picks the variant that fits. Kernels with no matching
matrix fall back to SRRIP, as the run's output reports. Kernels without a matrix include bc, sssp and Afforest cc.

### Building every variant

Graphs live in `gapbs_traces/inputs/<G>/<G>.<order>.sg`, with order `default`, `hubsort` or `random`:

```
IN=../gapbs_traces/inputs
for d in $IN/*/; do G=$(basename $d)
  for o in default hubsort random; do sg=$IN/$G/$G.$o.sg; [ -f $sg ] || continue
    bin/popt_matrix $sg data/popt/$G.$o.popt
    bin/popt_matrix $sg data/popt/$G.$o.wd.popt  --writes-at-dst
    bin/popt_matrix $sg data/popt/$G.$o.out.popt --neigh out
    bin/popt_matrix $sg data/popt/$G.$o.bm.popt  --elem-bits 1
  done
done
```

`data/popt` currently holds 228 matrices: 19 graphs x 3 orders x 4 variants, about 7.1 GB in total. `data/` is in
`.gitignore`, so copy the matrices along with the binaries for cluster runs, or pass `--popt-matrix-dir`.

### File format (little-endian)

1. **Header** (`PoptHeader`, magic `POPTRM2`): graph size, element and line size, epochs, sub-epochs, line count, variant flags.
2. **Check prefix**: the first min(num_arcs, 1024) neighbours in traversal order (int32), used to match a trace to the graph.
3. **Starts**: `num_epochs * 127 + 1` uint64 values. Each is the index of the first gather of an (epoch, sub-epoch)
   block; the last entry is `num_arcs`.
4. **Matrix**: `num_epochs` columns of `num_lines` bytes, one byte per (epoch, line):
   - MSB 1: the line is not referenced in this epoch. The low 7 bits give the number of epochs to its next
     reference; 127 means never.
   - MSB 0: the line is referenced in this epoch. The low 7 bits give the sub-epoch of its last reference.
5. **Sample trailer** (`.bm.popt` only): the magic `POPTSMP1`, then the stride (16), the sample count, and every 16th neighbour (int32).
