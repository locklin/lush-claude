# Mapper Package Design Notes

## Architecture

The Mapper package implements the Mapper algorithm (Singh, Memoli, Carlsson
2007) for Topological Data Analysis, plus an interactive visualization server.

### Package Layout

```
packages/mapper/
  mapper.lsh            Main entry point: mapper-run pipeline
  cover.lsh             Overlapping cover (intervals/hypercubes), 1D and 2D
  cluster.lsh           SLINK single-linkage + DBSCAN (compiled C kernels)
  metrics.lsh           Distance matrix computation (compiled C)
  graph.lsh             MapperGraph class + nerve construction
  lens.lsh              Filter functions (projection, geometry, spectral, UMAP)
  stats.lsh             Two-sample statistical tests + FDR correction
  visualize.lsh         Self-contained D3 v7 HTML visualization output
  mapper-db.lsh         SQLite persistence layer (via packages/sqlite)
  mapper-viz.lsh        HTTP visualization server (via packages/httpd)
  csvread.lsh           CSV/TSV reader (including gzipped)
  mapper-config.lsh     Package configuration and module loading
  mapper-c.c            All C acceleration: metrics, SLINK, DBSCAN, KD-tree
  mapper-c.h            C header (metric_fn_t, function declarations)
  umap.lsh              UMAP dimensionality reduction
  viz/                  Cytoscape.js browser UI (index.html, app.js, etc.)
  NKI/                  Demo dataset (breast cancer gene expression)
  demo-nki.lsh          Demo script
  load-nki-db.lsh       Load NKI dataset into SQLite project DB
```

### Three Output Modes

1. **Self-contained D3 HTML** (`visualize.lsh`): Single HTML file with embedded
   D3 v7 force-directed graph. Supports two-group comparison with statistical
   tests (Welch t, Mann-Whitney U, KS, hypergeometric) and BH-FDR correction.
   Good for quick inspection and sharing.

2. **SQLite persistence** (`mapper-db.lsh`): Stores datasets, run configs, and
   graph results in a SQLite database. Enables multi-run comparisons and
   session persistence. Schema uses WAL mode for concurrent reader/writer.

3. **Interactive browser UI** (`mapper-viz.lsh`): Lush HTTP server on localhost
   serving a Cytoscape.js single-page app. Reads/writes the SQLite database.
   Supports multi-graph tabs, lasso selection, group comparison, re-coloring,
   dataset upload, and launching new Mapper runs from the browser.

### Key Architecture Decision: Lush HTTP Server (not Electron/Gephi)

Five options were evaluated for the interactive visualization:

- **Gephi Toolkit + Swing**: gephi-toolkit lacks the viz/UI modules, so
  the interactive graph canvas would have to be built from scratch. Rejected.
- **Cytoscape.js + Electron**: Good viz capabilities but 150MB Electron
  bundle and Node.js dependency.
- **Gephi Plugin**: Fragile plugin API across versions; limited control
  over UX. Rejected.
- **Lush HTTP + Cytoscape.js in browser** (chosen): Zero external deps
  beyond a web browser. ~200-300 lines of Lush for HTTP serving. Tight
  integration (mapper engine and server share the same process).
- **Python/Flask + Cytoscape.js**: More moving parts (browser + server +
  SQLite + Lush as separate processes). Rejected.

Rationale: The HTTP subset needed is tiny (serve ~5 static files, ~10 JSON
endpoints, localhost only). Lush already has `socketaccept`/`socketselect`.
The browser-side Cytoscape.js code is identical regardless of what serves it.
If Lush HTTP proves insufficient, Electron is a drop-in replacement since
only the server changes.

---

## API Summary

### Mapper Pipeline

```lisp
;; Core pipeline
(setq graph (mapper-run data lens n-cubes overlap
              'metric "euclidean"     ;; distance metric
              'clusterer "slink"      ;; "slink" or "dbscan"
              'eps 0.5                ;; SLINK threshold or DBSCAN epsilon
              'min-pts 3              ;; DBSCAN min points (ignored for slink)
              'min-intersection 1))   ;; min shared points for edge

;; Inspect results
(==> graph summary)
(==> graph num-nodes)
(==> graph num-edges)

;; Color nodes
(==> graph color-by-mean data col-index)
(==> graph color-by-variable values-idx1)
```

