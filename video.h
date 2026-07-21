/*
 * video.h — video decode/encode via FFmpeg libav* (pure C, no OpenCV).
 *
 * decode: open a file, pull frames as BGR24 Img (channels==3), one per call.
 * encode: feed BGR24 frames, mux to a file (e.g. prores/mov).
 */
#ifndef VIDEO_H
#define VIDEO_H

#include "badapple.h"

typedef struct VideoDecoder VideoDecoder;

/* Open `path` for decoding. Returns NULL on failure. */
VideoDecoder *video_decoder_open(const char *path);

/* Decode the next frame into `out` (BGR24, channels==3). Returns 1 on success,
 * 0 at EOF, -1 on error. Allocates out->pixels; caller frees with img_free(). */
int video_decoder_next(VideoDecoder *vd, Img *out);

/* Load a single image file (PNG, JPEG, TIFF, BMP, etc.) into `out` using libav.
 * Returns 0 on success, -1 on error. Allocates out->pixels. */
int video_image_load(const char *path, Img *out);

/* Width/height/fps of the opened video. */
int video_decoder_width(VideoDecoder *vd);
int video_decoder_height(VideoDecoder *vd);
double video_decoder_fps(VideoDecoder *vd);

void video_decoder_close(VideoDecoder *vd);

/* ---- Encoder ---- */

typedef struct VideoEncoder VideoEncoder;

/* Open an encoder writing to `path`. `w`,`h` are the output frame size in
 * pixels; `fps` the frame rate; `pix_fmt_name` e.g. "yuv422p10le" or "gray". */
VideoEncoder *video_encoder_open(const char *path, int w, int h, double fps,
                                  const char *pix_fmt_name, const char *codec_name);

/* Encode one BGR24 frame. Returns 0 on success, -1 on error. */
int video_encoder_write(VideoEncoder *ve, const Img *frame);

void video_encoder_close(VideoEncoder *ve); /* flushes + writes trailer */

#endif /* VIDEO_H */
