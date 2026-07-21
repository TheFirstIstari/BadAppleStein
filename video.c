/*
 * video.c — FFmpeg libav* decode/encode (pure C).
 */
#include "video.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

struct VideoDecoder {
    AVFormatContext *fmt;
    int video_stream;
    const AVCodec *codec;
    AVCodecContext *ctx;
    struct SwsContext *sws;
    AVFrame *frame;
    AVPacket *pkt;
    int width, height;
    double fps;
    int eof;
};

VideoDecoder *video_decoder_open(const char *path) {
    static int network_inited = 0;
    if (!network_inited) { avformat_network_init(); network_inited = 1; }
    VideoDecoder *vd = (VideoDecoder *)calloc(1, sizeof(VideoDecoder));
    if (!vd) return NULL;
    if (avformat_open_input(&vd->fmt, path, NULL, NULL) != 0) { free(vd); return NULL; }
    if (avformat_find_stream_info(vd->fmt, NULL) < 0) { avformat_close_input(&vd->fmt); free(vd); return NULL; }

    vd->video_stream = -1;
    for (unsigned i = 0; i < vd->fmt->nb_streams; i++) {
        if (vd->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vd->video_stream = (int)i; break;
        }
    }
    if (vd->video_stream < 0) { avformat_close_input(&vd->fmt); free(vd); return NULL; }

    AVStream *st = vd->fmt->streams[vd->video_stream];
    vd->codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!vd->codec) { avformat_close_input(&vd->fmt); free(vd); return NULL; }
    vd->ctx = avcodec_alloc_context3(vd->codec);
    if (!vd->ctx) { avformat_close_input(&vd->fmt); free(vd); return NULL; }
    if (avcodec_parameters_to_context(vd->ctx, st->codecpar) < 0) { avcodec_free_context(&vd->ctx); avformat_close_input(&vd->fmt); free(vd); return NULL; }
    if (avcodec_open2(vd->ctx, vd->codec, NULL) < 0) { avcodec_free_context(&vd->ctx); avformat_close_input(&vd->fmt); free(vd); return NULL; }

    vd->width = vd->ctx->width;
    vd->height = vd->ctx->height;
    vd->fps = st->avg_frame_rate.num ? (double)st->avg_frame_rate.num / st->avg_frame_rate.den : 30.0;

    vd->sws = sws_getContext(vd->width, vd->height, vd->ctx->pix_fmt,
                              vd->width, vd->height, AV_PIX_FMT_BGR24,
                              SWS_BILINEAR, NULL, NULL, NULL);
    if (!vd->sws) { avcodec_free_context(&vd->ctx); avformat_close_input(&vd->fmt); free(vd); return NULL; }

    vd->frame = av_frame_alloc();
    vd->pkt = av_packet_alloc();
    return vd;
}

int video_decoder_width(VideoDecoder *vd) { return vd->width; }
int video_decoder_height(VideoDecoder *vd) { return vd->height; }
double video_decoder_fps(VideoDecoder *vd) { return vd->fps; }

int video_decoder_next(VideoDecoder *vd, Img *out) {
    if (vd->eof) return 0;
    out->pixels = NULL;
    while (1) {
        int ret = av_read_frame(vd->fmt, vd->pkt);
        if (ret < 0) { vd->eof = 1; return 0; }
        if (vd->pkt->stream_index != vd->video_stream) { av_packet_unref(vd->pkt); continue; }
        ret = avcodec_send_packet(vd->ctx, vd->pkt);
        av_packet_unref(vd->pkt);
        if (ret < 0) continue;
        while (ret >= 0) {
            ret = avcodec_receive_frame(vd->ctx, vd->frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) return -1;
            /* got a frame */
            uint8_t *dst = (uint8_t *)malloc((size_t)vd->width * vd->height * 3);
            uint8_t *dst_slices[1]; dst_slices[0] = dst;
            int dst_stride = vd->width * 3;
            sws_scale(vd->sws, (const uint8_t *const *)vd->frame->data, vd->frame->linesize,
                      0, vd->frame->height, dst_slices, &dst_stride);
            out->w = vd->width; out->h = vd->height; out->channels = 3;
            out->stride = dst_stride; out->pixels = dst;
            av_frame_unref(vd->frame);
            return 1;
        }
    }
}

