/*
 * imgops.c — pure-C image operations (see imgops.h).
 */
#include "imgops.h"
#include <math.h>

void img_to_gray(const Img *src, Img *dst) {
    if (src->channels == 1) {
        dst->w = src->w; dst->h = src->h; dst->channels = 1;
        dst->stride = src->w;
        dst->pixels = (uint8_t *)malloc((size_t)src->w * src->h);
        memcpy(dst->pixels, src->pixels, (size_t)src->w * src->h);
        return;
    }
    dst->w = src->w; dst->h = src->h; dst->channels = 1;
    dst->stride = src->w;
    dst->pixels = (uint8_t *)malloc((size_t)src->w * src->h);
    const uint8_t *s = src->pixels;
    uint8_t *d = dst->pixels;
    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < src->w; x++) {
            const uint8_t *p = s + (size_t)y * src->stride + x * 3;
            /* Rec.601 luma; BGR order in memory. */
            int v = (int)(0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]);
            if (v > 255) v = 255;
            d[(size_t)y * src->w + x] = (uint8_t)v;
        }
    }
}

void img_resize_area(const Img *src, Img *dst, int nw, int nh) {
    dst->w = nw; dst->h = nh; dst->channels = src->channels;
    dst->stride = nw * src->channels;
    dst->pixels = (uint8_t *)malloc((size_t)nw * nh * src->channels);
    int ch = src->channels;
    int sw = src->w, sh = src->h;

    /* Fast path: exact integer upscale.
     * For integer upscale factors, area resampling equals nearest-neighbor
     * replication (each dest pixel covers exactly k source pixels of the
     * same value). This is the common case for small tiles (e.g. 8×8→64×64). */
    if (nw % sw == 0 && nh % sh == 0 && nw != sw) {
        int ux = nw / sw, uy = nh / sh;
        if (ux > 0 && uy > 0) {
            for (int y = 0; y < nh; y++) {
                int sy = y / uy;
                const uint8_t *srow = src->pixels + (size_t)sy * src->stride;
                uint8_t *drow = dst->pixels + (size_t)y * dst->stride;
                for (int x = 0; x < nw; x++) {
                    int sx = x / ux;
                    const uint8_t *sp = srow + (size_t)sx * ch;
                    uint8_t *dp = drow + (size_t)x * ch;
                    for (int c = 0; c < ch; c++) dp[c] = sp[c];
                }
            }
            return;
        }
    }

    for (int y = 0; y < nh; y++) {
        /* source y range covering destination row y */
        double fy0 = (double)y * sh / nh;
        double fy1 = (double)(y + 1) * sh / nh;
        int sy0 = (int)fy0, sy1 = (int)ceil(fy1);
        if (sy1 > sh) sy1 = sh;
        if (sy0 < 0) sy0 = 0;
        if (sy0 >= sy1) sy1 = sy0 + 1;
        for (int x = 0; x < nw; x++) {
            double fx0 = (double)x * sw / nw;
            double fx1 = (double)(x + 1) * sw / nw;
            int sx0 = (int)fx0, sx1 = (int)ceil(fx1);
            if (sx1 > sw) sx1 = sw;
            if (sx0 < 0) sx0 = 0;
            if (sx0 >= sx1) sx1 = sx0 + 1;
            int area = (sy1 - sy0) * (sx1 - sx0);
            for (int c = 0; c < ch; c++) {
                unsigned long sum = 0;
                for (int sy = sy0; sy < sy1; sy++) {
                    for (int sx = sx0; sx < sx1; sx++) {
                        sum += src->pixels[(size_t)sy * src->stride + (size_t)sx * ch + c];
                    }
                }
                int v = (int)(sum / area);
                if (v > 255) v = 255;
                dst->pixels[(size_t)y * dst->stride + (size_t)x * ch + c] = (uint8_t)v;
            }
        }
    }
}

void img_threshold_u8(uint8_t *buf, int n, int thr, int maxval) {
    for (int i = 0; i < n; i++) buf[i] = (buf[i] > thr) ? (uint8_t)maxval : 0;
}

int32_t *img_integral(const uint8_t *gray, int w, int h) {
    int32_t *I = (int32_t *)malloc((size_t)(w + 1) * (h + 1) * sizeof(int32_t));
    int stride = w + 1;
    for (int x = 0; x <= w; x++) I[x] = 0;
    for (int y = 0; y < h; y++) {
        int32_t rowsum = 0;
        for (int x = 0; x < w; x++) {
            rowsum += gray[(size_t)y * w + x];
            I[(size_t)(y + 1) * stride + (x + 1)] = I[(size_t)y * stride + (x + 1)] + rowsum;
        }
        I[(size_t)(y + 1) * stride + 0] = 0;
    }
    return I;
}

void img_compute_feature(const Img *crop, int N, int G, int color, uint8_t *out) {
    int ch = color ? 3 : 1;
    Img rs;
    img_resize_area(crop, &rs, N, N);
    int maxv = (1 << G) - 1;
    int idx = 0;
    for (int c = 0; c < ch; c++) {
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                int v = rs.pixels[(size_t)y * rs.stride + (size_t)x * ch + c];
                int q = (G >= 8) ? v : (v * maxv / 255);
                if (q > maxv) q = maxv;
                out[idx++] = (uint8_t)q;
            }
        }
    }
    img_free(&rs);
}
