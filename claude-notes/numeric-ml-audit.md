# Lush Numeric & ML Capability Audit

Date: 2026-03-03

Audit of what exists and what's missing to build Lush into a complete ML
development environment comparable to R or Python's scikit-learn.

## 1. Linear Algebra

### What EXISTS

**High-level wrappers (libnum/linalgebra.lsh)** — the best stuff:

| Capability | Functions | Notes |
|---|---|---|
| Eigendecomp (symmetric) | `eigen-symm`, `eigen-symmv` | sorted descending |
| Eigendecomp (Hermitian) | `eigen-herm`, `eigen-hermv` | complex |
| SVD | `svd`, `svd-inplace` | Golub-Reinsch, M >= N |
| LU decomposition | `lu-decomp`, `lu-decomp-complex` | real and complex |
| Cholesky | `cholesky-decomp` | symmetric positive definite |
| Linear solve (SVD) | `solve-sv` | least-squares |
| Linear solve (Householder) | `solve-hh` | |
| Linear solve (LU) | `solve-lu`, `solve-lu-complex` | real and complex |
| Matrix inverse | `inverse-lu`, `inverse-lu-complex` | via LU |
| Determinant | `determinant-lu`, `log-determinant-lu`, `sign-determinant-lu` | complex variants too |
| IDX-to-GSL bridge macros | `IDX2GSL_MATRIX`, `IDX2GSL_VECTOR`, `IDX2GSL_PERMUTATION` | |

**BLAS (packages/blas/)** — complete Level 1/2/3 for all four types (s/d/c/z).

**LAPACK (packages/lapack/)** — massive auto-generated FORTRAN interface
(~10,000+ lines). All the raw routines are there: `dgesv`, `dgeev`, `dgesvd`,
`dgesdd`, `dgetrf`/`dgetri`, `dgels`/`dgelss`/`dgelsd`, `dgeqrf`/`dgeqr2`,
`dgees`, `dpotrf`/`dpotrs`, etc. But these are raw FORTRAN stubs — no
high-level Lush wrappers for most of them.

### What's MISSING

- **QR decomposition**: In LAPACK (`dgeqrf`) but NO high-level wrapper
- **General (non-symmetric) eigendecomp**: LAPACK `dgeev` exists raw, no wrapper
- **Matrix rank**: Must compute SVD and count manually
- **Condition number**: Must compute from SVD manually
- **Matrix norms** (Frobenius, 1-norm, inf-norm): Not wrapped
- **Pseudoinverse (pinv)**: Must construct from SVD manually
- **Schur decomposition**: In LAPACK but no wrapper
- **Matrix exponential/logarithm**: Not available
- **Kronecker product**: Not available
- **Sparse matrices**: Completely absent


## 2. Statistics

### What EXISTS

**High-level stats (libnum/stats.lsh)** — clean GSL wrappers:

`stat-mean`, `stat-variance`, `stat-sd`, `stat-skew`, `stat-kurtosis`,
`stat-covariance` (pairwise, two vectors), `stat-absdev`, `stat-median`
(requires sorted input), `stat-quantile` (requires sorted input),
`stat-autocorr` (lag-1). Weighted variants: `stat-wmean`, `stat-wvariance`,
`stat-wsdev`, `stat-wabsdev`, `stat-wskew`, `stat-wkurtosis`.

**Probability distributions (gsl/randist.lsh)** — ~30 distributions with
random sampling AND PDFs: Gaussian, uniform, exponential, chi-squared,
t, F, beta, gamma, binomial, Poisson, geometric, hypergeometric, Cauchy,
Rayleigh, Weibull, Pareto, lognormal, logistic, Laplace, Levy, Gumbel,
Bernoulli, etc. Also random directions, shuffle, choose, sample.

**Histograms (gsl/histogram.lsh)** — full 1D and 2D with PDF sampling.

**Raw GSL statistics (gsl/statistics.lsh)** — includes `ttest` (two-sample).

### What's MISSING

- **CDFs** (cumulative distribution functions): `gsl_cdf_*` NOT wrapped.
  Cannot compute p-values, quantiles of distributions, or pnorm/qnorm.
  **This is a critical gap** — blocks all statistical inference.
- **Correlation matrix / covariance matrix**: Only pairwise `stat-covariance`
  between two vectors. No `cov(X)` or `cor(X)` for a data matrix.
- **Hypothesis tests**: Only raw `gsl_stats_char_ttest`. Missing: chi-squared,
  F-test, KS test, Wilcoxon, Mann-Whitney, ANOVA.
- **Confidence intervals**: Not provided
- **Multivariate normal distribution**: Not available
- **Kernel density estimation**: Not available


## 3. ML Algorithms

### What EXISTS

