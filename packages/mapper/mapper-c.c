/* mapper-c.c -- C acceleration for the Lush Mapper/TDA package
 *
 * Distance metrics adapted from the C Clustering Library (hclust.c)
 * Copyright (C) 2002 Michiel Jan Laurens de Hoon.
 * Bug fixes and Lush adaptation by Claude.
 *
 * SLINK: Sibson, R. (1973). SLINK: An optimally efficient algorithm
 * for the single-link cluster method. The Computer Journal, 16(1): 30-34.
 *
 * NOTE: Lush int-matrix uses ST_I storage with sizeof(int) elements.
 * All integer array parameters use 'int *' to match.
 */

#include "mapper-c.h"
#include <stdio.h>

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef max
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif

#define ROW(data, i, ncols) ((data) + (i) * (ncols))
#define DM_IDX(i, j) ((i)*((i)-1)/2 + (j))

/* ================================================================
 * Section 1: Distance Metric Functions
 * ================================================================ */

static double metric_euclidean(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    double sum = 0.0;
    for (int k = 0; k < ncols; k++) {
        double d = a[k] - b[k];
        sum += d * d;
    }
    return sqrt(sum);
}

static double metric_sqeuclidean(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    double sum = 0.0;
    for (int k = 0; k < ncols; k++) {
        double d = a[k] - b[k];
        sum += d * d;
    }
    return sum;
}

static double metric_cityblock(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    double sum = 0.0;
    for (int k = 0; k < ncols; k++)
        sum += fabs(a[k] - b[k]);
    return sum;
}

/* Fixed: use fabs() not abs() (j-cluster bug) */
static double metric_chebyshev(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    double mx = 0.0;
    for (int k = 0; k < ncols; k++) {
        double d = fabs(a[k] - b[k]);
        if (d > mx) mx = d;
    }
    return mx;
}

/* Fixed: divide by sqrt(xx*yy) and return 1-similarity (j-cluster bugs) */
static double metric_cosine(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    double xx = 0.0, yy = 0.0, xy = 0.0;
    for (int k = 0; k < ncols; k++) {
        xx += a[k] * a[k];
        yy += b[k] * b[k];
        xy += a[k] * b[k];
    }
    if (xx == 0.0 || yy == 0.0) return 1.0;
    double sim = xy / sqrt(xx * yy);
    if (sim > 1.0) sim = 1.0;
    if (sim < -1.0) sim = -1.0;
    return 1.0 - sim;
}

/* Pearson correlation distance = 1 - r */
static double metric_correlation(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    double sa = 0, sb = 0, sab = 0, sa2 = 0, sb2 = 0;
    double n = (double)ncols;
    for (int k = 0; k < ncols; k++) {
        sa += a[k]; sb += b[k];
        sab += a[k] * b[k];
        sa2 += a[k] * a[k];
        sb2 += b[k] * b[k];
    }
    double da = sa2 - sa * sa / n;
    double db = sb2 - sb * sb / n;
    if (da <= 0.0 || db <= 0.0) return 1.0;
    double r = (sab - sa * sb / n) / sqrt(da * db);
    return 1.0 - r;
}

static double metric_hamming(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    int diff = 0;
    for (int k = 0; k < ncols; k++)
        if (a[k] != b[k]) diff++;
    return (double)diff / (double)ncols;
}

/* --- Spearman rank correlation --- */

static const double *_sortdata = NULL;

