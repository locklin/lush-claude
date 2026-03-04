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
| Split conformal (clf)  | Y   | Y    |          | Y        | *   |     |       |
| Split conformal (reg)  |     |      | Y        |          |     | *   | *     |
| predict-proba          | Y   |      |          | Y        | Y   |     |       |
| Mondrian CP            | Y   | Y    |          | Y        | *   |     |       |
| Venn-ABERS             | Y   | *    |          | Y        | *   |     |       |
| Predictive dist.       |     |      | Y        |          |     | *   | *     |
| Mondrian regression    |     |      | Y        |          |     | *   | *     |
| AgACI                  | Y   | Y    | Y        | Y        | Y   | *   | *     |
| Anomaly detection      |     | Y    |          |          |     |     |       |

Y = implemented, * = planned/natural extension

## 5. Implementation Priority

1. **Mondrian CP** (classification) - highest impact, straightforward
2. **Venn-ABERS** - calibrated probabilities are very useful
3. **Conformal predictive distributions** - easy for regression, very informative
4. **Split conformal for lm/ridge** - fill the gap for simpler regression models
5. **Mondrian regression** - conditional coverage for regression
6. **AgACI** - time series applications
7. **Anomaly detection** - unsupervised use case
8. **Semi-supervised CP** - niche, requires more theory

## 6. Design Principles

- **Split conformal by default**: Simpler, faster, no retraining
- **Consistent API**: `conformal-predict` method on model classes, standalone functions for non-class algorithms
- **Return types**: Classification = list of lists (prediction sets), Regression = Nx2 matrix (intervals)
- **Pure interpreted calibration**: Calibration sets are small (100s-1000s), no need for compiled C
- **No external dependencies**: Everything builds on existing mlcore/libnum
