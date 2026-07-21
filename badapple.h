/*
 * badapple.h — shared types and on-disk formats for the pure-C BadAppleStein tool.
 *
 * No OpenCV. Everything is plain C (C99) using libav* for video/raster I/O and
 * mupdf for PDF rasterization. The matcher (match.c) is linked into arrange.
 *
 * Feature format (features.bin):
 *   uint32 n_pages
 *   uint32 N        (grid is N x N cells)
 *   uint32 G        (bits per cell, 1..8)
 *   uint32 channels (1 = grayscale, 3 = BGR color)
 *   then n_pages * (N*N*channels) bytes, row-major, one uint8 per cell value.
 *
 * Registry format (registry.bin):
 *   uint32 n
 *   then per entry: int32 page_idx ; uint32 path_len ; path_len bytes (UTF-8).
 *
 * Manifest format (<dir>/<frame04d>.bin):
 *   uint32 n
 *   then n records of 24 bytes: int32 x, y, w, h, op_id, page_idx.
 */
#ifndef BADAPPLE_H
#define BADAPPLE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BA_MAGIC_1 0x4241u /* not currently used; reserved */

/* On-disk helpers (little-endian via fixed widths; host is LE on macOS/Linux). */
static inline int ba_read_file(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return -1; }
    *out = buf; *out_len = (size_t)sz;
    return 0;
}

/* Load features.bin header + data. Caller frees *features with free(). */
typedef struct {
    int n_pages;
    int N;
    int G;
    int channels;
    int feat_len;          /* N*N*channels */
    uint8_t *data;         /* n_pages * feat_len bytes */
} FeatureDB;

static inline int ba_load_features(const char *path, FeatureDB *db) {
    uint8_t *buf; size_t len;
    if (ba_read_file(path, &buf, &len) != 0) return -1;
    if (len < 16) { free(buf); return -1; }
    uint32_t n, N, G, ch;
    memcpy(&n, buf, 4); memcpy(&N, buf + 4, 4); memcpy(&G, buf + 8, 4); memcpy(&ch, buf + 12, 4);
    size_t need = 16 + (size_t)n * N * N * (size_t)ch;
    if (len < need) { free(buf); return -1; }
    db->n_pages = (int)n; db->N = (int)N; db->G = (int)G; db->channels = (int)ch;
    db->feat_len = (int)((size_t)N * N * (size_t)ch);
    /* keep header+data in one buffer; data points just past the 16-byte header. */
    db->data = buf + 16;
    /* We must not free header separately; stash the allocation for free(). */
    /* Store the base pointer in a field we add: reuse 'data' base via a wrapper. */
    /* Simpler: copy data out and free buf. */
    uint8_t *data = (uint8_t *)malloc(need - 16);
    if (!data) { free(buf); return -1; }
    memcpy(data, buf + 16, need - 16);
    free(buf);
    db->data = data;
    return 0;
}

static inline void ba_free_features(FeatureDB *db) {
    free(db->data); db->data = NULL;
}

typedef struct {
    char *pdf_path;
    int32_t page_idx;
} RegEntry;

typedef struct {
    int n;
    RegEntry *entries;
} Registry;

/* Load registry.bin. Caller frees with ba_free_registry(). */
static inline int ba_load_registry(const char *path, Registry *reg) {
    uint8_t *buf; size_t len;
    if (ba_read_file(path, &buf, &len) != 0) return -1;
    if (len < 4) { free(buf); return -1; }
    uint32_t n; memcpy(&n, buf, 4);
    RegEntry *e = (RegEntry *)calloc(n ? n : 1, sizeof(RegEntry));
    if (!e) { free(buf); return -1; }
    size_t off = 4;
    for (uint32_t i = 0; i < n; i++) {
        if (off + 8 > len) { free(e); free(buf); return -1; }
        int32_t page_idx; uint32_t plen;
        memcpy(&page_idx, buf + off, 4); off += 4;
        memcpy(&plen, buf + off, 4); off += 4;
        if (off + plen > len) { free(e); free(buf); return -1; }
        char *p = (char *)malloc(plen + 1);
        if (!p) { free(e); free(buf); return -1; }
        memcpy(p, buf + off, plen); p[plen] = '\0';
        e[i].pdf_path = p; e[i].page_idx = page_idx;
        off += plen;
    }
    free(buf);
    reg->n = (int)n; reg->entries = e;
    return 0;
}

static inline void ba_free_registry(Registry *reg) {
    for (int i = 0; i < reg->n; i++) free(reg->entries[i].pdf_path);
    free(reg->entries);
    reg->entries = NULL; reg->n = 0;
}

/* A simple RGB image buffer (what libav/mupdf produce and the solver consumes). */
typedef struct {
    int w, h;
    int stride;        /* bytes per row */
    uint8_t *pixels;   /* contiguous w*h*3 BGR (or w*h gray if channels==1) */
    int channels;      /* 1 or 3 */
} Img;

static inline void img_free(Img *im) { free(im->pixels); im->pixels = NULL; im->w = im->h = 0; }

/* Match function implemented in match.c (L1 over feature bytes). */
void match_batch(const uint8_t *lib, const uint8_t *targets,
                 int n_pages, int num_targets, int feat_len, int *results);

/* Set thread count for match_batch (OpenMP). 0 = auto. */
void match_set_threads(int n);

#ifdef __cplusplus
}
#endif

#endif /* BADAPPLE_H */
