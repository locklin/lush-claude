# Topological Data Analysis: Mapper Algorithm for Lush

## Implementation Plan

---

## Part 1: The Mapper Algorithm

### Mathematical Foundation

The Mapper algorithm (Singh, Memoli, Carlsson 2007) constructs a
simplicial complex that captures the topological shape of a dataset.

Given:
- A dataset X (N points in R^d)
- A filter function f: X -> R^k (the "lens," typically k=1 or k=2)
- A cover U = {U_i} of f(X) (overlapping intervals/hypercubes)
- A clustering algorithm C

The Mapper construction is:

1. **Project:** Compute f(x) for all x in X
2. **Cover:** Construct overlapping intervals on f(X)
3. **Pullback:** For each cover element U_i, collect f^{-1}(U_i) -- the
   original data points whose filter values fall in U_i
4. **Cluster:** Apply clustering algorithm C to each f^{-1}(U_i) using
   the **original** high-dimensional metric (not the filter metric)
5. **Nerve:** Build a graph: one node per cluster, edges between nodes
   that share data points (due to the cover overlap)

The result **M(f, U, C) = Nerve(pullback cover)** is a graph (1-skeleton
of the nerve) with rich metadata: each node knows which data points it
contains, and edges encode shared membership.

### Two-Lens Cartesian Product

When k=2 (two filter functions f1, f2), the cover is a 2D grid of
overlapping rectangles. Each hypercube is the Cartesian product of an
interval in f1-space and an interval in f2-space. KeplerMapper handles
this by allowing `n_cubes` and `perc_overlap` to be specified per
dimension: `Cover(n_cubes=[10,15], perc_overlap=[0.3,0.4])`.

### Why Mapper Is Useful

Mapper produces a compressed topological summary that reveals:
- Loops and flares in data (impossible to see in scatter plots)
- Subgroups and their relationships
- Where outliers or interesting phenomena concentrate

The graph can be colored by any variable (not just the filter), enabling
exploratory data analysis. For example: build topology on X, then color
by model residuals to see where a model fails.

---

## Part 2: The j-cluster Hierarchical Clustering Library

### Overview

The user's repository at https://github.com/locklin/j-cluster contains
a C hierarchical clustering library derived from Cluster 3.0 (de Hoon,
University of Tokyo). The actual computation lives in `hclust.c` and
`hclust.h`; the J language code is just an FFI wrapper.

### What It Implements

**Data structure:** `typedef struct {int left; int right; double distance;} Node;`
Standard dendrogram representation. N-1 Nodes for N elements.

**Distance metrics (11):**
- Euclidean ('e'), Manhattan/city-block ('b'), Chebyshev ('y')
- Pearson correlation ('c'), absolute correlation ('a')
- Uncentered correlation / cosine distance ('u'), absolute uncentered ('x')
- Spearman rank correlation ('s'), Kendall tau ('k')
- Angular distance ('o'), cosine similarity ('n')

**Linkage methods (4):**
- Single linkage ('s') — SLINK algorithm, O(N^2), optimal
- Complete linkage ('m') — naive, O(N^3)
- Average linkage / UPGMA ('a') — naive, O(N^3)
- Centroid linkage ('c') — O(N^3 * d), requires raw data

**Utilities:**
- `distancematrix()` — compute full pairwise distance matrix
- `cuttree()` — cut dendrogram at k clusters
- `treecluster()` — main entry point dispatching to method

### Bugs Found in j-cluster

| Bug | Location | Severity |
|-----|----------|----------|
| `cosine()` divides by `xx*yy` not `sqrt(xx*yy)` | cosine metric | Critical |
| `cosine()` returns similarity not distance | cosine metric | Critical |
| `angle()` same denominator bug | angle metric | Critical |
| `chebyshev()` uses `abs()` (int) on doubles | chebyshev metric | High |
| Centroid linkage sums instead of averaging | `pclcluster()` | High for L1/L2 |
| `sortdata` global static | sorting helper | Thread safety |

### Big-O Analysis

