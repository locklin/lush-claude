/* mapper-c.h -- C acceleration for the Lush Mapper/TDA package
 *
 * Distance metrics adapted from the C Clustering Library (hclust.c)
 * by Michiel Jan Laurens de Hoon, with bug fixes.
 * SLINK algorithm: Sibson (1973), adapted from hclust.c.
 * DBSCAN, KD-tree, sorted intersection: original code.
 *
 * NOTE: Lush int-matrix stores 4-byte int elements (ST_I type).
 * All integer array parameters use 'int *' to match.
 */

#ifndef MAPPER_C_H
#define MAPPER_C_H

#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <string.h>

/* ================================================================
 * Distance metrics
 * ================================================================
 * All metrics operate on a flat row-major data array.
 * data[i * ncols + k] = element k of row i.
 */

typedef double (*metric_fn_t)(const double *data, int ncols, int i, int j);

metric_fn_t mapper_select_metric(const char *name);

/* Compute lower-triangle distance matrix.
 * dm must have nrows*(nrows-1)/2 elements.
 * Access: dm[i*(i-1)/2 + j] for j < i. */
void mapper_distance_matrix(const double *data, int nrows, int ncols,
                            metric_fn_t metric, double *dm);

/* ================================================================
 * SLINK single-linkage clustering
 * ================================================================
 * Output arrays must have (nrows-1) elements each.
 * merge_left[k], merge_right[k] = children of merge k.
 * merge_dist[k] = distance of merge k (sorted ascending).
 * Elements: 0..n-1, clusters: -1..-(n-1).
 * Returns 0 on success, -1 on memory error.
 */

int mapper_slink(const double *dm, int nrows,
                 int *merge_left, int *merge_right, double *merge_dist);

int mapper_slink_data(const double *data, int nrows, int ncols,
                      metric_fn_t metric,
                      int *merge_left, int *merge_right, double *merge_dist);

/* Cut dendrogram into nclusters clusters.
 * labels must have nelements entries. */
void mapper_cut_tree(int nelements,
                     const int *merge_left, const int *merge_right,
                     const double *merge_dist,
                     int nclusters, int *labels);

/* Cut dendrogram at distance threshold.
 * Returns number of clusters. */
int mapper_cut_tree_dist(int nelements,
                         const int *merge_left, const int *merge_right,
                         const double *merge_dist,
                         double threshold, int *labels);

/* ================================================================
 * DBSCAN
 * ================================================================
 * labels must have nrows entries. Noise = -1.
 * Returns number of clusters found.
 */

int mapper_dbscan(const double *dm, int nrows,
                  double eps, int min_pts, int *labels);

int mapper_dbscan_data(const double *data, int nrows, int ncols,
                       metric_fn_t metric,
                       double eps, int min_pts, int *labels);

/* DBSCAN with KD-tree (Euclidean only) */
int mapper_dbscan_kdtree(const double *data, int nrows, int ncols,
                         double eps, int min_pts, int *labels);

/* ================================================================
 * KD-tree (Euclidean range queries)
 * ================================================================ */

typedef struct mapper_kdtree mapper_kdtree;

mapper_kdtree *mapper_kdtree_build(const double *data, int nrows, int ncols);
void mapper_kdtree_free(mapper_kdtree *tree);
/* Returns count of neighbors found. */
int mapper_kdtree_range(const mapper_kdtree *tree, int query_idx,
                        double eps, int *result, int max_results);

/* ================================================================
 * Sorted set intersection
 * ================================================================ */

int mapper_sorted_isect_count(const int *a, int na, const int *b, int nb);

/* ================================================================
 * TSV/CSV reader (tab-separated, optionally gzipped)
 * ================================================================ */

int mapper_tsv_dims(const char *filename, int skip_first_col,
                    int *out_nrows, int *out_ncols);
int mapper_tsv_read(const char *filename, int skip_first_col,
                    double *data, int nrows, int ncols);

#endif /* MAPPER_C_H */
