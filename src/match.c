// match.c — generalized feature matcher with coarse-to-fine acceleration.
//
// Each library entry and each query tile is described by a fixed-size feature
// vector: multi-resolution [gray_s0][edge_s0][gray_s1][edge_s1]...
//
// Coarse-to-fine matching:
//   1. Coarse stage: L1 over just the first coarse_len bytes (gray at smallest
//      scale, typically 1024 bytes for 32×32) against all n_pages.
//      Collect top-K candidates per target.
//   2. Fine stage: L1 over the full feat_len bytes against only the K candidates.
//      This reduces memory traffic from n_pages × feat_len to
//      (n_pages × coarse_len) + (K × feat_len) per target.
//
// Distance is L1 (sum of absolute differences) over the feature bytes.
//
// OpenMP-parallel over pages in the coarse stage and over targets in the fine
// stage.
//
// Build with -fopenmp (Linux) or -Xpreprocessor -fopenmp -lomp (macOS + libomp).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

// SSE2/AVX2 intrinsics for x86_64
#if defined(__AVX2__)
#include <immintrin.h>
#define HAS_SIMD 1
#define SIMD_WIDTH 32
#elif defined(__SSE2__)
#include <emmintrin.h>
#define HAS_SIMD 1
#define SIMD_WIDTH 16
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAS_SIMD 1
#define SIMD_WIDTH 16
#else
#define HAS_SIMD 0
#define SIMD_WIDTH 1
#endif

static int g_match_threads = 0;

void match_set_threads(int n) {
    if (n > 0) g_match_threads = n;
}

// ── SIMD L1 distance ─────────────────────────────────────────────────────

#if HAS_SIMD && defined(__AVX2__)
static inline uint32_t feature_l1(const uint8_t* a, const uint8_t* b, int len) {
    int j = 0;
    __m256i acc = _mm256_setzero_si256();
    for (; j + 32 <= len; j += 32) {
        __m256i va = _mm256_loadu_si256((const __m256i*)(a + j));
        __m256i vb = _mm256_loadu_si256((const __m256i*)(b + j));
        // _mm256_sad_epu8 computes sum of absolute differences of 8-bit unsigned
        // integers in 16-byte lanes (result in 64-bit lanes, zeros in between)
        acc = _mm256_add_epi64(acc, _mm256_sad_epu8(va, vb));
    }
    // Horizontal sum of the 4 x 64-bit accumulators
    __m128i lo = _mm256_castsi256_si128(acc);
    __m128i hi = _mm256_extracti128_si256(acc, 1);
    __m128i sum128 = _mm_add_epi64(lo, hi);
    uint64_t parts[2];
    _mm_storeu_si128((__m128i*)parts, sum128);
    uint64_t s = parts[0] + parts[1];
    // Handle remaining bytes
    for (; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        s += (uint64_t)(d < 0 ? -d : d);
    }
    return (s > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)s;
}

#elif HAS_SIMD && defined(__SSE2__)
static inline uint32_t feature_l1(const uint8_t* a, const uint8_t* b, int len) {
    int j = 0;
    __m128i acc = _mm_setzero_si128();
    for (; j + 16 <= len; j += 16) {
        __m128i va = _mm_loadu_si128((const __m128i*)(a + j));
        __m128i vb = _mm_loadu_si128((const __m128i*)(b + j));
        acc = _mm_add_epi64(acc, _mm_sad_epu8(va, vb));
    }
    // Horizontal sum: extract two 64-bit lanes via memcpy (portable SSE2)
    uint64_t parts[2];
    _mm_storeu_si128((__m128i*)parts, acc);
    uint64_t s = parts[0] + parts[1];
    for (; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        s += (uint64_t)(d < 0 ? -d : d);
    }
    return (s > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)s;
}

#elif HAS_SIMD && defined(__ARM_NEON)
static inline uint32_t feature_l1(const uint8_t* a, const uint8_t* b, int len) {
    int j = 0;
    uint32x4_t acc = vdupq_n_u32(0);
    for (; j + 16 <= len; j += 16) {
        uint8x16_t va = vld1q_u8(a + j);
        uint8x16_t vb = vld1q_u8(b + j);
        // vabdq_u8: absolute difference of unsigned 8-bit integers
        uint8x16_t diff = vabdq_u8(va, vb);
        // Accumulate into 32-bit: sum the 16 byte diffs into 4x32-bit lanes
        acc = vpadalq_u16(acc, vpaddlq_u8(diff));
    }
    // Horizontal sum of uint32x4_t
    uint32x2_t sum_pair = vadd_u32(vget_low_u32(acc), vget_high_u32(acc));
    uint64_t s = (uint64_t)vget_lane_u32(sum_pair, 0) + (uint64_t)vget_lane_u32(sum_pair, 1);
    for (; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        s += (uint32_t)(d < 0 ? -d : d);
    }
    return (s > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)s;
}

