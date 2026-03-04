/* hnsw-c.h -- C wrapper for hnswlib C++ library
 *
 * Provides a flat C API for building HNSW indices and performing
 * all-kNN search.  Compiled with g++ and loaded into Lush.
 *
 * Copyright (C) 2026 Scott Locklin, Lugos LLC
 * Distributed under the GNU General Public License v2+.
 */

#ifndef HNSW_C_H
#define HNSW_C_H

#ifdef __cplusplus
extern "C" {
#endif

/* Build an HNSW index from data and find k nearest neighbors for every point.
 *
 * data:            N x D row-major doubles (input)
 * n, d:            dimensions of data
 * k:               number of neighbors per point
 * knn_idx:         N x K row-major int output (neighbor indices)
 * knn_dist:        N x K row-major double output (Euclidean distances, ascending)
 * M:               HNSW connections per node (higher = more memory, better recall)
 * ef_construction: build-time quality parameter (higher = slower build, better index)
 * ef_search:       query-time quality parameter (higher = slower query, better recall)
 */
void hnsw_all_knn(const double *data, int n, int d, int k,
                  int *knn_idx, double *knn_dist,
                  int M, int ef_construction, int ef_search);

#ifdef __cplusplus
}
#endif

#endif /* HNSW_C_H */
