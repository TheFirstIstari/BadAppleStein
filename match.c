// match.c — generalized feature matcher.
//
// Each library entry and each query tile is described by a fixed-size feature
// vector: an N x N grid of cells, each holding `channels` quantized
// values of `G` bits (0 < G <= 8, stored one value per uint8_t byte).
// For grayscale channels=1; for color channels=3 (BGR order).
//
// Distance is L1 (sum of absolute differences) over the feature bytes.
// This subsumes the old 1-bit Hamming matcher: with G=1 each byte is
// 0/1 and L1 == popcount-of-XOR, so the legacy behaviour is recovered.
//
// Matching is pages-outer for cache locality: each library page is loaded
// once and compared against all targets (targets stay in L1 cache).
//
// OpenMP-parallel over pages; each thread gets a chunk of pages and uses
// thread-local best tracking to avoid false sharing.
//
// Build with -fopenmp (Linux) or -Xpreprocessor -fopenmp -lomp (macOS + libomp).
#include <stdint.h>
#include <stdlib.h>

#ifdef _OPENMP
#include <omp.h>
#endif

static int g_match_threads = 0;

void match_set_threads(int n) {
    if (n > 0) g_match_threads = n;
}

// Compute L1 distance between two equal-length feature vectors.
// Returns the sum of absolute byte differences.
static inline uint32_t feature_l1(const uint8_t* a, const uint8_t* b, int len) {
    uint32_t dist = 0;
    for (int j = 0; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        dist += (uint32_t)(d < 0 ? -d : d);
    }
    return dist;
}

// Match a batch of query tiles against the library.
//
//   lib      : n_pages * feat_len bytes (row-major library features)
//   targets  : num_targets * feat_len bytes (query features)
//   n_pages, num_targets : counts
//   feat_len : bytes per feature (= N*N*channels)
//   results  : out, num_targets int32 best library index per target
//
// feat_len is passed explicitly so the same compiled binary handles any
// (N, G, channels) configuration without recompiling.
//
// Pages-outer ordering: each library page is loaded once and compared
// against all targets. This keeps the small targets array in L1 cache
// while streaming the large library sequentially.
void match_batch(const uint8_t* lib, const uint8_t* targets,
                 int n_pages, int num_targets, int feat_len, int* results) {
    if (num_targets == 0 || n_pages == 0) return;

#ifdef _OPENMP
    /* Thread count can be set via match_set_threads() before calling. */
    if (g_match_threads > 0) omp_set_num_threads(g_match_threads);
#endif

    if (n_pages == 1) {
        for (int t = 0; t < num_targets; t++) {
            results[t] = 0;
        }
        return;
    }

#ifdef _OPENMP
    int nthreads = omp_get_max_threads();
    // Thread-local best tracking to avoid false sharing on dist/best arrays
    uint32_t **t_dist = (uint32_t **)malloc((size_t)nthreads * sizeof(uint32_t *));
    int **t_best = (int **)malloc((size_t)nthreads * sizeof(int *));
    for (int th = 0; th < nthreads; th++) {
        t_dist[th] = (uint32_t *)malloc((size_t)num_targets * sizeof(uint32_t));
        t_best[th] = (int *)malloc((size_t)num_targets * sizeof(int));
        for (int t = 0; t < num_targets; t++) {
            t_dist[th][t] = 0xFFFFFFFF;
            t_best[th][t] = 0;
        }
    }

    #pragma omp parallel
    {
        int th = omp_get_thread_num();
        // Dynamic schedule over pages: each thread gets a chunk of pages
        // and compares them against all targets (targets fit in L1).
        #pragma omp for schedule(dynamic, 8)
        for (int i = 0; i < n_pages; i++) {
            const uint8_t* page = &lib[(size_t)i * feat_len];
            for (int t = 0; t < num_targets; t++) {
                const uint8_t* target = &targets[(size_t)t * feat_len];
                uint32_t d = feature_l1(page, target, feat_len);
                if (d < t_dist[th][t]) {
                    t_dist[th][t] = d;
                    t_best[th][t] = i;
                }
            }
        }
    }

    // Reduce: take minimum distance per target across all threads
    for (int t = 0; t < num_targets; t++) {
        uint32_t best_d = 0xFFFFFFFF;
        int best_i = 0;
        for (int th = 0; th < nthreads; th++) {
            if (t_dist[th][t] < best_d) {
                best_d = t_dist[th][t];
                best_i = t_best[th][t];
            }
        }
        results[t] = best_i;
    }

    for (int th = 0; th < nthreads; th++) {
        free(t_dist[th]);
        free(t_best[th]);
    }
    free(t_dist);
    free(t_best);
#else
    // Single-threaded: pages-outer for better cache locality.
    // Each library page is loaded once; all targets (small, L1-resident)
    // are compared against it before moving to the next page.
    uint32_t *dist = (uint32_t *)malloc((size_t)num_targets * sizeof(uint32_t));
    int *best = (int *)malloc((size_t)num_targets * sizeof(int));
    for (int t = 0; t < num_targets; t++) {
        dist[t] = 0xFFFFFFFF;
        best[t] = 0;
    }

    for (int i = 0; i < n_pages; i++) {
        const uint8_t* page = &lib[(size_t)i * feat_len];
        for (int t = 0; t < num_targets; t++) {
            const uint8_t* target = &targets[(size_t)t * feat_len];
            uint32_t d = feature_l1(page, target, feat_len);
            if (d < dist[t]) {
                dist[t] = d;
                best[t] = i;
            }
        }
    }

    for (int t = 0; t < num_targets; t++) results[t] = best[t];
    free(dist);
    free(best);
#endif
}
