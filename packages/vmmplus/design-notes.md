# VMMplus Design Notes

## Overview

VMMplus implements seven variable-order Markov model (VMM) algorithms for discrete
sequence prediction. It is a C++17 rewrite of Begleiter, El-Yaniv & Yona's Java
VMM package (JAIR 2004, "On Prediction Using Variable Order Markov Models").

The same C++ core is used by both the R package (`/workspaces/claude-sandbox/VMMplus/`)
and this Lush package, connected via an `extern "C"` bridge with opaque `void*` handles.

## Algorithms

| Algorithm | Key Idea | Parameters |
|-----------|----------|------------|
| PPM-C     | Prediction by Partial Matching, escape Method C | max_order |
| CTW       | Context Tree Weighting (Volf variant), KT estimator + beta weighting | max_order |
| BCTW      | Binary CTW, single binary tree, symbols decomposed to bits | max_order |
| DCTW      | Decomposed CTW, separate binary CTW per bit position | max_order |
| PST       | Probabilistic Suffix Tree, BFS build with ratio-test pruning | pmin, alpha, gamma, r |
| LZms      | LZ78 with m backward + s forward shifts | m, s |
| LZ78      | Lempel-Ziv 78 (LZms with m=0, s=0) | (none) |

## Architecture

```
Lush code (vmmplus.lsh)
  │
  ├─ DHC wrappers (_vmm-create, _vmm-learn, etc.)
  │    └─ Cast int[] → uint16_t[] via malloc/copy/free
  │
  ├─ mod-load libvmmplus.so (from vmmplus-config.lsh)
  │
  └─ vmm_bridge.h extern "C" functions
       └─ C++ core (vmm_predictor.h, ppmc_predictor.cpp, etc.)
```

### DHC Bridge Pattern

Lush DHC uses `(-int-)` for integers (64-bit on this platform) but the C++ core
uses `uint16_t` symbol arrays and `size_t` lengths. The DHC wrappers:

1. Accept `(-idx1- (-int-))` arrays from Lush
2. Allocate `uint16_t*` buffers via `malloc()`
3. Copy with truncation: `buf[i] = (unsigned short)src[i]`
4. Call the bridge function with `(size_t)` casts for lengths
5. `free()` the temporary buffer

The `dhc-make-with-libs` directive includes `vmm_bridge.h`, so no inline
`extern` declarations are needed (they would conflict with the header's types).

### vmmplus-config.lsh

Compiles all C++ sources into `src/libvmmplus.so` using g++ with `-std=c++17 -O2`.
Checks file timestamps to skip unnecessary rebuilds. Uses `mod-load` to make
the extern "C" functions available to DHC.

## Files

- `vmmplus-config.lsh` — Compile C++ core, load libvmmplus.so
- `vmmplus.lsh` — DHC wrappers + high-level API + helptool docs
- `vmmplus-discretize.lsh` — Pure Lush discretization (SAX, equal-width, equal-freq)
- `src/*.cpp` / `src/*.h` — C++17 algorithm implementations
- `src/vmm_bridge.h/.cpp` — extern "C" bridge (shared with R package)
- `tests/run-all.lsh` — 68 tests

## High-Level API

```lisp
;; Train on symbol sequence
(let ((model (vmm-fit data "ppmc" 128 5)))
  ;; Predict distribution
  (vmm-predict model context)        ;; → idx1 of double
  ;; Single symbol probability
  (vmm-predict-symbol model sym ctx) ;; → double
  ;; Log-loss evaluation
  (vmm-eval model test-data)         ;; → bits/symbol
  ;; Cleanup
  (vmm-destroy model))

;; String convenience API
(let ((model (vmm-fit-string "abracadabra" "ppmc" 5)))
  (vmm-predict-string model "ab")
  (vmm-eval-string model "test")
  (vmm-destroy model))

;; Discretization for continuous data
(let* ((disc (vmm-discretize-fit data "sax" 8))
       (syms (vmm-discretize-transform data disc))
       (inv  (vmm-discretize-inverse syms disc)))
  ...)
```

## Known Issues / Design Decisions

1. **Memory management**: Models must be manually freed with `vmm-destroy`.
   Lush has no finalizers for gptr handles, so forgetting to destroy leaks memory.

2. **Symbol range**: Symbols must be in `[0, alphabet-size)`. No runtime bounds
   checking is performed for performance reasons.

3. **Insertion sort for discretization**: `_vmm-sort-double` uses insertion sort,
   which is O(n²). Fine for moderate-length time series (<10K) but would need
   replacement for very large datasets.

4. **PST probability fix**: The original Java code's `count_context` counts
   context occurrences including at the end of the sequence, but
   `count_symbol_after_context` only counts where a symbol follows. This caused
   distributions to sum to <1.0. Fixed by using the sum of symbol-after-context
   counts as the denominator instead of the raw context count.