#else
// Scalar fallback
static inline uint32_t feature_l1(const uint8_t* a, const uint8_t* b, int len) {
    uint32_t dist = 0;
    for (int j = 0; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        dist += (uint32_t)(d < 0 ? -d : d);
    }
    return dist;
}
#endif

// ── L1 with early termination ────────────────────────────────────────────
// Same as feature_l1 but stops as soon as running sum exceeds `bound`.
// Returns the partial distance (which may be >= bound if not pruned).
static inline uint32_t feature_l1_bounded(const uint8_t* a, const uint8_t* b,
                                           int len, uint32_t bound) {
#if HAS_SIMD && defined(__AVX2__)
    int j = 0;
    __m256i acc = _mm256_setzero_si256();
    for (; j + 32 <= len; j += 32) {
        __m256i va = _mm256_loadu_si256((const __m256i*)(a + j));
        __m256i vb = _mm256_loadu_si256((const __m256i*)(b + j));
        acc = _mm256_add_epi64(acc, _mm256_sad_epu8(va, vb));
        // Check every 4 iterations (128 bytes) via horizontal sum to avoid
        // false positives from per-lane overflow.
        if (((j + 32) % 128) == 0) {
            __m128i lo = _mm256_castsi256_si128(acc);
            __m128i hi = _mm256_extracti128_si256(acc, 1);
            __m128i sum128 = _mm_add_epi64(lo, hi);
            uint64_t parts[2];
            _mm_storeu_si128((__m128i*)parts, sum128);
            uint64_t s = parts[0] + parts[1];
            if (s > bound) return (uint32_t)(s > 0xFFFFFFFF ? 0xFFFFFFFF : s);
        }
    }
    __m128i lo = _mm256_castsi256_si128(acc);
    __m128i hi = _mm256_extracti128_si256(acc, 1);
    __m128i sum128 = _mm_add_epi64(lo, hi);
    uint64_t parts[2];
    _mm_storeu_si128((__m128i*)parts, sum128);
    uint64_t s = parts[0] + parts[1];
    for (; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        s += (uint64_t)(d < 0 ? -d : d);
        if (s > bound) return (uint32_t)(s > 0xFFFFFFFF ? 0xFFFFFFFF : s);
    }
    return (uint32_t)s;

#elif HAS_SIMD && defined(__ARM_NEON)
    int j = 0;
    uint32x4_t acc = vdupq_n_u32(0);
    for (; j + 16 <= len; j += 16) {
        uint8x16_t va = vld1q_u8(a + j);
        uint8x16_t vb = vld1q_u8(b + j);
        uint8x16_t diff = vabdq_u8(va, vb);
        acc = vpadalq_u16(acc, vpaddlq_u8(diff));
        /* Check every 4 iterations (64 bytes) to amortize hsum cost. */
        if (((j + 16) % 64) == 0) {
            uint32_t s = vaddvq_u32(acc);
            if (s > bound) return s;
        }
    }
    uint32_t s = vaddvq_u32(acc);
    for (; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        s += (uint32_t)(d < 0 ? -d : d);
        if (s > bound) return s;
    }
    return s;
#else
    uint32_t dist = 0;
    for (int j = 0; j < len; j++) {
        int d = (int)a[j] - (int)b[j];
        dist += (uint32_t)(d < 0 ? -d : d);
        if (dist > bound) return dist;
    }
    return dist;
#endif
}

// ── Coarse-to-fine batch matching ────────────────────────────────────────
//
// Coarse-to-fine matching:
//   1. Coarse stage: compare only the first coarse_len bytes (smallest-scale
//      gray feature) against all n_pages. Collect top-K candidates.
//   2. Fine stage: compare full feat_len bytes against only those K candidates.
//
// This reduces memory traffic from n_pages × feat_len to
// (n_pages × coarse_len) + (K × feat_len) per frame.
//
// With coarse_len=1024 (32×32 gray), K=16, feat_len=43008, n_pages=2000:
//   Old: 2000 × 43008 = 86 MB
//   New: 2000 × 1024 + 16 × 43008 = 2 MB + 0.7 MB = 2.7 MB (32× reduction)