### Lens Functions

```lisp
;; Projection-based
(mapper-lens-column data col)          ;; single column -> Nx1
(mapper-lens-columns data col1 col2)   ;; two columns -> Nx2
(mapper-lens-sum data)                 ;; row sum -> Nx1
(mapper-lens-mean data)                ;; row mean -> Nx1
(mapper-lens-variance data)            ;; row variance -> Nx1
(mapper-lens-kurtosis data)            ;; excess kurtosis -> Nx1
(mapper-lens-pca data k)              ;; top-k PCA components -> Nxk

;; Geometry-based
(mapper-lens-l2norm data)              ;; L2 row norms -> Nx1
(mapper-lens-eccentricity data metric) ;; mean distance to all points -> Nx1
(mapper-lens-linf-centrality data metric) ;; max distance to all points -> Nx1

;; Density-based
(mapper-lens-density data [n-neighbors]) ;; 1/mean-kNN-dist -> Nx1

;; Clustering-based
(mapper-lens-kmeans data [k])          ;; cluster assignment -> Nx1
(mapper-lens-kmeans-dist data [k])     ;; distance to centroid -> Nx1

;; Spectral (via mlcore/spectral)
(mapper-lens-laplacian data [k])       ;; Laplacian eigenmaps 1D
(mapper-lens-laplacian2d data [k])     ;; Laplacian eigenmaps 2D
(mapper-lens-diffusion data [k])       ;; diffusion maps 1D
(mapper-lens-diffusion2d data [k])     ;; diffusion maps 2D
(mapper-lens-kpca data [gamma])        ;; kernel PCA 1D (RBF)
(mapper-lens-kpca2d data [gamma])      ;; kernel PCA 2D (RBF)

;; UMAP (via mapper/umap)
(mapper-lens-umap data [k [min-dist]]) ;; UMAP 1D
(mapper-lens-umap2d data [k [min-dist]]) ;; UMAP 2D
```

### Distance Metrics

Available metric strings: `"euclidean"`, `"sqeuclidean"`, `"cityblock"`
(alias `"manhattan"`), `"chebyshev"`, `"cosine"`, `"correlation"`,
`"hamming"`, `"spearman"`, `"kendall"`, `"hellinger"`.

```lisp
(setq dm (mapper-distance-matrix data "euclidean")) ;; flat lower-triangle idx1
(mapper-dm-ref dm i j)                               ;; access element (i,j)
```

### Clustering

```lisp
;; SLINK single-linkage O(N^2)
(setq tree (mapper-slink data metric))   ;; -> (merge-left merge-right merge-dist)
(setq tree (mapper-slink-dm dm n))       ;; from precomputed distance matrix
(mapper-cut-tree n ml mr md nclusters)   ;; cut by cluster count
(mapper-cut-tree-dist n ml mr md thresh) ;; cut by distance threshold

;; DBSCAN (KD-tree for Euclidean, brute-force for others)
(setq result (mapper-dbscan data eps min-pts metric)) ;; -> (nclusters labels)
(setq result (mapper-dbscan-dm dm n eps min-pts))      ;; from distance matrix
```

### Visualization

```lisp
;; D3 HTML output
(==> graph visualize "/tmp/mapper.html" "My Data"
  'data full-data-matrix
  'col-names column-name-list)

;; SQLite persistence
(setq db (mapper-db-open "/path/to/project.db"))
(mapper-db-store-dataset db data col-names name source-file)
(mapper-db-store-graph db run-id graph data)
(mapper-db-close db)

;; Interactive browser server
(mapper-viz-start "/path/to/project.db")       ;; blocks, opens browser
(mapper-viz-setup "/path/to/project.db" port)  ;; returns server object
```

