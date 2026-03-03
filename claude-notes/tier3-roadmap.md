# Tier 3 ML Roadmap — Implementation Status

## Current State (Tiers 1–3 Complete)

### Tier 1–2 (Previously Complete)
- **libnum**: regression (OLS, ridge, logistic, elastic net), PCA/BCA, k-means, model selection, feature scaling, CDF, covariance, linear algebra
- **mapper**: full TDA pipeline with lenses (PCA, eccentricity, L-inf, stats, column), clustering (SLINK, DBSCAN), interactive viz, SQLite persistence
- **wavelets**: DWT (GSL), MODWT (pure C), MRA, denoising, 17 filter families
- **xgboost**: gradient-boosted trees via lush-pkg
- **columnardb**: query language, DataTable

### Tier 3 (Implemented 2026-03-03)

| Item | File | Status | Tests |
|------|------|--------|-------|
| UMAP | `packages/mapper/umap.lsh` | **Done** | 39/39 |
| Sparse CSR/CSC | `packages/sparse/sparse.lsh` | **Done** | 20/20 |
| k-NN (brute, VP-tree, KD-tree, HNSW) | `packages/libnum/knn.lsh` + `hnsw-config.lsh` | **Done** | 11/11 |
| Naive Bayes (Gaussian, Multinomial, Bernoulli + EM) | `packages/libnum/naive-bayes.lsh` | **Done** | 10/10 |
| Random Forest (classify, regress, proximity, conformal) | `packages/libnum/rf.lsh` | **Done** | 8/8 |
| Spectral Methods (LE, DM, Kernel PCA) | `packages/libnum/spectral.lsh` | **Done** | 10/10 |
| Mapper Lens Integration (8 new lenses + GUI) | `packages/mapper/lens.lsh` + `mapper-viz.lsh` + `viz/index.html` | **Done** | 7/7 |

**Total: 108 tests (69 Tier 3 + 39 UMAP), all passing.**

---

## Implementation Notes

### Sparse CSR/CSC
- Custom CSR format with compiled SpMV, transpose, row slicing
- `sparse-from-dense` and `sparse-from-triplets` constructors (triplets sum duplicates)
- ARPACK not implemented — power iteration with deflation suffices for spectral methods

### k-NN Search (4 algorithms)
- **Brute force**: O(N²d), best for N<1000. Compiled C.
- **VP-tree**: Exact, O(N log N) build. Parallel array layout. Any metric.
- **KD-tree**: Exact, max-spread dimension selection, leaf size 16. Best for D<20.
- **HNSW**: Approximate, sub-linear. hnswlib v0.8.0 headers vendored in `hnswlib/`. Compiled with g++ separately from DHC (avoids lushmake C++ linking issues). Lazy-loaded on first use.

### Random Forest
- Array-based tree storage (parallel int/double arrays, iterative stack-based build)
- Fisher-Yates shuffle for bootstrap/feature subsets
- Proximity matrix for mapper integration (fraction of trees sharing a leaf)
- Conformal prediction with calibration quantile

### Mapper Lenses
- 10 total lenses available in GUI: PCA, eccentricity, UMAP (1D/2D), Laplacian Eigenmaps (1D/2D), Diffusion Maps (1D/2D), Kernel PCA (1D/2D)
- libnum spectral module required by mapper for new lenses
- All exposed in viz/index.html dropdowns

---

## Open Items / Future Work

### Already Decided (Not Yet Implemented)
None — all user-requested items from the Tier 3 plan are complete.

### Possible Future Extensions
1. **ARPACK wrapping**: For sparse eigensolve (dsaupd/dseupd). Only needed if datasets grow beyond what power iteration handles (~10k+ points)
2. **Sparse Naive Bayes**: BernoulliNB currently uses dense matrices. Swap in CSR for text/NLP pipelines
3. **UMAP at scale**: Use HNSW kNN instead of brute-force for N > 8192
4. **idx reshape/broadcast**: NumPy-style convenience functions. Use as needed, not prioritized
5. **Permutation importance**: Currently RF only has mean decrease in impurity. Permutation importance is more robust but slower
