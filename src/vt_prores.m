/*
 * vt_prores.m — hardware ProRes encoding via AVFoundation (macOS).
 *
 * Links: -framework AVFoundation -framework CoreMedia -framework CoreVideo
 *
 * Supports both grayscale (NV12) and BGR color (BGRA) input.
 * On Apple Silicon, the ProRes encoding is hardware-accelerated.
 */
#include "vt_prores.h"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

struct VTEncoder {
    AVAssetWriter *writer;
    AVAssetWriterInput *input;
    AVAssetWriterInputPixelBufferAdaptor *adaptor;
    int width, height;
    int channels;      /* 1 = grayscale (NV12), 3 = BGR (BGRA) */
    double fps;
    int64_t frame_count;
    NSURL *outputURL;
};

int vt_prores_available(void) {
    @autoreleasepool {
        NSString *tmpPath = [NSTemporaryDirectory() stringByAppendingPathComponent:@"vt_prores_test.mov"];
        NSURL *tmpURL = [NSURL fileURLWithPath:tmpPath];

        NSError *error = nil;
        AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:tmpURL
                                                        fileType:AVFileTypeQuickTimeMovie
                                                           error:&error];
        if (!writer) return 0;

        NSDictionary *settings = @{
            AVVideoCodecKey: AVVideoCodecTypeAppleProRes422HQ,
            AVVideoWidthKey: @(64),
            AVVideoHeightKey: @(64),
        };

        AVAssetWriterInput *input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                      outputSettings:settings];
        if (!input) return 0;
        [writer addInput:input];
        [[NSFileManager defaultManager] removeItemAtURL:tmpURL error:nil];
        return 1;
    }
}

VTEncoder *vt_prores_open(const char *path, int width, int height, double fps,
                           int profile, int channels) {
    @autoreleasepool {
        AVVideoCodecType codecType;
        switch (profile) {
            case 0:  codecType = AVVideoCodecTypeAppleProRes422LT; break;
            case 1:  codecType = AVVideoCodecTypeAppleProRes422; break;
            case 2:  codecType = AVVideoCodecTypeAppleProRes422HQ; break;
            case 3:  codecType = AVVideoCodecTypeAppleProRes4444; break;
            case 4:  codecType = (AVVideoCodecType)@"ap4x"; break; /* ProRes 4444 XQ — macOS 15.0+ */
            default: codecType = AVVideoCodecTypeAppleProRes422HQ; break;
        }

        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];

        NSError *error = nil;
        AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:url
                                                        fileType:AVFileTypeQuickTimeMovie
                                                           error:&error];
        if (!writer || error) {
            if (error) NSLog(@"[vt_prores] writer error: %@", error);
            return NULL;
        }

        NSDictionary *settings = @{
            AVVideoCodecKey: codecType,
            AVVideoWidthKey: @(width),
            AVVideoHeightKey: @(height),
        };

        AVAssetWriterInput *input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                      outputSettings:settings];
        input.expectsMediaDataInRealTime = NO;

        /* Select pixel buffer format based on channel count.
         * channels=1 → NV12 (Y + neutral chroma, most efficient for grayscale)
         * channels=3 → BGRA (native 32-bit, Apple's preferred format for color) */
        OSType pb_fmt;
        if (channels == 3) {
            pb_fmt = kCVPixelFormatType_32BGRA;
        } else {
            pb_fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            channels = 1;
        }

        NSDictionary *pbAttrs = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey: @(pb_fmt),
            (NSString *)kCVPixelBufferWidthKey: @(width),
            (NSString *)kCVPixelBufferHeightKey: @(height),
            (NSString *)kCVPixelBufferPoolMinimumBufferCountKey: @(8),
        };

        AVAssetWriterInputPixelBufferAdaptor *adaptor =
            [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                                                     sourcePixelBufferAttributes:pbAttrs];

        [writer addInput:input];
        [writer startWriting];
        [writer startSessionAtSourceTime:kCMTimeZero];

        VTEncoder *enc = (VTEncoder *)calloc(1, sizeof(VTEncoder));
        if (!enc) return NULL;
        enc->writer = writer;
        enc->input = input;
        enc->adaptor = adaptor;
        enc->width = width;
        enc->height = height;
        enc->channels = channels;
        enc->fps = fps;
        enc->frame_count = 0;
        enc->outputURL = url;

        return enc;
    }
}

