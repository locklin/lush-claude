# Conformal Prediction in Lush: Design & Roadmap

## 1. Current State

### 1.1 Random Forest Conformal Prediction (`mlcore/rf.lsh`)
- **Method**: Split conformal with class vote fractions as nonconformity scores
- **Score**: `s(x,y) = 1 - P_hat(y|x)` where P_hat is the RF vote fraction
- **Coverage**: Marginal coverage >= 1-alpha (finite-sample valid)
- **API**: `(==> rf conformal-predict cal-data cal-targets test-data alpha)`
- **Returns**: List of prediction sets (list of class labels per test point)
- **Limitations**: Classification only; interpreted loop over trees per sample

### 1.2 k-NN Conformal Prediction (`mlcore/knn.lsh`)
- **Method**: Split conformal with distance-ratio nonconformity
- **Score**: `s(x,y) = d_k(x, same-class) / d_k(x, diff-class)`
  - k-th nearest same-class distance / k-th nearest different-class distance
- **Coverage**: Marginal coverage >= 1-alpha
- **API**: `(knn-conformal-predict cal-data cal-labels test-data k alpha)`
- **Returns**: List of prediction sets
- **Limitations**: O(n_cal * n_test * d) pure interpreted; calibration set should be small

### 1.3 GLMnet Conformal Prediction (`mlcore/regression.lsh`)
- **Gaussian family**: Absolute residual scores -> prediction intervals [y_hat-q, y_hat+q]
- **Binomial family**: 1-P(true class) scores -> prediction sets
- **API**: `(==> model conformal-predict cal-X cal-y test-X lambda alpha)`
- **Returns**: Nx2 interval matrix (gaussian) or list of prediction sets (binomial)

### 1.4 Utility: predict-proba (`mlcore/rf.lsh`)
- `(==> rf predict-proba data)` returns N x K probability matrix
- Enables external conformal methods to use RF probabilities

### 1.5 Utility: log-loss (`mlcore/model-selection.lsh`)
- `(log-loss y-true y-proba)` compiled C cross-entropy loss
- Useful for calibration diagnostics

## 2. Theoretical Background

### Split Conformal Prediction
Given calibration set {(X_i, Y_i)}_{i=1..n} and a nonconformity score function s:
1. Compute s_i = s(X_i, Y_i) for each calibration point
2. Find q = quantile(s_1,...,s_n, at level ceil((n+1)(1-alpha))/n)
3. For test point X: C(X) = {y : s(X,y) <= q}

**Guarantee**: P(Y_{n+1} in C(X_{n+1})) >= 1-alpha (exchangeability only)

### Key Properties
- **Distribution-free**: No assumptions on P(X,Y) beyond exchangeability
- **Finite-sample valid**: Not asymptotic; works for any n
- **Model-agnostic**: Any score function works, but better scores = tighter sets
- **Split vs Full**: Split uses separate calibration set (simpler, slight efficiency loss)

## 3. Roadmap: Advanced Methods

### 3.1 Mondrian Conformal Prediction (Priority: HIGH)
**What**: Class-conditional conformal prediction. Separate calibration per class.

**Why**: Standard conformal gives marginal coverage but can under-cover minority classes. Mondrian gives P(Y in C(X) | Y=c) >= 1-alpha for EACH class c.

**Algorithm**:
1. Partition calibration by class: S_c = {s_i : y_i = c}
2. Per-class quantile: q_c = quantile(S_c, level ceil((|S_c|+1)(1-alpha))/|S_c|)
3. Include class c in C(X) if s(X,c) <= q_c

**Applicability**: RF, k-NN, GLMnet (binomial), Naive Bayes -- any classifier

**Implementation**: ~50 lines per model, add `conformal-predict-mondrian` method

### 3.2 Venn-ABERS Prediction (Priority: HIGH)
**What**: Multi-probability predictor that outputs calibrated probability intervals [p_lower, p_upper] for each class.

**Why**: Gives both a prediction set AND calibrated probability estimates. More informative than raw conformal sets.

