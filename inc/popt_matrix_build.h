#ifndef __POPT_MATRIX_BUILD_H__
#define __POPT_MATRIX_BUILD_H__

// P-OPT Rereference Matrix construction, as the HPCA'21 builder (applications/popt/builder.h makeOffsetMatrix):
// shared by tools/popt_matrix.cc (matrix files) and the simulator's transpose mode (matrices built in memory).
//
// The traversal processes vertices u = 0 .. n-1 (the clock); processing u references element nb[k] of the
// irregular array for every k in [off[u], off[u+1]). For the array's line c, epoch e (EpochSize vertices of u) and
// num_sub sub-epochs per epoch, one byte (column-major: matrix[e * num_lines + c]):
//   MSB 1: c not referenced in epoch e; low 7 bits = epochs to its next referencing epoch, 127 = none;
//   MSB 0: c referenced in epoch e; low 7 bits = sub-epoch of its last reference (127 throughout the last epoch).
// Vertex v lies in line (v + off_v) / per_line: per_line vertices per line, off_v vertices of the first line that
// precede the array (the array starts off_v elements into a block).

#include <cstdint>
#include <vector>

inline void popt_build_matrix(uint64_t n, const int64_t* off, const int32_t* nb, uint64_t num_epochs, uint64_t num_sub,
                              uint64_t epoch_size, uint64_t sub_size, uint64_t per_line, uint64_t off_v,
                              uint64_t num_lines, std::vector<uint8_t>& matrix) {
    std::vector<int8_t> last(num_epochs * num_lines, -1);
    for (uint64_t u = 0; u < n; u++) {
        uint64_t e = u / epoch_size;
        int8_t s = (int8_t)((u - e * epoch_size) / sub_size);
        for (int64_t k = off[u]; k < off[u + 1]; k++)
            last[e * num_lines + ((uint64_t)nb[k] + off_v) / per_line] = s;   // u ascends: the last write is the last reference
    }
    (void)num_sub;
    matrix.assign(num_epochs * num_lines, 0);
    for (uint64_t c = 0; c < num_lines; c++) {
        uint64_t next = UINT64_MAX;   // next referencing epoch at or after e
        for (uint64_t e = num_epochs; e-- > 0;) {
            int8_t s = last[e * num_lines + c];
            if (s >= 0) {
                matrix[e * num_lines + c] = e == num_epochs - 1 ? 127 : (uint8_t)s;   // MSB 0; last epoch: 127 (HPCA'21)
                next = e;
            } else {
                uint64_t dist = next == UINT64_MAX ? 127 : (next - e < 127 ? next - e : 127);
                matrix[e * num_lines + c] = (uint8_t)(0x80 | dist);
            }
        }
    }
}

#endif