void video_decoder_close(VideoDecoder *vd) {
    if (!vd) return;
    sws_freeContext(vd->sws);
    av_frame_free(&vd->frame);
    av_packet_free(&vd->pkt);
    avcodec_free_context(&vd->ctx);
    avformat_close_input(&vd->fmt);
    free(vd);
}

/* ---- Image file loader ---- */

int video_image_load(const char *path, Img *out) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return -1; }

    int video_stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = (int)i; break;
        }
    }
    if (video_stream < 0) { avformat_close_input(&fmt); return -1; }

    AVStream *st = fmt->streams[video_stream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmt); return -1; }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) { avformat_close_input(&fmt); return -1; }
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return -1; }
    if (avcodec_open2(ctx, codec, NULL) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return -1; }

    struct SwsContext *sws = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
                                            ctx->width, ctx->height, AV_PIX_FMT_BGR24,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return -1; }

    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int got_frame = 0;

    /* Read packets until we decode the first frame */
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != video_stream) { av_packet_unref(pkt); continue; }
        int ret = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;
        ret = avcodec_receive_frame(ctx, frame);
        if (ret == 0) { got_frame = 1; break; }
        if (ret == AVERROR_EOF) break;
    }
    if (!got_frame) {
        /* Flush decoder (some image formats deliver only on flush) */
        avcodec_send_packet(ctx, NULL);
        if (avcodec_receive_frame(ctx, frame) == 0) got_frame = 1;
    }

    int ret_val = -1;
    if (got_frame) {
        int w = ctx->width, h = ctx->height;
        uint8_t *dst = (uint8_t *)malloc((size_t)w * h * 3);
        if (dst) {
            uint8_t *dst_slices[1] = { dst };
            int dst_stride = w * 3;
            sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
                      0, h, dst_slices, &dst_stride);
            out->w = w; out->h = h; out->channels = 3; out->stride = dst_stride;
            out->pixels = dst;
            ret_val = 0;
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ret_val;
}

/* ---------------- Encoder ---------------- */

struct VideoEncoder {
    AVFormatContext *fmt;
    AVStream *stream;
    const AVCodec *codec;
    AVCodecContext *ctx;
    struct SwsContext *sws;       /* BGR24 → dst_pix_fmt */
    struct SwsContext *sws_gray8; /* GRAY8 → dst_pix_fmt (avoids BGR expansion) */
    AVFrame *frame;
    AVPacket *pkt;
    int width, height;
    enum AVPixelFormat dst_pix_fmt;
};