| Package | Location | What it provides |
|---|---|---|
| **SVM** | `packages/svm/` | SVMKernel classes (linear, poly, RBF), LIBSVM interface, LASVM online SVM |
| **Neural nets** | `packages/gblearn2/` | LeCun's gradient-based learning: full-connect, conv, subsamp, sigmoid, RBF layers. Fprop/bprop/bbprop. LeNet-5. Trainers, meters, data sources |
| **XGBoost** | `packages/xgboost/` | Full 3.2.0 wrapper: DMatrix, train, predict, save/load |
| **Clustering** | `packages/mapper/cluster.lsh` | SLINK (single-linkage), DBSCAN |
| **Distance metrics** | `packages/mapper/metrics.lsh` | euclidean, manhattan, chebyshev, cosine, correlation, hamming, spearman, kendall, hellinger |
| **Mapper (TDA)** | `packages/mapper/` | Full pipeline: lenses, covers, clustering, graph, visualization |
| **Simple linear fit** | `packages/gsl/fit.lsh` | `gsl_fit_linear`, `gsl_fit_wlinear`, `gsl_fit_mul` |
| **Multivariate fit** | `packages/gsl/multifit.lsh` | `gsl_multifit_linear`, `gsl_multifit_wlinear`, Levenberg-Marquardt nonlinear |
| **k-NN** | `packages/sn28/` | Legacy implementation only |
| **Torch** | `packages/torch/` | Interface to pre-PyTorch Torch library |

### What's MISSING

- **K-Means clustering**: Only SLINK/DBSCAN exist
- **PCA**: Only a mapper lens function, no standalone implementation
- **Logistic regression**: Not available standalone
- **Linear regression (high-level)**: Raw GSL fit exists but no R-style `lm()`
  with coefficients, R-squared, p-values, standard errors
- **Random forests / decision trees (CART)**: Not available
- **Naive Bayes**: Not available
- **k-NN (modern)**: Only legacy SN28
- **Ridge / Lasso / Elastic Net**: Not available
- **GMMs**: Only through Torch interface
- **t-SNE / UMAP / MDS**: Not available


## 4. Optimization

### What EXISTS

| Capability | Location | Methods |
|---|---|---|
| **Multivariate minimization** | `gsl/multimin.lsh` | Conjugate gradient (FR, PR), BFGS, steepest descent |
| **1D minimization** | `gsl/min.lsh` | Golden section, Brent |
| **Root finding (1D)** | `gsl/roots.lsh` | Bisection, Brent, Newton, secant, Steffenson |
| **Root finding (nD)** | `gsl/multiroots.lsh` | Newton, hybrid methods |
| **Simulated annealing** | `gsl/siman.lsh` | `gsl_siman_solve` |
| **Monte Carlo integration** | `gsl/monte.lsh` | Plain, MISER, VEGAS |
| **Nonlinear least squares** | `gsl/multifit.lsh` | Levenberg-Marquardt |

### What's MISSING

- **Simplex / LP**: Not available
- **Convex optimization**: Not available
- **L-BFGS-B** (bounded BFGS): GSL has BFGS but not bounded variant
- **SGD / Adam / modern optimizers**: gblearn2 has internal gradient descent
  but no general-purpose implementations
- **Constrained optimization**: Not available


## 5. Data Preprocessing

### What EXISTS

- **DataTable** (`packages/datatable/`): Column-oriented data with select,
  filter, sort, group-by, join
- **ColumnarDB** (`packages/columnardb/`): SQL-like query language
- **CSV reading**: `packages/csvread/csvread.lsh`, also in mapper

### What's MISSING

- **Feature scaling** (StandardScaler, MinMaxScaler, RobustScaler)
- **One-hot encoding / label encoding**
- **Train/test split**
- **Cross-validation** (k-fold, stratified, leave-one-out)
- **Feature selection**
- **Missing value imputation**
- **Pipeline abstraction**


## 6. Model Evaluation

### What EXISTS

- gblearn2 meters: error rate tracking during training
- XGBoost: built-in eval metrics via params

### What's MISSING (essentially everything)

- Confusion matrix
- ROC curve / AUC
- Precision / recall / F1
- MSE / RMSE / MAE / R-squared (as standalone functions)
- Classification report
- Learning curves
- Grid search / hyperparameter tuning


## 7. Numerical Utilities

### What EXISTS (strong)

