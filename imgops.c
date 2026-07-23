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
        if (!dst->pixels) return;
        memcpy(dst->pixels, src->pixels, (size_t)src->w * src->h);
        return;
    }
    dst->w = src->w; dst->h = src->h; dst->channels = 1;
    dst->stride = src->w;
    dst->pixels = (uint8_t *)malloc((size_t)src->w * src->h);
    if (!dst->pixels) return;
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
    if (sw <= 0 || sh <= 0 || nw <= 0 || nh <= 0) {
        dst->pixels = NULL; dst->w = dst->h = dst->channels = dst->stride = 0;
        return;
    }

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

int64_t *img_integral(const uint8_t *gray, int w, int h) {
    int64_t *I = (int64_t *)malloc((size_t)(w + 1) * (h + 1) * sizeof(int64_t));
    if (!I) return NULL;
    int stride = w + 1;
    for (int x = 0; x <= w; x++) I[x] = 0;
    for (int y = 0; y < h; y++) {
        int64_t rowsum = 0;
        for (int x = 0; x < w; x++) {
            rowsum += gray[(size_t)y * w + x];
            I[(size_t)(y + 1) * stride + (x + 1)] = I[(size_t)y * stride + (x + 1)] + rowsum;
        }
        I[(size_t)(y + 1) * stride + 0] = 0;
    }
    return I;
}

void img_sobel_magnitude(const uint8_t *gray, int w, int h, uint8_t *out) {
    /* Sobel edge detection: compute gradient magnitude at each pixel.
     * Uses the standard 3×3 Sobel kernels:
     *   Gx = [-1 0 1; -2 0 2; -1 0 1]
     *   Gy = [-1 -2 -1; 0 0 0; 1 2 1]
     * Fast approximate magnitude: max(|gx|,|gy|) + min(|gx|,|gy|)/2
     * This is within 12% of true L2 and avoids the sqrt entirely.
     * Border pixels are copied from the nearest interior pixel. */
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int tl = gray[(size_t)(y-1) * w + (x-1)];
            int tc = gray[(size_t)(y-1) * w + x];
            int tr = gray[(size_t)(y-1) * w + (x+1)];
            int ml = gray[(size_t)y * w + (x-1)];
            int mr = gray[(size_t)y * w + (x+1)];
            int bl = gray[(size_t)(y+1) * w + (x-1)];
            int bc = gray[(size_t)(y+1) * w + x];
            int br = gray[(size_t)(y+1) * w + (x+1)];
            int gx = -tl + tr - 2*ml + 2*mr - bl + br;
            int gy = -tl - 2*tc - tr + bl + 2*bc + br;
            int ax = gx < 0 ? -gx : gx;
            int ay = gy < 0 ? -gy : gy;
            int mag = (ax >= ay) ? ax + ay / 2 : ay + ax / 2;
            if (mag > 255) mag = 255;
            out[(size_t)y * w + x] = (uint8_t)mag;
        }
    }
    /* Fill border pixels. */
    for (int x = 0; x < w; x++) {
        if (h > 1) out[x] = out[w + x];
        if (h > 2) out[(size_t)(h-1) * w + x] = out[(size_t)(h-2) * w + x];
    }
    for (int y = 0; y < h; y++) {
        if (w > 1) out[(size_t)y * w] = out[(size_t)y * w + 1];
        if (w > 2) out[(size_t)y * w + (w-1)] = out[(size_t)y * w + (w-2)];
    }
}

void img_compute_feature(const Img *crop, int N, int G, int color, uint8_t *out) {
    int ch = color ? 3 : 1;
    Img work;

    if (!color && crop->channels == 3) {
        /* Grayscale mode with BGR input: convert first so we average all
         * channels properly instead of accidentally reading just blue. */
        img_to_gray(crop, &work);
    } else if (color && crop->channels == 1) {
        /* Color mode with gray input: replicate to 3 channels. */
        work.w = crop->w; work.h = crop->h; work.channels = 3;
        work.stride = crop->w * 3;
        work.pixels = (uint8_t *)malloc((size_t)crop->w * crop->h * 3);
        if (!work.pixels) return;
        for (int i = 0; i < crop->w * crop->h; i++) {
            work.pixels[i * 3 + 0] = crop->pixels[i];
            work.pixels[i * 3 + 1] = crop->pixels[i];
            work.pixels[i * 3 + 2] = crop->pixels[i];
        }
    } else {
        /* Channel count matches mode: use as-is. */
        work.w = crop->w; work.h = crop->h; work.channels = crop->channels;
        work.stride = crop->stride;
        work.pixels = (uint8_t *)malloc((size_t)crop->stride * crop->h);
        if (!work.pixels) return;
        memcpy(work.pixels, crop->pixels, (size_t)crop->stride * crop->h);
    }

    Img rs;
    img_resize_area(&work, &rs, N, N);
    img_free(&work);

    int maxv = (1 << G) - 1;
    int idx = 0;
    for (int c = 0; c < ch; c++) {
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                int v = rs.pixels[(size_t)y * rs.stride + (size_t)x * ch + c];
                int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                if (q > maxv) q = maxv;
                out[idx++] = (uint8_t)q;
            }
        }
    }
    img_free(&rs);
}

