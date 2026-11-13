# Amoeba-Cache configurations in CacheSim

As of commit `3357242` (branch `performance-model`, 2026-11-04). There are two families:

- **Amoeba as the LLC** (`--replacement-policy amoeba`): works in both builds.
- **Amoeba as the L1D** (`--l1-amoeba`): `cachesim_multi` only.

The two families cannot be combined.

Base arguments used below:

```
BASE="--trace <trace.xz> --trace-format instructions --config configs/default.cfg --num-cache-sets 1024 --num-cache-ways 32"
```

## A. Amoeba LLC (`--replacement-policy amoeba`)

Rules:
- Requires `--cache-block-size 8`; any other block size exits with an error.
- Base+Delta tags are turned off (Amoeba keeps its own tag words), and so is set dueling.
- `cachesim_multi`: Amoeba LLC behind the normal 128KB 64B-sectored L1D.
- `cachesim`: Amoeba is the single cache.

| # | Config | Flags (after `$BASE`) | Words per set | Notes |
|---|---|---|---|---|
| A1 | iso-area, span insert (**default**) | `--cache-block-size 8 --replacement-policy amoeba` | 64B budget / 66 bits | Stores an L1D victim's first-to-last touched words as one block |
| A2 | iso-area, line insert | `--cache-block-size 8 --replacement-policy amoeba --amoeba-insert line` | same | Stores the whole victim line |
| A3 | non-iso, span | `--cache-block-size 8 --replacement-policy amoeba --no-iso-area` | 256 (32 x 8), tags still take words | Naive size, not iso-area |
| A4 | non-iso, line | `--cache-block-size 8 --replacement-policy amoeba --no-iso-area --amoeba-insert line` | 256 | |
| A5 | free tags (debug bound) | add `--amoeba-free-tags` to A1-A4 | as above | Tags cost nothing: a capacity upper bound |
| A6 | fixed size (debug) | add `--amoeba-words N` | N | Overrides both the iso-area and non-iso sizing |

That gives 8 meaningful LLC configs (insert {span, line} x size {iso, non-iso} x free tags {off, on}),
plus `--amoeba-words` sweeps. Each config runs in either build.

Extra output line: `Amoeba: fills ... blocks inserted ... words/block ... words dropped at fill ... blocks evicted ...`

## B. Amoeba L1D (`--l1-amoeba`, `cachesim_multi` only)

The L1D becomes an Amoeba word array: 128 sets x 138 words, iso-area with the 128 x 16 x 64B L1D. A miss
fetches one contiguous run of words around the demanded word. The run is limited to words the source holds
and the L1D does not already hold.

Rules:
- Cannot be combined with `--l2`, set dueling or `--replacement-policy amoeba`.
- The LLC uses whatever `--replacement-policy` is passed.
- **Only a 64B LLC is tested and supported** (`--cache-block-size 64`): the partial-line present mask exists
  only for 64B LLC ways.

| # | Config | Flags (after `$BASE --cache-block-size 64 --replacement-policy srrip`) | What it tests |
|---|---|---|---|
| B0 | Baseline (no Amoeba) | (none) | Reference |
| B1 | Line fetch | `--l1-amoeba --l1-amoeba-fetch line` | Amoeba's organization cost alone (about equal to the baseline) |
| B2 | History fetch, drop (**default**) | `--l1-amoeba` | Realistic predictor (span used in the line's previous L1 residency; whole line on first touch); strictly exclusive |
| B3 | History fetch, keep in LLC | `--l1-amoeba --l1-amoeba-keep-llc` | Ablation: unfetched words stay in / go into the LLC as a partial line (breaks line-level exclusivity) |
| B4 | Oracle fetch, drop | `--l1-amoeba --l1-amoeba-fetch oracle --l1-amoeba-oracle F` | Perfect-predictor bound |
| B5 | Oracle fetch, keep | `--l1-amoeba --l1-amoeba-fetch oracle --l1-amoeba-oracle F --l1-amoeba-keep-llc` | Perfect predictor without the exclusivity loss |
| B6 | Line fetch, keep | `--l1-amoeba --l1-amoeba-fetch line --l1-amoeba-keep-llc` | Allowed but a no-op: line fetch leaves nothing to keep |

**LLC policy dimension:** B0-B5 can pair with any 64B LLC policy (`srrip`, `lru`, `drrip`, `ship`, `hawkeye`,
`hru`, `phru`, ...). The runs so far used `srrip` only.

Extra output line: `Amoeba L1D (<fetch>[, keep-llc]): fills ... words/fill ... fills of lines with a previous residency ... residencies with an oracle mask ... words kept in the LLC ...`

### Oracle runs (B4 / B5) need a recording first, per trace

```
# 1) record: a line-fetch run, which is also config B1
cachesim_multi $BASE --cache-block-size 64 --replacement-policy srrip \
    --l1-amoeba --l1-amoeba-fetch line --l1-amoeba-record /path/<trace>.oracle

# 2) replay
cachesim_multi $BASE --cache-block-size 64 --replacement-policy srrip \
    --l1-amoeba --l1-amoeba-fetch oracle --l1-amoeba-oracle /path/<trace>.oracle [--l1-amoeba-keep-llc]
```

- One record file per trace and config. The format is binary: per line, the line address (u64), the
  residency count (u32), then one used-word mask byte per residency.
- Schedule B4 and B5 only after step 1 has finished for that trace.
- The masks come from a whole-line recording run, so this oracle approximates the paper's exact oracle.

## C. Invalid / no-op combinations

| Combination | Result |
|---|---|
| `--l1-amoeba` + `--replacement-policy amoeba` | Error |
| `--l1-amoeba` + `--l2` or `--set-dueling` | Error |
| `--l1-amoeba` in `cachesim` (single-level) | No effect (that build has no L1D) |
| `--replacement-policy amoeba` + `--cache-block-size 64` | Error |
| `--l1-amoeba-fetch oracle` without a readable `--l1-amoeba-oracle` | Error |
| Amoeba LLC + `--tag-bdi-*` | Silently ignored (Amoeba keeps its own tag words) |
| `--l1-amoeba` + 8B LLC (`--cache-block-size 8`) | Untested / unsupported |

## D. Suggested sweep

- **LLC study:**
  - A1, A2, A3 and A5 in `cachesim_multi` (optionally also `cachesim`);
  - against SRRIP-64B (`--cache-block-size 64 --replacement-policy srrip`) and SRRIP-8B
    (`--cache-block-size 8 --replacement-policy srrip`, iso-area with BDI tags by default).
- **L1D study:**
  - B0-B5 with `srrip` (optionally also `lru` / `hru`);
  - one oracle recording per trace (step 1 above, which is also B1).

Earlier results (2026-11-03/04, 21 pr_spmv_clean traces x B0-B5, SRRIP 2MB):
- Line fetch is within 0-1% of the baseline.
- LLC MPKI with drop: +6% to +137%; with keep: +1% to +6.5%. The LLC loss comes from exclusivity plus
  dropping words.
- L1D MPKI, history fetch: +6.5% in default order and -1.0% in random order. Oracle with keep: +3.3% / -1.6%.
- Best case is sx-stackoverflow at -5% to -7%.
