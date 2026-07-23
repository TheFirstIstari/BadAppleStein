/*
 * badapple.h — shared types and on-disk formats for the pure-C BadAppleStein tool.
 *
 * No OpenCV. Everything is plain C (C99) using libav* for video/raster I/O and
 * mupdf for PDF rasterization. The matcher (match.c) is linked into arrange.
 *
 * Feature format (features.bin):
 *   uint32 n_pages
 *   uint32 feat_len        (total bytes per feature)
 *   uint32 G               (bits per cell, 1..8)
 *   uint32 n_scales        (number of scale levels)
 *   uint32 scale_0 ...     (grid size per level, e.g., 32, 64, 128)
 *   uint32 has_edges       (1 = include Sobel edge features per scale)
 *   uint32 channels        (1 = grayscale only, 3 = color; may be absent in old files)
 *   then n_pages * feat_len bytes, one feature per page.
 *   Grayscale: [gray_s0][edge_s0?][gray_s1][edge_s1?]...
 *   Color:     [gray_s0][edge_s0?][color_s0][gray_s1][edge_s1?][color_s1]...
 *              where color_sX is 3 * N^2 bytes (BGR, quantized to G bits).
 *
 * Registry format (registry.bin):
 *   uint32 n
 *   then per entry: int32 page_idx ; uint32 path_len ; path_len bytes (UTF-8).
 *
 * Manifest format (<dir>/<frame04d>.bin):
 *   uint32 src_w, src_h  (source video frame dimensions, for coordinate scaling)
 *   uint32 n
 *   then n records of 24 bytes: int32 x, y, w, h, op_id, page_idx.
 *   Coordinates are in source-video pixel space; render scales them to output.
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
    int feat_len;          /* total bytes per feature (sum of all scale levels) */
    int G;                 /* bits per cell (1..8) */
    int n_scales;          /* number of scale levels */
    int *scales;           /* array of grid sizes (e.g., {32, 64, 128}) */
    int has_edges;         /* 1 if edge features included per scale */
    int channels;          /* 1 = grayscale features, 3 = color features */
    uint8_t *data;         /* n_pages * feat_len bytes */
} FeatureDB;

static inline int ba_load_features(const char *path, FeatureDB *db) {
    uint8_t *buf; size_t len;
    if (ba_read_file(path, &buf, &len) != 0) return -1;
    /* Minimum header: n_pages + feat_len + G + n_scales + 1 scale + has_edges = 6 uint32s = 24 bytes */
    if (len < 24) { free(buf); return -1; }
    uint32_t n, feat_len, G, n_scales, has_edges;
    memcpy(&n, buf, 4);
    memcpy(&feat_len, buf + 4, 4);
    memcpy(&G, buf + 8, 4);
    memcpy(&n_scales, buf + 12, 4);
    /* Validate basic header values */
    if (G == 0 || G > 8 || n_scales == 0 || n_scales > 16) { free(buf); return -1; }
    size_t header_len = 16 + (size_t)n_scales * 4 + 4;  /* 4 uint32s + n_scales scales + has_edges */
    if (len < header_len) { free(buf); return -1; }
    /* Read scale array */
    int *scales = (int *)malloc((size_t)n_scales * sizeof(int));
    if (!scales) { free(buf); return -1; }
    for (uint32_t i = 0; i < n_scales; i++) {
        uint32_t s; memcpy(&s, buf + 16 + i * 4, 4);
        if (s == 0 || s > 256) { free(scales); free(buf); return -1; }
        scales[i] = (int)s;
    }
    memcpy(&has_edges, buf + 16 + n_scales * 4, 4);
    /* Validate feat_len matches expectations (grayscale or color) */
    size_t gray_feat_len = 0;
    for (uint32_t i = 0; i < n_scales; i++) {
        gray_feat_len += (size_t)scales[i] * scales[i];
    }
    size_t expected_gray = gray_feat_len * (has_edges ? 2 : 1);
    size_t expected_color = gray_feat_len * (1 + (has_edges ? 1 : 0) + 3);
    int detected_channels = 0;
    if (feat_len == (uint32_t)expected_gray) {
        detected_channels = 1;
    } else if (feat_len == (uint32_t)expected_color) {
        detected_channels = 3;
        /* Try to read the channels field from header if present */
        if (len >= header_len + 4) {
            uint32_t ch; memcpy(&ch, buf + header_len, 4);
            if (ch == 3) header_len += 4;
        }
    } else {
        free(scales); free(buf); return -1;
    }
    if (len < header_len) { free(scales); free(buf); return -1; }
    size_t need = header_len + (size_t)n * feat_len;
    if (need < header_len || need > len) { free(scales); free(buf); return -1; }
    db->n_pages = (int)n;
    db->feat_len = (int)feat_len;
    db->G = (int)G;
    db->n_scales = (int)n_scales;
    db->scales = scales;
    db->has_edges = (int)has_edges;
    db->channels = detected_channels;
    /* Copy feature data out of the read buffer */
    uint8_t *data = (uint8_t *)malloc(need - header_len);
    if (!data) { free(scales); free(buf); return -1; }
    memcpy(data, buf + header_len, need - header_len);
    free(buf);
    db->data = data;
    return 0;
}

static inline void ba_free_features(FeatureDB *db) {
    free(db->data); db->data = NULL;
    free(db->scales); db->scales = NULL;
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

/* Coarse-to-fine matching: first match on the first coarse_len bytes (smallest
 * scale gray feature) to select top-K candidates, then full L1 on those K.
 * Reduces memory traffic from n_pages×feat_len to n_pages×coarse_len + K×feat_len.
 * coarse_len <= feat_len; if coarse_len >= feat_len, falls back to direct matching. */
void match_batch_coarse(const uint8_t *lib, const uint8_t *targets,
                        int n_pages, int num_targets, int feat_len,
                        int coarse_len, int *results);

/* Set thread count for match_batch (OpenMP). 0 = auto. */
void match_set_threads(int n);

#ifdef __cplusplus
}
#endif

#endif /* BADAPPLE_H */