| Component | Time | Notes |
|-----------|------|-------|
| Distance matrix | O(N^2 * d) | For O(d) metrics |
| SLINK (single) | O(N^2) | Optimal |
| Complete/Average | O(N^3) | Could be O(N^2 log N) with NN-chain |
| Centroid | O(N^3 * d) | Recomputes from data |
| Kendall tau per-pair | O(d^2) | Should be O(d log d) |
| Spearman per-pair | O(d log d) | But allocates per call |

**The O(N^3) linkage methods are the main bottleneck for Mapper.**
In Mapper, clustering runs on each cover element independently. If
cover elements contain ~100-1000 points, O(N^3) is:
- 100 points: 10^6 operations — fine
- 1000 points: 10^9 operations — slow
- 10000 points: 10^12 — infeasible

For Mapper's per-bin clustering, the bin sizes are typically small
(100-1000), so O(N^3) is acceptable. But for large bins, DBSCAN
(O(N log N) with a spatial index) is the better choice.

### j-cluster vs scikit-learn/KeplerMapper Clustering: Tradeoffs

KeplerMapper delegates to scikit-learn, which uses substantially better
algorithms for hierarchical clustering:

| Aspect | j-cluster (hclust.c) | scikit-learn / SciPy |
|--------|---------------------|---------------------|
| **Single linkage** | SLINK O(N^2) — optimal | MST-based O(N^2) — also optimal |
| **Complete/Average** | Naive O(N^3) | **NN-chain O(N^2)** — one order better |
| **Ward's** | Not implemented | **NN-chain O(N^2)** |
| **Centroid** | O(N^3 * d), buggy | O(N^3) (not reducible, no NN-chain) |
| **Distance metrics** | 11 metrics (3 buggy) | Euclidean, Manhattan, cosine, etc. |
| **Code size** | ~900 lines C | ~500 lines Cython |
| **Thread safety** | No (global state) | Yes |

**The nearest-neighbor chain (NN-chain) algorithm** is the key difference.
It achieves O(N^2) for Ward, complete, and average linkage:

1. Maintain a stack of clusters
2. Push an arbitrary unmerged cluster
3. Find its nearest neighbor; if the neighbor is already on the stack
   (mutual nearest neighbor pair), merge them both. Otherwise push
   the neighbor and repeat.
4. Total work: < 3N^2 distance lookups

NN-chain works for any **reducible** linkage (Ward, complete, average,
weighted, single). It does NOT work for centroid or median linkage.

**Recommendation: Do NOT port j-cluster's hierarchical clustering code.
Instead, implement the NN-chain algorithm from scratch.** The reasons:

1. NN-chain gives O(N^2) for the linkage methods that matter (Ward,
   complete, average). j-cluster's O(N^3) naive approach is a full
   order of magnitude worse for Mapper's per-bin clustering.
2. j-cluster's single-linkage (SLINK) is good, but NN-chain handles
   single linkage too, so a single implementation covers all methods.
3. j-cluster has several critical bugs (cosine, angle, chebyshev,
   centroid). Fixing them while porting adds risk.
4. j-cluster's `double**` ragged array interface is incompatible with
   Lush's idx system and cache-unfriendly.
5. The NN-chain algorithm is ~200-300 lines of C — comparable to
   porting and fixing j-cluster, with a better result.

**What IS worth taking from j-cluster:**

1. The **distance metric functions** (after bug fixes) — Euclidean,
   Manhattan, Pearson, Spearman, Kendall, Chebyshev. These are
   straightforward per-pair distance calculations.
2. The **dendrogram Node struct** and **cuttree** utility — simple and
   standard.
3. The **SLINK algorithm** for single linkage, if we want a dedicated
   fast path separate from NN-chain.

**What to implement fresh:**

1. **NN-chain** for Ward/complete/average/weighted linkage
2. **Hamming distance** metric (absent from j-cluster)
3. **Contiguous lower-triangle distance matrix** (not ragged array)
4. **Lush idx interface** (data as idx2, results as idx1)

---

## Part 3: DBSCAN as an Alternative Clusterer

### Why DBSCAN for Mapper

