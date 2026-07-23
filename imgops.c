/*
 * imgops.c — pure-C image operations (see imgops.h).
 */
#include "imgops.h"

#if defined(__AVX2__) || defined(__SSSE3__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

static void img_to_gray_simd(const uint8_t *src, uint8_t *dst, int n) {
    int i = 0;
#if defined(__AVX2__) || defined(__SSSE3__)
    {
        const __m128i zero   = _mm_setzero_si128();
        const __m128i coeffB = _mm_set1_epi16(29);
        const __m128i coeffG = _mm_set1_epi16(150);
        const __m128i coeffR = _mm_set1_epi16(77);
        const __m128i rnd    = _mm_set1_epi16(128);
        const __m128i shufB  = _mm_setr_epi8(0,3,6,9,12, -1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1);
        const __m128i shufG  = _mm_setr_epi8(1,4,7,10,13, -1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1);
        const __m128i shufR  = _mm_setr_epi8(2,5,8,11,14, -1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1);

        for (; i + 6 <= n; i += 5) {
            __m128i raw = _mm_loadu_si128((const __m128i*)(src + i * 3));
            __m128i bw = _mm_unpacklo_epi8(_mm_shuffle_epi8(raw, shufB), zero);
            __m128i gw = _mm_unpacklo_epi8(_mm_shuffle_epi8(raw, shufG), zero);
            __m128i rw = _mm_unpacklo_epi8(_mm_shuffle_epi8(raw, shufR), zero);

            __m128i lo = _mm_add_epi16(_mm_mullo_epi16(bw, coeffB),
                                       _mm_mullo_epi16(gw, coeffG));
            __m128i hi = _mm_add_epi16(_mm_mullo_epi16(rw, coeffR), rnd);
            __m128i luma = _mm_srli_epi16(_mm_add_epi16(lo, hi), 8);

            __m128i packed = _mm_packus_epi16(luma, luma);
            uint8_t tmp[8];
            _mm_storel_epi64((__m128i*)tmp, packed);
            memcpy(dst + i, tmp, 5);
        }
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    {
        for (; i + 8 <= n; i += 8) {
            uint8x8x3_t bgr = vld3_u8(src + i * 3);
            uint16x8_t luma = vaddq_u16(
                vaddq_u16(vmull_u8(bgr.val[0], vdup_n_u8(29)),
                          vmull_u8(bgr.val[1], vdup_n_u8(150))),
                vaddq_u16(vmull_u8(bgr.val[2], vdup_n_u8(77)),
                          vdupq_n_u16(128)));
            vst1_u8(dst + i, vqshrn_n_u16(luma, 8));
        }
    }
#endif
    for (; i < n; i++) {
        const uint8_t *p = src + i * 3;
        int v = (29 * p[0] + 150 * p[1] + 77 * p[2] + 128) >> 8;
        dst[i] = (uint8_t)(v > 255 ? 255 : v);
    }
}

void img_to_gray(const Img *src, Img *dst) {
    dst->w = dst->h = dst->channels = dst->stride = 0;
    dst->pixels = NULL;
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
        img_to_gray_simd(s + (size_t)y * src->stride, d + (size_t)y * src->w, src->w);
    }
}

