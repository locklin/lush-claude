# mlcore Package -- Design Notes

## Architecture

The `mlcore` package (`packages/mlcore/`) is Lush's scikit-learn equivalent: a
collection of machine-learning primitives sharing a common compilation pipeline
(dhc-make -> gcc -> mod-load) and consistent API conventions.

### Sub-modules

| Module             | File                | Purpose                                          |
|--------------------|---------------------|--------------------------------------------------|
| model-selection    | model-selection.lsh | Train/test split, k-fold CV, stratified, metrics  |
| feature-scaling    | feature-scaling.lsh | StandardScaler, MinMaxScaler, RobustScaler        |
| optimizers         | optimizers.lsh      | Coordinate descent, soft thresholding, elastic net|
| regression         | regression.lsh      | lm, ridge, logistic, glmnet (+ per-model CP)     |
| knn                | knn.lsh             | Brute/VP-tree/KD-tree/HNSW search, classify, CP  |
| naive-bayes        | naive-bayes.lsh     | GaussianNB, MultinomialNB, BernoulliNB            |
| rf                 | rf.lsh              | RandomForest (classify/regress), predict-proba, CP|
| kmeans             | kmeans.lsh          | k-means++ init, Lloyd's algorithm, compiled C     |
| spectral           | spectral.lsh        | Laplacian eigenmaps, diffusion maps, kernel PCA   |
| sparse             | sparse.lsh          | SparseCSR class, compiled C SpMV                  |
| conformal          | conformal.lsh       | Centralized conformal prediction methods          |

The top-level `mlcore.lsh` loads all sub-modules.  `mlcore.hlp` provides the
helptool documentation.

### Design principles

- **Compiled C for hot paths**: Inner loops (distance computations, matrix ops,
  PAVA, residual calculation) are compiled via dhc-make.  Outer orchestration
  (calibration, model fitting) stays interpreted since those loops are O(n_cal)
  with n_cal typically small.
- **No external dependencies**: Everything builds on Lush core and
  `packages/libnum`.
- **Generic + wrapper pattern** (conformal module): Each conformal method has a
  generic function taking a callback (`predict-fn` or `proba-fn`) plus thin
  convenience wrappers for specific models.  Adding a new model never requires
  touching conformal.lsh.
- **Return type conventions**: Classification methods return lists of lists
  (prediction sets).  Regression methods return Nx2 matrices (intervals).


## API Summary

### Model Selection & Utilities

- `(train-test-split X y ratio)` -- stratified or random split
- `(k-fold-cv model-fn X y k metric-fn)` -- k-fold cross-validation
- `(accuracy y-true y-pred)`, `(mse ...)`, `(mae ...)`, `(r-squared ...)`, `(log-loss ...)`

### Conformal Prediction

Conformal prediction provides distribution-free, finite-sample-valid uncertainty
quantification.  Coverage guarantee: P(Y in C(X)) >= 1-alpha under
exchangeability.

#### Per-model methods (in their respective modules)

These were the original implementations; they remain for backward compatibility.

- `(==> rf conformal-predict cal-data cal-targets test-data alpha)` -- RF split
  conformal classification via vote-fraction nonconformity scores
- `(knn-conformal-predict cal-data cal-labels test-data k alpha)` -- kNN split
  conformal classification via distance-ratio scores
- `(==> glmnet conformal-predict cal-X cal-y test-X lambda alpha)` -- GLMnet
  split conformal; intervals (gaussian) or prediction sets (binomial)

#### Centralized module (`conformal.lsh`)

All methods below are in conformal.lsh with 119 tests in
`tests/test-conformal.lsh`.

**Classification (Mondrian CP)**:
- `(conformal-mondrian-classify proba-fn cal-data cal-labels test-data alpha)`
  -- generic; separate calibration per class, guarantees per-class coverage
- Wrappers: `conformal-mondrian-rf`, `conformal-mondrian-nb`,
  `conformal-mondrian-glmnet`, `conformal-mondrian-knn`

**Calibrated probabilities (Venn-ABERS)**:
- `(venn-abers-predict score-fn cal-scores cal-labels test-scores)` -- returns
  calibrated [p_lower, p_upper] intervals per test point via isotonic regression
  (PAVA)