static int _compare(const void *a, const void *b) {
    double v1 = _sortdata[*(const int *)a];
    double v2 = _sortdata[*(const int *)b];
    return (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
}

static double *_getrank(int n, const double *data) {
    double *rank = malloc(n * sizeof(double));
    int *index = malloc(n * sizeof(int));
    if (!rank || !index) { free(rank); free(index); return NULL; }
    _sortdata = data;
    for (int i = 0; i < n; i++) index[i] = i;
    qsort(index, n, sizeof(int), _compare);
    for (int i = 0; i < n; i++) rank[index[i]] = i;
    int i = 0;
    while (i < n) {
        double val = data[index[i]];
        int j = i + 1;
        while (j < n && data[index[j]] == val) j++;
        if (j > i + 1) {
            double avg = rank[index[i]] + (j - i - 1) / 2.0;
            for (int k = i; k < j; k++) rank[index[k]] = avg;
        }
        i = j;
    }
    free(index);
    return rank;
}

static double metric_spearman(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    double *r1 = _getrank(ncols, a);
    double *r2 = _getrank(ncols, b);
    if (!r1 || !r2) { free(r1); free(r2); return 0.0; }
    double avg = 0.5 * (ncols - 1);
    double sab = 0, sa2 = 0, sb2 = 0;
    for (int k = 0; k < ncols; k++) {
        sab += r1[k] * r2[k];
        sa2 += r1[k] * r1[k];
        sb2 += r2[k] * r2[k];
    }
    free(r1); free(r2);
    sab /= ncols; sa2 /= ncols; sb2 /= ncols;
    double da = sa2 - avg * avg;
    double db = sb2 - avg * avg;
    if (da <= 0.0 || db <= 0.0) return 1.0;
    double r = (sab - avg * avg) / sqrt(da * db);
    return 1.0 - r;
}

static double metric_kendall(const double *data, int ncols, int i, int j) {
    const double *a = ROW(data, i, ncols);
    const double *b = ROW(data, j, ncols);
    int con = 0, dis = 0, exx = 0, exy = 0;
    for (int p = 0; p < ncols; p++) {
        for (int q = 0; q < p; q++) {
            double dx = a[p] - a[q];
            double dy = b[p] - b[q];
            if (dx > 0 && dy > 0) con++;
            else if (dx < 0 && dy < 0) con++;
            else if (dx > 0 && dy < 0) dis++;
            else if (dx < 0 && dy > 0) dis++;
            else if (dx == 0 && dy != 0) exx++;
            else if (dx != 0 && dy == 0) exy++;
        }
    }
    double denomx = con + dis + exx;
    double denomy = con + dis + exy;
    if (denomx == 0 || denomy == 0) return 1.0;
    return 1.0 - (con - dis) / sqrt(denomx * denomy);
}

/* Metric selection by name */
metric_fn_t mapper_select_metric(const char *name) {
    if (!strcmp(name, "euclidean"))    return metric_euclidean;
    if (!strcmp(name, "sqeuclidean"))  return metric_sqeuclidean;
    if (!strcmp(name, "cityblock"))    return metric_cityblock;
    if (!strcmp(name, "manhattan"))    return metric_cityblock;
    if (!strcmp(name, "chebyshev"))    return metric_chebyshev;
    if (!strcmp(name, "cosine"))       return metric_cosine;
    if (!strcmp(name, "correlation"))  return metric_correlation;
    if (!strcmp(name, "hamming"))      return metric_hamming;
    if (!strcmp(name, "spearman"))     return metric_spearman;
    if (!strcmp(name, "kendall"))      return metric_kendall;
    return NULL;
}

/* ================================================================
 * Section 2: Distance Matrix
 * ================================================================ */

void mapper_distance_matrix(const double *data, int nrows, int ncols,
                            metric_fn_t metric, double *dm) {
    for (int i = 1; i < nrows; i++)
        for (int j = 0; j < i; j++)
            dm[DM_IDX(i, j)] = metric(data, ncols, i, j);
}

/* ================================================================
 * Section 3: SLINK Single-Linkage Clustering
 * ================================================================
 * Sibson's SLINK algorithm: O(N^2) time, O(N) space.
 * Produces a dendrogram sorted by merge distance.
 *
 * Internal arrays (pi, lambda, temp) use int/double since they
 * are malloc'd locally and never shared with Lush.
 * Output arrays (merge_left, merge_right) use long to match Lush intg.
 */

/* Internal: convert SLINK (pi, lambda) to dendrogram arrays */
typedef struct { int left; double distance; } _slink_node;

static int _slink_cmp(const void *a, const void *b) {
    double d1 = ((_slink_node *)a)->distance;
    double d2 = ((_slink_node *)b)->distance;
    return (d1 < d2) ? -1 : (d1 > d2) ? 1 : 0;
}

static int _slink_postprocess(int n, int *pi, double *lambda,
                              int *merge_left, int *merge_right,
                              double *merge_dist) {
    int nn = n - 1;
    _slink_node *nodes = malloc(nn * sizeof(_slink_node));
    int *idx = malloc(n * sizeof(int));
    if (!nodes || !idx) { free(nodes); free(idx); return -1; }

    for (int i = 0; i < nn; i++) {
        nodes[i].left = i;
        nodes[i].distance = lambda[i];
    }
    qsort(nodes, nn, sizeof(_slink_node), _slink_cmp);

    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 0; i < nn; i++) {
        int j = nodes[i].left;
        int k = pi[j];
        merge_left[i] = idx[j];
        merge_right[i] = idx[k];
        merge_dist[i] = nodes[i].distance;
        idx[k] = -(i + 1);
    }
    free(nodes);
    free(idx);
    return 0;
}