int vt_prores_write(VTEncoder *enc, const uint8_t *pixels, int width, int height,
                     int stride, int channels) {
    if (!enc) return -1;

    @autoreleasepool {
        while (!enc->input.isReadyForMoreMediaData) {
            [NSThread sleepForTimeInterval:0.001];
        }

        /* Allocate pixel buffer from the adaptor's pool */
        CVPixelBufferRef pxbuf = NULL;
        CVReturn status = kCVReturnSuccess;
        for (int attempt = 0; attempt < 3; attempt++) {
            status = CVPixelBufferPoolCreatePixelBuffer(NULL,
                enc->adaptor.pixelBufferPool, &pxbuf);
            if (status == kCVReturnSuccess && pxbuf) break;
            NSLog(@"[vt_prores] pixel buffer alloc failed (attempt %d): %d", attempt + 1, status);
            if (pxbuf) { CVPixelBufferRelease(pxbuf); pxbuf = NULL; }
            [NSThread sleepForTimeInterval:0.01];
        }
        if (status != kCVReturnSuccess || !pxbuf) {
            NSLog(@"[vt_prores] pixel buffer alloc failed after retries: %d", status);
            return -1;
        }

        if (CVPixelBufferLockBaseAddress(pxbuf, 0) != kCVReturnSuccess) {
            NSLog(@"[vt_prores] CVPixelBufferLockBaseAddress failed");
            CVPixelBufferRelease(pxbuf);
            return -1;
        }

        if (enc->channels == 3) {
            /* ── BGRA path: BGR24 → BGRA ─────────────────────── */
            uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddress(pxbuf);
            size_t bufStride = CVPixelBufferGetBytesPerRow(pxbuf);
            for (int y = 0; y < height; y++) {
                const uint8_t *src = pixels + (size_t)y * stride;
                uint8_t *dst = base + (size_t)y * bufStride;
                for (int x = 0; x < width; x++) {
                    dst[x*4+0] = src[x*3+0]; /* B */
                    dst[x*4+1] = src[x*3+1]; /* G */
                    dst[x*4+2] = src[x*3+2]; /* R */
                    dst[x*4+3] = 255;         /* A */
                }
            }
        } else {
            /* ── NV12 path: GRAY8 → Y + neutral chroma ────────── */
            uint8_t *yPlane  = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pxbuf, 0);
            size_t yStride   = CVPixelBufferGetBytesPerRowOfPlane(pxbuf, 0);
            size_t yPlaneW   = CVPixelBufferGetWidthOfPlane(pxbuf, 0);
            size_t yPlaneH   = CVPixelBufferGetHeightOfPlane(pxbuf, 0);

            for (int y = 0; y < (int)yPlaneH; y++) {
                uint8_t *dst = yPlane + (size_t)y * yStride;
                if (y < height) {
                    size_t copy_w = (yPlaneW < (size_t)width) ? yPlaneW : (size_t)width;
                    memcpy(dst, pixels + (size_t)y * stride, copy_w);
                    if (yStride > copy_w) memset(dst + copy_w, 0, yStride - copy_w);
                } else {
                    memset(dst, 0, yStride);
                }
            }

            uint8_t *uvPlane = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pxbuf, 1);
            size_t uvStride  = CVPixelBufferGetBytesPerRowOfPlane(pxbuf, 1);
            size_t uvPlaneW  = CVPixelBufferGetWidthOfPlane(pxbuf, 1);
            size_t uvPlaneH  = CVPixelBufferGetHeightOfPlane(pxbuf, 1);

            for (int y = 0; y < (int)uvPlaneH; y++) {
                size_t fill = uvPlaneW * 2 < uvStride ? uvPlaneW * 2 : uvStride;
                memset(uvPlane + (size_t)y * uvStride, 128, fill);
                if (fill < uvStride) memset(uvPlane + (size_t)y * uvStride + fill, 0, uvStride - fill);
            }
        }

        CVPixelBufferUnlockBaseAddress(pxbuf, 0);

        /* Present at the correct timestamp */
        double t = (enc->fps > 0.0) ? enc->frame_count / enc->fps : 0.0;
        CMTime pts = CMTimeMakeWithSeconds(t, 600);

        if (![enc->adaptor appendPixelBuffer:pxbuf withPresentationTime:pts]) {
            NSLog(@"[vt_prores] append failed at frame %lld", enc->frame_count);
            CVPixelBufferRelease(pxbuf);
            return -1;
        }

        CVPixelBufferRelease(pxbuf);
        enc->frame_count++;
        return 0;
    }
}

void vt_prores_close(VTEncoder *enc) {
    if (!enc) return;

    @autoreleasepool {
        [enc->input markAsFinished];

        dispatch_semaphore_t sema = dispatch_semaphore_create(0);
        [enc->writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(sema);
        }];
        dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

        /* Release ObjC objects before freeing the C struct.
         * ARC manages these references but free() doesn't know about them. */
        enc->writer = nil;
        enc->input = nil;
        enc->adaptor = nil;
        enc->outputURL = nil;
        free(enc);
    }
}

#else /* !__APPLE__ — stubs */

int vt_prores_available(void) { return 0; }
VTEncoder *vt_prores_open(const char *path, int w, int h, double fps,
                           int profile, int channels) {
    (void)path; (void)w; (void)h; (void)fps; (void)profile; (void)channels;
    return NULL;
}
int vt_prores_write(VTEncoder *enc, const uint8_t *pixels, int w, int h,
                     int s, int ch) {
    (void)enc; (void)pixels; (void)w; (void)h; (void)s; (void)ch;
    return -1;
}
void vt_prores_close(VTEncoder *enc) { (void)enc; }

#endif /* __APPLE__ */