### HTTP Endpoints (visualization server)

```
GET  /                              -> index.html (Cytoscape.js SPA)
GET  /api/graphs                    -> list all graphs
GET  /api/graph/:id                 -> full graph data (nodes, edges, members)
GET  /api/datasets                  -> list all datasets
GET  /api/dataset/:id               -> dataset metadata + column names
GET  /api/dataset/:id/columns?cols= -> column data (JSON arrays)
GET  /api/runs                      -> list all mapper runs
GET  /api/run/:id                   -> run info + status
GET  /api/labels/:graphId           -> node group labels
POST /api/dataset                   -> upload CSV data
POST /api/load-file                 -> load local file into DB
POST /api/run                       -> start background mapper run
POST /api/label                     -> save node group label
POST /api/graph/:id/display         -> set graph active/inactive
```

---

## Implementation Details

### C Acceleration (mapper-c.c, mapper-c.h)

All performance-critical code is in a single C file (~1000 lines) compiled
into `mapper-c.so` via `dhc-make`. The C code provides:

- **10 distance metrics**: euclidean, squared euclidean, cityblock/manhattan,
  chebyshev, cosine, correlation, hamming, spearman, kendall, hellinger.
  All take `(const double *data, int ncols, int i, int j)` via function
  pointer `metric_fn_t`.
- **SLINK** (Sibson 1973): O(N^2) single-linkage. Operates on either raw
  data (computing distances on the fly) or a precomputed lower-triangle
  distance matrix.
- **DBSCAN**: Brute-force O(N^2) for arbitrary metrics, KD-tree accelerated
  O(N log N) for Euclidean.
- **KD-tree**: Euclidean range queries for DBSCAN acceleration.
- **Sorted set intersection**: O(A+B) merge-join for nerve computation.
- **TSV reader**: Handles plain and gzipped tab-separated files.

#### Distance Metric Bug Fixes (from j-cluster)

The metric implementations were adapted from the C Clustering Library
(de Hoon, University of Tokyo) with the following fixes applied:

| Bug | Fix Applied |
|-----|-------------|
| `cosine()` divides by `xx*yy` not `sqrt(xx*yy)` | Fixed to `sqrt(xx*yy)` |
| `cosine()` returns similarity not distance | Returns `1 - similarity` |
| `angle()` same denominator bug | Same fix as cosine |
| `chebyshev()` uses `abs()` (int) on doubles | Changed to `fabs()` |

#### Why SLINK Instead of NN-Chain

The original plan recommended implementing NN-chain for O(N^2) Ward/complete/
average linkage. The implementation uses only SLINK (single linkage) because:

1. Mapper's per-bin clustering typically has small bins (100-1000 points),
   where O(N^2) SLINK is fast enough.
2. Single linkage with a distance threshold is the most natural fit for
   Mapper's "how many clusters are in this bin?" question.
3. DBSCAN is provided as the alternative for cases where single linkage
   is inappropriate (no k parameter, handles noise).
4. The NN-chain algorithm remains a reasonable future addition if Ward or
   complete linkage is needed.

#### int-matrix DHC Bug Workaround

The Lush DHC compiler generates `Midx_maclear(m, intg)` for int-matrix
creation, which writes 8-byte values into 4-byte `ST_I` storage, causing
heap corruption. All int-matrix allocations are done in interpreted wrapper
functions (`mapper-slink`, `mapper-dbscan`, etc.), not in compiled inner
functions. The compiled functions receive pre-allocated matrices as parameters
and access elements via `IDX_PTR($var, int)`.

### Cover (cover.lsh)

Implements the KeplerMapper cover scheme:

- **Radius formula**: `range / (2 * n_cubes * (1 - perc_overlap))` with a
  small epsilon added to avoid boundary edge cases.
- **Center placement**: Linspace from `min + radius` to `max - radius`.
- **2D support**: Cartesian product of per-dimension center arrays.
- **Point assignment**: For each hypercube, iterate all points and check
  if within `[center - radius, center + radius]` in all dimensions. Points
  may appear in multiple hypercubes (this is the key feature enabling nerve
  construction).