/* Core SLINK loop (shared by both variants) */
static void _slink_update(int i, double *temp, int *pi, double *lambda) {
    for (int j = 0; j < i; j++) {
        int k = pi[j];
        if (lambda[j] >= temp[j]) {
            if (lambda[j] < temp[k]) temp[k] = lambda[j];
            lambda[j] = temp[j];
            pi[j] = i;
        } else {
            if (temp[j] < temp[k]) temp[k] = temp[j];
        }
    }
    for (int j = 0; j < i; j++) {
        if (lambda[j] >= lambda[pi[j]])
            pi[j] = i;
    }
}

int mapper_slink(const double *dm, int nrows,
                 int *merge_left, int *merge_right, double *merge_dist) {
    int nn = nrows - 1;
    int *pi = malloc(nn * sizeof(int));
    double *lambda = malloc(nrows * sizeof(double));
    double *temp = malloc(nn * sizeof(double));
    if (!pi || !lambda || !temp) {
        free(pi); free(lambda); free(temp); return -1;
    }
    for (int i = 0; i < nn; i++) pi[i] = i;

    for (int i = 0; i < nrows; i++) {
        lambda[i] = DBL_MAX;
        for (int j = 0; j < i; j++)
            temp[j] = dm[DM_IDX(i, j)];
        _slink_update(i, temp, pi, lambda);
    }

    int rc = _slink_postprocess(nrows, pi, lambda,
                                merge_left, merge_right, merge_dist);
    free(pi); free(lambda); free(temp);
    return rc;
}

int mapper_slink_data(const double *data, int nrows, int ncols,
                      metric_fn_t metric,
                      int *merge_left, int *merge_right, double *merge_dist) {
    int nn = nrows - 1;
    int *pi = malloc(nn * sizeof(int));
    double *lambda = malloc(nrows * sizeof(double));
    double *temp = malloc(nn * sizeof(double));
    if (!pi || !lambda || !temp) {
        free(pi); free(lambda); free(temp); return -1;
    }
    for (int i = 0; i < nn; i++) pi[i] = i;

    for (int i = 0; i < nrows; i++) {
        lambda[i] = DBL_MAX;
        for (int j = 0; j < i; j++)
            temp[j] = metric(data, ncols, i, j);
        _slink_update(i, temp, pi, lambda);
    }

    int rc = _slink_postprocess(nrows, pi, lambda,
                                merge_left, merge_right, merge_dist);
    free(pi); free(lambda); free(temp);
    return rc;
}

/* ================================================================
 * Section 4: Cut Tree
 * ================================================================ */

void mapper_cut_tree(int nelements,
                     const int *merge_left, const int *merge_right,
                     const double *merge_dist,
                     int nclusters, int *labels) {
    (void)merge_dist; /* used only by cut_tree_dist */
    int nn = nelements - 1;
    int n = nelements - nclusters; /* merges to keep */
    int icluster = 0;
    int *nodeid;

    /* Process unjoined merges (top of tree) */
    for (int i = nn - 1; i >= n; i--) {
        int k = merge_left[i];
        if (k >= 0) labels[k] = icluster++;
        k = merge_right[i];
        if (k >= 0) labels[k] = icluster++;
    }

    /* Propagate cluster IDs down through joined merges */
    nodeid = calloc(n, sizeof(int));
    if (!nodeid) { for (int i = 0; i < nelements; i++) labels[i] = -1; return; }
    for (int i = 0; i < n; i++) nodeid[i] = -1;

    for (int i = n - 1; i >= 0; i--) {
        int j;
        if (nodeid[i] < 0) { j = icluster++; nodeid[i] = j; }
        else j = nodeid[i];
        int k = merge_left[i];
        if (k < 0) nodeid[-k - 1] = j; else labels[k] = j;
        k = merge_right[i];
        if (k < 0) nodeid[-k - 1] = j; else labels[k] = j;
    }
    free(nodeid);
}

