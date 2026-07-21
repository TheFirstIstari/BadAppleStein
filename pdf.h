/*
 * pdf.h — PDF rasterization via mupdf (pure C). Replaces pypdfium2.
 */
#ifndef PDF_H
#define PDF_H

#include "badapple.h"

/* Render PDF page `page_idx` of file `path` at the given scale to a BGR24 Img.
 * Allocates out->pixels; caller frees with img_free(). Returns 0 on success. */
int pdf_render_page(const char *path, int page_idx, float scale, Img *out);

#endif /* PDF_H */