| Capability | Location |
|---|---|
| RNG | `gsl/rng.lsh` — mt19937, taus, etc. |
| Distribution sampling | `gsl/randist.lsh` — ~30 distributions |
| FFT | `gsl/fft.lsh` (GSL), `fftw/fftw.lsh` (FFTW3 with spectrogram) |
| Sorting | `libidx/idx-sort.lsh` — float/double/int, indexed sort, binary search |
| Interpolation | `libnum/interpolator.lsh` — linear, polynomial, cubic spline, Akima |
| Numerical integration | `gsl/integration.lsh` — QNG, QAG, QAGS, QAGI, QAWO, etc. |
| Numerical differentiation | `gsl/diff.lsh` — central, forward, backward |
| ODE solving | `gsl/ode-initval.lsh` — RK, Bulirsch-Stoer, adaptive |
| Polynomials | `gsl/poly.lsh` — eval, root finding, quadratic/cubic solvers |
| Special functions | `gsl/specfunc.lsh` — Airy, Bessel, Legendre, erf, gamma, beta, zeta, etc. |
| Complex numbers | `libnum/libcomplex.lsh` — full complex arithmetic |
| Chebyshev approx | `gsl/cheb.lsh` |
| Quasi-random | `gsl/qrng.lsh` — Sobol, Halton |
| Wavelets | `wavelets/wavelet.lsh` — DWT (GSL), MODWT, MRA, denoising |


## 8. Matrix Operations (libidx)

### What EXISTS

Core idx operations for float, double, int, ubyte:
- Matrix multiply (`idx-d2timesd2`, `idx-f2timesf2` — now BLAS-backed)
- Element-wise: dotc, addc, add, sub, mul, div
- Reductions: sum, sumsqr, avg, prod, sup, inf, indexmax, indexmin
- Special: lincomb, tanh, inv, sign, clip, entropy, logsum, logadd
- Tensor contractions: d3dotd3, d3dotd2, d4dotd1, etc.
- Outer products: subextd1, extd3, extd1
- Convolution: 1D/2D (idx-convol.lsh)
- Sort: sortup/sortdown, indexed sort, binary search
- Memory-mapped I/O: mmap-idx for all types
- View operations: select, narrow, unfold (no copy)

### What's MISSING

- **Broadcasting** (NumPy-style automatic): Must use explicit `idx-bloop`
- **Boolean / fancy indexing**: Not available
- **Concatenate / stack**: Not available
- **General reshape**: Must use narrow/unfold/select
- **Cumulative sum/product**: Not available
- **Unique / set operations**: Not available
- **Meshgrid**: Not available


## Priority Roadmap

### Tier 1 — Critical (blocks most ML workflows)

1. **CDF wrappers** — wrap `gsl_cdf_*` for p-values and distribution quantiles.
   Blocks all statistical inference. Straightforward to implement: the GSL
   functions exist, just need Lush bindings like randist.lsh.

2. **Covariance/correlation matrix** — `cov(X)` and `cor(X)` for an NxN data
   matrix. Can be built from BLAS dgemm (X'X after centering).

3. **PCA** — `eigen-symmv` of covariance matrix exists, but need a proper `pca`
   function: explained variance ratios, projection, inverse transform.
   Straightforward given existing SVD/eigen wrappers.

4. **Train/test split + cross-validation** — DONE. `model-selection.lsh`:
   shuffle, train-test-split, kfold, stratified variants, cross-validate.

5. **Model evaluation metrics** — DONE. In `model-selection.lsh`:
   accuracy, mse/rmse/mae, r-squared, confusion-matrix, precision,
   recall, f1-score, classification-report.

6. **Feature scaling** — DONE. `feature-scaling.lsh`:
   StandardScaler, MinMaxScaler, RobustScaler with fit/transform/inverse.

### Tier 2 — Important for completeness

7. **K-Means** — simple algorithm, pure C inner loop for distance computation.

8. **Logistic regression** — can build on GSL BFGS optimizer + existing
   matrix ops. Need sigmoid, cross-entropy loss, gradient.

9. **High-level linear regression** — wrap `gsl_multifit_linear` with R-style
   output: coefficients, std errors, t-values, p-values (needs CDFs from #1),
   R-squared, adjusted R-squared.

10. **Ridge / Lasso / Elastic Net** — Ridge is trivial (add lambda*I to X'X).
    Lasso needs coordinate descent. Elastic Net combines both.

11. **QR decomposition wrapper** — wrap LAPACK `dgeqrf`, straightforward.

12. **General eigendecomp wrapper** — wrap LAPACK `dgeev`.

13. **Pseudoinverse / rank / condition number** — build from existing SVD.

### Tier 3 — Nice to have

14. Random forests / decision trees
15. Naive Bayes
16. k-NN (modern)
17. t-SNE / UMAP
18. Sparse matrices
19. Concatenate / reshape / broadcast for idx


## Foundation Assessment

The existing foundation is strong. BLAS (complete, all types), LAPACK (complete
raw stubs), GSL (statistics, distributions, optimization, integration, FFT,
interpolation, special functions), SVM, gblearn2 (neural nets), XGBoost, and
the DataTable/ColumnarDB infrastructure provide serious computational muscle.

The biggest gaps are in the **glue layer** — the statistical and ML utility
functions that tie algorithms into practical workflows: metrics, preprocessing,
evaluation, convenient wrappers around existing raw routines. Most of Tier 1
could be built in pure interpreted Lush calling existing compiled primitives.
