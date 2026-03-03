/* hnsw-c.cpp -- C wrapper for hnswlib C++ library
 *
 * Wraps hnswlib's HierarchicalNSW into a flat C function for
 * use from Lush via mod-load.  Handles float/double conversion
 * and self-exclusion from neighbor results.
 *
 * Copyright (C) 2026 Scott Locklin, Lugos LLC
 * Distributed under the GNU General Public License v2+.
 */

#include "hnsw-c.h"
#include "hnswlib/hnswlib.h"
#include <cmath>
#include <vector>
#include <algorithm>
#include <utility>

extern "C" {

void hnsw_all_knn(const double *data, int n, int d, int k,
                  int *knn_idx, double *knn_dist,
                  int M, int ef_construction, int ef_search) {
    /* hnswlib operates on float; convert from double */
    std::vector<float> fdata((size_t)n * d);
    for (int i = 0; i < n * d; i++)
        fdata[i] = static_cast<float>(data[i]);

    /* Build HNSW index */
    hnswlib::L2Space space((size_t)d);
    hnswlib::HierarchicalNSW<float> index(
        &space, (size_t)n, (size_t)M, (size_t)ef_construction);

    for (int i = 0; i < n; i++)
        index.addPoint(fdata.data() + (size_t)i * d, (size_t)i);

    index.setEf((size_t)ef_search);

    /* Search each point for k nearest neighbors */
    for (int i = 0; i < n; i++) {
        /* Request k+1 results to account for self-match */
        auto result = index.searchKnn(
            fdata.data() + (size_t)i * d, (size_t)(k + 1));

        /* Extract from max-heap, excluding self */
        std::vector<std::pair<float, int>> neighbors;
        neighbors.reserve(k);
        while (!result.empty()) {
            auto top = result.top();
            result.pop();
            if (static_cast<int>(top.second) != i)
                neighbors.push_back(
                    {top.first, static_cast<int>(top.second)});
        }

        /* Sort ascending by distance */
        std::sort(neighbors.begin(), neighbors.end());

        /* Fill output arrays (L2Space returns squared distance) */
        for (int j = 0; j < k; j++) {
            if (j < static_cast<int>(neighbors.size())) {
                knn_idx[i * k + j] = neighbors[j].second;
                knn_dist[i * k + j] =
                    std::sqrt(static_cast<double>(neighbors[j].first));
            } else {
                knn_idx[i * k + j] = -1;
                knn_dist[i * k + j] = 1e30;
            }
        }
    }
}

} /* extern "C" */