int mapper_cut_tree_dist(int nelements,
                         const int *merge_left, const int *merge_right,
                         const double *merge_dist,
                         double threshold, int *labels) {
    int nn = nelements - 1;
    /* Find how many merges are below threshold */
    int n = 0;
    while (n < nn && merge_dist[n] <= threshold) n++;
    int nclusters = nelements - n;
    mapper_cut_tree(nelements, merge_left, merge_right, merge_dist,
                    nclusters, labels);
    return nclusters;
}

/* ================================================================
 * Section 5: DBSCAN
 * ================================================================ */

static inline double _dm_get(const double *dm, int i, int j) {
    if (i == j) return 0.0;
    if (i < j) { int t = i; i = j; j = t; }
    return dm[DM_IDX(i, j)];
}

int mapper_dbscan(const double *dm, int nrows,
                  double eps, int min_pts, int *labels) {
    int *visited = calloc(nrows, sizeof(int));
    int *stack = malloc(nrows * sizeof(int));
    int *neighbors = malloc(nrows * sizeof(int));
    if (!visited || !stack || !neighbors) {
        free(visited); free(stack); free(neighbors);
        return -1;
    }
    for (int i = 0; i < nrows; i++) labels[i] = -1;
    int cluster_id = 0;

    for (int i = 0; i < nrows; i++) {
        if (visited[i]) continue;
        visited[i] = 1;

        /* Find neighbors of i */
        int nn = 0;
        for (int j = 0; j < nrows; j++)
            if (_dm_get(dm, i, j) <= eps) neighbors[nn++] = j;

        if (nn < min_pts) continue; /* noise for now */

        /* Expand cluster */
        labels[i] = cluster_id;
        int sp = 0;
        for (int k = 0; k < nn; k++)
            if (neighbors[k] != i) stack[sp++] = neighbors[k];

        while (sp > 0) {
            int q = stack[--sp];
            if (labels[q] == -1) labels[q] = cluster_id; /* noise -> border */
            if (visited[q]) continue;
            visited[q] = 1;
            labels[q] = cluster_id;

            /* Find neighbors of q */
            int nn2 = 0;
            for (int j = 0; j < nrows; j++)
                if (_dm_get(dm, q, j) <= eps) nn2++;

            if (nn2 >= min_pts) {
                /* Add unvisited neighbors to stack */
                for (int j = 0; j < nrows; j++) {
                    if (_dm_get(dm, q, j) <= eps && !visited[j])
                        stack[sp++] = j;
                }
            }
        }
        cluster_id++;
    }
    free(visited); free(stack); free(neighbors);
    return cluster_id;
}

int mapper_dbscan_data(const double *data, int nrows, int ncols,
                       metric_fn_t metric,
                       double eps, int min_pts, int *labels) {
    /* Precompute distance matrix, then use DM version */
    int dm_size = nrows * (nrows - 1) / 2;
    double *dm = malloc(dm_size * sizeof(double));
    if (!dm) return -1;
    mapper_distance_matrix(data, nrows, ncols, metric, dm);
    int nc = mapper_dbscan(dm, nrows, eps, min_pts, labels);
    free(dm);
    return nc;
}

/* ================================================================
 * Section 6: KD-Tree
 * ================================================================ */

#define KD_LEAF_SIZE 16

typedef struct {
    int split_dim;
    double split_val;
    int left, right; /* child indices, -1 for leaf */
    int start, end;  /* range in indices array */
} kd_node_t;

struct mapper_kdtree {
    kd_node_t *nodes;
    int nnodes, capacity;
    int *indices;
    const double *data;
    int npoints, ndims;
};

/* Quickselect: partition indices[lo..hi) so that index at position k
 * has the k-th smallest value in the given dimension. */
static void _qselect(int *indices, int lo, int hi, int k,
                     const double *data, int ndims, int dim) {
    while (lo < hi - 1) {
        int mid = lo + (hi - lo) / 2;
        double pval = data[indices[mid] * ndims + dim];
        /* swap pivot to end */
        { int t = indices[mid]; indices[mid] = indices[hi-1]; indices[hi-1] = t; }
        int store = lo;
        for (int i = lo; i < hi - 1; i++) {
            if (data[indices[i] * ndims + dim] < pval) {
                int t = indices[i]; indices[i] = indices[store]; indices[store] = t;
                store++;
            }
        }
        { int t = indices[store]; indices[store] = indices[hi-1]; indices[hi-1] = t; }
        if (store == k) return;
        if (store < k) lo = store + 1;
        else hi = store;
    }
}