void match_batch_coarse(const uint8_t* lib, const uint8_t* targets,
                        int n_pages, int num_targets, int feat_len,
                        int coarse_len, int* results) {
    if (num_targets == 0 || n_pages == 0) return;

#ifdef _OPENMP
    if (g_match_threads > 0) omp_set_num_threads(g_match_threads);
#endif

    if (n_pages == 1) {
        for (int t = 0; t < num_targets; t++) results[t] = 0;
        return;
    }

    // If coarse_len >= feat_len, the coarse stage already computes the full
    // distance — the fine stage would be 100% redundant work.
    int fine_needed = (coarse_len < feat_len);
    if (coarse_len >= feat_len) {
        coarse_len = feat_len;  // ensure consistency
    }

    // Number of candidates to keep per target for the fine stage
    int K = 16;
    if (K > n_pages) K = n_pages;

    uint32_t *merged_dist = NULL;
    int *merged_best = NULL;

#ifdef _OPENMP
    // ── Coarse stage: OpenMP parallel over pages ──────────────────────
    {
        int nthreads = omp_get_max_threads();
        int omp_ok = 1;
        uint32_t **t_coarse_dist = (uint32_t **)malloc((size_t)nthreads * sizeof(uint32_t *));
        int **t_coarse_best = (int **)malloc((size_t)nthreads * sizeof(int *));

        if (!t_coarse_dist || !t_coarse_best) {
            omp_ok = 0;
            free(t_coarse_dist);
            free(t_coarse_best);
        }

        if (omp_ok) {
            for (int th = 0; th < nthreads; th++) {
                t_coarse_dist[th] = (uint32_t *)malloc((size_t)num_targets * K * sizeof(uint32_t));
                t_coarse_best[th] = (int *)malloc((size_t)num_targets * K * sizeof(int));
                if (!t_coarse_dist[th] || !t_coarse_best[th]) {
                    omp_ok = 0;
                    break;
                }
                for (int t = 0; t < num_targets * K; t++) {
                    t_coarse_dist[th][t] = 0xFFFFFFFF;
                    t_coarse_best[th][t] = -1;
                }
            }
        }

        if (omp_ok) {
            #pragma omp parallel
            {
                int th = omp_get_thread_num();
                uint32_t *td = t_coarse_dist[th];
                int *tb = t_coarse_best[th];

                #pragma omp for schedule(static)
                for (int i = 0; i < n_pages; i++) {
                    const uint8_t* page = &lib[(size_t)i * feat_len];
                    for (int t = 0; t < num_targets; t++) {
                        const uint8_t* target = &targets[(size_t)t * feat_len];
                        uint32_t d = feature_l1(page, target, coarse_len);

                        uint32_t *kd = &td[(size_t)t * K];
                        int *kb = &tb[(size_t)t * K];
                        if (d >= kd[K - 1]) continue;
                        kd[K - 1] = d;
                        kb[K - 1] = i;
                        for (int k = K - 2; k >= 0; k--) {
                            if (kd[k + 1] < kd[k]) {
                                uint32_t td2 = kd[k]; kd[k] = kd[k + 1]; kd[k + 1] = td2;
                                int tb2 = kb[k]; kb[k] = kb[k + 1]; kb[k + 1] = tb2;
                            } else {
                                break;
                            }
                        }
                    }
                }
            }

            // Merge per-thread top-K results
            merged_dist = (uint32_t *)malloc((size_t)num_targets * K * sizeof(uint32_t));
            merged_best = (int *)malloc((size_t)num_targets * K * sizeof(int));

            if (merged_dist && merged_best) {
                for (int t = 0; t < num_targets; t++) {
                    for (int k = 0; k < K; k++) {
                        merged_dist[(size_t)t * K + k] = 0xFFFFFFFF;
                        merged_best[(size_t)t * K + k] = -1;
                    }
                    for (int th = 0; th < nthreads; th++) {
                        uint32_t *td = &t_coarse_dist[th][(size_t)t * K];
                        int *kb = &t_coarse_best[th][(size_t)t * K];
                        for (int k = 0; k < K; k++) {
                            if (kb[k] < 0) continue;
                            uint32_t d = td[k];
                            uint32_t *md = &merged_dist[(size_t)t * K];
                            int *mb = &merged_best[(size_t)t * K];
                            if (d >= md[K - 1]) continue;
                            md[K - 1] = d;
                            mb[K - 1] = kb[k];
                            for (int kk = K - 2; kk >= 0; kk--) {
                                if (md[kk + 1] < md[kk]) {
                                    uint32_t td2 = md[kk]; md[kk] = md[kk + 1]; md[kk + 1] = td2;
                                    int tb2 = mb[kk]; mb[kk] = mb[kk + 1]; mb[kk + 1] = tb2;
                                } else {
                                    break;
                                }
                            }
                        }
                    }
                }
            } else {
                free(merged_dist);
                free(merged_best);
                merged_dist = NULL;
                merged_best = NULL;
            }
        }

        if (t_coarse_dist) {
            for (int th = 0; th < nthreads; th++) {
                free(t_coarse_dist[th]);
                free(t_coarse_best[th]);
            }
            free(t_coarse_dist);
            free(t_coarse_best);
        }
    }
#endif

    if (!merged_dist || !merged_best) {
        merged_dist = (uint32_t *)malloc((size_t)num_targets * K * sizeof(uint32_t));
        merged_best = (int *)malloc((size_t)num_targets * K * sizeof(int));

        if (!merged_dist || !merged_best) {
            free(merged_dist);
            free(merged_best);
            for (int t = 0; t < num_targets; t++) results[t] = 0;
            return;
        }

        for (int t = 0; t < num_targets; t++) {
            for (int k = 0; k < K; k++) {
                merged_dist[(size_t)t * K + k] = 0xFFFFFFFF;
                merged_best[(size_t)t * K + k] = -1;
            }
        }

        for (int i = 0; i < n_pages; i++) {
            const uint8_t* page = &lib[(size_t)i * feat_len];
            for (int t = 0; t < num_targets; t++) {
                const uint8_t* target = &targets[(size_t)t * feat_len];
                uint32_t d = feature_l1(page, target, coarse_len);

                uint32_t *kd = &merged_dist[(size_t)t * K];
                int *kb = &merged_best[(size_t)t * K];
                if (d >= kd[K - 1]) continue;
                kd[K - 1] = d;
                kb[K - 1] = i;
                for (int k = K - 2; k >= 0; k--) {
                    if (kd[k + 1] < kd[k]) {
                        uint32_t td2 = kd[k]; kd[k] = kd[k + 1]; kd[k + 1] = td2;
                        int tb2 = kb[k]; kb[k] = kb[k + 1]; kb[k + 1] = tb2;
                    } else {
                        break;
                    }
                }
            }
        }
    }

    if (fine_needed) {
        // ── Fine stage: full-feature L1 against top-K candidates ───────────
#ifdef _OPENMP
        #pragma omp parallel for schedule(static)
#endif
        for (int t = 0; t < num_targets; t++) {
            uint32_t best_d = 0xFFFFFFFF;
            int best_i = -1;
            const uint8_t* target = &targets[(size_t)t * feat_len];
            int *kb = &merged_best[(size_t)t * K];
            for (int k = 0; k < K; k++) {
                if (kb[k] < 0) break;
                const uint8_t* page = &lib[(size_t)kb[k] * feat_len];
                uint32_t d = feature_l1_bounded(page, target, feat_len, best_d);
                if (d < best_d) {
                    best_d = d;
                    best_i = kb[k];
                }
            }
            if (best_i == -1) best_i = merged_best[(size_t)t * K];
            results[t] = best_i;
        }
    } else {
        // Coarse stage already used the full feature vector — the best
        // candidate at index 0 is the final answer.
        for (int t = 0; t < num_targets; t++) {
            results[t] = merged_best[(size_t)t * K];
        }
    }

    free(merged_dist);
    free(merged_best);
}

void match_batch(const uint8_t* lib, const uint8_t* targets,
                 int n_pages, int num_targets, int feat_len, int* results) {
    match_batch_coarse(lib, targets, n_pages, num_targets, feat_len,
                       feat_len /* coarse_len = full for backward compat */,
                       results);
}