VideoEncoder *video_encoder_open(const char *path, int w, int h, double fps,
                                  const char *pix_fmt_name, const char *codec_name) {
    VideoEncoder *ve = (VideoEncoder *)calloc(1, sizeof(VideoEncoder));
    if (!ve) return NULL;
    ve->width = w; ve->height = h;
    ve->dst_pix_fmt = av_get_pix_fmt(pix_fmt_name);

    const AVOutputFormat *ofmt = av_guess_format(NULL, path, NULL);
    if (!ofmt && codec_name) ofmt = av_guess_format(codec_name, NULL, NULL);
    if (!ofmt) { free(ve); return NULL; }
    if (avformat_alloc_output_context2(&ve->fmt, (AVOutputFormat *)ofmt, NULL, path) < 0) { free(ve); return NULL; }

    ve->codec = avcodec_find_encoder_by_name(codec_name ? codec_name : "prores_ks");
    if (!ve->codec) ve->codec = avcodec_find_encoder(ofmt->video_codec);
    if (!ve->codec) { avformat_free_context(ve->fmt); free(ve); return NULL; }

    ve->ctx = avcodec_alloc_context3(ve->codec);
    if (!ve->ctx) { avformat_free_context(ve->fmt); free(ve); return NULL; }
    ve->ctx->width = w; ve->ctx->height = h;
    ve->ctx->pix_fmt = ve->dst_pix_fmt;
    ve->ctx->time_base = (AVRational){1, (int)(fps > 0 ? fps : 30)};
    if (ve->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        ve->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(ve->ctx, ve->codec, NULL) < 0) { avcodec_free_context(&ve->ctx); avformat_free_context(ve->fmt); free(ve); return NULL; }

    ve->stream = avformat_new_stream(ve->fmt, NULL);
    if (!ve->stream) { avcodec_free_context(&ve->ctx); avformat_free_context(ve->fmt); free(ve); return NULL; }
    avcodec_parameters_from_context(ve->stream->codecpar, ve->ctx);
    ve->stream->time_base = ve->ctx->time_base;

    if (!(ve->fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ve->fmt->pb, path, AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&ve->ctx); avformat_free_context(ve->fmt); free(ve); return NULL;
        }
    }
    if (avformat_write_header(ve->fmt, NULL) < 0) {
        avio_closep(&ve->fmt->pb); avcodec_free_context(&ve->ctx); avformat_free_context(ve->fmt); free(ve); return NULL;
    }

    ve->sws = sws_getContext(w, h, AV_PIX_FMT_BGR24, w, h, ve->dst_pix_fmt, SWS_BILINEAR, NULL, NULL, NULL);
    ve->sws_gray8 = sws_getContext(w, h, AV_PIX_FMT_GRAY8, w, h, ve->dst_pix_fmt, SWS_BILINEAR, NULL, NULL, NULL);
    ve->frame = av_frame_alloc();
    ve->frame->format = ve->dst_pix_fmt;
    ve->frame->width = w; ve->frame->height = h;
    ve->frame->pts = 0;
    if (av_frame_get_buffer(ve->frame, 0) < 0) {
        av_frame_free(&ve->frame);
        avcodec_free_context(&ve->ctx);
        avformat_free_context(ve->fmt);
        free(ve);
        return NULL;
    }
    ve->pkt = av_packet_alloc();
    return ve;
}

int video_encoder_write(VideoEncoder *ve, const Img *frame) {
    if (!ve) return -1;

    uint8_t *src_slices[1];
    int src_stride;

    if (frame->channels == 1) {
        /* Direct GRAY8 → dst — avoids 3× BGR24 expansion */
        src_slices[0] = frame->pixels;
        src_stride    = frame->stride;
        sws_scale(ve->sws_gray8, (const uint8_t *const *)src_slices, &src_stride,
                  0, frame->h, ve->frame->data, ve->frame->linesize);
    } else if (frame->channels == 3) {
        src_slices[0] = frame->pixels;
        src_stride    = frame->stride;
        sws_scale(ve->sws, (const uint8_t *const *)src_slices, &src_stride,
                  0, frame->h, ve->frame->data, ve->frame->linesize);
    } else {
        return -1;
    }
    int ret = avcodec_send_frame(ve->ctx, ve->frame);
    ve->frame->pts += 1;
    if (ret < 0) return -1;
    while (ret >= 0) {
        ret = avcodec_receive_packet(ve->ctx, ve->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return -1;
        av_packet_rescale_ts(ve->pkt, ve->ctx->time_base, ve->stream->time_base);
        ve->pkt->stream_index = ve->stream->index;
        av_interleaved_write_frame(ve->fmt, ve->pkt);
        av_packet_unref(ve->pkt);
    }
    return 0;
}

void video_encoder_close(VideoEncoder *ve) {
    if (!ve) return;
    avcodec_send_frame(ve->ctx, NULL); /* flush */
    while (avcodec_receive_packet(ve->ctx, ve->pkt) >= 0) {
        av_packet_rescale_ts(ve->pkt, ve->ctx->time_base, ve->stream->time_base);
        ve->pkt->stream_index = ve->stream->index;
        av_interleaved_write_frame(ve->fmt, ve->pkt);
        av_packet_unref(ve->pkt);
    }
    av_write_trailer(ve->fmt);
    if (!(ve->fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ve->fmt->pb);
    sws_freeContext(ve->sws);
    av_frame_free(&ve->frame);
    av_packet_free(&ve->pkt);
    avcodec_free_context(&ve->ctx);
    avformat_free_context(ve->fmt);
    free(ve);
}

#ifdef __cplusplus
}
#endif