**Algorithm** (binary case):
1. For each candidate label y in {0,1}:
   a. Augment calibration set with (X_test, y)
   b. Fit isotonic regression of y on scores s
   c. Get calibrated probability p_y = isotonic(s(X_test))
2. Return [p_0, p_1] (one is a lower bound, one upper)

**Applicability**: Any binary classifier. Multi-class via one-vs-rest.

**Implementation**: Requires isotonic regression (Pool Adjacent Violators), ~100 lines compiled C.

### 3.3 Conformal Predictive Distributions (Priority: MEDIUM)
**What**: For regression, produce a full predictive CDF, not just an interval.

**Why**: Users get P(Y <= y | X) for any y, enabling arbitrary quantile extraction, decision-theoretic optimal actions, etc.

**Algorithm**:
1. Compute residuals r_i = Y_i - f(X_i) on calibration
2. Conformal predictive distribution: F(y|X) = (#{i: r_i <= y - f(X)} + 1) / (n+1)
3. This is a step function with n+1 steps

**Applicability**: lm-model, ridge, glmnet (gaussian), RF (regression)

**Implementation**: ~30 lines, returns a sorted residual vector + intercept

### 3.4 Mondrian Conformal Regression (Priority: MEDIUM)
**What**: Conditional coverage for regression by binning X-space.

**Why**: Standard conformal can give very wide intervals in regions with high variance and tight intervals where variance is low, despite having correct marginal coverage.

**Algorithm**:
1. Partition X-space into bins (k-means, quantiles of f(X), etc.)
2. Run separate conformal calibration per bin
3. Adaptive intervals: wider where model is uncertain, tighter where confident

**Applicability**: Any regression model

**Implementation**: ~60 lines, compose with existing k-means

### 3.5 Adaptive Conformal Inference / AgACI (Priority: MEDIUM-LOW)
**What**: Online conformal prediction that adapts alpha over time to maintain coverage on non-exchangeable (e.g., time series) data.

**Reference**: Zaffran et al. (2022), "Adaptive Conformal Predictions for Time Series"

**Algorithm**:
- Maintain running alpha_t that adjusts based on past coverage errors
- alpha_{t+1} = alpha_t + gamma * (alpha - err_t)
- err_t = 1 if Y_t not in C_t(X_t), 0 otherwise
- gamma is a step size (typical: 0.005-0.05)

**Applicability**: Any model applied to streaming/time-series data

**Implementation**: ~40 lines, pure interpreted. Stateful class with update method.

### 3.6 Semi-supervised Conformal (Priority: LOW)
**What**: Use unlabeled data to improve conformal efficiency.

**Why**: When labeled data is scarce but unlabeled data is abundant, can sharpen prediction sets by better estimating the score distribution.

**Algorithm**: Transductive conformal with pseudo-labels from unlabeled data, or density-ratio weighting.

**Applicability**: Any classifier with predict-proba

### 3.7 Conformal Anomaly Detection (Priority: LOW)
**What**: One-class conformal prediction for unsupervised anomaly detection.

**Algorithm**:
1. Score = distance to k-th nearest neighbor (or other density estimate)
2. Calibrate on "normal" training data
3. Flag test point as anomaly if score > quantile threshold

**Applicability**: k-NN, kernel density, isolation forest (future)

**Implementation**: ~30 lines, reuses knn-search

## 4. Applicability Matrix

| Method                  | RF | k-NN | GLMnet-G | GLMnet-B | NB | lm | ridge |
|------------------------|-----|------|----------|----------|-----|-----|-------|
| Split conformal (clf)  | Y   | Y    |          | Y        |     |     |       |
| Split conformal (reg)  |     |      | Y        |          |     | Y   | Y     |
| predict-proba          | Y   |      |          | Y        | Y   |     |       |
| Mondrian CP            | Y   | Y    |          | Y        | Y   |     |       |
| Venn-ABERS             | Y   |      |          | Y        |     |     |       |
| Predictive dist.       |     |      | Y        |          |     | Y   |       |
| Mondrian regression    |     |      |          |          |     | Y   |       |
| AgACI                  | Y   | Y    | Y        | Y        | Y   | Y   | Y     |
| Anomaly detection      |     | Y    |          |          |     |     |       |
| Jackknife+             |     |      |          |          |     | Y   | Y     |
| CV+                    | Y   |      | Y        |          |     | Y   |       |

Y = implemented (all above now implemented as of 2026-03-04)

## 5. Implementation Status (2026-03-04)

### Per-model methods (original, unchanged):
- `(==> rf conformal-predict ...)` in rf.lsh
- `(knn-conformal-predict ...)` in knn.lsh
- `(==> glmnet conformal-predict ...)` in regression.lsh

### Centralized module (`conformal.lsh`) — all 8 sections implemented:
1. **Mondrian CP**: `conformal-mondrian-classify` + 4 wrappers (RF, NB, GLMnet, kNN)
2. **Venn-ABERS**: `venn-abers-predict` + 2 wrappers (RF, GLMnet)
3. **Conformal predictive distributions**: `ConformalPredDist` class + 2 wrappers (lm, GLMnet)
4. **Split conformal for lm/ridge**: `conformal-regression-interval` + 2 wrappers
5. **Mondrian regression**: `conformal-mondrian-regression` (quantile-binned)
6. **AgACI**: `AgACI` class with update/get-threshold/predict-interval
7. **Anomaly detection**: `conformal-anomaly-detect` (kNN-based)
8. **Jackknife+ and CV+**: `jackknife-plus` (generic), `jackknife-plus-lm` (Sherman-Morrison),
   `jackknife-plus-ridge`, `cv-plus` (generic), `cv-plus-lm`, `cv-plus-rf`, `cv-plus-glmnet`

### Compiled C helpers:
- `_cp-pava`: Pool Adjacent Violators (isotonic regression), O(n)
- `_cp-bsearch`: binary search in sorted array
- `_cp-abs-residuals`: |y - yhat| vectorized
- `_cp-jkplus-intervals`: Jackknife+ interval construction from LOO preds/residuals
- `_cp-loo-apply`: Sherman-Morrison LOO test predictions via cross-influence matrix

### Tests: 119 tests in `packages/mlcore/tests/test-conformal.lsh`

## 6. Design Principles

- **Split conformal by default**: Simpler, faster, no retraining
- **Generic + wrapper pattern**: Each method has a generic function taking a callback `predict-fn`
  or `proba-fn`, plus thin convenience wrappers for specific models.  This makes it trivial to
  add new models without touching conformal.lsh.
- **Return types**: Classification = list of lists (prediction sets), Regression = Nx2 matrix (intervals)
- **Compiled C for hot paths**: PAVA, binary search, absolute residuals.  Calibration-phase loops
  (score gathering, quantile computation) remain interpreted since they're O(n_cal) and n_cal is
  typically small.
- **No external dependencies**: Everything builds on existing mlcore/libnum

## 7. Performance Analysis

Benchmarked on this machine (Linux x86_64, single core) with n_cal=200, n_test=20
unless noted.  Times are for the conformal method only (model training excluded).

| Method                         | n_cal=200 | n_cal=500 | Bottleneck                     |
|-------------------------------|----------|----------|--------------------------------|
| Mondrian CP (RF, 50 trees)     | 0.108 s  | 0.261 s  | RF predict-proba (interpreted) |
| Mondrian CP (NB)               | 0.002 s  | ~0.005 s | Compiled NB likelihood         |
| Mondrian CP (kNN, k=5)         | 0.028 s  | 0.208 s  | Compiled brute kNN search      |
| Venn-ABERS (RF)                | 0.116 s  | 0.265 s  | 2*n_test RF predict-proba      |
| Split conformal (lm)           | <0.001 s | <0.001 s | Compiled residuals + sort      |
| Mondrian regression (lm, 5bin) | 0.001 s  | ~0.002 s | Compiled residuals + sort      |
| Conformal pred dist (lm)       | <0.001 s | <0.001 s | Sort only                      |
| Anomaly detection (kNN, k=5)   | 0.004 s  | 0.006 s  | Compiled brute kNN             |

### Scaling observations:
- **RF-based methods** scale as O(n_cal * n_trees * depth) for predict-proba, which dominates.
  The RF tree traversal is interpreted (Lush `each` over tree arrays); compiling it into C
  would be the single biggest speedup for RF conformal methods.
- **kNN Mondrian** was originally 13.4s at n_cal=500 due to O(n_cal^2) interpreted distance
  loops.  Rewritten to use the compiled `_knn-brute-search` via a stacked-matrix strategy:
  [query; cal] combined matrix -> compiled kNN -> filter to cal-row neighbors only.
  Now 0.2s at n_cal=500 (67x speedup).
- **Regression methods** are effectively instant because `_cp-abs-residuals` is compiled C
  and `idx-d1sortup` is O(n log n) compiled sort.
- **Venn-ABERS** is inherently O(n_test * n_cal * log(n_cal)) due to sorting augmented arrays
  for each test point and candidate label.  The PAVA itself is O(n_cal).  For large calibration
  sets, the insertion sort could be replaced with idx-d1sortup on contiguous copies, but the
  RF predict-proba calls currently dominate.
- **AgACI** accumulates scores in a list.  For very long streams (>10K steps), converting to
  an idx-backed ring buffer would avoid O(n) list traversal in `get-threshold`.

### Practical guidance:
- Up to n_cal ~1000: all methods run in well under 1 second
- n_cal ~5000: RF-based methods take a few seconds; regression methods still instant
- n_cal ~50K: would need compiled RF traversal and/or the VP-tree kNN backend

## 8. Jackknife+ and CV+ (Implemented 2026-03-04)

### What it is
Jackknife+ (Barber, Candès, Ramdas, Tibshirani 2021) is a conformal method that
retrains the model on leave-one-out (LOO) subsets of the training data.  For n training
points, it trains n separate models, computes the LOO residual for each, then builds
prediction intervals using these more honest residuals.

### Coverage guarantee
P(Y_{n+1} in C(X_{n+1})) >= 1 - 2*alpha  (slightly weaker than split conformal's 1-alpha).
The "CV+" variant (using K-fold instead of LOO) achieves 1 - 2*alpha with K-fold models.

### Implementation (2026-03-04)

#### Generic functions:
- `(jackknife-plus model-fn predict-fn train-x train-y test-x alpha)` — LOO retraining
- `(cv-plus model-fn predict-fn train-x train-y test-x alpha [k])` — K-fold variant

#### Optimized wrappers (Sherman-Morrison LOO, no retraining):
- `(jackknife-plus-lm lm train-x train-y test-x alpha)` — O(NP^2), same as one OLS fit
- `(jackknife-plus-ridge train-x train-y lambda test-x alpha)` — O(NP^2)

#### Convenience wrappers:
- `(cv-plus-lm train-x train-y test-x alpha [k])`
- `(cv-plus-rf n-trees train-x train-y test-x alpha [k])`
- `(cv-plus-glmnet alpha-enet lam train-x train-y test-x alpha [k])`

#### Compiled C helpers:
- `_cp-jkplus-intervals`: builds sorted lo/hi arrays, extracts quantiles
- `_cp-loo-apply`: computes LOO test predictions via cross-influence matrix G

#### Verification:
- `jackknife-plus-lm` output matches generic `jackknife-plus` with lm callbacks to 1e-6
  (confirms Sherman-Morrison correctness)
- 40 new tests (119 total), all passing

### References
- Barber, Candès, Ramdas, Tibshirani (2021). "Predictive Inference with the Jackknife+"
  Annals of Statistics 49(1), 486-507.
- Romano, Patterson, Candès (2019). "Conformalized Quantile Regression" NeurIPS.
- Vovk (2015). "Cross-conformal predictors" Annals of Mathematics and AI 74, 9-28.