- Wrappers: `venn-abers-rf`, `venn-abers-glmnet`

**Conformal predictive distributions**:
- `ConformalPredDist` class with `cdf`, `quantile`, `interval` methods --
  produces a full predictive CDF (step function) from calibration residuals
- Wrappers: `conformal-pred-dist-lm`, `conformal-pred-dist-glmnet`

**Split conformal regression**:
- `(conformal-regression-interval predict-fn cal-X cal-y test-X alpha)` --
  absolute-residual nonconformity scores
- Wrappers: `conformal-regression-lm`, `conformal-regression-ridge`

**Mondrian regression**:
- `(conformal-mondrian-regression predict-fn cal-X cal-y test-X alpha [n-bins])`
  -- quantile-binned adaptive intervals (wider where model is uncertain)

**Online adaptive (AgACI)**:
- `AgACI` class with `update`, `get-threshold`, `predict-interval` --
  adapts alpha_t over time for non-exchangeable (time series) data per
  Zaffran et al. (2022)

**Anomaly detection**:
- `(conformal-anomaly-detect train-data test-data alpha [k])` -- kNN-based
  one-class conformal; flags test points as anomalous if kNN distance exceeds
  calibrated quantile threshold

**Jackknife+ (LOO-based)**:
- `(jackknife-plus model-fn predict-fn train-x train-y test-x alpha)` -- generic
  LOO retraining; coverage >= 1 - 2*alpha (Barber, Candes, Ramdas, Tibshirani 2021)
- `(jackknife-plus-lm lm train-x train-y test-x alpha)` -- O(NP^2) via
  Sherman-Morrison, no retraining needed
- `(jackknife-plus-ridge train-x train-y lambda test-x alpha)` -- O(NP^2) via
  Sherman-Morrison

**CV+ (K-fold-based)**:
- `(cv-plus model-fn predict-fn train-x train-y test-x alpha [k])` -- generic
  K-fold variant; coverage >= 1 - 2*alpha
- Convenience wrappers: `cv-plus-lm`, `cv-plus-rf`, `cv-plus-glmnet`

#### Applicability matrix

| Method              | RF | kNN | GLMnet-G | GLMnet-B | NB | lm | ridge |
|---------------------|:--:|:---:|:--------:|:--------:|:--:|:--:|:-----:|
| Split conformal clf |  Y |  Y  |          |    Y     |    |    |       |
| Split conformal reg |    |     |    Y     |          |    |  Y |   Y   |
| Mondrian CP         |  Y |  Y  |          |    Y     | Y  |    |       |
| Venn-ABERS          |  Y |     |          |    Y     |    |    |       |
| Predictive dist     |    |     |    Y     |          |    |  Y |       |
| Mondrian regression |    |     |          |          |    |  Y |       |
| AgACI               |  Y |  Y  |    Y     |    Y     | Y  |  Y |   Y   |
| Anomaly detection   |    |  Y  |          |          |    |    |       |
| Jackknife+          |    |     |          |          |    |  Y |   Y   |
| CV+                 |  Y |     |    Y     |          |    |  Y |       |


## Implementation Details

### Compiled C helpers (dhc-make)

Five compiled C functions handle the performance-critical inner loops of conformal
prediction:

- **`_cp-pava`**: Pool Adjacent Violators Algorithm for isotonic regression.
  O(n), single pass.  Used by Venn-ABERS.
- **`_cp-bsearch`**: Binary search in a sorted double array.  Used by
  ConformalPredDist CDF evaluation.
- **`_cp-abs-residuals`**: Vectorized |y - yhat| computation.  Used by all
  regression conformal methods.
- **`_cp-jkplus-intervals`**: Builds sorted lo/hi arrays from LOO predictions
  and residuals, then extracts the (1-alpha) quantile interval.  Used by
  Jackknife+.
