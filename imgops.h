/*
 * imgops.h — minimal image operations in pure C (no OpenCV).
 * Replaces cv::resize(INTER_AREA), cv::cvtColor, cv::threshold, cv::integral,
 * and the feature extraction used by the greedy solver.
 */
#ifndef IMGOPS_H
#define IMGOPS_H

#include "badapple.h"

/* Convert an RGB/BGR Img (channels==3) to a grayscale Img (channels==1).
 * Uses Rec.601 luma. Allocates a new Img; caller frees with img_free(). */
void img_to_gray(const Img *src, Img *dst);

/* Area-resample `src` (any channels) into `dst` of size (nw, nh).
 * Uses a simple box/area average per destination pixel (INTER_AREA-like).
 * Allocates dst->pixels; caller frees. */
void img_resize_area(const Img *src, Img *dst, int nw, int nh);

/* Threshold a grayscale buffer in place: out = (v > thr) ? maxval : 0. */
void img_threshold_u8(uint8_t *buf, int n, int thr, int maxval);

/* Compute an integral (sum) image of a uint8 grayscale buffer.
 * Returns a (h+1)*(w+1) int32 buffer; out[y*(w+1)+x] = sum over [0..y-1,0..x-1].
 * Caller frees. */
int32_t *img_integral(const uint8_t *gray, int w, int h);

/* Extract a feature vector for a tile crop.
 *   crop     : source Img (3-channel BGR or 1-channel gray)
 *   N        : grid size
 *   G        : bits per cell (1..8)
 *   color    : if nonzero, 3 channels (BGR order) else 1 (gray)
 *   out      : caller-allocated buffer of N*N*(color?3:1) bytes
 * The crop is area-resampled to N x N, then each cell is quantized:
 *   v_quant = round(v/255 * (2^G - 1))   (G==8 => keep raw 0..255)
 * This mirrors the reference Python compute_feature(). */
void img_compute_feature(const Img *crop, int N, int G, int color, uint8_t *out);

#endif /* IMGOPS_H */