void img_compute_feature_multires(const Img *crop, const int *scales, int n_scales,
                                   int G, int has_edges, int color, uint8_t *out) {
    int idx = 0;

    /* For grayscale-only mode, convert to grayscale once outside the scale loop. */
    Img gray;
    if (!color) {
        if (crop->channels == 3) {
            img_to_gray(crop, &gray);
        } else {
            gray.w = crop->w; gray.h = crop->h; gray.channels = 1;
            gray.stride = crop->w;
            gray.pixels = crop->pixels;  /* shallow copy */
        }
    }

    for (int s = 0; s < n_scales; s++) {
        int N = scales[s];

        if (color) {
            /* --- Color mode: single BGR resize, derive gray + edge + color. --- */
            Img color_work;
            if (crop->channels == 1) {
                /* Replicate gray -> BGR. */
                color_work.w = crop->w; color_work.h = crop->h; color_work.channels = 3;
                color_work.stride = crop->w * 3;
                color_work.pixels = (uint8_t *)malloc((size_t)crop->w * crop->h * 3);
                if (!color_work.pixels) goto done;
                for (int i = 0; i < crop->w * crop->h; i++) {
                    color_work.pixels[i*3+0] = crop->pixels[i];
                    color_work.pixels[i*3+1] = crop->pixels[i];
                    color_work.pixels[i*3+2] = crop->pixels[i];
                }
            } else {
                color_work = *crop;
            }

            Img rs_color;
            img_resize_area(&color_work, &rs_color, N, N);
            if (crop->channels == 1) img_free(&color_work);

            int maxv = (1 << G) - 1;

            /* Derive grayscale from resized BGR (Rec.601 luma). */
            uint8_t *gray_buf = (uint8_t *)malloc((size_t)N * N);
            if (!gray_buf) { img_free(&rs_color); goto done; }
            for (int y = 0; y < N; y++) {
                for (int x = 0; x < N; x++) {
                    const uint8_t *p = rs_color.pixels + (size_t)y * rs_color.stride + x * 3;
                    int v = (int)(0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]);
                    if (v > 255) v = 255;
                    gray_buf[(size_t)y * N + x] = (uint8_t)v;
                }
            }

            /* Grayscale feature. */
            for (int y = 0; y < N; y++) {
                for (int x = 0; x < N; x++) {
                    int v = gray_buf[(size_t)y * N + x];
                    int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                    if (q > maxv) q = maxv;
                    out[idx++] = (uint8_t)q;
                }
            }

            /* Edge feature. */
            if (has_edges) {
                uint8_t *edge_buf = (uint8_t *)malloc((size_t)N * N);
                if (!edge_buf) { free(gray_buf); img_free(&rs_color); goto done; }
                img_sobel_magnitude(gray_buf, N, N, edge_buf);
                for (int y = 0; y < N; y++) {
                    for (int x = 0; x < N; x++) {
                        int v = edge_buf[(size_t)y * N + x];
                        int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                        if (q > maxv) q = maxv;
                        out[idx++] = (uint8_t)q;
                    }
                }
                free(edge_buf);
            }

            free(gray_buf);

            /* Color feature: BGR channels, quantized. */
            for (int c = 0; c < 3; c++) {
                for (int y = 0; y < N; y++) {
                    for (int x = 0; x < N; x++) {
                        int v = rs_color.pixels[(size_t)y * rs_color.stride + x * 3 + c];
                        int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                        if (q > maxv) q = maxv;
                        out[idx++] = (uint8_t)q;
                    }
                }
            }

            img_free(&rs_color);
        } else {
            /* --- Grayscale-only mode (original code path). --- */
            Img rs;
            img_resize_area(&gray, &rs, N, N);

            int maxv = (1 << G) - 1;

            /* Grayscale feature: quantize each cell. */
            for (int y = 0; y < N; y++) {
                for (int x = 0; x < N; x++) {
                    int v = rs.pixels[(size_t)y * rs.stride + x];
                    int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                    if (q > maxv) q = maxv;
                    out[idx++] = (uint8_t)q;
                }
            }

            /* Edge feature: Sobel magnitude, quantize. */
            if (has_edges) {
                uint8_t *edge_buf = (uint8_t *)malloc((size_t)N * N);
                if (!edge_buf) { img_free(&rs); goto done; }
                img_sobel_magnitude(rs.pixels, N, N, edge_buf);
                for (int y = 0; y < N; y++) {
                    for (int x = 0; x < N; x++) {
                        int v = edge_buf[(size_t)y * N + x];
                        int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                        if (q > maxv) q = maxv;
                        out[idx++] = (uint8_t)q;
                    }
                }
                free(edge_buf);
            }

            img_free(&rs);
        }
    }

done:
    /* Free gray only if we allocated it (grayscale-only mode, 3-channel input). */
    if (!color && crop->channels == 3) img_free(&gray);
}