- Empty bins use `(int-matrix 1)` as a placeholder to avoid the
  `(int-matrix 0)` Lush runtime error; `mapper-run` skips bins with
  fewer than 2 points.

### Nerve Construction (graph.lsh)

- Only checks node pairs from **different** cover bins (nodes from the same
  bin are separate clusters and cannot share points).
- Uses compiled sorted-array merge intersection (`mapper-isect-count`) for
  O(A+B) per pair instead of O(A*B) naive intersection.
- Edge weight = number of shared data points.

### PCA Lens (lens.lsh)

The current PCA lens (`mapper-lens-pca`) uses a simplified approach: compute
the covariance matrix X^T * X, then project onto the k columns with largest
diagonal variance. This is equivalent to selecting the highest-variance
original dimensions, not true PCA eigenvectors. A proper implementation
would use LAPACK `dsyev` or `dgesvd` for eigendecomposition. Marked with
a TODO in the code.

### Visualization Server Architecture

```
Lush Process (main)
  +-----------------+     +-------------------+
  | Mapper Engine   |     | HTTP Server       |
  | - mapper-run    |     | - socketaccept    |     +-----------+
  | - mapper-db-run |     | - serves static   |<--->| Browser   |
  +--------+--------+     | - serves JSON API |     | Cyto.js   |
           |               +--------+----------+     +-----------+
           v                        |
  +-----------------------------+   |
  |     SQLite Database (.db)   |   |
  +-----------------------------+   |
           |
           | fork (long-running runs)
           v
  +-------------------+
  | Lush child        |
  | (mapper worker,   |
  |  writes to DB,    |
  |  then exits)      |
  +-------------------+
```

- The main Lush process serves dual roles: mapper engine + HTTP server.
- SQLite is the persistence and communication layer.
- For mapper runs triggered from the browser, a child Lush process is forked
  so the HTTP server remains responsive. The child writes results to SQLite;
  the browser polls `/api/run/:id` for status.
- Worker script generation (`_viz-generate-worker`) writes a self-contained
  `.lsh` file that loads the necessary packages, reads run params from SQLite,
  runs mapper, and stores results.
- Crash detection: if the worker process crashes, a `.failed` marker file is
  written. The `/api/run/:id` handler also greps the worker log for `"*** "`
  (Lush error prefix) to detect crashes.

### Database Schema

Key tables: `datasets`, `dataset_values` (row-major), `mapper_runs` (with
status tracking), `graphs`, `graph_nodes` (with JSON member arrays),
`graph_edges`, `node_labels` (user-defined groups), `display_state`.

- **WAL mode** for concurrent reader (HTTP server) and writer (child process).
- **Dataset storage**: individual rows in `dataset_values` (simple, queryable).
  For a 272x500 matrix that's ~136K rows, which SQLite handles trivially.
- **Column selection**: Stored as JSON in `mapper_runs.column_selection`.
  Supports `"all"`, `"auto_variance"` (top-N by variance, skipping high-
  variance metadata columns), and explicit column index lists.
- **Dual lens**: `column_selection` JSON includes `lens_type_2` and
  `lens2_col` fields for 2D lens configurations.

### Browser UI (viz/ directory)

- **Cytoscape.js** for graph rendering with force-directed layout (fcose).
- **cytoscape-lasso** plugin for freeform node selection.
- **cola.js** layout as an alternative.
- **stats.js**: Client-side statistical tests (Welch t, Mann-Whitney U, KS,
  hypergeometric, BH-FDR) for group comparison.
- All JS libraries are vendored (checked into `viz/lib/`); no npm or CDN.
- Browser polls server at 5-second intervals for new graphs and run status.

### Dataset Caching

The visualization server caches the most recently loaded dataset matrix
(`*viz-dataset-cache*`) to avoid re-reading from SQLite on every column
data request. This is important because `/api/dataset/:id/columns` is called
frequently when the user re-colors the graph or runs statistical comparisons.

