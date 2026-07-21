/*
 * system_detect.h — cross-platform resource detection (single header, C99).
 *
 * Detects CPU cores and physical memory at runtime, computes adaptive
 * cache budgets and thread counts. No external dependencies beyond POSIX.
 */
#ifndef SYSTEM_DETECT_H
#define SYSTEM_DETECT_H

#include <stddef.h>

typedef struct {
    size_t total_memory_bytes;   /* Physical RAM */
    long   cpu_cores;            /* Online CPU cores */
    int    num_threads;          /* Adaptive thread count for OpenMP */
    size_t cache_budget_bytes;   /* Atlas cache budget (0 = disabled) */
    int    cache_enabled;        /* 0 = skip atlas cache */
} SystemConfig;

/*
 * Detect system resources at startup. Call once.
 * Respects OMP_NUM_THREADS if set.
 */
SystemConfig system_detect(void);

/*
 * Compute a cache budget from total memory.
 * Returns 0 for < 512 MB, 10% for 512 MB-1 GB, 25% (capped at 2 GB) for >= 1 GB.
 */
size_t system_cache_budget(size_t total_memory);

#endif /* SYSTEM_DETECT_H */
