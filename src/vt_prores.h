/*
 * vt_prores.h — hardware ProRes encoding via AVFoundation (macOS only).
 *
 * Uses Apple Silicon's dedicated ProRes encode engine through AVAssetWriter.
 * Supports both grayscale (channels=1) and BGR color (channels=3).
 * Falls back to returning NULL from vt_prores_open if not available.
 *
 * On non-macOS platforms, all functions are stubs that return 0/NULL.
 */
#ifndef VT_PRORES_H
#define VT_PRORES_H

#include <stdint.h>

typedef struct VTEncoder VTEncoder;

/*
 * Check if hardware ProRes encoding is available on this system.
 * Returns 1 if available, 0 otherwise.
 * Always returns 0 on non-macOS platforms.
 */
int vt_prores_available(void);

/*
 * Open a hardware ProRes encoder writing to `path`.
 *
 * profile: 0 = ProRes 422 LT
 *          1 = ProRes 422
 *          2 = ProRes 422 HQ (default / recommended)
 *          3 = ProRes 4444
 *          4 = ProRes 4444 XQ
 *
 * channels: 1 = grayscale (GRAY8, encoded as NV12)
 *           3 = BGR color (BGR24, encoded as BGRA)
 *
 * Returns opaque encoder handle, or NULL on failure.
 */
VTEncoder *vt_prores_open(const char *path, int width, int height, double fps,
                           int profile, int channels);

/*
 * Encode one frame.
 * pixels:  GRAY8 (channels=1) or BGR24 (channels=3) pixel data
 * stride:  bytes per row (may be > width * channels for alignment)
 * channels: 1 or 3 (must match the value used in vt_prores_open)
 * Returns 0 on success, -1 on error.
 */
int vt_prores_write(VTEncoder *enc, const uint8_t *pixels, int width, int height,
                     int stride, int channels);

/*
 * Flush, finalize, and close the encoder. Frees all resources.
 */
void vt_prores_close(VTEncoder *enc);

#endif /* VT_PRORES_H */
