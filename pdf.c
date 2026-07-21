/*
 * pdf.c — PDF rasterization via mupdf C API.
 *
 * Build notes: link with -lmingw (or -lmupdf) and -lfitz on most installs,
 * or use the mupdf single static lib. On Homebrew: `brew install mupdf`.
 *
 * When mupdf is not installed, link pdf.c without -DHAVE_MUPDF to get a
 * stub that still loads images via libav but returns -1 for actual PDFs.
 */
#include "pdf.h"
#include "video.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Check if a path ends with ".pdf" (case-insensitive). */
static int is_pdf(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return (dot[1] == 'p' || dot[1] == 'P') &&
           (dot[2] == 'd' || dot[2] == 'D') &&
           (dot[3] == 'f' || dot[3] == 'F') &&
           dot[4] == '\0';
}

#ifdef HAVE_MUPDF

#include <mupdf/fitz.h>

int pdf_render_page(const char *path, int page_idx, float scale, Img *out) {
    /* Not a PDF? Load as image via libav (page_idx and scale ignored). */
    if (!is_pdf(path)) {
        (void)page_idx;
        (void)scale;
        return video_image_load(path, out);
    }

    /* Quick file existence check before touching mupdf */
    FILE *test = fopen(path, "rb");
    if (!test) return -1;
    fclose(test);

    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!ctx) return -1;

    int ret = -1;
    fz_try(ctx) {
        fz_document *doc = fz_open_document(ctx, path);
        int n_pages = fz_count_pages(ctx, doc);
        if (page_idx < 0 || page_idx >= n_pages) fz_throw(ctx, FZ_ERROR_GENERIC, "page index out of range");

        fz_page *page = fz_load_page(ctx, doc, page_idx);
        fz_rect bounds = fz_bound_page(ctx, page);
        float zoom = scale > 0 ? scale : 1.0f;
        fz_matrix transform = fz_scale(zoom, zoom);
        fz_rect bbox = fz_transform_rect(bounds, transform);
        fz_irect ibbox = fz_round_rect(bbox);

        fz_pixmap *pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), ibbox, NULL, 1);
        fz_clear_pixmap_with_value(ctx, pix, 0xff);
        fz_device *dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_run_page(ctx, page, dev, transform, NULL);
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);

        int w = pix->w, h = pix->h;
        out->w = w; out->h = h; out->channels = 3; out->stride = w * 3;
        out->pixels = (uint8_t *)malloc((size_t)w * h * 3);
        memcpy(out->pixels, pix->samples, (size_t)w * h * 3);

        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
        fz_drop_document(ctx, doc);
        ret = 0;
    }
    fz_catch(ctx) {
        ret = -1;
    }

    fz_drop_context(ctx);
    return ret;
}

#else /* !HAVE_MUPDF — stub that can still load images */

int pdf_render_page(const char *path, int page_idx, float scale, Img *out) {
    /* Without mupdf we can still load images via libav */
    if (!is_pdf(path)) {
        (void)page_idx;
        (void)scale;
        return video_image_load(path, out);
    }
    (void)page_idx;
    (void)scale;
    return -1;
}

#endif /* HAVE_MUPDF */

#ifdef __cplusplus
}
#endif
