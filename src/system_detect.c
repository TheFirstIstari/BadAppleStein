/*
 * system_detect.c — cross-platform resource detection (POSIX, C99).
 *
 * macOS: sysctl for memory, sysconf for cores.
 * Linux: sysconf for memory, sysconf for cores.
 * Fallback: conservative defaults when detection fails.
 */
#include "system_detect.h"

#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <stdint.h>
#elif defined(__linux__)
#include <stdio.h>
#include <string.h>
#endif

/* ── Core detection ────────────────────────────────────────────── */

static long detect_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? n : 1;
}

/* ── Memory detection ──────────────────────────────────────────── */

static size_t detect_total_memory(void) {
#if defined(__APPLE__)
    {
        uint64_t memsize = 0;
        size_t sz = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &sz, NULL, 0) == 0 && memsize > 0)
            return (size_t)memsize;
    }
#endif
    /* POSIX fallback — works on Linux and macOS */
    {
        long pages     = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page_size > 0)
            return (size_t)pages * (size_t)page_size;
    }
    return 0;
}

/* ── Cache budget heuristic ────────────────────────────────────── */

size_t system_cache_budget(size_t total_memory) {
    if (total_memory == 0)
        return 64 * 1024 * 1024;                            /* unknown: 64 MB */
    if (total_memory < 512u * 1024u * 1024u)
        return 0;                                            /* < 512 MB: no cache */
    if (total_memory < 1024u * 1024u * 1024u)
        return total_memory / 10;                            /* 512 MB–1 GB: 10% */
    {
        size_t budget = total_memory / 4;                    /* >= 1 GB: 25% */
        size_t cap    = 4ULL * 1024 * 1024 * 1024;          /* cap at 4 GB */
        return budget < cap ? budget : cap;
    }
}

/* ── Public API ────────────────────────────────────────────────── */

SystemConfig system_detect(void) {
    SystemConfig r = {0};
    r.total_memory_bytes = detect_total_memory();
    r.cpu_cores          = detect_cores();
    r.cache_budget_bytes = system_cache_budget(r.total_memory_bytes);
    r.cache_enabled      = (r.cache_budget_bytes > 0);
    r.num_threads        = (int)r.cpu_cores;

    /* On very constrained systems, force single-threaded */
    if (r.total_memory_bytes > 0 && r.total_memory_bytes < 512u * 1024u * 1024u)
        r.num_threads = 1;

    return r;
}
