# Lush Performance Benchmarks

## Matrix Multiply (double precision, C = A * B)

Date: 2026-03-03
Platform: Linux 6.8.0-101-generic, x86_64
CPU: (cloud VM, details TBD)
BLAS: /lib/x86_64-linux-gnu/libblas.so (reference BLAS / OpenBLAS)
Compiler: gcc -O3 -march=native

### Before BLAS fix (idx-m2timesm2 column-loop)

```
  Size        |  idx-m2timesm2 (old)    |  BLAS dgemm             |  Naive C i-p-j
              |  time(s)  GFLOPS        |  time(s)  GFLOPS        |  time(s)  GFLOPS
  ------------|-------------------------|-------------------------|------------------------
    64 x 64   |   0.0946    2.77       |   0.0062   42.54       |   0.0179   14.64
   128 x 128  |   0.2114    1.98       |   0.0053   79.86       |   0.0323   12.98
   256 x 256  |   0.6725    1.00       |   0.0042  160.13       |   0.0782    8.58
   512 x 512  |   2.1744    0.37       |   0.0035  230.15       |   0.0553   14.55
  1024 x 1024 |   5.8054    0.37       |   0.0069  313.41       |   0.1668   12.87
```

**Root cause**: `idx-m2timesm2` decomposes matmul into N separate matrix-vector
products by column (`idx-eloop` + `idx-m2dotm1`).  Row-major storage + column
iteration = stride-N access on every cache-line fetch.  Each call scans the
entire A matrix, so A is read N times total.  Result: ~35x slower than even
naive C, ~850x slower than BLAS at 1024x1024.

Nobody in packages/ actually called it — all real linear algebra uses BLAS.

### After BLAS fix (idx-d2timesd2 → dgemm)

```
  Size        |  idx-d2timesd2 (BLAS)   |  Direct dgemm           |  Naive C i-p-j
              |  time(s)  GFLOPS        |  time(s)  GFLOPS        |  time(s)  GFLOPS
  ------------|-------------------------|-------------------------|------------------------
    64 x 64   |   0.0063   41.90       |   0.0061   43.12       |   0.0176   14.87
   128 x 128  |   0.0057   73.70       |   0.0055   76.01       |   0.0376   11.17
   256 x 256  |   0.0045  150.20       |   0.0043  154.34       |   0.0755    8.89
   512 x 512  |   0.0035  228.85       |   0.0036  226.72       |   0.0552   14.59
  1024 x 1024 |   0.0063  339.04       |   0.0065  332.53       |   0.1538   13.97
```

**Speedup**: idx-d2timesd2 is now ~916x faster at 1024x1024 (339 vs 0.37 GFLOPS).
Overhead vs direct dgemm call is <3% (interpreted wrapper cost).

### Implementation notes

- `idx-d2timesd2` (double) and `idx-f2timesf2` (float) now auto-detect BLAS
  via `find-shared-library "libblas"` at load time
- If BLAS found: compiled dgemm/sgemm wrapper via separate `dhc-make-with-libs`
- If BLAS not found: falls back to original `idx-m2timesm2` column-loop
- Row-major trick: `C = A*B` → col-major `C^T = B^T * A^T` via
  `dgemm("N","N", n, m, k, 1.0, B, n, A, k, 0.0, C, n)`
- Files modified: `lsh/libidx/idx-double.lsh`, `lsh/libidx/idx-float.lsh`
- Separate compilation unit (`idx_double_blas.so`, `idx_float_blas.so`) avoids
  filename collision with main `dhc-make`


## Naive C loop analysis

The naive C triple-loop (i-p-j order with `aip` hoisted) gets ~13 GFLOPS,
which is respectable for scalar code.  GCC's `-O3 -march=native` does
auto-vectorization.  The i-p-j ordering gives sequential access on both
B rows and C rows, so cache utilization is good.

BLAS is still 20-25x faster due to:
- Cache-tiled blocking (register/L1/L2/L3 tile hierarchy)
- Explicit SIMD (AVX2/AVX-512: 4-8 doubles/cycle)
- Micro-kernel assembly (hand-tuned inner loop for each architecture)
- Loop unrolling and software pipelining
