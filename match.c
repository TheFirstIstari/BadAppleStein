#include <stdint.h>
#include <omp.h>

// Matches multiple target tiles against the library in one C call.
// targets: Array of bitmasks for current frame tiles [num_targets * 64]
// results: Array to store the resulting best PDF IDs
void match_batch(const uint64_t* lib, const uint64_t* targets, int n_pages, int num_targets, int* results) {
    #pragma omp parallel for schedule(dynamic)
    for (int t = 0; t < num_targets; t++) {
        const uint64_t* target = &targets[t * 64];
        uint32_t min_dist = 0xFFFFFFFF;
        int best_idx = 0;

        for (int i = 0; i < n_pages; i++) {
            uint32_t dist = 0;
            const uint64_t* current_sig = &lib[i * 64];
            
            // Unrolled loop for speed
            for (int j = 0; j < 64; j++) {
                dist += __builtin_popcountll(current_sig[j] ^ target[j]);
            }

            if (dist < min_dist) {
                min_dist = dist;
                best_idx = i;
                if (dist == 0) break; // Perfect match, stop searching
            }
        }
        results[t] = best_idx;
    }
}