static int _kd_build(mapper_kdtree *tree, int start, int end, int depth) {
    if (tree->nnodes >= tree->capacity) {
        tree->capacity = tree->capacity * 2 + 1;
        tree->nodes = realloc(tree->nodes, tree->capacity * sizeof(kd_node_t));
    }
    int ni = tree->nnodes++;
    kd_node_t *node = &tree->nodes[ni];
    node->start = start;
    node->end = end;

    if (end - start <= KD_LEAF_SIZE) {
        node->left = node->right = -1;
        node->split_dim = 0;
        node->split_val = 0;
        return ni;
    }

    int dim = depth % tree->ndims;
    int mid = (start + end) / 2;
    _qselect(tree->indices, start, end, mid, tree->data, tree->ndims, dim);

    node->split_dim = dim;
    node->split_val = tree->data[tree->indices[mid] * tree->ndims + dim];

    int left = _kd_build(tree, start, mid, depth + 1);
    int right = _kd_build(tree, mid, end, depth + 1);
    /* Re-fetch node pointer (realloc may have moved it) */
    tree->nodes[ni].left = left;
    tree->nodes[ni].right = right;
    return ni;
}

mapper_kdtree *mapper_kdtree_build(const double *data, int nrows, int ncols) {
    mapper_kdtree *tree = calloc(1, sizeof(mapper_kdtree));
    if (!tree) return NULL;
    tree->data = data;
    tree->npoints = nrows;
    tree->ndims = ncols;
    tree->capacity = 2 * nrows / KD_LEAF_SIZE + 1;
    tree->nodes = malloc(tree->capacity * sizeof(kd_node_t));
    tree->indices = malloc(nrows * sizeof(int));
    if (!tree->nodes || !tree->indices) {
        free(tree->nodes); free(tree->indices); free(tree); return NULL;
    }
    for (int i = 0; i < nrows; i++) tree->indices[i] = i;
    _kd_build(tree, 0, nrows, 0);
    return tree;
}

void mapper_kdtree_free(mapper_kdtree *tree) {
    if (tree) { free(tree->nodes); free(tree->indices); free(tree); }
}

static void _kd_range(const mapper_kdtree *tree, int ni,
                      const double *query, double eps_sq,
                      int *result, int *nres, int maxres) {
    const kd_node_t *node = &tree->nodes[ni];

    if (node->left == -1) {
        /* Leaf: check all points */
        for (int i = node->start; i < node->end && *nres < maxres; i++) {
            int idx = tree->indices[i];
            double dsq = 0.0;
            const double *pt = ROW(tree->data, idx, tree->ndims);
            for (int d = 0; d < tree->ndims; d++) {
                double diff = query[d] - pt[d];
                dsq += diff * diff;
            }
            if (dsq <= eps_sq)
                result[(*nres)++] = idx;
        }
        return;
    }

    double diff = query[node->split_dim] - node->split_val;
    int near = (diff <= 0) ? node->left : node->right;
    int far  = (diff <= 0) ? node->right : node->left;

    _kd_range(tree, near, query, eps_sq, result, nres, maxres);
    if (diff * diff <= eps_sq)
        _kd_range(tree, far, query, eps_sq, result, nres, maxres);
}

int mapper_kdtree_range(const mapper_kdtree *tree, int query_idx,
                        double eps, int *result, int max_results) {
    const double *query = ROW(tree->data, query_idx, tree->ndims);
    int nres = 0;
    _kd_range(tree, 0, query, eps * eps, result, &nres, max_results);
    return nres;
}

/* ================================================================
 * Section 7: DBSCAN with KD-tree (Euclidean only)
 * ================================================================ */