KeplerMapper uses DBSCAN (or scikit-learn's agglomerative clustering)
as the default per-bin clusterer. DBSCAN has properties that make it
well-suited for Mapper:

- **No need to specify number of clusters** — it discovers them
- **Handles noise** — outlier points get label -1
- **O(N log N)** with a KD-tree or ball tree spatial index
- **O(N^2)** without spatial index (still better than O(N^3))

### DBSCAN Algorithm

```
DBSCAN(points, eps, min_pts):
  For each unvisited point p:
    Mark p as visited
    N = neighbors(p, eps)   // all points within eps distance
    If |N| < min_pts:
      Mark p as noise
    Else:
      Create new cluster C
      Add p to C
      For each q in N:
        If q not visited:
          Mark q as visited
          N' = neighbors(q, eps)
          If |N'| >= min_pts: N = N ∪ N'
        If q not in any cluster: Add q to C
```

**Complexity:**
- O(N^2) with brute-force neighbor search
- O(N log N) with KD-tree (for Euclidean / Lp metrics)
- O(N^2) for general metrics (no spatial index available)

### DBSCAN with Multiple Metric Spaces

DBSCAN only needs a distance function and an epsilon threshold. It
works with any metric space:
- **Euclidean:** KD-tree gives O(N log N)
- **Cosine:** After normalizing to unit sphere, use Euclidean KD-tree
- **Hamming:** For low-dimension binary data, bit manipulation tricks
- **Arbitrary metric:** Fall back to brute-force O(N^2) neighbor search

For Mapper's per-bin clustering (bin sizes 100-1000), O(N^2) brute-force
DBSCAN is likely fast enough. KD-tree acceleration is a nice-to-have.

---

## Part 4: Implementation Design for Lush

### Package Structure

```
packages/mapper/
  mapper.lsh            ;; Main Mapper API
  cover.lsh             ;; Cover class (intervals/hypercubes)
  cluster.lsh           ;; Clustering dispatch (hclust or DBSCAN)
  metrics.lsh           ;; Metric space definitions
  nerve.lsh             ;; Graph construction from clustered bins
  graph.lsh             ;; Graph data structure with node metadata
  visualize.lsh         ;; D3 HTML output
  mapper-config.lsh     ;; Package configuration

  ;; C acceleration
  hclust.c              ;; Hierarchical clustering (from j-cluster, fixed)
  hclust.h              ;; Header
  dbscan.c              ;; DBSCAN implementation
  metrics.c             ;; Compiled distance functions (SIMD where possible)
```

### Core Data Structures

**MapperGraph — the output of Mapper:**
```lisp
(defclass MapperGraph object
  ;; Nodes: each node is a cluster from one cover bin
  ((-int-) num-nodes)
  ((-idx1- (-int-)) node-sizes)          ;; points per node
  ((-obj- (Pool)) node-members)          ;; list of point-index arrays

  ;; Edges: pairs of nodes sharing data points
  ((-int-) num-edges)
  ((-idx2- (-int-)) edges)               ;; (N x 2) edge list
  ((-idx1- (-int-)) edge-weights)        ;; shared point count per edge

  ;; Metadata for coloring/analysis
  ((-idx2- (-real-)) node-values)        ;; (num-nodes x num-vars) summary stats
  ;; node-values[i][j] = mean of variable j over points in node i
)
```

**Cover — the overlapping interval structure:**
```lisp
(defclass MapperCover object
  ((-idx1- (-int-)) n-cubes)            ;; cubes per dimension
  ((-idx1- (-real-)) perc-overlap)       ;; overlap per dimension
  ((-int-) n-dims)                       ;; 1 or 2 (number of lenses)

  ;; Computed by fit:
  ((-idx2- (-real-)) centers)            ;; (n-hypercubes x n-dims)
  ((-idx1- (-real-)) radii)              ;; radius per dimension
)
```

### The Mapper Pipeline

```lisp
(defmethod Mapper fit (X lens &optional cover clusterer)
  ;; X: (N x d) idx2 of data
  ;; lens: (N x k) idx2 of filter values (k=1 or k=2)
  ;; cover: MapperCover object (default: 10 cubes, 50% overlap)
  ;; clusterer: 'hclust or 'dbscan (default: 'hclust)

  ;; Step 1: Fit cover to lens values
  (==> cover fit lens)

  ;; Step 2: For each hypercube in cover
  (let ((hypercubes (==> cover transform lens))
        (all-nodes ())
        (node-id 0))

    (each ((hc hypercubes) (bin-idx (range (length hypercubes))))
      ;; hc = indices of points falling in this hypercube
      (when (>= (length hc) min-cluster-samples)

        ;; Step 3: Cluster the pullback (original data, not lens values)
        (let* ((bin-data (select-rows X hc))
               (labels (cluster bin-data clusterer metric)))

          ;; Step 4: Create one node per cluster
          (for (k 0 (max-label labels))
            (let ((members (filter-by-label hc labels k)))
              (when (> (length members) 0)
                ;; Record node: its member point indices
                (add-node node-id members)
                (incr node-id)))))))

    ;; Step 5: Compute nerve (edges from shared points)
    (compute-nerve all-nodes min-intersection)

    ;; Step 6: Compute node summary statistics for coloring
    (compute-node-values all-nodes X color-variables)

    ;; Return MapperGraph
    graph))
```

### Cover Implementation Details

Following KeplerMapper's math:

```lisp
(defmethod MapperCover fit (lens)
  ;; lens: (N x k) array of filter values
  ;; Compute bounds per dimension
  (let* ((bounds-lo (idx-min-along-dim lens 0))
         (bounds-hi (idx-max-along-dim lens 0))
         (ranges (- bounds-hi bounds-lo)))

    ;; Compute radius per dimension:
    ;; radius = range / (2 * n_cubes * (1 - perc_overlap))
    (for (d 0 n-dims)
      (radii d (/ (ranges d)
                  (* 2 (n-cubes d) (- 1 (perc-overlap d))))))

    ;; Compute centers: linspace from lo+radius to hi-radius
    ;; For 2D: Cartesian product of 1D center arrays
    (let ((centers-per-dim (list)))
      (for (d 0 n-dims)
        (let ((lo (+ (bounds-lo d) (radii d)))
              (hi (- (bounds-hi d) (radii d)))
              (n  (n-cubes d)))
          (push (linspace lo hi n) centers-per-dim)))

      ;; For k=1: centers is just the 1D array
      ;; For k=2: Cartesian product
      (if (= n-dims 1)
        (setq centers (car centers-per-dim))
        (setq centers (cartesian-product centers-per-dim))))))

(defmethod MapperCover transform (lens)
  ;; For each hypercube, find points within [center-radius, center+radius]
  (let ((result ()))
    (for (i 0 (num-hypercubes))
      (let ((mask (ones-idx (idx-dim lens 0))))
        (for (d 0 n-dims)
          ;; Points within [center[i][d] - radius[d], center[i][d] + radius[d]]
          (idx-and mask
            (idx-and (>= (select-col lens d) (- (centers i d) (radii d)))
                     (<= (select-col lens d) (+ (centers i d) (radii d))))))
        (push (where mask) result)))
    result))
```

### Nerve Computation

The nerve creates edges between nodes that share data points:

```lisp
(defmethod MapperGraph compute-nerve (nodes min-intersection)
  ;; For each pair of nodes, count shared members
  ;; Nodes from the SAME bin cannot share members (they're different clusters)
  ;; Nodes from OVERLAPPING bins can share members
  (for (i 0 num-nodes)
    (for (j (+ i 1) num-nodes)
      (when (different-bins i j)
        (let ((shared (set-intersection (node-members i) (node-members j))))
          (when (>= (length shared) min-intersection)
            (add-edge i j (length shared))))))))
```

**Optimization:** Rather than checking all N^2 node pairs, only check
pairs from overlapping bins. Build an adjacency structure on the cover
elements first (which bins overlap?), then only check nodes from
adjacent bins. This reduces the search space substantially.

### Metric Space Abstraction

```lisp
(defclass MetricSpace object
  ((-str-) name)
  ;; The metric function: (metric data i j) -> distance
  ((-gptr-) metric-fn))   ;; C function pointer for speed

;; Predefined metrics
(setq *euclidean*   (new MetricSpace "euclidean"   euclidean-c))
(setq *manhattan*   (new MetricSpace "manhattan"   manhattan-c))
(setq *cosine*      (new MetricSpace "cosine"      cosine-c))
(setq *hamming*     (new MetricSpace "hamming"     hamming-c))
(setq *correlation* (new MetricSpace "correlation" correlation-c))
(setq *chebyshev*   (new MetricSpace "chebyshev"   chebyshev-c))
```

The C metric functions are compiled from `metrics.c` and passed as
function pointers to the clustering routines. This avoids the overhead
of Lush function calls in the inner distance computation loop.

---

## Part 5: D3 Visualization

### KeplerMapper's Approach

KeplerMapper generates a self-contained HTML file containing:
- The graph data embedded as JSON
- D3.js (v4) for force-directed layout
- Interactive features: zoom, pan, hover tooltips, node selection

The visualization uses:
- **Force-directed layout** (d3-force) for node positioning
- **Node radius** proportional to cluster size
- **Node color** from a continuous colormap (viridis, plasma, etc.)
  based on the mean of a coloring variable over points in the node
- **Edge thickness** proportional to intersection size

### Generating D3 from Lush

The plan is to generate an HTML file from Lush, not to embed a browser.

```lisp
(defmethod MapperGraph visualize (filename &optional color-fn title)
  ;; Generate self-contained HTML with embedded D3.js
  (let ((fp (open-write filename)))
    ;; Write HTML header with embedded D3.js (or CDN link)
    (write-html-header fp title)

    ;; Write graph data as JSON
    (fprintf fp "<script>\nvar graphData = ")
    (write-graph-json fp this color-fn)
    (fprintf fp ";\n</script>\n")

    ;; Write D3 visualization code
    (write-d3-code fp)

    ;; Write interactive controls
    (write-controls fp)

    (close fp)))
```

**The JSON structure:**

```json
{
  "nodes": [
    {"id": 0, "size": 42, "color": 0.73, "members": [1, 5, 7, ...]},
    {"id": 1, "size": 38, "color": 0.45, "members": [2, 3, 8, ...]},
    ...
  ],
  "edges": [
    {"source": 0, "target": 1, "weight": 5},
    ...
  ],
  "metadata": {
    "n_points": 10000,
    "n_cubes": 10,
    "overlap": 0.5,
    "metric": "euclidean",
    "clusterer": "single_linkage"
  }
}
```

### Interactive Features

**Minimum viable:**
1. Force-directed layout with zoom/pan
2. Nodes sized by cluster membership count
3. Nodes colored by mean of a user-specified variable
4. Hover tooltip showing node ID, size, color value
5. Click to select node (highlight it and its neighbors)

**Extended (for subset analysis):**
6. Lasso/rectangle selection of multiple nodes
7. "Compare selected vs rest" button — computes summary statistics
   on the union of points in selected nodes vs all other points
8. Color variable selector dropdown (switch coloring on the fly)
9. Node label overlay (show which cover bin produced each node)
10. Export selected point indices to clipboard/file

### D3 Code Complexity Estimate

The D3 visualization is essentially:
- ~50 lines of HTML/CSS boilerplate
- ~100 lines of D3 force simulation setup
- ~50 lines of node/edge rendering
- ~100 lines of interaction handlers (hover, click, zoom)
- ~50 lines of the JSON serialization in Lush

Total: ~350 lines of embedded JavaScript, ~100 lines of Lush for
the serialization. This is moderate complexity and well-documented
in D3 examples.

**The hardest part** is the "compare selected vs rest" statistical
analysis, which needs to run back in Lush (not JavaScript). Two
approaches:

1. **Export-and-reanalyze:** User selects nodes in the browser, copies
   the member indices (displayed in a text box), pastes into Lush
   for analysis. Simple but clunky.

2. **Embedded WebSocket server:** Lush runs a tiny HTTP/WebSocket
   server, the D3 page sends selection events back to Lush in real
   time. More elegant but requires network infrastructure in Lush.
   (Lush does have socket support.)

3. **Pre-compute all comparisons:** For each node (or pair of nodes),
   pre-compute summary stats for every variable and embed in the JSON.
   The JavaScript can then display comparisons without calling back
   to Lush. Works for moderate numbers of variables.

For initial implementation, approach 1 (export indices) is sufficient.
Approach 3 is the next step. Approach 2 is a future luxury.

---

## Part 6: Implementation Stages

### Stage 1: Distance Metrics and Hierarchical Clustering (C)

Implement from scratch using the NN-chain algorithm, taking only the
distance metric functions from j-cluster (with bug fixes).

**Files:** `hclust.c`, `hclust.h`, `metrics.c`, `metrics.h`

**Work items:**

*Distance metrics (adapt from j-cluster, fix bugs):*
1. Port Euclidean, Manhattan, Chebyshev (fix `abs` -> `fabs`)
2. Port Pearson correlation, Spearman (add buffer reuse), Kendall
3. Port uncentered correlation; fix cosine (divide by `sqrt(xx*yy)`,
   return `1 - similarity` not raw similarity)
4. Fix angular distance (same denominator bug as cosine)
5. Add Hamming distance (new, not in j-cluster)
6. All metrics take idx data pointers with strides, not `double**`

*Hierarchical clustering (implement fresh):*
7. Implement NN-chain algorithm with Lance-Williams distance updates
   supporting Ward, complete, average, and single linkage — all O(N^2)
8. Use contiguous lower-triangle distance matrix stored in a single
   idx1, indexed as `dm[i*(i-1)/2 + j]` for j < i
9. Implement `cuttree()` (can take from j-cluster, it's simple and correct)
10. Register as Lush DX functions via `dx_define`
11. No global mutable state — all state passed as parameters

**NN-chain core (pseudocode):**
```c
void nn_chain_linkage(double *dm, int n, int linkage, double *Z) {
    int *chain = malloc(n * sizeof(int));   /* stack */
    int chain_len = 0;
    int *size = calloc(n, sizeof(int));     /* cluster sizes */
    double *min_dist = malloc(n * sizeof(double));

    for (int i = 0; i < n; i++) size[i] = 1;

    for (int step = 0; step < n - 1; step++) {
        /* If chain empty, push arbitrary active cluster */
        if (chain_len == 0)
            chain[chain_len++] = find_any_active(n, ...);

        while (1) {
            int a = chain[chain_len - 1];
            /* Find nearest neighbor of a among active clusters */
            int b = find_nearest(a, dm, n, active);
            if (chain_len >= 2 && b == chain[chain_len - 2]) {
                /* Mutual nearest neighbors: merge a and b */
                chain_len -= 2;
                merge(a, b, dm, n, size, Z, step, linkage);
                break;
            }
            chain[chain_len++] = b;
        }
    }
}
```

**Lance-Williams update formula (used by merge()):**
```c
/* After merging clusters i and j into new cluster ij,
   update distance to every other cluster k: */
switch (linkage) {
  case WARD:
    d(ij,k) = sqrt(((n_k+n_i)*d(i,k)^2 + (n_k+n_j)*d(j,k)^2
                     - n_k*d(i,j)^2) / (n_i+n_j+n_k));
    break;
  case COMPLETE:
    d(ij,k) = max(d(i,k), d(j,k));
    break;
  case AVERAGE:
    d(ij,k) = (n_i*d(i,k) + n_j*d(j,k)) / (n_i + n_j);
    break;
  case SINGLE:
    d(ij,k) = min(d(i,k), d(j,k));
    break;
}
```

**Estimated size:** ~300 lines for NN-chain + ~200 lines for metrics
= ~500 lines of C total.

**Lush API:**
```lisp
(setq dm (distance-matrix data "euclidean"))   ;; returns idx1 (lower triangle)
(setq tree (hclust dm "ward"))                 ;; returns dendrogram
(setq labels (cut-tree tree 5))                ;; cut into 5 clusters
```

### Stage 2: DBSCAN (C)

Implement DBSCAN in C with pluggable distance functions.

**File:** `dbscan.c`

```c
DX(xdbscan) {
    /* args: data (idx2), eps (double), min_pts (int), metric (string) */
    /* returns: idx1 of cluster labels (-1 = noise) */
}
```

For the brute-force version: precompute distance matrix (or compute
on-the-fly), then run the DBSCAN algorithm. O(N^2) for general metrics.

Optional future work: KD-tree for Euclidean metric, O(N log N).

**Lush API:**
```lisp
(setq labels (dbscan data 0.5 5 "euclidean"))  ;; eps=0.5, min_pts=5
```

### Stage 3: Cover and Mapper Core (Pure Lush)

Implement the Cover class and Mapper pipeline in Lush.

**Files:** `cover.lsh`, `mapper.lsh`, `nerve.lsh`, `graph.lsh`

This is mostly data manipulation (selecting rows, set intersection)
which Lush handles well at the interpreted level. The per-bin clustering
calls into the C code from Stages 1-2.

**Lush API:**
```lisp
;; Basic usage with one lens
(setq km (new Mapper))
(setq lens (pca-projection data 1))           ;; 1D lens via PCA
(setq graph (==> km fit data lens))

;; Two lenses (Cartesian product)
(setq lens2 (idx-cat 1                        ;; stack two 1D lenses
  (pca-projection data 1)
  (density-estimate data)))
(setq graph (==> km fit data lens2
  :cover (new MapperCover 10 10 0.3 0.3)      ;; 10x10 grid, 30% overlap
  :clusterer 'dbscan
  :metric "cosine"))

;; Color by a variable
(==> graph set-color-variable residuals)
```

### Stage 4: D3 Visualization (Lush + JavaScript)

Generate HTML visualization from MapperGraph.

**Files:** `visualize.lsh`, `d3-template.js` (embedded)

**Lush API:**
```lisp
(==> graph visualize "/tmp/mapper_output.html"
  :color-by residuals
  :title "Model residuals topology")
;; Opens browser or prints path
```

### Stage 5: Lens Functions Library (Pure Lush)

Common filter functions used as Mapper lenses:

```lisp
;; Projection-based
(pca-projection data k)          ;; first k principal components
(tsne-projection data k)         ;; t-SNE (if available)

;; Density-based
(density-estimate data)          ;; KDE or k-NN density

;; Geometry-based
(eccentricity data metric)       ;; mean distance to all other points
(l2-norm data)                   ;; row norms

;; Model-based
(prediction-lens model data)     ;; model predictions as lens
(residual-lens model data y)     ;; model residuals as lens
```

### Stage 6: Interactive Analysis (JavaScript + Lush)

Add node selection, comparison, and statistical analysis.

---

## Part 7: Complexity Analysis of Full Mapper Pipeline

For N data points, d dimensions, K hypercubes, B average bin size:

| Step | Time | Space |
|------|------|-------|
| Filter function | O(N * d) typical | O(N * k) |
| Cover transform | O(N * K) | O(N) |
| Distance matrices (per bin) | O(B^2 * d) × K | O(B^2) |
| Clustering (per bin, hclust) | O(B^3) × K | O(B^2) |
| Clustering (per bin, DBSCAN) | O(B^2) × K | O(B^2) |
| Nerve computation | O(M^2 * B) worst case | O(M) |
| **Total (hclust)** | **O(K * B^3)** | **O(B^2)** |
| **Total (DBSCAN)** | **O(K * B^2 * d)** | **O(B^2)** |

Where M = total number of nodes (clusters across all bins).

Typical parameters: N=10000, K=100, B=200, d=50
- hclust: 100 * 200^3 = 8 × 10^8 — a few seconds
- DBSCAN: 100 * 200^2 * 50 = 2 × 10^8 — about a second

For N=100000, K=200, B=1000:
- hclust: 200 * 1000^3 = 2 × 10^11 — minutes to hours
- DBSCAN: 200 * 1000^2 * 50 = 10^10 — tens of seconds

**Conclusion:** DBSCAN is strongly preferred for large datasets. Hierarchical
clustering is fine for exploratory work on moderate data.

---

## Part 8: Show-Stopper Analysis

### Definite Show-Stoppers: None

Everything can be implemented within Lush's existing capabilities:
- The C clustering code builds as a Lush module (dlopen)
- Cover/nerve logic is straightforward Lush code
- D3 visualization is file I/O (write HTML)
- No external dependencies beyond a web browser for visualization

### Significant Challenges

1. **Set intersection performance.** The nerve computation requires
   finding shared points between cluster nodes. If nodes have thousands
   of members, naive intersection (O(A * B)) is slow. Using sorted
   arrays with merge-join (O(A + B)) or hash sets is important.

2. **PCA for lens functions.** Lush has LAPACK bindings but no
   ready-made PCA function. Implementing PCA via SVD (using the
   existing LAPACK package) is straightforward but requires setup.

3. **DBSCAN eps parameter selection.** DBSCAN requires choosing eps
   (neighborhood radius). This is data-dependent and non-trivial.
   A k-distance plot heuristic can help automate it.

4. **D3.js embedding.** The JavaScript must be either:
   - Embedded as a string in the Lush file (ugly but self-contained)
   - Written as a separate template file installed with the package
   - Loaded from a CDN at runtime (requires network)
   The template file approach is cleanest.

5. **Large graph rendering.** Mapper graphs with >1000 nodes will be
   slow in D3's force simulation. WebGL-based rendering (e.g., Sigma.js)
   would be needed for very large graphs, but this is an edge case.

### Non-Issues

- **Memory:** Even for N=100000, the per-bin data and distance matrices
  are small (bins of 100-1000 points). Total memory is modest.
- **Compiled code:** The inner loops (distance computation, DBSCAN
  neighbor search) are in C. The Lush-level code handles only data
  routing and graph construction.
- **Metric generality:** Both hclust and DBSCAN accept a function
  pointer for the distance metric. Any new metric is just a new C
  function with the same signature.

---

## Part 9: Relationship to DataTable

The Mapper package naturally complements the columnar DataTable:

```lisp
;; Load data from DataTable
(setq dt (DataTable-load-striped "/data/trades/"))
(setq X (==> dt to-matrix '("price" "volume" "spread")))

;; Build topology
(setq lens (eccentricity X *euclidean*))
(setq graph (==> mapper fit X lens))

;; Color by a DataTable column not used in fitting
(==> graph set-color-variable
  (==> dt get-column "model_residual"))

;; Visualize
(==> graph visualize "/tmp/trades_topology.html"
  :title "Trade topology colored by model residual")
```

---

## Part 10: Open Questions

1. **Should DBSCAN be the default, or hierarchical clustering?**
   KeplerMapper defaults to DBSCAN. For general use DBSCAN is better
   (no k parameter, handles noise). But hierarchical clustering gives
   more control and a dendrogram for each bin.

2. **Should we implement persistent homology too?** Mapper is the
   1-skeleton (graph) of the nerve. Computing higher-dimensional
   simplices (2-simplices for triangles, etc.) and their homology
   would give Betti numbers. This is substantially more complex but
   would make the TDA package complete.

3. **What D3 version to target?** D3 v7 is current but v4 is what
   KeplerMapper uses. v7 is modular (tree-shakeable). Either works
   for a self-contained HTML file.

4. **Should the visualization support 3D?** Some Mapper graphs benefit
   from 3D force layout (using three.js instead of D3). This adds
   significant complexity.

5. **Should we port the user's J clustering code bugs upstream?**
   The bugs in cosine, angle, and chebyshev are real and should
   probably be fixed in the j-cluster repo regardless of the Lush work.

6. **How to handle the user's J hierarchical clustering code?** The
   user indicated they might want to bring their J clustering
   algorithms into Lush. The C code in hclust.c is the portable piece.
   The J FFI layer (j-cluster.ijs) is J-specific and won't transfer,
   but the C entry point `treecluster()` maps directly to a Lush DX
   function.