---

## Known Issues and Limitations

### Algorithmic

1. **PCA lens is approximate**: Projects onto highest-variance original
   dimensions rather than true principal components. Proper SVD/eigen
   decomposition not yet implemented.

2. **No Ward/complete/average linkage**: Only SLINK (single linkage) is
   implemented. The NN-chain algorithm (O(N^2) for reducible linkages)
   was planned but not built. DBSCAN is the recommended alternative for
   most use cases.

3. **DBSCAN eps selection**: Requires choosing the epsilon parameter, which
   is data-dependent. No automatic selection heuristic (e.g., k-distance
   plot) is provided.

4. **Nerve computation is O(M^2)**: Checks all node pairs from different
   bins. The plan noted an optimization (only check pairs from overlapping
   bins via a cover adjacency structure) but this was not implemented.
   Acceptable for typical graph sizes (<1000 nodes).

5. **Cover assignment is O(N * K)**: Iterates all points for each hypercube.
   Could be accelerated with spatial indexing but is fast enough for typical
   datasets.

### Categorical / Mixed-Type Data

Categorical and mixed-type data support was planned but not implemented:

- **Planned metrics**: Jaccard, Dice, Gower distance (with per-column type
  descriptor struct).
- **Planned data model**: `column_desc_t` struct with `col_type` (continuous/
  categorical/binary), `col_range`, `col_weight` passed to metric functions.
- **Current state**: All data must be numeric doubles. Categorical variables
  must be manually encoded before use.
- **Workaround**: Use eccentricity lens (works with any metric) or column-
  based lenses for mixed data. PCA remains continuous-only.

### Visualization

1. **Large graphs**: Cytoscape.js force simulation slows down beyond ~5000
   nodes. The plan suggested WebGL rendering (Sigma.js) as a fallback but
   this was not implemented.

2. **D3 HTML is a snapshot**: Each run produces a separate file with no
   persistence. Use the SQLite/HTTP server mode for multi-run workflows.

3. **Single-threaded HTTP**: The Lush server handles one request at a time.
   Fine for single-user localhost, but requests block during handling. Long
   mapper runs are forked as child processes to avoid blocking.

4. **Browser tab closure**: Closing the tab loses no data (all state is in
   SQLite), but the user must manually reopen the URL.

### Lush Runtime

1. **int-matrix DHC bug**: Cannot create int-matrices in compiled functions.
   All int-matrix creation is in interpreted wrappers.

2. **`(int-matrix 0)` not supported**: Cover code uses `(int-matrix 1)` as
   a placeholder for empty bins.

3. **`each` on idx**: Does not work. Cover and nerve code use `for` loops
   with index access for idx arrays, `each` only for Lush lists.

### Complexity Summary

For N data points, d dimensions, K hypercubes, B average bin size:

| Step | Time | Notes |
|------|------|-------|
| Lens computation | O(N^2) typical | eccentricity needs distance matrix |
| Cover transform | O(N * K) | |
| SLINK per bin | O(B^2) x K | single-linkage |
| DBSCAN per bin (Euclidean) | O(B log B) x K | KD-tree |
| DBSCAN per bin (other) | O(B^2) x K | brute-force |
| Nerve | O(M^2 * B) worst | M = total nodes |
| **Total (SLINK)** | **O(K * B^2)** | |
| **Total (DBSCAN/Euclidean)** | **O(K * B * log B)** | |

Typical parameters (N=10000, K=100, B=200): SLINK takes a few seconds,
DBSCAN under a second. For N=100000 with large bins (B=1000), DBSCAN is
strongly preferred.

### Not Implemented (from original plan)

- Persistent homology / Betti numbers (higher simplices)
- NN-chain algorithm for Ward/complete/average linkage
- Jaccard, Dice, Gower distance metrics
- CSV auto-detection of column types
- MCA/FAMD lenses for categorical data
- 3D graph visualization (three.js)
- `mapper-viz-start-bg` for background server (returns REPL to user)
- Non-blocking HTTP via `socketselect`