int mapper_dbscan_kdtree(const double *data, int nrows, int ncols,
                         double eps, int min_pts, int *labels) {
    mapper_kdtree *tree = mapper_kdtree_build(data, nrows, ncols);
    if (!tree) return -1;

    int *visited = calloc(nrows, sizeof(int));
    int *stack = malloc(nrows * sizeof(int));
    int *nbuf = malloc(nrows * sizeof(int));
    if (!visited || !stack || !nbuf) {
        free(visited); free(stack); free(nbuf);
        mapper_kdtree_free(tree);
        return -1;
    }
    for (int i = 0; i < nrows; i++) labels[i] = -1;
    int cluster_id = 0;

    for (int i = 0; i < nrows; i++) {
        if (visited[i]) continue;
        visited[i] = 1;

        int nn = mapper_kdtree_range(tree, i, eps, nbuf, nrows);
        if (nn < min_pts) continue;

        labels[i] = cluster_id;
        int sp = 0;
        for (int k = 0; k < nn; k++)
            if (nbuf[k] != i) stack[sp++] = nbuf[k];

        while (sp > 0) {
            int q = stack[--sp];
            if (labels[q] == -1) labels[q] = cluster_id;
            if (visited[q]) continue;
            visited[q] = 1;
            labels[q] = cluster_id;

            int nn2 = mapper_kdtree_range(tree, q, eps, nbuf, nrows);
            if (nn2 >= min_pts) {
                for (int k = 0; k < nn2; k++)
                    if (!visited[nbuf[k]])
                        stack[sp++] = nbuf[k];
            }
        }
        cluster_id++;
    }
    free(visited); free(stack); free(nbuf);
    mapper_kdtree_free(tree);
    return cluster_id;
}

/* ================================================================
 * Section 8: Sorted Set Intersection
 * ================================================================ */

int mapper_sorted_isect_count(const int *a, int na, const int *b, int nb) {
    int i = 0, j = 0, count = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) i++;
        else if (a[i] > b[j]) j++;
        else { count++; i++; j++; }
    }
    return count;
}

/* ================================================================
 * Section 9: TSV/CSV Reader
 * ================================================================
 * Reads tab-separated files (optionally gzipped).
 * Uses getline() for long lines and strtod() for fast parsing.
 */

static FILE *_tsv_open(const char *filename, int *is_pipe) {
    size_t len = strlen(filename);
    if (len >= 3 && strcmp(filename + len - 3, ".gz") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "zcat '%s'", filename);
        *is_pipe = 1;
        return popen(cmd, "r");
    }
    *is_pipe = 0;
    return fopen(filename, "r");
}

static void _tsv_close(FILE *fp, int is_pipe) {
    if (is_pipe) pclose(fp);
    else fclose(fp);
}

int mapper_tsv_dims(const char *filename, int skip_first_col,
                    int *out_nrows, int *out_ncols) {
    int is_pipe = 0;
    FILE *fp = _tsv_open(filename, &is_pipe);
    if (!fp) return -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    /* Skip header line */
    len = getline(&line, &cap, fp);
    if (len < 0) { free(line); _tsv_close(fp, is_pipe); return -1; }

    /* Read first data line, count tab characters */
    len = getline(&line, &cap, fp);
    if (len < 0) { free(line); _tsv_close(fp, is_pipe); return -1; }

    int nfields = 1;
    for (ssize_t i = 0; i < len; i++)
        if (line[i] == '\t') nfields++;

    int ncols = skip_first_col ? nfields - 1 : nfields;

    /* Count remaining data lines */
    int nrows = 1; /* already read one data line */
    while ((len = getline(&line, &cap, fp)) > 0) {
        /* Skip blank lines */
        if (len == 1 && line[0] == '\n') continue;
        nrows++;
    }

    free(line);
    _tsv_close(fp, is_pipe);
    *out_nrows = nrows;
    *out_ncols = ncols;
    return 0;
}

int mapper_tsv_read(const char *filename, int skip_first_col,
                    double *data, int nrows, int ncols) {
    int is_pipe = 0;
    FILE *fp = _tsv_open(filename, &is_pipe);
    if (!fp) return -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    /* Skip header line */
    len = getline(&line, &cap, fp);
    if (len < 0) { free(line); _tsv_close(fp, is_pipe); return -1; }

    int row = 0;
    while (row < nrows && (len = getline(&line, &cap, fp)) > 0) {
        if (len == 1 && line[0] == '\n') continue;

        char *p = line;
        /* Skip first column (patient ID) if requested */
        if (skip_first_col) {
            while (*p && *p != '\t') p++;
            if (*p == '\t') p++;
        }

        for (int col = 0; col < ncols; col++) {
            char *end;
            data[row * ncols + col] = strtod(p, &end);
            /* Advance past delimiter (tab or newline) */
            p = end;
            if (*p == '\t' || *p == ',') p++;
        }
        row++;
    }

    free(line);
    _tsv_close(fp, is_pipe);
    return (row == nrows) ? 0 : -1;
}