void img_resize_area(const Img *src, Img *dst, int nw, int nh) {
    int ch = src->channels;
    int sw = src->w, sh = src->h;
    if (sw <= 0 || sh <= 0 || nw <= 0 || nh <= 0) {
        dst->w = dst->h = dst->channels = dst->stride = 0;
        dst->pixels = NULL;
        return;
    }
    dst->w = nw; dst->h = nh; dst->channels = ch;
    dst->stride = nw * ch;
    dst->pixels = (uint8_t *)malloc((size_t)nw * (size_t)nh * (size_t)ch);
    if (!dst->pixels) {
        dst->w = dst->h = dst->channels = dst->stride = 0;
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
        int sy0 = (int)((long long)y * sh / nh);
        int sy1 = (int)((long long)(y + 1) * sh / nh);
        if (sy1 > sh) sy1 = sh;
        if (sy0 >= sy1) sy1 = sy0 + 1;
        for (int x = 0; x < nw; x++) {
            int sx0 = (int)((long long)x * sw / nw);
            int sx1 = (int)((long long)(x + 1) * sw / nw);
            if (sx1 > sw) sx1 = sw;
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
    int64_t *I = (int64_t *)malloc((size_t)(w + 1) * (size_t)(h + 1) * sizeof(int64_t));
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
        const uint8_t *rt = gray + (size_t)(y - 1) * w;
        const uint8_t *rm = gray + (size_t)y * w;
        const uint8_t *rb = gray + (size_t)(y + 1) * w;
        uint8_t *ro = out + (size_t)y * w;

#if defined(__AVX2__)
        int x = 1;
        for (; x + 16 <= w - 1; x += 16) {
            __m128i raw_tl = _mm_loadu_si128((const __m128i*)(rt + x - 1));
            __m128i raw_tc = _mm_loadu_si128((const __m128i*)(rt + x));
            __m128i raw_tr = _mm_loadu_si128((const __m128i*)(rt + x + 1));
            __m128i raw_ml = _mm_loadu_si128((const __m128i*)(rm + x - 1));
            __m128i raw_mc = _mm_loadu_si128((const __m128i*)(rm + x));
            __m128i raw_mr = _mm_loadu_si128((const __m128i*)(rm + x + 1));
            __m128i raw_bl = _mm_loadu_si128((const __m128i*)(rb + x - 1));
            __m128i raw_bc = _mm_loadu_si128((const __m128i*)(rb + x));
            __m128i raw_br = _mm_loadu_si128((const __m128i*)(rb + x + 1));

            __m256i tl = _mm256_cvtepu8_epi16(raw_tl);
            __m256i tc = _mm256_cvtepu8_epi16(raw_tc);
            __m256i tr = _mm256_cvtepu8_epi16(raw_tr);
            __m256i ml = _mm256_cvtepu8_epi16(raw_ml);
            __m256i mc = _mm256_cvtepu8_epi16(raw_mc);
            __m256i mr = _mm256_cvtepu8_epi16(raw_mr);
            __m256i bl = _mm256_cvtepu8_epi16(raw_bl);
            __m256i bc = _mm256_cvtepu8_epi16(raw_bc);
            __m256i br = _mm256_cvtepu8_epi16(raw_br);

            __m256i two_ml = _mm256_slli_epi16(ml, 1);
            __m256i two_mr = _mm256_slli_epi16(mr, 1);
            __m256i two_tc = _mm256_slli_epi16(tc, 1);
            __m256i two_bc = _mm256_slli_epi16(bc, 1);

            __m256i gx = _mm256_sub_epi16(
                _mm256_add_epi16(_mm256_add_epi16(tr, br), two_mr),
                _mm256_add_epi16(_mm256_add_epi16(tl, bl), two_ml));
            __m256i gy = _mm256_sub_epi16(
                _mm256_add_epi16(_mm256_add_epi16(bl, br), two_bc),
                _mm256_add_epi16(_mm256_add_epi16(tl, tr), two_tc));

            __m256i ax = _mm256_abs_epi16(gx);
            __m256i ay = _mm256_abs_epi16(gy);
            __m256i mx = _mm256_max_epi16(ax, ay);
            __m256i mn = _mm256_min_epi16(ax, ay);
            __m256i mag = _mm256_add_epi16(mx, _mm256_srli_epi16(mn, 1));

            __m128i lo = _mm256_castsi256_si128(mag);
            __m128i hi = _mm256_extracti128_si256(mag, 1);
            _mm_storeu_si128((__m128i*)(ro + x), _mm_packus_epi16(lo, hi));
        }
        for (; x < w - 1; x++) {
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        int x = 1;
        for (; x + 16 <= w - 1; x += 16) {
            uint8x16_t raw_tl = vld1q_u8(rt + x - 1);
            uint8x16_t raw_tc = vld1q_u8(rt + x);
            uint8x16_t raw_tr = vld1q_u8(rt + x + 1);
            uint8x16_t raw_ml = vld1q_u8(rm + x - 1);
            uint8x16_t raw_mc = vld1q_u8(rm + x);
            uint8x16_t raw_mr = vld1q_u8(rm + x + 1);
            uint8x16_t raw_bl = vld1q_u8(rb + x - 1);
            uint8x16_t raw_bc = vld1q_u8(rb + x);
            uint8x16_t raw_br = vld1q_u8(rb + x + 1);

            uint16x8_t tl = vmovl_u8(vget_low_u8(raw_tl));
            uint16x8_t tc = vmovl_u8(vget_low_u8(raw_tc));
            uint16x8_t tr = vmovl_u8(vget_low_u8(raw_tr));
            uint16x8_t ml = vmovl_u8(vget_low_u8(raw_ml));
            uint16x8_t mc = vmovl_u8(vget_low_u8(raw_mc));
            uint16x8_t mr = vmovl_u8(vget_low_u8(raw_mr));
            uint16x8_t bl = vmovl_u8(vget_low_u8(raw_bl));
            uint16x8_t bc = vmovl_u8(vget_low_u8(raw_bc));
            uint16x8_t br = vmovl_u8(vget_low_u8(raw_br));

            int16x8_t gx_lo = vsubq_s16(
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(tr, br), vshlq_n_u16(mr, 1))),
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(tl, bl), vshlq_n_u16(ml, 1))));
            int16x8_t gy_lo = vsubq_s16(
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(bl, br), vshlq_n_u16(bc, 1))),
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(tl, tr), vshlq_n_u16(tc, 1))));

            int16x8_t ax_lo = vabsq_s16(gx_lo);
            int16x8_t ay_lo = vabsq_s16(gy_lo);
            int16x8_t mx_lo = vmaxq_s16(ax_lo, ay_lo);
            int16x8_t mn_lo = vminq_s16(ax_lo, ay_lo);
            int16x8_t mag_lo = vaddq_s16(mx_lo, vshrq_n_s16(mn_lo, 1));

            tl = vmovl_u8(vget_high_u8(raw_tl));
            tc = vmovl_u8(vget_high_u8(raw_tc));
            tr = vmovl_u8(vget_high_u8(raw_tr));
            ml = vmovl_u8(vget_high_u8(raw_ml));
            mc = vmovl_u8(vget_high_u8(raw_mc));
            mr = vmovl_u8(vget_high_u8(raw_mr));
            bl = vmovl_u8(vget_high_u8(raw_bl));
            bc = vmovl_u8(vget_high_u8(raw_bc));
            br = vmovl_u8(vget_high_u8(raw_br));

            int16x8_t gx_hi = vsubq_s16(
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(tr, br), vshlq_n_u16(mr, 1))),
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(tl, bl), vshlq_n_u16(ml, 1))));
            int16x8_t gy_hi = vsubq_s16(
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(bl, br), vshlq_n_u16(bc, 1))),
                vreinterpretq_s16_u16(vaddq_u16(vaddq_u16(tl, tr), vshlq_n_u16(tc, 1))));

            int16x8_t ax_hi = vabsq_s16(gx_hi);
            int16x8_t ay_hi = vabsq_s16(gy_hi);
            int16x8_t mx_hi = vmaxq_s16(ax_hi, ay_hi);
            int16x8_t mn_hi = vminq_s16(ax_hi, ay_hi);
            int16x8_t mag_hi = vaddq_s16(mx_hi, vshrq_n_s16(mn_hi, 1));

            vst1q_u8(ro + x, vcombine_u8(vqmovun_s16(mag_lo), vqmovun_s16(mag_hi)));
        }
        for (; x < w - 1; x++) {
#else
        for (int x = 1; x < w - 1; x++) {
#endif
            int tl = rt[x - 1];
            int tc = rt[x];
            int tr = rt[x + 1];
            int ml = rm[x - 1];
            int mr = rm[x + 1];
            int bl = rb[x - 1];
            int bc = rb[x];
            int br = rb[x + 1];
            int gx = -tl + tr - 2*ml + 2*mr - bl + br;
            int gy = -tl - 2*tc - tr + bl + 2*bc + br;
            int ax = gx < 0 ? -gx : gx;
            int ay = gy < 0 ? -gy : gy;
            int mag = (ax >= ay) ? ax + ay / 2 : ay + ax / 2;
            if (mag > 255) mag = 255;
            ro[x] = (uint8_t)mag;
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
    if (!rs.pixels) return;

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
    {
        size_t total = 0;
        for (int s = 0; s < n_scales; s++) {
            int N = scales[s];
            if (color) {
                total += (size_t)N * N * 4;
                if (has_edges) total += (size_t)N * N;
            } else {
                total += (size_t)N * N;
                if (has_edges) total += (size_t)N * N;
            }
        }
        memset(out, 0, total);
    }
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

    /* Pre-allocate max-size buffers once to avoid per-scale malloc/free. */
    int maxN = 0;
    for (int s = 0; s < n_scales; s++)
        if (scales[s] > maxN) maxN = scales[s];

    Img color_work = {0};
    uint8_t *gray_buf = NULL, *edge_buf = NULL;
    if (color) {
        if (crop->channels == 1) {
            color_work.w = crop->w; color_work.h = crop->h; color_work.channels = 3;
            color_work.stride = crop->w * 3;
            color_work.pixels = (uint8_t *)malloc((size_t)crop->w * crop->h * 3);
            if (!color_work.pixels) goto done;
        }
        gray_buf = (uint8_t *)malloc((size_t)maxN * maxN);
        if (!gray_buf) goto done;
    }
    if (has_edges) {
        edge_buf = (uint8_t *)malloc((size_t)maxN * maxN);
        if (!edge_buf) goto done;
    }

    for (int s = 0; s < n_scales; s++) {
        int N = scales[s];

        if (color) {
            /* --- Color mode: single BGR resize, derive gray + edge + color. --- */
            if (crop->channels == 1) {
                for (int i = 0; i < crop->w * crop->h; i++) {
                    color_work.pixels[i*3+0] = crop->pixels[i];
                    color_work.pixels[i*3+1] = crop->pixels[i];
                    color_work.pixels[i*3+2] = crop->pixels[i];
                }
            }

            Img rs_color;
            if (crop->channels == 1)
                img_resize_area(&color_work, &rs_color, N, N);
            else
                img_resize_area(crop, &rs_color, N, N);
            if (!rs_color.pixels) goto done;

            int maxv = (1 << G) - 1;

            /* Derive grayscale from resized BGR (Rec.601 luma). */
            for (int y = 0; y < N; y++) {
                for (int x = 0; x < N; x++) {
                    const uint8_t *p = rs_color.pixels + (size_t)y * rs_color.stride + x * 3;
                    int v = (29 * p[0] + 150 * p[1] + 77 * p[2] + 128) >> 8;
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
                img_sobel_magnitude(gray_buf, N, N, edge_buf);
                for (int y = 0; y < N; y++) {
                    for (int x = 0; x < N; x++) {
                        int v = edge_buf[(size_t)y * N + x];
                        int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                        if (q > maxv) q = maxv;
                        out[idx++] = (uint8_t)q;
                    }
                }
            }

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
            if (!rs.pixels) goto done;

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
                img_sobel_magnitude(rs.pixels, N, N, edge_buf);
                for (int y = 0; y < N; y++) {
                    for (int x = 0; x < N; x++) {
                        int v = edge_buf[(size_t)y * N + x];
                        int q = (G >= 8) ? v : (v * maxv + 127) / 255;
                        if (q > maxv) q = maxv;
                        out[idx++] = (uint8_t)q;
                    }
                }
            }

            img_free(&rs);
        }
    }

done:
    free(edge_buf);
    free(gray_buf);
    if (color_work.pixels) free(color_work.pixels);
    /* Free gray only if we allocated it (grayscale-only mode, 3-channel input). */
    if (!color && crop->channels == 3) img_free(&gray);
}