- **`_cp-loo-apply`**: Computes LOO test predictions via a cross-influence
  matrix G = X_test (X'X)^{-1} X'.  This is the key to O(NP^2) Jackknife+
  for linear models (Sherman-Morrison avoids retraining n models).

### Jackknife+ Sherman-Morrison optimization

For linear models (lm, ridge), naively retraining n LOO models would be O(n *
P^2).  Instead:

1. Fit once: beta = (X'X)^{-1} X'y
2. Hat matrix: H = X (X'X)^{-1} X'
3. LOO prediction for point i: yhat_{-i}(x_i) = (yhat_i - H_ii * y_i) / (1 - H_ii)
4. LOO residual: r_i = (y_i - yhat_i) / (1 - H_ii)
5. For test points: LOO test predictions via cross-influence matrix G

The compiled `_cp-loo-apply` handles step 5.  Verified to match generic LOO
retraining to 1e-6 tolerance.

### kNN Mondrian optimization

The original kNN Mondrian implementation was O(n_cal^2) interpreted and took
13.4 seconds at n_cal=500.  The fix: stack the query and calibration data into
a combined matrix, call the compiled `_knn-brute-search` once, then filter
results to calibration-row neighbors only.  Result: 0.2 seconds at n_cal=500
(67x speedup).

### Performance benchmarks

Measured on Linux x86_64, single core.  n_cal=200, n_test=20, conformal method
only (model training excluded).

| Method                         | n_cal=200 | n_cal=500 | Bottleneck                     |
|--------------------------------|-----------|-----------|--------------------------------|
| Mondrian CP (RF, 50 trees)     | 0.108 s   | 0.261 s   | RF predict-proba (interpreted) |
| Mondrian CP (NB)               | 0.002 s   | ~0.005 s  | Compiled NB likelihood         |
| Mondrian CP (kNN, k=5)         | 0.028 s   | 0.208 s   | Compiled brute kNN search      |
| Venn-ABERS (RF)                | 0.116 s   | 0.265 s   | 2*n_test RF predict-proba      |
| Split conformal (lm)           | <0.001 s  | <0.001 s  | Compiled residuals + sort      |
| Mondrian regression (lm, 5bin) | 0.001 s   | ~0.002 s  | Compiled residuals + sort      |
| Conformal pred dist (lm)       | <0.001 s  | <0.001 s  | Sort only                      |
| Anomaly detection (kNN, k=5)   | 0.004 s   | 0.006 s   | Compiled brute kNN             |

Practical guidance:
- n_cal up to ~1000: all methods well under 1 second
- n_cal ~5000: RF-based methods take a few seconds; regression methods instant
- n_cal ~50K: would need compiled RF tree traversal and/or VP-tree kNN backend


## Known Issues / Limitations

### AgACI memory accumulation
`AgACI.get-threshold` accumulates scores in a Lush list and sorts on every call.
For streams beyond ~10K steps, this becomes O(n) list traversal per update.  A
ring-buffer / windowed approach backed by an idx would eliminate this.  Not yet
implemented because current usage is well under 10K steps.

### RF predict-proba is interpreted
RF tree traversal in `predict-proba` iterates over tree arrays in Lush
interpreted code.  This is the dominant bottleneck for all RF-based conformal
methods.  Compiling the tree traversal into C would be the single largest
performance improvement across the conformal module.

### Venn-ABERS scaling
Venn-ABERS is inherently O(n_test * n_cal * log(n_cal)) due to sorting augmented
arrays for each test point and candidate label.  PAVA itself is O(n_cal).  For
large calibration sets, insertion sort could be replaced with `idx-d1sortup` on
contiguous copies, but RF predict-proba calls dominate in practice.

### Jackknife+ coverage guarantee
Jackknife+ and CV+ provide coverage >= 1 - 2*alpha (not 1 - alpha like split
conformal).  This is a theoretical limitation of the method, not an
implementation issue.

### Semi-supervised conformal not implemented
Using unlabeled data to sharpen prediction sets (transductive conformal with
pseudo-labels or density-ratio weighting) is a potential future direction but
has not been implemented.

### References
- Barber, Candes, Ramdas, Tibshirani (2021). "Predictive Inference with the
  Jackknife+" Annals of Statistics 49(1), 486-507.
- Zaffran et al. (2022). "Adaptive Conformal Predictions for Time Series"
- Vovk (2015). "Cross-conformal predictors" Annals of Mathematics and AI 74.
